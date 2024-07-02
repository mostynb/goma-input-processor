// Copyright 2011 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "compile_task.h"

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <algorithm>
#include <iomanip>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <google/protobuf/text_format.h>
#include <json/json.h>

#include "absl/algorithm/container.h"
#include "absl/base/call_once.h"
#include "absl/base/macros.h"
#include "absl/memory/memory.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/time/clock.h"
#include "autolock_timer.h"
#include "callback.h"
#include "clang_tidy_flags.h"
#include "compile_service.h"
#include "compile_stats.h"
#include "compiler_flag_type_specific.h"
#include "compiler_flags.h"
#include "compiler_flags_parser.h"
#include "compiler_flags_util.h"
#include "compiler_info.h"
#include "compiler_proxy_info.h"
#include "compiler_specific.h"
#include "compiler_type_specific_collection.h"
#include "cxx/include_processor/cpp_include_processor.h"
#include "cxx/include_processor/include_file_utils.h"
#include "file_data_output.h"
#include "file_dir.h"
#include "file_hash_cache.h"
#include "file_helper.h"
#include "file_path_util.h"
#include "filesystem.h"
#include "gcc_flags.h"
#include "glog/logging.h"
#include "glog/stl_logging.h"
#include "goma_blob.h"
#include "goma_data_util.h"
#include "goma_file.h"
#include "goma_file_dump.h"
#include "goma_file_http.h"
#include "http_rpc.h"
#include "ioutil.h"
#include "java/jar_parser.h"
#include "java_flags.h"
#include "local_output_cache.h"
#include "lockhelper.h"
#include "multi_http_rpc.h"
#include "mypath.h"
#include "options.h"
#include "path.h"
#include "path_resolver.h"
#include "path_util.h"
#include "rand_util.h"
#include "rpc_controller.h"
#include "simple_timer.h"
#include "subprocess_task.h"
#include "task/compiler_flag_utils.h"
#include "task/input_file_task.h"
#include "task/local_output_file_task.h"
#include "task/output_file_task.h"
#include "time_util.h"
#include "util.h"
#include "vc_flags.h"
#include "worker_thread.h"

MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "client/subprocess.pb.h"
#include "lib/goma_data.pb.h"
MSVC_POP_WARNING()

#ifdef _WIN32
# include "posix_helper_win.h"
#endif

namespace devtools_goma {

namespace {

constexpr int kMaxExecRetry = 4;

std::string GetLastErrorMessage() {
  char error_message[1024];
#ifndef _WIN32
  // Meaning of returned value of strerror_r is different between
  // XSI and GNU. Need to ignore.
  (void)strerror_r(errno, error_message, sizeof(error_message));
#else
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, 0, GetLastError(), 0,
                 error_message, sizeof error_message, 0);
#endif
  return error_message;
}

bool IsFatalError(ExecResp::ExecError error_code) {
  return error_code == ExecResp::BAD_REQUEST;
}

void DumpSubprograms(
    const google::protobuf::RepeatedPtrField<SubprogramSpec>& subprogram_specs,
    std::ostringstream* ss) {
  for (int i = 0; i < subprogram_specs.size(); ++i) {
    const SubprogramSpec& spec = subprogram_specs.Get(i);
    if (i > 0)
      *ss << ", ";
    *ss << "path=" << spec.path() << " hash=" << spec.binary_hash();
  }
}

void LogCompilerOutput(const std::string& trace_id,
                       const std::string& name,
                       absl::string_view out) {
  LOG(INFO) << trace_id << " " << name << ": size=" << out.size();
  static const int kMaxLines = 32;
  static const size_t kMaxCols = 512;
  static const char* kClExeShowIncludePrefix = "Note: including file:";
  if (out.size() == 0)
    return;
  if (out.size() < kMaxCols) {
    LOG(INFO) << trace_id << " " << name << ":" << out;
    return;
  }
  for (int i = 0; out.size() > 0 && i < kMaxLines;) {
    size_t end = out.find_first_of("\r\n");
    absl::string_view line;
    if (end == std::string::npos) {
      line = out;
      out = absl::string_view();
    } else if (end == 0) {
      out.remove_prefix(1);
      continue;
    } else {
      line = out.substr(0, end);
      out.remove_prefix(end + 1);
    }
    if (line.size() == 0)
      continue;
    if (absl::StartsWith(line, kClExeShowIncludePrefix))
      continue;
    size_t found = line.find("error");
    if (found == std::string::npos)
      found = line.find("warning");
    if (found != std::string::npos) {
      ++i;
      if (line.size() > kMaxCols) {
        LOG(INFO) << trace_id << " " << name << ":"
                  << line.substr(0, kMaxCols) << "...";
      } else {
        LOG(INFO) << trace_id << " " << name << ":" << line;
      }
    }
  }
}

void ReleaseMemoryForExecReqInput(ExecReq* req) {
  ExecReq new_req;
  new_req.Swap(req);
  new_req.clear_input();
  *req = new_req;
}

std::string CreateCommandVersionString(const CommandSpec& spec) {
  return spec.name() + ' ' + spec.version() + " (" + spec.binary_hash() + ")";
}

std::string StateName(CompileTask::State state) {
  static const char* names[] = {
    "INIT",
    "SETUP",
    "FILE_REQ",
    "CALL_EXEC",
    "LOCAL_OUTPUT",
    "FILE_RESP",
    "FINISHED",
    "LOCAL_RUN",
    "LOCAL_FINISHED",
  };

  static_assert(
      CompileTask::NUM_STATE == ABSL_ARRAYSIZE(names),
      "CompileTask::NUM_STATE and ABSL_ARRAYSIZE(names) is not matched");

  CHECK_GE(state, 0);
  CHECK_LT(state, CompileTask::NUM_STATE);
  return names[state];
}

template <typename Iter>
void NormalizeSystemIncludePaths(const std::string& home,
                                 const std::string& cwd,
                                 Iter path_begin,
                                 Iter path_end) {
  if (home.empty())
    return;

  for (Iter it = path_begin; it != path_end; ++it) {
    if (HasPrefixDir(*it, home)) {
      it->assign(PathResolver::WeakRelativePath(*it, cwd));
    }
  }
}

// Returns true if |buf| is bigobj format header.
// |buf| should contain 32 byte at least.
bool IsBigobjFormat(const unsigned char* buf) {
  static const unsigned char kV1UUID[16] = {
    0x38, 0xFE, 0xB3, 0x0C, 0xA5, 0xD9, 0xAB, 0x4D,
    0xAC, 0x9B, 0xD6, 0xB6, 0x22, 0x26, 0x53, 0xC2,
  };

  static const unsigned char kV2UUID[16] = {
    0xC7, 0xA1, 0xBA, 0xD1, 0xEE, 0xBA, 0xA9, 0x4B,
    0xAF, 0x20, 0xFA, 0xF6, 0x6A, 0xA4, 0xDC, 0xB8
  };

  if (*reinterpret_cast<const unsigned short*>(buf) != 0)
    return false;
  if (*reinterpret_cast<const unsigned short*>(buf + 2) != 0xFFFF)
    return false;

  // UUID can be different by bigobj version.
  const unsigned char* uuid = nullptr;
  if (*reinterpret_cast<const unsigned short*>(buf + 4) == 0x0001) {
    uuid = kV1UUID;
  } else if (*reinterpret_cast<const unsigned short*>(buf + 4) == 0x0002) {
    uuid = kV2UUID;
  } else {
    // Unknown bigobj version
    return false;
  }

  unsigned short magic = *reinterpret_cast<const unsigned short*>(buf + 6);
  if (!(magic == 0x014C || magic == 0x8664))
    return false;

  for (int i = 0; i < 16; ++i) {
    if (buf[12 + i] != uuid[i])
      return false;
  }

  return true;
}

bool IsSameErrorMessage(absl::string_view remote_stdout,
                        absl::string_view local_stdout,
                        absl::string_view remote_stderr,
                        absl::string_view local_stderr) {
  if (remote_stdout == local_stdout && remote_stderr == local_stderr) {
    return true;
  }

  // b/66308332
  // local error message might be merged to stdout.
  // stdout and stderr might be interleaved, but it's not considered.
  if (local_stdout == remote_stderr && local_stderr.empty() &&
      remote_stdout.empty()) {
    return true;
  }

  return false;
}

struct PlatformPropertyFormatter {
  void operator()(std::string* out, const PlatformProperty& p) {
    out->append(p.DebugString());
  }
};

}  // namespace

absl::once_flag CompileTask::init_once_;
Lock CompileTask::global_mu_;

std::deque<CompileTask*>* CompileTask::link_file_req_tasks_ = nullptr;

// Returns true if all outputs are FILE blob (so no need of further http_rpc).
bool IsOutputFileEmbedded(const ExecResult& result) {
  for (const auto& output : result.output()) {
    if (output.blob().blob_type() != FileBlob::FILE)
      return false;
  }
  return true;
}

/* static */
void CompileTask::InitializeStaticOnce() {
  AUTOLOCK(lock, &global_mu_);
  link_file_req_tasks_ = new std::deque<CompileTask*>;
}

CompileTask::CompileTask(CompileService* service, int id)
    : service_(service),
      id_(id),
      caller_thread_id_(service->wm()->GetCurrentThreadId()),
      stats_(new CompileStats),
      req_(new ExecReq),
      resp_(new ExecResp),
      http_rpc_status_(absl::make_unique<HttpRPC::Status>()) {
  thread_id_ = GetCurrentThreadId();
  absl::call_once(init_once_, InitializeStaticOnce);
  Ref();
  std::ostringstream ss;
  ss << "Task:" << id_;
  trace_id_ = ss.str();

  stats_->exec_log.set_start_time(absl::ToTimeT(absl::Now()));
  stats_->exec_log.set_compiler_proxy_user_agent(kUserAgentString);
}

void CompileTask::Ref() {
  AUTOLOCK(lock, &refcnt_mu_);
  refcnt_++;
}

void CompileTask::Deref() {
  int refcnt;
  {
    AUTOLOCK(lock, &refcnt_mu_);
    refcnt_--;
    refcnt = refcnt_;
  }
  if (refcnt == 0) {
    if (deref_cleanup_handler_) {
      deref_cleanup_handler_->OnCleanup(this);
    }
    delete this;
  }
}

void CompileTask::Init(RpcController* rpc,
                       std::unique_ptr<ExecReq> req,
                       ExecResp* resp,
                       OneshotClosure* done) {
  VLOG(1) << trace_id_ << " init";
  CHECK_EQ(INIT, state_);
  CHECK(service_ != nullptr);
  CHECK_EQ(caller_thread_id_, service_->wm()->GetCurrentThreadId());
  rpc_ = rpc;
  rpc_resp_ = resp;
  done_ = done;
  req_ = std::move(req);
#ifdef _WIN32
  pathext_ = GetEnvFromEnvIter(
      req_->env().begin(), req_->env().end(), "PATHEXT", true);
#endif

  requester_info_ = req_->requester_info();

  InitCompilerFlags();
}

void CompileTask::Start() {
  VLOG(1) << trace_id_ << " start";
  CHECK_EQ(INIT, state_);
  const absl::Duration pending_time = handler_timer_.GetDuration();
  stats_->exec_log.set_pending_time(DurationToIntMs(pending_time));
  stats_->pending_time = pending_time;

  // We switched to new thread.
  DCHECK(!BelongsToCurrentThread());
  thread_id_ = GetCurrentThreadId();

  input_file_stat_cache_ = absl::make_unique<FileStatCache>();
  output_file_stat_cache_ = absl::make_unique<FileStatCache>();

  rpc_->NotifyWhenClosed(NewCallback(this, &CompileTask::GomaccClosed));

  int api_version = req_->requester_info().api_version();
  if (api_version != RequesterInfo::CURRENT_VERSION) {
    LOG(ERROR) << trace_id_ << " unexpected api_version=" << api_version
               << " want=" << RequesterInfo::CURRENT_VERSION;
  }
#if defined(ENABLE_REVISION_CHECK)
  if (req_->requester_info().has_goma_revision() &&
      req_->requester_info().goma_revision() != kBuiltRevisionString) {
    LOG(WARNING) << trace_id_ << " goma revision mismatch:"
                 << " gomacc=" << req_->requester_info().goma_revision()
                 << " compiler_proxy=" << kBuiltRevisionString;
    gomacc_revision_mismatched_ = true;
  }
#endif
  CopyEnvFromRequest();

  gomacc_pid_ = requester_info_.pid();

  if (flags_.get() == nullptr) {
    LOG(ERROR) << trace_id_ << " Start error: CompilerFlags is nullptr";
    AddErrorToResponse(TO_USER, "Unsupported command", true);
    ProcessFinished("Unsupported command");
    return;
  }
  if (!IsLocalCompilerPathValid(trace_id_, *req_, flags_->compiler_name())) {
    LOG(ERROR) << trace_id_ << " Start error: invalid local compiler."
               << " path=" << req_->command_spec().local_compiler_path();
    AddErrorToResponse(TO_USER, "Invalid command", true);
    ProcessFinished("Invalid command");
    return;
  }
  if (!flags_->is_successful()) {
    LOG(WARNING) << trace_id_ << " Start error:" << flags_->fail_message();
    // It should fallback.
  } else if (flags_->is_precompiling_header()) {
    LOG(INFO) << trace_id_ << " Start precompile "
              << (flags_->input_filenames().empty()
                      ? "(no input)"
                      : flags_->input_filenames()[0])
              << " gomacc_pid=" << gomacc_pid_
              << " build_dir=" << flags_->cwd();
    if (!flags_->input_filenames().empty() && !flags_->output_files().empty()) {
      DCHECK_EQ(1U, flags_->input_filenames().size()) << trace_id_;
      const std::string& input_filename = file::JoinPathRespectAbsolute(
          flags_->cwd(), flags_->input_filenames()[0]);
      std::string output_filename;
      for (const auto& output_file : flags_->output_files()) {
        if (absl::EndsWith(output_file, ".gch")) {
          int output_filelen = output_file.size();
          // Full path and strip ".gch".
          output_filename =
              file::JoinPathRespectAbsolute(
                  flags_->cwd(),
                  output_file.substr(0, output_filelen - 4));
          break;
        }
      }
      // Copy the header file iff precompiling header to *.gch.
      if (!output_filename.empty()) {
        LOG(INFO) << trace_id_ << " copy " << input_filename
                  << " " << output_filename;
        if (input_filename != output_filename) {
          if (file::Copy(input_filename, output_filename, file::Overwrite())
                  .ok()) {
            VLOG(1) << trace_id_ << " copy ok";
            resp_->mutable_result()->set_exit_status(0);
          } else {
            AddErrorToResponse(TO_USER,
                               "Failed to copy " + input_filename + " to " +
                               output_filename, true);
          }
        }
      } else {
        AddErrorToResponse(TO_LOG, "Precompile to no *.gch output", false);
      }
    }
  } else if (flags_->is_linking()) {
    // build_dir will be used to infer the build directory
    // in `goma_ctl.py report`. See b/25487955.
    LOG(INFO) << trace_id_ << " Start linking "
              << (flags_->output_files().empty() ? "(no output)" :
                  flags_->output_files()[0])
              << " gomacc_pid=" << gomacc_pid_
              << " build_dir=" << flags_->cwd();
  } else {
    // build_dir will be used to infer the build directory
    // in `goma_ctl.py report`. See b/25487955.
    LOG(INFO) << trace_id_ << " Start "
              << (flags_->input_filenames().empty() ? "(no input)" :
                  flags_->input_filenames()[0])
              << " gomacc_pid=" << gomacc_pid_
              << " build_dir=" << flags_->cwd();
  }
  if (!FindLocalCompilerPath()) {
    // Unable to fallback.
    LOG(ERROR) << trace_id_ << " Failed to find local compiler path:"
               << req_->DebugString()
               << " env:" << requester_env_.DebugString();
    AddErrorToResponse(TO_USER, "Failed to find local compiler path", true);
    ProcessFinished("fail to find local compiler");
    return;
  }
  if (requester_info_.has_exec_root() ||
      requester_info_.platform_properties_size() > 0) {
    LOG(INFO) << trace_id_ << " exec_root=" << requester_info_.exec_root()
              << " platform_properties="
              << absl::StrJoin(requester_info_.platform_properties(), ",",
                               PlatformPropertyFormatter());
  }
  VLOG(1) << trace_id_
          << " local_compiler:" << req_->command_spec().local_compiler_path();
  local_compiler_path_ = req_->command_spec().local_compiler_path();

  verify_output_ = ShouldVerifyOutput();
  should_fallback_ = ShouldFallback();
  subproc_weight_ = GetTaskWeight();
  int ramp_up = service_->http_client()->ramp_up();

  if (verify_output_) {
    VLOG(1) << trace_id_ << " verify_output";
    SetupSubProcess();
    RunSubProcess("verify output");
    service_->RecordForcedFallbackInSetup(CompileService::kRequestedByUser);
    // we run both local and goma backend.
    return;
  }
  if (should_fallback_) {
    VLOG(1) << trace_id_ << " should fallback";
    SetupSubProcess();
    RunSubProcess("should fallback");
    // we don't call goma rpc.
    return;
  }
  if ((rand() % 100) >= ramp_up) {
    LOG(WARNING) << trace_id_ << " http disabled "
                 << " ramp_up=" << ramp_up;
    should_fallback_ = true;
    service_->RecordForcedFallbackInSetup(CompileService::kHTTPDisabled);
    SetupSubProcess();
    RunSubProcess("http disabled");
    // we don't call goma rpc.
    return;
  }
  if (flags_->is_precompiling_header() && service_->enable_gch_hack()) {
    VLOG(1) << trace_id_ << " gch hack";
    SetupSubProcess();
    RunSubProcess("gch hack");
    // we run both local and goma backend in parallel.
  } else if (!requester_env_.fallback()) {
    stats_->exec_log.set_local_run_reason(
        "should not run under GOMA_FALLBACK=false");
    LOG(INFO) << trace_id_ << " GOMA_FALLBACK=false";
  } else if (subproc_weight_ == SubProcessReq::HEAVY_WEIGHT) {
    stats_->exec_log.set_local_run_reason(
        "should not start running heavy subproc.");
  } else if (requester_env_.use_local()) {
    int num_pending_subprocs = SubProcessTask::NumPending();
    bool is_failed_input = false;
    if (service_->local_run_for_failed_input()) {
      is_failed_input = service_->ContainFailedInput(flags_->input_filenames());
    }
    const absl::Duration subproc_delay =
        service_->GetEstimatedSubprocessDelayTime();
    if (num_pending_subprocs == 0) {
      stats_->exec_log.set_local_run_reason("local idle");
      SetupSubProcess();
    } else if (is_failed_input) {
      stats_->exec_log.set_local_run_reason("previous failed");
      SetupSubProcess();
      // TODO: RunSubProcess to run it soon?
    } else if (subproc_delay <= absl::ZeroDuration()) {
      stats_->exec_log.set_local_run_reason("slow goma");
      SetupSubProcess();
    } else if (!service_->http_client()->IsHealthyRecently()) {
      stats_->exec_log.set_local_run_reason("goma unhealthy");
      SetupSubProcess();
    } else {
      stats_->exec_log.set_local_run_reason(
          "should not run while delaying subproc");
      stats_->exec_log.set_local_delay_time(
          absl::ToInt64Milliseconds(subproc_delay));
      stats_->local_delay_time = subproc_delay;
      VLOG(1) << trace_id_ << " subproc_delay=" << subproc_delay;
      DCHECK(delayed_setup_subproc_ == nullptr) << trace_id_ << " subproc";
      delayed_setup_subproc_ =
          service_->wm()->RunDelayedClosureInThread(
              FROM_HERE,
              thread_id_,
              subproc_delay,
              NewCallback(
                  this,
                  &CompileTask::SetupSubProcess));
    }
  } else {
    stats_->exec_log.set_local_run_reason(
        "should not run under GOMA_USE_LOCAL=false");
    LOG(INFO) << trace_id_ << " GOMA_USE_LOCAL=false";
  }
  if (subproc_ != nullptr && ShouldStopGoma()) {
    state_ = LOCAL_RUN;
    stats_->exec_log.set_local_run_reason(
        "slow goma, local run started in INIT");
    return;
  }
  ProcessSetup();
}

CompileTask::~CompileTask() {
  CHECK_EQ(0, refcnt_);
  CHECK(output_file_infos_.empty());
}

bool CompileTask::BelongsToCurrentThread() const {
  return THREAD_ID_IS_SELF(thread_id_);
}

bool CompileTask::IsGomaccRunning() {
  if (gomacc_pid_ == SubProcessState::kInvalidPid)
    return false;
#ifndef _WIN32
  int ret = kill(gomacc_pid_, 0);
  if (ret != 0) {
    if (errno == ESRCH) {
      gomacc_pid_ = SubProcessState::kInvalidPid;
    } else {
      PLOG(ERROR) << trace_id_ << " kill 0 failed with unexpected errno."
                  << " gomacc_pid=" << gomacc_pid_;
    }
  }
#else
  SimpleTimer timer;
  bool running = false;
  {
    ScopedFd proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                              gomacc_pid_));
    running = proc.valid();
  }
  const absl::Duration duration = timer.GetDuration();
  LOG_IF(WARNING, duration > absl::Milliseconds(100))
        << trace_id_ << " SLOW IsGomaccRunning in " << duration;
  if (!running) {
    gomacc_pid_ = SubProcessState::kInvalidPid;
  }
#endif
  return gomacc_pid_ != SubProcessState::kInvalidPid;
}

