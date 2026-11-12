// SPDX-License-Identifier: Apache-2.0
// Provenance: android.opengl.GLUtils native surface — the semantics of AOSP
// frameworks/base/core/jni/android_opengl_GLUtils.cpp (Apache-2.0): upload an
// android.graphics.Bitmap into the currently-bound GL texture via glTexImage2D
// / glTexSubImage2D. acl reads the Bitmap's pixels through the graphics_jni
// acl_read_bitmap seam (BGRA_8888), swizzles to RGBA, and uploads on the
// process's host Mesa libGLESv2 (the same context
// com.google.android.gles_jni.EGLImpl makes current). Needed by apps that load
// GL textures from Bitmaps (e.g. Telegram's IntroActivity intro renderer). No
// GPL-3.0 sources.

#include <GLES2/gl2.h>
#include <jni.h>

#include <dlfcn.h>
#include <cstdint>
#include <vector>

namespace {

// graphics_jni seam: copy a Bitmap's pixels (BGRA_8888, unpremul) given its
// native handle (android.graphics.Bitmap.mNativePtr). Resolved once from the
// process (libacl_graphics_jni).
using ReadBitmapFn = bool (*)(long, std::uint32_t*, int, int);
ReadBitmapFn read_bitmap() {
  static auto f =
      reinterpret_cast<ReadBitmapFn>(dlsym(RTLD_DEFAULT, "acl_read_bitmap"));
  return f;
}

// Mixed Java-EGL/native-GL bridging: a SurfaceTexture producer's context is
// bound on the app's native-GL donor %fs, so this Java-side upload must run
// there too (else no current context). The thread is flagged donor-routed by
// egl10's eglMakeCurrent; run_on_donor keys the right donor.
using RouteGetFn = int (*)(void);
using RunOnDonorFn = void (*)(void (*)(void*), void*);
RouteGetFn route_get() {
  static auto f = reinterpret_cast<RouteGetFn>(
      dlsym(RTLD_DEFAULT, "acl_gl_donor_route_get"));
  return f;
}
RunOnDonorFn run_on_donor() {
  static auto f = reinterpret_cast<RunOnDonorFn>(
      dlsym(RTLD_DEFAULT, "acl_gl_run_on_donor"));
  return f;
}
struct TexImgArgs {
  GLenum target;
  GLint level;
  GLsizei w, h;
  GLint border;
  const void* px;
};
void do_tex_image(void* p) {
  auto* a = static_cast<TexImgArgs*>(p);
  glTexImage2D(a->target, a->level, GL_RGBA, a->w, a->h, a->border, GL_RGBA,
               GL_UNSIGNED_BYTE, a->px);
}
struct TexSubArgs {
  GLenum target;
  GLint level, xoff, yoff;
  GLsizei w, h;
  const void* px;
};
void do_tex_sub(void* p) {
  auto* a = static_cast<TexSubArgs*>(p);
  glTexSubImage2D(a->target, a->level, a->xoff, a->yoff, a->w, a->h, GL_RGBA,
                  GL_UNSIGNED_BYTE, a->px);
}

jfieldID g_bmp_ptr = nullptr;
jmethodID g_bmp_w = nullptr, g_bmp_h = nullptr;

// Read `bitmap` into `out` as tightly-packed, top-down RGBA_8888 (GL upload
// order). false on any gap.
bool bitmap_rgba(JNIEnv* env,
                 jobject bitmap,
                 std::vector<std::uint32_t>* out,
                 int* w,
                 int* h) {
  if (bitmap == nullptr)
    return false;
  if (g_bmp_ptr == nullptr) {
    jclass c = env->GetObjectClass(bitmap);
    g_bmp_ptr = env->GetFieldID(c, "mNativePtr", "J");
    g_bmp_w = env->GetMethodID(c, "getWidth", "()I");
    g_bmp_h = env->GetMethodID(c, "getHeight", "()I");
    env->DeleteLocalRef(c);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      g_bmp_ptr = nullptr;
      return false;
    }
  }
  const int bw = env->CallIntMethod(bitmap, g_bmp_w);
  const int bh = env->CallIntMethod(bitmap, g_bmp_h);
  const long handle = static_cast<long>(env->GetLongField(bitmap, g_bmp_ptr));
  if (bw <= 0 || bh <= 0 || handle == 0)
    return false;
  ReadBitmapFn rb = read_bitmap();
  if (rb == nullptr)
    return false;
  out->assign(static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh), 0);
  if (!rb(handle, out->data(), bw, bh))
    return false;
  // acl_read_bitmap yields BGRA (LE uint32 0xAARRGGBB); GL_RGBA/UNSIGNED_BYTE
  // wants memory R,G,B,A (0xAABBGGRR) — swap the R and B bytes, keep A and G.
  for (std::uint32_t& px : *out) {
    const std::uint32_t v = px;
    px = (v & 0xFF00FF00u) | ((v & 0x00FF0000u) >> 16) |
         ((v & 0x000000FFu) << 16);
  }
  *w = bw;
  *h = bh;
  return true;
}

