// Copyright 2010 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "subprocess.h"

#include <fcntl.h>
#include <stdio.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <deque>
#include <iostream>
#include <memory>

#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "compiler_flags.h"
#include "compiler_specific.h"
#include "env_flags.h"
#include "file_path_util.h"
#include "file_stat.h"
#include "glog/logging.h"
#include "mypath.h"
#include "path.h"
#include "scoped_fd.h"
#ifndef _WIN32
#include "spawner_posix.h"
#else
#include "spawner_win.h"
#endif

namespace devtools_goma {

namespace {

#ifdef _WIN32
std::string GetPathExt(const std::vector<std::string>& envs) {
  return GetEnvFromEnvIter(envs.begin(), envs.end(), "PATHEXT");
}
#else
std::string GetPathExt(const std::vector<std::string>& envs ALLOW_UNUSED) {
  return "";
}
#endif

bool GetRealPrognameAndEnvs(const FileStat* gomacc_filestat,
                            const std::string& prog,
                            const std::vector<std::string>& args,
                            std::vector<std::string>* envs,
                            std::string* real_progname) {
  static const char kPath[] = "PATH";
  *real_progname = prog;
  if (gomacc_filestat != nullptr) {
    // We should set ReadCommand to avoid gomacc in GetRealExecutablePath.
#ifndef _WIN32
    InstallReadCommandOutputFunc(ReadCommandOutputByPopen);
#else
    InstallReadCommandOutputFunc(ReadCommandOutputByRedirector);
#endif
  }

  std::string no_goma_env_path;
  if (!GetRealExecutablePath(
          gomacc_filestat, prog, ".",
          GetEnvFromEnvIter(envs->begin(), envs->end(), kPath),
          GetPathExt(*envs), real_progname, &no_goma_env_path)) {
    LOG(ERROR) << "failed to get executable path."
               << " prog=" << prog << " path="
               << GetEnvFromEnvIter(envs->begin(), envs->end(), kPath)
               << " pathext=" << GetPathExt(*envs);
    return false;
  }
  if (!ReplaceEnvInEnvIter(envs->begin(), envs->end(), kPath,
                           no_goma_env_path)) {
    LOG(ERROR) << "failed to replace path env."
               << " kPath=" << kPath << " path="
               << GetEnvFromEnvIter(envs->begin(), envs->end(), kPath)
               << " no_goma_env_path=" << no_goma_env_path;
    return false;
  }

  return true;
}

} // namespace

#ifdef _WIN32

int SpawnAndWait(const std::string& prog,
                 const std::vector<std::string>& args,
                 const std::vector<std::string>& envs) {
  return SpawnAndWaitNonGomacc(nullptr, prog, args, envs);
}

int SpawnAndWaitNonGomacc(const FileStat* gomacc_filestat,
                          const std::string& prog,
                          const std::vector<std::string>& args,
                          std::vector<std::string> envs) {
  std::string real_progname;
  GetRealPrognameAndEnvs(gomacc_filestat, prog, args, &envs, &real_progname);

  std::unique_ptr<SpawnerWin> spawner(new SpawnerWin);
  int status = spawner->Run(
      real_progname, args, envs, GetCurrentDirNameOrDie());
  if (status == Spawner::kInvalidPid) {
    return -1;
  }
  while (spawner->IsChildRunning())
    spawner->Wait(Spawner::WAIT_INFINITE);
  return spawner->ChildStatus();
}

#else

int Execvpe(const std::string& prog,
            const std::vector<std::string>& args,
            const std::vector<std::string>& envs) {
  return ExecvpeNonGomacc(nullptr, prog, args, envs);
}

int ExecvpeNonGomacc(const FileStat* gomacc_filestat,
                     const std::string& prog,
                     const std::vector<std::string>& args,
                     std::vector<std::string> envs) {
  std::string real_progname;
  GetRealPrognameAndEnvs(gomacc_filestat, prog, args, &envs, &real_progname);

  std::vector<const char*> argvp;
  std::vector<const char*> envp;

  for (const auto& arg : args) {
    argvp.push_back(arg.c_str());
  }
  argvp.push_back(nullptr);

  for (const auto& env : envs) {
    envp.push_back(env.c_str());
  }
  envp.push_back(nullptr);

  return execve(real_progname.c_str(), const_cast<char**>(&argvp[0]),
                const_cast<char**>(&envp[0]));
}

#endif

#ifndef _WIN32
std::string ReadCommandOutputByPopen(const std::string& prog,
                                     const std::vector<std::string>& argv,
                                     const std::vector<std::string>& envs,
                                     const std::string& cwd,
                                     CommandOutputOption option,
                                     int32_t* status) {
  std::string commandline;
  if (!cwd.empty()) {
    commandline = "sh -c 'cd " + cwd + " && ";
  }
  for (const auto& env : envs)
    commandline += env + " ";
  for (const auto& arg : argv) {
    // Escaping only <, >, ( and ) is OK for now.
    if (arg.find_first_of(" <>();&'#") == std::string::npos) {
      CHECK(arg.find_first_of("\\\"") == std::string::npos) << arg;
      commandline += arg + " ";
    } else {
      commandline += "\"" + arg + "\" ";
    }
  }
  if (!cwd.empty()) {
    commandline += "'";
  }
  if (option == MERGE_STDOUT_STDERR)
    commandline += " 2>&1";

  FILE* p = popen(commandline.c_str(), "r");
  CHECK(p) << "popen for " << prog << " (" << commandline << ") failed";

  std::ostringstream strbuf;
  while (true) {
    const size_t kBufSize = 64 * 1024;
    char buf[kBufSize];
    size_t len = fread(buf, 1, kBufSize, p);
    if (len == 0) {
      if (errno == EINTR)
        continue;
      CHECK(feof(p)) << "could not read output for: " << commandline;
      break;
    }
    strbuf.write(buf, len);
  }

  int exit_status = pclose(p);
  if (status) {
    *status = exit_status;
  } else {
    LOG_IF(FATAL, exit_status != 0)
        << "If the caller expects the non-zero exit status, "
        << "the caller must set non-nullptr status in the argument."
        << " prog=" << prog
        << " args=" << absl::StrJoin(argv, " ")
        << " cwd=" << cwd
        << " exit_status=" << exit_status
        << " output=" << strbuf.str();
  }

  return strbuf.str();
}

void Daemonize(const std::string& stderr_filename,
               int pid_record_fd,
               const std::set<int>& preserve_fds) {
  PCHECK(setsid() >= 0);
  PCHECK(chdir("/") == 0);
  umask(0);

  // Fork again, so we'll never get tty.
  pid_t pid;
  if ((pid = fork())) {
    PCHECK(pid > 0);
    exit(0);
  }

  pid = Getpid();
  if (pid_record_fd >= 0) {
    PCHECK(write(pid_record_fd, &pid, sizeof(pid)) == sizeof(pid));
  } else {
    std::cout << pid << std::endl;
  }

  int devnullfd = ScopedFd::OpenNull();
  CHECK_GE(devnullfd, 0);
  PCHECK(dup2(devnullfd, STDIN_FILENO) >= 0);
  PCHECK(dup2(devnullfd, STDOUT_FILENO) >= 0);

  int stderrfd = -1;
  if (!stderr_filename.empty())
    stderrfd = open(stderr_filename.c_str(), O_WRONLY|O_CREAT, 0660);

  if (stderrfd >= 0) {
    PCHECK(dup2(stderrfd, STDERR_FILENO) >= 0);
  } else {
    PCHECK(dup2(devnullfd, STDERR_FILENO) >= 0);
  }

  // Close all file descriptors except stdin/stdout/stderr and in preserve_fds.
  int maxfd = sysconf(_SC_OPEN_MAX);
  for (int fd = STDERR_FILENO + 1; fd < maxfd; ++fd) {
    if (preserve_fds.count(fd) == 0)
      close(fd);
  }
}

#else

std::string ReadCommandOutputByRedirector(const std::string& prog,
                                          const std::vector<std::string>& argv,
                                          const std::vector<std::string>& env,
                                          const std::string& cwd,
                                          CommandOutputOption option,
                                          int32_t* status) {
  SpawnerWin spawner;
  Spawner::ConsoleOutputOption output_option =
      Spawner::MERGE_STDOUT_STDERR;
  if (option == STDOUT_ONLY)
    output_option = Spawner::STDOUT_ONLY;
  std::string output;
  spawner.SetConsoleOutputBuffer(&output, output_option);
  spawner.Run(prog, argv, env, cwd);
  while (spawner.IsChildRunning())
    spawner.Wait(Spawner::WAIT_INFINITE);
  int exit_status = spawner.ChildStatus();
  if (status) {
    *status = exit_status;
  } else {
    LOG_IF(FATAL, exit_status != 0)
        << "If the caller expects the non-zero exit status, "
        << "the caller must set non-nullptr status in the argument."
        << " prog=" << prog
        << " cwd=" << cwd
        << " exit_status=" << exit_status;
  }
  return output;
}

#endif

}  // namespace devtools_goma
