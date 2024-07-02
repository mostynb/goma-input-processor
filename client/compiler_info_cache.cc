// Copyright 2015 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "compiler_info_cache.h"

#include <memory>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/memory/memory.h"
#include "absl/strings/str_join.h"
#include "absl/time/clock.h"
#include "autolock_timer.h"
#include "compiler_flags.h"
#include "compiler_info_state.h"
#include "compiler_proxy_info.h"
#include "glog/logging.h"
#include "goma_hash.h"
#include "path.h"

MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "client/compiler_info_data.pb.h"
MSVC_POP_WARNING()

namespace devtools_goma {

constexpr absl::Duration kNegativeCacheDuration = absl::Minutes(10);
constexpr absl::Duration kUpdateLastUsedAtDuration = absl::Minutes(10);

CompilerInfoCache* CompilerInfoCache::instance_;

std::string CompilerInfoCache::Key::ToString(bool cwd_relative) const {
  if (cwd_relative) {
    return local_compiler_path + " " + base + cwd;
  }
  // if |local_compiler_path| is not absolute path,
  // CompilerInfo may not be independent of |cwd|.
  // e.g. with -no-canonical-prefixes
  DCHECK(file::IsAbsolutePath(local_compiler_path));
  return local_compiler_path + " " + base;
}

std::string CompilerInfoCache::Key::abs_local_compiler_path() const {
  return file::JoinPathRespectAbsolute(cwd, local_compiler_path);
}

/* static */
void CompilerInfoCache::Init(const std::string& cache_dir,
                             const std::string& cache_filename,
                             int num_entries,
                             absl::Duration cache_holding_time) {
  CHECK(instance_ == nullptr);
  if (cache_filename == "") {
    instance_ = new CompilerInfoCache("", num_entries, cache_holding_time);
    return;
  }
  instance_ = new CompilerInfoCache(
      file::JoinPathRespectAbsolute(cache_dir, cache_filename), num_entries,
      cache_holding_time);
}

/* static */
void CompilerInfoCache::LoadIfEnabled() {
  if (instance()->cache_file_.Enabled()) {
    instance()->Load();
  } else {
    LOG(INFO) << "compiler_info_cache: no cache file";
  }
}

void CompilerInfoCache::Quit() {
  delete instance_;
  instance_ = nullptr;
}

CompilerInfoCache::CompilerInfoCache(const std::string& cache_filename,
                                     int num_entries,
                                     absl::Duration cache_holding_time)
    : cache_file_(cache_filename),
      max_num_entries_(num_entries),
      cache_holding_time_(cache_holding_time) {}

CompilerInfoCache::~CompilerInfoCache() {
  if (cache_file_.Enabled()) {
    Save();
  }
  Clear();
}

void CompilerInfoCache::Clear() {
  AUTO_EXCLUSIVE_LOCK(lock, &mu_);
  ClearUnlocked();
}

void CompilerInfoCache::ClearUnlocked() {
  keys_by_hash_.clear();
  for (auto& it : compiler_info_) {
    it.second->Deref();
  }
  compiler_info_.clear();
}

/* static */
CompilerInfoCache::Key CompilerInfoCache::CreateKey(
    const CompilerFlags& flags,
    const std::string& local_compiler_path,
    const std::vector<std::string>& key_envs) {
  const std::vector<std::string>& compiler_info_flags =
      flags.compiler_info_flags();
  std::vector<std::string> compiler_info_keys(compiler_info_flags);
  copy(key_envs.begin(), key_envs.end(), back_inserter(compiler_info_keys));
  std::string compiler_info_keys_str = absl::StrJoin(compiler_info_keys, " ");

  Key key;
  key.base = compiler_info_keys_str + " lang:" + flags.lang() + " @";
  key.cwd = flags.cwd();
  key.local_compiler_path = local_compiler_path;
  return key;
}

CompilerInfoState* CompilerInfoCache::Lookup(const Key& key) {
  AUTO_SHARED_LOCK(lock, &mu_);
  CompilerInfoState* state = nullptr;
  if (file::IsAbsolutePath(key.local_compiler_path)) {
    state = LookupUnlocked(key.ToString(!Key::kCwdRelative),
                           key.local_compiler_path);
  }
  if (state == nullptr) {
    state = LookupUnlocked(key.ToString(Key::kCwdRelative),
                           key.abs_local_compiler_path());
  }

  // Update last used timestamp of |state| having old timestamp.
  if (state != nullptr &&
      absl::Now() - state->info().last_used_at() > kUpdateLastUsedAtDuration) {
    state->UpdateLastUsedAt();
  }

  return state;
}

CompilerInfoState* CompilerInfoCache::LookupUnlocked(
    const std::string& compiler_info_key,
    const std::string& abs_local_compiler_path) {
  auto it = compiler_info_.find(compiler_info_key);
  if (it == compiler_info_.end()) {
    return nullptr;
  }
  auto info = it->second;
  if (validator_->Validate(info->info(), abs_local_compiler_path)) {
    VLOG(1) << "Cache hit for compiler-info with key: "
            << compiler_info_key;

    if (!info->info().HasError()) {
      return info;
    }

    if (absl::Now() < *info->info().failed_at() + kNegativeCacheDuration) {
      return info;
    }

    VLOG(1) << "Negative cache is expired: " << compiler_info_key;
  }

  LOG(INFO) << "Cache hit, but obsolete compiler-info for key: "
            << compiler_info_key;
  return nullptr;
}

CompilerInfoState* CompilerInfoCache::Store(
    const Key& key, std::unique_ptr<CompilerInfoData> data) {
  AUTO_EXCLUSIVE_LOCK(lock, &mu_);
  DCHECK(data != nullptr);

  ScopedCompilerInfoState state;

  bool dup = false;
  std::string dup_compiler_info_key;
  std::string hash = HashKey(*data);
  {
    auto found = keys_by_hash_.find(hash);
    if (found != keys_by_hash_.end()) {
      auto* keys = found->second.get();
      if (!keys->empty()) {
        const std::string& compiler_info_key = *keys->begin();
        state.reset(LookupUnlocked(
            compiler_info_key, key.abs_local_compiler_path()));
        if (state.get() != nullptr) {
          LOG(INFO) << "hash=" << hash << " share with " << compiler_info_key;
          dup = true;
        }
      }
    }
  }

  if (state.get() == nullptr) {
    state.reset(new CompilerInfoState(std::move(data)));
  }
  state.get()->Ref();  // in cache.

  if (!state.get()->info().found()) {
    ++num_miss_;
    DCHECK(state.get()->info().HasError());
    DCHECK(state.get()->info().failed_at().has_value());
  } else if (state.get()->info().HasError()) {
    ++num_fail_;
    DCHECK(state.get()->info().failed_at().has_value());
  } else if (dup) {
    ++num_store_dups_;
    DCHECK(!state.get()->info().failed_at().has_value());
  } else {
    ++num_stores_;
    DCHECK(!state.get()->info().failed_at().has_value());
  }

  std::string old_hash;
  const std::string compiler_info_key =
      key.ToString(!file::IsAbsolutePath(key.local_compiler_path) ||
                   state.get()->info().DependsOnCwd(key.cwd));
  {
    auto p = compiler_info_.insert(
        std::make_pair(compiler_info_key, state.get()));
    if (!p.second) {
      CompilerInfoState* old_state = p.first->second;
      old_hash = HashKey(old_state->info().data());
      old_state->Deref();
      p.first->second = state.get();
    }
  }
  {
    auto p = keys_by_hash_.emplace(hash, nullptr);
    if (p.second) {
      p.first->second = absl::make_unique<absl::flat_hash_set<std::string>>();
    }
    p.first->second->insert(compiler_info_key);
    LOG(INFO) << "hash=" << hash << " key=" << compiler_info_key;
  }
  if (old_hash != "") {
    auto p = keys_by_hash_.find(old_hash);
    if (p != keys_by_hash_.end()) {
      LOG(INFO) << "delete hash=" << hash << " key=" << compiler_info_key;
      p->second->erase(compiler_info_key);
      if (p->second->empty()) {
        LOG(INFO) << "delete hash=" << hash;
        keys_by_hash_.erase(p);
      }
    }
  }
  if (dup) {
    DCHECK_GT(state.get()->refcnt(), 2);
  } else {
    DCHECK_EQ(state.get()->refcnt(), 2);
  }
  LOG(INFO) << "Update state=" << state.get()
            << " for key=" << compiler_info_key
            << " version=" << state.get()->info().version()
            << " target=" << state.get()->info().target()
            << " compiler_hash=" << state.get()->info().request_compiler_hash()
            << " hash=" << hash;

  // Check if the same local compiler was already disabled.
  for (const auto& info : compiler_info_) {
    CompilerInfoState* cis = info.second;
    if (!cis->disabled())
      continue;
    if (state.get()->info().IsSameCompiler(cis->info())) {
      state.get()->SetDisabled(true, "the same compiler is already disabled");
      LOG(INFO) << "Disabled state=" << state.get();
      break;
    }
  }
  // CompilerInfoState is referenced in cache, so it won't be destroyed
  // when state is destroyed.
  return state.get();
}

bool CompilerInfoCache::Disable(CompilerInfoState* compiler_info_state,
                                const std::string& disabled_reason) {
  AUTO_EXCLUSIVE_LOCK(lock, &mu_);

  LOG(INFO) << "Disable state=" << compiler_info_state;
  bool disabled = false;
  if (!compiler_info_state->disabled()) {
    compiler_info_state->SetDisabled(true, disabled_reason);
    LOG(INFO) << "Disabled state=" << compiler_info_state;
    disabled = true;
  }

  // Also mark other CompilerInfo disabled if it is the same
  // local compiler (but it would use different compiler_info_flags).
  for (auto& info : compiler_info_) {
    CompilerInfoState* cis = info.second;
    if (cis->disabled())
      continue;
    if (compiler_info_state->info().IsSameCompiler(cis->info())) {
      if (!cis->disabled()) {
        cis->SetDisabled(true, disabled_reason);
        LOG(INFO) << "Disabled state=" << cis;
      }
    }
  }

  return disabled;
}

void CompilerInfoCache::Dump(std::ostringstream* ss) {
  AUTO_SHARED_LOCK(lock, &mu_);
  (*ss) << "compiler info:" << compiler_info_.size()
        << " info_hashes=" << keys_by_hash_.size() << "\n";

  (*ss) << "\n[keys by hash]\n";
  for (const auto& it : keys_by_hash_) {
    (*ss) << "hash: " << it.first << "\n";
    for (const auto& k : *it.second) {
      (*ss) << " key:" << k << "\n";
    }
    (*ss) << "\n";
  }
  (*ss) << "\n";

  (*ss) << "\n[compiler info]\n\n";
  for (const auto& info : compiler_info_) {
    (*ss) << "key: " << info.first;
    (*ss) << "\n";
    if (info.second->disabled()) {
      (*ss) << "disabled ";
    }
    (*ss) << "state=" << info.second;
    (*ss) << " cnt=" << info.second->refcnt();
    (*ss) << " used=" << info.second->used();
    (*ss) << "\n";
    (*ss) << info.second->info().DebugString() << "\n";
  }
}

// Dump compiler itself information (not CompilerInfo).
// For each one compiler, only one entry is dumped.
void CompilerInfoCache::DumpCompilersJSON(Json::Value* json) {
  AUTO_SHARED_LOCK(lock, &mu_);

  // Dumping whole CompilerInfoData could be too large, and
  // it is not compiler itself information but CompilerInfo.
  // So, we extract a few fields from CompilerInfoData.

  Json::Value arr(Json::arrayValue);

  absl::flat_hash_set<std::string> used;
  for (const auto& info : compiler_info_) {
    const CompilerInfoData& data = info.second->info().data();

    // Check local_compiler_path so that the same compiler does not appear
    // twice.
    if (used.contains(data.local_compiler_path())) {
      continue;
    }
    used.insert(data.local_compiler_path());

    Json::Value value;
    value["name"] = data.name();
    value["version"] = data.version();
    value["target"] = data.target();

    value["local_compiler_path"] = data.local_compiler_path();
    value["local_compiler_hash"] = data.local_compiler_hash();

    value["real_compiler_path"] = data.real_compiler_path();
    value["real_compiler_hash"] = data.hash();  // hash() is real compiler hash.

    arr.append(std::move(value));
  }

  (*json)["compilers"] = std::move(arr);
}

bool CompilerInfoCache::HasCompilerMismatch() const {
  AUTO_SHARED_LOCK(lock, &mu_);
  for (const auto& info : compiler_info_) {
    if (info.second->disabled())
      return true;
  }
  return false;
}

int CompilerInfoCache::NumStores() const {
  AUTO_SHARED_LOCK(lock, &mu_);
  return num_stores_;
}

int CompilerInfoCache::NumStoreDups() const {
  AUTO_SHARED_LOCK(lock, &mu_);
  return num_store_dups_;
}

int CompilerInfoCache::NumMiss() const {
  AUTO_SHARED_LOCK(lock, &mu_);
  return num_miss_;
}

int CompilerInfoCache::NumFail() const {
  AUTO_SHARED_LOCK(lock, &mu_);
  return num_fail_;
}

int CompilerInfoCache::NumUsed() const {
  AUTO_SHARED_LOCK(lock, &mu_);
  int used = 0;
  for (const auto& info : compiler_info_) {
    CompilerInfoState* cis = info.second;
    if (cis->info().last_used_at() > loaded_timestamp_) {
      used++;
    }
  }
  return used;
}

int CompilerInfoCache::Count() const {
  AUTO_SHARED_LOCK(lock, &mu_);
  return compiler_info_.size();
}

int CompilerInfoCache::LoadedSize() const {
  AUTO_SHARED_LOCK(lock, &mu_);
  return loaded_size_;
}

void CompilerInfoCache::SetValidator(CompilerInfoValidator* validator) {
  CHECK(validator);
  AUTO_EXCLUSIVE_LOCK(lock, &mu_);
  validator_.reset(validator);
}

bool CompilerInfoCache::CompilerInfoValidator::Validate(
    const CompilerInfo& compiler_info,
    const std::string& local_compiler_path) {
  return compiler_info.IsUpToDate(local_compiler_path);
}

/* static */
std::string CompilerInfoCache::HashKey(const CompilerInfoData& data) {
  std::string serialized;
  data.SerializeToString(&serialized);
  std::string hash;
  ComputeDataHashKey(serialized, &hash);
  return hash;
}

bool CompilerInfoCache::Load() {
  AUTO_EXCLUSIVE_LOCK(lock, &mu_);

  LOG(INFO) << "loading from " << cache_file_.filename();

  CompilerInfoDataTable table;
  if (!cache_file_.Load(&table)) {
    LOG(ERROR) << "failed to load cache file " << cache_file_.filename();
    loaded_timestamp_ = absl::Now();
    return false;
  }

  UnmarshalUnlocked(table);
  if (table.built_revision() != kBuiltRevisionString) {
    LOG(WARNING) << "loaded from " << cache_file_.filename()
                 << " mismatch built_revision: got=" << table.built_revision()
                 << " want=" << kBuiltRevisionString;
    ClearUnlocked();
    loaded_timestamp_ = absl::Now();
    return false;
  }

  loaded_size_ = table.ByteSize();

  LOG(INFO) << "loaded from " << cache_file_.filename()
            << " loaded size " << loaded_size_;

  UpdateOlderCompilerInfoUnlocked();
  loaded_timestamp_ = absl::Now();
  return true;
}

void CompilerInfoCache::UpdateOlderCompilerInfo() {
  AUTO_EXCLUSIVE_LOCK(lock, &mu_);
  UpdateOlderCompilerInfoUnlocked();
}

void CompilerInfoCache::UpdateOlderCompilerInfoUnlocked() {
  // Check CompilerInfo validity. Obsolete CompilerInfo will be removed.
  // Since calculating sha256 is slow, we need cache. Otherwise, we will
  // need more than 2 seconds to check.
  std::vector<std::string> keys_to_remove;
  const absl::Time now = absl::Now();
  for (const auto& entry : compiler_info_) {
    const std::string& key = entry.first;
    CompilerInfoState* state = entry.second;

    const std::string& abs_local_compiler_path =
        state->compiler_info_->abs_local_compiler_path();

    // if the cache is not used recently, we do not reuse it.
    absl::Duration time_diff = now - state->info().last_used_at();
    if (time_diff > cache_holding_time_) {
      LOG(INFO) << "evict old cache: " << abs_local_compiler_path
                << " last used at: "
                << time_diff / (60 * 60 * 24)
                << " days ago";
      keys_to_remove.push_back(key);
      continue;
    }

    if (validator_->Validate(state->info(), abs_local_compiler_path)) {
      LOG(INFO) << "valid compiler: " << abs_local_compiler_path
                << " time_diff=" << time_diff;
      continue;
    }

    if (state->compiler_info_->UpdateFileStatIfHashMatch()) {
      LOG(INFO) << "compiler filestat didn't match, but hash matched: "
                << abs_local_compiler_path << " time_diff=" << time_diff;
      continue;
    }

    LOG(INFO) << "compiler outdated: " << abs_local_compiler_path
              << " time_diff=" << time_diff;
    keys_to_remove.push_back(key);
  }

  for (const auto& key : keys_to_remove) {
    LOG(INFO) << "Removing outdated compiler: " << key;
    auto it = compiler_info_.find(key);
    if (it != compiler_info_.end()) {
      it->second->Deref();
      compiler_info_.erase(it);
    }
  }
}

bool CompilerInfoCache::Unmarshal(const CompilerInfoDataTable& table) {
  AUTO_EXCLUSIVE_LOCK(lock, &mu_);
  return UnmarshalUnlocked(table);
}

bool CompilerInfoCache::UnmarshalUnlocked(const CompilerInfoDataTable& table) {
  for (const auto& it : table.compiler_info_data()) {
    auto keys = absl::make_unique<absl::flat_hash_set<std::string>>();
    for (const auto& key : it.keys()) {
      keys->insert(key);
    }
    const CompilerInfoData& data = it.data();
    if (data.language_extension_case() ==
        CompilerInfoData::LANGUAGE_EXTENSION_NOT_SET) {
      // No langauge extension. Ignore this entry.
      continue;
    }
    std::unique_ptr<CompilerInfoData> cid(new CompilerInfoData);
    *cid = data;
    const std::string& hash = HashKey(*cid);
    // http://b/188682224
    if (!cid->found() || cid->error_message() != "") {
      LOG(INFO) << "ignore negative entry " << cid->DebugString();
      continue;
    }
    ScopedCompilerInfoState state(new CompilerInfoState(std::move(cid)));
    for (const auto& key : *keys) {
      compiler_info_.insert(std::make_pair(key, state.get()));
      state.get()->Ref();
    }
    keys_by_hash_.emplace(hash, std::move(keys));
  }
  // TODO: can be void?
  return true;
}

bool CompilerInfoCache::Save() {
  if (!cache_file_.Enabled()) {
    return true;
  }

  LOG(INFO) << "saving to " << cache_file_.filename();

  CompilerInfoDataTable table;
  if (!Marshal(&table)) {
    return false;
  }

  if (!cache_file_.Save(table)) {
    LOG(ERROR) << "failed to save cache file " << cache_file_.filename();
    return false;
  }
  LOG(INFO) << "saved to " << cache_file_.filename();
  return true;
}

bool CompilerInfoCache::Marshal(CompilerInfoDataTable* table) {
  AUTO_SHARED_LOCK(lock, &mu_);
  return MarshalUnlocked(table);
}

bool last_used_desc(const CompilerInfoDataTable::Entry& a,
                    const CompilerInfoDataTable::Entry& b) {
  return a.data().last_used_at() > b.data().last_used_at();
}

/* static */
void CompilerInfoCache::LimitTableEntries(CompilerInfoDataTable* table,
                                          int num_entries) {
  absl::c_sort(*table->mutable_compiler_info_data(), last_used_desc);
  if (table->compiler_info_data_size() > num_entries) {
    LOG(INFO) << "Too many compiler_info_data entries="
              << table->compiler_info_data_size() << " truncate to "
              << num_entries;
    table->mutable_compiler_info_data()->DeleteSubrange(
        num_entries, table->compiler_info_data_size() - num_entries);
  }
}

bool CompilerInfoCache::MarshalUnlocked(CompilerInfoDataTable* table) {
  absl::flat_hash_map<std::string, CompilerInfoDataTable::Entry*> by_hash;
  for (const auto& it : compiler_info_) {
    const std::string& info_key = it.first;
    CompilerInfoState* state = it.second;
    if (state->disabled()) {
      continue;
    }
    const CompilerInfoData& data = state->info().data();
    // http://b/188682224
    if (!data.found() || data.error_message() != "") {
      LOG(INFO) << "ignore negative entry " << data.DebugString();
      continue;
    }
    std::string hash = HashKey(data);
    CompilerInfoDataTable::Entry* entry = nullptr;
    auto p = by_hash.insert(std::make_pair(hash, entry));
    if (p.second) {
      p.first->second = table->add_compiler_info_data();
      p.first->second->mutable_data()->CopyFrom(data);
    }
    entry = p.first->second;
    entry->add_keys(info_key);
  }
  LimitTableEntries(table, max_num_entries_);
  table->set_built_revision(kBuiltRevisionString);
  // TODO: can be void?
  return true;
}

}  // namespace devtools_goma
