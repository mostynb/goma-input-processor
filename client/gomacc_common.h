// Copyright 2011 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_CLIENT_GOMACC_COMMON_H_
#define DEVTOOLS_GOMA_CLIENT_GOMACC_COMMON_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/time/time.h"
#include "basictypes.h"
#include "goma_ipc.h"
#include "scoped_fd.h"

namespace devtools_goma {

class CompilerFlags;
class ExecReq;
class ExecResp;

// Returns the port where http server is running.
// Returns -1 when compiler proxy is not ready.
// |status| will be modified if |status| is non-NULL.
int GetCompilerProxyPort(GomaIPC::Status* status);

bool StartCompilerProxy();

class GomaClient {
 public:
  enum Result {
    IPC_OK = 0,
    IPC_FAIL = -1,
    IPC_REJECTED = -2,
  };

  GomaClient(int pid,
             std::unique_ptr<CompilerFlags> flags,
             const char** envp,
             std::string local_compiler_path);
  ~GomaClient();
  void OutputResp();

  absl::Duration compiler_proxy_time() const;
  int retval() const;
  int id() const { return id_; }
  const IOChannel* chan() const { return ipc_chan_.get(); }

  // Call IPC Request. Return IPC_OK if successful.
  Result CallIPCAsync();

  // Wait an already dispatched IPC request to finish.  This needs to be
  // called after CallIPCAsync().
  Result WaitIPC();

  std::string CreateStdinFile();

  // Blocking version of IPC call which calls CallIPCAsync and WaitIPC
  // internally.
  Result CallIPC();

  // Sets overriding gomacc_path.
  // The caller's executable path will be used by default when it is not set.
  void set_gomacc_path(const std::string& path) { gomacc_path_ = path; }

  void set_cwd(const std::string& cwd) { cwd_ = cwd; }

  void set_local_compiler_path(const std::string& local_compiler_path) {
    local_compiler_path_ = local_compiler_path;
  }

 private:
  bool PrepareExecRequest(const CompilerFlags& flags, ExecReq* req);
  void OutputExecResp(ExecResp* resp);
#ifndef _WIN32
  void OutputProfInfo(const ExecResp& resp);
#endif

  GomaIPC goma_ipc_;
  std::unique_ptr<IOChannel> ipc_chan_;
  GomaIPC::Status status_;

  int id_;
  std::unique_ptr<CompilerFlags> flags_;
  std::string name_;
  std::vector<std::string> envs_;
#ifdef _WIN32
  std::vector<ScopedFd*> optional_files_;
  std::vector<std::pair<std::string, ScopedFd*>> rsp_files_;
#endif
  std::unique_ptr<ExecResp> exec_resp_;
  ScopedFd stdin_file_;
  std::string stdin_filename_;
  std::string gomacc_path_;
  std::string cwd_;
  std::string local_compiler_path_;

  DISALLOW_COPY_AND_ASSIGN(GomaClient);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_GOMACC_COMMON_H_
