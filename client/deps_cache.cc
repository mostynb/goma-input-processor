// Copyright 2014 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "deps_cache.h"

#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_join.h"
#include "absl/time/clock.h"
#include "autolock_timer.h"
#include "compiler_flags.h"
#include "compiler_info.h"
#include "compiler_proxy_info.h"
#include "compiler_specific.h"
#include "content.h"
#include "cxx/cxx_compiler_info.h"
#include "cxx/include_processor/directive_filter.h"
#include "cxx/include_processor/include_cache.h"
#include "gcc_flags.h"
#include "goma_hash.h"
#include "path.h"
#include "path_resolver.h"
#include "proto_util.h"
#include "util.h"
#include "vc_flags.h"

MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "client/deps_cache_data.pb.h"
#include "lib/goma_stats.pb.h"
MSVC_POP_WARNING()

namespace {

template<typename Flags>
void AppendCompilerFlagsInfo(const Flags& flags, std::stringstream* ss) {
  (*ss) << ":include_dirs=";
  for (const auto& path : flags.include_dirs()) {
    (*ss) << path << ',';
  }
  (*ss) << ":commandline_macros=";
  for (const auto& key_value : flags.commandline_macros()) {
    (*ss) << key_value.first << ',' << key_value.second << ',';
  }
  (*ss) << ":compiler_info_flags=";
  for (const auto& flag : flags.compiler_info_flags()) {
    (*ss) << flag << ',';
  }
}

}  // anonymous namespace

