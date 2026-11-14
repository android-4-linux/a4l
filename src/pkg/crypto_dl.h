// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l implementation. OpenSSL (Apache-2.0) headers supply
// the declarations only. No GPL-3.0 sources.
//
// libcrypto, loaded privately at run time instead of linked.
//
// a4l shares one process with host ART, whose conscrypt and libjavacrypto use
// AOSP's BoringSSL. BoringSSL and OpenSSL export the same names with different
// layouts. A NEEDED entry for OpenSSL puts it in the global symbol scope ahead
// of everything ART loads later, so BoringSSL's libssl binds to OpenSSL and
// faults in NativeCrypto.<clinit>. dlopen with RTLD_LOCAL keeps OpenSSL out of
// that scope; nothing else in the process can bind to it.

#ifndef ACL_PKG_CRYPTO_DL_H_
#define ACL_PKG_CRYPTO_DL_H_

#include "acl_config.h"

#if ACL_HAVE_LIBCRYPTO

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace acl::pkg {

// Every libcrypto function pkg/ calls. A call that is not routed through this
// table fails at link time, because nothing links libcrypto.
#define ACL_CRYPTO_FUNCS(X)           \
  X(CRYPTO_memcmp)                    \
  X(d2i_PUBKEY)                       \
  X(d2i_X509)                         \
  X(EVP_Digest)                       \
  X(EVP_DigestFinal_ex)               \
  X(EVP_DigestInit_ex)                \
  X(EVP_DigestUpdate)                 \
  X(EVP_DigestVerify)                 \
  X(EVP_DigestVerifyInit)             \
  X(EVP_MD_CTX_free)                  \
  X(EVP_MD_CTX_new)                   \
  X(EVP_PKEY_CTX_set_rsa_mgf1_md)     \
  X(EVP_PKEY_CTX_set_rsa_padding)     \
  X(EVP_PKEY_CTX_set_rsa_pss_saltlen) \
  X(EVP_PKEY_eq)                      \
  X(EVP_PKEY_free)                    \
  X(EVP_PKEY_get_base_id)             \
  X(EVP_sha256)                       \
  X(EVP_sha512)                       \
  X(X509_free)                        \
  X(X509_get0_pubkey)

struct Crypto {
#define ACL_CRYPTO_FIELD(name) decltype(&::name) name = nullptr;
  ACL_CRYPTO_FUNCS(ACL_CRYPTO_FIELD)
#undef ACL_CRYPTO_FIELD
};

// The table, or null if libcrypto.so.3 is absent or lacks one of the
// functions. Loaded once on first call; thread-safe; never unloaded.
const Crypto* crypto() noexcept;

}  // namespace acl::pkg

#endif  // ACL_HAVE_LIBCRYPTO
#endif  // ACL_PKG_CRYPTO_DL_H_
