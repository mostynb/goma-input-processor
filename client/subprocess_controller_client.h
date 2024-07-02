// Copyright 2011 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_CLIENT_SUBPROCESS_CONTROLLER_CLIENT_H_
#define DEVTOOLS_GOMA_CLIENT_SUBPROCESS_CONTROLLER_CLIENT_H_

#include <map>
#include <memory>
#include <string>

#include "basictypes.h"
#include "lockhelper.h"
#include "scoped_fd.h"
#include "subprocess_controller.h"
#include "worker_thread.h"
#include "worker_thread_manager.h"

namespace devtools_goma {

class SubProcessTask;

// SubPrcessControllerClient runs in multi-thread mode, and communicates
// with SubProcessControllerServer via fd.
// The communication runs in the thread where Setup() is called.
class SubProcessControllerClient: public SubProcessController {
 public:
  static bool IsRunning();
  static SubProcessControllerClient* Get();
  static void Initialize(WorkerThreadManager* wm, const std::string& tmp_dir);

  WorkerThreadManager* wm() const { return wm_; }

  void SetTmpDir(const std::string& tmp_dir) {
    AUTO_EXCLUSIVE_LOCK(lock, &tmp_dir_mu_);
    tmp_dir_ = tmp_dir;
  }
  // This does not return "const std::string&", otherwise lock is meaningless.
  std::string TmpDir() const {
    AUTO_SHARED_LOCK(lock, &tmp_dir_mu_);
    return tmp_dir_;
  }

  void SetInitialized();
  bool Initialized() const;

  // Quit stops serving new SubProcessTask, and kills running subprocesses.
  void Quit();
  // Shutdown cleanups SubProcessControllerClient.
  // Quit must be called before Shutdown.
  void Shutdown();

  void RegisterTask(SubProcessTask* task);

  // Sends request to server.
  void RequestRun(std::unique_ptr<SubProcessRun> run) override;
  void Kill(std::unique_ptr<SubProcessKill> kill) override;
  void SetOption(std::unique_ptr<SubProcessSetOption> option) override;

  int NumPending() const;

  bool BelongsToCurrentThread() const;

  std::string DebugString() const;

 private:
  friend class SubProcessController;

  // Takes ownership of fd.
  // pid is process id of subprocess controller server.
  static SubProcessControllerClient* Create(
      int fd, pid_t pid, const Options& options);

  SubProcessControllerClient(int fd, pid_t pid, Options options);
  ~SubProcessControllerClient() override;
  void Setup(WorkerThreadManager* wm, std::string tmp_dir);
  void Delete() ABSL_LOCKS_EXCLUDED(mu_);

  // Sends request to server.
  void Register(std::unique_ptr<SubProcessReq> req) override;

  // Handles server notification.
  void Started(std::unique_ptr<SubProcessStarted> started) override;
  void Terminated(std::unique_ptr<SubProcessTerminated> terminated) override;

  void SendRequest(SubProcessController::Op op,
                   std::unique_ptr<google::protobuf::Message> message);
  void DoWrite();
  void WriteDone();

  void DoRead();

  void OnClosed() ABSL_LOCKS_EXCLUDED(mu_);
  void RunCheckSignaled();
  void CheckSignaled();

  void MaybeClearReadableEventUnlocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  WorkerThreadManager* wm_;
  WorkerThread::ThreadId thread_id_;
  SocketDescriptor* socket_descriptor_;
  // Ownership is transferred to |socket_descriptor_| at Setup().
  ScopedSocket fd_;
  const pid_t server_pid_;

  mutable ReadWriteLock tmp_dir_mu_;
  std::string tmp_dir_ ABSL_GUARDED_BY(tmp_dir_mu_);

  mutable Lock mu_;
  ConditionVariable cond_;  // condition to wait for all subproc_tasks_ done.
  int next_id_ ABSL_GUARDED_BY(mu_);
  std::map<int, SubProcessTask*> subproc_tasks_ ABSL_GUARDED_BY(mu_);
  Options current_options_ ABSL_GUARDED_BY(mu_);
  PeriodicClosureId periodic_closure_id_ ABSL_GUARDED_BY(mu_);
  bool quit_ ABSL_GUARDED_BY(mu_);
  bool closed_ ABSL_GUARDED_BY(mu_);
  // We prefer to use ClearReadable() instead of StopReadale() to unregister
  // poller events from the IO multiplexing module (e.g. epoll). However, epoll
  // doesn't allow an IO event to be unregistered for more than once. This flag
  // ensures it's unregistered only once.
  //
  bool registered_readable_events_ ABSL_GUARDED_BY(mu_);

  mutable Lock initialized_mu_;
  bool initialized_ ABSL_GUARDED_BY(initialized_mu_);

  DISALLOW_COPY_AND_ASSIGN(SubProcessControllerClient);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_SUBPROCESS_CONTROLLER_CLIENT_H_
