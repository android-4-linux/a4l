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
#include <unordered_map>
#include <string>
#include <vector>

#include "SkBitmap.h"
#include "SkCanvas.h"
#include "SkDrawable.h"
#include "SkPicture.h"
#include "SkPictureRecorder.h"
#include "SkClipOp.h"
#include "SkColor.h"
#include "SkColorSpace.h"
#include "SkData.h"
#include "SkSamplingOptions.h"
#include "SkStream.h"
#include "SkPixmap.h"
#include "SkPngEncoder.h"    // Bitmap.compress -> PNG
#include "SkJpegEncoder.h"   // Bitmap.compress -> JPEG
#include "SkWebpEncoder.h"   // Bitmap.compress -> WEBP
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
#include "SkPathEffect.h"
#include "SkDashPathEffect.h"
#include "SkCornerPathEffect.h"
#include "SkRuntimeEffect.h"
#include "SkImage.h"
#include "SkImageInfo.h"
#include "SkMatrix.h"
#include "SkPaint.h"
#include "SkColorFilter.h"
#include "SkPath.h"
#include "SkPathMeasure.h"
#include "SkPathOps.h"
#include "SkRRect.h"
#include "SkPoint3.h"
#include "utils/SkShadowUtils.h"
#include "SkShadowFlags.h"
#include "SkImageFilter.h"
#include "effects/SkImageFilters.h"
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
// ── RenderNode display list (hwui's live-reference tree model, re-owned on Skia) ──────────────
// hwui records each RenderNode's content as a display list whose child references are LIVE pointers,
// and the RenderThread WALKS the tree every frame — a child's update is picked up with no parent
// re-record. acl's earlier model recorded a node into ONE SkPicture, and drawRenderNode(child)
// became an SkDrawable that SkPictureRecorder SNAPSHOTS at finishRecording — so a parent cached a
// FROZEN copy of the child (the inversion that made settled TextureViews/Compose layers go stale and
// forced a re-record-everything hack). This restores hwui's model: a node's content is a SEQUENCE of
// segments (SkPictures of its own ops) split at child/texture-layer BOUNDARIES that the native walk
// (draw_rendernode_immediate) resolves LIVE at their position. SkPicture playback composes recorded
// setMatrix ops with the playback CTM, so splitting the recording preserves transforms across segment
// boundaries. Clean-room: hwui's semantics (SDK/AOSP-documented RenderNode model); no the predecessor.
struct AclRenderNode;      // self-reference in a child boundary
struct AclTextureLayer;    // TextureView layer boundary (defined with the SurfaceTexture natives)
struct DisplayItem {
  enum class Kind : std::uint8_t { Picture, Child, Layer } kind = Kind::Picture;
  sk_sp<SkPicture> picture;          // Picture: a segment of the node's own recorded ops
  sk_sp<AclRenderNode> child;        // Child: live reference to a child node (resolved at walk)
  AclTextureLayer* layer = nullptr;  // Layer: a TextureView's layer (Java-owned; not reffed here)
  SkMatrix matrix = SkMatrix::I();   // node-local CTM captured at the boundary (Child/Layer)
  bool z_ordered = false;            // Child recorded inside an enableZ region: sort by Z + cast shadow
};

struct AclRenderNode : public SkRefCnt {
  int left = 0, top = 0, right = 0, bottom = 0;
  float alpha = 1.0f, cameraDistance = 0.0f;
  float translationX = 0, translationY = 0, translationZ = 0, elevation = 0;
  float rotation = 0, rotationX = 0, rotationY = 0;
  float scaleX = 1.0f, scaleY = 1.0f;
  float pivotX = 0, pivotY = 0;
  bool pivotExplicit = false;
  int layerType = 0;
  // Shadow colors default to opaque BLACK (Android RenderNode default); the walk multiplies them by
  // the ambient/spot shadow alphas (0.039 / 0.19) when casting an elevation shadow.
  int spotShadowColor = 0xFF000000, ambientShadowColor = 0xFF000000;
  bool clipToBounds = true, clipToOutline = false, allowForceDark = true;
  bool hasOverlappingRendering = true;
  // clipToOutline outline geometry (ViewOutlineProvider): a round-rect (radius 0 == plain rect) or a
  // convex path, in the node's local coords. Applied as a clip by the walk when clipToOutline is set
  // — the reason Material cards / buttons / rounded images have rounded corners instead of squares.
  enum class OutlineKind : std::uint8_t { None, RoundRect, Path } outline_kind = OutlineKind::None;
  SkRect outline_rect = SkRect::MakeEmpty();
  float outline_radius = 0.0f;
  SkPath outline_path;
  // RenderNode.setRenderEffect (Compose Modifier.blur, backdrop blur): an SkImageFilter the walk
  // composites the node's content through (blur / color-filter / offset / chain).
  sk_sp<SkImageFilter> render_effect;
  // RenderNode record/replay (Compose renders its content into RenderNode layers). `display_list`
  // is the segmented content (own-op SkPictures + live child/layer boundaries), walked live each
  // frame by draw_rendernode_immediate; `picture` is a legacy single-picture fallback (deferred
  // present-root path, and leaf nodes with no boundaries). is_present_root marks acl's top-level
  // node whose RecordingCanvas draws immediately onto the GPU SkCanvas (not recorded).
  std::vector<DisplayItem> display_list;
  sk_sp<SkPicture> picture;
  bool is_present_root = false;
};

// ── Native handle guard ──────────────────────────────────────────────────────
// The framework stores every native object as a bare jlong in a Java field, so a native can
// be handed a pointer acl never minted and has no way to tell from the value alone. Two ways
// that happens here, both observed as SIGSEGV:
//   * an ART boot image pre-initializes boot classes and bakes their static state, including
//     native pointers minted inside dex2oat that mean nothing at run time
//     (Typeface_nativeGetStyle -> SkTypeface::fontStyle(), from a preinitialized
//     android.graphics.Typeface.<init>);
//   * a framework path acl does not intercept constructs the Java object itself and leaves a
//     stale or foreign long behind (android.graphics.fonts.Font.getStyle, fault 0x800000008).
// A null check does not catch either: the value is non-zero garbage. So acl records every
// handle it mints and validates membership before dereferencing — an unrecognized handle
// degrades to a documented default instead of taking the process down.
//
// The count field mirrors the object's own ref count: typeface_ref() hands the SAME pointer
// back with an extra Skia ref, so two live Java Typefaces can share one address and the first
// free must not invalidate the handle the second still holds.
enum class HandleKind : std::uint8_t { kTypeface, kFont, kFontFamily };

class HandleTable {
 public:
  static void add(const void* p, HandleKind k) {
    if (p == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex());
    Entry& e = map()[p];
    e.kind = k;
    ++e.count;
  }
  static void forget(const void* p) {
    if (p == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex());
    auto it = map().find(p);
    if (it == map().end()) return;
    if (--it->second.count <= 0) map().erase(it);
  }
  static bool holds(const void* p, HandleKind k) {
    if (p == nullptr) return false;
    std::lock_guard<std::mutex> lock(mutex());
    auto it = map().find(p);
    return it != map().end() && it->second.kind == k;
  }

 private:
  struct Entry { HandleKind kind = HandleKind::kTypeface; int count = 0; };
  static std::mutex& mutex() { static std::mutex m; return m; }
  static std::unordered_map<const void*, Entry>& map() {
    static std::unordered_map<const void*, Entry>* m = new std::unordered_map<const void*, Entry>();
    return *m;  // leaked deliberately: natives can run after static destructors on teardown
  }
};

// Mint: register a freshly produced handle and return it as the jlong the framework stores.
template <class T>
jlong handle_mint(T* p, HandleKind k) {
  HandleTable::add(p, k);
  return reinterpret_cast<jlong>(p);
}

// Consume: nullptr unless this is a live handle acl minted of exactly this kind. Set
// ACL_GRAPHICS_HANDLE_DEBUG=1 to see rejects — a silent default can otherwise hide a real gap
// (a framework path acl should be intercepting but is not).
template <class T>
T* handle_checked(jlong h, HandleKind k, const char* who) {
  auto* p = reinterpret_cast<T*>(static_cast<std::uintptr_t>(h));
  if (HandleTable::holds(p, k)) return p;
  if (h != 0) {
    static const bool debug = [] {
      const char* v = std::getenv("ACL_GRAPHICS_HANDLE_DEBUG");
      return v != nullptr && *v != '\0' && *v != '0';
    }();
    if (debug)
      std::fprintf(stderr, "[acl.graphics] %s: rejected foreign handle %p\n", who,
                   reinterpret_cast<void*>(static_cast<std::uintptr_t>(h)));
  }
  return nullptr;
}

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
  return handle_mint(tf.release(), HandleKind::kTypeface);
}

jclass g_bitmap_class = nullptr;    // global ref, for nativeCreate -> NewObject
jmethodID g_bitmap_ctor = nullptr;

// A GPU-surface-owned SkCanvas (from gpu_dmabuf.cpp's begin_frame) must not be deleted by
// the Java Canvas finalizer — the SkSurface owns it. Defined in gpu_dmabuf.cpp (same .so).
extern "C" bool acl_graphics_canvas_borrowed(void* c);
// A RenderNode recording canvas is owned by its SkPictureRecorder (see the RenderNode record/
// replay block) — the Canvas finalizer must not delete it. Defined below.
bool is_recording_canvas(void* p);

// A software Canvas — Canvas(Bitmap) / Canvas() — can be retargeted to a new Bitmap via
// Canvas.setBitmap (AdaptiveIconDrawable composites into an offscreen Bitmap this way). A plain
// SkCanvas cannot rebind its target, so a raster canvas is a tiny wrapper holding a swappable
// SkCanvas, and its Java handle carries a low tag bit to distinguish it from the raw SkCanvas*
// handles the recording/GPU paths hand out (heap pointers are >=8-byte aligned, so bit 0 is free,
// and a tagged handle never collides with an untagged SkCanvas* used as a g_recorders key or as
// g_present_gpu_canvas). canvas_sk() resolves either kind with one bit test — no lock, no lookup
// on the draw path.
struct AclRasterCanvas {
  std::unique_ptr<SkCanvas> sk;
};
constexpr jlong kRasterTag = 1;
// A RenderNode display-list recording canvas is a NodeRecorder (see the display-list model): its
// handle carries bit 1 so canvas_sk() resolves it to the CURRENT segment's SkCanvas, which changes
// each time the recording is split at a child/layer boundary while the Java-held handle stays stable.
// (bit 0 = raster wrapper, bit 1 = node recorder; heap pointers are >=8-byte aligned so both are free
// and never collide with a raw SkCanvas* handle.)
constexpr jlong kRecorderTag = 2;
inline bool is_raster_handle(jlong h) { return (h & kRasterTag) != 0; }
inline bool is_recorder_handle(jlong h) { return (h & kRecorderTag) != 0; }
inline AclRasterCanvas* raster_of(jlong h) {
  return reinterpret_cast<AclRasterCanvas*>(h & ~kRasterTag);
}
SkCanvas* recorder_seg_canvas(jlong h);  // current segment canvas; defined with NodeRecorder
inline SkCanvas* canvas_sk(jlong h) {
  if (h == 0) return nullptr;
  if (h & (kRasterTag | kRecorderTag)) {
    if (h & kRasterTag) return raster_of(h)->sk.get();
    return recorder_seg_canvas(h);
  }
  return reinterpret_cast<SkCanvas*>(h);
}

// NativeAllocationRegistry finalizers (returned as function pointers to Java).
void free_bitmap(void* p) { delete reinterpret_cast<AclBitmap*>(p); }
void free_paint(void* p) { delete reinterpret_cast<AclPaint*>(p); }
void free_node_recorder(jlong h);  // defined with NodeRecorder
void free_canvas(void* p) {
  const jlong h = reinterpret_cast<jlong>(p);
  if (is_raster_handle(h)) { delete raster_of(h); return; }  // our wrapper owns its SkCanvas
  if (is_recorder_handle(h)) { free_node_recorder(h); return; }  // our NodeRecorder owns its segments
  if (acl_graphics_canvas_borrowed(p)) return;  // surface-owned (GPU); not ours to delete
  if (is_recording_canvas(p)) return;           // owned by its SkPictureRecorder; not ours
  delete reinterpret_cast<SkCanvas*>(p);
}
void free_path(void* p) { delete reinterpret_cast<SkPath*>(p); }
void free_matrix(void* p) { delete reinterpret_cast<SkMatrix*>(p); }
void free_shader(void* p) { SkSafeUnref(reinterpret_cast<SkShader*>(p)); }
// Release Compose's ownership ref. The node survives past this if a parent's recorded picture still
// references it via an AclRenderNodeDrawable (resolve-at-replay), and is freed when the last ref drops.
void free_rendernode(void* p) { SkSafeUnref(reinterpret_cast<AclRenderNode*>(p)); }
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

// Bitmap.getPixels: read a w×h region at (x,y) into the int[] as unpremultiplied ARGB Color ints,
// at `offset` with `stride` ints per row. kN32 (BGRA on LE) unpremul packs as 0xAARRGGBB == the
// Android Color int. Used e.g. to read a downloaded icon's pixels when building a notification.
void Bitmap_nativeGetPixels(JNIEnv* env, jclass, jlong ptr, jintArray pixels, jint offset,
                            jint stride, jint x, jint y, jint width, jint height) {
  auto* w = reinterpret_cast<AclBitmap*>(ptr);
  if (w == nullptr || pixels == nullptr || width <= 0 || height <= 0) return;
  jint* dst = env->GetIntArrayElements(pixels, nullptr);
  if (dst == nullptr) return;
  const SkImageInfo info =
      SkImageInfo::Make(width, height, kN32_SkColorType, kUnpremul_SkAlphaType);
  w->bitmap.readPixels(info, dst + offset, static_cast<size_t>(stride) * sizeof(jint), x, y);
  env->ReleaseIntArrayElements(pixels, dst, 0);  // copy back + release
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

// Bitmap.copy(Config, isMutable) -> nativeCopy: allocate a new SkBitmap in the requested Config and
// convert-copy the source pixels into it. Skia's readPixels does the color-type / alpha / color-
// space conversion (e.g. ARGB_8888 -> RGB_565 or -> ALPHA_8); the common case is a same-config
// deep copy for a mutable working buffer. density + premultiplied are re-applied by Bitmap.copy()
// in Java on the returned object, so density=0 here matches nativeCreate.
// Bitmap.nativeComputeColorSpace(nativeBitmap) -> android.graphics.ColorSpace. acl's bitmaps are
// sRGB; return ColorSpace.get(ColorSpace.Named.SRGB) rather than reconstructing one from the
// SkColorSpace. Telegram computes this on its theme-loading thread; the missing native killed that
// thread and stalled the UI. Returns null on any resolution failure (Bitmap.getColorSpace tolerates
// a null from a recycled/odd bitmap).
jobject Bitmap_nativeComputeColorSpace(JNIEnv* env, jclass, jlong /*bitmapHandle*/) {
  jclass csCls = env->FindClass("android/graphics/ColorSpace");
  jclass namedCls = env->FindClass("android/graphics/ColorSpace$Named");
  if (csCls == nullptr || namedCls == nullptr) { env->ExceptionClear(); return nullptr; }
  jfieldID srgbF = env->GetStaticFieldID(namedCls, "SRGB", "Landroid/graphics/ColorSpace$Named;");
  jmethodID getM = env->GetStaticMethodID(
      csCls, "get", "(Landroid/graphics/ColorSpace$Named;)Landroid/graphics/ColorSpace;");
  if (srgbF == nullptr || getM == nullptr) { env->ExceptionClear(); return nullptr; }
  jobject srgb = env->GetStaticObjectField(namedCls, srgbF);
  jobject result = env->CallStaticObjectMethod(csCls, getM, srgb);
  if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
  return result;
}
jobject Bitmap_nativeCopy(JNIEnv* env, jclass, jlong srcPtr, jint dstConfig, jboolean isMutable) {
  auto* src = reinterpret_cast<AclBitmap*>(srcPtr);
  if (src == nullptr) return nullptr;
  const int width = src->bitmap.width();
  const int height = src->bitmap.height();
  const SkColorType ct = config_to_ct(dstConfig);
  // Coerce the alpha type to one the destination color type allows (565 is always opaque; a
  // single-channel ALPHA_8 can't be opaque) — otherwise SkImageInfo/tryAllocPixels rejects it.
  SkAlphaType at = src->bitmap.alphaType();
  if (ct == kRGB_565_SkColorType) at = kOpaque_SkAlphaType;
  else if (ct == kAlpha_8_SkColorType && at == kOpaque_SkAlphaType) at = kPremul_SkAlphaType;
  auto* w = new AclBitmap();
  sk_sp<SkColorSpace> cs = src->bitmap.refColorSpace();
  const SkImageInfo info =
      SkImageInfo::Make(width, height, ct, at, cs ? cs : SkColorSpace::MakeSRGB());
  if (!w->bitmap.tryAllocPixels(info)) {
    delete w;
    return nullptr;
  }
  if (!src->bitmap.readPixels(w->bitmap.pixmap(), 0, 0))
    w->bitmap.eraseColor(SK_ColorTRANSPARENT);  // unsupported conversion -> transparent, not a crash
  w->immutable = (isMutable == JNI_FALSE);
  return env->NewObject(g_bitmap_class, g_bitmap_ctor, reinterpret_cast<jlong>(w), width,
                        height, /*density=*/0, /*requestPremultiplied=*/JNI_TRUE,
                        /*ninePatchChunk=*/static_cast<jbyteArray>(nullptr),
                        /*ninePatchInsets=*/static_cast<jobject>(nullptr));
}

// Bitmap.compress(format, quality, OutputStream): encode the SkBitmap (PNG/JPEG/WEBP via the
// linked Skia encoders) and write the bytes to the Java OutputStream. CompressFormat ordinals:
// 0=JPEG, 1=PNG, 2=WEBP, 3=WEBP_LOSSY, 4=WEBP_LOSSLESS.
jboolean Bitmap_nativeCompress(JNIEnv* env, jclass, jlong ptr, jint format, jint quality,
                               jobject stream, jbyteArray /*tempStorage*/) {
  auto* w = reinterpret_cast<AclBitmap*>(ptr);
  if (w == nullptr || stream == nullptr) return JNI_FALSE;
  SkPixmap pm;
  if (!w->bitmap.peekPixels(&pm)) return JNI_FALSE;
  SkDynamicMemoryWStream out;
  bool ok = false;
  switch (format) {
    case 0: {  // JPEG
      SkJpegEncoder::Options o;
      o.fQuality = quality;
      ok = SkJpegEncoder::Encode(&out, pm, o);
      break;
    }
    case 2:
    case 3:
    case 4: {  // WEBP (lossy / lossless)
      SkWebpEncoder::Options o;
      o.fCompression = (format == 4) ? SkWebpEncoder::Compression::kLossless
                                     : SkWebpEncoder::Compression::kLossy;
      o.fQuality = static_cast<float>(quality);
      ok = SkWebpEncoder::Encode(&out, pm, o);
      break;
    }
    case 1:
    default: {  // PNG (lossless; quality ignored)
      SkPngEncoder::Options o;
      ok = SkPngEncoder::Encode(&out, pm, o);
      break;
    }
  }
  if (!ok) return JNI_FALSE;
  sk_sp<SkData> data = out.detachAsData();
  const jsize n = static_cast<jsize>(data->size());
  jbyteArray arr = env->NewByteArray(n);
  if (arr == nullptr) return JNI_FALSE;
  env->SetByteArrayRegion(arr, 0, n, reinterpret_cast<const jbyte*>(data->data()));
  jclass os = env->FindClass("java/io/OutputStream");
  jmethodID write = os != nullptr ? env->GetMethodID(os, "write", "([BII)V") : nullptr;
  if (write != nullptr) env->CallVoidMethod(stream, write, arr, 0, n);
  env->DeleteLocalRef(arr);
  if (os != nullptr) env->DeleteLocalRef(os);
  return env->ExceptionCheck() ? JNI_FALSE : JNI_TRUE;
}

jboolean Bitmap_nativeIsPremultiplied(JNIEnv*, jclass, jlong ptr) {
  auto* w = reinterpret_cast<AclBitmap*>(ptr);
  return (w != nullptr && w->bitmap.alphaType() == kPremul_SkAlphaType) ? JNI_TRUE : JNI_FALSE;
}
jboolean Bitmap_nativeHasAlpha(JNIEnv*, jclass, jlong ptr) {
  auto* w = reinterpret_cast<AclBitmap*>(ptr);
  return (w != nullptr && !w->bitmap.isOpaque()) ? JNI_TRUE : JNI_FALSE;
}
// acl's SkBitmaps carry no mipmap chain (a hint set via Bitmap.setHasMipMap), so always false.
jboolean Bitmap_nativeHasMipMap(JNIEnv*, jclass, jlong) { return JNI_FALSE; }

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
jboolean Bitmap_nativeIsBackedByAshmem(jlong) { return JNI_FALSE; }  // @CriticalNative

// Bitmap lifecycle natives Compose's VectorPainter/DrawCache exercises (draws each vector into a
// cache Bitmap, then draws that). prepareToDraw is a GPU-upload hint (the draw path uploads
// anyway); generationId lets Compose detect pixel changes (SkBitmap tracks it); reconfigure resizes
// the cache. All normal JNI (env/jclass).
void Bitmap_nativePrepareToDraw(JNIEnv*, jclass, jlong) {}
jint Bitmap_nativeGenerationId(JNIEnv*, jclass, jlong ptr) {
  auto* w = reinterpret_cast<AclBitmap*>(ptr);
  return w ? static_cast<jint>(w->bitmap.getGenerationID()) : 0;
}
jint Bitmap_nativeRowBytes(JNIEnv*, jclass, jlong ptr) {
  auto* w = reinterpret_cast<AclBitmap*>(ptr);
  return w ? static_cast<jint>(w->bitmap.rowBytes()) : 0;
}
void Bitmap_nativeSetImmutable(JNIEnv*, jclass, jlong ptr) {
  if (auto* w = reinterpret_cast<AclBitmap*>(ptr)) w->immutable = true;
}
void Bitmap_nativeSetHasMipMap(JNIEnv*, jclass, jlong, jboolean) {}
void Bitmap_nativeSetPremultiplied(JNIEnv*, jclass, jlong, jboolean) {}
void Bitmap_nativeSetColorSpace(JNIEnv*, jclass, jlong, jlong) {}
void Bitmap_nativeRecycle(JNIEnv*, jclass, jlong ptr) {
  if (auto* w = reinterpret_cast<AclBitmap*>(ptr)) w->bitmap.reset();
}
void Bitmap_nativeReconfigure(JNIEnv*, jclass, jlong ptr, jint w, jint h, jint config, jboolean) {
  auto* b = reinterpret_cast<AclBitmap*>(ptr);
  if (b == nullptr) return;
  SkColorType ct = kN32_SkColorType;
  if (config == 1) ct = kAlpha_8_SkColorType;
  else if (config == 3) ct = kRGB_565_SkColorType;
  b->bitmap.allocPixels(SkImageInfo::Make(w, h, ct, kPremul_SkAlphaType));
}

// ── Canvas / BaseCanvas ─────────────────────────────────────────────────────────
jlong Canvas_nInitRaster(JNIEnv*, jclass, jlong bitmapHandle) {  // @FastNative (normal sig)
  auto* w = reinterpret_cast<AclBitmap*>(bitmapHandle);
  auto* rc = new AclRasterCanvas();
  rc->sk.reset(w != nullptr ? new SkCanvas(w->bitmap) : new SkCanvas());
  return reinterpret_cast<jlong>(rc) | kRasterTag;
}

// Canvas.setBitmap(bitmap): retarget a software canvas onto a new Bitmap (or an empty one when
// null), resetting matrix/clip — which falls out of building a fresh SkCanvas. Only raster canvases
// retarget; a recording/GPU handle here would be a caller error, so ignore it rather than corrupt
// the present canvas.
void Canvas_nSetBitmap(JNIEnv*, jclass, jlong canvas, jlong bitmapHandle) {
  if (!is_raster_handle(canvas)) return;
  auto* w = reinterpret_cast<AclBitmap*>(bitmapHandle);
  raster_of(canvas)->sk.reset(w != nullptr ? new SkCanvas(w->bitmap) : new SkCanvas());
}

jlong Canvas_nGetNativeFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&free_canvas);
}

