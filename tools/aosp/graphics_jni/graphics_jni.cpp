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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "SkBitmap.h"
#include "SkCanvas.h"
#include "SkClipOp.h"
#include "SkColor.h"
#include "SkColorSpace.h"
#include "SkData.h"
#include "SkSamplingOptions.h"
#include "SkStream.h"
#include "SkPngChunkReader.h"
#include "codec/SkAndroidCodec.h"
#include "codec/SkCodec.h"
#include <androidfw/Asset.h>          // android::Asset (ImageDecoder resource decode)
#include <androidfw/ResourceTypes.h>  // android::Res_png_9patch (nine-patch npTc chunk)
#include "SkFontStyle.h"
#include "SkFont.h"
#include "SkFontMetrics.h"
#include "SkFontMgr.h"
#include "SkFontTypes.h"
#include "SkGradientShader.h"
#include "SkImage.h"
#include "SkImageInfo.h"
#include "SkMatrix.h"
#include "SkPaint.h"
#include "SkColorFilter.h"
#include "SkPath.h"
#include "SkPathMeasure.h"
#include "SkPathTypes.h"
#include "SkRegion.h"
#include "utils/SkParsePath.h"
#include "SkRect.h"
#include "SkRefCnt.h"
#include "SkSamplingOptions.h"
#include "SkShader.h"
#include "SkTileMode.h"
#include "SkTextBlob.h"
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
  int textAlign = 0;         // Paint.Align: LEFT=0, CENTER=1, RIGHT=2
  float letterSpacing = 0;   // em; SkFont has no letter spacing, tracked for get/set parity
};

// RenderNode shim (ADR-4: hwui dropped, render stack re-owned). acl renders via the GPU
// (Ganesh Vulkan → dma-buf), but immediate-mode: a Canvas over an SkSurface is NOT a
// RecordingCanvas, so the framework's View.draw() sees isHardwareAccelerated()==false and
// calls onDraw directly onto the (GPU or Bitmap) Canvas — hwui's RenderNode display-list
// recording is bypassed either way. So this only tracks the geometry/transform state the
// View reads back; recording/animation are no-ops. Lets the real android.view.View
// construct + measure/layout on host ART; the real present is the Vulkan Canvas (begin_frame).
struct AclRenderNode {
  int left = 0, top = 0, right = 0, bottom = 0;
  float alpha = 1.0f, cameraDistance = 0.0f;
  float translationX = 0, translationY = 0, translationZ = 0, elevation = 0;
  float rotation = 0, rotationX = 0, rotationY = 0;
  float scaleX = 1.0f, scaleY = 1.0f;
  float pivotX = 0, pivotY = 0;
  bool pivotExplicit = false;
  int layerType = 0, spotShadowColor = 0, ambientShadowColor = 0;
  bool clipToBounds = true, clipToOutline = false, allowForceDark = true;
  bool hasOverlappingRendering = true;
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

// Build an SkTypeface from a font file and hand back its raw handle (an SkTypeface*, +1 ref)
// as android.graphics.Typeface's native pointer — Typeface(long) wraps it, free_typeface() unrefs
// it. acl calls this to seed distinct system families (sans/serif/monospace) from real font files:
// the framework's own path (Typeface.createFromFile / the @CriticalNative Font$Builder natives) is
// unavailable on this host. A dedicated, non-@CriticalNative entry avoids that ABI hazard.
extern "C" jlong acl_typeface_from_path(const char* path) {
  if (path == nullptr || *path == '\0') return 0;
  sk_sp<SkTypeface> tf = SkTypeface::MakeFromFile(path);
  if (!tf) return 0;
  return reinterpret_cast<jlong>(tf.release());
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
void free_path(void* p) { delete reinterpret_cast<SkPath*>(p); }
void free_matrix(void* p) { delete reinterpret_cast<SkMatrix*>(p); }
void free_shader(void* p) { SkSafeUnref(reinterpret_cast<SkShader*>(p)); }
void free_rendernode(void* p) { delete reinterpret_cast<AclRenderNode*>(p); }
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
jboolean Bitmap_nativeHasAlpha(JNIEnv*, jclass, jlong ptr) {
  auto* w = reinterpret_cast<AclBitmap*>(ptr);
  return (w != nullptr && !w->bitmap.isOpaque()) ? JNI_TRUE : JNI_FALSE;
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
// Draw UTF-16 units [off, off+cnt) at (x,y) with the Paint's SkFont + SkPaint. No
// Minikin/HarfBuzz shaping yet — Skia does simple per-glyph layout (fine for Latin;
// complex scripts/RTL/bidi are a later shaping pass). The char[]/String draw + run
// variants all funnel here (context + RTL args are ignored without shaping).
void draw_utf16(SkCanvas* c, AclPaint* ap, const jchar* chars, int len, int off, int cnt,
                jfloat x, jfloat y) {
  if (c == nullptr || ap == nullptr || chars == nullptr) return;
  const int s = off < 0 ? 0 : off;
  const int n = (s + cnt > len) ? (len - s) : cnt;
  if (n > 0)
    c->drawSimpleText(chars + s, static_cast<std::size_t>(n) * 2, SkTextEncoding::kUTF16, x, y,
                      ap->font, ap->paint);
}
// nDrawText(String, start, end)
void Canvas_nDrawText(JNIEnv* env, jclass, jlong canvas, jstring text, jint start, jint end,
                      jfloat x, jfloat y, jint /*flags*/, jlong paint) {
  if (text == nullptr) return;
  const jchar* chars = env->GetStringChars(text, nullptr);
  if (chars != nullptr) {
    draw_utf16(reinterpret_cast<SkCanvas*>(canvas), reinterpret_cast<AclPaint*>(paint),
               chars, env->GetStringLength(text), start, end - start, x, y);
    env->ReleaseStringChars(text, chars);
  }
}
// nDrawText(char[], index, count)
void Canvas_nDrawTextChars(JNIEnv* env, jclass, jlong canvas, jcharArray text, jint index,
                           jint count, jfloat x, jfloat y, jint /*flags*/, jlong paint) {
  if (text == nullptr) return;
  jchar* chars = env->GetCharArrayElements(text, nullptr);
  if (chars != nullptr) {
    draw_utf16(reinterpret_cast<SkCanvas*>(canvas), reinterpret_cast<AclPaint*>(paint),
               chars, env->GetArrayLength(text), index, count, x, y);
    env->ReleaseCharArrayElements(text, chars, JNI_ABORT);
  }
}
// nDrawTextRun(String, start, end, contextStart, contextEnd, x, y, isRtl, paint)
void Canvas_nDrawTextRunString(JNIEnv* env, jclass, jlong canvas, jstring text, jint start, jint end,
                               jint, jint, jfloat x, jfloat y, jboolean, jlong paint) {
  if (text == nullptr) return;
  const jchar* chars = env->GetStringChars(text, nullptr);
  if (chars != nullptr) {
    draw_utf16(reinterpret_cast<SkCanvas*>(canvas), reinterpret_cast<AclPaint*>(paint),
               chars, env->GetStringLength(text), start, end - start, x, y);
    env->ReleaseStringChars(text, chars);
  }
}
// nDrawTextRun(char[], start, count, contextStart, contextCount, x, y, isRtl, paint, precomputed)
void Canvas_nDrawTextRunChars(JNIEnv* env, jclass, jlong canvas, jcharArray text, jint start,
                              jint count, jint, jint, jfloat x, jfloat y, jboolean, jlong paint,
                              jlong) {
  if (text == nullptr) return;
  jchar* chars = env->GetCharArrayElements(text, nullptr);
  if (chars != nullptr) {
    draw_utf16(reinterpret_cast<SkCanvas*>(canvas), reinterpret_cast<AclPaint*>(paint),
               chars, env->GetArrayLength(text), start, count, x, y);
    env->ReleaseCharArrayElements(text, chars, JNI_ABORT);
  }
}
// nDrawGlyphs(glyphIds, positions, glyphIdStart, positionStart, glyphCount, font, paint):
// pre-shaped glyphs (PrecomputedText path). Positions are absolute (x,y) pairs.
void Canvas_nDrawGlyphs(JNIEnv* env, jclass, jlong canvas, jintArray glyphIds, jfloatArray positions,
                        jint glyphIdStart, jint positionStart, jint glyphCount, jlong /*font*/,
                        jlong paint) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  if (c == nullptr || ap == nullptr || glyphIds == nullptr || positions == nullptr || glyphCount <= 0)
    return;
  jint* gids = env->GetIntArrayElements(glyphIds, nullptr);
  jfloat* pos = env->GetFloatArrayElements(positions, nullptr);
  if (gids != nullptr && pos != nullptr) {
    SkTextBlobBuilder builder;
    const SkTextBlobBuilder::RunBuffer& run = builder.allocRunPos(ap->font, glyphCount);
    for (int i = 0; i < glyphCount; ++i) {
      run.glyphs[i] = static_cast<uint16_t>(gids[glyphIdStart + i]);
      run.points()[i] = SkPoint::Make(pos[positionStart + 2 * i], pos[positionStart + 2 * i + 1]);
    }
    if (sk_sp<SkTextBlob> blob = builder.make()) c->drawTextBlob(blob, 0, 0, ap->paint);
  }
  if (gids != nullptr) env->ReleaseIntArrayElements(glyphIds, gids, JNI_ABORT);
  if (pos != nullptr) env->ReleaseFloatArrayElements(positions, pos, JNI_ABORT);
}
// Text-on-path isn't modeled (SkCanvas dropped drawTextOnPath) — accepted, not drawn.
void Canvas_nDrawTextOnPathChars(JNIEnv*, jclass, jlong, jcharArray, jint, jint, jlong, jfloat,
                                 jfloat, jint, jlong) {}
void Canvas_nDrawTextOnPathString(JNIEnv*, jclass, jlong, jstring, jlong, jfloat, jfloat, jint, jlong) {}

// nDrawBitmap(left,top): draw a Bitmap at (left,top). paintOrZero may be 0 (no paint).
void Canvas_nDrawBitmap(JNIEnv*, jclass, jlong canvas, jlong bitmapHandle, jfloat left, jfloat top,
                        jlong paint, jint /*canvasDensity*/, jint /*screenDensity*/,
                        jint /*bitmapDensity*/) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  auto* b = reinterpret_cast<AclBitmap*>(bitmapHandle);
  if (c == nullptr || b == nullptr) return;
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  if (sk_sp<SkImage> img = SkImage::MakeFromBitmap(b->bitmap))
    c->drawImage(img.get(), left, top, SkSamplingOptions(), ap != nullptr ? &ap->paint : nullptr);
}

// nDrawBitmap(src,dst): draw a src sub-rect of a Bitmap into a dst rect (scaled).
void Canvas_nDrawBitmapRect(JNIEnv*, jclass, jlong canvas, jlong bitmapHandle, jfloat sl, jfloat st,
                            jfloat sr, jfloat sb, jfloat dl, jfloat dt, jfloat dr, jfloat db,
                            jlong paint, jint /*screenDensity*/, jint /*bitmapDensity*/) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  auto* b = reinterpret_cast<AclBitmap*>(bitmapHandle);
  if (c == nullptr || b == nullptr) return;
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  if (sk_sp<SkImage> img = SkImage::MakeFromBitmap(b->bitmap))
    c->drawImageRect(img.get(), SkRect::MakeLTRB(sl, st, sr, sb), SkRect::MakeLTRB(dl, dt, dr, db),
                     SkSamplingOptions(), ap != nullptr ? &ap->paint : nullptr,
                     SkCanvas::kStrict_SrcRectConstraint);
}

void Canvas_nDrawPath(JNIEnv*, jclass, jlong canvas, jlong path, jlong paint) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  auto* sp = reinterpret_cast<SkPath*>(path);
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  if (c != nullptr && sp != nullptr) c->drawPath(*sp, ap != nullptr ? ap->paint : SkPaint());
}

// ── Path (android.graphics.Path over SkPath) ────────────────────────────────────
// Geometry mutators are the NORMAL JNI convention here (not @CriticalNative in this
// framework version); nReset/nRewind/nSetFillType ARE @CriticalNative (handle-only).
jlong Path_nInit(JNIEnv*, jclass) { return reinterpret_cast<jlong>(new SkPath()); }
// Path(Path src) copy constructor: nInit(long nPath) clones the source SkPath (0 => empty).
jlong Path_nInitCopy(JNIEnv*, jclass, jlong src) {
  auto* s = reinterpret_cast<SkPath*>(src);
  return reinterpret_cast<jlong>(s != nullptr ? new SkPath(*s) : new SkPath());
}
jlong Path_nGetFinalizer(JNIEnv*, jclass) { return reinterpret_cast<jlong>(&free_path); }
void Path_nMoveTo(JNIEnv*, jclass, jlong p, jfloat x, jfloat y) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->moveTo(x, y);
}
void Path_nLineTo(JNIEnv*, jclass, jlong p, jfloat x, jfloat y) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->lineTo(x, y);
}
void Path_nQuadTo(JNIEnv*, jclass, jlong p, jfloat x1, jfloat y1, jfloat x2, jfloat y2) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->quadTo(x1, y1, x2, y2);
}
void Path_nCubicTo(JNIEnv*, jclass, jlong p, jfloat x1, jfloat y1, jfloat x2, jfloat y2, jfloat x3,
                   jfloat y3) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->cubicTo(x1, y1, x2, y2, x3, y3);
}
void Path_nClose(JNIEnv*, jclass, jlong p) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->close();
}
// Path.Direction: CW=0, CCW=1 (SkPathDirection matches).
void Path_nAddRect(JNIEnv*, jclass, jlong p, jfloat l, jfloat t, jfloat r, jfloat b, jint dir) {
  if (auto* s = reinterpret_cast<SkPath*>(p))
    s->addRect(SkRect::MakeLTRB(l, t, r, b), static_cast<SkPathDirection>(dir));
}
void Path_nAddOval(JNIEnv*, jclass, jlong p, jfloat l, jfloat t, jfloat r, jfloat b, jint dir) {
  if (auto* s = reinterpret_cast<SkPath*>(p))
    s->addOval(SkRect::MakeLTRB(l, t, r, b), static_cast<SkPathDirection>(dir));
}
void Path_nAddCircle(JNIEnv*, jclass, jlong p, jfloat x, jfloat y, jfloat radius, jint dir) {
  if (auto* s = reinterpret_cast<SkPath*>(p))
    s->addCircle(x, y, radius, static_cast<SkPathDirection>(dir));
}
void Path_nAddRoundRect(JNIEnv*, jclass, jlong p, jfloat l, jfloat t, jfloat r, jfloat b, jfloat rx,
                        jfloat ry, jint dir) {
  if (auto* s = reinterpret_cast<SkPath*>(p))
    s->addRoundRect(SkRect::MakeLTRB(l, t, r, b), rx, ry, static_cast<SkPathDirection>(dir));
}
// Path.nApproximate(long, float) -> float[]: sample the path into [fraction, x, y]
// triples (PathInterpolator reads the x,y of each). Needed by androidx EdgeToEdge /
// any XML PathInterpolator. Arc-length sampling via SkPathMeasure over the first
// contour (interpolator curves are single-contour, monotonic in x).
jfloatArray Path_nApproximate(JNIEnv* env, jclass, jlong p, jfloat error) {
  auto* path = reinterpret_cast<SkPath*>(p);
  if (path == nullptr) return env->NewFloatArray(0);
  const float step = error > 1e-4f ? error : 0.5f / 200.0f;
  SkPathMeasure meas(*path, false);
  const float len = meas.getLength();
  if (len <= 0.0f) {                             // degenerate → flat [0,0,0, 1,0,0]
    const float z[6] = {0, 0, 0, 1, 0, 0};
    jfloatArray a = env->NewFloatArray(6);
    env->SetFloatArrayRegion(a, 0, 6, z);
    return a;
  }
  int n = static_cast<int>(len / step) + 1;
  if (n < 2) n = 2;
  if (n > 3000) n = 3000;
  // Endpoints from the authored control points, exact — getPosTan interpolates and can land
  // a hair off, and PathInterpolator requires the first/last points to be exactly the path
  // ends (e.g. (0,0)/(1,1) for an easing curve).
  const int cnt = path->countPoints();
  const SkPoint start = cnt > 0 ? path->getPoint(0) : SkPoint{0, 0};
  const SkPoint end = cnt > 0 ? path->getPoint(cnt - 1) : SkPoint{0, 0};
  std::vector<float> out;
  out.reserve(static_cast<std::size_t>(n) * 3);
  for (int i = 0; i < n; ++i) {
    const float frac = static_cast<float>(i) / static_cast<float>(n - 1);
    SkPoint pos{};
    if (i == 0) pos = start;
    else if (i == n - 1) pos = end;
    else if (!meas.getPosTan(frac * len, &pos, nullptr)) continue;
    out.push_back(frac);
    out.push_back(pos.fX);
    out.push_back(pos.fY);
  }
  jfloatArray arr = env->NewFloatArray(static_cast<jsize>(out.size()));
  env->SetFloatArrayRegion(arr, 0, static_cast<jsize>(out.size()), out.data());
  return arr;
}
// @CriticalNative — handle-only.
void Path_nReset(jlong p) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->reset();
}
void Path_nRewind(jlong p) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->rewind();
}
// Path.FillType: WINDING=0, EVEN_ODD=1, INVERSE_WINDING=2, INVERSE_EVEN_ODD=3 (SkPathFillType matches).
void Path_nSetFillType(jlong p, jint ft) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->setFillType(static_cast<SkPathFillType>(ft));
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
// nGetClipBounds(canvas, Rect): the current clip's local-space integer bounds (false if empty).
// Regular native (takes a Rect object), unlike the @CriticalNative clip ops above.
jboolean Canvas_nGetClipBounds(JNIEnv* env, jclass, jlong canvas, jobject bounds) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  if (c == nullptr) return JNI_FALSE;
  const SkRect local = c->getLocalClipBounds();
  if (local.isEmpty()) return JNI_FALSE;
  const SkIRect r = local.roundOut();
  if (bounds != nullptr) {
    jclass rc = env->GetObjectClass(bounds);
    jmethodID set = env->GetMethodID(rc, "set", "(IIII)V");
    if (set != nullptr) env->CallVoidMethod(bounds, set, r.left(), r.top(), r.right(), r.bottom());
    env->DeleteLocalRef(rc);
  }
  return JNI_TRUE;
}
// @CriticalNative — concat the CTM with the Matrix.
void Canvas_nConcat(jlong canvas, jlong matrix) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  auto* m = reinterpret_cast<SkMatrix*>(matrix);
  if (c != nullptr && m != nullptr) c->concat(*m);
}
// @CriticalNative — replace the CTM with the Matrix (matrix==0 → identity).
void Canvas_nSetMatrix(jlong canvas, jlong matrix) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  if (c == nullptr) return;
  c->resetMatrix();
  if (auto* m = reinterpret_cast<SkMatrix*>(matrix)) c->concat(*m);
}
// @CriticalNative — read the CTM into the Matrix.
void Canvas_nGetMatrix(jlong canvas, jlong matrix) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  auto* m = reinterpret_cast<SkMatrix*>(matrix);
  if (c != nullptr && m != nullptr) *m = c->getTotalMatrix();
}
// @CriticalNative — true if the rect/path is wholly outside the current clip (cull hint).
// ViewGroup.drawChild calls this per child.
jboolean Canvas_nQuickRejectRect(jlong canvas, jfloat l, jfloat t, jfloat r, jfloat b) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  return (c != nullptr && c->quickReject(SkRect::MakeLTRB(l, t, r, b))) ? JNI_TRUE : JNI_FALSE;
}
jboolean Canvas_nQuickRejectPath(jlong canvas, jlong path) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  auto* p = reinterpret_cast<SkPath*>(path);
  return (c != nullptr && p != nullptr && c->quickReject(*p)) ? JNI_TRUE : JNI_FALSE;
}

// ── Matrix (android.graphics.Matrix over SkMatrix) — 9-value layout matches get9/set9 ──
jlong Matrix_nCreate(JNIEnv*, jclass, jlong src) {
  auto* s = reinterpret_cast<SkMatrix*>(src);
  return reinterpret_cast<jlong>(s != nullptr ? new SkMatrix(*s) : new SkMatrix());
}
jlong Matrix_nGetNativeFinalizer(JNIEnv*, jclass) { return reinterpret_cast<jlong>(&free_matrix); }
// @FastNative
void Matrix_nGetValues(JNIEnv* env, jclass, jlong obj, jfloatArray values) {
  auto* m = reinterpret_cast<SkMatrix*>(obj);
  if (m == nullptr || values == nullptr) return;
  float buf[9];
  m->get9(buf);
  env->SetFloatArrayRegion(values, 0, 9, buf);
}
// @FastNative
void Matrix_nSetValues(JNIEnv* env, jclass, jlong obj, jfloatArray values) {
  auto* m = reinterpret_cast<SkMatrix*>(obj);
  if (m == nullptr || values == nullptr) return;
  float buf[9];
  env->GetFloatArrayRegion(values, 0, 9, buf);
  m->set9(buf);
}
// @CriticalNative — handle-only.
void Matrix_nReset(jlong obj) {
  if (auto* m = reinterpret_cast<SkMatrix*>(obj)) m->reset();
}
void Matrix_nSet(jlong obj, jlong other) {
  auto* m = reinterpret_cast<SkMatrix*>(obj);
  auto* o = reinterpret_cast<SkMatrix*>(other);
  if (m != nullptr && o != nullptr) *m = *o;
}
void Matrix_nSetConcat(jlong obj, jlong a, jlong b) {
  auto* m = reinterpret_cast<SkMatrix*>(obj);
  auto* A = reinterpret_cast<SkMatrix*>(a);
  auto* B = reinterpret_cast<SkMatrix*>(b);
  if (m != nullptr && A != nullptr && B != nullptr) m->setConcat(*A, *B);
}
// The set/pre/post transform API — all @CriticalNative (handle + primitives), SkMatrix ops.
#define MTX(p) auto* m = reinterpret_cast<SkMatrix*>(p); if (m == nullptr) return
void Matrix_nSetTranslate(jlong p, jfloat dx, jfloat dy) { MTX(p); m->setTranslate(dx, dy); }
void Matrix_nSetScale4(jlong p, jfloat sx, jfloat sy, jfloat px, jfloat py) { MTX(p); m->setScale(sx, sy, px, py); }
void Matrix_nSetScale2(jlong p, jfloat sx, jfloat sy) { MTX(p); m->setScale(sx, sy); }
void Matrix_nSetRotate3(jlong p, jfloat d, jfloat px, jfloat py) { MTX(p); m->setRotate(d, px, py); }
void Matrix_nSetRotate1(jlong p, jfloat d) { MTX(p); m->setRotate(d); }
void Matrix_nSetSkew4(jlong p, jfloat kx, jfloat ky, jfloat px, jfloat py) { MTX(p); m->setSkew(kx, ky, px, py); }
void Matrix_nSetSkew2(jlong p, jfloat kx, jfloat ky) { MTX(p); m->setSkew(kx, ky); }
void Matrix_nSetSinCos4(jlong p, jfloat s, jfloat c, jfloat px, jfloat py) { MTX(p); m->setSinCos(s, c, px, py); }
void Matrix_nSetSinCos2(jlong p, jfloat s, jfloat c) { MTX(p); m->setSinCos(s, c); }
void Matrix_nPreTranslate(jlong p, jfloat dx, jfloat dy) { MTX(p); m->preTranslate(dx, dy); }
void Matrix_nPreScale4(jlong p, jfloat sx, jfloat sy, jfloat px, jfloat py) { MTX(p); m->preScale(sx, sy, px, py); }
void Matrix_nPreScale2(jlong p, jfloat sx, jfloat sy) { MTX(p); m->preScale(sx, sy); }
void Matrix_nPreRotate3(jlong p, jfloat d, jfloat px, jfloat py) { MTX(p); m->preRotate(d, px, py); }
void Matrix_nPreRotate1(jlong p, jfloat d) { MTX(p); m->preRotate(d); }
void Matrix_nPreSkew4(jlong p, jfloat kx, jfloat ky, jfloat px, jfloat py) { MTX(p); m->preSkew(kx, ky, px, py); }
void Matrix_nPreSkew2(jlong p, jfloat kx, jfloat ky) { MTX(p); m->preSkew(kx, ky); }
void Matrix_nPreConcat(jlong p, jlong o) { MTX(p); if (auto* mo = reinterpret_cast<SkMatrix*>(o)) m->preConcat(*mo); }
void Matrix_nPostTranslate(jlong p, jfloat dx, jfloat dy) { MTX(p); m->postTranslate(dx, dy); }
void Matrix_nPostScale4(jlong p, jfloat sx, jfloat sy, jfloat px, jfloat py) { MTX(p); m->postScale(sx, sy, px, py); }
void Matrix_nPostScale2(jlong p, jfloat sx, jfloat sy) { MTX(p); m->postScale(sx, sy); }
void Matrix_nPostRotate3(jlong p, jfloat d, jfloat px, jfloat py) { MTX(p); m->postRotate(d, px, py); }
void Matrix_nPostRotate1(jlong p, jfloat d) { MTX(p); m->postRotate(d); }
void Matrix_nPostSkew4(jlong p, jfloat kx, jfloat ky, jfloat px, jfloat py) { MTX(p); m->postSkew(kx, ky, px, py); }
void Matrix_nPostSkew2(jlong p, jfloat kx, jfloat ky) { MTX(p); m->postSkew(kx, ky); }
void Matrix_nPostConcat(jlong p, jlong o) { MTX(p); if (auto* mo = reinterpret_cast<SkMatrix*>(o)) m->postConcat(*mo); }
#undef MTX
jboolean Matrix_nInvert(jlong p, jlong inv) {
  auto* m = reinterpret_cast<SkMatrix*>(p); auto* i = reinterpret_cast<SkMatrix*>(inv);
  return (m != nullptr && i != nullptr && m->invert(i)) ? JNI_TRUE : JNI_FALSE;
}
jboolean Matrix_nIsIdentity(jlong p) { auto* m = reinterpret_cast<SkMatrix*>(p); return (m != nullptr && m->isIdentity()) ? JNI_TRUE : JNI_FALSE; }
jboolean Matrix_nIsAffine(jlong p) { auto* m = reinterpret_cast<SkMatrix*>(p); return (m != nullptr && !m->hasPerspective()) ? JNI_TRUE : JNI_FALSE; }
jboolean Matrix_nRectStaysRect(jlong p) { auto* m = reinterpret_cast<SkMatrix*>(p); return (m != nullptr && m->rectStaysRect()) ? JNI_TRUE : JNI_FALSE; }
jboolean Matrix_nEquals(jlong a, jlong b) { auto* ma = reinterpret_cast<SkMatrix*>(a); auto* mb = reinterpret_cast<SkMatrix*>(b); return (ma != nullptr && mb != nullptr && *ma == *mb) ? JNI_TRUE : JNI_FALSE; }
jfloat Matrix_nMapRadius(jlong p, jfloat r) { auto* m = reinterpret_cast<SkMatrix*>(p); return m != nullptr ? m->mapRadius(r) : r; }
// Object-arg variants (normal natives). RectF has float left/top/right/bottom.
void read_rectf(JNIEnv* env, jobject rect, SkRect* out) {
  jclass c = env->GetObjectClass(rect);
  out->fLeft = env->GetFloatField(rect, env->GetFieldID(c, "left", "F"));
  out->fTop = env->GetFloatField(rect, env->GetFieldID(c, "top", "F"));
  out->fRight = env->GetFloatField(rect, env->GetFieldID(c, "right", "F"));
  out->fBottom = env->GetFloatField(rect, env->GetFieldID(c, "bottom", "F"));
}
void write_rectf(JNIEnv* env, jobject rect, const SkRect& r) {
  jclass c = env->GetObjectClass(rect);
  env->SetFloatField(rect, env->GetFieldID(c, "left", "F"), r.fLeft);
  env->SetFloatField(rect, env->GetFieldID(c, "top", "F"), r.fTop);
  env->SetFloatField(rect, env->GetFieldID(c, "right", "F"), r.fRight);
  env->SetFloatField(rect, env->GetFieldID(c, "bottom", "F"), r.fBottom);
}
jboolean Matrix_nMapRect(JNIEnv* env, jclass, jlong p, jobject dst, jobject src) {
  auto* m = reinterpret_cast<SkMatrix*>(p);
  if (m == nullptr || dst == nullptr || src == nullptr) return JNI_FALSE;
  SkRect s; read_rectf(env, src, &s);
  SkRect d; const bool rectStays = m->mapRect(&d, s);
  write_rectf(env, dst, d);
  return rectStays ? JNI_TRUE : JNI_FALSE;
}
void Matrix_nMapPoints(JNIEnv* env, jclass, jlong p, jfloatArray dst, jint dstIndex,
                       jfloatArray src, jint srcIndex, jint count, jboolean isPts) {
  auto* m = reinterpret_cast<SkMatrix*>(p);
  if (m == nullptr || dst == nullptr || src == nullptr || count <= 0) return;
  jfloat* s = env->GetFloatArrayElements(src, nullptr);
  jfloat* d = env->GetFloatArrayElements(dst, nullptr);
  if (s != nullptr && d != nullptr) {
    std::vector<SkPoint> pts(count);
    for (int i = 0; i < count; ++i) pts[i] = SkPoint::Make(s[srcIndex + 2 * i], s[srcIndex + 2 * i + 1]);
    if (isPts) m->mapPoints(pts.data(), pts.data(), count);
    else m->mapVectors(pts.data(), pts.data(), count);
    for (int i = 0; i < count; ++i) { d[dstIndex + 2 * i] = pts[i].fX; d[dstIndex + 2 * i + 1] = pts[i].fY; }
  }
  if (s != nullptr) env->ReleaseFloatArrayElements(src, s, JNI_ABORT);
  if (d != nullptr) env->ReleaseFloatArrayElements(dst, d, 0);
}
jboolean Matrix_nSetRectToRect(JNIEnv* env, jclass, jlong p, jobject src, jobject dst, jint stf) {
  auto* m = reinterpret_cast<SkMatrix*>(p);
  if (m == nullptr || src == nullptr || dst == nullptr) return JNI_FALSE;
  SkRect s, d; read_rectf(env, src, &s); read_rectf(env, dst, &d);
  return m->setRectToRect(s, d, static_cast<SkMatrix::ScaleToFit>(stf)) ? JNI_TRUE : JNI_FALSE;
}
jboolean Matrix_nSetPolyToPoly(JNIEnv*, jclass, jlong, jfloatArray, jint, jfloatArray, jint, jint) {
  return JNI_FALSE;  // rarely used; not modeled
}

