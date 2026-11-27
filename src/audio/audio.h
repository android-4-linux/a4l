// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/audio.md (pending), Android SDK AudioTrack docs, PipeWire
// pw_stream API. No GPL-3.0 sources.
#ifndef ACL_AUDIO_AUDIO_H
#define ACL_AUDIO_AUDIO_H

// The audio output seam: android.media.AudioTrack's native side writes 16-bit
// PCM into a Sink. The default Sink counts frames (deterministic, no device);
// the PipeWire backend (when built and selected) streams to the running daemon
// so a track actually plays. Selection is by the ACL_AUDIO env var.

#include <cstdint>
#include <memory>

namespace acl::audio {

// Interleaved signed-16-bit PCM (S16LE), the AudioTrack ENCODING_PCM_16BIT
// format.
struct Format {
  int sample_rate = 44100;
  int channels = 1;
};

class Sink {
 public:
  virtual ~Sink() = default;
  virtual bool open(const Format& fmt) = 0;
  // Accept `n_samples` interleaved 16-bit samples; returns how many were
  // accepted.
  virtual int write(const std::int16_t* pcm, int n_samples) = 0;
  virtual void drain() = 0;
  virtual void close() = 0;
  // Total frames (samples / channels) accepted since open() — the
  // deterministic, device-independent measure the tests assert on.
  virtual std::uint64_t frames_written() const = 0;
  // Frames accepted and not yet played. 0 for a sink that keeps none. What a
  // caller that must bound memory (the --split broker) checks before a write.
  virtual std::uint64_t frames_pending() const { return 0; }
  // How long a frame takes to sound once it is no longer pending: the
  // device's own delay (its period, a resampler, a Bluetooth link). 0 for a
  // sink that has none or cannot say; -1 for one that will say and has not
  // yet, which is so until its device has run once. A player subtracts it
  // to keep picture and sound together; it changes when the device does, so
  // it is read each time.
  virtual std::int64_t latency_ns() const { return 0; }
};

// Which device backend ACL_AUDIO selects in this process:
//   unset, "null" or anything else   none: the counting sink, a silent source
//   "pipewire"   PipeWire when it is built in and its daemon's socket is
//                there; ALSA when it is not and ALSA is built in
//   "alsa"       ALSA (the "default" PCM, or the one ACL_ALSA_DEVICE names)
// So ACL_AUDIO=pipewire means "the device", and a system with no PipeWire
// still sounds. Worked out once per process.
enum class DeviceBackend { kNone, kPipeWire, kAlsa };
DeviceBackend device_backend();

// Create the configured sink: the one device_backend() names, or the
// counting/null sink.
// In a --split app process it is neither: the samples go to the broker, which
// has the audio device and makes this choice itself (see make_device_sink).
std::unique_ptr<Sink> make_sink();

// make_sink() without the --split case: the sink for a process that has the
// audio device. What the broker plays a child's stream through.
std::unique_ptr<Sink> make_device_sink();

// The deterministic counting/null sink, exposed for a graceful fallback when a
// selected device backend (e.g. PipeWire with no reachable daemon) fails to
// open.
std::unique_ptr<Sink> make_null_sink();

// A manual audio-check tone. Generates a clean sine (short fades to avoid
// clicks) and plays it through the PipeWire device sink so it is actually
// audible — falling back to the silent null sink if PipeWire is unbuilt or no
// daemon is reachable. Paces writes to real time when streaming so any duration
// plays fully. Returns the number of frames written. Backs `acl tone`.
struct ToneSpec {
  int freq_hz = 440;  // A4
  double seconds = 2.0;
  int sample_rate = 48000;
  int channels = 2;
  double amplitude = 0.6;  // 0..1 of full scale
};
std::uint64_t play_tone(const ToneSpec& spec);

const char* module_name() noexcept;

}  // namespace acl::audio

#endif  // ACL_AUDIO_AUDIO_H
