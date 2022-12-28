/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tsdb.h"

#define TSDB_ITER_TYPE_MEM 0x0
#define TSDB_ITER_TYPE_DAT 0x1
#define TSDB_ITER_TYPE_STT 0x2

typedef struct {
} SMemDIter;

typedef struct {
  SDataFReader *pReader;
  SArray       *aBlockIdx;  // SArray<SBlockIdx>
  SMapData      mDataBlk;   // SMapData<SDataBlk>
  SBlockData    bData;
  int32_t       iBlockIdx;
  int32_t       iDataBlk;
  int32_t       iRow;
} SDataDIter;

typedef struct {
  SDataFReader *pReader;
  int32_t       iStt;
  SArray       *aSttBlk;  // SArray<SSttBlk>
  SBlockData    bData;
  int32_t       iSttBlk;
  int32_t       iRow;
} SSttDIter;

typedef struct STsdbDataIter {
  struct STsdbDataIter *next;

  int32_t     flag;
  SRowInfo    rowInfo;
  SRBTreeNode n;
  char        handle[];
} STsdbDataIter;

#define TSDB_DATA_ITER_FROM_RBTN(N) ((STsdbDataIter *)((char *)N - offsetof(STsdbDataIter, n)))

typedef struct {
  STsdb         *pTsdb;
  int64_t        cid;
  int32_t        maxRows;
  int32_t        minRows;
  STsdbFS        fs;
  int32_t        fid;
  SDFileSet     *pDFileSet;
  SDataFReader  *pReader;
  STsdbDataIter *iterList;  // list of iterators
  SRBTree        rtree;
  STsdbDataIter *pIter;
  SBlockData     bData;
  SSkmInfo       tbSkm;
} STsdbCompactor;

#define TSDB_FLG_DEEP_COMPACT 0x1

// ITER =========================
static int32_t tsdbDataIterNext(STsdbDataIter *pIter, TABLEID *pExcludeTableId);

static int32_t tsdbDataIterCmprFn(const SRBTreeNode *n1, const SRBTreeNode *n2) {
  const STsdbDataIter *pIter1 = (STsdbDataIter *)((char *)n1 - offsetof(STsdbDataIter, n));
  const STsdbDataIter *pIter2 = (STsdbDataIter *)((char *)n2 - offsetof(STsdbDataIter, n));

  return tRowInfoCmprFn(&pIter1->rowInfo, &pIter2->rowInfo);
}

static int32_t tsdbMemDIterOpen(STsdbDataIter **ppIter) {
  int32_t code = 0;
  int32_t lino = 0;

  STsdbDataIter *pIter = (STsdbDataIter *)taosMemoryCalloc(1, sizeof(*pIter) + sizeof(SMemDIter));
  if (pIter == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }

  // TODO

_exit:
  if (code) {
    *ppIter = NULL;
  } else {
    *ppIter = pIter;
  }
  return code;
}

static int32_t tsdbDataDIterOpen(SDataFReader *pReader, STsdbDataIter **ppIter) {
  int32_t code = 0;
  int32_t lino = 0;

  STsdbDataIter *pIter = (STsdbDataIter *)taosMemoryCalloc(1, sizeof(*pIter) + sizeof(SDataDIter));
  if (NULL == pIter) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }
  pIter->flag = TSDB_ITER_TYPE_DAT;

  SDataDIter *pDataDIter = (SDataDIter *)pIter->handle;
  pDataDIter->pReader = pReader;
  pDataDIter->aBlockIdx = taosArrayInit(0, sizeof(SBlockIdx));
  if (pDataDIter->aBlockIdx == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }

  code = tsdbReadBlockIdx(pReader, pDataDIter->aBlockIdx);
  TSDB_CHECK_CODE(code, lino, _exit);

  if (taosArrayGetSize(pDataDIter->aBlockIdx) == 0) goto _clear_exit;

  // TODO
  code = tBlockDataCreate(&pDataDIter->bData);
  TSDB_CHECK_CODE(code, lino, _exit);

  pDataDIter->iBlockIdx = -1;
  pDataDIter->iDataBlk = 0;
  pDataDIter->iRow = 0;

  code = tsdbDataIterNext(pIter, NULL);
  TSDB_CHECK_CODE(code, lino, _exit);

