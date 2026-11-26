// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.ToneGenerator native contract from AOSP
// frameworks/base/media/java/android/media/ToneGenerator.java (the
// Java-declared native method names and signatures, and the mNativeContext
// field) and the SDK documentation for ToneGenerator. No GPL-3.0 sources.
#ifndef ACL_RUNTIME_TONE_GENERATOR_JNI_H
#define ACL_RUNTIME_TONE_GENERATOR_JNI_H

#include <jni.h>

namespace acl::runtime {

// Register a4l's native side of the framework's android.media.ToneGenerator,
// over audio::TonePlayer. The host libandroid_runtime has none. Harmless when
// the class is not on the classpath.
void register_tone_generator_natives(JNIEnv* env);

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_TONE_GENERATOR_JNI_H
