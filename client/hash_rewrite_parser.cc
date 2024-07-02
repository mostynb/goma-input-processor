// Copyright 2015 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "hash_rewrite_parser.h"

#include <utility>

#include "absl/strings/str_split.h"
#include "glog/logging.h"

namespace {

bool IsSha256Hexadecimal(const std::string& str) {
  static const size_t kSha256Len = 256 / 8 * 2;
  if (str.length() != kSha256Len) {
    LOG(WARNING) << "wrong length:" << str;
    return false;
  }
  if (str.find_first_not_of("0123456789abcdef") != std::string::npos) {
    LOG(WARNING) << "wrong char:" << str;
    return false;
  }
  return true;
}

}  // namespace

namespace devtools_goma {

bool ParseRewriteRule(const std::string& contents,
                      std::map<std::string, std::string>* mapping) {
  for (auto&& line : absl::StrSplit(contents, '\n', absl::SkipEmpty())) {
    size_t pos = line.find(":");
    if (pos == std::string::npos) {
      LOG(WARNING) << "wrong rule file.";
      return false;
    }
    std::string key(line.substr(0, pos));
    if (!IsSha256Hexadecimal(key)) {
      LOG(WARNING) << "The key seems not SHA256 hexadecimal."
                   << " key=" << key;
      return false;
    }
    std::string value(line.substr(pos + 1));
    if (!IsSha256Hexadecimal(value)) {
      LOG(WARNING) << "The value seems not SHA256 hexadecimal."
                   << " value=" << value;
      return false;
    }
    if (!mapping->insert(std::make_pair(key, value)).second) {
      LOG(WARNING) << "found the same key twice."
                   << " key=" << key;
      return false;
    }
  }
  return true;
}

}  // namespace devtools_goma
