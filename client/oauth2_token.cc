// Copyright 2015 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "oauth2_token.h"

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "autolock_timer.h"
#include "callback.h"
#include "compiler_specific.h"
#include "file_helper.h"
#include "glog/logging.h"
#include "glog/stl_logging.h"
#include "http.h"
#include "json/json.h"
#include "json_util.h"
#include "jwt.h"
#include "scoped_fd.h"
#include "socket_factory.h"
#include "util.h"

namespace devtools_goma {

namespace {

const char kGCERefreshToken[] = "gce-metadata-service-account";
const char kServiceAccountRefreshTokenPrefix[] =
    "google-cloud-service-account:";
const char kgRPCType[] = "authorized_user";

// If something error happens during the refresh, the refresh task retries
// refresh for this time period.
constexpr absl::Duration kRefreshTimeout = absl::Seconds(10);
// If something error happens in the refresh of access token, the refresh task
// will not fetch access token again for this period.
constexpr absl::Duration kErrorRefreshPendingTimeout = absl::Seconds(60);
// If the access token is invalidated, the invalidation will not happen for this
// duration to avoid too frequent update of the access token.
constexpr absl::Duration kInvalidateTimeout = absl::Seconds(60);

class AuthRefreshConfig {
 public:
  virtual ~AuthRefreshConfig() {}
  virtual bool enabled() const = 0;
  virtual bool valid() const = 0;
  virtual bool GetOAuth2Config(OAuth2Config* config) const = 0;
  virtual bool CanRefresh() const = 0;
  virtual bool InitRequest(HttpRequest* req) const = 0;
  // TODO: use absl::string_view for resp_body instead?
  virtual bool ParseResponseBody(const std::string& resp_body,
                                 std::string* token_type,
                                 std::string* access_token,
                                 absl::Duration* expires_in) const = 0;
};

class GoogleOAuth2AccessTokenRefreshTask : public OAuth2AccessTokenRefreshTask {
 public:
  // Doesn't take ownership of wm.
  // Takes ownership of client and req.
  GoogleOAuth2AccessTokenRefreshTask(
      WorkerThreadManager* wm,
      std::unique_ptr<AuthRefreshConfig> config,
      std::unique_ptr<HttpClient> client,
      std::unique_ptr<HttpRequest> req)
      : wm_(wm),
        config_(std::move(config)),
        client_(std::move(client)),
        req_(std::move(req)) {
    LOG_IF(WARNING, !config_->enabled() || !config_->valid())
        << "config enabled=" << config_->enabled()
        << " valid=" << config_->valid();
  }

  ~GoogleOAuth2AccessTokenRefreshTask() override {
    CHECK(!cancel_refresh_now_);
    CHECK(!cancel_refresh_);
    CHECK(shutting_down_);
  }

  std::string GetAccount() override ABSL_LOCKS_EXCLUDED(mu_) {
    std::string access_token;
    {
      AUTOLOCK(lock, &mu_);
      // Keep showing the account email even after the access token is
      // invalidated.
      if (!account_email_.empty()) {
        return account_email_;
      }
      access_token = access_token_;
    }
    if (access_token.empty()) {
      return "";
    }

    HttpClient::Options options = client_->options();
    options.InitFromURL(kGoogleTokenInfoURI);
    HttpClient client(
        HttpClient::NewSocketFactoryFromOptions(options),
        HttpClient::NewTLSEngineFactoryFromOptions(options),
        options, wm_);

    HttpRequest req;
    std::ostringstream param;
    param << "?access_token=" << access_token;
    client.InitHttpRequest(&req, "GET", param.str());
    req.AddHeader("Connection", "close");

    HttpResponse resp;
    HttpClient::Status status;
    LOG(INFO) << "get tokeninfo for access_token";
    client.Do(&req, &resp, &status);
    if (status.err) {
      LOG(WARNING) << "tokeninfo err=" << status.err
                   << " " << status.err_message;
      return "";
    }
    if (status.http_return_code != 200) {
      LOG(WARNING) << "tokeninfo status=" << status.http_return_code;
      return "";
    }

    std::string email;
    {
      std::string err;
      Json::Reader reader;
      Json::Value root;
      if (reader.parse(resp.parsed_body(), root, false)) {
        if (!GetNonEmptyStringFromJson(root, "email", &email, &err)) {
          LOG(WARNING) << "parse tokeninfo: " << err;
        }
      } else {
        LOG(WARNING) << "invalid json";
      }
    }
    {
      AUTOLOCK(lock, &mu_);
      account_email_ = email;
    }
    return email;
  }

  bool GetOAuth2Config(OAuth2Config* config) const override {
    return config_->GetOAuth2Config(config);
  }

