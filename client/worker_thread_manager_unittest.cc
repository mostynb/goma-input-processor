// Copyright 2012 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "worker_thread_manager.h"

#ifndef _WIN32
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#else
#include "socket_helper_win.h"
#endif

#include <memory>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "absl/memory/memory.h"
#include "absl/time/time.h"
#include "callback.h"
#include "compiler_specific.h"
#include "lockhelper.h"
#include "mock_socket_factory.h"
#include "platform_thread.h"
#include "scoped_fd.h"
#include "simple_timer.h"
#include "socket_descriptor.h"
#include "worker_thread.h"

namespace devtools_goma {

class WorkerThreadManagerTest : public ::testing::Test {
 public:
  WorkerThreadManagerTest()
      : test_threadid_(0),
        num_test_threadid_(0),
        periodic_counter_(0) {
  }
  ~WorkerThreadManagerTest() override {
  }

 protected:
  class TestReadContext {
   public:
    TestReadContext(int fd, absl::Duration timeout)
        : fd_(fd),
          timeout_(timeout),
          num_read_(-1),
          socket_descriptor_(nullptr),
          timeout_called_(false) {}
    ~TestReadContext() {
    }
    const int fd_;
    const absl::Duration timeout_;
    int num_read_;
    SocketDescriptor* socket_descriptor_;
    bool timeout_called_;

   private:
    DISALLOW_COPY_AND_ASSIGN(TestReadContext);
  };

  class TestWriteContext {
   public:
    TestWriteContext(int fd, int total_write)
        : fd_(fd),
          total_write_(total_write),
          num_write_(-1),
          socket_descriptor_(nullptr) {}
    ~TestWriteContext() {
    }
    const int fd_;
    const int total_write_;
    int num_write_;
    SocketDescriptor* socket_descriptor_;

   private:
    DISALLOW_COPY_AND_ASSIGN(TestWriteContext);
  };

  void SetUp() override {
    wm_ = absl::make_unique<WorkerThreadManager>();
    test_threadid_ = 0;
    num_test_threadid_ = 0;
    periodic_counter_ = 0;
  }
  void TearDown() override {
    wm_.reset(nullptr);
  }

  void Reset() {
    AutoLock lock(&mu_);
    test_threadid_ = 0;
    num_test_threadid_ = 0;
  }

  OneshotClosure* NewTestRun() {
    {
      AutoLock lock(&mu_);
      EXPECT_TRUE(!test_threadid_);
    }
    return NewCallback(
        this, &WorkerThreadManagerTest::TestRun);
  }

  void TestRun() {
    AutoLock lock(&mu_);
    test_threadid_ = wm_->GetCurrentThreadId();
    cond_.Signal();
  }

  void WaitTestRun() {
    AutoLock lock(&mu_);
    while (test_threadid_ == 0) {
      cond_.Wait(&mu_);
    }
  }

  OneshotClosure* NewTestDispatch() {
    {
      AutoLock lock(&mu_);
      EXPECT_TRUE(!test_threadid_);
    }
    return NewCallback(
        this, &WorkerThreadManagerTest::TestDispatch);
  }

  void TestDispatch() {
    while (wm_->Dispatch()) {
      AutoLock lock(&mu_);
      if (test_threadid_ == 0)
        continue;
      EXPECT_EQ(test_threadid_, wm_->GetCurrentThreadId());
      cond_.Signal();
      return;
    }
    LOG(FATAL) << "Dispatch unexpectedly finished";
  }

  OneshotClosure* NewTestThreadId(
      WorkerThread::ThreadId id) {
    return NewCallback(
        this, &WorkerThreadManagerTest::TestThreadId, id);
  }

  void TestThreadId(WorkerThread::ThreadId id) {
    EXPECT_EQ(id, wm_->GetCurrentThreadId());
    AutoLock lock(&mu_);
    ++num_test_threadid_;
    cond_.Signal();
  }

  void WaitTestThreadHandle(int num) {
    AutoLock lock(&mu_);
    while (num_test_threadid_ < num) {
      cond_.Wait(&mu_);
    }
  }

  std::unique_ptr<PermanentClosure> NewPeriodicRun() {
    {
      AutoLock lock(&mu_);
      periodic_counter_ = 0;
    }
    return NewPermanentCallback(
        this, &WorkerThreadManagerTest::TestPeriodicRun);
  }

  void TestPeriodicRun() {
    AutoLock lock(&mu_);
    ++periodic_counter_;
    cond_.Signal();
  }

