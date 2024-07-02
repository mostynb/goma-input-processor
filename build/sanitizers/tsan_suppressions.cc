// Copyright 2017 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains the default suppressions for ThreadSanitizer.
// It should only be used under very limited circumstances such as suppressing
// a report caused by an interceptor call in a system-installed library.

#if defined(THREAD_SANITIZER)

// Please make sure the code below declares a single string variable
// kTSanDefaultSuppressions which contains TSan suppressions delimited by
// newlines.
char kTSanDefaultSuppressions[] =
  "race:third_party/glog/src/vlog_is_on.cc\n"
  "race:third_party/glog/src/raw_logging.cc\n"
  // b/124806470
  "race:third_party/abseil/src/absl/base/internal/spinlock.h\n"
  // goma_fetch sometimes show data race in buildbot, which we cannot reproduce
  // on our local machine (b/74301421).
  "race:PollEvents\n"
  // There is a race around VLOG (https://crbug.com/1294084#c9).
  "race:google::InitVLOG3__\n"
  // PLEASE READ ABOVE BEFORE ADDING NEW SUPPRESSIONS.
  // End of suppressions.
;  // Please keep this semicolon.

#endif  // THREAD_SANITIZER