// native_texImage2D(target, level, internalformat, bitmap, type, border).
// Always uploads the converted RGBA data as GL_RGBA/GL_UNSIGNED_BYTE
// (internalformat forced to GL_RGBA so it matches the format, as GLES2
// requires); the app's texcoords own orientation, so no flip (matches AOSP).
jint texImage2D(JNIEnv* env,
                jclass,
                jint target,
                jint level,
                jint /*internalformat*/,
                jobject bitmap,
                jint /*type*/,
                jint border) {
  std::vector<std::uint32_t> px;
  int w = 0, h = 0;
  if (!bitmap_rgba(env, bitmap, &px, &w, &h))
    return -1;  // GLUtils treats nonzero as error
  RouteGetFn rg = route_get();
  RunOnDonorFn rd = run_on_donor();
  if (rg != nullptr && rd != nullptr && rg()) {
    TexImgArgs a{static_cast<GLenum>(target), level, w, h, border, px.data()};
    rd(do_tex_image, &a);
  } else {
    glTexImage2D(static_cast<GLenum>(target), level, GL_RGBA, w, h, border,
                 GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  }
  return 0;
}

jint texSubImage2D(JNIEnv* env,
                   jclass,
                   jint target,
                   jint level,
                   jint xoffset,
                   jint yoffset,
                   jobject bitmap,
                   jint /*format*/,
                   jint /*type*/) {
  std::vector<std::uint32_t> px;
  int w = 0, h = 0;
  if (!bitmap_rgba(env, bitmap, &px, &w, &h))
    return -1;
  RouteGetFn rg = route_get();
  RunOnDonorFn rd = run_on_donor();
  if (rg != nullptr && rd != nullptr && rg()) {
    TexSubArgs a{
        static_cast<GLenum>(target), level, xoffset, yoffset, w, h, px.data()};
    rd(do_tex_sub, &a);
  } else {
    glTexSubImage2D(static_cast<GLenum>(target), level, xoffset, yoffset, w, h,
                    GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  }
  return 0;
}

jint getInternalFormat(JNIEnv*, jclass, jobject) {
  return GL_RGBA;
}
jint getType(JNIEnv*, jclass, jobject) {
  return GL_UNSIGNED_BYTE;
}

}  // namespace

extern "C" int register_android_opengl_GLUtils(JNIEnv* env) {
  jclass c = env->FindClass("android/opengl/GLUtils");
  if (c == nullptr) {
    env->ExceptionClear();
    return JNI_ERR;
  }
  const JNINativeMethod m[] = {
      {const_cast<char*>("native_texImage2D"),
       const_cast<char*>("(IIILandroid/graphics/Bitmap;II)I"),
       reinterpret_cast<void*>(texImage2D)},
      {const_cast<char*>("native_texSubImage2D"),
       const_cast<char*>("(IIIILandroid/graphics/Bitmap;II)I"),
       reinterpret_cast<void*>(texSubImage2D)},
      {const_cast<char*>("native_getInternalFormat"),
       const_cast<char*>("(Landroid/graphics/Bitmap;)I"),
       reinterpret_cast<void*>(getInternalFormat)},
      {const_cast<char*>("native_getType"),
       const_cast<char*>("(Landroid/graphics/Bitmap;)I"),
       reinterpret_cast<void*>(getType)},
  };
  const int rc = env->RegisterNatives(c, m, 4);
  env->DeleteLocalRef(c);
  return rc == 0 ? 0 : JNI_ERR;
}
