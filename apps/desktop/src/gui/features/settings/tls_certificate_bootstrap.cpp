/*
 * @Author: DI JUNKUN
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#include "features/settings/tls_certificate_bootstrap.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// winsock2.h must precede windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace crossdesk {
namespace {

bool EnsureWinsock() {
  static const bool ready = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ready;
}

std::string TakeBioString(BIO *bio) {
  if (!bio) return {};
  char *data = nullptr;
  const long size = BIO_get_mem_data(bio, &data);
  std::string text = (data && size > 0) ? std::string(data, static_cast<size_t>(size)) : std::string{};
  BIO_free(bio);
  return text;
}

std::string NameToString(X509_NAME *name) {
  if (!name) return {};
  BIO *bio = BIO_new(BIO_s_mem());
  if (!bio) return {};
  if (X509_NAME_print_ex(bio, name, 0, XN_FLAG_RFC2253) < 0) {
    BIO_free(bio);
    return {};
  }
  return TakeBioString(bio);
}

std::string TimeToString(const ASN1_TIME *time) {
  if (!time) return {};
  BIO *bio = BIO_new(BIO_s_mem());
  if (!bio) return {};
  if (ASN1_TIME_print(bio, time) != 1) {
    BIO_free(bio);
    return {};
  }
  return TakeBioString(bio);
}

std::string FingerprintSha256(X509 *cert) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (X509_digest(cert, EVP_sha256(), digest, &length) != 1) return {};
  std::string text;
  char part[4];
  for (unsigned int i = 0; i < length; ++i) {
    std::snprintf(part, sizeof(part), i == 0 ? "%02X" : ":%02X", digest[i]);
    text += part;
  }
  return text;
}

PresentedCertificate Describe(X509 *cert) {
  PresentedCertificate info;
  unsigned char *der = nullptr;
  const int length = i2d_X509(cert, &der);
  if (length > 0 && der) {
    info.der.assign(der, der + length);
    OPENSSL_free(der);
  }
  info.subject = NameToString(X509_get_subject_name(cert));
  info.issuer = NameToString(X509_get_issuer_name(cert));
  info.sha256_fingerprint = FingerprintSha256(cert);
  info.not_after = TimeToString(X509_get0_notAfter(cert));
  // A certificate that issued itself is a self-signed root; only that one can
  // serve as a trust anchor for a private deployment.
  info.self_signed = X509_check_issued(cert, cert) == X509_V_OK;
  info.is_ca = X509_check_ca(cert) > 0;
  return info;
}

std::string SocketErrorText(int code) {
  switch (code) {
    case WSAETIMEDOUT: return "connect_timed_out";
    case WSAECONNREFUSED: return "connect_refused";
    case WSAEHOSTUNREACH:
    case WSAENETUNREACH: return "host_unreachable";
    default: return "connect_failed";
  }
}

SOCKET ConnectSocket(const std::string &host, uint16_t port, int timeout_ms,
                     std::string &error, std::string &detail) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  char service[8] = {};
  std::snprintf(service, sizeof(service), "%u", static_cast<unsigned>(port));

  addrinfo *addresses = nullptr;
  const int resolve = getaddrinfo(host.c_str(), service, &hints, &addresses);
  if (resolve != 0 || !addresses) {
    error = "resolve_failed";
    detail = gai_strerrorA(resolve);
    if (addresses) freeaddrinfo(addresses);
    return INVALID_SOCKET;
  }

  DWORD last_error = 0;
  SOCKET connected = INVALID_SOCKET;
  for (addrinfo *entry = addresses; entry; entry = entry->ai_next) {
    SOCKET candidate = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (candidate == INVALID_SOCKET) {
      last_error = static_cast<DWORD>(WSAGetLastError());
      continue;
    }

    u_long non_blocking = 1;
    ioctlsocket(candidate, FIONBIO, &non_blocking);
    int rc = connect(candidate, entry->ai_addr, static_cast<int>(entry->ai_addrlen));
    if (rc == SOCKET_ERROR) {
      const int connect_error = WSAGetLastError();
      if (connect_error != WSAEWOULDBLOCK) {
        last_error = static_cast<DWORD>(connect_error);
        closesocket(candidate);
        continue;
      }
      fd_set writable;
      fd_set failed;
      FD_ZERO(&writable);
      FD_ZERO(&failed);
      FD_SET(candidate, &writable);
      FD_SET(candidate, &failed);
      timeval timeout{};
      timeout.tv_sec = timeout_ms / 1000;
      timeout.tv_usec = (timeout_ms % 1000) * 1000;
      const int ready = select(0, nullptr, &writable, &failed, &timeout);
      if (ready <= 0) {
        last_error = ready == 0 ? WSAETIMEDOUT : static_cast<DWORD>(WSAGetLastError());
        closesocket(candidate);
        continue;
      }
      int so_error = 0;
      int so_length = sizeof(so_error);
      getsockopt(candidate, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&so_error), &so_length);
      if (so_error != 0) {
        last_error = static_cast<DWORD>(so_error);
        closesocket(candidate);
        continue;
      }
    }

    u_long blocking = 0;
    ioctlsocket(candidate, FIONBIO, &blocking);
    const DWORD io_timeout = static_cast<DWORD>(timeout_ms);
    setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&io_timeout), sizeof(io_timeout));
    setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&io_timeout), sizeof(io_timeout));
    connected = candidate;
    break;
  }
  freeaddrinfo(addresses);

  if (connected == INVALID_SOCKET) {
    error = SocketErrorText(static_cast<int>(last_error));
    detail = std::to_string(last_error);
  }
  return connected;
}

}  // namespace

CertificateProbeResult ProbeServerCertificates(const std::string &host,
                                               uint16_t port,
                                               int timeout_ms) {
  CertificateProbeResult result;
  result.supported = true;
  if (host.empty() || port == 0) {
    result.error = "invalid_endpoint";
    return result;
  }
  if (!EnsureWinsock()) {
    result.error = "winsock_unavailable";
    return result;
  }

  SOCKET sock = ConnectSocket(host, port, timeout_ms, result.error, result.detail);
  if (sock == INVALID_SOCKET) return result;

  SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    closesocket(sock);
    result.error = "ssl_context_failed";
    return result;
  }
  // Verification stays off by design: the caller shows the fingerprint and
  // asks the user before anything is trusted.
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

  SSL *ssl = SSL_new(ctx);
  if (!ssl) {
    SSL_CTX_free(ctx);
    closesocket(sock);
    result.error = "ssl_context_failed";
    return result;
  }

  bool handshaken = false;
  do {
    if (SSL_set_fd(ssl, static_cast<int>(sock)) != 1) {
      result.error = "ssl_socket_failed";
      break;
    }
    // SNI is harmless for literal IPs and required when the server is fronted
    // by a hostname-based reverse proxy.
    SSL_set_tlsext_host_name(ssl, host.c_str());
    if (SSL_connect(ssl) != 1) {
      char text[256] = {};
      ERR_error_string_n(ERR_get_error(), text, sizeof(text));
      result.error = "tls_handshake_failed";
      result.detail = text;
      break;
    }
    handshaken = true;
  } while (false);

  if (handshaken) {
    result.connected = true;
    std::vector<std::vector<uint8_t>> seen;
    auto append = [&](X509 *cert) {
      if (!cert) return;
      PresentedCertificate info = Describe(cert);
      if (info.der.empty()) return;
      if (std::find(seen.begin(), seen.end(), info.der) != seen.end()) return;
      seen.push_back(info.der);
      result.chain.push_back(std::move(info));
    };

    if (X509 *leaf = SSL_get1_peer_certificate(ssl)) {
      append(leaf);
      X509_free(leaf);
    }
    if (STACK_OF(X509) * chain = SSL_get_peer_cert_chain(ssl)) {
      for (int i = 0; i < sk_X509_num(chain); ++i) append(sk_X509_value(chain, i));
    }

    // Prefer the deepest self-signed certificate: that is the anchor the
    // client has to trust.
    for (const auto &entry : result.chain) {
      if (entry.self_signed) result.found_self_signed_root = true;
    }
    if (!result.found_self_signed_root && !result.chain.empty()) {
      result.detail += result.detail.empty() ? "no_self_signed_certificate" : "; no_self_signed_certificate";
    }
  }

  SSL_shutdown(ssl);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  closesocket(sock);
  return result;
}

CertificateInstallResult InstallCertificateForCurrentUser(
    const std::vector<uint8_t> &der, const std::string &export_dir) {
  CertificateInstallResult result;
  if (der.empty()) {
    result.error = "empty_certificate";
    return result;
  }

  // Keep a PEM copy on disk first: it is the fallback for users who prefer to
  // inspect and install the certificate themselves.
  const unsigned char *cursor = der.data();
  X509 *cert = d2i_X509(nullptr, &cursor, static_cast<long>(der.size()));
  if (!cert) {
    result.error = "decode_failed";
    return result;
  }
  if (!export_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(export_dir, ec);
    const auto path = std::filesystem::path(export_dir) / "crossdesk_server_root.crt";
    FILE *file = nullptr;
    if (_wfopen_s(&file, path.wstring().c_str(), L"wb") == 0 && file) {
      BIO *bio = BIO_new_fp(file, BIO_NOCLOSE);
      if (bio && PEM_write_bio_X509(bio, cert) == 1) {
        result.exported_path = path.string();
      }
      BIO_free(bio);
      std::fclose(file);
    }
  }
  X509_free(cert);
  if (result.exported_path.empty()) {
    // Rewriting the file failed but installation may still succeed; the UI
    // simply cannot offer the manual fallback for this attempt.
  }

  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                   CERT_SYSTEM_STORE_CURRENT_USER, L"ROOT");
  if (!store) {
    result.error = "store_open_failed";
    return result;
  }
  const BOOL added = CertAddEncodedCertificateToStore(
      store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der.data(),
      static_cast<DWORD>(der.size()), CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
  CertCloseStore(store, 0);
  if (!added) {
    result.error = "store_add_failed";
    return result;
  }
  result.ok = true;
  return result;
}

std::string DescribeProbeFailure(const CertificateProbeResult &result) {
  return result.error;
}

}  // namespace crossdesk

#else  // !_WIN32

namespace crossdesk {

CertificateProbeResult ProbeServerCertificates(const std::string &, uint16_t,
                                               int) {
  CertificateProbeResult result;
  result.error = "unsupported_platform";
  return result;
}

CertificateInstallResult InstallCertificateForCurrentUser(
    const std::vector<uint8_t> &, const std::string &) {
  CertificateInstallResult result;
  result.error = "unsupported_platform";
  return result;
}

std::string DescribeProbeFailure(const CertificateProbeResult &result) {
  return result.error;
}

}  // namespace crossdesk

#endif  // _WIN32
