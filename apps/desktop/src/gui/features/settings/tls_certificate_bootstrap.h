/*
 * @Author: DI JUNKUN
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _TLS_CERTIFICATE_BOOTSTRAP_H_
#define _TLS_CERTIFICATE_BOOTSTRAP_H_

#include <cstdint>
#include <string>
#include <vector>

namespace crossdesk {

// One certificate from the chain the server presented during the TLS
// handshake. Self-signed roots are the only ones this module may install,
// because a public CA chain never needs a manual trust step.
struct PresentedCertificate {
  std::vector<uint8_t> der;
  std::string subject;
  std::string issuer;
  std::string sha256_fingerprint;
  std::string not_after;
  bool self_signed = false;
  bool is_ca = false;
};

struct CertificateProbeResult {
  // False on platforms without an automatic installer; the UI then only
  // offers manual instructions.
  bool supported = false;
  // True once the TLS handshake completed. Certificate verification stays
  // disabled during the probe: the whole point is to inspect an untrusted
  // certificate before trusting it.
  bool connected = false;
  bool found_self_signed_root = false;
  // Machine readable failure token ("connect_timeout", "tls_handshake_failed").
  std::string error;
  // Raw OpenSSL/Winsock text kept for the log file only.
  std::string detail;
  std::vector<PresentedCertificate> chain;
};

// Connects to the signaling endpoint with verification disabled and returns
// every certificate the server presented. Blocking: call from a worker thread.
CertificateProbeResult ProbeServerCertificates(const std::string &host,
                                               uint16_t port,
                                               int timeout_ms = 6000);

struct CertificateInstallResult {
  bool ok = false;
  std::string error;
  // PEM copy written next to the application data so the user can install it
  // manually or inspect it before trusting the server.
  std::string exported_path;
};

// Installs the certificate into the current user's "Trusted Root Certification
// Authorities" store. This never raises a UAC prompt, and the desktop client
// reads that store first when it builds its trust anchors.
CertificateInstallResult InstallCertificateForCurrentUser(
    const std::vector<uint8_t> &der, const std::string &export_dir);

// Human readable reason for the last failure, already localized by the caller.
std::string DescribeProbeFailure(const CertificateProbeResult &result);

}  // namespace crossdesk

#endif  // _TLS_CERTIFICATE_BOOTSTRAP_H_
