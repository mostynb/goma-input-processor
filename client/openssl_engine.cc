// Copyright 2012 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "openssl_engine.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

#include "absl/base/call_once.h"
#include "absl/base/macros.h"
#include "absl/container/flat_hash_map.h"
#include "absl/memory/memory.h"
#include "absl/strings/match.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "autolock_timer.h"
#include "callback.h"
#include "compiler_specific.h"
#include "file_dir.h"
#include "file_helper.h"
#include "glog/logging.h"
#include "glog/stl_logging.h"
MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "google/protobuf/io/zero_copy_stream.h"
MSVC_POP_WARNING()
#include "http.h"
#include "http_util.h"
#include "mypath.h"
#include "openssl/asn1.h"
#include "openssl/base.h"
#include "openssl/crypto.h"
#include "openssl/err.h"
#include "openssl/ssl.h"
#include "openssl/x509.h"
#include "openssl/x509v3.h"
#include "openssl_engine_helper.h"
#include "path.h"
#include "platform_thread.h"
#include "scoped_fd.h"
#include "socket_pool.h"

#ifndef OPENSSL_IS_BORINGSSL
#error "This code is written for BoringSSL"
#endif

namespace devtools_goma {

namespace {

// Prevent use of SSL on error for this period.
constexpr absl::Duration kErrorTimeout = absl::Seconds(60);

// Wait for this period if no more sockets are in the pool.
constexpr absl::Duration kWaitForThingsGetsBetter = absl::Seconds(1);

absl::once_flag g_openssl_init_once;

// TODO: use bssl::UniquePtr instead.
class ScopedBIOFree {
 public:
  inline void operator()(BIO *x) const { if (x) CHECK(BIO_free(x)); }
};

class ScopedX509Free {
 public:
  inline void operator()(X509 *x) const { if (x) X509_free(x); }
};

class ScopedX509StoreCtxFree {
 public:
  inline void operator()(X509_STORE_CTX *x) const {
    if (x) X509_STORE_CTX_free(x);
  }
};

class ScopedX509StoreFree {
 public:
  inline void operator()(X509_STORE *x) const { if (x) X509_STORE_free(x); }
};

template <typename T>
std::string GetHumanReadableInfo(T* data, int (*func)(BIO*, T*)) {
  std::unique_ptr<BIO, ScopedBIOFree> bio(BIO_new(BIO_s_mem()));
  func(bio.get(), data);
  char* x509_for_print;
  const int x509_for_print_len = BIO_get_mem_data(bio.get(), &x509_for_print);
  std::string ret(x509_for_print, x509_for_print_len);

  return ret;
}

std::string GetHumanReadableCert(X509* x509) {
  return GetHumanReadableInfo<X509>(x509, X509_print);
}

std::string GetHumanReadableCRL(X509_CRL* x509_crl) {
  return GetHumanReadableInfo<X509_CRL>(x509_crl, X509_CRL_print);
}

std::string GetHumanReadableCerts(STACK_OF(X509) * x509s) {
  std::string ret;
  for (size_t i = 0; i < sk_X509_num(x509s); i++) {
    ret.append(GetHumanReadableCert(sk_X509_value(x509s, i)));
  }
  return ret;
}

std::string GetHumanReadableSessionInfo(const SSL_SESSION* s) {
  std::ostringstream ss;
  ss << "SSL Session info:";
  ss << " protocol=" << SSL_SESSION_get_version(s);
  unsigned int len;
  const uint8_t* c = SSL_SESSION_get_id(s, &len);
  std::ostringstream sess_id;
  for (size_t i = 0; i < len; ++i) {
    sess_id << std::setfill('0') << std::setw(2)
            << std::hex << static_cast<int>(c[i]);
  }
  ss << " session_id=" << sess_id.str();
  ss << " time=" << SSL_SESSION_get_time(s);
  ss << " timeout=" << SSL_SESSION_get_timeout(s);
  return ss.str();
}

std::string GetHumanReadableSSLInfo(const SSL* ssl) {
  const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl);
  std::ostringstream ss;
  ss << "SSL info:";
  ss << " cipher:"
     << " name=" << SSL_CIPHER_get_name(cipher)
     << " bits=" << SSL_CIPHER_get_bits(cipher, nullptr)
     << " version=" << SSL_get_version(ssl);
  uint16_t curve_id = SSL_get_curve_id(ssl);
  if (curve_id != 0) {
    ss << " curve=" << SSL_get_curve_name(curve_id);
  }
  return ss.str();
}

// A class that controls lifetime of the SSL session.
class OpenSSLSessionCache {
 public:
  static void Init() {
    InitOpenSSLSessionCache();
  }

  // Set configs for the SSL session to the SSL context.
  static void Setup(SSL_CTX* ctx) {
    if (!cache_)
      InitOpenSSLSessionCache();

    DCHECK(cache_);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT);
    SSL_CTX_sess_set_new_cb(ctx, NewSessionCallBack);
    SSL_CTX_sess_set_remove_cb(ctx, RemoveSessionCallBack);
  }

  // Set a session to a SSL structure instance if we have a cache.
  static bool SetCachedSession(SSL_CTX* ctx, SSL* ssl) {
    DCHECK(cache_);
    return cache_->SetCachedSessionInternal(ctx, ssl);
  }

 private:
  OpenSSLSessionCache() {}
  ~OpenSSLSessionCache() {
    session_map_.clear();
  }

  static void InitOpenSSLSessionCache() {
    cache_ = new OpenSSLSessionCache();
    atexit(FinalizeOpenSSLSessionCache);
  }

  static void FinalizeOpenSSLSessionCache() {
    if (cache_)
      delete cache_;
    cache_ = nullptr;
  }

  static int NewSessionCallBack(SSL* ssl, SSL_SESSION* sess) {
    DCHECK(cache_);

    SSL_CTX* ctx = SSL_get_SSL_CTX(ssl);
    if (!cache_->RecordSessionInternal(ctx, sess)) {
      return 0;
    }
    return 1;
  }

  static void RemoveSessionCallBack(SSL_CTX* ctx, SSL_SESSION* sess) {
    DCHECK(cache_);

    LOG(INFO) << "Released stored SSL session."
              << " session_info=" << GetHumanReadableSessionInfo(sess);
    cache_->RemoveSessionInternal(ctx);
  }