  std::string GetAuthorization() const override ABSL_LOCKS_EXCLUDED(mu_) {
    AUTOLOCK(lock, &mu_);
    if (absl::Now() < token_expiration_time_ && !token_type_.empty() &&
        !access_token_.empty()) {
      return token_type_ + " " + access_token_;
    }
    return "";
  }

  bool ShouldRefresh() const override ABSL_LOCKS_EXCLUDED(mu_) {
    const absl::Time now = absl::Now();
    AUTOLOCK(lock, &mu_);
    if (!config_->CanRefresh()) {
      return false;
    }
    if (last_network_error_time_ &&
        *last_network_error_time_ > absl::Time() &&
        now < *last_network_error_time_ + kErrorRefreshPendingTimeout) {
      LOG(WARNING)
          << "prohibit to refresh OAuth2 access token for certain duration."
          << " last_network_error=" << *last_network_error_time_
          << " pending=" << kErrorRefreshPendingTimeout;
      return false;
    }
    return now >= token_expiration_time_ || token_type_.empty() ||
           access_token_.empty();
  }

  void RunAfterRefresh(WorkerThread::ThreadId thread_id,
                       OneshotClosure* closure) override
      ABSL_LOCKS_EXCLUDED(mu_) {
    const absl::Time now = absl::Now();
    {
      AUTOLOCK(lock, &mu_);
      if (now < token_expiration_time_ || shutting_down_) {
        DCHECK(shutting_down_ || !access_token_.empty());
        // access token is valid or oauth2 not available, go ahead.
        wm_->RunClosureInThread(FROM_HERE,
                                thread_id, closure,
                                WorkerThread::PRIORITY_MED);
        return;
      }
      if (last_network_error_time_ &&
          *last_network_error_time_ > absl::Time() &&
          now < *last_network_error_time_ + kErrorRefreshPendingTimeout) {
        LOG(WARNING) << "will not refresh token."
                     << " last_network_error=" << *last_network_error_time_
                     << " pending=" << kErrorRefreshPendingTimeout;
        wm_->RunClosureInThread(FROM_HERE,
                                thread_id, closure,
                                WorkerThread::PRIORITY_MED);
        return;
      }
      // should refresh access token.
      pending_tasks_.push_back(std::make_pair(thread_id, closure));
      switch (state_) {
        case NOT_STARTED: // first run.
          state_ = RUN;
          refresh_deadline_ = now + kRefreshTimeout;
          refresh_backoff_duration_ = client_->options().min_retry_backoff;
          break;
        case RUN:
          return;
      }
      if (!has_set_thread_id_) {
        refresh_task_thread_id_ = wm_->GetCurrentThreadId();
        has_set_thread_id_ = true;
      }
      wm_->RunClosureInThread(
          FROM_HERE,
          refresh_task_thread_id_,
          NewCallback(
              this, &GoogleOAuth2AccessTokenRefreshTask::RunRefresh),
          WorkerThread::PRIORITY_IMMEDIATE);
    }
  }

  void Invalidate() override ABSL_LOCKS_EXCLUDED(mu_) {
    const absl::Time now = absl::Now();
    AUTOLOCK(lock, &mu_);
    if (access_token_.empty()) {
      LOG(WARNING) << "no token to invalidate.";
      return;
    }
    if (last_invalidated_time_ &&
        now < *last_invalidated_time_ + kInvalidateTimeout) {
      LOG(WARNING) << "will not invalidate token."
                   << " last_invalidated_time=" << *last_invalidated_time_;
      return;
    }
    access_token_.clear();
    token_expiration_time_ = absl::Time();
    last_invalidated_time_ = now;

    std::ostringstream ss;
    if (last_network_error_time_) {
      ss << " last_network_error_time=" << *last_network_error_time_;
    }
    LOG(INFO) << "access token is invalidated." << ss.str();

    DoCancel();
  }

  void Shutdown() override ABSL_LOCKS_EXCLUDED(mu_) {
    {
      AUTOLOCK(lock, &mu_);
      if (shutting_down_) {
        return;
      }
      shutting_down_ = true;
      DoCancel();
    }
    client_->Shutdown();
  }

  void Wait() override ABSL_LOCKS_EXCLUDED(mu_) {
    {
      AUTOLOCK(lock, &mu_);
      CHECK(shutting_down_) << "You must call Shutdown() beforehand.";
      LOG(INFO) << "Wait cancel_refresh_now=" << cancel_refresh_now_;
      LOG(INFO) << "Wait cancel_refresh_=" << cancel_refresh_;
      while (cancel_refresh_now_ != nullptr || cancel_refresh_ != nullptr) {
        cond_.Wait(&mu_);
      }
    }
    client_->WaitNoActive();
    client_.reset();
  }