// ── Shader / gradients (LinearGradient, RadialGradient over SkGradientShader) ────
// Shader.TileMode: CLAMP=0, REPEAT=1, MIRROR=2 (SkTileMode matches; DECAL=3 unused here).
SkTileMode tile_mode(jint m) {
  switch (m) {
    case 1: return SkTileMode::kRepeat;
    case 2: return SkTileMode::kMirror;
    default: return SkTileMode::kClamp;
  }
}
// Decode a ColorLong[] (Android wide-color) to SkColor[]. For the sRGB color space (the
// common case) the ARGB int is packed in the high 32 bits; wide-gamut inputs degrade to
// their sRGB bits (full half-float decode is future work).
std::vector<SkColor> decode_colors(JNIEnv* env, jlongArray arr) {
  std::vector<SkColor> out;
  if (arr == nullptr) return out;
  const jsize n = env->GetArrayLength(arr);
  std::vector<jlong> cl(static_cast<std::size_t>(n));
  if (n > 0) env->GetLongArrayRegion(arr, 0, n, cl.data());
  out.reserve(static_cast<std::size_t>(n));
  for (jsize i = 0; i < n; ++i)
    out.push_back(static_cast<SkColor>(static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(cl[i]) >> 32)));
  return out;
}
std::vector<SkScalar> read_positions(JNIEnv* env, jfloatArray arr) {
  std::vector<SkScalar> pos;
  if (arr == nullptr) return pos;
  const jsize n = env->GetArrayLength(arr);
  pos.resize(static_cast<std::size_t>(n));
  if (n > 0) env->GetFloatArrayRegion(arr, 0, n, pos.data());
  return pos;
}

// LinearGradient.nativeCreate is an INSTANCE native (JNIEnv*, jobject, ...).
jlong LinearGradient_nativeCreate(JNIEnv* env, jobject, jlong matrix, jfloat x0, jfloat y0,
                                  jfloat x1, jfloat y1, jlongArray colorsArr, jfloatArray posArr,
                                  jint tileMode, jlong /*colorSpaceHandle*/) {
  const std::vector<SkColor> colors = decode_colors(env, colorsArr);
  if (colors.size() < 2) return 0;
  const std::vector<SkScalar> pos = read_positions(env, posArr);
  const SkPoint pts[2] = {{x0, y0}, {x1, y1}};
  sk_sp<SkShader> sh = SkGradientShader::MakeLinear(
      pts, colors.data(), pos.empty() ? nullptr : pos.data(),
      static_cast<int>(colors.size()), tile_mode(tileMode), 0,
      reinterpret_cast<const SkMatrix*>(matrix));
  return reinterpret_cast<jlong>(sh.release());
}

// RadialGradient.nativeCreate is a STATIC native (JNIEnv*, jclass, ...). Two-point conical
// covers both the simple radial (startRadius 0, start==end) and the general case.
jlong RadialGradient_nativeCreate(JNIEnv* env, jclass, jlong matrix, jfloat sx, jfloat sy,
                                  jfloat sr, jfloat ex, jfloat ey, jfloat er, jlongArray colorsArr,
                                  jfloatArray posArr, jint tileMode, jlong /*colorSpaceHandle*/) {
  const std::vector<SkColor> colors = decode_colors(env, colorsArr);
  if (colors.size() < 2) return 0;
  const std::vector<SkScalar> pos = read_positions(env, posArr);
  sk_sp<SkShader> sh = SkGradientShader::MakeTwoPointConical(
      {sx, sy}, sr, {ex, ey}, er, colors.data(), pos.empty() ? nullptr : pos.data(),
      static_cast<int>(colors.size()), tile_mode(tileMode), 0,
      reinterpret_cast<const SkMatrix*>(matrix));
  return reinterpret_cast<jlong>(sh.release());
}

jlong Shader_nativeGetFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&free_shader);
}

// ── RenderNode (hwui display-list node) — state-tracking shim over AclRenderNode ──
// nCreate/nGetNativeFinalizer + the object/matrix/animator ops are NORMAL (env,jclass);
// all the geometry/transform get/set are @CriticalNative (handle-only).
AclRenderNode* rnode(jlong h) { return reinterpret_cast<AclRenderNode*>(h); }

// Profiling bucket for a non-optimal path (ADR-4 hwui-drop RenderNode shim): how many
// RenderNodes the framework created. Read via acl_graphics_profile() to prioritize which
// non-optimal paths (if any) get hit hot enough to warrant a real implementation.
std::atomic<std::uint64_t> g_rn_nodes{0};

jlong RenderNode_nCreate(JNIEnv*, jclass, jstring) {
  g_rn_nodes.fetch_add(1, std::memory_order_relaxed);
  return reinterpret_cast<jlong>(new AclRenderNode());
}
jlong RenderNode_nGetNativeFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&free_rendernode);
}
void RenderNode_nOutput(JNIEnv*, jclass, jlong) {}
jint RenderNode_nGetUsageSize(JNIEnv*, jclass, jlong) { return 0; }
jint RenderNode_nGetAllocatedSize(JNIEnv*, jclass, jlong) { return 0; }
void RenderNode_nRequestPositionUpdates(JNIEnv*, jclass, jlong, jobject) {}
void RenderNode_nAddAnimator(JNIEnv*, jclass, jlong, jlong) {}
void RenderNode_nEndAllAnimators(JNIEnv*, jclass, jlong) {}
void RenderNode_nForceEndAnimators(JNIEnv*, jclass, jlong) {}

// @CriticalNative (handle-only). Macros for the uniform get/set forms; setters return
// JNI_TRUE ("value changed"), which at worst requests an extra redraw — harmless.
#define RN_SETF(fn, field) \
  jboolean fn(jlong h, jfloat v) { auto* n = rnode(h); if (n) n->field = v; return JNI_TRUE; }
#define RN_GETF(fn, field) \
  jfloat fn(jlong h) { auto* n = rnode(h); return n ? n->field : 0.0f; }
#define RN_SETI(fn, field) \
  jboolean fn(jlong h, jint v) { auto* n = rnode(h); if (n) n->field = v; return JNI_TRUE; }
#define RN_GETI(fn, field) \
  jint fn(jlong h) { auto* n = rnode(h); return n ? n->field : 0; }
#define RN_SETB(fn, field) \
  jboolean fn(jlong h, jboolean v) { auto* n = rnode(h); if (n) n->field = (v != JNI_FALSE); return JNI_TRUE; }
#define RN_GETB(fn, field) \
  jboolean fn(jlong h) { auto* n = rnode(h); return (n && n->field) ? JNI_TRUE : JNI_FALSE; }
#define RN_NOPB(fn) jboolean fn(jlong) { return JNI_TRUE; }

void RenderNode_nDiscardDisplayList(jlong) {}
jboolean RenderNode_nIsValid(jlong) { return JNI_TRUE; }
void RenderNode_nGetTransformMatrix(jlong, jlong m) {
  if (auto* mat = reinterpret_cast<SkMatrix*>(m)) mat->reset();
}
void RenderNode_nGetInverseTransformMatrix(jlong, jlong m) {
  if (auto* mat = reinterpret_cast<SkMatrix*>(m)) mat->reset();
}
jboolean RenderNode_nHasIdentityMatrix(jlong) { return JNI_TRUE; }
jboolean RenderNode_nOffsetTopAndBottom(jlong h, jint off) {
  auto* n = rnode(h); if (n) { n->top += off; n->bottom += off; } return JNI_TRUE;
}
jboolean RenderNode_nOffsetLeftAndRight(jlong h, jint off) {
  auto* n = rnode(h); if (n) { n->left += off; n->right += off; } return JNI_TRUE;
}
jboolean RenderNode_nSetLeftTopRightBottom(jlong h, jint l, jint t, jint r, jint b) {
  auto* n = rnode(h); if (n) { n->left = l; n->top = t; n->right = r; n->bottom = b; } return JNI_TRUE;
}
RN_SETI(RenderNode_nSetLeft, left)
RN_SETI(RenderNode_nSetTop, top)
RN_SETI(RenderNode_nSetRight, right)
RN_SETI(RenderNode_nSetBottom, bottom)
RN_GETI(RenderNode_nGetLeft, left)
RN_GETI(RenderNode_nGetTop, top)
RN_GETI(RenderNode_nGetRight, right)
RN_GETI(RenderNode_nGetBottom, bottom)
jint RenderNode_nGetWidth(jlong h) { auto* n = rnode(h); return n ? n->right - n->left : 0; }
jint RenderNode_nGetHeight(jlong h) { auto* n = rnode(h); return n ? n->bottom - n->top : 0; }
RN_SETF(RenderNode_nSetCameraDistance, cameraDistance)
RN_GETF(RenderNode_nGetCameraDistance, cameraDistance)
jboolean RenderNode_nSetPivotX(jlong h, jfloat v) {
  auto* n = rnode(h); if (n) { n->pivotX = v; n->pivotExplicit = true; } return JNI_TRUE;
}
jboolean RenderNode_nSetPivotY(jlong h, jfloat v) {
  auto* n = rnode(h); if (n) { n->pivotY = v; n->pivotExplicit = true; } return JNI_TRUE;
}
jboolean RenderNode_nResetPivot(jlong h) {
  auto* n = rnode(h); if (n) n->pivotExplicit = false; return JNI_TRUE;
}
RN_GETF(RenderNode_nGetPivotX, pivotX)
RN_GETF(RenderNode_nGetPivotY, pivotY)
jboolean RenderNode_nIsPivotExplicitlySet(jlong h) {
  auto* n = rnode(h); return (n && n->pivotExplicit) ? JNI_TRUE : JNI_FALSE;
}
RN_SETI(RenderNode_nSetLayerType, layerType)
RN_GETI(RenderNode_nGetLayerType, layerType)
jboolean RenderNode_nSetLayerPaint(jlong, jlong) { return JNI_TRUE; }
RN_SETB(RenderNode_nSetClipToBounds, clipToBounds)
RN_GETB(RenderNode_nGetClipToBounds, clipToBounds)
jboolean RenderNode_nSetClipBounds(jlong, jint, jint, jint, jint) { return JNI_TRUE; }
RN_NOPB(RenderNode_nSetClipBoundsEmpty)
RN_NOPB(RenderNode_nSetProjectBackwards)
RN_NOPB(RenderNode_nSetProjectionReceiver)
jboolean RenderNode_nSetOutlineRoundRect(jlong, jint, jint, jint, jint, jfloat, jfloat) {
  return JNI_TRUE;
}
jboolean RenderNode_nSetOutlinePath(jlong, jlong, jfloat) { return JNI_TRUE; }
RN_NOPB(RenderNode_nSetOutlineEmpty)
RN_NOPB(RenderNode_nSetOutlineNone)
RN_NOPB(RenderNode_nClearStretch)
jboolean RenderNode_nStretch(jlong, jfloat, jfloat, jfloat, jfloat) { return JNI_TRUE; }
jboolean RenderNode_nHasShadow(jlong) { return JNI_FALSE; }
RN_SETI(RenderNode_nSetSpotShadowColor, spotShadowColor)
RN_SETI(RenderNode_nSetAmbientShadowColor, ambientShadowColor)
RN_GETI(RenderNode_nGetSpotShadowColor, spotShadowColor)
RN_GETI(RenderNode_nGetAmbientShadowColor, ambientShadowColor)
RN_SETB(RenderNode_nSetClipToOutline, clipToOutline)
RN_GETB(RenderNode_nGetClipToOutline, clipToOutline)
jboolean RenderNode_nSetRevealClip(jlong, jboolean, jfloat, jfloat, jfloat) { return JNI_TRUE; }
RN_SETF(RenderNode_nSetAlpha, alpha)
RN_GETF(RenderNode_nGetAlpha, alpha)
jboolean RenderNode_nSetRenderEffect(jlong, jlong) { return JNI_TRUE; }
RN_SETB(RenderNode_nSetHasOverlappingRendering, hasOverlappingRendering)
RN_GETB(RenderNode_nHasOverlappingRendering, hasOverlappingRendering)
void RenderNode_nSetUsageHint(jlong, jint) {}
RN_SETF(RenderNode_nSetElevation, elevation)
RN_GETF(RenderNode_nGetElevation, elevation)
RN_SETF(RenderNode_nSetTranslationX, translationX)
RN_SETF(RenderNode_nSetTranslationY, translationY)
RN_SETF(RenderNode_nSetTranslationZ, translationZ)
RN_GETF(RenderNode_nGetTranslationX, translationX)
RN_GETF(RenderNode_nGetTranslationY, translationY)
RN_GETF(RenderNode_nGetTranslationZ, translationZ)
RN_SETF(RenderNode_nSetRotation, rotation)
RN_SETF(RenderNode_nSetRotationX, rotationX)
RN_SETF(RenderNode_nSetRotationY, rotationY)
RN_GETF(RenderNode_nGetRotation, rotation)
RN_GETF(RenderNode_nGetRotationX, rotationX)
RN_GETF(RenderNode_nGetRotationY, rotationY)
RN_SETF(RenderNode_nSetScaleX, scaleX)
RN_SETF(RenderNode_nSetScaleY, scaleY)
RN_GETF(RenderNode_nGetScaleX, scaleX)
RN_GETF(RenderNode_nGetScaleY, scaleY)
jboolean RenderNode_nSetStaticMatrix(jlong, jlong) { return JNI_TRUE; }
jboolean RenderNode_nSetAnimationMatrix(jlong, jlong) { return JNI_TRUE; }
jboolean RenderNode_nGetAnimationMatrix(jlong, jlong) { return JNI_FALSE; }
RN_SETB(RenderNode_nSetAllowForceDark, allowForceDark)
RN_GETB(RenderNode_nGetAllowForceDark, allowForceDark)
jlong RenderNode_nGetUniqueId(jlong h) { return h; }
#undef RN_SETF
#undef RN_GETF
#undef RN_SETI
#undef RN_GETI
#undef RN_SETB
#undef RN_GETB
#undef RN_NOPB

// ── Paint ─────────────────────────────────────────────────────────────────────
jlong Paint_nInit(JNIEnv*, jclass) {
  auto* ap = new AclPaint();
  ap->font.setTypeface(default_typeface());
  ap->font.setSize(16.0f);  // framework default is ~16px; overridden by nSetTextSize
  return reinterpret_cast<jlong>(ap);
}
// Paint(Paint) copy ctor — new AclPaint copied from the source (SkPaint + SkFont copy).
jlong Paint_nInitWithPaint(JNIEnv*, jclass, jlong src) {
  auto* s = reinterpret_cast<AclPaint*>(src);
  return reinterpret_cast<jlong>(s != nullptr ? new AclPaint(*s) : new AclPaint());
}
// Paint.set(Paint): copy src into dst.
void Paint_nSet(jlong dst, jlong src) {
  auto* d = reinterpret_cast<AclPaint*>(dst);
  auto* s = reinterpret_cast<AclPaint*>(src);
  if (d != nullptr && s != nullptr) *d = *s;
}
// Mechanical SkPaint/SkFont getters+setters (all @CriticalNative — handle-only). Batched
// so Material/TextView construction and text layout resolve without one-off UnsatisfiedLinks.
#define ACLP(ptr) auto* ap = reinterpret_cast<AclPaint*>(ptr)
jfloat Paint_nGetTextSize(jlong p) { ACLP(p); return ap ? ap->font.getSize() : 0; }
jint Paint_nGetStyle(jlong p) { ACLP(p); return ap ? static_cast<jint>(ap->paint.getStyle()) : 0; }
jint Paint_nGetStrokeCap(jlong p) { ACLP(p); return ap ? static_cast<jint>(ap->paint.getStrokeCap()) : 0; }
jint Paint_nGetStrokeJoin(jlong p) { ACLP(p); return ap ? static_cast<jint>(ap->paint.getStrokeJoin()) : 0; }
void Paint_nSetStrokeJoin(jlong p, jint j) { ACLP(p); if (ap) ap->paint.setStrokeJoin(static_cast<SkPaint::Join>(j)); }
jfloat Paint_nGetStrokeMiter(jlong p) { ACLP(p); return ap ? ap->paint.getStrokeMiter() : 0; }
void Paint_nSetStrokeMiter(jlong p, jfloat m) { ACLP(p); if (ap) ap->paint.setStrokeMiter(m); }
void Paint_nSetAlpha(jlong p, jint a) { ACLP(p); if (ap) ap->paint.setAlpha(static_cast<U8CPU>(a)); }
jfloat Paint_nGetTextScaleX(jlong p) { ACLP(p); return ap ? ap->font.getScaleX() : 1; }
void Paint_nSetTextScaleX(jlong p, jfloat s) { ACLP(p); if (ap) ap->font.setScaleX(s); }
jfloat Paint_nGetTextSkewX(jlong p) { ACLP(p); return ap ? ap->font.getSkewX() : 0; }
void Paint_nSetTextSkewX(jlong p, jfloat s) { ACLP(p); if (ap) ap->font.setSkewX(s); }
void Paint_nSetTypeface(jlong p, jlong tf) { ACLP(p); if (ap) ap->font.setTypeface(sk_ref_sp(reinterpret_cast<SkTypeface*>(tf))); }
jint Paint_nGetTextAlign(jlong p) { ACLP(p); return ap ? ap->textAlign : 0; }
void Paint_nSetTextAlign(jlong p, jint a) { ACLP(p); if (ap) ap->textAlign = a; }
jfloat Paint_nGetLetterSpacing(jlong p) { ACLP(p); return ap ? ap->letterSpacing : 0; }
void Paint_nSetLetterSpacing(jlong p, jfloat s) { ACLP(p); if (ap) ap->letterSpacing = s; }
// Filters/effects acl doesn't model yet: color filter is applied (SkColorFilter*), the rest
// are accepted and ignored. The (JJ)J setters return the incoming handle (Java stores it).
jlong Paint_nSetColorFilter(jlong p, jlong cf) { ACLP(p); if (ap) ap->paint.setColorFilter(sk_ref_sp(reinterpret_cast<SkColorFilter*>(cf))); return cf; }

