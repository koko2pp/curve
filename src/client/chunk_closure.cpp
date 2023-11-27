/*
 *  Copyright (c) 2020 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Project: curve
 * Created Date: 18-9-28
 * Author: wudemiao
 */

#include "src/client/chunk_closure.h"

#include <algorithm>
#include <memory>
#include <string>

#include "src/client/client_common.h"
#include "src/client/copyset_client.h"
#include "src/client/io_tracker.h"
#include "src/client/metacache.h"
#include "src/client/request_closure.h"
#include "src/client/request_context.h"
#include "src/client/service_helper.h"

// TODO(tongguangxun): Optimize retry logic by separating the retry logic from
// the RPC return logic
namespace curve {
namespace client {

ClientClosure::BackoffParam ClientClosure::backoffParam_;
FailureRequestOption ClientClosure::failReqOpt_;

void ClientClosure::PreProcessBeforeRetry(int rpcstatus, int cntlstatus) {
    RequestClosure* reqDone = static_cast<RequestClosure*>(done_);

    // If the leader of the corresponding copysetId may change,
    // set the retry request timeout to the default value.
    // This is done to retry this request as soon as possible.
    // When migrating from the copyset leader to obtaining a new leader
    // through client GetLeader, there may be a delay of 1~2 seconds.
    // For a given request, GetLeader may still return the old Leader.
    // The RPC timeout may be set to 2s/4s, and it will be only after
    // the timeout that the leader information is retrieved again.
    // To promptly retry the request on the new Leader, set the RPC timeout
    // to the default value.
    if (cntlstatus == brpc::ERPCTIMEDOUT || cntlstatus == ETIMEDOUT) {
        uint64_t nextTimeout = 0;
        uint64_t retriedTimes = reqDone->GetRetriedTimes();
        bool leaderMayChange = metaCache_->IsLeaderMayChange(
            chunkIdInfo_.lpid_, chunkIdInfo_.cpid_);

        // When a certain IO retry exceeds a certain number of times, an
        // exponential backoff must be performed during the timeout period When
        // the underlying chunkserver is under high pressure, unstable may also
        // be triggered Due to copyset leader may change, the request timeout
        // time will be set to the default value And chunkserver cannot process
        // it within this time, resulting in IO hang In the case of real
        // downtime, the request will be processed after a certain number of
        // retries If you keep trying again, it's not a downtime situation, and
        // at this point, the timeout still needs to enter the exponential
        // backoff logic
        if (retriedTimes <
                failReqOpt_
                    .chunkserverMinRetryTimesForceTimeoutBackoff &&  // NOLINT
            leaderMayChange) {
            nextTimeout = failReqOpt_.chunkserverRPCTimeoutMS;
        } else {
            nextTimeout = TimeoutBackOff(retriedTimes);
        }

        reqDone->SetNextTimeOutMS(nextTimeout);
        LOG(WARNING) << "rpc timeout, next timeout = " << nextTimeout << ", "
                     << *reqCtx_
                     << ", retried times = " << reqDone->GetRetriedTimes()
                     << ", IO id = " << reqDone->GetIOTracker()->GetID()
                     << ", request id = " << reqCtx_->id_ << ", remote side = "
                     << butil::endpoint2str(cntl_->remote_side()).c_str();
        return;
    }

    if (rpcstatus == CHUNK_OP_STATUS::CHUNK_OP_STATUS_OVERLOAD) {
        uint64_t nextsleeptime = OverLoadBackOff(reqDone->GetRetriedTimes());
        LOG(WARNING) << "chunkserver overload, sleep(us) = " << nextsleeptime
                     << ", " << *reqCtx_
                     << ", retried times = " << reqDone->GetRetriedTimes()
                     << ", IO id = " << reqDone->GetIOTracker()->GetID()
                     << ", request id = " << reqCtx_->id_ << ", remote side = "
                     << butil::endpoint2str(cntl_->remote_side()).c_str();
        bthread_usleep(nextsleeptime);
        return;
    }

    uint64_t nextSleepUS = 0;

    if (!retryDirectly_) {
        nextSleepUS = failReqOpt_.chunkserverOPRetryIntervalUS;
        if (rpcstatus == CHUNK_OP_STATUS::CHUNK_OP_STATUS_REDIRECTED) {
            nextSleepUS /= 10;
        }
    }

    LOG(WARNING) << "Rpc failed "
                 << (retryDirectly_
                         ? "retry directly, "
                         : "sleep " + std::to_string(nextSleepUS) + " us, ")
                 << *reqCtx_ << ", cntl status = " << cntlstatus
                 << ", response status = "
                 << curve::chunkserver::CHUNK_OP_STATUS_Name(
                        static_cast<curve::chunkserver::CHUNK_OP_STATUS>(
                            rpcstatus))
                 << ", retried times = " << reqDone->GetRetriedTimes()
                 << ", IO id = " << reqDone->GetIOTracker()->GetID()
                 << ", request id = " << reqCtx_->id_ << ", remote side = "
                 << butil::endpoint2str(cntl_->remote_side()).c_str();

    if (nextSleepUS != 0) {
        bthread_usleep(nextSleepUS);
    }
}

uint64_t ClientClosure::OverLoadBackOff(uint64_t currentRetryTimes) {
    uint64_t curpowTime =
        std::min(currentRetryTimes, backoffParam_.maxOverloadPow);

    uint64_t nextsleeptime =
        failReqOpt_.chunkserverOPRetryIntervalUS * (1 << curpowTime);

    // -10% ~ 10% jitter
    uint64_t random_time = std::rand() % (nextsleeptime / 5 + 1);
    random_time -= nextsleeptime / 10;
    nextsleeptime += random_time;

    nextsleeptime =
        std::min(nextsleeptime,
                 failReqOpt_.chunkserverMaxRetrySleepIntervalUS);  // NOLINT
    nextsleeptime = std::max(
        nextsleeptime, failReqOpt_.chunkserverOPRetryIntervalUS);  // NOLINT

    return nextsleeptime;
}

uint64_t ClientClosure::TimeoutBackOff(uint64_t currentRetryTimes) {
    uint64_t curpowTime =
        std::min(currentRetryTimes, backoffParam_.maxTimeoutPow);

    uint64_t nextTimeout =
        failReqOpt_.chunkserverRPCTimeoutMS * (1 << curpowTime);

    nextTimeout = std::min(nextTimeout, failReqOpt_.chunkserverMaxRPCTimeoutMS);
    nextTimeout = std::max(nextTimeout, failReqOpt_.chunkserverRPCTimeoutMS);

    return nextTimeout;
}

// Unified entry point for request callback functions.
// The overall processing logic remains the same as before.
// Specific handling is performed based on different request types
// and response status codes.
// Subclasses need to implement SendRetryRequest for retrying requests.
void ClientClosure::Run() {
    std::unique_ptr<ClientClosure> selfGuard(this);
    std::unique_ptr<brpc::Controller> cntlGuard(cntl_);
    brpc::ClosureGuard doneGuard(done_);

    metaCache_ = client_->GetMetaCache();
    reqDone_ = static_cast<RequestClosure*>(done_);
    fileMetric_ = reqDone_->GetMetric();
    reqCtx_ = reqDone_->GetReqCtx();
    chunkIdInfo_ = reqCtx_->idinfo_;
    status_ = -1;
    cntlstatus_ = cntl_->ErrorCode();

    bool needRetry = false;

    if (cntl_->Failed()) {
        needRetry = true;
        OnRpcFailed();
    } else {
        // As long as RPC returns normally, clear the timeout counter
        metaCache_->GetUnstableHelper().ClearTimeout(chunkserverID_,
                                                     chunkserverEndPoint_);

        status_ = GetResponseStatus();

        switch (status_) {
            // 1. Request successful
            case CHUNK_OP_STATUS::CHUNK_OP_STATUS_SUCCESS:
                OnSuccess();
                break;

            // 2.1 is not a leader
            case CHUNK_OP_STATUS::CHUNK_OP_STATUS_REDIRECTED:
                MetricHelper::IncremRedirectRPCCount(fileMetric_,
                                                     reqCtx_->optype_);
                needRetry = true;
                OnRedirected();
                break;

            // 2.2 Copyset does not exist, most likely due to configuration
            // changes
            case CHUNK_OP_STATUS::CHUNK_OP_STATUS_COPYSET_NOTEXIST:
                needRetry = true;
                OnCopysetNotExist();
                break;

            // 2.3 Chunk not exist, return directly without retry
            case CHUNK_OP_STATUS::CHUNK_OP_STATUS_CHUNK_NOTEXIST:
                OnChunkNotExist();
                break;

            // 2.4 Illegal parameter, returned directly without retry
            case CHUNK_OP_STATUS::CHUNK_OP_STATUS_INVALID_REQUEST:
                OnInvalidRequest();
                break;

            // 2.5 Return to feedback
            case CHUNK_OP_STATUS::CHUNK_OP_STATUS_BACKWARD:
                if (reqCtx_->optype_ == OpType::WRITE) {
                    needRetry = true;
                    OnBackward();
                } else {
                    LOG(ERROR)
                        << OpTypeToString(reqCtx_->optype_)
                        << " return backward, " << *reqCtx_
                        << ", status=" << status_
                        << ", retried times = " << reqDone_->GetRetriedTimes()
                        << ", IO id = " << reqDone_->GetIOTracker()->GetID()
                        << ", request id = " << reqCtx_->id_
                        << ", remote side = "
                        << butil::endpoint2str(cntl_->remote_side()).c_str();
                }
                break;

            // 2.6 Return Chunk Exist, directly return without retrying
            case CHUNK_OP_STATUS::CHUNK_OP_STATUS_CHUNK_EXIST:
                OnChunkExist();
                break;

            case CHUNK_OP_STATUS::CHUNK_OP_STATUS_EPOCH_TOO_OLD:
                OnEpochTooOld();
                break;

            default:
                needRetry = true;
                LOG(WARNING)
                    << OpTypeToString(reqCtx_->optype_)
                    << " failed for UNKNOWN reason, " << *reqCtx_ << ", status="
                    << curve::chunkserver::CHUNK_OP_STATUS_Name(
                           static_cast<CHUNK_OP_STATUS>(status_))
                    << ", retried times = " << reqDone_->GetRetriedTimes()
                    << ", IO id = " << reqDone_->GetIOTracker()->GetID()
                    << ", request id = " << reqCtx_->id_ << ", remote side = "
                    << butil::endpoint2str(cntl_->remote_side()).c_str();
        }
    }

    if (needRetry) {
        doneGuard.release();
        OnRetry();
    }
}

void ClientClosure::OnRpcFailed() {
    client_->ResetSenderIfNotHealth(chunkserverID_);

    status_ = cntl_->ErrorCode();

    // If the connection fails, wait for a certain amount of time before trying
    // again
    if (cntlstatus_ == brpc::ERPCTIMEDOUT) {
        // If RPC times out, the corresponding number of chunkserver timeout
        // requests+1
        metaCache_->GetUnstableHelper().IncreTimeout(chunkserverID_);
        MetricHelper::IncremTimeOutRPCCount(fileMetric_, reqCtx_->optype_);
    }

    LOG_EVERY_SECOND(WARNING)
        << OpTypeToString(reqCtx_->optype_)
        << " failed, error code: " << cntl_->ErrorCode()
        << ", error: " << cntl_->ErrorText() << ", " << *reqCtx_
        << ", retried times = " << reqDone_->GetRetriedTimes()
        << ", IO id = " << reqDone_->GetIOTracker()->GetID()
        << ", request id = " << reqCtx_->id_ << ", remote side = "
        << butil::endpoint2str(cntl_->remote_side()).c_str();

    ProcessUnstableState();
}

void ClientClosure::ProcessUnstableState() {
    UnstableState state =
        metaCache_->GetUnstableHelper().GetCurrentUnstableState(
            chunkserverID_, chunkserverEndPoint_);

    switch (state) {
        case UnstableState::ServerUnstable: {
            std::string ip = butil::ip2str(chunkserverEndPoint_.ip).c_str();
            int ret = metaCache_->SetServerUnstable(ip);
            if (ret != 0) {
                LOG(WARNING)
                    << "Set server(" << ip << ") unstable failed, "
                    << "now set chunkserver(" << chunkserverID_ << ") unstable";
                metaCache_->SetChunkserverUnstable(chunkserverID_);
            }
            break;
        }
        case UnstableState::ChunkServerUnstable: {
            metaCache_->SetChunkserverUnstable(chunkserverID_);
            break;
        }
        case UnstableState::NoUnstable: {
            RefreshLeader();
            break;
        }
        default:
            break;
    }
}

void ClientClosure::OnSuccess() {
    reqDone_->SetFailed(0);

    auto duration = cntl_->latency_us();
    MetricHelper::LatencyRecord(fileMetric_, duration, reqCtx_->optype_);
    MetricHelper::IncremRPCQPSCount(fileMetric_, reqCtx_->rawlength_,
                                    reqCtx_->optype_);
}

void ClientClosure::OnChunkNotExist() {
    reqDone_->SetFailed(status_);

    LOG(WARNING) << OpTypeToString(reqCtx_->optype_) << " not exists, "
                 << *reqCtx_ << ", status=" << status_
                 << ", retried times = " << reqDone_->GetRetriedTimes()
                 << ", IO id = " << reqDone_->GetIOTracker()->GetID()
                 << ", request id = " << reqCtx_->id_ << ", remote side = "
                 << butil::endpoint2str(cntl_->remote_side()).c_str();

    auto duration = cntl_->latency_us();
    MetricHelper::LatencyRecord(fileMetric_, duration, reqCtx_->optype_);
    MetricHelper::IncremRPCQPSCount(fileMetric_, reqCtx_->rawlength_,
                                    reqCtx_->optype_);
}

void ClientClosure::OnChunkExist() {
    reqDone_->SetFailed(status_);

    LOG(WARNING) << OpTypeToString(reqCtx_->optype_) << " exists, " << *reqCtx_
                 << ", status=" << status_
                 << ", retried times = " << reqDone_->GetRetriedTimes()
                 << ", IO id = " << reqDone_->GetIOTracker()->GetID()
                 << ", request id = " << reqCtx_->id_ << ", remote side = "
                 << butil::endpoint2str(cntl_->remote_side()).c_str();
}

void ClientClosure::OnEpochTooOld() {
    reqDone_->SetFailed(status_);
    LOG(WARNING) << OpTypeToString(reqCtx_->optype_)
                 << " epoch too old, reqCtx: " << *reqCtx_
                 << ", status: " << status_
                 << ", retried times: " << reqDone_->GetRetriedTimes()
                 << ", IO id: " << reqDone_->GetIOTracker()->GetID()
                 << ", request id: " << reqCtx_->id_ << ", remote side: "
                 << butil::endpoint2str(cntl_->remote_side()).c_str();
}

void ClientClosure::OnRedirected() {
    LOG(WARNING) << OpTypeToString(reqCtx_->optype_) << " redirected, "
                 << *reqCtx_ << ", status = " << status_
                 << ", retried times = " << reqDone_->GetRetriedTimes()
                 << ", IO id = " << reqDone_->GetIOTracker()->GetID()
                 << ", request id = " << reqCtx_->id_ << ", redirect leader is "
                 << (response_->has_redirect() ? response_->redirect()
                                               : "empty")
                 << ", remote side = "
                 << butil::endpoint2str(cntl_->remote_side()).c_str();

    if (response_->has_redirect()) {
        int ret = UpdateLeaderWithRedirectInfo(response_->redirect());
        if (ret == 0) {
            return;
        }
    }

    RefreshLeader();
}

void ClientClosure::OnCopysetNotExist() {
    LOG(WARNING) << OpTypeToString(reqCtx_->optype_) << " copyset not exists, "
                 << *reqCtx_ << ", status = " << status_
                 << ", retried times = " << reqDone_->GetRetriedTimes()
                 << ", IO id = " << reqDone_->GetIOTracker()->GetID()
                 << ", request id = " << reqCtx_->id_ << ", remote side = "
                 << butil::endpoint2str(cntl_->remote_side()).c_str();

    RefreshLeader();
}

void ClientClosure::OnRetry() {
    MetricHelper::IncremFailRPCCount(fileMetric_, reqCtx_->optype_);

    if (reqDone_->GetRetriedTimes() >= failReqOpt_.chunkserverOPMaxRetry) {
        reqDone_->SetFailed(status_);
        LOG(ERROR) << OpTypeToString(reqCtx_->optype_)
                   << " retried times exceeds"
                   << ", IO id = " << reqDone_->GetIOTracker()->GetID()
                   << ", request id = " << reqCtx_->id_;
        done_->Run();
        return;
    }

    if (CURVE_UNLIKELY(!reqDone_->IsSlowRequest() &&
                       (TimeUtility::GetTimeofDayMs() - reqDone_->CreatedMS() >
                        failReqOpt_.chunkserverSlowRequestThresholdMS))) {
        reqDone_->MarkAsSlowRequest();
        MetricHelper::IncremSlowRequestNum(fileMetric_);
        LOG(ERROR) << "Slow request, " << *reqCtx_
                   << ", IO id = " << reqDone_->GetIOTracker()->GetID()
                   << ", request id = " << reqCtx_->id_
                   << ", request created at " << reqDone_->CreatedMS();
    }

    PreProcessBeforeRetry(status_, cntlstatus_);
    SendRetryRequest();
}

void ClientClosure::RefreshLeader() {
    ChunkServerID leaderId = 0;
    butil::EndPoint leaderAddr;

    if (-1 == metaCache_->GetLeader(chunkIdInfo_.lpid_, chunkIdInfo_.cpid_,
                                    &leaderId, &leaderAddr, true,
                                    fileMetric_)) {
        LOG(WARNING) << "Refresh leader failed, "
                     << "logicpool id = " << chunkIdInfo_.lpid_
                     << ", copyset id = " << chunkIdInfo_.cpid_
                     << ", current op return status = " << status_
                     << ", IO id = " << reqDone_->GetIOTracker()->GetID()
                     << ", request id = " << reqCtx_->id_;
    } else {
        // If refresh leader obtains new leader information,
        // retry without sleeping before.
        retryDirectly_ = (leaderId != chunkserverID_);
    }
}

void ClientClosure::OnBackward() {
    const auto latestSn = metaCache_->GetLatestFileSn();
    LOG(WARNING) << OpTypeToString(reqCtx_->optype_) << " return BACKWARD, "
                 << *reqCtx_ << ", status = " << status_
                 << ", retried times = " << reqDone_->GetRetriedTimes()
                 << ", IO id = " << reqDone_->GetIOTracker()->GetID()
                 << ", request id = " << reqCtx_->id_ << ", remote side = "
                 << butil::endpoint2str(cntl_->remote_side()).c_str();

    reqCtx_->seq_ = latestSn;
}

void ClientClosure::OnInvalidRequest() {
    reqDone_->SetFailed(status_);
    LOG(ERROR) << OpTypeToString(reqCtx_->optype_)
               << " failed for invalid format, " << *reqCtx_
               << ", status=" << status_
               << ", retried times = " << reqDone_->GetRetriedTimes()
               << ", IO id = " << reqDone_->GetIOTracker()->GetID()
               << ", request id = " << reqCtx_->id_ << ", remote side = "
               << butil::endpoint2str(cntl_->remote_side()).c_str();
    MetricHelper::IncremFailRPCCount(fileMetric_, reqCtx_->optype_);
}

void WriteChunkClosure::SendRetryRequest() {
    client_->WriteChunk(reqCtx_->idinfo_, reqCtx_->fileId_, reqCtx_->epoch_,
                        reqCtx_->seq_, reqCtx_->writeData_, reqCtx_->offset_,
                        reqCtx_->rawlength_, reqCtx_->sourceInfo_, done_);
}

void WriteChunkClosure::OnSuccess() { ClientClosure::OnSuccess(); }

void ReadChunkClosure::SendRetryRequest() {
    client_->ReadChunk(reqCtx_->idinfo_, reqCtx_->seq_, reqCtx_->offset_,
                       reqCtx_->rawlength_, reqCtx_->sourceInfo_, done_);
}

void ReadChunkClosure::OnSuccess() {
    ClientClosure::OnSuccess();

    reqCtx_->readData_ = cntl_->response_attachment();
}

void ReadChunkClosure::OnChunkNotExist() {
    ClientClosure::OnChunkNotExist();

    reqDone_->SetFailed(0);
    reqCtx_->readData_.resize(reqCtx_->rawlength_, 0);
}

void ReadChunkSnapClosure::SendRetryRequest() {
    client_->ReadChunkSnapshot(reqCtx_->idinfo_, reqCtx_->seq_,
                               reqCtx_->offset_, reqCtx_->rawlength_, done_);
}

void ReadChunkSnapClosure::OnSuccess() {
    ClientClosure::OnSuccess();

    reqCtx_->readData_ = cntl_->response_attachment();
}

void DeleteChunkSnapClosure::SendRetryRequest() {
    client_->DeleteChunkSnapshotOrCorrectSn(reqCtx_->idinfo_,
                                            reqCtx_->correctedSeq_, done_);
}

void GetChunkInfoClosure::SendRetryRequest() {
    client_->GetChunkInfo(reqCtx_->idinfo_, done_);
}

void GetChunkInfoClosure::OnSuccess() {
    ClientClosure::OnSuccess();

    for (int i = 0; i < chunkinforesponse_->chunksn_size(); ++i) {
        reqCtx_->chunkinfodetail_->chunkSn.push_back(
            chunkinforesponse_->chunksn(i));
    }
}

void GetChunkInfoClosure::OnRedirected() {
    LOG(WARNING) << OpTypeToString(reqCtx_->optype_) << " redirected, "
                 << *reqCtx_ << ", status = " << status_
                 << ", retried times = " << reqDone_->GetRetriedTimes()
                 << ", IO id = " << reqDone_->GetIOTracker()->GetID()
                 << ", request id = " << reqCtx_->id_ << ", redirect leader is "
                 << (chunkinforesponse_->has_redirect()
                         ? chunkinforesponse_->redirect()
                         : "empty")
                 << ", remote side = "
                 << butil::endpoint2str(cntl_->remote_side()).c_str();

    if (chunkinforesponse_->has_redirect()) {
        int ret = UpdateLeaderWithRedirectInfo(chunkinforesponse_->redirect());
        if (0 == ret) {
            return;
        }
    }

    RefreshLeader();
}

void CreateCloneChunkClosure::SendRetryRequest() {
    client_->CreateCloneChunk(reqCtx_->idinfo_, reqCtx_->location_,
                              reqCtx_->seq_, reqCtx_->correctedSeq_,
                              reqCtx_->chunksize_, done_);
}

void RecoverChunkClosure::SendRetryRequest() {
    client_->RecoverChunk(reqCtx_->idinfo_, reqCtx_->offset_,
                          reqCtx_->rawlength_, done_);
}

int ClientClosure::UpdateLeaderWithRedirectInfo(const std::string& leaderInfo) {
    ChunkServerID leaderId = 0;
    PeerAddr leaderAddr;

    int ret = leaderAddr.Parse(leaderInfo);
    if (ret != 0) {
        LOG(WARNING) << "Parse leader adress from " << leaderInfo << " fail";
        return -1;
    }

    LogicPoolID lpId = chunkIdInfo_.lpid_;
    CopysetID cpId = chunkIdInfo_.cpid_;
    ret = metaCache_->UpdateLeader(lpId, cpId, leaderAddr.addr_);
    if (ret != 0) {
        LOG(WARNING) << "Update leader of copyset (" << lpId << ", " << cpId
                     << ") in metaCache fail";
        return -1;
    }

    butil::EndPoint leaderEp;
    ret = metaCache_->GetLeader(lpId, cpId, &leaderId, &leaderEp);
    if (ret != 0) {
        LOG(INFO) << "Get leader of copyset (" << lpId << ", " << cpId
                  << ") from metaCache fail";
        return -1;
    }

    retryDirectly_ = (leaderId != chunkserverID_);
    return 0;
}

}  // namespace client
}  // namespace curve
