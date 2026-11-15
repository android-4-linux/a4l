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
