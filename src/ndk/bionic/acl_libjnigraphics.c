// SPDX-License-Identifier: Apache-2.0
// Provenance: signatures from the NDK <android/bitmap.h> stable API (AOSP,
// Apache-2.0). No GPL-3.0 sources.
//
// Staged as bionic libjnigraphics.so. A native lib DT_NEEDs it for the three
// AndroidBitmap_* entrypoints, which lock a java.graphics.Bitmap's pixels for
// direct native access (image codecs, blurs, QR scanners). Telegram's
// Utilities.stackBlurBitmap is the motivating case: it locks a downsampled
// Bitmap, blurs it in place, and unlocks — with a stub lock (returning
// BAD_PARAMETER) the blur never runs, so the un-blurred downsampled image is
// upscaled = blocky/splotchy.
//
// The pixels live in acl's host-side SkBitmap (a different linker namespace /
// %fs from this bionic shim), so rather than reach across the boundary we go
// through the app's real JNIEnv (already bridged by the NDK loader): lockPixels
// copies the pixels OUT via Bitmap.getPixels into a heap buffer the caller
// mutates; unlockPixels copies them BACK via Bitmap.setPixels (acl's
// nativeSetPixels). getInfo reports width/height/rowBytes so the caller strides
// correctly. This is a copy-emulated lock (fine for the small downsampled
// bitmaps these paths use); the buffer holds unpremultiplied Color ints, which
// a channel-agnostic filter (blur) round-trips correctly.
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

// Mirrors <android/bitmap.h> so callers link cleanly.
typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  int32_t format;
  uint32_t flags;
} AndroidBitmapInfo;

#define ANDROID_BITMAP_RESULT_SUCCESS 0
#define ANDROID_BITMAP_RESULT_BAD_PARAMETER (-1)
#define ANDROID_BITMAP_FORMAT_NONE 0
#define ANDROID_BITMAP_FORMAT_RGBA_8888 1
#define ANDROID_BITMAP_FORMAT_RGB_565 4
#define ANDROID_BITMAP_FORMAT_A_8 8

#define EXPORT __attribute__((visibility("default")))

// Registry of locked bitmaps: the copy-out buffer + the geometry to copy back
// on unlock. Keyed by Bitmap.mNativePtr (stable across the paired lock/unlock
// calls on the same Bitmap).
typedef struct LockEntry {
  jlong key;
  void* buf;
  int w, h;
  struct LockEntry* next;
} LockEntry;
static LockEntry* g_locks = 0;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static int bmp_int(JNIEnv* env, jobject b, const char* name) {
  jclass c = (*env)->GetObjectClass(env, b);
  jmethodID m = (*env)->GetMethodID(env, c, name, "()I");
  (*env)->DeleteLocalRef(env, c);
  if (!m) {
    (*env)->ExceptionClear(env);
    return -1;
  }
  int v = (*env)->CallIntMethod(env, b, m);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionClear(env);
    return -1;
  }
  return v;
}

static jlong bmp_native_ptr(JNIEnv* env, jobject b) {
  jclass c = (*env)->GetObjectClass(env, b);
  jfieldID f = (*env)->GetFieldID(env, c, "mNativePtr", "J");
  (*env)->DeleteLocalRef(env, c);
  if (!f) {
    (*env)->ExceptionClear(env);
    return 0;
  }
  return (*env)->GetLongField(env, b, f);
}

EXPORT int AndroidBitmap_getInfo(JNIEnv* env,
                                 jobject bmp,
                                 AndroidBitmapInfo* info) {
  if (!env || !bmp || !info)
    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  int w = bmp_int(env, bmp, "getWidth");
  int h = bmp_int(env, bmp, "getHeight");
  int rb = bmp_int(env, bmp, "getRowBytes");
  if (w <= 0 || h <= 0 || rb <= 0)
    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  info->width = (uint32_t)w;
  info->height = (uint32_t)h;
  info->stride = (uint32_t)rb;
  int bpp = rb / w;
  info->format = (bpp >= 4)   ? ANDROID_BITMAP_FORMAT_RGBA_8888
                 : (bpp == 2) ? ANDROID_BITMAP_FORMAT_RGB_565
                 : (bpp == 1) ? ANDROID_BITMAP_FORMAT_A_8
                              : ANDROID_BITMAP_FORMAT_NONE;
  info->flags = 0;
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

EXPORT int AndroidBitmap_lockPixels(JNIEnv* env, jobject bmp, void** addr) {
  if (!env || !bmp || !addr)
    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  *addr = 0;
  int w = bmp_int(env, bmp, "getWidth");
  int h = bmp_int(env, bmp, "getHeight");
  if (w <= 0 || h <= 0)
    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  const jsize n = (jsize)((size_t)w * (size_t)h);
  jintArray arr = (*env)->NewIntArray(env, n);
  if (!arr) {
    (*env)->ExceptionClear(env);
    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  }
  jclass c = (*env)->GetObjectClass(env, bmp);
  jmethodID mGet = (*env)->GetMethodID(env, c, "getPixels", "([IIIIIII)V");
  (*env)->DeleteLocalRef(env, c);
  if (!mGet) {
    (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, arr);
    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  }
  (*env)->CallVoidMethod(env, bmp, mGet, arr, 0, w, 0, 0, w,
                         h);  // getPixels(arr,0,w,0,0,w,h)
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, arr);
    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  }
  void* buf = malloc((size_t)n * 4u);
  if (!buf) {
    (*env)->DeleteLocalRef(env, arr);
    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  }
  (*env)->GetIntArrayRegion(env, arr, 0, n, (jint*)buf);
  (*env)->DeleteLocalRef(env, arr);
  LockEntry* e = (LockEntry*)malloc(sizeof(LockEntry));
  if (!e) {
    free(buf);
    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  }
  e->key = bmp_native_ptr(env, bmp);
  e->buf = buf;
  e->w = w;
  e->h = h;
  pthread_mutex_lock(&g_mu);
  e->next = g_locks;
  g_locks = e;
  pthread_mutex_unlock(&g_mu);
  *addr = buf;
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

EXPORT int AndroidBitmap_unlockPixels(JNIEnv* env, jobject bmp) {
  if (!env || !bmp)
    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  const jlong key = bmp_native_ptr(env, bmp);
  LockEntry* e = 0;
  LockEntry** pp;
  pthread_mutex_lock(&g_mu);
  for (pp = &g_locks; *pp; pp = &(*pp)->next) {
    if ((*pp)->key == key) {
      e = *pp;
      *pp = e->next;
      break;
    }
  }
  pthread_mutex_unlock(&g_mu);
  if (!e)
    return ANDROID_BITMAP_RESULT_SUCCESS;  // never locked (or already
                                           // committed)
  const jsize n = (jsize)((size_t)e->w * (size_t)e->h);
  jintArray arr = (*env)->NewIntArray(env, n);
  if (arr) {
    (*env)->SetIntArrayRegion(env, arr, 0, n, (const jint*)e->buf);
    jclass c = (*env)->GetObjectClass(env, bmp);
    jmethodID mSet = (*env)->GetMethodID(env, c, "setPixels", "([IIIIIII)V");
    if (mSet) {
      (*env)->CallVoidMethod(env, bmp, mSet, arr, 0, e->w, 0, 0, e->w,
                             e->h);  // setPixels(arr,0,w,0,0,w,h)
      if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);  // immutable bitmap -> drop the write
    }
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, arr);
  }
  free(e->buf);
  free(e);
  return ANDROID_BITMAP_RESULT_SUCCESS;
}