void CompileTask::GomaccClosed() {
  LOG(INFO) << trace_id_ << " gomacc closed "
            << "at state=" << StateName(state_)
            << " subproc pid="
            << (subproc_ != nullptr ? subproc_->started().pid() : 0);
  canceled_ = true;
  gomacc_pid_ = SubProcessState::kInvalidPid;
  // Kill subprocess either it is running, or pending.
  if (subproc_ != nullptr) {
    KillSubProcess();
  }
}

bool CompileTask::IsSubprocRunning() const {
  return subproc_ != nullptr &&
      subproc_->started().pid() != SubProcessState::kInvalidPid;
}

void CompileTask::ProcessSetup() {
  VLOG(1) << trace_id_ << " setup";
  CHECK(BelongsToCurrentThread());
  CHECK_EQ(INIT, state_);
  CHECK(!abort_);
  CHECK(!should_fallback_);
  state_ = SETUP;
  if (ShouldStopGoma()) {
    state_ = LOCAL_RUN;
    stats_->exec_log.set_local_run_reason(
        "slow goma, local run started in SETUP");
    return;
  }
  FillCompilerInfo();
}

void CompileTask::TryProcessFileRequest() {
  file_request_timer_.Start();
  if (flags_->is_linking()) {
    AUTOLOCK(lock, &global_mu_);
    DCHECK(link_file_req_tasks_ != nullptr);
    link_file_req_tasks_->push_back(this);
    if (link_file_req_tasks_->front() != this) {
      VLOG(1) << trace_id_ << " pending file req "
              << link_file_req_tasks_->size();
      return;
    }
  }
  ProcessFileRequest();
}

void CompileTask::ProcessFileRequest() {
  VLOG(1) << trace_id_ << " file req";
  CHECK(BelongsToCurrentThread());
  // SETUP: first pass
  // FILE_REQ: failed in input file task, and retry
  // FILE_RESP: failed with missing inputs, and retry
  CHECK(state_ == SETUP || state_ == FILE_REQ || state_ == FILE_RESP)
      << trace_id_ << " " << StateName(state_);
  const absl::Duration fileload_pending_time =
      file_request_timer_.GetDuration();
  stats_->include_fileload_pending_time += fileload_pending_time;
  stats_->exec_log.add_include_fileload_pending_time(
      DurationToIntMs(fileload_pending_time));
  file_request_timer_.Start();
  if (abort_) {
    ProcessPendingFileRequest();
    ProcessFinished("aborted before file req");
    return;
  }
  if (canceled_) {
    ProcessPendingFileRequest();
    ProcessFinished("canceled before file req");
    return;
  }
  state_ = FILE_REQ;
  if (ShouldStopGoma()) {
    ProcessPendingFileRequest();
    state_ = LOCAL_RUN;
    stats_->exec_log.set_local_run_reason(
        "slow goma, local run started in FILE_REQ");
    return;
  }
  VLOG(1) << trace_id_
          << " start processing of input files "
          << required_files_.size();

  absl::flat_hash_set<std::string> missed_content_files;
  for (const auto& filename : resp_->missing_input()) {
    missed_content_files.insert(filename);
    VLOG(2) << trace_id_ << " missed content: " << filename;
    if (interleave_uploaded_files_.contains(filename)) {
      LOG(WARNING) << trace_id_ << " interleave-uploaded file missing:"
                   << filename;
    }
  }

  // InputFileTask assumes that filename is unique in single compile task.
  std::vector<std::string> removed_files;
  RemoveDuplicateFiles(flags_->cwd(), &required_files_, &removed_files);
  LOG_IF(INFO, !removed_files.empty())
      << trace_id_ << " de-duplicated:" << removed_files;

  // TODO: We don't need to clear the input when we are retrying.
  req_->clear_input();
  interleave_uploaded_files_.clear();
  SetInputFileCallback();
  std::vector<OneshotClosure*> closures;
  const absl::Time now = absl::Now();
  stats_->exec_log.set_num_total_input_file(required_files_.size());
  stats_->exec_log.set_total_input_file_size(sum_of_required_file_size_);

  for (const std::string& filename : required_files_) {
    ExecReq_Input* input = req_->add_input();
    input->set_filename(filename);
    const std::string abs_filename =
        file::JoinPathRespectAbsolute(flags_->cwd(), filename);
    bool missed_content = missed_content_files.contains(filename);
    absl::optional<absl::Time> mtime;
    std::string hash_key;
    bool hash_key_is_ok = false;
    absl::optional<absl::Time> missed_timestamp;
    if (missed_content) {
      missed_timestamp = last_req_timestamp_;
    }

    // If the file was reported as missing, we need to send the file content.
    //
    // Otherwise,
    //  if hash_key_is_ok is true, we can believe hash_key is valid,
    //  so uses hash_key only (no content uploading here)
    //
    //  if hash_key_is_ok is false, we're not sure hash_key is valid or not,
    //  so try reading the content.  InputFileTaskFinished determines whether
    //  we should upload the content or not, based on mtime and hash_key.
    //  if the content's hash matches with this hash_key, we can believe
    //  hash_key is valid, so don't upload content in this session.
    //
    // If we believed hash_key is valid, but goma servers couldn't find the
    // content, then it would be reported as missing_inputs_ and we'll set
    // missed_content to true in the retry session.
    // Even in this case, we need to consider the race condition of upload and
    // execution. If the file is uploaded by the other task during the task is
    // getting missing_inputs_, we do not have to upload the file again. We use
    // the timestamp of file upload and execution to identify this condition.
    // If upload time is later than execution time (last_req_timestamp_),
    // we can assume the file is uploaded by others.
    const FileStat input_file_stat = input_file_stat_cache_->Get(abs_filename);
    if (input_file_stat.IsValid()) {
      mtime = *input_file_stat.mtime;
    }
    hash_key_is_ok = service_->file_hash_cache()->GetFileCacheKey(
        abs_filename, missed_timestamp, input_file_stat, &hash_key);
    if (missed_content) {
      if (hash_key_is_ok) {
        LOG(INFO) << trace_id_ << " interleave uploaded: "
                  << " filename=" << abs_filename;
        interleave_uploaded_files_.insert(filename);
      } else {
        LOG(INFO) << trace_id_ << " missed content:" << abs_filename;
      }
    }
    if (mtime.has_value() &&
        *mtime > absl::FromTimeT(stats_->exec_log.latest_input_mtime())) {
      stats_->exec_log.set_latest_input_filename(abs_filename);
      stats_->exec_log.set_latest_input_mtime(absl::ToTimeT(*mtime));
    }
    if (hash_key_is_ok) {
      input->set_hash_key(hash_key);
      continue;
    }
    // In linking, we'll use hash_key instead of content in ExecReq to prevent
    // from bloating ExecReq.
    VLOG(1) << trace_id_ << " input file:" << abs_filename
            << (flags_->is_linking() ? " [linking]" : "");
    bool is_new_file = false;
    if (mtime.has_value()) {
      if (flags_->is_linking()) {
        // For linking, we assume input files is old if it is older than
        // compiler_proxy start time. (i.e. it would be built in previous
        // build session, so that the files were generated by goma backends
        // or uploaded by previous compiler_proxy.
        is_new_file = *mtime > service_->start_time();
      } else {
        is_new_file = (now - *mtime < service_->new_file_threshold_duration());
      }
    }
    // If need_to_send_content is set to true, we consider all file is new file.
    if (service_->need_to_send_content())
      is_new_file = true;

    InputFileTask* input_file_task = InputFileTask::NewInputFileTask(
        service_->wm(),
        service_->blob_client()->NewUploader(abs_filename, requester_info_,
                                             trace_id_),
        service_->file_hash_cache(), input_file_stat_cache_->Get(abs_filename),
        abs_filename, missed_content, flags_->is_linking(), is_new_file,
        hash_key, this, input);
    closures.push_back(
        NewCallback(
            input_file_task,
            &InputFileTask::Run,
            this,
            NewCallback(
                this,
                &CompileTask::InputFileTaskFinished,
                input_file_task)));
    DCHECK_EQ(closures.size(), static_cast<size_t>(num_input_file_task_));
  }
  DCHECK_EQ(closures.size(), static_cast<size_t>(num_input_file_task_));
  stats_->exec_log.add_num_uploading_input_file(closures.size());
  stats_->exec_log.add_num_file_uploaded_during_exec_failure(
      interleave_uploaded_files_.size());
  if (closures.empty()) {
    MaybeRunInputFileCallback(false);
    return;
  }
  for (auto* closure : closures)
    service_->wm()->RunClosure(
        FROM_HERE, closure, WorkerThread::PRIORITY_LOW);
}

namespace {

// ShrinkExecReq returns the number of content drop.
int ShrinkExecReq(absl::string_view trace_id, ExecReq* req) {
  // Drop embedded content randomly if it is larger than 1MiB +
  // 2MB (max chunk size).
  // http://b/161513480 reduce max execreq msg size
  // TODO: size limit is still configurable.
  size_t total_embedded_size = 0;
  constexpr size_t kEmbeddedThreshold = 1 * 1024 * 1024;  // 1MiB;
  int cleared = 0;

  absl::c_shuffle(*req->mutable_input(), MyCryptographicSecureRNG());
  for (auto& input : *req->mutable_input()) {
    if (!input.has_content()) {
      continue;
    }
    if (!input.content().has_content()) {
      continue;
    }
    size_t content_size = input.content().content().size();
    if (total_embedded_size >= kEmbeddedThreshold) {
      LOG(INFO) << trace_id << " embed:" << input.filename()
                << " content cleared "
                << " blob_type=" << input.content().blob_type()
                << " size=" << content_size;
      input.clear_content();
      ++cleared;
      continue;
    }

    total_embedded_size += content_size;
  }

  absl::c_sort(*req->mutable_input(),
               [](const ExecReq_Input& a, const ExecReq_Input& b) {
                 return a.filename() < b.filename();
               });
  return cleared;
}

}  // namespace

void CompileTask::ProcessFileRequestDone() {
  VLOG(1) << trace_id_ << " file req done";
  CHECK(BelongsToCurrentThread());
  CHECK_EQ(FILE_REQ, state_);

  {
    int dropped = ShrinkExecReq(trace_id_, req_.get());
    if (dropped > 0) {
      *stats_->exec_log.mutable_num_uploading_input_file()->rbegin() -= dropped;
      stats_->exec_log.add_num_dropped_input_file(dropped);
    }
  }

  const absl::Duration fileload_run_time = file_request_timer_.GetDuration();
  stats_->exec_log.add_include_fileload_run_time(
      DurationToIntMs(fileload_run_time));
  stats_->include_fileload_run_time += fileload_run_time;

  const absl::Duration include_fileload_time =
      include_timer_.GetDuration() - stats_->include_preprocess_time;
  stats_->include_fileload_time = include_fileload_time;

  VLOG(1) << trace_id_
          << " input files processing preprocess "
          << stats_->include_preprocess_time
          << ", loading " << stats_->include_fileload_time;

  ProcessPendingFileRequest();

  if (abort_) {
    ProcessFinished("aborted in file req");
    return;
  }
  if (canceled_) {
    ProcessFinished("canceled in file req");
    return;
  }
  if (!input_file_success_) {
    if (IsSubprocRunning()) {
      VLOG(1) << trace_id_ << " file request failed,"
              << " but subprocess running";
      state_ = LOCAL_RUN;
      stats_->exec_log.set_local_run_reason(
          "fail goma, local run started in FILE_REQ");
      return;
    }
    AddErrorToResponse(TO_LOG, "Failed to process file request", true);
    if (service_->http_client()->IsHealthyRecently() &&
        stats_->exec_log.num_uploading_input_file_size() > 0 &&
        stats_->exec_log.num_uploading_input_file(
            stats_->exec_log.num_uploading_input_file_size() - 1) > 0) {
      // TODO: don't retry for permanent error (no such file, etc).
      stats_->exec_log.set_exec_request_retry(
          stats_->exec_log.exec_request_retry() + 1);
      if (stats_->exec_log.exec_request_retry() <= kMaxExecRetry) {
        std::ostringstream ss;
        ss << "Failed to upload "
           << stats_->exec_log.num_uploading_input_file(
                  stats_->exec_log.num_uploading_input_file_size() - 1)
           << " files";
        stats_->exec_log.add_exec_request_retry_reason(ss.str());
        LOG(INFO) << trace_id_ << " retry in FILE_REQ";
        resp_->clear_error_message();

        service_->wm()->RunClosureInThread(
            FROM_HERE,
            thread_id_,
            NewCallback(this, &CompileTask::TryProcessFileRequest),
            WorkerThread::PRIORITY_LOW);
        return;
      }
    }
    ProcessFinished("fail in file request");
    return;
  }

  // Fix for GOMA_GCH.
  // We're sending *.gch.goma on local disk, but it must appear as *.gch
  // on backend.
  if (service_->enable_gch_hack()) {
    for (auto& input : *req_->mutable_input()) {
      if (absl::EndsWith(input.filename(), GOMA_GCH_SUFFIX)) {
        input.mutable_filename()->resize(
            input.filename().size() - strlen(".goma"));
      }
    }
  }

  // Here, |req_| is all prepared.
  // TODO: Instead of here, maybe we need to call this
  // in end of ProcessFileRequest?
  if (LocalOutputCache::IsEnabled()) {
    local_output_cache_key_ = LocalOutputCache::MakeCacheKey(*req_);
    if (LocalOutputCache::instance()->Lookup(local_output_cache_key_,
                                             resp_.get(),
                                             trace_id_)) {
      LOG(INFO) << trace_id_ << " lookup succeeded";
      stats_->exec_log.set_cache_hit(true);
      stats_->exec_log.set_cache_source(ExecLog::LOCAL_OUTPUT_CACHE);
      ReleaseMemoryForExecReqInput(req_.get());
      state_ = LOCAL_OUTPUT;
      ProcessFileResponse();
      return;
    }
  }

  ProcessCallExec();
}

void CompileTask::ProcessPendingFileRequest() {
  if (!flags_->is_linking())
    return;

  CompileTask* pending_task = nullptr;
  {
    AUTOLOCK(lock, &global_mu_);
    DCHECK_EQ(this, link_file_req_tasks_->front());
    link_file_req_tasks_->pop_front();
    if (!link_file_req_tasks_->empty()) {
      pending_task = link_file_req_tasks_->front();
    }
  }
  if (pending_task != nullptr) {
    VLOG(1) << pending_task->trace_id_ << " start file req";
    service_->wm()->RunClosureInThread(
        FROM_HERE,
        pending_task->thread_id_,
        NewCallback(pending_task, &CompileTask::ProcessFileRequest),
        WorkerThread::PRIORITY_LOW);
  }
}

void CompileTask::ProcessCallExec() {
  VLOG(1) << trace_id_ << " call exec";
  CHECK(BelongsToCurrentThread());
  CHECK_EQ(FILE_REQ, state_);
  if (abort_) {
    ProcessFinished("aborted before call exec");
    return;
  }
  if (canceled_) {
    ProcessFinished("canceled before call exec");
    return;
  }
  CHECK(!requester_env_.verify_command().empty() ||
        req_->input_size() > 0) << trace_id_ << " call exec";
  state_ = CALL_EXEC;
  if (ShouldStopGoma()) {
    state_ = LOCAL_RUN;
    stats_->exec_log.set_local_run_reason(
        "slow goma, local run started in CALL_EXEC");
    return;
  }

  if (req_->trace()) LOG(INFO) << trace_id_ << " requesting remote trace";
  rpc_call_timer_.Start();
  req_->mutable_requester_info()->set_retry(
      stats_->exec_log.exec_request_retry());
  VLOG(2) << trace_id_
          << " request string to send:" << req_->DebugString();
  {
    AUTOLOCK(lock, &mu_);
    http_rpc_status_ = absl::make_unique<HttpRPC::Status>();
    http_rpc_status_->trace_id = trace_id_;
    const auto& timeouts = service_->timeouts();
    for (const auto& timeout : timeouts) {
      http_rpc_status_->timeouts.push_back(timeout);
    }
  }

  exec_resp_ = absl::make_unique<ExecResp>();

  ModifyRequestCWDAndPWD();

  service_->exec_service_client()->ExecAsync(
      req_.get(), exec_resp_.get(), http_rpc_status_.get(),
      NewCallback(this, &CompileTask::ProcessCallExecDone));

  last_req_timestamp_ = absl::Now();
  if (requester_env_.use_local() &&
      (subproc_weight_ == SubProcessReq::HEAVY_WEIGHT) &&
      subproc_ == nullptr) {
    // now, it's ok to run subprocess.
    stats_->exec_log.set_local_run_reason("slow goma linking");
    SetupSubProcess();
  }
}

void CompileTask::ProcessCallExecDone() {
  VLOG(1) << trace_id_ << " call exec done";
  CHECK(BelongsToCurrentThread());
  CHECK_EQ(CALL_EXEC, state_);
  exit_status_ = exec_resp_->result().exit_status();
  resp_->Swap(exec_resp_.get());
  exec_resp_.reset();
  std::string retry_reason;
  for (const auto& msg : resp_->error_message()) {
    LOG(WARNING) << trace_id_ << " server error:" << msg;
    exec_error_message_.push_back(msg);
    if (!retry_reason.empty()) {
      retry_reason += "\n";
    }
    retry_reason += msg;
  }
  // clear error_message from server.
  // server error message logged, but not send back to user.
  resp_->clear_error_message();

  const absl::Duration rpc_call_timer_duration = rpc_call_timer_.GetDuration();
  stats_->exec_log.add_rpc_call_time(DurationToIntMs(rpc_call_timer_duration));
  stats_->total_rpc_call_time += rpc_call_timer_duration;

  stats_->AddStatsFromHttpStatus(*http_rpc_status_);
  stats_->AddStatsFromExecResp(*resp_);

  stats_->exec_log.set_cache_hit(
      resp_->cache_hit() == ExecResp::LOCAL_OUTPUT_CACHE ||
      (http_rpc_status_->finished && resp_->has_cache_hit() &&
       resp_->cache_hit() != ExecResp::NO_CACHE));

  if (stats_->exec_log.cache_hit()) {
    if (resp_->cache_hit() == ExecResp::NO_CACHE) {
      LOG(ERROR) << trace_id_ << " cache_hit, but NO_CACHE";
    } else {
      auto cache_source = CompileStats::GetCacheSourceFromExecResp(*resp_);
      if (cache_source == ExecLog::UNKNOWN_CACHE) {
        LOG(ERROR) << trace_id_
                   << " unknown cache_source=" << resp_->cache_hit();
      }
      stats_->exec_log.set_cache_source(cache_source);
    }
  }


  if (resp_->has_cache_key())
    resp_cache_key_ = resp_->cache_key();

  if (abort_) {
    ProcessFinished("aborted in call exec");
    return;
  }
  if (canceled_) {
    ProcessFinished("canceled in call exec");
    return;
  }

  stats_->exec_log.set_network_failure_type(
      CompileStats::GetNetworkFailureTypeFromHttpStatus(*http_rpc_status_));

  const int err = http_rpc_status_->err;
  if (err < 0) {
    LOG(WARNING) << trace_id_ << " rpc err=" << err << " "
                 << (err == ERR_TIMEOUT ? " timed out" : " failed")
                 << " " << http_rpc_status_->err_message;
    if (IsSubprocRunning() &&
        http_rpc_status_->state != HttpClient::Status::RECEIVING_RESPONSE) {
      // If rpc was failed while receiving response, goma should retry Exec call
      // because the reponse will be replied from cache with high probability.
      LOG(WARNING) << trace_id_ << " goma failed, but subprocess running.";
      state_ = LOCAL_RUN;
      stats_->exec_log.set_local_run_reason(
          "fail goma, local run started in CALL_EXEC");
      return;
    }
    AddErrorToResponse(TO_LOG, "", true);
    // Don't Retry if it is client error: 3xx or 4xx.
    // Retry if it is server error: 5xx (e.g. 502 error from GFE)
    //
    // Also, OK to retry
    //  on socket write failure during sending request.
    //  on socket timeout occurred during receiving response.
    if (((http_rpc_status_->http_return_code / 100) == 5) ||
        (http_rpc_status_->state == HttpClient::Status::SENDING_REQUEST) ||
        (http_rpc_status_->state == HttpClient::Status::RECEIVING_RESPONSE)) {
      std::ostringstream ss;
      ss << "RPC failed http=" << http_rpc_status_->http_return_code
         << ": " << http_rpc_status_->err_message;
      if (!retry_reason.empty()) {
        retry_reason += "\n";
      }
      retry_reason += ss.str();
    } else {
      // No retry for client error: 3xx, 4xx (302, 403 for dos block,
      // 401 for auth error, etc).
      std::string error_message =
          absl::StrCat("no retry: RPC failed http=",
                       http_rpc_status_->http_return_code,
                       " state=",
                       HttpClient::Status::StateName(http_rpc_status_->state),
                       ": ", http_rpc_status_->err_message);
      AddErrorToResponse(TO_LOG, error_message, false);
    }
  }
  if (err == OK && resp_->missing_input_size() > 0) {
    // missing input will be handled in ProcessFileResponse and
    // ProcessFileRequest will retry the request with uploading
    // contents of missing inputs.
    // Just retrying the request here would not upload contents
    // so probably fails with missing input again, so don't retry here.
    LOG_IF(WARNING, !retry_reason.empty())
        << trace_id_ << " missing inputs:" << resp_->missing_input_size()
        << " but retry_reason set:" << retry_reason;
  } else if (!retry_reason.empty()) {
    if (service_->http_client()->IsHealthyRecently()) {
      LOG(INFO) << trace_id_
                << " exec retry:" << stats_->exec_log.exec_request_retry()
                << " error=" << resp_->error() << " " << retry_reason;
      stats_->exec_log.set_exec_request_retry(
          stats_->exec_log.exec_request_retry() + 1);
      if (stats_->exec_log.exec_request_retry() <= kMaxExecRetry &&
          !(resp_->has_error() && IsFatalError(resp_->error()))) {
        stats_->exec_log.add_exec_request_retry_reason(retry_reason);
        LOG(INFO) << trace_id_ << " retry in CALL_EXEC";
        resp_->clear_error_message();
        resp_->clear_error();
        state_ = FILE_REQ;
        service_->wm()->RunClosureInThread(
            FROM_HERE,
            thread_id_,
            NewCallback(this, &CompileTask::ProcessCallExec),
            WorkerThread::PRIORITY_LOW);
        return;
      }
      if (service_->should_fail_for_unsupported_compiler_flag() &&
          resp_->bad_request_reason_code() ==
              ExecResp::UNSUPPORTED_COMPILER_FLAGS) {
        // TODO: Make a simple test for this after goma server has
        // started returning bad request reason code.
        std::string msg =
            "compile request was rejected by goma server. "
            "The request might contain unsupported compiler flag.\n"
            "If you want to continue compile with local fallback, set "
            "environment variable "
            "GOMA_FAIL_FOR_UNSUPPORTED_COMPILER_FLAGS=false and "
            "restart the compiler_proxy.\n";
        AddErrorToResponse(TO_USER, msg, true);
        want_fallback_ = false;
      } else {
        AddErrorToResponse(
            TO_LOG,
            absl::StrCat("no retry: exec error=", resp_->error(),
                         " retry=", stats_->exec_log.exec_request_retry(),
                         " reason=", retry_reason, " http=healthy"),
            false);
      }
    } else {
      AddErrorToResponse(
          TO_LOG,
          absl::StrCat("no retry: exec error=", resp_->error(),
                       " retry=", stats_->exec_log.exec_request_retry(),
                       " reason=", retry_reason, " http=unhealthy"),
          false);
    }
    CheckNoMatchingCommandSpec(retry_reason);
    ProcessFinished("fail in call exec");
    return;
  }

  if (err < 0) {
    ProcessFinished("fail in call exec");
    return;
  }

  // Saves embedded upload information. We have to call this before
  // clearing inputs.
  StoreEmbeddedUploadInformationIfNeeded();

  ReleaseMemoryForExecReqInput(req_.get());

  if (resp_->missing_input_size() == 0) {
    // Check command spec when not missing input response.
    CheckCommandSpec();
  }
  ProcessFileResponse();
}

