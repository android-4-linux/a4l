// SPDX-License-Identifier: Apache-2.0
// Provenance: original; the interface is the MPRIS D-Bus Interface
// Specification 2.2 (freedesktop.org). No GPL-3.0 sources.
#include "svc/mpris/mpris_backend.h"

#include <chrono>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include "acl_config.h"
#include "log/log.h"

#if ACL_HAVE_SDBUS
#include <sdbus-c++/sdbus-c++.h>
#endif

namespace acl::mpris {
namespace {

constexpr std::string_view kTag = "svc.mpris";

class NullBackend final : public IMprisBackend {
 public:
  const char* name() const noexcept override { return "null"; }
  void set_sink(std::function<void(const MprisCommand&)> /*sink*/) override {}
  bool update(const MprisState& /*state*/) override { return false; }
  void clear() override {}
};

#if ACL_HAVE_SDBUS
constexpr const char* kPath = "/org/mpris/MediaPlayer2";
constexpr const char* kRoot = "org.mpris.MediaPlayer2";
constexpr const char* kPlayer = "org.mpris.MediaPlayer2.Player";
// A position that differs from where playback would have got to by more than
// this is a seek.
constexpr int64_t kSeekSlackUs = 1'500'000;

using Clock = std::chrono::steady_clock;

class SessionBusBackend final : public IMprisBackend {
 public:
  explicit SessionBusBackend(std::unique_ptr<sdbus::IConnection> conn)
      : conn_(std::move(conn)) {
    conn_->enterEventLoopAsync();
  }

  ~SessionBusBackend() override {
    // The object's handlers read this object; it goes before the members do.
    object_.reset();
    conn_->leaveEventLoop();
  }

  const char* name() const noexcept override { return "mpris"; }

  void set_sink(std::function<void(const MprisCommand&)> sink) override {
    std::lock_guard<std::mutex> lk(m_);
    sink_ = std::move(sink);
  }

  bool update(const MprisState& next) override {
    std::vector<sdbus::PropertyName> changed;
    bool seeked = false;
    int64_t seek_position = 0;
    {
      std::lock_guard<std::mutex> lk(m_);
      const Clock::time_point now = Clock::now();
      if (!exported_) {
        if (!export_locked(next))
          return false;
      } else {
        if (next.playback != state_.playback)
          changed.emplace_back("PlaybackStatus");
        if (next.track != state_.track || next.title != state_.title ||
            next.artist != state_.artist || next.album != state_.album ||
            next.art_url != state_.art_url ||
            next.length_us != state_.length_us)
          changed.emplace_back("Metadata");
        if (next.rate != state_.rate)
          changed.emplace_back("Rate");
        const unsigned flipped = next.can ^ state_.can;
        if (flipped & kCanPlay)
          changed.emplace_back("CanPlay");
        if (flipped & kCanPause)
          changed.emplace_back("CanPause");
        if (flipped & kCanNext)
          changed.emplace_back("CanGoNext");
        if (flipped & kCanPrevious)
          changed.emplace_back("CanGoPrevious");
        if (flipped & kCanSeek)
          changed.emplace_back("CanSeek");
        // Position is not announced as a property change; a jump is a Seeked.
        if (next.track == state_.track) {
          const int64_t expected = position_locked(now);
          const int64_t off = next.position_us - expected;
          seeked = off > kSeekSlackUs || off < -kSeekSlackUs;
          seek_position = next.position_us;
        }
      }
      state_ = next;
      at_ = now;
    }
    // Emitted outside the lock: a property getter the emission triggers takes
    // it.
    try {
      if (!changed.empty())
        object_->emitPropertiesChangedSignal(sdbus::InterfaceName{kPlayer},
                                             changed);
      if (seeked)
        object_->emitSignal("Seeked").onInterface(kPlayer).withArguments(
            seek_position);
    } catch (const sdbus::Error& e) {
      log::w(kTag, "signal not sent: {}", e.getMessage());
    }
    return true;
  }

