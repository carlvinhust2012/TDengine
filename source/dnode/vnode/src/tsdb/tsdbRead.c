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
#define ASCENDING_TRAVERSE(o)      (o == TSDB_ORDER_ASC)
#define QH_GET_NUM_OF_COLS(handle) ((size_t)(taosArrayGetSize((handle)->pResBlock->pDataBlock)))

#define GET_FILE_DATA_BLOCK_INFO(_checkInfo, _block)                                      \
  ((SDataBlockInfo){.window = {.skey = (_block)->minKey.ts, .ekey = (_block)->maxKey.ts}, \
                    .rows = (_block)->numOfRows,                                          \
                    .uid = (_checkInfo)->tableId})

typedef struct SQueryFilePos {
  int32_t     fid;
  int32_t     slot;
  int32_t     pos;
  int64_t     lastKey;
  int32_t     rows;
  bool        composedBlock;
  bool        blockCompleted;
  STimeWindow win;
} SQueryFilePos;

typedef struct SDataBlockLoadInfo {
  SDFileSet* fileGroup;
  int32_t    slot;
  uint64_t   uid;
  SArray*    pLoadedCols;
} SDataBlockLoadInfo;

typedef struct STableBlockScanInfo {
  uint64_t     uid;
  TSKEY        lastKey;
  SBlockIdx    blockIdx;
  SArray*      pBlockList;        // block data index list
  bool         iterInit;       // whether to initialize the in-memory skip list iterator or not
  STbDataIter* iter;              // mem buffer skip list iterator
  STbDataIter* iiter;             // imem buffer skip list iterator
  bool         memHasVal;
  bool         imemHasVal;
} STableBlockScanInfo;

typedef struct SBlockOrderWrapper {
  int64_t uid;
  SBlock* pBlock;
} SBlockOrderWrapper;

typedef struct SBlockOrderSupporter {
  SBlockOrderWrapper**  pDataBlockInfo;
  int32_t*          indexPerTable;
  int32_t*          numOfBlocksPerTable;
  int32_t           numOfTables;
} SBlockOrderSupporter;

typedef struct SIOCostSummary {
  int64_t           blockLoadTime;
  int64_t           statisInfoLoadTime;
  int64_t           checkForNextTime;
  int64_t           headFileLoad;
  int64_t           headFileLoadTime;
} SIOCostSummary;

typedef struct SBlockLoadSuppInfo {
  SColumnDataAgg*   pstatis;
  SColumnDataAgg**  plist;
  int16_t*          colIds;      // column ids for loading file block data
  int32_t*          slotIds;     // colId to slotId
  char**            buildBuf;    // build string tmp buffer, todo remove it later after all string format being updated.
} SBlockLoadSuppInfo;

typedef struct SFileSetIter {
  int32_t           numOfFiles;  // number of total files
  int32_t           index;       // current accessed index in the list
  SArray*           pFileList;   // data file list
} SFileSetIter;

typedef struct SFileDataBlockInfo {
  int32_t tbBlockIdx;   // index position in STableBlockScanInfo in order to check whether neighbor block overlaps with it
  int64_t uid;
} SFileDataBlockInfo;

typedef struct SDataBlockIter {
  int32_t      numOfBlocks;
  int32_t      index;
  SArray*      blockList;   // SArray<SFileDataBlockInfo>
} SDataBlockIter;

typedef struct SFileBlockDumpInfo {
  int32_t      totalRows;
  int32_t      rowIndex;
  int64_t      lastKey;
} SFileBlockDumpInfo;

typedef struct SVersionRange {
  uint64_t minVer;
  uint64_t maxVer;
} SVersionRange;

typedef struct SReaderStatus {
  SQueryFilePos      cur;             // current position
  bool               loadFromFile;    // check file stage
  SHashObj*          pTableMap;       // SHash<STableBlockScanInfo>
  STableBlockScanInfo* pTableIter;    // table iterator used in building in-memory buffer data blocks.
  SFileBlockDumpInfo fBlockDumpInfo;

  SDFileSet*         pCurrentFileSet; // current opened file set
  SBlockData         fileBlockData;
  SFileSetIter       fileIter;
  SDataBlockIter     blockIter;
  bool               composedDataBlock;// the returned data block is a composed block or not
} SReaderStatus;

struct STsdbReader {
  STsdb*             pTsdb;
  uint64_t           suid;
  int16_t            order;
  STimeWindow        window;  // the primary query time window that applies to all queries
  SSDataBlock*       pResBlock;
  int32_t            capacity;
  SReaderStatus      status;
  char*              idStr;   // query info handle, for debug purpose
  int32_t            type;    // query type: 1. retrieve all data blocks, 2. retrieve direct prev|next rows
  SBlockLoadSuppInfo suppInfo;
  SArray*            prev;    // previous row which is before than time window
  SArray*            next;    // next row which is after the query time window
  SIOCostSummary     cost;
  STSchema*          pSchema;

  SDataFReader*      pFileReader;
  SVersionRange      verRange;
#if 0
  SFileBlockInfo*   pDataBlockInfo;
  SDataCols*         pDataCols;         // in order to hold current file data block
  int32_t            allocSize;         // allocated data block size
  SDataBlockLoadInfo dataBlockLoadInfo; /* record current block load information */
  SLoadCompBlockInfo compBlockLoadInfo; /* record current compblock information in SQueryAttr */
  //  SDFileSet* pFileGroup;
  // SFSIter            fileIter;
  // SReadH             rhelper;
  //  SColumnDataAgg* statis;  // query level statistics, only one table block statistics info exists at any time
  //  SColumnDataAgg** pstatis;// the ptr array list to return to caller
#endif
};

static SFileDataBlockInfo* getCurrentBlockInfo(SDataBlockIter* pBlockIter);
static int buildInmemDataBlockImpl(STableBlockScanInfo* pBlockScanInfo, TSDBKEY maxKey, int32_t capacity, STsdbReader* pReader);
static TSDBROW* getValidRow(STbDataIter* pIter, bool* hasVal, STsdbReader* pReader);
static int32_t doLoadRowsOfIdenticalTsInFileBlock(SFileDataBlockInfo* pFBlock, SBlock* pBlock, SBlockData* pBlockData,
                                                  STableBlockScanInfo* pScanInfo, STsdbReader* pReader, SRowMerger* pMerger);
static int32_t doLoadRowsOfIdenticalTs(STbDataIter *pIter, bool* hasVal, int64_t ts, SRowMerger* pMerger, STsdbReader* pReader);
static int32_t doAppendOneRow(SSDataBlock* pBlock, STsdbReader* pReader, STSRow* pTSRow);
static void setComposedBlockFlag(STsdbReader* pReader, bool composed);

// static void tsdbInitDataBlockLoadInfo(SDataBlockLoadInfo* pBlockLoadInfo) {
//   pBlockLoadInfo->slot = -1;
//   pBlockLoadInfo->uid = 0;
//   pBlockLoadInfo->fileGroup = NULL;
// }

// static void tsdbInitCompBlockLoadInfo(SLoadCompBlockInfo* pCompBlockLoadInfo) {
//   pCompBlockLoadInfo->tid = -1;
//   pCompBlockLoadInfo->fileId = -1;
// }

static int32_t setColumnIdSlotList(STsdbReader* pReader, SSDataBlock* pBlock) {
  SBlockLoadSuppInfo* pSupInfo = &pReader->suppInfo;

  size_t numOfCols = blockDataGetNumOfCols(pBlock);
//  pSupInfo->slotIds = taosMemoryCalloc(numOfCols, sizeof(int16_t));
//  if (pSupInfo->slotIds == NULL) {
//    return TSDB_CODE_OUT_OF_MEMORY;
//  }

  pSupInfo->colIds = taosMemoryMalloc(numOfCols * sizeof(int16_t));
  pSupInfo->buildBuf = taosMemoryCalloc(numOfCols, POINTER_BYTES);
  if (pSupInfo->buildBuf == NULL || pSupInfo->colIds == NULL) {
    taosMemoryFree(pSupInfo->colIds);
    taosMemoryFree(pSupInfo->buildBuf);
    return TSDB_CODE_OUT_OF_MEMORY;
  }

//  STSchema* pSchema = pReader->pSchema;
  for (int32_t i = 0; i < numOfCols; ++i) {
    SColumnInfoData* pCol = taosArrayGet(pBlock->pDataBlock, i);
    pSupInfo->colIds[i] = pCol->info.colId;

//    for (int32_t j = 0; j < pSchema->numOfCols; ++j) {
//      if (pCol->info.colId == pSchema->columns[j].colId) {
//        pSupInfo->slotIds[i] = j;
//        break;
//      }
//    }

    if (IS_VAR_DATA_TYPE(pCol->info.type)) {
      pSupInfo->buildBuf[i] = taosMemoryMalloc(pCol->info.bytes);
    }
  }

  return TSDB_CODE_SUCCESS;
}

static SHashObj* createDataBlockScanInfo(STsdbReader* pTsdbReader, const STableKeyInfo* idList, int32_t numOfTables) {
   ASSERT(numOfTables >= 1);

   // allocate buffer in order to load data blocks from file
   // todo use simple hash instead
   SHashObj* pTableMap = taosHashInit(numOfTables, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), false, HASH_NO_LOCK);
   if (pTableMap == NULL) {
     return NULL;
   }

   // todo apply the lastkey of table check to avoid to load header file
   for (int32_t j = 0; j < numOfTables; ++j) {
     STableBlockScanInfo info = {.lastKey = 0, .uid = idList[j].uid};
     if (ASCENDING_TRAVERSE(pTsdbReader->order)) {
       if (info.lastKey == INT64_MIN || info.lastKey < pTsdbReader->window.skey) {
         info.lastKey = pTsdbReader->window.skey;
       }

       ASSERT(info.lastKey >= pTsdbReader->window.skey && info.lastKey <= pTsdbReader->window.ekey);
     } else {
       info.lastKey = pTsdbReader->window.skey;
     }

     taosHashPut(pTableMap, &info.uid, sizeof(uint64_t), &info, sizeof(info));
     tsdbDebug("%p check table uid:%" PRId64 " from lastKey:%" PRId64 " %s", pTsdbReader, info.uid, info.lastKey,
               pTsdbReader->idStr);
   }

   return pTableMap;
}

// static void resetCheckInfo(STsdbReader* pTsdbReadHandle) {
//   size_t numOfTables = taosArrayGetSize(pTsdbReadHandle->pTableCheckInfo);
//   assert(numOfTables >= 1);

//   // todo apply the lastkey of table check to avoid to load header file
//   for (int32_t i = 0; i < numOfTables; ++i) {
//     STableBlockScanInfo* pCheckInfo = (STableBlockScanInfo*)taosArrayGet(pTsdbReadHandle->pTableCheckInfo, i);
//     pCheckInfo->lastKey = pTsdbReadHandle->window.skey;
//     pCheckInfo->iter = tsdbTbDataIterDestroy(pCheckInfo->iter);
//     pCheckInfo->iiter = tsdbTbDataIterDestroy(pCheckInfo->iiter);
//     pCheckInfo->initBuf = false;

//     if (ASCENDING_TRAVERSE(pTsdbReadHandle->order)) {
//       assert(pCheckInfo->lastKey >= pTsdbReadHandle->window.skey);
//     } else {
//       assert(pCheckInfo->lastKey <= pTsdbReadHandle->window.skey);
//     }
//   }
// }

// // only one table, not need to sort again
// static SArray* createCheckInfoFromCheckInfo(STableBlockScanInfo* pCheckInfo, TSKEY skey, SArray** psTable) {
//   SArray* pNew = taosArrayInit(1, sizeof(STableBlockScanInfo));

//   STableBlockScanInfo info = {.lastKey = skey};

//   info.tableId = pCheckInfo->tableId;
//   taosArrayPush(pNew, &info);
//   return pNew;
// }

static bool isEmptyQueryTimeWindow(STimeWindow* pWindow, int32_t order) {
   ASSERT(pWindow != NULL);
   bool asc = ASCENDING_TRAVERSE(order);
   return ((asc && pWindow->skey > pWindow->ekey) || (!asc && pWindow->ekey > pWindow->skey));
 }

// // Update the query time window according to the data time to live(TTL) information, in order to avoid to return
// // the expired data to client, even it is queried already.
// static int64_t getEarliestValidTimestamp(STsdb* pTsdb) {
//   STsdbKeepCfg* pCfg = &pTsdb->keepCfg;

//   int64_t now = taosGetTimestamp(pCfg->precision);
//   return now - (tsTickPerMin[pCfg->precision] * pCfg->keep2) + 1;  // needs to add one tick
// }

// todo remove this
static void setQueryTimewindow(STsdbReader* pReader, SQueryTableDataCond* pCond, int32_t tWinIdx) {
  // pReader->window = pCond->twindows[tWinIdx];

  // bool    updateTs = false;
  // int64_t startTs = getEarliestValidTimestamp(pReader->pTsdb);
  // if (ASCENDING_TRAVERSE(pReader->order)) {
  //   if (startTs > pReader->window.skey) {
  //     pReader->window.skey = startTs;
  //     pCond->twindows[tWinIdx].skey = startTs;
  //     updateTs = true;
  //   }
  // } else {
  //   if (startTs > pReader->window.ekey) {
  //     pReader->window.ekey = startTs;
  //     pCond->twindows[tWinIdx].ekey = startTs;
  //     updateTs = true;
  //   }
  // }

  // if (updateTs) {
  //   tsdbDebug("%p update the query time window, old:%" PRId64 " - %" PRId64 ", new:%" PRId64 " - %" PRId64 ", %s",
  //             pReader, pCond->twindows[tWinIdx].skey, pCond->twindows[tWinIdx].ekey, pReader->window.skey,
  //             pReader->window.ekey, pReader->idStr);
  // }
}

static void limitOutputBufferSize(const SQueryTableDataCond* pCond, int32_t* capacity) {
  int32_t rowLen = 0;
  for (int32_t i = 0; i < pCond->numOfCols; ++i) {
    rowLen += pCond->colList[i].bytes;
  }

  // make sure the output SSDataBlock size be less than 2MB.
  const int32_t TWOMB = 2 * 1024 * 1024;
  if ((*capacity) * rowLen > TWOMB) {
    (*capacity) = TWOMB / rowLen;
  }
}

// init file iterator
static int32_t initFileIterator(SFileSetIter* pIter, const STsdbFSState* pFState, const char* idstr) {
  pIter->index = -1;
  pIter->numOfFiles = taosArrayGetSize(pFState->aDFileSet);
  pIter->pFileList = taosArrayDup(pFState->aDFileSet);

  tsdbDebug("init fileset iterator, total files:%d %s", pIter->numOfFiles, idstr);
  return TSDB_CODE_SUCCESS;
}

static bool filesetIteratorNext(SFileSetIter* pIter, int32_t order, STsdbReader* pReader) {
  pIter->index += 1;
  if (pIter->index >= pIter->numOfFiles) {
    return false;
  }

  // check file the time range of coverage
  STimeWindow win = {0};
  pReader->status.pCurrentFileSet = (SDFileSet *)taosArrayGet(pIter->pFileList, pIter->index);

  int32_t code = tsdbDataFReaderOpen(&pReader->pFileReader, pReader->pTsdb, pReader->status.pCurrentFileSet);
  if (code != TSDB_CODE_SUCCESS) {
    goto _err;
  }

  // todo file range check
// tsdbGetFidKeyRange(pCfg->days, pCfg->precision, pTsdbReadHandle->pFileGroup->fid, &win.skey, &win.ekey);

 // current file are not overlapped with query time window, ignore remain files
 if ((ASCENDING_TRAVERSE(order) && win.skey > pReader->window.ekey) ||
     (!ASCENDING_TRAVERSE(order) && win.ekey < pReader->window.ekey)) {
   tsdbDebug("%p remain files are not qualified for qrange:%" PRId64 "-%" PRId64 ", ignore, %s", pReader,
             pReader->window.skey, pReader->window.ekey, pReader->idStr);
   return false;
 }

 return true;

  _err:
  return false;
}

static void resetDataBlockIterator(SDataBlockIter* pIter) {
  pIter->index = -1;
  pIter->numOfBlocks = -1;
  pIter->blockList = taosArrayInit(4, sizeof(SFileDataBlockInfo));
}

static void initReaderStatus(SReaderStatus* pStatus) {
  pStatus->cur.fid        = INT32_MIN;
  pStatus->cur.win        = TSWINDOW_INITIALIZER;
  pStatus->pTableIter     = NULL;
  pStatus->loadFromFile   = true;
}

static int32_t tsdbReaderCreate(SVnode* pVnode, SQueryTableDataCond* pCond, STsdbReader** ppReader, const char* idstr) {
  int32_t      code = 0;
  STsdbReader* pReader = (STsdbReader*)taosMemoryCalloc(1, sizeof(*pReader));
  if (pReader == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _end;
  }

  initReaderStatus(&pReader->status);

  pReader->pTsdb       = pVnode->pTsdb;
  pReader->suid        = pCond->suid;
  pReader->order       = pCond->order;
  pReader->capacity    = 4096;
  pReader->idStr       = strdup(idstr);
  pReader->verRange.minVer= pCond->startVersion;
  pReader->verRange.maxVer  = 100000;//pCond->endVersion;  // todo for test purpose
  pReader->type        = pCond->type;
  pReader->window      = *pCond->twindows;

  if (pReader->suid != 0) {
    pReader->pSchema = metaGetTbTSchema(pReader->pTsdb->pVnode->pMeta, pReader->suid, -1);
    ASSERT(pReader->pSchema);
  }

  // todo remove this
  setQueryTimewindow(pReader, pCond, 0);

  if (pCond->numOfCols > 0) {
    limitOutputBufferSize(pCond, &pReader->capacity);

    // allocate buffer in order to load data blocks from file
    pReader->suppInfo.pstatis = taosMemoryCalloc(pCond->numOfCols, sizeof(SColumnDataAgg));
    if (pReader->suppInfo.pstatis == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _end;
    }

    pReader->pResBlock = createDataBlock();
    for (int32_t i = 0; i < pCond->numOfCols; ++i) {
      SColumnInfoData colInfo = {{0}, 0};
      colInfo.info = pCond->colList[i];
      blockDataAppendColInfo(pReader->pResBlock, &colInfo);
    }

    blockDataEnsureCapacity(pReader->pResBlock, pReader->capacity);

    setColumnIdSlotList(pReader, pReader->pResBlock);
    pReader->suppInfo.plist = taosMemoryCalloc(pCond->numOfCols, POINTER_BYTES);
  }

  STsdbFSState* pFState = pReader->pTsdb->fs->cState;
  initFileIterator(&pReader->status.fileIter, pFState, pReader->idStr);
  resetDataBlockIterator(&pReader->status.blockIter);

  // no data in files, let's try buffer in memory
  if (pReader->status.fileIter.numOfFiles == 0) {
    pReader->status.loadFromFile = false;
  }

  *ppReader = pReader;
  return code;

_end:
  tsdbReaderClose(pReader);
  *ppReader = NULL;
  return code;
}

