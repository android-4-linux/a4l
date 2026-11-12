// SPDX-License-Identifier: Apache-2.0
// Provenance: plan.md §A01.3 (Gate G1.5a), §5 (Tier A unit); Android SDK
// PlaybackState/MediaSession docs; MPRIS2 spec (freedesktop, public). No
// GPL-3.0 sources.
//
// Tier-A unit tests for the headless media bridge (hosts/media).
// Automotive-profile only — the module is compiled out under the phone profile.

#include <catch2/catch_test_macros.hpp>

#include "hosts/media/media_bridge.h"

using namespace acl::hosts::media;

TEST_CASE("playback state mapping mirrors PlaybackState.STATE_*",
          "[media][automotive]") {
  CHECK(playback_from_state(0) == Playback::None);
  CHECK(playback_from_state(1) == Playback::Stopped);
  CHECK(playback_from_state(2) == Playback::Paused);
  CHECK(playback_from_state(3) == Playback::Playing);
  CHECK(playback_from_state(-1) == Playback::None);  // absent state
  CHECK(playback_from_state(99) == Playback::None);  // unknown value
}

TEST_CASE("publish bumps generation and snapshot reads it back",
          "[media][automotive]") {
  MediaBridge b;
  CHECK(b.generation() == 0);

  NowPlaying np;
  np.title = "Song One";
  np.artist = "The Artists";
  np.state = Playback::Playing;
  b.publish(np);

  const NowPlaying s = b.snapshot();
  CHECK(s.title == "Song One");
  CHECK(s.artist == "The Artists");
  CHECK(s.state == Playback::Playing);
  CHECK(s.generation == 1);
  CHECK(b.generation() == 1);

  b.publish(np);  // republish -> generation advances
  CHECK(b.generation() == 2);
  CHECK(b.snapshot().generation == 2);
}

TEST_CASE("mpris projection maps playback state to MPRIS PlaybackStatus",
          "[media][automotive]") {
  MediaBridge b;

  auto status_for = [&](Playback p) {
    NowPlaying np;
    np.title = "T";
    np.artist = "A";
    np.state = p;
    b.publish(np);
    return b.mpris();
  };

  CHECK(status_for(Playback::Playing).playback_status == "Playing");
  CHECK(status_for(Playback::Paused).playback_status == "Paused");
  CHECK(status_for(Playback::Stopped).playback_status == "Stopped");
  CHECK(status_for(Playback::None).playback_status ==
        "Stopped");  // None -> Stopped

  const MprisMetadata m = status_for(Playback::Playing);
  CHECK(m.title == "T");   // xesam:title
  CHECK(m.artist == "A");  // xesam:artist
}

TEST_CASE("transport command channel is an ordered FIFO",
          "[media][automotive]") {
  MediaBridge b;
  Command out;
  CHECK_FALSE(b.next_command(out));  // empty
  CHECK(b.pending_commands() == 0);

  b.post_command(Command::Play);
  b.post_command(Command::Next);
  b.post_command(Command::Pause);
  CHECK(b.pending_commands() == 3);

  REQUIRE(b.next_command(out));
  CHECK(out == Command::Play);
  REQUIRE(b.next_command(out));
  CHECK(out == Command::Next);
  REQUIRE(b.next_command(out));
  CHECK(out == Command::Pause);
  CHECK_FALSE(b.next_command(out));  // drained
  CHECK(b.pending_commands() == 0);
}

TEST_CASE("commands map to names and MediaSession.Callback methods",
          "[media][automotive]") {
  CHECK(std::string(command_name(Command::Play)) == "Play");
  CHECK(std::string(command_name(Command::Previous)) == "Previous");

  CHECK(std::string(command_callback(Command::Play)) == "onPlay");
  CHECK(std::string(command_callback(Command::Pause)) == "onPause");
  CHECK(std::string(command_callback(Command::Stop)) == "onStop");
  CHECK(std::string(command_callback(Command::Next)) == "onSkipToNext");
  CHECK(std::string(command_callback(Command::Previous)) == "onSkipToPrevious");
}

namespace {
// A fake device route that records the track it was asked to play.
class FakeRoute : public AudioRoute {
 public:
  std::uint64_t play(const NowPlaying& now) override {
    played_title = now.title;
    ++calls;
    return 4410;  // pretend frames
  }
  std::string played_title;
  int calls = 0;
};
}  // namespace

TEST_CASE("audio route is driven with the current now-playing",
          "[media][automotive]") {
  MediaBridge b;
  CHECK(b.route_now_playing() == 0);  // no route installed -> nothing played

  FakeRoute route;
  b.set_audio_route(&route);
  NowPlaying np;
  np.title = "Now Playing Track";
  np.state = Playback::Playing;
  b.publish(np);

  CHECK(b.route_now_playing() == 4410);
  CHECK(route.calls == 1);
  CHECK(route.played_title == "Now Playing Track");  // the published track

  b.set_audio_route(nullptr);  // clearing the route stops playback routing
  CHECK(b.route_now_playing() == 0);
  CHECK(route.calls == 1);
}

TEST_CASE("browse-tree snapshot round-trips", "[media][automotive]") {
  MediaBridge b;
  CHECK(b.browse_root().empty());
  CHECK(b.browse_items().empty());

  b.publish_browse("acl_root", {"Song One", "Song Two"});
  CHECK(b.browse_root() == "acl_root");
  REQUIRE(b.browse_items().size() == 2);
  CHECK(b.browse_items()[0] == "Song One");
  CHECK(b.browse_items()[1] == "Song Two");
}
