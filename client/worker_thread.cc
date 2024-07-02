// Copyright 2012 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "worker_thread.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include "absl/memory/memory.h"
#include "absl/time/clock.h"
#include "autolock_timer.h"
#include "callback.h"
#include "compiler_specific.h"
#include "descriptor_poller.h"
#include "glog/logging.h"
#include "ioutil.h"
#include "socket_descriptor.h"
#include "worker_thread_manager.h"

#ifdef _WIN32
# include "socket_helper_win.h"
#endif

#define LOG_EVERY_SEC_CONCAT(base, line) base##line
#define LOG_EVERY_SEC_VARNAME(base, line) LOG_EVERY_SEC_CONCAT(base, line)

#define LOG_EVERY_SEC_NEXT_LOG_TIME \
  LOG_EVERY_SEC_VARNAME(last_log_time_, __LINE__)
#define LOG_EVERY_SEC_NEXT_LOG_TIME_MU \
  LOG_EVERY_SEC_VARNAME(last_log_time_mu_, __LINE__)

// Used for LOG at most once per second.
// This will be useful to log inside high frequency loop.
#define LOG_EVERY_SEC(severity)                              \
  static absl::Time LOG_EVERY_SEC_NEXT_LOG_TIME;             \
  static devtools_goma::Lock LOG_EVERY_SEC_NEXT_LOG_TIME_MU; \
  if (update_log_time(&LOG_EVERY_SEC_NEXT_LOG_TIME,          \
                      &LOG_EVERY_SEC_NEXT_LOG_TIME_MU))      \
  LOG(severity)

namespace devtools_goma {

namespace {

constexpr absl::Duration kDefaultPollInterval = absl::Milliseconds(500);
constexpr int kMaxNumConsecutiveIdledLoops = 5000;

// This function returns true at most one per second,
// used to decide whether LOG_EVERY_SEC logs or not.
bool update_log_time(absl::Time* t, devtools_goma::Lock* mu) {
  AUTOLOCK(lock, mu);
  const auto now = absl::Now();
  if (*t > now)
    return false;
  *t = now + absl::Seconds(1);
  return true;
}

}  // namespace

WorkerThread::CancelableClosure::CancelableClosure(const char* const location,
                                                   Closure* closure)
    : closure_(closure), location_(location) {}

WorkerThread::CancelableClosure::~CancelableClosure() {
  CHECK(closure_ == nullptr);
}

void WorkerThread::CancelableClosure::Cancel() {
  delete closure_;
  closure_ = nullptr;
}

const char* WorkerThread::CancelableClosure::location() const {
  return location_;
}

WorkerThread::ClosureData::ClosureData(
    const char* const location,
    Closure* closure,
    int queuelen,
    int tick,
    Timestamp timestamp)
    : location_(location),
      closure_(closure),
      queuelen_(queuelen),
      tick_(tick),
      timestamp_(timestamp) {
}

void WorkerThread::DelayedClosureImpl::Run() {
    Closure* closure = GetClosure();
    if (closure != nullptr) {
      VLOG(3) << "delayed=" << closure;
      closure->Run();
    } else {
      VLOG(1) << "closure " << location() << " has been cancelled";
    }
    // Delete delayed_closure after closure runs.
    delete this;
}

class WorkerThread::PeriodicClosure {
 public:
  PeriodicClosure(PeriodicClosureId id, const char* const location,
                  Timestamp time_now, absl::Duration period,
                  std::unique_ptr<PermanentClosure> closure)
      : id_(id),
        location_(location),
        last_time_(time_now),
        period_(period),
        closure_(std::move(closure)) {
  }

  PeriodicClosureId id() const { return id_; }
  const char* location() const { return location_; }

  PermanentClosure* GetClosure(Timestamp time_now) {
    CHECK_GE(time_now, last_time_);
    if (time_now >= last_time_ + period_) {
      last_time_ = time_now;
      return closure_.get();
    }
    return nullptr;
  }