// DIAG (Compose draw-path investigation): count Canvas draw ops so we can tell whether a
// frame's content actually reaches the Skia canvas (vs a blank/white frame). acl_graphics_
// profile_drawops() reads it; jvm_vm logs the per-frame delta.
std::atomic<std::uint64_t> g_draw_ops{0};
inline void tick_draw() { g_draw_ops.fetch_add(1, std::memory_order_relaxed); }

void Canvas_nDrawColor(JNIEnv*, jclass, jlong canvas, jint color, jint /*mode*/) {
  tick_draw();
  auto* c = canvas_sk(canvas);
  if (c != nullptr) c->drawColor(static_cast<SkColor>(color), SkBlendMode::kSrcOver);
}

void Canvas_nDrawRect(JNIEnv*, jclass, jlong canvas, jfloat l, jfloat t, jfloat r, jfloat b,
                      jlong paint) {
  tick_draw();
  auto* c = canvas_sk(canvas);
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  SkPaint* p = ap != nullptr ? &ap->paint : nullptr;
  if (c != nullptr) c->drawRect(SkRect::MakeLTRB(l, t, r, b), p ? *p : SkPaint());
}

void Canvas_nDrawLine(JNIEnv*, jclass, jlong canvas, jfloat x0, jfloat y0, jfloat x1, jfloat y1,
                      jlong paint) {
  tick_draw();
  auto* c = canvas_sk(canvas);
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  SkPaint* p = ap != nullptr ? &ap->paint : nullptr;
  if (c != nullptr) c->drawLine(x0, y0, x1, y1, p ? *p : SkPaint());
}

void Canvas_nDrawOval(JNIEnv*, jclass, jlong canvas, jfloat l, jfloat t, jfloat r, jfloat b,
                      jlong paint) {
  tick_draw();
  auto* c = canvas_sk(canvas);
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  SkPaint* p = ap != nullptr ? &ap->paint : nullptr;
  if (c != nullptr) c->drawOval(SkRect::MakeLTRB(l, t, r, b), p ? *p : SkPaint());
}

void Canvas_nDrawCircle(JNIEnv*, jclass, jlong canvas, jfloat cx, jfloat cy, jfloat radius,
                        jlong paint) {
  tick_draw();
  auto* c = canvas_sk(canvas);
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  SkPaint* p = ap != nullptr ? &ap->paint : nullptr;
  if (c != nullptr) c->drawCircle(cx, cy, radius, p ? *p : SkPaint());
}

void Canvas_nDrawArc(JNIEnv*, jclass, jlong canvas, jfloat l, jfloat t, jfloat r, jfloat b,
                     jfloat startAngle, jfloat sweepAngle, jboolean useCenter, jlong paint) {
  tick_draw();
  auto* c = canvas_sk(canvas);
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  SkPaint* p = ap != nullptr ? &ap->paint : nullptr;
  if (c != nullptr) {
    c->drawArc(SkRect::MakeLTRB(l, t, r, b), startAngle, sweepAngle, useCenter, p ? *p : SkPaint());
  }
}

void Canvas_nDrawRoundRect(JNIEnv*, jclass, jlong canvas, jfloat l, jfloat t, jfloat r, jfloat b,
                           jfloat rx, jfloat ry, jlong paint) {
  tick_draw();
  auto* c = canvas_sk(canvas);
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
  tick_draw();
  if (text == nullptr) return;
  const jchar* chars = env->GetStringChars(text, nullptr);
  if (chars != nullptr) {
    draw_utf16(canvas_sk(canvas), reinterpret_cast<AclPaint*>(paint),
               chars, env->GetStringLength(text), start, end - start, x, y);
    env->ReleaseStringChars(text, chars);
  }
}
// nDrawText(char[], index, count)
void Canvas_nDrawTextChars(JNIEnv* env, jclass, jlong canvas, jcharArray text, jint index,
                           jint count, jfloat x, jfloat y, jint /*flags*/, jlong paint) {
  tick_draw();
  if (text == nullptr) return;
  jchar* chars = env->GetCharArrayElements(text, nullptr);
  if (chars != nullptr) {
    draw_utf16(canvas_sk(canvas), reinterpret_cast<AclPaint*>(paint),
               chars, env->GetArrayLength(text), index, count, x, y);
    env->ReleaseCharArrayElements(text, chars, JNI_ABORT);
  }
}
// nDrawTextRun(String, start, end, contextStart, contextEnd, x, y, isRtl, paint)
void Canvas_nDrawTextRunString(JNIEnv* env, jclass, jlong canvas, jstring text, jint start, jint end,
                               jint, jint, jfloat x, jfloat y, jboolean, jlong paint) {
  tick_draw();
  if (text == nullptr) return;
  const jchar* chars = env->GetStringChars(text, nullptr);
  if (chars != nullptr) {
    draw_utf16(canvas_sk(canvas), reinterpret_cast<AclPaint*>(paint),
               chars, env->GetStringLength(text), start, end - start, x, y);
    env->ReleaseStringChars(text, chars);
  }
}
// nDrawTextRun(char[], start, count, contextStart, contextCount, x, y, isRtl, paint, precomputed)
void Canvas_nDrawTextRunChars(JNIEnv* env, jclass, jlong canvas, jcharArray text, jint start,
                              jint count, jint, jint, jfloat x, jfloat y, jboolean, jlong paint,
                              jlong) {
  tick_draw();
  if (text == nullptr) return;
  jchar* chars = env->GetCharArrayElements(text, nullptr);
  if (chars != nullptr) {
    draw_utf16(canvas_sk(canvas), reinterpret_cast<AclPaint*>(paint),
               chars, env->GetArrayLength(text), start, count, x, y);
    env->ReleaseCharArrayElements(text, chars, JNI_ABORT);
  }
}
// nDrawGlyphs(glyphIds, positions, glyphIdStart, positionStart, glyphCount, font, paint):
// pre-shaped glyphs (PrecomputedText path). Positions are absolute (x,y) pairs.
void Canvas_nDrawGlyphs(JNIEnv* env, jclass, jlong canvas, jintArray glyphIds, jfloatArray positions,
                        jint glyphIdStart, jint positionStart, jint glyphCount, jlong /*font*/,
                        jlong paint) {
  tick_draw();
  auto* c = canvas_sk(canvas);
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
  tick_draw();
  auto* c = canvas_sk(canvas);
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
  tick_draw();
  auto* c = canvas_sk(canvas);
  auto* b = reinterpret_cast<AclBitmap*>(bitmapHandle);
  if (c == nullptr || b == nullptr) return;
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  if (sk_sp<SkImage> img = SkImage::MakeFromBitmap(b->bitmap))
    c->drawImageRect(img.get(), SkRect::MakeLTRB(sl, st, sr, sb), SkRect::MakeLTRB(dl, dt, dr, db),
                     SkSamplingOptions(), ap != nullptr ? &ap->paint : nullptr,
                     SkCanvas::kStrict_SrcRectConstraint);
}

void Canvas_nDrawPath(JNIEnv*, jclass, jlong canvas, jlong path, jlong paint) {
  tick_draw();
  auto* c = canvas_sk(canvas);
  auto* sp = reinterpret_cast<SkPath*>(path);
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  if (c != nullptr && sp != nullptr) {
    // ACL_DRAWPATH_DIAG: for a "presents but black" app (a custom View that draws its content as a
    // path — S.N.A.K.E's snake, many games), log each drawPath's path bounds (local + device),
    // verb count, and the paint's color/style/stroke, plus whether the clip is empty. Absence of
    // ANY line means the app's draw code never ran (content not produced — game not started / no
    // input), which is a different problem from an off-screen or wrong-colored draw.
    if (getenv("ACL_DRAWPATH_DIAG") != nullptr) {
      const SkRect bb = sp->getBounds();
      const SkPaint& p = ap != nullptr ? ap->paint : SkPaint();
      const SkRect dev = c->getLocalToDeviceAs3x3().mapRect(bb);
      __android_log_print(4, "acl.drawpath",
                          "path bounds [%.1f,%.1f %.1fx%.1f] dev [%.1f,%.1f %.1fx%.1f] verbs=%d "
                          "color=%08x style=%d strokeW=%.1f clipEmpty=%d",
                          bb.fLeft, bb.fTop, bb.width(), bb.height(), dev.fLeft, dev.fTop,
                          dev.width(), dev.height(), sp->countVerbs(), p.getColor(),
                          static_cast<int>(p.getStyle()), p.getStrokeWidth(), c->isClipEmpty());
    }
    c->drawPath(*sp, ap != nullptr ? ap->paint : SkPaint());
  }
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
// Relative + arc + compound path ops — Compose vector graphics (Material icons, shapes, clip paths)
// build geometry through these. All normal JNI (env/jclass) except the two boolean queries below.
void Path_nRMoveTo(JNIEnv*, jclass, jlong p, jfloat dx, jfloat dy) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->rMoveTo(dx, dy);
}
void Path_nRLineTo(JNIEnv*, jclass, jlong p, jfloat dx, jfloat dy) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->rLineTo(dx, dy);
}
void Path_nRQuadTo(JNIEnv*, jclass, jlong p, jfloat dx1, jfloat dy1, jfloat dx2, jfloat dy2) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->rQuadTo(dx1, dy1, dx2, dy2);
}
void Path_nRCubicTo(JNIEnv*, jclass, jlong p, jfloat x1, jfloat y1, jfloat x2, jfloat y2, jfloat x3, jfloat y3) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->rCubicTo(x1, y1, x2, y2, x3, y3);
}
void Path_nArcTo(JNIEnv*, jclass, jlong p, jfloat l, jfloat t, jfloat r, jfloat b, jfloat start,
                 jfloat sweep, jboolean forceMoveTo) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->arcTo(SkRect::MakeLTRB(l, t, r, b), start, sweep, forceMoveTo);
}
void Path_nAddArc(JNIEnv*, jclass, jlong p, jfloat l, jfloat t, jfloat r, jfloat b, jfloat start, jfloat sweep) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->addArc(SkRect::MakeLTRB(l, t, r, b), start, sweep);
}
void Path_nAddRoundRect8(JNIEnv* env, jclass, jlong p, jfloat l, jfloat t, jfloat r, jfloat b,
                         jfloatArray radii, jint dir) {
  auto* s = reinterpret_cast<SkPath*>(p);
  if (s == nullptr || radii == nullptr) return;
  jfloat rad[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  const jsize n = env->GetArrayLength(radii);
  env->GetFloatArrayRegion(radii, 0, n < 8 ? n : 8, rad);
  const SkVector v[4] = {{rad[0], rad[1]}, {rad[2], rad[3]}, {rad[4], rad[5]}, {rad[6], rad[7]}};
  SkRRect rr;
  rr.setRectRadii(SkRect::MakeLTRB(l, t, r, b), v);
  s->addRRect(rr, static_cast<SkPathDirection>(dir));
}
void Path_nAddPath(JNIEnv*, jclass, jlong p, jlong src) {
  auto* s = reinterpret_cast<SkPath*>(p);
  auto* q = reinterpret_cast<SkPath*>(src);
  if (s && q) s->addPath(*q);
}
void Path_nAddPathOffset(JNIEnv*, jclass, jlong p, jlong src, jfloat dx, jfloat dy) {
  auto* s = reinterpret_cast<SkPath*>(p);
  auto* q = reinterpret_cast<SkPath*>(src);
  if (s && q) s->addPath(*q, dx, dy);
}
void Path_nAddPathMatrix(JNIEnv*, jclass, jlong p, jlong src, jlong m) {
  auto* s = reinterpret_cast<SkPath*>(p);
  auto* q = reinterpret_cast<SkPath*>(src);
  auto* mat = reinterpret_cast<SkMatrix*>(m);
  if (s && q && mat) s->addPath(*q, *mat);
}
void Path_nOffset(JNIEnv*, jclass, jlong p, jfloat dx, jfloat dy) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->offset(dx, dy);
}
void Path_nSet(JNIEnv*, jclass, jlong p, jlong src) {
  auto* s = reinterpret_cast<SkPath*>(p);
  auto* q = reinterpret_cast<SkPath*>(src);
  if (s && q) *s = *q;
}
void Path_nTransform(JNIEnv*, jclass, jlong p, jlong m) {
  auto* s = reinterpret_cast<SkPath*>(p);
  auto* mat = reinterpret_cast<SkMatrix*>(m);
  if (s && mat) s->transform(*mat);
}
void Path_nTransformDst(JNIEnv*, jclass, jlong p, jlong m, jlong dst) {
  auto* s = reinterpret_cast<SkPath*>(p);
  auto* mat = reinterpret_cast<SkMatrix*>(m);
  auto* d = reinterpret_cast<SkPath*>(dst);
  if (s && mat && d) s->transform(*mat, d);
}
void Path_nSetLastPoint(JNIEnv*, jclass, jlong p, jfloat x, jfloat y) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->setLastPt(x, y);
}
void Path_nIncReserve(JNIEnv*, jclass, jlong p, jint n) {
  if (auto* s = reinterpret_cast<SkPath*>(p)) s->incReserve(static_cast<int>(n));
}
void Path_nComputeBounds(JNIEnv* env, jclass, jlong p, jobject rectf) {
  if (rectf == nullptr) return;
  auto* s = reinterpret_cast<SkPath*>(p);
  const SkRect b = s ? s->computeTightBounds() : SkRect::MakeEmpty();
  jclass c = env->GetObjectClass(rectf);
  env->SetFloatField(rectf, env->GetFieldID(c, "left", "F"), b.fLeft);
  env->SetFloatField(rectf, env->GetFieldID(c, "top", "F"), b.fTop);
  env->SetFloatField(rectf, env->GetFieldID(c, "right", "F"), b.fRight);
  env->SetFloatField(rectf, env->GetFieldID(c, "bottom", "F"), b.fBottom);
}
jboolean Path_nIsRect(JNIEnv* env, jclass, jlong p, jobject rectf) {
  auto* s = reinterpret_cast<SkPath*>(p);
  if (s == nullptr) return JNI_FALSE;
  SkRect r;
  const bool isr = s->isRect(&r);
  if (isr && rectf != nullptr) {
    jclass c = env->GetObjectClass(rectf);
    env->SetFloatField(rectf, env->GetFieldID(c, "left", "F"), r.fLeft);
    env->SetFloatField(rectf, env->GetFieldID(c, "top", "F"), r.fTop);
    env->SetFloatField(rectf, env->GetFieldID(c, "right", "F"), r.fRight);
    env->SetFloatField(rectf, env->GetFieldID(c, "bottom", "F"), r.fBottom);
  }
  return isr ? JNI_TRUE : JNI_FALSE;
}
// Path.Op enum matches SkPathOp 1:1 (DIFFERENCE/INTERSECT/UNION/XOR/REVERSE_DIFFERENCE = 0..4).
jboolean Path_nOp(JNIEnv*, jclass, jlong p1, jlong p2, jint op, jlong result) {
  auto* a = reinterpret_cast<SkPath*>(p1);
  auto* b = reinterpret_cast<SkPath*>(p2);
  auto* out = reinterpret_cast<SkPath*>(result);
  return (a && b && out && Op(*a, *b, static_cast<SkPathOp>(op), out)) ? JNI_TRUE : JNI_FALSE;
}
// @CriticalNative — handle-only.
jboolean Path_nIsEmpty(jlong p) { auto* s = reinterpret_cast<SkPath*>(p); return (s && s->isEmpty()) ? JNI_TRUE : JNI_FALSE; }
jboolean Path_nIsConvex(jlong p) { auto* s = reinterpret_cast<SkPath*>(p); return (s && s->isConvex()) ? JNI_TRUE : JNI_FALSE; }
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
jint Path_nGetFillType(jlong p) {
  auto* s = reinterpret_cast<SkPath*>(p);
  return s ? static_cast<jint>(s->getFillType()) : 0;  // Path.FillType.nativeInt == SkPathFillType
}

