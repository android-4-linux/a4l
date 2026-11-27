// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/audio.md (pending), Android SDK AudioTrack docs, PipeWire
// API, CTS. No GPL-3.0 sources.
//
// The audio subsystem: the AudioTrack sink abstraction, the counting/null
// backend, and backend selection. The PipeWire backend lives in
// pipewire_sink.cpp (built only when libpipewire is present); this file always
// builds so audio/ works headless with the deterministic counting sink.

#include "audio/audio.h"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "acl_config.h"
#if ACL_HAVE_ALSA
#include "audio/alsa_device.h"
#endif
#include "log/log.h"

namespace acl::audio {
namespace {

constexpr std::string_view kTag = "acl.audio";

// Counts frames without touching a device — deterministic and always available.
// It is the default backend and the fallback when PipeWire is
// unbuilt/unselected.
class NullSink final : public Sink {
 public:
  bool open(const Format& fmt) override {
    fmt_ = fmt;
    frames_ = 0;
    log::i(kTag, "sink=null open: {} Hz, {} ch, S16", fmt.sample_rate,
           fmt.channels);
    return true;
  }
  int write(const std::int16_t* pcm, int n_samples) override {
    (void)pcm;
    if (n_samples <= 0)
      return 0;
    frames_ +=
        static_cast<std::uint64_t>(n_samples) /
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

std::unique_ptr<Sink> make_null_sink() {
  return std::make_unique<NullSink>();
}

#if ACL_HAVE_PIPEWIRE
std::unique_ptr<Sink> make_pipewire_sink();  // defined in pipewire_sink.cpp
#endif

namespace {

// Whether a PipeWire daemon's socket is where a client would look for it:
// PIPEWIRE_REMOTE (a path, or a name under the runtime directory), default
// "pipewire-0". A socket left by a daemon that has died still counts; the
// stream then fails to connect as it did before.
[[maybe_unused]] bool pipewire_socket_present() {
  const char* remote = std::getenv("PIPEWIRE_REMOTE");
  const std::string name =
      remote != nullptr && remote[0] != '\0' ? remote : "pipewire-0";
  if (name[0] == '/')
    return ::access(name.c_str(), F_OK) == 0;
  for (const char* var : {"PIPEWIRE_RUNTIME_DIR", "XDG_RUNTIME_DIR"}) {
    const char* dir = std::getenv(var);
    if (dir != nullptr && dir[0] != '\0')
      return ::access((std::string(dir) + "/" + name).c_str(), F_OK) == 0;
  }
  return false;
}

DeviceBackend choose_device_backend() {
  const char* backend = std::getenv("ACL_AUDIO");
  if (backend == nullptr)
    return DeviceBackend::kNone;
  if (std::strcmp(backend, "alsa") == 0) {
#if ACL_HAVE_ALSA
    return DeviceBackend::kAlsa;
#else
    log::w(kTag, "built without ALSA (ACL_HAVE_ALSA=0); no audio device");
    return DeviceBackend::kNone;
#endif
  }
  if (std::strcmp(backend, "pipewire") != 0)
    return DeviceBackend::kNone;
#if ACL_HAVE_PIPEWIRE && ACL_HAVE_ALSA
  if (pipewire_socket_present())
    return DeviceBackend::kPipeWire;
  log::i(kTag, "no PipeWire daemon; audio goes to ALSA");
  return DeviceBackend::kAlsa;
#elif ACL_HAVE_PIPEWIRE
  return DeviceBackend::kPipeWire;
#elif ACL_HAVE_ALSA
  log::i(kTag, "built without PipeWire; audio goes to ALSA");
  return DeviceBackend::kAlsa;
#else
  log::w(kTag, "built without PipeWire and ALSA; no audio device");
  return DeviceBackend::kNone;
#endif
}

}  // namespace

DeviceBackend device_backend() {
  static const DeviceBackend chosen = choose_device_backend();
  return chosen;
}

namespace {

// The --split app process has no audio device. Its samples go to the broker
// over the channel the a4l executable holds. This file is also built into
// libsoundpool.so, a separate copy that cannot see that channel, so both
// copies reach it through three functions the executable exports by name.
using SplitOpenFn = int (*)(int rate, int channels);
using SplitWriteFn = int (*)(int stream, const std::int16_t* pcm, int n);
using SplitCloseFn = void (*)(int stream);
using SplitLatencyFn = long long (*)(int stream);

class SplitSink final : public Sink {
 public:
  SplitSink(SplitOpenFn open,
            SplitWriteFn write,
            SplitCloseFn close,
            SplitLatencyFn latency)
      : open_(open), write_(write), close_(close), latency_(latency) {}
  ~SplitSink() override { close(); }

  bool open(const Format& fmt) override {
    fmt_ = fmt;
    frames_ = 0;
    stream_ = open_(fmt.sample_rate, fmt.channels);
    if (stream_ < 0) {
      log::w(kTag, "sink=split: the broker has no stream free");
      return false;
    }
    log::i(kTag, "sink=split open: {} Hz, {} ch, S16 (stream {} at the broker)",
           fmt.sample_rate, fmt.channels, stream_);
    return true;
  }
  int write(const std::int16_t* pcm, int n_samples) override {
    if (stream_ < 0 || n_samples <= 0)
      return 0;
    const int sent = write_(stream_, pcm, n_samples);
    if (sent > 0)
      frames_ +=
          static_cast<std::uint64_t>(sent) /
          static_cast<std::uint64_t>(fmt_.channels > 0 ? fmt_.channels : 1);
    return sent;
  }
  void drain() override {}  // the broker plays what it holds
  void close() override {
    if (stream_ < 0)
      return;
    close_(stream_);
    stream_ = -1;
    log::i(kTag, "sink=split close: {} frames sent", frames_);
  }
  std::uint64_t frames_written() const override { return frames_; }
  // The broker's device, as the broker last reported it.
  std::int64_t latency_ns() const override {
    if (stream_ < 0 || latency_ == nullptr)
      return 0;
    const long long us = latency_(stream_);
    return us < 0 ? -1 : us * 1000;
  }

 private:
  SplitOpenFn open_;
  SplitWriteFn write_;
  SplitCloseFn close_;
  SplitLatencyFn latency_;
  Format fmt_;
  int stream_ = -1;
  std::uint64_t frames_ = 0;
};

std::unique_ptr<Sink> make_split_sink() {
  using ActiveFn = int (*)();
  const auto active = reinterpret_cast<ActiveFn>(
      ::dlsym(RTLD_DEFAULT, "acl_split_audio_active"));
  if (active == nullptr || active() == 0)
    return nullptr;
  const auto open = reinterpret_cast<SplitOpenFn>(
      ::dlsym(RTLD_DEFAULT, "acl_split_audio_open"));
  const auto write = reinterpret_cast<SplitWriteFn>(
      ::dlsym(RTLD_DEFAULT, "acl_split_audio_write"));
  const auto close = reinterpret_cast<SplitCloseFn>(
      ::dlsym(RTLD_DEFAULT, "acl_split_audio_close"));
  if (open == nullptr || write == nullptr || close == nullptr)
    return nullptr;
  const auto latency = reinterpret_cast<SplitLatencyFn>(
      ::dlsym(RTLD_DEFAULT, "acl_split_audio_latency_us"));
  return std::make_unique<SplitSink>(open, write, close, latency);
}

}  // namespace

std::unique_ptr<Sink> make_sink() {
  if (auto split = make_split_sink())
    return split;
  return make_device_sink();
}

std::unique_ptr<Sink> make_device_sink() {
  switch (device_backend()) {
#if ACL_HAVE_PIPEWIRE
    case DeviceBackend::kPipeWire:
      return make_pipewire_sink();
#endif
#if ACL_HAVE_ALSA
    case DeviceBackend::kAlsa:
      return make_alsa_sink();
#endif
    default:
      return std::make_unique<NullSink>();
  }
}

std::uint64_t play_tone(const ToneSpec& spec) {
  const int rate = spec.sample_rate > 0 ? spec.sample_rate : 48000;
  const int ch = spec.channels > 0 ? spec.channels : 2;
  const double secs = spec.seconds > 0.0 ? spec.seconds : 2.0;
  const int freq = spec.freq_hz > 0 ? spec.freq_hz : 440;
  const int frames = static_cast<int>(static_cast<double>(rate) * secs);
  const double amp = std::clamp(spec.amplitude, 0.0, 1.0) * 32767.0;
  const int fade =
      std::min(frames / 4, rate / 100);  // ~10ms fade in/out, click-free

  std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames) * ch);
  for (int i = 0; i < frames; ++i) {
    double env = 1.0;
    if (fade > 0 && i < fade)
      env = static_cast<double>(i) / fade;
    else if (fade > 0 && i >= frames - fade)
      env = static_cast<double>(frames - 1 - i) / fade;
    const double angle = 2.0 * M_PI * freq * i / rate;
    const auto s = static_cast<std::int16_t>(amp * env * std::sin(angle));
    for (int c = 0; c < ch; ++c)
      pcm[static_cast<std::size_t>(i) * ch + c] = s;
  }

