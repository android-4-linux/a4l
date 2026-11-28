// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; the public EGL 1.4, OpenGL ES 2.0 and
// NDK android/native_window.h ABIs. No GPL-3.0 sources.
//
// The engine half of tests/fixtures/native_egl_under_views/Main.java: what a
// Flutter engine does with EGL when an app opens a Flutter screen from its own
// views. On a thread of its own it asks the default display for an offscreen
// (pbuffer) config and a window config, makes a 1x1 pbuffer current on a
// context of its own, then draws to a window surface made from the
// SurfaceView's ANativeWindow: green on the left, blue on the right, a light
// band along the bottom.
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLContext;
typedef void* EGLSurface;
typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef struct ANativeWindow ANativeWindow;

extern ANativeWindow* ANativeWindow_fromSurface(JNIEnv*, jobject);
extern EGLDisplay eglGetDisplay(void*);
extern EGLBoolean eglInitialize(EGLDisplay, EGLint*, EGLint*);
extern EGLBoolean eglChooseConfig(EGLDisplay,
                                  const EGLint*,
                                  EGLConfig*,
                                  EGLint,
                                  EGLint*);
extern EGLContext eglCreateContext(EGLDisplay,
                                   EGLConfig,
                                   EGLContext,
                                   const EGLint*);
extern EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint*);
extern EGLSurface eglCreateWindowSurface(EGLDisplay,
                                         EGLConfig,
                                         void*,
                                         const EGLint*);
extern EGLBoolean eglMakeCurrent(EGLDisplay,
                                 EGLSurface,
                                 EGLSurface,
                                 EGLContext);
extern EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface);
extern EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface);
extern EGLint eglGetError(void);
extern void glViewport(int, int, int, int);
extern void glEnable(unsigned int);
extern void glScissor(int, int, int, int);
extern void glClearColor(float, float, float, float);
extern void glClear(unsigned int);

enum {
  kEglNone = 0x3038,
  kEglSurfaceType = 0x3033,
  kEglPbufferBit = 0x0001,
  kEglWindowBit = 0x0004,
  kEglRenderableType = 0x3040,
  kEglOpenGlEs2Bit = 0x0004,
  kEglRedSize = 0x3024,
  kEglGreenSize = 0x3023,
  kEglBlueSize = 0x3022,
  kEglWidth = 0x3057,
  kEglHeight = 0x3056,
  kEglContextClientVersion = 0x3098,
  kGlScissorTest = 0x0C11,
  kGlColorBufferBit = 0x4000,
};

static pthread_t g_thread;
static volatile int g_running;
static ANativeWindow* g_window;
static int g_width, g_height;

static void say(const char* what) {
  __android_log_print(ANDROID_LOG_INFO, "acl", "NATIVEEGL: %s", what);
}