  void WaitTestPeriodicRun(int n) {
    AutoLock lock(&mu_);
    while (periodic_counter_ < n) {
      cond_.Wait(&mu_);
    }
  }

  OneshotClosure* NewTestDescriptorRead(TestReadContext* tc) {
    {
      AutoLock lock(&mu_);
      EXPECT_GT(tc->fd_, 0);
      EXPECT_LT(tc->num_read_, 0);
      EXPECT_TRUE(tc->socket_descriptor_ == nullptr);
    }
    return NewCallback(
        this, &WorkerThreadManagerTest::TestDescriptorRead, tc);
  }

  void TestDescriptorRead(TestReadContext* tc) {
    ScopedSocket sock;
    absl::Duration timeout;
    {
      AutoLock lock(&mu_);
      EXPECT_LT(tc->num_read_, 0);
      EXPECT_TRUE(tc->socket_descriptor_ == nullptr);
      timeout = tc->timeout_;
      EXPECT_FALSE(tc->timeout_called_);
      sock.reset(tc->fd_);
    }
    SocketDescriptor* descriptor = wm_->RegisterSocketDescriptor(
        std::move(sock), WorkerThread::PRIORITY_HIGH);
    descriptor->NotifyWhenReadable(
        NewPermanentCallback(this, &WorkerThreadManagerTest::DoRead, tc));
    if (timeout > absl::ZeroDuration()) {
      descriptor->NotifyWhenTimedout(
          timeout, NewCallback(this, &WorkerThreadManagerTest::DoTimeout, tc));
    }
    AutoLock lock(&mu_);
    tc->num_read_ = 0;
    tc->socket_descriptor_ = descriptor;
    cond_.Signal();
  }

  void DoRead(TestReadContext* tc) {
    SocketDescriptor* descriptor = nullptr;
    {
      AutoLock lock(&mu_);
      EXPECT_GE(tc->num_read_, 0);
      EXPECT_EQ(tc->fd_, tc->socket_descriptor_->fd());
      EXPECT_EQ(WorkerThread::PRIORITY_HIGH,
                tc->socket_descriptor_->priority());
      descriptor = tc->socket_descriptor_;
    }
    char buf[1] = { 42 };
    int n = descriptor->Read(buf, 1);
    if (n > 0) {
      EXPECT_EQ(1, n);
    } else {
      descriptor->StopRead();
      wm_->RunClosureInThread(
          FROM_HERE,
          wm_->GetCurrentThreadId(),
          NewCallback(
              this, &WorkerThreadManagerTest::DoStopRead, tc),
          WorkerThread::PRIORITY_IMMEDIATE);
    }
    AutoLock lock(&mu_);
    ++tc->num_read_;
    cond_.Signal();
  }

  void WaitTestRead(TestReadContext* tc, int n) {
    AutoLock lock(&mu_);
    while (tc->num_read_ != n) {
      cond_.Wait(&mu_);
    }
  }

  void DoTimeout(TestReadContext* tc) {
    SocketDescriptor* descriptor = nullptr;
    {
      AutoLock lock(&mu_);
      EXPECT_EQ(tc->fd_, tc->socket_descriptor_->fd());
      EXPECT_EQ(WorkerThread::PRIORITY_HIGH,
                tc->socket_descriptor_->priority());
      EXPECT_GT(tc->timeout_, absl::ZeroDuration());
      EXPECT_FALSE(tc->timeout_called_);
      descriptor = tc->socket_descriptor_;
    }
    descriptor->StopRead();
    wm_->RunClosureInThread(
        FROM_HERE,
        wm_->GetCurrentThreadId(),
        NewCallback(
            this, &WorkerThreadManagerTest::DoStopRead, tc),
        WorkerThread::PRIORITY_IMMEDIATE);
    AutoLock lock(&mu_);
    tc->timeout_called_ = true;
    cond_.Signal();
  }

  void DoStopRead(TestReadContext* tc) {
    int fd;
    SocketDescriptor* descriptor = nullptr;
    {
      AutoLock lock(&mu_);
      EXPECT_EQ(tc->fd_, tc->socket_descriptor_->fd());
      EXPECT_EQ(WorkerThread::PRIORITY_HIGH,
                tc->socket_descriptor_->priority());
      fd = tc->fd_;
      descriptor = tc->socket_descriptor_;
    }
    descriptor->ClearWritable();  // Check b/148360680 case.
    descriptor->ClearReadable();
    descriptor->ClearTimeout();
    ScopedSocket sock(wm_->DeleteSocketDescriptor(descriptor));
    EXPECT_EQ(fd, sock.get());
    sock.Close();
    AutoLock lock(&mu_);
    tc->socket_descriptor_ = nullptr;
    cond_.Signal();
  }