  const Format fmt{rate, ch};
  std::unique_ptr<Sink> sink;
  bool device = false;
  const char* backend = std::getenv("ACL_AUDIO");
  const bool force_null =
      backend != nullptr && std::strcmp(backend, "null") == 0;
  // ACL_AUDIO=alsa asks for ALSA by name; otherwise PipeWire is tried first.
  [[maybe_unused]] const bool only_alsa =
      backend != nullptr && std::strcmp(backend, "alsa") == 0;
#if ACL_HAVE_PIPEWIRE
  if (!force_null && !only_alsa) {
    sink = make_pipewire_sink();
    device = sink->open(fmt);
  }
#endif
#if ACL_HAVE_ALSA
  if (!force_null && !device) {
    sink = make_alsa_sink();
    device = sink->open(fmt);
  }
#endif
  if (!force_null && !device)
    log::w(kTag,
           "tone: no PipeWire daemon and no ALSA device; using silent null "
           "sink (nothing to hear)");
  if (!device) {  // ACL_AUDIO=null, or no device opened
    sink = make_null_sink();
    sink->open(fmt);
  }
  log::i(kTag, "tone: {} Hz sine, {} s, {} Hz, {} ch, amp {}", freq, secs, rate,
         ch, spec.amplitude);

  const int total = frames * ch;
  if (device) {
    // Pace writes to ~real time so the FIFO stays small and any duration plays
    // fully (the sink's drain() is bounded and only tops off the tail).
    const int chunk = (rate / 10) * ch;  // 100ms
    for (int off = 0; off < total; off += chunk) {
      sink->write(pcm.data() + off, std::min(chunk, total - off));
      const timespec ts{
          0, 90 * 1000 * 1000};  // 90ms (< the 100ms chunk keeps it fed)
      nanosleep(&ts, nullptr);
    }
  } else {
    sink->write(pcm.data(), total);  // silent sink: no need to pace
  }
  sink->drain();
  sink->close();
  return sink->frames_written();
}

const char* module_name() noexcept {
  return "audio";
}

}  // namespace acl::audio
