// Copyright 2010 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_CLIENT_THREADPOOL_HTTP_SERVER_H_
#define DEVTOOLS_GOMA_CLIENT_THREADPOOL_HTTP_SERVER_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "basictypes.h"
#include "lockhelper.h"
#ifdef _WIN32
#include "named_pipe_server_win.h"
#endif
#include "scoped_fd.h"
#include "simple_timer.h"
#include "worker_thread.h"
#include "worker_thread_manager.h"

namespace devtools_goma {

class TrustedIpsManager;

class ThreadpoolHttpServer {
 public:
  typedef int RegisteredClosureID;

  enum SocketType {
    SOCKET_TCP,  // for http of status page etc.
    SOCKET_IPC,  // for IPC between gomacc and compiler_proxy.
    NUM_SOCKET_TYPES
  };
  class Stat {
   public:
    Stat() : req_size(0), resp_size(0) {}
    ~Stat() {}

    SimpleTimer timer;
    size_t req_size;
    size_t resp_size;

    absl::Duration waiting_time;
    absl::Duration read_req_time;
    absl::Duration handler_time;
    absl::Duration write_resp_time;
  };
  class Monitor {
   public:
    Monitor() {}
    virtual ~Monitor() {}

    virtual void FinishHandle(const Stat& stat) = 0;

   private:
    DISALLOW_COPY_AND_ASSIGN(Monitor);
  };

  class HttpServerRequest;
  class HttpHandler {
   public:
    HttpHandler() {}
    virtual ~HttpHandler() {}

    // HandleHttpRequest is responsible for freeing http_server_request by
    // calling http_server_request->SendReply()
    virtual void HandleHttpRequest(HttpServerRequest* http_server_request) = 0;

    virtual bool shutting_down() = 0;

   private:
    DISALLOW_COPY_AND_ASSIGN(HttpHandler);
  };

  class HttpServerRequest {
   public:
    HttpServerRequest(WorkerThreadManager* wm,
                      ThreadpoolHttpServer* server,
                      const Stat& stat,
                      Monitor* monitor);

    // Checks credential of peer.
    virtual bool CheckCredential() = 0;

    virtual bool IsTrusted() = 0;

    // Send response and delete this object.
    virtual void SendReply(const std::string& response) = 0;

    // Full request string with all the headers and body.
    const std::string& request() const { return request_; }

    absl::string_view header() const {
      absl::string_view h(request_.data(), request_offset_);
      return h;
    }
    size_t header_size() const { return request_offset_; }

    // Request body data.
    const char* request_content() const {
      return request_.data() + request_offset_;
    }
    size_t request_content_length() const {
      return request_content_length_;
    }

    // "GET", "POST", etc.
    const std::string& method() const { return method_; }
    const std::string& req_path() const { return req_path_; }

    // The string after ?
    const std::string& query() const { return query_; }

    pid_t peer_pid() const { return peer_pid_; }

    // if the HTTP Request was valid.
    bool ParsedValidHttpRequest() const { return parsed_valid_http_request_; }

    const ThreadpoolHttpServer& server() const { return *server_; }

    // Sets callback for request close.
    // It may be called on other thread than request's thread.
    // callback will be called on the thread where this method was called.
    virtual void NotifyWhenClosed(OneshotClosure* callback) = 0;

   protected:
    virtual ~HttpServerRequest() {}

    WorkerThreadManager* wm_;
    WorkerThread::ThreadId thread_id_;
    ThreadpoolHttpServer* server_;
    Monitor* monitor_;

    size_t request_offset_;
    size_t request_content_length_;
    size_t request_len_;
    std::string request_;
    std::string method_;
    std::string req_path_;
    std::string query_;
    std::string response_;
    // true if it got valid http request.
    bool parsed_valid_http_request_;

    pid_t peer_pid_;
    Stat stat_;

   private:
    DISALLOW_COPY_AND_ASSIGN(HttpServerRequest);
  };
  static const RegisteredClosureID kInvalidClosureId = 0;

  ThreadpoolHttpServer(std::string listen_addr,
                       int port,
                       int num_find_ports,
                       WorkerThreadManager* wm,
                       int num_threads,
                       HttpHandler* http_handler,
                       int max_num_sockets);
  ~ThreadpoolHttpServer();

