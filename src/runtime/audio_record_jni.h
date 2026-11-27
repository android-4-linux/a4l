// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.AudioRecord native contract from AOSP
// frameworks/base/media/java/android/media/AudioRecord.java (the
// Java-declared native method names and signatures, and the
// mNativeAudioRecordHandle field) and the SDK documentation for AudioRecord.
// No GPL-3.0 sources.
#ifndef ACL_RUNTIME_AUDIO_RECORD_JNI_H
#define ACL_RUNTIME_AUDIO_RECORD_JNI_H

#include <jni.h>

namespace acl::runtime {

// Register a4l's native side of the framework's android.media.AudioRecord,
// over audio::Recorder. The host libandroid_runtime has none. Harmless when
// the class is not on the classpath.
void register_audio_record_natives(JNIEnv* env);

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_AUDIO_RECORD_JNI_H
