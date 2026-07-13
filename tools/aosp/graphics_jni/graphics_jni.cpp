// SPDX-License-Identifier: Apache-2.0
// Provenance: acl P2.4/V3 (docs/render-host-scope.md); native method contracts read
// from AOSP frameworks/base android.graphics {Bitmap,Canvas,BaseCanvas,Paint,ColorSpace}
// (Apache-2.0); Skia public core API. No GPL-3.0 sources.
//
// acl-owned minimal android.graphics JNI: thin natives over core Skia (SkBitmap/SkCanvas/
// SkPaint), compiled with the DEVICE @CriticalNative convention (nativeIsImmutable / nSetColor
// take just the handle — no JNIEnv/jclass — matching the framework's @CriticalNative annotation
// that host ART honors). This is the ADR-4 "re-own android.graphics on our Skia" seam: real
// framework Bitmap/Canvas/Paint classes, acl natives, Android's host libskia. Registered by
// acl (register_acl_graphics) after dlopen; the Canvas's SkBitmap is later backed by acl's
// dma-buf VkImage for GPU present (S3/S4).

#include <jni.h>

#include <cstdint>

#include "SkBitmap.h"
#include "SkCanvas.h"
#include "SkColor.h"
#include "SkColorSpace.h"
#include "SkImageInfo.h"
#include "SkPaint.h"
#include "SkRect.h"
#include "SkRefCnt.h"

namespace {

// The Bitmap native handle: an SkBitmap + the mutability the framework tracks.
struct AclBitmap {
  SkBitmap bitmap;
  bool immutable = false;
};

jclass g_bitmap_class = nullptr;    // global ref, for nativeCreate -> NewObject
jmethodID g_bitmap_ctor = nullptr;

// A GPU-surface-owned SkCanvas (from gpu_dmabuf.cpp's begin_frame) must not be deleted by
// the Java Canvas finalizer — the SkSurface owns it. Defined in gpu_dmabuf.cpp (same .so).
extern "C" bool acl_graphics_canvas_borrowed(void* c);

// NativeAllocationRegistry finalizers (returned as function pointers to Java).
void free_bitmap(void* p) { delete reinterpret_cast<AclBitmap*>(p); }
void free_paint(void* p) { delete reinterpret_cast<SkPaint*>(p); }
void free_canvas(void* p) {
  if (acl_graphics_canvas_borrowed(p)) return;  // surface-owned (GPU); not ours to delete
  delete reinterpret_cast<SkCanvas*>(p);
}
void free_colorspace(void* p) { SkSafeUnref(reinterpret_cast<SkColorSpace*>(p)); }

// android.graphics.Bitmap.Config.nativeInt -> SkColorType (ARGB_8888 == 5).
SkColorType config_to_ct(jint cfg) {
  switch (cfg) {
    case 1: return kAlpha_8_SkColorType;
    case 3: return kRGB_565_SkColorType;
    case 5: return kN32_SkColorType;  // ARGB_8888
    default: return kN32_SkColorType;
  }
}

// ── Bitmap ────────────────────────────────────────────────────────────────────
jobject Bitmap_nativeCreate(JNIEnv* env, jclass, jintArray /*colors*/, jint /*offset*/,
                            jint /*stride*/, jint width, jint height, jint config,
                            jboolean isMutable, jlong colorSpacePtr) {
  auto* w = new AclBitmap();
  auto* cs = reinterpret_cast<SkColorSpace*>(colorSpacePtr);
  const SkImageInfo info = SkImageInfo::Make(width, height, config_to_ct(config),
                                             kPremul_SkAlphaType,
                                             cs ? sk_ref_sp(cs) : SkColorSpace::MakeSRGB());
  if (!w->bitmap.tryAllocPixels(info)) {
    delete w;
    return nullptr;
  }
  w->bitmap.eraseColor(SK_ColorTRANSPARENT);
  w->immutable = (isMutable == JNI_FALSE);
  return env->NewObject(g_bitmap_class, g_bitmap_ctor, reinterpret_cast<jlong>(w), width,
                        height, /*density=*/0, /*requestPremultiplied=*/JNI_TRUE,
                        /*ninePatchChunk=*/static_cast<jbyteArray>(nullptr),
                        /*ninePatchInsets=*/static_cast<jobject>(nullptr));
}

jlong Bitmap_nativeGetNativeFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&free_bitmap);
}

