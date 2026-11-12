// SPDX-License-Identifier: Apache-2.0
// Provenance: acl dev tool; prints (and optionally sets) the host PipeWire
// master volume via the svc/audio default-sink controller. No GPL-3.0 sources.
//
// Usage: acl-audio-probe            → print the master output volume/mute
//        acl-audio-probe <0..1>     → set the master volume (linear), then
//        print
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "audio/pw_volume.h"

int main(int argc, char** argv) {
  auto v = acl::audio::make_pw_volume();
  if (argc > 1) {
    v->set_volume(static_cast<float>(std::atof(argv[1])));
    // The set is dispatched on the PipeWire thread loop; let it flush before we
    // tear down.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  std::printf("acl master output: %s\n", v->available() ? "bound" : "none");
  std::printf("  volume  = %.3f (linear)\n", v->volume());
  std::printf("  muted   = %s\n", v->muted() ? "true" : "false");
  std::printf("  channels= %d\n", v->channels());
  return 0;
}
