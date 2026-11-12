// SPDX-License-Identifier: Apache-2.0
// Provenance: plan.md §A01.2 (media-app bridge), §A01.3 (Gate G1.5a); Android
// SDK MediaSession/PlaybackState docs; MPRIS2 org.mpris.MediaPlayer2.Player
// spec (freedesktop, public). No GPL-3.0 sources.
//
// The headless media-app bridge: publish/snapshot of the now-playing state
// (with a generation counter standing in for the ihs_shared seqlock) and a FIFO
// transport command channel (standing in for the SPSC ring). All state is
// mutex-guarded — single-process is the current model; the lock-free
// shared-memory form is a later swap behind the same interface.

#include "hosts/media/media_bridge.h"

namespace acl::hosts::media {

Playback playback_from_state(int state) noexcept {
  switch (state) {
    case 1:
      return Playback::Stopped;
    case 2:
      return Playback::Paused;
    case 3:
      return Playback::Playing;
    default:
      return Playback::None;  // STATE_NONE and any unknown value
  }
}

const char* playback_name(Playback p) noexcept {
  switch (p) {
    case Playback::Playing:
      return "Playing";
    case Playback::Paused:
      return "Paused";
    case Playback::Stopped:
      return "Stopped";
    case Playback::None:
      return "None";
  }
  return "None";
}

const char* command_name(Command c) noexcept {
  switch (c) {
    case Command::Play:
      return "Play";
    case Command::Pause:
      return "Pause";
    case Command::Stop:
      return "Stop";
    case Command::Next:
      return "Next";
    case Command::Previous:
      return "Previous";
  }
  return "?";
}

// The android.media.session.MediaSession.Callback method each command
// dispatches to.
const char* command_callback(Command c) noexcept {
  switch (c) {
    case Command::Play:
      return "onPlay";
    case Command::Pause:
      return "onPause";
    case Command::Stop:
      return "onStop";
    case Command::Next:
      return "onSkipToNext";
    case Command::Previous:
      return "onSkipToPrevious";
  }
  return "onStop";
}

void MediaBridge::publish(NowPlaying np) {
  std::lock_guard<std::mutex> lk(mu_);
  np.generation = ++gen_;
  now_ = std::move(np);
}

NowPlaying MediaBridge::snapshot() const {
  std::lock_guard<std::mutex> lk(mu_);
  return now_;
}

std::uint64_t MediaBridge::generation() const {
  std::lock_guard<std::mutex> lk(mu_);
  return gen_;
}

MprisMetadata MediaBridge::mpris() const {
  std::lock_guard<std::mutex> lk(mu_);
  MprisMetadata m;
  // MPRIS PlaybackStatus is one of Playing/Paused/Stopped; None maps to
  // Stopped.
  m.playback_status = now_.state == Playback::Playing  ? "Playing"
                      : now_.state == Playback::Paused ? "Paused"
                                                       : "Stopped";
  m.title = now_.title;
  m.artist = now_.artist;
  return m;
}

void MediaBridge::set_audio_route(AudioRoute* route) {
  std::lock_guard<std::mutex> lk(mu_);
  audio_ = route;
}

std::uint64_t MediaBridge::route_now_playing() {
  AudioRoute* route = nullptr;
  NowPlaying now;
  {
    std::lock_guard<std::mutex> lk(mu_);
    route = audio_;
    now = now_;
  }
  // Call the route outside the lock — play() may block for the playback
  // duration.
  return route != nullptr ? route->play(now) : 0;
}

void MediaBridge::publish_browse(std::string root_id,
                                 std::vector<std::string> items) {
  std::lock_guard<std::mutex> lk(mu_);
  browse_root_ = std::move(root_id);
  browse_items_ = std::move(items);
}

std::string MediaBridge::browse_root() const {
  std::lock_guard<std::mutex> lk(mu_);
  return browse_root_;
}

std::vector<std::string> MediaBridge::browse_items() const {
  std::lock_guard<std::mutex> lk(mu_);
  return browse_items_;
}

void MediaBridge::post_command(Command c) {
  std::lock_guard<std::mutex> lk(mu_);
  commands_.push_back(c);
}

bool MediaBridge::next_command(Command& out) {
  std::lock_guard<std::mutex> lk(mu_);
  if (commands_.empty())
    return false;
  out = commands_.front();
  commands_.pop_front();
  return true;
}

std::size_t MediaBridge::pending_commands() const {
  std::lock_guard<std::mutex> lk(mu_);
  return commands_.size();
}

MediaBridge& MediaBridge::instance() {
  static MediaBridge bridge;
  return bridge;
}

const char* module_name() noexcept {
  return "hosts/media";
}

}  // namespace acl::hosts::media