// ── Canvas transform/clip — all @CriticalNative (handle-only, no JNIEnv/jclass) ──
jint Canvas_nSave(jlong canvas, jint /*saveFlags*/) {
  auto* c = canvas_sk(canvas);
  return c != nullptr ? c->save() : 0;
}
// Canvas.saveLayerAlpha — an offscreen layer composited back at `alpha` (fades/cross-fades,
// Compose graphicsLayer alpha). @CriticalNative (handle-only, no env). Returns the save count.
jint Canvas_nSaveLayerAlpha(jlong canvas, jfloat l, jfloat t, jfloat r, jfloat b, jint alpha) {
  auto* c = canvas_sk(canvas);
  if (c == nullptr) return 0;
  const SkRect bounds = SkRect::MakeLTRB(l, t, r, b);
  return c->saveLayerAlpha(&bounds, static_cast<U8CPU>(alpha & 0xff));
}
jboolean Canvas_nRestore(jlong canvas) {
  auto* c = canvas_sk(canvas);
  if (c == nullptr || c->getSaveCount() <= 1) return JNI_FALSE;
  c->restore();
  return JNI_TRUE;
}
void Canvas_nRestoreToCount(jlong canvas, jint saveCount) {
  auto* c = canvas_sk(canvas);
  if (c != nullptr) c->restoreToCount(saveCount);
}
jint Canvas_nGetSaveCount(jlong canvas) {
  auto* c = canvas_sk(canvas);
  return c != nullptr ? c->getSaveCount() : 0;
}
jint Canvas_nGetWidth(jlong canvas) {  // @CriticalNative
  auto* c = canvas_sk(canvas);
  return c != nullptr ? c->getBaseLayerSize().width() : 0;
}
jint Canvas_nGetHeight(jlong canvas) {  // @CriticalNative
  auto* c = canvas_sk(canvas);
  return c != nullptr ? c->getBaseLayerSize().height() : 0;
}
void Canvas_nTranslate(jlong canvas, jfloat dx, jfloat dy) {
  auto* c = canvas_sk(canvas);
  if (c != nullptr) c->translate(dx, dy);
}
void Canvas_nScale(jlong canvas, jfloat sx, jfloat sy) {
  auto* c = canvas_sk(canvas);
  if (c != nullptr) c->scale(sx, sy);
}
void Canvas_nRotate(jlong canvas, jfloat degrees) {
  auto* c = canvas_sk(canvas);
  if (c != nullptr) c->rotate(degrees);
}
// Region.Op nativeInt: 0=DIFFERENCE, 1=INTERSECT (default clipRect), others map to intersect
// (modern Skia only has Intersect/Difference). Returns whether the clip is non-empty.
jboolean Canvas_nClipRect(jlong canvas, jfloat l, jfloat t, jfloat r, jfloat b, jint regionOp) {
  auto* c = canvas_sk(canvas);
  if (c == nullptr) return JNI_FALSE;
  c->clipRect(SkRect::MakeLTRB(l, t, r, b),
              regionOp == 0 ? SkClipOp::kDifference : SkClipOp::kIntersect, /*doAA=*/false);
  return c->isClipEmpty() ? JNI_FALSE : JNI_TRUE;
}
// @CriticalNative — clip to a Path. Same Region.Op mapping as nClipRect above. Antialiased,
// unlike the rect clip: a path clip is usually a curve (a rounded-corner background, a shaped
// container), and AOSP's SkiaCanvas::clipPath passes doAA=true for exactly that reason — without
// it every such edge is visibly stair-stepped.
//
// Real apps reach this early: SVS.pdfinspector's view tree hit it inside
// DecorView.draw -> ViewGroup.dispatchDraw -> View.draw, and an unimplemented native there does
// not just skip a clip — it throws UnsatisfiedLinkError out of the middle of the draw.
jboolean Canvas_nClipPath(jlong canvas, jlong path, jint regionOp) {
  auto* c = canvas_sk(canvas);
  auto* p = reinterpret_cast<SkPath*>(path);
  if (c == nullptr || p == nullptr) return JNI_FALSE;
  c->clipPath(*p, regionOp == 0 ? SkClipOp::kDifference : SkClipOp::kIntersect, /*doAA=*/true);
  return c->isClipEmpty() ? JNI_FALSE : JNI_TRUE;
}
// nGetClipBounds(canvas, Rect): the current clip's local-space integer bounds (false if empty).
// Regular native (takes a Rect object), unlike the @CriticalNative clip ops above.
jboolean Canvas_nGetClipBounds(JNIEnv* env, jclass, jlong canvas, jobject bounds) {
  auto* c = canvas_sk(canvas);
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
  auto* c = canvas_sk(canvas);
  auto* m = reinterpret_cast<SkMatrix*>(matrix);
  if (c != nullptr && m != nullptr) c->concat(*m);
}
// @CriticalNative — replace the CTM with the Matrix (matrix==0 → identity).
void Canvas_nSetMatrix(jlong canvas, jlong matrix) {
  auto* c = canvas_sk(canvas);
  if (c == nullptr) return;
  c->resetMatrix();
  if (auto* m = reinterpret_cast<SkMatrix*>(matrix)) c->concat(*m);
}
// @CriticalNative — read the CTM into the Matrix.
void Canvas_nGetMatrix(jlong canvas, jlong matrix) {
  auto* c = canvas_sk(canvas);
  auto* m = reinterpret_cast<SkMatrix*>(matrix);
  if (c != nullptr && m != nullptr) *m = c->getTotalMatrix();
}
// @CriticalNative — true if the rect/path is wholly outside the current clip (cull hint).
// ViewGroup.drawChild calls this per child.
jboolean Canvas_nQuickRejectRect(jlong canvas, jfloat l, jfloat t, jfloat r, jfloat b) {
  auto* c = canvas_sk(canvas);
  return (c != nullptr && c->quickReject(SkRect::MakeLTRB(l, t, r, b))) ? JNI_TRUE : JNI_FALSE;
}
jboolean Canvas_nQuickRejectPath(jlong canvas, jlong path) {
  auto* c = canvas_sk(canvas);
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

// BitmapShader.nativeCreate is a STATIC native: an image shader over the bitmap's pixels, tiled per
// axis. AdaptiveIconDrawable masks its layers through one of these. filterFromPaint picks bilinear
// vs nearest sampling; isDirectSampled is a HWUI texture-atlas hint acl doesn't need.
jlong BitmapShader_nativeCreate(JNIEnv*, jclass, jlong matrix, jlong bitmapHandle, jint tileX,
                                jint tileY, jboolean filter, jboolean /*isDirectSampled*/) {
  auto* b = reinterpret_cast<AclBitmap*>(bitmapHandle);
  if (b == nullptr) return 0;
  const SkSamplingOptions sampling(filter ? SkFilterMode::kLinear : SkFilterMode::kNearest);
  sk_sp<SkShader> sh = b->bitmap.makeShader(tile_mode(tileX), tile_mode(tileY), sampling,
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

jlong RenderNode_nCreate(JNIEnv* env, jclass, jstring name) {
  g_rn_nodes.fetch_add(1, std::memory_order_relaxed);
  auto* n = new AclRenderNode();
  // acl's top-level present node: its RecordingCanvas draws immediately onto the GPU SkCanvas.
  if (name != nullptr) {
    const char* s = env->GetStringUTFChars(name, nullptr);
    if (s != nullptr) {
      if (std::strcmp(s, "acl$present$root") == 0) n->is_present_root = true;
      env->ReleaseStringUTFChars(name, s);
    }
  }
  return reinterpret_cast<jlong>(n);
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

// ── Scene-change generation counter (damage/dirty foundation) ──────────────────────────────────
// A monotonically increasing count bumped whenever something that affects the rendered frame
// changes: a RenderNode property, a re-record (nFinishRecording), or a produced texture frame. It
// lets the present loop tell an unchanged (idle) scene from a changed one — the prerequisite for
// skipping redundant redraws. Exposed to the runtime via dlsym. Only bumps on an ACTUAL change (the
// setters compare first), so re-setting a property to its current value doesn't dirty the frame.
std::atomic<std::uint64_t> g_scene_gen{0};
inline void bump_scene() { g_scene_gen.fetch_add(1, std::memory_order_relaxed); }
extern "C" std::uint64_t acl_graphics_scene_generation() {
  return g_scene_gen.load(std::memory_order_relaxed);
}

// @CriticalNative (handle-only). Macros for the uniform get/set forms; setters bump the scene
// generation only when the value actually changes (so an idle frame that re-sets the same values is
// not counted dirty).
#define RN_SETF(fn, field) \
  jboolean fn(jlong h, jfloat v) { auto* n = rnode(h); if (n && n->field != v) { n->field = v; bump_scene(); } return JNI_TRUE; }
#define RN_GETF(fn, field) \
  jfloat fn(jlong h) { auto* n = rnode(h); return n ? n->field : 0.0f; }
#define RN_SETI(fn, field) \
  jboolean fn(jlong h, jint v) { auto* n = rnode(h); if (n && n->field != v) { n->field = v; bump_scene(); } return JNI_TRUE; }
#define RN_GETI(fn, field) \
  jint fn(jlong h) { auto* n = rnode(h); return n ? n->field : 0; }
#define RN_SETB(fn, field) \
  jboolean fn(jlong h, jboolean v) { auto* n = rnode(h); const bool b = (v != JNI_FALSE); if (n && n->field != b) { n->field = b; bump_scene(); } return JNI_TRUE; }
#define RN_GETB(fn, field) \
  jboolean fn(jlong h) { auto* n = rnode(h); return (n && n->field) ? JNI_TRUE : JNI_FALSE; }
#define RN_NOPB(fn) jboolean fn(jlong) { return JNI_TRUE; }

// nDiscardDisplayList: framework frees the node's recorded content — drop the picture so the next
// hasDisplayList()/nIsValid reports "empty" and View.updateDisplayListIfDirty() re-records.
void RenderNode_nDiscardDisplayList(jlong node) {
  if (auto* n = reinterpret_cast<AclRenderNode*>(node)) {
    n->picture = nullptr;
    n->display_list.clear();
  }
}
// RenderNode.hasDisplayList() -> nIsValid. This MUST be false until the node has actually recorded
// a display list (endRecording -> nFinishRecording sets picture): View.updateDisplayListIfDirty()
// only records when hasDisplayList() is false, so a blanket TRUE makes Compose/Views never record
// (drawRenderNode then replays an empty node -> blank). present-root is always "valid" (it draws
// immediately onto the GPU canvas and is never recorded into a picture).
// When set, non-root nodes report "no display list" so View.updateDisplayListIfDirty() re-records
// them every frame. The draw_rendernode drawable fix makes the MAIN content's settled layers resolve
// correctly, but an OVERLAY (Dialog/Popup) is a SEPARATE composition whose recomposer acl doesn't
// drive, so its children never re-record after the initial (empty) compose and stay blank. jvm_vm
// sets this only around the overlay pass to force those layers fresh each frame (the main content,
// drawn with the flag off, keeps its cache + the drawable fix).
std::atomic<int> g_force_record{0};
extern "C" void acl_graphics_set_force_record(int on) {
  g_force_record.store(on, std::memory_order_relaxed);
}
jboolean RenderNode_nIsValid(jlong node) {
  auto* n = reinterpret_cast<AclRenderNode*>(node);
  if (n == nullptr) return JNI_FALSE;
  if (g_force_record.load(std::memory_order_relaxed) && !n->is_present_root) return JNI_FALSE;
  return (n->is_present_root || !n->display_list.empty() || n->picture != nullptr) ? JNI_TRUE
                                                                                   : JNI_FALSE;
}
void RenderNode_nGetTransformMatrix(jlong, jlong m) {
  if (auto* mat = reinterpret_cast<SkMatrix*>(m)) mat->reset();
}
void RenderNode_nGetInverseTransformMatrix(jlong, jlong m) {
  if (auto* mat = reinterpret_cast<SkMatrix*>(m)) mat->reset();
}
jboolean RenderNode_nHasIdentityMatrix(jlong) { return JNI_TRUE; }
jboolean RenderNode_nOffsetTopAndBottom(jlong h, jint off) {
  auto* n = rnode(h); if (n && off != 0) { n->top += off; n->bottom += off; bump_scene(); } return JNI_TRUE;
}
jboolean RenderNode_nOffsetLeftAndRight(jlong h, jint off) {
  auto* n = rnode(h); if (n && off != 0) { n->left += off; n->right += off; bump_scene(); } return JNI_TRUE;
}
jboolean RenderNode_nSetLeftTopRightBottom(jlong h, jint l, jint t, jint r, jint b) {
  auto* n = rnode(h);
  if (n && (n->left != l || n->top != t || n->right != r || n->bottom != b)) {
    n->left = l; n->top = t; n->right = r; n->bottom = b; bump_scene();
  }
  return JNI_TRUE;
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
jboolean RenderNode_nSetOutlineRoundRect(jlong node, jint l, jint t, jint r, jint b, jfloat radius,
                                         jfloat /*alpha*/) {
  if (auto* n = rnode(node)) {
    n->outline_kind = AclRenderNode::OutlineKind::RoundRect;
    n->outline_rect = SkRect::MakeLTRB(static_cast<float>(l), static_cast<float>(t),
                                       static_cast<float>(r), static_cast<float>(b));
    n->outline_radius = radius;
    n->outline_path.reset();
  }
  return JNI_TRUE;
}
jboolean RenderNode_nSetOutlinePath(jlong node, jlong path, jfloat /*alpha*/) {
  auto* n = rnode(node);
  auto* p = reinterpret_cast<SkPath*>(path);
  if (n != nullptr && p != nullptr) {
    n->outline_kind = AclRenderNode::OutlineKind::Path;
    n->outline_path = *p;  // copy: the app may mutate/free its Path after setting the outline
  }
  return JNI_TRUE;
}
// setOutlineEmpty (outline present but empty) / setOutlineNone (no outline) — both clear any clip.
jboolean RenderNode_nSetOutlineEmpty(jlong node) {
  if (auto* n = rnode(node)) { n->outline_kind = AclRenderNode::OutlineKind::None; n->outline_path.reset(); }
  return JNI_TRUE;
}
jboolean RenderNode_nSetOutlineNone(jlong node) {
  if (auto* n = rnode(node)) { n->outline_kind = AclRenderNode::OutlineKind::None; n->outline_path.reset(); }
  return JNI_TRUE;
}
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
jboolean RenderNode_nSetRenderEffect(jlong node, jlong effect) {
  if (auto* n = rnode(node)) {
    n->render_effect = sk_ref_sp(reinterpret_cast<SkImageFilter*>(effect));  // may be null (cleared)
    bump_scene();
  }
  return JNI_TRUE;
}
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
jfloat Paint_nGetStrokeWidth(jlong p) { ACLP(p); return ap ? ap->paint.getStrokeWidth() : 0; }
void Paint_nSetStrokeMiter(jlong p, jfloat m) { ACLP(p); if (ap) ap->paint.setStrokeMiter(m); }
void Paint_nSetAlpha(jlong p, jint a) { ACLP(p); if (ap) ap->paint.setAlpha(static_cast<U8CPU>(a)); }
jfloat Paint_nGetTextScaleX(jlong p) { ACLP(p); return ap ? ap->font.getScaleX() : 1; }
void Paint_nSetTextScaleX(jlong p, jfloat s) { ACLP(p); if (ap) ap->font.setScaleX(s); }
jfloat Paint_nGetTextSkewX(jlong p) { ACLP(p); return ap ? ap->font.getSkewX() : 0; }
void Paint_nSetTextSkewX(jlong p, jfloat s) { ACLP(p); if (ap) ap->font.setSkewX(s); }
// A foreign typeface here would not crash until Skia shaped text with it, far from the cause,
// so fall back to the bundled default rather than storing an unvalidated pointer in the Paint.
void Paint_nSetTypeface(jlong p, jlong tf) {
  ACLP(p);
  if (!ap) return;
  auto* t = handle_checked<SkTypeface>(tf, HandleKind::kTypeface, "Paint.nSetTypeface");
  ap->font.setTypeface(t != nullptr ? sk_ref_sp(t) : default_typeface());
}
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
jlong Paint_nSetPathEffect(jlong p, jlong e) {
  // Apply the SkPathEffect (e.g. DashPathEffect) to the paint so dashed strokes actually render;
  // e==0 clears it. The effect is a ref-counted SkPathEffect* (see DashPathEffect_nativeCreate).
  ACLP(p);
  if (ap != nullptr)
    ap->paint.setPathEffect(sk_ref_sp(reinterpret_cast<SkPathEffect*>(e)));
  return e;
}
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
// Paint.getTextRunAdvances / measureText(char[]): the char[] overload of nGetTextAdvances. Fills
// `advances` with the per-UTF-16-code-unit advance and returns the total. Same per-unit measure as
// the String version (adequate for layout width without full cluster shaping). Text is UTF-16.
jfloat Paint_nGetTextAdvancesChars(JNIEnv* env, jclass, jlong paintPtr, jcharArray chars,
                                   jint index, jint count, jint /*ctxIndex*/, jint /*ctxCount*/,
                                   jint /*flags*/, jfloatArray advances, jint advIdx) {
  auto* ap = reinterpret_cast<AclPaint*>(paintPtr);
  if (ap == nullptr || chars == nullptr || count <= 0) return 0.0f;
  jchar* buf = env->GetCharArrayElements(chars, nullptr);
  if (buf == nullptr) return 0.0f;
  const int arlen = env->GetArrayLength(chars);
  const int s = index < 0 ? 0 : index;
  const int e = (index + count) > arlen ? arlen : (index + count);
  float total = 0.0f;
  if (e > s) {
    total = ap->font.measureText(buf + s, static_cast<std::size_t>(e - s) * sizeof(jchar),
                                 SkTextEncoding::kUTF16, nullptr, &ap->paint);
    if (advances != nullptr) {
      std::vector<jfloat> adv(static_cast<std::size_t>(e - s), 0.0f);
      for (int i = 0; i < e - s; ++i)
        adv[i] = ap->font.measureText(buf + s + i, sizeof(jchar), SkTextEncoding::kUTF16, nullptr,
                                      &ap->paint);
      env->SetFloatArrayRegion(advances, advIdx, e - s, adv.data());
    }
  }
  env->ReleaseCharArrayElements(chars, buf, JNI_ABORT);
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
// via VectorDrawable.nSetPathString.
struct AclPathData {
  SkPath path;
};

// Two paths can morph iff their verb sequences are identical — same verbs in the same order.
// AOSP compares the parsed PathData verbs + point counts; for an SkPath the point count is fixed
// by the verb, so matching verbs is the same test. AnimatorInflater.canMorph() gates inflation of
// every animated button drawable (btn_check_material_anim etc.), so a wrong `false` here means no
// CheckBox / Switch / RadioButton can inflate at all.
bool skpath_can_morph(const SkPath& a, const SkPath& b) {
  SkPath::Iter ia(a, /*forceClose=*/false), ib(b, /*forceClose=*/false);
  SkPoint pa[4], pb[4];
  for (;;) {
    const SkPath::Verb va = ia.next(pa);
    const SkPath::Verb vb = ib.next(pb);
    if (va != vb) return false;
    if (va == SkPath::kDone_Verb) return true;
  }
}

// out = lerp(from, to, t), point by point. from/to must have matching verb structure (the caller
// checked canMorph); if they somehow don't, bail so the animation freezes rather than draws garbage.
bool skpath_interpolate(SkPath* out, const SkPath& from, const SkPath& to, float t) {
  const auto lerp = [t](SkPoint a, SkPoint b) {
    return SkPoint::Make(a.fX + (b.fX - a.fX) * t, a.fY + (b.fY - a.fY) * t);
  };
  SkPath::Iter fi(from, false), ti(to, false);
  SkPoint fp[4], tp[4];
  SkPath r;
  for (;;) {
    const SkPath::Verb fv = fi.next(fp);
    const SkPath::Verb tv = ti.next(tp);
    if (fv != tv) return false;
    switch (fv) {
      case SkPath::kMove_Verb: r.moveTo(lerp(fp[0], tp[0])); break;
      case SkPath::kLine_Verb: r.lineTo(lerp(fp[1], tp[1])); break;
      case SkPath::kQuad_Verb: r.quadTo(lerp(fp[1], tp[1]), lerp(fp[2], tp[2])); break;
      case SkPath::kConic_Verb:
        r.conicTo(lerp(fp[1], tp[1]), lerp(fp[2], tp[2]),
                  fi.conicWeight() + (ti.conicWeight() - fi.conicWeight()) * t);
        break;
      case SkPath::kCubic_Verb:
        r.cubicTo(lerp(fp[1], tp[1]), lerp(fp[2], tp[2]), lerp(fp[3], tp[3]));
        break;
      case SkPath::kClose_Verb: r.close(); break;
      case SkPath::kDone_Verb: *out = r; return true;
    }
  }
}
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
// nInterpolatePathData(out, from, to, fraction): out = from*(1-fraction) + to*fraction.
jboolean PathData_nInterpolate(JNIEnv*, jclass, jlong out, jlong from, jlong to, jfloat fraction) {
  auto* o = reinterpret_cast<AclPathData*>(out);
  auto* f = reinterpret_cast<AclPathData*>(from);
  auto* t = reinterpret_cast<AclPathData*>(to);
  if (o == nullptr || f == nullptr || t == nullptr) return JNI_FALSE;
  return skpath_interpolate(&o->path, f->path, t->path, fraction) ? JNI_TRUE : JNI_FALSE;
}
jboolean PathData_nCanMorph(JNIEnv*, jclass, jlong from, jlong to) {
  auto* f = reinterpret_cast<AclPathData*>(from);
  auto* t = reinterpret_cast<AclPathData*>(to);
  if (f == nullptr || t == nullptr) return JNI_FALSE;
  return skpath_can_morph(f->path, t->path) ? JNI_TRUE : JNI_FALSE;
}
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
  auto* canvas = canvas_sk(canvasPtr);
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

// The release funcs run from the framework's NativeAllocationRegistry, i.e. on a handle the
// framework believes is live. Validate anyway and drop the registration first, so a foreign or
// double-freed pointer is ignored rather than unref'd/deleted.
void free_typeface(void* p) {
  if (!HandleTable::holds(p, HandleKind::kTypeface)) return;
  HandleTable::forget(p);
  SkSafeUnref(reinterpret_cast<SkTypeface*>(p));
}
void free_font(void* p) {
  if (!HandleTable::holds(p, HandleKind::kFont)) return;
  HandleTable::forget(p);
  delete reinterpret_cast<AclFont*>(p);
}
void free_fontfamily(void* p) {
  if (!HandleTable::holds(p, HandleKind::kFontFamily)) return;
  HandleTable::forget(p);
  delete reinterpret_cast<AclFontFamily*>(p);
}

// android.graphics.fonts.Font$Builder
//
// ABI: the natives below follow whatever the framework declares. The font classes annotate most
// of their natives @CriticalNative, which means ART passes ONLY the declared arguments — no
// JNIEnv*, no jclass. Declaring such a native the normal way makes every argument land one or
// two registers off, so the "handle" read is whatever happened to be in that register (a stack
// address, an interior pointer) and dereferencing it is undefined. That is exactly what crashed
// Typeface.nativeGetStyle and Font.nGetPackedStyle. @FastNative, by contrast, keeps the normal
// signature. Cross-check against frameworks/base/graphics/java/android/graphics/{Typeface,
// fonts/Font,fonts/FontFamily}.java before adding or changing one here.
jlong FontBuilder_nInitBuilder(JNIEnv*, jclass) { return handle_mint(new AclFont(), HandleKind::kFont); }
void FontBuilder_nAddAxis(jlong, jint, jfloat) {}  // @CriticalNative
jlong FontBuilder_nBuild(JNIEnv* env, jclass, jlong builder, jobject buffer, jstring path,
                        jstring /*localeList*/, jint weight, jboolean italic, jint ttc) {
  auto* f = handle_checked<AclFont>(builder, HandleKind::kFont, "Font$Builder.nBuild");
  const bool fresh = f == nullptr;  // no usable builder handle — build a Font from scratch
  if (fresh) f = new AclFont();
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
  return fresh ? handle_mint(f, HandleKind::kFont) : reinterpret_cast<jlong>(f);
}
jlong FontBuilder_nGetReleaseNativeFont() { return reinterpret_cast<jlong>(&free_font); }  // @CriticalNative
// @FastNative — normal signature.
jlong FontBuilder_nClone(JNIEnv*, jclass, jlong fontPtr, jlong, jint weight, jboolean italic, jint) {
  auto* src = handle_checked<AclFont>(fontPtr, HandleKind::kFont, "Font$Builder.nClone");
  auto* f = new AclFont();
  f->tf = src != nullptr ? src->tf : default_typeface();
  f->weight = weight; f->italic = italic;
  return handle_mint(f, HandleKind::kFont);
}
// android.graphics.fonts.FontFamily$Builder
jlong FontFamilyBuilder_nInitBuilder(JNIEnv*, jclass) {
  return handle_mint(new AclFontFamily(), HandleKind::kFontFamily);
}
void FontFamilyBuilder_nAddFont(jlong builder, jlong fontPtr) {  // @CriticalNative
  auto* fam = handle_checked<AclFontFamily>(builder, HandleKind::kFontFamily, "FontFamily$Builder.nAddFont");
  auto* f = handle_checked<AclFont>(fontPtr, HandleKind::kFont, "FontFamily$Builder.nAddFont");
  if (fam != nullptr && f != nullptr) fam->fonts.push_back(f->tf);
}
jlong FontFamilyBuilder_nBuild(JNIEnv*, jclass, jlong builder, jstring, jint, jboolean) { return builder; }
jlong FontFamilyBuilder_nGetReleaseNativeFamily() { return reinterpret_cast<jlong>(&free_fontfamily); }  // @CriticalNative
// android.graphics.fonts.FontFamily
jint FontFamily_nGetFontSize(jlong fam) {  // @CriticalNative
  auto* f = handle_checked<AclFontFamily>(fam, HandleKind::kFontFamily, "FontFamily.nGetFontSize");
  return f != nullptr ? static_cast<jint>(f->fonts.size()) : 0;
}
jlong FontFamily_nGetFont(jlong fam, jint i) {  // @CriticalNative
  auto* f = handle_checked<AclFontFamily>(fam, HandleKind::kFontFamily, "FontFamily.nGetFont");
  if (f == nullptr || i < 0 || i >= static_cast<jint>(f->fonts.size())) return 0;
  auto* out = new AclFont(); out->tf = f->fonts[i];
  return handle_mint(out, HandleKind::kFont);
}
jstring FontFamily_nGetLangTags(JNIEnv* env, jclass, jlong) { return env->NewStringUTF(""); }  // @FastNative
jint FontFamily_nGetVariant(jlong) { return 0; }  // @CriticalNative
// android.graphics.fonts.Font (the immutable Font; native instance is an AclFont*).
jlong Font_nGetReleaseNativeFont() { return reinterpret_cast<jlong>(&free_font); }  // @CriticalNative
jlong Font_nCloneFont(jlong p) {  // @CriticalNative
  auto* s = handle_checked<AclFont>(p, HandleKind::kFont, "Font.nCloneFont");
  auto* f = new AclFont();
  if (s != nullptr) { f->tf = s->tf; f->weight = s->weight; f->italic = s->italic; }
  return handle_mint(f, HandleKind::kFont);
}
// The Font.getStyle() path — @CriticalNative, and one of the two observed SIGSEGVs.
jint Font_nGetPackedStyle(jlong p) {
  auto* f = handle_checked<AclFont>(p, HandleKind::kFont, "Font.nGetPackedStyle");
  if (f == nullptr) return 400;  // FontStyle.FONT_WEIGHT_NORMAL, upright
  return (f->weight & 0xFFFF) | (f->italic ? (1 << 16) : 0);
}
jint Font_nGetIndex(jlong) { return 0; }               // @CriticalNative
jint Font_nGetAxisCount(jlong) { return 0; }           // @CriticalNative
jlong Font_nGetAxisInfo(jlong, jint) { return 0; }     // @CriticalNative
jstring Font_nGetFontPath(JNIEnv* env, jclass, jlong) { return env->NewStringUTF(""); }    // @FastNative
jstring Font_nGetLocaleList(JNIEnv* env, jclass, jlong) { return env->NewStringUTF(""); }  // @FastNative
jint Font_nGetSourceId(jlong) { return 0; }            // @CriticalNative
jlong Font_nGetMinikinFontPtr(jlong p) {               // @CriticalNative
  // acl has no minikin; the AclFont doubles as the "minikin font". Hand back only a handle we
  // minted, so a foreign long cannot be laundered into one that later passes validation.
  return handle_checked<AclFont>(p, HandleKind::kFont, "Font.nGetMinikinFontPtr") != nullptr ? p : 0;
}
jlongArray Font_nGetAvailableFontSet(JNIEnv*, jclass) { return nullptr; }  // @FastNative
jobject Font_nNewByteBuffer(JNIEnv*, jclass, jlong) { return nullptr; }    // @FastNative
jlong Font_nGetBufferAddress(jlong) { return 0; }                          // @CriticalNative
jfloat Font_nGetGlyphBounds(JNIEnv*, jclass, jlong, jint, jlong, jobject) { return 0; }
jfloat Font_nGetFontMetrics(JNIEnv*, jclass, jlong, jlong, jobject) { return 0; }

// android.graphics.Typeface — handle is SkTypeface* (+1 ref).
jint sk_typeface_style(const SkFontStyle& s) {
  const bool bold = s.weight() >= SkFontStyle::kBold_Weight;
  const bool italic = s.slant() != SkFontStyle::kUpright_Slant;
  return (bold ? 1 : 0) | (italic ? 2 : 0);  // NORMAL=0, BOLD=1, ITALIC=2, BOLD_ITALIC=3
}
// Derive a new Typeface handle from an existing one. The extra Skia ref gets its own
// registration (same address, count+1) so the two Java Typefaces are freed independently.
jlong typeface_ref(SkTypeface* t) {
  if (t != nullptr) { SkSafeRef(t); return handle_mint(t, HandleKind::kTypeface); }
  return handle_mint(default_typeface().release(), HandleKind::kTypeface);
}
// The jlong-taking natives all resolve their argument through this, never a raw cast.
SkTypeface* typeface_arg(jlong ni, const char* who) {
  return handle_checked<SkTypeface>(ni, HandleKind::kTypeface, who);
}
jlong Typeface_nativeGetReleaseFunc() { return reinterpret_cast<jlong>(&free_typeface); }  // @CriticalNative
jlong Typeface_nativeCreateFromArray(JNIEnv* env, jclass, jlongArray families, jlong, jint, jint) {
  sk_sp<SkTypeface> tf;
  if (families != nullptr && env->GetArrayLength(families) > 0) {
    jlong first = 0; env->GetLongArrayRegion(families, 0, 1, &first);
    if (auto* fam = handle_checked<AclFontFamily>(first, HandleKind::kFontFamily,
                                                  "Typeface.nativeCreateFromArray");
        fam != nullptr && !fam->fonts.empty())
      tf = fam->fonts[0];
  }
  if (!tf) tf = default_typeface();
  return handle_mint(tf.release(), HandleKind::kTypeface);
}
jlong Typeface_nativeCreateFromTypeface(JNIEnv*, jclass, jlong ni, jint) {
  return typeface_ref(typeface_arg(ni, "Typeface.nativeCreateFromTypeface"));
}
jlong Typeface_nativeCreateFromTypefaceWithExactStyle(JNIEnv*, jclass, jlong ni, jint, jboolean) {
  return typeface_ref(typeface_arg(ni, "Typeface.nativeCreateFromTypefaceWithExactStyle"));
}
jlong Typeface_nativeCreateFromTypefaceWithVariation(JNIEnv*, jclass, jlong ni, jobject) {
  return typeface_ref(typeface_arg(ni, "Typeface.nativeCreateFromTypefaceWithVariation"));
}
jlong Typeface_nativeCreateWeightAlias(JNIEnv*, jclass, jlong ni, jint) {
  return typeface_ref(typeface_arg(ni, "Typeface.nativeCreateWeightAlias"));
}
jintArray Typeface_nativeGetSupportedAxes(JNIEnv*, jclass, jlong) { return nullptr; }
void Typeface_nativeSetDefault(jlong) {}  // @CriticalNative
// Typeface.<init> calls this on every Typeface — @CriticalNative, and the first observed SIGSEGV.
// Unknown handle => Typeface.NORMAL.
jint Typeface_nativeGetStyle(jlong ni) {
  auto* t = typeface_arg(ni, "Typeface.nativeGetStyle");
  return t != nullptr ? sk_typeface_style(t->fontStyle()) : 0;
}
jint Typeface_nativeGetWeight(jlong ni) {  // @CriticalNative
  auto* t = typeface_arg(ni, "Typeface.nativeGetWeight");
  return t != nullptr ? t->fontStyle().weight() : 400;
}
jint Typeface_nativeGetFamilySize(jlong) { return 0; }        // @CriticalNative
jlong Typeface_nativeGetFamily(jlong, jint) { return 0; }     // @CriticalNative
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
void Typeface_nativeAddFontCollections(jlong) {}  // @CriticalNative
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
  tick_draw();
  auto* c = canvas_sk(canvas);
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

// android.graphics.animation.NativeInterpolatorFactory: each returns a native Interpolator handle
// used by the RenderThread AVD animator (VectorDrawableAnimatorRT). That animator is hwui-only and
// acl stubs its whole native chain to no-ops (kAnimatedVectorDrawable above), so the handle is
// handed to nAddAnimator and never dereferenced — it only has to be non-zero. Without these an AVD
// with a path/animated interpolator (btn_radio_material_anim → RadioButton/Switch) throws
// UnsatisfiedLinkError at inflation. One shared non-null address, so no per-call allocation or leak
// (and it is a plain byte, never a VirtualRefBase, so a stray inc/decStrong can't delete it).
jlong stub_interpolator() {
  static const char sink = 0;
  return reinterpret_cast<jlong>(&sink);
}
const JNINativeMethod kNativeInterpolatorFactory[] = {
    {"createAccelerateDecelerateInterpolator", "()J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jlong { return stub_interpolator(); })},
    {"createAccelerateInterpolator", "(F)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jfloat) -> jlong { return stub_interpolator(); })},
    {"createAnticipateInterpolator", "(F)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jfloat) -> jlong { return stub_interpolator(); })},
    {"createAnticipateOvershootInterpolator", "(F)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jfloat) -> jlong { return stub_interpolator(); })},
    {"createBounceInterpolator", "()J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jlong { return stub_interpolator(); })},
    {"createCycleInterpolator", "(F)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jfloat) -> jlong { return stub_interpolator(); })},
    {"createDecelerateInterpolator", "(F)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jfloat) -> jlong { return stub_interpolator(); })},
    {"createLinearInterpolator", "()J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jlong { return stub_interpolator(); })},
    {"createOvershootInterpolator", "(F)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jfloat) -> jlong { return stub_interpolator(); })},
    {"createPathInterpolator", "([F[F)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jfloatArray, jfloatArray) -> jlong {
       return stub_interpolator(); })},
    {"createLutInterpolator", "([F)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jfloatArray) -> jlong { return stub_interpolator(); })},
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
    {"nativeGetPixels", "(J[IIIIIII)V", reinterpret_cast<void*>(Bitmap_nativeGetPixels)},
    {"nativeGetAllocationByteCount", "(J)I",
     reinterpret_cast<void*>(Bitmap_nativeGetAllocationByteCount)},
    {"nativeSetHasAlpha", "(JZZ)V", reinterpret_cast<void*>(Bitmap_nativeSetHasAlpha)},
    {"nativeErase", "(JI)V", reinterpret_cast<void*>(Bitmap_nativeErase)},
    {"nativeCopy", "(JIZ)Landroid/graphics/Bitmap;", reinterpret_cast<void*>(Bitmap_nativeCopy)},
    {"nativeComputeColorSpace", "(J)Landroid/graphics/ColorSpace;",
     reinterpret_cast<void*>(Bitmap_nativeComputeColorSpace)},
    {"nativeCompress", "(JIILjava/io/OutputStream;[B)Z", reinterpret_cast<void*>(Bitmap_nativeCompress)},
    {"nativeIsPremultiplied", "(J)Z", reinterpret_cast<void*>(Bitmap_nativeIsPremultiplied)},
    {"nativeHasAlpha", "(J)Z", reinterpret_cast<void*>(Bitmap_nativeHasAlpha)},
    {"nativeHasMipMap", "(J)Z", reinterpret_cast<void*>(Bitmap_nativeHasMipMap)},
    {"nativeConfig", "(J)I", reinterpret_cast<void*>(Bitmap_nativeConfig)},
    {"nativeIsImmutable", "(J)Z", reinterpret_cast<void*>(Bitmap_nativeIsImmutable)},
    {"nativeIsBackedByAshmem", "(J)Z", reinterpret_cast<void*>(Bitmap_nativeIsBackedByAshmem)},
    {"nativePrepareToDraw", "(J)V", reinterpret_cast<void*>(Bitmap_nativePrepareToDraw)},
    {"nativeGenerationId", "(J)I", reinterpret_cast<void*>(Bitmap_nativeGenerationId)},
    {"nativeRowBytes", "(J)I", reinterpret_cast<void*>(Bitmap_nativeRowBytes)},
    {"nativeSetImmutable", "(J)V", reinterpret_cast<void*>(Bitmap_nativeSetImmutable)},
    {"nativeSetHasMipMap", "(JZ)V", reinterpret_cast<void*>(Bitmap_nativeSetHasMipMap)},
    {"nativeSetPremultiplied", "(JZ)V", reinterpret_cast<void*>(Bitmap_nativeSetPremultiplied)},
    {"nativeSetColorSpace", "(JJ)V", reinterpret_cast<void*>(Bitmap_nativeSetColorSpace)},
    {"nativeRecycle", "(J)V", reinterpret_cast<void*>(Bitmap_nativeRecycle)},
    {"nativeReconfigure", "(JIIIZ)V", reinterpret_cast<void*>(Bitmap_nativeReconfigure)},
};
// ── RenderNode record/replay (Compose) ───────────────────────────────────────────────────────
// Compose records each layer's content into a RenderNode via RecordingCanvas (beginRecording ->
// draw ops -> endRecording), then paints it with canvas.drawRenderNode(node). acl re-owns this on
// Skia: a RenderNode's recording canvas is an SkPictureRecorder (its SkCanvas ptr is STABLE across
// begin/reset on the same recorder, so the pool's nResetDisplayListCanvas works); endRecording
// finalizes the SkPicture into the node; drawRenderNode replays it with the node's transform/alpha/
// clip. acl's top-level (present-root) node is special: its RecordingCanvas draws IMMEDIATELY onto
// the GPU SkCanvas (set via acl_graphics_set_present_canvas) so the whole tree renders GPU-native.
jlong g_present_gpu_canvas = 0;
std::mutex g_rec_mu;
std::unordered_map<SkCanvas*, SkPictureRecorder*> g_recorders;  // recording canvas -> its recorder
// DIAG counters (Compose record/replay): begins=non-root nCreate, finishes, replays=nDrawRenderNode.
std::atomic<std::uint64_t> g_rn_begins{0}, g_rn_finishes{0}, g_rn_replays{0};
extern "C" std::uint64_t acl_graphics_profile_recstats() {
  return (g_rn_begins.load() << 40) | (g_rn_finishes.load() << 20) | g_rn_replays.load();
}

extern "C" void acl_graphics_set_present_canvas(jlong c) { g_present_gpu_canvas = c; }

// ── NodeRecorder: segmented display-list recording (hwui live-reference model) ────────────────
// A RenderNode's recording canvas. It records the node's own draw ops into an SkPictureRecorder, and
// at each child/texture-layer boundary (nDrawRenderNode / nDrawTextureLayer) it FINISHES the current
// segment into a Picture DisplayItem, records a live Child/Layer boundary, and BEGINS a fresh segment
// — so children are references the native walk resolves live, never snapshots. The Java-held handle
// (a kRecorderTag-tagged NodeRecorder*) is stable across splits; canvas_sk() resolves it to `seg`,
// the current segment's canvas, which changes each split. One NodeRecorder is pooled/reused across
// many nodes (nReset per node, nFinish moves its items into that node's display_list). The pre-split
// CTM is re-applied to each new segment (carry_m) so own-content after a boundary keeps its transform;
// SkPicture playback composes the recorded setMatrix with the walk CTM, so this nests correctly.
struct NodeRecorder {
  SkPictureRecorder rec;
  SkCanvas* seg = nullptr;          // current segment's recording canvas (null between finish and reset)
  int w = 1, h = 1;
  SkMatrix carry_m = SkMatrix::I();  // CTM at the last boundary, re-applied to the next segment
  int carry_saves = 0;               // unbalanced save() depth carried across the boundary
  bool z_enabled = false;            // inside a RecordingCanvas.enableZ() region (children are Z-sorted)
  std::vector<DisplayItem> items;

  explicit NodeRecorder(int W, int H) { reset(W, H); }

  // Begin a segment, restoring the canvas STATE the recording was in when the previous segment was
  // split: the CTM (carry_m) and the save-stack DEPTH (carry_saves). The depth matters because the
  // framework saves the canvas before dispatchDraw and restores after (fading edges / clipChildren),
  // straddling child boundaries — without re-issuing the saves, the restore in the next segment
  // underflows ("more restores than saves"). Balanced saves are auto-closed at finishRecording.
  void begin_seg() {
    seg = rec.beginRecording(SkRect::MakeIWH(w, h));
    if (seg == nullptr) return;
    if (!carry_m.isIdentity()) seg->setMatrix(carry_m);
    for (int i = 0; i < carry_saves; ++i) seg->save();
  }
  // Finalize the current segment into a Picture item (dropped if it recorded nothing).
  void flush_seg() {
    if (seg == nullptr) return;
    sk_sp<SkPicture> pic = rec.finishRecordingAsPicture();
    seg = nullptr;
    if (pic && pic->approximateOpCount(true) > 0) {
      DisplayItem it;
      it.kind = DisplayItem::Kind::Picture;
      it.picture = std::move(pic);
      items.push_back(std::move(it));
    }
  }
  // Split at a boundary: capture the node-local CTM + save depth, finish the segment, push the
  // boundary, restart the recording in the same canvas state so post-boundary ops (and the framework's
  // straddling restore) behave as if the recording were never split.
  void split(DisplayItem&& boundary) {
    const int sc = seg != nullptr ? seg->getSaveCount() : 1;  // 1 == base (no extra saves)
    carry_m = seg != nullptr ? seg->getTotalMatrix() : SkMatrix::I();
    carry_saves = sc > 1 ? sc - 1 : 0;
    boundary.matrix = carry_m;
    flush_seg();
    items.push_back(std::move(boundary));
    begin_seg();
  }
  void boundary_child(AclRenderNode* child) {
    DisplayItem it;
    it.kind = DisplayItem::Kind::Child;
    it.child = sk_ref_sp(child);
    it.z_ordered = z_enabled;
    split(std::move(it));
  }
  void boundary_layer(AclTextureLayer* layer) {
    DisplayItem it;
    it.kind = DisplayItem::Kind::Layer;
    it.layer = layer;
    split(std::move(it));
  }
  void reset(int W, int H) {
    if (seg != nullptr) { rec.finishRecordingAsPicture(); seg = nullptr; }
    items.clear();
    carry_m = SkMatrix::I();
    carry_saves = 0;
    z_enabled = false;
    w = W > 0 ? W : 1;
    h = H > 0 ? H : 1;
    begin_seg();
  }
  std::vector<DisplayItem> finish() {
    flush_seg();
    return std::move(items);
  }
};

inline NodeRecorder* recorder_of(jlong h) {
  return reinterpret_cast<NodeRecorder*>(h & ~kRecorderTag);
}
SkCanvas* recorder_seg_canvas(jlong h) {
  auto* nr = recorder_of(h);
  return nr != nullptr ? nr->seg : nullptr;
}
void free_node_recorder(jlong h) { delete recorder_of(h); }

// ── Deferred present (Step 1 of the UI-thread/RenderThread split) ─────────────────────────────
// Normally the present-root RecordingCanvas draws IMMEDIATELY onto the GPU SkCanvas, so View.draw
// does GrContext work on the present thread — which cannot be moved off it (GrDirectContext is
// single-threaded). When deferral is on, the present-root instead records into an SkPicture on the
// UI thread (no GrContext), and the picture is REPLAYED onto the GPU canvas at gpu_end (eventually on
// a dedicated RenderThread). This is the enabler for the WebView producer/consumer thread split; see
// webview-renderthread-fix. Env-gated (ACL_PRESENT_RT) so the immediate path stays the default.
std::atomic<int> g_present_defer{0};
SkPictureRecorder* g_present_recorder = nullptr;  // owns the present-root recording canvas (UI thread)
SkCanvas* g_present_record_canvas = nullptr;      // stable across begin/finish/begin
// Double-buffered picture handoff for the one-frame-behind RenderThread: the UI thread records into
// `ready`; at post time (still UI thread) it snapshots ready->rendering; the RenderThread replays
// `rendering`. Pictures from finishRecordingAsPicture are immutable, so the RT only reads immutable
// data; the sk_sp swaps are guarded by g_present_pic_mu. (Single-thread Step 1/2 use the same path;
// take is called right before replay there.)
sk_sp<SkPicture> g_present_picture_ready;
sk_sp<SkPicture> g_present_picture_held;       // one-frame-lag: the frame recorded LAST tick
sk_sp<SkPicture> g_present_picture_rendering;
// WebView functors can't be recorded into the picture: an SkDrawable is snapshotted via its onDraw
// (a no-op for AclVkFunctorDrawable — only onSnapGpuDrawHandler does GPU work), so the GPU secondary-CB
// path is lost. Instead capture each functor's transform+clip at record time and draw it DIRECTLY onto
// the GPU canvas at replay (where the snap handler fires). Same ready/rendering double-buffer as the
// picture.
struct PresentFunctor {
  int functor;
  SkMatrix matrix;
  SkIRect clip;
};
std::vector<PresentFunctor> g_present_functors_ready;
std::vector<PresentFunctor> g_present_functors_held;
std::vector<PresentFunctor> g_present_functors_rendering;
std::mutex g_present_pic_mu;
extern "C" bool acl_graphics_draw_webview_functor(void* sk_canvas, int functor);
extern "C" void acl_graphics_set_present_defer(int on) {
  g_present_defer.store(on, std::memory_order_relaxed);
}
// Called each frame before View.draw (UI thread): (re)begin recording. No-op unless deferring.
// ACL_PRESENT_DEFER_DEBUG=1 traces the deferred handoff (record -> take -> replay). The failure it
// exists for is a white window with content clearly being drawn, where the question is which stage
// drops it: nothing recorded, recorded but not handed over, or handed over but not replayed.
bool present_defer_debug() {
  static const bool on = [] {
    const char* v = std::getenv("ACL_PRESENT_DEFER_DEBUG");
    return v != nullptr && *v != '\0' && *v != '0';
  }();
  return on;
}
extern "C" void acl_graphics_present_begin_record(int w, int h) {
  if (!g_present_defer.load(std::memory_order_relaxed) || g_present_recorder == nullptr) return;
  SkCanvas* c = g_present_recorder->beginRecording(SkRect::MakeIWH(w > 0 ? w : 1, h > 0 ? h : 1));
  // The cached canvas pointer is handed to Java once (nCreate) and reused every frame. If
  // beginRecording ever returns a DIFFERENT canvas, Java records into a canvas nobody finishes and
  // the window goes white -- so assert the assumption rather than trusting it.
  if (c != g_present_record_canvas && present_defer_debug())
    std::fprintf(stderr, "[acl.present] begin_record: canvas moved %p -> %p (Java holds the old one)\n",
                 static_cast<void*>(g_present_record_canvas), static_cast<void*>(c));
  std::lock_guard<std::mutex> lk(g_present_pic_mu);
  g_present_functors_ready.clear();
}
// Capture a WebView functor's placement during View.draw (UI thread), so replay can draw it directly
// onto the GPU canvas. `sk_canvas` is the present recorder canvas at the WebView's position.
extern "C" void acl_graphics_present_record_functor(void* sk_canvas, int functor) {
  auto* c = reinterpret_cast<SkCanvas*>(sk_canvas);
  if (c == nullptr) return;
  SkIRect clip = c->getDeviceClipBounds();
  if (clip.isEmpty()) clip = SkIRect::MakeWH(c->imageInfo().width(), c->imageInfo().height());
  std::lock_guard<std::mutex> lk(g_present_pic_mu);
  // Dedup by functor handle: the view tree may be drawn more than once per frame (force-record +
  // overlay passes), but each functor must be drawn exactly ONCE per replay — a second draw_vk before
  // the first's post_draw_vk trips AwDrawFnImpl's !scoped_secondary_cb_draw_ DCHECK. Keep the latest.
  for (PresentFunctor& pf : g_present_functors_ready) {
    if (pf.functor == functor) {
      pf.matrix = c->getTotalMatrix();
      pf.clip = clip;
      return;
    }
  }
  g_present_functors_ready.push_back({functor, c->getTotalMatrix(), clip});
}
// Called each frame after overlays (UI thread): finalize the picture into `ready`.
extern "C" void acl_graphics_present_finish_record() {
  if (!g_present_defer.load(std::memory_order_relaxed) || g_present_recorder == nullptr) return;
  sk_sp<SkPicture> pic = g_present_recorder->finishRecordingAsPicture();
  if (present_defer_debug())
    std::fprintf(stderr, "[acl.present] finish_record: picture=%p ops=%d\n",
                 static_cast<void*>(pic.get()), pic ? pic->approximateOpCount(true) : -1);
  std::lock_guard<std::mutex> lk(g_present_pic_mu);
  g_present_picture_ready = std::move(pic);
}
// Snapshot ready->rendering, on the UI thread at RenderThread post time (Step 3), or just before
// replay (Step 1/2 synchronous). Hands the current frame's immutable picture to the RT.
extern "C" void acl_graphics_present_take_for_render() {
  std::lock_guard<std::mutex> lk(g_present_pic_mu);
  g_present_picture_rendering = g_present_picture_ready;
  g_present_functors_rendering = g_present_functors_ready;
}
// Called from gpu_end_frame (RenderThread, before flush): replay the taken present picture, then draw
// each WebView functor directly onto the GPU canvas (so its snap handler fires — see PresentFunctor).
extern "C" void acl_graphics_present_replay_onto(void* sk_canvas) {
  if (!g_present_defer.load(std::memory_order_relaxed)) return;
  sk_sp<SkPicture> pic;
  std::vector<PresentFunctor> functors;
  {
    std::lock_guard<std::mutex> lk(g_present_pic_mu);
    pic = g_present_picture_rendering;
    functors = g_present_functors_rendering;
  }
  auto* c = reinterpret_cast<SkCanvas*>(sk_canvas);
  if (present_defer_debug())
    std::fprintf(stderr, "[acl.present] replay_onto: canvas=%p picture=%p ops=%d functors=%zu\n",
                 static_cast<void*>(c), static_cast<void*>(pic.get()),
                 pic ? pic->approximateOpCount(true) : -1, functors.size());
  if (c == nullptr) return;
  if (pic) c->drawPicture(pic);
  for (const PresentFunctor& pf : functors) {
    c->save();
    c->resetMatrix();                          // device space for the clip
    c->clipRect(SkRect::Make(pf.clip));
    c->setMatrix(pf.matrix);                    // the WebView's transform at record time
    acl_graphics_draw_webview_functor(c, pf.functor);  // fires onSnapGpuDrawHandler on the GPU canvas
    c->restore();
  }
}

bool is_recording_canvas(void* p) {
  std::lock_guard<std::mutex> lk(g_rec_mu);
  return g_recorders.find(reinterpret_cast<SkCanvas*>(p)) != g_recorders.end();
}

void draw_texture_layer_immediate(SkCanvas* c, AclTextureLayer* l);  // defined with TextureLayer

// Apply a node's view-property transform (position + translation + scale/rotate about the pivot) —
// the placement shared by the node's content draw and its cast shadow. Does NOT apply clip/alpha.
void apply_view_transform(SkCanvas* c, const AclRenderNode* n) {
  const float w = static_cast<float>(n->right - n->left);
  const float h = static_cast<float>(n->bottom - n->top);
  c->translate(static_cast<float>(n->left), static_cast<float>(n->top));
  c->translate(n->translationX, n->translationY);
  if (n->scaleX != 1.0f || n->scaleY != 1.0f || n->rotation != 0.0f) {
    const float px = n->pivotExplicit ? n->pivotX : w * 0.5f;
    const float py = n->pivotExplicit ? n->pivotY : h * 0.5f;
    c->translate(px, py);
    if (n->rotation != 0.0f) c->rotate(n->rotation);
    c->scale(n->scaleX, n->scaleY);
    c->translate(-px, -py);
  }
}

// A node's shadow-casting outline (its ViewOutlineProvider shape) in local coords; false = no outline.
bool node_outline_path(const AclRenderNode* n, SkPath* out) {
  if (n->outline_kind == AclRenderNode::OutlineKind::RoundRect && !n->outline_rect.isEmpty()) {
    out->reset();
    if (n->outline_radius > 0.0f)
      out->addRRect(SkRRect::MakeRectXY(n->outline_rect, n->outline_radius, n->outline_radius));
    else
      out->addRect(n->outline_rect);
    return true;
  }
  if (n->outline_kind == AclRenderNode::OutlineKind::Path && !n->outline_path.isEmpty()) {
    *out = n->outline_path;
    return true;
  }
  return false;
}

// Cast a node's elevation shadow (ambient + spot), mirroring hwui's ReorderBarrierDrawables: a light
// centerd above the top of the screen (light_y=0, light_z=500dp, radius=800dp), zParams=(0,0,Z), and
// ambient/spot colors multiplied by the theme shadow alphas (0.039 / 0.19). SkShadowUtils tessellates
// the shadow of the node's outline for that geometry. The caller's canvas is in the parent space; this
// applies the node's view transform so the shadow lands under it. No-op unless the node has an outline
// and Z > 0. Opt out with ACL_NO_SHADOWS.
const bool g_shadows_off = std::getenv("ACL_NO_SHADOWS") != nullptr;
void draw_child_shadow(SkCanvas* c, const AclRenderNode* n) {
  if (g_shadows_off) return;
  const float z = n->elevation + n->translationZ;
  if (z <= 0.001f) return;
  SkPath path;
  if (!node_outline_path(n, &path)) return;
  SkAutoCanvasRestore acr(c, true);
  apply_view_transform(c, n);
  const SkISize base = c->getBaseLayerSize();
  const float lightX = base.width() > 0 ? base.width() * 0.5f : 640.0f;
  const SkPoint3 zParams = SkPoint3::Make(0.0f, 0.0f, z);
  const SkPoint3 lightPos = SkPoint3::Make(lightX, 0.0f, 500.0f);  // dp≈px (acl presents at scale 1)
  auto mulA = [](int color, float a) {
    const SkColor sc = static_cast<SkColor>(color);
    return SkColorSetA(sc, static_cast<U8CPU>(SkColorGetA(sc) * a + 0.5f));
  };
  const SkColor ambient = mulA(n->ambientShadowColor, 0.039f);
  const SkColor spot = mulA(n->spotShadowColor, 0.19f);
  const uint32_t flags =
      n->alpha < 1.0f ? SkShadowFlags::kTransparentOccluder_ShadowFlag : SkShadowFlags::kNone_ShadowFlag;
  SkShadowUtils::DrawShadow(c, path, zParams, lightPos, 800.0f, ambient, spot, flags);
}

// Walk a RenderNode's display list onto `c` NOW, applying the node's position + transform + clip +
// alpha (the properties the View/Compose set via the RenderNode setters). This is hwui's tree walk:
// own-op segments are replayed in order, and each child/texture-layer BOUNDARY is resolved LIVE at its
// recorded position — recursing into the child's CURRENT display list, or compositing the layer's
// CURRENT frame. So a settled subtree (a cached Compose layer, an animating TextureView) shows its
// latest content every frame with no parent re-record. Reads the node's current transform too, so an
// animated property takes effect immediately. Legacy leaf/deferred nodes with a single `picture` and
// no boundaries fall through to a plain drawPicture.
void draw_rendernode_immediate(SkCanvas* c, AclRenderNode* n) {
  if (c == nullptr || n == nullptr) return;
  if (n->display_list.empty() && n->picture == nullptr) return;
  const float w = static_cast<float>(n->right - n->left);
  const float h = static_cast<float>(n->bottom - n->top);
  SkAutoCanvasRestore acr(c, true);
  apply_view_transform(c, n);
  if (n->clipToBounds && w > 0 && h > 0) c->clipRect(SkRect::MakeWH(w, h));
  // clipToOutline: clip the node's content + children to its ViewOutlineProvider shape (rounded
  // rect or convex path), antialiased — Material cards/buttons/rounded images. Applied in the node's
  // transformed local space, so it scales/rotates with the content (like clipToBounds).
  if (n->clipToOutline && n->outline_kind != AclRenderNode::OutlineKind::None) {
    if (n->outline_kind == AclRenderNode::OutlineKind::RoundRect && !n->outline_rect.isEmpty()) {
      if (n->outline_radius > 0.0f)
        c->clipRRect(SkRRect::MakeRectXY(n->outline_rect, n->outline_radius, n->outline_radius), true);
      else
        c->clipRect(n->outline_rect, true);
    } else if (n->outline_kind == AclRenderNode::OutlineKind::Path && !n->outline_path.isEmpty()) {
      c->clipPath(n->outline_path, true);
    }
  }
  // A layer is needed for group alpha AND/OR a RenderEffect (blur etc.): render the node's content
  // into an offscreen and composite it through the effect + alpha. The filter applies in the node's
  // transformed local space (so a scaled node scales its blur too).
  if (n->alpha < 1.0f || n->render_effect != nullptr) {
    SkPaint lp;
    lp.setAlphaf(n->alpha);
    if (n->render_effect != nullptr) lp.setImageFilter(n->render_effect);
    c->saveLayer(nullptr, &lp);
  }
  static const bool walk_dbg = std::getenv("ACL_WALK_DEBUG") != nullptr;
  static std::atomic<int> walk_dbg_budget{400};
  if (walk_dbg && walk_dbg_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
    int np = 0, nc = 0, nl = 0;
    for (const DisplayItem& it : n->display_list)
      (it.kind == DisplayItem::Kind::Picture ? np : it.kind == DisplayItem::Kind::Child ? nc : nl)++;
    std::fprintf(stderr, "[acl.walk] node ltrb=(%d,%d,%d,%d) alpha=%.2f clip=%d dl=%zu (pic=%d child=%d layer=%d) pic=%p\n",
                 n->left, n->top, n->right, n->bottom, n->alpha, n->clipToBounds,
                 n->display_list.size(), np, nc, nl, static_cast<void*>(n->picture.get()));
  }
  if (!n->display_list.empty()) {
    const std::vector<DisplayItem>& dl = n->display_list;
    for (std::size_t i = 0; i < dl.size();) {
      const DisplayItem& it = dl[i];
      // A run of consecutive Z-ordered child boundaries (an enableZ region): draw them sorted by Z
      // (elevation + translationZ), shadows first (under all content), then content in ascending Z so
      // higher children draw on top. Reproduces hwui's reorder barrier for Material elevation.
      if (it.kind == DisplayItem::Kind::Child && it.z_ordered) {
        std::size_t j = i;
        std::vector<const DisplayItem*> grp;
        while (j < dl.size() && dl[j].kind == DisplayItem::Kind::Child && dl[j].z_ordered)
          grp.push_back(&dl[j++]);
        std::stable_sort(grp.begin(), grp.end(), [](const DisplayItem* a, const DisplayItem* b) {
          return (a->child->elevation + a->child->translationZ) <
                 (b->child->elevation + b->child->translationZ);
        });
        for (const DisplayItem* g : grp) {
          SkAutoCanvasRestore bcr(c, true);
          c->concat(g->matrix);
          draw_child_shadow(c, g->child.get());
        }
        for (const DisplayItem* g : grp) {
          SkAutoCanvasRestore bcr(c, true);
          c->concat(g->matrix);
          draw_rendernode_immediate(c, g->child.get());
        }
        i = j;
        continue;
      }
      switch (it.kind) {
        case DisplayItem::Kind::Picture:
          if (it.picture) c->drawPicture(it.picture);
          break;
        case DisplayItem::Kind::Child: {
          SkAutoCanvasRestore bcr(c, true);
          c->concat(it.matrix);  // node-local CTM at the boundary (composed with the walk CTM)
          draw_rendernode_immediate(c, it.child.get());
          break;
        }
        case DisplayItem::Kind::Layer: {
          SkAutoCanvasRestore bcr(c, true);
          c->concat(it.matrix);
          draw_texture_layer_immediate(c, it.layer);
          break;
        }
      }
      ++i;
    }
  } else {
    c->drawPicture(n->picture);
  }
}

// A deferred reference to a child RenderNode, recorded into a PARENT's picture. Its onDraw runs at
// REPLAY time and resolves the child's CURRENT picture — so an independently-updated Compose layer
// shows its latest content, instead of the stale copy that flattening (drawPicture at parent-record
// time) would cache (the "settled layers replay empty" blank). Holds a ref so the node outlives the
// parent picture that references it.
class AclRenderNodeDrawable : public SkDrawable {
 public:
  explicit AclRenderNodeDrawable(AclRenderNode* n) : node_(sk_ref_sp(n)) {}

 protected:
  SkRect onGetBounds() override {
    return SkRect::MakeLTRB(static_cast<float>(node_->left), static_cast<float>(node_->top),
                            static_cast<float>(node_->right), static_cast<float>(node_->bottom));
  }
  void onDraw(SkCanvas* c) override { draw_rendernode_immediate(c, node_.get()); }

 private:
  sk_sp<AclRenderNode> node_;
};

// Replay a child RenderNode onto `c`. If `c` is a parent's recording canvas (an SkPictureRecorder),
// record a deferred drawable so the child resolves at replay time; onto the GPU/immediate canvas,
// draw it now.
void draw_rendernode(SkCanvas* c, AclRenderNode* n) {
  if (c == nullptr || n == nullptr) return;
  if (is_recording_canvas(c)) {
    auto d = sk_make_sp<AclRenderNodeDrawable>(n);
    c->drawDrawable(d.get());
  } else {
    draw_rendernode_immediate(c, n);
  }
}

// ── SurfaceTexture + TextureView hardware layer (ADR-4: hwui dropped, render stack re-owned) ─────
// android.graphics.SurfaceTexture is a producer/consumer image stream; a TextureView composites its
// latest frame through a "texture layer". acl has no GLConsumer / BufferQueue+GL_TEXTURE_EXTERNAL_OES
// pipeline — in the re-owned render model a produced frame is an ordinary SkImage: the producer sets
// `pending`, updateTexImage latches pending->current, and the TextureLayer draws `current` into the
// display list as a resolve-at-replay SkDrawable (exactly like AclRenderNodeDrawable, so the layer
// shows its LATEST frame at present time, not a stale record-time copy). Clean-room: SurfaceTexture /
// TextureView SDK semantics only; no AOSP GLConsumer, no the predecessor. The producer half (Surface.lockCanvas /
// GL / MediaCodec feeding set_frame) is staged separately; until it exists a TextureView draws blank
// but never crashes the View traversal (the reason a bare UnsatisfiedLinkError here took the whole
// draw down).
struct AclSurfaceTexture {
  std::mutex mu;
  sk_sp<SkImage> pending;       // most-recent produced frame (producer thread; Stage 2 seam)
  sk_sp<SkImage> current;       // latched frame (updateTexImage: pending -> current)
  int def_w = 0, def_h = 0;     // setDefaultBufferSize
  jlong timestamp = 0;          // ns of the latched frame
  int tex_name = 0;             // GL texture name (nativeInit/attachToGLContext; GL path staged)
  bool detached = false;        // detachFromGLContext state
  bool released = false;
  jobject weak_self = nullptr;  // global ref to WeakReference<SurfaceTexture> (frame-available; staged)

  // Producer seam (Stage 2): hand the stream a freshly produced frame.
  void set_frame(sk_sp<SkImage> img, jlong ts) {
    std::lock_guard<std::mutex> lk(mu);
    pending = std::move(img);
    timestamp = ts;
  }
  // Consumer: latch the newest produced frame (updateTexImage / TextureLayer.update).
  void latch() {
    std::lock_guard<std::mutex> lk(mu);
    if (pending) { current = pending; }
  }
  sk_sp<SkImage> snapshot() {
    std::lock_guard<std::mutex> lk(mu);
    return current;
  }
};

// A TextureView's hardware layer: binds a SurfaceTexture + the layer's size/alpha. Java wraps the
// handle in a VirtualRefBasePtr (inc/decStrong), so it MUST be a VirtualLightRefBase or the wrapper
// dereferences a bad vtable and SIGSEGVs — the same trap as HardwareRendererObserver. It is freed by
// the wrapper's final decStrong, so nothing here deletes it.
struct AclTextureLayer : public VirtualLightRefBase {
  AclSurfaceTexture* st = nullptr;  // NOT owned (the SurfaceTexture outlives the layer, held Java-side)
  int width = 0, height = 0;
  float alpha = 1.0f;
};

inline AclSurfaceTexture* stex(jlong h) { return reinterpret_cast<AclSurfaceTexture*>(h); }
inline AclTextureLayer* tlayer(jlong h) { return reinterpret_cast<AclTextureLayer*>(h); }

// SurfaceTexture.mSurfaceTexture (long) — where nativeInit stashes the AclSurfaceTexture*.
jfieldID g_st_field = nullptr;
AclSurfaceTexture* st_from(JNIEnv* env, jobject thiz) {
  if (g_st_field == nullptr || thiz == nullptr) return nullptr;
  return reinterpret_cast<AclSurfaceTexture*>(env->GetLongField(thiz, g_st_field));
}

// DIAG (acl_graphics_profile): SurfaceTextures created; texture-layer draws with a frame vs blank.
std::atomic<std::uint64_t> g_st_created{0}, g_tl_frames{0}, g_tl_blanks{0};
std::atomic<std::uint64_t> g_st_produced{0};  // produced SurfaceTexture frames (drives re-record)

// SurfaceTexture.postEventFromNative(WeakReference) — the frame-available notify. Firing it makes a
// TextureView invalidate() and re-record its display list, so the resolve-at-replay layer drawable
// (which SkPictureRecorder snapshots at record time, NOT at replay) picks up each new produced frame
// AND the layer's real size. Without it a settled tree (ACL_FORCE_RECORD=0) shows the first snapshot
// forever (blank 0x0). Cached at nativeInit; the notify may run on a producer/donor thread, so it
// attaches to the VM if needed.
JavaVM* g_st_vm = nullptr;
jclass g_st_class = nullptr;         // global ref to android.graphics.SurfaceTexture
jmethodID g_st_post = nullptr;       // static postEventFromNative(WeakReference)
void post_frame_available(AclSurfaceTexture* st) {
  if (g_st_vm == nullptr || g_st_post == nullptr || st == nullptr || st->weak_self == nullptr) return;
  JNIEnv* env = nullptr;
  if (g_st_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    if (g_st_vm->AttachCurrentThreadAsDaemon(&env, nullptr) != JNI_OK)
      return;  // stays attached (pooled producer/donor thread) — cheap on later frames
  }
  env->CallStaticVoidMethod(g_st_class, g_st_post, st->weak_self);
  if (env->ExceptionCheck()) env->ExceptionClear();
}

// Draw a texture layer's latest frame at the canvas origin, scaled to the layer's WxH (the canvas is
// already positioned + clipped at the TextureView by the enclosing View.draw). Null frame -> nothing.
void draw_texture_layer_immediate(SkCanvas* c, AclTextureLayer* l) {
  static const bool walk_dbg = std::getenv("ACL_WALK_DEBUG") != nullptr;
  static std::atomic<int> ldbg{60};
  const bool dbg = walk_dbg && ldbg.fetch_sub(1, std::memory_order_relaxed) > 0;
  if (c == nullptr || l == nullptr || l->st == nullptr) {
    if (dbg) std::fprintf(stderr, "[acl.walk] layer draw SKIP c=%p l=%p st=%p\n",
                          static_cast<void*>(c), static_cast<void*>(l),
                          static_cast<void*>(l ? l->st : nullptr));
    return;
  }
  sk_sp<SkImage> img = l->st->snapshot();
  if (dbg) std::fprintf(stderr, "[acl.walk] layer draw l=%p wh=%dx%d img=%p %dx%d\n",
                        static_cast<void*>(l), l->width, l->height, static_cast<void*>(img.get()),
                        img ? img->width() : 0, img ? img->height() : 0);
  if (!img) { g_tl_blanks.fetch_add(1, std::memory_order_relaxed); return; }
  g_tl_frames.fetch_add(1, std::memory_order_relaxed);
  const float w = l->width > 0 ? static_cast<float>(l->width) : static_cast<float>(img->width());
  const float h = l->height > 0 ? static_cast<float>(l->height) : static_cast<float>(img->height());
  SkPaint p;
  p.setAlphaf(l->alpha);
  c->drawImageRect(img.get(),
                   SkRect::MakeWH(static_cast<float>(img->width()), static_cast<float>(img->height())),
                   SkRect::MakeWH(w, h), SkSamplingOptions(SkFilterMode::kLinear),
                   l->alpha < 1.0f ? &p : nullptr, SkCanvas::kFast_SrcRectConstraint);
}

// Resolve-at-replay drawable: reads the layer's CURRENT frame at replay time, so a producing
// TextureView shows its latest content (mirrors AclRenderNodeDrawable). Holds a ref on the layer so
// it can't be freed by a Java-side decStrong between record and replay.
class AclTextureLayerDrawable : public SkDrawable {
 public:
  AclTextureLayerDrawable(AclTextureLayer* l, SkRect bounds) : layer_(l), bounds_(bounds) {
    if (layer_ != nullptr) layer_->incStrong(this);
  }
  ~AclTextureLayerDrawable() override {
    if (layer_ != nullptr) layer_->decStrong(this);
  }
 protected:
  SkRect onGetBounds() override { return bounds_; }
  void onDraw(SkCanvas* c) override { draw_texture_layer_immediate(c, layer_); }
 private:
  AclTextureLayer* layer_;
  SkRect bounds_;
};

// Draw a texture layer onto `c`: deferred (resolve-at-replay) onto a recording canvas, immediate onto
// the GPU/present canvas — same discipline as draw_rendernode.
void draw_texture_layer(SkCanvas* c, jlong layer) {
  auto* l = tlayer(layer);
  if (c == nullptr || l == nullptr) return;
  const SkRect bounds = SkRect::MakeWH(l->width > 0 ? static_cast<float>(l->width) : 1.0f,
                                       l->height > 0 ? static_cast<float>(l->height) : 1.0f);
  if (is_recording_canvas(c)) {
    auto d = sk_make_sp<AclTextureLayerDrawable>(l, bounds);
    c->drawDrawable(d.get());
  } else {
    draw_texture_layer_immediate(c, l);
  }
}

// ── Producer feed: the runtime routes an app's GLThread-rendered TextureView frame here ──────────
// A SurfaceTexture's producer (eglCreateWindowSurfaceTexture -> an offscreen GL window) hands each
// rendered frame's pixels to acl-side, which calls this with the AclSurfaceTexture handle (read from
// SurfaceTexture.mSurfaceTexture). Software path: a CPU-copied raster SkImage (immutable, built on
// the producer thread) latched as `current`, so the TextureLayer drawable draws it at the next
// present. `rgba` is tightly packed, top-down RGBA_8888. Called from acl runtime via dlsym.
extern "C" void acl_graphics_surfacetexture_set_pixels(jlong st_handle, const void* rgba, int w,
                                                       int h, int stride) {
  auto* st = reinterpret_cast<AclSurfaceTexture*>(st_handle);
  if (st == nullptr || rgba == nullptr || w <= 0 || h <= 0) return;
  const SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kOpaque_SkAlphaType);
  const SkPixmap pm(info, rgba, stride > 0 ? static_cast<size_t>(stride) : info.minRowBytes());
  sk_sp<SkImage> img = SkImage::MakeRasterCopy(pm);
  if (!img) return;
  { std::lock_guard<std::mutex> lk(st->mu); st->current = std::move(img); }  // present reads via snapshot()
  g_st_produced.fetch_add(1, std::memory_order_relaxed);  // signal the present loop to re-record
  bump_scene();  // a TextureView produced a new frame -> the scene changed
  post_frame_available(st);  // -> TextureView.invalidate() -> re-record so the drawable re-snapshots
}
// A monotonically increasing count of produced SurfaceTexture frames. The present loop force-records
// the frame after any TextureView produced, so cached ANCESTOR pictures re-snapshot the resolve-at-
// replay layer drawable (SkPicture snapshots drawables, so a cached ancestor otherwise holds a stale
// frame). Self-limiting: only while a TextureView animates. Read via dlsym by the runtime.
extern "C" std::uint64_t acl_graphics_surfacetexture_produced() {
  return g_st_produced.load(std::memory_order_relaxed);
}
// The SurfaceTexture's default buffer size (TextureView set it to the view's WxH before the app's
// GLThread created the producer surface), so acl can size the offscreen GL window to match — no
// aspect distortion. false if unset. Called from acl runtime via dlsym.
extern "C" bool acl_graphics_surfacetexture_default_size(jlong st_handle, int* w, int* h) {
  auto* st = reinterpret_cast<AclSurfaceTexture*>(st_handle);
  if (st == nullptr) return false;
  std::lock_guard<std::mutex> lk(st->mu);
  if (st->def_w <= 0 || st->def_h <= 0) return false;
  if (w != nullptr) *w = st->def_w;
  if (h != nullptr) *h = st->def_h;
  return true;
}

// android.graphics.SurfaceTexture natives (instance methods; regular JNI, none @CriticalNative).
void SurfaceTexture_nativeInit(JNIEnv* env, jobject thiz, jboolean isDetached, jint texName,
                               jboolean /*singleBuffer*/, jobject weakSelf) {
  auto* st = new AclSurfaceTexture();
  st->detached = (isDetached == JNI_TRUE);
  st->tex_name = texName;
  st->weak_self = weakSelf != nullptr ? env->NewGlobalRef(weakSelf) : nullptr;
  if (g_st_field != nullptr) env->SetLongField(thiz, g_st_field, reinterpret_cast<jlong>(st));
  if (g_st_post == nullptr) {  // cache the frame-available notify seam once
    env->GetJavaVM(&g_st_vm);
    jclass c = env->GetObjectClass(thiz);
    g_st_class = static_cast<jclass>(env->NewGlobalRef(c));
    g_st_post = env->GetStaticMethodID(c, "postEventFromNative", "(Ljava/lang/ref/WeakReference;)V");
    env->DeleteLocalRef(c);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  g_st_created.fetch_add(1, std::memory_order_relaxed);
}
void SurfaceTexture_nativeFinalize(JNIEnv* env, jobject thiz) {
  auto* st = st_from(env, thiz);
  if (st == nullptr) return;
  if (st->weak_self != nullptr) env->DeleteGlobalRef(st->weak_self);
  delete st;
  if (g_st_field != nullptr) env->SetLongField(thiz, g_st_field, 0);
}
void SurfaceTexture_nativeGetTransformMatrix(JNIEnv* env, jobject, jfloatArray mtx) {
  // Identity 4x4 (column-major): acl's SkImage frame is already upright — unlike a GL BufferQueue,
  // whose OES transform flips Y and crops. Apps multiply this into their texture coords.
  static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  if (mtx != nullptr && env->GetArrayLength(mtx) >= 16) env->SetFloatArrayRegion(mtx, 0, 16, ident);
}
jlong SurfaceTexture_nativeGetTimestamp(JNIEnv* env, jobject thiz) {
  auto* st = st_from(env, thiz);
  return st != nullptr ? st->timestamp : 0;
}
jint SurfaceTexture_nativeGetDataSpace(JNIEnv*, jobject) { return 0; }  // DataSpace.DATASPACE_UNKNOWN
void SurfaceTexture_nativeSetDefaultBufferSize(JNIEnv* env, jobject thiz, jint w, jint h) {
  auto* st = st_from(env, thiz);
  if (st != nullptr) { st->def_w = w; st->def_h = h; }
}
void SurfaceTexture_nativeUpdateTexImage(JNIEnv* env, jobject thiz) {
  auto* st = st_from(env, thiz);
  if (st != nullptr) st->latch();
}
void SurfaceTexture_nativeReleaseTexImage(JNIEnv*, jobject) {}
jint SurfaceTexture_nativeDetachFromGLContext(JNIEnv* env, jobject thiz) {
  auto* st = st_from(env, thiz);
  if (st != nullptr) st->detached = true;
  return 0;  // NO_ERROR
}
jint SurfaceTexture_nativeAttachToGLContext(JNIEnv* env, jobject thiz, jint texName) {
  auto* st = st_from(env, thiz);
  if (st != nullptr) { st->detached = false; st->tex_name = texName; }
  return 0;
}
void SurfaceTexture_nativeRelease(JNIEnv* env, jobject thiz) {
  auto* st = st_from(env, thiz);
  if (st == nullptr) return;
  std::lock_guard<std::mutex> lk(st->mu);
  st->released = true;
  st->pending = nullptr;
  st->current = nullptr;
}
jboolean SurfaceTexture_nativeIsReleased(JNIEnv* env, jobject thiz) {
  auto* st = st_from(env, thiz);
  return (st != nullptr && st->released) ? JNI_TRUE : JNI_FALSE;
}

// android.graphics.TextureLayer natives (static; layerUpdater is the AclTextureLayer handle).
jboolean TextureLayer_nPrepare(JNIEnv*, jclass, jlong layer, jint w, jint h, jboolean /*opaque*/) {
  auto* l = tlayer(layer);
  if (l != nullptr) { l->width = w; l->height = h; }
  return JNI_TRUE;  // "layer changed" — cheap; acl re-records the layer draw every frame anyway
}
void TextureLayer_nSetLayerPaint(JNIEnv*, jclass, jlong layer, jlong paint) {
  auto* l = tlayer(layer);
  if (l == nullptr) return;
  auto* ap = reinterpret_cast<AclPaint*>(paint);
  l->alpha = ap != nullptr ? ap->paint.getAlphaf() : 1.0f;
}
void TextureLayer_nSetTransform(JNIEnv*, jclass, jlong, jlong) {}  // TextureView.setTransform: staged
void TextureLayer_nSetSurfaceTexture(JNIEnv* env, jclass, jlong layer, jobject stObj) {
  auto* l = tlayer(layer);
  if (l != nullptr) l->st = stObj != nullptr ? st_from(env, stObj) : nullptr;
}
void TextureLayer_nUpdateSurfaceTexture(JNIEnv*, jclass, jlong layer) {
  auto* l = tlayer(layer);
  if (l != nullptr && l->st != nullptr) l->st->latch();
}

// android.graphics.HardwareRenderer layer natives (static; nativeProxy ignored — acl has no RT proxy).
jlong HardwareRenderer_nCreateTextureLayer(JNIEnv*, jclass, jlong /*proxy*/) {
  return reinterpret_cast<jlong>(new AclTextureLayer());  // TextureLayer.<init> incStrongs via VirtualRefBasePtr
}
void HardwareRenderer_nBuildLayer(JNIEnv*, jclass, jlong, jlong) {}
jboolean HardwareRenderer_nCopyLayerInto(JNIEnv*, jclass, jlong, jlong, jlong) { return JNI_FALSE; }
void HardwareRenderer_nPushLayerUpdate(JNIEnv*, jclass, jlong, jlong layer) {
  auto* l = tlayer(layer);
  if (l != nullptr && l->st != nullptr) l->st->latch();
}
void HardwareRenderer_nCancelLayerUpdate(JNIEnv*, jclass, jlong, jlong) {}
void HardwareRenderer_nDetachSurfaceTexture(JNIEnv*, jclass, jlong, jlong layer) {
  auto* l = tlayer(layer);
  if (l != nullptr) l->st = nullptr;
}

// android.view.TextureView natives — the producer side of a TextureView's own SurfaceTexture. The
// "native window" is the producer end of the SAME stream (Surface(SurfaceTexture) / GL / lockCanvas
// feed frames in), so mNativeWindow just references the AclSurfaceTexture (NOT owned — the
// SurfaceTexture owns it via mSurfaceTexture/nativeFinalize). Stored so getTextureLayer completes and
// TextureView.lockCanvas can find the stream (the software producer is Stage 2 — nLockCanvas is a
// truthful "can't lock yet" for now, so lockCanvas() returns null rather than crashing the traversal).
jfieldID g_tv_window_field = nullptr;  // TextureView.mNativeWindow (long)
void TextureView_nCreateNativeWindow(JNIEnv* env, jobject thiz, jobject stObj) {
  AclSurfaceTexture* st = stObj != nullptr ? st_from(env, stObj) : nullptr;
  if (g_tv_window_field != nullptr) env->SetLongField(thiz, g_tv_window_field, reinterpret_cast<jlong>(st));
}
void TextureView_nDestroyNativeWindow(JNIEnv* env, jobject thiz) {
  if (g_tv_window_field != nullptr) env->SetLongField(thiz, g_tv_window_field, 0);
}
jboolean TextureView_nLockCanvas(JNIEnv*, jclass, jlong, jobject, jobject) { return JNI_FALSE; }
void TextureView_nUnlockCanvasAndPost(JNIEnv*, jclass, jlong, jobject) {}

// NOTE: every RecordingCanvas native is @CriticalNative — the JVM calls it with the raw C ABI and
// passes ONLY the declared Java args (NO JNIEnv*, NO jclass). These signatures must therefore start
// at the first Java parameter, or the args land two registers off. (BaseRecordingCanvas natives are
// @FastNative, which keeps the normal env/jclass convention — hence kBaseCanvas is reused for them.)

// RecordingCanvas.nCreateDisplayListCanvas(node, w, h): the present-root node returns the GPU
// SkCanvas (immediate); any other node gets a fresh SkPictureRecorder (records into a picture).
jlong RecordingCanvas_nCreate(jlong node, jint w, jint h) {
  auto* n = reinterpret_cast<AclRenderNode*>(node);
  if (n != nullptr && n->is_present_root) {
    // Deferred present: record into a picture (replayed at gpu_end). The recorder is created once
    // and its canvas ptr is stable across begin/finish; nTraverse drives begin/finish each frame.
    if (g_present_defer.load(std::memory_order_relaxed)) {
      if (g_present_recorder == nullptr) {
        g_present_recorder = new SkPictureRecorder();
        g_present_record_canvas =
            g_present_recorder->beginRecording(SkRect::MakeIWH(w > 0 ? w : 1, h > 0 ? h : 1));
        std::lock_guard<std::mutex> lk(g_rec_mu);
        g_recorders[g_present_record_canvas] = g_present_recorder;
      }
      return reinterpret_cast<jlong>(g_present_record_canvas);
    }
    if (g_present_gpu_canvas != 0) return g_present_gpu_canvas;
  }
  g_rn_begins.fetch_add(1, std::memory_order_relaxed);
  // A NodeRecorder: records segmented content (own ops + live child/layer boundaries). Its handle is
  // tagged so canvas_sk() resolves it to the current segment; stable across the boundary splits.
  auto* nr = new NodeRecorder(w, h);
  return reinterpret_cast<jlong>(nr) | kRecorderTag;
}
// nResetDisplayListCanvas(canvas, node, w, h): re-begin recording (clears the prior display list).
void RecordingCanvas_nReset(jlong canvas, jlong /*node*/, jint w, jint h) {
  if (is_recorder_handle(canvas)) { recorder_of(canvas)->reset(w, h); return; }
  if (canvas == g_present_gpu_canvas) return;  // root draws immediate; nothing to reset
  auto* c = canvas_sk(canvas);  // deferred present-root record canvas (g_present_record_canvas)
  std::lock_guard<std::mutex> lk(g_rec_mu);
  auto it = g_recorders.find(c);
  if (it != g_recorders.end()) it->second->beginRecording(SkRect::MakeIWH(w > 0 ? w : 1, h > 0 ? h : 1));
}
// nFinishRecording(canvas, node): finalize the recording into the node's display list.
void RecordingCanvas_nFinish(jlong canvas, jlong node) {
  g_rn_finishes.fetch_add(1, std::memory_order_relaxed);
  auto* n = reinterpret_cast<AclRenderNode*>(node);
  if (is_recorder_handle(canvas)) {
    std::vector<DisplayItem> items = recorder_of(canvas)->finish();
    if (n != nullptr) { n->display_list = std::move(items); n->picture = nullptr; }
    bump_scene();  // a node re-recorded its content -> the frame changed
    return;
  }
  if (canvas == g_present_gpu_canvas) return;  // root: content already drawn immediate
  auto* c = canvas_sk(canvas);
  std::lock_guard<std::mutex> lk(g_rec_mu);
  auto it = g_recorders.find(c);
  if (it != g_recorders.end() && n != nullptr) n->picture = it->second->finishRecordingAsPicture();
}
// nDrawRenderNode(canvas, node): record a live child boundary into the recording, or (on the present-
// root GPU/deferred canvas) replay the node — draw_rendernode walks it live / defers it.
void RecordingCanvas_nDrawRenderNode(jlong canvas, jlong node) {
  g_rn_replays.fetch_add(1, std::memory_order_relaxed);
  tick_draw();
  if (is_recorder_handle(canvas)) {
    recorder_of(canvas)->boundary_child(reinterpret_cast<AclRenderNode*>(node));
    return;
  }
  draw_rendernode(canvas_sk(canvas), reinterpret_cast<AclRenderNode*>(node));
}
// RecordingCanvas.enableZ(true)/disableZ(false): the framework brackets a ViewGroup's children with
// this so hwui draws them in Z order and casts elevation shadows. Mark the recorder so child
// boundaries recorded in the region are Z-ordered by the walk.
void RecordingCanvas_nEnableZ(jlong canvas, jboolean enable) {
  if (is_recorder_handle(canvas)) recorder_of(canvas)->z_enabled = (enable == JNI_TRUE);
}
// RecordingCanvas.drawTextureLayer(layer): composite a TextureView's SurfaceTexture frame into the
// display list (@CriticalNative — canvas, layer only). Draws the layer's latest frame, or nothing
// until a producer feeds one (Stage 2) — either way the traversal completes.
void RecordingCanvas_nDrawTextureLayer(jlong canvas, jlong layer) {
  tick_draw();
  if (is_recorder_handle(canvas)) {
    recorder_of(canvas)->boundary_layer(tlayer(layer));
    return;
  }
  draw_texture_layer(canvas_sk(canvas), layer);
}
jint RecordingCanvas_nGetMaximumTextureWidth() { return 8192; }
jint RecordingCanvas_nGetMaximumTextureHeight() { return 8192; }
// RecordingCanvas' hardware-only draws take animatable CanvasProperty handles (ripple/reveal) or a
// WebView functor — acl has no display-list animator, so these are no-op stubs (no crash; the
// static content still renders through the BaseRecordingCanvas.nDraw* path). All @CriticalNative.
void RecordingCanvas_nDrawCircleProp(jlong, jlong, jlong, jlong, jlong) {}
void RecordingCanvas_nDrawRoundRectProp(jlong, jlong, jlong, jlong, jlong, jlong, jlong, jlong) {}
void RecordingCanvas_nDrawRipple(jlong, jlong, jlong, jlong, jlong, jlong, jlong, jint, jlong) {}
// The WebView hardware draw: RecordingCanvas.drawWebViewFunctor(functor) lands here on acl's present-
// root canvas. Route the functor into acl's Ganesh-Vulkan present via the AwDrawFn Vulkan path
// (webview_functor.cpp): draw an SkDrawable whose Skia GPU flush hands Chromium the frame's secondary
// command buffer to record into. No-op if the VK seam isn't wired (GL mode / hooks unset) — the WebView
// area is then left unpainted rather than crashing.
extern "C" void acl_graphics_present_record_functor(void* sk_canvas, int functor);
void RecordingCanvas_nDrawWebViewFunctor(jlong canvas, jint functor) {
  // Deferred present: the functor SkDrawable would be lost in the picture snapshot (its onDraw is a
  // no-op), so capture its placement now and draw it directly onto the GPU canvas at replay.
  if (g_present_defer.load(std::memory_order_relaxed))
    acl_graphics_present_record_functor(reinterpret_cast<void*>(canvas), static_cast<int>(functor));
  else
    acl_graphics_draw_webview_functor(reinterpret_cast<void*>(canvas), static_cast<int>(functor));
}
// nPunchHole (SurfaceView transparency) is @FastNative on BaseRecordingCanvas -> keep env/jclass.
void BaseRecordingCanvas_nPunchHole(JNIEnv*, jclass, jlong, jfloat, jfloat, jfloat, jfloat, jfloat, jfloat) {}

const JNINativeMethod kBaseRecordingExtra[] = {
    {"nPunchHole", "(JFFFFFF)V", reinterpret_cast<void*>(BaseRecordingCanvas_nPunchHole)},
};

// android.graphics.CanvasProperty — an animatable float/paint holder for RecordingCanvas' ripple/
// reveal draws (RippleDrawable). acl has no display-list animator (nDrawCircleProp is a no-op), so
// the value is never read; the handle only has to be a real VirtualLightRefBase because
// CanvasProperty wraps it in a VirtualRefBasePtr that inc/decStrongs it on finalize.
const JNINativeMethod kCanvasProperty[] = {
    {"nCreateFloat", "(F)J", reinterpret_cast<void*>(+[](JNIEnv*, jclass, jfloat) -> jlong {
       return reinterpret_cast<jlong>(new VirtualLightRefBase()); })},
    {"nCreatePaint", "(J)J", reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong) -> jlong {
       return reinterpret_cast<jlong>(new VirtualLightRefBase()); })},
};

// android.graphics.animation.RenderNodeAnimator — the RenderThread-driven property animator behind
// Material ripples, circular reveals, and property (alpha/translation/scale) animations. acl re-owns
// the render stack (hwui dropped), and there is no display-list animator on the RenderThread, so the
// animated value is never read: the paired CanvasProperty/RecordingCanvas draws (nDrawRipple/
// nDrawCircleProp) are no-ops. Stub the whole seam so an app that starts one does not hit an
// UnsatisfiedLinkError mid-draw (which threw every frame and froze the screen on the transition —
// e.g. tapping a list item's ripple). The native ptr is wrapped in a VirtualRefBasePtr (inc/decStrong
// on finalize), so each creator returns a real ref-counted VirtualLightRefBase; control calls are
// no-ops. The animation never fires callOnFinished, so it stays "running" and draws nothing —
// harmless (a ripple/reveal simply shows its end state). nGetDuration returns the AOSP default.
const JNINativeMethod kRenderNodeAnimator[] = {
    {"nCreateAnimator", "(IF)J", reinterpret_cast<void*>(+[](JNIEnv*, jclass, jint, jfloat) -> jlong {
       return reinterpret_cast<jlong>(new VirtualLightRefBase()); })},
    {"nCreateCanvasPropertyFloatAnimator", "(JF)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jfloat) -> jlong {
       return reinterpret_cast<jlong>(new VirtualLightRefBase()); })},
    {"nCreateCanvasPropertyPaintAnimator", "(JIF)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jint, jfloat) -> jlong {
       return reinterpret_cast<jlong>(new VirtualLightRefBase()); })},
    {"nCreateRevealAnimator", "(IIFF)J",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jint, jint, jfloat, jfloat) -> jlong {
       return reinterpret_cast<jlong>(new VirtualLightRefBase()); })},
    {"nSetStartValue", "(JF)V", reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jfloat) {})},
    {"nSetDuration", "(JJ)V", reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jlong) {})},
    {"nGetDuration", "(J)J", reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong) -> jlong {
       return 300; })},
    {"nSetStartDelay", "(JJ)V", reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jlong) {})},
    {"nSetInterpolator", "(JJ)V", reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jlong) {})},
    {"nSetAllowRunningAsync", "(JZ)V", reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jboolean) {})},
    {"nSetListener", "(JLandroid/graphics/animation/RenderNodeAnimator;)V",
     reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong, jobject) {})},
    {"nStart", "(J)V", reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong) {})},
    {"nEnd", "(J)V", reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong) {})},
};

