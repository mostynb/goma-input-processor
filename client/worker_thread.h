// Copyright 2012 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#ifndef DEVTOOLS_GOMA_CLIENT_WORKER_THREAD_H_
#define DEVTOOLS_GOMA_CLIENT_WORKER_THREAD_H_

#include <deque>
#include <map>
#include <queue>
#include <vector>

#include "absl/base/call_once.h"
#include "absl/base/thread_annotations.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "autolock_timer.h"
#include "basictypes.h"
#include "callback.h"
#include "descriptor_event_type.h"
#include "lockhelper.h"
#include "notification.h"
#include "platform_thread.h"
#include "scoped_fd.h"
#include "simple_timer.h"
#include "thread_safe_variable.h"

// Note: __LINE__ need to be wrapped twice to make its number string.
//       Otherwise, not a line number but string literal "__LINE__" would be
//       shown.
#define GOMA_WORKER_THREAD_STRINGFY(i) #i
#define GOMA_WORKER_THREAD_STR(i) GOMA_WORKER_THREAD_STRINGFY(i)
#define FROM_HERE __FILE__ ":" GOMA_WORKER_THREAD_STR(__LINE__)

namespace devtools_goma {

class AutoLockStat;
class DescriptorPoller;
class SocketDescriptor;

using PeriodicClosureId = int;
constexpr PeriodicClosureId kInvalidPeriodicClosureId = -1;

class WorkerThread : public PlatformThread::Delegate {
 public:
  // Windows often pass back 0xfffffffe (pseudo handle) as thread handle.
  // Therefore the reliable way of selecting a thread is to use the thread id.
  // ThreadHandle is used for Join().
  using ThreadHandle = PlatformThreadHandle;
  using ThreadId = PlatformThreadId;

  // We use SimpleTimer for monotonicity, which absl::Now() does not have. All
  // timestamps are given as durations since the start of the thread.
  using Timestamp = absl::Duration;

  // Priority of closures and descriptors.
  enum Priority {
    PRIORITY_MIN = 0,
    PRIORITY_LOW = 0,    // Used in compile_task.
    PRIORITY_MED,        // Used in http rpc and subprocess ipc.
    PRIORITY_HIGH,       // Used in http server (http and goma ipc serving)
    PRIORITY_IMMEDIATE,  // Called without descriptor polling.
                         // Used to clear notification closures of descriptor,
                         // delayed closures, or periodic closures.
    NUM_PRIORITIES
  };

  // Thread unsafe.  See RunDelayedClosureInThread.
  class CancelableClosure {
   public:
    CancelableClosure(const char* const locaction, Closure* closure);
    const char* location() const;
    void Cancel();
   protected:
    virtual ~CancelableClosure();
    Closure* closure_;
   private:
    const char* const location_;
    DISALLOW_COPY_AND_ASSIGN(CancelableClosure);
  };

  // See UnregisterPeriodicClosure
  class UnregisteredClosureData {
   public:
    UnregisteredClosureData() : done_(false), location_(nullptr) {}

    bool Done() const ABSL_LOCKS_EXCLUDED(mu_) {
      AUTOLOCK(lock, &mu_);
      return done_;
    }
    void SetDone(bool b) ABSL_LOCKS_EXCLUDED(mu_) {
      AUTOLOCK(lock, &mu_);
      done_ = b;
    }

    const char* Location() const ABSL_LOCKS_EXCLUDED(mu_) {
      AUTOLOCK(lock, &mu_);
      return location_;
    }
    void SetLocation(const char* location) ABSL_LOCKS_EXCLUDED(mu_) {
      AUTOLOCK(lock, &mu_);
      location_ = location;
    }

   private:
    mutable Lock mu_;
    bool done_ ABSL_GUARDED_BY(mu_);
    const char* location_ ABSL_GUARDED_BY(mu_);

    DISALLOW_COPY_AND_ASSIGN(UnregisteredClosureData);
  };

  class DelayedClosureImpl : public CancelableClosure {
   public:
    DelayedClosureImpl(const char* const location,
                       Timestamp t, Closure* closure)
        : CancelableClosure(location, closure), time_(t) {}
    Timestamp time() const { return time_; }
    Closure* GetClosure() {
      Closure* closure = closure_;
      closure_ = NULL;
      return closure;
    }