void CompileTask::ProcessFileResponse() {
  VLOG(1) << trace_id_ << " file resp";
  CHECK(BelongsToCurrentThread());
  CHECK(state_ == CALL_EXEC || state_ == LOCAL_OUTPUT) << state_;
  if (abort_) {
    ProcessFinished("aborted before file resp");
    return;
  }
  if (canceled_) {
    ProcessFinished("canceled before file resp");
    return;
  }
  state_ = FILE_RESP;
  if (ShouldStopGoma()) {
    state_ = LOCAL_RUN;
    stats_->exec_log.set_local_run_reason(
        "slow goma, local run started in FILE_RESP");
    return;
  }
  file_response_timer_.Start();
  if (resp_->missing_input_size() > 0) {
    stats_->exec_log.add_num_missing_input_file(resp_->missing_input_size());
    LOG(WARNING) << trace_id_ << " request didn't have full content:"
                 << resp_->missing_input_size() << " in "
                 << required_files_.size()
                 << " : retry=" << stats_->exec_log.exec_request_retry();
    for (const auto& filename : resp_->missing_input()) {
      std::ostringstream ss;
      ss << "Required file not on goma cache:" << filename;
      if (interleave_uploaded_files_.contains(filename)) {
        ss << " (interleave uploaded)";
      }
      AddErrorToResponse(TO_LOG, ss.str(), true);
    }
    for (const auto& reason : resp_->missing_reason()) {
      AddErrorToResponse(TO_LOG, reason, true);
    }
    int need_to_send_content_threshold = required_files_.size() / 2;
    if (!service_->need_to_send_content()
        && (resp_->missing_input_size() > need_to_send_content_threshold)) {
      LOG(WARNING) << trace_id_
                   << " Lots of missing files. Will send file contents"
                   << " even if it's old enough.";
      service_->SetNeedToSendContent(true);
    }
    output_file_success_ = false;
    ProcessFileResponseDone();
    return;
  }
  if (stats_->exec_log.exec_request_retry() == 0 &&
      service_->need_to_send_content()) {
    LOG(INFO) << trace_id_ << " no missing files."
              << " Turn off to force sending old file contents";
    service_->SetNeedToSendContent(false);
  }

  // No missing input files.
  if (!IsGomaccRunning()) {
    PLOG(WARNING) << trace_id_
                  << " pid:" << gomacc_pid_ << " does not receive signal 0 "
                  << " abort=" << abort_;
    // user may not receive the error message, because gomacc already killed.
    AddErrorToResponse(TO_LOG, "gomacc killed?", true);
    // If the requesting process was already dead, we should not write output
    // files.
    ProcessFinished("gomacc killed");
    return;
  }

  // Decide if it could use in-memory output or not and should write output
  // in tmp file or not.
  bool want_in_memory_output = true;
  std::string need_rename_reason;
  if (verify_output_) {
    VLOG(1) << trace_id_ << " output need_rename for verify_output";
    want_in_memory_output = false;
    need_rename_reason = "verify_output";
  } else if (!success()) {
    VLOG(1) << trace_id_ << " output need_rename for fail exec";
    // TODO: we don't need to write remote output for fail exec?
    want_in_memory_output = false;
    need_rename_reason = "fail exec";
  } else {
    // resp_ contains whole output data, and no need to more http_rpc to
    // fetch output file data, so no need to run local compiler any more.
    if (delayed_setup_subproc_ != nullptr) {
      delayed_setup_subproc_->Cancel();
      delayed_setup_subproc_ = nullptr;
    }
    if (subproc_ != nullptr) {
      // racing between remote and local.
      // even if subproc_->started().pid() == kInvalidPid, subproc might
      // have started (because compile_proxy and subproc is async).
      // The compile task wants in_memory output by default, but when it
      // couldn't use in memory output because of lack of memory, it
      // should write output in tmp file (i.e. need to rename).
      // TODO: cancel subproc if it was not started yet,
      //             or use local subproc if it has already started.
      VLOG(1) << trace_id_ << " output need_rename for local_subproc "
              << subproc_->started().pid();
      std::ostringstream ss;
      ss << "local_subproc pid=" << subproc_->started().pid();
      need_rename_reason = ss.str();
    }
  }

  exec_output_files_.clear();
  ClearOutputFile();
  output_file_infos_.resize(resp_->result().output_size());
  SetOutputFileCallback();
  std::vector<OneshotClosure*> closures;
  for (int i = 0; i < resp_->result().output_size(); ++i) {
    const std::string& output_filename = resp_->result().output(i).filename();
    CheckOutputFilename(output_filename);

    exec_output_files_.push_back(output_filename);
    std::string filename =
        file::JoinPathRespectAbsolute(stats_->exec_log.cwd(), output_filename);
    // TODO: check output paths matches with flag's output filenames?
    if (service_->enable_gch_hack() && absl::EndsWith(filename, ".gch"))
      filename += ".goma";

    auto* output_info = &output_file_infos_[i];
    output_info->filename = filename;
    bool try_acquire_output_buffer = want_in_memory_output;
    if (IsValidFileBlob(resp_->result().output(i).blob())) {
      output_info->size = resp_->result().output(i).blob().file_size();
    } else {
      LOG(ERROR) << trace_id_ << " output is invalid:"
                 << filename;
      try_acquire_output_buffer = false;
    }
    if (try_acquire_output_buffer && service_->AcquireOutputBuffer(
            output_info->size, &output_info->content)) {
      output_info->tmp_filename.clear();
      VLOG(1) << trace_id_ << " output in buffer:"
              << filename
              << " size="
              << output_info->size;
    } else {
      if (!need_rename_reason.empty()) {
        std::ostringstream ss;
        ss << filename << ".tmp." << id();
        output_info->tmp_filename = ss.str();
        LOG(INFO) << trace_id_ << " output in tmp file:"
                  << output_info->tmp_filename
                  << " for " << need_rename_reason;
      } else {
        // no need to rename, so write output directly to the output file.
        output_info->tmp_filename = filename;
        LOG(INFO) << trace_id_ << " output in file:" << filename;
      }
    }
    if (resp_->result().output(i).is_executable())
      output_info->mode = 0777;
    if (requester_env_.has_umask()) {
      output_info->mode &= ~requester_env_.umask();
      VLOG(1) << trace_id_ << " output file mode is updated."
              << " filename=" << filename
              << " mode=" << std::oct << output_info->mode;
    }
    std::unique_ptr<OutputFileTask> output_file_task(new OutputFileTask(
        service_->wm(),
        service_->blob_client()->NewDownloader(requester_info_, trace_id_),
        this, i, resp_->result().output(i), output_info));

    OutputFileTask* output_file_task_pointer = output_file_task.get();
    closures.push_back(
        NewCallback(
            output_file_task_pointer,
            &OutputFileTask::Run,
            NewCallback(
                this,
                &CompileTask::OutputFileTaskFinished,
                std::move(output_file_task))));
  }
  stats_->exec_log.set_num_output_file(closures.size());
  if (closures.empty()) {
    MaybeRunOutputFileCallback(-1, false);
  } else {
    for (auto* closure : closures) {
      service_->wm()->RunClosure(
          FROM_HERE, closure, WorkerThread::PRIORITY_LOW);
    }
  }
}

void CompileTask::ProcessFileResponseDone() {
  VLOG(1) << trace_id_ << " file resp done";
  CHECK(BelongsToCurrentThread());
  CHECK_EQ(FILE_RESP, state_);

  const absl::Duration file_response_time = file_response_timer_.GetDuration();
  stats_->file_response_time += file_response_time;
  stats_->exec_log.set_file_response_time(DurationToIntMs(file_response_time));

  if (abort_) {
    ProcessFinished("aborted in file resp");
    return;
  }
  if (canceled_) {
    ProcessFinished("canceled in file resp");
    return;
  }
  if (!output_file_success_) {
    // TODO: remove following if (!abort_).
    // I belive it should always be true, or abort_ must be protected by mutex.
    if (!abort_) {
      if (!(flags_->is_precompiling_header() && service_->enable_gch_hack()) &&
          IsSubprocRunning()) {
        VLOG(1) << trace_id_ << " failed to process file response,"
                << " but subprocess running";
        state_ = LOCAL_RUN;
        stats_->exec_log.set_local_run_reason(
            "fail goma, local run started in FILE_RESP");
        return;
      }

      // For missing input error, we don't make it as error but warning
      // when this is the first try and we will retry it later.
      bool should_error = stats_->exec_log.exec_request_retry() > 0;
      std::ostringstream ss;
      ss << "Try:" << stats_->exec_log.exec_request_retry() << ": ";
      if (resp_->missing_input_size() > 0) {
        // goma server replied with missing inputs.
        // retry: use the list of missing files in response to fill in
        // needed files
        ss << "Missing " << resp_->missing_input_size() << " input files.";
      } else {
        should_error = true;
        ss << "Failed to download " << stats_->exec_log.num_output_file()
           << " files"
           << " in " << (cache_hit() ? "cached" : "no-cached") << "result";
      }

      bool do_retry = false;
      std::ostringstream no_retry_reason;
      if (compiler_info_state_.disabled()) {
        no_retry_reason << "compiler disabled. no retry."
                        << " disabled_reason="
                        << compiler_info_state_.GetDisabledReason();
      } else if (!service_->http_client()->IsHealthyRecently()) {
        no_retry_reason << "http is unhealthy. no retry."
                        << " health_status="
                        << service_->http_client()->GetHealthStatusMessage();
      } else {
        stats_->exec_log.set_exec_request_retry(
            stats_->exec_log.exec_request_retry() + 1);
        do_retry = stats_->exec_log.exec_request_retry() <= kMaxExecRetry;
        if (!do_retry) {
          no_retry_reason << "too many retry";
        }
      }

      if (!do_retry)
        should_error = true;
      AddErrorToResponse(TO_LOG, ss.str(), should_error);

      if (do_retry) {
        if (!service_->http_client()->IsHealthy()) {
          LOG(WARNING) << trace_id_ << " http is unhealthy, but retry."
                       << " health_status="
                       << service_->http_client()->GetHealthStatusMessage();
        }
        VLOG(2) << trace_id_
                << " Failed to process file response (we will retry):"
                << resp_->DebugString();
        stats_->exec_log.add_exec_request_retry_reason(ss.str());
        LOG(INFO) << trace_id_ << " retry in FILE_RESP";
        resp_->clear_error_message();
        TryProcessFileRequest();
        return;
      }
      AddErrorToResponse(TO_LOG, no_retry_reason.str(), true);
    }
    VLOG(2) << trace_id_
            << " Failed to process file response (second time):"
            << resp_->DebugString();
    ProcessFinished("failed in file response");
    return;
  }

  if (verify_output_) {
    CHECK(subproc_ == nullptr);
    CHECK(delayed_setup_subproc_ == nullptr);
    for (const auto& info : output_file_infos_) {
      const std::string& filename = info.filename;
      const std::string& tmp_filename = info.tmp_filename;
      if (!VerifyOutput(filename, tmp_filename)) {
        output_file_success_ = false;
      }
    }
    output_file_infos_.clear();
    ProcessFinished("verify done");
    return;
  }
  if (success()) {
    ProcessFinished("");
  } else {
    ClearOutputFile();
    ProcessFinished("fail exec");
  }
}

void CompileTask::ProcessFinished(const std::string& msg) {
  if (abort_ || canceled_ || !msg.empty()) {
    LOG(INFO) << trace_id_ << " finished " << msg
              << " state=" << StateName(state_)
              << " abort=" << abort_
              << " canceled=" << canceled_;
  } else {
    VLOG(1) << trace_id_ << " finished " << msg
            << " state=" << StateName(state_);
    DCHECK(success()) << trace_id_ << " finished";
    DCHECK_EQ(FILE_RESP, state_) << trace_id_ << " finished";
  }
  CHECK(BelongsToCurrentThread());
  CHECK_LT(state_, FINISHED);
  DCHECK(!finished_);
  finished_ = true;
  if (state_ == INIT) {
    // failed to find local compiler path.
    // it also happens if user uses old gomacc.
    LOG(ERROR) << trace_id_ << " failed in INIT.";
    CHECK(subproc_ == nullptr);
    CHECK(delayed_setup_subproc_ == nullptr);
    CHECK(!abort_);
    state_ = FINISHED;
    ReplyResponse("failed in INIT");
    return;
  }
  if (!abort_)
    state_ = FINISHED;
  if (verify_output_) {
    VLOG(2) << trace_id_ << " verify response:" << resp_->DebugString();
    CHECK(subproc_ == nullptr);
    CHECK(delayed_setup_subproc_ == nullptr);
    ReplyResponse("verify done");
    return;
  }
  if (flags_->is_precompiling_header() && service_->enable_gch_hack()) {
    // In gch hack mode, we'll run both local and remote simultaneously.
    if (subproc_ != nullptr) {
      // subprocess still running.
      // we'll reply response when subprocess is finished.
      return;
    }
    // subprocess finished first.
    CHECK(delayed_setup_subproc_ == nullptr);
    VLOG(1) << trace_id_ << " gch hack: local and goma finished.";
    ProcessReply();
    return;
  }

  // Unlike want_fallback_ = false case,
  // we won't start subprocs for GOMA_FALLBACK=false, we can reply here.
  if (!requester_env_.fallback()) {
    VLOG(1) << trace_id_ << " goma finished and no fallback.";
    CHECK(subproc_ == nullptr);
    CHECK(delayed_setup_subproc_ == nullptr);
    ProcessReply();
    return;
  }
  if (abort_) {
    // local finished first (race or verify output).
    if (local_output_file_callback_ == nullptr)
      Done();
    // If local_output_file_callback_ is not nullptr, uploading local output
    // file is on the fly, so ProcessLocalFileOutputDone() will be called
    // later.
    return;
  }
  CHECK_EQ(FINISHED, state_);
  if (success() || !IsGomaccRunning() || !want_fallback_) {
    if (!success() && !want_fallback_) {
      LOG(INFO) << trace_id_ << " failed and no need to fallback";
    } else {
      VLOG(1) << trace_id_ << " success or gomacc killed.";
    }
    stats_->exec_log.clear_local_run_reason();
    if (delayed_setup_subproc_ != nullptr) {
      delayed_setup_subproc_->Cancel();
      delayed_setup_subproc_ = nullptr;
    }
    if (subproc_ != nullptr) {
      LOG(INFO) << trace_id_ << " goma finished, killing subproc pid="
                << subproc_->started().pid();
      KillSubProcess();  // FinishSubProcess will be called.
    } else {
      ProcessReply();  // GOMA_FALLBACK=false or GOMA_USE_LOCAL=false
    }
    return;
  }
  LOG(INFO) << trace_id_ << " fail fallback"
            << " exit=" << resp_->result().exit_status()
            << " cache_key=" << resp_->cache_key()
            << " flag=" << flag_dump_;
  DCHECK(requester_env_.fallback());
  DCHECK(!fail_fallback_);
  stdout_ = resp_->result().stdout_buffer();
  stderr_ = resp_->result().stderr_buffer();
  LogCompilerOutput(trace_id_, "stdout", stdout_);
  LogCompilerOutput(trace_id_, "stderr", stderr_);

  fail_fallback_ = true;
  // If we allow fallback, the result should be updated by the local run.
  // We don't need to show remote failures in such a case. (b/120168939)
  if (requester_env_.fallback()) {
    resp_->mutable_result()->clear_stdout_buffer();
    resp_->mutable_result()->clear_stderr_buffer();
  }
  // TODO: active fail fallback only for http error?
  // b/36576025 b/36577821
  if (!service_->IncrementActiveFailFallbackTasks()) {
    AddErrorToResponse(TO_USER,
                       "reached max number of active fail fallbacks. "
                       "Please check logs to understand the cause.",
                       true);
    if (delayed_setup_subproc_ != nullptr) {
      delayed_setup_subproc_->Cancel();
      delayed_setup_subproc_ = nullptr;
    }
    if (subproc_ != nullptr) {
      LOG(INFO) << trace_id_ << " killing subproc pid="
                << subproc_->started().pid();
      KillSubProcess();  // FinishSubProcess will be called.
    } else {
      ProcessReply();  // GOMA_FALLBACK=false or GOMA_USE_LOCAL=false
    }
    return;
  }
  if (subproc_ == nullptr) {
    // subproc_ might be nullptr (e.g. GOMA_USE_LOCAL=false).
    SetupSubProcess();
  }
  RunSubProcess(msg);
}

void CompileTask::ProcessReply() {
  VLOG(1) << trace_id_ << " process reply";
  DCHECK(BelongsToCurrentThread());
  CHECK_EQ(FINISHED, state_);
  CHECK(subproc_ == nullptr);
  CHECK(delayed_setup_subproc_ == nullptr);
  CHECK(!abort_);
  std::string msg;
  if (IsGomaccRunning()) {
    VLOG(2) << trace_id_ << " goma result:" << resp_->DebugString();
    if (local_run_ && service_->dont_kill_subprocess()) {
      // if we ran local process and dont_kill_subprocess is true, we just
      // use local results, so we don't need to rename remote outputs.
      CommitOutput(false);
      msg = "goma success, but local used";
    } else {
      CommitOutput(true);
      if (local_cache_hit()) {
        msg = "goma success (local cache hit)";
      } else if (cache_hit()) {
        msg = "goma success (cache hit)";
      } else {
        msg = "goma success";
      }
    }

    if (LocalOutputCache::IsEnabled()) {
      if (!local_cache_hit() && !local_output_cache_key_.empty() && success()) {
        // Here, local or remote output has been performed,
        // and output cache key exists.
        // Note: we need to save output before ReplyResponse. Otherwise,
        // output file might be removed by ninja.
        if (!LocalOutputCache::instance()->SaveOutput(local_output_cache_key_,
                                                      req_.get(),
                                                      resp_.get(),
                                                      trace_id_)) {
          LOG(ERROR) << trace_id_ << " failed to save localoutputcache";
        }
      }
    }
  } else {
    msg = "goma canceled";
  }

  if (!subproc_stdout_.empty()) remove(subproc_stdout_.c_str());
  if (!subproc_stderr_.empty()) remove(subproc_stderr_.c_str());
  ReplyResponse(msg);
}

struct CompileTask::RenameParam {
  std::string oldpath;
  std::string newpath;
};

void CompileTask::RenameCallback(RenameParam* param, std::string* err) {
  err->clear();
  int r = rename(param->oldpath.c_str(), param->newpath.c_str());
  if (r == 0) {
    return;
  }
  // if errno != EEXIST, log, AddErrorToResponse and returns without
  // setting *err (so no retry in DoOutput), since non-EEXIST error might
  // not be worth to retry?
  std::ostringstream ss;
  ss << "rename error:" << param->oldpath << " " << param->newpath
     << " errno=" << errno;
  *err = ss.str();
}

struct CompileTask::ContentOutputParam {
  std::string filename;
  OutputFileInfo* info = nullptr;
};

void CompileTask::ContentOutputCallback(ContentOutputParam* param,
                                        std::string* err) {
  err->clear();
  remove(param->filename.c_str());
  std::unique_ptr<FileDataOutput> fout(
      FileDataOutput::NewFileOutput(param->filename, param->info->mode));
  if (!fout->IsValid()) {
    std::ostringstream ss;
    ss << "open for write error:" << param->filename;
    *err = ss.str();
    return;
  }
  if (!fout->WriteAt(0L, param->info->content) || !fout->Close()) {
    std::ostringstream ss;
    ss << "write error:" << param->filename;
    *err = ss.str();
    return;
  }
}

