<!-- SPDX-License-Identifier: Apache-2.0 -->
# A clip with sound through a media3 `PlayerView`

The app a4l is run against to answer "does a `SurfaceView` player show its video in step with
its sound": one Activity whose content is a media3 `PlayerView` playing a bundled clip. A
`PlayerView` shows video on a `SurfaceView` by default, which a4l puts on its video plane (a
subsurface below the window). `tests/fixtures/flutter_video_player` covers the other path, a
GL texture.

It is not built by the test suite: it needs the Android SDK, and Gradle fetches media3. Build
it by hand, as a Gradle project with `Main.java` under `app/src/main/java/com/acl/svp/`, the
clip as `app/src/main/assets/test.mp4`, and:

```kotlin
// app/build.gradle.kts
plugins { id("com.android.application") }
android {
    namespace = "com.acl.svp"
    compileSdk = 36
    defaultConfig { applicationId = "com.acl.svp"; minSdk = 24; targetSdk = 36 }
}
dependencies {
    implementation("androidx.media3:media3-exoplayer:1.5.1")
    implementation("androidx.media3:media3-ui:1.5.1")
}
```

The manifest declares `.Main` as the launcher Activity with
`android:theme="@android:style/Theme.Black.NoTitleBar"`. The clip and the measurement are the
ones in `tests/fixtures/flutter_video_player/README.md`, "Measuring picture against sound".

## What a run shows

- `maybeInjectVideoSurface: video SurfaceView surfaceCreated (1280x800 @0,0)`;
- `SVP: state 3`, `SVP: first frame`, then `SVP: position N ms playing true` twice a second
  to `SVP: state 4`;
- no `UnsatisfiedLinkError` and no `surfaceDestroyed`.

## Measured

2026-11-15, media3 1.5.1, host ART, x86-64, headless sway, null sink, the two-event clip.
Audio minus video in ms, positive when the sound is late; each a4l figure is one run's two
events:

| | ms |
|---|---|
| mpv, the reference, two runs | +26 +26, +24 +23 |
| a4l before | no picture: 0 flashes seen, 2 beeps heard |
| a4l, single process, five runs | +36 +47, +53 +49, +37 +49, +53 +48, +51 +46 |
| a4l, `--split`, four runs | +53 +49, +31 +43, +50 +47, +30 +27 |

So the picture is there, and the sound is about 20 ms later against it than the reference's,
single-process and under `--split`. A frame of this clip is 33 ms and the screen is sampled
every 16.

Before, the player had no picture at all. A `PlayerView` lays its `SurfaceView` out again once
it knows the picture's aspect ratio. a4l had marked the view as having a frame, so that layout
ran `SurfaceView.updateSurface`, which found no window surface (a4l has no `ViewRootImpl`
surface), told the player its surface was destroyed, and threw `UnsatisfiedLinkError` on
`Surface.nativeForceScopedDisconnect`: 1389 times in an 8 s clip, once per frame. The player
played on with nowhere to show its frames. a4l now leaves that mark unset and tells the app
the surface's new size itself. `tests/fixtures/video_plane` lays its `SurfaceView` out again
while it plays, and the `split-video` test fails on the old code.

## Pause, seek, speed

With `SVP_SCRIPT=1` in the environment, or an asset named `script` in the APK (a confined
`--split` app process does not get a4l's environment), the app pauses for 1.5 s at 1.0 s,
seeks from 6.5 s back to 2.0 s, and from 6.5 s back to 2.0 s again at twice the speed. The
clip's two events follow each, so a 30 s capture has six.

Measured 2026-11-15, audio minus video in ms, two events per run; mpv playing straight
through gave +14 +20 and +14 +22:

| | after a pause | after a seek | at twice the speed |
|---|---|---|---|
| `PlayerView`, single process, four runs | +30 +43, +40 +35, +44 +46, +53 +44 | +71 +68, +73 +70, +60 +57, +58 +55 | +48 +62, +64 +62, +44 +49, +62 +68 |
| `PlayerView`, `--split`, three runs | +51 +47, +41 +36, +41 +36 | +80 +73, +80 +76, +84 +79 | +61 +59, +70 +68, +67 +64 |

The figures after a seek are about 25 ms higher than after the pause. That is AAC's decoder
delay of one frame, which media3 trims at the start of a stream and not after a seek: the
same clip with FLAC audio (`ffmpeg -i test.mp4 -c:v copy -c:a flac -strict -2 flac.mp4`)
shows no such step. The ADR has the numbers.

Before: after the pause the sound came 1.5 s after the picture (+1525 for both events), and
after the seek the player stayed in buffering. a4l showed a `SurfaceView`'s frames on its own
clock, whatever the player released, and `MediaCodec.flush()` left the codec waiting on
input buffers the player no longer held. `docs/adr/ADR-0009-containment-process-boundary.md`,
"Pause, seek, speed", has the causes.

## On a compositor without the decoder's format

`ACL_VIDEO_CONVERT=1` makes a4l convert every frame to XBGR8888 on the GPU, as it does by
itself once a compositor has turned NV12 away. Measured 2026-11-16 with
`tools/measure-av-sync.py`, audio minus video in ms, the mean of a run's two events, three
runs each taken in turn; mpv gave +22 and +25:

| | as decoded | converted |
|---|---|---|
| single process | +34, +50, +28 | +42, +33, +40 |
| `--split` | +54, +61, +55 | +41, +55, +43 |

Conversion makes no difference that these runs can show: they differ by 20 ms among
themselves. One GPU (AMD, radeonsi), one clip (320x240).

Not covered: a `PlayerView` inside a Compose screen; a
`GLSurfaceView` that is laid out again, which takes the same mark in a4l's GL path and was not
changed here; seeking forward into data not yet read, or while paused; speeds other than 2.
