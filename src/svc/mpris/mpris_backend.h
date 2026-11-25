// SPDX-License-Identifier: Apache-2.0
// Provenance: original; the interface is the MPRIS D-Bus Interface
// Specification 2.2 (freedesktop.org). No GPL-3.0 sources.
//
// The app's media session on the desktop's media controls: an MPRIS player on
// the session bus. Android's analog is the media session service, which gives a
// session to the system's media controls, the lock screen and a car's media
// app.
#ifndef ACL_SVC_MPRIS_MPRIS_BACKEND_H
#define ACL_SVC_MPRIS_MPRIS_BACKEND_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace acl::mpris {

// What a session can be asked to do, as bits of MprisState::can.
inline constexpr unsigned kCanPlay = 1U << 0;
inline constexpr unsigned kCanPause = 1U << 1;
inline constexpr unsigned kCanNext = 1U << 2;
inline constexpr unsigned kCanPrevious = 1U << 3;
inline constexpr unsigned kCanSeek = 1U << 4;

enum class Playback { kStopped, kPaused, kPlaying };

struct MprisState {
  std::string identity;       // the app's label
  std::string desktop_entry;  // the desktop entry's name: the package
  std::string bus_suffix;     // org.mpris.MediaPlayer2.<bus_suffix>
  Playback playback = Playback::kStopped;
  std::string title;
  std::string artist;
  std::string album;
  std::string art_url;  // file:, http: or https:, or empty
  int64_t length_us = 0;
  int64_t position_us = 0;  // at the moment of the update
  double rate = 1.0;        // while playing
  unsigned can = 0;
  uint64_t track = 0;  // changes when the track does
};

struct MprisCommand {
  enum class Kind {
    kPlay,
    kPause,
    kPlayPause,
    kStop,
    kNext,
    kPrevious,
    kSeekBy,  // arg: offset in microseconds
    kSeekTo,  // arg: position in microseconds
  };
  Kind kind;
  int64_t arg = 0;
};

class IMprisBackend {
 public:
  virtual ~IMprisBackend() = default;
  virtual const char* name() const noexcept = 0;
  // Commands from the desktop, on the bus thread.
  virtual void set_sink(std::function<void(const MprisCommand&)> sink) = 0;
  // The session's state. The first call takes the bus name. True when the
  // player is on the bus.
  virtual bool update(const MprisState& state) = 0;
  // No session any more: the player leaves the bus.
  virtual void clear() = 0;
};

// The session-bus player, or one that does nothing where there is no bus or no
// sdbus-c++.
std::unique_ptr<IMprisBackend> make_mpris_backend();

}  // namespace acl::mpris

#endif  // ACL_SVC_MPRIS_MPRIS_BACKEND_H