  void clear() override {
    std::unique_ptr<sdbus::IObject> gone;
    std::string bus_name;
    {
      std::lock_guard<std::mutex> lk(m_);
      if (!exported_)
        return;
      exported_ = false;
      gone = std::move(object_);
      bus_name = std::move(bus_name_);
    }
    gone.reset();
    try {
      conn_->releaseName(sdbus::ServiceName{bus_name});
    } catch (const sdbus::Error& e) {
      log::w(kTag, "name not released: {}", e.getMessage());
    }
    log::i(kTag, "player left the bus ({})", bus_name);
  }

 private:
  // Takes the bus name and puts the object up. m_ is held.
  bool export_locked(const MprisState& first) {
    bus_name_ = std::string{kRoot} + "." + first.bus_suffix;
    try {
      conn_->requestName(sdbus::ServiceName{bus_name_});
    } catch (const sdbus::Error& e) {
      log::w(kTag, "bus name {} not taken: {}", bus_name_, e.getMessage());
      return false;
    }
    state_ = first;
    at_ = Clock::now();
    object_ = sdbus::createObject(*conn_, sdbus::ObjectPath{kPath});
    const auto none = [] { return std::vector<std::string>{}; };
    object_
        ->addVTable(
            sdbus::registerMethod("Raise").implementedAs([] {}),
            sdbus::registerMethod("Quit").implementedAs([] {}),
            sdbus::registerProperty("CanQuit").withGetter([] { return false; }),
            sdbus::registerProperty("CanRaise").withGetter([] {
              return false;
            }),
            sdbus::registerProperty("HasTrackList").withGetter([] {
              return false;
            }),
            sdbus::registerProperty("Identity").withGetter([this] {
              std::lock_guard<std::mutex> lk(m_);
              return state_.identity;
            }),
            sdbus::registerProperty("DesktopEntry").withGetter([this] {
              std::lock_guard<std::mutex> lk(m_);
              return state_.desktop_entry;
            }),
            sdbus::registerProperty("SupportedUriSchemes").withGetter(none),
            sdbus::registerProperty("SupportedMimeTypes").withGetter(none))
        .forInterface(sdbus::InterfaceName{kRoot});
    object_
        ->addVTable(
            sdbus::registerMethod("Play").implementedAs(
                [this] { send({MprisCommand::Kind::kPlay, 0}); }),
            sdbus::registerMethod("Pause").implementedAs(
                [this] { send({MprisCommand::Kind::kPause, 0}); }),
            sdbus::registerMethod("PlayPause").implementedAs([this] {
              send({MprisCommand::Kind::kPlayPause, 0});
            }),
            sdbus::registerMethod("Stop").implementedAs(
                [this] { send({MprisCommand::Kind::kStop, 0}); }),
            sdbus::registerMethod("Next").implementedAs(
                [this] { send({MprisCommand::Kind::kNext, 0}); }),
            sdbus::registerMethod("Previous").implementedAs([this] {
              send({MprisCommand::Kind::kPrevious, 0});
            }),
            sdbus::registerMethod("Seek").implementedAs([this](int64_t offset) {
              send({MprisCommand::Kind::kSeekBy, offset});
            }),
            sdbus::registerMethod("SetPosition")
                .implementedAs([this](const sdbus::ObjectPath& track,
                                      int64_t position) {
                  // A position for a track that is no longer the current one
                  // is stale, and the specification has it ignored.
                  {
                    std::lock_guard<std::mutex> lk(m_);
                    if (track != track_path_locked() || position < 0 ||
                        (state_.length_us > 0 && position > state_.length_us))
                      return;
                  }
                  send({MprisCommand::Kind::kSeekTo, position});
                }),
            sdbus::registerMethod("OpenUri").implementedAs(
                [](const std::string& /*uri*/) {}),
            sdbus::registerSignal("Seeked").withParameters<int64_t>(),
            sdbus::registerProperty("PlaybackStatus").withGetter([this] {
              std::lock_guard<std::mutex> lk(m_);
              return std::string{
                  state_.playback == Playback::kPlaying  ? "Playing"
                  : state_.playback == Playback::kPaused ? "Paused"
                                                         : "Stopped"};
            }),
            sdbus::registerProperty("Rate")
                .withGetter([this] {
                  std::lock_guard<std::mutex> lk(m_);
                  return state_.rate;
                })
                .withSetter([](double /*rate*/) {}),
            sdbus::registerProperty("Metadata").withGetter([this] {
              std::lock_guard<std::mutex> lk(m_);
              return metadata_locked();
            }),
            sdbus::registerProperty("Volume")
                .withGetter([] { return 1.0; })
                .withSetter([](double /*volume*/) {}),
            sdbus::registerProperty("Position").withGetter([this] {
              std::lock_guard<std::mutex> lk(m_);
              return position_locked(Clock::now());
            }),
            sdbus::registerProperty("MinimumRate").withGetter([] {
              return 1.0;
            }),
            sdbus::registerProperty("MaximumRate").withGetter([] {
              return 1.0;
            }),
            sdbus::registerProperty("CanGoNext").withGetter([this] {
              return can(kCanNext);
            }),
            sdbus::registerProperty("CanGoPrevious").withGetter([this] {
              return can(kCanPrevious);
            }),
            sdbus::registerProperty("CanPlay").withGetter(
                [this] { return can(kCanPlay); }),
            sdbus::registerProperty("CanPause").withGetter([this] {
              return can(kCanPause);
            }),
            sdbus::registerProperty("CanSeek").withGetter(
                [this] { return can(kCanSeek); }),
            sdbus::registerProperty("CanControl").withGetter([] {
              return true;
            }))
        .forInterface(sdbus::InterfaceName{kPlayer});
    exported_ = true;
    log::i(kTag, "player on the bus as {}", bus_name_);
    return true;
  }