 private:
  enum State {
    NOT_STARTED,
    RUN,
  };

  void InitRequest() {
    if (!config_->enabled()) {
      LOG(INFO) << "not enabled.";
      return;
    }
    if (!config_->InitRequest(req_.get())) {
      LOG(WARNING) << "failed to init request.";
    }
  }

  void ParseOAuth2AccessTokenUnlocked(absl::Duration* next_update_in)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    constexpr auto kOAuthExpireTimeMargin = absl::Seconds(60);
    if (status_->err != OK) {
      LOG(ERROR) << "HTTP communication failed to refresh OAuth2 access token."
                 << " err_message=" << status_->err_message;
      return;
    }
    absl::Duration expires_in;
    if (!config_->ParseResponseBody(resp_.parsed_body(), &token_type_,
                                    &access_token_, &expires_in)) {
      LOG(ERROR) << "Failed to parse OAuth2 access token:"
                 << resp_.parsed_body();
      token_type_.clear();
      access_token_.clear();
      account_email_.clear();
      return;
    }
    const absl::Time now = absl::Now();
    token_expiration_time_ = now + expires_in - kOAuthExpireTimeMargin;
    LOG(INFO) << "Got new OAuth2 access token."
              << " now=" << now << " expires_in=" << expires_in
              << " token_expiration_time=" << token_expiration_time_;
    VLOG(1) << "access_token=" << access_token_;
    // expires_in is usually large enough. e.g. 3600.
    // If it is small, auto update of access token will not work.
    *next_update_in = expires_in - kOAuthExpireTimeMargin * 2;
    LOG_IF(WARNING, *next_update_in <= absl::ZeroDuration())
        << "expires_in is too small.  auto update will not work."
        << " next_update_in=" << *next_update_in << " expires_in=" << expires_in
        << " kOAuthExpireTimeMargin=" << kOAuthExpireTimeMargin;
  }

  void Done() ABSL_LOCKS_EXCLUDED(mu_) {
    AUTOLOCK(lock, &mu_);
    DCHECK(THREAD_ID_IS_SELF(refresh_task_thread_id_));
    if (shutting_down_) {
      std::vector<std::pair<WorkerThread::ThreadId,
          OneshotClosure*>> callbacks;
      callbacks.swap(pending_tasks_);
      for (const auto& callback : callbacks) {
        wm_->RunClosureInThread(FROM_HERE,
                                callback.first, callback.second,
                                WorkerThread::PRIORITY_MED);
      }
      return;
    }
    bool http_ok = true;
    if (status_->err != OK &&
        (status_->http_return_code == 0 ||
         status_->http_return_code / 100 == 5)) {
      const absl::Time now = absl::Now();
      http_ok = false;
      {
        if (refresh_deadline_ && now < *refresh_deadline_) {
          LOG(WARNING) << "refresh failed http=" << status_->http_return_code
                       << " retry until deadline=" << *refresh_deadline_
                       << " refresh_backoff_duration_="
                       << refresh_backoff_duration_;

          refresh_backoff_duration_ =
              HttpClient::GetNextBackoff(
                  client_->options(), refresh_backoff_duration_, true);
          LOG(INFO) << "backoff"
                    << " refresh_backoff_duration="
                    << refresh_backoff_duration_;
          CHECK(cancel_refresh_ == nullptr)
              << "Somebody else seems to run refresh task and failing?";
          cancel_refresh_ = wm_->RunDelayedClosureInThread(
              FROM_HERE, wm_->GetCurrentThreadId(), refresh_backoff_duration_,
              NewCallback(this,
                          &GoogleOAuth2AccessTokenRefreshTask::RunRefresh));
          return;
        }
        LOG(WARNING) << "refresh failed http=" << status_->http_return_code
                     << " deadline_exceeded now=" << now
                     << " deadline=" << *refresh_deadline_;

        // If last_network_error_time_ is set, ShouldRefresh() starts returning
        // false to make task local fallback.  Let me make it postponed
        // until refresh attempts reaches refresh_deadline_.
        last_network_error_time_ = now;
      }
    }
    LOG_IF(ERROR, status_->err != OK)
        << "refresh failed."
        << " err=" << status_->err
        << " err_message=" << status_->err_message
        << " http=" << status_->http_return_code;
    VLOG(1) << "Get access token done.";
    std::vector<std::pair<WorkerThread::ThreadId,
                          OneshotClosure*>> callbacks;
    absl::Duration next_update_in;
    {
      DCHECK_EQ(state_, RUN);
      state_ = NOT_STARTED;
      refresh_deadline_.reset();
      ParseOAuth2AccessTokenUnlocked(&next_update_in);
      if (http_ok && !access_token_.empty()) {
        last_network_error_time_.reset();
        refresh_backoff_duration_ = absl::ZeroDuration();
      }
      callbacks.swap(pending_tasks_);
    }
    for (const auto& callback : callbacks) {
      wm_->RunClosureInThread(FROM_HERE,
                              callback.first, callback.second,
                              WorkerThread::PRIORITY_MED);
    }
    if (next_update_in > absl::ZeroDuration()) {
      {
        if (shutting_down_) {
          return;
        }
        if (cancel_refresh_now_) {
          // The other RunRefreshNow task seems to be running.
          // We will not add new delayed task.
          LOG(INFO) << "The other OAuth2 RunRefreshNow task has already been "
                    << "registred.  We will not override with newone.";
          return;
        }

        DCHECK(THREAD_ID_IS_SELF(refresh_task_thread_id_));
        cancel_refresh_now_ = wm_->RunDelayedClosureInThread(
            FROM_HERE, refresh_task_thread_id_, next_update_in,
            NewCallback(this,
                        &GoogleOAuth2AccessTokenRefreshTask::RunRefreshNow));
      }
      LOG(INFO) << "Registered the OAuth2 refresh task to be executed later."
                << " next_update_in=" << next_update_in;
    }
  }