_exit:
  if (code) {
  _clear_exit:
    *ppIter = NULL;
    if (pIter) {
      tBlockDataDestroy(&pDataDIter->bData, 1);
      tMapDataClear(&pDataDIter->mDataBlk);
      taosArrayDestroy(pDataDIter->aBlockIdx);
      taosMemoryFree(pIter);
    }
  } else {
    *ppIter = pIter;
  }
  return code;
}

static int32_t tsdbSttDIterOpen(SDataFReader *pReader, int32_t iStt, STsdbDataIter **ppIter) {
  int32_t code = 0;
  int32_t lino = 0;

  STsdbDataIter *pIter = (STsdbDataIter *)taosMemoryCalloc(1, sizeof(*pIter) + sizeof(SSttDIter));
  if (pIter == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }
  pIter->flag = TSDB_ITER_TYPE_STT;

  SSttDIter *pSttDIter = (SSttDIter *)pIter->handle;
  pSttDIter->pReader = pReader;
  pSttDIter->iStt = iStt;
  pSttDIter->aSttBlk = taosArrayInit(0, sizeof(SSttBlk));
  if (pSttDIter->aSttBlk == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }

  code = tsdbReadSttBlk(pReader, pSttDIter->iStt, pSttDIter->aSttBlk);
  TSDB_CHECK_CODE(code, lino, _exit);

  if (taosArrayGetSize(pSttDIter->aSttBlk) == 0) goto _clear_exit;

  code = tBlockDataCreate(&pSttDIter->bData);
  TSDB_CHECK_CODE(code, lino, _exit);

  pSttDIter->iSttBlk = -1;
  pSttDIter->iRow = -1;

  code = tsdbDataIterNext(pIter, NULL);
  TSDB_CHECK_CODE(code, lino, _exit);

_exit:
  if (code) {
  _clear_exit:
    *ppIter = NULL;
    if (pIter) {
      tBlockDataDestroy(&pSttDIter->bData, 1);
      taosArrayDestroy(pSttDIter->aSttBlk);
      taosMemoryFree(pIter);
    }
  } else {
    *ppIter = pIter;
  }
  return code;
}

static void tsdbDataIterClose(STsdbDataIter *pIter) {
  // TODO
  ASSERT(0);
}

static int32_t tsdbDataIterNext(STsdbDataIter *pIter, TABLEID *pExcludeTableId) {
  int32_t code = 0;
  int32_t lino = 0;

  if (pIter->flag & TSDB_ITER_TYPE_MEM) {
    // TODO
    ASSERT(0);
  } else if (pIter->flag & TSDB_ITER_TYPE_DAT) {
    // TODO
    ASSERT(0);
  } else if (pIter->flag & TSDB_ITER_TYPE_STT) {
    SSttDIter *pSttDIter = (SSttDIter *)pIter->handle;

    for (;;) {
      if (++pSttDIter->iRow >= pSttDIter->bData.nRow) {
        for (;;) {
          if (++pSttDIter->iSttBlk < taosArrayGetSize(pSttDIter->aSttBlk)) {
            SSttBlk *pSttBlk = (SSttBlk *)taosArrayGet(pSttDIter->aSttBlk, pSttDIter->iSttBlk);

            // check exclusion
            if (pExcludeTableId) {
              if (pExcludeTableId->uid) {  // exclude (suid, uid)
                if (pSttBlk->minUid == pExcludeTableId->uid && pSttBlk->maxUid == pExcludeTableId->uid) continue;
              } else {  // exclude (suid, *)
                if (pSttBlk->suid == pExcludeTableId->suid) continue;
              }
            }

            code = tsdbReadSttBlockEx(pSttDIter->pReader, pSttDIter->iStt, pSttBlk, &pSttDIter->bData);
            TSDB_CHECK_CODE(code, lino, _exit);

            pIter->rowInfo.suid = pSttBlk->suid;
            pSttDIter->iRow = 0;
            break;
          } else {
            // iter end, all set 0 and exit
            pIter->rowInfo.suid = 0;
            pIter->rowInfo.uid = 0;
            goto _exit;
          }
        }
      }

      pIter->rowInfo.uid = pSttDIter->bData.uid ? pSttDIter->bData.uid : pSttDIter->bData.aUid[pSttDIter->iRow];
      pIter->rowInfo.row = tsdbRowFromBlockData(&pSttDIter->bData, pSttDIter->iRow);

      // check exclusion
      if (pExcludeTableId) {
        if (pExcludeTableId->uid) {  // exclude (suid, uid)
          if (pIter->rowInfo.uid == pExcludeTableId->uid) continue;
        } else {  // exclude (suid, *)
          if (pIter->rowInfo.suid == pExcludeTableId->suid) continue;
        }
      }

      break;
    }
  } else {
    ASSERT(0);
  }

_exit:
  return code;
}