// android.graphics.DashPathEffect -> SkDashPathEffect; the android.graphics.PathEffect base owns the
// native SkPathEffect* and frees it on finalize. Applied to a Paint by Paint.nSetPathEffect, so a
// dashed stroke (e.g. a grid separator) renders instead of throwing UnsatisfiedLinkError mid-draw.
jlong DashPathEffect_nativeCreate(JNIEnv* env, jclass, jfloatArray intervals, jfloat phase) {
  if (intervals == nullptr) return 0;
  const jsize count = env->GetArrayLength(intervals);
  if (count < 2 || (count & 1)) return 0;  // Skia requires an even interval count >= 2
  std::vector<SkScalar> iv(static_cast<size_t>(count));
  env->GetFloatArrayRegion(intervals, 0, count, iv.data());
  sk_sp<SkPathEffect> e = SkDashPathEffect::Make(iv.data(), count, phase);
  return reinterpret_cast<jlong>(e.release());  // may be null (Java tolerates 0)
}
void PathEffect_nativeDestructor(JNIEnv*, jclass, jlong ptr) {
  SkSafeUnref(reinterpret_cast<SkPathEffect*>(ptr));
}
// android.graphics.CornerPathEffect -> SkCornerPathEffect (rounds path corners by `radius`).
// Telegram builds rounded UI shapes with it in LaunchActivity.onCreate; without it the native
// throws UnsatisfiedLinkError and onCreate fails. Same PathEffect base owns/frees the handle.
jlong CornerPathEffect_nativeCreate(JNIEnv*, jclass, jfloat radius) {
  sk_sp<SkPathEffect> e = SkCornerPathEffect::Make(radius);
  return reinterpret_cast<jlong>(e.release());  // may be null (radius<=0); Java tolerates 0
}

