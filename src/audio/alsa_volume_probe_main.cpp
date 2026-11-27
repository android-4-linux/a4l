// SPDX-License-Identifier: Apache-2.0
// Provenance: acl dev tool; after pw_volume_probe_main.cpp. No GPL-3.0 sources.
//
// acl-alsa-volume-probe [linear]: the ALSA master volume a4l uses for
// ACL_ALSA_DEVICE (and ACL_ALSA_MIXER), set first when a value in [0,1] is
// given.
#include <cstdio>
#include <cstdlib>

#include "audio/alsa_device.h"

int main(int argc, char** argv) {
  auto v = acl::audio::make_alsa_volume();
  if (argc > 1) {
    const bool ok = v->set_volume(static_cast<float>(std::atof(argv[1])));
    std::printf("set       = %s\n", ok ? "ok" : "refused");
  }
  std::printf("control   = %s\n", v->available() ? "bound" : "none");
  std::printf("volume    = %.3f (linear)\n", v->volume());
  std::printf("muted     = %s\n", v->muted() ? "true" : "false");
  std::printf("channels  = %d\n", v->channels());
  return 0;
}