// COMPACT =========================
static int32_t tsdbBeginCompact(STsdb *pTsdb, STsdbCompactor *pCompactor) {
  int32_t code = 0;
  int32_t lino = 0;

  pCompactor->pTsdb = pTsdb;
  // pCompactor->cid = 0; (TODO)
  pCompactor->maxRows = pTsdb->pVnode->config.tsdbCfg.maxRows;
  pCompactor->minRows = pTsdb->pVnode->config.tsdbCfg.minRows;

  code = tsdbFSCopy(pTsdb, &pCompactor->fs);
  TSDB_CHECK_CODE(code, lino, _exit);

  pCompactor->fid = INT32_MIN;

  code = tBlockDataCreate(&pCompactor->bData);
  TSDB_CHECK_CODE(code, lino, _exit);

_exit:
  if (code) {
    tsdbError("vgId:%d %s failed at line %d since %s", TD_VID(pTsdb->pVnode), __func__, lino, tstrerror(code));
  }
  return code;
}

static int32_t tsdbCommitCompact(STsdbCompactor *pCompactor) {
  int32_t code = 0;
  int32_t lino = 0;

  STsdb *pTsdb = pCompactor->pTsdb;

  // TODO

_exit:
  if (code) {
    tsdbError("vgId:%d %s failed at line %d since %s", TD_VID(pTsdb->pVnode), __func__, lino, tstrerror(code));
  }
  return code;
}

static int32_t tsdbAbortCompact(STsdbCompactor *pCompactor) {
  int32_t code = 0;
  int32_t lino = 0;

  STsdb *pTsdb = pCompactor->pTsdb;

  // TODO

_exit:
  if (code) {
    tsdbError("vgId:%d %s failed at line %d since %s", TD_VID(pTsdb->pVnode), __func__, lino, tstrerror(code));
  }
  return code;
}

static int32_t tsdbDeepCompact(STsdbCompactor *pCompactor) {
  int32_t code = 0;
  int32_t lino = 0;

  STsdb *pTsdb = pCompactor->pTsdb;

  code = tsdbDataFReaderOpen(&pCompactor->pReader, pTsdb, pCompactor->pDFileSet);
  TSDB_CHECK_CODE(code, lino, _exit);

_exit:
  if (code) {
    tsdbError("vgId:%d %s failed at line %d since %s", TD_VID(pTsdb->pVnode), __func__, lino, tstrerror(code));
  }
  return code;
}

static int32_t tsdbShallowCompact(STsdbCompactor *pCompactor) {
  int32_t code = 0;
  int32_t lino = 0;

  STsdb *pTsdb = pCompactor->pTsdb;

_exit:
  if (code) {
    tsdbError("vgId:%d %s failed at line %d since %s", TD_VID(pTsdb->pVnode), __func__, lino, tstrerror(code));
  }
  return code;
}