static void* failed(const char* step) {
  __android_log_print(ANDROID_LOG_INFO, "acl",
                      "NATIVEEGL: FAILED at %s (error 0x%x)", step,
                      eglGetError());
  return 0;
}

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void* render(void* unused) {
  (void)unused;
  EGLDisplay d = eglGetDisplay(0);
  if (d == 0)
    return failed("eglGetDisplay");
  if (!eglInitialize(d, 0, 0))
    return failed("eglInitialize");

  // The offscreen surface an engine keeps a context current on while it has
  // no window.
  const EGLint offscreen[] = {kEglRenderableType,
                              kEglOpenGlEs2Bit,
                              kEglSurfaceType,
                              kEglPbufferBit,
                              kEglRedSize,
                              8,
                              kEglGreenSize,
                              8,
                              kEglBlueSize,
                              8,
                              kEglNone};
  const EGLint onscreen[] = {kEglRenderableType,
                             kEglOpenGlEs2Bit,
                             kEglSurfaceType,
                             kEglWindowBit,
                             kEglRedSize,
                             8,
                             kEglGreenSize,
                             8,
                             kEglBlueSize,
                             8,
                             kEglNone};
  const EGLint version[] = {kEglContextClientVersion, 2, kEglNone};
  const EGLint one_by_one[] = {kEglWidth, 1, kEglHeight, 1, kEglNone};
  EGLConfig off_cfg = 0, on_cfg = 0;
  EGLint n = 0;
  if (!eglChooseConfig(d, offscreen, &off_cfg, 1, &n) || n < 1)
    return failed("eglChooseConfig for a pbuffer");
  if (!eglChooseConfig(d, onscreen, &on_cfg, 1, &n) || n < 1)
    return failed("eglChooseConfig for a window");
  EGLContext off_ctx = eglCreateContext(d, off_cfg, 0, version);
  if (off_ctx == 0)
    return failed("eglCreateContext for the pbuffer");
  EGLSurface pbuffer = eglCreatePbufferSurface(d, off_cfg, one_by_one);
  if (pbuffer == 0)
    return failed("eglCreatePbufferSurface");
  if (!eglMakeCurrent(d, pbuffer, pbuffer, off_ctx))
    return failed("eglMakeCurrent on the pbuffer");
  say("pbuffer current");

  EGLContext ctx = eglCreateContext(d, on_cfg, off_ctx, version);
  if (ctx == 0)
    return failed("eglCreateContext for the window");
  EGLSurface s = eglCreateWindowSurface(d, on_cfg, g_window, 0);
  if (s == 0)
    return failed("eglCreateWindowSurface");
  if (!eglMakeCurrent(d, s, s, ctx))
    return failed("eglMakeCurrent on the window");
  say("window surface made");

  // An engine makes its window surface again when the view is given its
  // size: it unbinds, destroys the one it has and makes another, which the
  // driver is free to give the handle the first one had.
  for (int round = 0; round < 8; ++round) {
    eglMakeCurrent(d, 0, 0, 0);
    if (!eglDestroySurface(d, s))
      return failed("eglDestroySurface");
    s = eglCreateWindowSurface(d, on_cfg, g_window, 0);
    if (s == 0)
      return failed("eglCreateWindowSurface, again");
    if (!eglMakeCurrent(d, s, s, ctx))
      return failed("eglMakeCurrent on the window made again");
  }
  say("window surface made again 8 times");

  const long start = now_ms();
  int frames = 0;
  while (g_running) {
    const int w = g_width, h = g_height;
    glViewport(0, 0, w, h);
    glEnable(kGlScissorTest);
    glScissor(0, 0, w / 2, h);
    glClearColor(0x20 / 255.0f, 0xC0 / 255.0f, 0x20 / 255.0f, 1.0f);
    glClear(kGlColorBufferBit);
    glScissor(w / 2, 0, w - w / 2, h);
    glClearColor(0x20 / 255.0f, 0x20 / 255.0f, 0xC0 / 255.0f, 1.0f);
    glClear(kGlColorBufferBit);
    glScissor(0, 0, w, h / 4);  // GL's y runs up: this is the bottom
    glClearColor(0xE0 / 255.0f, 0xE0 / 255.0f, 0xE0 / 255.0f, 1.0f);
    glClear(kGlColorBufferBit);
    if (!eglSwapBuffers(d, s))
      return failed("eglSwapBuffers");
    if (++frames == 120)
      __android_log_print(ANDROID_LOG_INFO, "acl",
                          "NATIVEEGL: 120 frames in %ld ms", now_ms() - start);
  }
  eglMakeCurrent(d, 0, 0, 0);
  say("render thread ended");
  return 0;
}

JNIEXPORT jboolean JNICALL Java_com_acl_nativeegl_Main_start(JNIEnv* env,
                                                             jclass clazz,
                                                             jobject surface,
                                                             jint width,
                                                             jint height) {
  (void)clazz;
  if (g_running)
    return JNI_TRUE;
  g_window = ANativeWindow_fromSurface(env, surface);
  if (g_window == 0) {
    say("FAILED at ANativeWindow_fromSurface");
    return JNI_FALSE;
  }
  g_width = width;
  g_height = height;
  g_running = 1;
  if (pthread_create(&g_thread, 0, render, 0) != 0) {
    g_running = 0;
    say("FAILED at pthread_create");
    return JNI_FALSE;
  }
  return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_com_acl_nativeegl_Main_stop(JNIEnv* env,
                                                        jclass clazz) {
  (void)env;
  (void)clazz;
  if (!g_running)
    return;
  g_running = 0;
  pthread_join(g_thread, 0);
}