// android.graphics.Color HSV conversions. The host libandroid_runtime does not register the
// graphics JNI, so Color.HSVToColor/RGBToHSV (e.g. an app picking a random stroke color) throws
// UnsatisfiedLinkError mid-onTouchEvent — which propagates out of dispatchTouchEvent and looks like
// dropped input. Back them with Skia's SkHSVToColor / SkRGBToHSV.
jint Color_nativeHSVToColor(JNIEnv* env, jclass, jint alpha, jfloatArray hsvArray) {
  if (hsvArray == nullptr || env->GetArrayLength(hsvArray) < 3) return 0;
  jfloat buf[3];
  env->GetFloatArrayRegion(hsvArray, 0, 3, buf);
  const SkScalar hsv[3] = {buf[0], buf[1], buf[2]};
  return static_cast<jint>(SkHSVToColor(static_cast<U8CPU>(alpha), hsv));
}
void Color_nativeRGBToHSV(JNIEnv* env, jclass, jint r, jint g, jint b, jfloatArray hsvArray) {
  if (hsvArray == nullptr || env->GetArrayLength(hsvArray) < 3) return;
  SkScalar hsv[3];
  SkRGBToHSV(static_cast<U8CPU>(r), static_cast<U8CPU>(g), static_cast<U8CPU>(b), hsv);
  const jfloat buf[3] = {hsv[0], hsv[1], hsv[2]};
  env->SetFloatArrayRegion(hsvArray, 0, 3, buf);
}
const JNINativeMethod kColor[] = {
    {"nativeHSVToColor", "(I[F)I", reinterpret_cast<void*>(Color_nativeHSVToColor)},
    {"nativeRGBToHSV", "(III[F)V", reinterpret_cast<void*>(Color_nativeRGBToHSV)},
};
const JNINativeMethod kDashPathEffect[] = {
    {"nativeCreate", "([FF)J", reinterpret_cast<void*>(DashPathEffect_nativeCreate)},
};
const JNINativeMethod kCornerPathEffect[] = {
    {"nativeCreate", "(F)J", reinterpret_cast<void*>(CornerPathEffect_nativeCreate)},
};
const JNINativeMethod kPathEffect[] = {
    {"nativeDestructor", "(J)V", reinterpret_cast<void*>(PathEffect_nativeDestructor)},
};