#ifdef _WIN32
void CompileTask::DoOutput(const std::string& opname,
                           const std::string& filename,
                           PermanentClosure* closure,
                           std::string* err) {
  static const int kMaxDeleteRetryForDoOutput = 5;
  // Large sleep time will not harm a normal user.
  // Followings are executed after termination of the child process,
  // and deletion usually succeeds without retrying.
  static const int kInitialRetrySleepInMs = 100;
  // On Posix, rename success if target file already exists and it is
  // in writable directory.
  // On Win32, rename will fail if target file already exists, so we
  // need to delete it explicitly before rename.
  // In this code, we assume a file is temporary locked by a process
  // like AntiVirus, and the lock will be released for a while.
  //
  // You may consider to use MoveFileEx with MOVEFILE_REPLACE_EXISTING.
  // Calling it may take forever and stall compiler_proxy if the process
  // having the lock is not behaving. As a result, we do not use it.
  int sleep_in_ms = kInitialRetrySleepInMs;
  for (int retry = 0; retry < kMaxDeleteRetryForDoOutput; ++retry) {
    closure->Run();
    if (err->empty()) {
      return;
    }
    LOG(WARNING) << trace_id_ << " DoOutput operation failed."
                 << " opname=" << opname
                 << " filename=" << filename
                 << " err=" << *err;

    // TODO: identify a process that has a file lock.
    // As far as I know, people seems to use NtQueryInformationProcess,
    // which is an obsoleted function, to list up processes.

    // http://msdn.microsoft.com/en-us/library/windows/desktop/aa364944(v=vs.85).aspx
    DWORD attr = GetFileAttributesA(filename.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
      LOG_SYSRESULT(GetLastError());
      std::ostringstream ss;
      ss << opname << " failed but GetFileAttributes "
         << "returns INVALID_FILE_ATTRIBUTES"
         << " filename=" << filename
         << " attr=" << attr;
      AddErrorToResponse(TO_USER, ss.str(), true);
      return;
    }

    LOG(INFO) << trace_id_ << " "
              << "The file exists. We need to remove."
              << " filename=" << filename
              << " attr=" << attr;
    if (remove(filename.c_str()) == 0) {
      LOG(INFO) << trace_id_ << " "
                << "Delete succeeds."
                << " filename=" << filename;
      continue;
    }

    LOG(WARNING) << trace_id_ << " "
                 << "Failed to delete file:"
                 << " filename=" << filename
                 << " retry=" << retry
                 << " sleep_in_ms=" << sleep_in_ms;
    Sleep(sleep_in_ms);
    sleep_in_ms *= 2;
  }
  if (err->empty()) {
    std::ostringstream ss;
    ss << opname << " failed but err is empty?";
    *err = ss.str();
  }
  PLOG(ERROR) << trace_id_ << " " << *err;
  AddErrorToResponse(TO_USER, *err, true);
}
#else
void CompileTask::DoOutput(const std::string& opname,
                           const std::string& filename,
                           PermanentClosure* closure,
                           std::string* err) {
  closure->Run();
  if (!err->empty()) {
    PLOG(ERROR) << trace_id_ << " DoOutput operation failed."
                << " opname=" << opname
                << " filename=" << filename
                << " err=" << *err;
    AddErrorToResponse(TO_USER, *err, true);
  }
}
#endif

void CompileTask::RewriteCoffTimestamp(const std::string& filename) {
  absl::string_view ext = file::Extension(filename);
  if (ext != "obj")
    return;

  ScopedFd fd(ScopedFd::OpenForRewrite(filename));
  if (!fd.valid()) {
    LOG(ERROR) << trace_id_ << " failed to open file for coff rewrite: "
               << filename;
    return;
  }

  // Check COFF file header. COFF header is like this.
  // c.f. http://delorie.com/djgpp/doc/coff/
  // 0-1   version. must be 0x014c for x86, 0x8664 for x64
  // 2-3   number of sections (not necessary for us)
  // 4-7   timestamp
  // ...
  //
  // All numeric fields are stored in host native order.
  // Currently we're checking magic is x86 or x64, all numeric
  // should be little endian here.
  //
  // When /bigobj is specified in cl.exe, microsoft extends COFF file format
  // to accept more sections.
  // In this case, the file header is like this:
  // 0-1   0x0000 (IMAGE_FILE_MACHINE_UNKNOWN)
  // 2-3   0xFFFF
  // 4-5   version (0x0001 or 0x0002)
  // 6-7   machine (0x014c or 0x8664)
  // 8-11  timestamp
  // 12-27 uuid: 38feb30ca5d9ab4dac9bd6b6222653c2 for version 0x0001
  //             c7a1bad1eebaa94baf20faf66aa4dcb8 for version 0x0002
  //
  // TODO: Find bigobj version 1 document and add link here.

  unsigned char buf[32];
  ssize_t read_byte = fd.Read(buf, sizeof(buf));
  if (read_byte != sizeof(buf)) {
    LOG(ERROR) << trace_id_
               << " couldn't read the first " << sizeof(buf)
               << " byte. file is too small?"
               << " filename=" << filename
               << " read_byte=" << read_byte;
    return;
  }

  unsigned short magic = *reinterpret_cast<unsigned short*>(buf);
  int offset = 0;
  if (magic == 0x014c || magic == 0x8664) {
    offset = 4;
  } else if (IsBigobjFormat(buf)) {
    offset = 8;
  }
  if (offset > 0) {
    unsigned int old = *reinterpret_cast<unsigned int*>(buf + offset);
    unsigned int now = time(nullptr);

    fd.Seek(offset, ScopedFd::SeekAbsolute);
    fd.Write(&now, 4);

    LOG(INFO) << trace_id_
              << " Rewriting timestamp:" << " file=" << filename
              << " offset=" << offset
              << " old=" << old << " new=" << now;
    return;
  }

  std::stringstream ss;
  for (size_t i = 0; i < sizeof(buf); ++i) {
    ss << std::hex << std::setw(2) << std::setfill('0')
       << (static_cast<unsigned int>(buf[i]) & 0xFF);
  }
  LOG(ERROR) << trace_id_
             << " Unknown COFF header."
             << " filename=" << filename
             << " first " << sizeof(buf) << "byte=" << ss.str();
  return;
}

void CompileTask::CommitOutput(bool use_remote) {
  VLOG(1) << trace_id_ << " commit output " << use_remote;
  DCHECK(BelongsToCurrentThread());
  CHECK(state_ == FINISHED);
  CHECK(!abort_);
  CHECK(subproc_ == nullptr);
  CHECK(delayed_setup_subproc_ == nullptr);

  std::vector<std::string> output_bases;
  bool has_obj = false;

  for (auto& info : output_file_infos_) {
    SimpleTimer timer;
    const std::string& filename = info.filename;
    const std::string& tmp_filename = info.tmp_filename;
    // TODO: fix to support cas digest.
    const std::string& hash_key = info.hash_key;
    DCHECK(!use_remote || !hash_key.empty())
        << trace_id_ << " if remote is used, hash_key must be set."
        << " filename=" << filename;
    const bool use_content = tmp_filename.empty();
    bool need_rename = !tmp_filename.empty() && tmp_filename != filename;
    if (!use_remote) {
      // If use_remote is false, we should have outputs of local process.
      VLOG(1) << trace_id_ << " commit output (use local) in "
              << filename;
      if (access(filename.c_str(), R_OK) == 0) {
        if (need_rename) {
          // We might have written tmp file for remote output, but decided
          // to use local output.
          // In this case, we want to remove tmp file of remote output.
          remove(tmp_filename.c_str());
        }
      } else {
        // !use_remote, but local output doesn't exist?
        PLOG(ERROR) << trace_id_ << " " << filename;
      }
      if (use_content) {
        VLOG(1) << trace_id_ << " release buffer of remote output";
        service_->ReleaseOutputBuffer(info.size, &info.content);
      }
      need_rename = false;
    } else if (use_content) {
      // If use_remote is true, and use_content is true,
      // write content (remote output) in filename.
      VLOG(1) << trace_id_ << " commit output (use remote content) to "
              << filename;
      ContentOutputParam param;
      param.filename = filename;
      param.info = &info;
      std::string err;
      std::unique_ptr<PermanentClosure> callback(
          NewPermanentCallback(
              this,
              &CompileTask::ContentOutputCallback,
              &param, &err));
      DoOutput("content_output", filename, callback.get(), &err);
      service_->ReleaseOutputBuffer(info.size, &info.content);
      need_rename = false;
    } else if (need_rename) {
      // If use_remote is true, use_content is false, and
      // need_rename is true, we wrote remote output in
      // tmp_filename, and we need to rename tmp_filename
      // to filename.
      VLOG(1) << trace_id_ << " commit output (use remote tmp file) "
              << "rename " << tmp_filename << " => " << filename;
      RenameParam param;
      param.oldpath = tmp_filename;
      param.newpath = filename;
      std::string err;
      std::unique_ptr<PermanentClosure> callback(
          NewPermanentCallback(
             this, &CompileTask::RenameCallback, &param, &err));
      DoOutput("rename", filename, callback.get(), &err);
    } else {
      // If use_remote is true, use_content is false, and
      // need_rename is false, we wrote remote output in
      // filename, so do nothing here.
      VLOG(1) << trace_id_ << " commit output (use remote file) in "
              << filename;
    }

    // Incremental Link doesn't work well if object file timestamp is wrong.
    // If it's Windows object file (.obj) from remote,
    // we'd like to rewrite timestamp when the content is from remote cache.
    // According to our measurement, this doesn't have
    // measureable performance penalty.
    // see b/24388745
    if (use_remote && stats_->exec_log.cache_hit() &&
        flags_->type() == CompilerFlagType::Clexe) {
      // We should not rewrite coff if /Brepro or something similar is set.
      // See b/72768585
      const VCFlags& vc_flag = static_cast<const VCFlags&>(*flags_);
      if (!vc_flag.has_Brepro()) {
        RewriteCoffTimestamp(filename);
      }
    }

    service_->RecordOutputRename(need_rename);
    // The output file is generated in goma cache, so we believe the cache_key
    // is valid.  It would be used in link phase.
    service_->file_hash_cache()->StoreFileCacheKey(
        filename, hash_key, absl::Now(),
        output_file_stat_cache_->Get(filename));
    VLOG(1) << trace_id_ << " "
            << tmp_filename << " -> " << filename
            << " " << hash_key;
    LOG_IF(ERROR, !info.content.empty())
        << trace_id_ << " content was not released: " << filename;
    const absl::Duration duration = timer.GetDuration();
    LOG_IF(WARNING, duration > absl::Milliseconds(100))
          << trace_id_
          << " CommitOutput " << duration
          << " size=" << info.size
          << " filename=" << info.filename;
    absl::string_view output_base = file::Basename(info.filename);
    output_bases.push_back(std::string(output_base));
    absl::string_view ext = file::Extension(output_base);
    if (flags_->type() == CompilerFlagType::Gcc && ext == "o") {
      has_obj = true;
    } else if (flags_->type() == CompilerFlagType::Clexe && ext == "obj") {
      has_obj = true;
    } else if (flags_->type() == CompilerFlagType::Javac && ext == "class") {
      has_obj = true;
    }
  }
  output_file_infos_.clear();

  // TODO: For clang-tidy, maybe we don't need to output
  // no obj warning?

  if (has_obj) {
    LOG(INFO) << trace_id_ << " CommitOutput num=" << output_bases.size()
              << " cache_key=" << resp_->cache_key()
              << ": " << output_bases;
  } else {
    LOG(WARNING) << trace_id_ << " CommitOutput num=" << output_bases.size()
                 << " no obj: cache_key=" << resp_->cache_key()
                 << ": " << output_bases;
  }
}

// static
std::string CompileTask::OmitDurationFromUserError(absl::string_view str) {
  absl::string_view::size_type colon_pos = str.find(':');
  if (colon_pos == absl::string_view::npos) {
    return std::string(str);
  }

  return absl::StrCat("compiler_proxy <duration omitted>",
                      str.substr(colon_pos));
}

void CompileTask::ReplyResponse(const std::string& msg) {
  LOG(INFO) << trace_id_ << " ReplyResponse: " << msg;
  DCHECK(BelongsToCurrentThread());
  CHECK(state_ == FINISHED || state_ == LOCAL_FINISHED || abort_);
  CHECK(rpc_ != nullptr);
  CHECK(rpc_resp_ != nullptr);
  CHECK(subproc_ == nullptr);
  CHECK(delayed_setup_subproc_ == nullptr);

  if (failed() || fail_fallback_) {
    auto allowed_error_duration = service_->AllowedNetworkErrorDuration();
    auto error_start_time = service_->http_client()->NetworkErrorStartedTime();
    if (allowed_error_duration.has_value() && error_start_time.has_value()) {
      if (absl::Now() > *error_start_time + *allowed_error_duration) {
        AddErrorToResponse(
            TO_USER, "network error continued for a long time", true);
      }
    }
  }

  if (resp_->has_result()) {
    VLOG(1) << trace_id_ << " exit=" << resp_->result().exit_status();
    stats_->exec_log.set_exec_exit_status(resp_->result().exit_status());
  } else {
    LOG(WARNING) << trace_id_ << " empty result";
    stats_->exec_log.set_exec_exit_status(-256);
  }
  if (service_->local_run_for_failed_input() && flags_.get() != nullptr) {
    service_->RecordInputResult(flags_->input_filenames(),
                                stats_->exec_log.exec_exit_status() == 0);
  }
  if (resp_->error_message_size() != 0) {
    std::vector<std::string> errs(resp_->error_message().begin(),
                                  resp_->error_message().end());
    LOG_IF(ERROR, resp_->result().exit_status() == 0)
        << trace_id_ << " should not have error message on exit_status=0."
        << " errs=" << errs;
    std::transform(errs.begin(), errs.end(), errs.begin(),
                   OmitDurationFromUserError);
    service_->RecordErrorsToUser(errs);
  }
  UpdateStats();
  *rpc_resp_ = *resp_;
  // Here, rpc_resp_ has created, so we can set gomacc_resp_size. b/109783082
  stats_->gomacc_resp_size = rpc_resp_->ByteSize();

  OneshotClosure* done = done_;
  done_ = nullptr;
  rpc_resp_ = nullptr;
  rpc_ = nullptr;
  if (done) {
    service_->wm()->RunClosureInThread(
        FROM_HERE,
        caller_thread_id_, done, WorkerThread::PRIORITY_IMMEDIATE);
  }
  if (!canceled_ && stats_->exec_log.exec_exit_status() != 0) {
    if (exit_status_ == 0 && subproc_exit_status_ == 0) {
      stats_->exec_log.set_compiler_proxy_error(true);
      LOG(ERROR) << trace_id_ << " compilation failure "
                 << "due to compiler_proxy error.";
    }
  }

  // The caching of the HTTP return code in |response_code_| might not be
  // required, but let's keep it to be safe. If we directly wrote
  // |http_rpc_status_->http_return_code| to the JSON output in DumpToJson(), it
  // will probably work. However, there is no guarantee that http_rpc_status_
  // won't be overwritten by a new call to ProcessCallExec() before calling
  // DumpToJson().
  response_code_ = http_rpc_status_->http_return_code;

  stats_->handler_time = handler_timer_.GetDuration();
  stats_->exec_log.set_handler_time(DurationToIntMs(stats_->handler_time));
  gomacc_pid_ = SubProcessState::kInvalidPid;

  if (stats_->handler_time > absl::Minutes(5)) {
    ExecLog stats = stats_->exec_log;
    // clear non-stats fields.
    stats.clear_username();
    stats.clear_nodename();
    stats.clear_port();
    stats.clear_compiler_proxy_start_time();
    stats.clear_task_id();
    stats.clear_compiler_proxy_user_agent();
    stats.clear_start_time();
    stats.clear_arg();
    stats.clear_env();
    stats.clear_cwd();
    stats.clear_expanded_arg();
    stats.clear_command_version();
    stats.clear_command_target();
    LOG(ERROR) << trace_id_ << " SLOW:" << stats.DebugString();
  }

  // if abort_, remote process is still on the fly.
  // Done() will be called later in ProcessFinished.
  if (abort_)
    CHECK(!finished_);
  // if local_output_file_callback_ is not nullptr, uploading local output file
  // is on the fly, so ProcessLocalFileOutputDone() will be called later.
  if (finished_ && local_output_file_callback_ == nullptr) {
    CHECK_GE(state_, FINISHED);
    CHECK_EQ(0, num_local_output_file_task_);
    Done();
  }
}

void CompileTask::ProcessLocalFileOutput() {
  VLOG(1) << trace_id_ << " local output";
  CHECK(BelongsToCurrentThread());
  CHECK(local_output_file_callback_ == nullptr);
  CHECK_EQ(0, num_local_output_file_task_);
  if (!service_->store_local_run_output())
    return;

  SetLocalOutputFileCallback();
  std::vector<OneshotClosure*> closures;
  for (const auto& output_file : flags_->output_files()) {
    const std::string& filename =
        file::JoinPathRespectAbsolute(flags_->cwd(), output_file);
    // only uploads *.o
    if (!absl::EndsWith(filename, ".o"))
      continue;
    std::string hash_key;
    const FileStat& output_file_stat = output_file_stat_cache_->Get(filename);
    bool found_in_cache = service_->file_hash_cache()->GetFileCacheKey(
        filename, absl::nullopt, output_file_stat, &hash_key);
    if (found_in_cache) {
      VLOG(1) << trace_id_ << " file:" << filename
              << " already on cache: " << hash_key;
      continue;
    }
    LOG(INFO) << trace_id_ << " local output:" << filename;
    std::unique_ptr<LocalOutputFileTask> local_output_file_task(
        new LocalOutputFileTask(
            service_->wm(),
            service_->blob_client()->NewUploader(
                filename, requester_info_, trace_id_),
            service_->file_hash_cache(), output_file_stat_cache_->Get(filename),
            this, filename));

    LocalOutputFileTask* local_output_file_task_pointer =
        local_output_file_task.get();

    closures.push_back(
        NewCallback(
            local_output_file_task_pointer,
            &LocalOutputFileTask::Run,
            NewCallback(
                this,
                &CompileTask::LocalOutputFileTaskFinished,
                std::move(local_output_file_task))));
  }
  if (closures.empty()) {
    VLOG(1) << trace_id_ << " no local output upload";
    service_->wm()->RunClosureInThread(
        FROM_HERE,
        thread_id_,
        NewCallback(
            this,
            &CompileTask::MaybeRunLocalOutputFileCallback, false),
        WorkerThread::PRIORITY_LOW);
    return;
  }
  for (auto* closure : closures)
    service_->wm()->RunClosure(
        FROM_HERE, closure, WorkerThread::PRIORITY_LOW);
}

void CompileTask::ProcessLocalFileOutputDone() {
  VLOG(1) << trace_id_ << " local output done";
  CHECK(BelongsToCurrentThread());
  local_output_file_callback_ = nullptr;
  if (finished_) {
    CHECK(subproc_ == nullptr);
    CHECK(delayed_setup_subproc_ == nullptr);
    Done();
    return;
  }
  // if !finished_, remote call is still on the fly, and eventually
  // ProcessFinished will be called, and Done will be called
  // because local_output_file_callback_ is already nullptr.
}

void CompileTask::Done() {
  VLOG(1) << trace_id_ << " Done";
  // FINISHED: normal case.
  // LOCAL_FINISHED: fallback by should_fallback_.
  // abort_: idle fallback.
  replied_ = true;
  if (!abort_)
    CHECK_GE(state_, FINISHED);
  CHECK(rpc_ == nullptr) << trace_id_
                      << " " << StateName(state_) << " abort:" << abort_;
  CHECK(rpc_resp_ == nullptr);
  CHECK(done_ == nullptr);
  CHECK(subproc_ == nullptr);
  CHECK(delayed_setup_subproc_ == nullptr);
  CHECK(input_file_callback_ == nullptr);
  CHECK(output_file_callback_ == nullptr);
  CHECK(local_output_file_callback_ == nullptr);
  ClearOutputFile();

  // If compile failed, delete deps cache entry here.
  if (DepsCache::IsEnabled()) {
    if ((failed() || fail_fallback_) && deps_identifier_.has_value()) {
      DepsCache::instance()->RemoveDependency(deps_identifier_);
      LOG(INFO) << trace_id_ << " remove deps cache entry.";
    }
  }

  SaveInfoFromInputOutput();
  service_->CompileTaskDone(this);
  VLOG(1) << trace_id_ << " finalized.";
}

