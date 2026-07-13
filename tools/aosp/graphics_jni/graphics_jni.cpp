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

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "SkBitmap.h"
#include "SkCanvas.h"
#include "SkClipOp.h"
#include "SkColor.h"
#include "SkColorSpace.h"
#include "SkFont.h"
#include "SkFontMetrics.h"
#include "SkFontMgr.h"
#include "SkFontTypes.h"
#include "SkImageInfo.h"
#include "SkPaint.h"
#include "SkRect.h"
#include "SkRefCnt.h"
#include "SkTypeface.h"

namespace {

// The Bitmap native handle: an SkBitmap + the mutability the framework tracks.
struct AclBitmap {
  SkBitmap bitmap;
  bool immutable = false;
};

// The Paint native handle. Android's Paint carries both draw state (color/style/stroke →
// SkPaint) AND text state (typeface/size → SkFont, which modern Skia split out of SkPaint).
struct AclPaint {
  SkPaint paint;
  SkFont font;
};

// A shared default typeface for text, resolved once. The AOSP host Skia default SkFontMgr
// only exposes Skia's TEST fonts (ahem.ttf etc.), which lack real Latin glyphs, so load a
// real font file: ACL_GRAPHICS_FONT if set (point it at Android's Roboto-Regular.ttf),
// else a common host font, else fall back to the test typeface. Per-app font config
// (fonts.xml + the fallback chain) is future work.
sk_sp<SkTypeface> default_typeface() {
  static sk_sp<SkTypeface> tf = [] {
    const char* candidates[] = {
        std::getenv("ACL_GRAPHICS_FONT"),
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
    };
    for (const char* path : candidates) {
      if (path == nullptr || *path == '\0') continue;
      if (sk_sp<SkTypeface> t = SkTypeface::MakeFromFile(path)) return t;
    }
    sk_sp<SkFontMgr> mgr = SkFontMgr::RefDefault();
    return mgr ? mgr->legacyMakeTypeface(nullptr, SkFontStyle::Normal()) : sk_sp<SkTypeface>();
  }();
  return tf;
}

jclass g_bitmap_class = nullptr;    // global ref, for nativeCreate -> NewObject
jmethodID g_bitmap_ctor = nullptr;

// A GPU-surface-owned SkCanvas (from gpu_dmabuf.cpp's begin_frame) must not be deleted by
// the Java Canvas finalizer — the SkSurface owns it. Defined in gpu_dmabuf.cpp (same .so).
extern "C" bool acl_graphics_canvas_borrowed(void* c);

// NativeAllocationRegistry finalizers (returned as function pointers to Java).
void free_bitmap(void* p) { delete reinterpret_cast<AclBitmap*>(p); }
void free_paint(void* p) { delete reinterpret_cast<AclPaint*>(p); }
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
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  SkPaint* p = ap != nullptr ? &ap->paint : nullptr;
  if (c != nullptr) c->drawRect(SkRect::MakeLTRB(l, t, r, b), p ? *p : SkPaint());
}

void Canvas_nDrawLine(JNIEnv*, jclass, jlong canvas, jfloat x0, jfloat y0, jfloat x1, jfloat y1,
                      jlong paint) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  SkPaint* p = ap != nullptr ? &ap->paint : nullptr;
  if (c != nullptr) c->drawLine(x0, y0, x1, y1, p ? *p : SkPaint());
}

void Canvas_nDrawOval(JNIEnv*, jclass, jlong canvas, jfloat l, jfloat t, jfloat r, jfloat b,
                      jlong paint) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  SkPaint* p = ap != nullptr ? &ap->paint : nullptr;
  if (c != nullptr) c->drawOval(SkRect::MakeLTRB(l, t, r, b), p ? *p : SkPaint());
}

void Canvas_nDrawCircle(JNIEnv*, jclass, jlong canvas, jfloat cx, jfloat cy, jfloat radius,
                        jlong paint) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  SkPaint* p = ap != nullptr ? &ap->paint : nullptr;
  if (c != nullptr) c->drawCircle(cx, cy, radius, p ? *p : SkPaint());
}

void Canvas_nDrawRoundRect(JNIEnv*, jclass, jlong canvas, jfloat l, jfloat t, jfloat r, jfloat b,
                           jfloat rx, jfloat ry, jlong paint) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  SkPaint* p = ap != nullptr ? &ap->paint : nullptr;
  if (c != nullptr) c->drawRoundRect(SkRect::MakeLTRB(l, t, r, b), rx, ry, p ? *p : SkPaint());
}

