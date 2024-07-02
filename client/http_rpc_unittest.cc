// Copyright 2012 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "http_rpc.h"

#include <string>
#include <sstream>

#include "absl/memory/memory.h"
#include "callback.h"
#include "compiler_proxy_info.h"
#include "compiler_specific.h"
#include "fake_tls_engine.h"
MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "google/protobuf/message_lite.h"
#include "google/protobuf/io/gzip_stream.h"
#include "google/protobuf/io/zero_copy_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
MSVC_POP_WARNING()
#include "ioutil.h"
#include "lockhelper.h"
#include "mock_socket_factory.h"
#include "platform_thread.h"
MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "lib/goma_data.pb.h"
MSVC_POP_WARNING()
#include "scoped_fd.h"
#include "socket_factory.h"
#include "worker_thread.h"
#include "worker_thread_manager.h"

#include <glog/logging.h>
#include <gtest/gtest.h>

namespace devtools_goma {

class HttpRPCTest : public ::testing::Test {
 protected:
  class TestLookupFileContext {
   public:
    enum State { INIT, CALL, DONE };
    TestLookupFileContext(HttpRPC* http_rpc,
                          OneshotClosure* callback)
        : http_rpc_(http_rpc),
          callback_(callback),
          r_(0),
          state_(INIT) {
    }

    HttpRPC* http_rpc_;
    OneshotClosure* callback_;
    LookupFileReq req_;
    LookupFileResp resp_;
    HttpRPC::Status status_;
    int r_;
    int state_;
  };

  HttpRPCTest() : pool_(-1) {}

  void SetUp() override {
    wm_ = absl::make_unique<WorkerThreadManager>();
    wm_->Start(1);
    pool_ = wm_->StartPool(1, "test");
    mock_server_ = absl::make_unique<MockSocketServer>(wm_.get());
  }
  void TearDown() override {
    mock_server_.reset();
    wm_->Finish();
    wm_.reset();
    pool_ = -1;
  }

  void RunTestLookupFile(TestLookupFileContext* tc) {
    wm_->RunClosureInPool(
        FROM_HERE,
        pool_,
        NewCallback(
            this, &HttpRPCTest::DoTestLookupFile, tc),
        WorkerThread::PRIORITY_LOW);
  }

  void DoTestLookupFile(TestLookupFileContext* tc) {
    if (tc->callback_ != nullptr) {
      tc->http_rpc_->CallWithCallback(
          "/l", &tc->req_, &tc->resp_, &tc->status_, tc->callback_);
      AutoLock lock(&mu_);
      tc->state_ = TestLookupFileContext::CALL;
      cond_.Signal();
    } else {
      int r = tc->http_rpc_->Call(
          "/l", &tc->req_, &tc->resp_, &tc->status_);
      AutoLock lock(&mu_);
      tc->r_ = r;
      tc->state_ = TestLookupFileContext::DONE;
      cond_.Signal();
    }
  }

  void WaitTestLookupFile(TestLookupFileContext* tc) {
    wm_->RunClosureInPool(
        FROM_HERE,
        pool_,
        NewCallback(
            this, &HttpRPCTest::DoWaitTestLookupFile, tc),
        WorkerThread::PRIORITY_LOW);
  }

  void DoWaitTestLookupFile(TestLookupFileContext* tc) {
    tc->http_rpc_->Wait(&tc->status_);
    AutoLock lock(&mu_);
    tc->state_ = TestLookupFileContext::DONE;
    cond_.Signal();
  }

  OneshotClosure* NewDoneCallback(bool* done) {
    {
      AutoLock lock(&mu_);
      *done = false;
    }
    return NewCallback(
        this, &HttpRPCTest::DoneCallback, done);
  }

  void DoneCallback(bool* done) {
    AutoLock lock(&mu_);
    *done = true;
    cond_.Signal();
  }

  void SerializeCompressToString(const google::protobuf::Message& msg,
                                 int compression_level,
                                 std::string* serialized) {
    std::string buf;
    google::protobuf::io::StringOutputStream stream(&buf);
    google::protobuf::io::GzipOutputStream::Options options;
    options.format = google::protobuf::io::GzipOutputStream::ZLIB;
    options.compression_level = compression_level;
    google::protobuf::io::GzipOutputStream gzip_stream(&stream, options);
    msg.SerializeToZeroCopyStream(&gzip_stream);
    ASSERT_TRUE(gzip_stream.Close());
    ASSERT_GE(buf.size(), 1);
    ASSERT_FALSE(buf[1] >> 5 & 1)
        << "serialized has FDICT, which should not be supported";
    absl::string_view v(buf);
    v.remove_prefix(2);
    *serialized = std::string(v);
  }