void CompileTask::DumpToJson(bool need_detail, Json::Value* root) const {
  SubProcessState::State subproc_state = SubProcessState::NUM_STATE;
  pid_t subproc_pid = static_cast<pid_t>(SubProcessState::kInvalidPid);
  {
    AUTOLOCK(lock, &mu_);
    if (subproc_ != nullptr) {
      subproc_state = subproc_->state();
      subproc_pid = subproc_->started().pid();
    }
  }

  stats_->DumpToJson(root,
                     need_detail ? CompileStats::DumpDetailLevel::kDetailed
                                 : CompileStats::DumpDetailLevel::kNotDetailed);

  (*root)["id"] = id_;

  if ((state_ < FINISHED && !abort_) || state_ == LOCAL_RUN) {
    // Elapsed total time for current running process.
    // This field needs to be strictly in milliseconds so that it can be sorted.
    (*root)["elapsed"] =
        FormatDurationInMilliseconds(handler_timer_.GetDuration());
  }
  if (gomacc_pid_ != SubProcessState::kInvalidPid)
    (*root)["pid"] = gomacc_pid_;
  if (!flag_dump_.empty())
    (*root)["command"] = flag_dump_;
  (*root)["state"] = StateName(state_);
  if (abort_) (*root)["abort"] = 1;
  if (subproc_pid != static_cast<pid_t>(SubProcessState::kInvalidPid)) {
    (*root)["subproc_state"] =
        SubProcessState::State_Name(subproc_state);
    (*root)["subproc_pid"] = Json::Value::Int64(subproc_pid);
  }
  // for task color.
  if (response_code_)
    (*root)["http_status"] = response_code_;
  if (fail_fallback_) (*root)["fail_fallback"]= 1;
  if (canceled_)
    (*root)["canceled"] = 1;
  if (replied_)
    (*root)["replied"] = 1;

  // additional message
  if (gomacc_revision_mismatched_) {
    (*root)["gomacc_revision_mismatch"] = 1;
  }

  if (need_detail) {
    if (num_input_file_task_ > 0) {
      (*root)["num_input_file_task"] = num_input_file_task_;
    }
    {
      AUTOLOCK(lock, &mu_);
      if (!http_rpc_status_->response_header.empty()) {
        (*root)["response_header"] = http_rpc_status_->response_header;
      }
    }

    if (exec_output_files_.size() > 0) {
      Json::Value exec_output_files(Json::arrayValue);
      for (size_t i = 0; i < exec_output_files_.size(); ++i) {
        exec_output_files.append(exec_output_files_[i]);
      }
      (*root)["exec_output_files"] = exec_output_files;
    }
    if (!resp_cache_key_.empty())
      (*root)["cache_key"] = resp_cache_key_;

    if (exec_error_message_.size() > 0) {
      Json::Value error_message(Json::arrayValue);
      for (size_t i = 0; i < exec_error_message_.size(); ++i) {
        error_message.append(exec_error_message_[i]);
      }
      (*root)["error_message"] = error_message;
    }
    if (!orig_flag_dump_.empty())
        (*root)["orig_flag"] = orig_flag_dump_;
    if (!stdout_.empty())
      (*root)["stdout"] = stdout_;
    if (!stderr_.empty())
      (*root)["stderr"] = stderr_;

    Json::Value input_files(Json::arrayValue);
    for (const auto& file : required_files_) {
      input_files.append(file);
    }
    (*root)["input_files"] = input_files;
    (*root)["total_input_file_size"] = sum_of_required_file_size_;

    if (system_library_paths_.size() > 0) {
      Json::Value system_library_paths(Json::arrayValue);
      for (size_t i = 0; i < system_library_paths_.size(); ++i) {
        system_library_paths.append(system_library_paths_[i]);
      }
      (*root)["system_library_paths"] = system_library_paths;
    }

  } else {
    (*root)["summaryOnly"] = 1;
  }
}

// ----------------------------------------------------------------
// state_: INIT
void CompileTask::CopyEnvFromRequest() {
  CHECK_EQ(INIT, state_);
  requester_env_ = req_->requester_env();
  want_fallback_ = requester_env_.fallback();
  req_->clear_requester_env();
}

void CompileTask::InitCompilerFlags() {
  CHECK_EQ(INIT, state_);
  std::vector<std::string> args(req_->arg().begin(), req_->arg().end());
  VLOG(1) << trace_id_ << " " << args;
  flags_ = CompilerFlagsParser::New(args, req_->cwd());
  if (flags_.get() == nullptr) {
    return;
  }
  compiler_type_specific_ =
      service_->compiler_type_specific_collection()->Get(flags_->type());

  flag_dump_ = flags_->DebugString();
  if (flags_->type() == CompilerFlagType::ClangTidy) {
    InitClangTidyFlags(static_cast<ClangTidyFlags*>(flags_.get()));
  }

  if (!flags_->is_successful()) {
    LOG(WARNING) << trace_id_ << " " << flags_->fail_message();
  }
}

bool CompileTask::FindLocalCompilerPath() {
  CHECK_EQ(INIT, state_);
  CHECK(flags_.get());

  // If gomacc sets local_compiler_path, just use it.
  if (!req_->command_spec().local_compiler_path().empty()) {
    std::string local_compiler = PathResolver::PlatformConvert(
        req_->command_spec().local_compiler_path());

    // TODO: confirm why local_compiler_path should not be
    //                    basename, and remove the code if possible.
    // local_compiler_path should not be basename only.
    if (local_compiler.find(PathResolver::kPathSep) == std::string::npos) {
      LOG(ERROR) << trace_id_ << " local_compiler_path should not be basename:"
                 << local_compiler;
    } else if (service_->FindLocalCompilerPath(
                   requester_env_.gomacc_path(), local_compiler,
                   stats_->exec_log.cwd(), requester_env_.local_path(),
                   pathext_, &local_compiler, &local_path_)) {
      // Since compiler_info resolves relative path to absolute path,
      // we do not need to make local_comiler_path to absolute path
      // any more. (b/6340137, b/28088682)
      if (!pathext_.empty() &&
          !absl::EndsWith(local_compiler,
                             req_->command_spec().local_compiler_path())) {
        // PathExt should be resolved on Windows.  Let me use it.
        req_->mutable_command_spec()->set_local_compiler_path(local_compiler);
      }
      return true;
    }
    return false;
  }

  if (!requester_env_.has_local_path() ||
      requester_env_.local_path().empty()) {
    LOG(ERROR) << trace_id_ << " no PATH in requester env."
               << requester_env_.DebugString();
    AddErrorToResponse(TO_USER,
                       "no PATH in requester env.  Using old gomacc?", true);
    return false;
  }
  if (!requester_env_.has_gomacc_path()) {
    LOG(ERROR) << trace_id_ << " no gomacc path in requester env."
               << requester_env_.DebugString();
    AddErrorToResponse(TO_USER,
                       "no gomacc in requester env.  Using old gomacc?", true);
    return false;
  }

  std::string local_compiler_path;
  if (service_->FindLocalCompilerPath(
          requester_env_.gomacc_path(), flags_->compiler_base_name(),
          stats_->exec_log.cwd(), requester_env_.local_path(), pathext_,
          &local_compiler_path, &local_path_)) {
    req_->mutable_command_spec()->set_local_compiler_path(
          local_compiler_path);
    return true;
  }
  return false;
}

bool CompileTask::ShouldFallback() const {
  CHECK_EQ(INIT, state_);
  CHECK(flags_.get());
  if (!requester_env_.verify_command().empty())
    return false;
  if (!flags_->is_successful()) {
    service_->RecordForcedFallbackInSetup(CompileService::kFailToParseFlags);
    LOG(INFO) << trace_id_
              << " force fallback. failed to parse compiler flags.";
    return true;
  }
  if (flags_->input_filenames().empty()) {
    service_->RecordForcedFallbackInSetup(
        CompileService::kNoRemoteCompileSupported);
    LOG(INFO) << trace_id_
              << " force fallback. no input files give.";
    return true;
  }
  if (!compiler_type_specific_->RemoteCompileSupported(trace_id_, *flags_,
                                                       verify_output_)) {
    service_->RecordForcedFallbackInSetup(
        CompileService::kNoRemoteCompileSupported);
    LOG(INFO) << trace_id_
              << " force fallback. due to compiler specific reason.";
    return true;
  }

#ifndef _WIN32
  // TODO: check "NUL", "CON", "AUX" on windows?
  for (const auto & input_filename : flags_->input_filenames()) {
    const std::string input =
        file::JoinPathRespectAbsolute(flags_->cwd(), input_filename);
    struct stat st;
    if (stat(input.c_str(), &st) != 0) {
      PLOG(INFO) << trace_id_ << " " << input << ": stat error";
      service_->RecordForcedFallbackInSetup(
          CompileService::kNoRemoteCompileSupported);
      return true;
    }
    if (!S_ISREG(st.st_mode)) {
      LOG(INFO) << trace_id_ << " " << input << " not regular file";
      service_->RecordForcedFallbackInSetup(
          CompileService::kNoRemoteCompileSupported);
      return true;
    }
  }
#endif

  // TODO: fallback input file should be flag of compiler proxy?
  if (requester_env_.fallback_input_file_size() == 0)
    return false;

  std::vector<std::string> fallback_input_files(
      requester_env_.fallback_input_file().begin(),
      requester_env_.fallback_input_file().end());
  std::sort(fallback_input_files.begin(), fallback_input_files.end());
  for (const auto& input_filename : flags_->input_filenames()) {
    if (binary_search(fallback_input_files.begin(),
                      fallback_input_files.end(),
                      input_filename)) {
      service_->RecordForcedFallbackInSetup(CompileService::kRequestedByUser);
      return true;
    }
  }
  return false;
}

bool CompileTask::ShouldVerifyOutput() const {
  CHECK_EQ(INIT, state_);
  return requester_env_.verify_output();
}

SubProcessReq::Weight CompileTask::GetTaskWeight() const {
  CHECK_EQ(INIT, state_);
  int weight_score = req_->arg_size();
  if (flags_->is_linking())
    weight_score *= 10;

  if (weight_score > 1000)
    return SubProcessReq::HEAVY_WEIGHT;
  return SubProcessReq::LIGHT_WEIGHT;
}

bool CompileTask::ShouldStopGoma() const {
  if (verify_output_)
    return false;
  if (flags_->is_precompiling_header() && service_->enable_gch_hack())
    return false;
  if (subproc_ == nullptr) {
    DCHECK(!abort_);
    return false;
  }
  if (IsSubprocRunning()) {
    if (service_->dont_kill_subprocess()) {
      // When dont_kill_subprocess is true, we'll ignore remote results and
      // always use local results, so calling remote is not useless when
      // subprocess is already running.
      return true;
    }
    if (service_->local_run_preference() >= state_)
      return true;
  }
  if (stats_->exec_log.exec_request_retry() > 1) {
    int num_pending = SubProcessTask::NumPending();
    // Prefer local when pendings are few.
    return num_pending <= service_->max_subprocs_pending();
  }
  if (service_->http_client()->ramp_up() == 0) {
    // If http blocked (i.e. got 302, 403 error), stop calling remote.
    LOG(INFO) << trace_id_ << " stop goma. http disabled";
    return true;
  }
  return false;
}

// ----------------------------------------------------------------
// state_: SETUP
void CompileTask::FillCompilerInfo() {
  CHECK_EQ(SETUP, state_);

  compiler_info_timer_.Start();

  std::vector<std::string> key_envs(stats_->exec_log.env().begin(),
                                    stats_->exec_log.env().end());
  std::vector<std::string> run_envs(key_envs);
  if (!local_path_.empty())
    run_envs.push_back("PATH=" + local_path_);
#ifdef _WIN32
  if (!pathext_.empty())
    run_envs.push_back("PATHEXT=" + pathext_);
  if (flags_->type() == CompilerFlagType::Clexe) {
    run_envs.push_back("TMP=" + service_->tmp_dir());
    run_envs.push_back("TEMP=" + service_->tmp_dir());
  }
#endif
  auto param = std::make_unique<GetCompilerInfoParam>();
  param->thread_id = service_->wm()->GetCurrentThreadId();
  param->trace_id = trace_id_;
  DCHECK_NE(
      req_->command_spec().local_compiler_path().find(PathResolver::kPathSep),
      std::string::npos)
      << trace_id_
      << " expect local_compiler_path is relative path"
         " or absolute path but "
      << req_->command_spec().local_compiler_path();
  param->key = CompilerInfoCache::CreateKey(
      *flags_,
      req_->command_spec().local_compiler_path(),
      key_envs);
  param->flags = flags_.get();
  param->run_envs = run_envs;

  GetCompilerInfoParam* param_pointer = param.get();
  service_->GetCompilerInfo(
      param_pointer,
      NewCallback(
          this, &CompileTask::FillCompilerInfoDone, std::move(param)));
}

void CompileTask::FillCompilerInfoDone(
    std::unique_ptr<GetCompilerInfoParam> param) {
  CHECK_EQ(SETUP, state_);

  const absl::Duration compiler_info_time = compiler_info_timer_.GetDuration();
  stats_->exec_log.set_compiler_info_process_time(
      DurationToIntMs(compiler_info_time));
  stats_->compiler_info_process_time = compiler_info_time;
  std::ostringstream ss;
  ss << " cache_hit=" << param->cache_hit
     << " updated=" << param->updated
     << " state=" << param->state.get()
     << " in " << compiler_info_time;
  if (compiler_info_time > absl::Seconds(1)) {
    LOG(WARNING) << trace_id_ << " SLOW fill compiler info"
                 << ss.str();
  } else {
    LOG(INFO) << trace_id_ << " fill compiler info"
              << ss.str();
  }

  if (param->state.get() == nullptr) {
    AddErrorToResponse(TO_USER,
                       "something wrong trying to get compiler info.", true);
    service_->RecordForcedFallbackInSetup(
        CompileService::kFailToGetCompilerInfo);
    SetupRequestDone(false);
    return;
  }

  compiler_info_state_ = std::move(param->state);
  DCHECK(compiler_info_state_.get() != nullptr);

  if (compiler_info_state_.get()->info().HasError()) {
    // In this case, it found local compiler, but failed to get necessary
    // information, such as system include paths.
    // It would happen when multiple -arch options are used.
    if (!service_->fail_fast() && want_fallback_) {
      // Force to fallback mode to handle this case.
      should_fallback_ = true;
      service_->RecordForcedFallbackInSetup(
          CompileService::kFailToGetCompilerInfo);
    }
    AddErrorToResponse(should_fallback_ ? TO_LOG : TO_USER,
                       compiler_info_state_.get()->info().error_message(),
                       true);
    SetupRequestDone(false);
    return;
  }
  if (compiler_info_state_.disabled()) {
    // In this case, it found local compiler, but not in server side
    // (by past compile task).
    if (!service_->fail_fast() && want_fallback_) {
      should_fallback_ = true;
      service_->RecordForcedFallbackInSetup(CompileService::kCompilerDisabled);
    }
    // we already responded "<local compiler path> is disabled" when it
    // was disabled the compiler info, so won't show the same error message
    // to user.
    AddErrorToResponse(TO_LOG, "compiler is disabled", true);
    SetupRequestDone(false);
    return;
  }
  if (service_->hermetic()) {
    req_->set_hermetic_mode(true);
  }
#ifndef _WIN32
  if (service_->use_relative_paths_in_argv()) {
    MakeWeakRelativeInArgv();
  }
#endif
  MayUpdateSubprogramSpec();
  UpdateExpandedArgs();
  if (service_->send_expected_outputs()) {
    SetExpectedOutputs(req_.get(), *flags_);
  }
  SetCompilerResources();

  ModifyRequestArgs();
  ModifyRequestEnvs();
  UpdateCommandSpec();
  UpdateRequesterInfo();
  stats_->exec_log.set_command_version(req_->command_spec().version());
  stats_->exec_log.set_command_target(req_->command_spec().target());

  UpdateRequiredFiles();
}

void CompileTask::UpdateRequiredFiles() {
  CHECK_EQ(SETUP, state_);
  include_timer_.Start();
  include_wait_timer_.Start();

  // Go to the general include processor phase.
  StartIncludeProcessor();
}

void CompileTask::UpdateRequiredFilesDone(bool ok) {
  if (!ok) {
    // Failed to update required_files.
    if (requester_env_.verify_command().empty()) {
      if (!canceled_ && !abort_) {
        LOG(INFO) << trace_id_ << " failed to update required files. ";
        service_->RecordForcedFallbackInSetup(
            CompileService::kFailToUpdateRequiredFiles);
        should_fallback_ = true;
      }
      SetupRequestDone(false);
      return;
    }
    VLOG(1) << trace_id_ << "verify_command="
            << requester_env_.verify_command();
  }
  // Add the input files as well.
  for (const auto& input_filename : flags_->input_filenames()) {
    required_files_.insert(input_filename);
  }
  for (const auto& opt_input_filename: flags_->optional_input_filenames()) {
    const std::string& abs_filename = file::JoinPathRespectAbsolute(
        stats_->exec_log.cwd(), opt_input_filename);
    if (access(abs_filename.c_str(), R_OK) == 0) {
      required_files_.insert(opt_input_filename);
    } else {
      LOG(WARNING) << trace_id_ << " optional file not found:" << abs_filename;
    }
  }
  // If gomacc sets input file, add them as well.
  for (const auto& input : req_->input()) {
    required_files_.insert(input.filename());
  }
  if (VLOG_IS_ON(2)) {
    for (const auto& required_file : required_files_) {
      LOG(INFO) << trace_id_ << " required files:" << required_file;
    }
  }
  req_->clear_input();

  for (const auto& input : required_files_) {
    const std::string& path =
        file::JoinPathRespectAbsolute(flags_->cwd(), input);
    const auto stat = input_file_stat_cache_->Get(path);
    if (stat.IsValid()) {
      sum_of_required_file_size_ += stat.size;
    } else {
      LOG(ERROR) << trace_id_ << " invalid file stat " << path;
    }
  }

  const absl::Duration include_preprocess_time = include_timer_.GetDuration();
  stats_->exec_log.set_include_preprocess_time(
      DurationToIntMs(include_preprocess_time));
  stats_->include_preprocess_time = include_preprocess_time;
  stats_->exec_log.set_depscache_used(depscache_used_);

  LOG_IF(WARNING, stats_->include_processor_run_time > absl::Seconds(1))
      << trace_id_ << " SLOW run IncludeProcessor"
      << " required_files=" << required_files_.size()
      << " depscache=" << depscache_used_
      << " in " << stats_->include_processor_run_time;

  SetupRequestDone(true);
}

void CompileTask::SetupRequestDone(bool ok) {
  CHECK_EQ(SETUP, state_);

  if (abort_) {
    // subproc of local idle was already finished.
    ProcessFinished("aborted in setup");
    return;
  }

  if (canceled_) {
    ProcessFinished("canceled in setup");
    return;
  }

  if (!ok) {
    if (should_fallback_) {
      VLOG(1) << trace_id_ << " should fallback by setup failure";
      // should_fallback_ expects INIT state when subprocess finishes
      // in CompileTask::FinishSubProcess().
      state_ = INIT;
      if (subproc_ == nullptr)
        SetupSubProcess();
      RunSubProcess("fallback by setup failure");
      return;
    }
    // no fallback.
    AddErrorToResponse(TO_USER, "Failed to setup request", true);
    ProcessFinished("fail in setup");
    return;
  }
  TryProcessFileRequest();
}

#ifndef _WIN32
bool CompileTask::MakeWeakRelativeInArgv() {
  CHECK_EQ(SETUP, state_);
  DCHECK(compiler_info_state_.get() != nullptr);

  // Support only C/C++.
  if (compiler_info_state_.get()->info().type() != CompilerInfoType::Cxx) {
    return false;
  }

  orig_flag_dump_ = flag_dump_;
  // If cwd is in tmp directory, we can't know output path is
  // whether ./path/to/output or $TMP/path/to/output.
  // If latter, make the path relative would produce wrong output file.
  if (HasPrefixDir(req_->cwd(), "/tmp") || HasPrefixDir(req_->cwd(), "/var")) {
    LOG(WARNING) << trace_id_
                 << " GOMA_USE_RELATIVE_PATHS_IN_ARGV=true, but cwd may be "
                 << "under temp directory: " << req_->cwd() << ". "
                 << "Use original args.";
    orig_flag_dump_ = "";
    return false;
  }
  bool changed = false;
  std::ostringstream ss;
  const std::vector<std::string>& parsed_args =
      CompilerFlagsUtil::MakeWeakRelative(
          flags_->args(), req_->cwd(),
          ToCxxCompilerInfo(compiler_info_state_.get()->info()));
  for (size_t i = 0; i < parsed_args.size(); ++i) {
    if (req_->arg(i) != parsed_args[i]) {
      VLOG(1) << trace_id_ << " Arg[" << i << "]: " << req_->arg(i) << " => "
              << parsed_args[i];
      req_->set_arg(i, parsed_args[i]);
      changed = true;
    }
    ss << req_->arg(i) << " ";
  }
  flag_dump_ = ss.str();
  if (!changed) {
    VLOG(1) << trace_id_ << " GOMA_USE_RELATIVE_PATHS_IN_ARGV=true, "
            << "but no argv changed";
    orig_flag_dump_ = "";
  }
  return changed;
}
#endif

static void FixCommandSpec(const CompilerInfo& compiler_info,
                           const CompilerFlags& flags,
                           CommandSpec* command_spec) {
  // Overwrites name in command_spec if possible.
  // The name is used for selecting a compiler in goma backend.
  // The name set by gomacc could be wrong if a given compiler, especially it is
  // cc or c++, is a symlink to non-gcc compiler. Since compiler_info knows
  // more details on the compiler, we overwrite the name with the value comes
  // from it.
  //
  // You may think we can use realpath(3) in gomacc. We do not do that because
  // of two reasons:
  // 1. compiler_info is cached.
  // 2. we can know more detailed info there.
  if (compiler_info.HasName())
    command_spec->set_name(compiler_info.name());

  if (!command_spec->has_version())
    command_spec->set_version(compiler_info.version());
  if (!command_spec->has_target())
    command_spec->set_target(compiler_info.target());
  command_spec->set_binary_hash(compiler_info.request_compiler_hash());
  command_spec->set_size(compiler_info.local_compiler_stat().size);

  command_spec->clear_system_include_path();
  command_spec->clear_cxx_system_include_path();
  command_spec->clear_system_framework_path();
  command_spec->clear_system_library_path();

  // C++ program should only send C++ include paths, otherwise, include order
  // might be wrong. For C program, cxx_system_include_paths would be empty.
  // c.f. b/25675250
  if (compiler_info.type() == CompilerInfoType::Cxx) {
    DCHECK(flags.type() == CompilerFlagType::Gcc ||
           flags.type() == CompilerFlagType::Clexe ||
           flags.type() == CompilerFlagType::ClangTidy)
        << flags.type();

    bool is_cplusplus = static_cast<const CxxFlags&>(flags).is_cplusplus();
    const CxxCompilerInfo& cxxci = ToCxxCompilerInfo(compiler_info);
    if (!is_cplusplus) {
      for (const auto& path : cxxci.system_include_paths())
        command_spec->add_system_include_path(path);
    }
    for (const auto& path : cxxci.cxx_system_include_paths())
      command_spec->add_cxx_system_include_path(path);
    for (const auto& path : cxxci.system_framework_paths())
      command_spec->add_system_framework_path(path);
  }
}

