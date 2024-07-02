// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/compiler_flag_type_specific.h"

#include "absl/memory/memory.h"
#include "glog/logging.h"
#include "glog/stl_logging.h"
#include "lib/clang_tidy_execreq_normalizer.h"
#include "lib/clang_tidy_flags.h"
#include "lib/compiler_flags.h"
#include "lib/dart_analyzer_execreq_normalizer.h"
#include "lib/dart_analyzer_flags.h"
#include "lib/fake_execreq_normalizer.h"
#include "lib/fake_flags.h"
#include "lib/gcc_execreq_normalizer.h"
#include "lib/gcc_flags.h"
#include "lib/java_execreq_normalizer.h"
#include "lib/java_flags.h"
#include "lib/rustc_execreq_normalizer.h"
#include "lib/rustc_flags.h"
#include "lib/vc_execreq_normalizer.h"
#include "lib/vc_flags.h"

namespace devtools_goma {

namespace {

CompilerFlagType CompilerFlagTypeFromArg(absl::string_view arg) {
  if (GCCFlags::IsGCCCommand(arg)) {
    return CompilerFlagType::Gcc;
  }
  if (VCFlags::IsVCCommand(arg) || VCFlags::IsClangClCommand(arg)) {
    // clang-cl gets compatible options with cl.exe.
    // See Also: http://clang.llvm.org/docs/UsersManual.html#clang-cl
    return CompilerFlagType::Clexe;
  }
  if (JavacFlags::IsJavacCommand(arg)) {
    return CompilerFlagType::Javac;
  }
  if (JavaFlags::IsJavaCommand(arg)) {
    return CompilerFlagType::Java;
  }
  if (ClangTidyFlags::IsClangTidyCommand(arg)) {
    return CompilerFlagType::ClangTidy;
  }
  if (RustcFlags::IsRustcCommand(arg)) {
    return CompilerFlagType::Rustc;
  }
  if (DartAnalyzerFlags::IsDartAnalyzerCommand(arg)) {
    return CompilerFlagType::DartAnalyzer;
  }
  if (FakeFlags::IsFakeCommand(arg)) {
    return CompilerFlagType::Fake;
  }

  return CompilerFlagType::Unknown;
}

}  // namespace

// static
CompilerFlagTypeSpecific CompilerFlagTypeSpecific::FromArg(
    absl::string_view arg) {
  CompilerFlagType type = CompilerFlagTypeFromArg(arg);
  if (type == CompilerFlagType::Unknown) {
    LOG(WARNING) << "Unknown compiler type: arg=" << arg;
  }

  return CompilerFlagTypeSpecific(type);
}

std::unique_ptr<CompilerFlags> CompilerFlagTypeSpecific::NewCompilerFlags(
    const std::vector<std::string>& args,
    const std::string& cwd) const {
  switch (type_) {
    case CompilerFlagType::Gcc:
      return absl::make_unique<GCCFlags>(args, cwd);
    case CompilerFlagType::Clexe:
      return absl::make_unique<VCFlags>(args, cwd);
    case CompilerFlagType::ClangTidy:
      return absl::make_unique<ClangTidyFlags>(args, cwd);
    case CompilerFlagType::Javac:
      return absl::make_unique<JavacFlags>(args, cwd);
    case CompilerFlagType::Java:
      return absl::make_unique<JavaFlags>(args, cwd);
    case CompilerFlagType::Rustc:
      return absl::make_unique<RustcFlags>(args, cwd);
    case CompilerFlagType::DartAnalyzer:
      return absl::make_unique<DartAnalyzerFlags>(args, cwd);
    case CompilerFlagType::Fake:
      return absl::make_unique<FakeFlags>(args, cwd);
    case CompilerFlagType::Unknown:
    default:
      return nullptr;
  }
}

std::string CompilerFlagTypeSpecific::GetCompilerName(
    absl::string_view arg) const {
  switch (type_) {
    case CompilerFlagType::Gcc:
      return GCCFlags::GetCompilerName(arg);
    case CompilerFlagType::Clexe:
      return VCFlags::GetCompilerName(arg);
    case CompilerFlagType::ClangTidy:
      return ClangTidyFlags::GetCompilerName(arg);
    case CompilerFlagType::Javac:
      return JavacFlags::GetCompilerName(arg);
    case CompilerFlagType::Java:
      return JavaFlags::GetCompilerName(arg);
    case CompilerFlagType::Rustc:
      return RustcFlags::GetCompilerName(arg);
    case CompilerFlagType::DartAnalyzer:
      return DartAnalyzerFlags::GetCompilerName(arg);
    case CompilerFlagType::Fake:
      return FakeFlags::GetCompilerName(arg);
    case CompilerFlagType::Unknown:
    default:
      return "";
  }
}

std::unique_ptr<ExecReqNormalizer>
CompilerFlagTypeSpecific::NewExecReqNormalizer() const {
  switch (type_) {
    case CompilerFlagType::Gcc:
      return absl::make_unique<GCCExecReqNormalizer>();
    case CompilerFlagType::Clexe:
      return absl::make_unique<VCExecReqNormalizer>();
    case CompilerFlagType::ClangTidy:
      return absl::make_unique<ClangTidyExecReqNormalizer>();
    case CompilerFlagType::Javac:
      return absl::make_unique<JavacExecReqNormalizer>();
    case CompilerFlagType::Java:
      return absl::make_unique<JavaExecReqNormalizer>();
    case CompilerFlagType::Rustc:
      return absl::make_unique<RustcExecReqNormalizer>();
    case CompilerFlagType::DartAnalyzer:
      return absl::make_unique<DartAnalyzerExecReqNormalizer>();
    case CompilerFlagType::Fake:
      return absl::make_unique<FakeExecReqNormalizer>();
    case CompilerFlagType::Unknown:
    default:
      return absl::make_unique<AsIsExecReqNormalizer>();
  }
}

}  // namespace devtools_goma
