// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/audio.md (pending), Android SDK AudioTrack docs, PipeWire API,
// CTS. No GPL-3.0 sources.
//
// The audio subsystem: the AudioTrack sink abstraction, the counting/null backend,
// and backend selection. The PipeWire backend lives in pipewire_sink.cpp (built
// only when libpipewire is present); this file always builds so audio/ works
// headless with the deterministic counting sink.

#include "audio/audio.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string_view>
#include <vector>

#include "acl_config.h"
#include "log/log.h"

namespace acl::audio {
namespace {

constexpr std::string_view kTag = "acl.audio";

// Counts frames without touching a device — deterministic and always available.
// It is the default backend and the fallback when PipeWire is unbuilt/unselected.
class NullSink final : public Sink {
 public:
  bool open(const Format& fmt) override {
    fmt_ = fmt;
    frames_ = 0;
    log::i(kTag, "sink=null open: {} Hz, {} ch, S16", fmt.sample_rate, fmt.channels);
    return true;
  }
  int write(const std::int16_t* pcm, int n_samples) override {
    (void)pcm;
    if (n_samples <= 0) return 0;
    frames_ += static_cast<std::uint64_t>(n_samples) /
               static_cast<std::uint64_t>(fmt_.channels > 0 ? fmt_.channels : 1);
    return n_samples;
  }
  void drain() override {}
  void close() override {
    log::i(kTag, "sink=null close: {} frames written", frames_);
  }
  std::uint64_t frames_written() const override { return frames_; }

 private:
  Format fmt_;
  std::uint64_t frames_ = 0;
};

}  // namespace

std::unique_ptr<Sink> make_null_sink() { return std::make_unique<NullSink>(); }

#if ACL_HAVE_PIPEWIRE
std::unique_ptr<Sink> make_pipewire_sink();  // defined in pipewire_sink.cpp
#endif

std::unique_ptr<Sink> make_sink() {
  const char* backend = std::getenv("ACL_AUDIO");
#if ACL_HAVE_PIPEWIRE
  if (backend != nullptr && std::strcmp(backend, "pipewire") == 0) {
    if (auto s = make_pipewire_sink()) return s;
    log::w(kTag, "PipeWire sink unavailable; falling back to null sink");
  }
#else
  if (backend != nullptr && std::strcmp(backend, "pipewire") == 0) {
    log::w(kTag, "built without PipeWire (ACL_HAVE_PIPEWIRE=0); using null sink");
  }
#endif
  return std::make_unique<NullSink>();
}

std::uint64_t play_tone(const ToneSpec& spec) {
  const int rate = spec.sample_rate > 0 ? spec.sample_rate : 48000;
  const int ch = spec.channels > 0 ? spec.channels : 2;
  const double secs = spec.seconds > 0.0 ? spec.seconds : 2.0;
  const int freq = spec.freq_hz > 0 ? spec.freq_hz : 440;
  const int frames = static_cast<int>(static_cast<double>(rate) * secs);
  const double amp = std::clamp(spec.amplitude, 0.0, 1.0) * 32767.0;
  const int fade = std::min(frames / 4, rate / 100);  // ~10ms fade in/out, click-free

  std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames) * ch);
  for (int i = 0; i < frames; ++i) {
    double env = 1.0;
    if (fade > 0 && i < fade) env = static_cast<double>(i) / fade;
    else if (fade > 0 && i >= frames - fade) env = static_cast<double>(frames - 1 - i) / fade;
    const double angle = 2.0 * M_PI * freq * i / rate;
    const auto s = static_cast<std::int16_t>(amp * env * std::sin(angle));
    for (int c = 0; c < ch; ++c) pcm[static_cast<std::size_t>(i) * ch + c] = s;
  }

  const Format fmt{rate, ch};
  std::unique_ptr<Sink> sink;
  bool device = false;
  const char* backend = std::getenv("ACL_AUDIO");
  const bool force_null = backend != nullptr && std::strcmp(backend, "null") == 0;
#if ACL_HAVE_PIPEWIRE
  if (!force_null) {
    sink = make_pipewire_sink();
    device = sink->open(fmt);
    if (!device)
      log::w(kTag, "tone: no PipeWire daemon reachable; using silent null sink (nothing to hear)");
  }
#else
  if (!force_null)
    log::w(kTag, "tone: built without PipeWire; using silent null sink (nothing to hear)");
#endif
  if (!device) {  // ACL_AUDIO=null, no PipeWire, or open failed
    sink = make_null_sink();
    sink->open(fmt);
  }
  log::i(kTag, "tone: {} Hz sine, {} s, {} Hz, {} ch, amp {}", freq, secs, rate, ch,
         spec.amplitude);

  const int total = frames * ch;
  if (device) {
    // Pace writes to ~real time so the FIFO stays small and any duration plays fully
    // (the sink's drain() is bounded and only tops off the tail).
    const int chunk = (rate / 10) * ch;  // 100ms
    for (int off = 0; off < total; off += chunk) {
      sink->write(pcm.data() + off, std::min(chunk, total - off));
      const timespec ts{0, 90 * 1000 * 1000};  // 90ms (< the 100ms chunk keeps it fed)
      nanosleep(&ts, nullptr);
    }
  } else {
    sink->write(pcm.data(), total);  // silent sink: no need to pace
  }
  sink->drain();
  sink->close();
  return sink->frames_written();
}

const char* module_name() noexcept { return "audio"; }

}  // namespace acl::audio
