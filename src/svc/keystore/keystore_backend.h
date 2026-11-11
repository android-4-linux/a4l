// SPDX-License-Identifier: Apache-2.0
// Provenance: Android android.security.keystore /
// java.security.KeyStore("AndroidKeyStore") public surface (AOSP android.jar
// stubs, Apache-2.0); host mapping onto the freedesktop Secret Service
// (libsecret C API, org.freedesktop.Secret.* docs). g_base64 from GLib docs. No
// GPL-3.0 sources.
//
// svc/keystore is the STORAGE half of acl's AndroidKeyStore. On a device,
// keystore keys are non-exportable and live in secure hardware
// (keystore2/Keymaster). acl has no such hardware, so acl's AndroidKeyStore JCE
// provider (Java) generates real keys with conscrypt and persists the key
// MATERIAL here — in the login keyring via the Secret Service (gnome-keyring /
// KWallet): encrypted at rest and gated on the keyring unlock (a genuine, if
// weaker, analog of setUserAuthenticationRequired). It is an honest desktop
// tradeoff, NOT hardware-equivalent: the material is readable in-process, and
// key ATTESTATION is impossible (no TEE, no Google chain).
//
// Blobs are opaque to this layer — the Java provider owns the serialization
// (algorithm, purposes, the encoded key). We key them by (package, alias), the
// AndroidKeyStore identity within an app.
#ifndef ACL_SVC_KEYSTORE_KEYSTORE_BACKEND_H
#define ACL_SVC_KEYSTORE_KEYSTORE_BACKEND_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace acl::keystore {

// A backend persists opaque key blobs keyed by (package, alias). All calls are
// synchronous; the Secret Service backend may block briefly on first use to
// unlock the login keyring.
class IKeystoreBackend {
 public:
  virtual ~IKeystoreBackend() = default;

  // Human-readable backend id for logs ("secret-service", "memory").
  virtual const char* name() const noexcept = 0;

  // Store (overwrite) the blob for (package, alias). Returns false if it could
  // not be persisted.
  virtual bool store(const std::string& pkg,
                     const std::string& alias,
                     const std::vector<std::uint8_t>& blob) = 0;

  // Load the blob for (package, alias) into out. Returns false (out cleared)
  // when absent.
  virtual bool load(const std::string& pkg,
                    const std::string& alias,
                    std::vector<std::uint8_t>& out) = 0;

  // Whether an entry exists for (package, alias).
  virtual bool contains(const std::string& pkg, const std::string& alias) = 0;

  // Delete the entry. Returns true if one existed.
  virtual bool erase(const std::string& pkg, const std::string& alias) = 0;

  // All aliases stored for a package (order unspecified).
  virtual std::vector<std::string> aliases(const std::string& pkg) = 0;
};

// Construct the backend: the Secret Service (libsecret) when reachable, else an
// in-process memory backend (non-persistent). ACL_KEYSTORE_BACKEND=memory
// forces the fallback. Never returns null.
std::unique_ptr<IKeystoreBackend> make_keystore_backend();

}  // namespace acl::keystore

#endif  // ACL_SVC_KEYSTORE_KEYSTORE_BACKEND_H