const JNINativeMethod kRecordingCanvas[] = {
    {"nCreateDisplayListCanvas", "(JII)J", reinterpret_cast<void*>(RecordingCanvas_nCreate)},
    {"nResetDisplayListCanvas", "(JJII)V", reinterpret_cast<void*>(RecordingCanvas_nReset)},
    {"nFinishRecording", "(JJ)V", reinterpret_cast<void*>(RecordingCanvas_nFinish)},
    {"nDrawRenderNode", "(JJ)V", reinterpret_cast<void*>(RecordingCanvas_nDrawRenderNode)},
    {"nEnableZ", "(JZ)V", reinterpret_cast<void*>(RecordingCanvas_nEnableZ)},
    {"nDrawTextureLayer", "(JJ)V", reinterpret_cast<void*>(RecordingCanvas_nDrawTextureLayer)},
    {"nGetMaximumTextureWidth", "()I", reinterpret_cast<void*>(RecordingCanvas_nGetMaximumTextureWidth)},
    {"nGetMaximumTextureHeight", "()I", reinterpret_cast<void*>(RecordingCanvas_nGetMaximumTextureHeight)},
    {"nDrawCircle", "(JJJJJ)V", reinterpret_cast<void*>(RecordingCanvas_nDrawCircleProp)},
    {"nDrawRoundRect", "(JJJJJJJJ)V", reinterpret_cast<void*>(RecordingCanvas_nDrawRoundRectProp)},
    {"nDrawRipple", "(JJJJJJJIJ)V", reinterpret_cast<void*>(RecordingCanvas_nDrawRipple)},
    {"nDrawWebViewFunctor", "(JI)V", reinterpret_cast<void*>(RecordingCanvas_nDrawWebViewFunctor)},
};

