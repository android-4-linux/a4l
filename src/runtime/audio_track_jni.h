// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.AudioTrack native contract from AOSP
// frameworks/base/media/java/android/media/AudioTrack.java (the Java-declared
// native method names and signatures, and the mNativeTrackInJavaObj field) and
// the SDK documentation for AudioTrack. No GPL-3.0 sources.
#ifndef ACL_RUNTIME_AUDIO_TRACK_JNI_H
#define ACL_RUNTIME_AUDIO_TRACK_JNI_H

#include <jni.h>

namespace acl::runtime {

// Register a4l's native side of the framework's android.media.AudioTrack, over
// audio::Track and audio::Sink. The host libandroid_runtime has none: it has
// no audio HAL to talk to. Harmless when the class is not on the classpath.
void register_audio_track_natives(JNIEnv* env);

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_AUDIO_TRACK_JNI_H