// android.graphics.ColorFilter / BlendModeColorFilter: a tint (color + blend mode) applied to a
// Paint — pervasive (drawable/icon tinting). native_CreateBlendModeFilter → SkColorFilters::Blend;
// the ColorFilter finalizer unrefs the SkColorFilter (its NativeAllocationRegistry needs it).
void free_colorfilter(void* p) { SkSafeUnref(reinterpret_cast<SkColorFilter*>(p)); }
jlong ColorFilter_nativeGetFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&free_colorfilter);
}
jlong BlendModeColorFilter_native_Create(JNIEnv*, jclass, jint color, jint blendmode) {
  const SkBlendMode mode = (blendmode >= 0 && blendmode <= static_cast<jint>(SkBlendMode::kLastMode))
                               ? static_cast<SkBlendMode>(blendmode)
                               : SkBlendMode::kSrcOver;
  sk_sp<SkColorFilter> cf = SkColorFilters::Blend(static_cast<SkColor>(color), mode);
  return reinterpret_cast<jlong>(cf.release());  // may be null (no-op blend); Java tolerates it
}
jlong Paint_nSetPathEffect(jlong /*p*/, jlong e) { return e; }
jlong Paint_nSetMaskFilter(jlong /*p*/, jlong f) { return f; }
void Paint_nSetShadowLayer(jlong, jfloat, jfloat, jfloat, jlong, jlong) {}
void Paint_nSetDither(jlong p, jboolean d) { ACLP(p); if (ap) ap->paint.setDither(d); }
void Paint_nSetLinearText(jlong, jboolean) {}
void Paint_nSetSubpixelText(jlong p, jboolean on) { ACLP(p); if (ap) ap->font.setSubpixel(on); }
void Paint_nSetUnderlineText(jlong, jboolean) {}
jint Paint_nGetHinting(jlong p) { ACLP(p); return ap ? static_cast<jint>(ap->font.getHinting()) : 0; }
void Paint_nSetHinting(jlong p, jint h) { ACLP(p); if (ap) ap->font.setHinting(static_cast<SkFontHinting>(h)); }
jfloat Paint_nGetWordSpacing(jlong) { return 0; }
void Paint_nSetWordSpacing(jlong, jfloat) {}
jint Paint_nGetStartHyphenEdit(jlong) { return 0; }
jint Paint_nGetEndHyphenEdit(jlong) { return 0; }
void Paint_nSetStartHyphenEdit(jlong, jint) {}
void Paint_nSetEndHyphenEdit(jlong, jint) {}
void Paint_nSetFontFeatureSettings(JNIEnv*, jclass, jlong, jstring) {}
void Paint_nSetFakeBoldText(jlong p, jboolean on) { ACLP(p); if (ap) ap->font.setEmbolden(on); }
void Paint_nSetFilterBitmap(jlong, jboolean) {}
void Paint_nSetStrikeThruText(jlong, jboolean) {}
void Paint_nSetUnderlineTextFlag(jlong, jboolean) {}
jboolean Paint_nIsElegantTextHeight(jlong) { return JNI_FALSE; }
void Paint_nSetElegantTextHeight(jlong, jboolean) {}
jboolean Paint_nHasShadowLayer(jlong) { return JNI_FALSE; }
jfloat Paint_nAscent(jlong p) { ACLP(p); if (!ap) return 0; SkFontMetrics m; ap->font.getMetrics(&m); return m.fAscent; }
jfloat Paint_nDescent(jlong p) { ACLP(p); if (!ap) return 0; SkFontMetrics m; ap->font.getMetrics(&m); return m.fDescent; }
jfloat Paint_nGetUnderlinePosition(jlong p) { ACLP(p); if (!ap) return 0; SkFontMetrics m; ap->font.getMetrics(&m); float u = 0; m.hasUnderlinePosition(&u); return u; }
jfloat Paint_nGetUnderlineThickness(jlong p) { ACLP(p); if (!ap) return 0; SkFontMetrics m; ap->font.getMetrics(&m); float t = 0; m.hasUnderlineThickness(&t); return t; }
jfloat Paint_nGetStrikeThruPosition(jlong p) { ACLP(p); if (!ap) return 0; SkFontMetrics m; ap->font.getMetrics(&m); return m.fAscent / 2.0f; }
jfloat Paint_nGetStrikeThruThickness(jlong p) { ACLP(p); if (!ap) return 0; SkFontMetrics m; ap->font.getMetrics(&m); float t = 0; m.hasUnderlineThickness(&t); return t; }
jboolean Paint_nEqualsForTextMeasurement(jlong a, jlong b) {
  auto* pa = reinterpret_cast<AclPaint*>(a); auto* pb = reinterpret_cast<AclPaint*>(b);
  return (pa && pb && pa->font.getSize() == pb->font.getSize()
          && pa->font.getTypeface() == pb->font.getTypeface()) ? JNI_TRUE : JNI_FALSE;
}
// Approximate: the fill path of a shape is the shape itself (no stroke expansion modeled).
jboolean Paint_nGetFillPath(jlong, jlong src, jlong dst) {
  auto* s = reinterpret_cast<SkPath*>(src); auto* d = reinterpret_cast<SkPath*>(dst);
  if (s == nullptr || d == nullptr) return JNI_FALSE;
  *d = *s;
  return JNI_TRUE;
}
// nGetFontMetrics(paint, Paint$FontMetrics) — fill top/ascent/descent/bottom/leading; return
// the recommended line spacing.
jfloat Paint_nGetFontMetrics(JNIEnv* env, jclass, jlong p, jobject fm) {
  auto* ap = reinterpret_cast<AclPaint*>(p);
  if (ap == nullptr) return 0;
  SkFontMetrics m;
  const float spacing = ap->font.getMetrics(&m);
  if (fm != nullptr) {
    static jfieldID ft = nullptr, fa, fd, fb, fl;
    if (ft == nullptr) {
      jclass c = env->GetObjectClass(fm);
      ft = env->GetFieldID(c, "top", "F"); fa = env->GetFieldID(c, "ascent", "F");
      fd = env->GetFieldID(c, "descent", "F"); fb = env->GetFieldID(c, "bottom", "F");
      fl = env->GetFieldID(c, "leading", "F");
    }
    env->SetFloatField(fm, ft, m.fTop); env->SetFloatField(fm, fa, m.fAscent);
    env->SetFloatField(fm, fd, m.fDescent); env->SetFloatField(fm, fb, m.fBottom);
    env->SetFloatField(fm, fl, m.fLeading);
  }
  return spacing;
}
jboolean Paint_nHasGlyph(JNIEnv*, jclass, jlong p, jint, jstring) {
  return reinterpret_cast<AclPaint*>(p) != nullptr ? JNI_TRUE : JNI_FALSE;
}
// Advance width of UTF-16 units [start, offset) — text run measurement for layout.
jfloat Paint_nGetRunAdvance(JNIEnv* env, jclass, jlong p, jcharArray text, jint start, jint /*end*/,
                            jint /*ctxStart*/, jint /*ctxEnd*/, jboolean /*isRtl*/, jint offset) {
  auto* ap = reinterpret_cast<AclPaint*>(p);
  if (ap == nullptr || text == nullptr) return 0;
  jchar* chars = env->GetCharArrayElements(text, nullptr);
  if (chars == nullptr) return 0;
  const jsize len = env->GetArrayLength(text);
  const int s = start < 0 ? 0 : start;
  const int o = offset > len ? len : offset;
  float adv = 0;
  if (o > s)
    adv = ap->font.measureText(chars + s, static_cast<std::size_t>(o - s) * 2,
                               SkTextEncoding::kUTF16, nullptr, &ap->paint);
  env->ReleaseCharArrayElements(text, chars, JNI_ABORT);
  return adv;
}
// Offset whose advance from start is closest to `advance` (per-unit walk).
jint Paint_nGetOffsetForAdvance(JNIEnv* env, jclass, jlong p, jcharArray text, jint start, jint end,
                                jint, jint, jboolean, jfloat advance) {
  auto* ap = reinterpret_cast<AclPaint*>(p);
  if (ap == nullptr || text == nullptr) return start;
  jchar* chars = env->GetCharArrayElements(text, nullptr);
  if (chars == nullptr) return start;
  const jsize len = env->GetArrayLength(text);
  const int s = start < 0 ? 0 : start;
  const int e = end > len ? len : end;
  float acc = 0; int off = s;
  for (int i = s; i < e; ++i) {
    const float w = ap->font.measureText(chars + i, 2, SkTextEncoding::kUTF16, nullptr, &ap->paint);
    if (acc + w * 0.5f > advance) break;
    acc += w; off = i + 1;
  }
  env->ReleaseCharArrayElements(text, chars, JNI_ABORT);
  return off;
}
void fill_metrics_int(JNIEnv* env, AclPaint* ap, jobject fmi) {
  if (fmi == nullptr) return;
  SkFontMetrics m; ap->font.getMetrics(&m);
  static jfieldID ft = nullptr, fa, fd, fb, fl;
  if (ft == nullptr) {
    jclass c = env->GetObjectClass(fmi);
    ft = env->GetFieldID(c, "top", "I"); fa = env->GetFieldID(c, "ascent", "I");
    fd = env->GetFieldID(c, "descent", "I"); fb = env->GetFieldID(c, "bottom", "I");
    fl = env->GetFieldID(c, "leading", "I");
  }
  env->SetIntField(fmi, ft, static_cast<jint>(std::floor(m.fTop)));
  env->SetIntField(fmi, fa, static_cast<jint>(std::floor(m.fAscent)));
  env->SetIntField(fmi, fd, static_cast<jint>(std::ceil(m.fDescent)));
  env->SetIntField(fmi, fb, static_cast<jint>(std::ceil(m.fBottom)));
  env->SetIntField(fmi, fl, static_cast<jint>(std::ceil(m.fLeading)));
}
void Paint_nGetFontMetricsIntForTextChars(JNIEnv* env, jclass, jlong p, jcharArray, jint, jint,
                                          jint, jint, jboolean, jobject fmi) {
  if (auto* ap = reinterpret_cast<AclPaint*>(p)) fill_metrics_int(env, ap, fmi);
}
void Paint_nGetFontMetricsIntForTextString(JNIEnv* env, jclass, jlong p, jstring, jint, jint,
                                           jint, jint, jboolean, jobject fmi) {
  if (auto* ap = reinterpret_cast<AclPaint*>(p)) fill_metrics_int(env, ap, fmi);
}
void Paint_nGetCharArrayBounds(JNIEnv* env, jclass, jlong p, jcharArray text, jint index, jint count,
                               jint, jobject rect) {
  auto* ap = reinterpret_cast<AclPaint*>(p);
  if (ap == nullptr || text == nullptr || rect == nullptr) return;
  jchar* chars = env->GetCharArrayElements(text, nullptr);
  if (chars == nullptr) return;
  const jsize len = env->GetArrayLength(text);
  const int i = index < 0 ? 0 : index;
  const int cnt = (i + count > len) ? (len - i) : count;
  SkRect b{};
  if (cnt > 0)
    ap->font.measureText(chars + i, static_cast<std::size_t>(cnt) * 2, SkTextEncoding::kUTF16,
                         &b, &ap->paint);
  env->ReleaseCharArrayElements(text, chars, JNI_ABORT);
  static jfieldID fl = nullptr, ft, fr, fb;
  if (fl == nullptr) {
    jclass c = env->GetObjectClass(rect);
    fl = env->GetFieldID(c, "left", "I"); ft = env->GetFieldID(c, "top", "I");
    fr = env->GetFieldID(c, "right", "I"); fb = env->GetFieldID(c, "bottom", "I");
  }
  env->SetIntField(rect, fl, static_cast<jint>(std::floor(b.fLeft)));
  env->SetIntField(rect, ft, static_cast<jint>(std::floor(b.fTop)));
  env->SetIntField(rect, fr, static_cast<jint>(std::ceil(b.fRight)));
  env->SetIntField(rect, fb, static_cast<jint>(std::ceil(b.fBottom)));
}
// Cursor motion: step one unit toward the run direction (no grapheme clustering).
// (long, text, contextStart, contextEnd, dir, offset, cursorOpt)
jint Paint_nGetTextRunCursorChars(JNIEnv*, jclass, jlong, jcharArray, jint, jint, jint,
                                  jint offset, jint opt) {
  return opt == 0 /*AFTER*/ ? offset + 1 : (opt == 2 /*BEFORE*/ ? offset - 1 : offset);
}
jint Paint_nGetTextRunCursorString(JNIEnv*, jclass, jlong, jstring, jint, jint, jint,
                                   jint offset, jint opt) {
  return opt == 0 ? offset + 1 : (opt == 2 ? offset - 1 : offset);
}
jint Paint_nBreakTextChars(JNIEnv* env, jclass, jlong p, jcharArray text, jint index, jint count,
                           jfloat /*maxWidth*/, jint /*bidiFlags*/, jfloatArray measured) {
  auto* ap = reinterpret_cast<AclPaint*>(p);
  const int n = count < 0 ? -count : count;
  if (ap != nullptr && text != nullptr && measured != nullptr && env->GetArrayLength(measured) > 0) {
    jchar* chars = env->GetCharArrayElements(text, nullptr);
    if (chars != nullptr) {
      const jsize len = env->GetArrayLength(text);
      const int i = index < 0 ? 0 : index;
      const int cnt = (i + n > len) ? (len - i) : n;
      const float w = cnt > 0 ? ap->font.measureText(chars + i, static_cast<std::size_t>(cnt) * 2,
                                                      SkTextEncoding::kUTF16, nullptr, &ap->paint) : 0;
      env->SetFloatArrayRegion(measured, 0, 1, &w);
      env->ReleaseCharArrayElements(text, chars, JNI_ABORT);
    }
  }
  return n;  // best-effort: everything fits (no wrapping in the measure stub)
}
jint Paint_nBreakTextString(JNIEnv* env, jclass, jlong p, jstring text, jboolean, jfloat, jint,
                            jfloatArray measured) {
  auto* ap = reinterpret_cast<AclPaint*>(p);
  if (text == nullptr) return 0;
  const jsize n = env->GetStringLength(text);
  if (ap != nullptr && measured != nullptr && env->GetArrayLength(measured) > 0) {
    const jchar* chars = env->GetStringChars(text, nullptr);
    if (chars != nullptr) {
      const float w = n > 0 ? ap->font.measureText(chars, static_cast<std::size_t>(n) * 2,
                                                   SkTextEncoding::kUTF16, nullptr, &ap->paint) : 0;
      env->SetFloatArrayRegion(measured, 0, 1, &w);
      env->ReleaseStringChars(text, chars);
    }
  }
  return n;
}
#undef ACLP
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
// @CriticalNative — Paint.nSetXfermode(paint, porterDuffMode): the int is an SkBlendMode
// value (PorterDuff.Mode.nativeInt aligns with SkBlendMode). Modern Paint keeps the blend
// mode on the paint (no SkXfermode object). Used by Material MaterialShapeDrawable.
void Paint_nSetXfermode(jlong paintPtr, jint mode) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  if (ap == nullptr) return;
  if (mode >= 0 && mode <= static_cast<jint>(SkBlendMode::kLastMode))
    ap->paint.setBlendMode(static_cast<SkBlendMode>(mode));
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
// @CriticalNative — set the paint's shader (handle-only); returns the shader handle.
jlong Paint_nSetShader(jlong paintPtr, jlong shader) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  if (ap != nullptr) ap->paint.setShader(sk_ref_sp(reinterpret_cast<SkShader*>(shader)));
  return shader;
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
// Backs Paint.measureText / getTextWidths: return the total advance of text[start,end); fill
// per-glyph advances if the array is provided. Latin path (per-char measure; no shaping).
jfloat Paint_nGetTextAdvances(JNIEnv* env, jclass, jlong paintPtr, jstring text, jint start,
                              jint end, jint, jint, jint, jfloatArray advances, jint advIdx) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  if (ap == nullptr || text == nullptr) return 0.0f;
  const char* utf8 = env->GetStringUTFChars(text, nullptr);
  if (utf8 == nullptr) return 0.0f;
  const int len = static_cast<int>(std::strlen(utf8));
  const int s = start < 0 ? 0 : start;
  const int e = end > len ? len : end;
  float total = 0.0f;
  if (e > s) {
    total = ap->font.measureText(utf8 + s, static_cast<std::size_t>(e - s), SkTextEncoding::kUTF8,
                                 nullptr, &ap->paint);
    if (advances != nullptr) {
      std::vector<jfloat> adv(static_cast<std::size_t>(e - s), 0.0f);
      for (int i = 0; i < e - s; ++i)
        adv[i] = ap->font.measureText(utf8 + s + i, 1, SkTextEncoding::kUTF8, nullptr, &ap->paint);
      env->SetFloatArrayRegion(advances, advIdx, e - s, adv.data());
    }
  }
  env->ReleaseStringUTFChars(text, utf8);
  return total;
}
// Backs Paint.getTextBounds(String): fill the Rect with the text's tight bounds (relative to
// the baseline at the origin — top is negative, matching Skia's measureText bounds).
void Paint_nGetStringBounds(JNIEnv* env, jclass, jlong paintPtr, jstring text, jint start, jint end,
                            jint, jobject rect) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  if (ap == nullptr || text == nullptr || rect == nullptr) return;
  const char* utf8 = env->GetStringUTFChars(text, nullptr);
  if (utf8 == nullptr) return;
  const int len = static_cast<int>(std::strlen(utf8));
  const int s = start < 0 ? 0 : start;
  const int e = end > len ? len : end;
  SkRect b = SkRect::MakeEmpty();
  if (e > s)
    ap->font.measureText(utf8 + s, static_cast<std::size_t>(e - s), SkTextEncoding::kUTF8, &b,
                         &ap->paint);
  env->ReleaseStringUTFChars(text, utf8);
  jclass rc = env->GetObjectClass(rect);
  const auto set = [&](const char* f, int v) {
    if (jfieldID id = env->GetFieldID(rc, f, "I")) env->SetIntField(rect, id, v);
  };
  set("left", static_cast<int>(std::floor(b.fLeft)));
  set("top", static_cast<int>(std::floor(b.fTop)));
  set("right", static_cast<int>(std::ceil(b.fRight)));
  set("bottom", static_cast<int>(std::ceil(b.fBottom)));
  env->DeleteLocalRef(rc);
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

// ── PathParser (android.util.PathParser$PathData) ───────────────────────────────
// A PathData is just an SkPath parsed from an SVG-style path string. VectorDrawable
// holds one per <path> (for morphing/animation); the render path is set separately
// via VectorDrawable.nSetPathString. Morph interpolation isn't supported (returns
// false → no path animation), which static drawables don't use.
struct AclPathData {
  SkPath path;
};
jlong PathData_nCreateEmpty(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(new AclPathData());
}
jlong PathData_nCreateFromString(JNIEnv* env, jclass, jstring str, jint /*len*/) {
  auto* pd = new AclPathData();
  if (str != nullptr) {
    const char* s = env->GetStringUTFChars(str, nullptr);
    if (s != nullptr) { SkParsePath::FromSVGString(s, &pd->path); env->ReleaseStringUTFChars(str, s); }
  }
  return reinterpret_cast<jlong>(pd);
}
jlong PathData_nCreateCopy(JNIEnv*, jclass, jlong src) {
  auto* s = reinterpret_cast<AclPathData*>(src);
  return reinterpret_cast<jlong>(s != nullptr ? new AclPathData(*s) : new AclPathData());
}
void PathData_nSet(JNIEnv*, jclass, jlong dst, jlong src) {
  auto* d = reinterpret_cast<AclPathData*>(dst);
  auto* s = reinterpret_cast<AclPathData*>(src);
  if (d != nullptr && s != nullptr) d->path = s->path;
}
void PathData_nParseString(JNIEnv* env, jclass, jlong pd, jstring str, jint /*len*/) {
  auto* d = reinterpret_cast<AclPathData*>(pd);
  if (d == nullptr || str == nullptr) return;
  const char* s = env->GetStringUTFChars(str, nullptr);
  if (s != nullptr) { d->path.reset(); SkParsePath::FromSVGString(s, &d->path); env->ReleaseStringUTFChars(str, s); }
}
void PathData_nCreatePathFromData(JNIEnv*, jclass, jlong outPath, jlong pd) {
  auto* p = reinterpret_cast<SkPath*>(outPath);
  auto* d = reinterpret_cast<AclPathData*>(pd);
  if (p != nullptr && d != nullptr) *p = d->path;
}
jboolean PathData_nInterpolate(JNIEnv*, jclass, jlong, jlong, jlong, jfloat) { return JNI_FALSE; }
jboolean PathData_nCanMorph(JNIEnv*, jclass, jlong, jlong) { return JNI_FALSE; }
void PathData_nFinalize(JNIEnv*, jclass, jlong pd) { delete reinterpret_cast<AclPathData*>(pd); }
// nParseStringForPath(long skPath, String, int): parse an SVG string into an SkPath.
void PathParser_nParseStringForPath(JNIEnv* env, jclass, jlong skPath, jstring str, jint /*len*/) {
  auto* p = reinterpret_cast<SkPath*>(skPath);
  if (p == nullptr || str == nullptr) return;
  const char* s = env->GetStringUTFChars(str, nullptr);
  if (s != nullptr) { p->reset(); SkParsePath::FromSVGString(s, p); env->ReleaseStringUTFChars(str, s); }
}

// ── VectorDrawable (android.graphics.drawable.VectorDrawable) ────────────────────
// A retained SkPath/SkPaint scene-graph (Tree → Groups → FullPaths/ClipPaths). acl
// dropped hwui (ADR-4), so nDraw renders the tree DIRECTLY onto the (GPU Ganesh)
// SkCanvas instead of hwui's bitmap-cache/texture-atlas indirection.

// ABI-compatible with android::VirtualLightRefBase (hwui, off our include path): a
// vtable ptr + an atomic refcount, deleting on last release. com.android.internal.
// util.VirtualRefBasePtr (in libhwui) reinterpret_casts the tree ptr to this and
// inc/decStrongs it, so the layout must match. Only the Tree is Java-ref-managed.
class VirtualLightRefBase {
 public:
  virtual ~VirtualLightRefBase() = default;
  void incStrong(const void*) const { mRefCount.fetch_add(1, std::memory_order_relaxed); }
  void decStrong(const void*) const {
    if (mRefCount.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
  }
 private:
  mutable std::atomic<int32_t> mRefCount{0};
};

SkColor vd_apply_alpha(SkColor color, float alpha) {
  return SkColorSetA(color, static_cast<U8CPU>(SkColorGetA(color) * alpha));
}
void vd_apply_trim(SkPath* out, const SkPath& in, float start, float end, float offset) {
  if (start == 0.0f && end == 1.0f) { *out = in; return; }
  out->reset();
  if (start == end) return;
  SkPathMeasure measure(in, false);
  const float len = measure.getLength();
  const float s = len * std::fmod(start + offset, 1.0f);
  const float e = len * std::fmod(end + offset, 1.0f);
  if (s > e) {
    measure.getSegment(s, len, out, true);
    if (e > 0) measure.getSegment(0, e, out, true);
  } else {
    measure.getSegment(s, e, out, true);
  }
}

struct VDNode {
  std::string name;
  virtual ~VDNode() = default;
  virtual void draw(SkCanvas*, bool antiAlias) = 0;
  virtual std::unique_ptr<VDNode> clone() const = 0;
};

struct VDFullPath : VDNode {
  SkPath path;
  float strokeWidth = 0;
  SkColor strokeColor = SK_ColorTRANSPARENT;
  float strokeAlpha = 1;
  SkColor fillColor = SK_ColorTRANSPARENT;
  float fillAlpha = 1;
  float trimStart = 0, trimEnd = 1, trimOffset = 0;
  int strokeLineCap = 0, strokeLineJoin = 0;  // SkPaint::Cap/Join match
  float strokeMiter = 4;
  int fillType = 0;  // SkPathFillType: 0 winding, 1 even-odd
  sk_sp<SkShader> fillGradient, strokeGradient;