  std::unique_ptr<WorkerThreadManager> wm_;
  int pool_;
  std::unique_ptr<MockSocketServer> mock_server_;
  mutable Lock mu_;
  ConditionVariable cond_;
};

TEST_F(HttpRPCTest, EnableCompression) {
  // TODO: cleanup as http_unittest.
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(-1));
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 80;
  HttpClient http_client(
      std::move(socket_factory), nullptr, options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.compression_level = 3;
  rpc_options.start_compression = true;
  rpc_options.accept_encoding = "deflate";
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  std::unique_ptr<HttpRPC> http_rpc(
      absl::make_unique<HttpRPC>(&http_client, rpc_options));
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: deflate\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: deflate, gzip\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: gzip\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::GZIP)
      << GetEncodingName(http_rpc->request_encoding_type());

  rpc_options.accept_encoding = "gzip";
  http_rpc = absl::make_unique<HttpRPC>(&http_client, rpc_options);
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::GZIP)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: deflate\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: deflate, gzip\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: gzip\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::GZIP)
      << GetEncodingName(http_rpc->request_encoding_type());

  // accepts lzma2, but client can send deflate or gzip only.
  rpc_options.accept_encoding = "lzma2";
  http_rpc = absl::make_unique<HttpRPC>(&http_client, rpc_options);
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: deflate\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: deflate, gzip\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: gzip\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::GZIP)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: lzma2\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: deflate, gzip, lzma2\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());

  rpc_options.accept_encoding = "deflate, lzma2";
  http_rpc = absl::make_unique<HttpRPC>(&http_client, rpc_options);
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  rpc_options.accept_encoding = "lzma2, deflate";
  http_rpc = absl::make_unique<HttpRPC>(&http_client, rpc_options);
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: deflate\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: deflate, gzip\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: gzip\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::GZIP)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: lzma2\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: deflate, gzip, lzma2\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: gzip, lzma2, deflate\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::GZIP)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: lzma2, gzip, deflate\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::GZIP)
      << GetEncodingName(http_rpc->request_encoding_type());


  rpc_options.accept_encoding = "deflate, gzip, lzma2";
  http_rpc = absl::make_unique<HttpRPC>(&http_client, rpc_options);
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: deflate\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: deflate, gzip\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: gzip, deflate\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::GZIP)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: gzip\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::GZIP)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: lzma2\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: deflate, gzip, lzma2\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::DEFLATE)
      << GetEncodingName(http_rpc->request_encoding_type());
  http_rpc->DisableCompression();
  EXPECT_FALSE(http_rpc->IsCompressionEnabled());
  http_rpc->EnableCompression("HTTP/1.1 200 OK\r\n"
                              "Accept-Encoding: gzip, deflate, lzma2\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n");
  EXPECT_TRUE(http_rpc->IsCompressionEnabled());
  EXPECT_TRUE(http_rpc->request_encoding_type() == EncodingType::GZIP)
      << GetEncodingName(http_rpc->request_encoding_type());
}

TEST_F(HttpRPCTest, PingFail) {
  std::unique_ptr<MockSocketFactory> socket_factory(
      new MockSocketFactory(-1));
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 80;
  HttpClient http_client(
      std::move(socket_factory), nullptr, options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  int r = http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(0, r);
  EXPECT_EQ("running: failed to connect to backend servers",
            http_client.GetHealthStatusMessage());
  http_client.WaitNoActive();
}

TEST_F(HttpRPCTest, PingRejected) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  std::ostringstream req_ss;
  req_ss << "POST /pingz HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: 0\r\n\r\n";

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 401 Unauthorized\r\n"
          << "Content-Type: text/plain\r\n"
          << "Content-Length: 5\r\n\r\n"
          << "error";
  mock_server_->ServerWrite(socks[0], resp_ss.str());
  mock_server_->ServerClose(socks[0]);

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      new MockSocketFactory(socks[1], &socket_status));
  socket_factory->set_dest("goma.chromium.org:80");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(80);
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 80;
  HttpClient http_client(
      std::move(socket_factory), nullptr, options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  int r = http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(req_expected, req_buf);
  EXPECT_EQ(401, r);
  EXPECT_EQ("error: access to backend servers was rejected.",
            http_client.GetHealthStatusMessage());
  http_client.WaitNoActive();
  EXPECT_FALSE(socket_status.is_owned());
  EXPECT_TRUE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_err());
  EXPECT_FALSE(socket_status.is_released());
}