static void FixSystemLibraryPath(const std::vector<std::string>& library_paths,
                                 CommandSpec* command_spec) {
  for (const auto& path : library_paths)
    command_spec->add_system_library_path(path);
}

void CompileTask::UpdateExpandedArgs() {
  for (const auto& expanded_arg : flags_->expanded_args()) {
    req_->add_expanded_arg(expanded_arg);
    stats_->exec_log.add_expanded_arg(expanded_arg);
  }
}

void CompileTask::SetExpectedOutputs(ExecReq* req,
                                     const CompilerFlags& flags) const {
  for (const auto& file : flags.output_files()) {
    req->add_expected_output_files(file);
  }
  for (const auto& dir : flags.output_dirs()) {
    req->add_expected_output_dirs(dir);
  }
}

void CompileTask::SetCompilerResources() {
  DCHECK(compiler_info_state_.get() != nullptr);
  const bool send_compiler_binary_as_input =
      service_->send_compiler_binary_as_input();
  const CompilerInfo& compiler_info = compiler_info_state_.get()->info();
  bool need_set_toolchain_specs = false;
  for (const auto& r : compiler_info.resource()) {
    const std::string& path = r.name;
    if (r.type == CompilerInfoData::EXECUTABLE_BINARY) {
      if (!send_compiler_binary_as_input) {
        continue;
      }
      need_set_toolchain_specs = true;
      if (!r.symlink_path.empty()) {
        // don't add symlink as input.
        continue;
      }
    }
    LOG(INFO) << trace_id_ << " input automatically added: " << path;
    req_->add_input()->set_filename(path);
  }

  // Set toolchain information (if enabled and found)
  if (need_set_toolchain_specs) {
    LOG(INFO) << trace_id_ << " input contains toolchain";
    SetToolchainSpecs(req_.get(), compiler_info);
  }
}

void CompileTask::SetToolchainSpecs(ExecReq* req,
                                    const CompilerInfo& compiler_info) const {
  req->set_toolchain_included(true);

  for (const auto& r : compiler_info.resource()) {
    if (r.type != CompilerInfoData::EXECUTABLE_BINARY) {
      continue;
    }

    // Also set toolchains
    ToolchainSpec* toolchain_spec = req->add_toolchain_specs();
    toolchain_spec->set_path(r.name);
    if (r.symlink_path.empty()) {
      // non symlink case
      toolchain_spec->set_hash(r.hash);
      toolchain_spec->set_size(r.file_stat.size);
      toolchain_spec->set_is_executable(r.is_executable);
      LOG(INFO) << trace_id_
                << " toolchain spec automatically added: " << r.name;
    } else {
      // symlink case
      toolchain_spec->set_symlink_path(r.symlink_path);
      // hash, file_stat, and is_executable are empty/default value.
      DCHECK(r.hash.empty());
      DCHECK(!r.file_stat.IsValid());
      DCHECK(!r.is_executable);
    }
  }
}

void CompileTask::ModifyRequestArgs() {
  DCHECK(compiler_info_state_.get() != nullptr);
  const CompilerInfo& compiler_info = compiler_info_state_.get()->info();
  bool modified_args = false;
  bool use_expanded_args = (req_->expanded_arg_size() > 0);
  for (const auto& flag : compiler_info.additional_flags()) {
    req_->add_arg(flag);
    if (use_expanded_args) {
      req_->add_expanded_arg(flag);
    }
    modified_args = true;
  }
  if (flags_->type() == CompilerFlagType::Clexe) {
    // If /Yu is specified, we add /Y- to tell the backend compiler not
    // to try using PCH. We add this here because we don't want to show
    // this flag in compiler_proxy's console.
    const std::string& using_pch =
        static_cast<const VCFlags&>(*flags_).using_pch();
    if (!using_pch.empty()) {
      req_->add_arg("/Y-");
      if (use_expanded_args) {
        req_->add_expanded_arg("/Y-");
      }
      modified_args = true;
    }
  }

  LOG_IF(INFO, modified_args) << trace_id_ << " modified args: "
                              << absl::StrJoin(req_->arg(), " ");
}

/* static */
bool CompileTask::IsRelocatableCompilerFlags(const CompilerFlags& flags) {
  bool has_fcoverage_mapping = false;
  absl::string_view fdebug_compilation_dir;
  absl::string_view fcoverage_compilation_dir;
  absl::string_view ffile_compilation_dir;

  switch (flags.type()) {
    case CompilerFlagType::Clexe: {
      const VCFlags& vc_flags = static_cast<const VCFlags&>(flags);
      has_fcoverage_mapping = vc_flags.has_fcoverage_mapping();
      fdebug_compilation_dir = vc_flags.fdebug_compilation_dir();
      fcoverage_compilation_dir = vc_flags.fcoverage_compilation_dir();
      ffile_compilation_dir = vc_flags.ffile_compilation_dir();
    } break;
    case CompilerFlagType::Gcc: {
      const GCCFlags& gcc_flags = static_cast<const GCCFlags&>(flags);
      has_fcoverage_mapping = gcc_flags.has_fcoverage_mapping();
      fdebug_compilation_dir = gcc_flags.fdebug_compilation_dir();
      fcoverage_compilation_dir = gcc_flags.fcoverage_compilation_dir();
      ffile_compilation_dir = gcc_flags.ffile_compilation_dir();
    } break;
    default:
      return false;
  }

  // Do not allow mismatching between -ffile-compilation-dir and other flags.
  // Although the flag used in the last win, since our flag parser do not
  // record the order of the flags, let me mark the case non-relocatable.
  // TODO: understand the -f*-compilation-dir order.
  if (!ffile_compilation_dir.empty()) {
    if (!fdebug_compilation_dir.empty() &&
        fdebug_compilation_dir != ffile_compilation_dir) {
      return false;
    }
    if (!fcoverage_compilation_dir.empty() &&
        fcoverage_compilation_dir != ffile_compilation_dir) {
      return false;
    }
  }

  // Handle -g
  // TODO: excute this only if -g* flag is set.
  // Currently gcc_flags does not have the way to tell -g exist or not,
  // let us assume it is always on until the feature is implemented.
  if (fdebug_compilation_dir != "." && ffile_compilation_dir != ".") {
    return false;
  }

  // -fcoverage-mapping
  if (has_fcoverage_mapping && fcoverage_compilation_dir != "." &&
      ffile_compilation_dir != ".") {
    return false;
  }

  return true;
}

void CompileTask::ModifyRequestCWDAndPWD() {
  if (!service_->enable_cwd_normalization()) {
    return;
  }

  const char fixed_cwd[] =
#ifdef _WIN32
      "C:\\this\\path\\is\\set\\by\\goma";
#else
      "/this/path/is/set/by/goma";
#endif

  if (req_->cwd() == fixed_cwd) {
    return;
  }

  if (!IsRelocatableCompilerFlags(*flags_)) {
    return;
  }
  for (const auto& input : req_->input()) {
    if (file::IsAbsolutePath(input.filename())) {
      return;
    }
  }

  for (auto& env : *req_->mutable_env()) {
    if (absl::StartsWith(env, "PWD=") && env != "PWD=/proc/self/cwd") {
      LOG(INFO) << trace_id_ << " pwd is changed from " << env << " to "
                << fixed_cwd;
      env = absl::StrCat("PWD=", fixed_cwd);
    }
  }

  LOG(INFO) << trace_id_ << " cwd is changed from " << req_->cwd() << " to "
            << fixed_cwd;
  req_->set_original_cwd(req_->cwd());
  req_->set_cwd(fixed_cwd);
}

void CompileTask::ModifyRequestEnvs() {
  std::vector<std::string> envs;
  for (const auto& env : req_->env()) {
    if (flags_->IsServerImportantEnv(env.c_str())) {
      envs.push_back(env);
    }
  }
  if (envs.size() == static_cast<size_t>(req_->env_size())) {
    return;
  }

  req_->clear_env();
  for (const auto& env : envs) {
    req_->add_env(env);
  }
  LOG(INFO) << trace_id_ << " modified env: " << envs;
}

void CompileTask::UpdateCommandSpec() {
  CHECK_EQ(SETUP, state_);
  command_spec_ = req_->command_spec();
  CommandSpec* command_spec = req_->mutable_command_spec();
  if (compiler_info_state_.get() == nullptr)
    return;
  const CompilerInfo& compiler_info = compiler_info_state_.get()->info();
  FixCommandSpec(compiler_info, *flags_, command_spec);
}

void CompileTask::UpdateRequesterInfo() {
  CHECK_EQ(SETUP, state_);
  FixRequesterInfo(compiler_info_state_.get()->info(),
                   req_->mutable_requester_info());
}

void CompileTask::FixRequesterInfo(const CompilerInfo& compiler_info,
                                   RequesterInfo* info) const {
#ifdef _WIN32
  info->set_path_style(RequesterInfo::WINDOWS_STYLE);
#else
  info->set_path_style(RequesterInfo::POSIX_STYLE);
#endif

  LOG_IF(WARNING, info->dimensions_size() != 0)
      << trace_id_
      << " somebody has already set dimensions:" << info->DebugString();
  for (const auto& d : compiler_info.dimensions()) {
    info->add_dimensions(d);
  }
  // If dimensions are set by compiler_info, we do not modify.
  if (info->dimensions_size() != 0) {
    return;
  }
  // Set default dimensions if nothing given.
#ifdef _WIN32
  info->add_dimensions("os:win");
#elif defined(__MACH__)
  info->add_dimensions("os:mac");
#elif defined(__linux__)
  info->add_dimensions("os:linux");
#else
#error "unsupported platform"
#endif
}

void CompileTask::MayFixSubprogramSpec(
    google::protobuf::RepeatedPtrField<SubprogramSpec>* subprogram_specs)
        const {
  std::set<std::string> used_subprogram_name;
  subprogram_specs->Clear();
  if (compiler_info_state_.get() == nullptr) {
    return;
  }
  for (const auto& info : compiler_info_state_.get()->info().subprograms()) {
    if (!used_subprogram_name.insert(info.abs_path).second) {
      LOG(ERROR) << trace_id_
                 << " The same subprogram is added twice.  Ignoring."
                 << " info.abs_path=" << info.abs_path
                 << " info.hash=" << info.hash;
      continue;
    }
    SubprogramSpec* subprog_spec = subprogram_specs->Add();

    subprog_spec->set_path(service_->use_user_specified_path_for_subprograms()
                               ? info.user_specified_path
                               : info.abs_path);
    subprog_spec->set_binary_hash(info.hash);
    subprog_spec->set_size(info.file_stat.size);
  }
}

void CompileTask::MayUpdateSubprogramSpec() {
  CHECK_EQ(SETUP, state_);
  MayFixSubprogramSpec(req_->mutable_subprogram());
  if (VLOG_IS_ON(3)) {
    for (const auto& subprog_spec : req_->subprogram()) {
      LOG(INFO) << trace_id_ << " update subprogram spec:"
                << " path=" << subprog_spec.path()
                << " hash=" << subprog_spec.binary_hash();
    }
  }
}

struct CompileTask::IncludeProcessorRequestParam {
  // input file_stat_cache will be moved to temporarily.
  std::unique_ptr<FileStatCache> file_stat_cache;
};

struct CompileTask::IncludeProcessorResponseParam {
  // result of IncludeProcessor.
  CompilerTypeSpecific::IncludeProcessorResult result;
  // return borrowed file_stat_cache to CompileTask.
  std::unique_ptr<FileStatCache> file_stat_cache;
  // true if include processor was canceled.
  bool canceled = false;
};

void CompileTask::StartIncludeProcessor() {
  VLOG(1) << trace_id_ << " StartIncludeProcessor";
  CHECK_EQ(SETUP, state_);

  // TODO: DepsCache should be able to support multiple input files,
  // however currently we have to pass |abs_input_filename|, so DeosCache
  // supports a compile task that has one input file.
  if (DepsCache::IsEnabled() &&
      compiler_type_specific_->SupportsDepsCache(*flags_) &&
      flags_->input_filenames().size() == 1U) {
    const std::string& input_filename = flags_->input_filenames()[0];
    const std::string& abs_input_filename =
        file::JoinPathRespectAbsolute(flags_->cwd(), input_filename);

    DepsCache* dc = DepsCache::instance();
    deps_identifier_ = DepsCache::MakeDepsIdentifier(
        compiler_info_state_.get()->info(), *flags_);
    if (deps_identifier_.has_value() &&
        dc->GetDependencies(deps_identifier_, flags_->cwd(), abs_input_filename,
                            &required_files_, input_file_stat_cache_.get())) {
      LOG(INFO) << trace_id_ << " use deps cache. required_files="
                << required_files_.size();
      depscache_used_ = true;
      UpdateRequiredFilesDone(true);
      return;
    }
  }

  auto request_param = absl::make_unique<IncludeProcessorRequestParam>();

  input_file_stat_cache_->ReleaseOwner();
  request_param->file_stat_cache = std::move(input_file_stat_cache_);

  OneshotClosure* closure = NewCallback(this, &CompileTask::RunIncludeProcessor,
                                        std::move(request_param));
  service_->wm()->RunClosureInPool(
      FROM_HERE, service_->include_processor_pool(),
      closure,
      WorkerThread::PRIORITY_LOW);
}

void CompileTask::RunIncludeProcessor(
    std::unique_ptr<IncludeProcessorRequestParam> request_param) {
  VLOG(1) << trace_id_ << " RunIncludeProcessor";
  DCHECK(compiler_info_state_.get() != nullptr);

  // Pass ownership temporary to IncludeProcessor thread.
  request_param->file_stat_cache->AcquireOwner();

  const absl::Duration include_processor_wait_time =
      include_wait_timer_.GetDuration();
  stats_->exec_log.set_include_processor_wait_time(
      DurationToIntMs(include_processor_wait_time));
  stats_->include_processor_wait_time = include_processor_wait_time;

  if (canceled_ || abort_) {
    LOG(INFO) << trace_id_
              << " won't run include processor because result won't be used."
              << " canceled=" << canceled_
              << " abort=" << abort_;
    auto response_param = absl::make_unique<IncludeProcessorResponseParam>();
    response_param->canceled = true;
    response_param->file_stat_cache = std::move(request_param->file_stat_cache);
    response_param->file_stat_cache->ReleaseOwner();
    service_->wm()->RunClosureInThread(
        FROM_HERE, thread_id_,
        NewCallback(this, &CompileTask::RunIncludeProcessorDone,
                    std::move(response_param)),
        WorkerThread::PRIORITY_LOW);
    return;
  }

  LOG_IF(WARNING, stats_->include_processor_wait_time > absl::Seconds(1))
      << trace_id_ << " SLOW start IncludeProcessor"
      << " in " << stats_->include_processor_wait_time;

  SimpleTimer include_timer(SimpleTimer::START);
  CompilerTypeSpecific::IncludeProcessorResult result =
      compiler_type_specific_->RunIncludeProcessor(
          trace_id_, *flags_, compiler_info_state_.get()->info(),
          req_->command_spec(), request_param->file_stat_cache.get());
  const absl::Duration include_processor_run_time = include_timer.GetDuration();
  stats_->exec_log.set_include_processor_run_time(
      DurationToIntMs(include_processor_run_time));
  stats_->include_processor_run_time = include_processor_run_time;

  auto response_param = absl::make_unique<IncludeProcessorResponseParam>();
  response_param->result = std::move(result);
  response_param->file_stat_cache = std::move(request_param->file_stat_cache);
  response_param->file_stat_cache->ReleaseOwner();

  service_->wm()->RunClosureInThread(
      FROM_HERE, thread_id_,
      NewCallback(this, &CompileTask::RunIncludeProcessorDone,
                  std::move(response_param)),
      WorkerThread::PRIORITY_LOW);
}

void CompileTask::RunIncludeProcessorDone(
    std::unique_ptr<IncludeProcessorResponseParam> response_param) {
  VLOG(1) << trace_id_ << " RunIncludeProcessorDone";
  DCHECK(BelongsToCurrentThread());
  DCHECK(response_param->file_stat_cache.get() != nullptr);

  input_file_stat_cache_ = std::move(response_param->file_stat_cache);
  input_file_stat_cache_->AcquireOwner();
  if (response_param->canceled) {
    UpdateRequiredFilesDone(false);
    return;
  }
  required_files_ = std::move(response_param->result.required_files);

  if (!response_param->result.system_library_paths.empty()) {
    system_library_paths_ =
        std::move(response_param->result.system_library_paths);
    FixSystemLibraryPath(system_library_paths_, req_->mutable_command_spec());
  }

  if (response_param->result.total_files) {
    stats_->exec_log.set_include_preprocess_total_files(
        *response_param->result.total_files);
  }
  if (response_param->result.skipped_files) {
    stats_->exec_log.set_include_preprocess_skipped_files(
        *response_param->result.skipped_files);
  }

  if (!response_param->result.ok) {
    LOG(WARNING) << trace_id_ << " include processor failed"
                 << " error=" << response_param->result.error_reason
                 << " flags=" << flags_->DebugString();
    if (response_param->result.error_to_user) {
      AddErrorToResponse(TO_USER, response_param->result.error_reason, true);
    }
  }

  // When deps_identifier_.has_value() is true, the condition to use DepsCache
  // should be satisfied. However, several checks are done for the safe.
  if (DepsCache::IsEnabled() &&
      compiler_type_specific_->SupportsDepsCache(*flags_) &&
      response_param->result.ok && deps_identifier_.has_value() &&
      flags_->input_filenames().size() == 1U) {
    const std::string& input_filename = flags_->input_filenames()[0];
    const std::string& abs_input_filename =
        file::JoinPathRespectAbsolute(flags_->cwd(), input_filename);

    DepsCache* dc = DepsCache::instance();
    if (!dc->SetDependencies(deps_identifier_, flags_->cwd(),
                             abs_input_filename, required_files_,
                             input_file_stat_cache_.get())) {
      LOG(INFO) << trace_id_ << " failed to save dependencies.";
    }
  }

  UpdateRequiredFilesDone(response_param->result.ok);
}

// ----------------------------------------------------------------
// state_: FILE_REQ.
void CompileTask::SetInputFileCallback() {
  CHECK(BelongsToCurrentThread());
  CHECK_EQ(FILE_REQ, state_);
  CHECK(!input_file_callback_);
  input_file_callback_ = NewCallback(
      this, &CompileTask::ProcessFileRequestDone);
  num_input_file_task_ = 0;
  input_file_success_ = true;
}

void CompileTask::StartInputFileTask() {
  CHECK(BelongsToCurrentThread());
  CHECK_EQ(FILE_REQ, state_);
  ++num_input_file_task_;
}

void CompileTask::InputFileTaskFinished(InputFileTask* input_file_task) {
  CHECK(BelongsToCurrentThread());
  CHECK_EQ(FILE_REQ, state_);

  if (abort_) {
    VLOG(1) << trace_id_ << "aborted ";
    input_file_success_ = false;
    input_file_task->Done(this);
    return;
  }

  const std::string& filename = input_file_task->filename();
  const std::string& hash_key = input_file_task->hash_key();
  const ssize_t file_size = input_file_task->file_size();
  const absl::optional<absl::Time>& mtime = input_file_task->mtime();
  VLOG(1) << trace_id_ << " input done:" << filename;
  if (mtime.has_value() &&
      *mtime > absl::FromTimeT(stats_->exec_log.latest_input_mtime())) {
    stats_->exec_log.set_latest_input_filename(filename);
    stats_->exec_log.set_latest_input_mtime(absl::ToTimeT(*mtime));
  }
  if (!input_file_task->success()) {
    AddErrorToResponse(TO_LOG, "Create file blob failed for:" + filename, true);
    input_file_success_ = false;
    input_file_task->Done(this);
    return;
  }
  DCHECK(!hash_key.empty()) << filename;
  stats_->exec_log.add_input_file_time(
      DurationToIntMs(input_file_task->timer().GetDuration()));
  stats_->exec_log.add_input_file_size(file_size);
  if (!input_file_task->UpdateInputInTask(this)) {
    LOG(ERROR) << trace_id_ << " bad input data "
               << filename;
    input_file_success_ = false;
  }
  const HttpClient::Status& http_status =
      input_file_task->http_status();
  stats_->input_file_rpc_size += http_status.req_size;
  stats_->input_file_rpc_raw_size += http_status.raw_req_size;
  input_file_task->Done(this);
}

void CompileTask::MaybeRunInputFileCallback(bool task_finished) {
  CHECK(BelongsToCurrentThread());
  CHECK_EQ(FILE_REQ, state_);
  OneshotClosure* closure = nullptr;
  if (task_finished) {
    --num_input_file_task_;
    VLOG(1) << trace_id_ << " input remain=" << num_input_file_task_;
    if (num_input_file_task_ > 0)
      return;
  }
  CHECK_EQ(0, num_input_file_task_);
  if (input_file_callback_) {
    closure = input_file_callback_;
    input_file_callback_ = nullptr;
  }
  if (closure)
    closure->Run();
}

// ----------------------------------------------------------------
// state_: CALL_EXEC.