  SkPath renderPath() const {
    SkPath out;
    vd_apply_trim(&out, path, trimStart, trimEnd, trimOffset);
    if (fillGradient || fillColor != SK_ColorTRANSPARENT)
      out.setFillType(static_cast<SkPathFillType>(fillType));
    return out;
  }
  void draw(SkCanvas* c, bool antiAlias) override {
    const SkPath rp = renderPath();
    SkPaint fill;
    bool needsFill = false;
    if (fillGradient) {
      fill.setColor(vd_apply_alpha(SK_ColorBLACK, fillAlpha));
      fill.setShader(fillGradient);
      needsFill = true;
    } else if (fillColor != SK_ColorTRANSPARENT) {
      fill.setColor(vd_apply_alpha(fillColor, fillAlpha));
      needsFill = true;
    }
    if (needsFill) {
      fill.setStyle(SkPaint::kFill_Style);
      fill.setAntiAlias(antiAlias);
      c->drawPath(rp, fill);
    }
    SkPaint stroke;
    bool needsStroke = false;
    if (strokeGradient) {
      stroke.setColor(vd_apply_alpha(SK_ColorBLACK, strokeAlpha));
      stroke.setShader(strokeGradient);
      needsStroke = true;
    } else if (strokeColor != SK_ColorTRANSPARENT) {
      stroke.setColor(vd_apply_alpha(strokeColor, strokeAlpha));
      needsStroke = true;
    }
    if (needsStroke) {
      stroke.setStyle(SkPaint::kStroke_Style);
      stroke.setAntiAlias(antiAlias);
      stroke.setStrokeJoin(static_cast<SkPaint::Join>(strokeLineJoin));
      stroke.setStrokeCap(static_cast<SkPaint::Cap>(strokeLineCap));
      stroke.setStrokeMiter(strokeMiter);
      stroke.setStrokeWidth(strokeWidth);
      c->drawPath(rp, stroke);
    }
  }
  std::unique_ptr<VDNode> clone() const override {
    return std::make_unique<VDFullPath>(*this);
  }
};

struct VDClipPath : VDNode {
  SkPath path;
  void draw(SkCanvas* c, bool /*aa*/) override { c->clipPath(path, true); }
  std::unique_ptr<VDNode> clone() const override {
    return std::make_unique<VDClipPath>(*this);
  }
};

struct VDGroup : VDNode {
  float rotate = 0, pivotX = 0, pivotY = 0, scaleX = 1, scaleY = 1, translateX = 0, translateY = 0;
  std::vector<std::unique_ptr<VDNode>> children;
  void draw(SkCanvas* c, bool antiAlias) override {
    SkAutoCanvasRestore acr(c, true);
    SkMatrix m;
    m.reset();
    m.postTranslate(-pivotX, -pivotY);
    m.postScale(scaleX, scaleY);
    m.postRotate(rotate, 0, 0);
    m.postTranslate(translateX + pivotX, translateY + pivotY);
    c->concat(m);
    for (auto& ch : children) ch->draw(c, antiAlias);
  }
  std::unique_ptr<VDNode> clone() const override {
    auto g = std::make_unique<VDGroup>();
    g->name = name;
    g->rotate = rotate; g->pivotX = pivotX; g->pivotY = pivotY;
    g->scaleX = scaleX; g->scaleY = scaleY; g->translateX = translateX; g->translateY = translateY;
    for (const auto& ch : children) g->children.push_back(ch->clone());
    return g;
  }
};

struct VDTree : VirtualLightRefBase {
  std::unique_ptr<VDGroup> root;
  float viewportW = 0, viewportH = 0;
  float rootAlpha = 1;
  bool antiAlias = true;
  bool allowCaching = true;
};

// nCreate*
jlong VD_nCreateFullPath(JNIEnv*, jclass) { return reinterpret_cast<jlong>(new VDFullPath()); }
jlong VD_nCreateFullPathCopy(JNIEnv*, jclass, jlong src) {
  auto* s = reinterpret_cast<VDFullPath*>(src);
  return reinterpret_cast<jlong>(s != nullptr ? new VDFullPath(*s) : new VDFullPath());
}
jlong VD_nCreateClipPath(JNIEnv*, jclass) { return reinterpret_cast<jlong>(new VDClipPath()); }
jlong VD_nCreateClipPathCopy(JNIEnv*, jclass, jlong src) {
  auto* s = reinterpret_cast<VDClipPath*>(src);
  return reinterpret_cast<jlong>(s != nullptr ? new VDClipPath(*s) : new VDClipPath());
}
jlong VD_nCreateGroup(JNIEnv*, jclass) { return reinterpret_cast<jlong>(new VDGroup()); }
jlong VD_nCreateGroupCopy(JNIEnv*, jclass, jlong src) {
  auto* s = reinterpret_cast<VDGroup*>(src);
  if (s == nullptr) return reinterpret_cast<jlong>(new VDGroup());
  return reinterpret_cast<jlong>(static_cast<VDGroup*>(s->clone().release()));
}
jlong VD_nCreateTree(JNIEnv*, jclass, jlong rootGroup) {
  auto* t = new VDTree();
  t->root.reset(reinterpret_cast<VDGroup*>(rootGroup));  // tree owns the root graph
  return reinterpret_cast<jlong>(t);
}
jlong VD_nCreateTreeFromCopy(JNIEnv*, jclass, jlong copyTree, jlong rootGroup) {
  auto* t = new VDTree();
  t->root.reset(reinterpret_cast<VDGroup*>(rootGroup));
  if (auto* c = reinterpret_cast<VDTree*>(copyTree)) {
    t->viewportW = c->viewportW; t->viewportH = c->viewportH;
    t->rootAlpha = c->rootAlpha; t->antiAlias = c->antiAlias; t->allowCaching = c->allowCaching;
  }
  return reinterpret_cast<jlong>(t);
}
void VD_nAddChild(JNIEnv*, jclass, jlong group, jlong child) {
  auto* g = reinterpret_cast<VDGroup*>(group);
  auto* ch = reinterpret_cast<VDNode*>(child);
  if (g != nullptr && ch != nullptr) g->children.emplace_back(ch);  // group owns the child
}

// Tree-level setters
void VD_nSetAllowCaching(JNIEnv*, jclass, jlong tree, jboolean allow) {
  if (auto* t = reinterpret_cast<VDTree*>(tree)) t->allowCaching = allow;
}
void VD_nSetAntiAlias(JNIEnv*, jclass, jlong tree, jboolean aa) {
  if (auto* t = reinterpret_cast<VDTree*>(tree)) t->antiAlias = aa;
}
jboolean VD_nSetRootAlpha(JNIEnv*, jclass, jlong tree, jfloat alpha) {
  auto* t = reinterpret_cast<VDTree*>(tree);
  if (t == nullptr) return JNI_FALSE;
  t->rootAlpha = alpha;
  return JNI_TRUE;
}
jfloat VD_nGetRootAlpha(JNIEnv*, jclass, jlong tree) {
  auto* t = reinterpret_cast<VDTree*>(tree);
  return t != nullptr ? t->rootAlpha : 1.0f;
}
void VD_nSetRendererViewportSize(JNIEnv*, jclass, jlong tree, jfloat w, jfloat h) {
  if (auto* t = reinterpret_cast<VDTree*>(tree)) { t->viewportW = w; t->viewportH = h; }
}
void VD_nSetName(JNIEnv* env, jclass, jlong nodePtr, jstring name) {
  auto* n = reinterpret_cast<VDNode*>(nodePtr);
  if (n == nullptr || name == nullptr) return;
  const char* s = env->GetStringUTFChars(name, nullptr);
  if (s != nullptr) { n->name = s; env->ReleaseStringUTFChars(name, s); }
}

// Path data
void VD_nSetPathString(JNIEnv* env, jclass, jlong pathPtr, jstring str, jint /*len*/) {
  auto* fp = reinterpret_cast<VDFullPath*>(pathPtr);
  if (fp == nullptr || str == nullptr) return;
  const char* s = env->GetStringUTFChars(str, nullptr);
  if (s != nullptr) { fp->path.reset(); SkParsePath::FromSVGString(s, &fp->path); env->ReleaseStringUTFChars(str, s); }
}
void VD_nSetPathData(JNIEnv*, jclass, jlong pathPtr, jlong pathDataPtr) {
  auto* fp = reinterpret_cast<VDFullPath*>(pathPtr);
  auto* pd = reinterpret_cast<AclPathData*>(pathDataPtr);
  if (fp != nullptr && pd != nullptr) fp->path = pd->path;
}

// FullPath property setters (used by animation; inflation uses the bulk nUpdate).
#define VDFP(ptr) auto* fp = reinterpret_cast<VDFullPath*>(ptr); if (fp == nullptr) return
void VD_nSetStrokeWidth(JNIEnv*, jclass, jlong p, jfloat v) { VDFP(p); fp->strokeWidth = v; }
void VD_nSetStrokeColor(JNIEnv*, jclass, jlong p, jint v) { VDFP(p); fp->strokeColor = v; }
void VD_nSetStrokeAlpha(JNIEnv*, jclass, jlong p, jfloat v) { VDFP(p); fp->strokeAlpha = v; }
void VD_nSetFillColor(JNIEnv*, jclass, jlong p, jint v) { VDFP(p); fp->fillColor = v; }
void VD_nSetFillAlpha(JNIEnv*, jclass, jlong p, jfloat v) { VDFP(p); fp->fillAlpha = v; }
void VD_nSetTrimPathStart(JNIEnv*, jclass, jlong p, jfloat v) { VDFP(p); fp->trimStart = v; }
void VD_nSetTrimPathEnd(JNIEnv*, jclass, jlong p, jfloat v) { VDFP(p); fp->trimEnd = v; }
void VD_nSetTrimPathOffset(JNIEnv*, jclass, jlong p, jfloat v) { VDFP(p); fp->trimOffset = v; }
void VD_nUpdateFullPathFillGradient(JNIEnv*, jclass, jlong p, jlong shader) {
  VDFP(p); fp->fillGradient = sk_ref_sp(reinterpret_cast<SkShader*>(shader));
}
void VD_nUpdateFullPathStrokeGradient(JNIEnv*, jclass, jlong p, jlong shader) {
  VDFP(p); fp->strokeGradient = sk_ref_sp(reinterpret_cast<SkShader*>(shader));
}
void VD_nUpdateFullPathProperties(JNIEnv*, jclass, jlong p, jfloat strokeWidth, jint strokeColor,
                                  jfloat strokeAlpha, jint fillColor, jfloat fillAlpha,
                                  jfloat trimStart, jfloat trimEnd, jfloat trimOffset,
                                  jfloat strokeMiter, jint strokeCap, jint strokeJoin,
                                  jint fillType) {
  VDFP(p);
  fp->strokeWidth = strokeWidth; fp->strokeColor = strokeColor; fp->strokeAlpha = strokeAlpha;
  fp->fillColor = fillColor; fp->fillAlpha = fillAlpha;
  fp->trimStart = trimStart; fp->trimEnd = trimEnd; fp->trimOffset = trimOffset;
  fp->strokeMiter = strokeMiter; fp->strokeLineCap = strokeCap; fp->strokeLineJoin = strokeJoin;
  fp->fillType = fillType;
}
// FullPath getters
float VD_nGetStrokeWidth(JNIEnv*, jclass, jlong p) { auto* fp = reinterpret_cast<VDFullPath*>(p); return fp ? fp->strokeWidth : 0; }
jint VD_nGetStrokeColor(JNIEnv*, jclass, jlong p) { auto* fp = reinterpret_cast<VDFullPath*>(p); return fp ? static_cast<jint>(fp->strokeColor) : 0; }
float VD_nGetStrokeAlpha(JNIEnv*, jclass, jlong p) { auto* fp = reinterpret_cast<VDFullPath*>(p); return fp ? fp->strokeAlpha : 1; }
jint VD_nGetFillColor(JNIEnv*, jclass, jlong p) { auto* fp = reinterpret_cast<VDFullPath*>(p); return fp ? static_cast<jint>(fp->fillColor) : 0; }
float VD_nGetFillAlpha(JNIEnv*, jclass, jlong p) { auto* fp = reinterpret_cast<VDFullPath*>(p); return fp ? fp->fillAlpha : 1; }
float VD_nGetTrimPathStart(JNIEnv*, jclass, jlong p) { auto* fp = reinterpret_cast<VDFullPath*>(p); return fp ? fp->trimStart : 0; }
float VD_nGetTrimPathEnd(JNIEnv*, jclass, jlong p) { auto* fp = reinterpret_cast<VDFullPath*>(p); return fp ? fp->trimEnd : 1; }
float VD_nGetTrimPathOffset(JNIEnv*, jclass, jlong p) { auto* fp = reinterpret_cast<VDFullPath*>(p); return fp ? fp->trimOffset : 0; }
jboolean VD_nGetFullPathProperties(JNIEnv* env, jclass, jlong p, jbyteArray outProps, jint length) {
  auto* fp = reinterpret_cast<VDFullPath*>(p);
  if (fp == nullptr) return JNI_FALSE;
  int32_t buf[12];
  auto putF = [&](int i, float v) { std::memcpy(&buf[i], &v, 4); };
  putF(0, fp->strokeWidth); buf[1] = static_cast<int32_t>(fp->strokeColor); putF(2, fp->strokeAlpha);
  buf[3] = static_cast<int32_t>(fp->fillColor); putF(4, fp->fillAlpha);
  putF(5, fp->trimStart); putF(6, fp->trimEnd); putF(7, fp->trimOffset);
  buf[8] = fp->strokeLineCap; buf[9] = fp->strokeLineJoin; putF(10, fp->strokeMiter);
  buf[11] = fp->fillType;
  const jsize avail = env->GetArrayLength(outProps);
  const jsize count = std::min<jsize>(std::min<jsize>(length, avail), sizeof(buf));
  env->SetByteArrayRegion(outProps, 0, count, reinterpret_cast<const jbyte*>(buf));
  return JNI_TRUE;
}

// Group property setters/getters
#define VDGP(ptr) auto* g = reinterpret_cast<VDGroup*>(ptr); if (g == nullptr) return
void VD_nSetRotation(JNIEnv*, jclass, jlong p, jfloat v) { VDGP(p); g->rotate = v; }
void VD_nSetPivotX(JNIEnv*, jclass, jlong p, jfloat v) { VDGP(p); g->pivotX = v; }
void VD_nSetPivotY(JNIEnv*, jclass, jlong p, jfloat v) { VDGP(p); g->pivotY = v; }
void VD_nSetScaleX(JNIEnv*, jclass, jlong p, jfloat v) { VDGP(p); g->scaleX = v; }
void VD_nSetScaleY(JNIEnv*, jclass, jlong p, jfloat v) { VDGP(p); g->scaleY = v; }
void VD_nSetTranslateX(JNIEnv*, jclass, jlong p, jfloat v) { VDGP(p); g->translateX = v; }
void VD_nSetTranslateY(JNIEnv*, jclass, jlong p, jfloat v) { VDGP(p); g->translateY = v; }
void VD_nUpdateGroupProperties(JNIEnv*, jclass, jlong p, jfloat rotate, jfloat pivotX, jfloat pivotY,
                               jfloat scaleX, jfloat scaleY, jfloat translateX, jfloat translateY) {
  VDGP(p);
  g->rotate = rotate; g->pivotX = pivotX; g->pivotY = pivotY;
  g->scaleX = scaleX; g->scaleY = scaleY; g->translateX = translateX; g->translateY = translateY;
}
float VD_nGetRotation(JNIEnv*, jclass, jlong p) { auto* g = reinterpret_cast<VDGroup*>(p); return g ? g->rotate : 0; }
float VD_nGetPivotX(JNIEnv*, jclass, jlong p) { auto* g = reinterpret_cast<VDGroup*>(p); return g ? g->pivotX : 0; }
float VD_nGetPivotY(JNIEnv*, jclass, jlong p) { auto* g = reinterpret_cast<VDGroup*>(p); return g ? g->pivotY : 0; }
float VD_nGetScaleX(JNIEnv*, jclass, jlong p) { auto* g = reinterpret_cast<VDGroup*>(p); return g ? g->scaleX : 1; }
float VD_nGetScaleY(JNIEnv*, jclass, jlong p) { auto* g = reinterpret_cast<VDGroup*>(p); return g ? g->scaleY : 1; }
float VD_nGetTranslateX(JNIEnv*, jclass, jlong p) { auto* g = reinterpret_cast<VDGroup*>(p); return g ? g->translateX : 0; }
float VD_nGetTranslateY(JNIEnv*, jclass, jlong p) { auto* g = reinterpret_cast<VDGroup*>(p); return g ? g->translateY : 0; }
jboolean VD_nGetGroupProperties(JNIEnv* env, jclass, jlong p, jfloatArray out, jint length) {
  auto* g = reinterpret_cast<VDGroup*>(p);
  if (g == nullptr) return JNI_FALSE;
  const float f[7] = {g->rotate, g->pivotX, g->pivotY, g->scaleX, g->scaleY, g->translateX, g->translateY};
  const jsize count = std::min<jsize>(std::min<jsize>(length, env->GetArrayLength(out)), 7);
  env->SetFloatArrayRegion(out, 0, count, f);
  return JNI_TRUE;
}

// The draw: render the tree directly onto the (GPU) canvas, scaled from the SVG
// viewport to the destination bounds, with root alpha + color filter in a layer.
jint VD_nDraw(JNIEnv* env, jclass, jlong treePtr, jlong canvasPtr, jlong colorFilterPtr,
              jobject boundsRect, jboolean needsMirroring, jboolean /*canReuseCache*/) {
  auto* tree = reinterpret_cast<VDTree*>(treePtr);
  auto* canvas = reinterpret_cast<SkCanvas*>(canvasPtr);
  if (tree == nullptr || canvas == nullptr || !tree->root || boundsRect == nullptr) return 0;

  static jfieldID f_left = nullptr, f_top = nullptr, f_right = nullptr, f_bottom = nullptr;
  if (f_left == nullptr) {
    jclass rc = env->GetObjectClass(boundsRect);
    f_left = env->GetFieldID(rc, "left", "I");
    f_top = env->GetFieldID(rc, "top", "I");
    f_right = env->GetFieldID(rc, "right", "I");
    f_bottom = env->GetFieldID(rc, "bottom", "I");
  }
  const float l = env->GetIntField(boundsRect, f_left);
  const float t = env->GetIntField(boundsRect, f_top);
  const float r = env->GetIntField(boundsRect, f_right);
  const float b = env->GetIntField(boundsRect, f_bottom);
  const float w = r - l, h = b - t;
  if (w <= 0 || h <= 0) return 0;
  const float vw = tree->viewportW > 0 ? tree->viewportW : w;
  const float vh = tree->viewportH > 0 ? tree->viewportH : h;

  SkAutoCanvasRestore acr(canvas, true);
  canvas->translate(l, t);
  if (needsMirroring) { canvas->translate(w, 0); canvas->scale(-1.0f, 1.0f); }

  auto* colorFilter = reinterpret_cast<SkColorFilter*>(colorFilterPtr);
  const bool useLayer = tree->rootAlpha < 1.0f || colorFilter != nullptr;
  if (useLayer) {
    SkPaint layerPaint;
    layerPaint.setAlphaf(tree->rootAlpha);
    if (colorFilter != nullptr) layerPaint.setColorFilter(sk_ref_sp(colorFilter));
    canvas->saveLayer(nullptr, &layerPaint);
  }
  canvas->scale(w / vw, h / vh);
  tree->root->draw(canvas, tree->antiAlias);
  if (useLayer) canvas->restore();
  return static_cast<jint>(w * h);
}

// ── Region (android.graphics.Region) ────────────────────────────────────────────
// Backed by SkRegion. Region.Op ordinals match SkRegion::Op (DIFFERENCE=0, INTERSECT,
// UNION, XOR, REVERSE_DIFFERENCE, REPLACE). Used by Material MaterialShapeDrawable.
SkRegion* region_of(JNIEnv* env, jobject obj) {
  static jfieldID fid = nullptr;
  if (obj == nullptr) return nullptr;
  if (fid == nullptr) fid = env->GetFieldID(env->GetObjectClass(obj), "mNativeRegion", "J");
  return reinterpret_cast<SkRegion*>(env->GetLongField(obj, fid));
}
void rect_to_java(JNIEnv* env, jobject rect, const SkIRect& r) {
  static jfieldID fl = nullptr, ft, fr, fb;
  if (fl == nullptr) {
    jclass c = env->GetObjectClass(rect);
    fl = env->GetFieldID(c, "left", "I"); ft = env->GetFieldID(c, "top", "I");
    fr = env->GetFieldID(c, "right", "I"); fb = env->GetFieldID(c, "bottom", "I");
  }
  env->SetIntField(rect, fl, r.fLeft); env->SetIntField(rect, ft, r.fTop);
  env->SetIntField(rect, fr, r.fRight); env->SetIntField(rect, fb, r.fBottom);
}
jlong Region_nativeConstructor(JNIEnv*, jclass) { return reinterpret_cast<jlong>(new SkRegion()); }
void Region_nativeDestructor(JNIEnv*, jclass, jlong r) { delete reinterpret_cast<SkRegion*>(r); }
void Region_nativeSetRegion(JNIEnv*, jclass, jlong dst, jlong src) {
  auto* d = reinterpret_cast<SkRegion*>(dst);
  auto* s = reinterpret_cast<SkRegion*>(src);
  if (d != nullptr && s != nullptr) *d = *s;
}
jboolean Region_nativeSetRect(JNIEnv*, jclass, jlong r, jint l, jint t, jint ri, jint b) {
  auto* d = reinterpret_cast<SkRegion*>(r);
  return (d != nullptr && d->setRect(SkIRect::MakeLTRB(l, t, ri, b))) ? JNI_TRUE : JNI_FALSE;
}
jboolean Region_nativeSetPath(JNIEnv*, jclass, jlong r, jlong path, jlong clip) {
  auto* d = reinterpret_cast<SkRegion*>(r);
  auto* p = reinterpret_cast<SkPath*>(path);
  auto* c = reinterpret_cast<SkRegion*>(clip);
  if (d == nullptr || p == nullptr || c == nullptr) return JNI_FALSE;
  return d->setPath(*p, *c) ? JNI_TRUE : JNI_FALSE;
}
jboolean Region_nativeGetBounds(JNIEnv* env, jclass, jlong r, jobject rect) {
  auto* d = reinterpret_cast<SkRegion*>(r);
  if (d == nullptr) return JNI_FALSE;
  rect_to_java(env, rect, d->getBounds());
  return d->isEmpty() ? JNI_FALSE : JNI_TRUE;
}
jboolean Region_nativeGetBoundaryPath(JNIEnv*, jclass, jlong r, jlong path) {
  auto* d = reinterpret_cast<SkRegion*>(r);
  auto* p = reinterpret_cast<SkPath*>(path);
  return (d != nullptr && p != nullptr && d->getBoundaryPath(p)) ? JNI_TRUE : JNI_FALSE;
}
jboolean Region_nativeOpIIIII(JNIEnv*, jclass, jlong r, jint l, jint t, jint ri, jint b, jint op) {
  auto* d = reinterpret_cast<SkRegion*>(r);
  return (d != nullptr && d->op(SkIRect::MakeLTRB(l, t, ri, b),
                                static_cast<SkRegion::Op>(op))) ? JNI_TRUE : JNI_FALSE;
}
jboolean Region_nativeOpRectRegion(JNIEnv* env, jclass, jlong r, jobject rect, jlong region, jint op) {
  auto* d = reinterpret_cast<SkRegion*>(r);
  auto* rgn = reinterpret_cast<SkRegion*>(region);
  if (d == nullptr || rgn == nullptr) return JNI_FALSE;
  static jfieldID fl = nullptr, ft, fr, fb;
  if (fl == nullptr) {
    jclass c = env->GetObjectClass(rect);
    fl = env->GetFieldID(c, "left", "I"); ft = env->GetFieldID(c, "top", "I");
    fr = env->GetFieldID(c, "right", "I"); fb = env->GetFieldID(c, "bottom", "I");
  }
  const SkIRect ir = SkIRect::MakeLTRB(env->GetIntField(rect, fl), env->GetIntField(rect, ft),
                                       env->GetIntField(rect, fr), env->GetIntField(rect, fb));
  return d->op(ir, *rgn, static_cast<SkRegion::Op>(op)) ? JNI_TRUE : JNI_FALSE;
}
jboolean Region_nativeOpRegionRegion(JNIEnv*, jclass, jlong r, jlong a, jlong b, jint op) {
  auto* d = reinterpret_cast<SkRegion*>(r);
  auto* ra = reinterpret_cast<SkRegion*>(a);
  auto* rb = reinterpret_cast<SkRegion*>(b);
  if (d == nullptr || ra == nullptr || rb == nullptr) return JNI_FALSE;
  return d->op(*ra, *rb, static_cast<SkRegion::Op>(op)) ? JNI_TRUE : JNI_FALSE;
}
jboolean Region_nativeEquals(JNIEnv*, jclass, jlong a, jlong b) {
  auto* ra = reinterpret_cast<SkRegion*>(a);
  auto* rb = reinterpret_cast<SkRegion*>(b);
  return (ra != nullptr && rb != nullptr && *ra == *rb) ? JNI_TRUE : JNI_FALSE;
}
jstring Region_nativeToString(JNIEnv* env, jclass, jlong r) {
  auto* d = reinterpret_cast<SkRegion*>(r);
  if (d == nullptr) return env->NewStringUTF("Region(null)");
  const SkIRect& b = d->getBounds();
  char buf[96];
  std::snprintf(buf, sizeof(buf), "Region(%d,%d,%d,%d)", b.fLeft, b.fTop, b.fRight, b.fBottom);
  return env->NewStringUTF(buf);
}
jlong Region_nativeCreateFromParcel(JNIEnv*, jclass, jobject) { return reinterpret_cast<jlong>(new SkRegion()); }
jboolean Region_nativeWriteToParcel(JNIEnv*, jclass, jlong, jobject) { return JNI_FALSE; }
// Instance methods (read mNativeRegion off the object).
jboolean Region_isEmpty(JNIEnv* env, jobject o) { auto* r = region_of(env, o); return (r && r->isEmpty()) ? JNI_TRUE : JNI_FALSE; }
jboolean Region_isRect(JNIEnv* env, jobject o) { auto* r = region_of(env, o); return (r && r->isRect()) ? JNI_TRUE : JNI_FALSE; }
jboolean Region_isComplex(JNIEnv* env, jobject o) { auto* r = region_of(env, o); return (r && r->isComplex()) ? JNI_TRUE : JNI_FALSE; }
jboolean Region_contains(JNIEnv* env, jobject o, jint x, jint y) { auto* r = region_of(env, o); return (r && r->contains(x, y)) ? JNI_TRUE : JNI_FALSE; }
jboolean Region_quickContains(JNIEnv* env, jobject o, jint l, jint t, jint ri, jint b) {
  auto* r = region_of(env, o); return (r && r->quickContains(SkIRect::MakeLTRB(l, t, ri, b))) ? JNI_TRUE : JNI_FALSE;
}
jboolean Region_quickRejectRect(JNIEnv* env, jobject o, jint l, jint t, jint ri, jint b) {
  auto* r = region_of(env, o); return (r && r->quickReject(SkIRect::MakeLTRB(l, t, ri, b))) ? JNI_TRUE : JNI_FALSE;
}
jboolean Region_quickRejectRegion(JNIEnv* env, jobject o, jobject other) {
  auto* r = region_of(env, o); auto* ro = region_of(env, other);
  return (r && ro && r->quickReject(*ro)) ? JNI_TRUE : JNI_FALSE;
}
void Region_translate(JNIEnv* env, jobject o, jint dx, jint dy, jobject dst) {
  auto* r = region_of(env, o);
  if (r == nullptr) return;
  r->translate(dx, dy, dst != nullptr ? region_of(env, dst) : r);
}
void Region_scale(JNIEnv* env, jobject o, jfloat scale, jobject dst) {
  auto* r = region_of(env, o);
  if (r == nullptr) return;
  SkRegion out;
  for (SkRegion::Iterator it(*r); !it.done(); it.next()) {
    const SkIRect& s = it.rect();
    out.op(SkIRect::MakeLTRB(static_cast<int>(s.fLeft * scale), static_cast<int>(s.fTop * scale),
                             static_cast<int>(s.fRight * scale), static_cast<int>(s.fBottom * scale)),
           SkRegion::kUnion_Op);
  }
  if (auto* d = (dst != nullptr ? region_of(env, dst) : r)) *d = out;
}

// ── Typeface / Font / FontFamily (android.graphics.Typeface + .fonts.*) ──────────
// Backed by SkTypeface. acl ships a single bundled font (default_typeface); Fonts and
// FontFamilies loaded from a path/buffer use SkTypeface::MakeFromFile/MakeFromData, else
// fall back to it. Typeface's native handle is an SkTypeface* (+1 ref, released by the
// nativeGetReleaseFunc registry). The default typefaces are seeded from Java (HostBootstrap
// .seedTypefaces) since the system font map isn't pushed to a standalone run.
struct AclFont { sk_sp<SkTypeface> tf; int weight = 400; bool italic = false; };
struct AclFontFamily { std::vector<sk_sp<SkTypeface>> fonts; };

void free_typeface(void* p) { SkSafeUnref(reinterpret_cast<SkTypeface*>(p)); }
void free_font(void* p) { delete reinterpret_cast<AclFont*>(p); }
void free_fontfamily(void* p) { delete reinterpret_cast<AclFontFamily*>(p); }

// android.graphics.fonts.Font$Builder
jlong FontBuilder_nInitBuilder(JNIEnv*, jclass) { return reinterpret_cast<jlong>(new AclFont()); }
void FontBuilder_nAddAxis(JNIEnv*, jclass, jlong, jint, jfloat) {}
jlong FontBuilder_nBuild(JNIEnv* env, jclass, jlong builder, jobject buffer, jstring path,
                        jstring /*localeList*/, jint weight, jboolean italic, jint ttc) {
  auto* f = reinterpret_cast<AclFont*>(builder);
  if (f == nullptr) f = new AclFont();
  sk_sp<SkTypeface> tf;
  if (path != nullptr) {
    const char* s = env->GetStringUTFChars(path, nullptr);
    if (s != nullptr) { tf = SkTypeface::MakeFromFile(s, ttc); env->ReleaseStringUTFChars(path, s); }
  }
  if (!tf && buffer != nullptr) {
    void* addr = env->GetDirectBufferAddress(buffer);
    const jlong cap = env->GetDirectBufferCapacity(buffer);
    if (addr != nullptr && cap > 0)
      tf = SkTypeface::MakeFromData(SkData::MakeWithCopy(addr, static_cast<size_t>(cap)), ttc);
  }
  if (!tf) tf = default_typeface();
  f->tf = tf; f->weight = weight; f->italic = italic;
  return reinterpret_cast<jlong>(f);
}
jlong FontBuilder_nGetReleaseNativeFont(JNIEnv*, jclass) { return reinterpret_cast<jlong>(&free_font); }
jlong FontBuilder_nClone(JNIEnv*, jclass, jlong fontPtr, jlong, jint weight, jboolean italic, jint) {
  auto* src = reinterpret_cast<AclFont*>(fontPtr);
  auto* f = new AclFont();
  f->tf = src != nullptr ? src->tf : default_typeface();
  f->weight = weight; f->italic = italic;
  return reinterpret_cast<jlong>(f);
}
// android.graphics.fonts.FontFamily$Builder
jlong FontFamilyBuilder_nInitBuilder(JNIEnv*, jclass) { return reinterpret_cast<jlong>(new AclFontFamily()); }
void FontFamilyBuilder_nAddFont(JNIEnv*, jclass, jlong builder, jlong fontPtr) {
  auto* fam = reinterpret_cast<AclFontFamily*>(builder);
  auto* f = reinterpret_cast<AclFont*>(fontPtr);
  if (fam != nullptr && f != nullptr) fam->fonts.push_back(f->tf);
}
jlong FontFamilyBuilder_nBuild(JNIEnv*, jclass, jlong builder, jstring, jint, jboolean) { return builder; }
jlong FontFamilyBuilder_nGetReleaseNativeFamily(JNIEnv*, jclass) { return reinterpret_cast<jlong>(&free_fontfamily); }
// android.graphics.fonts.FontFamily
jint FontFamily_nGetFontSize(JNIEnv*, jclass, jlong fam) {
  auto* f = reinterpret_cast<AclFontFamily*>(fam); return f != nullptr ? static_cast<jint>(f->fonts.size()) : 0;
}
jlong FontFamily_nGetFont(JNIEnv*, jclass, jlong fam, jint i) {
  auto* f = reinterpret_cast<AclFontFamily*>(fam);
  if (f == nullptr || i < 0 || i >= static_cast<jint>(f->fonts.size())) return 0;
  auto* out = new AclFont(); out->tf = f->fonts[i];
  return reinterpret_cast<jlong>(out);
}
jstring FontFamily_nGetLangTags(JNIEnv* env, jclass, jlong) { return env->NewStringUTF(""); }
jint FontFamily_nGetVariant(JNIEnv*, jclass, jlong) { return 0; }
// android.graphics.fonts.Font (the immutable Font; native instance is an AclFont*).
jlong Font_nGetReleaseNativeFont(JNIEnv*, jclass) { return reinterpret_cast<jlong>(&free_font); }
jlong Font_nCloneFont(JNIEnv*, jclass, jlong p) {
  auto* s = reinterpret_cast<AclFont*>(p); auto* f = new AclFont();
  if (s != nullptr) { f->tf = s->tf; f->weight = s->weight; f->italic = s->italic; }
  return reinterpret_cast<jlong>(f);
}
jint Font_nGetPackedStyle(JNIEnv*, jclass, jlong p) {
  auto* f = reinterpret_cast<AclFont*>(p);
  if (f == nullptr) return 400;
  return (f->weight & 0xFFFF) | (f->italic ? (1 << 16) : 0);
}
jint Font_nGetIndex(JNIEnv*, jclass, jlong) { return 0; }
jint Font_nGetAxisCount(JNIEnv*, jclass, jlong) { return 0; }
jlong Font_nGetAxisInfo(JNIEnv*, jclass, jlong, jint) { return 0; }
jstring Font_nGetFontPath(JNIEnv* env, jclass, jlong) { return env->NewStringUTF(""); }
jstring Font_nGetLocaleList(JNIEnv* env, jclass, jlong) { return env->NewStringUTF(""); }
jint Font_nGetSourceId(JNIEnv*, jclass, jlong) { return 0; }
jlong Font_nGetMinikinFontPtr(JNIEnv*, jclass, jlong p) { return p; }
jlongArray Font_nGetAvailableFontSet(JNIEnv*, jclass) { return nullptr; }
jobject Font_nNewByteBuffer(JNIEnv*, jclass, jlong) { return nullptr; }
jlong Font_nGetBufferAddress(JNIEnv*, jclass, jlong) { return 0; }
jfloat Font_nGetGlyphBounds(JNIEnv*, jclass, jlong, jint, jlong, jobject) { return 0; }
jfloat Font_nGetFontMetrics(JNIEnv*, jclass, jlong, jlong, jobject) { return 0; }

// android.graphics.Typeface — handle is SkTypeface* (+1 ref).
jint sk_typeface_style(const SkFontStyle& s) {
  const bool bold = s.weight() >= SkFontStyle::kBold_Weight;
  const bool italic = s.slant() != SkFontStyle::kUpright_Slant;
  return (bold ? 1 : 0) | (italic ? 2 : 0);  // NORMAL=0, BOLD=1, ITALIC=2, BOLD_ITALIC=3
}
jlong typeface_ref(SkTypeface* t) {
  if (t != nullptr) { SkSafeRef(t); return reinterpret_cast<jlong>(t); }
  return reinterpret_cast<jlong>(default_typeface().release());
}
jlong Typeface_nativeGetReleaseFunc(JNIEnv*, jclass) { return reinterpret_cast<jlong>(&free_typeface); }
jlong Typeface_nativeCreateFromArray(JNIEnv* env, jclass, jlongArray families, jlong, jint, jint) {
  sk_sp<SkTypeface> tf;
  if (families != nullptr && env->GetArrayLength(families) > 0) {
    jlong first = 0; env->GetLongArrayRegion(families, 0, 1, &first);
    if (auto* fam = reinterpret_cast<AclFontFamily*>(first); fam != nullptr && !fam->fonts.empty())
      tf = fam->fonts[0];
  }
  if (!tf) tf = default_typeface();
  return reinterpret_cast<jlong>(tf.release());
}
jlong Typeface_nativeCreateFromTypeface(JNIEnv*, jclass, jlong ni, jint) { return typeface_ref(reinterpret_cast<SkTypeface*>(ni)); }
jlong Typeface_nativeCreateFromTypefaceWithExactStyle(JNIEnv*, jclass, jlong ni, jint, jboolean) { return typeface_ref(reinterpret_cast<SkTypeface*>(ni)); }
jlong Typeface_nativeCreateFromTypefaceWithVariation(JNIEnv*, jclass, jlong ni, jobject) { return typeface_ref(reinterpret_cast<SkTypeface*>(ni)); }
jlong Typeface_nativeCreateWeightAlias(JNIEnv*, jclass, jlong ni, jint) { return typeface_ref(reinterpret_cast<SkTypeface*>(ni)); }
jintArray Typeface_nativeGetSupportedAxes(JNIEnv*, jclass, jlong) { return nullptr; }
void Typeface_nativeSetDefault(JNIEnv*, jclass, jlong) {}
jint Typeface_nativeGetStyle(JNIEnv*, jclass, jlong ni) {
  auto* t = reinterpret_cast<SkTypeface*>(ni); return t != nullptr ? sk_typeface_style(t->fontStyle()) : 0;
}
jint Typeface_nativeGetWeight(JNIEnv*, jclass, jlong ni) {
  auto* t = reinterpret_cast<SkTypeface*>(ni); return t != nullptr ? t->fontStyle().weight() : 400;
}
jint Typeface_nativeGetFamilySize(JNIEnv*, jclass, jlong) { return 0; }
jlong Typeface_nativeGetFamily(JNIEnv*, jclass, jlong, jint) { return 0; }
void Typeface_nativeRegisterGenericFamily(JNIEnv*, jclass, jstring, jlong) {}
jint Typeface_nativeWriteTypefaces(JNIEnv*, jclass, jobject, jlongArray) { return 0; }
jlongArray Typeface_nativeReadTypefaces(JNIEnv*, jclass, jobject) { return nullptr; }
void Typeface_nativeForceSetStaticFinalField(JNIEnv* env, jclass clazz, jstring name, jobject value) {
  const char* s = env->GetStringUTFChars(name, nullptr);
  if (s != nullptr) {
    jfieldID f = env->GetStaticFieldID(clazz, s, "Landroid/graphics/Typeface;");
    if (f != nullptr) env->SetStaticObjectField(clazz, f, value); else env->ExceptionClear();
    env->ReleaseStringUTFChars(name, s);
  }
}
void Typeface_nativeAddFontCollections(JNIEnv*, jclass, jlong) {}
void Typeface_nativeWarmUpCache(JNIEnv*, jclass, jstring) {}

// android.graphics.HardwareRenderer — acl dropped hwui (ADR-4); RT-animation and other
// process-global config natives are accepted no-ops (Compose's recomposer toggles them).
void HardwareRenderer_nSetRtAnimationsEnabled(JNIEnv*, jclass, jboolean) {}

// ── android.view.DisplayEventReceiver (synthetic vsync source) ───────────────────
// The Choreographer's vsync source (normally SurfaceFlinger). acl provides a synthetic
// one: scheduleVsync dispatches a frame with a MONOTONIC timestamp (posted async by the
// Choreographer's FrameHandler, so it runs when the looper is pumped — the present loop).
// Enough for Compose's frame clock / recomposer to initialize. Real vsync via
// wp_presentation is a later refinement.
struct AclVsyncReceiver {
  jobject weakRef;      // global ref to the WeakReference<DisplayEventReceiver>
  bool pending = false;  // a scheduleVsync request awaiting delivery (approach A)
};
// Registry of live receivers so acl_deliver_vsync (the Looper-driven present, approach A)
// can fan a compositor-clocked vsync out to whichever DisplayEventReceiver(s) asked for one.
// Access is on the app main thread in practice; the mutex guards rare cross-thread create.
static std::vector<AclVsyncReceiver*> g_vsync_receivers;
static std::mutex g_vsync_mutex;
// Legacy default: scheduleVsync dispatches a MONOTONIC-stamped vsync inline (self-clocked —
// what the poll-loop present path relies on). The PresentDriver flips this off so vsync is
// instead *delivered* by acl_deliver_vsync at the compositor's wp_presentation cadence.
static bool g_vsync_immediate = true;

jobject make_vsync_event_data(JNIEnv* env) {
  jclass c = env->FindClass("android/view/DisplayEventReceiver$VsyncEventData");
  if (c == nullptr) { env->ExceptionClear(); return nullptr; }
  jmethodID ctor = env->GetMethodID(c, "<init>", "()V");
  jobject o = ctor != nullptr ? env->NewObject(c, ctor) : nullptr;
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(c);
  return o;
}
// Deliver one vsync (frameTimeNanos) into a receiver's DisplayEventReceiver.dispatchVsync,
// which posts Choreographer.doFrame on the main Handler.
static void dispatch_vsync_to(JNIEnv* env, AclVsyncReceiver* r, jlong frameTimeNanos) {
  if (r == nullptr || r->weakRef == nullptr) return;
  static jmethodID wrGet = nullptr;
  if (wrGet == nullptr) {
    jclass wrc = env->FindClass("java/lang/ref/Reference");
    wrGet = env->GetMethodID(wrc, "get", "()Ljava/lang/Object;");
    env->DeleteLocalRef(wrc);
  }
  jobject receiver = env->CallObjectMethod(r->weakRef, wrGet);
  if (receiver == nullptr) return;
  jclass derc = env->GetObjectClass(receiver);
  jmethodID dispatch = env->GetMethodID(
      derc, "dispatchVsync", "(JJILandroid/view/DisplayEventReceiver$VsyncEventData;)V");
  if (dispatch != nullptr) {
    jobject vsync = make_vsync_event_data(env);
    env->CallVoidMethod(receiver, dispatch, frameTimeNanos, static_cast<jlong>(0),
                        static_cast<jint>(1), vsync);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (vsync != nullptr) env->DeleteLocalRef(vsync);
  } else {
    env->ExceptionClear();
  }
  env->DeleteLocalRef(derc);
  env->DeleteLocalRef(receiver);
}
jlong DisplayEventReceiver_nativeInit(JNIEnv* env, jclass, jobject weakReceiver, jobject /*mq*/,
                                      jint /*source*/, jint /*config*/) {
  auto* r = new AclVsyncReceiver();
  r->weakRef = env->NewGlobalRef(weakReceiver);
  { std::lock_guard<std::mutex> lk(g_vsync_mutex); g_vsync_receivers.push_back(r); }
  return reinterpret_cast<jlong>(r);
}
void DisplayEventReceiver_nativeDispose(JNIEnv* env, jclass, jlong ptr) {
  auto* r = reinterpret_cast<AclVsyncReceiver*>(ptr);
  if (r == nullptr) return;
  {
    std::lock_guard<std::mutex> lk(g_vsync_mutex);
    g_vsync_receivers.erase(
        std::remove(g_vsync_receivers.begin(), g_vsync_receivers.end(), r),
        g_vsync_receivers.end());
  }
  if (r->weakRef != nullptr) env->DeleteGlobalRef(r->weakRef);
  delete r;
}
void DisplayEventReceiver_nativeScheduleVsync(JNIEnv* env, jclass, jlong ptr) {
  auto* r = reinterpret_cast<AclVsyncReceiver*>(ptr);
  if (r == nullptr) return;
  r->pending = true;
  if (g_vsync_immediate) {  // legacy self-clocked path: dispatch a MONOTONIC vsync now
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    r->pending = false;
    dispatch_vsync_to(env, r, static_cast<jlong>(ts.tv_sec) * 1000000000LL + ts.tv_nsec);
  }
}
jobject DisplayEventReceiver_nativeGetLatestVsyncEventData(JNIEnv* env, jclass, jlong) {
  return make_vsync_event_data(env);
}

// ── approach A: compositor-clocked vsync delivery (see docs/looper-driven-present.md) ──
// Deliver a vsync at time `frameTimeNanos` (the presentation clock) to every receiver with a
// pending scheduleVsync request → Choreographer.doFrame. Called from the PresentDriver's
// native tick on the app main thread (which owns `env`). Snapshots pending receivers under
// the lock, then dispatches outside it (dispatchVsync re-enters Java).
extern "C" void acl_deliver_vsync(JNIEnv* env, jlong frameTimeNanos) {
  std::vector<AclVsyncReceiver*> pend;
  {
    std::lock_guard<std::mutex> lk(g_vsync_mutex);
    for (auto* r : g_vsync_receivers)
      if (r->pending) { r->pending = false; pend.push_back(r); }
  }
  for (auto* r : pend) dispatch_vsync_to(env, r, frameTimeNanos);
}
// Any receiver currently has a pending vsync request (the driver only clocks when someone —
// Compose's frame clock or the traversal — actually asked for a frame).
extern "C" int acl_vsync_pending() {
  std::lock_guard<std::mutex> lk(g_vsync_mutex);
  for (auto* r : g_vsync_receivers)
    if (r->pending) return 1;
  return 0;
}
// Switch vsync from legacy self-clocked (immediate) to delivered mode. The PresentDriver
// calls acl_vsync_set_immediate(0) at startup so frames are paced by the compositor instead.
extern "C" void acl_vsync_set_immediate(int on) { g_vsync_immediate = (on != 0); }

// ── android.graphics.Interpolator (legacy keyframe interpolator) ─────────────────
// Skia dropped SkInterpolator; acl keeps the first keyframe's values and returns them
// statically (animations don't advance in a single-frame render). Enough to inflate
// views that reference an interpolator.
struct AclInterpolator { int valueCount = 1; std::vector<float> firstValues; };
jlong Interpolator_nativeConstructor(JNIEnv*, jclass, jint valueCount, jint /*frameCount*/) {
  auto* it = new AclInterpolator();
  it->valueCount = valueCount > 0 ? valueCount : 1;
  it->firstValues.assign(it->valueCount, 0.0f);
  return reinterpret_cast<jlong>(it);
}
void Interpolator_nativeDestructor(JNIEnv*, jclass, jlong p) { delete reinterpret_cast<AclInterpolator*>(p); }
void Interpolator_nativeReset(JNIEnv*, jclass, jlong p, jint valueCount, jint /*frameCount*/) {
  if (auto* it = reinterpret_cast<AclInterpolator*>(p)) {
    it->valueCount = valueCount > 0 ? valueCount : 1;
    it->firstValues.assign(it->valueCount, 0.0f);
  }
}
void Interpolator_nativeSetKeyFrame(JNIEnv* env, jclass, jlong p, jint index, jint /*msec*/,
                                    jfloatArray values, jfloatArray /*blend*/) {
  auto* it = reinterpret_cast<AclInterpolator*>(p);
  if (it != nullptr && index == 0 && values != nullptr) {
    const jsize n = env->GetArrayLength(values);
    it->firstValues.resize(n);
    env->GetFloatArrayRegion(values, 0, n, it->firstValues.data());
  }
}
void Interpolator_nativeSetRepeatMirror(JNIEnv*, jclass, jlong, jfloat, jboolean) {}
jint Interpolator_nativeTimeToValues(JNIEnv* env, jclass, jlong p, jint /*msec*/, jfloatArray values) {
  auto* it = reinterpret_cast<AclInterpolator*>(p);
  if (it != nullptr && values != nullptr && !it->firstValues.empty()) {
    const jsize n = env->GetArrayLength(values);
    const jsize cnt = n < static_cast<jsize>(it->firstValues.size())
                          ? n : static_cast<jsize>(it->firstValues.size());
    env->SetFloatArrayRegion(values, 0, cnt, it->firstValues.data());
  }
  return 0;  // NORMAL
}

// com.android.internal.util.VirtualRefBasePtr — the tiny ref-counting wrapper Java uses
// for the VectorDrawable tree (normally in libhwui). Drives our VirtualLightRefBase.
void VirtualRefBasePtr_nIncStrong(JNIEnv*, jclass, jlong ptr) {
  if (auto* r = reinterpret_cast<VirtualLightRefBase*>(ptr)) r->incStrong(nullptr);
}
void VirtualRefBasePtr_nDecStrong(JNIEnv*, jclass, jlong ptr) {
  if (auto* r = reinterpret_cast<VirtualLightRefBase*>(ptr)) r->decStrong(nullptr);
}

// ── android.graphics.ImageDecoder (Skia SkAndroidCodec) ──────────────────────────
// The modern decode API (API 28+): a resource/asset/byte-source ImageView src goes through
// ImageDecoder → nCreate (build a codec) → nDecodeBitmap (decode to a Bitmap). Backed by
// Skia's SkAndroidCodec (libpng/libjpeg/libwebp linked). Enough for static images (drawables,
// layout inflation); animation is a follow-on.
// The PNG chunk reader that captures a .9.png's nine-patch data (the aapt-written "npTc"
// chunk). Mirrors AOSP's NinePatchPeeker (Apache-2.0) minus the insets/scale helpers we don't
// need: readChunk deserializes Res_png_9patch into a malloc'd copy the Bitmap chunk is built
// from. Ref-counted (SkPngChunkReader is SkRefCnt) so SkCodec can hold it during decode.
class AclNinePatchPeeker : public SkPngChunkReader {
 public:
  ~AclNinePatchPeeker() override { free(patch); }
  bool readChunk(const char tag[], const void* data, size_t length) override {
    if (std::strcmp("npTc", tag) == 0 && length >= sizeof(android::Res_png_9patch)) {
      auto* p = static_cast<android::Res_png_9patch*>(const_cast<void*>(data));
      const size_t sz = p->serializedSize();
      if (length != sz) return true;  // malformed; ignore, keep decoding
      auto* copy = static_cast<android::Res_png_9patch*>(std::malloc(sz));
      if (copy == nullptr) return true;
      std::memcpy(copy, p, sz);
      android::Res_png_9patch::deserialize(copy);
      copy->fileToDevice();
      std::free(patch);
      patch = copy;
      patch_size = sz;
    }
    return true;  // keep decoding
  }
  android::Res_png_9patch* patch = nullptr;
  size_t patch_size = 0;
};

struct AclImageDecoder {
  std::unique_ptr<SkAndroidCodec> codec;
  int width = 0, height = 0;
  sk_sp<AclNinePatchPeeker> peeker;  // holds the npTc chunk when the source is a nine-patch
};
jclass g_imagedecoder_class = nullptr;
jmethodID g_imagedecoder_ctor = nullptr;  // private ImageDecoder(long,int,int,boolean,boolean)

// Build the Java ImageDecoder wrapping the encoded `data`; null if it can't be decoded. A
// nine-patch peeker rides the decode so .9.png sources surface their stretch/padding data.
jobject make_image_decoder(JNIEnv* env, sk_sp<SkData> data) {
  if (data == nullptr || data->isEmpty()) return nullptr;
  sk_sp<AclNinePatchPeeker> peeker(new AclNinePatchPeeker());
  SkCodec::Result result = SkCodec::kInternalError;
  std::unique_ptr<SkCodec> c =
      SkCodec::MakeFromStream(SkMemoryStream::Make(data), &result, peeker.get());
  if (c == nullptr) return nullptr;
  std::unique_ptr<SkAndroidCodec> acodec = SkAndroidCodec::MakeFromCodec(std::move(c));
  if (acodec == nullptr) return nullptr;
  auto* d = new AclImageDecoder();
  const SkImageInfo info = acodec->getInfo();
  d->width = info.width();
  d->height = info.height();
  d->codec = std::move(acodec);
  d->peeker = peeker;
  const bool is_nine_patch = peeker->patch != nullptr;
  if (g_imagedecoder_class == nullptr) {
    jclass lc = env->FindClass("android/graphics/ImageDecoder");
    if (lc != nullptr) {
      g_imagedecoder_class = static_cast<jclass>(env->NewGlobalRef(lc));
      g_imagedecoder_ctor = env->GetMethodID(g_imagedecoder_class, "<init>", "(JIIZZ)V");
      env->DeleteLocalRef(lc);
    }
  }
  if (g_imagedecoder_class == nullptr || g_imagedecoder_ctor == nullptr) { delete d; return nullptr; }
  return env->NewObject(g_imagedecoder_class, g_imagedecoder_ctor, reinterpret_cast<jlong>(d),
                        d->width, d->height, JNI_FALSE /*animated*/,
                        is_nine_patch ? JNI_TRUE : JNI_FALSE);
}

// nCreate(long asset, boolean, Source): decode from an android::Asset (a resource/asset stream).
jobject ImageDecoder_nCreateAsset(JNIEnv* env, jclass, jlong asset, jboolean, jobject) {
  auto* a = reinterpret_cast<android::Asset*>(asset);
  if (a == nullptr) return nullptr;
  const void* buf = a->getBuffer(false);
  const off64_t len = a->getLength();
  if (buf == nullptr || len <= 0) return nullptr;
  return make_image_decoder(env, SkData::MakeWithCopy(buf, static_cast<size_t>(len)));
}
// nCreate(byte[] data, int offset, int length, boolean, Source).
jobject ImageDecoder_nCreateBytes(JNIEnv* env, jclass, jbyteArray data, jint offset, jint length,
                                  jboolean, jobject) {
  if (data == nullptr || length <= 0) return nullptr;
  jbyte* p = env->GetByteArrayElements(data, nullptr);
  if (p == nullptr) return nullptr;
  sk_sp<SkData> d = SkData::MakeWithCopy(p + offset, static_cast<size_t>(length));
  env->ReleaseByteArrayElements(data, p, JNI_ABORT);
  return make_image_decoder(env, d);
}

// nCreate(InputStream is, byte[] storage, boolean, Source): drain the stream (storage is the
// read buffer) and decode the accumulated bytes.
jobject ImageDecoder_nCreateStream(JNIEnv* env, jclass, jobject is, jbyteArray storage, jboolean,
                                   jobject) {
  if (is == nullptr || storage == nullptr) return nullptr;
  jclass isc = env->GetObjectClass(is);
  jmethodID readM = env->GetMethodID(isc, "read", "([BII)I");
  env->DeleteLocalRef(isc);
  if (readM == nullptr) { env->ExceptionClear(); return nullptr; }
  const jsize cap = env->GetArrayLength(storage);
  std::vector<std::uint8_t> buf;
  for (;;) {
    const jint n = env->CallIntMethod(is, readM, storage, 0, cap);
    if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionClear(); break; }
    if (n <= 0) break;
    jbyte* p = env->GetByteArrayElements(storage, nullptr);
    if (p == nullptr) break;
    buf.insert(buf.end(), reinterpret_cast<std::uint8_t*>(p),
               reinterpret_cast<std::uint8_t*>(p) + n);
    env->ReleaseByteArrayElements(storage, p, JNI_ABORT);
  }
  if (buf.empty()) return nullptr;
  return make_image_decoder(env, SkData::MakeWithCopy(buf.data(), buf.size()));
}
// nCreate(ByteBuffer buffer, int position, int limit, boolean, Source): decode a direct buffer.
jobject ImageDecoder_nCreateBuffer(JNIEnv* env, jclass, jobject buffer, jint position, jint limit,
                                   jboolean, jobject) {
  void* addr = env->GetDirectBufferAddress(buffer);
  if (addr == nullptr || limit <= position) return nullptr;
  return make_image_decoder(
      env, SkData::MakeWithCopy(static_cast<std::uint8_t*>(addr) + position,
                                static_cast<size_t>(limit - position)));
}

