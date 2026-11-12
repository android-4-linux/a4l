// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl dev tooling; exercises svc/keystore's
// IKeystoreBackend. No GPL-3.0 sources.
//
// acl-keystore-probe: round-trip a blob through the selected keystore backend
// (store -> contains -> load -> list -> erase), verifying the Secret Service
// (or memory fallback) works before wiring the AndroidKeyStore JCE provider on
// top. Persists under package "org.acl.probe" and cleans up.
#include "svc/keystore/keystore_backend.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  const std::string pkg = "org.acl.probe";
  const std::string alias = argc > 1 ? argv[1] : "probe-key";
  auto be = acl::keystore::make_keystore_backend();
  std::printf("backend    : %s\n", be->name());

  const std::vector<std::uint8_t> blob = {0xAC, 0x1B, 0x10, 0xB0,
                                          0x00, 0xFF, 0x42, 0x13};
  std::printf("store      : %s\n",
              be->store(pkg, alias, blob) ? "ok" : "FAILED");
  std::printf("contains   : %s\n", be->contains(pkg, alias) ? "yes" : "NO");

  std::vector<std::uint8_t> got;
  const bool loaded = be->load(pkg, alias, got);
  const bool match = loaded && got == blob;
  std::printf("load       : %s (%zu bytes, %s)\n", loaded ? "ok" : "FAILED",
              got.size(), match ? "round-trip OK" : "MISMATCH");

  auto names = be->aliases(pkg);
  std::printf("aliases    : %zu [", names.size());
  for (size_t i = 0; i < names.size(); ++i)
    std::printf("%s%s", i ? ", " : "", names[i].c_str());
  std::printf("]\n");

  std::printf("erase      : %s\n", be->erase(pkg, alias) ? "ok" : "FAILED");
  std::printf("gone       : %s\n",
              be->contains(pkg, alias) ? "STILL PRESENT" : "yes");
  return match ? 0 : 1;
}
