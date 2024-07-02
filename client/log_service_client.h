// Copyright 2011 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_CLIENT_LOG_SERVICE_CLIENT_H_
#define DEVTOOLS_GOMA_CLIENT_LOG_SERVICE_CLIENT_H_

#include <string>

#include "absl/time/time.h"
#include "basictypes.h"
#include "lockhelper.h"
#include "simple_timer.h"
#include "worker_thread_manager.h"

namespace devtools_goma {

class ExecLog;
class HttpRPC;
class MemoryUsageLog;
class PermanentClosure;
class WorkerThreadManager;

class LogServiceClient {
 public:
  LogServiceClient(HttpRPC* http_rpc,
                   std::string save_log_path,
                   size_t max_log_in_req,
                   absl::Duration max_pending_duration,
                   WorkerThreadManager* wm);
  ~LogServiceClient();

  // Saves exec_log in goma backends.
  // Should be called on a WorkerThread.
  void SaveExecLog(const ExecLog& exec_log);

  void SaveMemoryUsageLog(const MemoryUsageLog& memory_usage_log);

  // Flushes pending logs.
  // Could be called on main thread (non WorkerThread).
  void Flush();

  // Waits for all active requests.
  // Could be called on main thread (non WorkerThread).
  void Wait();

 private:
  class SaveLogJob;
  friend class SaveLogJob;
  friend struct ExecLogSaveFunc;
  friend struct MemoryUsageLogSaveFunc;
  void CheckPending();
  void FinishSaveLogJob();
  template<typename SaveLogFunc>
  void SaveLogImpl(const SaveLogFunc& func);

  WorkerThreadManager* wm_;
  HttpRPC* http_rpc_;
  const std::string save_log_path_;
  const size_t max_log_in_req_;
  const absl::Duration max_pending_duration_;

  PeriodicClosureId periodic_callback_id_;

  mutable Lock mu_;
  // Condition to check num_save_log_job_ becomes 0.
  ConditionVariable cond_;
  // Current SaveLogJob accumulating logs.
  SaveLogJob* save_log_job_ ABSL_GUARDED_BY(mu_);
  // Number of SaveLogJobs sending to the server.
  int num_save_log_job_ ABSL_GUARDED_BY(mu_);
  SimpleTimer timer_;
  // Duration after start of |timer_| when Save*Log was called.
  absl::Duration last_timestamp_ ABSL_GUARDED_BY(mu_);

  DISALLOW_COPY_AND_ASSIGN(LogServiceClient);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_LOG_SERVICE_CLIENT_H_
