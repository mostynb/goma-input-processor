// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/clang_flags_helper.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace devtools_goma {

TEST(ClangFlagsHelperTest, FDebugCompilationDir) {
  std::vector<std::string> args = {"clang", "-Xclang",
                                   "-fdebug-compilation-dir", "-Xclang", "."};
  ClangFlagsHelper flag(args);
  ASSERT_TRUE(flag.fdebug_compilation_dir().has_value());
  EXPECT_EQ(*flag.fdebug_compilation_dir(), ".");
}

TEST(ClangFlagsHelperTest, FCoverageCompilationDir) {
  std::vector<std::string> args = {"clang", "-Xclang",
                                   "-fcoverage-compilation-dir=."};
  ClangFlagsHelper flag(args);
  ASSERT_TRUE(flag.fcoverage_compilation_dir().has_value());
  EXPECT_EQ(*flag.fcoverage_compilation_dir(), ".");
}

}  // namespace devtools_goma