// nDecodeBitmap(nativePtr, decoder, doPostProcess, width, height, cropRect, mutable, allocator,
//   unpremulRequired, conserveMemory, decodeAsAlphaMask, desiredColorSpace, extended) -> Bitmap.
jobject ImageDecoder_nDecodeBitmap(JNIEnv* env, jclass, jlong nativePtr, jobject /*decoder*/,
                                   jboolean /*doPostProcess*/, jint width, jint height,
                                   jobject /*cropRect*/, jboolean isMutable, jint /*allocator*/,
                                   jboolean /*unpremul*/, jboolean /*conserve*/,
                                   jboolean /*alphaMask*/, jlong /*colorSpace*/, jboolean /*extended*/) {
  auto* d = reinterpret_cast<AclImageDecoder*>(nativePtr);
  if (d == nullptr || !d->codec) return nullptr;
  const SkImageInfo nInfo =
      d->codec->getInfo().makeColorType(kN32_SkColorType).makeAlphaType(kPremul_SkAlphaType);
  SkBitmap native;
  if (!native.tryAllocPixels(nInfo)) return nullptr;
  const SkCodec::Result r =
      d->codec->getAndroidPixels(nInfo, native.getPixels(), native.rowBytes());
  if (r != SkCodec::kSuccess && r != SkCodec::kIncompleteInput) return nullptr;
  auto* w = new AclBitmap();
  if (width <= 0 || height <= 0 || (width == nInfo.width() && height == nInfo.height())) {
    w->bitmap = native;
  } else {  // ImageDecoder requested a resize
    const SkImageInfo tInfo = nInfo.makeWH(width, height);
    if (!w->bitmap.tryAllocPixels(tInfo)) { delete w; return nullptr; }
    native.pixmap().scalePixels(w->bitmap.pixmap(), SkSamplingOptions(SkFilterMode::kLinear));
  }
  w->immutable = (isMutable == JNI_FALSE);
  // A nine-patch source: hand the deserialized npTc chunk to the Bitmap so getNinePatchChunk()
  // is non-null and the framework builds a NinePatchDrawable (backgrounds for EditText/Button/…).
  jbyteArray np_chunk = nullptr;
  if (d->peeker && d->peeker->patch != nullptr && d->peeker->patch_size > 0) {
    np_chunk = env->NewByteArray(static_cast<jsize>(d->peeker->patch_size));
    if (np_chunk != nullptr)
      env->SetByteArrayRegion(np_chunk, 0, static_cast<jsize>(d->peeker->patch_size),
                              reinterpret_cast<const jbyte*>(d->peeker->patch));
  }
  return env->NewObject(g_bitmap_class, g_bitmap_ctor, reinterpret_cast<jlong>(w),
                        w->bitmap.width(), w->bitmap.height(), /*density=*/0,
                        /*requestPremultiplied=*/JNI_TRUE, np_chunk,
                        /*ninePatchInsets=*/static_cast<jobject>(nullptr));
}