// ── Text measurement + line breaking (android.graphics.text.*) ────────────────────────────────
// Compose's text stack drives layout through MeasuredText (per-char advances + font metrics) and
// LineBreaker (greedy line breaking over those advances). acl backs both with Skia SkFont — the same
// font/size each run's Paint carries — so text composes/measures/wraps. Advances are per-UTF-16 code
// unit (measured individually); adequate for layout width without full cluster shaping. The nInit/
// nAddStyleRun/nBuild/nComputeLineBreaks natives are normal JNI (env/jclass); the width/metric/
// line getters + release funcs are @CriticalNative (handle-only) — see the ABI note on RecordingCanvas.
struct AclMtRun { const AclPaint* paint; int start; int end; bool replacement; float replaceWidth; };
struct AclMtBuilder { std::vector<AclMtRun> runs; };
struct AclMeasuredText {
  std::vector<float> adv;         // advance per UTF-16 code unit
  float ascent = 0, descent = 0;  // Skia convention: ascent negative, descent positive
};
struct AclLineBreaker { int breakStrategy = 0; bool justified = false; };
struct AclLbResult {
  std::vector<int> offset;        // break offset (exclusive end index) per line
  std::vector<float> width, ascent, descent;
  std::vector<int> flag;
};
extern "C" void acl_free_measuredtext(void* p) { delete reinterpret_cast<AclMeasuredText*>(p); }
extern "C" void acl_free_linebreaker(void* p) { delete reinterpret_cast<AclLineBreaker*>(p); }
extern "C" void acl_free_lbresult(void* p) { delete reinterpret_cast<AclLbResult*>(p); }

jlong MtBuilder_nInit(JNIEnv*, jclass) { return reinterpret_cast<jlong>(new AclMtBuilder()); }
void MtBuilder_nAddStyleRun(JNIEnv*, jclass, jlong b, jlong paint, jint, jint, jint start, jint end, jboolean) {
  if (auto* bd = reinterpret_cast<AclMtBuilder*>(b))
    bd->runs.push_back({reinterpret_cast<AclPaint*>(paint), start, end, false, 0.f});
}
void MtBuilder_nAddReplacementRun(JNIEnv*, jclass, jlong b, jlong paint, jint start, jint end, jfloat w) {
  if (auto* bd = reinterpret_cast<AclMtBuilder*>(b))
    bd->runs.push_back({reinterpret_cast<AclPaint*>(paint), start, end, true, w});
}
jlong MtBuilder_nBuild(JNIEnv* env, jclass, jlong b, jlong /*hint*/, jcharArray text, jboolean, jboolean, jboolean) {
  auto* bd = reinterpret_cast<AclMtBuilder*>(b);
  auto* mt = new AclMeasuredText();
  const jsize n = text ? env->GetArrayLength(text) : 0;
  mt->adv.assign(n, 0.f);
  jchar* chars = n ? env->GetCharArrayElements(text, nullptr) : nullptr;
  if (bd != nullptr && chars != nullptr) {
    for (const auto& r : bd->runs) {
      const int s = std::max(0, r.start), e = std::min<int>(n, r.end);
      if (r.replacement) { if (s < e) mt->adv[s] = r.replaceWidth; continue; }
      const AclPaint* ap = r.paint;
      if (ap == nullptr) continue;
      SkFontMetrics fm;
      ap->font.getMetrics(&fm);
      mt->ascent = std::min(mt->ascent, fm.fAscent);
      mt->descent = std::max(mt->descent, fm.fDescent);
      for (int i = s; i < e; ++i) {
        const jchar u = chars[i];
        mt->adv[i] = ap->font.measureText(&u, sizeof(jchar), SkTextEncoding::kUTF16, nullptr, &ap->paint);
      }
    }
  }
  if (chars != nullptr) env->ReleaseCharArrayElements(text, chars, JNI_ABORT);
  if (mt->ascent == 0 && mt->descent == 0) { mt->ascent = -12.f; mt->descent = 4.f; }  // fallback metrics
  return reinterpret_cast<jlong>(mt);
}
void MtBuilder_nFree(JNIEnv*, jclass, jlong b) { delete reinterpret_cast<AclMtBuilder*>(b); }

jfloat MeasuredText_nGetWidth(jlong p, jint start, jint end) {
  auto* mt = reinterpret_cast<AclMeasuredText*>(p);
  if (mt == nullptr) return 0.f;
  const int n = static_cast<int>(mt->adv.size());
  float w = 0.f;
  for (int i = std::max(0, start); i < std::min(n, end); ++i) w += mt->adv[i];
  return w;
}
jfloat MeasuredText_nGetCharWidthAt(jlong p, jint off) {
  auto* mt = reinterpret_cast<AclMeasuredText*>(p);
  return (mt != nullptr && off >= 0 && off < static_cast<int>(mt->adv.size())) ? mt->adv[off] : 0.f;
}
jint MeasuredText_nGetMemoryUsage(jlong p) {
  auto* mt = reinterpret_cast<AclMeasuredText*>(p);
  return mt ? static_cast<jint>(mt->adv.size() * sizeof(float)) : 0;
}
jlong MeasuredText_nGetReleaseFunc() { return reinterpret_cast<jlong>(&acl_free_measuredtext); }
void MeasuredText_nGetBounds(JNIEnv* env, jclass, jlong p, jcharArray, jint start, jint end, jobject rect) {
  if (rect == nullptr) return;
  auto* mt = reinterpret_cast<AclMeasuredText*>(p);
  jclass rc = env->GetObjectClass(rect);
  const float w = MeasuredText_nGetWidth(p, start, end);
  env->SetIntField(rect, env->GetFieldID(rc, "left", "I"), 0);
  env->SetIntField(rect, env->GetFieldID(rc, "top", "I"), mt ? static_cast<jint>(std::floor(mt->ascent)) : 0);
  env->SetIntField(rect, env->GetFieldID(rc, "right", "I"), static_cast<jint>(std::ceil(w)));
  env->SetIntField(rect, env->GetFieldID(rc, "bottom", "I"), mt ? static_cast<jint>(std::ceil(mt->descent)) : 0);
}

jlong LineBreaker_nInit(JNIEnv*, jclass, jint breakStrategy, jint, jboolean justified, jintArray) {
  auto* lb = new AclLineBreaker();
  lb->breakStrategy = breakStrategy;
  lb->justified = justified;
  return reinterpret_cast<jlong>(lb);
}
jlong LineBreaker_nGetReleaseFunc() { return reinterpret_cast<jlong>(&acl_free_linebreaker); }
jlong LineBreaker_nGetReleaseResultFunc() { return reinterpret_cast<jlong>(&acl_free_lbresult); }
// Greedy line break over the MeasuredText advances: accumulate width, wrap at the last space that
// fit (or hard-break on '\n'). firstWidth applies to the first firstWidthLineCount lines, restWidth
// after. Good enough for Compose Text/paragraph layout; not Knuth-Plass optimal.
jlong LineBreaker_nComputeLineBreaks(JNIEnv* env, jclass, jlong /*lb*/, jcharArray text, jlong mtPtr,
                                     jint length, jfloat firstWidth, jint firstWidthLineCount,
                                     jfloat restWidth, jfloatArray, jfloat, jint) {
  auto* mt = reinterpret_cast<AclMeasuredText*>(mtPtr);
  auto* res = new AclLbResult();
  const float asc = mt ? mt->ascent : -12.f, desc = mt ? mt->descent : 4.f;
  jchar* chars = (text != nullptr && length > 0) ? env->GetCharArrayElements(text, nullptr) : nullptr;
  const auto advAt = [&](int i) -> float {
    return (mt != nullptr && i >= 0 && i < static_cast<int>(mt->adv.size())) ? mt->adv[i] : 0.f;
  };
  const auto emit = [&](int end, float w) {
    res->offset.push_back(end);
    res->width.push_back(w);
    res->ascent.push_back(asc);
    res->descent.push_back(desc);
    res->flag.push_back(0);
  };
  int lineStart = 0, lineNo = 0, lastBreak = -1;
  float lineW = 0.f, breakW = 0.f;
  for (int i = 0; i < length; ++i) {
    const jchar u = chars ? chars[i] : 0;
    if (u == '\n') {  // hard break
      emit(i + 1, lineW);
      lineStart = i + 1; ++lineNo; lineW = 0.f; lastBreak = -1; breakW = 0.f;
      continue;
    }
    const float a = advAt(i);
    const float limit = (lineNo < firstWidthLineCount) ? firstWidth : restWidth;
    if (lineW + a > limit && lastBreak > lineStart) {  // wrap at last fitting space
      emit(lastBreak, breakW);
      lineStart = lastBreak; ++lineNo; lastBreak = -1; breakW = 0.f;
      lineW = 0.f;
      for (int k = lineStart; k <= i; ++k) lineW += advAt(k);
    } else {
      lineW += a;
    }
    if (u == ' ' || u == '\t') { lastBreak = i + 1; breakW = lineW; }
  }
  if (lineStart < length || res->offset.empty()) emit(length, lineW);  // trailing / empty line
  if (chars != nullptr) env->ReleaseCharArrayElements(text, chars, JNI_ABORT);
  return reinterpret_cast<jlong>(res);
}
jint LineBreaker_nGetLineCount(jlong p) {
  auto* r = reinterpret_cast<AclLbResult*>(p);
  return r ? static_cast<jint>(r->offset.size()) : 0;
}
jint LineBreaker_nGetLineBreakOffset(jlong p, jint i) {
  auto* r = reinterpret_cast<AclLbResult*>(p);
  return (r && i >= 0 && i < static_cast<int>(r->offset.size())) ? r->offset[i] : 0;
}
jfloat LineBreaker_nGetLineWidth(jlong p, jint i) {
  auto* r = reinterpret_cast<AclLbResult*>(p);
  return (r && i >= 0 && i < static_cast<int>(r->width.size())) ? r->width[i] : 0.f;
}
jfloat LineBreaker_nGetLineAscent(jlong p, jint i) {
  auto* r = reinterpret_cast<AclLbResult*>(p);
  return (r && i >= 0 && i < static_cast<int>(r->ascent.size())) ? r->ascent[i] : 0.f;
}
jfloat LineBreaker_nGetLineDescent(jlong p, jint i) {
  auto* r = reinterpret_cast<AclLbResult*>(p);
  return (r && i >= 0 && i < static_cast<int>(r->descent.size())) ? r->descent[i] : 0.f;
}
jint LineBreaker_nGetLineFlag(jlong p, jint i) {
  auto* r = reinterpret_cast<AclLbResult*>(p);
  return (r && i >= 0 && i < static_cast<int>(r->flag.size())) ? r->flag[i] : 0;
}

const JNINativeMethod kMeasuredTextBuilder[] = {
    {"nInitBuilder", "()J", reinterpret_cast<void*>(MtBuilder_nInit)},
    {"nAddStyleRun", "(JJIIIIZ)V", reinterpret_cast<void*>(MtBuilder_nAddStyleRun)},
    {"nAddReplacementRun", "(JJIIF)V", reinterpret_cast<void*>(MtBuilder_nAddReplacementRun)},
    {"nBuildMeasuredText", "(JJ[CZZZ)J", reinterpret_cast<void*>(MtBuilder_nBuild)},
    {"nFreeBuilder", "(J)V", reinterpret_cast<void*>(MtBuilder_nFree)},
};
const JNINativeMethod kMeasuredText[] = {
    {"nGetWidth", "(JII)F", reinterpret_cast<void*>(MeasuredText_nGetWidth)},
    {"nGetCharWidthAt", "(JI)F", reinterpret_cast<void*>(MeasuredText_nGetCharWidthAt)},
    {"nGetMemoryUsage", "(J)I", reinterpret_cast<void*>(MeasuredText_nGetMemoryUsage)},
    {"nGetReleaseFunc", "()J", reinterpret_cast<void*>(MeasuredText_nGetReleaseFunc)},
    {"nGetBounds", "(J[CIILandroid/graphics/Rect;)V", reinterpret_cast<void*>(MeasuredText_nGetBounds)},
};
const JNINativeMethod kLineBreaker[] = {
    {"nInit", "(IIZ[I)J", reinterpret_cast<void*>(LineBreaker_nInit)},
    {"nGetReleaseFunc", "()J", reinterpret_cast<void*>(LineBreaker_nGetReleaseFunc)},
    {"nComputeLineBreaks", "(J[CJIFIF[FFI)J", reinterpret_cast<void*>(LineBreaker_nComputeLineBreaks)},
    {"nGetLineCount", "(J)I", reinterpret_cast<void*>(LineBreaker_nGetLineCount)},
    {"nGetLineBreakOffset", "(JI)I", reinterpret_cast<void*>(LineBreaker_nGetLineBreakOffset)},
    {"nGetLineWidth", "(JI)F", reinterpret_cast<void*>(LineBreaker_nGetLineWidth)},
    {"nGetLineAscent", "(JI)F", reinterpret_cast<void*>(LineBreaker_nGetLineAscent)},
    {"nGetLineDescent", "(JI)F", reinterpret_cast<void*>(LineBreaker_nGetLineDescent)},
    {"nGetLineFlag", "(JI)I", reinterpret_cast<void*>(LineBreaker_nGetLineFlag)},
    {"nGetReleaseResultFunc", "()J", reinterpret_cast<void*>(LineBreaker_nGetReleaseResultFunc)},
};

// ── android.graphics.RuntimeShader (AGSL/SkSL runtime effects) ─────────────────────────────────
// The Material touch ripple (RippleShader), blur, and app custom shaders build an SkRuntimeEffect
// from an AGSL program, push uniforms, then make an SkShader. acl backs it with Skia's
// SkRuntimeEffect + SkRuntimeShaderBuilder. If the AGSL doesn't compile under this Skia (version
// skew), the builder keeps a null effect and makeShader yields a transparent shader so the draw
// completes without crashing (the effect just doesn't paint). All natives are normal JNI.
struct AclRuntimeShaderBuilder {
  sk_sp<SkRuntimeEffect> effect;
  SkRuntimeShaderBuilder* builder = nullptr;
  ~AclRuntimeShaderBuilder() { delete builder; }
};
extern "C" void acl_free_runtime_builder(void* p) { delete reinterpret_cast<AclRuntimeShaderBuilder*>(p); }

jlong RuntimeShader_nGetFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&acl_free_runtime_builder);
}
jlong RuntimeShader_nCreateBuilder(JNIEnv* env, jclass, jstring agsl) {
  auto* b = new AclRuntimeShaderBuilder();
  if (agsl != nullptr) {
    const char* s = env->GetStringUTFChars(agsl, nullptr);
    if (s != nullptr) {
      SkRuntimeEffect::Result r = SkRuntimeEffect::MakeForShader(SkString(s));
      env->ReleaseStringUTFChars(agsl, s);
      if (r.effect != nullptr) {
        b->effect = r.effect;
        b->builder = new SkRuntimeShaderBuilder(r.effect);
      }
    }
  }
  return reinterpret_cast<jlong>(b);
}
jlong RuntimeShader_nCreateShader(JNIEnv*, jclass, jlong builderPtr, jlong matrixPtr) {
  auto* b = reinterpret_cast<AclRuntimeShaderBuilder*>(builderPtr);
  sk_sp<SkShader> shader;
  if (b != nullptr && b->builder != nullptr) {
    shader = b->builder->makeShader(reinterpret_cast<const SkMatrix*>(matrixPtr));
  }
  if (shader == nullptr) shader = SkShaders::Color(SK_ColorTRANSPARENT);  // never crash the draw
  return reinterpret_cast<jlong>(shader.release());  // freed by the Shader finalizer (free_shader)
}
// uniform(name).set(vals, count): count is the ELEMENT count; a size mismatch is silently ignored
// (Skia returns false without writing), so a stale/absent uniform never corrupts state.
void RuntimeShader_nUpdateUniformsFloatArr(JNIEnv* env, jclass, jlong bp, jstring name,
                                           jfloatArray values, jboolean /*isColor*/) {
  auto* b = reinterpret_cast<AclRuntimeShaderBuilder*>(bp);
  if (b == nullptr || b->builder == nullptr || name == nullptr || values == nullptr) return;
  const char* nm = env->GetStringUTFChars(name, nullptr);
  const jsize n = env->GetArrayLength(values);
  jfloat* v = env->GetFloatArrayElements(values, nullptr);
  if (nm != nullptr && v != nullptr) b->builder->uniform(nm).set(v, static_cast<int>(n));
  if (v != nullptr) env->ReleaseFloatArrayElements(values, v, JNI_ABORT);
  if (nm != nullptr) env->ReleaseStringUTFChars(name, nm);
}
void RuntimeShader_nUpdateUniformsFloats(JNIEnv* env, jclass, jlong bp, jstring name, jfloat f1,
                                         jfloat f2, jfloat f3, jfloat f4, jint count) {
  auto* b = reinterpret_cast<AclRuntimeShaderBuilder*>(bp);
  if (b == nullptr || b->builder == nullptr || name == nullptr) return;
  const char* nm = env->GetStringUTFChars(name, nullptr);
  const float vals[4] = {f1, f2, f3, f4};
  if (nm != nullptr) b->builder->uniform(nm).set(vals, (count > 0 && count <= 4) ? count : 1);
  if (nm != nullptr) env->ReleaseStringUTFChars(name, nm);
}
void RuntimeShader_nUpdateUniformsIntArr(JNIEnv* env, jclass, jlong bp, jstring name, jintArray values) {
  auto* b = reinterpret_cast<AclRuntimeShaderBuilder*>(bp);
  if (b == nullptr || b->builder == nullptr || name == nullptr || values == nullptr) return;
  const char* nm = env->GetStringUTFChars(name, nullptr);
  const jsize n = env->GetArrayLength(values);
  jint* v = env->GetIntArrayElements(values, nullptr);
  if (nm != nullptr && v != nullptr) b->builder->uniform(nm).set(reinterpret_cast<const int*>(v), static_cast<int>(n));
  if (v != nullptr) env->ReleaseIntArrayElements(values, v, JNI_ABORT);
  if (nm != nullptr) env->ReleaseStringUTFChars(name, nm);
}
void RuntimeShader_nUpdateUniformsInts(JNIEnv* env, jclass, jlong bp, jstring name, jint i1, jint i2,
                                       jint i3, jint i4, jint count) {
  auto* b = reinterpret_cast<AclRuntimeShaderBuilder*>(bp);
  if (b == nullptr || b->builder == nullptr || name == nullptr) return;
  const char* nm = env->GetStringUTFChars(name, nullptr);
  const int vals[4] = {i1, i2, i3, i4};
  if (nm != nullptr) b->builder->uniform(nm).set(vals, (count > 0 && count <= 4) ? count : 1);
  if (nm != nullptr) env->ReleaseStringUTFChars(name, nm);
}
void RuntimeShader_nUpdateShader(JNIEnv* env, jclass, jlong bp, jstring name, jlong shaderPtr) {
  auto* b = reinterpret_cast<AclRuntimeShaderBuilder*>(bp);
  if (b == nullptr || b->builder == nullptr || name == nullptr) return;
  const char* nm = env->GetStringUTFChars(name, nullptr);
  if (nm != nullptr) b->builder->child(nm) = sk_ref_sp(reinterpret_cast<SkShader*>(shaderPtr));
  if (nm != nullptr) env->ReleaseStringUTFChars(name, nm);
}

const JNINativeMethod kRuntimeShader[] = {
    {"nativeGetFinalizer", "()J", reinterpret_cast<void*>(RuntimeShader_nGetFinalizer)},
    {"nativeCreateBuilder", "(Ljava/lang/String;)J", reinterpret_cast<void*>(RuntimeShader_nCreateBuilder)},
    {"nativeCreateShader", "(JJ)J", reinterpret_cast<void*>(RuntimeShader_nCreateShader)},
    {"nativeUpdateUniforms", "(JLjava/lang/String;[FZ)V", reinterpret_cast<void*>(RuntimeShader_nUpdateUniformsFloatArr)},
    {"nativeUpdateUniforms", "(JLjava/lang/String;FFFFI)V", reinterpret_cast<void*>(RuntimeShader_nUpdateUniformsFloats)},
    {"nativeUpdateUniforms", "(JLjava/lang/String;[I)V", reinterpret_cast<void*>(RuntimeShader_nUpdateUniformsIntArr)},
    {"nativeUpdateUniforms", "(JLjava/lang/String;IIIII)V", reinterpret_cast<void*>(RuntimeShader_nUpdateUniformsInts)},
    {"nativeUpdateShader", "(JLjava/lang/String;J)V", reinterpret_cast<void*>(RuntimeShader_nUpdateShader)},
};