static int32_t tsdbCompactNextRowImpl(STsdbCompactor *pCompactor, TABLEID *pExcludeTableId) {
  int32_t code = 0;
  int32_t lino = 0;

  for (;;) {
    if (pCompactor->pIter) {
      code = tsdbDataIterNext(pCompactor->pIter, pExcludeTableId);
      TSDB_CHECK_CODE(code, lino, _exit);

      if (pCompactor->pIter->rowInfo.suid == 0 && pCompactor->pIter->rowInfo.uid == 0) {
        pCompactor->pIter = NULL;
      } else {
        SRBTreeNode *pNode = tRBTreeMin(&pCompactor->rtree);
        if (pNode) {
          STsdbDataIter *pIter = TSDB_DATA_ITER_FROM_RBTN(pNode);

          int32_t c = tRowInfoCmprFn(&pCompactor->pIter->rowInfo, &pIter->rowInfo);
          ASSERT(c);

          if (c > 0) {
            tRBTreePut(&pCompactor->rtree, &pCompactor->pIter->n);
            pCompactor->pIter = NULL;
          }
        }
      }
    }

    if (pCompactor->pIter == NULL) {
      SRBTreeNode *pNode = tRBTreeMin(&pCompactor->rtree);
      if (pNode) {
        pCompactor->pIter = TSDB_DATA_ITER_FROM_RBTN(pNode);
        tRBTreeDrop(&pCompactor->rtree, pNode);

        if (pExcludeTableId) {
          if (pExcludeTableId->uid) {
            if (pCompactor->pIter->rowInfo.uid == pExcludeTableId->uid) continue;
          } else {
            if (pCompactor->pIter->rowInfo.suid == pExcludeTableId->suid) continue;
          }
        }
      }
    }

    break;
  }

_exit:
  return code;
}

static int32_t tsdbCompactNextRow(STsdbCompactor *pCompactor) {
  int32_t code = 0;
  int32_t lino = 0;

  TABLEID  excludeTableId;
  TABLEID *pExcludeTableId = NULL;

  for (;;) {
    code = tsdbCompactNextRowImpl(pCompactor, pExcludeTableId);
    TSDB_CHECK_CODE(code, lino, _exit);

    // check if the table of the row exists
    if (pCompactor->pIter) {
      SRowInfo *pRowInfo = &pCompactor->pIter->rowInfo;

      // Table exists, just break out
      if (pRowInfo->uid == pCompactor->tbSkm.uid) break;

      SMetaInfo info;
      if (pRowInfo->suid) {  // child table

        // check if super table exists
        if (pRowInfo->suid != pCompactor->tbSkm.suid) {
          if (metaGetInfo(pCompactor->pTsdb->pVnode->pMeta, pRowInfo->uid, &info, NULL) != TSDB_CODE_SUCCESS) {
            excludeTableId.suid = pRowInfo->suid;
            excludeTableId.uid = 0;
            pExcludeTableId = &excludeTableId;
            continue;
          }

          // super table exists
          pCompactor->tbSkm.suid = pRowInfo->suid;
          pCompactor->tbSkm.uid = 0;
          tDestroyTSchema(pCompactor->tbSkm.pTSchema);
          pCompactor->tbSkm.pTSchema = metaGetTbTSchema(pCompactor->pTsdb->pVnode->pMeta, pRowInfo->suid, -1, 1);
          if (pCompactor->tbSkm.pTSchema == NULL) {
            code = TSDB_CODE_OUT_OF_MEMORY;
            TSDB_CHECK_CODE(code, lino, _exit);
          }
        }

        // check if table exists
        if (metaGetInfo(pCompactor->pTsdb->pVnode->pMeta, pRowInfo->uid, &info, NULL) != TSDB_CODE_SUCCESS) {
          excludeTableId.suid = pRowInfo->suid;
          excludeTableId.uid = pRowInfo->uid;
          pExcludeTableId = &excludeTableId;
          continue;
        }

        // table exists
        pCompactor->tbSkm.uid = pRowInfo->uid;
      } else {  // normal table
        // check if table exists
        if (metaGetInfo(pCompactor->pTsdb->pVnode->pMeta, pRowInfo->uid, &info, NULL) != TSDB_CODE_SUCCESS) {
          excludeTableId.suid = pRowInfo->suid;
          excludeTableId.uid = pRowInfo->uid;
          pExcludeTableId = &excludeTableId;
          continue;
        }

        // table exists
        pCompactor->tbSkm.suid = pRowInfo->suid;
        pCompactor->tbSkm.uid = pRowInfo->uid;
        tDestroyTSchema(pCompactor->tbSkm.pTSchema);

        pCompactor->tbSkm.pTSchema = metaGetTbTSchema(pCompactor->pTsdb->pVnode->pMeta, pRowInfo->suid, -1, 1);
        if (pCompactor->tbSkm.pTSchema == NULL) {
          code = TSDB_CODE_OUT_OF_MEMORY;
          TSDB_CHECK_CODE(code, lino, _exit);
        }
      }

      break;
    } else {
      // iter end, just break out
      break;
    }
  }

_exit:
  if (code) {
    tsdbError("vgId:%d %s failed at line %d since %s", TD_VID(pCompactor->pTsdb->pVnode), __func__, lino,
              tstrerror(code));
  }
  return code;
}

