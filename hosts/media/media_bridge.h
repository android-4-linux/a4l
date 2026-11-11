// SPDX-License-Identifier: Apache-2.0
// Provenance: plan.md §A01.2 (media-app bridge), §A01.3 (Gate G1.5a); Android
// SDK MediaSession/PlaybackState docs (state constants, transport callbacks);
// MPRIS2 org.mpris.MediaPlayer2.Player spec (freedesktop, public). No GPL-3.0
// sources.
#ifndef ACL_HOSTS_MEDIA_MEDIA_BRIDGE_H
#define ACL_HOSTS_MEDIA_MEDIA_BRIDGE_H

// The headless media-app bridge (Gate G1.5a). MediaBrowserService/MediaSession
// apps draw no UI — the OEM shell does — so acl hosts them in-process (no
// Binder, ADR-3) and bridges their now-playing state + browse tree northbound,
// while routing transport commands (from an MPRIS controller or the shell) back
// to the app's MediaSession.Callback.
//
// This is the single-process form of the plan's data plane: publish() bumps a
// generation (the seqlock analogue — a lock-free shared-memory seqlock is the
// eventual ihs_shared form) and post_command()/next_command() are the transport
// command channel (the SPSC ring analogue). mpris() projects the now-playing
// state onto org.mpris.MediaPlayer2.Player so a stock Linux desktop gets
// controls for free once the D-Bus transport is bolted onto this seam.

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace acl::hosts::media {

// Mirrors android.media.session.PlaybackState.STATE_* (the values an app
// publishes).
enum class Playback : int { None = 0, Stopped = 1, Paused = 2, Playing = 3 };

// Map a raw PlaybackState.getState() int onto Playback (unknown values ->
// None).
Playback playback_from_state(int state) noexcept;
const char* playback_name(Playback p) noexcept;  // "Playing" / "Paused" / …

// Transport commands a controller (MPRIS / OEM shell) issues to the hosted app.
enum class Command { Play, Pause, Stop, Next, Previous };
const char* command_name(Command c) noexcept;  // "Play" …
const char* command_callback(
    Command c) noexcept;  // MediaSession.Callback method

// The northbound now-playing snapshot (what a UI / MPRIS controller reads).
struct NowPlaying {
  std::string title;
  std::string artist;
  std::string album;
  Playback state = Playback::None;
  long position_ms = 0;
  std::uint64_t generation = 0;  // bumped on each publish()
};

// org.mpris.MediaPlayer2.Player projection of the now-playing snapshot.
struct MprisMetadata {
  std::string
      playback_status;  // MPRIS PlaybackStatus: Playing / Paused / Stopped
  std::string title;    // xesam:title
  std::string artist;   // xesam:artist
};

// The device audio route for the bridge's now-playing audio. The concrete
// backend (acl::audio -> PipeWire) is injected by the composition root (the
// runtime), so hosts/media stays independent of the audio module and its build
// order. Real media apps feed decoded PCM via AudioTrack (which already routes
// to acl::audio); this is the seam by which the bridge itself drives playback
// to the device.
class AudioRoute {
 public:
  virtual ~AudioRoute() = default;
  // Play the current track; returns frames written (0 if nothing was played).
  virtual std::uint64_t play(const NowPlaying& now) = 0;
};

class MediaBridge {
 public:
  // Northbound publication (hosted app -> bridge). Assigns the next generation.
  void publish(NowPlaying np);
  NowPlaying snapshot() const;
  MprisMetadata mpris() const;
  std::uint64_t generation() const;

  // Install (or clear, with nullptr) the device audio route.
  void set_audio_route(AudioRoute* route);
  // Route the current now-playing audio to the device; returns frames written
  // (0 when no route is installed). Used when playback starts.
  std::uint64_t route_now_playing();

  // The app's browse tree (root id + child titles), published after
  // onLoadChildren.
  void publish_browse(std::string root_id, std::vector<std::string> items);
  std::string browse_root() const;
  std::vector<std::string> browse_items() const;

  // Transport command channel (controller -> app). FIFO; next_command drains
  // one, returning false when empty.
  void post_command(Command c);
  bool next_command(Command& out);
  std::size_t pending_commands() const;

  // Process-wide hub the runtime drives; tests construct their own instances.
  static MediaBridge& instance();

 private:
  mutable std::mutex mu_;
  NowPlaying now_{};
  std::string browse_root_;
  std::vector<std::string> browse_items_;
  std::deque<Command> commands_;
  std::uint64_t gen_ = 0;
  AudioRoute* audio_ = nullptr;
};

const char* module_name() noexcept;

}  // namespace acl::hosts::media

#endif  // ACL_HOSTS_MEDIA_MEDIA_BRIDGE_H