  PermanentClosure* closure() const { return closure_.get(); }
  std::unique_ptr<PermanentClosure> ReleaseClosure() {
    return std::move(closure_);
  }

 private:
  const PeriodicClosureId id_;
  const char* const location_;
  Timestamp last_time_;
  const absl::Duration period_;
  std::unique_ptr<PermanentClosure> closure_;
  DISALLOW_COPY_AND_ASSIGN(PeriodicClosure);
};

WorkerThread::ThreadSafeThreadId::ThreadSafeThreadId()
    : id_(kInvalidThreadId) {}

void WorkerThread::ThreadSafeThreadId::Initialize(ThreadId id) {
  CHECK_NE(id, kInvalidThreadId);
  AUTO_EXCLUSIVE_LOCK(lock, &mu_);
  id_ = id;
  init_n_.Notify();
}

void WorkerThread::ThreadSafeThreadId::WaitUntilInitialized() {
  init_n_.WaitForNotification();
  CHECK_NE(get(), kInvalidThreadId);
}

WorkerThread::ThreadId WorkerThread::ThreadSafeThreadId::get() const {
  AUTO_SHARED_LOCK(lock, &mu_);
  return id_;
}

void WorkerThread::ThreadSafeThreadId::Reset() {
  AUTO_EXCLUSIVE_LOCK(lock, &mu_);
  id_ = kInvalidThreadId;
}

WorkerThread::IdleLoopsThrottler::IdleLoopsThrottler(std::string name, Lock* mu)
    : name_(std::move(name)), mu_(mu), num_idle_(0) {}

void WorkerThread::IdleLoopsThrottler::OnBusy() {
  num_idle_ = 0;
}

void WorkerThread::IdleLoopsThrottler::OnIdle() {
  if (++num_idle_ == kMaxNumConsecutiveIdledLoops) {
    num_idle_ = 0;

    mu_->Release();
    absl::SleepFor(kDefaultPollInterval);
    LOG_EVERY_N(WARNING, 1000) << "Worker thread name_=" << name_
                               << " is throttled due to a long period of idle "
                                  "looping, consider restarting compiler_proxy";
    mu_->Acquire();
  }
}

WorkerThread::IdleLoopsThrottler::Raii::Raii(IdleLoopsThrottler* throttler)
    : throttler_(throttler), idle_(false) {}

WorkerThread::IdleLoopsThrottler::Raii::~Raii() {
  if (idle_) {
    throttler_->OnIdle();
  } else {
    throttler_->OnBusy();
  }
}

WorkerThread::WorkerThread(int pool, std::string name)
    : name_(std::move(name)),
      pool_(pool),
      handle_(kNullThreadHandle),
      tick_(0),
      shutting_down_(false),
      quit_(false),
      auto_lock_stat_next_closure_(nullptr),
      auto_lock_stat_poll_events_(nullptr),
      idle_loops_throttler_(name_, &mu_) {
  VLOG(2) << "WorkerThread " << name_;
  int pipe_fd[2];
#ifndef _WIN32
  PCHECK(pipe(pipe_fd) == 0);
#else
  CHECK_EQ(async_socketpair(pipe_fd), 0);
#endif
  ScopedSocket pr(pipe_fd[0]);
  PCHECK(pr.SetCloseOnExec());
  PCHECK(pr.SetNonBlocking());
  ScopedSocket pw(pipe_fd[1]);
  PCHECK(pw.SetCloseOnExec());
  PCHECK(pw.SetNonBlocking());
  // poller takes ownership of both pipe fds.
  poller_ = DescriptorPoller::NewDescriptorPoller(
      absl::make_unique<SocketDescriptor>(std::move(pr), PRIORITY_HIGH, this),
      std::move(pw));
  timer_.Start();
  if (g_auto_lock_stats) {
    // TODO: Split stats per pool.
    auto_lock_stat_next_closure_ = g_auto_lock_stats->NewStat(
        "worker_thread::NextClosure");

    auto_lock_stat_poll_events_ = g_auto_lock_stats->NewStat(
        "descriptor_poller::PollEvents");
  }
  for (int priority = PRIORITY_MIN; priority < NUM_PRIORITIES; ++priority) {
    max_queuelen_[priority] = 0;
    max_wait_time_[priority] = absl::ZeroDuration();
  }
}

WorkerThread::~WorkerThread() {
  VLOG(2) << "~WorkerThread " << name_;
  CHECK_EQ(kNullThreadHandle, handle_);
  CHECK_EQ(id(), kInvalidThreadId);
}

/* static */
void WorkerThread::Initialize() {
  absl::call_once(key_worker_once_,
                  &WorkerThread::InitializeWorkerKey);
}

/* static */
WorkerThread* WorkerThread::GetCurrentWorker() {
#ifndef _WIN32
  return static_cast<WorkerThread*>(pthread_getspecific(key_worker_));
#else
  return static_cast<WorkerThread*>(TlsGetValue(key_worker_));
#endif
}

WorkerThread::Timestamp WorkerThread::NowCached() {
  Timestamp result;
  const auto now_opt = now_cached_.get();
  if (now_opt) {
    result = *now_opt;
  } else {
    result = timer_.GetDuration();
    now_cached_.set(result);
  }
  return result;
}

void WorkerThread::Shutdown() {
  VLOG(2) << "Shutdown " << name_;
  AUTOLOCK(lock, &mu_);
  shutting_down_ = true;
}

void WorkerThread::Quit() {
  VLOG(2) << "Quit " << name_;
  AUTOLOCK(lock, &mu_);
  shutting_down_ = true;
  quit_ = true;
  poller_->Signal();
}

void WorkerThread::ThreadMain() {
#ifndef _WIN32
  pthread_setspecific(key_worker_, this);
#else
  TlsSetValue(key_worker_, this);
#endif
  PlatformThread::SetName(handle_, name_);
  {
    const ThreadId id = GetCurrentThreadId();
    VLOG(1) << "Start thread:" << id << " " << name_;
    id_.Initialize(id);
  }
  while (Dispatch()) { }
  LOG(INFO) << id() << " Dispatch loop finished " << name_;
  {
    AUTOLOCK(lock, &mu_);
    for (int priority = PRIORITY_MIN; priority < NUM_PRIORITIES; ++priority) {
      CHECK(pendings_[priority].empty());
    }
    CHECK(descriptors_.empty());
    CHECK(periodic_closures_.empty());
    CHECK(quit_);
  }
}

bool WorkerThread::Dispatch() {
  VLOG(2) << "Dispatch " << name_;
  now_cached_.set(absl::nullopt);
  // These variables are defined so that we don't have to use them under |mu_|.
  ClosureData closure_data_copy;
  {
    AUTOLOCK_WITH_STAT(lock, &mu_, auto_lock_stat_next_closure_);
    if (!NextClosure()) {
      VLOG(2) << "Dispatch end " << name_;
      return false;
    }
    if (!current_closure_data_)
      return true;
    closure_data_copy = current_closure_data_.value();
  }
  VLOG(2) << "Loop closure=" << closure_data_copy.closure_ << " " << name_;
  const Timestamp start = timer_.GetDuration();
  closure_data_copy.closure_->Run();
  LOG_EVERY_SEC(INFO) << "dispatched closure location:"
                      << closure_data_copy.location_;

  absl::Duration duration = timer_.GetDuration() - start;
  if (duration > absl::Minutes(1)) {
    LOG(WARNING) << id() << " closure run too long: " << duration << " "
                 << closure_data_copy.location_ << " "
                 << closure_data_copy.closure_;
  }
  return true;
}

absl::once_flag WorkerThread::key_worker_once_;

#ifndef _WIN32
pthread_key_t WorkerThread::key_worker_;
#else
DWORD WorkerThread::key_worker_ = TLS_OUT_OF_INDEXES;
#endif

SocketDescriptor* WorkerThread::RegisterSocketDescriptor(ScopedSocket&& fd,
                                                         Priority priority) {
  VLOG(2) << "RegisterSocketDescriptor " << name_;
  AUTOLOCK(lock, &mu_);
  DCHECK_LT(priority, PRIORITY_IMMEDIATE);
  auto d = absl::make_unique<SocketDescriptor>(std::move(fd), priority, this);
  auto* d_ptr = d.get();
  CHECK(descriptors_.emplace(d_ptr->fd(), std::move(d)).second);
  return d_ptr;
}

ScopedSocket WorkerThread::DeleteSocketDescriptor(
    SocketDescriptor* d) {
  VLOG(2) << "DeleteSocketDescriptor " << name_;
  AUTOLOCK(lock, &mu_);
  poller_->UnregisterDescriptor(d);
  ScopedSocket fd(d->ReleaseFd());
  if (fd.valid()) {
    descriptors_.erase(fd.get());
  }
  return fd;
}

void WorkerThread::RegisterPeriodicClosure(
    PeriodicClosureId id, const char* const location,
    absl::Duration period, std::unique_ptr<PermanentClosure> closure) {
  VLOG(2) << "RegisterPeriodicClosure " << name_;
  AUTOLOCK(lock, &mu_);
  periodic_closures_.emplace_back(
     new PeriodicClosure(id, location, NowCached(), period,
                         std::move(closure)));
}

void WorkerThread::UnregisterPeriodicClosure(
    PeriodicClosureId id, UnregisteredClosureData* data) {
  VLOG(2) << "UnregisterPeriodicClosure " << name_;
  DCHECK(data);
  AUTOLOCK(lock, &mu_);
  CHECK_NE(id, kInvalidPeriodicClosureId);

  {
    std::unique_ptr<PermanentClosure> closure;

    auto it = std::find_if(periodic_closures_.begin(), periodic_closures_.end(),
                           [id](const std::unique_ptr<PeriodicClosure>& it) {
                             return it->id() == id;
                           });
    if (it != periodic_closures_.end()) {
      closure = (*it)->ReleaseClosure();
      // Since location is used when this function
      // takes long time, this should be set when it's available.
      data->SetLocation((*it)->location());
      periodic_closures_.erase(it);
    }

    DCHECK(closure) << "Removing unregistered closure id=" << id;

    std::deque<ClosureData> pendings;
    while (!pendings_[PRIORITY_IMMEDIATE].empty()) {
      ClosureData pending_closure =
        pendings_[PRIORITY_IMMEDIATE].front();
      pendings_[PRIORITY_IMMEDIATE].pop_front();
      if (pending_closure.closure_ == closure.get())
        continue;
      pendings.push_back(pending_closure);
    }
    pendings_[PRIORITY_IMMEDIATE].swap(pendings);
  }

  // Notify that |closure| is removed from the queues.
  // SetDone(true) after |closure| has been deleted.
  data->SetDone(true);
}

void WorkerThread::RunClosure(const char* const location, Closure* closure,
                              Priority priority) {
  VLOG(2) << "RunClosure " << name_;
  DCHECK_GE(priority, PRIORITY_MIN);
  DCHECK_LT(priority, NUM_PRIORITIES);
  {
    AUTOLOCK(lock, &mu_);
    AddClosure(location, priority, closure);
    // If this is the same thread, or this worker is running some closure
    // (or in other words, this worker is not in select wait),
    // next Dispatch could pick a closure from pendings_, so we don't need
    // to signal via pipe.
    if (THREAD_ID_IS_SELF(id()) || current_closure_data_)
      return;
  }
  // send select loop something to read about, so new pendings will be
  // processed soon.
  poller_->Signal();
}

WorkerThread::CancelableClosure* WorkerThread::RunDelayedClosure(
    const char* const location,
    absl::Duration delay, Closure* closure) {
  VLOG(2) << "RunDelayedClosure " << name_;
  AUTOLOCK(lock, &mu_);
  DelayedClosureImpl* delayed_closure =
      new DelayedClosureImpl(location, NowCached() + delay, closure);
  delayed_pendings_.push(delayed_closure);
  return delayed_closure;
}

size_t WorkerThread::load() const {
  AUTOLOCK(lock, &mu_);
  size_t n = 0;
  if (current_closure_data_) {
    n += 1;
  }
  n += descriptors_.size();
  for (int priority = PRIORITY_MIN; priority < NUM_PRIORITIES; ++priority) {
    int w = 1 << priority;
    n += pendings_[priority].size() * w;
  }
  return n;
}

size_t WorkerThread::pendings() const {
  AUTOLOCK(lock, &mu_);
  size_t n = 0;
  for (int priority = PRIORITY_MIN; priority < NUM_PRIORITIES; ++priority) {
    n += pendings_[priority].size();
  }
  return n;
}

bool WorkerThread::IsIdle() const {
  AUTOLOCK(lock, &mu_);
  return !current_closure_data_ && descriptors_.size() == 0;
}

std::string WorkerThread::DebugString() const {
  AUTOLOCK(lock, &mu_);
  std::ostringstream s;
  s << "thread[" << id() << "/" << name_ << "] ";
  s << " tick=" << tick_;
  if (current_closure_data_) {
    s << " " << current_closure_data_->location_;
    s << " " << current_closure_data_->closure_;
  }
  s << ": " << descriptors_.size() << " descriptors";
  s << ": poll_interval=" << poll_interval_;
  s << ": ";
  for (int priority = PRIORITY_MIN; priority < NUM_PRIORITIES; ++priority) {
    s << Priority_Name(static_cast<Priority>(priority))
      << "[" << pendings_[priority].size() << " pendings "
      << " q=" << max_queuelen_[priority]
      << " w=" << max_wait_time_[priority]
      << "] ";
  }
  s << ": delayed=" << delayed_pendings_.size();
  s << ": periodic=" << periodic_closures_.size();
  const auto current_pool = pool();
  if (current_pool != 0)
    s << ": pool=" << current_pool;
  return s.str();
}

/* static */
std::string WorkerThread::Priority_Name(Priority priority) {
  switch (priority) {
    case PRIORITY_LOW: return "PriLow";
    case PRIORITY_MED: return "PriMed";
    case PRIORITY_HIGH: return "PriHigh";
    case PRIORITY_IMMEDIATE: return "PriImmediate";
    default:
      break;
  }
  std::ostringstream ss;
  ss << "PriUnknown[" << priority << "]";
  return ss.str();
}

bool WorkerThread::NextClosure() {
  VLOG(5) << "NextClosure " << name_;
  DCHECK(!now_cached_.get());  // NowCached() will get new time
  ++tick_;
  current_closure_data_.reset();
  IdleLoopsThrottler::Raii throttler_raii(&idle_loops_throttler_);

  // If there are pending closures, it will check descriptors without timeout.
  // If there are deplayed closures, it will reduce intervals to the nearest
  // delayed closure.
  poll_interval_ = kDefaultPollInterval;

  int priority = PRIORITY_IMMEDIATE;
  for (priority = PRIORITY_IMMEDIATE; priority >= PRIORITY_MIN; --priority) {
    if (!pendings_[priority].empty()) {
      // PRIORITY_IMMEDIATE has higher priority than descriptors.
      if (priority == PRIORITY_IMMEDIATE) {
        current_closure_data_ = GetClosure(static_cast<Priority>(priority));
        return true;
      }
      // For lower priorities, descriptor availability is checked before
      // running the closures.
      poll_interval_ = absl::ZeroDuration();
      break;
    }
  }

  if (poll_interval_ > absl::ZeroDuration() && !delayed_pendings_.empty()) {
    // Adjust poll_interval for delayed closure.
    absl::Duration next_delay = delayed_pendings_.top()->time() - NowCached();
    if (next_delay < absl::ZeroDuration())
      next_delay = absl::ZeroDuration();
    poll_interval_ = std::min(poll_interval_, next_delay);
  }
  DescriptorPoller::CallbackQueue io_pendings;
  VLOG(2) << "poll_interval=" << poll_interval_;
  CHECK_GE(poll_interval_, absl::ZeroDuration());

  const Timestamp poll_start_time = timer_.GetDuration();
  poller_->PollEvents(descriptors_, poll_interval_, priority, &io_pendings,
                      &mu_, &auto_lock_stat_poll_events_);
  // Updated cached time value.
  now_cached_.set(timer_.GetDuration());
  // on Windows, poll time would be 0.51481 or so when no event happened.
  // multiply 1.1 (i.e. 0.55) would be good.
  if (NowCached() - poll_start_time > 1.1 * kDefaultPollInterval) {
    LOG(WARNING) << id() << " poll too slow:" << (NowCached() - poll_start_time)
                 << " nsec"
                 << " interval=" << poll_interval_ << " msec"
                 << " #descriptors=" << descriptors_.size()
                 << " priority=" << priority;
    if (NowCached() - poll_start_time > absl::Seconds(1)) {
      for (const auto& desc : descriptors_) {
        LOG(WARNING) << id() << " list of sockets on slow poll:"
                     << " fd=" << desc.first << " sd=" << desc.second.get()
                     << " sd.fd=" << desc.second->fd()
                     << " readable=" << desc.second->IsReadable()
                     << " closed=" << desc.second->IsClosed()
                     << " canreuse=" << desc.second->CanReuse()
                     << " err=" << desc.second->GetLastErrorMessage();
      }
    }
  }

  // Check delayed closures.
  while (!delayed_pendings_.empty() &&
         (delayed_pendings_.top()->time() < NowCached() || shutting_down_)) {
    DelayedClosureImpl* delayed_closure = delayed_pendings_.top();
    LOG_EVERY_SEC(INFO) << "delayed_closure location:"
                        << delayed_closure->location()
                        << " time:" << delayed_closure->time();
    delayed_pendings_.pop();
    AddClosure(delayed_closure->location(), PRIORITY_IMMEDIATE,
               NewCallback(delayed_closure, &DelayedClosureImpl::Run));
  }

  // Check periodic closures.
  for (const auto& periodic_closure : periodic_closures_) {
    PermanentClosure* closure = periodic_closure->GetClosure(NowCached());
    if (closure != nullptr) {
      VLOG(3) << "periodic=" << closure;
      LOG_EVERY_SEC(INFO) << "periodic_closure location:"
                          << periodic_closure->location();
      AddClosure(periodic_closure->location(),
                 PRIORITY_IMMEDIATE, closure);
    }
  }

  // Check descriptors I/O.
  for (auto& iter : io_pendings) {
    Priority io_priority = iter.first;
    std::deque<OneshotClosure*>& pendings = iter.second;
    while (!pendings.empty()) {
      LOG_EVERY_SEC(INFO) << "io closure: " << pendings.front();
      // TODO: use original location
      AddClosure(FROM_HERE, io_priority, pendings.front());
      pendings.pop_front();
    }
  }

  // Check pendings again.
  for (priority = PRIORITY_IMMEDIATE; priority >= PRIORITY_MIN; --priority) {
    if (!pendings_[priority].empty()) {
      auto priority_typed = static_cast<Priority>(priority);
      VLOG(2) << "pendings " << Priority_Name(priority_typed);
      current_closure_data_ = GetClosure(priority_typed);

      if (quit_) {
        // If worker thread is quiting, wake up thread soon.
        poller_->Signal();
      }
      return true;
    }
  }

  // No pendings.
  DCHECK_LT(priority, PRIORITY_MIN);
  if (quit_) {
    VLOG(3) << "NextClosure: terminating";
    if (delayed_pendings_.empty() &&
        periodic_closures_.empty() &&
        descriptors_.empty()) {
      pool_.set(WorkerThreadManager::kDeadPool);
      return false;
    }
    LOG(INFO) << "NextClosure: terminating but still active "
              << " delayed_pendings=" << delayed_pendings_.size()
              << " periodic_closures=" << periodic_closures_.size()
              << " descriptors=" << descriptors_.empty();
  }
  VLOG(4) << "NextClosure: no closure to run, name_=" << name_;
  throttler_raii.MarkLoopIdle();
  return true;
}

void WorkerThread::AddClosure(const char* const location, Priority priority,
                              Closure* closure) {
  VLOG(2) << "AddClosure " << name_;
  // mu_ held.
  ClosureData closure_data(location, closure, pendings_[priority].size(), tick_,
                           timer_.GetDuration());
  if (closure_data.queuelen_ > max_queuelen_[priority]) {
    max_queuelen_[priority] = closure_data.queuelen_;
  }
  pendings_[priority].push_back(closure_data);
}

WorkerThread::ClosureData WorkerThread::GetClosure(Priority priority) {
  // mu_ held.
  CHECK(!pendings_[priority].empty());
  ClosureData closure_data = pendings_[priority].front();
  pendings_[priority].pop_front();
  absl::Duration wait_time = timer_.GetDuration() - closure_data.timestamp_;
  if (wait_time > max_wait_time_[priority]) {
    max_wait_time_[priority] = wait_time;
  }
  if (wait_time > absl::Minutes(1)) {
    LOG(WARNING) << id() << " too long in pending queue "
                 << Priority_Name(priority) << " " << wait_time
                 << " queuelen=" << closure_data.queuelen_
                 << " tick=" << (tick_ - closure_data.tick_);
  }
  return closure_data;
}

void WorkerThread::InitializeWorkerKey() {
#ifndef _WIN32
  pthread_key_create(&key_worker_, nullptr);
#else
  key_worker_ = TlsAlloc();
#endif
}

void WorkerThread::RegisterPollEvent(SocketDescriptor* d,
                                     DescriptorEventType type) {
  VLOG(2) << "RegisterPollEvent " << name_;
  AUTOLOCK(lock, &mu_);
  poller_->RegisterPollEvent(d, type);
}

void WorkerThread::UnregisterPollEvent(SocketDescriptor* d,
                                       DescriptorEventType type) {
  VLOG(2) << "UnregisterPollEvent " << name_;
  AUTOLOCK(lock, &mu_);
  poller_->UnregisterPollEvent(d, type);
}

void WorkerThread::RegisterTimeoutEvent(SocketDescriptor* d) {
  VLOG(2) << "RegisterTimeoutEvent " << name_;
  AUTOLOCK(lock, &mu_);
  poller_->RegisterTimeoutEvent(d);
}

void WorkerThread::UnregisterTimeoutEvent(SocketDescriptor* d) {
  VLOG(2) << "UnregisterTimeoutEvent " << name_;
  AUTOLOCK(lock, &mu_);
  poller_->UnregisterTimeoutEvent(d);
}

void WorkerThread::Start() {
  VLOG(2) << "Start " << name_;
  CHECK(PlatformThread::Create(this, &handle_));
  CHECK_NE(handle_, kNullThreadHandle);
  id_.WaitUntilInitialized();
}

void WorkerThread::Join() {
  VLOG(2) << "Join " << name_;
  if (handle_ != kNullThreadHandle) {
    LOG(INFO) << "Join thread:" << DebugString();
    {
      AUTOLOCK(lock, &mu_);
      CHECK(quit_);
    }
    FlushLogFiles();
    PlatformThread::Join(handle_);
  }
  handle_ = kNullThreadHandle;
  id_.Reset();
}

}  // namespace devtools_goma