TEST_F(HttpRPCTest, PingOk) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  std::ostringstream req_ss;
  req_ss << "POST /pingz HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: 0\r\n\r\n";

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: text/plain\r\n"
          << "Content-Length: 2\r\n\r\n"
          << "ok";
  mock_server_->ServerWrite(socks[0], resp_ss.str());

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      new MockSocketFactory(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:80");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(80);
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 80;
  HttpClient http_client(
      std::move(socket_factory), nullptr, options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  int r = http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(req_expected, req_buf);
  EXPECT_EQ(200, r);
  EXPECT_EQ("ok", http_client.GetHealthStatusMessage());
  http_client.WaitNoActive();
  EXPECT_TRUE(socket_status.is_owned());
  EXPECT_FALSE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_released());
}

TEST_F(HttpRPCTest, CallLookupFile) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  LookupFileReq req;
  std::string serialized_req;
  req.SerializeToString(&serialized_req);
  std::ostringstream req_ss;
  req_ss << "POST /l HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: " << serialized_req.size() << "\r\n\r\n"
         << serialized_req;

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  LookupFileResp resp;
  std::string serialized_resp;
  resp.SerializeToString(&serialized_resp);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: text/x-protocol-buffer\r\n"
          << "Content-Length: " << serialized_resp.size() << "\r\n\r\n"
          << serialized_resp;
  mock_server_->ServerWrite(socks[0], resp_ss.str());

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      new MockSocketFactory(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:80");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(80);
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 80;
  HttpClient http_client(
      std::move(socket_factory), nullptr, options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  rpc_options.start_compression = false;
  HttpRPC http_rpc(&http_client, rpc_options);
  TestLookupFileContext tc(&http_rpc, nullptr);
  RunTestLookupFile(&tc);
  {
    AutoLock lock(&mu_);
    while (tc.state_ != TestLookupFileContext::DONE) {
      cond_.Wait(&mu_);
    }

    EXPECT_EQ(req_expected, req_buf);
    EXPECT_EQ(0, tc.r_);
    EXPECT_TRUE(tc.status_.connect_success);
    EXPECT_TRUE(tc.status_.finished);
    EXPECT_EQ(0, tc.status_.err);
    EXPECT_EQ("", tc.status_.err_message);
    EXPECT_EQ(200, tc.status_.http_return_code);
  }
  http_client.WaitNoActive();
  EXPECT_TRUE(socket_status.is_owned());
  EXPECT_FALSE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_released());
}