// nGetPadding: the nine-patch content padding (from the npTc chunk), or (-1,-1,-1,-1) when the
// source is not a nine-patch.
void ImageDecoder_nGetPadding(JNIEnv* env, jclass, jlong nativePtr, jobject outRect) {
  if (outRect == nullptr) return;
  auto* d = reinterpret_cast<AclImageDecoder*>(nativePtr);
  jint l = -1, t = -1, r = -1, b = -1;
  if (d != nullptr && d->peeker && d->peeker->patch != nullptr) {
    const android::Res_png_9patch* p = d->peeker->patch;
    l = static_cast<jint>(p->paddingLeft);
    t = static_cast<jint>(p->paddingTop);
    r = static_cast<jint>(p->paddingRight);
    b = static_cast<jint>(p->paddingBottom);
  }
  jclass rc = env->GetObjectClass(outRect);
  jmethodID set = env->GetMethodID(rc, "set", "(IIII)V");
  if (set != nullptr) env->CallVoidMethod(outRect, set, l, t, r, b);
  env->DeleteLocalRef(rc);
}
void ImageDecoder_nClose(JNIEnv*, jclass, jlong nativePtr) {
  delete reinterpret_cast<AclImageDecoder*>(nativePtr);
}
jobject ImageDecoder_nGetSampledSize(JNIEnv* env, jclass, jlong nativePtr, jint sampleSize) {
  auto* d = reinterpret_cast<AclImageDecoder*>(nativePtr);
  int w = d != nullptr ? d->width : 0, h = d != nullptr ? d->height : 0;
  if (sampleSize > 1) { w = (w + sampleSize - 1) / sampleSize; h = (h + sampleSize - 1) / sampleSize; }
  jclass sz = env->FindClass("android/util/Size");
  if (sz == nullptr) return nullptr;
  jmethodID ctor = env->GetMethodID(sz, "<init>", "(II)V");
  jobject o = ctor != nullptr ? env->NewObject(sz, ctor, w, h) : nullptr;
  env->DeleteLocalRef(sz);
  return o;
}

// ── android.graphics.NinePatch (over the npTc chunk) ─────────────────────────────
// The chunk is the (deserialized) Res_png_9patch a nine-patch Bitmap carries. validate
// copies it to the native heap and hands back the pointer NinePatch/NinePatchDrawable holds.
jboolean NinePatch_isNinePatchChunk(JNIEnv* env, jclass, jbyteArray obj) {
  if (obj == nullptr || env->GetArrayLength(obj) < static_cast<jsize>(sizeof(android::Res_png_9patch)))
    return JNI_FALSE;
  jbyte* arr = env->GetByteArrayElements(obj, nullptr);
  if (arr == nullptr) return JNI_FALSE;
  const auto* chunk = reinterpret_cast<const android::Res_png_9patch*>(arr);
  const int8_t was = chunk->wasDeserialized;
  env->ReleaseByteArrayElements(obj, arr, JNI_ABORT);
  return was != -1 ? JNI_TRUE : JNI_FALSE;  // deserialized ⇒ a valid chunk
}
jlong NinePatch_validateNinePatchChunk(JNIEnv* env, jclass, jbyteArray obj) {
  const jsize sz = env->GetArrayLength(obj);
  if (sz < static_cast<jsize>(sizeof(android::Res_png_9patch))) return 0;
  auto* storage = new int8_t[sz];
  env->GetByteArrayRegion(obj, 0, sz, reinterpret_cast<jbyte*>(storage));
  return reinterpret_cast<jlong>(storage);  // Res_png_9patch* (getXDivs/getColors point within)
}
void NinePatch_nativeFinalize(JNIEnv*, jclass, jlong chunk) {
  delete[] reinterpret_cast<int8_t*>(chunk);
}

// Canvas.nDrawNinePatch: draw the nine-patch bitmap into `dst` with Skia's lattice (the chunk's
// stretch divs). Fixed-color/transparent regions aren't mapped (rare for widget backgrounds).
void Canvas_nDrawNinePatch(JNIEnv*, jclass, jlong canvas, jlong bitmapHandle, jlong chunkHandle,
                           jfloat left, jfloat top, jfloat right, jfloat bottom, jlong paintHandle,
                           jint /*screenDensity*/, jint /*bitmapDensity*/) {
  auto* c = reinterpret_cast<SkCanvas*>(canvas);
  auto* b = reinterpret_cast<AclBitmap*>(bitmapHandle);
  const auto* chunk = reinterpret_cast<const android::Res_png_9patch*>(chunkHandle);
  if (c == nullptr || b == nullptr || chunk == nullptr) return;
  const int w = b->bitmap.width(), h = b->bitmap.height();
  // NinePatchUtils::SetLatticeDivs (AOSP, Apache-2.0), inlined: the last div == size carries no
  // extra info and Skia rejects it.
  SkCanvas::Lattice lattice{};
  lattice.fXCount = chunk->numXDivs;
  lattice.fYCount = chunk->numYDivs;
  lattice.fXDivs = chunk->getXDivs();
  lattice.fYDivs = chunk->getYDivs();
  if (lattice.fXCount > 0 && w == lattice.fXDivs[lattice.fXCount - 1]) lattice.fXCount--;
  if (lattice.fYCount > 0 && h == lattice.fYDivs[lattice.fYCount - 1]) lattice.fYCount--;
  lattice.fRectTypes = nullptr;
  lattice.fColors = nullptr;
  lattice.fBounds = nullptr;
  auto* ap = reinterpret_cast<AclPaint*>(paintHandle);
  if (sk_sp<SkImage> img = SkImage::MakeFromBitmap(b->bitmap)) {
    const SkRect dst = SkRect::MakeLTRB(left, top, right, bottom);
    c->drawImageLattice(img.get(), lattice, dst, SkFilterMode::kNearest,
                        ap != nullptr ? &ap->paint : nullptr);
  }
}

// android.graphics.drawable.AnimatedVectorDrawable: acl re-owns the render stack (ADR-4, hwui
// dropped), and the RenderThread AnimatorSet backing this lives in hwui. Stub the animator seam
// so an AVD inflates and draws its underlying (static) VectorDrawable — the animation itself is a
// follow-on. Property-holder creators return a non-zero sentinel; control calls are no-ops.
// The AnimatorSet ptr is wrapped in a VirtualRefBasePtr (inc/decStrong'd), so it must be a real
// ref-counted object, not a sentinel. Property holders may be too — hand out real ones as well.
const JNINativeMethod kAnimatedVectorDrawable[] = {
    {"nCreateAnimatorSet", "()J", reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jlong {
       return reinterpret_cast<jlong>(new VirtualLightRefBase()); })},
    {"nSetVectorDrawableTarget", "(JJ)V",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jlong) {})},
    {"nAddAnimator", "(JJJJJII)V",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jlong, jlong, jlong, jlong, jint, jint) {})},
    {"nSetPropertyHolderData", "(J[FI)V",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jfloatArray, jint) {})},
    {"nSetPropertyHolderData", "(J[II)V",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jintArray, jint) {})},
    {"nStart",
     "(JLandroid/graphics/drawable/AnimatedVectorDrawable$VectorDrawableAnimatorRT;I)V",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jobject, jint) {})},
    {"nReverse",
     "(JLandroid/graphics/drawable/AnimatedVectorDrawable$VectorDrawableAnimatorRT;I)V",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jobject, jint) {})},
    {"nCreateGroupPropertyHolder", "(JIFF)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jint, jfloat, jfloat) -> jlong {
       return reinterpret_cast<jlong>(new VirtualLightRefBase()); })},
    {"nCreatePathDataPropertyHolder", "(JJJ)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jlong, jlong) -> jlong {
       return reinterpret_cast<jlong>(new VirtualLightRefBase()); })},
    {"nCreatePathColorPropertyHolder", "(JIII)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jint, jint, jint) -> jlong {
       return reinterpret_cast<jlong>(new VirtualLightRefBase()); })},
    {"nCreatePathPropertyHolder", "(JIFF)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jint, jfloat, jfloat) -> jlong {
       return reinterpret_cast<jlong>(new VirtualLightRefBase()); })},
    {"nCreateRootAlphaPropertyHolder", "(JFF)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jfloat, jfloat) -> jlong {
       return reinterpret_cast<jlong>(new VirtualLightRefBase()); })},
    {"nEnd", "(J)V", reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong) {})},
    {"nReset", "(J)V", reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong) {})},
};

const JNINativeMethod kColorFilter[] = {
    {"nativeGetFinalizer", "()J", reinterpret_cast<void*>(ColorFilter_nativeGetFinalizer)},
};
const JNINativeMethod kBlendModeColorFilter[] = {
    {"native_CreateBlendModeFilter", "(II)J",
     reinterpret_cast<void*>(BlendModeColorFilter_native_Create)},
};

const JNINativeMethod kNinePatch[] = {
    {"isNinePatchChunk", "([B)Z", reinterpret_cast<void*>(NinePatch_isNinePatchChunk)},
    {"validateNinePatchChunk", "([B)J", reinterpret_cast<void*>(NinePatch_validateNinePatchChunk)},
    {"nativeFinalize", "(J)V", reinterpret_cast<void*>(NinePatch_nativeFinalize)},
};