  // To avoid race condition, you SHOULD call SSL_set_session while
  // |mu_| is held.  Or, you may cause use-after-free.
  //
  // The SSL_SESSION instance life time is controlled by reference counting.
  // SSL_set_session increase the reference count, and SSL_SESSION_free
  // or SSL_free SSL instance that has the session decrease the reference
  // count.  When session is revoked, SSL_SESSION instance is free'd via
  // RemoveSession.  At the same time, RemoveSession removes the instance
  // from internal session_map_.
  // If you do SSL_set_session outside of |mu_| lock, you may use the
  // SSL_SESSION instance already free'd.
  // Note that increasing reference count and decreasing reference count
  // are done under a lock held by BoringSSL, we do not need to lock for them.
  // That is why we use ReadWriteLock.
  // TODO: use mutex lock if it is much faster than shared lock.
  bool SetCachedSessionInternal(SSL_CTX* ctx, SSL* ssl)
      ABSL_LOCKS_EXCLUDED(mu_) {
    AUTO_SHARED_LOCK(lock, &mu_);
    SSL_SESSION* sess = GetInternalUnlocked(ctx);
    if (sess == nullptr) {
      return false;
    }

    VLOG(3) << "Reused session."
            << " ssl_ctx=" << ctx
            << " session_info=" << GetHumanReadableSessionInfo(sess);
    SSL_set_session(ssl, sess);
    return true;
  }

  // Returns true if the session is added or updated.
  bool RecordSessionInternal(SSL_CTX* ctx, SSL_SESSION* session)
      ABSL_LOCKS_EXCLUDED(mu_) {
    AUTO_EXCLUSIVE_LOCK(lock, &mu_);
    if (!session_map_
             .insert_or_assign(ctx, bssl::UniquePtr<SSL_SESSION>(session))
             .second) {
      LOG(INFO) << "Updated the session. ssl_ctx=" << ctx;
    }
    return true;
  }

  // Returns true if the session is removed.
  bool RemoveSessionInternal(SSL_CTX* ctx) ABSL_LOCKS_EXCLUDED(mu_) {
    AUTO_EXCLUSIVE_LOCK(lock, &mu_);
    return session_map_.erase(ctx) > 0;
  }

  SSL_SESSION* GetInternalUnlocked(SSL_CTX* ctx)
      ABSL_SHARED_LOCKS_REQUIRED(mu_) {
    auto found = session_map_.find(ctx);
    if (found != session_map_.end()) {
      return found->second.get();
    }
    return nullptr;
  }

  mutable ReadWriteLock mu_;
  // Won't take ownership of SSL_CTX*.
  absl::flat_hash_map<SSL_CTX*, bssl::UniquePtr<SSL_SESSION>> session_map_
      ABSL_GUARDED_BY(mu_);

  static OpenSSLSessionCache* cache_;

  DISALLOW_COPY_AND_ASSIGN(OpenSSLSessionCache);
};

/* static */
OpenSSLSessionCache* OpenSSLSessionCache::cache_ = nullptr;

// A class that controls socket_pool used in OpenSSL engine.
class OpenSSLSocketPoolCache {
 public:
  static void Init() {
    if (!cache_) {
      cache_.reset(new OpenSSLSocketPoolCache);
      atexit(FinalizeOpenSSLSocketPoolCache);
    }
  }

  static SocketPool* GetSocketPool(const std::string& host, int port) {
    DCHECK(cache_);
    return cache_->GetSocketPoolInternal(host, port);
  }

 private:
  OpenSSLSocketPoolCache() {}
  ~OpenSSLSocketPoolCache() = default;
  friend std::unique_ptr<OpenSSLSocketPoolCache>::deleter_type;

  static void FinalizeOpenSSLSocketPoolCache() { cache_.reset(); }

  SocketPool* GetSocketPoolInternal(const std::string& host, int port) {
    std::ostringstream ss;
    ss << host << ":" << port;
    const std::string key = ss.str();

    AUTOLOCK(lock, &socket_pool_mu_);
    auto p = socket_pools_.emplace(key, nullptr);
    if (p.second) {
      p.first->second = absl::make_unique<SocketPool>(host, port);
    }
    return p.first->second.get();
  }

  Lock socket_pool_mu_;
  absl::flat_hash_map<std::string, std::unique_ptr<SocketPool>> socket_pools_;

  static std::unique_ptr<OpenSSLSocketPoolCache> cache_;
  DISALLOW_COPY_AND_ASSIGN(OpenSSLSocketPoolCache);
};

/* static */
std::unique_ptr<OpenSSLSocketPoolCache> OpenSSLSocketPoolCache::cache_ =
    nullptr;

class OpenSSLCertificateStore {
 public:
  static void Init() {
    if (!store_) {
      store_ = new OpenSSLCertificateStore;
      store_->InitInternal();
      atexit(FinalizeOpenSSLCertificateStore);
    }
  }

  static bool AddCertificateFromFile(const std::string& filename) {
    DCHECK(store_);
    if (store_->IsKnownCertfileInternal(filename)) {
      LOG(INFO) << "Known cerficiate:" << filename;
      return false;
    }

    std::string user_cert;
    if (!ReadFileToString(filename.c_str(), &user_cert)) {
      LOG(ERROR) << "Failed to read:" << filename;
      return false;
    }
    return store_->AddCertificateFromStringInternal(filename, user_cert);
  }

  static bool AddCertificateFromString(const std::string& source,
                                       const std::string& cert) {
    DCHECK(store_);
    return store_->AddCertificateFromStringInternal(source, cert);
  }

  static void SetCertsToCTX(SSL_CTX* ctx) {
    DCHECK(store_);
    store_->SetCertsToCTXInternal(ctx);
  }

  static bool IsReady() {
    DCHECK(store_);
    return store_->IsReadyInternal();
  }

  static std::string GetTrustedCertificates() {
    DCHECK(store_);
    return store_->GetTrustedCertificatesInternal();
  }

 private:
  OpenSSLCertificateStore() {}
  ~OpenSSLCertificateStore() {}

  static void FinalizeOpenSSLCertificateStore() {
    delete store_;
    store_ = nullptr;
  }

  void InitInternal() {
    std::string root_certs;
    CHECK(GetTrustedRootCerts(&root_certs))
        << "Failed to read trusted root certificates from the system.";
    AddCertificateFromStringInternal("system", root_certs);
    LOG(INFO) << "Loaded root certificates.";
  }

  bool IsReadyInternal() const ABSL_LOCKS_EXCLUDED(mu_) {
    AUTO_SHARED_LOCK(lock, &mu_);
    return certs_.size() != 0;
  }

  // Note: you must not return the value via const reference.
  // trusted_certificates_ is a member of the class, which is protected
  // by the mutex (mu_).  It could be updated after return of the function
  // by another thread.
  std::string GetTrustedCertificatesInternal() const ABSL_LOCKS_EXCLUDED(mu_) {
    AUTO_SHARED_LOCK(lock, &mu_);
    return trusted_certificates_;
  }