// void tsdbResetQueryHandleForNewTable(STsdbReader* queryHandle, SQueryTableDataCond* pCond, STableListInfo* tableList,
//                                      int32_t tWinIdx) {
//   STsdbReader* pTsdbReadHandle = queryHandle;

//   pTsdbReadHandle->order = pCond->order;
//   pTsdbReadHandle->window = pCond->twindows[tWinIdx];
//   pTsdbReadHandle->type = TSDB_QUERY_TYPE_ALL;
//   pTsdbReadHandle->cur.fid = -1;
//   pTsdbReadHandle->cur.win = TSWINDOW_INITIALIZER;
//   pTsdbReadHandle->checkFiles = true;
//   pTsdbReadHandle->activeIndex = 0;  // current active table index
//   pTsdbReadHandle->locateStart = false;
//   pTsdbReadHandle->loadExternalRow = pCond->loadExternalRows;

//   if (ASCENDING_TRAVERSE(pCond->order)) {
//     assert(pTsdbReadHandle->window.skey <= pTsdbReadHandle->window.ekey);
//   } else {
//     assert(pTsdbReadHandle->window.skey >= pTsdbReadHandle->window.ekey);
//   }

//   // allocate buffer in order to load data blocks from file
//   memset(pTsdbReadHandle->suppInfo.pstatis, 0, sizeof(SColumnDataAgg));
//   memset(pTsdbReadHandle->suppInfo.plist, 0, POINTER_BYTES);

//   tsdbInitDataBlockLoadInfo(&pTsdbReadHandle->dataBlockLoadInfo);
//   tsdbInitCompBlockLoadInfo(&pTsdbReadHandle->compBlockLoadInfo);

//   SArray* pTable = NULL;
//   //  STsdbMeta* pMeta = tsdbGetMeta(pTsdbReadHandle->pTsdb);

//   //  pTsdbReadHandle->pTableCheckInfo = destroyTableCheckInfo(pTsdbReadHandle->pTableCheckInfo);

//   pTsdbReadHandle->pTableCheckInfo = NULL;  // createDataBlockScanInfo(pTsdbReadHandle, groupList, pMeta,
//                                             // &pTable);
//   if (pTsdbReadHandle->pTableCheckInfo == NULL) {
//     //    tsdbReaderClose(pTsdbReadHandle);
//     terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
//   }

//   //  pTsdbReadHandle->prev = doFreeColumnInfoData(pTsdbReadHandle->prev);
//   //  pTsdbReadHandle->next = doFreeColumnInfoData(pTsdbReadHandle->next);
// }

// SArray* tsdbGetQueriedTableList(STsdbReader** pHandle) {
//   assert(pHandle != NULL);

//   STsdbReader* pTsdbReadHandle = (STsdbReader*)pHandle;

//   size_t  size = taosArrayGetSize(pTsdbReadHandle->pTableCheckInfo);
//   SArray* res = taosArrayInit(size, POINTER_BYTES);
//   return res;
// }

// static TSKEY extractFirstTraverseKey(STableBlockScanInfo* pCheckInfo, int32_t order, int32_t update, TDRowVerT maxVer) {
//   TSDBROW row = {0};
//   STSRow *rmem = NULL, *rimem = NULL;

//   if (pCheckInfo->iter) {
//     if (tsdbTbDataIterGet(pCheckInfo->iter, &row)) {
//       rmem = row.pTSRow;
//     }
//   }

//   if (pCheckInfo->iiter) {
//     if (tsdbTbDataIterGet(pCheckInfo->iiter, &row)) {
//       rimem = row.pTSRow;
//     }
//   }

//   if (rmem == NULL && rimem == NULL) {
//     return TSKEY_INITIAL_VAL;
//   }

//   if (rmem != NULL && rimem == NULL) {
//     pCheckInfo->chosen = CHECKINFO_CHOSEN_MEM;
//     return TD_ROW_KEY(rmem);
//   }

//   if (rmem == NULL && rimem != NULL) {
//     pCheckInfo->chosen = CHECKINFO_CHOSEN_IMEM;
//     return TD_ROW_KEY(rimem);
//   }

//   TSKEY r1 = TD_ROW_KEY(rmem);
//   TSKEY r2 = TD_ROW_KEY(rimem);

//   if (r1 == r2) {
//     if (TD_SUPPORT_UPDATE(update)) {
//       pCheckInfo->chosen = CHECKINFO_CHOSEN_BOTH;
//     } else {
//       pCheckInfo->chosen = CHECKINFO_CHOSEN_IMEM;
//       tsdbTbDataIterNext(pCheckInfo->iter);
//     }
//     return r1;
//   } else if (r1 < r2 && ASCENDING_TRAVERSE(order)) {
//     pCheckInfo->chosen = CHECKINFO_CHOSEN_MEM;
//     return r1;
//   } else {
//     pCheckInfo->chosen = CHECKINFO_CHOSEN_IMEM;
//     return r2;
//   }
// }

// static STSRow* getSRowInTableMem(STableBlockScanInfo* pCheckInfo, int32_t order, int32_t update, STSRow** extraRow,
//                                  TDRowVerT maxVer) {
//   TSDBROW row;
//   STSRow *rmem = NULL, *rimem = NULL;
//   if (pCheckInfo->iter) {
//     if (tsdbTbDataIterGet(pCheckInfo->iter, &row)) {
//       rmem = row.pTSRow;
//     }
//   }

//   if (pCheckInfo->iiter) {
//     if (tsdbTbDataIterGet(pCheckInfo->iiter, &row)) {
//       rimem = row.pTSRow;
//     }
//   }

//   if (rmem == NULL && rimem == NULL) {
//     return NULL;
//   }

//   if (rmem != NULL && rimem == NULL) {
//     pCheckInfo->chosen = 0;
//     return rmem;
//   }

//   if (rmem == NULL && rimem != NULL) {
//     pCheckInfo->chosen = 1;
//     return rimem;
//   }

//   TSKEY r1 = TD_ROW_KEY(rmem);
//   TSKEY r2 = TD_ROW_KEY(rimem);

//   if (r1 == r2) {
//     if (TD_SUPPORT_UPDATE(update)) {
//       pCheckInfo->chosen = CHECKINFO_CHOSEN_BOTH;
//       *extraRow = rimem;
//       return rmem;
//     } else {
//       tsdbTbDataIterNext(pCheckInfo->iter);
//       pCheckInfo->chosen = CHECKINFO_CHOSEN_IMEM;
//       return rimem;
//     }
//   } else {
//     if (ASCENDING_TRAVERSE(order)) {
//       if (r1 < r2) {
//         pCheckInfo->chosen = CHECKINFO_CHOSEN_MEM;
//         return rmem;
//       } else {
//         pCheckInfo->chosen = CHECKINFO_CHOSEN_IMEM;
//         return rimem;
//       }
//     } else {
//       if (r1 < r2) {
//         pCheckInfo->chosen = CHECKINFO_CHOSEN_IMEM;
//         return rimem;
//       } else {
//         pCheckInfo->chosen = CHECKINFO_CHOSEN_IMEM;
//         return rmem;
//       }
//     }
//   }
// }

// static bool moveToNextRowInMem(STableBlockScanInfo* pCheckInfo) {
//   bool hasNext = false;
//   if (pCheckInfo->chosen == CHECKINFO_CHOSEN_MEM) {
//     if (pCheckInfo->iter != NULL) {
//       hasNext = tsdbTbDataIterNext(pCheckInfo->iter);
//     }

//     if (hasNext) {
//       return hasNext;
//     }

//     if (pCheckInfo->iiter != NULL) {
//       return tsdbTbDataIterGet(pCheckInfo->iiter, NULL);
//     }
//   } else if (pCheckInfo->chosen == CHECKINFO_CHOSEN_IMEM) {
//     if (pCheckInfo->iiter != NULL) {
//       hasNext = tsdbTbDataIterNext(pCheckInfo->iiter);
//     }

//     if (hasNext) {
//       return hasNext;
//     }

//     if (pCheckInfo->iter != NULL) {
//       return tsdbTbDataIterGet(pCheckInfo->iter, NULL);
//     }
//   } else {
//     if (pCheckInfo->iter != NULL) {
//       hasNext = tsdbTbDataIterNext(pCheckInfo->iter);
//     }
//     if (pCheckInfo->iiter != NULL) {
//       hasNext = tsdbTbDataIterNext(pCheckInfo->iiter) || hasNext;
//     }
//   }

//   return hasNext;
// }

// static int32_t getFileIdFromKey(TSKEY key, int32_t daysPerFile, int32_t precision) {
//   assert(precision >= TSDB_TIME_PRECISION_MICRO || precision <= TSDB_TIME_PRECISION_NANO);
//   if (key == TSKEY_INITIAL_VAL) {
//     return INT32_MIN;
//   }

//   if (key < 0) {
//     key -= (daysPerFile * tsTickPerMin[precision]);
//   }

//   int64_t fid = (int64_t)(key / (daysPerFile * tsTickPerMin[precision]));  // set the starting fileId
//   if (fid < 0L && llabs(fid) > INT32_MAX) {                                // data value overflow for INT32
//     fid = INT32_MIN;
//   }

//   if (fid > 0L && fid > INT32_MAX) {
//     fid = INT32_MAX;
//   }

//   return (int32_t)fid;
// }

// static int32_t binarySearchForBlock(SBlock* pBlock, int32_t numOfBlocks, TSKEY skey, int32_t order) {
//   int32_t firstSlot = 0;
//   int32_t lastSlot = numOfBlocks - 1;

//   int32_t midSlot = firstSlot;

//   while (1) {
//     numOfBlocks = lastSlot - firstSlot + 1;
//     midSlot = (firstSlot + (numOfBlocks >> 1));

//     if (numOfBlocks == 1) break;

//     if (skey > pBlock[midSlot].maxKey.ts) {
//       if (numOfBlocks == 2) break;
//       if ((order == TSDB_ORDER_DESC) && (skey < pBlock[midSlot + 1].minKey.ts)) break;
//       firstSlot = midSlot + 1;
//     } else if (skey < pBlock[midSlot].minKey.ts) {
//       if ((order == TSDB_ORDER_ASC) && (skey > pBlock[midSlot - 1].maxKey.ts)) break;
//       lastSlot = midSlot - 1;
//     } else {
//       break;  // got the slot
//     }
//   }

//   return midSlot;
// }

static int32_t doLoadBlockIndex(STsdbReader* pReader, SDataFReader* pFileReader, SArray* pIndexList) {
  int32_t code = 0;

  SArray *aBlockIdx = taosArrayInit(0, sizeof(SBlockIdx));

  code = tsdbReadBlockIdx(pFileReader, aBlockIdx, NULL);
  if (code != TSDB_CODE_SUCCESS) {
    goto _err;
  }

  if (taosArrayGetSize(aBlockIdx) == 0) {
    taosArrayClear(aBlockIdx);
    return TSDB_CODE_SUCCESS;
  }

  SBlockIdx *pBlockIdx;
  for (int32_t i = 0; i < taosArrayGetSize(aBlockIdx); ++i) {
    pBlockIdx = (SBlockIdx *)taosArrayGet(aBlockIdx, i);

    if (pBlockIdx->suid != pReader->suid) {
      continue;
    }

    // this block belongs to a table that is not queried.
    void* p = taosHashGet(pReader->status.pTableMap, &pBlockIdx->uid, sizeof(uint64_t));
    if (p == NULL) {
      continue;
    }

    if ((ASCENDING_TRAVERSE(pReader->order) &&
         (pBlockIdx->minKey > pReader->window.ekey || pBlockIdx->maxKey < pReader->window.skey)) ||
        (!ASCENDING_TRAVERSE(pReader->order) &&
         (pBlockIdx->minKey > pReader->window.skey || pBlockIdx->maxKey < pReader->window.ekey))) {
      continue;
    }

    STableBlockScanInfo* pScanInfo = p;
    if (pScanInfo->pBlockList == NULL) {
      pScanInfo->pBlockList = taosArrayInit(16, sizeof(SBlock));
    }

    pScanInfo->blockIdx = *pBlockIdx;
    taosArrayPush(pIndexList, pBlockIdx);
  }

  taosArrayDestroy(aBlockIdx);
  return TSDB_CODE_SUCCESS;

_err:
  taosArrayDestroy(aBlockIdx);
  return code;
}

static int32_t doLoadFileBlock(STsdbReader* pReader, SArray* pIndexList, uint32_t* numOfValidTables, int32_t* numOfBlocks) {
  size_t numOfTables = taosArrayGetSize(pIndexList);

  *numOfValidTables = 0;

  for(int32_t i = 0; i < numOfTables; ++i) {
    SBlockIdx* pBlockIdx = taosArrayGet(pIndexList, i);

    SMapData mapData = {0};
    tMapDataReset(&mapData);
    tsdbReadBlock(pReader->pFileReader, pBlockIdx, &mapData, NULL);

    STableBlockScanInfo* pScanInfo = taosHashGet(pReader->status.pTableMap, &pBlockIdx->uid, sizeof(int64_t));
    taosArrayClear(pScanInfo->pBlockList);

    for (int32_t j = 0; j < mapData.nItem; ++j) {
      SBlock block = {0};

      int32_t code = tMapDataGetItemByIdx(&mapData, j, &block, tGetBlock);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }

      // 1. time range check
      if ((ASCENDING_TRAVERSE(pReader->order) &&
           (block.minKey.ts > pReader->window.ekey || block.maxKey.ts < pReader->window.skey)) ||
          (!ASCENDING_TRAVERSE(pReader->order) &&
           (block.minKey.ts > pReader->window.skey || block.maxKey.ts < pReader->window.ekey))) {
        continue;
      }

      // 2. version range check
      if (block.minVersion > pReader->verRange.maxVer || block.maxVersion < pReader->verRange.minVer) {
        continue;
      }

      void* p = taosArrayPush(pScanInfo->pBlockList, &block);
      if (p == NULL) {
        return TSDB_CODE_OUT_OF_MEMORY;
      }

      (*numOfBlocks) += 1;
    }

    if (pScanInfo->pBlockList != NULL && taosArrayGetSize(pScanInfo->pBlockList) > 0) {
      (*numOfValidTables) += 1;
    }
  }

  return TSDB_CODE_SUCCESS;
}

static void setBlockDumpCompleted(SFileBlockDumpInfo* pDumpInfo, SBlock* pBlock, int32_t order) {
  int32_t step = ASCENDING_TRAVERSE(order)? 1:-1;

  pDumpInfo->rowIndex  = pBlock->nRow;
  pDumpInfo->totalRows = pBlock->nRow;
  pDumpInfo->lastKey   = pBlock->maxKey.ts + step;
}

static void doCopyColVal(SColumnInfoData* pColInfoData, int32_t rowIndex, int32_t colIndex, SColVal* pColVal, SBlockLoadSuppInfo* pSup) {
  if (IS_VAR_DATA_TYPE(pColVal->type)) {
    if (pColVal->isNull) {
      colDataAppendNULL(pColInfoData, rowIndex);
    } else {
      varDataSetLen(pSup->buildBuf[colIndex], pColVal->value.nData);
      memcpy(varDataVal(pSup->buildBuf[colIndex]), pColVal->value.pData, pColVal->value.nData);
      colDataAppend(pColInfoData, rowIndex, pSup->buildBuf[colIndex], false);
    }
  } else {
    colDataAppend(pColInfoData, rowIndex, (const char*)&pColVal->value, pColVal->isNull);
  }
}

static int32_t doLoadFileBlockData(STsdbReader* pReader, SDataBlockIter* pBlockIter, STableBlockScanInfo* pBlockScanInfo, SBlockData* pBlockData) {
  int64_t st = taosGetTimestampUs();

  SFileDataBlockInfo* pFBlock = getCurrentBlockInfo(pBlockIter);
  SBlock*             pBlock = taosArrayGet(pBlockScanInfo->pBlockList, pFBlock->tbBlockIdx);
  SSDataBlock*        pResBlock = pReader->pResBlock;
  int32_t             numOfCols = blockDataGetNumOfCols(pResBlock);

  SBlockLoadSuppInfo* pSupInfo = &pReader->suppInfo;

  uint8_t *pb = NULL, *pb1 = NULL;
  int32_t  code = tsdbReadColData(pReader->pFileReader, &pBlockScanInfo->blockIdx, pBlock, pSupInfo->colIds, numOfCols, pBlockData, &pb, &pb1);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  SColVal cv = {0};
  int32_t colIndex = 0;

  for (int32_t i = 0; i < numOfCols && colIndex < taosArrayGetSize(pBlockData->aColDataP); ++i) {
    SColumnInfoData* pColData = taosArrayGet(pResBlock->pDataBlock, i);
    if (pColData->info.colId == PRIMARYKEY_TIMESTAMP_COL_ID) {
      for (int32_t j = 0; j < pBlockData->nRow; ++j) {
        colDataAppend(pColData, j, (const char*)&pBlockData->aTSKEY[j], false);
      }
    } else {
      SColData* pData = (SColData*)taosArrayGetP(pBlockData->aColDataP, colIndex);

      if (pData->cid == pColData->info.colId) {
        for (int32_t j = 0; j < pBlockData->nRow; ++j) {
          tColDataGetValue(pData, j, &cv);
          doCopyColVal(pColData, j, i, &cv, pSupInfo);
        }
        colIndex += 1;
      } else {  // the specified column does not exist in file block, fill with null data
        colDataAppendNNULL(pColData, 0, pBlockData->nRow);
      }
    }
  }

  pResBlock->info.rows = pBlockData->nRow;
  setBlockDumpCompleted(&pReader->status.fBlockDumpInfo, pBlock, pReader->order);

  int64_t elapsedTime = (taosGetTimestampUs() - st);
  pReader->cost.blockLoadTime += elapsedTime;

  tsdbDebug("%p load file block into buffer, global index:%d, table index:%d, brange:%" PRId64 "-%" PRId64
            ", rows:%d, minVer:%" PRId64 ", maxVer:%" PRId64 ", elapsed time:%" PRId64 " us, %s",
            pReader, pBlockIter->index, pFBlock->tbBlockIdx, pBlock->minKey.ts, pBlock->maxKey.ts, pBlockData->nRow,
            pBlock->minVersion, pBlock->maxVersion, elapsedTime, pReader->idStr);
  return TSDB_CODE_SUCCESS;

_error:
  tsdbError("%p error occurs in loading file block, global index:%d, table index:%d, brange:%" PRId64 "-%" PRId64
            ", rows:%d, %s",
            pReader, pBlockIter->index, pFBlock->tbBlockIdx, pBlock->minKey.ts, pBlock->maxKey.ts, pBlock->nRow,
            pReader->idStr);
  return code;
}

