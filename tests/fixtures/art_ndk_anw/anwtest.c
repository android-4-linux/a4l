// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl test fixture; the public NDK android/native_window.h ABI
// (ANativeWindow_fromSurface/lock/unlockAndPost + ANativeWindow_Buffer layout). No GPL-3.0 sources.
//
// A bionic-ABI (x86_64-linux-android) NDK library: the producer half of the ANativeWindow
// end-to-end (tests/fixtures/art_ndk_anw/Main.java). It links acl's staged libandroid.so and calls
// the real ANativeWindow C ABI on the ImageReader producer Surface, so acl's ANativeWindow_* (which
// resolve the Surface -> BufferQueue and cross the bionic->host %fs boundary) are exercised exactly
// as a MediaCodec/GL producer would exercise them.
#include <android/log.h>
#include <jni.h>
#include <stdint.h>
#include <string.h>

typedef struct ANativeWindow ANativeWindow;
// The NDK ANativeWindow_Buffer ABI — a producer writes `bits` (stride is in PIXELS) after lock().
typedef struct {
  int32_t width;
  int32_t height;
  int32_t stride;
  int32_t format;
  void* bits;
  uint32_t reserved[6];
} ANativeWindow_Buffer;
extern ANativeWindow* ANativeWindow_fromSurface(JNIEnv*, jobject);
extern int32_t ANativeWindow_lock(ANativeWindow*, ANativeWindow_Buffer*, void* /*ARect* dirty*/);
extern int32_t ANativeWindow_unlockAndPost(ANativeWindow*);

// One producer frame: resolve the Surface, dequeue+CPU-map the next pooled buffer, fill it, and
// queue it. Returns the locked width (64) so the Java side proves the map, or a negative step code.
JNIEXPORT jint JNICALL
Java_com_acl_anwtest_Main_produce(JNIEnv* env, jclass clazz, jobject surface, jint color) {
  (void)clazz;
  ANativeWindow* w = ANativeWindow_fromSurface(env, surface);
  if (!w) {
    __android_log_print(ANDROID_LOG_ERROR, "anwtest", "ANativeWindow_fromSurface returned NULL");
    return -1;
  }
  ANativeWindow_Buffer b;
  memset(&b, 0, sizeof(b));
  int rc = ANativeWindow_lock(w, &b, NULL);
  if (rc != 0) {
    __android_log_print(ANDROID_LOG_ERROR, "anwtest", "ANativeWindow_lock rc=%d", rc);
    return -2;
  }
  // Fill the whole buffer with `color` (ABGR8888 byte order R,G,B,A). stride is in pixels; 4 bytes/px.
  uint8_t px[4] = {(uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color, (uint8_t)(color >> 24)};
  for (int32_t y = 0; y < b.height; y++) {
    uint8_t* row = (uint8_t*)b.bits + (size_t)y * (size_t)b.stride * 4;
    for (int32_t x = 0; x < b.width; x++) {
      row[0] = px[0];
      row[1] = px[1];
      row[2] = px[2];
      row[3] = px[3];
      row += 4;
    }
  }
  int locked_w = b.width;
  rc = ANativeWindow_unlockAndPost(w);
  if (rc != 0) {
    __android_log_print(ANDROID_LOG_ERROR, "anwtest", "ANativeWindow_unlockAndPost rc=%d", rc);
    return -3;
  }
  __android_log_print(ANDROID_LOG_INFO, "anwtest", "produced %dx%d stride=%d", b.width, b.height, b.stride);
  return locked_w;
}
