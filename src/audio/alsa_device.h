// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l interface. No GPL-3.0 sources.
#ifndef ACL_AUDIO_ALSA_DEVICE_H
#define ACL_AUDIO_ALSA_DEVICE_H
// The ALSA backend (alsa_sink.cpp, alsa_source.cpp, alsa_volume.cpp). Only in a
// build with alsa-lib (ACL_HAVE_ALSA).
#include <memory>
#include <string>

#include "audio/audio.h"
#include "audio/audio_record.h"
#include "audio/pw_volume.h"

namespace acl::audio {

// The PCM both directions open: ACL_ALSA_DEVICE, or "default".
std::string alsa_device_name();

std::unique_ptr<Sink> make_alsa_sink();

// The capture PCM. Not gated: make_source() does that.
std::unique_ptr<Source> make_alsa_source();

// The master volume as one mixer control of that device's card
// (alsa_volume.cpp). Never null: with no control to set it reports full volume
// and available() false.
std::unique_ptr<IPwVolume> make_alsa_volume();

// A control's decibel value, in hundredths as alsa-lib gives it, as a linear
// amplitude with 1.0 at the top of the control's range, and back.
float alsa_linear_from_db(long db_hundredths, long max_hundredths);
long alsa_db_from_linear(float linear,
                         long min_hundredths,
                         long max_hundredths);

}  // namespace acl::audio

#endif  // ACL_AUDIO_ALSA_DEVICE_H