   private:
    friend class WorkerThread;
    friend class WorkerThreadTest;
    ~DelayedClosureImpl() override {}

    // Run closure if it is still set, and destroy itself.
    void Run();

    Timestamp time_;
    DISALLOW_COPY_AND_ASSIGN(DelayedClosureImpl);
  };

  static void Initialize();
  static WorkerThread* GetCurrentWorker();

  WorkerThread(int pool, std::string name);
  ~WorkerThread() override;

  int pool() const { return pool_.get(); }
  ThreadId id() const { return id_.get(); }
  Timestamp NowCached();
  void Start();

  // Runs delayed closures as soon as possible.
  void Shutdown() ABSL_LOCKS_EXCLUDED(mu_);

  // Requests to quit dispatch loop of the WorkerThread's thread, and terminate
  // the thread.
  void Quit() ABSL_LOCKS_EXCLUDED(mu_);

  // Joins the WorkerThread's thread.  You must call Quit() before Join(), and
  // call Join() before destructing the WorkerThread.
  void Join() ABSL_LOCKS_EXCLUDED(mu_);

  void ThreadMain() override ABSL_LOCKS_EXCLUDED(mu_);
  bool Dispatch() ABSL_LOCKS_EXCLUDED(mu_);

  // Registers file descriptor fd in priority.
  SocketDescriptor* RegisterSocketDescriptor(
      ScopedSocket&& fd, Priority priority);
  ScopedSocket DeleteSocketDescriptor(SocketDescriptor* d);

  void RegisterPollEvent(SocketDescriptor* d, DescriptorEventType)
      ABSL_LOCKS_EXCLUDED(mu_);
  void UnregisterPollEvent(SocketDescriptor* d, DescriptorEventType)
      ABSL_LOCKS_EXCLUDED(mu_);
  void RegisterTimeoutEvent(SocketDescriptor* d) ABSL_LOCKS_EXCLUDED(mu_);
  void UnregisterTimeoutEvent(SocketDescriptor* d) ABSL_LOCKS_EXCLUDED(mu_);

  void RegisterPeriodicClosure(PeriodicClosureId id,
                               const char* const location,
                               absl::Duration period,
                               std::unique_ptr<PermanentClosure> closure)
      ABSL_LOCKS_EXCLUDED(mu_);
  void UnregisterPeriodicClosure(PeriodicClosureId id,
                                 UnregisteredClosureData* data)
      ABSL_LOCKS_EXCLUDED(mu_);

  void RunClosure(const char* const location,
                  Closure* closure,
                  Priority priority) ABSL_LOCKS_EXCLUDED(mu_);
  CancelableClosure* RunDelayedClosure(const char* const location,
                                       absl::Duration delay,
                                       Closure* closure)
      ABSL_LOCKS_EXCLUDED(mu_);

  size_t load() const ABSL_LOCKS_EXCLUDED(mu_);
  size_t pendings() const ABSL_LOCKS_EXCLUDED(mu_);

  bool IsIdle() const ABSL_LOCKS_EXCLUDED(mu_);
  std::string DebugString() const ABSL_LOCKS_EXCLUDED(mu_);

  static std::string Priority_Name(Priority priority);

 private:
  struct ClosureData {
    ClosureData(const char* const location,
                Closure* closure,
                int queuelen,
                int tick,
                Timestamp timestamp);
    ClosureData() = default;
    ClosureData(const ClosureData&) = default;
    ClosureData& operator=(const ClosureData&) = default;

    const char* location_ = nullptr;
    Closure* closure_ = nullptr;
    int queuelen_;
    int tick_;
    Timestamp timestamp_;
  };

  class CompareDelayedClosureImpl {
   public:
    bool operator()(DelayedClosureImpl* a, DelayedClosureImpl* b) const {
      return a->time() > b->time();
    }
  };

  class ThreadSafeThreadId {
   public:
    ThreadSafeThreadId();
    // These two functions can only be called once.
    void Initialize(ThreadId id) ABSL_LOCKS_EXCLUDED(mu_);
    void WaitUntilInitialized() ABSL_LOCKS_EXCLUDED(mu_);
    // Returns the thread ID. This must be called after WaitUntilInitialized()
    // has returned.
    ThreadId get() const ABSL_LOCKS_EXCLUDED(mu_);
    // Resets the thread ID when this thread is joined.
    void Reset() ABSL_LOCKS_EXCLUDED(mu_);