// nDrawText(String): draw text[start,end) at (x,y) using the Paint's SkFont + SkPaint.
// Latin/default-font path via SkCanvas::drawSimpleText — no Minikin/HarfBuzz shaping yet, so
// char indices are treated as UTF-8 byte offsets (correct for ASCII, the common case).
void Canvas_nDrawText(JNIEnv* env, jclass, jlong canvas, jstring text, jint start, jint end,
                      jfloat x, jfloat y, jint /*flags*/, jlong paint) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  if (c == nullptr || ap == nullptr || text == nullptr) return;
  const char* utf8 = env->GetStringUTFChars(text, nullptr);
  if (utf8 == nullptr) return;
  const int len = static_cast<int>(std::strlen(utf8));
  const int s = start < 0 ? 0 : start;
  const int e = end > len ? len : end;
  if (e > s)
    c->drawSimpleText(utf8 + s, static_cast<std::size_t>(e - s), SkTextEncoding::kUTF8, x, y,
                      ap->font, ap->paint);
  env->ReleaseStringUTFChars(text, utf8);
}

// ── Canvas transform/clip — all @CriticalNative (handle-only, no JNIEnv/jclass) ──
jint Canvas_nSave(jlong canvas, jint /*saveFlags*/) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  return c != nullptr ? c->save() : 0;
}
jboolean Canvas_nRestore(jlong canvas) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  if (c == nullptr || c->getSaveCount() <= 1) return JNI_FALSE;
  c->restore();
  return JNI_TRUE;
}
void Canvas_nRestoreToCount(jlong canvas, jint saveCount) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  if (c != nullptr) c->restoreToCount(saveCount);
}
jint Canvas_nGetSaveCount(jlong canvas) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  return c != nullptr ? c->getSaveCount() : 0;
}
void Canvas_nTranslate(jlong canvas, jfloat dx, jfloat dy) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  if (c != nullptr) c->translate(dx, dy);
}
void Canvas_nScale(jlong canvas, jfloat sx, jfloat sy) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  if (c != nullptr) c->scale(sx, sy);
}
void Canvas_nRotate(jlong canvas, jfloat degrees) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  if (c != nullptr) c->rotate(degrees);
}
// Region.Op nativeInt: 0=DIFFERENCE, 1=INTERSECT (default clipRect), others map to intersect
// (modern Skia only has Intersect/Difference). Returns whether the clip is non-empty.
jboolean Canvas_nClipRect(jlong canvas, jfloat l, jfloat t, jfloat r, jfloat b, jint regionOp) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  if (c == nullptr) return JNI_FALSE;
  c->clipRect(SkRect::MakeLTRB(l, t, r, b),
              regionOp == 0 ? SkClipOp::kDifference : SkClipOp::kIntersect, /*doAA=*/false);
  return c->isClipEmpty() ? JNI_FALSE : JNI_TRUE;
}

