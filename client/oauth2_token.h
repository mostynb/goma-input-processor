// Copyright 2015 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVTOOLS_GOMA_CLIENT_OAUTH2_TOKEN_H_
#define DEVTOOLS_GOMA_CLIENT_OAUTH2_TOKEN_H_

#include <memory>
#include <string>

#include "http.h"
#include "oauth2.h"
#include "worker_thread.h"
#include "worker_thread_manager.h"

namespace devtools_goma {

class OneshotClosure;

class OAuth2AccessTokenRefreshTask {
 public:
  // Creates new OAuth2AccessTokenRefreshTask for http_options.
  // Caller should take ownership of returned object.
  static std::unique_ptr<OAuth2AccessTokenRefreshTask> New(
      WorkerThreadManager* em,
      const HttpClient::Options& http_options);

  OAuth2AccessTokenRefreshTask() {}
  virtual ~OAuth2AccessTokenRefreshTask() {}

  virtual std::string GetAccount() = 0;

  virtual bool GetOAuth2Config(OAuth2Config* config) const = 0;

  virtual std::string GetAuthorization() const = 0;
  virtual bool ShouldRefresh() const = 0;
  virtual void RunAfterRefresh(WorkerThread::ThreadId thread_id,
                               OneshotClosure* closure) = 0;
  virtual void Invalidate() = 0;
  virtual void Shutdown() = 0;
  virtual void Wait() = 0;
 private:
  DISALLOW_COPY_AND_ASSIGN(OAuth2AccessTokenRefreshTask);
};

}  // namespace devtools_goma

#endif  // DEVTOOLS_GOMA_CLIENT_OAUTH2_TOKEN_H_
