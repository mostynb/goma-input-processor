// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/execreq_normalizer.h"

#include "base/path.h"
#include "google/protobuf/text_format.h"
#include "google/protobuf/util/message_differencer.h"
#include "gtest/gtest.h"
#include "lib/compiler_flag_type_specific.h"
#include "lib/compiler_flags.h"
#include "lib/execreq_verifier.h"

using google::protobuf::TextFormat;
using google::protobuf::util::MessageDifferencer;

namespace devtools_goma {

namespace {

void NormalizeExecReqForCacheKey(
    int id,
    bool normalize_include_path,
    bool is_linking,
    const std::vector<std::string>& normalize_weak_relative_for_arg,
    const std::map<std::string, std::string>& debug_prefix_map,
    ExecReq* req) {
  CompilerFlagTypeSpecific::FromArg(req->command_spec().name())
      .NewExecReqNormalizer()
      ->NormalizeForCacheKey(id, normalize_include_path, is_linking,
                             normalize_weak_relative_for_arg, debug_prefix_map,
                             req);
}

}  // namespace

TEST(ClangTidyExecReqNormalizerTest, Normalize) {
  static const char kExecReq[] = R"(
command_spec {
  name: "clang-tidy"
  version: "4.2.1[clang version 5.0.0 (trunk 300839)]"
  target: "x86_64-unknown-linux-gnu"
}
arg: "clang-tidy"
arg: "-checks='*'"
arg: "test.cc"
cwd: "/home/goma/src"
env: "PWD=/home/goma/src"
Input {
  filename: "/home/goma/src/test.cc"
  hash_key: "152d72ea117deff2af0cf0ca3aaa46a20a5f0c0e4ccb8b6d559d507401ae81e9"
}
)";

  // Nothing will be normalized.
  static const char* const kExecReqExpected = kExecReq;

  const std::vector<std::string> kTestOptions{
      "Xclang", "B", "I", "gcc-toolchain", "-sysroot", "resource-dir"};

  ExecReq req, req_expected;
  ASSERT_TRUE(TextFormat::ParseFromString(kExecReq, &req));
  ASSERT_TRUE(VerifyExecReq(req));
  ASSERT_TRUE(TextFormat::ParseFromString(kExecReqExpected, &req_expected));
  ASSERT_TRUE(VerifyExecReq(req_expected));

  NormalizeExecReqForCacheKey(0, true, false, kTestOptions,
                              std::map<std::string, std::string>(), &req);

  MessageDifferencer differencer;
  std::string difference_reason;
  differencer.ReportDifferencesToString(&difference_reason);
  EXPECT_TRUE(differencer.Compare(req_expected, req)) << difference_reason;
}

}  // namespace devtools_goma
