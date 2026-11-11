// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.SoundPool JNI seam (AOSP method table
// names/descriptors, Apache-2.0); FFmpeg decode + acl audio Sink. No GPL-3.0
// sources.
#ifndef ACL_AUDIO_SOUNDPOOL_H
#define ACL_AUDIO_SOUNDPOOL_H

#include <jni.h>

namespace acl::audio {

// RegisterNatives on the real framework's android.media.SoundPool (host-ART:
// the class ships but its libsoundpool.so native is absent). acl decodes
// samples with FFmpeg and mixes/plays them through the audio Sink. Best-effort;
// returns false if the class or registration is unavailable.
bool register_soundpool(JNIEnv* env);

}  // namespace acl::audio

#endif  // ACL_AUDIO_SOUNDPOOL_H