// ── Paint ─────────────────────────────────────────────────────────────────────
jlong Paint_nInit(JNIEnv*, jclass) {
  auto* ap = new AclPaint();
  ap->font.setTypeface(default_typeface());
  ap->font.setSize(16.0f);  // framework default is ~16px; overridden by nSetTextSize
  return reinterpret_cast<jlong>(ap);
}
jlong Paint_nGetNativeFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&free_paint);
}
// @CriticalNative
void Paint_nSetColor(jlong paintPtr, jint color) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  if (ap != nullptr) ap->paint.setColor(static_cast<SkColor>(color));
}
// @CriticalNative — reset draw state; keep the resolved default typeface, reset size.
void Paint_nReset(jlong paintPtr) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  if (ap == nullptr) return;
  ap->paint = SkPaint();
  ap->font = SkFont();
  ap->font.setTypeface(default_typeface());
  ap->font.setSize(16.0f);
}
// @CriticalNative — ANTI_ALIAS_FLAG == 0x01.
void Paint_nSetFlags(jlong paintPtr, jint flags) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  if (ap != nullptr) ap->paint.setAntiAlias((flags & 0x01) != 0);
}
// @CriticalNative — return the flags we track (just antialias for the raster subset).
jint Paint_nGetFlags(jlong paintPtr) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  return (ap != nullptr && ap->paint.isAntiAlias()) ? 0x01 : 0;
}
// @FastNative — minikin locale id; no-op (no text in the raster subset).
jint Paint_nSetTextLocales(JNIEnv*, jclass, jlong, jstring) { return 0; }
// @CriticalNative — set-by-cached-id fast path; no-op.
void Paint_nSetTextLocalesByMinikinLocaleListId(jlong, jint) {}
// @CriticalNative — Paint.Style: FILL=0, STROKE=1, FILL_AND_STROKE=2 (SkPaint::Style matches).
void Paint_nSetStyle(jlong paintPtr, jint style) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  if (ap != nullptr) ap->paint.setStyle(static_cast<SkPaint::Style>(style));
}
// @CriticalNative
void Paint_nSetStrokeWidth(jlong paintPtr, jfloat width) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  if (ap != nullptr) ap->paint.setStrokeWidth(width);
}
// @CriticalNative — Paint.Cap: BUTT=0, ROUND=1, SQUARE=2 (SkPaint::Cap matches).
void Paint_nSetStrokeCap(jlong paintPtr, jint cap) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  if (ap != nullptr) ap->paint.setStrokeCap(static_cast<SkPaint::Cap>(cap));
}
// @CriticalNative
void Paint_nSetAntiAlias(jlong paintPtr, jboolean aa) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  if (ap != nullptr) ap->paint.setAntiAlias(aa != JNI_FALSE);
}
// @CriticalNative — text size in pixels (into the SkFont; SkPaint no longer holds it).
void Paint_nSetTextSize(jlong paintPtr, jfloat textSize) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  if (ap != nullptr) ap->font.setSize(textSize);
}
// @FastNative — fill Paint.FontMetricsInt (top/ascent/descent/bottom/leading) from the SkFont.
jint Paint_nGetFontMetricsInt(JNIEnv* env, jclass, jlong paintPtr, jobject fmi) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  if (ap == nullptr) return 0;
  SkFontMetrics m{};
  ap->font.getMetrics(&m);
  const int ascent = static_cast<int>(std::floor(m.fAscent));
  const int descent = static_cast<int>(std::ceil(m.fDescent));
  if (fmi != nullptr) {
    jclass c = env->GetObjectClass(fmi);
    const auto set = [&](const char* f, int v) {
      if (jfieldID id = env->GetFieldID(c, f, "I")) env->SetIntField(fmi, id, v);
    };
    set("top", static_cast<int>(std::floor(m.fTop)));
    set("ascent", ascent);
    set("descent", descent);
    set("bottom", static_cast<int>(std::ceil(m.fBottom)));
    set("leading", static_cast<int>(std::ceil(m.fLeading)));
    env->DeleteLocalRef(c);
  }
  return descent - ascent;
}

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
    // @CriticalNative transform/clip (handle-only).
    {"nSave", "(JI)I", reinterpret_cast<void*>(Canvas_nSave)},
    {"nRestore", "(J)Z", reinterpret_cast<void*>(Canvas_nRestore)},
    {"nRestoreToCount", "(JI)V", reinterpret_cast<void*>(Canvas_nRestoreToCount)},
    {"nGetSaveCount", "(J)I", reinterpret_cast<void*>(Canvas_nGetSaveCount)},
    {"nTranslate", "(JFF)V", reinterpret_cast<void*>(Canvas_nTranslate)},
    {"nScale", "(JFF)V", reinterpret_cast<void*>(Canvas_nScale)},
    {"nRotate", "(JF)V", reinterpret_cast<void*>(Canvas_nRotate)},
    {"nClipRect", "(JFFFFI)Z", reinterpret_cast<void*>(Canvas_nClipRect)},
};
const JNINativeMethod kBaseCanvas[] = {
    {"nDrawColor", "(JII)V", reinterpret_cast<void*>(Canvas_nDrawColor)},
    {"nDrawRect", "(JFFFFJ)V", reinterpret_cast<void*>(Canvas_nDrawRect)},
    {"nDrawLine", "(JFFFFJ)V", reinterpret_cast<void*>(Canvas_nDrawLine)},
    {"nDrawOval", "(JFFFFJ)V", reinterpret_cast<void*>(Canvas_nDrawOval)},
    {"nDrawCircle", "(JFFFJ)V", reinterpret_cast<void*>(Canvas_nDrawCircle)},
    {"nDrawRoundRect", "(JFFFFFFJ)V", reinterpret_cast<void*>(Canvas_nDrawRoundRect)},
    {"nDrawText", "(JLjava/lang/String;IIFFIJ)V", reinterpret_cast<void*>(Canvas_nDrawText)},
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
    {"nSetStyle", "(JI)V", reinterpret_cast<void*>(Paint_nSetStyle)},
    {"nSetStrokeWidth", "(JF)V", reinterpret_cast<void*>(Paint_nSetStrokeWidth)},
    {"nSetStrokeCap", "(JI)V", reinterpret_cast<void*>(Paint_nSetStrokeCap)},
    {"nSetAntiAlias", "(JZ)V", reinterpret_cast<void*>(Paint_nSetAntiAlias)},
    {"nSetTextSize", "(JF)V", reinterpret_cast<void*>(Paint_nSetTextSize)},
    {"nGetFontMetricsInt", "(JLandroid/graphics/Paint$FontMetricsInt;)I",
     reinterpret_cast<void*>(Paint_nGetFontMetricsInt)},
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
  const auto n = [](auto& arr) { return static_cast<jint>(sizeof(arr) / sizeof(arr[0])); };
  bool ok = true;
  ok = reg(env, "android/graphics/Bitmap", kBitmap, n(kBitmap)) && ok;
  ok = reg(env, "android/graphics/Canvas", kCanvas, n(kCanvas)) && ok;
  ok = reg(env, "android/graphics/BaseCanvas", kBaseCanvas, n(kBaseCanvas)) && ok;
  ok = reg(env, "android/graphics/Paint", kPaint, n(kPaint)) && ok;
  ok = reg(env, "android/graphics/ColorSpace$Rgb", kColorSpaceRgb, n(kColorSpaceRgb)) && ok;
  return ok ? JNI_OK : JNI_ERR;
}
