// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/audio.md (pending), Android SDK AudioTrack docs, PipeWire
// pw_stream API. No GPL-3.0 sources.
#ifndef ACL_AUDIO_AUDIO_H
#define ACL_AUDIO_AUDIO_H

// The audio output seam: android.media.AudioTrack's native side writes 16-bit PCM
// into a Sink. The default Sink counts frames (deterministic, no device); the
// PipeWire backend (when built and selected) streams to the running daemon so a
// track actually plays. Selection is by the ACL_AUDIO env var.

#include <cstdint>
#include <memory>

namespace acl::audio {

// Interleaved signed-16-bit PCM (S16LE), the AudioTrack ENCODING_PCM_16BIT format.
struct Format {
  int sample_rate = 44100;
  int channels = 1;
};

class Sink {
 public:
  virtual ~Sink() = default;
  virtual bool open(const Format& fmt) = 0;
  // Accept `n_samples` interleaved 16-bit samples; returns how many were accepted.
  virtual int write(const std::int16_t* pcm, int n_samples) = 0;
  virtual void drain() = 0;
  virtual void close() = 0;
  // Total frames (samples / channels) accepted since open() — the deterministic,
  // device-independent measure the tests assert on.
  virtual std::uint64_t frames_written() const = 0;
};

// Create the configured sink. ACL_AUDIO=pipewire selects the PipeWire backend
// (only if built with it); anything else (default) is the counting/null sink.
std::unique_ptr<Sink> make_sink();

const char* module_name() noexcept;

}  // namespace acl::audio

#endif  // ACL_AUDIO_AUDIO_H