const JNINativeMethod kImageDecoder[] = {
    {"nCreate", "(JZLandroid/graphics/ImageDecoder$Source;)Landroid/graphics/ImageDecoder;",
     reinterpret_cast<void*>(ImageDecoder_nCreateAsset)},
    {"nCreate", "([BIIZLandroid/graphics/ImageDecoder$Source;)Landroid/graphics/ImageDecoder;",
     reinterpret_cast<void*>(ImageDecoder_nCreateBytes)},
    {"nCreate",
     "(Ljava/io/InputStream;[BZLandroid/graphics/ImageDecoder$Source;)Landroid/graphics/ImageDecoder;",
     reinterpret_cast<void*>(ImageDecoder_nCreateStream)},
    {"nCreate",
     "(Ljava/nio/ByteBuffer;IIZLandroid/graphics/ImageDecoder$Source;)Landroid/graphics/ImageDecoder;",
     reinterpret_cast<void*>(ImageDecoder_nCreateBuffer)},
    {"nDecodeBitmap",
     "(JLandroid/graphics/ImageDecoder;ZIILandroid/graphics/Rect;ZIZZZJZ)Landroid/graphics/Bitmap;",
     reinterpret_cast<void*>(ImageDecoder_nDecodeBitmap)},
    {"nGetSampledSize", "(JI)Landroid/util/Size;",
     reinterpret_cast<void*>(ImageDecoder_nGetSampledSize)},
    {"nGetPadding", "(JLandroid/graphics/Rect;)V", reinterpret_cast<void*>(ImageDecoder_nGetPadding)},
    {"nClose", "(J)V", reinterpret_cast<void*>(ImageDecoder_nClose)},
};

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
    {"nativeHasAlpha", "(J)Z", reinterpret_cast<void*>(Bitmap_nativeHasAlpha)},
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
    {"nGetClipBounds", "(JLandroid/graphics/Rect;)Z", reinterpret_cast<void*>(Canvas_nGetClipBounds)},
    {"nConcat", "(JJ)V", reinterpret_cast<void*>(Canvas_nConcat)},
    {"nSetMatrix", "(JJ)V", reinterpret_cast<void*>(Canvas_nSetMatrix)},
    {"nGetMatrix", "(JJ)V", reinterpret_cast<void*>(Canvas_nGetMatrix)},
    {"nQuickReject", "(JFFFF)Z", reinterpret_cast<void*>(Canvas_nQuickRejectRect)},
    {"nQuickReject", "(JJ)Z", reinterpret_cast<void*>(Canvas_nQuickRejectPath)},
};
const JNINativeMethod kMatrix[] = {
    {"nCreate", "(J)J", reinterpret_cast<void*>(Matrix_nCreate)},
    {"nGetNativeFinalizer", "()J", reinterpret_cast<void*>(Matrix_nGetNativeFinalizer)},
    {"nGetValues", "(J[F)V", reinterpret_cast<void*>(Matrix_nGetValues)},
    {"nSetValues", "(J[F)V", reinterpret_cast<void*>(Matrix_nSetValues)},
    {"nReset", "(J)V", reinterpret_cast<void*>(Matrix_nReset)},
    {"nSet", "(JJ)V", reinterpret_cast<void*>(Matrix_nSet)},
    {"nSetConcat", "(JJJ)V", reinterpret_cast<void*>(Matrix_nSetConcat)},
    {"nSetTranslate", "(JFF)V", reinterpret_cast<void*>(Matrix_nSetTranslate)},
    {"nSetScale", "(JFFFF)V", reinterpret_cast<void*>(Matrix_nSetScale4)},
    {"nSetScale", "(JFF)V", reinterpret_cast<void*>(Matrix_nSetScale2)},
    {"nSetRotate", "(JFFF)V", reinterpret_cast<void*>(Matrix_nSetRotate3)},
    {"nSetRotate", "(JF)V", reinterpret_cast<void*>(Matrix_nSetRotate1)},
    {"nSetSkew", "(JFFFF)V", reinterpret_cast<void*>(Matrix_nSetSkew4)},
    {"nSetSkew", "(JFF)V", reinterpret_cast<void*>(Matrix_nSetSkew2)},
    {"nSetSinCos", "(JFFFF)V", reinterpret_cast<void*>(Matrix_nSetSinCos4)},
    {"nSetSinCos", "(JFF)V", reinterpret_cast<void*>(Matrix_nSetSinCos2)},
    {"nPreTranslate", "(JFF)V", reinterpret_cast<void*>(Matrix_nPreTranslate)},
    {"nPreScale", "(JFFFF)V", reinterpret_cast<void*>(Matrix_nPreScale4)},
    {"nPreScale", "(JFF)V", reinterpret_cast<void*>(Matrix_nPreScale2)},
    {"nPreRotate", "(JFFF)V", reinterpret_cast<void*>(Matrix_nPreRotate3)},
    {"nPreRotate", "(JF)V", reinterpret_cast<void*>(Matrix_nPreRotate1)},
    {"nPreSkew", "(JFFFF)V", reinterpret_cast<void*>(Matrix_nPreSkew4)},
    {"nPreSkew", "(JFF)V", reinterpret_cast<void*>(Matrix_nPreSkew2)},
    {"nPreConcat", "(JJ)V", reinterpret_cast<void*>(Matrix_nPreConcat)},
    {"nPostTranslate", "(JFF)V", reinterpret_cast<void*>(Matrix_nPostTranslate)},
    {"nPostScale", "(JFFFF)V", reinterpret_cast<void*>(Matrix_nPostScale4)},
    {"nPostScale", "(JFF)V", reinterpret_cast<void*>(Matrix_nPostScale2)},
    {"nPostRotate", "(JFFF)V", reinterpret_cast<void*>(Matrix_nPostRotate3)},
    {"nPostRotate", "(JF)V", reinterpret_cast<void*>(Matrix_nPostRotate1)},
    {"nPostSkew", "(JFFFF)V", reinterpret_cast<void*>(Matrix_nPostSkew4)},
    {"nPostSkew", "(JFF)V", reinterpret_cast<void*>(Matrix_nPostSkew2)},
    {"nPostConcat", "(JJ)V", reinterpret_cast<void*>(Matrix_nPostConcat)},
    {"nInvert", "(JJ)Z", reinterpret_cast<void*>(Matrix_nInvert)},
    {"nIsIdentity", "(J)Z", reinterpret_cast<void*>(Matrix_nIsIdentity)},
    {"nIsAffine", "(J)Z", reinterpret_cast<void*>(Matrix_nIsAffine)},
    {"nRectStaysRect", "(J)Z", reinterpret_cast<void*>(Matrix_nRectStaysRect)},
    {"nEquals", "(JJ)Z", reinterpret_cast<void*>(Matrix_nEquals)},
    {"nMapRadius", "(JF)F", reinterpret_cast<void*>(Matrix_nMapRadius)},
    {"nMapRect", "(JLandroid/graphics/RectF;Landroid/graphics/RectF;)Z", reinterpret_cast<void*>(Matrix_nMapRect)},
    {"nMapPoints", "(J[FI[FIIZ)V", reinterpret_cast<void*>(Matrix_nMapPoints)},
    {"nSetRectToRect", "(JLandroid/graphics/RectF;Landroid/graphics/RectF;I)Z", reinterpret_cast<void*>(Matrix_nSetRectToRect)},
    {"nSetPolyToPoly", "(J[FI[FII)Z", reinterpret_cast<void*>(Matrix_nSetPolyToPoly)},
};
const JNINativeMethod kBaseCanvas[] = {
    {"nDrawColor", "(JII)V", reinterpret_cast<void*>(Canvas_nDrawColor)},
    {"nDrawRect", "(JFFFFJ)V", reinterpret_cast<void*>(Canvas_nDrawRect)},
    {"nDrawLine", "(JFFFFJ)V", reinterpret_cast<void*>(Canvas_nDrawLine)},
    {"nDrawOval", "(JFFFFJ)V", reinterpret_cast<void*>(Canvas_nDrawOval)},
    {"nDrawCircle", "(JFFFJ)V", reinterpret_cast<void*>(Canvas_nDrawCircle)},
    {"nDrawRoundRect", "(JFFFFFFJ)V", reinterpret_cast<void*>(Canvas_nDrawRoundRect)},
    {"nDrawText", "(JLjava/lang/String;IIFFIJ)V", reinterpret_cast<void*>(Canvas_nDrawText)},
    {"nDrawText", "(J[CIIFFIJ)V", reinterpret_cast<void*>(Canvas_nDrawTextChars)},
    {"nDrawTextRun", "(JLjava/lang/String;IIIIFFZJ)V", reinterpret_cast<void*>(Canvas_nDrawTextRunString)},
    {"nDrawTextRun", "(J[CIIIIFFZJJ)V", reinterpret_cast<void*>(Canvas_nDrawTextRunChars)},
    {"nDrawGlyphs", "(J[I[FIIIJJ)V", reinterpret_cast<void*>(Canvas_nDrawGlyphs)},
    {"nDrawTextOnPath", "(J[CIIJFFIJ)V", reinterpret_cast<void*>(Canvas_nDrawTextOnPathChars)},
    {"nDrawTextOnPath", "(JLjava/lang/String;JFFIJ)V", reinterpret_cast<void*>(Canvas_nDrawTextOnPathString)},
    {"nDrawBitmap", "(JJFFJIII)V", reinterpret_cast<void*>(Canvas_nDrawBitmap)},
    {"nDrawBitmap", "(JJFFFFFFFFJII)V", reinterpret_cast<void*>(Canvas_nDrawBitmapRect)},
    {"nDrawNinePatch", "(JJJFFFFJII)V", reinterpret_cast<void*>(Canvas_nDrawNinePatch)},
    {"nDrawPath", "(JJJ)V", reinterpret_cast<void*>(Canvas_nDrawPath)},
};
const JNINativeMethod kPath[] = {
    {"nInit", "()J", reinterpret_cast<void*>(Path_nInit)},
    {"nInit", "(J)J", reinterpret_cast<void*>(Path_nInitCopy)},  // Path(Path) copy ctor
    {"nGetFinalizer", "()J", reinterpret_cast<void*>(Path_nGetFinalizer)},
    {"nMoveTo", "(JFF)V", reinterpret_cast<void*>(Path_nMoveTo)},
    {"nLineTo", "(JFF)V", reinterpret_cast<void*>(Path_nLineTo)},
    {"nQuadTo", "(JFFFF)V", reinterpret_cast<void*>(Path_nQuadTo)},
    {"nCubicTo", "(JFFFFFF)V", reinterpret_cast<void*>(Path_nCubicTo)},
    {"nClose", "(J)V", reinterpret_cast<void*>(Path_nClose)},
    {"nAddRect", "(JFFFFI)V", reinterpret_cast<void*>(Path_nAddRect)},
    {"nAddOval", "(JFFFFI)V", reinterpret_cast<void*>(Path_nAddOval)},
    {"nAddCircle", "(JFFFI)V", reinterpret_cast<void*>(Path_nAddCircle)},
    {"nAddRoundRect", "(JFFFFFFI)V", reinterpret_cast<void*>(Path_nAddRoundRect)},
    {"nApproximate", "(JF)[F", reinterpret_cast<void*>(Path_nApproximate)},
    // @CriticalNative (handle-only).
    {"nReset", "(J)V", reinterpret_cast<void*>(Path_nReset)},
    {"nRewind", "(J)V", reinterpret_cast<void*>(Path_nRewind)},
    {"nSetFillType", "(JI)V", reinterpret_cast<void*>(Path_nSetFillType)},
};
const JNINativeMethod kPaint[] = {
    {"nInit", "()J", reinterpret_cast<void*>(Paint_nInit)},
    {"nInitWithPaint", "(J)J", reinterpret_cast<void*>(Paint_nInitWithPaint)},
    {"nSet", "(JJ)V", reinterpret_cast<void*>(Paint_nSet)},
    {"nGetTextSize", "(J)F", reinterpret_cast<void*>(Paint_nGetTextSize)},
    {"nGetStyle", "(J)I", reinterpret_cast<void*>(Paint_nGetStyle)},
    {"nGetStrokeCap", "(J)I", reinterpret_cast<void*>(Paint_nGetStrokeCap)},
    {"nGetStrokeJoin", "(J)I", reinterpret_cast<void*>(Paint_nGetStrokeJoin)},
    {"nSetStrokeJoin", "(JI)V", reinterpret_cast<void*>(Paint_nSetStrokeJoin)},
    {"nGetStrokeMiter", "(J)F", reinterpret_cast<void*>(Paint_nGetStrokeMiter)},
    {"nSetStrokeMiter", "(JF)V", reinterpret_cast<void*>(Paint_nSetStrokeMiter)},
    {"nSetAlpha", "(JI)V", reinterpret_cast<void*>(Paint_nSetAlpha)},
    {"nGetTextScaleX", "(J)F", reinterpret_cast<void*>(Paint_nGetTextScaleX)},
    {"nSetTextScaleX", "(JF)V", reinterpret_cast<void*>(Paint_nSetTextScaleX)},
    {"nGetTextSkewX", "(J)F", reinterpret_cast<void*>(Paint_nGetTextSkewX)},
    {"nSetTextSkewX", "(JF)V", reinterpret_cast<void*>(Paint_nSetTextSkewX)},
    {"nSetTypeface", "(JJ)V", reinterpret_cast<void*>(Paint_nSetTypeface)},
    {"nGetTextAlign", "(J)I", reinterpret_cast<void*>(Paint_nGetTextAlign)},
    {"nSetTextAlign", "(JI)V", reinterpret_cast<void*>(Paint_nSetTextAlign)},
    {"nGetLetterSpacing", "(J)F", reinterpret_cast<void*>(Paint_nGetLetterSpacing)},
    {"nSetLetterSpacing", "(JF)V", reinterpret_cast<void*>(Paint_nSetLetterSpacing)},
    {"nSetColorFilter", "(JJ)J", reinterpret_cast<void*>(Paint_nSetColorFilter)},
    {"nSetPathEffect", "(JJ)J", reinterpret_cast<void*>(Paint_nSetPathEffect)},
    {"nSetMaskFilter", "(JJ)J", reinterpret_cast<void*>(Paint_nSetMaskFilter)},
    {"nSetShadowLayer", "(JFFFJJ)V", reinterpret_cast<void*>(Paint_nSetShadowLayer)},
    {"nSetDither", "(JZ)V", reinterpret_cast<void*>(Paint_nSetDither)},
    {"nSetLinearText", "(JZ)V", reinterpret_cast<void*>(Paint_nSetLinearText)},
    {"nSetSubpixelText", "(JZ)V", reinterpret_cast<void*>(Paint_nSetSubpixelText)},
    {"nSetUnderlineText", "(JZ)V", reinterpret_cast<void*>(Paint_nSetUnderlineText)},
    {"nGetHinting", "(J)I", reinterpret_cast<void*>(Paint_nGetHinting)},
    {"nSetHinting", "(JI)V", reinterpret_cast<void*>(Paint_nSetHinting)},
    {"nGetWordSpacing", "(J)F", reinterpret_cast<void*>(Paint_nGetWordSpacing)},
    {"nSetWordSpacing", "(JF)V", reinterpret_cast<void*>(Paint_nSetWordSpacing)},
    {"nGetStartHyphenEdit", "(J)I", reinterpret_cast<void*>(Paint_nGetStartHyphenEdit)},
    {"nGetEndHyphenEdit", "(J)I", reinterpret_cast<void*>(Paint_nGetEndHyphenEdit)},
    {"nSetStartHyphenEdit", "(JI)V", reinterpret_cast<void*>(Paint_nSetStartHyphenEdit)},
    {"nSetEndHyphenEdit", "(JI)V", reinterpret_cast<void*>(Paint_nSetEndHyphenEdit)},
    {"nSetFontFeatureSettings", "(JLjava/lang/String;)V", reinterpret_cast<void*>(Paint_nSetFontFeatureSettings)},
    {"nSetFakeBoldText", "(JZ)V", reinterpret_cast<void*>(Paint_nSetFakeBoldText)},
    {"nSetFilterBitmap", "(JZ)V", reinterpret_cast<void*>(Paint_nSetFilterBitmap)},
    {"nSetStrikeThruText", "(JZ)V", reinterpret_cast<void*>(Paint_nSetStrikeThruText)},
    {"nIsElegantTextHeight", "(J)Z", reinterpret_cast<void*>(Paint_nIsElegantTextHeight)},
    {"nSetElegantTextHeight", "(JZ)V", reinterpret_cast<void*>(Paint_nSetElegantTextHeight)},
    {"nHasShadowLayer", "(J)Z", reinterpret_cast<void*>(Paint_nHasShadowLayer)},
    {"nAscent", "(J)F", reinterpret_cast<void*>(Paint_nAscent)},
    {"nDescent", "(J)F", reinterpret_cast<void*>(Paint_nDescent)},
    {"nGetUnderlinePosition", "(J)F", reinterpret_cast<void*>(Paint_nGetUnderlinePosition)},
    {"nGetUnderlineThickness", "(J)F", reinterpret_cast<void*>(Paint_nGetUnderlineThickness)},
    {"nGetStrikeThruPosition", "(J)F", reinterpret_cast<void*>(Paint_nGetStrikeThruPosition)},
    {"nGetStrikeThruThickness", "(J)F", reinterpret_cast<void*>(Paint_nGetStrikeThruThickness)},
    {"nEqualsForTextMeasurement", "(JJ)Z", reinterpret_cast<void*>(Paint_nEqualsForTextMeasurement)},
    {"nGetFillPath", "(JJJ)Z", reinterpret_cast<void*>(Paint_nGetFillPath)},
    {"nGetFontMetrics", "(JLandroid/graphics/Paint$FontMetrics;)F", reinterpret_cast<void*>(Paint_nGetFontMetrics)},
    {"nHasGlyph", "(JILjava/lang/String;)Z", reinterpret_cast<void*>(Paint_nHasGlyph)},
    {"nGetRunAdvance", "(J[CIIIIZI)F", reinterpret_cast<void*>(Paint_nGetRunAdvance)},
    {"nGetOffsetForAdvance", "(J[CIIIIZF)I", reinterpret_cast<void*>(Paint_nGetOffsetForAdvance)},
    {"nGetFontMetricsIntForText", "(J[CIIIIZLandroid/graphics/Paint$FontMetricsInt;)V", reinterpret_cast<void*>(Paint_nGetFontMetricsIntForTextChars)},
    {"nGetFontMetricsIntForText", "(JLjava/lang/String;IIIIZLandroid/graphics/Paint$FontMetricsInt;)V", reinterpret_cast<void*>(Paint_nGetFontMetricsIntForTextString)},
    {"nGetCharArrayBounds", "(J[CIIILandroid/graphics/Rect;)V", reinterpret_cast<void*>(Paint_nGetCharArrayBounds)},
    {"nGetTextRunCursor", "(J[CIIIII)I", reinterpret_cast<void*>(Paint_nGetTextRunCursorChars)},
    {"nGetTextRunCursor", "(JLjava/lang/String;IIIII)I", reinterpret_cast<void*>(Paint_nGetTextRunCursorString)},
    {"nBreakText", "(J[CIIFI[F)I", reinterpret_cast<void*>(Paint_nBreakTextChars)},
    {"nBreakText", "(JLjava/lang/String;ZFI[F)I", reinterpret_cast<void*>(Paint_nBreakTextString)},
    {"nGetNativeFinalizer", "()J", reinterpret_cast<void*>(Paint_nGetNativeFinalizer)},
    {"nSetColor", "(JI)V", reinterpret_cast<void*>(Paint_nSetColor)},
    {"nReset", "(J)V", reinterpret_cast<void*>(Paint_nReset)},
    {"nSetFlags", "(JI)V", reinterpret_cast<void*>(Paint_nSetFlags)},
    {"nGetFlags", "(J)I", reinterpret_cast<void*>(Paint_nGetFlags)},
    {"nSetTextLocales", "(JLjava/lang/String;)I", reinterpret_cast<void*>(Paint_nSetTextLocales)},
    {"nSetTextLocalesByMinikinLocaleListId", "(JI)V", reinterpret_cast<void*>(Paint_nSetTextLocalesByMinikinLocaleListId)},
    {"nSetStyle", "(JI)V", reinterpret_cast<void*>(Paint_nSetStyle)},
    {"nSetXfermode", "(JI)V", reinterpret_cast<void*>(Paint_nSetXfermode)},
    {"nSetStrokeWidth", "(JF)V", reinterpret_cast<void*>(Paint_nSetStrokeWidth)},
    {"nSetStrokeCap", "(JI)V", reinterpret_cast<void*>(Paint_nSetStrokeCap)},
    {"nSetAntiAlias", "(JZ)V", reinterpret_cast<void*>(Paint_nSetAntiAlias)},
    {"nSetTextSize", "(JF)V", reinterpret_cast<void*>(Paint_nSetTextSize)},
    {"nSetShader", "(JJ)J", reinterpret_cast<void*>(Paint_nSetShader)},
    {"nGetFontMetricsInt", "(JLandroid/graphics/Paint$FontMetricsInt;)I",
     reinterpret_cast<void*>(Paint_nGetFontMetricsInt)},
    {"nGetTextAdvances", "(JLjava/lang/String;IIIII[FI)F",
     reinterpret_cast<void*>(Paint_nGetTextAdvances)},
    {"nGetStringBounds", "(JLjava/lang/String;IIILandroid/graphics/Rect;)V",
     reinterpret_cast<void*>(Paint_nGetStringBounds)},
};
const JNINativeMethod kShader[] = {
    {"nativeGetFinalizer", "()J", reinterpret_cast<void*>(Shader_nativeGetFinalizer)},
};
const JNINativeMethod kLinearGradient[] = {
    {"nativeCreate", "(JFFFF[J[FIJ)J", reinterpret_cast<void*>(LinearGradient_nativeCreate)},
};
const JNINativeMethod kRadialGradient[] = {
    {"nativeCreate", "(JFFFFFF[J[FIJ)J", reinterpret_cast<void*>(RadialGradient_nativeCreate)},
};
#define RNM(name, sig, fn) {name, sig, reinterpret_cast<void*>(fn)}
const JNINativeMethod kRenderNode[] = {
    // normal (env, jclass)
    RNM("nCreate", "(Ljava/lang/String;)J", RenderNode_nCreate),
    RNM("nGetNativeFinalizer", "()J", RenderNode_nGetNativeFinalizer),
    RNM("nOutput", "(J)V", RenderNode_nOutput),
    RNM("nGetUsageSize", "(J)I", RenderNode_nGetUsageSize),
    RNM("nGetAllocatedSize", "(J)I", RenderNode_nGetAllocatedSize),
    RNM("nRequestPositionUpdates", "(JLjava/lang/ref/WeakReference;)V",
        RenderNode_nRequestPositionUpdates),
    RNM("nAddAnimator", "(JJ)V", RenderNode_nAddAnimator),
    RNM("nEndAllAnimators", "(J)V", RenderNode_nEndAllAnimators),
    RNM("nForceEndAnimators", "(J)V", RenderNode_nForceEndAnimators),
    // @CriticalNative (handle-only)
    RNM("nDiscardDisplayList", "(J)V", RenderNode_nDiscardDisplayList),
    RNM("nIsValid", "(J)Z", RenderNode_nIsValid),
    RNM("nGetTransformMatrix", "(JJ)V", RenderNode_nGetTransformMatrix),
    RNM("nGetInverseTransformMatrix", "(JJ)V", RenderNode_nGetInverseTransformMatrix),
    RNM("nHasIdentityMatrix", "(J)Z", RenderNode_nHasIdentityMatrix),
    RNM("nOffsetTopAndBottom", "(JI)Z", RenderNode_nOffsetTopAndBottom),
    RNM("nOffsetLeftAndRight", "(JI)Z", RenderNode_nOffsetLeftAndRight),
    RNM("nSetLeftTopRightBottom", "(JIIII)Z", RenderNode_nSetLeftTopRightBottom),
    RNM("nSetLeft", "(JI)Z", RenderNode_nSetLeft),
    RNM("nSetTop", "(JI)Z", RenderNode_nSetTop),
    RNM("nSetRight", "(JI)Z", RenderNode_nSetRight),
    RNM("nSetBottom", "(JI)Z", RenderNode_nSetBottom),
    RNM("nGetLeft", "(J)I", RenderNode_nGetLeft),
    RNM("nGetTop", "(J)I", RenderNode_nGetTop),
    RNM("nGetRight", "(J)I", RenderNode_nGetRight),
    RNM("nGetBottom", "(J)I", RenderNode_nGetBottom),
    RNM("nSetCameraDistance", "(JF)Z", RenderNode_nSetCameraDistance),
    RNM("nSetPivotY", "(JF)Z", RenderNode_nSetPivotY),
    RNM("nSetPivotX", "(JF)Z", RenderNode_nSetPivotX),
    RNM("nResetPivot", "(J)Z", RenderNode_nResetPivot),
    RNM("nSetLayerType", "(JI)Z", RenderNode_nSetLayerType),
    RNM("nGetLayerType", "(J)I", RenderNode_nGetLayerType),
    RNM("nSetLayerPaint", "(JJ)Z", RenderNode_nSetLayerPaint),
    RNM("nSetClipToBounds", "(JZ)Z", RenderNode_nSetClipToBounds),
    RNM("nGetClipToBounds", "(J)Z", RenderNode_nGetClipToBounds),
    RNM("nSetClipBounds", "(JIIII)Z", RenderNode_nSetClipBounds),
    RNM("nSetClipBoundsEmpty", "(J)Z", RenderNode_nSetClipBoundsEmpty),
    RNM("nSetProjectBackwards", "(JZ)Z", RenderNode_nSetProjectBackwards),
    RNM("nSetProjectionReceiver", "(JZ)Z", RenderNode_nSetProjectionReceiver),
    RNM("nSetOutlineRoundRect", "(JIIIIFF)Z", RenderNode_nSetOutlineRoundRect),
    RNM("nSetOutlinePath", "(JJF)Z", RenderNode_nSetOutlinePath),
    RNM("nSetOutlineEmpty", "(J)Z", RenderNode_nSetOutlineEmpty),
    RNM("nSetOutlineNone", "(J)Z", RenderNode_nSetOutlineNone),
    RNM("nClearStretch", "(J)Z", RenderNode_nClearStretch),
    RNM("nStretch", "(JFFFF)Z", RenderNode_nStretch),
    RNM("nHasShadow", "(J)Z", RenderNode_nHasShadow),
    RNM("nSetSpotShadowColor", "(JI)Z", RenderNode_nSetSpotShadowColor),
    RNM("nSetAmbientShadowColor", "(JI)Z", RenderNode_nSetAmbientShadowColor),
    RNM("nGetSpotShadowColor", "(J)I", RenderNode_nGetSpotShadowColor),
    RNM("nGetAmbientShadowColor", "(J)I", RenderNode_nGetAmbientShadowColor),
    RNM("nSetClipToOutline", "(JZ)Z", RenderNode_nSetClipToOutline),
    RNM("nSetRevealClip", "(JZFFF)Z", RenderNode_nSetRevealClip),
    RNM("nSetAlpha", "(JF)Z", RenderNode_nSetAlpha),
    RNM("nSetRenderEffect", "(JJ)Z", RenderNode_nSetRenderEffect),
    RNM("nSetHasOverlappingRendering", "(JZ)Z", RenderNode_nSetHasOverlappingRendering),
    RNM("nSetUsageHint", "(JI)V", RenderNode_nSetUsageHint),
    RNM("nSetElevation", "(JF)Z", RenderNode_nSetElevation),
    RNM("nSetTranslationX", "(JF)Z", RenderNode_nSetTranslationX),
    RNM("nSetTranslationY", "(JF)Z", RenderNode_nSetTranslationY),
    RNM("nSetTranslationZ", "(JF)Z", RenderNode_nSetTranslationZ),
    RNM("nSetRotation", "(JF)Z", RenderNode_nSetRotation),
    RNM("nSetRotationX", "(JF)Z", RenderNode_nSetRotationX),
    RNM("nSetRotationY", "(JF)Z", RenderNode_nSetRotationY),
    RNM("nSetScaleX", "(JF)Z", RenderNode_nSetScaleX),
    RNM("nSetScaleY", "(JF)Z", RenderNode_nSetScaleY),
    RNM("nSetStaticMatrix", "(JJ)Z", RenderNode_nSetStaticMatrix),
    RNM("nSetAnimationMatrix", "(JJ)Z", RenderNode_nSetAnimationMatrix),
    RNM("nHasOverlappingRendering", "(J)Z", RenderNode_nHasOverlappingRendering),
    RNM("nGetAnimationMatrix", "(JJ)Z", RenderNode_nGetAnimationMatrix),
    RNM("nGetClipToOutline", "(J)Z", RenderNode_nGetClipToOutline),
    RNM("nGetAlpha", "(J)F", RenderNode_nGetAlpha),
    RNM("nGetCameraDistance", "(J)F", RenderNode_nGetCameraDistance),
    RNM("nGetScaleX", "(J)F", RenderNode_nGetScaleX),
    RNM("nGetScaleY", "(J)F", RenderNode_nGetScaleY),
    RNM("nGetElevation", "(J)F", RenderNode_nGetElevation),
    RNM("nGetTranslationX", "(J)F", RenderNode_nGetTranslationX),
    RNM("nGetTranslationY", "(J)F", RenderNode_nGetTranslationY),
    RNM("nGetTranslationZ", "(J)F", RenderNode_nGetTranslationZ),
    RNM("nGetRotation", "(J)F", RenderNode_nGetRotation),
    RNM("nGetRotationX", "(J)F", RenderNode_nGetRotationX),
    RNM("nGetRotationY", "(J)F", RenderNode_nGetRotationY),
    RNM("nIsPivotExplicitlySet", "(J)Z", RenderNode_nIsPivotExplicitlySet),
    RNM("nGetPivotX", "(J)F", RenderNode_nGetPivotX),
    RNM("nGetPivotY", "(J)F", RenderNode_nGetPivotY),
    RNM("nGetWidth", "(J)I", RenderNode_nGetWidth),
    RNM("nGetHeight", "(J)I", RenderNode_nGetHeight),
    RNM("nSetAllowForceDark", "(JZ)Z", RenderNode_nSetAllowForceDark),
    RNM("nGetAllowForceDark", "(J)Z", RenderNode_nGetAllowForceDark),
    RNM("nGetUniqueId", "(J)J", RenderNode_nGetUniqueId),
};
#undef RNM
const JNINativeMethod kColorSpaceRgb[] = {
    {"nativeCreate", "(FFFFFFF[F)J", reinterpret_cast<void*>(ColorSpaceRgb_nativeCreate)},
    {"nativeGetNativeFinalizer", "()J",
     reinterpret_cast<void*>(ColorSpaceRgb_nativeGetNativeFinalizer)},
};

