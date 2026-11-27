// SPDX-License-Identifier: Apache-2.0
// Provenance: acl dev tool. No GPL-3.0 sources.
//
// Asks the desktop for its color scheme the way a4l does while an app starts,
// twice, and prints each answer with the time it took: `acl-appearance-probe`.
// -1 is "no answer".
#include "svc/appearance/appearance.h"

#include <chrono>
#include <cstdio>

int main() {
  for (int i = 1; i <= 2; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    const int scheme = acl::appearance::query_color_scheme();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    std::printf("color-scheme query %d: %d in %lld ms\n", i, scheme,
                static_cast<long long>(ms));
  }
  return 0;
}