TEST_F(HttpRPCTest, CallAsyncLookupFile) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  LookupFileReq req;
  std::string serialized_req;
  req.SerializeToString(&serialized_req);
  std::ostringstream req_ss;
  req_ss << "POST /l HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: " << serialized_req.size() << "\r\n\r\n"
         << serialized_req;

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  LookupFileResp resp;
  std::string serialized_resp;
  resp.SerializeToString(&serialized_resp);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: text/x-protocol-buffer\r\n"
          << "Content-Length: " << serialized_resp.size() << "\r\n\r\n"
          << serialized_resp;

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      new MockSocketFactory(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:80");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(80);
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 80;
  HttpClient http_client(
      std::move(socket_factory), nullptr, options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  rpc_options.start_compression = false;
  HttpRPC http_rpc(&http_client, rpc_options);
  bool done = false;
  TestLookupFileContext tc(&http_rpc, NewDoneCallback(&done));
  RunTestLookupFile(&tc);
  {
    AutoLock lock(&mu_);
    while (tc.state_ != TestLookupFileContext::CALL) {
      cond_.Wait(&mu_);
    }

    EXPECT_TRUE(tc.status_.connect_success);
    EXPECT_FALSE(tc.status_.finished);
  }

  mock_server_->ServerWrite(socks[0], resp_ss.str());
  WaitTestLookupFile(&tc);

  {
    AutoLock lock(&mu_);
    while (!done) {
      cond_.Wait(&mu_);
    }
    while (tc.state_ != TestLookupFileContext::DONE) {
      cond_.Wait(&mu_);
    }
    EXPECT_EQ(req_expected, req_buf);
    EXPECT_EQ(0, tc.r_);
    EXPECT_TRUE(tc.status_.connect_success);
    EXPECT_TRUE(tc.status_.finished);
    EXPECT_EQ(0, tc.status_.err);
    EXPECT_EQ("", tc.status_.err_message);
    EXPECT_EQ(200, tc.status_.http_return_code);
  }
  http_client.WaitNoActive();
  EXPECT_TRUE(socket_status.is_owned());
  EXPECT_FALSE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEnginePingFail) {
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(-1));
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(
      std::move(socket_factory),
      std::move(tls_engine_factory),
      options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  int r = http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(0, r);
  EXPECT_EQ("running: failed to connect to backend servers",
            http_client.GetHealthStatusMessage());
  http_client.WaitNoActive();
}

TEST_F(HttpRPCTest, TLSEnginePingRejected) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  std::ostringstream req_ss;
  req_ss << "POST /pingz HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: 0\r\n\r\n";

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 401 Unauthorized\r\n"
          << "Content-Type: text/plain\r\n"
          << "Content-Length: 5\r\n\r\n"
          << "error";
  mock_server_->ServerWrite(socks[0], resp_ss.str());
  mock_server_->ServerClose(socks[0]);

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  int r = http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(req_expected, req_buf);
  EXPECT_EQ(401, r);
  EXPECT_EQ("error: access to backend servers was rejected.",
            http_client.GetHealthStatusMessage());
  http_client.WaitNoActive();
  EXPECT_FALSE(socket_status.is_owned());
  EXPECT_TRUE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_err());
  EXPECT_FALSE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEnginePingOk) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  std::ostringstream req_ss;
  req_ss << "POST /pingz HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: 0\r\n\r\n";

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: text/plain\r\n"
          << "Content-Length: 2\r\n\r\n"
          << "ok";
  mock_server_->ServerWrite(socks[0], resp_ss.str());

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  int r = http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(req_expected, req_buf);
  EXPECT_EQ(200, r);
  EXPECT_EQ("ok", http_client.GetHealthStatusMessage());
  http_client.WaitNoActive();
  EXPECT_TRUE(socket_status.is_owned());
  EXPECT_FALSE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEngineCallLookupFile) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  LookupFileReq req;
  std::string serialized_req;
  req.SerializeToString(&serialized_req);
  std::ostringstream req_ss;
  req_ss << "POST /l HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: " << serialized_req.size() << "\r\n\r\n"
         << serialized_req;

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  LookupFileResp resp;
  std::string serialized_resp;
  resp.SerializeToString(&serialized_resp);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: text/x-protocol-buffer\r\n"
          << "Content-Length: " << serialized_resp.size() << "\r\n\r\n"
          << serialized_resp;
  mock_server_->ServerWrite(socks[0], resp_ss.str());

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  rpc_options.start_compression = false;
  HttpRPC http_rpc(&http_client, rpc_options);
  TestLookupFileContext tc(&http_rpc, nullptr);
  RunTestLookupFile(&tc);
  {
    AutoLock lock(&mu_);
    while (tc.state_ != TestLookupFileContext::DONE) {
      cond_.Wait(&mu_);
    }

    EXPECT_EQ(req_expected, req_buf);
    EXPECT_EQ(0, tc.r_);
    EXPECT_TRUE(tc.status_.connect_success);
    EXPECT_TRUE(tc.status_.finished);
    EXPECT_EQ(0, tc.status_.err);
    EXPECT_EQ("", tc.status_.err_message);
    EXPECT_EQ(200, tc.status_.http_return_code);
  }
  http_client.WaitNoActive();
  EXPECT_TRUE(socket_status.is_owned());
  EXPECT_FALSE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEngineCallLookupFileDeflate) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  const int kCompressionLevel = 3;
  LookupFileReq req;
  std::string serialized_req;
  SerializeCompressToString(req, kCompressionLevel, &serialized_req);
  std::ostringstream req_ss;
  req_ss << "POST /l HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: " << serialized_req.size() << "\r\n"
         << "Accept-Encoding: deflate\r\n"
         << "Content-Encoding: deflate\r\n\r\n"
         << serialized_req;

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  LookupFileResp resp;
  std::string serialized_resp;
  SerializeCompressToString(resp, kCompressionLevel, &serialized_resp);
  LOG(INFO) << "resp length=" << serialized_resp.size()
            << " data=" << serialized_resp;
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: text/x-protocol-buffer\r\n"
          << "Accept-Encoding: deflate\r\n"
          << "Content-Encoding: deflate\r\n"
          << "Content-Length: " << serialized_resp.size() << "\r\n\r\n"
          << serialized_resp;
  mock_server_->ServerWrite(socks[0], resp_ss.str());

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  rpc_options.start_compression = true;
  rpc_options.compression_level = kCompressionLevel;
  rpc_options.accept_encoding = "deflate";
  HttpRPC http_rpc(&http_client, rpc_options);
  TestLookupFileContext tc(&http_rpc, nullptr);
  RunTestLookupFile(&tc);
  {
    AutoLock lock(&mu_);
    while (tc.state_ != TestLookupFileContext::DONE) {
      cond_.Wait(&mu_);
    }

    EXPECT_EQ(req_expected, req_buf);
    EXPECT_EQ(0, tc.r_);
    EXPECT_TRUE(tc.status_.connect_success);
    EXPECT_TRUE(tc.status_.finished);
    EXPECT_EQ(0, tc.status_.err);
    EXPECT_EQ("", tc.status_.err_message);
    EXPECT_EQ(200, tc.status_.http_return_code);
  }
  http_client.WaitNoActive();
  EXPECT_TRUE(socket_status.is_owned());
  EXPECT_FALSE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEngineCallAsyncLookupFile) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  LookupFileReq req;
  std::string serialized_req;
  req.SerializeToString(&serialized_req);
  std::ostringstream req_ss;
  req_ss << "POST /l HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: " << serialized_req.size() << "\r\n\r\n"
         << serialized_req;

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  LookupFileResp resp;
  std::string serialized_resp;
  resp.SerializeToString(&serialized_resp);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: text/x-protocol-buffer\r\n"
          << "Content-Length: " << serialized_resp.size() << "\r\n\r\n"
          << serialized_resp;

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  rpc_options.start_compression = false;
  HttpRPC http_rpc(&http_client, rpc_options);
  bool done = false;
  TestLookupFileContext tc(&http_rpc, NewDoneCallback(&done));
  RunTestLookupFile(&tc);
  {
    AutoLock lock(&mu_);
    while (tc.state_ != TestLookupFileContext::CALL) {
      cond_.Wait(&mu_);
    }

    EXPECT_TRUE(tc.status_.connect_success);
    EXPECT_FALSE(tc.status_.finished);
  }

  mock_server_->ServerWrite(socks[0], resp_ss.str());
  WaitTestLookupFile(&tc);

  {
    AutoLock lock(&mu_);
    while (!done) {
      cond_.Wait(&mu_);
    }
    while (tc.state_ != TestLookupFileContext::DONE) {
      cond_.Wait(&mu_);
    }
    EXPECT_EQ(req_expected, req_buf);
    EXPECT_EQ(0, tc.r_);
    EXPECT_TRUE(tc.status_.connect_success);
    EXPECT_TRUE(tc.status_.finished);
    EXPECT_EQ(0, tc.status_.err);
    EXPECT_EQ("", tc.status_.err_message);
    EXPECT_EQ(200, tc.status_.http_return_code);
  }
  http_client.WaitNoActive();
  EXPECT_TRUE(socket_status.is_owned());
  EXPECT_FALSE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEngineFailWithTLSErrorAtSetData) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  std::ostringstream req_ss;
  req_ss << "POST /pingz HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: 0\r\n\r\n";

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: text/plain\r\n"
          << "Content-Length: 2\r\n\r\n"
          << "ok";
  mock_server_->ServerWrite(socks[0], resp_ss.str());
  mock_server_->ServerClose(socks[0]);

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  tls_engine_factory->SetBroken(FakeTLSEngine::FAKE_TLS_SET_BROKEN);
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  int r = http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(req_expected, req_buf);
  EXPECT_EQ(500, r);
  EXPECT_EQ("running: failed to send request to backend servers",
            http_client.GetHealthStatusMessage());
  http_client.WaitNoActive();
  EXPECT_FALSE(socket_status.is_owned());
  EXPECT_TRUE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_err());
  EXPECT_FALSE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEngineFailWithTLSErrorAtRead) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  std::ostringstream req_ss;
  req_ss << "POST /pingz HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: 0\r\n\r\n";

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  mock_server_->ServerClose(socks[0]);

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  tls_engine_factory->SetBroken(FakeTLSEngine::FAKE_TLS_READ_BROKEN);
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  int r = http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(500, r);
  EXPECT_EQ("running: failed to send request to backend servers",
            http_client.GetHealthStatusMessage());
  http_client.WaitNoActive();
  EXPECT_FALSE(socket_status.is_owned());
  EXPECT_TRUE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_err());
  EXPECT_FALSE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEngineFailWithTLSErrorAtWrite) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  std::string req_buf;
  mock_server_->ServerRead(socks[0], &req_buf);

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  tls_engine_factory->SetBroken(FakeTLSEngine::FAKE_TLS_WRITE_BROKEN);
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  int r = http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(500, r);
  EXPECT_EQ("running: failed to send request to backend servers",
            http_client.GetHealthStatusMessage());
  // Nothing should be requested to the server.
  EXPECT_EQ("", req_buf);
  mock_server_->ServerClose(socks[0]);
  http_client.WaitNoActive();
  EXPECT_FALSE(socket_status.is_owned());
  EXPECT_TRUE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_err());
  EXPECT_FALSE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEngineServerTimeoutSendingHeaderShouldBeError) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  std::ostringstream req_ss;
  req_ss << "POST /pingz HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: 0\r\n\r\n";

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  mock_server_->ServerWait(absl::Milliseconds(1500));
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n";
  mock_server_->ServerWrite(socks[0], resp_ss.str());
  mock_server_->ServerClose(socks[0]);

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  status.timeouts.push_back(absl::Seconds(1));
  http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(ERR_TIMEOUT, status.err);
  http_client.WaitNoActive();
  EXPECT_FALSE(socket_status.is_owned());
  EXPECT_TRUE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_err());
  EXPECT_FALSE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEngineServerCloseWithoutContentLengthShouldBeOk) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  std::ostringstream req_ss;
  req_ss << "POST /pingz HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: 0\r\n\r\n";

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: text/plain\r\n\r\n"
          << "ok";
  mock_server_->ServerWrite(socks[0], resp_ss.str());
  mock_server_->ServerClose(socks[0]);

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  int r = http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(req_expected, req_buf);
  EXPECT_EQ(200, r);
  EXPECT_EQ("ok", http_client.GetHealthStatusMessage());
  http_client.WaitNoActive();
  EXPECT_FALSE(socket_status.is_owned());
  EXPECT_TRUE(socket_status.is_closed());
  EXPECT_FALSE(socket_status.is_err());
  EXPECT_FALSE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEngineServerCloseBeforeSendingHeaderShouldBeError) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  std::ostringstream req_ss;
  req_ss << "POST /pingz HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: 0\r\n\r\n";

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n";
  mock_server_->ServerWrite(socks[0], resp_ss.str());
  mock_server_->ServerClose(socks[0]);

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(req_expected, req_buf);
  EXPECT_EQ(FAIL, status.err);
  http_client.WaitNoActive();
  EXPECT_FALSE(socket_status.is_owned());
  EXPECT_TRUE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_err());
  EXPECT_FALSE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEngineServerCloseBeforeReadingAnythingShouldBeError) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  mock_server_->ServerClose(socks[0]);

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
    HttpClient::Options options;
    options.dest_host_name = "goma.chromium.org";
    options.dest_port = 443;
    options.use_ssl = true;
    HttpClient http_client(std::move(socket_factory),
                           std::move(tls_engine_factory), options, wm_.get());
    HttpRPC::Options rpc_options;
    rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
    HttpRPC http_rpc(&http_client, rpc_options);
    HttpRPC::Status status;
    int r = http_rpc.Ping(wm_.get(), "/pingz", &status);
    EXPECT_EQ(500, r);
    EXPECT_EQ(FAIL, status.err);
    http_client.WaitNoActive();
    EXPECT_FALSE(socket_status.is_owned());
    EXPECT_TRUE(socket_status.is_closed());
    EXPECT_TRUE(socket_status.is_err());
    EXPECT_FALSE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEngineServerCloseBeforeSendingEnoughDataShouldBeError) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  std::ostringstream req_ss;
  req_ss << "POST /pingz HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: 0\r\n\r\n";

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: text/plain\r\n"
          << "Content-Length: 128\r\n\r\n"
          << "ok";
  mock_server_->ServerWrite(socks[0], resp_ss.str());
  mock_server_->ServerClose(socks[0]);

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(req_expected, req_buf);
  EXPECT_EQ(FAIL, status.err);
  http_client.WaitNoActive();
  EXPECT_FALSE(socket_status.is_owned());
  EXPECT_TRUE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_err());
  EXPECT_FALSE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEngineServerCloseWithoutContentLengthShouldNotHangUp) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  std::ostringstream req_ss;
  req_ss << "POST /pingz HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: 0\r\n\r\n";

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: text/plain\r\n\r\n"
          << "dummydata";
  mock_server_->ServerWrite(socks[0], resp_ss.str());
  mock_server_->ServerClose(socks[0]);

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  tls_engine_factory->SetMaxReadSize(10);
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(req_expected, req_buf);
  EXPECT_EQ(OK, status.err);
  http_client.WaitNoActive();
  EXPECT_FALSE(socket_status.is_owned());
  EXPECT_TRUE(socket_status.is_closed());
  EXPECT_FALSE(socket_status.is_err());
  EXPECT_FALSE(socket_status.is_released());
}