  void WaitTestReadFinish(TestReadContext* tc) {
    AutoLock lock(&mu_);
    while (tc->socket_descriptor_ != nullptr) {
      cond_.Wait(&mu_);
    }
  }

  OneshotClosure* NewTestDescriptorWrite(TestWriteContext* tc) {
    {
      AutoLock lock(&mu_);
      EXPECT_GT(tc->fd_, 0);
      EXPECT_LT(tc->num_write_, 0);
      EXPECT_TRUE(tc->socket_descriptor_ == nullptr);
    }
    return NewCallback(
        this, &WorkerThreadManagerTest::TestDescriptorWrite, tc);
  }

  void TestDescriptorWrite(TestWriteContext* tc) {
    ScopedSocket sock;
    {
      AutoLock lock(&mu_);
      EXPECT_LT(tc->num_write_, 0);
      EXPECT_TRUE(tc->socket_descriptor_ == nullptr);
      sock.reset(tc->fd_);
    }
    SocketDescriptor* descriptor = wm_->RegisterSocketDescriptor(
        std::move(sock), WorkerThread::PRIORITY_HIGH);
    descriptor->NotifyWhenWritable(
        NewPermanentCallback(this, &WorkerThreadManagerTest::DoWrite, tc));
    AutoLock lock(&mu_);
    tc->num_write_ = 0;
    tc->socket_descriptor_ = descriptor;
    cond_.Signal();
  }

  void DoWrite(TestWriteContext* tc) {
    int num_write = 0;
    int total_write = 0;
    SocketDescriptor* descriptor = nullptr;
    {
      AutoLock lock(&mu_);
      EXPECT_GE(tc->num_write_, 0);
      EXPECT_EQ(tc->fd_, tc->socket_descriptor_->fd());
      EXPECT_EQ(WorkerThread::PRIORITY_HIGH,
                tc->socket_descriptor_->priority());
      num_write = tc->num_write_;
      total_write = tc->total_write_;
      descriptor = tc->socket_descriptor_;
    }
    char buf[1] = { 42 };
    ssize_t write_size = 0;
    if (num_write < total_write) {
      write_size = descriptor->Write(buf, 1);
    }
    if (write_size > 0) {
      EXPECT_EQ(1, write_size);
    } else {
      descriptor->StopWrite();
      wm_->RunClosureInThread(
          FROM_HERE,
          wm_->GetCurrentThreadId(),
          NewCallback(
              this, &WorkerThreadManagerTest::DoStopWrite, tc),
          WorkerThread::PRIORITY_IMMEDIATE);
      return;
    }
    AutoLock lock(&mu_);
    ++tc->num_write_;
    cond_.Signal();
  }

  void WaitTestWrite(TestWriteContext* tc, int n) {
    AutoLock lock(&mu_);
    while (tc->num_write_ < n) {
      cond_.Wait(&mu_);
    }
  }

  void DoStopWrite(TestWriteContext* tc) {
    int fd;
    SocketDescriptor* descriptor = nullptr;
    {
      AutoLock lock(&mu_);
      EXPECT_EQ(tc->fd_, tc->socket_descriptor_->fd());
      EXPECT_EQ(WorkerThread::PRIORITY_HIGH,
                tc->socket_descriptor_->priority());
      fd = tc->fd_;
      descriptor = tc->socket_descriptor_;
    }
    descriptor->ClearReadable();  // Check b/148360680 case.
    descriptor->ClearWritable();
    ScopedSocket sock(wm_->DeleteSocketDescriptor(descriptor));
    EXPECT_EQ(fd, sock.get());
    sock.Close();
    AutoLock lock(&mu_);
    tc->socket_descriptor_ = nullptr;
    cond_.Signal();
  }

  void WaitTestWriteFinish(TestWriteContext* tc) {
    AutoLock lock(&mu_);
    while (tc->socket_descriptor_ != nullptr) {
      cond_.Wait(&mu_);
    }
  }

  WorkerThread::ThreadId test_threadid() const {
    AutoLock lock(&mu_);
    return test_threadid_;
  }

