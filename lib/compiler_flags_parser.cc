// Copyright 2018 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/compiler_flags_parser.h"

#include "glog/logging.h"
#include "glog/stl_logging.h"
#include "lib/compiler_flag_type_specific.h"

namespace devtools_goma {

// static
std::unique_ptr<CompilerFlags> CompilerFlagsParser::New(
    const std::vector<std::string>& args,
    const std::string& cwd) {
  if (args.empty()) {
    LOG(ERROR) << "Empty args";
    return nullptr;
  }

  return CompilerFlagTypeSpecific::FromArg(args[0]).NewCompilerFlags(args, cwd);
}

std::unique_ptr<CompilerFlags> CompilerFlagsParser::MustNew(
    const std::vector<std::string>& args,
    const std::string& cwd) {
  std::unique_ptr<CompilerFlags> flags = New(args, cwd);
  LOG_IF(FATAL, !flags) << "unsupported command line:" << args;
  return flags;
}

}  // namespace devtools_goma
