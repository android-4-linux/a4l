// SPDX-License-Identifier: Apache-2.0
// Provenance: JNI Functions docs (RegisterNatives); android.media.ImageReader native contract from
// AOSP frameworks/base/media/java/android/media/ImageReader.java (the Java-declared native method
// names/signatures + the mNativeContext/mNativeBuffer fields it documents as "used by native code").
// The native underside of the REAL vendored android.media.ImageReader, over gfx::BufferQueue
// ([[flutter-texture-bridge]] M3c). No GPL-3.0 sources.
#ifndef ACL_RUNTIME_IMAGE_READER_JNI_H
#define ACL_RUNTIME_IMAGE_READER_JNI_H

#include <jni.h>

#include "acl_config.h"
#if ACL_HAVE_GL
#include "gfx/buffer_queue.h"
#endif

namespace acl::runtime {

// Register acl's native underside for android.media.ImageReader + its $SurfaceImage over a
// gfx::BufferQueue (dma-buf pool). No-op (and harmless) when the classes aren't on the classpath or
// GL is unavailable. Called from register_acl_supplemental_natives.
void register_image_reader_natives(JNIEnv* env);

#if ACL_HAVE_GL
// Resolve an android.view.Surface (created by ImageReader.getSurface) to its BufferQueue, or nullptr
// if it isn't a live acl ImageReader surface. The MediaCodec zero-copy output path uses this to
// queue_imported() decoded frames into the reader (releaseOutputBuffer(render=true)).
gfx::BufferQueue* image_reader_queue_from_surface(JNIEnv* env, jobject surface);

// Create a bare producer context (a ReaderCtx over a fresh BufferQueue, registered like an
// ImageReader's so image_reader_queue_from_surface accepts it) and return it as the native handle for
// a Surface.mNativeObject. Used by Surface.nativeCreateFromSurfaceTexture so MediaCodec has a valid,
// decodable output surface — ExoPlayer's PlaceholderSurface (a throwaway SurfaceTexture) needs this to
// initialize its video renderer. Decoded frames are queued (queue_imported, any size) and, with no
// consumer attached, auto-dropped; feeding them to the SurfaceTexture for display is a follow-on.
// Returns 0 on failure.
jlong image_reader_make_producer(JNIEnv* env, int w, int h);
#endif

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_IMAGE_READER_JNI_H