  void RunRefreshUnlocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    DCHECK_EQ(RUN, state_);
    DCHECK(THREAD_ID_IS_SELF(refresh_task_thread_id_));
    InitRequest();
    // Make HttpClient get access token.
    LOG(INFO) << "Going to refresh OAuth2 access token.";
    resp_.Reset();
    status_ = absl::make_unique<HttpClient::Status>();
    status_->trace_id = "oauth2Refresh";
    client_->DoAsync(
        req_.get(), &resp_, status_.get(),
        NewCallback(
            this, &GoogleOAuth2AccessTokenRefreshTask::Done));
  }

  void RunRefresh() ABSL_LOCKS_EXCLUDED(mu_) {
    LOG(INFO) << "Run refresh.";

    AUTOLOCK(lock, &mu_);
    DCHECK(THREAD_ID_IS_SELF(refresh_task_thread_id_));

    // Set nullptr to make OAuth2AccessTokenRefreshTask::Cancel() know
    // it must not execute cancel_refresh_->Cancel().
    cancel_refresh_ = nullptr;
    cond_.Signal();
    if (shutting_down_) {
      return;
    }
    RunRefreshUnlocked();
  }

  // RunRefreshNow() is used for RunDelayedClosureInThread in Done() above.
  void RunRefreshNow() ABSL_LOCKS_EXCLUDED(mu_) {
    LOG(INFO) << "Run refresh now.";

    AUTOLOCK(lock, &mu_);
    DCHECK(THREAD_ID_IS_SELF(refresh_task_thread_id_));
    CHECK(cancel_refresh_now_)
        << "RunRefreshNow has been cancelled, but called?";
    // Set nullptr to make OAuth2AccessTokenRefreshTask::Cancel() know
    // it must not execute cancel_refresh_now_->Cancel().
    cancel_refresh_now_ = nullptr;
    cond_.Signal();
    if (shutting_down_) {
      return;
    }
    switch (state_) {
      case NOT_STARTED: // first run.
        state_ = RUN;
        refresh_deadline_ = absl::Now() + kRefreshTimeout;
        refresh_backoff_duration_ = client_->options().min_retry_backoff;
        break;
      case RUN:
        return;
    }
    RunRefreshUnlocked();
  }