static int32_t tsdbCompactGetRow(STsdbCompactor *pCompactor, SRowInfo **ppRowInfo, STSchema **ppTSchema) {
  int32_t code = 0;
  int32_t lino = 0;

  if (pCompactor->pIter == NULL) {
    code = tsdbCompactNextRow(pCompactor);
    TSDB_CHECK_CODE(code, lino, _exit);
  }

  if (pCompactor->pIter) {
    ASSERT(pCompactor->pIter->rowInfo.suid == pCompactor->tbSkm.suid);
    ASSERT(pCompactor->pIter->rowInfo.uid == pCompactor->tbSkm.uid);
    *ppRowInfo = &pCompactor->pIter->rowInfo;
    *ppTSchema = pCompactor->tbSkm.pTSchema;
  } else {
    *ppRowInfo = NULL;
    *ppTSchema = NULL;
  }

_exit:
  return code;
}

static int32_t tsdbOpenCompactor(STsdbCompactor *pCompactor) {
  int32_t code = 0;
  int32_t lino = 0;

  STsdb *pTsdb = pCompactor->pTsdb;

  // next compact file
  pCompactor->pDFileSet = (SDFileSet *)taosArraySearch(pCompactor->fs.aDFileSet, &(SDFileSet){.fid = pCompactor->fid},
                                                       tDFileSetCmprFn, TD_GT);
  if (pCompactor->pDFileSet == NULL) goto _exit;

  pCompactor->fid = pCompactor->pDFileSet->fid;

  code = tsdbDataFReaderOpen(&pCompactor->pReader, pTsdb, pCompactor->pDFileSet);
  TSDB_CHECK_CODE(code, lino, _exit);

  // open iters
  STsdbDataIter *pIter;

  pCompactor->iterList = NULL;
  tRBTreeCreate(&pCompactor->rtree, tsdbDataIterCmprFn);

  code = tsdbDataDIterOpen(pCompactor->pReader, &pIter);
  TSDB_CHECK_CODE(code, lino, _exit);

  if (pIter) {
    pIter->next = pCompactor->iterList;
    pCompactor->iterList = pIter;
    tRBTreePut(&pCompactor->rtree, &pIter->n);
  }

  for (int32_t iStt = 0; iStt < pCompactor->pReader->pSet->nSttF; iStt++) {
    code = tsdbSttDIterOpen(pCompactor->pReader, iStt, &pIter);
    TSDB_CHECK_CODE(code, lino, _exit);

    if (pIter) {
      pIter->next = pCompactor->iterList;
      pCompactor->iterList = pIter;
      tRBTreePut(&pCompactor->rtree, &pIter->n);
    }
  }
  pCompactor->pIter = NULL;
  tBlockDataReset(&pCompactor->bData);

_exit:
  if (code) {
    tsdbError("vgId:%d %s failed at line %d since %s", TD_VID(pTsdb->pVnode), __func__, lino, tstrerror(code));
  } else {
    tsdbDebug("vgId:%d %s done", TD_VID(pTsdb->pVnode), __func__);
  }
  return code;
}

