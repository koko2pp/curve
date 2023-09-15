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
 * File Created: Monday, 8th October 2018 4:47:17 pm
 * Author: tongguangxun
 */

#ifndef SRC_CLIENT_REQUEST_CLOSURE_H_
#define SRC_CLIENT_REQUEST_CLOSURE_H_

// for Closure
#include <google/protobuf/stubs/callback.h>

#include "include/curve_compiler_specific.h"
#include "src/client/client_common.h"
#include "src/client/client_metric.h"
#include "src/client/inflight_controller.h"
#include "src/common/concurrent/concurrent.h"
#include "src/common/timeutility.h"

namespace curve {
namespace client {

class IOTracker;
class IOManager;
class RequestScheduler;
struct FileMetric;
struct RequestContext;

class CURVE_CACHELINE_ALIGNMENT RequestClosure
    : public ::google::protobuf::Closure {
 public:
    explicit RequestClosure(RequestContext* reqctx) : reqCtx_(reqctx) {}
    virtual ~RequestClosure() = default;

    void Run() override;

    /**
     * @brief Get the inflight token before sending rpc
     */
    void GetInflightRPCToken();

    /**
     * @brief Release the inflight token when rpc returned
     */
    void ReleaseInflightRPCToken();

    /**
     * @brief Get error code
     */
    virtual int GetErrorCode() {
        return errcode_;
    }

    /**
     * @brief Set error code, 0 means success
     */
    virtual void SetFailed(int errorCode) {
        errcode_ = errorCode;
    }

    /**
     * @brief to obtain which request the current closure belongs to
     */
    virtual RequestContext* GetReqCtx() {
        return reqCtx_;
    }

    /**
     * @brief: Obtain which iotracker the current request belongs to
     */
    virtual IOTracker* GetIOTracker() {
        return tracker_;
    }

    /**
     * @brief Set which iotracker currently belongs to
     */
    void SetIOTracker(IOTracker* ioTracker) {
        tracker_ = ioTracker;
    }

    /**
     * @brief Set the iomanager to which it belongs
     */
    void SetIOManager(IOManager* ioManager) {
        ioManager_ = ioManager;
    }

    /**
     * @brief Set the current closure retry count
     */
    void IncremRetriedTimes() {
        retryTimes_++;
    }

    uint64_t GetRetriedTimes() const {
        return retryTimes_;
    }

    /**
     * Set metric
     */
    void SetFileMetric(FileMetric* fm) {
        metric_ = fm;
    }

    /**
     * Get metric pointer
     */
    FileMetric* GetMetric() const {
        return metric_;
    }

    /**
     * Obtain the next RPC timeout, which implements an exponential backoff strategy
     */
    uint64_t GetNextTimeoutMS() const {
        return nextTimeoutMS_;
    }

    /**
     * Set the next retry timeout time
     */
    void SetNextTimeOutMS(uint64_t timeout) {
        nextTimeoutMS_ = timeout;
    }

    bool IsSlowRequest() const { return slowRequest_; }

    void MarkAsSlowRequest() { slowRequest_ = true; }

    uint64_t CreatedMS() const { return createdMS_; }

 private:
    bool slowRequest_ = false;

    // whether own inflight count
    bool ownInflight_ = false;

    // The error code of the current request
    int errcode_ = -1;

    // Tracker information for the current request
    IOTracker* tracker_ = nullptr;

    // Request information for closures
    RequestContext* reqCtx_ = nullptr;

    // metric Information
    FileMetric* metric_ = nullptr;

    // Number of retries
    uint64_t retryTimes_ = 0;

    // The iomanager to which the current closure belongs
    IOManager* ioManager_ = nullptr;

    // Next RPC timeout
    uint64_t nextTimeoutMS_ = 0;

    // create time of this closure(in millisecond)
    uint64_t createdMS_ = common::TimeUtility::GetTimeofDayMs();
};

}  // namespace client
}  // namespace curve

#endif  // SRC_CLIENT_REQUEST_CLOSURE_H_