  void DoCancel() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    if (cancelling_) {
      LOG(INFO) << "already cancelling...";
      return;
    }
    if (!cancel_refresh_now_ && !cancel_refresh_) {
      return;
    }
    if (THREAD_ID_IS_SELF(refresh_task_thread_id_)) {
      // in goma_fetch.cc, refresh_task_thread_id_ and current thread
      // is same, so call cancel in the same thread.
      // since Wait() is also called on the same thread, there would be
      // no chance to run Cancel on the thread and never get cond_
      // signalled.
      if (cancel_refresh_now_) {
        LOG(INFO) << "cancel now " << cancel_refresh_now_;
        cancel_refresh_now_->Cancel();
        cancel_refresh_now_ = nullptr;
      }
      if (cancel_refresh_) {
        LOG(INFO) << "cancel " << cancel_refresh_;
        cancel_refresh_->Cancel();
        cancel_refresh_ = nullptr;
      }
      cond_.Signal();
    } else {
      LOG(INFO) << "cancelling now... " << cancel_refresh_now_;
      LOG(INFO) << "cancelling... " << cancel_refresh_;
      cancelling_ = true;
      wm_->RunClosureInThread(
          FROM_HERE, refresh_task_thread_id_,
          NewCallback(this, &GoogleOAuth2AccessTokenRefreshTask::Cancel),
          WorkerThread::PRIORITY_IMMEDIATE);
    }
  }

  void Cancel() ABSL_LOCKS_EXCLUDED(mu_) {
    AUTOLOCK(lock, &mu_);
    DCHECK(THREAD_ID_IS_SELF(refresh_task_thread_id_));
    if (cancel_refresh_now_) {
      cancel_refresh_now_->Cancel();
      cancel_refresh_now_ = nullptr;
      cond_.Signal();
      LOG(INFO) << "cancelled";
    }
    if (cancel_refresh_) {
      cancel_refresh_->Cancel();
      cancel_refresh_ = nullptr;
      cond_.Signal();
      LOG(INFO) << "cancelled";
    }
    cancelling_ = false;
  }

  WorkerThreadManager* wm_;
  std::unique_ptr<AuthRefreshConfig> config_;
  std::unique_ptr<HttpClient> client_;
  std::unique_ptr<HttpRequest> req_;
  HttpResponse resp_;

  mutable Lock mu_;
  // signaled when cancel_refresh_now_ or cancel_refresh_ become nullptr.
  ConditionVariable cond_;
  std::unique_ptr<HttpClient::Status> status_ ABSL_GUARDED_BY(mu_);
  State state_ ABSL_GUARDED_BY(mu_) = NOT_STARTED;
  absl::optional<absl::Time> refresh_deadline_ ABSL_GUARDED_BY(mu_);
  std::string token_type_ ABSL_GUARDED_BY(mu_);
  std::string access_token_ ABSL_GUARDED_BY(mu_);
  std::string account_email_ ABSL_GUARDED_BY(mu_);
  absl::Time token_expiration_time_ ABSL_GUARDED_BY(mu_);
  absl::optional<absl::Time> last_network_error_time_ ABSL_GUARDED_BY(mu_);
  absl::optional<absl::Time> last_invalidated_time_ ABSL_GUARDED_BY(mu_);
  absl::Duration refresh_backoff_duration_ ABSL_GUARDED_BY(mu_);
  std::vector<std::pair<WorkerThread::ThreadId, OneshotClosure*>> pending_tasks_
      ABSL_GUARDED_BY(mu_);

  // This class cannot have an ownership of CancelableClosure.
  // It is valid until Cancel() is called or the closure is executed, and
  // cancel_refresh_now_ is used as a flag to represent the CancelableClosure
  // is valid (i.e. we can execute cancel_refresh_now_->Cancel()).
  //
  // cancel_refresh_now_ should set to nullptr when it become invalid.
  // cancel_refresh_ should also set to nullptr when it become invalid.
  WorkerThreadManager::CancelableClosure* cancel_refresh_now_
      ABSL_GUARDED_BY(mu_) = nullptr;
  WorkerThreadManager::CancelableClosure* cancel_refresh_ ABSL_GUARDED_BY(mu_) =
      nullptr;
  bool cancelling_ ABSL_GUARDED_BY(mu_) = false;
  WorkerThread::ThreadId refresh_task_thread_id_ ABSL_GUARDED_BY(mu_);
  bool has_set_thread_id_ ABSL_GUARDED_BY(mu_) = false;
  bool shutting_down_ ABSL_GUARDED_BY(mu_) = false;

  DISALLOW_COPY_AND_ASSIGN(GoogleOAuth2AccessTokenRefreshTask);
};

class OAuth2RefreshConfig : public AuthRefreshConfig {
 public:
  OAuth2RefreshConfig(const OAuth2RefreshConfig&) = delete;
  OAuth2RefreshConfig& operator=(const OAuth2RefreshConfig&) = delete;

  bool enabled() const override {
    return config_.enabled();
  }

  bool valid() const override {
    return config_.valid();
  }

  bool GetOAuth2Config(OAuth2Config* config) const override {
    if (!config_.enabled() && config_.refresh_token != kGCERefreshToken) {
      return false;
    }
    *config = config_;
    return true;
  }

  bool CanRefresh() const override {
    // if refresh token is not given, couldn't get access token and
    // no need to refresh.
    // go with logout state (i.e. no Authorization header).
    return !config_.refresh_token.empty();
  }

  bool ParseResponseBody(const std::string& resp_body,
                         std::string* token_type,
                         std::string* access_token,
                         absl::Duration* expires_in) const override {
    return ParseOAuth2AccessToken(
        resp_body, token_type, access_token, expires_in);
  }