jint Bitmap_nativeGetPixel(JNIEnv*, jclass, jlong ptr, jint x, jint y) {
  auto* w = reinterpret_cast<AclBitmap*>(ptr);
  return static_cast<jint>(w->bitmap.getColor(x, y));
}

jint Bitmap_nativeGetAllocationByteCount(JNIEnv*, jclass, jlong ptr) {
  auto* w = reinterpret_cast<AclBitmap*>(ptr);
  return static_cast<jint>(w->bitmap.computeByteSize());
}

void Bitmap_nativeSetHasAlpha(JNIEnv*, jclass, jlong ptr, jboolean hasAlpha, jboolean premul) {
  auto* w = reinterpret_cast<AclBitmap*>(ptr);
  if (w == nullptr) return;
  const SkAlphaType at = (hasAlpha == JNI_FALSE)
                             ? kOpaque_SkAlphaType
                             : (premul != JNI_FALSE ? kPremul_SkAlphaType : kUnpremul_SkAlphaType);
  w->bitmap.setAlphaType(at);
}

void Bitmap_nativeErase(JNIEnv*, jclass, jlong ptr, jint color) {
  auto* w = reinterpret_cast<AclBitmap*>(ptr);
  if (w != nullptr) w->bitmap.eraseColor(static_cast<SkColor>(color));
}

jboolean Bitmap_nativeIsPremultiplied(JNIEnv*, jclass, jlong ptr) {
  auto* w = reinterpret_cast<AclBitmap*>(ptr);
  return (w != nullptr && w->bitmap.alphaType() == kPremul_SkAlphaType) ? JNI_TRUE : JNI_FALSE;
}

jint Bitmap_nativeConfig(JNIEnv*, jclass, jlong ptr) {
  auto* w = reinterpret_cast<AclBitmap*>(ptr);
  if (w == nullptr) return 0;
  switch (w->bitmap.colorType()) {
    case kAlpha_8_SkColorType: return 1;
    case kRGB_565_SkColorType: return 3;
    default: return 5;  // ARGB_8888 (N32)
  }
}

// @CriticalNative — handle only, no JNIEnv/jclass (matches the framework annotation).
jboolean Bitmap_nativeIsImmutable(jlong ptr) {
  auto* w = reinterpret_cast<AclBitmap*>(ptr);
  return (w != nullptr && w->immutable) ? JNI_TRUE : JNI_FALSE;
}

// ── Canvas / BaseCanvas ─────────────────────────────────────────────────────────
jlong Canvas_nInitRaster(JNIEnv*, jclass, jlong bitmapHandle) {  // @FastNative (normal sig)
  auto* w = reinterpret_cast<AclBitmap*>(bitmapHandle);
  SkCanvas* c = (w != nullptr) ? new SkCanvas(w->bitmap) : new SkCanvas();
  return reinterpret_cast<jlong>(c);
}

jlong Canvas_nGetNativeFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&free_canvas);
}

void Canvas_nDrawColor(JNIEnv*, jclass, jlong canvas, jint color, jint /*mode*/) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  if (c != nullptr) c->drawColor(static_cast<SkColor>(color), SkBlendMode::kSrcOver);
}

void Canvas_nDrawRect(JNIEnv*, jclass, jlong canvas, jfloat l, jfloat t, jfloat r, jfloat b,
                      jlong paint) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  auto* p = reinterpret_cast<SkPaint*>(paint);
  if (c != nullptr) c->drawRect(SkRect::MakeLTRB(l, t, r, b), p ? *p : SkPaint());
}