namespace devtools_goma {

DepsCache* DepsCache::instance_;

DepsCache::DepsCache(const std::string& cache_filename,
                     absl::optional<absl::Duration> identifier_alive_duration,
                     int deps_table_size_threshold,
                     int max_proto_size_in_mega_bytes)
    : cache_file_(cache_filename),
      identifier_alive_duration_(identifier_alive_duration),
      deps_table_size_threshold_(deps_table_size_threshold),
      max_proto_size_in_mega_bytes_(max_proto_size_in_mega_bytes),
      hit_count_(0),
      missed_count_(0),
      missed_by_updated_count_(0) {}

DepsCache::~DepsCache() {}

// static
void DepsCache::Init(const std::string& cache_filename,
                     absl::optional<absl::Duration> identifier_alive_duration,
                     int deps_table_size_threshold,
                     int max_proto_size_in_mega_bytes) {
  if (cache_filename.empty()) {
    LOG(INFO) << "DepsCache is disabled.";
    return;
  }

  if (!IncludeCache::IsEnabled()) {
    LOG(WARNING) << "DepsCache is disabled since IncludeCache is not enabled.";
    return;
  }
  if (!IncludeCache::instance()->calculates_directive_hash()) {
    LOG(WARNING) << "DepsCache is disabeld since IncludeCache does not "
                 << "calculate directive hash. Enable IncludeCache with "
                 << "directive hash calculation";
    return;
  }

  LOG(INFO) << "DepsCache is enabled. cache_filename=" << cache_filename;
  instance_ = new DepsCache(cache_filename, identifier_alive_duration,
                            deps_table_size_threshold,
                            max_proto_size_in_mega_bytes);
}

// static
void DepsCache::LoadIfEnabled() {
  if (!instance_) {
    return;
  }
  if (!instance_->LoadGomaDeps()) {
    // If deps cache is broken (or does not exist), clear all cache.
    LOG(INFO) << "couldn't load deps cache file. "
              << "The cache file is broken or too large";
  }
}

// static
void DepsCache::Quit() {
  if (!IsEnabled())
    return;

  instance_->SaveGomaDeps();
  delete instance_;
  instance_ = nullptr;
}

void DepsCache::Clear() {
  {
    AUTO_EXCLUSIVE_LOCK(lock, &mu_);
    deps_table_.clear();
  }

  filename_id_table_.Clear();
  file_stat_id_table_.Clear();
  directive_hash_id_table_.Clear();

  {
    AUTOLOCK(lock, &count_mu_);
    hit_count_ = 0;
    missed_by_updated_count_ = 0;
    missed_count_ = 0;
  }
}

bool DepsCache::SetDependencies(const DepsCache::Identifier& identifier,
                                const std::string& cwd,
                                const std::string& input_file,
                                const std::set<std::string>& dependencies,
                                FileStatCache* file_stat_cache) {
  DCHECK(identifier.has_value());
  DCHECK(file::IsAbsolutePath(cwd)) << cwd;

  auto deps_size = dependencies.size();
  if (dependencies.find(input_file) == dependencies.end()) {
    ++deps_size;
  }

  std::vector<DepsHashId> deps_hash_ids;
  deps_hash_ids.reserve(deps_size);

  bool all_ok = true;

  auto process_file = [&](const std::string& filename) {
    DCHECK(!filename.empty());
    const std::string& abs_filename =
        file::JoinPathRespectAbsolute(cwd, filename);

    FilenameIdTable::Id id = filename_id_table_.InsertFilename(filename);
    if (id == FilenameIdTable::kInvalidId) {
      all_ok = false;
      return;
    }

    FileStat file_stat(file_stat_cache->Get(abs_filename));
    if (!file_stat.IsValid()) {
      all_ok = false;
      LOG(WARNING) << "invalid file id: " << abs_filename;
      return;
    }

    absl::optional<SHA256HashValue> directive_hash =
        IncludeCache::instance()->GetDirectiveHash(abs_filename, file_stat);
    if (!directive_hash.has_value()) {
      all_ok = false;
      LOG(WARNING) << "invalid directive hash: " << abs_filename;
      return;
    }

    const DepsHashId deps_hash_id(
        id, file_stat_id_table_.GetId(file_stat),
        directive_hash_id_table_.GetId(directive_hash.value()));
    DCHECK(deps_hash_id.IsValid(file_stat_id_table_));
    deps_hash_ids.push_back(deps_hash_id);
  };

  for (const auto& filename : dependencies) {
    process_file(filename);
    if (!all_ok) {
      break;
    }
  }

  // We set input_file as dependency also.
  if (all_ok && dependencies.find(input_file) == dependencies.end()) {
    process_file(input_file);
  }

  AUTO_EXCLUSIVE_LOCK(lock, &mu_);
  if (!all_ok) {
    deps_table_.erase(identifier.value());
    return false;
  }

  auto& deps = deps_table_[identifier.value()];
  deps.last_used_time = absl::ToTimeT(absl::Now());
  deps.deps_hash_ids = std::move(deps_hash_ids);

  return true;
}

bool DepsCache::GetDependencies(const DepsCache::Identifier& identifier,
                                const std::string& cwd,
                                const std::string& input_file,
                                std::set<std::string>* dependencies,
                                FileStatCache* file_stat_cache) {
  DCHECK(identifier.has_value());
  DCHECK(file::IsAbsolutePath(cwd)) << cwd;

  std::vector<DepsHashId> deps_hash_ids;
  {
    AUTO_SHARED_LOCK(lock, &mu_);
    auto it = deps_table_.find(identifier.value());
    if (it == deps_table_.end()) {
      IncrMissedCount();
      return false;
    }
    it->second.last_used_time = absl::ToTimeT(absl::Now());
    deps_hash_ids = it->second.deps_hash_ids;
  }

  std::set<std::string> result;
  for (const auto& deps_hash_id : deps_hash_ids) {
    const std::string& filename =
        filename_id_table_.ToFilename(deps_hash_id.id);
    if (filename.empty()) {
      LOG(ERROR) << "Unexpected FilenameIdTable conversion failure: "
                 << "id=" << deps_hash_id.id;
      IncrMissedCount();
      return false;
    }

    if (IsDirectiveModified(
            file::JoinPathRespectAbsolute(cwd, filename),
            file_stat_id_table_.GetValue(deps_hash_id.file_stat_id),
            directive_hash_id_table_.GetValue(deps_hash_id.directive_hash_id),
            file_stat_cache)) {
      IncrMissedByUpdatedCount();
      return false;
    }

    result.insert(filename);
  }

  // We don't add input_file in dependencies.
  result.erase(input_file);

  std::swap(*dependencies, result);
  IncrHitCount();
  return true;
}

void DepsCache::RemoveDependency(const DepsCache::Identifier& identifier) {
  DCHECK(identifier.has_value());

  AUTO_EXCLUSIVE_LOCK(lock, &mu_);
  deps_table_.erase(identifier.value());
}

void DepsCache::IncrMissedCount() {
  AUTOLOCK(lock, &count_mu_);
  ++missed_count_;
}

void DepsCache::IncrMissedByUpdatedCount() {
  AUTOLOCK(lock, &count_mu_);
  ++missed_by_updated_count_;
}

void DepsCache::IncrHitCount() {
  AUTOLOCK(lock, &count_mu_);
  ++hit_count_;
}

void DepsCache::DumpStatsToProto(DepsCacheStats* stat) const {
  {
    AUTO_SHARED_LOCK(lock, &mu_);
    stat->set_deps_table_size(deps_table_.size());
    size_t max_entries = 0;
    size_t total_entries = 0;
    for (const auto& entry : deps_table_) {
      size_t size = entry.second.deps_hash_ids.size();
      total_entries += size;
      max_entries = std::max(max_entries, size);
    }
    stat->set_max_entries(max_entries);
    stat->set_total_entries(total_entries);
  }
  stat->set_idtable_size(filename_id_table_.Size());
  {
    AUTOLOCK(lock, &count_mu_);
    stat->set_hit(hit_count_);
    stat->set_updated(missed_by_updated_count_);
    stat->set_missed(missed_count_);
  }
}

// static
bool DepsCache::IsDirectiveModified(const std::string& filename,
                                    const FileStat& old_file_stat,
                                    const SHA256HashValue& old_directive_hash,
                                    FileStatCache* file_stat_cache) {
  FileStat file_stat(file_stat_cache->Get(filename));

  if (!file_stat.IsValid()) {
    // When file doesn't exist, let's consider a directive is changed.
    return true;
  }
  if (file_stat == old_file_stat)
    return false;

  absl::optional<SHA256HashValue> directive_hash =
      IncludeCache::instance()->GetDirectiveHash(filename, file_stat);
  if (!directive_hash.has_value()) {
    // The file couldn't be read or the file is removed during the build.
    LOG(ERROR) << "couldn't read a file in deps: " << filename;
    return true;
  }
  if (directive_hash.value() == old_directive_hash) {
    return false;
  }

  return true;
}

bool DepsCache::UpdateLastUsedTime(const Identifier& identifier,
                                   absl::optional<absl::Time> last_used_time) {
  AUTO_SHARED_LOCK(lock, &mu_);
  auto it = deps_table_.find(identifier.value());
  if (it == deps_table_.end())
    return false;

  it->second.last_used_time =
      last_used_time.has_value() ? absl::ToTimeT(*last_used_time) : 0;
  return true;
}

bool DepsCache::GetDepsHashId(const Identifier& identifier,
                              const FilenameIdTable::Id& id,
                              DepsHashId* deps_hash_id) const {
  AUTO_SHARED_LOCK(lock, &mu_);
  auto it = deps_table_.find(identifier.value());
  if (it == deps_table_.end())
    return false;

  for (const auto& dhi : it->second.deps_hash_ids) {
    if (dhi.id == id) {
      *deps_hash_id = dhi;
      return true;
    }
  }

  return false;
}

bool DepsCache::LoadGomaDeps() {
  absl::optional<absl::Time> time_threshold;
  if (identifier_alive_duration_.has_value()) {
    time_threshold = absl::Now() - *identifier_alive_duration_;
  }
  GomaDeps goma_deps;

  int total_bytes_limit = max_proto_size_in_mega_bytes_ * 1024 * 1024;
  if (max_proto_size_in_mega_bytes_ < 0) {
    // use the default limit.
    // See:
    // https://developers.google.com/protocol-buffers/docs/reference/cpp/google.protobuf.io.coded_stream#CodedInputStream.SetTotalBytesLimit.details
    total_bytes_limit = -1;
  }

  if (!cache_file_.LoadWithMaxLimit(&goma_deps, total_bytes_limit)) {
    LOG(ERROR) << "failed to load cache file " << cache_file_.filename();
    Clear();
    SetLoaded();
    return false;
  }

  // Version check

  // Version mismatch. Older deps won't be reused.
  if (goma_deps.built_revision() != kBuiltRevisionString) {
    LOG(INFO) << "Old deps cache was detected. This deps cache is ignored. "
              << "Current version should be " << kBuiltRevisionString
              << " but deps cache version is " << goma_deps.built_revision();
    Clear();
    SetLoaded();
    return false;
  }

  LOG(INFO) << "Version matched.";

  // Load FilenameIdTable
  absl::flat_hash_set<FilenameIdTable::Id> valid_ids;
  if (!filename_id_table_.LoadFrom(goma_deps.filename_id_table(), &valid_ids)) {
    Clear();
    SetLoaded();
    return false;
  }

  // Load DepsIdTable
  absl::flat_hash_map<FilenameIdTable::Id, std::pair<FileStat, std::string>>
      deps_hash_id_map;
  {
    const GomaDepsIdTable& table = goma_deps.deps_id_table();
    deps_hash_id_map.reserve(table.record_size());
    for (const auto& record : table.record()) {
      if (!valid_ids.count(record.filename_id())) {
        LOG(ERROR) << "DepsIdTable contains unexpected filename_id: "
                   << record.filename_id();
        Clear();
        SetLoaded();
        return false;
      }

      if (deps_hash_id_map.contains(record.filename_id())) {
        LOG(ERROR) << "DepsIdTable contains duplicated filename_id: "
                   << record.filename_id();
        Clear();
        SetLoaded();
        return false;
      }

      FileStat file_stat;
      if (record.has_mtime_ts()) {
        file_stat.mtime = ProtoToTime(record.mtime_ts());
        file_stat.size = record.size();
      }

      deps_hash_id_map[record.filename_id()] =
          std::make_pair(file_stat, record.directive_hash());
    }
  }

  LOG(INFO) << "Loading DepsTable OK.";

  // Load Dependencies
  AUTO_EXCLUSIVE_LOCK(lock, &mu_);
  {
    const GomaDependencyTable& table = goma_deps.dependency_table();
    deps_table_.reserve(table.record_size());
    for (const auto& record : table.record()) {
      const absl::Time record_last_used_time =
          absl::FromTimeT(record.last_used_time());
      if (time_threshold.has_value() &&
          record_last_used_time < *time_threshold) {
        continue;
      }

      Key key;
      if (!SHA256HashValue::ConvertFromHexString(record.identifier(),
                                                 &key)) {
        LOG(ERROR) << "DependecyTable contains corrupted sha256 string: "
                   << record.identifier();
        Clear();
        SetLoaded();
        return false;
      }

      if (deps_table_.count(key)) {
        LOG(ERROR) << "DependencyTable contains duplicated identifier: "
                   << record.identifier();
        Clear();
        SetLoaded();
        return false;
      }

      DepsTableData* deps_table_data = &deps_table_[key];
      deps_table_data->last_used_time = record.last_used_time();
      deps_table_data->deps_hash_ids.reserve(record.filename_id_size());
      for (const auto& id : record.filename_id()) {
        if (!valid_ids.count(id)) {
          LOG(ERROR) << "DependencyTable contains unexpected filename_id: "
                     << id;
          Clear();
          SetLoaded();
          return false;
        }

        const auto& hashid = deps_hash_id_map[id];
        SHA256HashValue hash_value;
        if (!SHA256HashValue::ConvertFromHexString(hashid.second,
                                                   &hash_value)) {
          LOG(ERROR) << "DependencyTable contains corrupted sha256 string: "
                     << hashid.second;
          Clear();
          SetLoaded();
          return false;
        }
        deps_table_data->deps_hash_ids.push_back(
            DepsHashId(id, file_stat_id_table_.GetId(hashid.first),
                       directive_hash_id_table_.GetId(hash_value)));
      }
    }
    SetLoaded();
  }

  LOG(INFO) << cache_file_.filename() << " has been successfully loaded.";

  return true;
}

bool DepsCache::SaveGomaDeps() {
  AUTO_SHARED_LOCK(lock, &mu_);

  GomaDeps goma_deps;
  goma_deps.set_built_revision(kBuiltRevisionString);

  // First, drop older DepsTable entry from deps_table_.
  if (identifier_alive_duration_.has_value()) {
    absl::Time time_threshold = absl::Now() - *identifier_alive_duration_;
    for (auto it = deps_table_.begin(); it != deps_table_.end(); ) {
      if (absl::FromTimeT(it->second.last_used_time) < time_threshold) {
        // should be OK since all iterators but deleted one keep valid.
        deps_table_.erase(it++);
      } else {
        ++it;
      }
    }
  }

  // Checks the size of DepsTable. If it exceeds threshold, we'd like to remove
  // older identifiers.
  if (deps_table_size_threshold_ >= 0 &&
      deps_table_.size() > deps_table_size_threshold_) {
    LOG(INFO) << "DepsTable size " << deps_table_.size()
              << " exceeds the threshold " << deps_table_size_threshold_
              << ". Older cache will be deleted";
    std::vector<std::pair<absl::Time, Key>> keys_by_time;
    keys_by_time.reserve(deps_table_.size());
    for (const auto& entry : deps_table_) {
      keys_by_time.emplace_back(
          absl::FromTimeT(entry.second.last_used_time.load()), entry.first);
    }
    std::sort(keys_by_time.begin(), keys_by_time.end(),
         std::greater<std::pair<absl::Time, Key>>());
    for (size_t i = deps_table_size_threshold_; i < keys_by_time.size(); ++i) {
      deps_table_.erase(keys_by_time[i].second);
    }
  }

  // We create a map:
  //   FilenameIdTable::Id -> pair<FileStat, directive-hash id>.
  // When we saw multiple DepsHashId for one FilenameIdTable::Id,
  // we choose the one whose mtime is the latest.
  absl::flat_hash_map<FilenameIdTable::Id,
                      std::pair<FileStat, DirectiveHashIDTable::Id>>
      m;
  for (const auto& deps_table_entry : deps_table_) {
    for (const auto& deps_hash_id : deps_table_entry.second.deps_hash_ids) {
      FilenameIdTable::Id id = deps_hash_id.id;
      const auto file_stat =
          file_stat_id_table_.GetValue(deps_hash_id.file_stat_id);
      if (!m.contains(id) || m[id].first.mtime < file_stat.mtime) {
        m[id] = std::make_pair(file_stat, deps_hash_id.directive_hash_id);
      }
    }
  }

  // Store all ids which have been saved. We only save these ids.
  std::set<FilenameIdTable::Id> used_ids;

  // Save DepsHashIdTable. We remove records whose directive_hash is not the
  // same one in |m|, because it's old. In that case, we need to
  // recalculate deps cache at all next time, so it's no worth to save them.
  {
    GomaDependencyTable* table = goma_deps.mutable_dependency_table();
    for (const auto& deps_table_entry : deps_table_) {
      // First, check all the deps_table_entry are valid.
      bool ok = true;
      for (const auto& deps_hash_id : deps_table_entry.second.deps_hash_ids) {
        if (deps_hash_id.directive_hash_id != m[deps_hash_id.id].second) {
          ok = false;
          break;
        }
      }

      if (!ok)
        continue;

      GomaDependencyTableRecord* record = table->add_record();
      record->set_identifier(deps_table_entry.first.ToHexString());
      record->set_last_used_time(deps_table_entry.second.last_used_time);
      for (const auto& deps_hash_id : deps_table_entry.second.deps_hash_ids) {
        used_ids.insert(deps_hash_id.id);
        record->add_filename_id(deps_hash_id.id);
      }
    }
  }

  // Save GomaDepsIdTable
  {
    GomaDepsIdTable* table = goma_deps.mutable_deps_id_table();
    for (const auto& entry : m) {
      if (!used_ids.count(entry.first))
        continue;
      GomaDepsIdTableRecord* record = table->add_record();
      record->set_filename_id(entry.first);

      if (entry.second.first.IsValid()) {
        *record->mutable_mtime_ts() = TimeToProto(*entry.second.first.mtime);
      }

      record->set_size(entry.second.first.size);
      record->set_directive_hash(
          directive_hash_id_table_.GetValue(entry.second.second).ToHexString());
    }
  }

  // Save FilenameIdTable. We remove id which does not appear in |used_ids|,
  // because no one will refer it.
  filename_id_table_.SaveTo(used_ids, goma_deps.mutable_filename_id_table());

  if (!cache_file_.Save(goma_deps)) {
    LOG(ERROR) << "failed to save cache file " << cache_file_.filename();
    return false;
  }
  LOG(INFO) << "saved to " << cache_file_.filename();
  return true;
}

// static
DepsCache::Identifier DepsCache::MakeDepsIdentifier(
    const CompilerInfo& compiler_info,
    const CompilerFlags& compiler_flags) {
  // TODO: Support javac.
  if (compiler_info.type() != CompilerInfoType::Cxx) {
    LOG(INFO) << "Only CxxCompilerInfo is supported: type="
              << compiler_info.type();
    return DepsCache::Identifier();
  }

  const CxxCompilerInfo& cxxci = ToCxxCompilerInfo(compiler_info);

  std::stringstream ss;

  // TODO: Maybe we need to merge some code with IncludeProcessor
  // to enumerate what information is necessary for enumerating headers?

  ss << "compiler_name=" << cxxci.name();
  ss << ":compiler_path=" << cxxci.real_compiler_path();

  // Some buildbot always copies nacl-gcc compiler to target directory.
  // In that case, FileStat is different per build. So, we'd like to use
  // compiler hash.
  ss << ":compiler_hash=" << cxxci.real_compiler_hash();

  ss << ":cwd=" << compiler_flags.cwd();

  ss << ":input=";
  for (const auto& filename : compiler_flags.input_filenames()) {
    ss << filename << ',';
  }

  ss << ":cxx_system_include_paths=";
  for (const auto& path : cxxci.cxx_system_include_paths()) {
    ss << path << ",";
  }
  ss << ":system_include_paths=";
  for (const auto& path : cxxci.system_include_paths()) {
    ss << path << ",";
  }
  ss << ":system_framework_paths=";
  for (const auto& path : cxxci.system_framework_paths()) {
    ss << path << ",";
  }
  ss << ":predefined_macros=" << cxxci.predefined_macros();

  if (compiler_flags.type() == CompilerFlagType::Gcc) {
    const GCCFlags& flags = static_cast<const GCCFlags&>(compiler_flags);
    AppendCompilerFlagsInfo(flags, &ss);
  } else if (compiler_flags.type() == CompilerFlagType::Clexe) {
    const VCFlags& flags = static_cast<const VCFlags&>(compiler_flags);
    AppendCompilerFlagsInfo(flags, &ss);
  } else {
    // TODO: Support javac.
    LOG(INFO) << "Cannot handle this CompilerFlags yet: "
              << compiler_flags.compiler_name();
    return DepsCache::Identifier();
  }

  SHA256HashValue value;
  ComputeDataHashKeyForSHA256HashValue(ss.str(), &value);
  return DepsCache::Identifier(value);
}

}  // namespace devtools_goma