  void SetCertsToCTXInternal(SSL_CTX* ctx) const ABSL_LOCKS_EXCLUDED(mu_) {
    AUTO_SHARED_LOCK(lock, &mu_);
    for (const auto& it : certs_) {
      LOG(INFO) << "setting certs from: " << it.first
                << " size=" << it.second->size();
      for (const auto& x509 : *it.second) {
        X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), x509.get());
      }
    }
  }

  bool IsKnownCertfileInternal(const std::string& filename) const
      ABSL_LOCKS_EXCLUDED(mu_) {
    AUTO_SHARED_LOCK(lock, &mu_);
    return certs_.find(filename) != certs_.end();
  }

  bool AddCertificateFromStringInternal(const std::string& source,
                                        const std::string& cert)
      ABSL_LOCKS_EXCLUDED(mu_) {
    // Create BIO instance to be used by PEM_read_bio_X509_AUX.
    std::unique_ptr<BIO, ScopedBIOFree> bio(
        BIO_new_mem_buf(cert.data(), cert.size()));

    AUTO_EXCLUSIVE_LOCK(lock, &mu_);
    auto it = certs_.insert(std::make_pair(source, nullptr));
    if (!it.second) {
      LOG(WARNING) << "cert store already has certificate for "
                   << source;
      return false;
    }
    it.first->second =
        absl::make_unique<std::vector<std::unique_ptr<X509, ScopedX509Free>>>();
    for (;;) {
      std::unique_ptr<X509, ScopedX509Free> x509(
          PEM_read_bio_X509_AUX(bio.get(), nullptr, nullptr, nullptr));
      if (x509.get() == nullptr)
        break;

      const std::string readable_cert = GetHumanReadableCert(x509.get());
      if (source == "system") {  // system certificate should be trivial.
        VLOG(2) << "Certificate loaded from " << source << ": "
                << readable_cert;
      } else {
        LOG(INFO) << "Certificate loaded from " << source << ": "
                  << readable_cert;
      }
      trusted_certificates_.append(readable_cert);
      it.first->second->emplace_back(std::move(x509));
    }
    if (ERR_GET_REASON(ERR_peek_last_error()) == PEM_R_NO_START_LINE)
      ERR_clear_error();
    else
      LOG(ERROR) << "Unexpected error occured during reading SSL certificate."
                 << " source:" << source;
    // TODO: log error with source info when no certificate found.
    LOG_IF(ERROR, it.first->second->size() == 0)
        << "No certificate found in " << source;
    return it.first->second->size() > 0;
  }

  mutable ReadWriteLock mu_;
  std::map<std::string,
           std::unique_ptr<std::vector<std::unique_ptr<X509, ScopedX509Free>>>>
      certs_ ABSL_GUARDED_BY(mu_);
  std::string trusted_certificates_ ABSL_GUARDED_BY(mu_);

  static OpenSSLCertificateStore* store_;
  DISALLOW_COPY_AND_ASSIGN(OpenSSLCertificateStore);
};

/* static */
OpenSSLCertificateStore* OpenSSLCertificateStore::store_ = nullptr;

class OpenSSLCRLCache {
 public:
  static void Init() {
    if (!cache_) {
      cache_ = new OpenSSLCRLCache;
      atexit(FinalizeOpenSSLCRLCache);
    }
  }

  // Caller owns returned X509_CRL*.
  // It is caller's responsibility to free it with X509_CRL_free.
  static ScopedX509CRL LookupCRL(const std::string& url) {
    DCHECK(cache_);
    return cache_->LookupCRLInternal(url);
  }

  // Returns true if url exists in internal database and successfully removed.
  // Otherwise, e.g. not registered, returns false.
  static bool DeleteCRL(const std::string& url) {
    DCHECK(cache_);
    return cache_->DeleteCRLInternal(url);
  }

  // Won't take ownership of |crl|.  This function duplicates it internally.
  static void SetCRL(const std::string& url, X509_CRL* crl) {
    DCHECK(cache_);
    return cache_->SetCRLInternal(url, crl);
  }

 private:
  OpenSSLCRLCache() {}
  ~OpenSSLCRLCache() {
    crls_.clear();
  }
  static void FinalizeOpenSSLCRLCache() {
    delete cache_;
    cache_ = nullptr;
  }

  // Note: caller should free X509_CRL.
  ScopedX509CRL LookupCRLInternal(const std::string& url)
      ABSL_LOCKS_EXCLUDED(mu_) {
    AUTO_SHARED_LOCK(lock, &mu_);
    const auto& it = crls_.find(url);
    if (it == crls_.end())
      return nullptr;
    return ScopedX509CRL(X509_CRL_dup(it->second.get()));
  }

  bool DeleteCRLInternalUnlocked(const std::string& url)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    const auto& it = crls_.find(url);
    if (it == crls_.end())
      return false;
    crls_.erase(it);
    return true;
  }

  bool DeleteCRLInternal(const std::string& url) ABSL_LOCKS_EXCLUDED(mu_) {
    AUTO_EXCLUSIVE_LOCK(lock, &mu_);
    return DeleteCRLInternalUnlocked(url);
  }

  void SetCRLInternal(const std::string& url, X509_CRL* crl)
      ABSL_LOCKS_EXCLUDED(mu_) {
    AUTO_EXCLUSIVE_LOCK(lock, &mu_);
    DeleteCRLInternalUnlocked(url);
    CHECK(crls_.insert(
            std::make_pair(url, ScopedX509CRL(X509_CRL_dup(crl)))).second)
        << "We already have the same URL in CRL store."
        << " url=" << url;
  }

  mutable ReadWriteLock mu_;
  std::map<std::string, ScopedX509CRL> crls_ ABSL_GUARDED_BY(mu_);

  static OpenSSLCRLCache* cache_;
  DISALLOW_COPY_AND_ASSIGN(OpenSSLCRLCache);
};

/* static */
OpenSSLCRLCache* OpenSSLCRLCache::cache_ = nullptr;

// Goma client also uses BoringSSL.
// Let's follow chromium's net/socket/ssl_client_socket_impl.cc.
// It uses BoringSSL default but avoid to select CBC ciphers.
const char* kCipherList = "ALL::!aPSK:!ECDSA+SHA1";
constexpr absl::Duration kCrlIoTimeout = absl::Seconds(1);
constexpr size_t kMaxDownloadCrlRetry = 5;  // times.

void InitOpenSSL() {
  CRYPTO_library_init();
  OpenSSLSessionCache::Init();
  OpenSSLSocketPoolCache::Init();
  OpenSSLCertificateStore::Init();
  OpenSSLCRLCache::Init();
  LOG(INFO) << "OpenSSL is initialized.";
}

int NormalizeChar(int input) {
  if (!isalnum(input)) {
    return '_';
  }
  return input;
}

