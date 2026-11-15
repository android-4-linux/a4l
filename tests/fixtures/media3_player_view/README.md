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

Not covered: seeking, pausing, speed; a `PlayerView` inside a Compose screen; a
`GLSurfaceView` that is laid out again, which takes the same mark in a4l's GL path and was not
changed or tested here.
