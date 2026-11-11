// SPDX-License-Identifier: Apache-2.0
// Provenance: Khronos EGL 1.4 spec + EGL/egl.h; the JNI contract of AOSP
// android.opengl.EGL14 (frameworks/base/core/jni/android_opengl_EGL14.cpp —
// behavioral: the method set, signatures, and the
// EGLDisplay/EGLContext/EGLSurface/EGLConfig wrapper-object handle model, each
// Java object holding a `long` native handle via getNativeHandle()).
// Re-authored (not copied) as thin passthroughs to host Mesa libEGL. No GPL-3.0
// sources.
#pragma once

#include "acl_config.h"

#if ACL_HAVE_GL

#include <jni.h>

namespace acl::gfx {

// Register android.opengl.EGL14 native methods on host Mesa libEGL.
// media3/ExoPlayer's PlaceholderSurface builds an off-screen pbuffer GL context
// through EGL14 during video-renderer init; without these natives its <clinit>
// UnsatisfiedLinkErrors and the whole video path dies. Returns JNI_OK on
// success. Compiled only when ACL_HAVE_GL and JNI are present.
jint register_egl14(JNIEnv* env);

}  // namespace acl::gfx

#endif  // ACL_HAVE_GL