void CompileTask::CheckCommandSpec() {
  CHECK_EQ(CALL_EXEC, state_);
  if (!resp_->result().has_command_spec()) {
    return;
  }

  // Checks all mismatches first, then decide behavior later.
  bool is_name_mismatch = false;
  bool is_target_mismatch = false;
  bool is_binary_hash_mismatch = false;
  bool is_version_mismatch = false;
  bool is_subprograms_mismatch = false;
  const CommandSpec& req_command_spec = req_->command_spec();
  const CommandSpec& resp_command_spec = resp_->result().command_spec();
  const std::string message_on_mismatch(
      "local:" + CreateCommandVersionString(req_command_spec) +
      " but remote:" + CreateCommandVersionString(resp_command_spec));
  if (req_command_spec.name() != resp_command_spec.name()) {
    is_name_mismatch = true;
    std::ostringstream ss;
    ss << trace_id_ << " compiler name mismatch:"
       << " local:" << req_command_spec.name()
       << " remote:" << resp_command_spec.name();
    AddErrorToResponse(TO_LOG, ss.str(), false);
    stats_->exec_log.set_exec_command_name_mismatch(message_on_mismatch);
  }
  if (req_command_spec.target() != resp_command_spec.target()) {
    is_target_mismatch = true;
    std::ostringstream ss;
    ss << trace_id_ << " compiler target mismatch:"
       << " local:" << req_command_spec.target()
       << " remote:" << resp_command_spec.target();
    AddErrorToResponse(TO_LOG, ss.str(), false);
    stats_->exec_log.set_exec_command_target_mismatch(message_on_mismatch);
  }
  if (req_command_spec.binary_hash() != resp_command_spec.binary_hash()) {
    is_binary_hash_mismatch = true;
    LOG(WARNING) << trace_id_ << " compiler binary hash mismatch:"
                 << " local:" << req_command_spec.binary_hash()
                 << " remote:" << resp_command_spec.binary_hash();
    stats_->exec_log.set_exec_command_binary_hash_mismatch(message_on_mismatch);
  }
  if (req_command_spec.version() != resp_command_spec.version()) {
    is_version_mismatch = true;
    LOG(WARNING) << trace_id_ << " compiler version mismatch:"
                 << " local:" << req_command_spec.version()
                 << " remote:" << resp_command_spec.version();
    stats_->exec_log.set_exec_command_version_mismatch(message_on_mismatch);
  }
  if (!IsSameSubprograms(*req_, *resp_)) {
    is_subprograms_mismatch = true;
    std::ostringstream local_subprograms;
    DumpSubprograms(req_->subprogram(), &local_subprograms);
    std::ostringstream remote_subprograms;
    DumpSubprograms(resp_->result().subprogram(), &remote_subprograms);
    LOG(WARNING) << trace_id_ << " compiler subprograms mismatch:"
                 << " local:" << local_subprograms.str()
                 << " remote:" << remote_subprograms.str();
    std::ostringstream ss;
    ss << "local:" << CreateCommandVersionString(req_command_spec)
       << " subprogram:" << local_subprograms.str()
       << " but remote:" << CreateCommandVersionString(resp_command_spec)
       << " subprogram:" << remote_subprograms.str();
    stats_->exec_log.set_exec_command_subprograms_mismatch(ss.str());
  }

  if (service_->hermetic()) {
    bool mismatch = false;
    // Check if remote used the same command spec.
    if (is_name_mismatch) {
      mismatch = true;
      AddErrorToResponse(TO_USER, "compiler name mismatch", true);
    }
    if (is_target_mismatch) {
      mismatch = true;
      AddErrorToResponse(TO_USER, "compiler target mismatch", true);
    }
    if (is_binary_hash_mismatch) {
      mismatch = true;
      AddErrorToResponse(TO_USER, "compiler binary hash mismatch", true);
    }
    if (is_version_mismatch) {
      AddErrorToResponse(TO_USER, "compiler version mismatch", true);
      mismatch = true;
    }
    if (is_subprograms_mismatch) {
      AddErrorToResponse(TO_USER, "subprograms mismatch", true);
      mismatch = true;
    }
    if (mismatch) {
      if (service_->DisableCompilerInfo(compiler_info_state_.get(),
                                        "hermetic mismatch")) {
        AddErrorToResponse(
            TO_USER,
            req_->command_spec().local_compiler_path() + " is disabled.",
            true);
      }
      want_fallback_ &= service_->hermetic_fallback();
      if (want_fallback_ != requester_env_.fallback()) {
        LOG(INFO) << trace_id_ << " hermetic mismatch: fallback changed from "
                  << requester_env_.fallback()
                  << " to " << want_fallback_;
      }
    }
    return;
  }

  if (is_name_mismatch || is_target_mismatch) {
    AddErrorToResponse(TO_USER, "compiler name or target mismatch", true);
    if (service_->DisableCompilerInfo(compiler_info_state_.get(),
                                      "compiler name or target mismatch")) {
      AddErrorToResponse(
          TO_USER,
          req_->command_spec().local_compiler_path() + " is disabled.",
          true);
    }
    return;
  }
  // TODO: drop command_check_level support in the future.
  //                    GOMA_HERMETIC should be recommended.
  if (is_binary_hash_mismatch) {
    std::string error_message;
    bool set_error = false;
    if (service_->RecordCommandSpecBinaryHashMismatch(
            stats_->exec_log.exec_command_binary_hash_mismatch())) {
      error_message = "compiler binary hash mismatch: " +
                      stats_->exec_log.exec_command_binary_hash_mismatch();
    }
    if (service_->command_check_level() == "checksum") {
      set_error = true;
    }
    if (!requester_env_.verify_command().empty()) {
      if (requester_env_.verify_command() == "checksum" ||
          requester_env_.verify_command() == "all") {
        AddErrorToResponse(TO_LOG, "", true);
        resp_->mutable_result()->set_stderr_buffer(
            "compiler binary hash mismatch: " +
            stats_->exec_log.exec_command_binary_hash_mismatch() + "\n" +
            resp_->mutable_result()->stderr_buffer());
      }
      // ignore when other verify command mode.
    } else if (!error_message.empty()) {
      error_message =
          (set_error ? "Error: " : "Warning: ") + error_message;
      AddErrorToResponse(TO_USER, error_message, set_error);
    }
  }
  if (is_version_mismatch) {
    std::string error_message;
    bool set_error = false;
    if (service_->RecordCommandSpecVersionMismatch(
            stats_->exec_log.exec_command_version_mismatch())) {
      error_message = "compiler version mismatch: " +
                      stats_->exec_log.exec_command_version_mismatch();
    }
    if (service_->command_check_level() == "version") {
      set_error = true;
    }
    if (!requester_env_.verify_command().empty()) {
      if (requester_env_.verify_command() == "version" ||
          requester_env_.verify_command() == "all") {
        AddErrorToResponse(TO_LOG, "", true);
        resp_->mutable_result()->set_stderr_buffer(
            "compiler version mismatch: " +
            stats_->exec_log.exec_command_version_mismatch() + "\n" +
            resp_->mutable_result()->stderr_buffer());
      }
      // ignore when other verify command mode.
    } else if (!error_message.empty()) {
      error_message =
          (set_error ? "Error: " : "Warning: ") + error_message;
      AddErrorToResponse(TO_USER, error_message, set_error);
    }
  }
  if (is_subprograms_mismatch) {
    std::ostringstream error_message;
    bool set_error = false;

    absl::flat_hash_set<std::string> remote_hashes;
    for (const auto& subprog : resp_->result().subprogram()) {
      remote_hashes.insert(subprog.binary_hash());
    }
    for (const auto& subprog : req_->subprogram()) {
      if (remote_hashes.contains(subprog.binary_hash())) {
        continue;
      }
      std::ostringstream ss;
      ss << subprog.path() << " " << subprog.binary_hash();
      if (service_->RecordSubprogramMismatch(ss.str())) {
        if (!error_message.str().empty()) {
          error_message << std::endl;
        }
        error_message << "subprogram mismatch: "
                      << ss.str();
      }
    }

    if (service_->command_check_level() == "checksum") {
      set_error = true;
    }
    if (!requester_env_.verify_command().empty()) {
      if (requester_env_.verify_command() == "checksum" ||
          requester_env_.verify_command() == "all") {
        AddErrorToResponse(TO_LOG, "", true);
        resp_->mutable_result()->set_stderr_buffer(
            error_message.str() + "\n" +
            resp_->mutable_result()->stderr_buffer());
      }
      // ignore when other verify command mode.
    } else if (!error_message.str().empty()) {
      AddErrorToResponse(
          TO_USER,
          (set_error ? "Error: " : "Warning: ") + error_message.str(),
          set_error);
    }
  }
}

void CompileTask::CheckNoMatchingCommandSpec(const std::string& retry_reason) {
  CHECK_EQ(CALL_EXEC, state_);

  // If ExecResult does not have CommandSpec, goma backend did not try
  // to find the compiler. No need to check mismatches.
  if (!resp_->result().has_command_spec()) {
    return;
  }

  bool is_compiler_missing = false;
  bool is_subprogram_missing = false;
  // If ExecResult has incomplete CommandSpec, it means that goma backend
  // tried to select a matching compiler but failed.
  if (!resp_->result().command_spec().has_binary_hash()) {
    is_compiler_missing = true;
  }
  if (!IsSameSubprograms(*req_, *resp_)) {
    is_subprogram_missing = true;
  }
  // Nothing is missing.
  if (!is_compiler_missing && !is_subprogram_missing) {
    return;
  }

  std::ostringstream local_subprograms;
  std::ostringstream remote_subprograms;
  DumpSubprograms(req_->subprogram(), &local_subprograms);
  DumpSubprograms(resp_->result().subprogram(), &remote_subprograms);

  std::ostringstream what_missing;
  if (is_compiler_missing) {
    LOG(WARNING) << trace_id_
                 << " compiler not found:"
                 << " local: "
                 << CreateCommandVersionString(req_->command_spec())
                 << " remote: none";
    what_missing << "compiler("
                 << CreateCommandVersionString(req_->command_spec())
                 << ")";
  }
  if (is_subprogram_missing) {
    LOG(WARNING) << trace_id_
                 << " subprogram not found:"
                 << " local: " << local_subprograms.str()
                 << " remote: " << remote_subprograms.str();
    if (!what_missing.str().empty())
      what_missing << "/";
    what_missing << "subprograms("
                 << local_subprograms.str()
                 << ")";
  }

  std::ostringstream ss;
  ss << "local: " << CreateCommandVersionString(req_->command_spec())
     << " subprogram: " << local_subprograms.str()
     << " but remote: ";
  if (is_compiler_missing) {
    ss << "none";
  } else {
    ss << CreateCommandVersionString(resp_->result().command_spec());
  }
  ss << " subprogram: " << remote_subprograms.str();
  stats_->exec_log.set_exec_command_not_found(ss.str());

  if (service_->hermetic() && !what_missing.str().empty()) {
    std::ostringstream msg;
    msg << "No matching " << what_missing.str() << " found in server";
    AddErrorToResponse(TO_USER, msg.str(), true);
    if (is_compiler_missing &&
        service_->DisableCompilerInfo(compiler_info_state_.get(),
                                      "no matching compiler found in server")) {
        AddErrorToResponse(
            TO_USER, req_->command_spec().local_compiler_path() +
            " is disabled.",
            true);
    }

    want_fallback_ &= service_->hermetic_fallback();
    if (want_fallback_ != requester_env_.fallback()) {
      LOG(INFO) << trace_id_
                << " hermetic miss "
                << what_missing.str()
                << ": fallback changed from "
                << requester_env_.fallback()
                << " to " << want_fallback_;
    }
  }
}

void CompileTask::StoreEmbeddedUploadInformationIfNeeded() {
  // We save embedded upload information only if missing input size is 0.
  // Let's consider the situation we're using cluster A and cluster B.
  // When we send a compile request to cluster A, cluster A might report
  // there are missing inputs. Then we retry to send a compile request.
  // However, we might send it to another cluster B. Then cluster B might
  // report missing input error again.
  // So, we would like to save the embedded upload information only if
  // missing input error did not happen.
  // TODO: This can reduce the number of input file missing, it would
  // still exist. After uploading a file to cluster B was succeeded, we might
  // send another compile request to cluster A. When cluster A does not have
  // the file cache, missing inputs error will occur.

  if (resp_->missing_input_size() > 0)
    return;

  // TODO: What time should we use here?
  const absl::Time upload_timestamp_ms = absl::Now();

  for (const auto& input : req_->input()) {
    // If content does not exist, it's not embedded upload.
    if (!input.has_content())
      continue;
    const std::string& abs_filename = file::JoinPathRespectAbsolute(
        flags_->cwd(), input.filename());
    bool new_cache_key = service_->file_hash_cache()->StoreFileCacheKey(
        abs_filename, input.hash_key(), upload_timestamp_ms,
        input_file_stat_cache_->Get(abs_filename));
    VLOG(1) << trace_id_
            << " store file cache key for embedded upload: "
            << abs_filename
            << " : is new cache key? = " << new_cache_key;
  }
}

// ----------------------------------------------------------------
// state_: FILE_RESP.
void CompileTask::SetOutputFileCallback() {
  CHECK(BelongsToCurrentThread());
  CHECK_EQ(FILE_RESP, state_);
  CHECK(!output_file_callback_);
  output_file_callback_ = NewCallback(
      this, &CompileTask::ProcessFileResponseDone);
  num_output_file_task_ = 0;
  output_file_success_ = true;
}

void CompileTask::CheckOutputFilename(const std::string& filename) {
  CHECK_EQ(FILE_RESP, state_);
  if (filename[0] == '/') {
    if (HasPrefixDir(filename, service_->tmp_dir()) ||
        HasPrefixDir(filename, "/var")) {
      VLOG(1) << trace_id_ << " Output to temp directory:" << filename;
    } else if (service_->use_relative_paths_in_argv()) {
      // If FLAGS_USE_RELATIVE_PATHS_IN_ARGV is false, output path may be
      // absolute path specified by -o or so.

      Json::Value json;
      DumpToJson(true, &json);
      LOG(ERROR) << trace_id_ << " " << json;
      LOG(FATAL) << trace_id_ << " Absolute output filename:" << filename;
    }
  }
}

void CompileTask::StartOutputFileTask() {
  CHECK(BelongsToCurrentThread());
  CHECK_EQ(FILE_RESP, state_);
  ++num_output_file_task_;
}

void CompileTask::OutputFileTaskFinished(
    std::unique_ptr<OutputFileTask> output_file_task) {
  CHECK(BelongsToCurrentThread());
  CHECK_EQ(FILE_RESP, state_);

  DCHECK_EQ(this, output_file_task->task());
  const ExecResult_Output& output = output_file_task->output();
  const std::string& filename = output.filename();

  if (abort_) {
    output_file_success_ = false;
    return;
  }
  if (!output_file_task->success()) {
    AddErrorToResponse(
        TO_LOG,
        "Failed to download file blob:" + filename + " (" +
            (cache_hit() ? "cached" : "no-cached") +
            "): http err:" + output_file_task->http_status().err_message,
        true);
    output_file_success_ = false;

    // If it fails to write file, goma has ExecResult in cache but might
    // lost output file.  It would be better to retry with STORE_ONLY
    // to recreate output file and store it in cache.
    ExecReq::CachePolicy cache_policy = req_->cache_policy();
    if (cache_policy == ExecReq::LOOKUP_AND_STORE ||
        cache_policy == ExecReq::LOOKUP_AND_STORE_SUCCESS) {
      LOG(WARNING) << trace_id_
                   << " will retry with STORE_ONLY";
      req_->set_cache_policy(ExecReq::STORE_ONLY);
    }
    return;
  }
  absl::Duration output_file_time = output_file_task->timer().GetDuration();
  LOG_IF(WARNING, output_file_time > absl::Minutes(1))
      << trace_id_ << " SLOW output file:"
      << " filename=" << filename
      << " http_rpc=" << output_file_task->http_status().DebugString()
      << " num_rpc=" << output_file_task->num_rpc()
      << " in_memory=" << output_file_task->IsInMemory() << " in "
      << output_file_time;
  LOG_IF(WARNING,
         output.blob().blob_type() != FileBlob::FILE &&
         output.blob().blob_type() != FileBlob::FILE_META)
      << "Invalid blob type: " << output.blob().blob_type();
  stats_->AddStatsFromOutputFileTask(*output_file_task);
}

void CompileTask::MaybeRunOutputFileCallback(int index, bool task_finished) {
  CHECK(BelongsToCurrentThread());
  CHECK_EQ(FILE_RESP, state_);
  OneshotClosure* closure = nullptr;
  if (task_finished) {
    DCHECK_NE(-1, index);
    // Once output.blob has been written on disk, we don't need it
    // any more.
    resp_->mutable_result()->mutable_output(index)->clear_blob();
    --num_output_file_task_;
    if (num_output_file_task_ > 0)
      return;
  } else {
    CHECK_EQ(-1, index);
  }
  CHECK_EQ(0, num_output_file_task_);
  if (output_file_callback_) {
    closure = output_file_callback_;
    output_file_callback_ = nullptr;
  }
  if (closure)
    closure->Run();
}

bool CompileTask::VerifyOutput(const std::string& local_output_path,
                               const std::string& goma_output_path) {
  CHECK_EQ(FILE_RESP, state_);
  LOG(INFO) << trace_id_ << " Verify Output: "
            << " local:" << local_output_path << " goma:" << goma_output_path;
  std::ostringstream error_message;
  static const int kSize = 1024;
  char local_buf[kSize];
  char goma_buf[kSize];
  ScopedFd local_fd(ScopedFd::OpenForRead(local_output_path));
  if (!local_fd.valid()) {
    error_message << "Not found: local file:" << local_output_path;
    AddErrorToResponse(TO_USER, error_message.str(), true);
    return false;
  }
  ScopedFd goma_fd(ScopedFd::OpenForRead(goma_output_path));
  if (!goma_fd.valid()) {
    error_message << "Not found: goma file:" << goma_output_path;
    AddErrorToResponse(TO_USER, error_message.str(), true);
    return false;
  }
  int local_len;
  int goma_len;
  for (size_t len = 0; ; len += local_len) {
    local_len = local_fd.Read(local_buf, kSize);
    if (local_len < 0) {
      error_message << "read error local:" << local_output_path
                    << " @" << len << " " << GetLastErrorMessage();
      AddErrorToResponse(TO_USER, error_message.str(), true);
      return false;
    }
    goma_len = goma_fd.Read(goma_buf, kSize);
    if (goma_len < 0) {
      error_message << "read error goma:" << goma_output_path
                    << " @" << len << " " << GetLastErrorMessage();
      AddErrorToResponse(TO_USER, error_message.str(), true);
      return false;
    }
    if (local_len != goma_len) {
      error_message << "read len: " << local_len << "!=" << goma_len
                    << " " << local_output_path << " @" << len;
      AddErrorToResponse(TO_USER, error_message.str(), true);
      return false;
    }
    if (local_len == 0) {
      LOG(INFO) << trace_id_
                << " Verify OK: " << local_output_path
                << " size=" << len;
      return true;
    }
    if (memcmp(local_buf, goma_buf, local_len) != 0) {
      error_message << "output mismatch: "
                    << " local:" << local_output_path
                    << " goma:" << goma_output_path
                    << " @[" << len << "," <<  local_len << ")";
      AddErrorToResponse(TO_USER, error_message.str(), true);
      return false;
    }
    VLOG(2) << trace_id_ << " len:" << len << "+" << local_len;
  }
  return true;
}

void CompileTask::ClearOutputFile() {
  for (auto& info : output_file_infos_) {
    if (!info.content.empty()) {
      LOG(INFO) << trace_id_ << " clear output, but content is not empty";
      service_->ReleaseOutputBuffer(info.size, &info.content);
      continue;
    }
    // Remove if we wrote tmp file for the output.
    // Don't remove filename, which is the actual output filename,
    // and local run might have output to the file.
    const std::string& filename = info.filename;
    const std::string& tmp_filename = info.tmp_filename;
    if (!tmp_filename.empty() && tmp_filename != filename) {
      remove(tmp_filename.c_str());
    }
  }
  output_file_infos_.clear();
}

// ----------------------------------------------------------------
// local run finished.
void CompileTask::SetLocalOutputFileCallback() {
  CHECK(BelongsToCurrentThread());
  CHECK(!local_output_file_callback_);
  local_output_file_callback_ = NewCallback(
      this, &CompileTask::ProcessLocalFileOutputDone);
  num_local_output_file_task_ = 0;
}

void CompileTask::StartLocalOutputFileTask() {
  CHECK(BelongsToCurrentThread());
  ++num_local_output_file_task_;
}

void CompileTask::LocalOutputFileTaskFinished(
    std::unique_ptr<LocalOutputFileTask> local_output_file_task) {
  CHECK(BelongsToCurrentThread());

  DCHECK_EQ(this, local_output_file_task->task());
  const std::string& filename = local_output_file_task->filename();
  if (!local_output_file_task->success()) {
    LOG(WARNING) << trace_id_
                 << " Create file blob failed for local output:" << filename;
    return;
  }
  const absl::Duration local_output_file_task_duration =
      local_output_file_task->timer().GetDuration();
  stats_->exec_log.add_local_output_file_time(
      DurationToIntMs(local_output_file_task_duration));
  stats_->total_local_output_file_time += local_output_file_task_duration;

  const FileStat& file_stat = local_output_file_task->file_stat();
  stats_->exec_log.add_local_output_file_size(file_stat.size);
}

void CompileTask::MaybeRunLocalOutputFileCallback(bool task_finished) {
  CHECK(BelongsToCurrentThread());
  OneshotClosure* closure = nullptr;
  if (task_finished) {
    --num_local_output_file_task_;
    if (num_local_output_file_task_ > 0)
      return;
  }
  CHECK_EQ(0, num_local_output_file_task_);
  if (local_output_file_callback_) {
    closure = local_output_file_callback_;
    local_output_file_callback_ = nullptr;
  }
  if (closure)
    closure->Run();
}

