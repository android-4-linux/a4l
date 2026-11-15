<!-- SPDX-License-Identifier: Apache-2.0 -->
# A clip with sound through `video_player` (media3 / ExoPlayer)

The app a4l is run against to answer "does a video play with sound": a Flutter app whose only
content is the `video_player` plugin playing a bundled clip. On Android the plugin is media3's
ExoPlayer, which parses the MP4 itself and uses `MediaCodecList`, `MediaCodec` (callback mode)
and `AudioTrack` for the rest. It logs the player's state twice a second as `ACLVID: ...`.

It is not built by the test suite: it needs a Flutter SDK and the Android SDK, and Gradle
fetches its dependencies. Build it by hand:

```sh
flutter create --platforms=android --org com.acl --project-name aclvid aclvid
cd aclvid
cp <repo>/tests/fixtures/flutter_video_player/main.dart lib/main.dart
rm -rf test
# pubspec.yaml: add `video_player: ^2.9.2` under dependencies, and under `flutter:`
#   assets:
#     - assets/test.mp4
mkdir assets
ffmpeg -f lavfi -i "testsrc2=s=320x240:r=30:d=5" \
       -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=5" \
       -c:v libx264 -pix_fmt yuv420p -profile:v baseline -bf 0 -g 30 \
       -c:a aac -b:a 128k -ac 2 -shortest assets/test.mp4
flutter build apk --debug --target-platform android-x64
```

Run it (`--present` is required; without it a4l takes the one-shot headless path):

```sh
ACL_PRESENT_GPU=1 tools/run-apk.sh build/app/outputs/flutter-apk/app-debug.apk --present
tools/run-apk.sh build/app/outputs/flutter-apk/app-debug.apk --verify=off --split --present
```

What to look for, in either mode:

- `native_start: decoder up (ASYNC)` for `video/avc` and
  `native_start: audio decoder up (ASYNC) for audio/mp4a-latm`;
- `ACLVID: initialized 320.0x240.0 duration 5000 ms`;
- `ACLVID: position N ms playing true`, N rising by about 500 between lines, to
  `position 5000 ms ... completed true`;
- `sink=null close: 240640 frames written` (235 AAC frames of 1024), or
  `sink=split close: 240640 frames sent` under `--split`.

With `ACL_AUDIO=pipewire` the tone is audible. To check it without a speaker, load a null sink
(`pactl load-module module-null-sink sink_name=a4l-test`), run with `PIPEWIRE_NODE=a4l-test`,
and record its monitor (`pw-record --target a4l-test -P stream.capture.sink=true out.wav`).

Measured 2026-11-15, Flutter 3.33.0-pre, `video_player` 2.x with media3, host ART, x86-64: the
clip plays to the end in both modes, the position advancing in real time; 240640 frames reach
the sink; the recording is 4.997 s of 440 Hz with no gap, the same single-process and under
`--split`; a screenshot during playback shows the test pattern.

## Measuring picture against sound

`tools/measure-av-sync.py` grabs a point of the screen and records a sink's monitor on one
clock and reports, for a clip that flashes and beeps at the same instants, how far each beep
is from its flash. Use a clip whose events cannot be mistaken for one another:

```sh
ffmpeg -f lavfi -i "color=c=0x101010:s=320x240:r=30:d=8,drawbox=x=0:y=0:w=320:h=240:color=white:t=fill:enable='between(t\,3\,3.199)+between(t\,5.5\,5.699)'" \
       -f lavfi -i "aevalsrc='0.5*sin(2*PI*1000*t)*(between(t\,3\,3.2)+between(t\,5.5\,5.7))':s=48000:d=8:c=stereo" \
       -c:v libx264 -pix_fmt yuv420p -profile:v baseline -bf 0 -g 30 -c:a aac -b:a 128k -shortest assets/test.mp4
```

On a headless sway (`WLR_BACKENDS=headless sway`) with a null sink
(`pactl load-module module-null-sink sink_name=a4l-test`):

```sh
tools/measure-av-sync.py wayland-1 a4l-test 24 640,400 a4l &
WAYLAND_DISPLAY=wayland-1 ACL_AUDIO=pipewire PIPEWIRE_NODE=a4l-test ACL_PRESENT_GPU=1 \
    tools/run-apk.sh app-debug.apk --present
```

The capture has a constant delay of its own in each path, so run a reference player the same
way and compare: `mpv --vo=gpu --gpu-context=wayland --ao=pipewire
--audio-device=pipewire/a4l-test --fs assets/test.mp4`.

Measured 2026-11-15, audio minus video, positive when the sound is late:

| | ms |
|---|---|
| mpv, the reference, four runs | +16, +18, +16, +18 |
| a4l before, single process | +277 to +342, and five flashes seen for two shown |
| a4l, single process, three runs of the two-event clip | +18, +26, +21 |
| a4l, single process, a clip with eight events | +26 |
| a4l, `--split`, five runs of the eight-event clip | +30, +24, +35, +44, +21 |

So a4l is within about 10 ms of the reference single-process and 25 ms under `--split`. The
screen is sampled every 16 ms, which is the resolution of each number.