// Converts non-alphanum in a filename to '_'.
std::string NormalizeToUseFilename(const std::string& input) {
  std::string out(input);
  std::transform(out.begin(), out.end(), out.begin(), NormalizeChar);
  return out;
}

ScopedX509CRL ParseCrl(const std::string& crl_str) {
  // See: http://www.openssl.org/docs/apps/crl.html
  if (crl_str.find("-----BEGIN X509 CRL-----") != std::string::npos) {  // PEM
    std::unique_ptr<BIO, ScopedBIOFree> bio(
        BIO_new_mem_buf(crl_str.data(), crl_str.size()));
    return ScopedX509CRL(
        PEM_read_bio_X509_CRL(bio.get(), nullptr, nullptr, nullptr));
  }
  // DER
  const unsigned char* p =
      reinterpret_cast<const unsigned char*>(crl_str.data());
  return ScopedX509CRL(d2i_X509_CRL(nullptr, &p, crl_str.size()));
}

// URL should be http (not https).
void DownloadCrl(
    ScopedSocket* sock,
    const HttpRequest& req,
    HttpResponse* resp) {
  resp->Reset();

  // Send request.
  if (!sock->valid()) {
    LOG(ERROR) << "connection failure:" << *sock;
    return;
  }

  std::unique_ptr<google::protobuf::io::ZeroCopyInputStream> request =
      req.NewStream();

  const void* data = nullptr;
  int size = 0;
  while (request->Next(&data, &size)) {
    absl::string_view buf(static_cast<const char*>(data), size);
    if (sock->WriteString(buf, kCrlIoTimeout) != OK) {
      LOG(ERROR) << "write failure:"
                 << " fd=" << *sock;
      return;
    }
  }

  for (;;) {
    char* buf;
    int buf_size;
    resp->NextBuffer(&buf, &buf_size);
    ssize_t len = sock->ReadWithTimeout(buf, buf_size, kCrlIoTimeout);
    if (len < 0) {
      LOG(ERROR) << "read failure:"
                 << " fd=" << *sock
                 << " len=" << len
                 << " resp has_header=" << resp->HasHeader()
                 << " resp status_code=" << resp->status_code()
                 << " resp total_recv_len=" << resp->total_recv_len();
      return;
    }
    if (resp->Recv(len)) {
      resp->Parse();
      return;
    }
  }
  // UNREACHABLE.
}

std::string GetCrlUrl(X509* x509) {
  int loc = X509_get_ext_by_NID(x509, NID_crl_distribution_points, -1);
  if (loc < 0)
    return "";
  X509_EXTENSION* ext = X509_get_ext(x509, loc);
  ASN1_OCTET_STRING* asn1_os = X509_EXTENSION_get_data(ext);
  const unsigned char* data = ASN1_STRING_data(asn1_os);
  const long data_len = ASN1_STRING_length(asn1_os);
  STACK_OF(DIST_POINT)* dps = d2i_CRL_DIST_POINTS(nullptr, &data, data_len);
  if (dps == nullptr) {
    LOG(ERROR) << "could not find distpoints in CRL.";
    return "";
  }
  std::string url;
  for (size_t i = 0; i < sk_DIST_POINT_num(dps) && url.empty(); i++) {
    DIST_POINT* dp = sk_DIST_POINT_value(dps, i);
    if (dp->distpoint && dp->distpoint->type == 0) {
      STACK_OF(GENERAL_NAME)* general_names = dp->distpoint->name.fullname;
      for (size_t j = 0; j < sk_GENERAL_NAME_num(general_names) && url.empty();
           j++) {
        GENERAL_NAME* general_name = sk_GENERAL_NAME_value(general_names, j);
        if (general_name->type == GEN_URI) {
          url.assign(reinterpret_cast<const char*>(general_name->d.ia5->data));
          if (url.find("http://") != 0) {
            LOG(INFO) << "Unsupported distribution point URI:" << url;
            url.clear();
            continue;
          }
        } else {
          LOG(INFO) << "Unsupported distribution point type:"
                    << general_name->type;
        }
      }
    }
  }
  sk_DIST_POINT_pop_free(dps, DIST_POINT_free);
  return url;
}

bool VerifyCrl(X509_CRL* crl, X509_STORE_CTX* store_ctx) {
  bool ok = true;
  STACK_OF(X509)* x509s = X509_STORE_get1_certs(store_ctx,
                                                X509_CRL_get_issuer(crl));
  for (size_t j = 0; j < sk_X509_num(x509s); j++) {
    EVP_PKEY *pkey;
    pkey = X509_get_pubkey(sk_X509_value(x509s, j));
    if (!X509_CRL_verify(crl, pkey)) {
      ok = false;
      break;
    }
  }
  sk_X509_pop_free(x509s, X509_free);
  return ok;
}

bool IsCrlExpired(const std::string& label,
                  X509_CRL* crl,
                  absl::optional<absl::Duration> crl_max_valid_duration) {
  // Is the CRL expired?
  if (!X509_CRL_get_nextUpdate(crl) ||
      X509_cmp_current_time(X509_CRL_get_nextUpdate(crl)) <= 0) {
    LOG(INFO) << "CRL is expired: label=" << label
              << " info=" << GetHumanReadableCRL(crl);
    return true;
  }

  // Does the CRL hit max valid duration set by the user?
  if (crl_max_valid_duration.has_value()) {
    ASN1_TIME* crl_last_update = X509_CRL_get_lastUpdate(crl);
    time_t t = time(nullptr) -
               absl::ToInt64Milliseconds(*crl_max_valid_duration);
    if (X509_cmp_time(crl_last_update, &t) < 0) {
      LOG(INFO) << "CRL is too old to use.  We need to refresh: "
                << " label=" << label
                << " crl_max_valid_duration_=" << *crl_max_valid_duration
                << " info=" << GetHumanReadableCRL(crl);
      return true;
    }
  }
  return false;
}

}  // anonymous namespace

//
// OpenSSLContext
//
void OpenSSLContext::Init(const std::string& hostname,
                          absl::optional<absl::Duration> crl_max_valid_duration,
                          OneshotClosure* invalidate_closure) {
  AUTOLOCK(lock, &mu_);
  // To keep room to support higher version, let's allow to understand all
  // TLS protocols here, and limit min supported version below.
  // Note: if TLSv1_method is used, it won't understand TLS 1.1 or TLS 1.2.
  // See: http://www.openssl.org/docs/ssl/SSL_CTX_new.html
  ctx_ = SSL_CTX_new(TLS_method());
  CHECK(ctx_);

  // Disable legacy protocols.
  SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
  // Allow BoringSSL to accept TLS 1.3.
  SSL_CTX_set_max_proto_version(ctx_, TLS1_3_VERSION);

  OpenSSLSessionCache::Setup(ctx_);

  SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
  CHECK(SSL_CTX_set_cipher_list(ctx_, kCipherList));
  // TODO: write more config to ctx_.

  OpenSSLCertificateStore::SetCertsToCTX(ctx_);
  certs_info_ = OpenSSLCertificateStore::GetTrustedCertificates();
  hostname_ = hostname;
  crl_max_valid_duration_ = crl_max_valid_duration;
  notify_invalidate_closure_ = invalidate_closure;
}

