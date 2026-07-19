// SPDX-License-Identifier: Apache-2.0
// Provenance: signatures from the NDK <android/bitmap.h> stable API (AOSP, Apache-2.0). No GPL-3.0 sources.
//
// Staged as bionic libjnigraphics.so. A native lib (libflutter.so) DT_NEEDs it for the three
// AndroidBitmap_* entrypoints, which lock a java.graphics.Bitmap's pixels for direct native access.
// These are placeholders so the lib resolves and loads; Impeller's GL render path does not touch
// them (it renders to its own EGL surface). Wiring them to acl's Bitmap (the CPU readback / snapshot
// path) is a later step — until then getInfo reports failure so callers do not read a bogus buffer.
#include <stdint.h>

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

// JNIEnv*, jobject, AndroidBitmapInfo* — opaque here; we only need the ABI, not the JNI types.
int AndroidBitmap_getInfo(void* env, void* jbitmap, AndroidBitmapInfo* info) {
  (void)env;
  (void)jbitmap;
  (void)info;
  return ANDROID_BITMAP_RESULT_BAD_PARAMETER;  // not wired yet — do not hand back a buffer
}

int AndroidBitmap_lockPixels(void* env, void* jbitmap, void** addr) {
  (void)env;
  (void)jbitmap;
  if (addr) *addr = 0;
  return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
}

int AndroidBitmap_unlockPixels(void* env, void* jbitmap) {
  (void)env;
  (void)jbitmap;
  return ANDROID_BITMAP_RESULT_SUCCESS;
}
