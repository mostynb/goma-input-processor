// Copyright 2016 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "lib/execreq_normalizer.h"

#include <map>
#include <string>
#include <vector>

#include "base/path.h"
#include "gtest/gtest.h"

namespace devtools_goma {

TEST(ExecReqNormalizerTest, RewritePathWithDebugPrefixMap) {
  const std::map<std::string, std::string> empty_map;
  const std::map<std::string, std::string> single_rule_map = {
      {"/usr/local", "/debug"},
  };
  const std::map<std::string, std::string> value_shows_up_in_key_map = {
      {"/usr/local", "/foo"},
      {"/foo", "/bar"},
  };

  std::string path;
  path = "";
  EXPECT_FALSE(
      devtools_goma::RewritePathWithDebugPrefixMap(
          single_rule_map, &path));

  path = "/tmp";
  EXPECT_FALSE(
      devtools_goma::RewritePathWithDebugPrefixMap(
          empty_map, &path));

  path = "/usr/local/include/stdio.h";
  EXPECT_TRUE(
      devtools_goma::RewritePathWithDebugPrefixMap(
          single_rule_map, &path));
  EXPECT_EQ(file::JoinPath("/debug", "/include/stdio.h"), path);

  path = "/usr/local/include/stdio.h";
  EXPECT_TRUE(
      devtools_goma::RewritePathWithDebugPrefixMap(
          value_shows_up_in_key_map, &path));
  EXPECT_EQ(file::JoinPath("/foo", "include/stdio.h"), path);

  path = "/foo/local/include/stdio.h";
  EXPECT_TRUE(
      devtools_goma::RewritePathWithDebugPrefixMap(
          value_shows_up_in_key_map, &path));
  EXPECT_EQ(file::JoinPath("/bar", "local/include/stdio.h"), path);
}

TEST(ExecReqNormalizerTest, HasAmbiguityInDebugPrefixMap) {
  EXPECT_FALSE(devtools_goma::HasAmbiguityInDebugPrefixMap(
      std::map<std::string, std::string>()));
  EXPECT_FALSE(devtools_goma::HasAmbiguityInDebugPrefixMap(
      std::map<std::string, std::string>({
          {"/usr/local", "/debug"},
      })));
  EXPECT_TRUE(devtools_goma::HasAmbiguityInDebugPrefixMap(
      std::map<std::string, std::string>({
          {"/usr/local", "/debug"},
          {"/usr", "/debug2"},
      })));
  EXPECT_TRUE(devtools_goma::HasAmbiguityInDebugPrefixMap(
      std::map<std::string, std::string>({
          {"/usr/lib", "/debug"},
          {"/usr/libexec", "/debug2"},
      })));
  EXPECT_FALSE(devtools_goma::HasAmbiguityInDebugPrefixMap(
      std::map<std::string, std::string>({
          {"/usr/lib", "/debug"},
          {"/usr//libexec", "/debug2"},
      })));
  EXPECT_TRUE(devtools_goma::HasAmbiguityInDebugPrefixMap(
      std::map<std::string, std::string>({
          {"/usr/local", "/debug"},
          {"dummy", "dummy2"},
          {"/usr", "/debug2"},
      })));
  EXPECT_TRUE(devtools_goma::HasAmbiguityInDebugPrefixMap(
      std::map<std::string, std::string>({
          {"lib", "/debug"},
          {"dummy", "dummy2"},
          {"lib64", "/debug2"},
      })));
  EXPECT_FALSE(devtools_goma::HasAmbiguityInDebugPrefixMap(
      std::map<std::string, std::string>({
          {"/home/alice/chromium/src", "."},
      })));
}

}  // namespace devtools_goma