 protected:
  explicit OAuth2RefreshConfig(OAuth2Config config)
      : config_(std::move(config)) {}

  OAuth2Config config_;
};

class GCEServiceAccountRefreshConfig : public OAuth2RefreshConfig {
 public:
  GCEServiceAccountRefreshConfig(const GCEServiceAccountRefreshConfig&)
      = delete;
  GCEServiceAccountRefreshConfig&
      operator=(const GCEServiceAccountRefreshConfig&) = delete;

  static std::unique_ptr<OAuth2AccessTokenRefreshTask> New(
      WorkerThreadManager* wm, const HttpClient::Options& http_options) {
    HttpClient::Options options = http_options;
    options.ClearAuthConfig();
    options.allow_throttle = false;

    LOG(INFO) << "gce service account:"
              << http_options.gce_service_account;
    // https://cloud.google.com/compute/docs/authentication#applications
    // https://pkg.go.dev/cloud.google.com/go/compute/metadata#Client.Get
    std::ostringstream url;
    url << "http://";
    absl::optional<std::string> metadata_host = GetEnv("GCE_METADATA_HOST");
    if (metadata_host) {
      url << *metadata_host;
    } else {
      url << "metadata";
    }
    const char kMetadataPath[] =
        "/computeMetadata/v1/instance/service-accounts/";
    url << kMetadataPath << http_options.gce_service_account << "/token";
    options.InitFromURL(url.str());
    std::unique_ptr<HttpClient> client(new HttpClient(
        HttpClient::NewSocketFactoryFromOptions(options),
        HttpClient::NewTLSEngineFactoryFromOptions(options),
        options, wm));

    // HTTP setup.
    std::unique_ptr<HttpRequest> req(new HttpRequest);
    client->InitHttpRequest(req.get(), "GET", "");
    req->AddHeader("Connection", "close");
    req->AddHeader("Metadata-Flavor", "Google");

    OAuth2Config config = http_options.oauth2_config;
    config.auth_uri = kGoogleAuthURI;
    config.token_uri = kGoogleTokenURI;
    config.scope = "scope_is_configured_when_instance_created";
    config.client_id = "client_is_not_needed";
    config.client_secret = "client_secret_is_not_needed";
    config.refresh_token = kGCERefreshToken;

    std::unique_ptr<AuthRefreshConfig> refresh_config(
        new GCEServiceAccountRefreshConfig(config));

    return std::unique_ptr<OAuth2AccessTokenRefreshTask>(
        new GoogleOAuth2AccessTokenRefreshTask(
            wm, std::move(refresh_config), std::move(client), std::move(req)));
  }

  bool InitRequest(HttpRequest* req) const override {
    // on GCE, just get service account token from metadata server.
    LOG(INFO) << "init request:GCE service account";
    return true;
  }

 private:
  explicit GCEServiceAccountRefreshConfig(const OAuth2Config& config)
      : OAuth2RefreshConfig(config) {}
};

class ServiceAccountRefreshConfig : public OAuth2RefreshConfig {
 public:
  ServiceAccountRefreshConfig(const ServiceAccountRefreshConfig&) = delete;
  ServiceAccountRefreshConfig&
      operator=(const ServiceAccountRefreshConfig&) = delete;

  static std::unique_ptr<OAuth2AccessTokenRefreshTask> New(
      WorkerThreadManager* wm, const HttpClient::Options& http_options) {
    HttpClient::Options options = http_options;
    options.ClearAuthConfig();
    options.allow_throttle = false;

    LOG(INFO) << "service account:"
              << http_options.service_account_json_filename;
    // https://developers.google.com/identity/protocols/OAuth2ServiceAccount#authorizingrequests
    options.InitFromURL(kGoogleTokenAudienceURI);
    std::unique_ptr<HttpClient> client(new HttpClient(
        HttpClient::NewSocketFactoryFromOptions(options),
        HttpClient::NewTLSEngineFactoryFromOptions(options),
        options, wm));

    // HTTP setup.
    std::unique_ptr<HttpRequest> req(new HttpRequest);
    client->InitHttpRequest(req.get(), "POST", "");
    req->SetContentType("application/x-www-form-urlencoded");
    req->AddHeader("Connection", "close");
    OAuth2Config config = http_options.oauth2_config;
    config.auth_uri = kGoogleAuthURI;
    config.token_uri = kGoogleTokenURI;
    if (config.scope == "") {
      config.scope = kGomaAuthScope;
    }
    config.client_id = "client_is_not_needed";
    config.client_secret = "client_secret_is_not_needed";
    config.refresh_token = kServiceAccountRefreshTokenPrefix +
        http_options.service_account_json_filename;
    LOG(INFO) << config.refresh_token;

    std::unique_ptr<AuthRefreshConfig> refresh_config(
        new ServiceAccountRefreshConfig(config));

    return std::unique_ptr<OAuth2AccessTokenRefreshTask>(
        new GoogleOAuth2AccessTokenRefreshTask(
            wm, std::move(refresh_config), std::move(client), std::move(req)));
  }