// static int32_t handleDataMergeIfNeeded(STsdbReader* pTsdbReadHandle, SBlock* pBlock, STableBlockScanInfo* pCheckInfo) {
//   SQueryFilePos* cur = &pTsdbReadHandle->cur;
//   STsdbCfg*      pCfg = REPO_CFG(pTsdbReadHandle->pTsdb);
//   SDataBlockInfo binfo = GET_FILE_DATA_BLOCK_INFO(pCheckInfo, pBlock);
//   TSKEY          key;
//   int32_t        code = TSDB_CODE_SUCCESS;

//   /*bool hasData = */ initTableMemIterator(pTsdbReadHandle, pCheckInfo);
//   assert(cur->pos >= 0 && cur->pos <= binfo.rows);

//   key = extractFirstTraverseKey(pCheckInfo, pTsdbReadHandle->order, pCfg->update, TD_VER_MAX);

//   if (key != TSKEY_INITIAL_VAL) {
//     tsdbDebug("%p key in mem:%" PRId64 ", %s", pTsdbReadHandle, key, pTsdbReadHandle->idStr);
//   } else {
//     tsdbDebug("%p no data in mem, %s", pTsdbReadHandle, pTsdbReadHandle->idStr);
//   }

//   bool ascScan = ASCENDING_TRAVERSE(pTsdbReadHandle->order);

//   if ((ascScan && (key != TSKEY_INITIAL_VAL && key <= binfo.window.ekey)) ||
//       (!ascScan && (key != TSKEY_INITIAL_VAL && key >= binfo.window.skey))) {
//     bool cacheDataInFileBlockHole = (ascScan && (key != TSKEY_INITIAL_VAL && key < binfo.window.skey)) ||
//                                     (!ascScan && (key != TSKEY_INITIAL_VAL && key > binfo.window.ekey));
//     if (cacheDataInFileBlockHole) {
//       // do not load file block into buffer
//       int32_t step = ascScan ? 1 : -1;

//       TSKEY maxKey = ascScan ? (binfo.window.skey - step) : (binfo.window.ekey - step);
//       cur->rows =
//           buildInmemDataBlockImpl(pCheckInfo, maxKey, pTsdbReadHandle->outputCapacity, &cur->win, pTsdbReadHandle);
//       pTsdbReadHandle->realNumOfRows = cur->rows;

//       // update the last key value
//       pCheckInfo->lastKey = cur->win.ekey + step;

//       if (!ascScan) {
//         TSWAP(cur->win.skey, cur->win.ekey);
//       }

//       cur->mixBlock = true;
//       cur->blockCompleted = false;
//       return code;
//     }

//     // return error, add test cases
//     if ((code = doLoadFileBlockData(pTsdbReadHandle, pBlock, pCheckInfo, cur->slot)) != TSDB_CODE_SUCCESS) {
//       return code;
//     }

//     doMergeTwoLevelData(pTsdbReadHandle, pCheckInfo, pBlock);
//   } else {
//     /*
//      * no data in cache, only load data from file
//      * during the query processing, data in cache will not be checked anymore.
//      * Here the buffer is not enough, so only part of file block can be loaded into memory buffer
//      */
//     int32_t endPos = getEndPosInDataBlock(pTsdbReadHandle, &binfo);

//     bool wholeBlockReturned = ((abs(cur->pos - endPos) + 1) == binfo.rows);
//     if (wholeBlockReturned) {
//       pTsdbReadHandle->realNumOfRows = binfo.rows;

//       cur->rows = binfo.rows;
//       cur->win = binfo.window;
//       cur->mixBlock = false;
//       cur->blockCompleted = true;

//       if (ascScan) {
//         cur->lastKey = binfo.window.ekey + 1;
//         cur->pos = binfo.rows;
//       } else {
//         cur->lastKey = binfo.window.skey - 1;
//         cur->pos = -1;
//       }
//     } else {  // partially copy to dest buffer
//       // make sure to only load once
//       bool firstTimeExtract = ((cur->pos == 0 && ascScan) || (cur->pos == binfo.rows - 1 && (!ascScan)));
//       if (pTsdbReadHandle->outputCapacity < binfo.rows && firstTimeExtract) {
//         code = doLoadFileBlockData(pTsdbReadHandle, pBlock, pCheckInfo, cur->slot);
//         if (code != TSDB_CODE_SUCCESS) {
//           return code;
//         }
//       }

//       copyAllRemainRowsFromFileBlock(pTsdbReadHandle, pCheckInfo, &binfo, endPos);
//       cur->mixBlock = true;
//     }

//     if (pTsdbReadHandle->outputCapacity >= binfo.rows) {
//       ASSERT(cur->blockCompleted || cur->mixBlock);
//     }

//     if (cur->rows == binfo.rows) {
//       tsdbDebug("%p whole file block qualified, brange:%" PRId64 "-%" PRId64 ", rows:%d, lastKey:%" PRId64 ", %s",
//                 pTsdbReadHandle, cur->win.skey, cur->win.ekey, cur->rows, cur->lastKey, pTsdbReadHandle->idStr);
//     } else {
//       tsdbDebug("%p create data block from remain file block, brange:%" PRId64 "-%" PRId64
//                 ", rows:%d, total:%d, lastKey:%" PRId64 ", %s",
//                 pTsdbReadHandle, cur->win.skey, cur->win.ekey, cur->rows, binfo.rows, cur->lastKey,
//                 pTsdbReadHandle->idStr);
//     }
//   }

//   return code;
// }

// static int32_t loadFileDataBlock(STsdbReader* pTsdbReadHandle, SBlock* pBlock, STableBlockScanInfo* pCheckInfo,
//                                  bool* exists) {
//   SQueryFilePos* cur = &pTsdbReadHandle->cur;
//   int32_t        code = TSDB_CODE_SUCCESS;
//   bool           asc = ASCENDING_TRAVERSE(pTsdbReadHandle->order);

//   if (asc) {
//     // query ended in/started from current block
//     if (pTsdbReadHandle->window.ekey < pBlock->maxKey.ts || pCheckInfo->lastKey > pBlock->minKey.ts) {
//       if ((code = doLoadFileBlockData(pTsdbReadHandle, pBlock, pCheckInfo, cur->slot)) != TSDB_CODE_SUCCESS) {
//         *exists = false;
//         return code;
//       }

//       SDataCols* pTSCol = pTsdbReadHandle->rhelper.pDCols[0];
//       assert(pTSCol->cols->type == TSDB_DATA_TYPE_TIMESTAMP && pTSCol->numOfRows == pBlock->numOfRows);

//       if (pCheckInfo->lastKey > pBlock->minKey.ts) {
//         cur->pos =
//             binarySearchForKey(pTSCol->cols[0].pData, pBlock->numOfRows, pCheckInfo->lastKey,
//             pTsdbReadHandle->order);
//       } else {
//         cur->pos = 0;
//       }

//       assert(pCheckInfo->lastKey <= pBlock->maxKey.ts);
//       doMergeTwoLevelData(pTsdbReadHandle, pCheckInfo, pBlock);
//     } else {  // the whole block is loaded in to buffer
//       cur->pos = asc ? 0 : (pBlock->numOfRows - 1);
//       code = handleDataMergeIfNeeded(pTsdbReadHandle, pBlock, pCheckInfo);
//     }
//   } else {  // desc order, query ended in current block
//     if (pTsdbReadHandle->window.ekey > pBlock->minKey.ts || pCheckInfo->lastKey < pBlock->maxKey.ts) {
//       if ((code = doLoadFileBlockData(pTsdbReadHandle, pBlock, pCheckInfo, cur->slot)) != TSDB_CODE_SUCCESS) {
//         *exists = false;
//         return code;
//       }

//       SDataCols* pTsCol = pTsdbReadHandle->rhelper.pDCols[0];
//       if (pCheckInfo->lastKey < pBlock->maxKey.ts) {
//         cur->pos =
//             binarySearchForKey(pTsCol->cols[0].pData, pBlock->numOfRows, pCheckInfo->lastKey,
//             pTsdbReadHandle->order);
//       } else {
//         cur->pos = pBlock->numOfRows - 1;
//       }

//       assert(pCheckInfo->lastKey >= pBlock->minKey.ts);
//       doMergeTwoLevelData(pTsdbReadHandle, pCheckInfo, pBlock);
//     } else {
//       cur->pos = asc ? 0 : (pBlock->numOfRows - 1);
//       code = handleDataMergeIfNeeded(pTsdbReadHandle, pBlock, pCheckInfo);
//     }
//   }

//   *exists = pTsdbReadHandle->realNumOfRows > 0;
//   return code;
// }

// static int doBinarySearchKey(char* pValue, int num, TSKEY key, int order) {
//   int    firstPos, lastPos, midPos = -1;
//   int    numOfRows;
//   TSKEY* keyList;

//   assert(order == TSDB_ORDER_ASC || order == TSDB_ORDER_DESC);

//   if (num <= 0) return -1;

//   keyList = (TSKEY*)pValue;
//   firstPos = 0;
//   lastPos = num - 1;

//   if (order == TSDB_ORDER_DESC) {
//     // find the first position which is smaller than the key
//     while (1) {
//       if (key >= keyList[lastPos]) return lastPos;
//       if (key == keyList[firstPos]) return firstPos;
//       if (key < keyList[firstPos]) return firstPos - 1;

//       numOfRows = lastPos - firstPos + 1;
//       midPos = (numOfRows >> 1) + firstPos;

//       if (key < keyList[midPos]) {
//         lastPos = midPos - 1;
//       } else if (key > keyList[midPos]) {
//         firstPos = midPos + 1;
//       } else {
//         break;
//       }
//     }

//   } else {
//     // find the first position which is bigger than the key
//     while (1) {
//       if (key <= keyList[firstPos]) return firstPos;
//       if (key == keyList[lastPos]) return lastPos;

//       if (key > keyList[lastPos]) {
//         lastPos = lastPos + 1;
//         if (lastPos >= num)
//           return -1;
//         else
//           return lastPos;
//       }

//       numOfRows = lastPos - firstPos + 1;
//       midPos = (numOfRows >> 1) + firstPos;

//       if (key < keyList[midPos]) {
//         lastPos = midPos - 1;
//       } else if (key > keyList[midPos]) {
//         firstPos = midPos + 1;
//       } else {
//         break;
//       }
//     }
//   }

//   return midPos;
// }

// static int32_t doCopyRowsFromFileBlock(STsdbReader* pTsdbReadHandle, int32_t capacity, int32_t numOfRows, int32_t
// start,
//                                        int32_t end) {
//   SDataCols* pCols = pTsdbReadHandle->rhelper.pDCols[0];
//   TSKEY*     tsArray = pCols->cols[0].pData;

//   int32_t num = end - start + 1;
//   assert(num >= 0);

//   if (num == 0) {
//     return numOfRows;
//   }

//   bool    ascScan = ASCENDING_TRAVERSE(pTsdbReadHandle->order);
//   int32_t trueStart = ascScan ? start : end;
//   int32_t trueEnd = ascScan ? end : start;
//   int32_t step = ascScan ? 1 : -1;

//   int32_t requiredNumOfCols = (int32_t)taosArrayGetSize(pTsdbReadHandle->pColumns);

//   // data in buffer has greater timestamp, copy data in file block
//   int32_t i = 0, j = 0;
//   while (i < requiredNumOfCols && j < pCols->numOfCols) {
//     SColumnInfoData* pColInfo = taosArrayGet(pTsdbReadHandle->pColumns, i);

//     SDataCol* src = &pCols->cols[j];
//     if (src->colId < pColInfo->info.colId) {
//       j++;
//       continue;
//     }

//     if (!isAllRowsNull(src) && pColInfo->info.colId == src->colId) {
//       if (!IS_VAR_DATA_TYPE(pColInfo->info.type)) {  // todo opt performance
//         //        memmove(pData, (char*)src->pData + bytes * start, bytes * num);
//         int32_t rowIndex = numOfRows;
//         for (int32_t k = trueStart; ((ascScan && k <= trueEnd) || (!ascScan && k >= trueEnd)); k += step, ++rowIndex)
//         {
//           SCellVal sVal = {0};
//           if (tdGetColDataOfRow(&sVal, src, k, pCols->bitmapMode) < 0) {
//             TASSERT(0);
//           }

//           if (sVal.valType == TD_VTYPE_NORM) {
//             colDataAppend(pColInfo, rowIndex, sVal.val, false);
//           } else {
//             colDataAppendNULL(pColInfo, rowIndex);
//           }
//         }
//       } else {  // handle the var-string
//         int32_t rowIndex = numOfRows;

//         // todo refactor, only copy one-by-one
//         for (int32_t k = trueStart; ((ascScan && k <= trueEnd) || (!ascScan && k >= trueEnd)); k += step, ++rowIndex)
//         {
//           SCellVal sVal = {0};
//           if (tdGetColDataOfRow(&sVal, src, k, pCols->bitmapMode) < 0) {
//             TASSERT(0);
//           }

//           if (sVal.valType == TD_VTYPE_NORM) {
//             colDataAppend(pColInfo, rowIndex, sVal.val, false);
//           } else {
//             colDataAppendNULL(pColInfo, rowIndex);
//           }
//         }
//       }

//       j++;
//       i++;
//     } else {  // pColInfo->info.colId < src->colId, it is a NULL data
//       colDataAppendNNULL(pColInfo, numOfRows, num);
//       i++;
//     }
//   }

//   while (i < requiredNumOfCols) {  // the remain columns are all null data
//     SColumnInfoData* pColInfo = taosArrayGet(pTsdbReadHandle->pColumns, i);
//     colDataAppendNNULL(pColInfo, numOfRows, num);
//     i++;
//   }

//   pTsdbReadHandle->cur.win.ekey = tsArray[trueEnd];
//   pTsdbReadHandle->cur.lastKey = tsArray[trueEnd] + step;

//   return numOfRows + num;
// }

// static int32_t mergeTwoRowFromMem(STsdbReader* pTsdbReadHandle, int32_t capacity, int32_t* curRow, STSRow* row1,
//                                   STSRow* row2, int32_t numOfCols, uint64_t uid, STSchema* pSchema1, STSchema*
//                                   pSchema2, bool update, TSKEY* lastRowKey) {
// #if 1
//   STSchema* pSchema;
//   STSRow*   row;
//   int16_t   colId;
//   int16_t   offset;

//   bool     isRow1DataRow = TD_IS_TP_ROW(row1);
//   bool     isRow2DataRow;
//   bool     isChosenRowDataRow;
//   int32_t  chosen_itr;
//   SCellVal sVal = {0};
//   TSKEY    rowKey = TSKEY_INITIAL_VAL;
//   int32_t  nResult = 0;
//   int32_t  mergeOption = 0;  // 0 discard 1 overwrite 2 merge

//   // the schema version info is embeded in STSRow
//   int32_t numOfColsOfRow1 = 0;

//   if (pSchema1 == NULL) {
//     pSchema1 = metaGetTbTSchema(REPO_META(pTsdbReadHandle->pTsdb), uid, TD_ROW_SVER(row1));
//   }

// #ifdef TD_DEBUG_PRINT_ROW
//   char   flags[70] = {0};
//   STsdb* pTsdb = pTsdbReadHandle->rhelper.pRepo;
//   snprintf(flags, 70, "%s:%d vgId:%d dir:%s row1%s=NULL,row2%s=NULL", __func__, __LINE__, TD_VID(pTsdb->pVnode),
//            pTsdb->dir, row1 ? "!" : "", row2 ? "!" : "");
//   tdSRowPrint(row1, pSchema1, flags);
// #endif

//   if (isRow1DataRow) {
//     numOfColsOfRow1 = schemaNCols(pSchema1);
//   } else {
//     numOfColsOfRow1 = tdRowGetNCols(row1);
//   }

//   int32_t numOfColsOfRow2 = 0;
//   if (row2) {
//     isRow2DataRow = TD_IS_TP_ROW(row2);
//     if (pSchema2 == NULL) {
//       pSchema2 = metaGetTbTSchema(REPO_META(pTsdbReadHandle->pTsdb), uid, TD_ROW_SVER(row2));
//     }
//     if (isRow2DataRow) {
//       numOfColsOfRow2 = schemaNCols(pSchema2);
//     } else {
//       numOfColsOfRow2 = tdRowGetNCols(row2);
//     }
//   }

//   int32_t i = 0, j = 0, k = 0;
//   while (i < numOfCols && (j < numOfColsOfRow1 || k < numOfColsOfRow2)) {
//     SColumnInfoData* pColInfo = taosArrayGet(pTsdbReadHandle->pColumns, i);

//     int32_t colIdOfRow1;
//     if (j >= numOfColsOfRow1) {
//       colIdOfRow1 = INT32_MAX;
//     } else if (isRow1DataRow) {
//       colIdOfRow1 = pSchema1->columns[j].colId;
//     } else {
//       colIdOfRow1 = tdKvRowColIdAt(row1, j);
//     }

//     int32_t colIdOfRow2;
//     if (k >= numOfColsOfRow2) {
//       colIdOfRow2 = INT32_MAX;
//     } else if (isRow2DataRow) {
//       colIdOfRow2 = pSchema2->columns[k].colId;
//     } else {
//       colIdOfRow2 = tdKvRowColIdAt(row2, k);
//     }

//     if (colIdOfRow1 < colIdOfRow2) {  // the most probability
//       if (colIdOfRow1 < pColInfo->info.colId) {
//         ++j;
//         continue;
//       }
//       row = row1;
//       pSchema = pSchema1;
//       isChosenRowDataRow = isRow1DataRow;
//       chosen_itr = j;
//     } else if (colIdOfRow1 == colIdOfRow2) {
//       if (colIdOfRow1 < pColInfo->info.colId) {
//         ++j;
//         ++k;
//         continue;
//       }
//       row = row1;
//       pSchema = pSchema1;
//       isChosenRowDataRow = isRow1DataRow;
//       chosen_itr = j;
//     } else {
//       if (colIdOfRow2 < pColInfo->info.colId) {
//         ++k;
//         continue;
//       }
//       row = row2;
//       pSchema = pSchema2;
//       chosen_itr = k;
//       isChosenRowDataRow = isRow2DataRow;
//     }

