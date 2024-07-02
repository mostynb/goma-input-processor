// Copyright 2016 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//
// How garbage collection works:
// 1. When LocalOutputCache starts, StartLoadCacheEntries() is called.
//    In that function, it reads all cache entries, sorts them by mtime,
//    and inserts to |entries_|. When all done, |ready_| becomes true.
//    All methods like Lookup()/SaveOutput() will be blocked until
//    |ready_| becomes true.
//
//    TODO: We have design choice here. When we block until all load is
//    done, compile does not start until load is done. When we don't block
//    but return false until ready, a user might commit duplicated results.
//    It makes states complex. Currently we're choosing a safer option,
//    but this should be reconsidered.
//
// 2. When loading thread starts, we also start garbage collection thread.
//
// 3. During builds, when total cache size exceeds max_cache_size, GC thread
//    is waken up (by notifying |gc_cond_|)
//
// 4. When GC thread awake, and |entries_total_cache_amount_| exceeds
//    |max_cache_amount_byte|, GC happens. It removes older entries until
//    |entries_total_cache_amount_| become lower than
//    |threshold_cache_amount_byte_|.
//
// * Cache Directory Structure
//
// proto_file = <cache dir>/<first 2 chars of key>/<key>
//   <key> is always hex notation of SHA256.

#include "local_output_cache.h"

#include <stdio.h>  // For rename

#include <algorithm>
#include <fstream>
#include <memory>
#include <vector>

#include "absl/time/clock.h"
#include "callback.h"
#include "compiler_flag_type_specific.h"
#include "file_dir.h"
#include "file_helper.h"
#include "file_stat.h"
#include "filesystem.h"
#include "glog/logging.h"
#include "goma_hash.h"
#include "histogram.h"
#include "options.h"
#include "path.h"
#include "simple_timer.h"
#include "worker_thread.h"

MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "lib/goma_stats.pb.h"
MSVC_POP_WARNING()

#ifndef _WIN32
# include <sys/stat.h>
# include <sys/types.h>
# include <unistd.h>
#else
# include "config_win.h"
# include "posix_helper_win.h"
#endif

namespace {

// Timeout value in seconds for LoadCacheEntries().
constexpr absl::Duration kLoadCacheEntriesTimeout = absl::Seconds(1);

}  // anonymous namespace

