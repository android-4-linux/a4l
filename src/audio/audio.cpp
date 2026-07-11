// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/audio.md (pending), Android SDK AudioTrack docs, PipeWire API,
// CTS. No GPL-3.0 sources.
//
// The audio subsystem: the AudioTrack sink abstraction, the counting/null backend,
// and backend selection. The PipeWire backend lives in pipewire_sink.cpp (built
// only when libpipewire is present); this file always builds so audio/ works
// headless with the deterministic counting sink.

#include "audio/audio.h"

#include <cstdlib>
#include <cstring>
#include <string_view>

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

const char* module_name() noexcept { return "audio"; }

}  // namespace acl::audio