//     if (isChosenRowDataRow) {
//       colId = pSchema->columns[chosen_itr].colId;
//       offset = pSchema->columns[chosen_itr].offset;
//       // TODO: use STSRowIter
//       tdSTpRowGetVal(row, colId, pSchema->columns[chosen_itr].type, pSchema->flen, offset, chosen_itr - 1, &sVal);
//       if (colId == PRIMARYKEY_TIMESTAMP_COL_ID) {
//         rowKey = *(TSKEY*)sVal.val;
//         if (rowKey != *lastRowKey) {
//           mergeOption = 1;
//           if (*lastRowKey != TSKEY_INITIAL_VAL) {
//             ++(*curRow);
//           }
//           *lastRowKey = rowKey;
//           ++nResult;
//         } else if (update) {
//           mergeOption = 2;
//         } else {
//           mergeOption = 0;
//           break;
//         }
//       }
//     } else {
//       // TODO: use STSRowIter
//       if (chosen_itr == 0) {
//         colId = PRIMARYKEY_TIMESTAMP_COL_ID;
//         tdSKvRowGetVal(row, PRIMARYKEY_TIMESTAMP_COL_ID, -1, -1, &sVal);
//         rowKey = *(TSKEY*)sVal.val;
//         if (rowKey != *lastRowKey) {
//           mergeOption = 1;
//           if (*lastRowKey != TSKEY_INITIAL_VAL) {
//             ++(*curRow);
//           }
//           *lastRowKey = rowKey;
//           ++nResult;
//         } else if (update) {
//           mergeOption = 2;
//         } else {
//           mergeOption = 0;
//           break;
//         }
//       } else {
//         SKvRowIdx* pColIdx = tdKvRowColIdxAt(row, chosen_itr - 1);
//         colId = pColIdx->colId;
//         offset = pColIdx->offset;
//         tdSKvRowGetVal(row, colId, offset, chosen_itr - 1, &sVal);
//       }
//     }

//     ASSERT(rowKey != TSKEY_INITIAL_VAL);

//     if (colId == pColInfo->info.colId) {
//       if (tdValTypeIsNorm(sVal.valType)) {
//         colDataAppend(pColInfo, *curRow, sVal.val, false);
//       } else if (tdValTypeIsNull(sVal.valType)) {
//         colDataAppend(pColInfo, *curRow, NULL, true);
//       } else if (tdValTypeIsNone(sVal.valType)) {
//         // TODO: Set null if nothing append for this row
//         if (mergeOption == 1) {
//           colDataAppend(pColInfo, *curRow, NULL, true);
//         }
//       } else {
//         ASSERT(0);
//       }

//       ++i;

//       if (row == row1) {
//         ++j;
//       } else {
//         ++k;
//       }
//     } else {
//       if (mergeOption == 1) {
//         colDataAppend(pColInfo, *curRow, NULL, true);
//       }
//       ++i;
//     }
//   }

//   if (mergeOption == 1) {
//     while (i < numOfCols) {  // the remain columns are all null data
//       SColumnInfoData* pColInfo = taosArrayGet(pTsdbReadHandle->pColumns, i);
//       colDataAppend(pColInfo, *curRow, NULL, true);
//       ++i;
//     }
//   }

//   return nResult;
// #endif
// }

// static void getQualifiedRowsPos(STsdbReader* pTsdbReadHandle, int32_t startPos, int32_t endPos, int32_t numOfExisted,
//                                 int32_t* start, int32_t* end) {
//   *start = -1;

//   if (ASCENDING_TRAVERSE(pTsdbReadHandle->order)) {
//     int32_t remain = endPos - startPos + 1;
//     if (remain + numOfExisted > pTsdbReadHandle->outputCapacity) {
//       *end = (pTsdbReadHandle->outputCapacity - numOfExisted) + startPos - 1;
//     } else {
//       *end = endPos;
//     }

//     *start = startPos;
//   } else {
//     int32_t remain = (startPos - endPos) + 1;
//     if (remain + numOfExisted > pTsdbReadHandle->outputCapacity) {
//       *end = startPos + 1 - (pTsdbReadHandle->outputCapacity - numOfExisted);
//     } else {
//       *end = endPos;
//     }

//     *start = *end;
//     *end = startPos;
//   }
// }

// static void updateInfoAfterMerge(STsdbReader* pTsdbReadHandle, STableBlockScanInfo* pCheckInfo, int32_t numOfRows,
//                                  int32_t endPos) {
//   SQueryFilePos* cur = &pTsdbReadHandle->cur;

//   pCheckInfo->lastKey = cur->lastKey;
//   pTsdbReadHandle->realNumOfRows = numOfRows;
//   cur->rows = numOfRows;
//   cur->pos = endPos;
// }

// static void doCheckGeneratedBlockRange(STsdbReader* pTsdbReadHandle) {
//   SQueryFilePos* cur = &pTsdbReadHandle->cur;

//   if (cur->rows > 0) {
//     if (ASCENDING_TRAVERSE(pTsdbReadHandle->order)) {
//       assert(cur->win.skey >= pTsdbReadHandle->window.skey && cur->win.ekey <= pTsdbReadHandle->window.ekey);
//     } else {
//       assert(cur->win.skey >= pTsdbReadHandle->window.ekey && cur->win.ekey <= pTsdbReadHandle->window.skey);
//     }

//     SColumnInfoData* pColInfoData = taosArrayGet(pTsdbReadHandle->pColumns, 0);
//     assert(cur->win.skey == ((TSKEY*)pColInfoData->pData)[0] &&
//            cur->win.ekey == ((TSKEY*)pColInfoData->pData)[cur->rows - 1]);
//   } else {
//     cur->win = pTsdbReadHandle->window;

//     int32_t step = ASCENDING_TRAVERSE(pTsdbReadHandle->order) ? 1 : -1;
//     cur->lastKey = pTsdbReadHandle->window.ekey + step;
//   }
// }

// static void copyAllRemainRowsFromFileBlock(STsdbReader* pTsdbReadHandle, STableBlockScanInfo* pCheckInfo,
//                                            SDataBlockInfo* pBlockInfo, int32_t endPos) {
//   SQueryFilePos* cur = &pTsdbReadHandle->cur;

//   SDataCols* pCols = pTsdbReadHandle->rhelper.pDCols[0];
//   TSKEY*     tsArray = pCols->cols[0].pData;

//   bool ascScan = ASCENDING_TRAVERSE(pTsdbReadHandle->order);

//   int32_t step = ascScan ? 1 : -1;

//   int32_t start = cur->pos;
//   int32_t end = endPos;

//   if (!ascScan) {
//     TSWAP(start, end);
//   }

//   assert(pTsdbReadHandle->outputCapacity >= (end - start + 1));
//   int32_t numOfRows = doCopyRowsFromFileBlock(pTsdbReadHandle, pTsdbReadHandle->outputCapacity, 0, start, end);

//   // the time window should always be ascending order: skey <= ekey
//   cur->win = (STimeWindow){.skey = tsArray[start], .ekey = tsArray[end]};
//   cur->mixBlock = (numOfRows != pBlockInfo->rows);
//   cur->lastKey = tsArray[endPos] + step;
//   cur->blockCompleted = (ascScan ? (endPos == pBlockInfo->rows - 1) : (endPos == 0));

//   // The value of pos may be -1 or pBlockInfo->rows, and it is invalid in both cases.
//   int32_t pos = endPos + step;
//   updateInfoAfterMerge(pTsdbReadHandle, pCheckInfo, numOfRows, pos);
//   doCheckGeneratedBlockRange(pTsdbReadHandle);

//   tsdbDebug("%p uid:%" PRIu64 ", data block created, mixblock:%d, brange:%" PRIu64 "-%" PRIu64 " rows:%d, %s",
//             pTsdbReadHandle, pCheckInfo->tableId, cur->mixBlock, cur->win.skey, cur->win.ekey, cur->rows,
//             pTsdbReadHandle->idStr);
// }

// int32_t getEndPosInDataBlock(STsdbReader* pTsdbReadHandle, SDataBlockInfo* pBlockInfo) {
//   // NOTE: reverse the order to find the end position in data block
//   int32_t endPos = -1;
//   bool    ascScan = ASCENDING_TRAVERSE(pTsdbReadHandle->order);
//   int32_t order = ascScan ? TSDB_ORDER_DESC : TSDB_ORDER_ASC;

//   SQueryFilePos* cur = &pTsdbReadHandle->cur;
//   SDataCols*     pCols = pTsdbReadHandle->rhelper.pDCols[0];

//   if (pTsdbReadHandle->outputCapacity >= pBlockInfo->rows) {
//     if (ascScan && pTsdbReadHandle->window.ekey >= pBlockInfo->window.ekey) {
//       endPos = pBlockInfo->rows - 1;
//       cur->mixBlock = (cur->pos != 0);
//     } else if ((!ascScan) && pTsdbReadHandle->window.ekey <= pBlockInfo->window.skey) {
//       endPos = 0;
//       cur->mixBlock = (cur->pos != pBlockInfo->rows - 1);
//     } else {
//       assert(pCols->numOfRows > 0);
//       endPos = doBinarySearchKey(pCols->cols[0].pData, pCols->numOfRows, pTsdbReadHandle->window.ekey, order);
//       cur->mixBlock = true;
//     }
//   } else {
//     if (ascScan && pTsdbReadHandle->window.ekey >= pBlockInfo->window.ekey) {
//       endPos = TMIN(cur->pos + pTsdbReadHandle->outputCapacity - 1, pBlockInfo->rows - 1);
//     } else if ((!ascScan) && pTsdbReadHandle->window.ekey <= pBlockInfo->window.skey) {
//       endPos = TMAX(cur->pos - pTsdbReadHandle->outputCapacity + 1, 0);
//     } else {
//       ASSERT(pCols->numOfRows > 0);
//       endPos = doBinarySearchKey(pCols->cols[0].pData, pCols->numOfRows, pTsdbReadHandle->window.ekey, order);

//       // current data is more than the capacity
//       int32_t size = abs(cur->pos - endPos) + 1;
//       if (size > pTsdbReadHandle->outputCapacity) {
//         int32_t delta = size - pTsdbReadHandle->outputCapacity;
//         if (ascScan) {
//           endPos -= delta;
//         } else {
//           endPos += delta;
//         }
//       }
//     }
//     cur->mixBlock = true;
//   }

//   return endPos;
// }

// // only return the qualified data to client in terms of query time window, data rows in the same block but do not
// // be included in the query time window will be discarded
// static void doMergeTwoLevelData(STsdbReader* pTsdbReadHandle, STableBlockScanInfo* pCheckInfo, SBlock* pBlock) {
//   SQueryFilePos* cur = &pTsdbReadHandle->cur;
//   SDataBlockInfo blockInfo = GET_FILE_DATA_BLOCK_INFO(pCheckInfo, pBlock);
//   STsdbCfg*      pCfg = REPO_CFG(pTsdbReadHandle->pTsdb);

//   initTableMemIterator(pTsdbReadHandle, pCheckInfo);

//   SDataCols* pCols = pTsdbReadHandle->rhelper.pDCols[0];
//   assert(pCols->cols[0].type == TSDB_DATA_TYPE_TIMESTAMP && pCols->cols[0].colId == PRIMARYKEY_TIMESTAMP_COL_ID &&
//          cur->pos >= 0 && cur->pos < pBlock->numOfRows);
//   // Even Multi-Version supported, the records with duplicated TSKEY would be merged inside of tsdbLoadData
//   interface. TSKEY* tsArray = pCols->cols[0].pData; assert(pCols->numOfRows == pBlock->numOfRows && tsArray[0] ==
//   pBlock->minKey.ts &&
//          tsArray[pBlock->numOfRows - 1] == pBlock->maxKey.ts);

//   bool    ascScan = ASCENDING_TRAVERSE(pTsdbReadHandle->order);
//   int32_t step = ascScan ? 1 : -1;

//   // for search the endPos, so the order needs to reverse
//   int32_t order = ascScan ? TSDB_ORDER_DESC : TSDB_ORDER_ASC;

//   int32_t numOfCols = (int32_t)(QH_GET_NUM_OF_COLS(pTsdbReadHandle));
//   int32_t endPos = getEndPosInDataBlock(pTsdbReadHandle, &blockInfo);

//   STimeWindow* pWin = &blockInfo.window;
//   tsdbDebug("%p uid:%" PRIu64 " start merge data block, file block range:%" PRIu64 "-%" PRIu64
//             " rows:%d, start:%d, end:%d, %s",
//             pTsdbReadHandle, pCheckInfo->tableId, pWin->skey, pWin->ekey, blockInfo.rows, cur->pos, endPos,
//             pTsdbReadHandle->idStr);

//   // compared with the data from in-memory buffer, to generate the correct timestamp array list
//   int32_t numOfRows = 0;
//   int32_t curRow = 0;

//   int16_t   rv1 = -1;
//   int16_t   rv2 = -1;
//   STSchema* pSchema1 = NULL;
//   STSchema* pSchema2 = NULL;

//   int32_t pos = cur->pos;
//   cur->win = TSWINDOW_INITIALIZER;
//   bool adjustPos = false;

//   // no data in buffer, load data from file directly
//   if (pCheckInfo->iiter == NULL && pCheckInfo->iter == NULL) {
//     copyAllRemainRowsFromFileBlock(pTsdbReadHandle, pCheckInfo, &blockInfo, endPos);
//     return;
//   } else if (pCheckInfo->iter != NULL || pCheckInfo->iiter != NULL) {
//     SSkipListNode* node = NULL;
//     TSKEY          lastKeyAppend = TSKEY_INITIAL_VAL;

//     do {
//       STSRow* row2 = NULL;
//       STSRow* row1 = getSRowInTableMem(pCheckInfo, pTsdbReadHandle->order, pCfg->update, &row2, TD_VER_MAX);
//       if (row1 == NULL) {
//         break;
//       }

//       TSKEY key = TD_ROW_KEY(row1);
//       if ((key > pTsdbReadHandle->window.ekey && ascScan) || (key < pTsdbReadHandle->window.ekey && !ascScan)) {
//         break;
//       }

//       if (adjustPos) {
//         if (key == lastKeyAppend) {
//           pos -= step;
//         }
//         adjustPos = false;
//       }

//       if (((pos > endPos || tsArray[pos] > pTsdbReadHandle->window.ekey) && ascScan) ||
//           ((pos < endPos || tsArray[pos] < pTsdbReadHandle->window.ekey) && !ascScan)) {
//         break;
//       }

//       if ((key < tsArray[pos] && ascScan) || (key > tsArray[pos] && !ascScan)) {
//         if (rv1 != TD_ROW_SVER(row1)) {
//           //          pSchema1 = tsdbGetTableSchemaByVersion(pTable, memRowVersion(row1));
//           rv1 = TD_ROW_SVER(row1);
//         }
//         if (row2 && rv2 != TD_ROW_SVER(row2)) {
//           //          pSchema2 = tsdbGetTableSchemaByVersion(pTable, memRowVersion(row2));
//           rv2 = TD_ROW_SVER(row2);
//         }

//         numOfRows +=
//             mergeTwoRowFromMem(pTsdbReadHandle, pTsdbReadHandle->outputCapacity, &curRow, row1, row2, numOfCols,
//                                pCheckInfo->tableId, pSchema1, pSchema2, pCfg->update, &lastKeyAppend);
//         if (cur->win.skey == TSKEY_INITIAL_VAL) {
//           cur->win.skey = key;
//         }

//         cur->win.ekey = key;
//         cur->lastKey = key + step;
//         cur->mixBlock = true;
//         moveToNextRowInMem(pCheckInfo);
//       } else if (key == tsArray[pos]) {  // data in buffer has the same timestamp of data in file block, ignore it
//         if (TD_SUPPORT_UPDATE(pCfg->update)) {
//           if (lastKeyAppend != key) {
//             if (lastKeyAppend != TSKEY_INITIAL_VAL) {
//               ++curRow;
//             }
//             lastKeyAppend = key;
//           }
//           // load data from file firstly
//           numOfRows = doCopyRowsFromFileBlock(pTsdbReadHandle, pTsdbReadHandle->outputCapacity, curRow, pos, pos);

//           if (rv1 != TD_ROW_SVER(row1)) {
//             rv1 = TD_ROW_SVER(row1);
//           }
//           if (row2 && rv2 != TD_ROW_SVER(row2)) {
//             rv2 = TD_ROW_SVER(row2);
//           }

//           // still assign data into current row
//           numOfRows +=
//               mergeTwoRowFromMem(pTsdbReadHandle, pTsdbReadHandle->outputCapacity, &curRow, row1, row2, numOfCols,
//                                  pCheckInfo->tableId, pSchema1, pSchema2, pCfg->update, &lastKeyAppend);

//           if (cur->win.skey == TSKEY_INITIAL_VAL) {
//             cur->win.skey = key;
//           }

//           cur->win.ekey = key;
//           cur->lastKey = key + step;
//           cur->mixBlock = true;

//           moveToNextRowInMem(pCheckInfo);

//           pos += step;
//           adjustPos = true;
//         } else {
//           // discard the memory record
//           moveToNextRowInMem(pCheckInfo);
//         }
//       } else if ((key > tsArray[pos] && ascScan) || (key < tsArray[pos] && !ascScan)) {
//         if (cur->win.skey == TSKEY_INITIAL_VAL) {
//           cur->win.skey = tsArray[pos];
//         }

//         int32_t end = doBinarySearchKey(pCols->cols[0].pData, pCols->numOfRows, key, order);
//         assert(end != -1);

//         if (tsArray[end] == key) {  // the value of key in cache equals to the end timestamp value, ignore it
// #if 0
//           if (pCfg->update == TD_ROW_DISCARD_UPDATE) {
//             moveToNextRowInMem(pCheckInfo);
//           } else {
//             end -= step;
//           }
// #endif
//           if (!TD_SUPPORT_UPDATE(pCfg->update)) {
//             moveToNextRowInMem(pCheckInfo);
//           } else {
//             end -= step;
//           }
//         }

//         int32_t qstart = 0, qend = 0;
//         getQualifiedRowsPos(pTsdbReadHandle, pos, end, numOfRows, &qstart, &qend);

//         if ((lastKeyAppend != TSKEY_INITIAL_VAL) && (lastKeyAppend != (ascScan ? tsArray[qstart] : tsArray[qend]))) {
//           ++curRow;
//         }

//         numOfRows = doCopyRowsFromFileBlock(pTsdbReadHandle, pTsdbReadHandle->outputCapacity, curRow, qstart, qend);
//         pos += (qend - qstart + 1) * step;
//         if (numOfRows > 0) {
//           curRow = numOfRows - 1;
//         }

//         cur->win.ekey = ascScan ? tsArray[qend] : tsArray[qstart];
//         cur->lastKey = cur->win.ekey + step;
//         lastKeyAppend = cur->win.ekey;
//       }
//     } while (numOfRows < pTsdbReadHandle->outputCapacity);

//     if (numOfRows < pTsdbReadHandle->outputCapacity) {
//       /**
//        * if cache is empty, load remain file block data. In contrast, if there are remain data in cache, do NOT
//        * copy them all to result buffer, since it may be overlapped with file data block.
//        */
//       if (node == NULL || ((TD_ROW_KEY((STSRow*)SL_GET_NODE_DATA(node)) > pTsdbReadHandle->window.ekey) && ascScan)
//       ||
//           ((TD_ROW_KEY((STSRow*)SL_GET_NODE_DATA(node)) < pTsdbReadHandle->window.ekey) && !ascScan)) {
//         // no data in cache or data in cache is greater than the ekey of time window, load data from file block
//         if (cur->win.skey == TSKEY_INITIAL_VAL) {
//           cur->win.skey = tsArray[pos];
//         }