OpenSSLContext::OpenSSLContext() : is_crl_ready_(false), ref_cnt_(0) {
}

OpenSSLContext::~OpenSSLContext() {
  CHECK_EQ(ref_cnt_, 0UL);
  // The remove callback is called by SSL_CTX_free.
  // See: http://www.openssl.org/docs/ssl/SSL_CTX_sess_set_get_cb.html
  SSL_CTX_free(ctx_);

  // In case it's not called.
  if (notify_invalidate_closure_ != nullptr) {
    delete notify_invalidate_closure_;
  }
}

ScopedX509CRL OpenSSLContext::GetX509CrlsFromUrl(const std::string& url,
                                                 std::string* crl_str) {
  LOG(INFO) << "ctx:" << this << ": DownloadCrl:" << url;

  HttpClient::Options options;
  if (!proxy_host_.empty()) {
    options.proxy_host_name = proxy_host_;
    options.proxy_port = proxy_port_;
  }
  options.InitFromURL(url);

  HttpRequest req;
  req.Init("GET", "", options);
  req.AddHeader("Connection", "close");
  HttpResponse resp;
  resp.SetRequestPath(url);
  resp.SetTraceId("downloadCrl");

  SocketPool* socket_pool(OpenSSLSocketPoolCache::GetSocketPool(
      options.SocketHost(), options.SocketPort()));
  if (socket_pool == nullptr) {
    LOG(ERROR) << "ctx:" << this << ": Socket Pool is nullptr:"
               << " host=" << options.SocketHost()
               << " port=" << options.SocketPort();
    return nullptr;
  }

  for (size_t retry = 0;
       retry < std::max(kMaxDownloadCrlRetry, socket_pool->NumAddresses());
       ++retry) {
    ScopedSocket sock(socket_pool->NewSocket());
    if (!sock.valid()) {
      // We might have used up all candidate addresses in the pool.
      // It might be better to wait a while.
      LOG(WARNING) << "ctx:" << this
                   << ": It seems to fail to connect to all available "
                   << "addresses. Going to wait for a while."
                   << " kWaitForThingsGetsBetter=" << kWaitForThingsGetsBetter;
      absl::SleepFor(kWaitForThingsGetsBetter);
      continue;
    }
    DownloadCrl(&sock, req, &resp);
    if (resp.status_code() != 200) {
      LOG(WARNING) << "ctx:" << this << ": download CRL retrying:"
                   << " retry=" << retry
                   << " url=" << url
                   << " http=" << resp.status_code();
      socket_pool->CloseSocket(std::move(sock), true);
      continue;
    }
    crl_str->assign(resp.parsed_body());
    ScopedX509CRL x509_crl(ParseCrl(*crl_str));
    if (x509_crl == nullptr) {
      LOG(WARNING) << "ctx:" << this << ": failed to parse CRL data:"
                   << " url=" << url
                   << " contents length=" << crl_str->length()
                   << " resp header=" << resp.Header();
      socket_pool->CloseSocket(std::move(sock), true);
      continue;
    }
    // we requested "Connection: close", so close the socket, but no error.
    socket_pool->CloseSocket(std::move(sock), false);
    return x509_crl;
  }

  LOG(ERROR) << "ctx:" << this << ": failed to download CRL from " << url;
  return nullptr;
}

bool OpenSSLContext::SetupCrlsUnlocked(STACK_OF(X509)* x509s) {
  CHECK(!is_crl_ready_);
  crls_.clear();
  std::unique_ptr<X509_STORE, ScopedX509StoreFree> store(X509_STORE_new());
  std::unique_ptr<X509_STORE_CTX, ScopedX509StoreCtxFree>
      store_ctx(X509_STORE_CTX_new());
  X509_STORE_CTX_init(store_ctx.get(), store.get(), nullptr, x509s);
  const int num_x509s = sk_X509_num(x509s);
  for (int i = 0; i < num_x509s; i++) {
    X509* x509 = sk_X509_value(x509s, i);
    std::string url = GetCrlUrl(x509);
    if (url.empty())
      continue;
    ScopedX509CRL crl;
    std::string crl_str;

    // CRL is loaded in following steps:
    // 1. try memory cache.
    // 2. try disk cache.
    // 3. download from URL.

    // Read from memory cache.
    bool is_mem_cache_used = false;
    crl = OpenSSLCRLCache::LookupCRL(url);
    if (crl) {
      if (IsCrlExpired("memory", crl.get(), crl_max_valid_duration_)) {
        OpenSSLCRLCache::DeleteCRL(url);
        crl.reset();
      }
      // Is the CRL valid?
      if (crl.get() && !VerifyCrl(crl.get(), store_ctx.get())) {
        LOG(WARNING) << "ctx:" << this
                     << ": Failed to verify memory cached CRL."
                     << " url=" << url;
        OpenSSLCRLCache::DeleteCRL(url);
        crl.reset();
      }

      is_mem_cache_used = (crl.get() != nullptr);
    }

    // Read from disk cache.
    const std::string& cache_file = file::JoinPath(
        GetCacheDirectory(), "CRL-" + NormalizeToUseFilename(url));
    bool is_disk_cache_used = false;
    if (!is_mem_cache_used && ReadFileToString(cache_file.c_str(), &crl_str)) {
      crl = ParseCrl(crl_str);
      if (crl &&
          IsCrlExpired(cache_file, crl.get(), crl_max_valid_duration_)) {
        remove(cache_file.c_str());
        crl.reset();
      }

      // Is the CRL valid?
      if (crl.get() && !VerifyCrl(crl.get(), store_ctx.get())) {
        LOG(WARNING) << "ctx:" << this
                     << ": Failed to verify disk cached CRL: " << cache_file;
        remove(cache_file.c_str());
        crl.reset();
      }

      is_disk_cache_used = (crl.get() != nullptr);
    }

    // Download from URL.
    if (!is_mem_cache_used && !is_disk_cache_used) {
      crl = GetX509CrlsFromUrl(url, &crl_str);
      if (crl &&
          IsCrlExpired(url, crl.get(), crl_max_valid_duration_)) {
        crl.reset();
      }

      // Is the CRL valid?
      if (crl.get() && !VerifyCrl(crl.get(), store_ctx.get())) {
        LOG(WARNING) << "ctx:" << this << ": Failed to verify CRL: " << url;
        crl.reset();
      }
    }

    // Without CRL, TLS is not safe.
    if (!crl.get()) {
      std::ostringstream ss;
      ss << "CRL is not available";
      last_error_ = ss.str();
      last_error_time_ = absl::Now();
      ss << ":" << GetHumanReadableCert(x509);
      // This error may occurs if the network is broken, unstable,
      // or untrustable.
      // We believe that not running compiler_proxy is better than hiding
      // the strange situation. However, at the same time, sudden death is
      // usually difficult for users to understand what is bad.
      // Decision: die at start-time, won't die after that, but it seems
      // too late to die here.
      LOG(ERROR) << "ctx:" << this << ": " << ss.str();
      return false;
    }

    X509_STORE_add_crl(SSL_CTX_get_cert_store(ctx_), crl.get());
    certs_info_.append(GetHumanReadableCRL(crl.get()));
    if (!is_mem_cache_used && !is_disk_cache_used) {
      LOG(INFO) << "ctx:" << this
                << ": CRL loaded from: " << url;
      const std::string& cache_dir = std::string(file::Dirname(cache_file));
      if (!EnsureDirectory(cache_dir, 0700)) {
        LOG(WARNING) << "Failed to create cache dir: " << cache_dir;
      }
      if (WriteStringToFile(crl_str, cache_file)) {
        LOG(INFO) << "CRL is cached to: " << cache_file;
      } else {
        LOG(WARNING) << "Failed to write CRL cache to: " << cache_file;
      }
    }
    if (is_disk_cache_used) {
      LOG(INFO) << "ctx:" << this
                << ": Read CRL from cache:"
                << " url=" << url
                << " cache_file=" << cache_file;
    }
    if (is_mem_cache_used) {
      LOG(INFO) << "ctx:" << this
                << ": loaded CRL in memory: " << url;
    } else {
      OpenSSLCRLCache::SetCRL(url, crl.get());
      // If loaded from memory, we can assume we have already shown CRL info
      // to log, and we do not show it again.
      LOG(INFO) << "ctx:" << this << ": " << GetHumanReadableCRL(crl.get());
    }
    crls_.emplace_back(std::move(crl));
  }

  LOG_IF(WARNING, crls_.empty())
      << "ctx:" << this
      << ": A certificate should usually have its CRL."
      << " If we cannot not load any CRLs, something should be broken."
      << " certificates=" << GetHumanReadableCerts(x509s);

  if (!crls_.empty()) {
    VLOG(1) << "ctx:" << this
            << ": CRL is loaded.  We will check it during verification.";
    X509_VERIFY_PARAM *verify_param = X509_VERIFY_PARAM_new();
    X509_VERIFY_PARAM_set_flags(verify_param, X509_V_FLAG_CRL_CHECK);
    SSL_CTX_set1_param(ctx_, verify_param);
    X509_VERIFY_PARAM_free(verify_param);
    LOG(INFO) << "ctx:" << this
              << ": We may reject if the domain is not listed in loaded CRLs.";
  }

  is_crl_ready_ = true;
  return true;
}

