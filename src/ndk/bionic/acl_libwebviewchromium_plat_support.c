// SPDX-License-Identifier: Apache-2.0
// Provenance: JNINativeMethod tables mirror AOSP frameworks/base/native/webview/plat_support
// (jni_entry_point/draw_functor/draw_gl_functor/graphics_utils, Apache-2.0). No GPL-3.0 sources.
//
// Staged as bionic libwebviewchromium_plat_support.so. The Chromium WebView provider
// (System.loadLibrary("webviewchromium_plat_support")) DT-needs this framework lib for the draw
// functor glue that, on a real device, bridges WebView's render to libhwui's RenderThread functor +
// GraphicBuffer. acl dropped hwui, so the REAL lib will not link here. This is a Stage-1 STUB: it
// registers the same six native entrypoints so the provider loads and WebView constructs, but the
// draw-function tables are null — actual painting is the render bridge (Stage 3: back
// GraphicsUtils.nativeGetDrawSWFunctionTable with a Skia software draw table). Until then the six
// return 0 / no-op, so a WebView inflates but does not yet paint web content.
#include <jni.h>

static jlong drawfunctor_getFunctionTable(JNIEnv* e, jclass c) { (void)e; (void)c; return 0; }
static jlong drawgl_create(JNIEnv* e, jclass c, jlong h) { (void)e; (void)c; (void)h; return 0; }
static void drawgl_destroy(JNIEnv* e, jclass c, jlong h) { (void)e; (void)c; (void)h; }
static void drawgl_setChromiumFn(JNIEnv* e, jclass c, jlong h) { (void)e; (void)c; (void)h; }
static jlong graphics_swTable(JNIEnv* e, jclass c) { (void)e; (void)c; return 0; }
static jlong graphics_glTable(JNIEnv* e, jclass c) { (void)e; (void)c; return 0; }

static void reg(JNIEnv* env, const char* cls, const JNINativeMethod* m, int n) {
  jclass c = (*env)->FindClass(env, cls);
  if (c != 0) (*env)->RegisterNatives(env, c, m, n);
  // A missing class leaves those natives unbound; the provider only touches them at draw time (Stage 3).
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)reserved;
  JNIEnv* env = 0;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK || env == 0)
    return JNI_VERSION_1_6;
  static const JNINativeMethod kDrawFunctor[] = {
      {"nativeGetFunctionTable", "()J", (void*)drawfunctor_getFunctionTable}};
  static const JNINativeMethod kDrawGLFunctor[] = {
      {"nativeCreateGLFunctor", "(J)J", (void*)drawgl_create},
      {"nativeDestroyGLFunctor", "(J)V", (void*)drawgl_destroy},
      {"nativeSetChromiumAwDrawGLFunction", "(J)V", (void*)drawgl_setChromiumFn}};
  static const JNINativeMethod kGraphicsUtils[] = {
      {"nativeGetDrawSWFunctionTable", "()J", (void*)graphics_swTable},
      {"nativeGetDrawGLFunctionTable", "()J", (void*)graphics_glTable}};
  reg(env, "com/android/webview/chromium/DrawFunctor", kDrawFunctor, 1);
  reg(env, "com/android/webview/chromium/DrawGLFunctor", kDrawGLFunctor, 3);
  reg(env, "com/android/webview/chromium/GraphicsUtils", kGraphicsUtils, 2);
  return JNI_VERSION_1_6;
}