// ── Paint ─────────────────────────────────────────────────────────────────────
jlong Paint_nInit(JNIEnv*, jclass) { return reinterpret_cast<jlong>(new SkPaint()); }
jlong Paint_nGetNativeFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&free_paint);
}
// @CriticalNative
void Paint_nSetColor(jlong paintPtr, jint color) {
  auto* p = reinterpret_cast<SkPaint*>(paintPtr);
  if (p != nullptr) p->setColor(static_cast<SkColor>(color));
}
// @CriticalNative
void Paint_nReset(jlong paintPtr) {
  auto* p = reinterpret_cast<SkPaint*>(paintPtr);
  if (p != nullptr) *p = SkPaint();
}
// @CriticalNative — ANTI_ALIAS_FLAG == 0x01.
void Paint_nSetFlags(jlong paintPtr, jint flags) {
  auto* p = reinterpret_cast<SkPaint*>(paintPtr);
  if (p != nullptr) p->setAntiAlias((flags & 0x01) != 0);
}
// @CriticalNative — return the flags we track (just antialias for the raster subset).
jint Paint_nGetFlags(jlong paintPtr) {
  auto* p = reinterpret_cast<SkPaint*>(paintPtr);
  return (p != nullptr && p->isAntiAlias()) ? 0x01 : 0;
}
// @FastNative — minikin locale id; no-op (no text in the raster subset).
jint Paint_nSetTextLocales(JNIEnv*, jclass, jlong, jstring) { return 0; }
// @CriticalNative — set-by-cached-id fast path; no-op.
void Paint_nSetTextLocalesByMinikinLocaleListId(jlong, jint) {}

// ── ColorSpace$Rgb (createBitmap's sRGB) ────────────────────────────────────────
jlong ColorSpaceRgb_nativeCreate(JNIEnv*, jclass, jfloat, jfloat, jfloat, jfloat, jfloat,
                                 jfloat, jfloat, jfloatArray) {
  // Minimal: sRGB (the transfer/gamut params are ignored for the first milestone).
  return reinterpret_cast<jlong>(SkColorSpace::MakeSRGB().release());
}
jlong ColorSpaceRgb_nativeGetNativeFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&free_colorspace);
}

const JNINativeMethod kBitmap[] = {
    {"nativeCreate", "([IIIIIIZJ)Landroid/graphics/Bitmap;",
     reinterpret_cast<void*>(Bitmap_nativeCreate)},
    {"nativeGetNativeFinalizer", "()J", reinterpret_cast<void*>(Bitmap_nativeGetNativeFinalizer)},
    {"nativeGetPixel", "(JII)I", reinterpret_cast<void*>(Bitmap_nativeGetPixel)},
    {"nativeGetAllocationByteCount", "(J)I",
     reinterpret_cast<void*>(Bitmap_nativeGetAllocationByteCount)},
    {"nativeSetHasAlpha", "(JZZ)V", reinterpret_cast<void*>(Bitmap_nativeSetHasAlpha)},
    {"nativeErase", "(JI)V", reinterpret_cast<void*>(Bitmap_nativeErase)},
    {"nativeIsPremultiplied", "(J)Z", reinterpret_cast<void*>(Bitmap_nativeIsPremultiplied)},
    {"nativeConfig", "(J)I", reinterpret_cast<void*>(Bitmap_nativeConfig)},
    {"nativeIsImmutable", "(J)Z", reinterpret_cast<void*>(Bitmap_nativeIsImmutable)},
};
const JNINativeMethod kCanvas[] = {
    {"nInitRaster", "(J)J", reinterpret_cast<void*>(Canvas_nInitRaster)},
    {"nGetNativeFinalizer", "()J", reinterpret_cast<void*>(Canvas_nGetNativeFinalizer)},
};
const JNINativeMethod kBaseCanvas[] = {
    {"nDrawColor", "(JII)V", reinterpret_cast<void*>(Canvas_nDrawColor)},
    {"nDrawRect", "(JFFFFJ)V", reinterpret_cast<void*>(Canvas_nDrawRect)},
};
const JNINativeMethod kPaint[] = {
    {"nInit", "()J", reinterpret_cast<void*>(Paint_nInit)},
    {"nGetNativeFinalizer", "()J", reinterpret_cast<void*>(Paint_nGetNativeFinalizer)},
    {"nSetColor", "(JI)V", reinterpret_cast<void*>(Paint_nSetColor)},
    {"nReset", "(J)V", reinterpret_cast<void*>(Paint_nReset)},
    {"nSetFlags", "(JI)V", reinterpret_cast<void*>(Paint_nSetFlags)},
    {"nGetFlags", "(J)I", reinterpret_cast<void*>(Paint_nGetFlags)},
    {"nSetTextLocales", "(JLjava/lang/String;)I", reinterpret_cast<void*>(Paint_nSetTextLocales)},
    {"nSetTextLocalesByMinikinLocaleListId", "(JI)V", reinterpret_cast<void*>(Paint_nSetTextLocalesByMinikinLocaleListId)},
};
const JNINativeMethod kColorSpaceRgb[] = {
    {"nativeCreate", "(FFFFFFF[F)J", reinterpret_cast<void*>(ColorSpaceRgb_nativeCreate)},
    {"nativeGetNativeFinalizer", "()J",
     reinterpret_cast<void*>(ColorSpaceRgb_nativeGetNativeFinalizer)},
};