  void HandleIncoming(HttpServerRequest* request);

  // Starts the main loop waiting for HTTP connections.
  int Loop();

  // Waits for all http requests process.
  void Wait();

  // Sets monitor.  Doesn't take ownership.
  void SetMonitor(Monitor* monitor);

  // Sets TrustedIpsManager.  Doesn't take ownership.
  void SetTrustedIpsManager(TrustedIpsManager* trustedipsmanager);

  // Starts IPC handlers on addr.  Must call before Loop.
  // num_threads and max_overcommit_incoming_sockets are used
  // to calculate max num incoming requests for IPC handlers.
  void StartIPC(const std::string& addr,
                int num_threads,
                int max_overcommit_incoming_sockets);

  // Stops IPC handlers.
  void StopIPC();

  // Utility function: Parse HTTP request string and extract method,
  // path, and query string.
  static bool ParseRequestLine(absl::string_view request,
                               std::string* method,
                               std::string* path,
                               std::string* query);

  int port() const { return port_; }

  const std::string& un_socket_name() const { return un_socket_name_; }

  // Registers idle closure.  closure must be permanent callback.
  // closure will be called after idle counter reaches "count".
  // Takes ownership of closure.
  RegisteredClosureID RegisterIdleClosure(
      SocketType socket_type, int count,
      std::unique_ptr<PermanentClosure> closure);
  // Unregisters idle closure.
  void UnregisterIdleClosure(RegisteredClosureID id);

  // Idle counter for socket_type.
  int idle_counter(SocketType socket_type) const;

  void SuspendIdleCounter();
  void ResumeIdleCounter();

 private:
  class RequestFromSocket;
  class IdleClosure;
#ifdef _WIN32
  class PipeHandler;
  class RequestFromNamedPipe;
#else

  // Opens unix domain socket to serve.  Must call before Loop().
  // Returns true if unix domain socket is successully opened.
  // On Windows, the path is actually the port number for socket IPC.
  bool OpenUnixDomainSocket(const std::string& path);
  void CloseUnixDomainSocket();
#endif

  // Sets limits of accepting sockets for "socket_type".
  void SetAcceptLimit(int n, SocketType socket_type);

  void AddAccept(SocketType socket_type);
  void RemoveAccept(SocketType socket_type);

  void WaitPortReady();
#ifdef _WIN32
  void SendNamedPipeJobToWorkerThread(NamedPipeServer::Request* req);
#endif
  void SendJobToWorkerThread(ScopedSocket&& socket, SocketType socket_type);
  void UpdateSocketIdleUnlocked(SocketType socket_type)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  const std::string listen_addr_;
  int port_;
  int port_ready_ ABSL_GUARDED_BY(mu_);
  int num_find_ports_;
  WorkerThreadManager* wm_;
  int pool_;
  int num_http_threads_;
  HttpHandler* http_handler_;
  Monitor* monitor_;
  TrustedIpsManager* trustedipsmanager_;
  ScopedSocket un_socket_;
  std::string un_socket_name_;

  const int max_num_sockets_;

  mutable Lock mu_;
  ConditionVariable cond_;
  int max_sockets_[NUM_SOCKET_TYPES] ABSL_GUARDED_BY(mu_);
  int num_sockets_[NUM_SOCKET_TYPES] ABSL_GUARDED_BY(mu_);
  int idle_counter_[NUM_SOCKET_TYPES] ABSL_GUARDED_BY(mu_);
  bool idle_counting_ ABSL_GUARDED_BY(mu_);
  std::vector<IdleClosure*> idle_closures_ ABSL_GUARDED_BY(mu_);
  RegisteredClosureID last_closure_id_ ABSL_GUARDED_BY(mu_);

#ifdef _WIN32
  std::unique_ptr<PipeHandler> pipe_handler_;
  std::unique_ptr<NamedPipeServer> pipe_server_;
#endif

  DISALLOW_COPY_AND_ASSIGN(ThreadpoolHttpServer);
};

}  // namespace devtools_goma
#endif  // DEVTOOLS_GOMA_CLIENT_THREADPOOL_HTTP_SERVER_H_
