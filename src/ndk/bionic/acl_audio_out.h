// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l. No GPL-3.0 sources.
//
// PCM output for native audio APIs (a4l's libaaudio.so). The shim library runs
// under bionic; the output is a4l's audio track and sink, host code. These are
// the calls between them. libandroid.so holds the table (acl_audio_out_ops),
// the bionic runtime points it at trampolines that restore the host %fs, and
// the runtime supplies what the trampolines call.
//
// Plain C: compiled for bionic (C) and for the host (C++).
#ifndef ACL_NDK_BIONIC_ACL_AUDIO_OUT_H_
#define ACL_NDK_BIONIC_ACL_AUDIO_OUT_H_

#include <stdint.h>

struct acl_audio_out_ops {
  // An output of 16-bit PCM at `rate` Hz with `channels` channels. NULL if it
  // cannot be made.
  void* (*open)(int rate, int channels);
  // Queues `frames` frames and returns how many were taken. Blocks while the
  // output has no room; returns short when it is stopped or closed meanwhile.
  int (*write)(void* out, const int16_t* pcm, int frames);
  void (*start)(void* out);
  // Stops at once: what is queued is dropped.
  void (*stop)(void* out);
  // 1 and a frame position with its CLOCK_MONOTONIC time, or 0 while the
  // output has none to give.
  int (*timestamp)(void* out, int64_t* frames, int64_t* time_ns);
  void (*close)(void* out);
};

#endif  // ACL_NDK_BIONIC_ACL_AUDIO_OUT_H_