// ----------------------------------------------------------------
// state_: FINISHED/LOCAL_FINISHED or abort_
void CompileTask::UpdateStats() {
  CHECK(state_ >= FINISHED || abort_);

  resp_->set_compiler_proxy_time(DurationToIntMs(handler_timer_.GetDuration()));

  stats_->StoreStatsInExecResp(resp_.get());

  // TODO: similar logic found in CompileService::CompileTaskDone, so
  // it would be better to be merged.  Note that ExecResp are not available
  // in CompileService::CompileTaskDone.
  switch (state_) {
    case FINISHED:
      resp_->set_compiler_proxy_goma_finished(true);
      if (cache_hit())
        resp_->set_compiler_proxy_goma_cache_hit(true);
      break;
    case LOCAL_FINISHED:
      resp_->set_compiler_proxy_local_finished(true);
      break;
    default:
      resp_->set_compiler_proxy_goma_aborted(true);
      break;
  }
  if (local_run_)
    resp_->set_compiler_proxy_local_run(true);
  if (local_killed_)
    resp_->set_compiler_proxy_local_killed(true);
}

void CompileTask::SaveInfoFromInputOutput() {
  DCHECK(BelongsToCurrentThread());
  CHECK(state_ >= FINISHED || abort_);
  CHECK(req_.get());
  CHECK(resp_.get());
  CHECK(!exec_resp_.get());

  if (failed() || fail_fallback_) {
    if (!fail_fallback_) {
      // if fail fallback, we already stored remote outputs in stdout_ and
      // stderr_, and resp_ becomes local process output.
      stdout_ = resp_->result().stdout_buffer();
      stderr_ = resp_->result().stderr_buffer();
    }
  }
  // arg, env and expanded_arg are used for dumping ExecReq.
  // We should keep what we actually used instead of what came from gomacc.
  *stats_->exec_log.mutable_arg() = std::move(*req_->mutable_arg());
  *stats_->exec_log.mutable_env() = std::move(*req_->mutable_env());
  *stats_->exec_log.mutable_expanded_arg() =
      std::move(*req_->mutable_expanded_arg());
  req_.reset();
  resp_.reset();
  flags_.reset();
  input_file_stat_cache_.reset();
  output_file_stat_cache_.reset();
}

// ----------------------------------------------------------------
// subprocess handling.
void CompileTask::SetupSubProcess() {
  VLOG(1) << trace_id_ << " SetupSubProcess "
          << SubProcessReq::Weight_Name(subproc_weight_);
  CHECK(BelongsToCurrentThread());
  CHECK(subproc_ == nullptr) << trace_id_ << " " << StateName(state_)
                             << " pid=" << subproc_->started().pid()
                             << stats_->exec_log.local_run_reason();
  CHECK(!req_->command_spec().local_compiler_path().empty())
      << req_->DebugString();
  if (delayed_setup_subproc_ != nullptr) {
    delayed_setup_subproc_->Cancel();
    delayed_setup_subproc_ = nullptr;
  }

  std::vector<const char*> argv;
  argv.push_back(req_->command_spec().local_compiler_path().c_str());
  for (int i = 1; i < stats_->exec_log.arg_size(); ++i) {
    argv.push_back(stats_->exec_log.arg(i).c_str());
  }
  argv.push_back(nullptr);

  subproc_ = new SubProcessTask(
      trace_id_,
      req_->command_spec().local_compiler_path().c_str(),
      const_cast<char**>(&argv[0]));
  SubProcessReq* req = subproc_->mutable_req();
  req->set_cwd(flags_->cwd());
  if (requester_env_.has_umask()) {
    req->set_umask(requester_env_.umask());
  }
  if (flags_->type() == CompilerFlagType::Gcc) {
    const GCCFlags& gcc_flag = static_cast<const GCCFlags&>(*flags_);
    if (gcc_flag.is_stdin_input()) {
      CHECK_GE(req_->input_size(), 1) << req_->DebugString();
      req->set_stdin_filename(req_->input(0).filename());
    }
  } else if (flags_->type() == CompilerFlagType::Clexe) {
    // TODO: handle input is stdin case for VC++?
  }
  {
    std::ostringstream filenamebuf;
    filenamebuf << "gomacc." << id_ << ".out";
    subproc_stdout_ = file::JoinPath(service_->tmp_dir(), filenamebuf.str());
    req->set_stdout_filename(subproc_stdout_);
  }
  {
    std::ostringstream filenamebuf;
    filenamebuf << "gomacc." << id_ << ".err";
    subproc_stderr_ = file::JoinPath(service_->tmp_dir(), filenamebuf.str());
    req->set_stderr_filename(subproc_stderr_);
  }
  for (const auto& env : stats_->exec_log.env()) {
    req->add_env(env);
  }
  if (local_path_.empty()) {
    LOG(WARNING) << trace_id_ << " Empty PATH: " << req_->DebugString();
  } else {
    req->add_env("PATH=" + local_path_);
  }
#ifdef _WIN32
  req->add_env("TMP=" + service_->tmp_dir());
  req->add_env("TEMP=" + service_->tmp_dir());
  if (pathext_.empty()) {
    LOG(WARNING) << trace_id_ << " Empty PATHEXT: " << req_->DebugString();
  } else {
    req->add_env("PATHEXT=" + pathext_);
  }
#endif

  req->set_weight(subproc_weight_);
  subproc_->Start(
      NewCallback(
          this,
          &CompileTask::FinishSubProcess));
}

void CompileTask::RunSubProcess(const std::string& reason) {
  VLOG(1) << trace_id_ << " RunSubProcess " << reason;
  CHECK(!abort_);
  if (subproc_ == nullptr) {
    LOG(WARNING) << trace_id_ << " subproc already finished.";
    return;
  }
  stats_->exec_log.set_local_run_reason(reason);
  subproc_->RequestRun();
  VLOG(1) << trace_id_ << " Run " << reason << " "
          << subproc_->req().DebugString();
}

void CompileTask::KillSubProcess() {
  // TODO: support the case subprocess is killed by FAIL_FAST.
  VLOG(1) << trace_id_ << " KillSubProcess";
  CHECK(subproc_ != nullptr);
  SubProcessState::State state = subproc_->state();
  local_killed_ = subproc_->Kill();  // Will call FinishSubProcess().
  VLOG(1) << trace_id_ << " kill pid=" << subproc_->started().pid()
          << " " << local_killed_
          << " " << SubProcessState::State_Name(state)
          << "->" << SubProcessState::State_Name(subproc_->state());
  if (local_killed_) {
    if (service_->dont_kill_subprocess()) {
      stats_->exec_log.set_local_run_reason("fast goma, but wait for local.");
    } else {
      stats_->exec_log.set_local_run_reason("killed by fast goma");
    }
  } else if (subproc_->started().pid() != SubProcessState::kInvalidPid) {
    // subproc was signaled but not waited yet.
    stats_->exec_log.set_local_run_reason("fast goma, local signaled");
  } else {
    // subproc was initialized, but not yet started.
    stats_->exec_log.set_local_run_reason("fast goma, local not started");
  }
}

void CompileTask::FinishSubProcess() {
  VLOG(1) << trace_id_ << " FinishSubProcess";
  CHECK(BelongsToCurrentThread());
  CHECK(!abort_);
  SubProcessTask* subproc = nullptr;
  {
    AUTOLOCK(lock, &mu_);
    subproc = subproc_;
    subproc_ = nullptr;
  }
  CHECK(subproc);

  LOG(INFO) << trace_id_ << " finished subprocess."
            << " pid=" << subproc->started().pid()
            << " status=" << subproc->terminated().status()
            << " pending_ms=" << subproc->started().pending_ms()
            << " run_ms=" << subproc->terminated().run_ms()
            << " mem_kb=" << subproc->terminated().mem_kb()
            << " local_killed=" << local_killed_;

  bool local_run_failed = false;
  bool local_run_goma_failure = false;
  if (subproc->started().pid() != SubProcessState::kInvalidPid) {
    local_run_ = true;
    if (!local_killed_) {
      subproc_exit_status_ = subproc->terminated().status();
      // something failed after start of subproc. e.g. kill failed.
      if (subproc_exit_status_ < 0) {
        stats_->exec_log.set_compiler_proxy_error(true);
        LOG(ERROR) << trace_id_ << " subproc exec failure by goma"
                   << " pid=" << subproc->started().pid()
                   << " status=" << subproc_exit_status_
                   << " error=" << SubProcessTerminated_ErrorTerminate_Name(
                       subproc->terminated().error());
        local_run_goma_failure = true;
      }
      if (subproc_exit_status_ != 0) {
        local_run_failed = true;
      }
    }
    stats_->exec_log.set_local_pending_time(subproc->started().pending_ms());
    stats_->local_pending_time =
        absl::Milliseconds(subproc->started().pending_ms());

    stats_->exec_log.set_local_run_time(subproc->terminated().run_ms());
    stats_->local_run_time = absl::Milliseconds(subproc->terminated().run_ms());

    stats_->exec_log.set_local_mem_kb(subproc->terminated().mem_kb());
    VLOG(1) << trace_id_ << " subproc finished"
            << " pid=" << subproc->started().pid();
  } else {
    // pid is kInvalidPid
    if (subproc->terminated().status() ==
        SubProcessTerminated::kInternalError) {
      std::ostringstream ss;
      ss << "failed to run compiler locally."
         << " pid=" << subproc->started().pid()
         << " error=" << SubProcessTerminated_ErrorTerminate_Name(
             subproc->terminated().error())
         << " status=" << subproc->terminated().status();
      AddErrorToResponse(TO_USER, ss.str(), true);
      local_run_failed = true;
      local_run_goma_failure = true;
    }
  }

  if (state_ == FINISHED && !fail_fallback_) {
    ProcessReply();
    return;
  }

  // This subprocess would be
  // - gch hack (state_ < FINISHED, goma service was slower than local).
  // - verify output. (state_ == INIT) -> SETUP
  // - should fallback. (state_ == INIT) -> LOCAL_FINISHED.
  // - fail fallback. (state_ = FINISHED, fail_fallback_ == true)
  // - fallback only (state_ == LOCAL_RUN)
  // - idle fallback (state_ < FINISHED, goma service was slower than local).
  //   - might be killed because gomacc closed the ipc.
  std::string orig_stdout = resp_->result().stdout_buffer();
  std::string orig_stderr = resp_->result().stderr_buffer();

  CHECK(resp_.get() != nullptr) << trace_id_ << " state=" << state_;
  ExecResult* result = resp_->mutable_result();
  CHECK(result != nullptr) << trace_id_ << " state=" << state_;
  if (fail_fallback_ && local_run_ &&
      result->exit_status() != subproc->terminated().status())
    stats_->exec_log.set_goma_error(true);
  result->set_exit_status(subproc->terminated().status());
  if (subproc->terminated().has_term_signal()) {
    std::ostringstream ss;
    ss << "child process exited unexpectedly with signal."
       << " signal=" << subproc->terminated().term_signal();
    exec_error_message_.push_back(ss.str());
    CHECK(result->exit_status() != 0)
        << trace_id_ << " if term signal is not 0, exit status must not be 0."
        << ss.str();
  }

  std::string stdout_buffer;
  CHECK(!subproc_stdout_.empty()) << trace_id_ << " state=" << state_;
  ReadFileToString(subproc_stdout_.c_str(), &stdout_buffer);
  remove(subproc_stdout_.c_str());

  std::string stderr_buffer;
  CHECK(!subproc_stderr_.empty()) << trace_id_ << " state=" << state_;
  ReadFileToString(subproc_stderr_.c_str(), &stderr_buffer);
  remove(subproc_stderr_.c_str());

  if (fail_fallback_ && local_run_ &&
      !IsSameErrorMessage(orig_stdout, stdout_buffer, orig_stderr,
                          stderr_buffer)) {
    stats_->exec_log.set_goma_error(true);
  }

  result->set_stdout_buffer(stdout_buffer);
  result->set_stderr_buffer(stderr_buffer);

  if (verify_output_) {
    if (should_fallback_) {
      LOG(WARNING) << trace_id_
                   << " handled locally, no remote results to verify";
    } else {
      CHECK_EQ(INIT, state_);
      // local runs done, start remote.
      ProcessSetup();
      return;
    }
  }

  if (flags_->is_precompiling_header() && service_->enable_gch_hack()) {
    CHECK_LT(state_, FINISHED) << trace_id_ << " finish subproc";
    CHECK(subproc_ == nullptr) << trace_id_ << " finish subproc";
    // local runs done, not yet goma.
    return;
  }

  // Upload output files asynchronously, so that these files could be
  // used in link phrase.
  if (!local_run_failed) {
    ProcessLocalFileOutput();
    // The callback must be called asynchronously.
    if (service_->store_local_run_output())
      CHECK(local_output_file_callback_ != nullptr);
  }
  if (should_fallback_) {
    CHECK_EQ(INIT, state_);
    state_ = LOCAL_FINISHED;
    finished_ = true;
    // reply fallback response.
    VLOG(2) << trace_id_ << " should fallback:" << resp_->DebugString();
    if (!local_run_failed) {
      ReplyResponse("should fallback");
    } else {
      ReplyResponse("should fallback but local run failed");
    }
    return;
  }
  if (fail_fallback_) {
    CHECK_EQ(FINISHED, state_);
    VLOG(2) << trace_id_ << " fail fallback:" << resp_->DebugString();
    if (!local_run_failed) {
      ReplyResponse("fail fallback");
    } else {
      // If both remote and local failed, it is a real compile failure.
      // We must not preserve goma's error message then. (b/27889459)
      resp_->clear_error_message();
      ReplyResponse("fail fallback and local run also failed");
    }
    return;
  }
  if (state_ == LOCAL_RUN) {
    VLOG(2) << trace_id_ << " local run finished:" << resp_->DebugString();
    state_ = LOCAL_FINISHED;
    finished_ = true;
    if (!local_run_goma_failure) {
      resp_->clear_error_message();
    }
    ReplyResponse("local finish, no goma");
    // TODO: restart from the beginning.
    // Since no remote compile is running here, it is nice to start remote
    // compile in this case.  However, let me postpone the implementation
    // until I understand procedure of CompileTask well.
    return;
  }
  // otherwise, local finishes earlier than remote, or setup.
  if (!local_run_goma_failure) {
    abort_ = true;
    VLOG(2) << trace_id_ << " idle fallback:" << resp_->DebugString();
    resp_->clear_error_message();
    ReplyResponse("local finish, abort goma");
    return;
  }
  // In this case, remote should be running and we expect that success.
  LOG(INFO) << trace_id_ << " local compile failed because of goma."
            << " waiting for remote result.";
}

// ----------------------------------------------------------------

bool CompileTask::failed() const {
  return stats_->exec_log.exec_exit_status() != 0;
}

bool CompileTask::canceled() const {
  return canceled_;
}

bool CompileTask::cache_hit() const {
  return stats_->exec_log.cache_hit();
}

bool CompileTask::local_cache_hit() const {
  return stats_->LocalCacheHit();
}

void CompileTask::AddErrorToResponse(ErrDest dest,
                                     const std::string& error_message,
                                     bool set_error) {
  if (!error_message.empty()) {
    if (set_error)
      LOG(ERROR) << trace_id_ << " " << error_message;
    else
      LOG(WARNING) << trace_id_ << " " << error_message;
    // Update OmitDurationFromUserError when you change the following code.
    std::ostringstream msg;
    msg << "compiler_proxy ";
    msg << "[" << handler_timer_.GetDuration() << "]: ";
    msg << error_message;
    if (dest == TO_USER) {
      DCHECK(set_error) << trace_id_
                        << " user error should always set error."
                        << " msg=" << error_message;
      resp_->add_error_message(msg.str());
    } else {
      service_->RecordErrorToLog(error_message, set_error);
    }
    exec_error_message_.push_back(msg.str());
  }
  if (set_error &&
      (!resp_->has_result() || resp_->result().exit_status() == 0)) {
    resp_->mutable_result()->set_exit_status(1);
  }
}

CommandSpec CompileTask::DumpCommandSpec() const {
  CommandSpec command_spec = command_spec_;
  command_spec.set_local_compiler_path(local_compiler_path_);
  if (compiler_info_state_.get() != nullptr) {
    const CompilerInfo& compiler_info = compiler_info_state_.get()->info();
    std::vector<std::string> args(stats_->exec_log.arg().begin(),
                                  stats_->exec_log.arg().end());
    std::unique_ptr<CompilerFlags> flags(
        CompilerFlagsParser::New(std::move(args), stats_->exec_log.cwd()));
    FixCommandSpec(compiler_info, *flags, &command_spec);
    FixSystemLibraryPath(system_library_paths_, &command_spec);
  }
  return command_spec;
}

std::string CompileTask::DumpRequest() const {
  if (!frozen_timestamp_.has_value()) {
    LOG(ERROR) << trace_id_ << " cannot dump an active task request";
    return "cannot dump an active task request\n";
  }
  std::string message;
  LOG(INFO) << trace_id_ << " DumpRequest";
  std::string filename = "exec_req.data";
  ExecReq req;
  *req.mutable_command_spec() = DumpCommandSpec();
  if (compiler_info_state_.get() != nullptr) {
    const CompilerInfo& compiler_info = compiler_info_state_.get()->info();
    std::vector<std::string> args(stats_->exec_log.arg().begin(),
                                  stats_->exec_log.arg().end());
    std::unique_ptr<CompilerFlags> flags(
        CompilerFlagsParser::New(args, stats_->exec_log.cwd()));
    MayFixSubprogramSpec(req.mutable_subprogram());
    if (service_->send_compiler_binary_as_input()) {
      SetToolchainSpecs(&req, compiler_info);
    }
    if (service_->send_expected_outputs()) {
      SetExpectedOutputs(&req, *flags);
    }
  } else {
    // If compiler_info_state_ is nullptr, it would be should_fallback_.
    LOG_IF(ERROR, !should_fallback_)
        << trace_id_ << " DumpRequest compiler_info_state_ is nullptr.";
    filename = "local_exec_req.data";
  }

  // stats_ contains modified version of args, env.
  for (const auto& arg : stats_->exec_log.arg())
    req.add_arg(arg);
  for (const auto& env : stats_->exec_log.env())
    req.add_env(env);
  for (const auto& expanded_arg : stats_->exec_log.expanded_arg())
    req.add_expanded_arg(expanded_arg);
  req.set_cwd(stats_->exec_log.cwd());
  *req.mutable_requester_info() = requester_info_;
  if (compiler_info_state_.get() != nullptr) {
    FixRequesterInfo(compiler_info_state_.get()->info(),
                     req.mutable_requester_info());
  }

  std::ostringstream ss;
  ss << "task_request_" << id_;
  const std::string task_request_dir =
      file::JoinPath(service_->tmp_dir(), ss.str());
  file::RecursivelyDelete(task_request_dir, file::Defaults());
#ifndef _WIN32
  PCHECK(mkdir(task_request_dir.c_str(), 0755) == 0);
#else
  if (!CreateDirectoryA(task_request_dir.c_str(), nullptr)) {
    DWORD err = GetLastError();
    LOG_SYSRESULT(err);
    LOG_IF(FATAL, FAILED(err)) << "CreateDirectoryA " << task_request_dir;
  }
#endif

  // required_files_ contains all input files used in exec req,
  // including compiler resources, toolchain binaries when
  // send_compiler_binary_as_input is true.
  for (const auto& input_filename : required_files_) {
    ExecReq_Input* input = req.add_input();
    input->set_filename(input_filename);
    FileServiceDumpClient fs;
    const std::string abs_input_filename =
        file::JoinPathRespectAbsolute(req.cwd(), input_filename);
    if (!fs.CreateFileBlob(abs_input_filename, true,
                           input->mutable_content())) {
      LOG(ERROR) << trace_id_ << " DumpRequest failed to create fileblob:"
                 << input_filename;
      message += absl::StrCat(
          "DumpRequest failed to create fileblob: ", input_filename, "\n");
    } else {
      input->set_hash_key(ComputeFileBlobHashKey(input->content()));
      if (!fs.Dump(file::JoinPath(task_request_dir, input->hash_key()))) {
        LOG(ERROR) << trace_id_ << " DumpRequest failed to store fileblob:"
                   << input_filename
                   << " hash:" << input->hash_key();
        message += absl::StrCat("DumpRequest failed to store fileblob:",
                                " input_filename=", input_filename,
                                " hash=", input->hash_key());
      }
    }
  }
  std::string r;
  req.SerializeToString(&r);
  filename = file::JoinPath(task_request_dir, filename);
  if (!WriteStringToFile(r, filename)) {
    LOG(ERROR) << trace_id_
               << " failed to write serialized proto: " << filename;
    message +=
        absl::StrCat("failed to write serialized proto: ", filename, "\n");
  } else {
    LOG(INFO) << trace_id_ << " DumpRequest wrote serialized proto: "
              << filename;
    message +=
        absl::StrCat("DumpRequest wrote serialized proto: ", filename, "\n");
  }

  // Only show file hash for text_format.
  for (auto& input : *req.mutable_input()) {
    input.clear_content();
  }

  std::string text_req;
  google::protobuf::TextFormat::PrintToString(req, &text_req);
  filename += ".txt";
  if (!WriteStringToFile(text_req, filename)) {
    LOG(ERROR) << trace_id_ << " failed to write text proto: " << filename;
    message += absl::StrCat("failed to write text proto: ", filename, "\n");
  } else {
    LOG(INFO) << trace_id_ << " DumpRequest wrote text proto: " << filename;
    message += absl::StrCat("DumpRequest wrote text proto: ", filename, "\n");
  }

  LOG(INFO) << trace_id_ << " DumpRequest done";
  return message;
}

}  // namespace devtools_goma