//         int32_t start = -1, end = -1;
//         getQualifiedRowsPos(pTsdbReadHandle, pos, endPos, numOfRows, &start, &end);

//         numOfRows = doCopyRowsFromFileBlock(pTsdbReadHandle, pTsdbReadHandle->outputCapacity, numOfRows, start, end);
//         pos += (end - start + 1) * step;

//         cur->win.ekey = ascScan ? tsArray[end] : tsArray[start];
//         cur->lastKey = cur->win.ekey + step;
//         cur->mixBlock = true;
//       }
//     }
//   }

//   cur->blockCompleted = (((pos > endPos || cur->lastKey > pTsdbReadHandle->window.ekey) && ascScan) ||
//                          ((pos < endPos || cur->lastKey < pTsdbReadHandle->window.ekey) && !ascScan));

//   if (!ascScan) {
//     TSWAP(cur->win.skey, cur->win.ekey);
//   }

//   updateInfoAfterMerge(pTsdbReadHandle, pCheckInfo, numOfRows, pos);
//   doCheckGeneratedBlockRange(pTsdbReadHandle);

//   tsdbDebug("%p uid:%" PRIu64 ", data block created, mixblock:%d, brange:%" PRIu64 "-%" PRIu64 " rows:%d, %s",
//             pTsdbReadHandle, pCheckInfo->tableId, cur->mixBlock, cur->win.skey, cur->win.ekey, cur->rows,
//             pTsdbReadHandle->idStr);
// }

// int32_t binarySearchForKey(char* pValue, int num, TSKEY key, int order) {
//   int    firstPos, lastPos, midPos = -1;
//   int    numOfRows;
//   TSKEY* keyList;

//   if (num <= 0) return -1;

//   keyList = (TSKEY*)pValue;
//   firstPos = 0;
//   lastPos = num - 1;

//   if (order == TSDB_ORDER_DESC) {
//     // find the first position which is smaller than the key
//     while (1) {
//       if (key >= keyList[lastPos]) return lastPos;
//       if (key == keyList[firstPos]) return firstPos;
//       if (key < keyList[firstPos]) return firstPos - 1;

//       numOfRows = lastPos - firstPos + 1;
//       midPos = (numOfRows >> 1) + firstPos;

//       if (key < keyList[midPos]) {
//         lastPos = midPos - 1;
//       } else if (key > keyList[midPos]) {
//         firstPos = midPos + 1;
//       } else {
//         break;
//       }
//     }

//   } else {
//     // find the first position which is bigger than the key
//     while (1) {
//       if (key <= keyList[firstPos]) return firstPos;
//       if (key == keyList[lastPos]) return lastPos;

//       if (key > keyList[lastPos]) {
//         lastPos = lastPos + 1;
//         if (lastPos >= num)
//           return -1;
//         else
//           return lastPos;
//       }

//       numOfRows = lastPos - firstPos + 1;
//       midPos = (numOfRows >> 1) + firstPos;

//       if (key < keyList[midPos]) {
//         lastPos = midPos - 1;
//       } else if (key > keyList[midPos]) {
//         firstPos = midPos + 1;
//       } else {
//         break;
//       }
//     }
//   }

//   return midPos;
// }

static void cleanupBlockOrderSupporter(SBlockOrderSupporter* pSup) {
  taosMemoryFreeClear(pSup->numOfBlocksPerTable);
  taosMemoryFreeClear(pSup->indexPerTable);

  for (int32_t i = 0; i < pSup->numOfTables; ++i) {
    SBlockOrderWrapper* pBlockInfo = pSup->pDataBlockInfo[i];
    taosMemoryFreeClear(pBlockInfo);
  }

  taosMemoryFreeClear(pSup->pDataBlockInfo);
}

