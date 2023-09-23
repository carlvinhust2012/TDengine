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

#ifndef _STREAM_INC_H_
#define _STREAM_INC_H_

#include "executor.h"
#include "query.h"
#include "tstream.h"
#include "streamBackendRocksdb.h"
#include "trpc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHECK_DOWNSTREAM_INTERVAL 100

// clang-format off
#define stFatal(...) do { if (stDebugFlag & DEBUG_FATAL) { taosPrintLog("STM FATAL ", DEBUG_FATAL, 255, __VA_ARGS__); }}     while(0)
#define stError(...) do { if (stDebugFlag & DEBUG_ERROR) { taosPrintLog("STM ERROR ", DEBUG_ERROR, 255, __VA_ARGS__); }}     while(0)
#define stWarn(...)  do { if (stDebugFlag & DEBUG_WARN)  { taosPrintLog("STM WARN ", DEBUG_WARN, 255, __VA_ARGS__); }}       while(0)
#define stInfo(...)  do { if (stDebugFlag & DEBUG_INFO)  { taosPrintLog("STM ", DEBUG_INFO, 255, __VA_ARGS__); }}            while(0)
#define stDebug(...) do { if (stDebugFlag & DEBUG_DEBUG) { taosPrintLog("STM ", DEBUG_DEBUG, tqDebugFlag, __VA_ARGS__); }} while(0)
#define stTrace(...) do { if (stDebugFlag & DEBUG_TRACE) { taosPrintLog("STM ", DEBUG_TRACE, tqDebugFlag, __VA_ARGS__); }} while(0)
// clang-format on

typedef struct SStreamGlobalEnv {
  int8_t inited;
  void*  timer;
} SStreamGlobalEnv;

typedef struct SStreamContinueExecInfo {
  SEpSet  epset;
  int32_t taskId;
  SRpcMsg msg;
} SStreamContinueExecInfo;

struct STokenBucket {
  int32_t capacity;     // total capacity
  int64_t fillTimestamp;// fill timestamp
  int32_t numOfToken;   // total available tokens
  int32_t rate;         // number of token per second
};

struct STaskTimer {
  void* hTaskLaunchTimer;
  void* dispatchTimer;
  void* checkTimer;
};

extern SStreamGlobalEnv streamEnv;
extern int32_t streamBackendId;
extern int32_t streamBackendCfWrapperId;

void        streamRetryDispatchData(SStreamTask* pTask, int64_t waitDuration);
int32_t     streamDispatchStreamBlock(SStreamTask* pTask);
void        destroyDispatchMsg(SStreamDispatchReq* pReq, int32_t numOfVgroups);
int32_t     getNumOfDispatchBranch(SStreamTask* pTask);

int32_t           streamProcessCheckpointBlock(SStreamTask* pTask, SStreamDataBlock* pBlock);
SStreamDataBlock* createStreamBlockFromDispatchMsg(const SStreamDispatchReq* pReq, int32_t blockType, int32_t srcVg);
SStreamDataBlock* createStreamBlockFromResults(SStreamQueueItem* pItem, SStreamTask* pTask, int64_t resultSize,
                                               SArray* pRes);
void              destroyStreamDataBlock(SStreamDataBlock* pBlock);

int32_t streamRetrieveReqToData(const SStreamRetrieveReq* pReq, SStreamDataBlock* pData);
int32_t streamBroadcastToChildren(SStreamTask* pTask, const SSDataBlock* pBlock);

int32_t tEncodeStreamRetrieveReq(SEncoder* pEncoder, const SStreamRetrieveReq* pReq);

int32_t streamSaveAllTaskStatus(SStreamMeta* pMeta, int64_t checkpointId);
int32_t streamTaskBuildCheckpoint(SStreamTask* pTask);
int32_t streamSendCheckMsg(SStreamTask* pTask, const SStreamTaskCheckReq* pReq, int32_t nodeId, SEpSet* pEpSet);

int32_t streamAddCheckpointReadyMsg(SStreamTask* pTask, int32_t srcTaskId, int32_t index, int64_t checkpointId);
int32_t streamTaskSendCheckpointReadyMsg(SStreamTask* pTask);
int32_t streamTaskSendCheckpointSourceRsp(SStreamTask* pTask);
int32_t streamTaskGetNumOfDownstream(const SStreamTask* pTask);

int32_t     streamTaskGetDataFromInputQ(SStreamTask* pTask, SStreamQueueItem** pInput, int32_t* numOfBlocks);
int32_t     streamQueueGetNumOfItemsInQueue(const SStreamQueue* pQueue);
int32_t     streamQueueItemGetSize(const SStreamQueueItem* pItem);
void        streamQueueItemIncSize(const SStreamQueueItem* pItem, int32_t size);
const char* streamQueueItemGetTypeStr(int32_t type);

SStreamQueueItem* streamMergeQueueItem(SStreamQueueItem* dst, SStreamQueueItem* pElem);

int32_t streamTaskBuildScanhistoryRspMsg(SStreamTask* pTask, SStreamScanHistoryFinishReq* pReq, void** pBuffer, int32_t* pLen);
int32_t streamAddEndScanHistoryMsg(SStreamTask* pTask, SRpcHandleInfo* pRpcInfo, SStreamScanHistoryFinishReq* pReq);
int32_t streamNotifyUpstreamContinue(SStreamTask* pTask);
int32_t streamTaskFillHistoryFinished(SStreamTask* pTask);
int32_t streamTransferStateToStreamTask(SStreamTask* pTask);

int32_t streamTaskInitTokenBucket(STokenBucket* pBucket, int32_t cap, int32_t rate);

SStreamQueue* streamQueueOpen(int64_t cap);
void          streamQueueClose(SStreamQueue* pQueue, int32_t taskId);
void          streamQueueProcessSuccess(SStreamQueue* queue);
void          streamQueueProcessFail(SStreamQueue* queue);
void*         streamQueueNextItem(SStreamQueue* pQueue);
void          streamFreeQitem(SStreamQueueItem* data);

STaskId extractStreamTaskKey(const SStreamTask* pTask);

#ifdef __cplusplus
}
#endif

#endif /* ifndef _STREAM_INC_H_ */
