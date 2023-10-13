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
 * Project: nebd
 * Created Date: 2020-02-05
 * Author: lixiaocui
 */

#include <gtest/gtest.h>

#include "nebd/proto/client.pb.h"
#include "nebd/proto/heartbeat.pb.h"
#include "nebd/src/part2/file_service.h"
#include "nebd/src/part2/request_executor_curve.h"
#include "nebd/test/part2/mock_curve_client.h"

namespace nebd {
namespace server {

using ::testing::_;
using ::testing::DoAll;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::SetArgPointee;

class TestReuqestExecutorCurveClosure : public google::protobuf::Closure {
 public:
    TestReuqestExecutorCurveClosure() : runned_(false) {}
    ~TestReuqestExecutorCurveClosure() {}
    void Run() { runned_ = true; }
    bool IsRunned() { return runned_; }
    void Reset() { runned_ = false; }

 private:
    bool runned_;
};

void NebdUnitTestCallback(NebdServerAioContext* context) {
    std::cout << "callback" << std::endl;
}

class TestReuqestExecutorCurve : public ::testing::Test {
 protected:
    void SetUp() {
        curveClient_ = std::make_shared<MockCurveClient>();
        CurveRequestExecutor::GetInstance().Init(curveClient_);
    }

 protected:
    std::shared_ptr<MockCurveClient> curveClient_;
};

TEST_F(TestReuqestExecutorCurve, test_Open) {
    auto executor = CurveRequestExecutor::GetInstance();

    std::string fileName("cbd:pool1//cinder/volume-1234_cinder_:/client.conf");
    std::string curveFileName("/cinder/volume-1234_cinder_");

    // 1. Failed to parse the passed in fileName
    {
        std::string errFileName("cbd:pool1/:");
        EXPECT_CALL(*curveClient_, Open(fileName, _)).Times(0);
        std::shared_ptr<NebdFileInstance> ret =
            executor.Open(errFileName, nullptr);
        ASSERT_TRUE(nullptr == ret);
    }

    // 2. Curveclient open failed
    {
        EXPECT_CALL(*curveClient_, Open(curveFileName, _)).WillOnce(Return(-1));
        std::shared_ptr<NebdFileInstance> ret =
            executor.Open(fileName, nullptr);
        ASSERT_TRUE(nullptr == ret);
    }

    // 3. Open successful
    {
        EXPECT_CALL(*curveClient_, Open(curveFileName, _)).WillOnce(Return(1));
        std::shared_ptr<NebdFileInstance> ret =
            executor.Open(fileName, nullptr);
        ASSERT_TRUE(nullptr != ret);
        auto* curveIns = dynamic_cast<CurveFileInstance*>(ret.get());
        ASSERT_TRUE(nullptr != curveIns);
        ASSERT_EQ(curveFileName, curveIns->fileName);
        ASSERT_EQ(1, curveIns->fd);
        ASSERT_EQ("", curveIns->xattr["session"]);
    }
}

TEST_F(TestReuqestExecutorCurve, test_ReOpen) {
    auto executor = CurveRequestExecutor::GetInstance();
    ExtendAttribute xattr;
    xattr["session"] = "abc";
    std::string fileName("cbd:pool1//cinder/volume-1234_cinder_:/client.conf");
    std::string curveFileName("/cinder/volume-1234_cinder_");

    // 1. Failed to parse the passed in fileName
    {
        std::string errFileName("cbd:pool1/:");
        EXPECT_CALL(*curveClient_, Open(_, _)).Times(0);
        std::shared_ptr<NebdFileInstance> ret =
            executor.Reopen(errFileName, xattr);
        ASSERT_TRUE(nullptr == ret);
    }

    // 2. repoen failed
    {
        EXPECT_CALL(*curveClient_, ReOpen(curveFileName, _))
            .WillOnce(Return(-1));
        std::shared_ptr<NebdFileInstance> ret =
            executor.Reopen(fileName, xattr);
        ASSERT_TRUE(nullptr == ret);
    }

    // 3. reopen successful
    {
        EXPECT_CALL(*curveClient_, ReOpen(curveFileName, _))
            .WillOnce(Return(1));
        std::shared_ptr<NebdFileInstance> ret =
            executor.Reopen(fileName, xattr);
        ASSERT_TRUE(nullptr != ret);
        auto* curveIns = dynamic_cast<CurveFileInstance*>(ret.get());
        ASSERT_TRUE(nullptr != curveIns);
        ASSERT_EQ(curveFileName, curveIns->fileName);
        ASSERT_EQ(1, curveIns->fd);
        ASSERT_EQ("", curveIns->xattr["session"]);
    }
}

TEST_F(TestReuqestExecutorCurve, test_Close) {
    auto executor = CurveRequestExecutor::GetInstance();

    // 1. nebdFileIns is not of type CurveFileInstance, close failed
    {
        auto nebdFileIns = new NebdFileInstance();
        EXPECT_CALL(*curveClient_, Close(_)).Times(0);
        ASSERT_EQ(-1, executor.Close(nebdFileIns));
    }

    // 2. fd<0 in nebdFileIns, close failed
    {
        auto curveFileIns = new CurveFileInstance();
        curveFileIns->fd = -1;
        EXPECT_CALL(*curveClient_, Close(_)).Times(0);
        ASSERT_EQ(-1, executor.Close(curveFileIns));
    }

    // 3. Calling the close interface of curveclient failed, close failed
    {
        auto curveFileIns = new CurveFileInstance();
        curveFileIns->fd = 1;
        EXPECT_CALL(*curveClient_, Close(1))
            .WillOnce(Return(LIBCURVE_ERROR::FAILED));
        ASSERT_EQ(-1, executor.Close(curveFileIns));
    }

    // 4. close successful
    {
        auto curveFileIns = new CurveFileInstance();
        curveFileIns->fd = 1;
        EXPECT_CALL(*curveClient_, Close(1))
            .WillOnce(Return(LIBCURVE_ERROR::OK));
        ASSERT_EQ(0, executor.Close(curveFileIns));
    }
}

TEST_F(TestReuqestExecutorCurve, test_Extend) {
    auto executor = CurveRequestExecutor::GetInstance();
    std::string curveFilename("/cinder/volume-1234_cinder_");

    // 1. nebdFileIns is not of type CurveFileInstance, extend failed
    {
        auto nebdFileIns = new NebdFileInstance();
        EXPECT_CALL(*curveClient_, Extend(_, _)).Times(0);
        ASSERT_EQ(-1, executor.Extend(nebdFileIns, 1));
    }

    // 2. FileName in nebdFileIns is empty, extend failed
    {
        auto curveFileIns = new CurveFileInstance();
        EXPECT_CALL(*curveClient_, Extend(_, _)).Times(0);
        ASSERT_EQ(-1, executor.Extend(curveFileIns, 1));
    }

    // 3. Calling the extend interface of curveclient failed, extend failed
    {
        auto curveFileIns = new CurveFileInstance();
        curveFileIns->fileName = curveFilename;
        EXPECT_CALL(*curveClient_, Extend(curveFilename, 1))
            .WillOnce(Return(LIBCURVE_ERROR::FAILED));
        ASSERT_EQ(-1, executor.Extend(curveFileIns, 1));
    }

    // 4. extend successful
    {
        auto curveFileIns = new CurveFileInstance();
        curveFileIns->fileName = curveFilename;
        EXPECT_CALL(*curveClient_, Extend(curveFilename, 1))
            .WillOnce(Return(LIBCURVE_ERROR::OK));
        ASSERT_EQ(0, executor.Extend(curveFileIns, 1));
    }
}

TEST_F(TestReuqestExecutorCurve, test_GetInfo) {
    auto executor = CurveRequestExecutor::GetInstance();
    NebdFileInfo fileInfo;
    int curveFd = 123;

    // 1. nebdFileIns is not of type CurveFileInstance, stat failed
    {
        auto nebdFileIns = new NebdFileInstance();
        EXPECT_CALL(*curveClient_, StatFile(curveFd, _)).Times(0);
        ASSERT_EQ(-1, executor.GetInfo(nebdFileIns, &fileInfo));
    }

    // 2. Fd in nebdFileIns is empty, stat failed
    {
        auto curveFileIns = new CurveFileInstance();
        EXPECT_CALL(*curveClient_, StatFile(curveFd, _)).Times(0);
        ASSERT_EQ(-1, executor.GetInfo(curveFileIns, &fileInfo));
    }

    // 3. Calling the stat interface of curveclient failed, stat failed
    {
        auto curveFileIns = new CurveFileInstance();
        curveFileIns->fd = curveFd;
        EXPECT_CALL(*curveClient_, StatFile(curveFd, _)).WillOnce(Return(-1));
        ASSERT_EQ(-1, executor.GetInfo(curveFileIns, &fileInfo));
    }

    // 4. stat successful
    {
        const uint64_t size = 10ull * 1024 * 1024 * 1024;
        const uint32_t blocksize = 4096;
        auto curveFileIns = new CurveFileInstance();
        curveFileIns->fd = curveFd;
        EXPECT_CALL(*curveClient_, StatFile(curveFd, _))
            .WillOnce(Invoke([size, blocksize](int /*fd*/, FileStatInfo* info) {
                info->length = size;
                info->blocksize = blocksize;
                return 0;
            }));
        ASSERT_EQ(0, executor.GetInfo(curveFileIns, &fileInfo));
        ASSERT_EQ(size, fileInfo.size);
        ASSERT_EQ(blocksize, fileInfo.block_size);
    }
}

TEST_F(TestReuqestExecutorCurve, test_AioRead) {
    auto executor = CurveRequestExecutor::GetInstance();
    NebdServerAioContext aiotcx;
    aiotcx.cb = NebdUnitTestCallback;
    std::string curveFilename("/cinder/volume-1234_cinder_");

    // 1. nebdFileIns is not of type CurveFileInstance, asynchronous read failed
    {
        auto nebdFileIns = new NebdFileInstance();
        EXPECT_CALL(*curveClient_, AioRead(_, _, _)).Times(0);
        ASSERT_EQ(-1, executor.AioRead(nebdFileIns, &aiotcx));
    }

    // 2. fd<0 in nebdFileIns, asynchronous read failed
    {
        auto curveFileIns = new CurveFileInstance();
        curveFileIns->fd = -1;
        EXPECT_CALL(*curveClient_, AioRead(_, _, _)).Times(0);
        ASSERT_EQ(-1, executor.AioRead(curveFileIns, &aiotcx));
    }

    // 3. Calling the AioRead interface of curveclient failed, asynchronous read
    // failed
    {
        auto curveFileIns = new CurveFileInstance();
        aiotcx.size = 1;
        aiotcx.offset = 0;
        aiotcx.buf = new char[10];
        aiotcx.op = LIBAIO_OP::LIBAIO_OP_READ;
        curveFileIns->fd = 1;
        curveFileIns->fileName = curveFilename;
        EXPECT_CALL(*curveClient_, AioRead(1, _, _))
            .WillOnce(Return(LIBCURVE_ERROR::FAILED));
        ASSERT_EQ(-1, executor.AioRead(curveFileIns, &aiotcx));
    }

    // 4. Asynchronous read successful
    {
        auto curveFileIns = new CurveFileInstance();
        curveFileIns->fd = 1;
        curveFileIns->fileName = curveFilename;
        CurveAioContext* curveCtx;
        EXPECT_CALL(*curveClient_, AioRead(1, _, _))
            .WillOnce(DoAll(SaveArg<1>(&curveCtx), Return(LIBCURVE_ERROR::OK)));
        ASSERT_EQ(0, executor.AioRead(curveFileIns, &aiotcx));
        curveCtx->cb(curveCtx);
    }
}

TEST_F(TestReuqestExecutorCurve, test_AioWrite) {
    auto executor = CurveRequestExecutor::GetInstance();
    NebdServerAioContext aiotcx;
    aiotcx.cb = NebdUnitTestCallback;
    std::string curveFilename("/cinder/volume-1234_cinder_");

    // 1. nebdFileIns is not of type CurveFileInstance, asynchronous write
    // failed
    {
        auto nebdFileIns = new NebdFileInstance();
        EXPECT_CALL(*curveClient_, AioWrite(_, _, _)).Times(0);
        ASSERT_EQ(-1, executor.AioWrite(nebdFileIns, &aiotcx));
    }

    // 2. fd<0 in nebdFileIns, asynchronous write failed
    {
        auto curveFileIns = new CurveFileInstance();
        curveFileIns->fd = -1;
        EXPECT_CALL(*curveClient_, AioWrite(_, _, _)).Times(0);
        ASSERT_EQ(-1, executor.AioWrite(curveFileIns, &aiotcx));
    }

    // 3. Calling the AioWrite interface of curveclient failed, asynchronous
    // write failed
    {
        auto curveFileIns = new CurveFileInstance();
        aiotcx.size = 1;
        aiotcx.offset = 0;
        aiotcx.buf = new char[10];
        aiotcx.op = LIBAIO_OP::LIBAIO_OP_READ;
        curveFileIns->fd = 1;
        curveFileIns->fileName = curveFilename;
        EXPECT_CALL(*curveClient_, AioWrite(1, _, _))
            .WillOnce(Return(LIBCURVE_ERROR::FAILED));
        ASSERT_EQ(-1, executor.AioWrite(curveFileIns, &aiotcx));
    }

    // 4. Asynchronous write successful
    {
        auto curveFileIns = new CurveFileInstance();
        curveFileIns->fd = 1;
        curveFileIns->fileName = curveFilename;
        CurveAioContext* curveCtx;
        EXPECT_CALL(*curveClient_, AioWrite(1, _, _))
            .WillOnce(DoAll(SaveArg<1>(&curveCtx), Return(LIBCURVE_ERROR::OK)));
        ASSERT_EQ(0, executor.AioWrite(curveFileIns, &aiotcx));
        curveCtx->cb(curveCtx);
    }
}

TEST_F(TestReuqestExecutorCurve, test_Discard) {
    auto executor = CurveRequestExecutor::GetInstance();
    NebdServerAioContext aioctx;
    aioctx.cb = NebdUnitTestCallback;
    std::string curveFilename("/cinder/volume-1234_cinder_");

    // 1. not an curve volume
    {
        std::unique_ptr<NebdFileInstance> nebdFileIns(new NebdFileInstance());
        EXPECT_CALL(*curveClient_, AioDiscard(_, _)).Times(0);
        ASSERT_EQ(-1, executor.Discard(nebdFileIns.get(), &aioctx));
    }

    // 2. fd is invalid
    {
        std::unique_ptr<CurveFileInstance> curveFileIns(
            new CurveFileInstance());
        curveFileIns->fd = -1;
        EXPECT_CALL(*curveClient_, AioDiscard(_, _)).Times(0);
        ASSERT_EQ(-1, executor.Discard(curveFileIns.get(), &aioctx));
    }

    // 3. curve client return failed
    {
        std::unique_ptr<CurveFileInstance> curveFileIns(
            new CurveFileInstance());
        aioctx.size = 1;
        aioctx.offset = 0;
        aioctx.op = LIBAIO_OP::LIBAIO_OP_DISCARD;
        curveFileIns->fd = 1;
        curveFileIns->fileName = curveFilename;
        EXPECT_CALL(*curveClient_, AioDiscard(_, _))
            .WillOnce(Return(LIBCURVE_ERROR::FAILED));
        ASSERT_EQ(-1, executor.Discard(curveFileIns.get(), &aioctx));
    }

    // 4. ok
    {
        std::unique_ptr<CurveFileInstance> curveFileIns(
            new CurveFileInstance());
        aioctx.size = 1;
        aioctx.offset = 0;
        aioctx.op = LIBAIO_OP::LIBAIO_OP_DISCARD;
        curveFileIns->fd = 1;
        curveFileIns->fileName = curveFilename;
        CurveAioContext* curveCtx;
        EXPECT_CALL(*curveClient_, AioDiscard(_, _))
            .WillOnce(DoAll(SaveArg<1>(&curveCtx), Return(LIBCURVE_ERROR::OK)));
        ASSERT_EQ(0, executor.Discard(curveFileIns.get(), &aioctx));
        curveCtx->cb(curveCtx);
    }
}

TEST_F(TestReuqestExecutorCurve, test_Flush) {
    auto executor = CurveRequestExecutor::GetInstance();
    std::string curveFilename("/cinder/volume-1234_cinder_");
    std::unique_ptr<CurveFileInstance> curveFileIns(new CurveFileInstance());
    NebdServerAioContext* aioctx = new NebdServerAioContext();
    nebd::client::FlushResponse response;
    TestReuqestExecutorCurveClosure done;

    aioctx->op = LIBAIO_OP::LIBAIO_OP_FLUSH;
    aioctx->cb = NebdFileServiceCallback;
    aioctx->response = &response;
    aioctx->done = &done;

    ASSERT_EQ(0, executor.Flush(curveFileIns.get(), aioctx));
    ASSERT_TRUE(done.IsRunned());
    ASSERT_EQ(response.retcode(), nebd::client::RetCode::kOK);
}

TEST_F(TestReuqestExecutorCurve, test_InvalidCache) {
    auto executor = CurveRequestExecutor::GetInstance();
    std::string curveFilename("/cinder/volume-1234_cinder_");

    // 1. nebdFileIns is not of type CurveFileInstance, illegal
    {
        auto nebdFileIns = new NebdFileInstance();
        ASSERT_EQ(-1, executor.InvalidCache(nebdFileIns));
    }

    // 2. fd<0, illegal
    {
        auto curveFileIns = new CurveFileInstance();
        curveFileIns->fileName = curveFilename;
        curveFileIns->fd = -1;
        ASSERT_EQ(-1, executor.InvalidCache(curveFileIns));
    }

    // 3. The filename is empty and illegal
    {
        auto curveFileIns = new CurveFileInstance();
        curveFileIns->fd = 1;
        ASSERT_EQ(-1, executor.InvalidCache(curveFileIns));
    }

    // 4. legitimate
    {
        auto curveFileIns = new CurveFileInstance();
        curveFileIns->fd = 1;
        curveFileIns->fileName = curveFilename;
        ASSERT_EQ(0, executor.InvalidCache(curveFileIns));
    }
}

TEST(TestFileNameParser, test_Parse) {
    std::string fileName("cbd:pool1//cinder/volume-1234_cinder_:/client.conf");
    std::pair<std::string, std::string> res("/cinder/volume-1234_cinder_",
                                            "/client.conf");
    ASSERT_EQ(res, FileNameParser::Parse(fileName));

    fileName = "cbd:pool1//cinder/volume-1234_cinder_";
    res = std::make_pair("/cinder/volume-1234_cinder_", "");
    ASSERT_EQ(res, FileNameParser::Parse(fileName));

    fileName = "cbd:pool1";
    res = std::make_pair("", "");
    ASSERT_EQ(res, FileNameParser::Parse(fileName));

    fileName = "cbd:pool1//:";
    ASSERT_EQ(res, FileNameParser::Parse(fileName));

    fileName = "cbd:pool1//";
    ASSERT_EQ(res, FileNameParser::Parse(fileName));
}

}  // namespace server
}  // namespace nebd

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::InitGoogleMock(&argc, argv);
    return RUN_ALL_TESTS();
}