bool reg(JNIEnv* env, const char* cls, const JNINativeMethod* m, jint n) {
  jclass c = env->FindClass(cls);
  if (c == nullptr) {
    env->ExceptionClear();
    return false;
  }
  const bool ok = env->RegisterNatives(c, m, n) == JNI_OK;
  if (!ok) env->ExceptionClear();
  return ok;
}

}  // namespace

// S3b: read a rendered Bitmap's pixels (by its native handle) into `dst` as 0xAARRGGBB
// (kBGRA_8888 on little-endian — the gfx::Surface / dma-buf convention). acl dlsyms this
// to get the app's android.graphics render output onto the Wayland present path.
extern "C" bool acl_read_bitmap(long handle, std::uint32_t* dst, int w, int h) {
  auto* b = reinterpret_cast<AclBitmap*>(handle);
  if (b == nullptr || dst == nullptr) return false;
  const SkImageInfo info = SkImageInfo::Make(w, h, kBGRA_8888_SkColorType,
                                             kUnpremul_SkAlphaType, SkColorSpace::MakeSRGB());
  return b->bitmap.readPixels(info, dst, static_cast<std::size_t>(w) * 4, 0, 0);
}

// acl calls this after dlopen. Returns JNI_OK when the minimal graphics floor is bound.
extern "C" jint register_acl_graphics(JNIEnv* env) {
  jclass bmp = env->FindClass("android/graphics/Bitmap");
  if (bmp == nullptr) {
    env->ExceptionClear();
    return JNI_ERR;
  }
  g_bitmap_class = static_cast<jclass>(env->NewGlobalRef(bmp));
  g_bitmap_ctor = env->GetMethodID(
      bmp, "<init>", "(JIIIZ[BLandroid/graphics/NinePatch$InsetStruct;)V");
  if (g_bitmap_ctor == nullptr) {
    env->ExceptionClear();
    return JNI_ERR;
  }
  bool ok = true;
  ok = reg(env, "android/graphics/Bitmap", kBitmap, 9) && ok;
  ok = reg(env, "android/graphics/Canvas", kCanvas, 2) && ok;
  ok = reg(env, "android/graphics/BaseCanvas", kBaseCanvas, 2) && ok;
  ok = reg(env, "android/graphics/Paint", kPaint, 8) && ok;
  ok = reg(env, "android/graphics/ColorSpace$Rgb", kColorSpaceRgb, 2) && ok;
  return ok ? JNI_OK : JNI_ERR;
}
