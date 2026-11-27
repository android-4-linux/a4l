// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l interface. No GPL-3.0 sources.
#ifndef ACL_AUDIO_ALSA_DEVICE_H
#define ACL_AUDIO_ALSA_DEVICE_H
// The ALSA backend (alsa_sink.cpp, alsa_source.cpp). Only in a build with
// alsa-lib (ACL_HAVE_ALSA).
#include <memory>
#include <string>

#include "audio/audio.h"
#include "audio/audio_record.h"

namespace acl::audio {

// The PCM both directions open: ACL_ALSA_DEVICE, or "default".
std::string alsa_device_name();

std::unique_ptr<Sink> make_alsa_sink();

// The capture PCM. Not gated: make_source() does that.
std::unique_ptr<Source> make_alsa_source();

}  // namespace acl::audio

#endif  // ACL_AUDIO_ALSA_DEVICE_H