static int32_t initBlockOrderSupporter(SBlockOrderSupporter* pSup, int32_t numOfTables) {
  ASSERT(numOfTables >= 1);

  pSup->numOfBlocksPerTable = taosMemoryCalloc(1, sizeof(int32_t) * numOfTables);
  pSup->indexPerTable       = taosMemoryCalloc(1, sizeof(int32_t) * numOfTables);
  pSup->pDataBlockInfo      = taosMemoryCalloc(1, POINTER_BYTES * numOfTables);

  if (pSup->numOfBlocksPerTable == NULL || pSup->indexPerTable == NULL || pSup->pDataBlockInfo == NULL) {
    cleanupBlockOrderSupporter(pSup);
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t fileDataBlockOrderCompar(const void* pLeft, const void* pRight, void* param) {
  int32_t leftIndex  = *(int32_t*)pLeft;
  int32_t rightIndex = *(int32_t*)pRight;

  SBlockOrderSupporter* pSupporter = (SBlockOrderSupporter*)param;

  int32_t leftTableBlockIndex = pSupporter->indexPerTable[leftIndex];
  int32_t rightTableBlockIndex = pSupporter->indexPerTable[rightIndex];

  if (leftTableBlockIndex > pSupporter->numOfBlocksPerTable[leftIndex]) {
    /* left block is empty */
    return 1;
  } else if (rightTableBlockIndex > pSupporter->numOfBlocksPerTable[rightIndex]) {
    /* right block is empty */
    return -1;
  }

  SBlockOrderWrapper* pLeftBlock  = &pSupporter->pDataBlockInfo[leftIndex][leftTableBlockIndex];
  SBlockOrderWrapper* pRightBlock = &pSupporter->pDataBlockInfo[rightIndex][rightTableBlockIndex];

  return pLeftBlock->pBlock->aSubBlock[0].offset > pRightBlock->pBlock->aSubBlock[0].offset ? 1 : -1;
}

static int32_t initBlockIterator(STsdbReader* pReader, SDataBlockIter* pBlockIter, int32_t numOfBlocks) {
 pBlockIter->numOfBlocks = numOfBlocks;

 // access data blocks according to the offset of each block in asc/desc order.
 int32_t numOfTables = (int32_t)taosHashGetSize(pReader->status.pTableMap);

 SBlockOrderSupporter sup = {0};
 int32_t code = initBlockOrderSupporter(&sup, numOfTables);
 if (code != TSDB_CODE_SUCCESS) {
   return code;
 }

 int32_t cnt = 0;
 void* ptr = NULL;
 while(1) {
   ptr = taosHashIterate(pReader->status.pTableMap, ptr);
   if (ptr == NULL) {
     break;
   }

   STableBlockScanInfo* pTableScanInfo = (STableBlockScanInfo*)ptr;
   if (pTableScanInfo->pBlockList == NULL || taosArrayGetSize(pTableScanInfo->pBlockList) == 0) {
     continue;
   }

   size_t num = taosArrayGetSize(pTableScanInfo->pBlockList);
   sup.numOfBlocksPerTable[sup.numOfTables] = num;

   char* buf = taosMemoryMalloc(sizeof(SBlockOrderWrapper) * num);
   if (buf == NULL) {
     cleanupBlockOrderSupporter(&sup);
     return TSDB_CODE_TDB_OUT_OF_MEMORY;
   }

   sup.pDataBlockInfo[sup.numOfTables] = (SBlockOrderWrapper*)buf;
   for (int32_t k = 0; k < num; ++k) {
     SBlockOrderWrapper wrapper = {0};
     wrapper.pBlock = (SBlock*)taosArrayGet(pTableScanInfo->pBlockList, k);
     wrapper.uid = pTableScanInfo->uid;

     sup.pDataBlockInfo[sup.numOfTables][k] = wrapper;
     cnt++;
   }

   sup.numOfTables += 1;
 }

 ASSERT(numOfBlocks == cnt);

 // since there is only one table qualified, blocks are not sorted
 if (sup.numOfTables == 1) {
   for(int32_t i = 0; i < numOfBlocks; ++i) {
     SFileDataBlockInfo blockInfo = {.uid = sup.pDataBlockInfo[0][i].uid, .tbBlockIdx = i};
     taosArrayPush(pBlockIter->blockList, &blockInfo);
   }
   tsdbDebug("%p create blocks info struct completed for one table, %d blocks not sorted %s",
       pReader, cnt, pReader->idStr);

   pBlockIter->index = 0;
   return TSDB_CODE_SUCCESS;
 }

 tsdbDebug("%p create data blocks info struct completed, %d blocks in %d tables %s", pReader, cnt, sup.numOfTables, pReader->idStr);
 assert(cnt <= numOfBlocks && sup.numOfTables <= numOfTables);  // the pTableQueryInfo[j]->numOfBlocks may be 0

 SMultiwayMergeTreeInfo* pTree = NULL;

 uint8_t ret = tMergeTreeCreate(&pTree, sup.numOfTables, &sup, fileDataBlockOrderCompar);
 if (ret != TSDB_CODE_SUCCESS) {
   cleanupBlockOrderSupporter(&sup);
   return TSDB_CODE_TDB_OUT_OF_MEMORY;
 }

 int32_t numOfTotal = 0;
 while (numOfTotal < cnt) {
   int32_t pos = tMergeTreeGetChosenIndex(pTree);
   int32_t index = sup.indexPerTable[pos]++;

   SFileDataBlockInfo blockInfo = {.uid = sup.pDataBlockInfo[pos][index].uid, .tbBlockIdx = index};
   taosArrayPush(pBlockIter->blockList, &blockInfo);

   // set data block index overflow, in order to disable the offset comparator
   if (sup.indexPerTable[pos] >= sup.numOfBlocksPerTable[pos]) {
     sup.indexPerTable[pos] = sup.numOfBlocksPerTable[pos] + 1;
   }

   numOfTotal += 1;
   tMergeTreeAdjust(pTree, tMergeTreeGetAdjustIndex(pTree));
 }

 tsdbDebug("%p %d data blocks sort completed, %s", pReader, cnt, pReader->idStr);
 cleanupBlockOrderSupporter(&sup);
 taosMemoryFree(pTree);

 pBlockIter->index = 0;
 return TSDB_CODE_SUCCESS;
}

static bool blockIteratorNext(SDataBlockIter* pBlockIter) {
  if (pBlockIter->index >= pBlockIter->numOfBlocks - 1) {
    return false;
  }

  pBlockIter->index += 1;
  return true;
}

// static int32_t getFirstFileDataBlock(STsdbReader* pTsdbReadHandle, bool* exists);

//static int32_t getDataBlock(STsdbReader* pTsdbReadHandle, SFileBlockInfo* pNext, bool* exists) {
// int32_t        step = ASCENDING_TRAVERSE(pTsdbReadHandle->order) ? 1 : -1;
// SQueryFilePos* cur = &pTsdbReadHandle->cur;
//
// while (1) {
//   int32_t code = loadFileDataBlock(pTsdbReadHandle, pNext->compBlock, pNext->pTableCheckInfo, exists);
//   if (code != TSDB_CODE_SUCCESS || *exists) {
//     return code;
//   }
//
//   if ((cur->slot == pTsdbReadHandle->numOfBlocks - 1 && ASCENDING_TRAVERSE(pTsdbReadHandle->order)) ||
//       (cur->slot == 0 && !ASCENDING_TRAVERSE(pTsdbReadHandle->order))) {
//     // all data blocks in current file has been checked already, try next file if exists
//     return getFirstFileDataBlock(pTsdbReadHandle, exists);
//   } else {  // next block of the same file
//     cur->slot += step;
//     cur->mixBlock = false;
//     cur->blockCompleted = false;
//     pNext = &pTsdbReadHandle->pDataBlockInfo[cur->slot];
//   }
// }
//}

// static int32_t getFirstFileDataBlock(STsdbReader* pTsdbReadHandle, bool* exists) {
//   pTsdbReadHandle->numOfBlocks = 0;
//   SQueryFilePos* cur = &pTsdbReadHandle->cur;

//   int32_t code = TSDB_CODE_SUCCESS;

//   int32_t numOfBlocks = 0;
//   int32_t numOfTables = (int32_t)taosArrayGetSize(pTsdbReadHandle->pTableCheckInfo);

//   STsdbKeepCfg* pCfg = REPO_KEEP_CFG(pTsdbReadHandle->pTsdb);
//   STimeWindow   win = TSWINDOW_INITIALIZER;

//   while (true) {
//     tsdbRLockFS(REPO_FS(pTsdbReadHandle->pTsdb));

//     if ((pTsdbReadHandle->pFileGroup = tsdbFSIterNext(&pTsdbReadHandle->fileIter)) == NULL) {
//       tsdbUnLockFS(REPO_FS(pTsdbReadHandle->pTsdb));
//       break;
//     }

//     tsdbGetFidKeyRange(pCfg->days, pCfg->precision, pTsdbReadHandle->pFileGroup->fid, &win.skey, &win.ekey);

//     // current file are not overlapped with query time window, ignore remain files
//     if ((ASCENDING_TRAVERSE(pTsdbReadHandle->order) && win.skey > pTsdbReadHandle->window.ekey) ||
//         (!ASCENDING_TRAVERSE(pTsdbReadHandle->order) && win.ekey < pTsdbReadHandle->window.ekey)) {
//       tsdbUnLockFS(REPO_FS(pTsdbReadHandle->pTsdb));
//       tsdbDebug("%p remain files are not qualified for qrange:%" PRId64 "-%" PRId64 ", ignore, %s", pTsdbReadHandle,
//                 pTsdbReadHandle->window.skey, pTsdbReadHandle->window.ekey, pTsdbReadHandle->idStr);
//       pTsdbReadHandle->pFileGroup = NULL;
//       assert(pTsdbReadHandle->numOfBlocks == 0);
//       break;
//     }

//     if (tsdbSetAndOpenReadFSet(&pTsdbReadHandle->rhelper, pTsdbReadHandle->pFileGroup) < 0) {
//       tsdbUnLockFS(REPO_FS(pTsdbReadHandle->pTsdb));
//       code = terrno;
//       break;
//     }

//     tsdbUnLockFS(REPO_FS(pTsdbReadHandle->pTsdb));

//     if (tsdbLoadBlockIdx(&pTsdbReadHandle->rhelper) < 0) {
//       code = terrno;
//       break;
//     }

//     if ((code = getFileCompInfo(pTsdbReadHandle, &numOfBlocks)) != TSDB_CODE_SUCCESS) {
//       break;
//     }

//     tsdbDebug("%p %d blocks found in file for %d table(s), fid:%d, %s", pTsdbReadHandle, numOfBlocks, numOfTables,
//               pTsdbReadHandle->pFileGroup->fid, pTsdbReadHandle->idStr);

//     assert(numOfBlocks >= 0);
//     if (numOfBlocks == 0) {
//       continue;
//     }

//     // todo return error code to query engine
//     if ((code = createDataBlocksInfo(pTsdbReadHandle, numOfBlocks, &pTsdbReadHandle->numOfBlocks)) !=
//         TSDB_CODE_SUCCESS) {
//       break;
//     }

//     assert(numOfBlocks >= pTsdbReadHandle->numOfBlocks);
//     if (pTsdbReadHandle->numOfBlocks > 0) {
//       break;
//     }
//   }

//   // no data in file anymore
//   if (pTsdbReadHandle->numOfBlocks <= 0 || code != TSDB_CODE_SUCCESS) {
//     if (code == TSDB_CODE_SUCCESS) {
//       assert(pTsdbReadHandle->pFileGroup == NULL);
//     }

//     cur->fid = INT32_MIN;  // denote that there are no data in file anymore
//     *exists = false;
//     return code;
//   }

//   assert(pTsdbReadHandle->pFileGroup != NULL && pTsdbReadHandle->numOfBlocks > 0);
//   cur->slot = ASCENDING_TRAVERSE(pTsdbReadHandle->order) ? 0 : pTsdbReadHandle->numOfBlocks - 1;
//   cur->fid = pTsdbReadHandle->pFileGroup->fid;

//   SFileBlockInfo* pBlockInfo = &pTsdbReadHandle->pDataBlockInfo[cur->slot];
//   return getDataBlock(pTsdbReadHandle, pBlockInfo, exists);
// }

// static int32_t getBucketIndex(int32_t startRow, int32_t bucketRange, int32_t numOfRows) {
//   return (numOfRows - startRow) / bucketRange;
// }

// query ended in/started from current block
static int32_t dataBlockPartialRequired(STimeWindow* pWindow, SBlock* pBlock) {
 return ((pWindow->ekey < pBlock->maxKey.ts && pWindow->ekey >= pBlock->minKey.ts) ||
     (pWindow->skey <= pBlock->maxKey.ts && pWindow->skey > pBlock->minKey.ts));
}

static SFileDataBlockInfo* getCurrentBlockInfo(SDataBlockIter* pBlockIter) {
  SFileDataBlockInfo* pFBlockInfo = taosArrayGet(pBlockIter->blockList, pBlockIter->index);
  return pFBlockInfo;
}

static bool overlapWithNeighborBlock(SFileDataBlockInfo *pFBlockInfo, SBlock* pBlock, STableBlockScanInfo* pTableBlockScanInfo) {
  // it is the last block in current file, no chance to overlap with neighbor blocks.
  if(pFBlockInfo->tbBlockIdx == taosArrayGetSize(pTableBlockScanInfo->pBlockList) - 1) { // last block in current file,
    return false;
  }

  SBlock* pNext = taosArrayGet(pTableBlockScanInfo->pBlockList, pFBlockInfo->tbBlockIdx + 1);
  return (pNext->minKey.ts == pBlock->maxKey.ts);
}

static bool bufferDataInFileBlockGap(int32_t order, TSDBKEY key, SBlock* pBlock) {
  bool ascScan = ASCENDING_TRAVERSE(order);

  return (ascScan && (key.ts != TSKEY_INITIAL_VAL && key.ts <= pBlock->minKey.ts)) ||
       (!ascScan && (key.ts != TSKEY_INITIAL_VAL && key.ts >= pBlock->maxKey.ts));
}

static bool keyOverlapFileBlock(TSDBKEY key, SBlock* pBlock, SVersionRange* pVerRange) {
  return (key.ts >= pBlock->minKey.ts && key.ts <= pBlock->maxKey.ts) && (pBlock->maxVersion >= pVerRange->minVer) && (pBlock->minVersion <= pVerRange->maxVer);
}

static bool fileBlockShouldLoad(STsdbReader* pReader, SFileDataBlockInfo *pFBlock, SBlock* pBlock, STableBlockScanInfo *pScanInfo, TSDBKEY key) {
  return (dataBlockPartialRequired(&pReader->window, pBlock) || overlapWithNeighborBlock(pFBlock, pBlock, pScanInfo) ||
      keyOverlapFileBlock(key, pBlock, &pReader->verRange));
}

static int32_t buildInmemDataBlock(STsdbReader* pReader, STableBlockScanInfo* pBlockScanInfo, TSDBKEY *key) {
  if (!(pBlockScanInfo->imemHasVal || pBlockScanInfo->memHasVal)) {
    return TSDB_CODE_SUCCESS;
  }

  SSDataBlock* pBlock = pReader->pResBlock;

  int64_t st = taosGetTimestampUs();
  int32_t code = buildInmemDataBlockImpl(pBlockScanInfo, *key, pReader->capacity, pReader);

  int64_t elapsedTime = taosGetTimestampUs() - st;

  tsdbDebug("%p build data block from cache completed, elapsed time:%" PRId64 " us, numOfRows:%d, numOfCols:%d, %s",
            pReader, elapsedTime, pBlock->info.rows, (int32_t)blockDataGetNumOfCols(pBlock), pReader->idStr);

  pBlock->info.uid = pBlockScanInfo->uid;
  setComposedBlockFlag(pReader, true);
  return code;
}

static int32_t buildComposedDataBlockImpl(STsdbReader* pReader, SFileDataBlockInfo* pFBlock, SBlock* pBlock, STableBlockScanInfo* pBlockScanInfo) {
  SFileBlockDumpInfo *pDumpInfo = &pReader->status.fBlockDumpInfo;
  SBlockData* pBlockData = &pReader->status.fileBlockData;

  SRowMerger merge = {0};
  STSRow*    pTSRow = NULL;
  TSKEY      mergeTs = TSKEY_INITIAL_VAL;

  int64_t key = pBlockData->aTSKEY[pDumpInfo->rowIndex];
  TSDBROW* pRow = getValidRow(pBlockScanInfo->iter, &pBlockScanInfo->memHasVal, pReader);
  TSDBROW* piRow = getValidRow(pBlockScanInfo->iiter, &pBlockScanInfo->imemHasVal, pReader);

  if (pBlockScanInfo->memHasVal && pBlockScanInfo->imemHasVal) {
    TSDBKEY k = TSDBROW_KEY(pRow);
    TSDBKEY ik = TSDBROW_KEY(piRow);

    // [1&2] key <= [k.ts|ik.ts]
    if (key <= k.ts || key <= ik.ts) {
      TSDBROW fRow = tsdbRowFromBlockData(pBlockData, pDumpInfo->rowIndex);
      tRowMergerInit(&merge, &fRow, pReader->pSchema);

      doLoadRowsOfIdenticalTsInFileBlock(pFBlock, pBlock, pBlockData, pBlockScanInfo, pReader, &merge);

      if (ik.ts == mergeTs) {
        doLoadRowsOfIdenticalTs(pBlockScanInfo->iiter, &pBlockScanInfo->imemHasVal, ik.ts, &merge, pReader);
      }

      if (k.ts == mergeTs) {
        doLoadRowsOfIdenticalTs(pBlockScanInfo->iter, &pBlockScanInfo->memHasVal, k.ts, &merge, pReader);
      }

      tRowMergerGetRow(&merge, &pTSRow);
      doAppendOneRow(pReader->pResBlock, pReader, pTSRow);
    } else {
      // [3] ik.ts < key <= k.ts
      if (ik.ts < k.ts) {
        doLoadRowsOfIdenticalTs(pBlockScanInfo->iiter, &pBlockScanInfo->imemHasVal, ik.ts, &merge, pReader);
        tRowMergerGetRow(&merge, &pTSRow);
        doAppendOneRow(pReader->pResBlock, pReader, pTSRow);
        return TSDB_CODE_SUCCESS;
      }

      // [4] k.ts < key <= ik.ts
      if (k.ts < ik.ts) {
        doLoadRowsOfIdenticalTs(pBlockScanInfo->iter, &pBlockScanInfo->memHasVal, k.ts, &merge, pReader);
        tRowMergerGetRow(&merge, &pTSRow);
        doAppendOneRow(pReader->pResBlock, pReader, pTSRow);
        return TSDB_CODE_SUCCESS;
      }

      // [5] k.ts == ik.ts < key
      if (k.ts == ik.ts) {
        doLoadRowsOfIdenticalTs(pBlockScanInfo->iiter, &pBlockScanInfo->imemHasVal, ik.ts, &merge, pReader);
        tRowMergerGetRow(&merge, &pTSRow);

        if (k.ts == mergeTs) {
          doLoadRowsOfIdenticalTs(pBlockScanInfo->iter, &pBlockScanInfo->memHasVal, k.ts, &merge, pReader);
        }

        tRowMergerGetRow(&merge, &pTSRow);
        doAppendOneRow(pReader->pResBlock, pReader, pTSRow);
        return TSDB_CODE_SUCCESS;
      }

      // [6] k.ts < ik.ts < key
      if (k.ts < ik.ts) {
        doLoadRowsOfIdenticalTs(pBlockScanInfo->iter, &pBlockScanInfo->memHasVal, k.ts, &merge, pReader);
        tRowMergerGetRow(&merge, &pTSRow);
        doAppendOneRow(pReader->pResBlock, pReader, pTSRow);

        return TSDB_CODE_SUCCESS;
      }

      // [6] ik.ts < k.ts < key
      if (ik.ts < k.ts) {
        doLoadRowsOfIdenticalTs(pBlockScanInfo->iiter, &pBlockScanInfo->imemHasVal, ik.ts, &merge, pReader);
        tRowMergerGetRow(&merge, &pTSRow);
        doAppendOneRow(pReader->pResBlock, pReader, pTSRow);
        return TSDB_CODE_SUCCESS;
      }
    }
  } else if (pBlockScanInfo->imemHasVal) {
    TSDBKEY ik = TSDBROW_KEY(piRow);
    if (key <= ik.ts) {
      TSDBROW fRow = tsdbRowFromBlockData(pBlockData, pDumpInfo->rowIndex);
      tRowMergerInit(&merge, &fRow, pReader->pSchema);

      doLoadRowsOfIdenticalTsInFileBlock(pFBlock, pBlock, pBlockData, pBlockScanInfo, pReader, &merge);

      if (ik.ts == mergeTs) {
        doLoadRowsOfIdenticalTs(pBlockScanInfo->iiter, &pBlockScanInfo->imemHasVal, ik.ts, &merge, pReader);
      }

      tRowMergerGetRow(&merge, &pTSRow);
      doAppendOneRow(pReader->pResBlock, pReader, pTSRow);
      return TSDB_CODE_SUCCESS;
    }

    if (ik.ts < key) {
      doLoadRowsOfIdenticalTs(pBlockScanInfo->iiter, &pBlockScanInfo->imemHasVal, ik.ts, &merge, pReader);
      tRowMergerGetRow(&merge, &pTSRow);
      doAppendOneRow(pReader->pResBlock, pReader, pTSRow);
      return TSDB_CODE_SUCCESS;
    }
  } else { // pBlockScanInfo->memHasVal != NULL
    TSDBKEY k = TSDBROW_KEY(pRow);
    if (key <= k.ts) {
      TSDBROW fRow = tsdbRowFromBlockData(pBlockData, pDumpInfo->rowIndex);
      tRowMergerInit(&merge, &fRow, pReader->pSchema);

      doLoadRowsOfIdenticalTsInFileBlock(pFBlock, pBlock, pBlockData, pBlockScanInfo, pReader, &merge);

      if (k.ts == mergeTs) {
        doLoadRowsOfIdenticalTs(pBlockScanInfo->iter, &pBlockScanInfo->memHasVal, k.ts, &merge, pReader);
      }

      tRowMergerGetRow(&merge, &pTSRow);
      doAppendOneRow(pReader->pResBlock, pReader, pTSRow);
      return TSDB_CODE_SUCCESS;
    }

    if (k.ts < key) {
      doLoadRowsOfIdenticalTs(pBlockScanInfo->iter, &pBlockScanInfo->memHasVal, k.ts, &merge, pReader);
      tRowMergerGetRow(&merge, &pTSRow);
      doAppendOneRow(pReader->pResBlock, pReader, pTSRow);
      return TSDB_CODE_SUCCESS;
    }
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t buildComposedDataBlock(STsdbReader* pReader, SFileDataBlockInfo* pFBlock, SBlock* pBlock, STableBlockScanInfo* pBlockScanInfo) {
  SSDataBlock* pResBlock = pReader->pResBlock;

  while(1) {
    buildComposedDataBlockImpl(pReader, pFBlock, pBlock, pBlockScanInfo);
    SFileBlockDumpInfo* pDumpInfo = &pReader->status.fBlockDumpInfo;

    SFileDataBlockInfo* pBlockInfo = getCurrentBlockInfo(&pReader->status.blockIter);
    if (pBlockInfo->tbBlockIdx == pFBlock->tbBlockIdx) {  // still in the same file block now

      if (pDumpInfo->rowIndex >= pBlock->nRow) {
        break;
      }

      if (pResBlock->info.rows >= pReader->capacity) {
        break;
      }
    } else {  // todo traverse to next file due to time window overlap
      if (pResBlock->info.rows >= pReader->capacity) {
        ASSERT(0);
        return TSDB_CODE_SUCCESS;
      }
    }
  }

  pResBlock->info.uid = pBlockScanInfo->uid;
  setComposedBlockFlag(pReader, true);
  return TSDB_CODE_SUCCESS;
}

void setComposedBlockFlag(STsdbReader* pReader, bool composed) { pReader->status.composedDataBlock = composed; }

static int32_t initMemIterator(STableBlockScanInfo* pBlockScanInfo, STsdbReader* pReader) {
  if (pBlockScanInfo->iterInit) {
    return TSDB_CODE_SUCCESS;
  }

  int32_t code = TSDB_CODE_SUCCESS;
  TSDBKEY startKey = {.ts = pReader->window.skey, .version = pReader->verRange.minVer};

  STbData* d = NULL;
  if (pReader->pTsdb->mem != NULL) {
    tsdbGetTbDataFromMemTable(pReader->pTsdb->mem, pReader->suid, pBlockScanInfo->uid, &d);
    if (d != NULL) {
      code = tsdbTbDataIterCreate(d, &startKey, 0, &pBlockScanInfo->iter);
      if (code == TSDB_CODE_SUCCESS) {
        pBlockScanInfo->memHasVal = (tsdbTbDataIterGet(pBlockScanInfo->iter) != NULL);

        tsdbDebug("%p uid:%" PRId64 ", check data in mem from skey:%" PRId64 ", order:%d, ts range in buf:%" PRId64
                      "-%" PRId64 " %s",
                  pReader, pBlockScanInfo->uid, pReader->window.skey, pReader->order, d->minKey, d->maxKey,
                  pReader->idStr);
      } else {
        tsdbError("%p uid:%" PRId64 ", failed to create iterator for imem, code:%s, %s",
                  pReader, pBlockScanInfo->uid, tstrerror(code), pReader->idStr);
        return code;
      }
    }
  } else {
     tsdbDebug("%p uid:%" PRId64 ", no data in mem, %s", pReader, pBlockScanInfo->uid, pReader->idStr);
  }

  STbData* di = NULL;
  if (pReader->pTsdb->imem != NULL) {
    tsdbGetTbDataFromMemTable(pReader->pTsdb->imem, pReader->suid, pBlockScanInfo->uid, &di);
    if (di != NULL) {
      code = tsdbTbDataIterCreate(di, &startKey, 0, &pBlockScanInfo->iiter);
      if (code == TSDB_CODE_SUCCESS) {
        pBlockScanInfo->imemHasVal = (tsdbTbDataIterGet(pBlockScanInfo->iiter) != NULL);

        tsdbDebug("%p uid:%" PRId64 ", check data in imem from skey:%" PRId64 ", order:%d, ts range in buf:%" PRId64
                  "-%" PRId64 " %s",
                  pReader, pBlockScanInfo->uid, pReader->window.skey, pReader->order, di->minKey, di->maxKey,
                  pReader->idStr);
      } else {
        tsdbError("%p uid:%" PRId64 ", failed to create iterator for mem, code:%s, %s",
                  pReader, pBlockScanInfo->uid, tstrerror(code), pReader->idStr);
        return code;
      }
    }
  } else {
    tsdbDebug("%p uid:%" PRId64 ", no data in imem, %s", pReader, pBlockScanInfo->uid, pReader->idStr);
  }

  pBlockScanInfo->iterInit = true;
  return TSDB_CODE_SUCCESS;
}

static TSDBKEY getCurrentKeyInBuf(SDataBlockIter* pBlockIter, STsdbReader* pReader) {
  TSDBKEY key = {.ts = TSKEY_INITIAL_VAL};

  SFileDataBlockInfo* pFBlock = getCurrentBlockInfo(pBlockIter);
  STableBlockScanInfo* pScanInfo = taosHashGet(pReader->status.pTableMap, &pFBlock->uid, sizeof(pFBlock->uid));

  initMemIterator(pScanInfo, pReader);
  if (pScanInfo->memHasVal) {
    TSDBROW* pRow = getValidRow(pScanInfo->iter, &pScanInfo->memHasVal, pReader);
    key = TSDBROW_KEY(pRow);
  }

  if (pScanInfo->imemHasVal) {
    TSDBROW* pRow = getValidRow(pScanInfo->iiter, &pScanInfo->imemHasVal, pReader);
    TSDBKEY k = TSDBROW_KEY(pRow);
    if (key.ts > k.ts) {
      key = k;
    }
  }

  return key;
}

static int32_t moveToNextFile(STsdbReader* pReader, int32_t* numOfBlocks) {
  SReaderStatus* pStatus = &pReader->status;

  while (1) {
    bool hasNext = filesetIteratorNext(&pStatus->fileIter, pReader->order, pReader);
    if (!hasNext) { // no data files on disk
      break;
    }

    SArray* pIndexList = taosArrayInit(4, sizeof(SBlockIdx));
    int32_t code = doLoadBlockIndex(pReader, pReader->pFileReader, pIndexList);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }

    if (taosArrayGetSize(pIndexList) > 0) {
      uint32_t numOfValidTable = 0;
      code = doLoadFileBlock(pReader, pIndexList, &numOfValidTable, numOfBlocks);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }

      if (numOfValidTable > 0) {
        break;
      }
    }

    // no blocks in current file, try next files
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t doBuildDataBlock(STsdbReader* pReader) {
  int32_t code = TSDB_CODE_SUCCESS;

  SReaderStatus* pStatus = &pReader->status;
  SDataBlockIter* pBlockIter = &pStatus->blockIter;

  SFileDataBlockInfo* pFBlock = getCurrentBlockInfo(pBlockIter);
  STableBlockScanInfo* pScanInfo = taosHashGet(pReader->status.pTableMap, &pFBlock->uid, sizeof(pFBlock->uid));

  SBlock* pBlock = taosArrayGet(pScanInfo->pBlockList, pFBlock->tbBlockIdx);

  TSDBKEY key = getCurrentKeyInBuf(pBlockIter, pReader);
  if (fileBlockShouldLoad(pReader, pFBlock, pBlock, pScanInfo, key)) {
    SBlockData data = {0};
    tBlockDataInit(&data);

    code = doLoadFileBlockData(pReader, pBlockIter, pScanInfo, &data);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }

    // build composed data block
    code = buildComposedDataBlock(pReader, pFBlock, pBlock, pScanInfo);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }
  } else if (bufferDataInFileBlockGap(pReader->order, key, pBlock)) {
    // data in memory that are earlier than current file block
    TSDBKEY maxKey = {.ts = pReader->window.ekey, .version = pReader->verRange.maxVer};
    code = buildInmemDataBlock(pReader, pScanInfo, &maxKey);
    // build data block from in-memory buffer data completed.
  } else { // whole block is required, return it directly
    // todo
    // 1. the version of all rows should be less than the endVersion
    // 2. current block should not overlap with next neighbor block
    // 3. current timestamp should not be overlap with each other
    SDataBlockInfo* pInfo = &pReader->pResBlock->info;
    pInfo->rows   = pBlock->nRow;
    pInfo->uid    = pScanInfo->uid;
    pInfo->window = (STimeWindow) {.skey = pBlock->minKey.ts, .ekey = pBlock->maxKey.ts};
    setComposedBlockFlag(pReader, false);
    setBlockDumpCompleted(&pStatus->fBlockDumpInfo, pBlock, pReader->order);
  }

  return code;
}

static int32_t buildBlockFromBufferSequentially(STsdbReader* pReader) {
  SReaderStatus* pStatus = &pReader->status;

  while(1) {
    if (pStatus->pTableIter == NULL) {
      pStatus->pTableIter = taosHashIterate(pStatus->pTableMap, NULL);
      if (pStatus->pTableIter == NULL) {
        return TSDB_CODE_SUCCESS;
      }
    }

    STableBlockScanInfo* pBlockScanInfo = pStatus->pTableIter;
    initMemIterator(pBlockScanInfo, pReader);

    TSDBKEY maxKey = {.ts = pReader->window.ekey, .version = pReader->verRange.maxVer};
    int32_t code = buildInmemDataBlock(pReader, pBlockScanInfo, &maxKey);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }

    if (pReader->pResBlock->info.rows > 0) {
      return TSDB_CODE_SUCCESS;
    }

    // current table is exhausted, let's try the next table
    pStatus->pTableIter = taosHashIterate(pStatus->pTableMap, pStatus->pTableIter);
    if (pStatus->pTableIter == NULL) {
      return TSDB_CODE_SUCCESS;
    }
  }
}

static int32_t initForFirstBlockOfFile(STsdbReader* pReader, SDataBlockIter* pBlockIter) {
  int32_t numOfBlocks = 0;
  int32_t code = moveToNextFile(pReader, &numOfBlocks);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  // all data files are consumed, try data in buffer
  if (numOfBlocks == 0) {
    pReader->status.loadFromFile = false;
    return code;
  }

  // initialize the block iterator for a new fileset
  code = initBlockIterator(pReader, pBlockIter, numOfBlocks);
  return code;
}

static int32_t buildBlockFromFiles(STsdbReader* pReader) {
  int32_t code = TSDB_CODE_SUCCESS;

  SReaderStatus* pStatus = &pReader->status;
  SFileSetIter*  pFIter = &pStatus->fileIter;

  SDataBlockIter* pBlockIter = &pReader->status.blockIter;
  ASSERT (pFIter->index < pFIter->numOfFiles);

  if (pReader->status.blockIter.index == -1) {
    code = initForFirstBlockOfFile(pReader, pBlockIter);
    if ((code != TSDB_CODE_SUCCESS) || (pReader->status.loadFromFile == false)) {
      return code;
    }
    
    code = doBuildDataBlock(pReader);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }
  } else {
    SFileBlockDumpInfo* pDumpInfo = &pReader->status.fBlockDumpInfo;
    SFileDataBlockInfo* pFBlock = getCurrentBlockInfo(&pReader->status.blockIter);
    STableBlockScanInfo* pScanInfo = taosHashGet(pReader->status.pTableMap, &pFBlock->uid, sizeof(pFBlock->uid));
    SBlock* pBlock = taosArrayGet(pScanInfo->pBlockList, pFBlock->tbBlockIdx);

    // current block are exhausted, try the next file block
    if (pDumpInfo->rowIndex >= pBlock->nRow) {
      // try next data block in current file
      bool hasNext = blockIteratorNext(&pReader->status.blockIter);
      if (!hasNext) {  // current file is exhausted, let's try the next file
        code = initForFirstBlockOfFile(pReader, pBlockIter);
        if ((code != TSDB_CODE_SUCCESS) || (pReader->status.loadFromFile == false)) {
          return code;
        }
      }
      
      code = doBuildDataBlock(pReader);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }
    } else {
      code = buildComposedDataBlock(pReader, pFBlock, pBlock, pScanInfo);
      return code;
    }
  }

  return code;
}