   private:
    Notification init_n_;
    mutable ReadWriteLock mu_;
    ThreadId id_ ABSL_GUARDED_BY(mu_);
  };

  typedef std::priority_queue<DelayedClosureImpl*,
                              std::vector<DelayedClosureImpl*>,
                              CompareDelayedClosureImpl> DelayedClosureQueue;

  // Forward declaration, actual prototype in worker_thread.cc.
  class PeriodicClosure;

  friend class WorkerThreadTest;

  // Updates current_closure_ to run if any.
  // Returns false if no closure to run now (no pending, no network I/O and
  // no timeout).
  bool NextClosure() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Adds closure in priority.
  // Assert mu_ held.
  void AddClosure(const char* const location,
                  Priority priority,
                  Closure* closure) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // Gets closure in priority.
  // Assert mu_ held.
  ClosureData GetClosure(Priority priority) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  static void InitializeWorkerKey();

  const std::string name_;

  ThreadSafeVariable<int> pool_;
  ThreadHandle handle_;
  ThreadSafeThreadId id_;
  SimpleTimer timer_;
  ThreadSafeVariable<absl::optional<Timestamp>> now_cached_;

  mutable Lock mu_;
  absl::optional<ClosureData> current_closure_data_ ABSL_GUARDED_BY(mu_);
  int tick_ ABSL_GUARDED_BY(mu_);
  bool shutting_down_ ABSL_GUARDED_BY(mu_);
  bool quit_ ABSL_GUARDED_BY(mu_);

  // These auto_lock_stat_* are owned by g_auto_lock_stats.
  AutoLockStat* auto_lock_stat_next_closure_;
  AutoLockStat* auto_lock_stat_poll_events_;

  std::deque<ClosureData> pendings_[NUM_PRIORITIES] ABSL_GUARDED_BY(mu_);
  int max_queuelen_[NUM_PRIORITIES] ABSL_GUARDED_BY(mu_);
  absl::Duration max_wait_time_[NUM_PRIORITIES] ABSL_GUARDED_BY(mu_);

  // delayed_pendings_ and periodic_closures_ are handled in PRIORITY_IMMEDIATE
  DelayedClosureQueue delayed_pendings_ ABSL_GUARDED_BY(mu_);
  std::vector<std::unique_ptr<PeriodicClosure>> periodic_closures_
      ABSL_GUARDED_BY(mu_);

  std::map<int, std::unique_ptr<SocketDescriptor>> descriptors_
      ABSL_GUARDED_BY(mu_);
  std::unique_ptr<DescriptorPoller> poller_;
  absl::Duration poll_interval_ ABSL_GUARDED_BY(mu_);

  // Due to bugs like b/145046673 and b/131856067, we need to throttle the
  // thread by forcefully yielding the CPU if it has been in a consecutive idle
  // loop for too long. This could hurt IO throughput in theory (I doubt it,
  // because whenever Goma is in such a state, it is pretty much dead in the
  // water), but it works and the ideal fix (threading + IO model) will take
  // much longer time.
  class IdleLoopsThrottler {
   public:
    IdleLoopsThrottler(std::string name, Lock* mu);
    class Raii {
     public:
      explicit Raii(IdleLoopsThrottler* throttler);
      // ~Raii() could block.
      ~Raii();

      void MarkLoopIdle() { idle_ = true; }

     private:
      IdleLoopsThrottler* const throttler_;
      bool idle_;
    };

   private:
    friend class Raii;

    void OnBusy() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
    void OnIdle() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

    const std::string name_;
    Lock* const mu_;
    int num_idle_;
  };
  IdleLoopsThrottler idle_loops_throttler_ ABSL_GUARDED_BY(mu_);

  static absl::once_flag key_worker_once_;
#ifndef _WIN32
  static pthread_key_t key_worker_;
#else
  static DWORD key_worker_;
#endif
  DISALLOW_COPY_AND_ASSIGN(WorkerThread);
};

constexpr WorkerThread::ThreadId kInvalidThreadId = 0;

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_WORKER_THREAD_H_
