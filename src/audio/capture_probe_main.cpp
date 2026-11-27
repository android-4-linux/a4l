// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l dev tool over audio/pipewire_source.cpp. No
// GPL-3.0 sources.
//
// acl-capture-probe [seconds]: record 16 kHz mono from the PipeWire input
// a4l uses (the desktop's default, or ACL_AUDIO_CAPTURE_NODE) and print the
// level each 100 ms, then the peak. Speak into the microphone to see which
// one it is.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "audio/audio.h"
#include "audio/audio_record.h"

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? std::clamp(std::atoi(argv[1]), 1, 60) : 3;
  acl::audio::Format fmt;
  fmt.sample_rate = 16000;
  fmt.channels = 1;
  auto source = acl::audio::make_pipewire_source("acl-capture-probe");
  if (!source->open(fmt, fmt.sample_rate / 10)) {
    std::fprintf(stderr, "cannot open the source\n");
    return 1;
  }
  source->start();
  std::vector<std::int16_t> pcm(1600);
  int peak = 0;
  long frames = 0;
  for (int i = 0; i < seconds * 10; ++i) {
    const int n = source->read(pcm.data(), 1600, /*blocking=*/true);
    if (n <= 0)
      break;
    double sum = 0.0;
    int block_peak = 0;
    for (int k = 0; k < n; ++k) {
      const int v =
          std::abs(static_cast<int>(pcm[static_cast<std::size_t>(k)]));
      block_peak = std::max(block_peak, v);
      sum += static_cast<double>(v) * v;
    }
    peak = std::max(peak, block_peak);
    frames += n;
    const double rms = std::sqrt(sum / n);
    const int bar = static_cast<int>(std::min(50.0, rms / 200.0));
    std::printf("%5.1f s  rms %6.0f  %.*s\n", (i + 1) / 10.0, rms, bar,
                "##################################################");
  }
  source->stop();
  source->close();
  std::printf("frames %ld  peak %d of 32767\n", frames, peak);
  return peak > 0 ? 0 : 1;
}
