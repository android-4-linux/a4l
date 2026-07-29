// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.ImageReader native contract (AOSP frameworks/base/media/java/android/
// media/ImageReader.java — method names/descriptors, mNativeContext/mNativeBuffer/mTimestamp fields,
// postEventFromNative, ACQUIRE_* codes, the private HardwareBuffer(long) from-native constructor).
// Implementation is acl-original over gfx::BufferQueue. No GPL-3.0 sources.

#include "runtime/image_reader_jni.h"

#include "acl_config.h"

#if ACL_HAVE_GL

#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "gfx/ahardware_buffer.h"
#include "gfx/buffer_queue.h"
#include "log/log.h"
#include "ndk/bionic_runtime.h"

namespace acl::runtime {
namespace {
constexpr const char* kTag = "acl.media.imagereader";

// Cached ids (resolved once in register_image_reader_natives; classes are boot-classpath, stable).
jclass g_reader_cls = nullptr;     // global ref: android.media.ImageReader
jmethodID g_post_event = nullptr;  // static ImageReader.postEventFromNative(Object)
jfieldID g_ctx_fid = nullptr;      // ImageReader.mNativeContext (J)
jfieldID g_img_buf_fid = nullptr;  // ImageReader$SurfaceImage.mNativeBuffer (J)
jfieldID g_img_ts_fid = nullptr;   // ImageReader$SurfaceImage.mTimestamp (J)
jclass g_hb_cls = nullptr;         // global ref: android.hardware.HardwareBuffer
jmethodID g_hb_ctor = nullptr;     // private HardwareBuffer(long)
jclass g_surface_cls = nullptr;    // global ref: android.view.Surface
jmethodID g_surface_ctor = nullptr;  // private Surface(long) — pure-Java, no Surface natives needed
jfieldID g_surface_ctx_fid = nullptr;  // android.view.Surface.mNativeObject (J) — our ReaderCtx
JavaVM* g_anw_vm = nullptr;        // for the ANativeWindow_fromSurface producer resolve (GetEnv)

struct ReaderCtx {
  std::unique_ptr<gfx::BufferQueue> queue;
  JavaVM* vm = nullptr;
  jobject weak_self = nullptr;  // global ref to WeakReference<ImageReader>
  bool self_produce = false;    // ACL_IMAGEREADER_TEST_PRODUCER: run a producer thread of test frames
  int frame = 0;
  std::thread producer;         // self-produce: queues an animated frame every ~30ms
  std::atomic<bool> stop{false};
  ::AHardwareBuffer* anw_pending = nullptr;  // dequeued+locked by ANativeWindow_lock, until unlockAndPost
};

// Live ImageReader contexts, so an ANativeWindow producer can validate that a Surface's mNativeObject
// is really one of our readers (and not the GL-window token) before cycling its BufferQueue. Small N.
std::mutex g_readers_mu;
std::unordered_set<ReaderCtx*> g_readers;
bool reader_live(ReaderCtx* ctx) {
  std::lock_guard<std::mutex> lk(g_readers_mu);
  return g_readers.count(ctx) != 0;
}

// ── ANativeWindow producer ops (backing libandroid's ANativeWindow_* via set_anw_ops) ─────────────
// Resolve a producer Surface to its ImageReader: read Surface.mNativeObject (its ReaderCtx, set by
// nativeGetSurface) via JNI on the calling thread's env (GetEnv — the proven up-call pattern, same as
// postEventFromNative), and return it only if it's a live reader. NULL for any other surface (the GL
// window), so ANativeWindow_fromSurface falls back to its identity token. Runs under glibc %fs (the
// trampoline swapped), where the local `surface` ref is valid because it's the same thread.
void* anw_from_surface(void* jsurface) {
  if (g_anw_vm == nullptr || g_surface_ctx_fid == nullptr || jsurface == nullptr) return nullptr;
  JNIEnv* env = nullptr;
  if (g_anw_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return nullptr;
  const jlong no = env->GetLongField(static_cast<jobject>(jsurface), g_surface_ctx_fid);
  if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
  auto* ctx = reinterpret_cast<ReaderCtx*>(no);
  return reader_live(ctx) ? ctx : nullptr;
}
// Producer dequeues + CPU-maps the next free buffer. -EAGAIN when the pool is momentarily full (all
// buffers outstanding) so the producer retries; -EBUSY if a prior lock wasn't posted; -EINVAL on a
// dead reader / map failure. On 0, *out describes the mapped buffer and it stays "pending" until post.
int anw_lock(void* anw, ndk::AnwBuffer* out) {
  auto* ctx = reinterpret_cast<ReaderCtx*>(anw);
  if (!reader_live(ctx) || out == nullptr) return -EINVAL;
  if (ctx->anw_pending != nullptr) return -EBUSY;
  ::AHardwareBuffer* b = ctx->queue->dequeue();
  if (b == nullptr) return -EAGAIN;
  std::uint32_t stride_bytes = 0;
  void* bits = gfx::ahb_lock(b, &stride_bytes);
  if (bits == nullptr) return -EINVAL;  // buffer returns to FREE at next dequeue (still owned by queue)
  ctx->anw_pending = b;
  ::AHardwareBuffer_Desc d{};
  gfx::ahb_describe(b, &d);
  out->bits = bits;
  out->width = static_cast<std::int32_t>(d.width);
  out->height = static_cast<std::int32_t>(d.height);
  out->stride = static_cast<std::int32_t>(stride_bytes / 4);  // 4 bytes/px (RGBA/ABGR8888)
  out->format = 1;                                            // WINDOW_FORMAT_RGBA_8888
  return 0;
}
// Producer publishes the pending buffer: unmap + queue -> onImageAvailable -> the engine composites.
int anw_unlock_post(void* anw) {
  auto* ctx = reinterpret_cast<ReaderCtx*>(anw);
  if (!reader_live(ctx)) return -EINVAL;
  if (ctx->anw_pending == nullptr) return -EINVAL;
  ::AHardwareBuffer* b = ctx->anw_pending;
  ctx->anw_pending = nullptr;
  gfx::ahb_unlock(b);
  ctx->queue->queue(b);
  return 0;
}

// A recognizable animated ARGB color from a frame counter (~3s R/G/B cycle).
std::uint32_t hue(int frame) {
  const double t = (frame % 90) / 90.0 * 6.283185;
  auto ch = [](double x) { return static_cast<std::uint32_t>((0.5 + 0.5 * x) * 255.0) & 0xffu; };
  return 0xFF000000u | (ch(__builtin_sin(t)) << 16) | (ch(__builtin_sin(t + 2.094)) << 8) |
         ch(__builtin_sin(t + 4.188));
}

ReaderCtx* ctx_of(JNIEnv* env, jobject reader) {
  return g_ctx_fid != nullptr
             ? reinterpret_cast<ReaderCtx*>(env->GetLongField(reader, g_ctx_fid))
             : nullptr;
}
::AHardwareBuffer* img_buf(JNIEnv* env, jobject image) {
  return g_img_buf_fid != nullptr
             ? reinterpret_cast<::AHardwareBuffer*>(env->GetLongField(image, g_img_buf_fid))
             : nullptr;
}
jlong now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<jlong>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// ── ImageReader natives ─────────────────────────────────────────────────────────────────────────
void JNICALL IR_nativeClassInit(JNIEnv*, jclass) {}  // ids are cached at register time

void JNICALL IR_nativeInit(JNIEnv* env, jobject thiz, jobject weakSelf, jint w, jint h, jint maxImgs,
                           jlong usage, jint hbFormat, jint /*dataSpace*/) {
  std::string err;
  auto q = gfx::BufferQueue::create(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                    static_cast<std::uint32_t>(hbFormat),
                                    static_cast<std::uint64_t>(usage), maxImgs, &err);
  if (!q) { log::w(kTag, "nativeInit {}x{} failed: {}", w, h, err); return; }
  auto* ctx = new ReaderCtx();
  ctx->queue = std::move(q);
  env->GetJavaVM(&ctx->vm);
  ctx->weak_self = env->NewGlobalRef(weakSelf);
  ReaderCtx* c = ctx;
  // A produced frame fires onImageAvailable via the static postEventFromNative(weakSelf). Runs on
  // the producer thread — attach it if needed (it may be a bionic engine/codec thread).
  ctx->queue->set_on_frame_available([c] {
    if (c->vm == nullptr || g_reader_cls == nullptr || g_post_event == nullptr) return;
    JNIEnv* e = nullptr;
    bool attached = false;
    if (c->vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK) {
      if (c->vm->AttachCurrentThread(reinterpret_cast<void**>(&e), nullptr) != JNI_OK) return;
      attached = true;
    }
    jvalue a;
    a.l = c->weak_self;
    e->CallStaticVoidMethodA(g_reader_cls, g_post_event, &a);  // A-variant: variadic is mangled (see above)
    if (e->ExceptionCheck()) e->ExceptionClear();
    if (attached) c->vm->DetachCurrentThread();
  });
  ctx->self_produce = std::getenv("ACL_IMAGEREADER_TEST_PRODUCER") != nullptr;
  { std::lock_guard<std::mutex> lk(g_readers_mu); g_readers.insert(ctx); }  // ANativeWindow producer registry
  env->SetLongField(thiz, g_ctx_fid, reinterpret_cast<jlong>(ctx));
  log::i(kTag, "nativeInit {}x{} maxImages={} hbFormat={} self_produce={}", w, h, maxImgs, hbFormat,
         ctx->self_produce);

  // Test PRODUCER THREAD: once ImageReaderSurfaceProducer installs its OnImageAvailableListener (just
  // after nativeGetSurface returns), keep dequeuing a free buffer, filling it with the next animated
  // hue, and queuing it (~30fps). Each queue() fires on_frame_available -> postEventFromNative -> the
  // listener's handler -> the engine's acquireLatestImage/composite (on its own threads). Producing
  // off-thread here (only dequeue/fill/queue + a post) decouples us from the consumer's cadence, so
  // the frame genuinely animates. dequeue() returns null while all buffers are outstanding — skip and
  // retry (the consumer's acquire_latest recycles them). Joined in nativeClose.
  if (ctx->self_produce) {
    ctx->producer = std::thread([c] {
      timespec warm{0, 300 * 1000 * 1000};
      nanosleep(&warm, nullptr);  // let the listener install first
      while (!c->stop.load(std::memory_order_acquire)) {
        if (::AHardwareBuffer* p = c->queue->dequeue()) {
          gfx::ahb_fill_color(p, hue(c->frame++));
          c->queue->queue(p);  // -> on_frame_available -> postEventFromNative -> engine acquires
        }
        timespec tick{0, 30 * 1000 * 1000};
        nanosleep(&tick, nullptr);
      }
    });
  }
}

void JNICALL IR_nativeClose(JNIEnv* env, jobject thiz) {
  ReaderCtx* ctx = ctx_of(env, thiz);
  if (ctx == nullptr) return;
  { std::lock_guard<std::mutex> lk(g_readers_mu); g_readers.erase(ctx); }  // no new ANativeWindow producer calls
  ctx->stop.store(true, std::memory_order_release);
  if (ctx->producer.joinable()) ctx->producer.join();  // stop producing before tearing down the queue
  if (ctx->weak_self != nullptr) env->DeleteGlobalRef(ctx->weak_self);
  delete ctx;  // BufferQueue destructor releases the pooled buffers
  env->SetLongField(thiz, g_ctx_fid, 0);
}

// acquireLatestImage's engine step: bind the newest ready buffer to the SurfaceImage. 0=SUCCESS,
// 1=NO_BUFS (ImageReader's ACQUIRE_SUCCESS / ACQUIRE_NO_BUFS).
jint JNICALL IR_nativeImageSetup(JNIEnv* env, jobject thiz, jobject image, jboolean /*legacy*/) {
  ReaderCtx* ctx = ctx_of(env, thiz);
  if (ctx == nullptr) return 1;
  // Bind the newest ready buffer (the producer thread queues them; see nativeInit). acquire_latest
  // recycles older ready buffers so the drain loop terminates once none are newer. NO_BUFS -> 1.
  ::AHardwareBuffer* b = ctx->queue->acquire_latest();
  if (b == nullptr) return 1;
  env->SetLongField(image, g_img_buf_fid, reinterpret_cast<jlong>(b));
  env->SetLongField(image, g_img_ts_fid, now_ns());
  if (ctx->frame <= 3 || ctx->frame % 60 == 0) log::i(kTag, "nativeImageSetup ok (frame {})", ctx->frame);
  return 0;
}

void JNICALL IR_nativeReleaseImage(JNIEnv* env, jobject thiz, jobject image) {
  ReaderCtx* ctx = ctx_of(env, thiz);
  ::AHardwareBuffer* b = img_buf(env, image);
  if (std::getenv("ACL_IR_DIAG") != nullptr) log::i(kTag, "nativeReleaseImage b={}", (void*)b);
  if (ctx != nullptr && b != nullptr) ctx->queue->release(b);
  if (image != nullptr) env->SetLongField(image, g_img_buf_fid, 0);
}

jint JNICALL IR_nativeDetachImage(JNIEnv*, jobject, jobject) { return 0; }
void JNICALL IR_nativeDiscardFreeBuffers(JNIEnv*, jobject) {}
// The producer-side Surface (M3c stage 3). Return a real android.view.Surface whose mNativeObject is
// this reader's ReaderCtx — a valid, non-null handle so the engine's ImageReaderSurfaceProducer
// activates. Surface(long) is pure Java (no Surface natives). The ANativeWindow producer over the
// BufferQueue (ANativeWindow_fromSurface reads mNativeObject) lands next; the consumer/composite path
// works via the ImageReader directly.
jobject JNICALL IR_nativeGetSurface(JNIEnv* env, jobject thiz) {
  ReaderCtx* ctx = ctx_of(env, thiz);
  if (ctx == nullptr || g_surface_cls == nullptr || g_surface_ctor == nullptr) return nullptr;
  jvalue a;
  a.j = reinterpret_cast<jlong>(ctx);
  jobject surf = env->NewObjectA(g_surface_cls, g_surface_ctor, &a);
  if (surf == nullptr) { env->ExceptionClear(); return nullptr; }
  log::i(kTag, "nativeGetSurface -> Surface(mNativeObject=ctx)");
  return surf;
}
jobjectArray JNICALL IR_nativeCreateImagePlanes(JNIEnv*, jclass, jint, jobject, jint, jint, jint,
                                                jint, jint, jint) {
  return nullptr;  // HardwareBuffer-format readers use getHardwareBuffer(), not planes
}
void JNICALL IR_nativeUnlockGraphicBuffer(JNIEnv*, jclass, jobject) {}

// ── ImageReader$SurfaceImage natives ────────────────────────────────────────────────────────────
jobject JNICALL SI_nativeGetHardwareBuffer(JNIEnv* env, jobject thiz) {
  ::AHardwareBuffer* b = img_buf(env, thiz);
  const bool diag = std::getenv("ACL_IR_DIAG") != nullptr;
  if (diag) log::i(kTag, "getHardwareBuffer: enter b={} tid={}", (void*)b, static_cast<long>(::syscall(SYS_gettid)));
  if (b == nullptr || g_hb_cls == nullptr || g_hb_ctor == nullptr) return nullptr;
  // The returned HardwareBuffer holds its OWN ref (its NativeAllocationRegistry finalizer releases
  // it); the BufferQueue keeps its ref until nativeReleaseImage.
  gfx::ahb_acquire(b);
  // NewObjectA (explicit jvalue array), NOT the variadic NewObject: through acl's wrapped/%fs-
  // switching JNIEnv on the engine's raster thread, a variadic jlong is mangled (arrives as 2).
  jvalue arg;
  arg.j = reinterpret_cast<jlong>(b);
  jobject hb = env->NewObjectA(g_hb_cls, g_hb_ctor, &arg);
  if (hb == nullptr) { env->ExceptionClear(); gfx::ahb_release(b); return nullptr; }
  if (diag) {
    jmethodID gw = env->GetMethodID(g_hb_cls, "getWidth", "()I");
    jmethodID gf = env->GetMethodID(g_hb_cls, "getFormat", "()I");
    jint w = gw != nullptr ? env->CallIntMethod(hb, gw) : -1;
    jint fmt = gf != nullptr ? env->CallIntMethod(hb, gf) : -1;
    if (env->ExceptionCheck()) env->ExceptionClear();
    log::i(kTag, "getHardwareBuffer: hb.getWidth()={} getFormat()={} (expect 256, 1)", w, fmt);
  }
  return hb;
}
jint JNICALL SI_nativeGetWidth(JNIEnv* env, jobject thiz) {
  ::AHardwareBuffer* b = img_buf(env, thiz);
  if (b == nullptr) return 0;
  ::AHardwareBuffer_Desc d{};
  gfx::ahb_describe(b, &d);
  return static_cast<jint>(d.width);
}
jint JNICALL SI_nativeGetHeight(JNIEnv* env, jobject thiz) {
  ::AHardwareBuffer* b = img_buf(env, thiz);
  if (b == nullptr) return 0;
  ::AHardwareBuffer_Desc d{};
  gfx::ahb_describe(b, &d);
  return static_cast<jint>(d.height);
}
jint JNICALL SI_nativeGetFormat(JNIEnv* env, jobject thiz, jint /*readerFormat*/) {
  ::AHardwareBuffer* b = img_buf(env, thiz);
  if (b == nullptr) return 0;
  ::AHardwareBuffer_Desc d{};
  gfx::ahb_describe(b, &d);
  return static_cast<jint>(d.format);
}
jint JNICALL SI_nativeGetFenceFd(JNIEnv*, jobject) { return -1; }  // no fence; buffer is ready
jobjectArray JNICALL SI_nativeCreatePlanes(JNIEnv*, jobject, jint, jint, jlong) { return nullptr; }

// ── android.media.PublicFormatUtils (also from media_jni) ─────────────────────────────────────────
// public<->HAL format mapping. For the direct formats acl handles (RGBA_8888, PRIVATE/
// IMPLEMENTATION_DEFINED) the public value already equals the HAL value, so map identically;
// dataspace is advisory (UNKNOWN).
jint JNICALL PFU_nativeGetHalFormat(JNIEnv*, jclass, jint imageFormat) { return imageFormat; }
jint JNICALL PFU_nativeGetHalDataspace(JNIEnv*, jclass, jint) { return 0; /* DATASPACE_UNKNOWN */ }
jint JNICALL PFU_nativeGetPublicFormat(JNIEnv*, jclass, jint imageFormat, jint) { return imageFormat; }

bool bind(JNIEnv* env, const char* cls, const JNINativeMethod* m, int n) {
  jclass c = env->FindClass(cls);
  if (c == nullptr) { env->ExceptionClear(); return false; }
  const bool ok = env->RegisterNatives(c, m, n) == JNI_OK;
  if (!ok) env->ExceptionClear();
  env->DeleteLocalRef(c);
  return ok;
}
}  // namespace

void register_image_reader_natives(JNIEnv* env) {
  if (env == nullptr) return;
  env->GetJavaVM(&g_anw_vm);  // for the ANativeWindow_fromSurface producer resolve
  jclass rc = env->FindClass("android/media/ImageReader");
  if (rc == nullptr) { env->ExceptionClear(); return; }  // no framework -> nothing to do
  g_reader_cls = static_cast<jclass>(env->NewGlobalRef(rc));
  g_ctx_fid = env->GetFieldID(rc, "mNativeContext", "J");
  g_post_event = env->GetStaticMethodID(rc, "postEventFromNative", "(Ljava/lang/Object;)V");
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(rc);

  if (jclass si = env->FindClass("android/media/ImageReader$SurfaceImage"); si != nullptr) {
    g_img_buf_fid = env->GetFieldID(si, "mNativeBuffer", "J");
    g_img_ts_fid = env->GetFieldID(si, "mTimestamp", "J");
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(si);
  }
  if (jclass hb = env->FindClass("android/hardware/HardwareBuffer"); hb != nullptr) {
    g_hb_cls = static_cast<jclass>(env->NewGlobalRef(hb));
    g_hb_ctor = env->GetMethodID(hb, "<init>", "(J)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(hb);
  }
  if (jclass sf = env->FindClass("android/view/Surface"); sf != nullptr) {
    g_surface_cls = static_cast<jclass>(env->NewGlobalRef(sf));
    g_surface_ctor = env->GetMethodID(sf, "<init>", "(J)V");  // private, pure-Java
    g_surface_ctx_fid = env->GetFieldID(sf, "mNativeObject", "J");  // producer resolve (fromSurface)
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(sf);
  }

  static const JNINativeMethod ir_m[] = {
      {const_cast<char*>("nativeClassInit"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&IR_nativeClassInit)},
      {const_cast<char*>("nativeInit"), const_cast<char*>("(Ljava/lang/Object;IIIJII)V"),
       reinterpret_cast<void*>(&IR_nativeInit)},
      {const_cast<char*>("nativeClose"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&IR_nativeClose)},
      {const_cast<char*>("nativeReleaseImage"), const_cast<char*>("(Landroid/media/Image;)V"),
       reinterpret_cast<void*>(&IR_nativeReleaseImage)},
      {const_cast<char*>("nativeGetSurface"), const_cast<char*>("()Landroid/view/Surface;"),
       reinterpret_cast<void*>(&IR_nativeGetSurface)},
      {const_cast<char*>("nativeDetachImage"), const_cast<char*>("(Landroid/media/Image;)I"),
       reinterpret_cast<void*>(&IR_nativeDetachImage)},
      {const_cast<char*>("nativeDiscardFreeBuffers"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&IR_nativeDiscardFreeBuffers)},
      {const_cast<char*>("nativeImageSetup"), const_cast<char*>("(Landroid/media/Image;Z)I"),
       reinterpret_cast<void*>(&IR_nativeImageSetup)},
      {const_cast<char*>("nativeCreateImagePlanes"),
       const_cast<char*>("(ILandroid/graphics/GraphicBuffer;IIIIII)[Landroid/media/ImageReader$ImagePlane;"),
       reinterpret_cast<void*>(&IR_nativeCreateImagePlanes)},
      {const_cast<char*>("nativeUnlockGraphicBuffer"),
       const_cast<char*>("(Landroid/graphics/GraphicBuffer;)V"),
       reinterpret_cast<void*>(&IR_nativeUnlockGraphicBuffer)},
  };
  const bool ir_ok = bind(env, "android/media/ImageReader", ir_m, 10);

  static const JNINativeMethod si_m[] = {
      {const_cast<char*>("nativeCreatePlanes"),
       const_cast<char*>("(IIJ)[Landroid/media/ImageReader$SurfaceImage$SurfacePlane;"),
       reinterpret_cast<void*>(&SI_nativeCreatePlanes)},
      {const_cast<char*>("nativeGetWidth"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&SI_nativeGetWidth)},
      {const_cast<char*>("nativeGetHeight"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&SI_nativeGetHeight)},
      {const_cast<char*>("nativeGetFormat"), const_cast<char*>("(I)I"),
       reinterpret_cast<void*>(&SI_nativeGetFormat)},
      {const_cast<char*>("nativeGetFenceFd"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&SI_nativeGetFenceFd)},
      {const_cast<char*>("nativeGetHardwareBuffer"),
       const_cast<char*>("()Landroid/hardware/HardwareBuffer;"),
       reinterpret_cast<void*>(&SI_nativeGetHardwareBuffer)},
  };
  const bool si_ok = bind(env, "android/media/ImageReader$SurfaceImage", si_m, 6);

  static const JNINativeMethod pfu_m[] = {
      {const_cast<char*>("nativeGetHalFormat"), const_cast<char*>("(I)I"),
       reinterpret_cast<void*>(&PFU_nativeGetHalFormat)},
      {const_cast<char*>("nativeGetHalDataspace"), const_cast<char*>("(I)I"),
       reinterpret_cast<void*>(&PFU_nativeGetHalDataspace)},
      {const_cast<char*>("nativeGetPublicFormat"), const_cast<char*>("(II)I"),
       reinterpret_cast<void*>(&PFU_nativeGetPublicFormat)},
  };
  const bool pfu_ok = bind(env, "android/media/PublicFormatUtils", pfu_m, 3);

  log::write((ir_ok && si_ok && pfu_ok) ? log::Level::Info : log::Level::Warn, kTag,
             (ir_ok && si_ok && pfu_ok)
                 ? "ImageReader + SurfaceImage + PublicFormatUtils natives bound (consumer path)"
                 : "ImageReader native bind incomplete");

  // Back libandroid's ANativeWindow_* (producer side) with these readers' BufferQueues. Setting the
  // host ops is enough; the loader points ANativeWindow_fromSurface/lock/unlockAndPost at %fs-aware
  // trampolines that call them (bionic_runtime), and the trampolines self-guard until this runs.
  ndk::set_anw_ops(&anw_from_surface, &anw_lock, &anw_unlock_post);
}

gfx::BufferQueue* image_reader_queue_from_surface(JNIEnv* env, jobject surface) {
  if (env == nullptr || surface == nullptr || g_surface_ctx_fid == nullptr) return nullptr;
  const jlong no = env->GetLongField(surface, g_surface_ctx_fid);  // Surface.mNativeObject == ReaderCtx
  if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
  auto* ctx = reinterpret_cast<ReaderCtx*>(no);
  return reader_live(ctx) ? ctx->queue.get() : nullptr;
}

jlong image_reader_make_producer(JNIEnv* env, int w, int h) {
  if (env == nullptr) return 0;
  if (w <= 0) w = 64;
  if (h <= 0) h = 64;
  // The pool geometry here is nominal: the MediaCodec output path uses queue_imported() (external
  // decoder AHBs at the real frame size), so this only sizes the (unused) dequeue pool. R8G8B8A8 +
  // GPU sampled/color-output usage, a tiny pool.
  std::string err;
  auto q = gfx::BufferQueue::create(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                    /*R8G8B8A8_UNORM*/ 1u,
                                    /*GPU_SAMPLED_IMAGE|GPU_COLOR_OUTPUT*/ 0x300ull, /*count*/ 2, &err);
  if (!q) { log::w(kTag, "make_producer {}x{} failed: {}", w, h, err); return 0; }
  auto* ctx = new ReaderCtx();
  ctx->queue = std::move(q);
  env->GetJavaVM(&ctx->vm);
  { std::lock_guard<std::mutex> lk(g_readers_mu); g_readers.insert(ctx); }
  log::i(kTag, "make_producer {}x{} -> ReaderCtx (MediaCodec-decodable Surface; no consumer yet)", w, h);
  return reinterpret_cast<jlong>(ctx);
}

}  // namespace acl::runtime

#endif  // ACL_HAVE_GL