// // todo not unref yet, since it is not support multi-group interpolation query
// static UNUSED_FUNC void changeQueryHandleForInterpQuery(STsdbReader* pHandle) {
//   // filter the queried time stamp in the first place
//   STsdbReader* pTsdbReadHandle = (STsdbReader*)pHandle;

//   // starts from the buffer in case of descending timestamp order check data blocks
//   size_t numOfTables = taosArrayGetSize(pTsdbReadHandle->pTableCheckInfo);

//   int32_t i = 0;
//   while (i < numOfTables) {
//     STableBlockScanInfo* pCheckInfo = taosArrayGet(pTsdbReadHandle->pTableCheckInfo, i);

//     // the first qualified table for interpolation query
//     //    if ((pTsdbReadHandle->window.skey <= pCheckInfo->pTableObj->lastKey) &&
//     //        (pCheckInfo->pTableObj->lastKey != TSKEY_INITIAL_VAL)) {
//     //      break;
//     //    }

//     i++;
//   }

//   // there are no data in all the tables
//   if (i == numOfTables) {
//     return;
//   }

//   STableBlockScanInfo info = *(STableBlockScanInfo*)taosArrayGet(pTsdbReadHandle->pTableCheckInfo, i);
//   taosArrayClear(pTsdbReadHandle->pTableCheckInfo);

//   info.lastKey = pTsdbReadHandle->window.skey;
//   taosArrayPush(pTsdbReadHandle->pTableCheckInfo, &info);
// }

TSDBROW* getValidRow(STbDataIter* pIter, bool* hasVal, STsdbReader* pReader) {
  if (!(*hasVal)) {
    return NULL;
  }

  TSDBROW* pRow = tsdbTbDataIterGet(pIter);

  TSDBKEY key = TSDBROW_KEY(pRow);
  if (key.ts > pReader->window.ekey) {
    *hasVal = false;
    return NULL;
  }

  if (key.version <= pReader->verRange.maxVer) {
    return pRow;
  }

  while(1) {
    *hasVal = tsdbTbDataIterNext(pIter);
    if (!(*hasVal)) {
      return NULL;
    }

    pRow = tsdbTbDataIterGet(pIter);

    key = TSDBROW_KEY(pRow);
    if (key.ts > pReader->window.ekey) {
      *hasVal = false;
      return NULL;
    }

    if (key.version <= pReader->verRange.maxVer) {
      return pRow;
    }
  }
}

int32_t doLoadRowsOfIdenticalTs(STbDataIter *pIter, bool* hasVal, int64_t ts, SRowMerger* pMerger, STsdbReader* pReader) {
  while (1) {
    *hasVal = tsdbTbDataIterNext(pIter);
    if (!(*hasVal)) {
      break;
    }

    TSDBROW* pRow = getValidRow(pIter, hasVal, pReader);
    TSDBKEY k = TSDBROW_KEY(pRow);
    if (k.ts > ts) {
      break;
    }

    tRowMerge(pMerger, pRow);
  }

  return TSDB_CODE_SUCCESS;
}

int32_t doLoadRowsOfIdenticalTsInFileBlock(SFileDataBlockInfo* pFBlock, SBlock* pBlock, SBlockData* pBlockData,
                                           STableBlockScanInfo* pScanInfo, STsdbReader* pReader, SRowMerger* pMerger) {
  SFileBlockDumpInfo* pDumpInfo = &pReader->status.fBlockDumpInfo;

  int64_t key = pBlockData->aTSKEY[pDumpInfo->rowIndex];
  if (pDumpInfo->rowIndex < pBlockData->nRow - 1) {
    if (pBlockData->aTSKEY[pDumpInfo->rowIndex + 1] == key) {
      int32_t rowIndex = pDumpInfo->rowIndex + 1;

      while (pBlockData->aTSKEY[rowIndex] == key) {
        if (pBlockData->aVersion[rowIndex] > pReader->verRange.maxVer) {
          continue;
        }

        TSDBROW fRow = tsdbRowFromBlockData(pBlockData, rowIndex);
        tRowMerge(pMerger, &fRow);
        rowIndex += 1;
      }

      pDumpInfo->rowIndex = rowIndex;
    }
  } else { // last row of current block, check if current block is overlapped with neighbor block
    pDumpInfo->rowIndex += 1;
    bool overlap = overlapWithNeighborBlock(pFBlock, pBlock, pScanInfo);
    if (overlap) {
      // load next block

    }

  }

  return TSDB_CODE_SUCCESS;
}

static void checkUpdateSchema(TSDBROW* pRow, uint64_t uid, STsdbReader* pReader) {
  int32_t sversion = TSDBROW_SVERSION(pRow);

  if (pReader->pSchema == NULL) {
    pReader->pSchema = metaGetTbTSchema(pReader->pTsdb->pVnode->pMeta, uid, sversion);
  } else if (pReader->pSchema->version != sversion) {
    taosMemoryFreeClear(pReader->pSchema);
    pReader->pSchema = metaGetTbTSchema(pReader->pTsdb->pVnode->pMeta, uid, sversion);
  }
}

int32_t tsdbGetNextRowInMem(STableBlockScanInfo* pBlockScanInfo, STsdbReader* pReader, STSRow** pTSRow) {
  TSKEY     mergeTs = TSKEY_INITIAL_VAL;

  SRowMerger merge = {0};

  TSDBROW* pRow  = getValidRow(pBlockScanInfo->iter, &pBlockScanInfo->memHasVal, pReader);
  TSDBROW* piRow = getValidRow(pBlockScanInfo->iiter, &pBlockScanInfo->imemHasVal, pReader);

  TSDBKEY k  = {.ts = TSKEY_INITIAL_VAL};
  TSDBKEY ik = {.ts = TSKEY_INITIAL_VAL};

  if (pBlockScanInfo->memHasVal && pBlockScanInfo->imemHasVal) {
    k = TSDBROW_KEY(pRow);
    ik = TSDBROW_KEY(piRow);

    if (ik.ts <= k.ts) {
      checkUpdateSchema(piRow, pBlockScanInfo->uid, pReader);

      tRowMergerInit(&merge, piRow, pReader->pSchema);
      doLoadRowsOfIdenticalTs(pBlockScanInfo->iiter, &pBlockScanInfo->imemHasVal, ik.ts, &merge, pReader);

      if (k.ts == mergeTs) {
        doLoadRowsOfIdenticalTs(pBlockScanInfo->iter, &pBlockScanInfo->memHasVal, k.ts, &merge, pReader);
      }

      tRowMergerGetRow(&merge, pTSRow);
      return TSDB_CODE_SUCCESS;
    } else {  // k.ts < ik.ts
      checkUpdateSchema(pRow, pBlockScanInfo->uid, pReader);

      tRowMergerInit(&merge, pRow, pReader->pSchema);
      doLoadRowsOfIdenticalTs(pBlockScanInfo->iter, &pBlockScanInfo->memHasVal, k.ts, &merge, pReader);
      tRowMergerGetRow(&merge, pTSRow);
      return TSDB_CODE_SUCCESS;
    }
  }


  if (pBlockScanInfo->memHasVal) {
    k = TSDBROW_KEY(pRow);
    checkUpdateSchema(pRow, pBlockScanInfo->uid, pReader);

    tRowMergerInit(&merge, pRow, pReader->pSchema);
    doLoadRowsOfIdenticalTs(pBlockScanInfo->iter, &pBlockScanInfo->memHasVal, k.ts, &merge, pReader);
    tRowMergerGetRow(&merge, pTSRow);
    return TSDB_CODE_SUCCESS;
  }

  if (pBlockScanInfo->imemHasVal) {
    ik = TSDBROW_KEY(piRow);
    checkUpdateSchema(piRow, pBlockScanInfo->uid, pReader);

    tRowMergerInit(&merge, piRow, pReader->pSchema);
    doLoadRowsOfIdenticalTs(pBlockScanInfo->iiter, &pBlockScanInfo->imemHasVal, ik.ts, &merge, pReader);
    tRowMergerGetRow(&merge, pTSRow);
    return TSDB_CODE_SUCCESS;
  }

  return TSDB_CODE_SUCCESS;
}

int32_t doAppendOneRow(SSDataBlock* pBlock, STsdbReader* pReader, STSRow* pTSRow) {
  int32_t numOfRows = pBlock->info.rows;
  int32_t numOfCols = (int32_t)taosArrayGetSize(pBlock->pDataBlock);

  SBlockLoadSuppInfo* pSupInfo = &pReader->suppInfo;
  STSchema *pSchema = pReader->pSchema;

  SColVal colVal = {0};
  int32_t i = 0, j = 0;

  SColumnInfoData* pColInfoData = taosArrayGet(pBlock->pDataBlock, i);
  if (pColInfoData->info.colId == PRIMARYKEY_TIMESTAMP_COL_ID) {
    colDataAppend(pColInfoData, numOfRows, (const char*)&pTSRow->ts, false);
    i += 1;
  }

  while (i < numOfCols && j < pSchema->numOfCols) {
    pColInfoData = taosArrayGet(pBlock->pDataBlock, i);
    col_id_t colId = pColInfoData->info.colId;

    if (colId == pSchema->columns[j].colId) {
      tTSRowGetVal(pTSRow, pReader->pSchema, j, &colVal);
      doCopyColVal(pColInfoData, numOfRows, i, &colVal, pSupInfo);
      i += 1;
      j += 1;
    } else if (colId < pSchema->columns[j].colId) {
      colDataAppendNULL(pColInfoData, numOfRows);
      i += 1;
    } else if (colId > pSchema->columns[j].colId) {
      j += 1;
    }
  }

  pBlock->info.rows += 1;
  return TSDB_CODE_SUCCESS;
}

int32_t buildInmemDataBlockImpl(STableBlockScanInfo* pBlockScanInfo, TSDBKEY maxKey, int32_t capacity, STsdbReader* pReader) {
  SSDataBlock* pBlock = pReader->pResBlock;

  do {
    STSRow* pTSRow = NULL;
    tsdbGetNextRowInMem(pBlockScanInfo, pReader, &pTSRow);
    doAppendOneRow(pBlock, pReader, pTSRow);

    if (pBlockScanInfo->memHasVal) {
      TSDBROW* pRow = tsdbTbDataIterGet(pBlockScanInfo->iter);
      TSDBKEY k = TSDBROW_KEY(pRow);
      if (k.ts >= maxKey.ts) {
        break;
      }
    }

    if (pBlockScanInfo->imemHasVal) {
      TSDBROW* pRow = tsdbTbDataIterGet(pBlockScanInfo->iiter);
      TSDBKEY k = TSDBROW_KEY(pRow);
      if (k.ts >= maxKey.ts) {
        break;
      }
    }

    // no data in buffer, return immediately
    if (!(pBlockScanInfo->memHasVal || pBlockScanInfo->imemHasVal)) {
      break;
    }

    if (pBlock->info.rows >= capacity) {
      break;
    }
  } while (1);

  ASSERT(pBlock->info.rows <= capacity);
  return TSDB_CODE_SUCCESS;
}

/**
 * @brief Get all suids since suid
 *
 * @param pMeta
 * @param suid return all suids in one vnode if suid is 0
 * @param list
 * @return int32_t
 */
int32_t tsdbGetStbIdList(SMeta* pMeta, int64_t suid, SArray* list) {
  SMStbCursor* pCur = metaOpenStbCursor(pMeta, suid);
  if(!pCur) {
    return TSDB_CODE_FAILED;
  }

  while (1) {
    tb_uid_t id = metaStbCursorNext(pCur);
    if (id == 0) {
      break;
    }

    taosArrayPush(list, &id);
  }

  metaCloseStbCursor(pCur);
  return TSDB_CODE_SUCCESS;
}

// static void destroyHelper(void* param) {
//   if (param == NULL) {
//     return;
//   }

//   //  tQueryInfo* pInfo = (tQueryInfo*)param;
//   //  if (pInfo->optr != TSDB_RELATION_IN) {
//   //    taosMemoryFreeClear(pInfo->q);
//   //  } else {
//   //    taosHashCleanup((SHashObj *)(pInfo->q));
//   //  }

//   taosMemoryFree(param);
// }

// #define TSDB_PREV_ROW 0x1
// #define TSDB_NEXT_ROW 0x2

// static bool loadBlockOfActiveTable(STsdbReader* pTsdbReadHandle) {
//   if (pTsdbReadHandle->checkFiles) {
//     // check if the query range overlaps with the file data block
//     bool exists = true;

//     int32_t code = buildBlockFromFiles(pTsdbReadHandle, &exists);
//     if (code != TSDB_CODE_SUCCESS) {
//       pTsdbReadHandle->checkFiles = false;
//       return false;
//     }

//     if (exists) {
//       tsdbRetrieveDataBlock((STsdbReader**)pTsdbReadHandle, NULL);
//       if (pTsdbReadHandle->currentLoadExternalRows && pTsdbReadHandle->window.skey == pTsdbReadHandle->window.ekey) {
//         SColumnInfoData* pColInfo = taosArrayGet(pTsdbReadHandle->pColumns, 0);
//         assert(*(int64_t*)pColInfo->pData == pTsdbReadHandle->window.skey);
//       }

//       pTsdbReadHandle->currentLoadExternalRows = false;  // clear the flag, since the exact matched row is found.
//       return exists;
//     }

//     pTsdbReadHandle->checkFiles = false;
//   }

//   if (hasMoreDataInCache(pTsdbReadHandle)) {
//     pTsdbReadHandle->currentLoadExternalRows = false;
//     return true;
//   }

//   // current result is empty
//   if (pTsdbReadHandle->currentLoadExternalRows && pTsdbReadHandle->window.skey == pTsdbReadHandle->window.ekey &&
//       pTsdbReadHandle->cur.rows == 0) {
//     //    SMemTable* pMemRef = pTsdbReadHandle->pMemTable;

//     //    doGetExternalRow(pTsdbReadHandle, TSDB_PREV_ROW, pMemRef);
//     //    doGetExternalRow(pTsdbReadHandle, TSDB_NEXT_ROW, pMemRef);

//     bool result = tsdbGetExternalRow(pTsdbReadHandle);

//     //    pTsdbReadHandle->prev = doFreeColumnInfoData(pTsdbReadHandle->prev);
//     //    pTsdbReadHandle->next = doFreeColumnInfoData(pTsdbReadHandle->next);
//     pTsdbReadHandle->currentLoadExternalRows = false;

//     return result;
//   }

//   return false;
// }

// static bool loadDataBlockFromTableSeq(STsdbReader* pTsdbReadHandle) {
//   size_t numOfTables = taosArrayGetSize(pTsdbReadHandle->pTableCheckInfo);
//   assert(numOfTables > 0);

//   int64_t stime = taosGetTimestampUs();

//   while (pTsdbReadHandle->activeIndex < numOfTables) {
//     if (loadBlockOfActiveTable(pTsdbReadHandle)) {
//       return true;
//     }

//     STableBlockScanInfo* pCheckInfo = taosArrayGet(pTsdbReadHandle->pTableCheckInfo, pTsdbReadHandle->activeIndex);
//     pCheckInfo->numOfBlocks = 0;

//     pTsdbReadHandle->activeIndex += 1;
//     pTsdbReadHandle->locateStart = false;
//     pTsdbReadHandle->checkFiles = true;
//     pTsdbReadHandle->cur.rows = 0;
//     pTsdbReadHandle->currentLoadExternalRows = pTsdbReadHandle->loadExternalRow;

//     terrno = TSDB_CODE_SUCCESS;

//     int64_t elapsedTime = taosGetTimestampUs() - stime;
//     pTsdbReadHandle->cost.checkForNextTime += elapsedTime;
//   }

//   return false;
// }

// bool tsdbGetExternalRow(STsdbReader* pHandle) {
//   STsdbReader*   pTsdbReadHandle = (STsdbReader*)pHandle;
//   SQueryFilePos* cur = &pTsdbReadHandle->cur;

//   cur->fid = INT32_MIN;
//   cur->mixBlock = true;
//   if (pTsdbReadHandle->prev == NULL || pTsdbReadHandle->next == NULL) {
//     cur->rows = 0;
//     return false;
//   }

//   int32_t numOfCols = (int32_t)QH_GET_NUM_OF_COLS(pTsdbReadHandle);
//   for (int32_t i = 0; i < numOfCols; ++i) {
//     SColumnInfoData* pColInfoData = taosArrayGet(pTsdbReadHandle->pColumns, i);
//     SColumnInfoData* first = taosArrayGet(pTsdbReadHandle->prev, i);

//     memcpy(pColInfoData->pData, first->pData, pColInfoData->info.bytes);

//     SColumnInfoData* sec = taosArrayGet(pTsdbReadHandle->next, i);
//     memcpy(((char*)pColInfoData->pData) + pColInfoData->info.bytes, sec->pData, pColInfoData->info.bytes);

//     if (i == 0 && pColInfoData->info.type == TSDB_DATA_TYPE_TIMESTAMP) {
//       cur->win.skey = *(TSKEY*)pColInfoData->pData;
//       cur->win.ekey = *(TSKEY*)(((char*)pColInfoData->pData) + TSDB_KEYSIZE);
//     }
//   }

//   cur->rows = 2;
//   return true;
// }

// static void* doFreeColumnInfoData(SArray* pColumnInfoData) {
//   if (pColumnInfoData == NULL) {
//     return NULL;
//   }

//   size_t cols = taosArrayGetSize(pColumnInfoData);
//   for (int32_t i = 0; i < cols; ++i) {
//     SColumnInfoData* pColInfo = taosArrayGet(pColumnInfoData, i);
//     colDataDestroy(pColInfo);
//   }

//   taosArrayDestroy(pColumnInfoData);
//   return NULL;
// }

// static void* destroyTableCheckInfo(SArray* pTableCheckInfo) {
//   size_t size = taosArrayGetSize(pTableCheckInfo);
//   for (int32_t i = 0; i < size; ++i) {
//     STableBlockScanInfo* p = taosArrayGet(pTableCheckInfo, i);
//     destroyTableMemIterator(p);

//     taosMemoryFreeClear(p->pCompInfo);
//   }

//   taosArrayDestroy(pTableCheckInfo);
//   return NULL;
// }

