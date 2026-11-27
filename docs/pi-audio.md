<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original a4l documentation; the ALSA PCM API (alsa-lib). No GPL-3.0 sources. -->

# Audio on a Raspberry Pi 4 or 5, and other systems with no PipeWire

a4l plays and records through PipeWire where a PipeWire daemon runs, and through ALSA where
none does. A Pi that boots to a console has ALSA and, unless it was installed, no PipeWire.

`ACL_AUDIO` selects the device backend for `AudioTrack`, `AudioRecord`, AAudio and OpenSL ES:

| `ACL_AUDIO` | Backend |
|---|---|
| unset, `null` | none: samples are counted and dropped, a recording is silence |
| `pipewire` | PipeWire when a4l was built with it and the daemon's socket is there; ALSA otherwise |
| `alsa` | ALSA |

So `ACL_AUDIO=pipewire` means "the audio device" on both kinds of system, and a4l logs which it
took: `sink=pipewire open` or `sink=alsa open`, the latter after
`no PipeWire daemon; audio goes to ALSA` or `built without PipeWire; audio goes to ALSA`.

The ALSA device is `default`. `ACL_ALSA_DEVICE` names another, for playback and capture both:

```sh
ACL_AUDIO=pipewire a4l run app.apk --present            # the default device
ACL_AUDIO=pipewire ACL_ALSA_DEVICE=plughw:1,0 a4l run app.apk --present
```

`aplay -L` lists the names. A `hw:` device takes only the rates and channel counts the
hardware has; `default` and `plughw:` convert.

To hear that it works, without an app:

```sh
ACL_AUDIO=alsa a4l tone --seconds=1
```

## Build

a4l needs alsa-lib's headers at build time (`libasound2-dev` on Debian, `alsa-lib-devel` on
Fedora); meson then defines `ACL_HAVE_ALSA`. A build with neither PipeWire nor ALSA has no
audio device.

## What was checked, and what was not

On a Raspberry Pi 4 (Debian 13, the bcm2835 headphone device, no PipeWire installed), with a
cross-built a4l that has ALSA and no PipeWire: `a4l tone` of 0.05 s, 0.5 s and 2 s ends in
0.12 s, 0.53 s and 2.04 s on `default`; the native-app tests' AAudio and OpenSL ES modes pass
with `ACL_AUDIO=alsa`.

Not checked: that the tone was heard (nobody was listening to the jack), capture from a
microphone (the board has none), HDMI audio, and a Pi 5.

With ALSA only, one process at a time can usually hold a hardware device unless the
configuration mixes (`dmix`). The system volume an app sets through `AudioManager` is bound
to PipeWire's default sink; on ALSA it is not connected to the mixer.