TEST_F(HttpRPCTest, TLSEngineServerCloseWithoutEndOfChunkShouldNotHangUp) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  std::ostringstream req_ss;
  req_ss << "POST /pingz HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: 0\r\n\r\n";

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n"
          << "Transfer-Encoding: chunked\r\n"
          << "Content-Type: text/plain\r\n\r\n"
          << "1\r\na";  // not sending all data but closed.
  mock_server_->ServerWrite(socks[0], resp_ss.str());
  mock_server_->ServerClose(socks[0]);

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  tls_engine_factory->SetMaxReadSize(10);
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(req_expected, req_buf);
  EXPECT_EQ(FAIL, status.err);
  http_client.WaitNoActive();
  EXPECT_FALSE(socket_status.is_owned());
  EXPECT_TRUE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_err());
  EXPECT_FALSE(socket_status.is_released());
}


TEST_F(HttpRPCTest, TLSEngineServerCloseWithoutAllChunksShouldNotHangUp) {
  int socks[2];
  ASSERT_EQ(0, OpenSocketPairForTest(socks));
  std::ostringstream req_ss;
  req_ss << "POST /pingz HTTP/1.1\r\n"
         << "Host: goma.chromium.org\r\n"
         << "User-Agent: " << kUserAgentString << "\r\n"
         << "Content-Type: binary/x-protocol-buffer\r\n"
         << "Content-Length: 0\r\n\r\n";

  const std::string req_expected = req_ss.str();
  std::string req_buf;
  req_buf.resize(req_expected.size());
  mock_server_->ServerRead(socks[0], &req_buf);
  std::ostringstream resp_ss;
  resp_ss << "HTTP/1.1 200 OK\r\n"
          << "Transfer-Encoding: chunked\r\n"
          << "Content-Type: text/plain\r\n\r\n"
          << "1\r\na123\r\nbcd";  // not sending all data but closed.
  mock_server_->ServerWrite(socks[0], resp_ss.str());
  mock_server_->ServerClose(socks[0]);

  MockSocketFactory::SocketStatus socket_status;
  std::unique_ptr<MockSocketFactory> socket_factory(
      absl::make_unique<MockSocketFactory>(socks[1], &socket_status));

  socket_factory->set_dest("goma.chromium.org:443");
  socket_factory->set_host_name("goma.chromium.org");
  socket_factory->set_port(443);
  std::unique_ptr<FakeTLSEngineFactory> tls_engine_factory(
      absl::make_unique<FakeTLSEngineFactory>());
  tls_engine_factory->SetMaxReadSize(10);
  HttpClient::Options options;
  options.dest_host_name = "goma.chromium.org";
  options.dest_port = 443;
  options.use_ssl = true;
  HttpClient http_client(std::move(socket_factory),
                         std::move(tls_engine_factory),
                         options, wm_.get());
  HttpRPC::Options rpc_options;
  rpc_options.content_type_for_protobuf = "binary/x-protocol-buffer";
  HttpRPC http_rpc(&http_client, rpc_options);
  HttpRPC::Status status;
  http_rpc.Ping(wm_.get(), "/pingz", &status);
  EXPECT_EQ(req_expected, req_buf);
  EXPECT_EQ(FAIL, status.err);
  http_client.WaitNoActive();
  EXPECT_FALSE(socket_status.is_owned());
  EXPECT_TRUE(socket_status.is_closed());
  EXPECT_TRUE(socket_status.is_err());
  EXPECT_FALSE(socket_status.is_released());
}

}  // namespace devtools_goma