const JNINativeMethod kTypeface[] = {
    {"nativeGetReleaseFunc", "()J", reinterpret_cast<void*>(Typeface_nativeGetReleaseFunc)},
    {"nativeCreateFromArray", "([JJII)J", reinterpret_cast<void*>(Typeface_nativeCreateFromArray)},
    {"nativeCreateFromTypeface", "(JI)J", reinterpret_cast<void*>(Typeface_nativeCreateFromTypeface)},
    {"nativeCreateFromTypefaceWithExactStyle", "(JIZ)J", reinterpret_cast<void*>(Typeface_nativeCreateFromTypefaceWithExactStyle)},
    {"nativeCreateFromTypefaceWithVariation", "(JLjava/util/List;)J", reinterpret_cast<void*>(Typeface_nativeCreateFromTypefaceWithVariation)},
    {"nativeCreateWeightAlias", "(JI)J", reinterpret_cast<void*>(Typeface_nativeCreateWeightAlias)},
    {"nativeGetSupportedAxes", "(J)[I", reinterpret_cast<void*>(Typeface_nativeGetSupportedAxes)},
    {"nativeSetDefault", "(J)V", reinterpret_cast<void*>(Typeface_nativeSetDefault)},
    {"nativeGetStyle", "(J)I", reinterpret_cast<void*>(Typeface_nativeGetStyle)},
    {"nativeGetWeight", "(J)I", reinterpret_cast<void*>(Typeface_nativeGetWeight)},
    {"nativeGetFamilySize", "(J)I", reinterpret_cast<void*>(Typeface_nativeGetFamilySize)},
    {"nativeGetFamily", "(JI)J", reinterpret_cast<void*>(Typeface_nativeGetFamily)},
    {"nativeRegisterGenericFamily", "(Ljava/lang/String;J)V", reinterpret_cast<void*>(Typeface_nativeRegisterGenericFamily)},
    {"nativeWriteTypefaces", "(Ljava/nio/ByteBuffer;[J)I", reinterpret_cast<void*>(Typeface_nativeWriteTypefaces)},
    {"nativeReadTypefaces", "(Ljava/nio/ByteBuffer;)[J", reinterpret_cast<void*>(Typeface_nativeReadTypefaces)},
    {"nativeForceSetStaticFinalField", "(Ljava/lang/String;Landroid/graphics/Typeface;)V", reinterpret_cast<void*>(Typeface_nativeForceSetStaticFinalField)},
    {"nativeAddFontCollections", "(J)V", reinterpret_cast<void*>(Typeface_nativeAddFontCollections)},
    {"nativeWarmUpCache", "(Ljava/lang/String;)V", reinterpret_cast<void*>(Typeface_nativeWarmUpCache)},
};
const JNINativeMethod kFontBuilder[] = {
    {"nInitBuilder", "()J", reinterpret_cast<void*>(FontBuilder_nInitBuilder)},
    {"nAddAxis", "(JIF)V", reinterpret_cast<void*>(FontBuilder_nAddAxis)},
    {"nBuild", "(JLjava/nio/ByteBuffer;Ljava/lang/String;Ljava/lang/String;IZI)J", reinterpret_cast<void*>(FontBuilder_nBuild)},
    {"nGetReleaseNativeFont", "()J", reinterpret_cast<void*>(FontBuilder_nGetReleaseNativeFont)},
    {"nClone", "(JJIZI)J", reinterpret_cast<void*>(FontBuilder_nClone)},
};
const JNINativeMethod kFontFamilyBuilder[] = {
    {"nInitBuilder", "()J", reinterpret_cast<void*>(FontFamilyBuilder_nInitBuilder)},
    {"nAddFont", "(JJ)V", reinterpret_cast<void*>(FontFamilyBuilder_nAddFont)},
    {"nBuild", "(JLjava/lang/String;IZ)J", reinterpret_cast<void*>(FontFamilyBuilder_nBuild)},
    {"nGetReleaseNativeFamily", "()J", reinterpret_cast<void*>(FontFamilyBuilder_nGetReleaseNativeFamily)},
};
const JNINativeMethod kFontFamily[] = {
    {"nGetFontSize", "(J)I", reinterpret_cast<void*>(FontFamily_nGetFontSize)},
    {"nGetFont", "(JI)J", reinterpret_cast<void*>(FontFamily_nGetFont)},
    {"nGetLangTags", "(J)Ljava/lang/String;", reinterpret_cast<void*>(FontFamily_nGetLangTags)},
    {"nGetVariant", "(J)I", reinterpret_cast<void*>(FontFamily_nGetVariant)},
};
const JNINativeMethod kFont[] = {
    {"nGetReleaseNativeFont", "()J", reinterpret_cast<void*>(Font_nGetReleaseNativeFont)},
    {"nCloneFont", "(J)J", reinterpret_cast<void*>(Font_nCloneFont)},
    {"nGetPackedStyle", "(J)I", reinterpret_cast<void*>(Font_nGetPackedStyle)},
    {"nGetIndex", "(J)I", reinterpret_cast<void*>(Font_nGetIndex)},
    {"nGetAxisCount", "(J)I", reinterpret_cast<void*>(Font_nGetAxisCount)},
    {"nGetAxisInfo", "(JI)J", reinterpret_cast<void*>(Font_nGetAxisInfo)},
    {"nGetFontPath", "(J)Ljava/lang/String;", reinterpret_cast<void*>(Font_nGetFontPath)},
    {"nGetLocaleList", "(J)Ljava/lang/String;", reinterpret_cast<void*>(Font_nGetLocaleList)},
    {"nGetSourceId", "(J)I", reinterpret_cast<void*>(Font_nGetSourceId)},
    {"nGetMinikinFontPtr", "(J)J", reinterpret_cast<void*>(Font_nGetMinikinFontPtr)},
    {"nGetAvailableFontSet", "()[J", reinterpret_cast<void*>(Font_nGetAvailableFontSet)},
    {"nNewByteBuffer", "(J)Ljava/nio/ByteBuffer;", reinterpret_cast<void*>(Font_nNewByteBuffer)},
    {"nGetBufferAddress", "(J)J", reinterpret_cast<void*>(Font_nGetBufferAddress)},
    {"nGetGlyphBounds", "(JIJLandroid/graphics/RectF;)F", reinterpret_cast<void*>(Font_nGetGlyphBounds)},
    {"nGetFontMetrics", "(JJLandroid/graphics/Paint$FontMetrics;)F", reinterpret_cast<void*>(Font_nGetFontMetrics)},
};
const JNINativeMethod kRegion[] = {
    {"isEmpty", "()Z", reinterpret_cast<void*>(Region_isEmpty)},
    {"isRect", "()Z", reinterpret_cast<void*>(Region_isRect)},
    {"isComplex", "()Z", reinterpret_cast<void*>(Region_isComplex)},
    {"contains", "(II)Z", reinterpret_cast<void*>(Region_contains)},
    {"quickContains", "(IIII)Z", reinterpret_cast<void*>(Region_quickContains)},
    {"quickReject", "(IIII)Z", reinterpret_cast<void*>(Region_quickRejectRect)},
    {"quickReject", "(Landroid/graphics/Region;)Z", reinterpret_cast<void*>(Region_quickRejectRegion)},
    {"translate", "(IILandroid/graphics/Region;)V", reinterpret_cast<void*>(Region_translate)},
    {"scale", "(FLandroid/graphics/Region;)V", reinterpret_cast<void*>(Region_scale)},
    {"nativeEquals", "(JJ)Z", reinterpret_cast<void*>(Region_nativeEquals)},
    {"nativeConstructor", "()J", reinterpret_cast<void*>(Region_nativeConstructor)},
    {"nativeDestructor", "(J)V", reinterpret_cast<void*>(Region_nativeDestructor)},
    {"nativeSetRegion", "(JJ)V", reinterpret_cast<void*>(Region_nativeSetRegion)},
    {"nativeSetRect", "(JIIII)Z", reinterpret_cast<void*>(Region_nativeSetRect)},
    {"nativeSetPath", "(JJJ)Z", reinterpret_cast<void*>(Region_nativeSetPath)},
    {"nativeGetBounds", "(JLandroid/graphics/Rect;)Z", reinterpret_cast<void*>(Region_nativeGetBounds)},
    {"nativeGetBoundaryPath", "(JJ)Z", reinterpret_cast<void*>(Region_nativeGetBoundaryPath)},
    {"nativeOp", "(JIIIII)Z", reinterpret_cast<void*>(Region_nativeOpIIIII)},
    {"nativeOp", "(JLandroid/graphics/Rect;JI)Z", reinterpret_cast<void*>(Region_nativeOpRectRegion)},
    {"nativeOp", "(JJJI)Z", reinterpret_cast<void*>(Region_nativeOpRegionRegion)},
    {"nativeCreateFromParcel", "(Landroid/os/Parcel;)J", reinterpret_cast<void*>(Region_nativeCreateFromParcel)},
    {"nativeWriteToParcel", "(JLandroid/os/Parcel;)Z", reinterpret_cast<void*>(Region_nativeWriteToParcel)},
    {"nativeToString", "(J)Ljava/lang/String;", reinterpret_cast<void*>(Region_nativeToString)},
};
const JNINativeMethod kHardwareRenderer[] = {
    {"nSetRtAnimationsEnabled", "(Z)V", reinterpret_cast<void*>(HardwareRenderer_nSetRtAnimationsEnabled)},
    // ViewRootImpl.<init> -> HighContrastTextManager sets this (a static debug toggle); no-op.
    {"nSetHighContrastText", "(Z)V", reinterpret_cast<void*>(+[](JNIEnv*, jclass, jboolean) {})},
};
const JNINativeMethod kDisplayEventReceiver[] = {
    {"nativeInit", "(Ljava/lang/ref/WeakReference;Landroid/os/MessageQueue;II)J", reinterpret_cast<void*>(DisplayEventReceiver_nativeInit)},
    {"nativeDispose", "(J)V", reinterpret_cast<void*>(DisplayEventReceiver_nativeDispose)},
    {"nativeScheduleVsync", "(J)V", reinterpret_cast<void*>(DisplayEventReceiver_nativeScheduleVsync)},
    {"nativeGetLatestVsyncEventData", "(J)Landroid/view/DisplayEventReceiver$VsyncEventData;", reinterpret_cast<void*>(DisplayEventReceiver_nativeGetLatestVsyncEventData)},
};
const JNINativeMethod kInterpolator[] = {
    {"nativeConstructor", "(II)J", reinterpret_cast<void*>(Interpolator_nativeConstructor)},
    {"nativeDestructor", "(J)V", reinterpret_cast<void*>(Interpolator_nativeDestructor)},
    {"nativeReset", "(JII)V", reinterpret_cast<void*>(Interpolator_nativeReset)},
    {"nativeSetKeyFrame", "(JII[F[F)V", reinterpret_cast<void*>(Interpolator_nativeSetKeyFrame)},
    {"nativeSetRepeatMirror", "(JFZ)V", reinterpret_cast<void*>(Interpolator_nativeSetRepeatMirror)},
    {"nativeTimeToValues", "(JI[F)I", reinterpret_cast<void*>(Interpolator_nativeTimeToValues)},
};
const JNINativeMethod kVirtualRefBasePtr[] = {
    {"nIncStrong", "(J)V", reinterpret_cast<void*>(VirtualRefBasePtr_nIncStrong)},
    {"nDecStrong", "(J)V", reinterpret_cast<void*>(VirtualRefBasePtr_nDecStrong)},
};
const JNINativeMethod kPathParser[] = {
    {"nParseStringForPath", "(JLjava/lang/String;I)V", reinterpret_cast<void*>(PathParser_nParseStringForPath)},
    {"nCreatePathDataFromString", "(Ljava/lang/String;I)J", reinterpret_cast<void*>(PathData_nCreateFromString)},
    {"nCreatePathFromPathData", "(JJ)V", reinterpret_cast<void*>(PathData_nCreatePathFromData)},
    {"nCreateEmptyPathData", "()J", reinterpret_cast<void*>(PathData_nCreateEmpty)},
    {"nCreatePathData", "(J)J", reinterpret_cast<void*>(PathData_nCreateCopy)},
    {"nInterpolatePathData", "(JJJF)Z", reinterpret_cast<void*>(PathData_nInterpolate)},
    {"nFinalize", "(J)V", reinterpret_cast<void*>(PathData_nFinalize)},
    {"nCanMorph", "(JJ)Z", reinterpret_cast<void*>(PathData_nCanMorph)},
    {"nSetPathData", "(JJ)V", reinterpret_cast<void*>(PathData_nSet)},
};
const JNINativeMethod kVectorDrawable[] = {
    {"nDraw", "(JJJLandroid/graphics/Rect;ZZ)I", reinterpret_cast<void*>(VD_nDraw)},
    {"nCreateTree", "(J)J", reinterpret_cast<void*>(VD_nCreateTree)},
    {"nCreateTreeFromCopy", "(JJ)J", reinterpret_cast<void*>(VD_nCreateTreeFromCopy)},
    {"nSetRendererViewportSize", "(JFF)V", reinterpret_cast<void*>(VD_nSetRendererViewportSize)},
    {"nSetRootAlpha", "(JF)Z", reinterpret_cast<void*>(VD_nSetRootAlpha)},
    {"nGetRootAlpha", "(J)F", reinterpret_cast<void*>(VD_nGetRootAlpha)},
    {"nSetAntiAlias", "(JZ)V", reinterpret_cast<void*>(VD_nSetAntiAlias)},
    {"nSetAllowCaching", "(JZ)V", reinterpret_cast<void*>(VD_nSetAllowCaching)},
    {"nCreateFullPath", "()J", reinterpret_cast<void*>(VD_nCreateFullPath)},
    {"nCreateFullPath", "(J)J", reinterpret_cast<void*>(VD_nCreateFullPathCopy)},
    {"nGetFullPathProperties", "(J[BI)Z", reinterpret_cast<void*>(VD_nGetFullPathProperties)},
    {"nUpdateFullPathProperties", "(JFIFIFFFFFIII)V", reinterpret_cast<void*>(VD_nUpdateFullPathProperties)},
    {"nUpdateFullPathFillGradient", "(JJ)V", reinterpret_cast<void*>(VD_nUpdateFullPathFillGradient)},
    {"nUpdateFullPathStrokeGradient", "(JJ)V", reinterpret_cast<void*>(VD_nUpdateFullPathStrokeGradient)},
    {"nCreateClipPath", "()J", reinterpret_cast<void*>(VD_nCreateClipPath)},
    {"nCreateClipPath", "(J)J", reinterpret_cast<void*>(VD_nCreateClipPathCopy)},
    {"nCreateGroup", "()J", reinterpret_cast<void*>(VD_nCreateGroup)},
    {"nCreateGroup", "(J)J", reinterpret_cast<void*>(VD_nCreateGroupCopy)},
    {"nSetName", "(JLjava/lang/String;)V", reinterpret_cast<void*>(VD_nSetName)},
    {"nGetGroupProperties", "(J[FI)Z", reinterpret_cast<void*>(VD_nGetGroupProperties)},
    {"nUpdateGroupProperties", "(JFFFFFFF)V", reinterpret_cast<void*>(VD_nUpdateGroupProperties)},
    {"nAddChild", "(JJ)V", reinterpret_cast<void*>(VD_nAddChild)},
    {"nSetPathString", "(JLjava/lang/String;I)V", reinterpret_cast<void*>(VD_nSetPathString)},
    {"nSetPathData", "(JJ)V", reinterpret_cast<void*>(VD_nSetPathData)},
    {"nGetRotation", "(J)F", reinterpret_cast<void*>(VD_nGetRotation)},
    {"nSetRotation", "(JF)V", reinterpret_cast<void*>(VD_nSetRotation)},
    {"nGetPivotX", "(J)F", reinterpret_cast<void*>(VD_nGetPivotX)},
    {"nSetPivotX", "(JF)V", reinterpret_cast<void*>(VD_nSetPivotX)},
    {"nGetPivotY", "(J)F", reinterpret_cast<void*>(VD_nGetPivotY)},
    {"nSetPivotY", "(JF)V", reinterpret_cast<void*>(VD_nSetPivotY)},
    {"nGetScaleX", "(J)F", reinterpret_cast<void*>(VD_nGetScaleX)},
    {"nSetScaleX", "(JF)V", reinterpret_cast<void*>(VD_nSetScaleX)},
    {"nGetScaleY", "(J)F", reinterpret_cast<void*>(VD_nGetScaleY)},
    {"nSetScaleY", "(JF)V", reinterpret_cast<void*>(VD_nSetScaleY)},
    {"nGetTranslateX", "(J)F", reinterpret_cast<void*>(VD_nGetTranslateX)},
    {"nSetTranslateX", "(JF)V", reinterpret_cast<void*>(VD_nSetTranslateX)},
    {"nGetTranslateY", "(J)F", reinterpret_cast<void*>(VD_nGetTranslateY)},
    {"nSetTranslateY", "(JF)V", reinterpret_cast<void*>(VD_nSetTranslateY)},
    {"nGetStrokeWidth", "(J)F", reinterpret_cast<void*>(VD_nGetStrokeWidth)},
    {"nSetStrokeWidth", "(JF)V", reinterpret_cast<void*>(VD_nSetStrokeWidth)},
    {"nGetStrokeColor", "(J)I", reinterpret_cast<void*>(VD_nGetStrokeColor)},
    {"nSetStrokeColor", "(JI)V", reinterpret_cast<void*>(VD_nSetStrokeColor)},
    {"nGetStrokeAlpha", "(J)F", reinterpret_cast<void*>(VD_nGetStrokeAlpha)},
    {"nSetStrokeAlpha", "(JF)V", reinterpret_cast<void*>(VD_nSetStrokeAlpha)},
    {"nGetFillColor", "(J)I", reinterpret_cast<void*>(VD_nGetFillColor)},
    {"nSetFillColor", "(JI)V", reinterpret_cast<void*>(VD_nSetFillColor)},
    {"nGetFillAlpha", "(J)F", reinterpret_cast<void*>(VD_nGetFillAlpha)},
    {"nSetFillAlpha", "(JF)V", reinterpret_cast<void*>(VD_nSetFillAlpha)},
    {"nGetTrimPathStart", "(J)F", reinterpret_cast<void*>(VD_nGetTrimPathStart)},
    {"nSetTrimPathStart", "(JF)V", reinterpret_cast<void*>(VD_nSetTrimPathStart)},
    {"nGetTrimPathEnd", "(J)F", reinterpret_cast<void*>(VD_nGetTrimPathEnd)},
    {"nSetTrimPathEnd", "(JF)V", reinterpret_cast<void*>(VD_nSetTrimPathEnd)},
    {"nGetTrimPathOffset", "(J)F", reinterpret_cast<void*>(VD_nGetTrimPathOffset)},
    {"nSetTrimPathOffset", "(JF)V", reinterpret_cast<void*>(VD_nSetTrimPathOffset)},
};

// ── SurfaceControl no-op floor ──────────────────────────────────────────────────────────────
// The host libandroid_runtime ships NO SurfaceControl natives (no SurfaceFlinger/gui), and
// android.view.WindowManagerImpl/WindowManagerGlobal are final, so Dialog/PopupWindow/Toast
// unavoidably reach ViewRootImpl.setView -> SurfaceControl. Register no-op natives so that path
// SURVIVES (the view is held by WindowManagerGlobal but no real surface is produced); acl renders
// such secondary windows itself (overlay pass). SurfaceControl$Transaction.<clinit> feeds the
// finalizer into a NativeAllocationRegistry, which REQUIRES a non-zero free-function pointer, so
// sc_free is a real function. Native handles are a fixed non-zero sentinel and ops are no-ops, so
// the no-op free is safe (no per-object allocation, no aliasing/double-free). Only a curated subset
// is registered — an unregistered native throws UnsatisfiedLinkError only if actually called, so
// the set is grown empirically to whatever setView + the first traversal touch.
int g_sc_dummy = 0;
void sc_free(void*) {}
jlong sc_handle() { return reinterpret_cast<jlong>(&g_sc_dummy); }

jlong SC_nGetNativeTransactionFinalizer(JNIEnv*, jclass) { return reinterpret_cast<jlong>(&sc_free); }
jlong SC_nCreateTransaction(JNIEnv*, jclass) { return sc_handle(); }
jlong SC_nCreate(JNIEnv*, jclass, jobject, jstring, jint, jint, jint, jint, jlong, jobject) { return sc_handle(); }
jlong SC_nReadFromParcel(JNIEnv*, jclass, jobject) { return sc_handle(); }
jlong SC_nReadTransactionFromParcel(JNIEnv*, jclass, jobject) { return sc_handle(); }
jlong SC_nCopyFromSurfaceControl(JNIEnv*, jclass, jlong) { return sc_handle(); }
jlong SC_nMirrorSurface(JNIEnv*, jclass, jlong) { return sc_handle(); }
jlong SC_nGetTransactionId(JNIEnv*, jclass, jlong) { return 0; }
void SC_nApplyTransaction(JNIEnv*, jclass, jlong, jboolean) {}
void SC_nMergeTransaction(JNIEnv*, jclass, jlong, jlong) {}
void SC_nClearTransaction(JNIEnv*, jclass, jlong) {}
void SC_nSetAnimationTransaction(JNIEnv*, jclass, jlong) {}
void SC_nWriteToParcel(JNIEnv*, jclass, jlong, jobject) {}
void SC_nWriteTransactionToParcel(JNIEnv*, jclass, jlong, jobject) {}
void SC_nRelease(JNIEnv*, jclass, jlong) {}
void SC_nDisconnect(JNIEnv*, jclass, jlong) {}
void SC_nUpdateDefaultBufferSize(JNIEnv*, jclass, jlong, jint, jint) {}
void SC_nReparent(JNIEnv*, jclass, jlong, jlong, jlong) {}
void SC_nSetLayer(JNIEnv*, jclass, jlong, jlong, jint) {}
void SC_nSetRelativeLayer(JNIEnv*, jclass, jlong, jlong, jlong, jint) {}
void SC_nSetPosition(JNIEnv*, jclass, jlong, jlong, jfloat, jfloat) {}
void SC_nSetScale(JNIEnv*, jclass, jlong, jlong, jfloat, jfloat) {}
void SC_nSetSize(JNIEnv*, jclass, jlong, jlong, jint, jint) {}
void SC_nSetAlpha(JNIEnv*, jclass, jlong, jlong, jfloat) {}
void SC_nSetMatrix(JNIEnv*, jclass, jlong, jlong, jfloat, jfloat, jfloat, jfloat) {}
void SC_nSetFlags(JNIEnv*, jclass, jlong, jlong, jint, jint) {}
void SC_nSetWindowCrop(JNIEnv*, jclass, jlong, jlong, jint, jint, jint, jint) {}
void SC_nSetCornerRadius(JNIEnv*, jclass, jlong, jlong, jfloat) {}
void SC_nSetBackgroundBlurRadius(JNIEnv*, jclass, jlong, jlong, jint) {}
void SC_nSetLayerStack(JNIEnv*, jclass, jlong, jlong, jint) {}
void SC_nSetTransparentRegionHint(JNIEnv*, jclass, jlong, jlong, jobject) {}
void SC_nSetTrustedOverlay(JNIEnv*, jclass, jlong, jlong, jboolean) {}
void SC_nSetGeometry(JNIEnv*, jclass, jlong, jlong, jobject, jobject, jlong) {}
void SC_nSetColor(JNIEnv*, jclass, jlong, jlong, jfloatArray) {}
void SC_nSetColorTransform(JNIEnv*, jclass, jlong, jlong, jfloatArray, jfloatArray) {}
void SC_nSetColorSpaceAgnostic(JNIEnv*, jclass, jlong, jlong, jboolean) {}
void SC_nSetFrameRateSelectionPriority(JNIEnv*, jclass, jlong, jlong, jint) {}
void SC_nSetDropInputMode(JNIEnv*, jclass, jlong, jlong, jint) {}

// android.view.SurfaceSession (ViewRootImpl creates one) — no-op create/destroy.
jlong SS_nCreate(JNIEnv*, jclass) { return sc_handle(); }
void SS_nDestroy(JNIEnv*, jclass, jlong) {}
const JNINativeMethod kSurfaceSession[] = {
    {"nativeCreate", "()J", reinterpret_cast<void*>(SS_nCreate)},
    {"nativeDestroy", "(J)V", reinterpret_cast<void*>(SS_nDestroy)},
};

const JNINativeMethod kSurfaceControl[] = {
    {"nativeGetNativeTransactionFinalizer", "()J", reinterpret_cast<void*>(SC_nGetNativeTransactionFinalizer)},
    {"nativeCreateTransaction", "()J", reinterpret_cast<void*>(SC_nCreateTransaction)},
    {"nativeCreate", "(Landroid/view/SurfaceSession;Ljava/lang/String;IIIIJLandroid/os/Parcel;)J", reinterpret_cast<void*>(SC_nCreate)},
    {"nativeReadFromParcel", "(Landroid/os/Parcel;)J", reinterpret_cast<void*>(SC_nReadFromParcel)},
    {"nativeReadTransactionFromParcel", "(Landroid/os/Parcel;)J", reinterpret_cast<void*>(SC_nReadTransactionFromParcel)},
    {"nativeCopyFromSurfaceControl", "(J)J", reinterpret_cast<void*>(SC_nCopyFromSurfaceControl)},
    {"nativeMirrorSurface", "(J)J", reinterpret_cast<void*>(SC_nMirrorSurface)},
    {"nativeGetTransactionId", "(J)J", reinterpret_cast<void*>(SC_nGetTransactionId)},
    {"nativeApplyTransaction", "(JZ)V", reinterpret_cast<void*>(SC_nApplyTransaction)},
    {"nativeMergeTransaction", "(JJ)V", reinterpret_cast<void*>(SC_nMergeTransaction)},
    {"nativeClearTransaction", "(J)V", reinterpret_cast<void*>(SC_nClearTransaction)},
    {"nativeSetAnimationTransaction", "(J)V", reinterpret_cast<void*>(SC_nSetAnimationTransaction)},
    {"nativeWriteToParcel", "(JLandroid/os/Parcel;)V", reinterpret_cast<void*>(SC_nWriteToParcel)},
    {"nativeWriteTransactionToParcel", "(JLandroid/os/Parcel;)V", reinterpret_cast<void*>(SC_nWriteTransactionToParcel)},
    {"nativeRelease", "(J)V", reinterpret_cast<void*>(SC_nRelease)},
    {"nativeDisconnect", "(J)V", reinterpret_cast<void*>(SC_nDisconnect)},
    {"nativeUpdateDefaultBufferSize", "(JII)V", reinterpret_cast<void*>(SC_nUpdateDefaultBufferSize)},
    {"nativeReparent", "(JJJ)V", reinterpret_cast<void*>(SC_nReparent)},
    {"nativeSetLayer", "(JJI)V", reinterpret_cast<void*>(SC_nSetLayer)},
    {"nativeSetRelativeLayer", "(JJJI)V", reinterpret_cast<void*>(SC_nSetRelativeLayer)},
    {"nativeSetPosition", "(JJFF)V", reinterpret_cast<void*>(SC_nSetPosition)},
    {"nativeSetScale", "(JJFF)V", reinterpret_cast<void*>(SC_nSetScale)},
    {"nativeSetSize", "(JJII)V", reinterpret_cast<void*>(SC_nSetSize)},
    {"nativeSetAlpha", "(JJF)V", reinterpret_cast<void*>(SC_nSetAlpha)},
    {"nativeSetMatrix", "(JJFFFF)V", reinterpret_cast<void*>(SC_nSetMatrix)},
    {"nativeSetFlags", "(JJII)V", reinterpret_cast<void*>(SC_nSetFlags)},
    {"nativeSetWindowCrop", "(JJIIII)V", reinterpret_cast<void*>(SC_nSetWindowCrop)},
    {"nativeSetCornerRadius", "(JJF)V", reinterpret_cast<void*>(SC_nSetCornerRadius)},
    {"nativeSetBackgroundBlurRadius", "(JJI)V", reinterpret_cast<void*>(SC_nSetBackgroundBlurRadius)},
    {"nativeSetLayerStack", "(JJI)V", reinterpret_cast<void*>(SC_nSetLayerStack)},
    {"nativeSetTransparentRegionHint", "(JJLandroid/graphics/Region;)V", reinterpret_cast<void*>(SC_nSetTransparentRegionHint)},
    {"nativeSetTrustedOverlay", "(JJZ)V", reinterpret_cast<void*>(SC_nSetTrustedOverlay)},
    {"nativeSetGeometry", "(JJLandroid/graphics/Rect;Landroid/graphics/Rect;J)V", reinterpret_cast<void*>(SC_nSetGeometry)},
    {"nativeSetColor", "(JJ[F)V", reinterpret_cast<void*>(SC_nSetColor)},
    {"nativeSetColorTransform", "(JJ[F[F)V", reinterpret_cast<void*>(SC_nSetColorTransform)},
    {"nativeSetColorSpaceAgnostic", "(JJZ)V", reinterpret_cast<void*>(SC_nSetColorSpaceAgnostic)},
    {"nativeSetFrameRateSelectionPriority", "(JJI)V", reinterpret_cast<void*>(SC_nSetFrameRateSelectionPriority)},
    {"nativeSetDropInputMode", "(JJI)V", reinterpret_cast<void*>(SC_nSetDropInputMode)},
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

// Profiling: bucket counts of non-optimal paths, for prioritising real implementations
// before the final optimal path. Today: RenderNode shim nodes created (ADR-4 hwui-drop).
extern "C" std::uint64_t acl_graphics_profile_rendernodes() {
  return g_rn_nodes.load(std::memory_order_relaxed);
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
  ok = reg(env, "android/graphics/Path", kPath, n(kPath)) && ok;
  ok = reg(env, "android/graphics/ImageDecoder", kImageDecoder, n(kImageDecoder)) && ok;
  ok = reg(env, "android/graphics/NinePatch", kNinePatch, n(kNinePatch)) && ok;
  ok = reg(env, "android/graphics/ColorFilter", kColorFilter, n(kColorFilter)) && ok;
  ok = reg(env, "android/graphics/BlendModeColorFilter", kBlendModeColorFilter,
           n(kBlendModeColorFilter)) && ok;
  ok = reg(env, "android/graphics/drawable/AnimatedVectorDrawable", kAnimatedVectorDrawable,
           n(kAnimatedVectorDrawable)) && ok;
  ok = reg(env, "android/graphics/Matrix", kMatrix, n(kMatrix)) && ok;
  ok = reg(env, "android/graphics/RenderNode", kRenderNode, n(kRenderNode)) && ok;
  ok = reg(env, "android/graphics/Paint", kPaint, n(kPaint)) && ok;
  ok = reg(env, "android/graphics/Shader", kShader, n(kShader)) && ok;
  ok = reg(env, "android/graphics/LinearGradient", kLinearGradient, n(kLinearGradient)) && ok;
  ok = reg(env, "android/graphics/RadialGradient", kRadialGradient, n(kRadialGradient)) && ok;
  ok = reg(env, "android/graphics/ColorSpace$Rgb", kColorSpaceRgb, n(kColorSpaceRgb)) && ok;
  ok = reg(env, "android/graphics/Region", kRegion, n(kRegion)) && ok;
  ok = reg(env, "android/graphics/Typeface", kTypeface, n(kTypeface)) && ok;
  ok = reg(env, "android/graphics/fonts/Font$Builder", kFontBuilder, n(kFontBuilder)) && ok;
  ok = reg(env, "android/graphics/fonts/FontFamily$Builder", kFontFamilyBuilder, n(kFontFamilyBuilder)) && ok;
  ok = reg(env, "android/graphics/fonts/FontFamily", kFontFamily, n(kFontFamily)) && ok;
  ok = reg(env, "android/graphics/fonts/Font", kFont, n(kFont)) && ok;
  ok = reg(env, "android/graphics/Interpolator", kInterpolator, n(kInterpolator)) && ok;
  ok = reg(env, "android/graphics/HardwareRenderer", kHardwareRenderer, n(kHardwareRenderer)) && ok;
  ok = reg(env, "android/view/DisplayEventReceiver", kDisplayEventReceiver, n(kDisplayEventReceiver)) && ok;
  ok = reg(env, "com/android/internal/util/VirtualRefBasePtr", kVirtualRefBasePtr, n(kVirtualRefBasePtr)) && ok;
  ok = reg(env, "android/util/PathParser", kPathParser, n(kPathParser)) && ok;
  ok = reg(env, "android/graphics/drawable/VectorDrawable", kVectorDrawable, n(kVectorDrawable)) && ok;
  // SurfaceControl no-op floor is best-effort — it must not gate the graphics floor (a failure
  // here would skip the font seam + demos in load_acl_graphics). Absent -> dialogs still crash;
  // present -> ViewRootImpl.setView survives. Grown empirically against a real Dialog.
  reg(env, "android/view/SurfaceControl", kSurfaceControl, n(kSurfaceControl));
  reg(env, "android/view/SurfaceSession", kSurfaceSession, n(kSurfaceSession));
  return ok ? JNI_OK : JNI_ERR;
}