bool OpenSSLContext::IsRevoked(STACK_OF(X509)* x509s) {
  AUTOLOCK(lock, &mu_);
  if (!last_error_.empty() && last_error_time_ &&
      absl::Now() < *last_error_time_ + kErrorTimeout) {
    LOG(ERROR) << "ctx:" << this
               << ": Preventing using SSL because of:" << last_error_
               << " last_error_time_=" << *last_error_time_;
    return true;
  }
  if (!is_crl_ready_ && !SetupCrlsUnlocked(x509s)) {
    LOG(ERROR) << "ctx:" << this
               << ": Failed to load CRLs:"
               << GetHumanReadableCerts(x509s);
    return true;
  }
  // Check CRLs.
  for (size_t i = 0; i < sk_X509_num(x509s); i++) {
    X509* x509 = sk_X509_value(x509s, i);
    for (size_t j = 0; j < crls_.size(); j++) {
      X509_REVOKED* rev;
      if (X509_CRL_get0_by_cert(crls_[j].get(), &rev, x509)) {
        LOG(ERROR) << "ctx:" << this
                   << ": Certificate is already revoked:"
                   << GetHumanReadableCert(x509);
        return true;
      }
    }
  }
  return false;
}

/* static */
bool OpenSSLContext::IsValidServerIdentity(X509* cert) {
  AUTOLOCK(lock, &mu_);
  uint8_t in4[4];
  if (inet_pton(AF_INET, hostname_.c_str(), &in4) == 1) {
    // hostname is IPv4 addr.
    int status = X509_check_ip(cert, in4, sizeof(in4),
                               X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT);
    if (status == 1) {
      LOG(INFO) << "ctx:" << this << ": Hostname matches with IPv4 address:"
                << " hostname=" << hostname_;
      return true;
    }
    LOG(INFO) << "ctx:" << this
              << ": Hostname(IPv4) didn't match with certificate:"
              << " status=" << status << " hostname=" << hostname_;
    return false;
  }

  uint8_t in6[16];
  if (inet_pton(AF_INET6, hostname_.c_str(), &in6) == 1) {
    // hostname is IPv6 addr.
    int status = X509_check_ip(cert, in6, sizeof(in6),
                               X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT);
    if (status == 1) {
      LOG(INFO) << "ctx:" << this
                << ": Hostname matches with IPv6 address:"
                << " hostname=" << hostname_;
      return true;
    }
    LOG(INFO) << "ctx:" << this
              << ": Hostname(IPv6) didn't match with certificate:"
              << " status=" << status << " hostname=" << hostname_;
    return false;
  }

  int status;
  char* peer;
  status = X509_check_host(cert, hostname_.c_str(), hostname_.size(),
                           X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT, &peer);
  if (status == 1) {
    LOG(INFO) << "ctx:" << this << ": Hostname matches:"
              << " hostname=" << hostname_ << " peer=" << peer;
    OPENSSL_free(peer);
    return true;
  } else if (status < 0) {  // i.e. error
    LOG(WARNING) << "ctx:" << this << ": Error on X509_check_host."
                 << " status=" << status << " hostname=" << hostname_;
  }

  LOG(ERROR) << "ctx:" << this
             << ": Hostname did not match with certificate:"
             << " hostname=" << hostname_;
  return false;
}

void OpenSSLContext::SetProxy(const std::string& proxy_host,
                              const int proxy_port) {
  proxy_host_.assign(proxy_host);
  proxy_port_ = proxy_port;
}

void OpenSSLContext::Invalidate() {
  OneshotClosure* c = nullptr;
  {
    AUTOLOCK(lock, &mu_);
    if (notify_invalidate_closure_) {
      c = notify_invalidate_closure_;
      notify_invalidate_closure_ = nullptr;
    }
  }

  if (c) {
    c->Run();
  }
}