namespace devtools_goma {

LocalOutputCache* LocalOutputCache::instance_;

LocalOutputCache::LocalOutputCache(std::string cache_dir,
                                   std::int64_t max_cache_amount_byte,
                                   std::int64_t threshold_cache_amount_byte,
                                   size_t max_cache_items,
                                   size_t threshold_cache_items)
    : cache_dir_(std::move(cache_dir)),
      max_cache_amount_byte_(max_cache_amount_byte),
      threshold_cache_amount_byte_(threshold_cache_amount_byte),
      max_cache_items_(max_cache_items),
      threshold_cache_items_(threshold_cache_items),
      ready_(false),
      entries_total_cache_amount_(0),
      gc_should_done_(false),
      gc_working_(false) {}

LocalOutputCache::~LocalOutputCache() {
}

// static
void LocalOutputCache::Init(std::string cache_dir,
                            WorkerThreadManager* wm,
                            int max_cache_amount_in_mb,
                            int threshold_cache_amount_in_mb,
                            size_t max_cache_items,
                            size_t threshold_cache_items) {
  CHECK(instance_ == nullptr);
  if (cache_dir.empty()) {
    return;
  }

  if (!EnsureDirectory(cache_dir, 0700)) {
    LOG(ERROR) << "failed to make cache directory: " << cache_dir
               << " LocalOutputCache is not enabled";
    return;
  }

  std::int64_t max_cache_amount_byte =
      max_cache_amount_in_mb * std::int64_t(1000000);
  std::int64_t threshold_cache_amount_byte =
      threshold_cache_amount_in_mb * std::int64_t(1000000);

  instance_ = new LocalOutputCache(std::move(cache_dir),
                                   max_cache_amount_byte,
                                   threshold_cache_amount_byte,
                                   max_cache_items,
                                   threshold_cache_items);
  if (wm != nullptr) {
    // Loading cache entries can take long time. Don't block here.
    // When blocked, compiler_proxy start might be failed due to timeout.
    instance_->StartLoadCacheEntries(wm);
    instance_->StartGarbageCollection(wm);
  } else {
    // wm is nullptr in test. Just make ready_ = true.
    instance_->SetReady(true);
  }
}

// static
void LocalOutputCache::Quit() {
  if (instance_ == nullptr) {
    return;
  }

  LOG(INFO) << "LocalOutputCache quiting...";
  // Might be still loading. Wait for that case.
  instance_->WaitUntilReady();
  // Stop garbage collection thread.
  instance_->StopGarbageCollection();
  instance_->WaitUntilGarbageCollectionThreadDone();
  LOG(INFO) << "LocalOutputCache GC thread has been terminated.";

  delete instance_;
  instance_ = nullptr;
}

void LocalOutputCache::StartLoadCacheEntries(WorkerThreadManager* wm) {
  wm->RunClosure(FROM_HERE,
                 NewCallback(this, &LocalOutputCache::LoadCacheEntries),
                 WorkerThread::PRIORITY_LOW);
}

void LocalOutputCache::LoadCacheEntries() {
  // For fine load time measurement.
  Histogram list_directory_histogram;
  Histogram file_stat_histogram;

  list_directory_histogram.SetName("LocalOutputCache ListDirectory");
  file_stat_histogram.SetName("LocalOutputCache FileStat");

  SimpleTimer walk_timer(SimpleTimer::START);
  size_t total_file_size = 0;

  std::vector<std::pair<SHA256HashValue, CacheEntry>> cache_entries;

  std::vector<DirEntry> key_prefix_entries;
  {
    SimpleTimer timer(SimpleTimer::START);
    if (!ListDirectory(cache_dir_, &key_prefix_entries)) {
      LOG(ERROR) << "failed to load LocalOutputCache entries:"
                 << " cache_dir=" << cache_dir_;
      LoadCacheEntriesDone();
      return;
    }
    const absl::Duration duration = timer.GetDuration();
    list_directory_histogram.Add(absl::ToInt64Nanoseconds(duration));
    if (duration >= kLoadCacheEntriesTimeout) {
      LOG(WARNING) << "SLOW ListDirectory: " << cache_dir_;
    }
  }

  for (const auto& key_prefix_entry : key_prefix_entries) {
    if (!key_prefix_entry.is_dir ||
        key_prefix_entry.name == "." ||
        key_prefix_entry.name == "..") {
      continue;
    }

    std::string cache_dir_with_key_prefix =
        file::JoinPath(cache_dir_, key_prefix_entry.name);
    std::vector<DirEntry> key_entries;

    {
      SimpleTimer timer(SimpleTimer::START);
      if (!ListDirectory(cache_dir_with_key_prefix, &key_entries)) {
        // Might be better to remove this directory contents.
        continue;
      }
      const absl::Duration duration = timer.GetDuration();
      list_directory_histogram.Add(absl::ToInt64Nanoseconds(duration));
      if (duration >= kLoadCacheEntriesTimeout) {
        LOG(WARNING) << "SLOW ListDirectory: " << cache_dir_with_key_prefix;
      }
    }

    for (const auto& key_entry : key_entries) {
      if (key_entry.name == "." || key_entry.name == "..") {
        continue;
      }

      std::string cache_file_path =
          file::JoinPath(cache_dir_with_key_prefix, key_entry.name);

      if (key_entry.is_dir) {
        // Probably old style cache. remove this.
        LOG(INFO) << "directory found. remove: " << cache_file_path;
        if (!file::RecursivelyDelete(cache_file_path, file::Defaults()).ok()) {
          LOG(ERROR) << "failed to remove: " << cache_file_path;
        }
        continue;
      }

      SHA256HashValue key;
      if (!SHA256HashValue::ConvertFromHexString(key_entry.name, &key)) {
        LOG(WARNING) << "Invalid filename found. remove: filename="
                     << cache_file_path;
        ::util::Status status = file::Delete(cache_file_path, file::Defaults());
        if (!status.ok()) {
          LOG(ERROR) << "failed to remove: " << cache_file_path;
        }
        continue;
      }

      FileStat file_stat;
      {
        SimpleTimer timer(SimpleTimer::START);
        file_stat = FileStat(cache_file_path);
        const absl::Duration duration = timer.GetDuration();
        file_stat_histogram.Add(absl::ToInt64Nanoseconds(duration));
        if (duration >= kLoadCacheEntriesTimeout) {
          LOG(WARNING) << "SLOW FileStat: " << cache_file_path;
        }
      }

      if (!file_stat.IsValid()) {
        LOG(ERROR) << "unexpectedly file is removed? "
                   << "path=" << cache_file_path;
        continue;
      }

      total_file_size += file_stat.size;
      cache_entries.emplace_back(
          key, CacheEntry(*file_stat.mtime, file_stat.size));
    }
  }

  LOG(INFO) << "walk_time=" << walk_timer.GetDuration() << " "
            << "total_cache_count=" << cache_entries.size() << " "
            << "total_size_in_byte=" << total_file_size;

  // DebugString() triggers CHECK if count() == 0.
  if (list_directory_histogram.count() > 0) {
    LOG(INFO) << list_directory_histogram.DebugString();
  }
  if (file_stat_histogram.count() > 0) {
    LOG(INFO) << file_stat_histogram.DebugString();
  }

  // Sort by mtime. Older cache entry comes first for GC.
  std::sort(cache_entries.begin(), cache_entries.end(),
            [](const std::pair<SHA256HashValue, CacheEntry>& lhs,
               const std::pair<SHA256HashValue, CacheEntry>& rhs) {
                return lhs.second.mtime < rhs.second.mtime;
            });

  {
    AUTO_EXCLUSIVE_LOCK(lock, &entries_mu_);
    for (auto&& entry : cache_entries) {
      entries_.emplace_back(std::move(entry.first), std::move(entry.second));
    }
    entries_total_cache_amount_ = total_file_size;
  }

  LoadCacheEntriesDone();
}

void LocalOutputCache::LoadCacheEntriesDone() {
  AUTOLOCK(lock, &ready_mu_);
  ready_ = true;
  ready_cond_.Broadcast();
}

void LocalOutputCache::WaitUntilReady() {
  AUTOLOCK(lock, &ready_mu_);
  while (!ready_) {
    ready_cond_.Wait(&ready_mu_);
  }
}

void LocalOutputCache::AddCacheEntry(const SHA256HashValue& key,
                                     std::int64_t cache_size) {
  const absl::Time cache_mtime = absl::Now();
  bool needs_wake_gc_thread = false;
  {
    AUTO_EXCLUSIVE_LOCK(lock, &entries_mu_);
    entries_.emplace_back(key, CacheEntry(cache_mtime, cache_size));
    entries_total_cache_amount_ += cache_size;

    if (ShouldInvokeGarbageCollectionUnlocked()) {
      needs_wake_gc_thread = true;
    }
  }

  // Don't call WakeGCThread with holding entries_mu_.
  if (needs_wake_gc_thread) {
    WakeGCThread();
  }
}

void LocalOutputCache::UpdateCacheEntry(const SHA256HashValue& key) {
  AUTO_EXCLUSIVE_LOCK(lock, &entries_mu_);

  // Because of GC, key might be removed here.
  auto it = entries_.find(key);
  if (it != entries_.end()) {
    entries_.MoveToBack(it);
  }
}

void LocalOutputCache::StartGarbageCollection(WorkerThreadManager* wm) {
  {
    AUTOLOCK(lock, &gc_mu_);
    gc_should_done_ = false;
    gc_working_ = true;
  }
  wm->NewThread(NewCallback(this,
                            &LocalOutputCache::GarbageCollectionThread),
                "local-output-cache-gc");
}

void LocalOutputCache::StopGarbageCollection() {
  LOG(INFO) << "try to stop gc thread";

  AUTOLOCK(lock, &gc_mu_);
  gc_should_done_ = true;
  gc_cond_.Broadcast();
}

void LocalOutputCache::WakeGCThread() {
  LOG(INFO) << "try to wake gc thread";

  AUTOLOCK(lock, &gc_mu_);
  gc_cond_.Broadcast();
}

void LocalOutputCache::GarbageCollectionThread() {
  // GC should not start until ready.
  WaitUntilReady();

  while (true) {
    while (true) {
      AUTOLOCK(lock, &gc_mu_);

      // Return if gc done.
      if (gc_should_done_) {
        LOG(INFO) << "gc has done. gc thread will be done.";
        gc_working_ = false;
        gc_cond_.Signal();
        return;
      }

      // With this condition, start GC.
      if (ShouldInvokeGarbageCollection()) {
        break;
      }

      // Wait until gc-wakeup signal comes.
      gc_cond_.Wait(&gc_mu_);
    }

    LOG(INFO) << "LocalOutputCache GC thread awaken";
    GarbageCollectionStat stat;
    RunGarbageCollection(&stat);

    LOG(INFO) << "LocalOutputCache GC Done:"
              << " removed_count=" << stat.num_removed
              << " removed_bytes=" << stat.removed_bytes
              << " failed=" << stat.num_failed;

    stats_gc_removed_items_.Add(stat.num_removed);
    stats_gc_removed_bytes_.Add(stat.removed_bytes);
    stats_gc_failed_items_.Add(stat.num_failed);
  }
}

bool LocalOutputCache::ShouldInvokeGarbageCollection() const {
  AUTO_SHARED_LOCK(lock, &entries_mu_);
  return ShouldInvokeGarbageCollectionUnlocked();
}

bool LocalOutputCache::ShouldInvokeGarbageCollectionUnlocked() const {
  if (max_cache_amount_byte_ < entries_total_cache_amount_) {
    VLOG(1) << "GC will be invoked:"
            << " max_cache_amount_byte=" << max_cache_amount_byte_
            << " entries_total_cache_amount=" << entries_total_cache_amount_;
    return true;
  }
  if (max_cache_items_ < entries_.size()) {
    VLOG(1) << "GC will be invoked:"
            << " max_cache_items=" << max_cache_items_
            << " entries_size=" << entries_.size();
    return true;
  }

  return false;
}

bool LocalOutputCache::ShouldContinueGarbageCollectionUnlocked() const {
  if (max_cache_amount_byte_ < entries_total_cache_amount_) {
    return true;
  }
  if (threshold_cache_amount_byte_ < entries_total_cache_amount_) {
    return true;
  }
  if (max_cache_items_ < entries_.size()) {
    return true;
  }
  if (threshold_cache_items_ < entries_.size()) {
    return true;
  }

  return false;
}

void LocalOutputCache::RunGarbageCollection(GarbageCollectionStat* stat) {
  // cache exceeded the max size. Removing the cache entries.
  stats_gc_count_.Add(1);
  SimpleTimer timer(SimpleTimer::START);

  while (true) {
    AUTO_EXCLUSIVE_LOCK(lock, &entries_mu_);

    if (!ShouldContinueGarbageCollectionUnlocked()) {
      break;
    }

    const CacheEntry& entry = entries_.front().second;
    std::string key_string = entries_.front().first.ToHexString();

    std::string cache_file_path = CacheFilePath(key_string);
    ::util::Status status = file::Delete(cache_file_path, file::Defaults());
    if (!status.ok()) {
      LOG(ERROR) << "failed to remove cache: path=" << cache_file_path;
      break;
    }

    stat->num_removed += 1;
    stat->removed_bytes += entry.amount_byte;
    entries_total_cache_amount_ -= entry.amount_byte;
    entries_.pop_front();
  }

  stats_gc_total_time_ms_.Add(absl::ToInt64Milliseconds(timer.GetDuration()));
}

void LocalOutputCache::WaitUntilGarbageCollectionThreadDone() {
  AUTOLOCK(lock, &gc_mu_);
  while (gc_working_) {
    LOG(INFO) << "LocalOutputCache: waiting GC finished";
    gc_cond_.Wait(&gc_mu_);
  }
}

void LocalOutputCache::SetReady(bool ready) {
  AUTOLOCK(lock, &ready_mu_);
  ready_ = ready;
}

bool LocalOutputCache::SaveOutput(const std::string& key,
                                  const ExecReq* req,
                                  const ExecResp* resp,
                                  const std::string& trace_id) {
  WaitUntilReady();
  SimpleTimer timer(SimpleTimer::START);

  if (!resp->has_result()) {
    return false;
  }

  SHA256HashValue key_hash;
  if (!SHA256HashValue::ConvertFromHexString(key, &key_hash)) {
    LOG(ERROR) << "key is invalid format: key=" << key;
    return false;
  }

  // --- Ensure cache directory exists.
  std::string cache_dir_with_key_prefix = CacheDirWithKeyPrefix(key);
  if (!EnsureDirectory(cache_dir_with_key_prefix, 0755)) {
    LOG(ERROR) << trace_id << " failed to create " << cache_dir_with_key_prefix;
    return false;
  }

  // --- Make cache_entry.
  LocalOutputCacheEntry cache_entry;
  const ExecResult& result = resp->result();
  if (result.has_stdout_buffer())
    cache_entry.set_stdout_buffer(result.stdout_buffer());
  if (result.has_stderr_buffer())
    cache_entry.set_stderr_buffer(result.stderr_buffer());
  for (const auto& output : result.output()) {
    std::string src_path =
        file::JoinPathRespectAbsolute(req->cwd(), output.filename());
    if (req->has_original_cwd()) {
      src_path =
          file::JoinPathRespectAbsolute(req->original_cwd(), output.filename());
    }

    std::string output_file_content;
    if (!ReadFileToString(src_path, &output_file_content)) {
      LOG(ERROR) << " failed to read file: " << src_path;
      return false;
    }

    LocalOutputCacheFile* cache_file = cache_entry.add_files();
    cache_file->set_filename(output.filename());
    cache_file->set_content(std::move(output_file_content));
    cache_file->set_is_executable(output.is_executable());
  }

  // --- Serialize LocalOutputCacheEntry to a file.
  // When compiler_proxy is killed during writing a file, the file will be
  // invalid but it might be a valid proto (when we're unlucky).
  // So, we serialize a data to a tmp file, and rename it.
  // We should be able to expect this is atomic.
  std::int64_t cache_amount_in_byte = 0;
  {
    std::string cache_file_path = CacheFilePath(key);
    std::string cache_file_tmp_path = cache_file_path + ".tmp";

    std::string serialized;
    if (!cache_entry.SerializeToString(&serialized)) {
      LOG(ERROR) << trace_id << " failed to serialize LocalOutputCacheEntry: "
                 << " path=" << cache_file_path;
      return false;
    }
    if (!WriteStringToFile(serialized, cache_file_tmp_path)) {
      stats_save_failure_.Add(1);
      LOG(ERROR) << trace_id << " failed to write LocalOutputCacheEntry:"
                 << " path=" << cache_file_path;
      return false;
    }

    int r = rename(cache_file_tmp_path.c_str(), cache_file_path.c_str());
    if (r < 0) {
      LOG(ERROR) << trace_id << " failed to rename LocalOutputCacheEntry:"
                 << " path=" << cache_file_path
                 << " result=" << r;
      (void)file::Delete(cache_file_path, file::Defaults());
      return false;
    }

    cache_amount_in_byte = serialized.size();
  }

  AddCacheEntry(key_hash, cache_amount_in_byte);

  stats_save_success_.Add(1);
  stats_save_success_time_ms_.Add(
      absl::ToInt64Milliseconds(timer.GetDuration()));
  return true;
}

bool LocalOutputCache::Lookup(const std::string& key,
                              ExecResp* resp,
                              const std::string& trace_id) {
  WaitUntilReady();
  SimpleTimer timer(SimpleTimer::START);

  SHA256HashValue key_hash;
  if (!SHA256HashValue::ConvertFromHexString(key, &key_hash)) {
    LOG(DFATAL) << "unexpected key format: key=" << key;
    return false;
  }

  // Check cache entry first.
  {
    AUTO_SHARED_LOCK(lock, &entries_mu_);
    if (!entries_.contains(key_hash)) {
      stats_lookup_miss_.Add(1);
      return false;
    }
  }

  const std::string cache_file_path = CacheFilePath(key);

  // Read file.
  // If GC happened after entries_find(), this file might be lost.
  std::ifstream ifs(cache_file_path, std::ifstream::binary);
  if (!ifs.is_open()) {
    stats_lookup_miss_.Add(1);
    return false;
  }

  LocalOutputCacheEntry cache_entry;
  if (!cache_entry.ParseFromIstream(&ifs)) {
    LOG(ERROR) << trace_id << " LocalOutputCache: failed to parse:"
               << " path=" << cache_file_path;
    stats_lookup_failure_.Add(1);
    return false;
  }

  UpdateCacheEntry(key_hash);

  // Create dummy ExecResp from LocalOutputCacheEntry.
  resp->set_cache_hit(ExecResp::LOCAL_OUTPUT_CACHE);
  ExecResult* result = resp->mutable_result();
  result->set_exit_status(0);
  if (cache_entry.has_stdout_buffer())
    result->set_stdout_buffer(cache_entry.stdout_buffer());
  if (cache_entry.has_stderr_buffer())
    result->set_stderr_buffer(cache_entry.stderr_buffer());
  for (auto&& file : cache_entry.files()) {
    ExecResult_Output* output = result->add_output();
    output->set_filename(file.filename());
    output->set_is_executable(file.is_executable());
    FileBlob* blob = output->mutable_blob();
    blob->set_blob_type(FileBlob::FILE);  // Always FILE.
    blob->set_file_size(file.content().size());
    blob->set_content(std::move(file.content()));
  }

  stats_lookup_success_.Add(1);
  stats_lookup_success_time_ms_.Add(
      absl::ToInt64Milliseconds(timer.GetDuration()));
  return true;
}

std::string LocalOutputCache::CacheDirWithKeyPrefix(
    absl::string_view key) const {
  return file::JoinPath(cache_dir_, key.substr(0, 2));
}

std::string LocalOutputCache::CacheFilePath(absl::string_view key) const {
  return file::JoinPath(cache_dir_, key.substr(0, 2), key);
}

void LocalOutputCache::DumpStatsToProto(LocalOutputCacheStats* stats) {
  stats->set_save_success(stats_save_success_.value());
  stats->set_save_success_time_ms(stats_save_success_time_ms_.value());
  stats->set_save_failure(stats_save_failure_.value());

  stats->set_lookup_success(stats_lookup_success_.value());
  stats->set_lookup_success_time_ms(stats_lookup_success_time_ms_.value());
  stats->set_lookup_miss(stats_lookup_miss_.value());
  stats->set_lookup_failure(stats_lookup_failure_.value());

  stats->set_commit_success(stats_commit_success_.value());
  stats->set_commit_success_time_ms(stats_commit_success_time_ms_.value());
  stats->set_commit_failure(stats_commit_failure_.value());

  stats->set_gc_count(stats_gc_count_.value());
  stats->set_gc_total_time_ms(stats_gc_total_time_ms_.value());
}

size_t LocalOutputCache::TotalCacheCount() {
  AUTO_SHARED_LOCK(lock, &entries_mu_);
  return entries_.size();
}

std::int64_t LocalOutputCache::TotalCacheAmountInByte() {
  AUTO_SHARED_LOCK(lock, &entries_mu_);
  return entries_total_cache_amount_;
}

// static
std::string LocalOutputCache::MakeCacheKey(const ExecReq& req) {
  ExecReq normalized(req);

  // Use the goma server default.
  const std::vector<std::string> flags{"Xclang", "B", "gcc-toolchain",
                                       "-sysroot", "resource-dir"};

  // TODO: Set debug_prefix_map, too?
  CompilerFlagTypeSpecific::FromArg(req.command_spec().name())
      .NewExecReqNormalizer()
      ->NormalizeForCacheKey(0, true, false, flags,
                             std::map<std::string, std::string>(), &normalized);

  std::string serialized;
  if (!normalized.SerializeToString(&serialized)) {
    LOG(ERROR) << "failed to make cache key: "
               << normalized.DebugString();
    return std::string();
  }

  std::string digest;
  ComputeDataHashKey(serialized, &digest);
  return digest;
}

} // namespace devtools_goma
