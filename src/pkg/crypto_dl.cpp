// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l implementation. No GPL-3.0 sources.

#include "pkg/crypto_dl.h"

#if ACL_HAVE_LIBCRYPTO

#include <dlfcn.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define ACL_PKG_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ACL_PKG_SANITIZED 1
#endif

namespace acl::pkg {

namespace {

// The soname pins the 3.x ABI the headers were compiled against.
constexpr char kSoname[] = "libcrypto.so.3";

const Crypto* load() noexcept {
  // RTLD_NOW binds libcrypto's own references before ART brings BoringSSL
  // into the global scope. RTLD_DEEPBIND makes that hold for anything bound
  // later too (a provider loaded on demand); sanitizer runtimes reject it.
  int flags = RTLD_NOW | RTLD_LOCAL;
#if !defined(ACL_PKG_SANITIZED)
  flags |= RTLD_DEEPBIND;
#endif
  void* handle = ::dlopen(kSoname, flags);
  if (handle == nullptr)
    return nullptr;
  static Crypto table;
#define ACL_CRYPTO_LOAD(name)                                                  \
  table.name = reinterpret_cast<decltype(table.name)>(::dlsym(handle, #name)); \
  if (table.name == nullptr)                                                   \
    return nullptr;
  ACL_CRYPTO_FUNCS(ACL_CRYPTO_LOAD)
#undef ACL_CRYPTO_LOAD
  return &table;
}

}  // namespace

const Crypto* crypto() noexcept {
  static const Crypto* const instance = load();
  return instance;
}

}  // namespace acl::pkg

#endif  // ACL_HAVE_LIBCRYPTO