SSL* OpenSSLContext::NewSSL() {
  SSL* ssl = SSL_new(ctx_);
  CHECK(ssl) << "Failed on SSL_new.";

  // TLS Server Name Indication (SNI).
  DCHECK(!hostname_.empty());
  CHECK(SSL_set_tlsext_host_name(ssl, hostname_.c_str()))
      << "TLS Server Name Indication (SNI) failed:" << hostname_;
  if (!OpenSSLSessionCache::SetCachedSession(ctx_, ssl)) {
    LOG(INFO) << "ctx:" << this
              << ": No session is cached. We need to start from handshake."
              << " key=" << ctx_
              << " hostname=" << hostname_;
  }

  ++ref_cnt_;
  return ssl;
}

void OpenSSLContext::DeleteSSL(SSL* ssl) {
  DCHECK(ssl);
  DCHECK_GT(ref_cnt_, 0UL);
  --ref_cnt_;
  SSL_free(ssl);
}

//
// TLS Engine
//
OpenSSLEngine::OpenSSLEngine()
    : ssl_(nullptr),
      network_bio_(nullptr),
      want_read_(false),
      want_write_(false),
      recycled_(false),
      need_self_verify_(false),
      state_(BEFORE_INIT) {}

OpenSSLEngine::~OpenSSLEngine() {
  if (ssl_ != nullptr) {
    // TODO: actually send shutdown to server not BIO.
    SSL_shutdown(ssl_);
    ctx_->DeleteSSL(ssl_);
  }
  if (network_bio_ != nullptr) {
    BIO_free_all(network_bio_);
  }
}

void OpenSSLEngine::Init(OpenSSLContext* ctx) {
  DCHECK(ctx);
  DCHECK(!ssl_);
  DCHECK_EQ(state_, BEFORE_INIT);

  // If IsCrlReady() comes after creating SSL*, ssl_ may not check CRLs
  // even if it should do.  Since loaded CRLs are cached in OpenSSLContext,
  // penalty to check it should be little.
  need_self_verify_ = !ctx->IsCrlReady();
  ssl_ = ctx->NewSSL();
  DCHECK(ssl_);

  // Since internal_bio is free'd by SSL_free, we do not need to keep this
  // separately.
  BIO* internal_bio;
  CHECK(BIO_new_bio_pair(&internal_bio, kNetworkBufSize, &network_bio_,
                         kNetworkBufSize))
      << "BIO_new_bio_pair failed.";
  // Both SSL_set0_rbio and SSL_set0_wbio take ownership.
  // |internal_bio|'s reference count become one after BIO_new_bio_pair,
  // and we need to make reference count +1 to set two owners
  // (i.e. rbio and wbio).
  CHECK(BIO_up_ref(internal_bio));
  SSL_set0_rbio(ssl_, internal_bio);
  SSL_set0_wbio(ssl_, internal_bio);

  ctx_ = ctx;
  Connect();  // Do not check anything since nothing has started here.
  state_ = IN_CONNECT;
}

bool OpenSSLEngine::IsIOPending() const {
  return (state_ == IN_CONNECT) || want_read_ || want_write_;
}

bool OpenSSLEngine::IsReady() const {
  return state_ == READY;
}

int OpenSSLEngine::GetDataToSendTransport(std::string* data) {
  DCHECK_NE(state_, BEFORE_INIT);
  size_t max_read = BIO_ctrl(network_bio_, BIO_CTRL_PENDING, 0, nullptr);
  if (max_read > 0) {
    data->resize(max_read);
    char* buf = &((*data)[0]);
    int read_bytes = BIO_read(network_bio_, buf, max_read);
    DCHECK_GT(read_bytes, 0);
    CHECK_EQ(static_cast<int>(max_read), read_bytes);
  }
  want_write_ = false;
  if (state_ == IN_CONNECT) {
    int status = Connect();
    if (status < 0 && status != TLSEngine::TLS_WANT_READ &&
        status != TLSEngine::TLS_WANT_WRITE)
      return TLSEngine::TLS_VERIFY_ERROR;
  }
  return max_read;
}

size_t OpenSSLEngine::GetBufSizeFromTransport() {
  return BIO_ctrl_get_write_guarantee(network_bio_);
}

int OpenSSLEngine::SetDataFromTransport(const absl::string_view& data) {
  DCHECK_NE(state_, BEFORE_INIT);
  size_t max_write = BIO_ctrl_get_write_guarantee(network_bio_);
  CHECK_LE(data.size(), max_write);
  int ret = BIO_write(network_bio_, data.data(), data.size());
  CHECK_EQ(ret, static_cast<int>(data.size()));
  want_read_ = false;
  if (state_ == IN_CONNECT) {
    int status = Connect();
    if (status < 0 && status != TLSEngine::TLS_WANT_READ &&
        status != TLSEngine::TLS_WANT_WRITE)
      return TLSEngine::TLS_VERIFY_ERROR;
  }
  return ret;
}

int OpenSSLEngine::Read(void* data, int size) {
  DCHECK_EQ(state_, READY);
  int ret = SSL_read(ssl_, data, size);
  return UpdateStatus(ret);
}

int OpenSSLEngine::Write(const void* data, int size) {
  DCHECK_EQ(state_, READY);
  int ret = SSL_write(ssl_, data, size);
  return UpdateStatus(ret);
}

int OpenSSLEngine::UpdateStatus(int return_value) {
  want_read_ = false;
  want_write_ = false;
  if (return_value > 0)
    return return_value;

  int ssl_err = SSL_get_error(ssl_, return_value);
  switch (ssl_err) {
    case SSL_ERROR_WANT_READ:
      want_read_ = true;
      return TLSEngine::TLS_WANT_READ;
    case SSL_ERROR_WANT_WRITE:
      want_write_ = true;
      return TLSEngine::TLS_WANT_WRITE;
    case SSL_ERROR_SSL:
      if (SSL_get_verify_result(ssl_) != X509_V_OK) {
        // Renew CRLs in the next connection but fails for this time.
        LOG(WARNING) << "ctx:" << ctx_
                     << ": Resetting CRLs because of verify error."
                     << " details=" << X509_verify_cert_error_string(
                         SSL_get_verify_result(ssl_));
        ctx_->Invalidate();
      }
      ABSL_FALLTHROUGH_INTENDED;
    default:
      LOG(ERROR) << "ctx:" << ctx_ << ": OpenSSL error"
                 << " ret=" << return_value
                 << " ssl_err=" << ssl_err
                 << " err_msg=" << GetLastErrorMessage();
      return TLSEngine::TLS_ERROR;
  }
}