  bool can(unsigned bit) {
    std::lock_guard<std::mutex> lk(m_);
    return (state_.can & bit) != 0;
  }

  sdbus::ObjectPath track_path_locked() const {
    return sdbus::ObjectPath{"/org/a4l/track/" + std::to_string(state_.track)};
  }

  // Where playback is now: the reported position, moved on by the time since
  // while playing, and held to the track's length.
  int64_t position_locked(Clock::time_point now) const {
    int64_t pos = state_.position_us;
    if (state_.playback == Playback::kPlaying) {
      const auto us =
          std::chrono::duration_cast<std::chrono::microseconds>(now - at_)
              .count();
      pos += static_cast<int64_t>(static_cast<double>(us) * state_.rate);
    }
    if (state_.length_us > 0 && pos > state_.length_us)
      pos = state_.length_us;
    return pos < 0 ? 0 : pos;
  }

  std::map<std::string, sdbus::Variant> metadata_locked() const {
    std::map<std::string, sdbus::Variant> md;
    md.emplace("mpris:trackid", sdbus::Variant{track_path_locked()});
    if (state_.length_us > 0)
      md.emplace("mpris:length", sdbus::Variant{state_.length_us});
    if (!state_.art_url.empty())
      md.emplace("mpris:artUrl", sdbus::Variant{state_.art_url});
    if (!state_.title.empty())
      md.emplace("xesam:title", sdbus::Variant{state_.title});
    if (!state_.artist.empty())
      md.emplace("xesam:artist",
                 sdbus::Variant{std::vector<std::string>{state_.artist}});
    if (!state_.album.empty())
      md.emplace("xesam:album", sdbus::Variant{state_.album});
    return md;
  }

  void send(const MprisCommand& command) {
    std::function<void(const MprisCommand&)> cb;
    {
      std::lock_guard<std::mutex> lk(m_);
      cb = sink_;
    }
    if (cb)
      cb(command);
  }

  std::unique_ptr<sdbus::IConnection> conn_;
  std::unique_ptr<sdbus::IObject> object_;
  std::mutex m_;
  std::function<void(const MprisCommand&)> sink_;
  MprisState state_;
  Clock::time_point at_;
  std::string bus_name_;
  bool exported_ = false;
};
#endif  // ACL_HAVE_SDBUS

}  // namespace

std::unique_ptr<IMprisBackend> make_mpris_backend() {
#if ACL_HAVE_SDBUS
  try {
    return std::make_unique<SessionBusBackend>(
        sdbus::createSessionBusConnection());
  } catch (const sdbus::Error& e) {
    log::w(kTag, "no session bus ({}); no media player on the desktop",
           e.getMessage());
  }
#endif
  return std::make_unique<NullBackend>();
}

}  // namespace acl::mpris