  bool InitRequest(HttpRequest* req) const override {
    const std::string& service_account_json_filename =
        config_.refresh_token.substr(strlen(kServiceAccountRefreshTokenPrefix));
    LOG(INFO) << service_account_json_filename;
    // service account.
    std::string saj;
    if (!ReadFileToString(service_account_json_filename, &saj)) {
      LOG(ERROR) << "Failed to read "
                 << service_account_json_filename;
      return false;
    }
    ServiceAccountConfig sa;
    if (!ParseServiceAccountJson(saj, &sa)) {
      LOG(ERROR) << "Failed to parse service account json in "
                 << service_account_json_filename;
      return false;
    }
    std::unique_ptr<JsonWebToken::Key> key(JsonWebToken::LoadKey(
        sa.private_key));
    if (key == nullptr) {
      LOG(ERROR) << "Invalid private key in "
                 << service_account_json_filename;
      return false;
    }
    LOG(INFO) << "service account:"
              << sa.client_email
              << " client_id=" << sa.client_id
              << " project_id=" << sa.project_id
              << " private_key_id=" << sa.private_key_id;
    JsonWebToken::ClaimSet cs;
    cs.iss = sa.client_email;
    cs.scopes.emplace_back(kGomaAuthScope);
    if (config_.scope != "" && config_.scope != kGomaAuthScope) {
      LOG(INFO) << "additional scope:" << config_.scope;
      cs.scopes.emplace_back(config_.scope);
    }
    JsonWebToken jwt(cs);
    std::string assertion = jwt.Token(*key);
    const std::string req_body =
        absl::StrCat("grant_type=", JsonWebToken::kGrantTypeEncoded,
                     "&assertion=", assertion);
    VLOG(1) << req_body;
    req->SetBody(req_body);
    return true;
  }

 private:
  explicit ServiceAccountRefreshConfig(const OAuth2Config& config)
      : OAuth2RefreshConfig(config) {}
};

class RefreshTokenRefreshConfig : public OAuth2RefreshConfig {
 public:
  RefreshTokenRefreshConfig(const RefreshTokenRefreshConfig&) = delete;
  RefreshTokenRefreshConfig&
      operator=(const RefreshTokenRefreshConfig&) = delete;

  static std::unique_ptr<OAuth2AccessTokenRefreshTask> New(
      WorkerThreadManager* wm, const HttpClient::Options& http_options) {
    HttpClient::Options options = http_options;
    options.ClearAuthConfig();
    options.allow_throttle = false;

    LOG(INFO) << "oauth2 enabled";

    OAuth2Config config = http_options.oauth2_config;
    if (config.token_uri != kGoogleTokenURI) {
      LOG(ERROR) << "unsupported token_uri=" << config.token_uri;
      return nullptr;
    }
    options.InitFromURL(kGoogleTokenURI);
    std::unique_ptr<HttpClient> client(new HttpClient(
        HttpClient::NewSocketFactoryFromOptions(options),
        HttpClient::NewTLSEngineFactoryFromOptions(options),
        options, wm));

    // HTTP setup.
    std::unique_ptr<HttpRequest> req(new HttpRequest);
    client->InitHttpRequest(req.get(), "POST", "");
    req->SetContentType("application/x-www-form-urlencoded");
    req->AddHeader("Connection", "close");
    config.type = kgRPCType;

    std::unique_ptr<AuthRefreshConfig> refresh_config(
        new RefreshTokenRefreshConfig(config));

    return std::unique_ptr<OAuth2AccessTokenRefreshTask>(
        new GoogleOAuth2AccessTokenRefreshTask(
            wm, std::move(refresh_config), std::move(client), std::move(req)));
  }

  bool InitRequest(HttpRequest* req) const override {
    LOG(INFO) << "init request:refresh token";

    // TODO: reconstruct client if config_.token_uri has been changed?
    const std::string req_body = absl::StrCat(
        "client_id=", config_.client_id,
        "&client_secret=", config_.client_secret,
        "&refresh_token=", config_.refresh_token, "&grant_type=refresh_token");
    VLOG(1) << req_body;
    req->SetBody(req_body);
    return true;
  }