int OpenSSLEngine::Connect() {
  int ret = SSL_connect(ssl_);
  if (ret > 0) {
    VLOG(3) << "ctx:" << ctx_
            << ": session reused=" << SSL_session_reused(ssl_);
    state_ = READY;
    if (need_self_verify_) {
      LOG(INFO) << GetHumanReadableSSLInfo(ssl_);

      STACK_OF(X509)* x509s = SSL_get_peer_cert_chain(ssl_);
      if (!x509s) {
        LOG(ERROR) << "ctx:" << ctx_
                   << ": No x509 stored in SSL structure.";
        return TLSEngine::TLS_VERIFY_ERROR;
      }
      LOG(INFO) << "ctx:" << ctx_ << ": "
                << GetHumanReadableCerts(x509s)
                << " session_info="
                << GetHumanReadableSessionInfo(SSL_get_session(ssl_));

      // Get server certificate to verify.
      // For ease of the code, I will not get certificate from the certificate
      // chain got above.
      std::unique_ptr<X509, ScopedX509Free> cert(
          SSL_get_peer_certificate(ssl_));
      if (cert.get() == nullptr) {
        LOG(ERROR) << "ctx:" << ctx_
                   << ": Cannot obtain the server's certificate";
        return TLSEngine::TLS_VERIFY_ERROR;
      }

      LOG(INFO) << "ctx:" << ctx_ << ": Checking server's identity.";
      // OpenSSL library does not check a name written in certificate
      // matches what we are connecting now.
      // We MUST do it by ourselves. Or, we allow spoofing.
      if (!ctx_->IsValidServerIdentity(cert.get())) {
        return TLSEngine::TLS_VERIFY_ERROR;
      }

      // Since CRL did not set when SSL started, CRL verification should be
      // done by myself. Note that this is usually treated by OpenSSL library.
      LOG(INFO) << "ctx:" << ctx_
                << ": need to verify revoked certificate by myself.";
      if (ctx_->IsRevoked(x509s)) {
        return TLSEngine::TLS_VERIFY_ERROR;
      }
    }
  }
  return UpdateStatus(ret);
}

std::string OpenSSLEngine::GetErrorString() const {
  auto err = ERR_peek_last_error();
  if (err == 0) {
    return "ok";
  }
  char error_message[1024];
  ERR_error_string_n(err, error_message, sizeof error_message);
  return error_message;
}

std::string OpenSSLEngine::GetLastErrorMessage() const {
  std::ostringstream oss;
  oss << GetErrorString();
  const std::string& ctx_err = ctx_->GetLastErrorMessage();
  if (!ctx_err.empty()) {
    oss << " ctx_error=" << ctx_err;
  }
  if (ERR_GET_REASON(ERR_peek_last_error()) ==
      SSL_R_CERTIFICATE_VERIFY_FAILED) {
    oss << " verify_error="
        << X509_verify_cert_error_string(SSL_get_verify_result(ssl_));
  }
  return oss.str();
}

OpenSSLEngineCache::OpenSSLEngineCache() : ctx_(nullptr) {
  absl::call_once(g_openssl_init_once, InitOpenSSL);
}

OpenSSLEngineCache::~OpenSSLEngineCache() {
  // If OpenSSLEngineCache is deleted correctly, we can expect:
  // 1. all outgoing sockets are closed.
  // 2. all counterpart OpenSSLEngine instances are free'd.
  // 3. contexts_to_delete_ should be empty.
  // 4. reference count of ctx_ should be zero.
  CHECK(contexts_to_delete_.empty());
  CHECK(!ctx_.get() || ctx_->ref_cnt() == 0UL);
}

std::unique_ptr<OpenSSLEngine> OpenSSLEngineCache::GetOpenSSLEngineUnlocked() {
  if (ctx_.get() == nullptr) {
    CHECK(OpenSSLCertificateStore::IsReady())
        << "OpenSSLCertificateStore does not have any certificates.";
    ctx_ = absl::make_unique<OpenSSLContext>();
    ctx_->Init(hostname_, crl_max_valid_duration_,
               NewCallback(this, &OpenSSLEngineCache::InvalidateContext));
    if (!proxy_host_.empty())
      ctx_->SetProxy(proxy_host_, proxy_port_);
  }
  std::unique_ptr<OpenSSLEngine> engine(new OpenSSLEngine());
  engine->Init(ctx_.get());
  return engine;
}

void OpenSSLEngineCache::AddCertificateFromFile(
    const std::string& ssl_cert_filename) {
  OpenSSLCertificateStore::AddCertificateFromFile(ssl_cert_filename);
}

void OpenSSLEngineCache::AddCertificateFromString(const std::string& ssl_cert) {
  OpenSSLCertificateStore::AddCertificateFromString("user", ssl_cert);
}

TLSEngine* OpenSSLEngineCache::NewTLSEngine(int sock) {
  AUTOLOCK(lock, &mu_);
  auto found = ssl_map_.find(sock);
  if (found != ssl_map_.end()) {
    found->second->SetRecycled();
    return found->second.get();
  }
  std::unique_ptr<OpenSSLEngine> engine = GetOpenSSLEngineUnlocked();
  OpenSSLEngine* engine_ptr = engine.get();
  CHECK(ssl_map_.emplace(sock, std::move(engine)).second)
      << "ssl_map_ should not have the same key:" << sock;
  VLOG(1) << "SSL engine allocated. sock=" << sock;
  return engine_ptr;
}

void OpenSSLEngineCache::WillCloseSocket(int sock) {
  AUTOLOCK(lock, &mu_);
  VLOG(1) << "SSL engine release. sock=" << sock;
  auto found = ssl_map_.find(sock);
  if (found != ssl_map_.end()) {
    ssl_map_.erase(found);
  }

  if (!contexts_to_delete_.empty()) {
    std::vector<std::unique_ptr<OpenSSLContext>> new_contexts_to_delete;
    for (auto& ctx : contexts_to_delete_) {
      if (ctx->ref_cnt() == 0) {
        CHECK(ctx.get() != ctx_.get());
        continue;
      }
      new_contexts_to_delete.emplace_back(std::move(ctx));
    }
    contexts_to_delete_ = std::move(new_contexts_to_delete);
  }
}

void OpenSSLEngineCache::InvalidateContext() {
  AUTOLOCK(lock, &mu_);
  // OpenSSLContext instance should be held until ref_cnt become zero
  // i.e. no OpenSSLEngine instance use it.
  LOG_IF(ERROR, ctx_->hostname() != hostname_)
      << "OpenSSLContext hostname is different from OpenSSLEngineFactory one. "
      << " It might be changed after ctx_ is created?"
      << " ctx_hostname=" << ctx_->hostname()
      << " factory_hostname=" << hostname_;
  contexts_to_delete_.emplace_back(std::move(ctx_));
}

}  // namespace devtools_goma
