// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.AudioBridge (backing an IAudioService
// shim) to the svc/audio host PipeWire master-volume controller. No GPL-3.0 sources.
#ifndef ACL_AUDIO_AUDIO_JNI_H
#define ACL_AUDIO_AUDIO_JNI_H

#include <jni.h>

namespace acl::audio {

// Create the PipeWire master-volume controller (make_pw_volume) and RegisterNatives on
// org.acl.internal.AudioBridge (stream volume get/set/mute over the default sink). Best-effort:
// on any failure the class keeps no native and AudioManager reads a fixed silent default. Safe
// to call once at host-ART boot, after the acl boot classpath is up.
void register_audio_natives(JNIEnv* env);

}  // namespace acl::audio

#endif  // ACL_AUDIO_AUDIO_JNI_H