 private:
  explicit RefreshTokenRefreshConfig(const OAuth2Config& config)
      : OAuth2RefreshConfig(config) {}
};

class LuciAuthRefreshConfig : public AuthRefreshConfig {
 public:
  static std::unique_ptr<OAuth2AccessTokenRefreshTask> New(
      WorkerThreadManager* wm, const HttpClient::Options& http_options) {
    static const char kLuciLocalAuthServiceHost[] = "127.0.0.1";
    static const char kLuciLocalAuthServicePath[] =
        "/rpc/LuciLocalAuthService.GetOAuthToken";

    HttpClient::Options options = http_options;
    options.ClearAuthConfig();
    options.allow_throttle = false;

    const LuciContextAuth& local_auth = http_options.luci_context_auth;
    options.use_ssl = false;
    options.dest_host_name = kLuciLocalAuthServiceHost;
    options.dest_port = local_auth.rpc_port;
    options.url_path_prefix = kLuciLocalAuthServicePath;
    std::vector<std::string> scopes;
    scopes.emplace_back(kGomaAuthScope);
    const std::string& scope = http_options.oauth2_config.scope;
    if (scope != "" && scope != kGomaAuthScope) {
      scopes.emplace_back(scope);
    }

    LOG(INFO) << "LUCI_CONTEXT local_auth is used with account: "
              << local_auth.default_account_id
              << " scopes=" << scopes;

    std::unique_ptr<HttpClient> client(new HttpClient(
        HttpClient::NewSocketFactoryFromOptions(options),
        nullptr, options, wm));

    std::unique_ptr<HttpRequest> req(new HttpRequest);
    client->InitHttpRequest(req.get(), "POST", "");
    req->SetContentType("application/json");
    req->AddHeader("Connection", "close");

    std::unique_ptr<AuthRefreshConfig> refresh_config(
        new LuciAuthRefreshConfig(local_auth, std::move(scopes)));

    return std::unique_ptr<OAuth2AccessTokenRefreshTask>(
        new GoogleOAuth2AccessTokenRefreshTask(
            wm, std::move(refresh_config), std::move(client), std::move(req)));
  }

  bool enabled() const override {
    return true;
  }

  bool valid() const override {
    return local_auth_.enabled();
  }

  bool GetOAuth2Config(OAuth2Config* config) const override {
    LOG(WARNING) << "GetOAuth2Config won't work for LUCI_CONTEXT.";
    return false;
  }

  bool CanRefresh() const override {
    return valid();
  }

  bool InitRequest(HttpRequest* req) const override {
    LuciOAuthTokenRequest treq;
    std::copy(scopes_.begin(), scopes_.end(), std::back_inserter(treq.scopes));
    treq.secret = local_auth_.secret;
    treq.account_id = local_auth_.default_account_id;

    VLOG(1) << treq.ToString();
    req->SetBody(treq.ToString());
    return true;
  }

  bool ParseResponseBody(const std::string& resp_body,
                         std::string* token_type,
                         std::string* access_token,
                         absl::Duration* expires_in) const override {
    static const char kTokenType[] = "Bearer";
    LuciOAuthTokenResponse resp;
    if (!ParseLuciOAuthTokenResponse(resp_body, &resp)) {
      LOG(WARNING) << "Failed to parse luci auth token response."
                   << " body=" << resp_body;
      return false;
    }
    *token_type = kTokenType;
    *access_token = resp.access_token;
    *expires_in = absl::FromTimeT(resp.expiry) - absl::Now();
    return true;
  }

 private:
  LuciAuthRefreshConfig(LuciContextAuth local_auth,
                        std::vector<std::string> scopes)
      : local_auth_(std::move(local_auth)), scopes_(std::move(scopes)) {}

  LuciContextAuth local_auth_;
  const std::vector<std::string> scopes_;
};

}  // namespace

/* static */
std::unique_ptr<OAuth2AccessTokenRefreshTask>
OAuth2AccessTokenRefreshTask::New(
    WorkerThreadManager* wm,
    const HttpClient::Options& http_options) {
  if (!http_options.gce_service_account.empty()) {
    return GCEServiceAccountRefreshConfig::New(wm, http_options);
  }

  if (!http_options.service_account_json_filename.empty()) {
    return ServiceAccountRefreshConfig::New(wm, http_options);
  }

  if (http_options.oauth2_config.enabled()) {
    return RefreshTokenRefreshConfig::New(wm, http_options);
  }

  if (http_options.luci_context_auth.enabled()) {
    return LuciAuthRefreshConfig::New(wm, http_options);
  }

  return nullptr;
}

}  // namespace devtools_goma