const JNINativeMethod kCanvas[] = {
    {"nInitRaster", "(J)J", reinterpret_cast<void*>(Canvas_nInitRaster)},
    {"nSetBitmap", "(JJ)V", reinterpret_cast<void*>(Canvas_nSetBitmap)},
    {"nGetNativeFinalizer", "()J", reinterpret_cast<void*>(Canvas_nGetNativeFinalizer)},
    {"nGetWidth", "(J)I", reinterpret_cast<void*>(Canvas_nGetWidth)},    // @CriticalNative
    {"nGetHeight", "(J)I", reinterpret_cast<void*>(Canvas_nGetHeight)},  // @CriticalNative
    // @CriticalNative transform/clip (handle-only).
    {"nSave", "(JI)I", reinterpret_cast<void*>(Canvas_nSave)},
    {"nSaveLayerAlpha", "(JFFFFI)I", reinterpret_cast<void*>(Canvas_nSaveLayerAlpha)},
    {"nRestore", "(J)Z", reinterpret_cast<void*>(Canvas_nRestore)},
    {"nRestoreToCount", "(JI)V", reinterpret_cast<void*>(Canvas_nRestoreToCount)},
    {"nGetSaveCount", "(J)I", reinterpret_cast<void*>(Canvas_nGetSaveCount)},
    {"nTranslate", "(JFF)V", reinterpret_cast<void*>(Canvas_nTranslate)},
    {"nScale", "(JFF)V", reinterpret_cast<void*>(Canvas_nScale)},
    {"nRotate", "(JF)V", reinterpret_cast<void*>(Canvas_nRotate)},
    {"nClipRect", "(JFFFFI)Z", reinterpret_cast<void*>(Canvas_nClipRect)},
    {"nClipPath", "(JJI)Z", reinterpret_cast<void*>(Canvas_nClipPath)},
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
    {"nDrawArc", "(JFFFFFFZJ)V", reinterpret_cast<void*>(Canvas_nDrawArc)},
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
// android.graphics.PathMeasure — arc-length traversal of a Path (progress arcs, path-following
// animations). Backed by SkPathMeasure. Plain JNI (normal env/jclass ABI; not @CriticalNative —
// the by-name __JZ fallback in the UnsatisfiedLinkError confirms this).
jlong PathMeasure_nCreate(JNIEnv*, jclass, jlong pathPtr, jboolean forceClosed) {
  auto* path = reinterpret_cast<SkPath*>(pathPtr);
  return reinterpret_cast<jlong>(path != nullptr ? new SkPathMeasure(*path, forceClosed)
                                                  : new SkPathMeasure());
}
void PathMeasure_nSetPath(JNIEnv*, jclass, jlong ptr, jlong pathPtr, jboolean forceClosed) {
  if (auto* m = reinterpret_cast<SkPathMeasure*>(ptr))
    m->setPath(reinterpret_cast<SkPath*>(pathPtr), forceClosed);
}
jfloat PathMeasure_nGetLength(JNIEnv*, jclass, jlong ptr) {
  auto* m = reinterpret_cast<SkPathMeasure*>(ptr);
  return m != nullptr ? m->getLength() : 0.0f;
}
jboolean PathMeasure_nGetPosTan(JNIEnv* env, jclass, jlong ptr, jfloat distance, jfloatArray posA,
                                jfloatArray tanA) {
  auto* m = reinterpret_cast<SkPathMeasure*>(ptr);
  if (m == nullptr) return JNI_FALSE;
  SkPoint pos{}; SkVector tan{};
  if (!m->getPosTan(distance, &pos, &tan)) return JNI_FALSE;
  if (posA != nullptr) { jfloat v[2] = {pos.x(), pos.y()}; env->SetFloatArrayRegion(posA, 0, 2, v); }
  if (tanA != nullptr) { jfloat v[2] = {tan.x(), tan.y()}; env->SetFloatArrayRegion(tanA, 0, 2, v); }
  return JNI_TRUE;
}
jboolean PathMeasure_nGetMatrix(JNIEnv*, jclass, jlong ptr, jfloat distance, jlong matPtr, jint flags) {
  auto* m = reinterpret_cast<SkPathMeasure*>(ptr);
  auto* mat = reinterpret_cast<SkMatrix*>(matPtr);
  if (m == nullptr || mat == nullptr) return JNI_FALSE;
  return m->getMatrix(distance, mat, static_cast<SkPathMeasure::MatrixFlags>(flags)) ? JNI_TRUE
                                                                                     : JNI_FALSE;
}
jboolean PathMeasure_nGetSegment(JNIEnv*, jclass, jlong ptr, jfloat startD, jfloat stopD,
                                 jlong pathPtr, jboolean startWithMoveTo) {
  auto* m = reinterpret_cast<SkPathMeasure*>(ptr);
  auto* dst = reinterpret_cast<SkPath*>(pathPtr);
  if (m == nullptr || dst == nullptr) return JNI_FALSE;
  return m->getSegment(startD, stopD, dst, startWithMoveTo) ? JNI_TRUE : JNI_FALSE;
}
jboolean PathMeasure_nIsClosed(JNIEnv*, jclass, jlong ptr) {
  auto* m = reinterpret_cast<SkPathMeasure*>(ptr);
  return (m != nullptr && m->isClosed()) ? JNI_TRUE : JNI_FALSE;
}
jboolean PathMeasure_nNextContour(JNIEnv*, jclass, jlong ptr) {
  auto* m = reinterpret_cast<SkPathMeasure*>(ptr);
  return (m != nullptr && m->nextContour()) ? JNI_TRUE : JNI_FALSE;
}
void PathMeasure_nDestroy(JNIEnv*, jclass, jlong ptr) {
  delete reinterpret_cast<SkPathMeasure*>(ptr);
}
const JNINativeMethod kPathMeasure[] = {
    {"native_create", "(JZ)J", reinterpret_cast<void*>(PathMeasure_nCreate)},
    {"native_setPath", "(JJZ)V", reinterpret_cast<void*>(PathMeasure_nSetPath)},
    {"native_getLength", "(J)F", reinterpret_cast<void*>(PathMeasure_nGetLength)},
    {"native_getPosTan", "(JF[F[F)Z", reinterpret_cast<void*>(PathMeasure_nGetPosTan)},
    {"native_getMatrix", "(JFJI)Z", reinterpret_cast<void*>(PathMeasure_nGetMatrix)},
    {"native_getSegment", "(JFFJZ)Z", reinterpret_cast<void*>(PathMeasure_nGetSegment)},
    {"native_isClosed", "(J)Z", reinterpret_cast<void*>(PathMeasure_nIsClosed)},
    {"native_nextContour", "(J)Z", reinterpret_cast<void*>(PathMeasure_nNextContour)},
    {"native_destroy", "(J)V", reinterpret_cast<void*>(PathMeasure_nDestroy)},
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
    {"nAddRoundRect", "(JFFFF[FI)V", reinterpret_cast<void*>(Path_nAddRoundRect8)},
    {"nApproximate", "(JF)[F", reinterpret_cast<void*>(Path_nApproximate)},
    {"nRMoveTo", "(JFF)V", reinterpret_cast<void*>(Path_nRMoveTo)},
    {"nRLineTo", "(JFF)V", reinterpret_cast<void*>(Path_nRLineTo)},
    {"nRQuadTo", "(JFFFF)V", reinterpret_cast<void*>(Path_nRQuadTo)},
    {"nRCubicTo", "(JFFFFFF)V", reinterpret_cast<void*>(Path_nRCubicTo)},
    {"nArcTo", "(JFFFFFFZ)V", reinterpret_cast<void*>(Path_nArcTo)},
    {"nAddArc", "(JFFFFFF)V", reinterpret_cast<void*>(Path_nAddArc)},
    {"nAddPath", "(JJ)V", reinterpret_cast<void*>(Path_nAddPath)},
    {"nAddPath", "(JJFF)V", reinterpret_cast<void*>(Path_nAddPathOffset)},
    {"nAddPath", "(JJJ)V", reinterpret_cast<void*>(Path_nAddPathMatrix)},
    {"nOffset", "(JFF)V", reinterpret_cast<void*>(Path_nOffset)},
    {"nSet", "(JJ)V", reinterpret_cast<void*>(Path_nSet)},
    {"nTransform", "(JJ)V", reinterpret_cast<void*>(Path_nTransform)},
    {"nTransform", "(JJJ)V", reinterpret_cast<void*>(Path_nTransformDst)},
    {"nSetLastPoint", "(JFF)V", reinterpret_cast<void*>(Path_nSetLastPoint)},
    {"nIncReserve", "(JI)V", reinterpret_cast<void*>(Path_nIncReserve)},
    {"nComputeBounds", "(JLandroid/graphics/RectF;)V", reinterpret_cast<void*>(Path_nComputeBounds)},
    {"nIsRect", "(JLandroid/graphics/RectF;)Z", reinterpret_cast<void*>(Path_nIsRect)},
    {"nOp", "(JJIJ)Z", reinterpret_cast<void*>(Path_nOp)},
    {"nIsEmpty", "(J)Z", reinterpret_cast<void*>(Path_nIsEmpty)},
    {"nIsConvex", "(J)Z", reinterpret_cast<void*>(Path_nIsConvex)},
    // @CriticalNative (handle-only).
    {"nReset", "(J)V", reinterpret_cast<void*>(Path_nReset)},
    {"nRewind", "(J)V", reinterpret_cast<void*>(Path_nRewind)},
    {"nSetFillType", "(JI)V", reinterpret_cast<void*>(Path_nSetFillType)},
    {"nGetFillType", "(J)I", reinterpret_cast<void*>(Path_nGetFillType)},
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
    {"nGetStrokeWidth", "(J)F", reinterpret_cast<void*>(Paint_nGetStrokeWidth)},
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
    {"nGetTextAdvances", "(J[CIIIII[FI)F",
     reinterpret_cast<void*>(Paint_nGetTextAdvancesChars)},
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
const JNINativeMethod kBitmapShader[] = {
    {"nativeCreate", "(JJIIZZ)J", reinterpret_cast<void*>(BitmapShader_nativeCreate)},
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
    // TextureView hardware-layer path (see AclTextureLayer): create/update/detach a texture layer.
    {"nCreateTextureLayer", "(J)J", reinterpret_cast<void*>(HardwareRenderer_nCreateTextureLayer)},
    {"nBuildLayer", "(JJ)V", reinterpret_cast<void*>(HardwareRenderer_nBuildLayer)},
    {"nCopyLayerInto", "(JJJ)Z", reinterpret_cast<void*>(HardwareRenderer_nCopyLayerInto)},
    {"nPushLayerUpdate", "(JJ)V", reinterpret_cast<void*>(HardwareRenderer_nPushLayerUpdate)},
    {"nCancelLayerUpdate", "(JJ)V", reinterpret_cast<void*>(HardwareRenderer_nCancelLayerUpdate)},
    {"nDetachSurfaceTexture", "(JJ)V", reinterpret_cast<void*>(HardwareRenderer_nDetachSurfaceTexture)},
};
const JNINativeMethod kSurfaceTexture[] = {
    {"nativeInit", "(ZIZLjava/lang/ref/WeakReference;)V", reinterpret_cast<void*>(SurfaceTexture_nativeInit)},
    {"nativeFinalize", "()V", reinterpret_cast<void*>(SurfaceTexture_nativeFinalize)},
    {"nativeGetTransformMatrix", "([F)V", reinterpret_cast<void*>(SurfaceTexture_nativeGetTransformMatrix)},
    {"nativeGetTimestamp", "()J", reinterpret_cast<void*>(SurfaceTexture_nativeGetTimestamp)},
    {"nativeGetDataSpace", "()I", reinterpret_cast<void*>(SurfaceTexture_nativeGetDataSpace)},
    {"nativeSetDefaultBufferSize", "(II)V", reinterpret_cast<void*>(SurfaceTexture_nativeSetDefaultBufferSize)},
    {"nativeUpdateTexImage", "()V", reinterpret_cast<void*>(SurfaceTexture_nativeUpdateTexImage)},
    {"nativeReleaseTexImage", "()V", reinterpret_cast<void*>(SurfaceTexture_nativeReleaseTexImage)},
    {"nativeDetachFromGLContext", "()I", reinterpret_cast<void*>(SurfaceTexture_nativeDetachFromGLContext)},
    {"nativeAttachToGLContext", "(I)I", reinterpret_cast<void*>(SurfaceTexture_nativeAttachToGLContext)},
    {"nativeRelease", "()V", reinterpret_cast<void*>(SurfaceTexture_nativeRelease)},
    {"nativeIsReleased", "()Z", reinterpret_cast<void*>(SurfaceTexture_nativeIsReleased)},
};
const JNINativeMethod kTextureLayer[] = {
    {"nPrepare", "(JIIZ)Z", reinterpret_cast<void*>(TextureLayer_nPrepare)},
    {"nSetLayerPaint", "(JJ)V", reinterpret_cast<void*>(TextureLayer_nSetLayerPaint)},
    {"nSetTransform", "(JJ)V", reinterpret_cast<void*>(TextureLayer_nSetTransform)},
    {"nSetSurfaceTexture", "(JLandroid/graphics/SurfaceTexture;)V", reinterpret_cast<void*>(TextureLayer_nSetSurfaceTexture)},
    {"nUpdateSurfaceTexture", "(J)V", reinterpret_cast<void*>(TextureLayer_nUpdateSurfaceTexture)},
};
const JNINativeMethod kTextureView[] = {
    {"nCreateNativeWindow", "(Landroid/graphics/SurfaceTexture;)V", reinterpret_cast<void*>(TextureView_nCreateNativeWindow)},
    {"nDestroyNativeWindow", "()V", reinterpret_cast<void*>(TextureView_nDestroyNativeWindow)},
    {"nLockCanvas", "(JLandroid/graphics/Canvas;Landroid/graphics/Rect;)Z", reinterpret_cast<void*>(TextureView_nLockCanvas)},
    {"nUnlockCanvasAndPost", "(JLandroid/graphics/Canvas;)V", reinterpret_cast<void*>(TextureView_nUnlockCanvasAndPost)},
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
// A secondary window's real ViewRootImpl.relayoutWindow() calls SurfaceControl.setTransformHint
// during performTraversals (Dialog/PopupWindow); acl composites such windows itself, so no-op it.
void SC_nSetTransformHint(JNIEnv*, jclass, jlong, jint) {}

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
    {"nativeSetTransformHint", "(JI)V", reinterpret_cast<void*>(SC_nSetTransformHint)},
};

// ── android.graphics.RenderEffect (RenderNode.setRenderEffect / Compose Modifier.blur) ──────────
// Each factory builds an SkImageFilter (mirroring AOSP hwui jni/RenderEffect.cpp) and returns the raw
// pointer; ownership transfers to Java via nativeGetFinalizer (SkSafeUnref). setRenderEffect stashes
// it on the node and the walk composites the node's content through it. Blur / offset / color-filter /
// chain are backed; bitmap/blend/shader/runtime return 0 (Java reads 0 as "no effect").
float blur_radius_to_sigma(float r) { return r > 0.0f ? 0.57735f * r + 0.5f : 0.0f; }  // Skia BLUR_SIGMA_SCALE
void free_image_filter(void* p) { SkSafeUnref(reinterpret_cast<SkImageFilter*>(p)); }

jlong RenderEffect_nGetFinalizer(JNIEnv*, jclass) {
  return reinterpret_cast<jlong>(&free_image_filter);
}
jlong RenderEffect_nCreateBlur(JNIEnv*, jclass, jfloat rx, jfloat ry, jlong input, jint edge) {
  auto* in = reinterpret_cast<SkImageFilter*>(input);
  sk_sp<SkImageFilter> f = SkImageFilters::Blur(blur_radius_to_sigma(rx), blur_radius_to_sigma(ry),
                                                static_cast<SkTileMode>(edge), sk_ref_sp(in), nullptr);
  return reinterpret_cast<jlong>(f.release());
}
jlong RenderEffect_nCreateOffset(JNIEnv*, jclass, jfloat ox, jfloat oy, jlong input) {
  auto* in = reinterpret_cast<SkImageFilter*>(input);
  sk_sp<SkImageFilter> f = SkImageFilters::Offset(ox, oy, sk_ref_sp(in));
  return reinterpret_cast<jlong>(f.release());
}
jlong RenderEffect_nCreateColorFilter(JNIEnv*, jclass, jlong cf, jlong input) {
  auto* colorFilter = reinterpret_cast<SkColorFilter*>(cf);
  auto* in = reinterpret_cast<SkImageFilter*>(input);
  sk_sp<SkImageFilter> f = SkImageFilters::ColorFilter(sk_ref_sp(colorFilter), sk_ref_sp(in), nullptr);
  return reinterpret_cast<jlong>(f.release());
}
jlong RenderEffect_nCreateChain(JNIEnv*, jclass, jlong outer, jlong inner) {
  auto* o = reinterpret_cast<SkImageFilter*>(outer);
  auto* i = reinterpret_cast<SkImageFilter*>(inner);
  sk_sp<SkImageFilter> f = SkImageFilters::Compose(sk_ref_sp(o), sk_ref_sp(i));
  return reinterpret_cast<jlong>(f.release());
}
jlong RenderEffect_nCreateBitmap(JNIEnv*, jclass, jlong, jfloat, jfloat, jfloat, jfloat, jfloat,
                                 jfloat, jfloat, jfloat) { return 0; }
jlong RenderEffect_nCreateBlend(JNIEnv*, jclass, jlong, jlong, jint) { return 0; }
jlong RenderEffect_nCreateShader(JNIEnv*, jclass, jlong) { return 0; }
jlong RenderEffect_nCreateRuntimeShader(JNIEnv*, jclass, jlong, jstring) { return 0; }

const JNINativeMethod kRenderEffect[] = {
    {"nativeGetFinalizer", "()J", reinterpret_cast<void*>(RenderEffect_nGetFinalizer)},
    {"nativeCreateOffsetEffect", "(FFJ)J", reinterpret_cast<void*>(RenderEffect_nCreateOffset)},
    {"nativeCreateBlurEffect", "(FFJI)J", reinterpret_cast<void*>(RenderEffect_nCreateBlur)},
    {"nativeCreateBitmapEffect", "(JFFFFFFFF)J", reinterpret_cast<void*>(RenderEffect_nCreateBitmap)},
    {"nativeCreateColorFilterEffect", "(JJ)J", reinterpret_cast<void*>(RenderEffect_nCreateColorFilter)},
    {"nativeCreateBlendModeEffect", "(JJI)J", reinterpret_cast<void*>(RenderEffect_nCreateBlend)},
    {"nativeCreateChainEffect", "(JJ)J", reinterpret_cast<void*>(RenderEffect_nCreateChain)},
    {"nativeCreateShaderEffect", "(J)J", reinterpret_cast<void*>(RenderEffect_nCreateShader)},
    {"nativeCreateRuntimeShaderEffect", "(JLjava/lang/String;)J",
     reinterpret_cast<void*>(RenderEffect_nCreateRuntimeShader)},
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

// DIAG: total Canvas draw ops issued so far (see g_draw_ops). jvm_vm logs the per-frame delta
// to tell whether Compose content actually paints or the frame is blank.
extern "C" std::uint64_t acl_graphics_profile_drawops() {
  return g_draw_ops.load(std::memory_order_relaxed);
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
  // RenderNode record/replay (Compose). Best-effort: absent -> Compose layers don't replay (blank);
  // present -> Compose renders. Not gated on `ok` so a signature drift never kills the whole floor.
  // A RecordingCanvas draws through BaseRecordingCanvas.nDraw* (its OWN natives, distinct from
  // BaseCanvas) — those share BaseCanvas' signatures/impl, so reuse kBaseCanvas here.
  reg(env, "android/graphics/BaseRecordingCanvas", kBaseCanvas, n(kBaseCanvas));
  reg(env, "android/graphics/BaseRecordingCanvas", kBaseRecordingExtra, n(kBaseRecordingExtra));
  reg(env, "android/graphics/RecordingCanvas", kRecordingCanvas, n(kRecordingCanvas));
  reg(env, "android/graphics/CanvasProperty", kCanvasProperty, n(kCanvasProperty));
  reg(env, "android/graphics/animation/RenderNodeAnimator", kRenderNodeAnimator, n(kRenderNodeAnimator));
  reg(env, "android/graphics/DashPathEffect", kDashPathEffect, n(kDashPathEffect));
  reg(env, "android/graphics/CornerPathEffect", kCornerPathEffect, n(kCornerPathEffect));
  reg(env, "android/graphics/PathEffect", kPathEffect, n(kPathEffect));
  reg(env, "android/graphics/Color", kColor, n(kColor));
  // Text measurement + line breaking (Compose's text layout). Best-effort: without these,
  // LineBreaker/MeasuredText fail to link and Compose text throws mid-measure (blank content).
  reg(env, "android/graphics/text/MeasuredText$Builder", kMeasuredTextBuilder, n(kMeasuredTextBuilder));
  reg(env, "android/graphics/text/MeasuredText", kMeasuredText, n(kMeasuredText));
  reg(env, "android/graphics/text/LineBreaker", kLineBreaker, n(kLineBreaker));
  // RuntimeShader (AGSL) — Material ripple / custom shaders. Best-effort.
  reg(env, "android/graphics/RuntimeShader", kRuntimeShader, n(kRuntimeShader));
  reg(env, "android/graphics/RenderEffect", kRenderEffect, n(kRenderEffect));
  ok = reg(env, "android/graphics/BaseCanvas", kBaseCanvas, n(kBaseCanvas)) && ok;
  ok = reg(env, "android/graphics/Path", kPath, n(kPath)) && ok;
  // PathMeasure (progress arcs / path-following animations) — a missing native_create blanks the
  // composing screen (forkyz's add-puzzle sub-screen). Best-effort.
  reg(env, "android/graphics/PathMeasure", kPathMeasure, n(kPathMeasure));
  ok = reg(env, "android/graphics/ImageDecoder", kImageDecoder, n(kImageDecoder)) && ok;
  ok = reg(env, "android/graphics/NinePatch", kNinePatch, n(kNinePatch)) && ok;
  ok = reg(env, "android/graphics/ColorFilter", kColorFilter, n(kColorFilter)) && ok;
  ok = reg(env, "android/graphics/BlendModeColorFilter", kBlendModeColorFilter,
           n(kBlendModeColorFilter)) && ok;
  // PorterDuffColorFilter is the older tint API; its native_CreateBlendModeFilter(color, mode) has
  // the same signature + semantics as BlendModeColorFilter's, so share the impl. Pervasive: Material
  // drawable/icon tinting (e.g. opendrummer's toolbar + vector drawables) goes through it.
  ok = reg(env, "android/graphics/PorterDuffColorFilter", kBlendModeColorFilter,
           n(kBlendModeColorFilter)) && ok;
  ok = reg(env, "android/graphics/animation/NativeInterpolatorFactory",
           kNativeInterpolatorFactory, n(kNativeInterpolatorFactory)) && ok;
  ok = reg(env, "android/graphics/drawable/AnimatedVectorDrawable", kAnimatedVectorDrawable,
           n(kAnimatedVectorDrawable)) && ok;
  ok = reg(env, "android/graphics/Matrix", kMatrix, n(kMatrix)) && ok;
  ok = reg(env, "android/graphics/RenderNode", kRenderNode, n(kRenderNode)) && ok;
  ok = reg(env, "android/graphics/Paint", kPaint, n(kPaint)) && ok;
  ok = reg(env, "android/graphics/Shader", kShader, n(kShader)) && ok;
  ok = reg(env, "android/graphics/LinearGradient", kLinearGradient, n(kLinearGradient)) && ok;
  ok = reg(env, "android/graphics/RadialGradient", kRadialGradient, n(kRadialGradient)) && ok;
  ok = reg(env, "android/graphics/BitmapShader", kBitmapShader, n(kBitmapShader)) && ok;
  ok = reg(env, "android/graphics/ColorSpace$Rgb", kColorSpaceRgb, n(kColorSpaceRgb)) && ok;
  ok = reg(env, "android/graphics/Region", kRegion, n(kRegion)) && ok;
  ok = reg(env, "android/graphics/Typeface", kTypeface, n(kTypeface)) && ok;
  ok = reg(env, "android/graphics/fonts/Font$Builder", kFontBuilder, n(kFontBuilder)) && ok;
  ok = reg(env, "android/graphics/fonts/FontFamily$Builder", kFontFamilyBuilder, n(kFontFamilyBuilder)) && ok;
  ok = reg(env, "android/graphics/fonts/FontFamily", kFontFamily, n(kFontFamily)) && ok;
  ok = reg(env, "android/graphics/fonts/Font", kFont, n(kFont)) && ok;
  ok = reg(env, "android/graphics/Interpolator", kInterpolator, n(kInterpolator)) && ok;
  ok = reg(env, "android/graphics/HardwareRenderer", kHardwareRenderer, n(kHardwareRenderer)) && ok;
  // SurfaceTexture / TextureView hardware-layer path. Best-effort (a missing native here must not
  // gate the whole graphics floor): register the natives and cache mSurfaceTexture. Absent -> a
  // TextureView in the tree throws UnsatisfiedLinkError and takes the View.draw traversal down.
  if (jclass st = env->FindClass("android/graphics/SurfaceTexture")) {
    g_st_field = env->GetFieldID(st, "mSurfaceTexture", "J");
    if (env->ExceptionCheck()) env->ExceptionClear();
    reg(env, "android/graphics/SurfaceTexture", kSurfaceTexture, n(kSurfaceTexture));
    env->DeleteLocalRef(st);
  } else {
    env->ExceptionClear();
  }
  reg(env, "android/graphics/TextureLayer", kTextureLayer, n(kTextureLayer));
  if (jclass tv = env->FindClass("android/view/TextureView")) {
    g_tv_window_field = env->GetFieldID(tv, "mNativeWindow", "J");
    if (env->ExceptionCheck()) env->ExceptionClear();
    reg(env, "android/view/TextureView", kTextureView, n(kTextureView));
    env->DeleteLocalRef(tv);
  } else {
    env->ExceptionClear();
  }
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
