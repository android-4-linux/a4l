// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l. No GPL-3.0 sources.
//
// PCM input for native audio APIs (a4l's libOpenSLES.so). The shim library
// runs under bionic; the input is a4l's audio recorder and source, host code.
// These are the calls between them. libandroid.so holds the table
// (acl_audio_in_ops), the bionic runtime points it at trampolines that restore
// the host %fs, and the runtime supplies what the trampolines call.
//
// Plain C: compiled for bionic (C) and for the host (C++).
#ifndef ACL_NDK_BIONIC_ACL_AUDIO_IN_H_
#define ACL_NDK_BIONIC_ACL_AUDIO_IN_H_

#include <stdint.h>

struct acl_audio_in_ops {
  // An input of 16-bit PCM at `rate` Hz with `channels` channels. NULL if it
  // cannot be made.
  void* (*open)(int rate, int channels);
  void (*start)(void* in);
  // Reads `frames` frames and returns how many were read. Blocks until they
  // have all come; returns short, or 0, when the input is stopped or closed
  // meanwhile, and 0 at once while it is stopped.
  int (*read)(void* in, int16_t* pcm, int frames);
  // Stops, and lets go a read that is blocked.
  void (*stop)(void* in);
  // Not while another thread is in read.
  void (*close)(void* in);
};

#endif  // ACL_NDK_BIONIC_ACL_AUDIO_IN_H_
