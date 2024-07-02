// Copyright 2011 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "subprocess_controller_client.h"

#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#else
#include "config_win.h"
#include "socket_helper_win.h"
#endif

#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "absl/base/call_once.h"
#include "autolock_timer.h"
#include "callback.h"
#include "compiler_specific.h"
#include "socket_descriptor.h"
#include "glog/logging.h"
#include "glog/stl_logging.h"
MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "client/subprocess.pb.h"
MSVC_POP_WARNING()
#include "subprocess_task.h"
#include "worker_thread.h"

namespace devtools_goma {

namespace {

// g_mu is initialized in Create().
// SubProcessControllerClient is created in Create(), which is called
// from SubProcessController::Initialize. So, Create is called first.
//
// TODO: We cannot call IsRunning() or Get() unless Create()
// is called with this implementation.
Lock* g_mu;
SubProcessControllerClient* g_client_instance ABSL_GUARDED_BY(*g_mu);

absl::once_flag g_init_once;
void InitializeOnce() {
  g_mu = new Lock();
}

}  // anonymous namespace

/* static */
SubProcessControllerClient* SubProcessControllerClient::Create(
    int fd, pid_t pid, const Options& options) {
  // Must be called before starting threads.

  absl::call_once(g_init_once, InitializeOnce);

  AUTOLOCK(lock, g_mu);

  CHECK(g_client_instance == nullptr);
  g_client_instance = new SubProcessControllerClient(fd, pid, options);
  CHECK(g_client_instance != nullptr);
  return g_client_instance;
}

/* static */
bool SubProcessControllerClient::IsRunning() {
  AUTOLOCK(lock, g_mu);
  return g_client_instance != nullptr;
}

/* static */
SubProcessControllerClient* SubProcessControllerClient::Get() {
  AUTOLOCK(lock, g_mu);
  CHECK(g_client_instance != nullptr);
  return g_client_instance;
}

/* static */
void SubProcessControllerClient::Initialize(WorkerThreadManager* wm,
                                            const std::string& tmp_dir) {
  wm->NewThread(
      NewCallback(
          Get(), &SubProcessControllerClient::Setup,
          wm, tmp_dir), "subprocess_controller_client");
}

SubProcessControllerClient::SubProcessControllerClient(int fd,
                                                       pid_t pid,
                                                       Options options)
    : wm_(nullptr),
      thread_id_(0),
      socket_descriptor_(nullptr),
      fd_(fd),
      server_pid_(pid),
      next_id_(0),
      current_options_(std::move(options)),
      periodic_closure_id_(kInvalidPeriodicClosureId),
      quit_(false),
      closed_(false),
      registered_readable_events_(false),
      initialized_(false) {}

SubProcessControllerClient::~SubProcessControllerClient() {
  CHECK(quit_);
  CHECK(subproc_tasks_.empty());
  CHECK_EQ(periodic_closure_id_, kInvalidPeriodicClosureId);
  ScopedSocket fd(wm_->DeleteSocketDescriptor(socket_descriptor_));
  fd.Close();
  socket_descriptor_ = nullptr;
  thread_id_ = 0;
  wm_ = nullptr;
}

void SubProcessControllerClient::Setup(WorkerThreadManager* wm,
                                       std::string tmp_dir) {
  wm_ = wm;
  thread_id_ = wm_->GetCurrentThreadId();
  socket_descriptor_ =
      wm_->RegisterSocketDescriptor(std::move(fd_), WorkerThread::PRIORITY_MED);
  SetInitialized();
  {
    AUTOLOCK(lock, &mu_);
    socket_descriptor_->NotifyWhenReadable(
        NewPermanentCallback(this, &SubProcessControllerClient::DoRead));
    registered_readable_events_ = true;
  }
  SetTmpDir(tmp_dir);
  {
    AUTOLOCK(lock, &mu_);
    CHECK_EQ(periodic_closure_id_, kInvalidPeriodicClosureId);
    periodic_closure_id_ = wm_->RegisterPeriodicClosure(
        FROM_HERE, absl::Seconds(10), NewPermanentCallback(
            this, &SubProcessControllerClient::RunCheckSignaled));
  }
  LOG(INFO) << "SubProcessControllerClient Initialized"
            << " fd=" << socket_descriptor_->fd();
}

void SubProcessControllerClient::SetInitialized() {
  AUTOLOCK(lock, &initialized_mu_);
  initialized_ = true;
}

bool SubProcessControllerClient::Initialized() const {
  AUTOLOCK(lock, &initialized_mu_);
  return initialized_;
}

void SubProcessControllerClient::Quit() {
  LOG(INFO) << "SubProcessControllerClient Quit";

  std::vector<std::unique_ptr<SubProcessKill>> kills;
  {
    AUTOLOCK(lock, &mu_);
    quit_ = true;
    for (std::map<int, SubProcessTask*>::iterator iter = subproc_tasks_.begin();
         iter != subproc_tasks_.end();
         ++iter) {
      std::unique_ptr<SubProcessKill> kill(new SubProcessKill);
      kill->set_id(iter->first);
      kills.emplace_back(std::move(kill));
    }
  }
  for (size_t i = 0; i < kills.size(); ++i) {
    Kill(std::move(kills[i]));
  }
  wm_->RunClosureInThread(
      FROM_HERE,
      thread_id_,
      devtools_goma::NewCallback(
          this, &SubProcessControllerClient::SendRequest,
          SubProcessController::SHUTDOWN,
          std::unique_ptr<google::protobuf::Message>(
              absl::make_unique<SubProcessShutdown>())),
      WorkerThread::PRIORITY_MED);
  {
    AUTOLOCK(lock, &mu_);
    if (periodic_closure_id_ != kInvalidPeriodicClosureId) {
      wm_->UnregisterPeriodicClosure(periodic_closure_id_);
      periodic_closure_id_ = kInvalidPeriodicClosureId;
    }
  }
}

void SubProcessControllerClient::Shutdown() {
  LOG(INFO) << "SubProcessControllerClient shutdown";
  {
    AUTOLOCK(lock, &mu_);
    CHECK(quit_);
    CHECK_EQ(periodic_closure_id_, kInvalidPeriodicClosureId);
    while (!subproc_tasks_.empty() || !closed_) {
      LOG(INFO) << "wait for subproc_tasks_ become empty and peer closed";
      cond_.Wait(&mu_);
    }
  }
  // Not to pass SubProcessControllerClient::SendRequest to send Kill,
  // this should be executed with PRIORITY_MED.
  wm_->RunClosureInThread(
      FROM_HERE,
      thread_id_,
      NewCallback(
          this, &SubProcessControllerClient::Delete),
      WorkerThread::PRIORITY_MED);
}

void SubProcessControllerClient::RegisterTask(SubProcessTask* task) {
  CHECK_EQ(-1, task->req().id()) << task->req().DebugString();
  CHECK_EQ(SubProcessState::PENDING, task->state())
      << task->req().DebugString();
  int id = 0;
  bool quit = false;
  {
    AUTOLOCK(lock, &mu_);
    if (quit_) {
      quit = true;
      // don't put in subproc_tasks_.
    } else {
      id = ++next_id_;
      // detach task would not notify back, so no need to set it
      // in subproc_tasks_.
      if (!task->req().detach()) {
        subproc_tasks_.insert(std::make_pair(id, task));
      }
    }
  }
  if (quit) {
    LOG(INFO) << task->req().trace_id() << ": RegisterTask in quit";
    std::unique_ptr<SubProcessTerminated> terminated(new SubProcessTerminated);
    terminated->set_id(id);
    terminated->set_status(SubProcessTerminated::kNotStarted);
    wm_->RunClosureInThread(
        FROM_HERE,
        thread_id_,
        devtools_goma::NewCallback(
            task, &SubProcessTask::Terminated, std::move(terminated)),
        WorkerThread::PRIORITY_MED);
    return;
  }
  VLOG(1) << task->req().trace_id() << ": RegisterTask id=" << id;
  task->mutable_req()->set_id(id);
  std::unique_ptr<SubProcessReq> req(new SubProcessReq);
  *req = task->req();
  Register(std::move(req));
}

void SubProcessControllerClient::Register(std::unique_ptr<SubProcessReq> req) {
  {
    AUTOLOCK(lock, &mu_);
    if (quit_)
      return;
  }
  VLOG(1) << "Register id=" << req->id() << " " << req->trace_id();
  wm_->RunClosureInThread(
      FROM_HERE,
      thread_id_,
      devtools_goma::NewCallback(
          this, &SubProcessControllerClient::SendRequest,
          SubProcessController::REGISTER,
          std::unique_ptr<google::protobuf::Message>(std::move(req))),
      WorkerThread::PRIORITY_MED);
}

void SubProcessControllerClient::RequestRun(
    std::unique_ptr<SubProcessRun> run) {
  VLOG(1) << "Run id=" << run->id();
  {
    AUTOLOCK(lock, &mu_);
    if (quit_)
      return;
  }
  wm_->RunClosureInThread(
      FROM_HERE,
      thread_id_,
      devtools_goma::NewCallback(
          this, &SubProcessControllerClient::SendRequest,
          SubProcessController::REQUEST_RUN,
          std::unique_ptr<google::protobuf::Message>(std::move(run))),
      WorkerThread::PRIORITY_MED);
}

void SubProcessControllerClient::Kill(std::unique_ptr<SubProcessKill> kill) {
  {
    AUTOLOCK(lock, &mu_);
    if (periodic_closure_id_ == kInvalidPeriodicClosureId) {
      return;
    }
  }
  LOG(INFO) << "Kill id=" << kill->id();
  wm_->RunClosureInThread(
      FROM_HERE,
      thread_id_,
      devtools_goma::NewCallback(
          this, &SubProcessControllerClient::SendRequest,
          SubProcessController::KILL,
          std::unique_ptr<google::protobuf::Message>(std::move(kill))),
      WorkerThread::PRIORITY_MED);
}

void SubProcessControllerClient::SetOption(
    std::unique_ptr<SubProcessSetOption> option) {
  {
    AUTOLOCK(lock, &mu_);
    if (periodic_closure_id_ == kInvalidPeriodicClosureId) {
      return;
    }

    current_options_.max_subprocs = option->max_subprocs();
    current_options_.max_subprocs_low_priority =
        option->max_subprocs_low_priority();
    current_options_.max_subprocs_heavy_weight =
        option->max_subprocs_heavy_weight();
  }
  LOG(INFO) << "SetOption"
            << " max_subprocs=" << option->max_subprocs()
            << " max_subprocs_heavy_weight="
            << option->max_subprocs_heavy_weight()
            << " max_subprocs_low_priority="
            << option->max_subprocs_low_priority();
  wm_->RunClosureInThread(
      FROM_HERE,
      thread_id_,
      devtools_goma::NewCallback(
          this, &SubProcessControllerClient::SendRequest,
          SubProcessController::SET_OPTION,
          std::unique_ptr<google::protobuf::Message>(std::move(option))),
      WorkerThread::PRIORITY_MED);
}

void SubProcessControllerClient::Started(
    std::unique_ptr<SubProcessStarted> started) {
  VLOG(1) << "Started " << started->id() << " pid=" << started->pid();
  DCHECK(BelongsToCurrentThread());
  int id = started->id();
  SubProcessTask* task = nullptr;
  {
    AUTOLOCK(lock, &mu_);
    std::map<int, SubProcessTask*>::iterator found =
        subproc_tasks_.find(id);
    if (found != subproc_tasks_.end()) {
      task = found->second;
    }
  }
  if (task == nullptr) {
    LOG(WARNING) << "No task for id=" << id;
    std::unique_ptr<SubProcessKill> kill(new SubProcessKill);
    kill->set_id(id);
    Kill(std::move(kill));
    return;
  }
  task->Started(std::move(started));
}

void SubProcessControllerClient::Terminated(
    std::unique_ptr<SubProcessTerminated> terminated) {
  DCHECK(BelongsToCurrentThread());
  VLOG(1) << "Terminated " << terminated->id()
          << " status=" << terminated->status();
  int id = terminated->id();
  SubProcessTask* task = nullptr;
  {
    AUTOLOCK(lock, &mu_);
    std::map<int, SubProcessTask*>::iterator found =
        subproc_tasks_.find(id);
    if (found != subproc_tasks_.end()) {
      task = found->second;
      subproc_tasks_.erase(found);
    }
  }
  if (task != nullptr) {
    bool async = task->async_callback();
    task->Terminated(std::move(terminated));
    // If task is synchronous (!async), task may already be deleted here.
    if (async) {
      wm_->RunClosureInThread(
          FROM_HERE,
          task->thread_id(),
          NewCallback(
              task, &SubProcessTask::Done),
          WorkerThread::PRIORITY_MED);
    }
  } else {
    std::ostringstream ss;
    ss << "no task found for id=" << id
       << " status=" << terminated->status()
       << " error=" << SubProcessTerminated_ErrorTerminate_Name(
           terminated->error());
    if (terminated->error() == SubProcessTerminated::kFailedToLookup) {
      LOG(INFO) << ss.str();
    } else {
      LOG(WARNING) << ss.str();
    }
  }

  {
    AUTOLOCK(lock, &mu_);
    if (quit_ && subproc_tasks_.empty()) {
      LOG(INFO) << "all subproc_tasks done";
      CHECK(subproc_tasks_.empty());
      cond_.Signal();
    }
  }
}

int SubProcessControllerClient::NumPending() const {
  AUTOLOCK(lock, &mu_);
  int num_pending = 0;
  for (std::map<int, SubProcessTask*>::const_iterator iter =
           subproc_tasks_.begin();
       iter != subproc_tasks_.end();
       ++iter) {
    SubProcessTask* task = iter->second;
    switch (task->state()) {
      case SubProcessState::SETUP: case SubProcessState::PENDING:
        ++num_pending;
        break;
      default:
        { }
    }
  }
  return num_pending;
}

bool SubProcessControllerClient::BelongsToCurrentThread() const {
  return THREAD_ID_IS_SELF(thread_id_);
}

void SubProcessControllerClient::Delete() {
  DCHECK(BelongsToCurrentThread());
  {
    AUTOLOCK(lock, &mu_);
    MaybeClearReadableEventUnlocked();
  }

  // Maybe not good to accessing g_client_instance which is being
  // deleted. So, guard `delete this`, too.
#ifndef _WIN32
  pid_t server_pid = server_pid_;
#endif
  {
    AUTOLOCK(lock, g_mu);
    delete this;
    g_client_instance = nullptr;
  }
#ifndef _WIN32
  int status = 0;
  if (waitpid(server_pid, &status, 0) == -1) {
    PLOG(ERROR) << "SubProcessControllerServer wait failed pid="
                << server_pid;
    return;
  }
  int exit_status = -1;
  if (WIFEXITED(status)) {
    exit_status = WEXITSTATUS(status);
  }
  int signaled = 0;
  if (WIFSIGNALED(status)) {
    signaled = WTERMSIG(status);
  }
  LOG(INFO) << "SubProcessControllerServer exited"
            << " status=" << exit_status
            << " signal=" << signaled;
  if (exit_status != 0 && signaled != 0) {
    LOG(ERROR) << "unexpected SubProcessController exit";
  }
#endif
}

void SubProcessControllerClient::SendRequest(
    SubProcessController::Op op,
    std::unique_ptr<google::protobuf::Message> message) {
  DCHECK(BelongsToCurrentThread());
  if (AddMessage(op, *message)) {
    VLOG(3) << "SendRequest has pending write";
    socket_descriptor_->NotifyWhenWritable(
        NewPermanentCallback(this, &SubProcessControllerClient::DoWrite));
  }
}

void SubProcessControllerClient::DoWrite() {
  VLOG(2) << "DoWrite";
  DCHECK(BelongsToCurrentThread());
  if (!WriteMessage(socket_descriptor_->wrapper())) {
    LOG(FATAL) << "Unexpected peer shutdown in WriteMessage";
  }
  if (!has_pending_write()) {
    VLOG(3) << "DoWrite no pending";
    wm_->RunClosureInThread(
        FROM_HERE,
        thread_id_,
        NewCallback(
            this, &SubProcessControllerClient::WriteDone),
        WorkerThread::PRIORITY_IMMEDIATE);
  }
}

void SubProcessControllerClient::WriteDone() {
  VLOG(2) << "WriteDone";
  DCHECK(BelongsToCurrentThread());
  if (has_pending_write())
    return;
  socket_descriptor_->ClearWritable();
}

void SubProcessControllerClient::DoRead() {
  VLOG(2) << "DoRead";
  DCHECK(BelongsToCurrentThread());
  int op = 0;
  int len = 0;
  if (!ReadMessage(socket_descriptor_->wrapper(), &op, &len)) {
    VLOG(2) << "pending read op=" << op << " len=" << len;
    return;
  }
  VLOG(2) << "DoRead op=" << op << " len=" << len;
  switch (op) {
    case SubProcessController::CLOSED:
      {
        AUTOLOCK(lock, &mu_);
        if (quit_) {
          VLOG(1) << "peer shutdown in quit";
          CHECK(subproc_tasks_.empty())
              << "SubProcessControllerServer closed but subproc_tasks exist:"
              << subproc_tasks_.size();
          wm_->RunClosureInThread(
              FROM_HERE,
              thread_id_,
              devtools_goma::NewCallback(
                  this, &SubProcessControllerClient::OnClosed),
              WorkerThread::PRIORITY_MED);
          break;
        }
      }
      LOG(FATAL) << "Unexpected peer shutdown in ReadMessage";

    // Note: STARTED and TERMINATED should run closure with the same priority
    // Otherwise, they may not be executed in order.
    case SubProcessController::STARTED: {
        std::unique_ptr<SubProcessStarted> started(new SubProcessStarted);
        if (started->ParseFromArray(payload_data(), len)) {
          wm_->RunClosureInThread(
              FROM_HERE,
              thread_id_,
              devtools_goma::NewCallback(
                  this, &SubProcessControllerClient::Started,
                  std::move(started)),
              WorkerThread::PRIORITY_MED);
        } else {
          LOG(ERROR) << "broken SubProcessStarted";
        }
      }
      break;

    case SubProcessController::TERMINATED: {
        std::unique_ptr<SubProcessTerminated> terminated(
            new SubProcessTerminated);
        if (terminated->ParseFromArray(payload_data(), len)) {
          wm_->RunClosureInThread(
              FROM_HERE,
              thread_id_,
              devtools_goma::NewCallback(
                  this, &SubProcessControllerClient::Terminated,
                  std::move(terminated)),
              WorkerThread::PRIORITY_MED);
        } else {
          LOG(ERROR) << "broken SubProcessTerminated";
        }
      }
      break;

    default:
      LOG(FATAL) << "Unknown SubProcessController::Op " << op;
  }
  ReadDone();
  return;
}

void SubProcessControllerClient::OnClosed() {
  AUTOLOCK(lock, &mu_);
  if (closed_) {
    return;
  }
  LOG(INFO) << "peer closed";
  CHECK(subproc_tasks_.empty());
  closed_ = true;
  MaybeClearReadableEventUnlocked();
  socket_descriptor_->ClearWritable();
  cond_.Signal();
}

void SubProcessControllerClient::RunCheckSignaled() {
  if (!IsRunning()) {
    // RunCheckSignaled is periodic closure managed by g_client_instance
    // it should never be called when not running.
    LOG(FATAL) << "SubProcessControllerClient is not running";
    return;
  }
  // Switch from alarm worker to client thread.
  wm_->RunClosureInThread(
      FROM_HERE,
      thread_id_,
      NewCallback(
          this, &SubProcessControllerClient::CheckSignaled),
      WorkerThread::PRIORITY_MED);
}

void SubProcessControllerClient::CheckSignaled() {
  if (!IsRunning()) {
    // g_client_instnace (and this pointer) may be nullptr because Delete is
    // higher priority (put in WorkerThreadManager in Shutdown).
    // Should not access any member fields here.
    return;
  }
  DCHECK(BelongsToCurrentThread());
  std::vector<std::unique_ptr<SubProcessKill>> kills;
  {
    AUTOLOCK(lock, &mu_);
    for (std::map<int, SubProcessTask*>::const_iterator iter =
             subproc_tasks_.begin();
         iter != subproc_tasks_.end();
         ++iter) {
      int id = iter->first;
      SubProcessTask* task = iter->second;
      if (task->state() == SubProcessState::SIGNALED) {
        std::unique_ptr<SubProcessKill> kill(new SubProcessKill);
        kill->set_id(id);
        kills.emplace_back(std::move(kill));
      }
    }
  }
  if (!kills.empty()) {
    for (size_t i = 0; i < kills.size(); ++i) {
      Kill(std::move(kills[i]));
    }
  }
}

std::string SubProcessControllerClient::DebugString() const {
  AUTOLOCK(lock, &mu_);
  std::ostringstream ss;

  ss << "options: " << current_options_.DebugString() << '\n';

  for (std::map<int, SubProcessTask*>::const_iterator iter =
           subproc_tasks_.begin();
       iter != subproc_tasks_.end();
       ++iter) {
    int id = iter->first;
    SubProcessTask* task = iter->second;
    ss << id << " "
       << task->req().trace_id() << " "
       << SubProcessState::State_Name(task->state()) << " "
       << SubProcessReq::Priority_Name(task->req().priority()) << " "
       << SubProcessReq::Weight_Name(task->req().weight()) << " "
       << "pid=" << task->started().pid() << "\n";
  }
  return ss.str();
}

void SubProcessControllerClient::MaybeClearReadableEventUnlocked() {
  if (registered_readable_events_) {
    socket_descriptor_->ClearReadable();
    registered_readable_events_ = false;
  }
}

}  // namespace devtools_goma