static void tsdbCloseCompactor(STsdbCompactor *pCompactor) {
  STsdb *pTsdb = pCompactor->pTsdb;

  for (STsdbDataIter *pIter = pCompactor->iterList; pIter;) {
    STsdbDataIter *pIterNext = pIter->next;
    tsdbDataIterClose(pIter);
    pIter = pIterNext;
  }

  // TODO
  ASSERT(0);

_exit:
  tsdbDebug("vgId:%d %s done", TD_VID(pTsdb->pVnode), __func__);
}

int32_t tsdbCompact(STsdb *pTsdb, int32_t flag) {
  int32_t code = 0;
  int32_t lino = 0;

  // Check if can do compact (TODO)

  // Do compact
  STsdbCompactor *pCompactor = &(STsdbCompactor){0};

  code = tsdbBeginCompact(pTsdb, pCompactor);
  TSDB_CHECK_CODE(code, lino, _exit);

  while (true) {
    code = tsdbOpenCompactor(pCompactor);
    TSDB_CHECK_CODE(code, lino, _exit);

    if (pCompactor->pDFileSet == NULL) break;

    // loop to merge row by row
    SRowInfo *pRowInfo = NULL;
    STSchema *pTSchema = NULL;
    int64_t   nRow = 0;
    for (;;) {
      code = tsdbCompactGetRow(pCompactor, &pRowInfo, &pTSchema);
      TSDB_CHECK_CODE(code, lino, _exit);

      if (pRowInfo == NULL) break;

      nRow++;

      if (pCompactor->bData.suid == 0 && pCompactor->bData.uid == 0) {  // init the block data if not initialized yet
        code = tBlockDataInit(&pCompactor->bData, &(TABLEID){.suid = pRowInfo->suid, .uid = pRowInfo->uid}, pTSchema,
                              NULL, 0);
        TSDB_CHECK_CODE(code, lino, _exit);
      } else {
        if (pCompactor->bData.suid != pRowInfo->suid) {  // not same super table
          if (pCompactor->bData.nRow < pCompactor->minRows) {
            // TODO: write block data to .stt file, need to check if nRow is 0
            tBlockDataClear(&pCompactor->bData);
          } else {
            // TODO: write block data to .data file, need to check if nRow is 0
            tBlockDataClear(&pCompactor->bData);
          }

          code = tBlockDataInit(&pCompactor->bData, &(TABLEID){.suid = pRowInfo->suid, .uid = pRowInfo->uid}, pTSchema,
                                NULL, 0);
          TSDB_CHECK_CODE(code, lino, _exit);
        } else if (pCompactor->bData.uid != pRowInfo->uid) {
          if (pRowInfo->suid) {  // different child table
            if (pCompactor->bData.nRow > pCompactor->minRows) {
              // TODO
            }
          } else {  // different normal table
            if (pCompactor->bData.nRow < pCompactor->minRows) {
              // TODO: write data to .stt file, need to check if nRow is 0
              tBlockDataClear(&pCompactor->bData);
            } else {
              // TODO: write data to .data file, need to check if nRow is 0
              tBlockDataClear(&pCompactor->bData);
            }

            code = tBlockDataInit(&pCompactor->bData, &(TABLEID){.suid = pRowInfo->suid, .uid = pRowInfo->uid},
                                  pTSchema, NULL, 0);
            TSDB_CHECK_CODE(code, lino, _exit);
          }
        }
      }

      // append row to block data
      code = tBlockDataAppendRow(&pCompactor->bData, &pRowInfo->row, pTSchema, pRowInfo->uid);
      TSDB_CHECK_CODE(code, lino, _exit);

      // check if block data is full
      if (pCompactor->bData.nRow >= pCompactor->maxRows) {
        tBlockDataClear(&pCompactor->bData);
      }

      // iterate to next row
      code = tsdbCompactNextRow(pCompactor);
      TSDB_CHECK_CODE(code, lino, _exit);
    }

    tsdbCloseCompactor(pCompactor);
  }

_exit:
  if (code) {
    tsdbError("vgId:%d %s failed at line %d since %s", TD_VID(pTsdb->pVnode), __func__, lino, tstrerror(code));
    tsdbAbortCompact(pCompactor);
  } else {
    tsdbCommitCompact(pCompactor);
  }
  return code;
}
