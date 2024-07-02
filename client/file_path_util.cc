// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "file_path_util.h"

#include <deque>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_split.h"
#include "compiler_flag_type_specific.h"
#include "compiler_specific.h"
#include "glog/logging.h"
#include "glog/stl_logging.h"
#include "ioutil.h"
#include "path.h"
#include "path_resolver.h"
#include "util.h"
#include "vc_flags.h"

MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "lib/goma_data.pb.h"
MSVC_POP_WARNING()

namespace devtools_goma {

namespace {

// Path separators are platform dependent
#ifndef _WIN32
const char* kPathListSep = ":";
#else
const char* kPathListSep = ";";
#endif

#ifdef _WIN32
std::deque<std::string> ParsePathExts(const std::string& pathext_spec) {
  std::vector<std::string> pathexts;
  if (!pathext_spec.empty()) {
    pathexts =
        ToVector(absl::StrSplit(pathext_spec, kPathListSep, absl::SkipEmpty()));
  } else {
    // If |pathext_spec| is empty, we should use the default PATHEXT.
    // See:
    // http://technet.microsoft.com/en-us/library/cc723564.aspx#XSLTsection127121120120
    static const char* kDefaultPathext = ".COM;.EXE;.BAT;.CMD";
    pathexts = ToVector(
        absl::StrSplit(kDefaultPathext, kPathListSep, absl::SkipEmpty()));
  }

  for (auto& pathext : pathexts) {
    absl::AsciiStrToLower(&pathext);
  }
  return std::deque<std::string>(pathexts.begin(), pathexts.end());
}

bool HasExecutableExtension(const std::deque<std::string>& pathexts,
                            const std::string& filename) {
  const size_t pos = filename.rfind(".");
  if (pos == std::string::npos)
    return false;

  std::string ext = filename.substr(pos);
  absl::AsciiStrToLower(&ext);
  for (const auto& pathext : pathexts) {
    if (ext == pathext)
      return true;
  }
  return false;
}

std::string GetExecutableWithExtension(const std::deque<std::string>& pathexts,
                                       const std::string& cwd,
                                       const std::string& prefix) {
  for (const auto& pathext : pathexts) {
    const std::string& fullname = prefix + pathext;
    // Do not return cwd prefixed path here.
    const std::string& candidate = file::JoinPathRespectAbsolute(cwd, fullname);
    DWORD attr = GetFileAttributesA(candidate.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES &&
        (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      return fullname;
    }
  }
  return "";
}
#endif

}  // anonymous namespace

// True if |candidate_path| is gomacc, by running it under an invalid GOMA env
// flag.  It is usually used to confirm |candidate_path| is not gomacc.
// If candadate_path is (a copy of or a symlink to) gomacc, it will die with
// "unknown GOMA_ parameter".
// It assumes real compiler doesn't emit "GOMA" in its output.
// On Windows, path must include a directory where mspdb*.dll,
// otherwise, real cl.exe will pops up a dialog:
//  This application has failed to start because mspdb100.dll was not found.
// Error mode SEM_FAILCRITICALERRORS and SEM_NOGPFAULTERRORBOX
// prevent from popping up message box on error, which we did in
// compiler_proxy.cc:main()
bool IsGomacc(const std::string& candidate_path,
              const std::string& path,
              const std::string& pathext,
              const std::string& cwd) {
  // TODO: fix workaround.
  // Workaround not to pause with dialog when cl.exe is executed.
  if (VCFlags::IsVCCommand(candidate_path))
    return false;

  std::vector<std::string> argv;
  argv.push_back(candidate_path);
  std::vector<std::string> env;
  env.push_back("GOMA_WILL_FAIL_WITH_UKNOWN_FLAG=true");
  env.push_back("PATH=" + path);
  if (!pathext.empty())
    env.push_back("PATHEXT=" + pathext);
  int32_t status = 0;
  std::string out = ReadCommandOutput(candidate_path, argv, env, cwd,
                                      MERGE_STDOUT_STDERR, &status);
  return (status == 1) && (out.find("GOMA") != std::string::npos);
}

bool GetRealExecutablePath(const FileStat* gomacc_filestat,
                           const std::string& cmd,
                           const std::string& cwd,
                           const std::string& path_env,
                           const std::string& pathext_env,
                           std::string* local_executable_path,
                           std::string* no_goma_path_env) {
  CHECK(local_executable_path);
#ifndef _WIN32
  DCHECK(pathext_env.empty());
#else
  std::deque<std::string> pathexts = ParsePathExts(pathext_env);
  if (HasExecutableExtension(pathexts, cmd)) {
    pathexts.push_front("");
  }
#endif

  if (no_goma_path_env)
    *no_goma_path_env = path_env;

  // Fast path.
  // If cmd contains '/', it is just cwd/cmd.
  if (cmd.find_first_of(PathResolver::kPathSep) != std::string::npos) {
#ifndef _WIN32
    std::string candidate_fullpath = file::JoinPathRespectAbsolute(cwd, cmd);
    if (access(candidate_fullpath.c_str(), X_OK) != 0)
      return false;
    const std::string& candidate_path = cmd;
#else
    std::string candidate_path = GetExecutableWithExtension(pathexts, cwd, cmd);
    if (candidate_path.empty()) {
      LOG(ERROR) << "empty candidate_path from GetExecutableWithExtension"
                 << " pathexts=" << pathexts << " cwd=" << cwd
                 << " cmd=" << cmd;
      return false;
    }
    std::string candidate_fullpath =
        file::JoinPathRespectAbsolute(cwd, candidate_path);
#endif
    const FileStat candidate_filestat(candidate_fullpath);

    if (!candidate_filestat.IsValid()) {
      LOG(ERROR) << "invalid filestats candidate_path=" << candidate_path
                 << " candidate_fullpath=" << candidate_fullpath;
      return false;
    }

    if (gomacc_filestat && candidate_filestat == *gomacc_filestat)
      return false;

    if (gomacc_filestat &&
        IsGomacc(candidate_fullpath, path_env, pathext_env, cwd))
      return false;

    *local_executable_path = candidate_path;
    return true;
  }

  for (size_t pos = 0, next_pos; pos != std::string::npos; pos = next_pos) {
    next_pos = path_env.find(kPathListSep, pos);
    absl::string_view dir;
    if (next_pos == absl::string_view::npos) {
      dir = absl::string_view(path_env.c_str() + pos, path_env.size() - pos);
    } else {
      dir = absl::string_view(path_env.c_str() + pos, next_pos - pos);
      ++next_pos;
    }

    // Empty paths should be treated as the current directory.
    if (dir.empty()) {
      dir = cwd;
    }
    VLOG(2) << "dir:" << dir;

    std::string candidate_path(PathResolver::ResolvePath(
        file::JoinPath(file::JoinPathRespectAbsolute(cwd, dir), cmd)));
    VLOG(2) << "candidate:" << candidate_path;

#ifndef _WIN32
    if (access(candidate_path.c_str(), X_OK) != 0)
      continue;
#else
    candidate_path = GetExecutableWithExtension(pathexts, cwd, candidate_path);
    if (candidate_path.empty())
      continue;
#endif
    DCHECK(file::IsAbsolutePath(candidate_path));

    FileStat candidate_filestat(candidate_path);
    if (candidate_filestat.IsValid()) {
      if (gomacc_filestat && candidate_filestat == *gomacc_filestat &&
          next_pos != std::string::npos) {
        // file is the same as gomacc.
        // Update local path.
        // TODO: drop a path of gomacc only. preserve other paths
        // For example,
        // PATH=c:\P\MVS10.0\Common7\Tools;c:\goma;c:\P\MVS10.0\VC\bin
        // we should not drop c:\P\MVS10.0\Common7\Tools.
        if (no_goma_path_env)
          *no_goma_path_env = path_env.substr(next_pos);
      } else {
        // file is executable, and from file id, it is different
        // from gomacc.
        if (gomacc_filestat &&
            IsGomacc(candidate_path, path_env.substr(pos), pathext_env, cwd)) {
          LOG(ERROR) << "You have 2 goma directories in your path? "
                     << candidate_path << " seems gomacc";
          if (next_pos != std::string::npos && no_goma_path_env)
            *no_goma_path_env = path_env.substr(next_pos);
          continue;
        }
        *local_executable_path = candidate_path;
        return true;
      }
    }
  }
  return false;
}

bool IsLocalCompilerPathValid(const std::string& trace_id,
                              const ExecReq& req,
                              const std::string& compiler_name) {
  // Compiler_proxy will resolve local_compiler_path if gomacc is masqueraded or
  // prepended compiler is basename. No need to think this as error.
  if (!req.command_spec().has_local_compiler_path()) {
    return true;
  }
  // If local_compiler_path exists, it must be the same compiler_name with
  // flag_'s.
  const std::string name = CompilerFlagTypeSpecific::GetCompilerNameFromArg(
      req.command_spec().local_compiler_path());
  if (req.command_spec().has_name() && req.command_spec().name() != name) {
    LOG(ERROR) << trace_id << " compiler name mismatches."
               << " command_spec.name=" << req.command_spec().name()
               << " name=" << name;
    return false;
  }
  if (compiler_name != name) {
    LOG(ERROR) << trace_id << " compiler name mismatches."
               << " compiler_name=" << compiler_name
               << " name=" << name;
    return false;
  }
  return true;
}

void RemoveDuplicateFiles(const std::string& cwd,
                          std::set<std::string>* filenames,
                          std::vector<std::string>* removed_files) {
  absl::flat_hash_map<std::string, std::string> path_map;
  path_map.reserve(filenames->size());

  std::set<std::string> unique_files;
  for (const auto& filename : *filenames) {
    std::string abs_filename = file::JoinPathRespectAbsolute(cwd, filename);
    auto p = path_map.emplace(std::move(abs_filename), filename);
    if (p.second) {
      unique_files.insert(filename);
      continue;
    }

    // If there is already registered filename, compare and take shorter one.
    // If length is same, take lexicographically smaller one.
    const std::string& existing_filename = p.first->second;
    if (filename.size() < existing_filename.size() ||
        (filename.size() == existing_filename.size() &&
         filename < existing_filename)) {
      unique_files.erase(existing_filename);
      removed_files->push_back(existing_filename);
      unique_files.insert(filename);
      p.first->second = filename;
    } else {
      removed_files->push_back(filename);
    }
  }

  *filenames = std::move(unique_files);
}

#ifdef _WIN32

std::string ResolveExtension(const std::string& cmd,
                             const std::string& pathext_env,
                             const std::string& cwd) {
  std::deque<std::string> pathexts = ParsePathExts(pathext_env);
  if (HasExecutableExtension(pathexts, cmd)) {
    pathexts.push_front("");
  }
  return GetExecutableWithExtension(pathexts, cwd, cmd);
}

#endif

}  // namespace devtools_goma
