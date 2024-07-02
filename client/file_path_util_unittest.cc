// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "file_path_util.h"

#include "absl/strings/string_view.h"
#include "compiler_flag_type_specific.h"
#include "compiler_flags_parser.h"
#include "compiler_specific.h"
#include "glog/logging.h"
#include "gtest/gtest.h"
#include "path.h"
#include "unittest_util.h"
#include "util.h"

MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "lib/goma_data.pb.h"
MSVC_POP_WARNING()

namespace devtools_goma {

namespace {

#ifdef _WIN32
constexpr absl::string_view kRootDir("C:\\");
#else
constexpr absl::string_view kRootDir("/");
#endif

#ifdef _WIN32
std::string LocateExecutable(const char* cwd_in,
                             const char* path_in,
                             const char* pathext_in,
                             const char* cmd_in) {
  std::string path;
  if (path_in == nullptr) {
    auto optional_path = devtools_goma::GetEnv("PATH");
    CHECK(optional_path);
    path = std::move(*optional_path);
    CHECK(!path.empty());
  } else {
    path.assign(path_in);
  }

  std::string pathext;
  if (pathext_in == nullptr) {
    auto optional_pathext = devtools_goma::GetEnv("PATHEXT");
    CHECK(optional_pathext);
    pathext = std::move(*optional_pathext);
    CHECK(!pathext.empty());
  } else {
    pathext.assign(pathext_in);
  }

  std::string exec_path;
  if (devtools_goma::GetRealExecutablePath(nullptr, cmd_in, cwd_in, path,
                                           pathext, &exec_path, nullptr)) {
    return exec_path;
  }
  return "";
}
#endif

}  // namespace

TEST(FilePathUtil, GetRealExecutablePath) {
// TODO: write test for POSIX.
#ifdef _WIN32
  std::string located = LocateExecutable("", nullptr, nullptr, "cmd");
  EXPECT_GT(located.size(), 3UL);

  // Shouls accept command with an extension.
  located = LocateExecutable("", nullptr, nullptr, "cmd.exe");
  EXPECT_GT(located.size(), 7UL);

  // Should ignore case.
  located = LocateExecutable("", nullptr, nullptr, "cmd.ExE");
  EXPECT_GT(located.size(), 7UL);

  // Not existing file.
  located = LocateExecutable("", nullptr, nullptr, "shall_not_have_this_file");
  EXPECT_TRUE(located.empty());

  // Empty PATHEXT.  Default pathext is used. i.e. it should not be empty.
  located = LocateExecutable("", nullptr, "", "cmd");
  EXPECT_GT(located.size(), 3UL);

  // Strange PATHEXT.  Nothing should match.
  located = LocateExecutable("", nullptr, ".non_exist_pathext", "cmd");
  EXPECT_TRUE(located.empty());

  // Expected PATHEXT.
  located = LocateExecutable("", nullptr, ".exe", "cmd");
  EXPECT_GT(located.size(), 3UL);

  // Expected PATHEXT with upper case letters.
  located = LocateExecutable("", nullptr, ".EXE", "cmd");
  EXPECT_GT(located.size(), 3UL);

  // Unexpected PATHEXT.
  located = LocateExecutable("", nullptr, ".com", "cmd");
  EXPECT_TRUE(located.empty());

  // Extension is not listed in PATHEXT. Nothing should match.
  located = LocateExecutable("", nullptr, ".com", "cmd.exe");
  EXPECT_TRUE(located.empty());

  // Expected PATHEXT comes after unexpected PATHEXT.
  located = LocateExecutable("", nullptr, ".com;.exe", "cmd");
  EXPECT_GT(located.size(), 3UL);

  // Expected PATHEXT comes after unexpected PATHEXT (upper case letters).
  located = LocateExecutable("", nullptr, ".COM;.EXE", "cmd");
  EXPECT_GT(located.size(), 3UL);

  // Expected PATHEXT should be automatically added even if full-path given.
  std::string expected = located;
  std::string input = located.substr(0, located.length() - 4);
  EXPECT_FALSE(input.empty());
  located = LocateExecutable("", "", nullptr, input.c_str());
  EXPECT_EQ(expected, located);
#endif
  // TODO: revise this using TmpdirUtil.
}

TEST(FilePathUtilTest, IsLocalCompilerPathValidWithEmptyLocalCompilerPath) {
  ExecReq req;
  req.mutable_command_spec()->set_name("gcc");
  req.mutable_command_spec()->set_version("1.2.3");
  req.mutable_command_spec()->set_target("x86_64-linux-gnu");
  req.mutable_command_spec()->set_binary_hash("deadbeef");
  req.set_cwd("/tmp");

  std::vector<std::string> args = {"gcc", "-c", "dummy.cc"};
  std::unique_ptr<CompilerFlags> flag(CompilerFlagsParser::MustNew(args, "."));

  EXPECT_TRUE(IsLocalCompilerPathValid("dummy", req, flag->compiler_name()));
}

TEST(FilePathUtilTest, IsLocalCompilerPathValidWithSameCommandSpec) {
  ExecReq req;
  req.mutable_command_spec()->set_name("gcc");
  req.mutable_command_spec()->set_version("1.2.3");
  req.mutable_command_spec()->set_target("x86_64-linux-gnu");
  req.mutable_command_spec()->set_binary_hash("deadbeef");
  req.set_cwd("/tmp");
  req.mutable_command_spec()->set_local_compiler_path("/usr/bin/gcc");

  std::vector<std::string> args = {"gcc", "-c", "dummy.cc"};
  std::unique_ptr<CompilerFlags> flag(CompilerFlagsParser::MustNew(args, "."));

  EXPECT_TRUE(IsLocalCompilerPathValid("dummy", req, flag->compiler_name()));
}

TEST(FilePathUtilTest, IsLocalCompilerPathValidWithUnknownName) {
  ExecReq req;
  req.mutable_command_spec()->set_name("gcc");
  req.mutable_command_spec()->set_version("1.2.3");
  req.mutable_command_spec()->set_target("x86_64-linux-gnu");
  req.mutable_command_spec()->set_binary_hash("deadbeef");
  req.set_cwd("/tmp");
  req.mutable_command_spec()->set_local_compiler_path("/usr/bin/id");

  std::vector<std::string> args = {"gcc", "-c", "dummy.cc"};
  std::unique_ptr<CompilerFlags> flag(CompilerFlagsParser::MustNew(args, "."));

  EXPECT_FALSE(IsLocalCompilerPathValid("dummy", req, flag->compiler_name()));
}

TEST(FilePathUtilTest, IsLocalCompilerPathValidWithCommandSpecMismatch) {
  ExecReq req;
  req.mutable_command_spec()->set_name("clang");
  req.mutable_command_spec()->set_version("1.2.3");
  req.mutable_command_spec()->set_target("x86_64-linux-gnu");
  req.mutable_command_spec()->set_binary_hash("deadbeef");
  req.set_cwd("/tmp");
  req.mutable_command_spec()->set_local_compiler_path("/usr/bin/gcc");

  std::vector<std::string> args = {"gcc", "-c", "dummy.cc"};
  std::unique_ptr<CompilerFlags> flag(CompilerFlagsParser::MustNew(args, "."));

  EXPECT_FALSE(IsLocalCompilerPathValid("dummy", req, flag->compiler_name()));
}

TEST(FilePathUtilTest, IsLocalCompilerPathValidWithArgsMismatch) {
  ExecReq req;
  req.mutable_command_spec()->set_name("gcc");
  req.mutable_command_spec()->set_version("1.2.3");
  req.mutable_command_spec()->set_target("x86_64-linux-gnu");
  req.mutable_command_spec()->set_binary_hash("deadbeef");
  req.set_cwd("/tmp");
  req.mutable_command_spec()->set_local_compiler_path("/usr/bin/gcc");

  std::vector<std::string> args = {"clang", "-c", "dummy.cc"};
  std::unique_ptr<CompilerFlags> flag(CompilerFlagsParser::MustNew(args, "."));

  EXPECT_FALSE(IsLocalCompilerPathValid("dummy", req, flag->compiler_name()));
}

TEST(FilePathUtilTest, IsLocalCompilerPathValidWithSameCommandSpecClExe) {
  ExecReq req;
  req.mutable_command_spec()->set_name("cl.exe");
  req.mutable_command_spec()->set_version("1.2.3");
  req.mutable_command_spec()->set_target("x86");
  req.mutable_command_spec()->set_binary_hash("deadbeef");
  req.set_cwd("/tmp");
  req.mutable_command_spec()->set_local_compiler_path("c:\\dummy\\cl.exe");

  std::vector<std::string> args = {"c:\\dummy\\cl.exe", "/c", "dummy.cc"};
  std::unique_ptr<CompilerFlags> flag(CompilerFlagsParser::MustNew(args, "."));

  EXPECT_TRUE(IsLocalCompilerPathValid("dummy", req, flag->compiler_name()));
}

TEST(FilePathUtilTest, IsLocalCompilerPathValidWithOmittingExtension) {
  ExecReq req;
  req.mutable_command_spec()->set_name("cl.exe");
  req.mutable_command_spec()->set_version("1.2.3");
  req.mutable_command_spec()->set_target("x86");
  req.mutable_command_spec()->set_binary_hash("deadbeef");
  req.set_cwd("/tmp");
  req.mutable_command_spec()->set_local_compiler_path("c:\\dummy\\cl");

  std::vector<std::string> args = {"cl", "/c", "dummy.cc"};
  std::unique_ptr<CompilerFlags> flag(CompilerFlagsParser::MustNew(args, "."));

  EXPECT_TRUE(IsLocalCompilerPathValid("dummy", req, flag->compiler_name()));
}

TEST(FilePathUtilTest, IsLocalCompilerPathValidWithUnknownNameClExe) {
  ExecReq req;
  req.mutable_command_spec()->set_name("cl.exe");
  req.mutable_command_spec()->set_version("1.2.3");
  req.mutable_command_spec()->set_target("x86");
  req.mutable_command_spec()->set_binary_hash("deadbeef");
  req.set_cwd("/tmp");
  req.mutable_command_spec()->set_local_compiler_path(
      "c:\\dummy\\shutdown.exe");

  std::vector<std::string> args = {"c:\\dummy\\cl.exe", "/c", "dummy.cc"};
  std::unique_ptr<CompilerFlags> flag(CompilerFlagsParser::MustNew(args, "."));

  EXPECT_FALSE(IsLocalCompilerPathValid("dummy", req, flag->compiler_name()));
}

TEST(FilePathUtilTest, IsLocalCompilerPathValidWithCommandSpecMismatchClExe) {
  ExecReq req;
  req.mutable_command_spec()->set_name("clang-cl");
  req.mutable_command_spec()->set_version("1.2.3");
  req.mutable_command_spec()->set_target("x86");
  req.mutable_command_spec()->set_binary_hash("deadbeef");
  req.set_cwd("/tmp");
  req.mutable_command_spec()->set_local_compiler_path("c:\\dummy\\cl.exe");

  std::vector<std::string> args = {"c:\\dummy\\cl.exe", "/c", "dummy.cc"};
  std::unique_ptr<CompilerFlags> flag(CompilerFlagsParser::MustNew(args, "."));

  EXPECT_FALSE(IsLocalCompilerPathValid("dummy", req, flag->compiler_name()));
}

TEST(FilePathUtilTest, IsLocalCompilerPathValidWithArgsMismatchClExe) {
  ExecReq req;
  req.mutable_command_spec()->set_name("cl.exe");
  req.mutable_command_spec()->set_version("1.2.3");
  req.mutable_command_spec()->set_target("x86");
  req.mutable_command_spec()->set_binary_hash("deadbeef");
  req.set_cwd("/tmp");
  req.mutable_command_spec()->set_local_compiler_path("c:\\dummy\\cl.exe");

  std::vector<std::string> args = {"c:\\dummy\\clang-cl.exe", "/c", "dummy.cc"};
  std::unique_ptr<CompilerFlags> flag(CompilerFlagsParser::MustNew(args, "."));

  EXPECT_FALSE(IsLocalCompilerPathValid("dummy", req, flag->compiler_name()));
}
// TODO: add other combinations if necessary.

TEST(FilePathUtilTest, RemoveDuplicateFiles) {
  {
    // different filepath
    std::set<std::string> filenames{file::JoinPath(kRootDir, "foo", "bar.cc"),
                                    file::JoinPath(kRootDir, "foo", "baz.cc")};
    std::vector<std::string> removed;
    RemoveDuplicateFiles("", &filenames, &removed);

    std::set<std::string> expected{file::JoinPath(kRootDir, "foo", "bar.cc"),
                                   file::JoinPath(kRootDir, "foo", "baz.cc")};
    EXPECT_EQ(filenames, expected);
    EXPECT_TRUE(removed.empty());
  }

  {
    std::set<std::string> filenames{
        file::JoinPath(kRootDir, "fOO"),
        file::JoinPath(kRootDir, "Foo"),
        file::JoinPath(kRootDir, "fOo"),
        file::JoinPath(absl::AsciiStrToLower(kRootDir), "FOO"),
        file::JoinPath(absl::AsciiStrToLower(kRootDir), "foO"),
    };
    std::vector<std::string> removed;
    RemoveDuplicateFiles("", &filenames, &removed);
    std::set<std::string> expected{
        file::JoinPath(kRootDir, "fOO"),
        file::JoinPath(kRootDir, "Foo"),
        file::JoinPath(kRootDir, "fOo"),
        file::JoinPath(absl::AsciiStrToLower(kRootDir), "FOO"),
        file::JoinPath(absl::AsciiStrToLower(kRootDir), "foO"),
    };
    std::vector<std::string> expected_removed;
    EXPECT_EQ(filenames, expected);
    EXPECT_EQ(removed, expected_removed);
  }

  {
    // same filepath when JoinPathRespectAbsolute
    std::set<std::string> filenames{"bar.cc",
                                    file::JoinPath(kRootDir, "foo", "bar.cc")};
    std::vector<std::string> removed;
    RemoveDuplicateFiles(file::JoinPath(kRootDir, "foo"), &filenames,
                         &removed);

    std::set<std::string> expected{"bar.cc"};
    std::vector<std::string> expected_removed{
      file::JoinPath(kRootDir, "foo", "bar.cc"),
    };
    EXPECT_EQ(filenames, expected);
    EXPECT_EQ(removed, expected_removed);
  }

  {
    // same filepath when JoinPathRespectAbsolute with ..
    std::set<std::string> filenames{
        file::JoinPath("..", "bar.cc"),
        file::JoinPath(kRootDir, "foo", "baz", "..", "bar.cc")};
    std::vector<std::string> removed;
    RemoveDuplicateFiles(file::JoinPath(kRootDir, "foo", "baz"), &filenames,
                         &removed);

    std::set<std::string> expected{file::JoinPath("..", "bar.cc")};
    std::vector<std::string> expected_removed {
        file::JoinPath(kRootDir, "foo", "baz", "..", "bar.cc"),
    };
    EXPECT_EQ(filenames, expected);
    EXPECT_EQ(removed, expected_removed);
  }

  {
    // different filepath when JoinPathRespectAbsolute
    std::set<std::string> filenames{
        file::JoinPath(kRootDir, "foo", "baz", "..", "bar.cc"),
        file::JoinPath(kRootDir, "foo", "bar.cc")};
    std::vector<std::string> removed;
    RemoveDuplicateFiles("", &filenames, &removed);

    std::set<std::string> expected{
        file::JoinPath(kRootDir, "foo", "baz", "..", "bar.cc"),
        file::JoinPath(kRootDir, "foo", "bar.cc")};
    EXPECT_EQ(filenames, expected);
    EXPECT_TRUE(removed.empty());
  }

  {
    // different filepath when JoinPathRespectAbsolute
    std::set<std::string> filenames{file::JoinPath("baz", "..", "bar.cc"),
                                    file::JoinPath(kRootDir, "foo", "bar.cc")};
    std::vector<std::string> removed;
    RemoveDuplicateFiles(file::JoinPath(kRootDir, "foo"), &filenames,
                         &removed);

    std::set<std::string> expected{file::JoinPath("baz", "..", "bar.cc"),
                                   file::JoinPath(kRootDir, "foo", "bar.cc")};
    EXPECT_EQ(filenames, expected);
    EXPECT_TRUE(removed.empty());
  }

  {
    // different filepath when JoinPathRespectAbsolute
    std::set<std::string> filenames{file::JoinPath("..", "bar.cc"),
                                    file::JoinPath(kRootDir, "foo", "bar.cc")};
    std::vector<std::string> removed;
    RemoveDuplicateFiles(file::JoinPath(kRootDir, "foo", "baz"), &filenames,
                         &removed);

    std::set<std::string> expected{file::JoinPath("..", "bar.cc"),
                                   file::JoinPath(kRootDir, "foo", "bar.cc")};
    EXPECT_EQ(filenames, expected);
    EXPECT_TRUE(removed.empty());
  }

  {
    // We want to test path X and path Y where
    // 1. X < Y (alphabetical order)
    // 2. JoinPathRespectAbsolute(cwd, X) == JoinPathRespectAbsolutePath(cwd, Y)
    // 3. len(X) > len(Y).
    //
    // Due to (1) and (2), If X is relative path, Y should be absolute path
    // or vice versa. Since we don't resolve path and due to (3),
    // Y must be relative path, and X must be absolute path.
    //
    // So, relative path should start with a character which is larger than
    // 'C' (on Win) or '/' (on non Win).
    std::set<std::string> filenames{
        file::JoinPath(kRootDir, "a", "a", "bar.cc"),
        file::JoinPath("a", "bar.cc"),
    };
    ASSERT_EQ(file::JoinPath(kRootDir, "a", "a", "bar.cc"), *filenames.begin());
    std::vector<std::string> removed;
    RemoveDuplicateFiles(file::JoinPath(kRootDir, "a"), &filenames, &removed);

    std::set<std::string> expected{
        file::JoinPath("a", "bar.cc"),
    };
    std::vector<std::string> expected_removed{
        file::JoinPath(kRootDir, "a", "a", "bar.cc"),
    };
    EXPECT_EQ(filenames, expected);
    EXPECT_EQ(removed, expected_removed);
  }
}

}  // namespace devtools_goma
