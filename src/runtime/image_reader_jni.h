// SPDX-License-Identifier: Apache-2.0
// Provenance: JNI Functions docs (RegisterNatives); android.media.ImageReader native contract from
// AOSP frameworks/base/media/java/android/media/ImageReader.java (the Java-declared native method
// names/signatures + the mNativeContext/mNativeBuffer fields it documents as "used by native code").
// The native underside of the REAL vendored android.media.ImageReader, over gfx::BufferQueue
// ([[flutter-texture-bridge]] M3c). No GPL-3.0 sources.
#ifndef ACL_RUNTIME_IMAGE_READER_JNI_H
#define ACL_RUNTIME_IMAGE_READER_JNI_H

#include <jni.h>

namespace acl::runtime {

// Register acl's native underside for android.media.ImageReader + its $SurfaceImage over a
// gfx::BufferQueue (dma-buf pool). No-op (and harmless) when the classes aren't on the classpath or
// GL is unavailable. Called from register_acl_supplemental_natives.
void register_image_reader_natives(JNIEnv* env);

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_IMAGE_READER_JNI_H