// ====================================== EXPOSED APIs ======================================
int32_t tsdbReaderOpen(SVnode* pVnode, SQueryTableDataCond* pCond, SArray* pTableList, STsdbReader** ppReader, const char* idstr) {
  int32_t code = tsdbReaderCreate(pVnode, pCond, ppReader, idstr);
  if (code) {
    goto _err;
  }

  STsdbReader* pReader = *ppReader;
  if (isEmptyQueryTimeWindow(&pReader->window, pReader->order)) {
    tsdbDebug("%p query window not overlaps with the data set, no result returned, %s", pReader, pReader->idStr);
    return TSDB_CODE_SUCCESS;
  }

  int32_t numOfTables = taosArrayGetSize(pTableList);
  pReader->status.pTableMap = createDataBlockScanInfo(pReader, pTableList->pData, numOfTables);
  if (pReader->status.pTableMap == NULL) {
    tsdbReaderClose(pReader);
    *ppReader = NULL;

    code = TSDB_CODE_TDB_OUT_OF_MEMORY;
    goto _err;
  }

#if 0
  // int32_t code = setCurrentSchema(pVnode, pReader);
  // if (code != TSDB_CODE_SUCCESS) {
  //   terrno = code;
  //   return NULL;
  // }

  // int32_t  numOfCols = taosArrayGetSize(pReader->suppInfo.defaultLoadColumn);
  // int16_t* ids = pReader->suppInfo.defaultLoadColumn->pData;

  // STSchema* pSchema = pReader->pSchema;

  // int32_t i = 0, j = 0;
  // while (i < numOfCols && j < pSchema->numOfCols) {
  //   if (ids[i] == pSchema->columns[j].colId) {
  //     pReader->suppInfo.slotIds[i] = j;
  //     i++;
  //     j++;
  //   } else if (ids[i] > pSchema->columns[j].colId) {
  //     j++;
  //   } else {
  //     //    tsdbReaderClose(pTsdbReadHandle);
  //     terrno = TSDB_CODE_INVALID_PARA;
  //     return NULL;
  //   }
  // }
#endif

  tsdbDebug("%p total numOfTable:%d in this query %s", pReader, numOfTables, pReader->idStr);
  return code;

_err:
  tsdbError("failed to create tsdb reader, code: %s %s", tstrerror(code), pReader->idStr);
  return code;
}

void tsdbReaderClose(STsdbReader* pReader) {
   if (pReader == NULL) {
     return;
   }

   blockDataDestroy(pReader->pResBlock);

   taosMemoryFreeClear(pReader->suppInfo.pstatis);
   taosMemoryFreeClear(pReader->suppInfo.plist);
   taosMemoryFree(pReader->suppInfo.slotIds);

   if (!isEmptyQueryTimeWindow(&pReader->window, pReader->order)) {
     //    tsdbMayUnTakeMemSnapshot(pTsdbReadHandle);
   } else {
     ASSERT(pReader->status.pTableMap == NULL);
   }
#if 0
//   if (pReader->status.pTableScanInfo != NULL) {
//     pReader->status.pTableScanInfo = destroyTableCheckInfo(pReader->status.pTableScanInfo);
//   }

//   tsdbDestroyReadH(&pReader->rhelper);

//   tdFreeDataCols(pReader->pDataCols);
//   pReader->pDataCols = NULL;
//
//   pReader->prev = doFreeColumnInfoData(pReader->prev);
//   pReader->next = doFreeColumnInfoData(pReader->next);
#endif

   SIOCostSummary* pCost = &pReader->cost;

   tsdbDebug("%p :io-cost summary: head-file read cnt:%" PRIu64 ", head-file time:%" PRIu64 " us, statis-info:%" PRId64
             " us, datablock:%" PRId64 " us, check data:%" PRId64 " us, %s",
             pReader, pCost->headFileLoad, pCost->headFileLoadTime, pCost->statisInfoLoadTime, pCost->blockLoadTime,
             pCost->checkForNextTime, pReader->idStr);

   taosMemoryFree(pReader->idStr);
   taosMemoryFree(pReader->pSchema);
   taosMemoryFreeClear(pReader);
}

bool tsdbNextDataBlock(STsdbReader* pReader) {
  if (isEmptyQueryTimeWindow(&pReader->window, pReader->order)) {
    return false;
  }

  // cleanup the data that belongs to the previous data block
  SSDataBlock* pBlock = pReader->pResBlock;
  blockDataCleanup(pBlock);

  int64_t stime = taosGetTimestampUs();
  int64_t elapsedTime = stime;
  SReaderStatus* pStatus = &pReader->status;

  if (pReader->type == BLOCK_LOAD_OFFSET_ORDER) {
    if (pStatus->loadFromFile) {
      int32_t code = buildBlockFromFiles(pReader);
      if (code != TSDB_CODE_SUCCESS) {
        return false;
      }

      if (pBlock->info.rows > 0) {
        return true;
      } else {
        buildBlockFromBufferSequentially(pReader);
        return pBlock->info.rows > 0;
      }
    } else { // no data in files, let's try the buffer
      buildBlockFromBufferSequentially(pReader);
      return pBlock->info.rows > 0;
    }
  } else if (pReader->type == BLOCK_LOAD_TABLESEQ_ORDER) {

  } else if (pReader->type == BLOCK_LOAD_EXTERN_ORDER) {

  } else {
    ASSERT(0);
  }
  // if (pReader->loadType == BLOCK_LOAD_TABLE_SEQ_ORDER) {
  //   return loadDataBlockFromTableSeq(pReader);
  // } else {  // loadType == RR and Offset Order
  //   if (pReader->checkFiles) {
  //     // check if the query range overlaps with the file data block
  //     bool exists = true;
  //     int32_t code = buildBlockFromFiles(pReader, &exists);
  //     if (code != TSDB_CODE_SUCCESS) {
  //       pReader->activeIndex = 0;
  //       pReader->checkFiles = false;

  //       return false;
  //     }

  //     if (exists) {
  //       pReader->cost.checkForNextTime += (taosGetTimestampUs() - stime);
  //       return exists;
  //     }

  //     pReader->activeIndex = 0;
  //     pReader->checkFiles = false;
  //   }

  //   // TODO: opt by consider the scan order
  //   bool ret = doHasDataInBuffer(pReader);
  //   terrno = TSDB_CODE_SUCCESS;

  //   elapsedTime = taosGetTimestampUs() - stime;
  //   pReader->cost.checkForNextTime += elapsedTime;
  //   return ret;
  // }
  return false;
}

void tsdbRetrieveDataBlockInfo(STsdbReader* pReader, SDataBlockInfo* pDataBlockInfo) {
  ASSERT(pDataBlockInfo != NULL && pReader != NULL);
  pDataBlockInfo->rows = pReader->pResBlock->info.rows;
  pDataBlockInfo->uid = pReader->pResBlock->info.uid;
  pDataBlockInfo->window = pReader->pResBlock->info.window;
}

int32_t tsdbRetrieveDataBlockStatisInfo(STsdbReader* pReader, SColumnDataAgg*** pBlockStatis, bool* allHave) {
  int32_t code = 0;
   *allHave = false;

   if (pReader->status.composedDataBlock) {
     *pBlockStatis = NULL;
     return TSDB_CODE_SUCCESS;
   }

  // SFileBlockInfo* pBlockInfo = &pReader->pDataBlockInfo[c->slot];
  // assert((c->slot >= 0 && c->slot < pReader->numOfBlocks) || ((c->slot == pReader->numOfBlocks) && (c->slot == 0)));

  // // file block with sub-blocks has no statistics data
  // if (pBlockInfo->compBlock->numOfSubBlocks > 1) {
  //   *pBlockStatis = NULL;
  //   return TSDB_CODE_SUCCESS;
  // }

  // int64_t stime = taosGetTimestampUs();
  // int     statisStatus = tsdbLoadBlockStatis(&pReader->rhelper, pBlockInfo->compBlock);
  // if (statisStatus < TSDB_STATIS_OK) {
  //   return terrno;
  // } else if (statisStatus > TSDB_STATIS_OK) {
  //   *pBlockStatis = NULL;
  //   return TSDB_CODE_SUCCESS;
  // }

  // tsdbDebug("vgId:%d, succeed to load block statis part for uid %" PRIu64, REPO_ID(pReader->pTsdb),
  //           TSDB_READ_TABLE_UID(&pReader->rhelper));

  // int16_t* colIds = pReader->suppInfo.defaultLoadColumn->pData;

  // size_t numOfCols = QH_GET_NUM_OF_COLS(pReader);
  // memset(pReader->suppInfo.plist, 0, numOfCols * POINTER_BYTES);
  // memset(pReader->suppInfo.pstatis, 0, numOfCols * sizeof(SColumnDataAgg));

  // for (int32_t i = 0; i < numOfCols; ++i) {
  //   pReader->suppInfo.pstatis[i].colId = colIds[i];
  // }

  // *allHave = true;
  // tsdbGetBlockStatis(&pReader->rhelper, pReader->suppInfo.pstatis, (int)numOfCols, pBlockInfo->compBlock);

  // // always load the first primary timestamp column data
  // SColumnDataAgg* pPrimaryColStatis = &pReader->suppInfo.pstatis[0];
  // assert(pPrimaryColStatis->colId == PRIMARYKEY_TIMESTAMP_COL_ID);

  // pPrimaryColStatis->numOfNull = 0;
  // pPrimaryColStatis->min = pBlockInfo->compBlock->minKey.ts;
  // pPrimaryColStatis->max = pBlockInfo->compBlock->maxKey.ts;
  // pReader->suppInfo.plist[0] = &pReader->suppInfo.pstatis[0];

  // // update the number of NULL data rows
  // int32_t* slotIds = pReader->suppInfo.slotIds;
  // for (int32_t i = 1; i < numOfCols; ++i) {
  //   ASSERT(colIds[i] == pReader->pSchema->columns[slotIds[i]].colId);
  //   if (IS_BSMA_ON(&(pReader->pSchema->columns[slotIds[i]]))) {
  //     if (pReader->suppInfo.pstatis[i].numOfNull == -1) {  // set the column data are all NULL
  //       pReader->suppInfo.pstatis[i].numOfNull = pBlockInfo->compBlock->numOfRows;
  //     }

  //     pReader->suppInfo.plist[i] = &pReader->suppInfo.pstatis[i];
  //   } else {
  //     *allHave = false;
  //   }
  // }

  // int64_t elapsed = taosGetTimestampUs() - stime;
  // pReader->cost.statisInfoLoadTime += elapsed;

  // *pBlockStatis = pReader->suppInfo.plist;
  return code;
}

SArray* tsdbRetrieveDataBlock(STsdbReader* pReader, SArray* pIdList) {
  SReaderStatus* pStatus = &pReader->status;

  if (pStatus->composedDataBlock) {
    return pReader->pResBlock->pDataBlock;
  } else {
    SFileDataBlockInfo* pFBlock = getCurrentBlockInfo(&pStatus->blockIter);
    STableBlockScanInfo* pBlockScanInfo = taosHashGet(pStatus->pTableMap, &pFBlock->uid, sizeof(pFBlock->uid));

    int32_t code = tBlockDataInit(&pStatus->fileBlockData);
    if (code != TSDB_CODE_SUCCESS) {
      terrno = code;
      return NULL;
    }

    code = doLoadFileBlockData(pReader, &pStatus->blockIter, pBlockScanInfo, &pStatus->fileBlockData);
    if (code != TSDB_CODE_SUCCESS) {
      terrno = code;
      return NULL;
    }

    return pReader->pResBlock->pDataBlock;
  }
}

void tsdbResetReadHandle(STsdbReader* pReader, SQueryTableDataCond* pCond, int32_t tWinIdx) {
  // if (isEmptyQueryTimeWindow(pReader)) {
  //   if (pCond->order != pReader->order) {
  //     pReader->order = pCond->order;
  //     TSWAP(pReader->window.skey, pReader->window.ekey);
  //   }

  //   return;
  // }

  // pReader->order = pCond->order;
  // setQueryTimewindow(pReader, pCond, tWinIdx);
  // pReader->type = TSDB_QUERY_TYPE_ALL;
  // pReader->cur.fid = -1;
  // pReader->cur.win = TSWINDOW_INITIALIZER;
  // pReader->checkFiles = true;
  // pReader->activeIndex = 0;  // current active table index
  // pReader->locateStart = false;
  // pReader->loadExternalRow = pCond->loadExternalRows;

  // if (ASCENDING_TRAVERSE(pCond->order)) {
  //   assert(pReader->window.skey <= pReader->window.ekey);
  // } else {
  //   assert(pReader->window.skey >= pReader->window.ekey);
  // }

  // // allocate buffer in order to load data blocks from file
  // memset(pReader->suppInfo.pstatis, 0, sizeof(SColumnDataAgg));
  // memset(pReader->suppInfo.plist, 0, POINTER_BYTES);

  // tsdbInitDataBlockLoadInfo(&pReader->dataBlockLoadInfo);
  // tsdbInitCompBlockLoadInfo(&pReader->compBlockLoadInfo);

  // resetCheckInfo(pReader);
}

int32_t tsdbGetFileBlocksDistInfo(STsdbReader* pReader, STableBlockDistInfo* pTableBlockInfo) {
  int32_t code = 0;
  // pTableBlockInfo->totalSize = 0;
  // pTableBlockInfo->totalRows = 0;

  // STsdbFS* pFileHandle = REPO_FS(pReader->pTsdb);

  // // find the start data block in file
  // pReader->locateStart = true;
  // STsdbKeepCfg* pCfg = REPO_KEEP_CFG(pReader->pTsdb);
  // int32_t       fid = getFileIdFromKey(pReader->window.skey, pCfg->days, pCfg->precision);

  // tsdbRLockFS(pFileHandle);
  // tsdbFSIterInit(&pReader->fileIter, pFileHandle, pReader->order);
  // tsdbFSIterSeek(&pReader->fileIter, fid);
  // tsdbUnLockFS(pFileHandle);

  // STsdbCfg* pc = REPO_CFG(pReader->pTsdb);
  // pTableBlockInfo->defMinRows = pc->minRows;
  // pTableBlockInfo->defMaxRows = pc->maxRows;

  // int32_t bucketRange = ceil((pc->maxRows - pc->minRows) / 20.0);

  // pTableBlockInfo->numOfFiles += 1;

  // int32_t     code = TSDB_CODE_SUCCESS;
  // int32_t     numOfBlocks = 0;
  // int32_t     numOfTables = (int32_t)taosArrayGetSize(pReader->pTableCheckInfo);
  // int         defaultRows = 4096;
  // STimeWindow win = TSWINDOW_INITIALIZER;

  // while (true) {
  //   numOfBlocks = 0;
  //   tsdbRLockFS(REPO_FS(pReader->pTsdb));

  //   if ((pReader->pFileGroup = tsdbFSIterNext(&pReader->fileIter)) == NULL) {
  //     tsdbUnLockFS(REPO_FS(pReader->pTsdb));
  //     break;
  //   }

  //   tsdbGetFidKeyRange(pCfg->days, pCfg->precision, pReader->pFileGroup->fid, &win.skey, &win.ekey);

  //   // current file are not overlapped with query time window, ignore remain files
  //   if ((win.skey > pReader->window.ekey) /* || (!ascTraverse && win.ekey < pTsdbReadHandle->window.ekey)*/) {
  //     tsdbUnLockFS(REPO_FS(pReader->pTsdb));
  //     tsdbDebug("%p remain files are not qualified for qrange:%" PRId64 "-%" PRId64 ", ignore, %s", pReader,
  //               pReader->window.skey, pReader->window.ekey, pReader->idStr);
  //     pReader->pFileGroup = NULL;
  //     break;
  //   }

  //   pTableBlockInfo->numOfFiles += 1;
  //   if (tsdbSetAndOpenReadFSet(&pReader->rhelper, pReader->pFileGroup) < 0) {
  //     tsdbUnLockFS(REPO_FS(pReader->pTsdb));
  //     code = terrno;
  //     break;
  //   }

  //   tsdbUnLockFS(REPO_FS(pReader->pTsdb));

  //   if (tsdbLoadBlockIdx(&pReader->rhelper) < 0) {
  //     code = terrno;
  //     break;
  //   }

  //   if ((code = getFileCompInfo(pReader, &numOfBlocks)) != TSDB_CODE_SUCCESS) {
  //     break;
  //   }

  //   tsdbDebug("%p %d blocks found in file for %d table(s), fid:%d, %s", pReader, numOfBlocks, numOfTables,
  //             pReader->pFileGroup->fid, pReader->idStr);

  //   if (numOfBlocks == 0) {
  //     continue;
  //   }

  //   pTableBlockInfo->numOfBlocks += numOfBlocks;

  //   for (int32_t i = 0; i < numOfTables; ++i) {
  //     STableBlockScanInfo* pCheckInfo = taosArrayGet(pReader->pTableCheckInfo, i);

  //     SBlock* pBlock = pCheckInfo->pCompInfo->blocks;

  //     for (int32_t j = 0; j < pCheckInfo->numOfBlocks; ++j) {
  //       pTableBlockInfo->totalSize += pBlock[j].len;

  //       int32_t numOfRows = pBlock[j].numOfRows;
  //       pTableBlockInfo->totalRows += numOfRows;

  //       if (numOfRows > pTableBlockInfo->maxRows) {
  //         pTableBlockInfo->maxRows = numOfRows;
  //       }

  //       if (numOfRows < pTableBlockInfo->minRows) {
  //         pTableBlockInfo->minRows = numOfRows;
  //       }

  //       if (numOfRows < defaultRows) {
  //         pTableBlockInfo->numOfSmallBlocks += 1;
  //       }

  //       int32_t bucketIndex = getBucketIndex(pTableBlockInfo->defMinRows, bucketRange, numOfRows);
  //       pTableBlockInfo->blockRowsHisto[bucketIndex]++;
  //     }
  //   }
  // }

  // pTableBlockInfo->numOfTables = numOfTables;
  return code;
}

int64_t tsdbGetNumOfRowsInMemTable(STsdbReader* pReader) {
  int64_t rows = 0;

  SReaderStatus* pStatus = &pReader->status;
  pStatus->pTableIter = taosHashIterate(pStatus->pTableMap, NULL);

  while (pStatus->pTableIter != NULL) {
    STableBlockScanInfo* pBlockScanInfo = pStatus->pTableIter;

    STbData* d = NULL;
    if (pReader->pTsdb->mem != NULL) {
      tsdbGetTbDataFromMemTable(pReader->pTsdb->mem, pReader->suid, pBlockScanInfo->uid, &d);
      if (d != NULL) {
        rows += tsdbGetNRowsInTbData(d);
      }
    }

    STbData* di = NULL;
    if (pReader->pTsdb->imem != NULL) {
      tsdbGetTbDataFromMemTable(pReader->pTsdb->imem, pReader->suid, pBlockScanInfo->uid, &di);
      if (di != NULL) {
        rows += tsdbGetNRowsInTbData(di);
      }
    }

    // current table is exhausted, let's try the next table
    pStatus->pTableIter = taosHashIterate(pStatus->pTableMap, pStatus->pTableIter);
  }

  return rows;
}
