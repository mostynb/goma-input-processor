// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/clang_tidy_flags.h"

#include "base/path.h"
#include "gtest/gtest.h"
#include "lib/compiler_flags_parser.h"

namespace devtools_goma {

class ClangTidyFlagsTest : public testing::Test {
};

TEST_F(ClangTidyFlagsTest, ClangTidyFlag) {
  const std::vector<std::string> args{
      "clang-tidy",
      "-analyze-temporary-drots",
      "-checks=*",
      "-config={}",
      "-dump-config",
      "-enable-check-profile",
      "-explain-config",
      "-export-fixes=ex.yaml",
      "-extra-arg=-std=c++11",
      "-extra-arg-before=-DFOO",
      "-fix",
      "-fix-errors",
      "-header-filter=*",
      "-line-filter=[]",
      "-list-checks",
      "-p=.",
      "-system-headers",
      "-warnings-as-errors=*",
      "foo.cc",
  };

  std::unique_ptr<CompilerFlags> flags(
      CompilerFlagsParser::MustNew(args, "/tmp"));
  EXPECT_EQ(args, flags->args());

  EXPECT_EQ(1U, flags->output_files().size());
  EXPECT_EQ("ex.yaml", flags->output_files()[0]);

  EXPECT_EQ(1U, flags->input_filenames().size());
  EXPECT_EQ(file::JoinPath("/tmp", "foo.cc"), flags->input_filenames()[0]);

  EXPECT_TRUE(flags->is_successful());
  EXPECT_EQ("", flags->fail_message());
  EXPECT_EQ("clang-tidy", flags->compiler_name());
  EXPECT_EQ(CompilerFlagType::ClangTidy, flags->type());
  EXPECT_EQ("/tmp", flags->cwd());

  const ClangTidyFlags& clang_tidy_flags =
      static_cast<const ClangTidyFlags&>(*flags);
  EXPECT_EQ(std::vector<std::string>{"-std=c++11"},
            clang_tidy_flags.extra_arg());
  EXPECT_EQ(std::vector<std::string>{"-DFOO"},
            clang_tidy_flags.extra_arg_before());
  EXPECT_FALSE(clang_tidy_flags.seen_hyphen_hyphen());
  EXPECT_EQ(std::vector<std::string>{},
            clang_tidy_flags.args_after_hyphen_hyphen());
}

TEST_F(ClangTidyFlagsTest, ClangTidyFlagWithClangArgs) {
  const std::vector<std::string> args{
      "clang-tidy",
      "-analyze-temporary-drots",
      "-checks=*",
      "-config={}",
      "-dump-config",
      "-enable-check-profile",
      "-explain-config",
      "-export-fixes=ex.yaml",
      "-extra-arg=-std=c++11",
      "-extra-arg-before=-DFOO",
      "-fix",
      "-fix-errors",
      "-header-filter=*",
      "-line-filter=[]",
      "-list-checks",
      "-p=.",
      "-system-headers",
      "-warnings-as-errors=*",
      "foo.cc",
      "--",
      "-DBAR",
  };

  std::unique_ptr<CompilerFlags> flags(
      CompilerFlagsParser::MustNew(args, "/tmp"));
  EXPECT_EQ(args, flags->args());

  EXPECT_EQ(1U, flags->output_files().size());
  EXPECT_EQ("ex.yaml", flags->output_files()[0]);

  EXPECT_EQ(1U, flags->input_filenames().size());
  EXPECT_EQ(file::JoinPath("/tmp", "foo.cc"), flags->input_filenames()[0]);

  EXPECT_TRUE(flags->is_successful());
  EXPECT_EQ("", flags->fail_message());
  EXPECT_EQ("clang-tidy", flags->compiler_name());
  EXPECT_EQ(CompilerFlagType::ClangTidy, flags->type());
  EXPECT_EQ("/tmp", flags->cwd());

  const ClangTidyFlags& clang_tidy_flags =
      static_cast<const ClangTidyFlags&>(*flags);
  EXPECT_EQ(std::vector<std::string>{"-std=c++11"},
            clang_tidy_flags.extra_arg());
  EXPECT_EQ(std::vector<std::string>{"-DFOO"},
            clang_tidy_flags.extra_arg_before());
  EXPECT_TRUE(clang_tidy_flags.seen_hyphen_hyphen());
  EXPECT_EQ(std::vector<std::string>{"-DBAR"},
            clang_tidy_flags.args_after_hyphen_hyphen());
}

TEST_F(ClangTidyFlagsTest, ClangTidyFlagWithClangArgsEndingWithHyphenHyphen) {
  const std::vector<std::string> args{
      "clang-tidy",
      "-analyze-temporary-drots",
      "-checks=*",
      "-config={}",
      "-dump-config",
      "-enable-check-profile",
      "-explain-config",
      "-export-fixes=ex.yaml",
      "-extra-arg=-std=c++11",
      "-extra-arg-before=-DFOO",
      "-fix",
      "-fix-errors",
      "-header-filter=*",
      "-line-filter=[]",
      "-list-checks",
      "-p=.",
      "-system-headers",
      "-warnings-as-errors=*",
      "foo.cc",
      "--",
  };

  std::unique_ptr<CompilerFlags> flags(
      CompilerFlagsParser::MustNew(args, "/tmp"));
  EXPECT_EQ(args, flags->args());

  EXPECT_EQ(1U, flags->output_files().size());
  EXPECT_EQ("ex.yaml", flags->output_files()[0]);

  EXPECT_EQ(1U, flags->input_filenames().size());
  EXPECT_EQ(file::JoinPath("/tmp", "foo.cc"), flags->input_filenames()[0]);

  EXPECT_TRUE(flags->is_successful());
  EXPECT_EQ("", flags->fail_message());
  EXPECT_EQ("clang-tidy", flags->compiler_name());
  EXPECT_EQ(CompilerFlagType::ClangTidy, flags->type());
  EXPECT_EQ("/tmp", flags->cwd());

  const ClangTidyFlags& clang_tidy_flags =
      static_cast<const ClangTidyFlags&>(*flags);
  EXPECT_EQ(std::vector<std::string>{"-std=c++11"},
            clang_tidy_flags.extra_arg());
  EXPECT_EQ(std::vector<std::string>{"-DFOO"},
            clang_tidy_flags.extra_arg_before());
  EXPECT_TRUE(clang_tidy_flags.seen_hyphen_hyphen());
  EXPECT_TRUE(clang_tidy_flags.args_after_hyphen_hyphen().empty());
}

}  // namespace devtools_goma