  int num_test_threadid() const {
    AutoLock lock(&mu_);
    return num_test_threadid_;
  }

  int periodic_counter() const {
    AutoLock lock(&mu_);
    return periodic_counter_;
  }

  std::unique_ptr<WorkerThreadManager> wm_;
  mutable Lock mu_;

 private:
  ConditionVariable cond_;
  WorkerThread::ThreadId test_threadid_;
  int num_test_threadid_;
  int periodic_counter_;
  DISALLOW_COPY_AND_ASSIGN(WorkerThreadManagerTest);
};

TEST_F(WorkerThreadManagerTest, NoRun) {
  wm_->Start(2);
  EXPECT_EQ(2U, wm_->num_threads());
  wm_->Finish();
}

TEST_F(WorkerThreadManagerTest, RunClosure) {
  wm_->Start(2);
  wm_->RunClosure(FROM_HERE, NewTestRun(),
                  WorkerThread::PRIORITY_LOW);
  WaitTestRun();
  wm_->Finish();
  EXPECT_NE(test_threadid(), static_cast<WorkerThread::ThreadId>(0));
  EXPECT_NE(test_threadid(), wm_->GetCurrentThreadId());
}

TEST_F(WorkerThreadManagerTest, Dispatch) {
  wm_->Start(1);
  wm_->RunClosure(FROM_HERE, NewTestDispatch(),
                  WorkerThread::PRIORITY_LOW);
  wm_->RunClosure(FROM_HERE, NewTestRun(),
                  WorkerThread::PRIORITY_LOW);
  WaitTestRun();
  wm_->Finish();
  EXPECT_NE(test_threadid(), static_cast<WorkerThread::ThreadId>(0));
  EXPECT_NE(test_threadid(), wm_->GetCurrentThreadId());
}

TEST_F(WorkerThreadManagerTest, RunClosureInThread) {
  wm_->Start(2);
  wm_->RunClosure(FROM_HERE, NewTestRun(),
                  WorkerThread::PRIORITY_LOW);
  WaitTestRun();
  EXPECT_NE(test_threadid(), static_cast<WorkerThread::ThreadId>(0));
  EXPECT_NE(test_threadid(), wm_->GetCurrentThreadId());
  WorkerThread::ThreadId id = test_threadid();
  Reset();
  EXPECT_TRUE(!test_threadid());
  EXPECT_EQ(num_test_threadid(), 0);
  const int kNumTestThreadHandle = 100;
  for (int i = 0; i < kNumTestThreadHandle; ++i) {
    wm_->RunClosureInThread(FROM_HERE, id, NewTestThreadId(id),
                          WorkerThread::PRIORITY_LOW);
  }
  WaitTestThreadHandle(kNumTestThreadHandle);
  wm_->Finish();
}

TEST_F(WorkerThreadManagerTest, RunClosureInPool) {
  wm_->Start(1);
  int pool = wm_->StartPool(1, "test");
  EXPECT_NE(pool, WorkerThreadManager::kAlarmPool);
  EXPECT_NE(pool, WorkerThreadManager::kFreePool);
  EXPECT_EQ(2U, wm_->num_threads());

  wm_->RunClosure(FROM_HERE, NewTestRun(),
                  WorkerThread::PRIORITY_LOW);
  WaitTestRun();
  EXPECT_NE(test_threadid(), static_cast<WorkerThread::ThreadId>(0));
  EXPECT_NE(test_threadid(), wm_->GetCurrentThreadId());
  WorkerThread::ThreadId free_id = test_threadid();
  Reset();
  EXPECT_TRUE(!test_threadid());

  wm_->RunClosureInPool(FROM_HERE, pool, NewTestRun(),
                        WorkerThread::PRIORITY_LOW);
  WaitTestRun();
  EXPECT_NE(test_threadid(), static_cast<WorkerThread::ThreadId>(0));
  EXPECT_NE(test_threadid(), wm_->GetCurrentThreadId());
  EXPECT_NE(test_threadid(), free_id);
  WorkerThread::ThreadId pool_id = test_threadid();
  Reset();
  EXPECT_TRUE(!test_threadid());
  EXPECT_TRUE(!num_test_threadid());
  const int kNumTestThreadHandle = 100;
  for (int i = 0; i < kNumTestThreadHandle; ++i) {
    wm_->RunClosureInPool(FROM_HERE, pool, NewTestThreadId(pool_id),
                          WorkerThread::PRIORITY_LOW);
  }
  WaitTestThreadHandle(kNumTestThreadHandle);
  wm_->Finish();
}

TEST_F(WorkerThreadManagerTest, PeriodicClosure) {
  wm_->Start(1);
  SimpleTimer timer;
  PeriodicClosureId id = wm_->RegisterPeriodicClosure(
      FROM_HERE, absl::Milliseconds(100), NewPeriodicRun());
  WaitTestPeriodicRun(2);
  wm_->UnregisterPeriodicClosure(id);
  wm_->Finish();
  EXPECT_GE(timer.GetDuration(), absl::Milliseconds(200));
}

TEST_F(WorkerThreadManagerTest, DescriptorReadable) {
  wm_->Start(1);
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  TestReadContext tc(socks[0], absl::ZeroDuration());
  ScopedSocket s(socks[1]);
  wm_->RunClosure(FROM_HERE, NewTestDescriptorRead(&tc),
                  WorkerThread::PRIORITY_LOW);
  WaitTestRead(&tc, 0);
  {
    AutoLock lock(&mu_);
    EXPECT_EQ(0, tc.num_read_);
    EXPECT_TRUE(tc.socket_descriptor_ != nullptr);
  }
  char buf[1] = { 42 };
  EXPECT_EQ(1, s.Write(buf, 1));
  WaitTestRead(&tc, 1);
  {
    AutoLock lock(&mu_);
    EXPECT_EQ(1, tc.num_read_);
    EXPECT_TRUE(tc.socket_descriptor_ != nullptr);
  }
  s.Close();
  WaitTestReadFinish(&tc);
  {
    AutoLock lock(&mu_);
    EXPECT_EQ(2, tc.num_read_);
    EXPECT_TRUE(tc.socket_descriptor_ == nullptr);
  }
  wm_->Finish();
}

TEST_F(WorkerThreadManagerTest, DescriptorWritable) {
  wm_->Start(1);
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  const int kTotalWrite = 8192;
  TestWriteContext tc(socks[1], kTotalWrite);
  ScopedSocket s0(socks[0]);
  ScopedSocket s1(socks[1]);
  wm_->RunClosure(FROM_HERE, NewTestDescriptorWrite(&tc),
                  WorkerThread::PRIORITY_LOW);
  WaitTestWrite(&tc, 1);
  {
    AutoLock lock(&mu_);
    EXPECT_GE(tc.num_write_, 1);
    EXPECT_TRUE(tc.socket_descriptor_ != nullptr);
  }
  char buf[1] = { 42 };
  int total_read = 0;
  for (;;) {
    int n = s0.Read(buf, 1);
    if (n == 0) {
      break;
    }
    if (n < 0) {
      PLOG(ERROR) << "read " << n;
      break;
    }
    EXPECT_EQ(1, n);
    total_read += n;
  }
  WaitTestWriteFinish(&tc);
  {
    AutoLock lock(&mu_);
    EXPECT_TRUE(tc.socket_descriptor_ == nullptr);
    EXPECT_EQ(kTotalWrite, tc.num_write_);
    EXPECT_EQ(kTotalWrite, total_read);
  }
  s1.Close();
  wm_->Finish();
}

TEST_F(WorkerThreadManagerTest, DescriptorTimeout) {
  wm_->Start(1);
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  TestReadContext tc(socks[0], absl::Milliseconds(500));
  ScopedSocket s(socks[1]);
  wm_->RunClosure(FROM_HERE, NewTestDescriptorRead(&tc),
                  WorkerThread::PRIORITY_LOW);
  WaitTestRead(&tc, 0);
  {
    AutoLock lock(&mu_);
    EXPECT_EQ(0, tc.num_read_);
    EXPECT_TRUE(tc.socket_descriptor_ != nullptr);
  }
  char buf[1] = { 42 };
  EXPECT_EQ(1, s.Write(buf, 1));
  WaitTestRead(&tc, 1);
  {
    AutoLock lock(&mu_);
    EXPECT_EQ(1, tc.num_read_);
    EXPECT_FALSE(tc.timeout_called_);
    EXPECT_TRUE(tc.socket_descriptor_ != nullptr);
  }
  WaitTestReadFinish(&tc);
  {
    AutoLock lock(&mu_);
    EXPECT_EQ(1, tc.num_read_);
    EXPECT_TRUE(tc.timeout_called_);
    EXPECT_TRUE(tc.socket_descriptor_ == nullptr);
  }
  wm_->Finish();
}

}  // namespace devtools_goma
