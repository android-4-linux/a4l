// SPDX-License-Identifier: Apache-2.0
// Provenance: original test helper; sdbus-c++ usage after
// jwinarske/sdbus-cpp-examples (Apache-2.0). No GPL-3.0 sources.
//
// Owns the bus name given on its command line on the session bus it is started
// on and never answers a call to it: a service that is there and does nothing,
// which is what a desktop portal with no backend looks like to its callers.
#include <sdbus-c++/sdbus-c++.h>

#include <unistd.h>

#include <cstdio>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: acl-silent-name-stub <bus name>\n");
    return 2;
  }
  // The name is requested here. With no event loop run, nothing sent to it is
  // read.
  const auto conn =
      sdbus::createSessionBusConnection(sdbus::ServiceName{argv[1]});
  (void)conn;  // held for the name; never read from
  std::printf("STUB: ready\n");
  std::fflush(stdout);
  for (;;)
    ::pause();
}
