// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l. No GPL-3.0 sources.
//
// The host side of native audio output (ndk/bionic/acl_audio_out.h): what
// a4l's libaaudio.so plays through. Each output is an audio::Track over the
// configured sink, the same pair android.media.AudioTrack plays through, so
// native and Java audio share the device, the volume and ACL_AUDIO.
#include "runtime/ndk_audio.h"

#include <algorithm>
#include <cstdint>
#include <memory>

#include "audio/audio.h"
#include "audio/audio_track.h"
#include "log/log.h"
#include "ndk/bionic_runtime.h"

namespace acl::runtime {
namespace {

constexpr const char* kTag = "acl.audio.ndk";

audio::Track* track_of(void* out) {
  return static_cast<audio::Track*>(out);
}

void* out_open(int rate, int channels) {
  audio::Track::Config cfg;
  cfg.sample_rate = rate;
  cfg.channels = channels;
  cfg.capacity_frames = audio::Track::min_capacity_frames(rate);
  if (!audio::Track::valid(cfg)) {
    log::w(kTag, "output refused: {} Hz, {} ch", rate, channels);
    return nullptr;
  }
  auto track = std::make_unique<audio::Track>(cfg, audio::make_sink());
  if (!track->ok())
    return nullptr;
  log::i(kTag, "output: {} Hz, {} ch, {} frames", rate, channels,
         cfg.capacity_frames);
  return track.release();
}

int out_write(void* out, const std::int16_t* pcm, int frames) {
  if (out == nullptr || pcm == nullptr || frames <= 0)
    return 0;
  return std::max(0, track_of(out)->write(pcm, frames, /*blocking=*/true));
}

void out_start(void* out) {
  if (out != nullptr)
    track_of(out)->play();
}

// pause() and flush(), not stop(): Track::stop() plays out what is queued,
// and a writer blocked on a full track would stay blocked meanwhile.
void out_stop(void* out) {
  if (out == nullptr)
    return;
  track_of(out)->pause();
  track_of(out)->flush();
}

int out_timestamp(void* out, std::int64_t* frames, std::int64_t* time_ns) {
  if (out == nullptr || frames == nullptr || time_ns == nullptr)
    return 0;
  std::uint64_t f = 0;
  std::int64_t ns = 0;
  if (!track_of(out)->timestamp(&f, &ns))
    return 0;
  *frames = static_cast<std::int64_t>(f);
  *time_ns = ns;
  return 1;
}

void out_close(void* out) {
  if (out == nullptr)
    return;
  std::unique_ptr<audio::Track> track(track_of(out));
  track->shutdown();  // lets go a writer still blocked in it
}

const acl_audio_out_ops kOps = {
    &out_open, &out_write, &out_start, &out_stop, &out_timestamp, &out_close,
};

}  // namespace

void register_ndk_audio() {
  ndk::set_audio_out_ops(&kOps);
}

}  // namespace acl::runtime
