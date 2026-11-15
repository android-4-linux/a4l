// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.MediaCodec native method contract from AOSP
// frameworks/base/media/java/ android/media/MediaCodec.java
// (native_setup/native_configure/native_start/native_dequeueInputBuffer/
// native_queueInputBuffer/native_dequeueOutputBuffer/releaseOutputBuffer/getBuffer
// + mNativeContext, BufferInfo, the INFO_*/BUFFER_FLAG_* constants) and the
// MediaCodec synchronous ByteBuffer decode model (SDK docs). Drives
// media::CodecDecoder; decoded frames are queue_imported into the configured
// Surface's ImageReader (zero-copy). No GPL-3.0 sources.

#include "runtime/media_codec_jni.h"

#include "acl_config.h"

// The real body needs both halves: the present path (wcs presenter + Ganesh
// Vulkan device) to put a decoded frame on a Surface, and FFmpeg for
// media::CodecDecoder to produce one.
#define ACL_MEDIA_CODEC_JNI (ACL_GL_PRESENT && ACL_HAVE_FFMPEG)

#if ACL_MEDIA_CODEC_JNI

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>  // dup/close for the HW-plane holding queue

#include "gfx/ahardware_buffer.h"
#include "gfx/buffer_queue.h"
#include "log/log.h"
#include "media/audio_codec.h"
#include "media/timed_release.h"
#include "media/video_decoder.h"
#include "runtime/image_reader_jni.h"
#include "wm/present.h"  // wm::active_video_presenter (route decoded video to the HW plane)

namespace acl::runtime {
namespace {
constexpr const char* kTag = "acl.media.codec";

// MediaCodec constants (MediaCodec.java).
constexpr jint INFO_TRY_AGAIN_LATER = -1;
constexpr jint INFO_OUTPUT_FORMAT_CHANGED = -2;
constexpr jint BUFFER_FLAG_CODEC_CONFIG = 2;
constexpr jint BUFFER_FLAG_END_OF_STREAM = 4;
// Async callback delivery (postEventFromNative what/arg1).
constexpr jint EVENT_CALLBACK = 1;
constexpr jint CB_INPUT_AVAILABLE = 1;
constexpr jint CB_OUTPUT_AVAILABLE = 2;
constexpr jint CB_OUTPUT_FORMAT_CHANGE = 4;

// Cached ids (resolved at register time; classes are boot-classpath, stable).
jclass g_mc_cls = nullptr;
jfieldID g_mc_ctx_fid =
    nullptr;  // MediaCodec.mNativeContext (J) — holds our CodecCtx*
jmethodID g_bufinfo_set =
    nullptr;  // MediaCodec$BufferInfo.set(int,int,long,int)
jclass g_integer_cls = nullptr;
jmethodID g_integer_intvalue = nullptr;
jmethodID g_integer_valueof = nullptr;  // Integer.valueOf(int)
jclass g_hashmap_cls = nullptr;
jmethodID g_hashmap_ctor = nullptr;  // HashMap()
jmethodID g_hashmap_put = nullptr;   // HashMap.put(Object,Object)
jmethodID g_post_event =
    nullptr;  // MediaCodec.postEventFromNative(int,int,int,Object)
jclass g_bufinfo_cls = nullptr;
jmethodID g_bufinfo_ctor = nullptr;  // MediaCodec$BufferInfo()

// One MediaCodec instance's decode state.
struct CodecCtx {
  std::string name;  // codec name or MIME (native_setup)
  bool name_is_type = false;
  std::string mime;
  int width = 0, height = 0;
  std::vector<std::uint8_t>
      csd;  // codec-specific data (csd-0[+csd-1]) from the configure format
  std::unique_ptr<media::CodecDecoder> dec;
  // Video frames released for display at a time, waiting for it.
  media::TimedRelease present;
  // An audio decoder: byte buffers in, 16-bit PCM in byte buffers out. Set at
  // start() when the MIME is audio/; everything video below is then unused.
  std::unique_ptr<media::AudioCodec> audio;
  std::map<int, std::vector<std::uint8_t>> csd_parts;  // csd-N, by N
  int sample_rate = 0, channels = 0;                   // the format's hints
  // Async: flush() stops callbacks until start() is called again, as
  // MediaCodec specifies; a client discards what arrives in between.
  std::atomic<bool> await_start{false};
  // Held by whoever is using `dec`: the async driver for a pass, and flush()
  // from the app's thread, which must not run into the middle of one.
  std::mutex dmu;
  gfx::BufferQueue* out_queue =
      nullptr;  // the configured Surface's ImageReader BufferQueue (or null)
  bool started = false;

  struct InBuf {
    std::vector<std::uint8_t> mem;
    bool free = true;
  };
  std::vector<InBuf>
      inbufs;  // input buffer pool, handed out as direct ByteBuffers

  struct OutSlot {
    ::AHardwareBuffer* ahb =
        nullptr;  // an imported decoded frame, awaiting releaseOutputBuffer
    bool used = false;
    std::int64_t pts = 0;
    // A frame for the video plane (a SurfaceView's video): kept as it came
    // from the decoder, with its hold on the decoder's surface, and shown
    // when the app releases it. `ahb` is then null.
    bool plane = false;
    gfx::DmabufFrame frame{};
  };
  std::vector<OutSlot> outslots;
  bool format_signaled = false;  // dequeueOutputBuffer returns
                                 // INFO_OUTPUT_FORMAT_CHANGED once first
  bool first_input =
      true;  // prepend csd (SPS/PPS) in-band to the first access unit

  // Async Callback mode (setCallback): a driver thread OWNS the CodecDecoder
  // and delivers events via MediaCodec.postEventFromNative; queueInputBuffer
  // only enqueues packet copies (so the decoder is never touched from two
  // threads). ExoPlayer uses this on configs that enable async operation.
  bool async = false;
  jobject self =
      nullptr;  // global ref to the MediaCodec (for postEventFromNative)
  JavaVM* vm = nullptr;
  std::thread driver;
  std::atomic<bool> stop{false};
  std::mutex
      amu;  // guards pending + input-buffer free state + outslots in async mode
  std::condition_variable acv;
  struct Packet {
    std::vector<std::uint8_t> data;
    std::int64_t pts = 0;
    bool eos = false;
  };
  std::deque<Packet>
      pending;  // input packets from queueInputBuffer, consumed by the driver
  int dbg_sent = 0,
      dbg_recv =
          0;  // ACL_MC_DIAG: log the first few send/receive on the driver
  std::uint64_t tot_sent = 0;
  std::int64_t last_sent_pts = 0;  // ACL_MC_DIAG: input packet cadence
  std::chrono::steady_clock::time_point
      t0_sent{};  // ACL_MC_DIAG: input-rate window start
  std::uint64_t tot_recv = 0, tot_queued = 0, tot_rel_render = 0,
                tot_rel_drop = 0;  // ACL_MC_DIAG totals
  std::uint64_t tot_poolfull =
      0;  // ACL_MC_DIAG: driver breaks on a full output pool
  double recv_ms_sum = 0.0,
         ahb_ms_sum =
             0.0;  // ACL_MC_DIAG: per-30 receive_frame / ahb_import time
  std::chrono::steady_clock::time_point
      t0_recv{};  // ACL_MC_DIAG: decode-rate window start
};

// Input packets the async driver holds before it stops offering buffers. A
// client fills every buffer it is offered; without a bound a player hands
// over everything it has read ahead.
constexpr std::size_t kMaxPendingPackets = 16;

// Whether this codec's frames go to the video plane: a video decoder while a
// View-tree window is up, which is where a SurfaceView's video is shown.
wm::IPresenter* video_plane_for(const CodecCtx* ctx) {
  return ctx->mime.rfind("video/", 0) == 0 ? wm::active_video_presenter()
                                           : nullptr;
}

// Give up whatever an output slot holds.
void drop_slot(CodecCtx::OutSlot& s) {
  if (s.ahb != nullptr)
    gfx::ahb_release(s.ahb);
  s = {};
}

bool is_audio(const CodecCtx* ctx) {
  return ctx->mime.rfind("audio/", 0) == 0;
}

CodecCtx* ctx_of(JNIEnv* env, jobject thiz) {
  return g_mc_ctx_fid != nullptr ? reinterpret_cast<CodecCtx*>(
                                       env->GetLongField(thiz, g_mc_ctx_fid))
                                 : nullptr;
}

// Read one MediaFormat entry's value as an int (Integer.intValue).
int int_value(JNIEnv* env, jobject v) {
  if (v == nullptr || g_integer_intvalue == nullptr)
    return 0;
  return static_cast<int>(env->CallIntMethod(v, g_integer_intvalue));
}

// Append a MediaFormat ByteBuffer value's bytes (csd) to `out`. Handles BOTH
// direct buffers and array-backed HeapByteBuffers — ExoPlayer passes
// csd-0/csd-1 as ByteBuffer.wrap(byte[]) (heap), so a direct-only read misses
// the SPS/PPS (csd=0B) and the decoder can't start.
void append_bytebuffer(JNIEnv* env,
                       jobject bb,
                       std::vector<std::uint8_t>& out) {
  if (bb == nullptr)
    return;
  if (void* addr = env->GetDirectBufferAddress(bb)) {
    const jlong cap = env->GetDirectBufferCapacity(bb);
    if (cap > 0)
      out.insert(out.end(), static_cast<std::uint8_t*>(addr),
                 static_cast<std::uint8_t*>(addr) + cap);
    return;
  }
  // Heap buffer: read remaining() bytes from its backing array at
  // arrayOffset()+position().
  jclass cls = env->GetObjectClass(bb);
  const jint rem =
      env->CallIntMethod(bb, env->GetMethodID(cls, "remaining", "()I"));
  if (rem > 0 &&
      env->CallBooleanMethod(bb, env->GetMethodID(cls, "hasArray", "()Z")) ==
          JNI_TRUE) {
    auto arr = static_cast<jbyteArray>(
        env->CallObjectMethod(bb, env->GetMethodID(cls, "array", "()[B")));
    const jint off =
        env->CallIntMethod(bb, env->GetMethodID(cls, "arrayOffset", "()I")) +
        env->CallIntMethod(bb, env->GetMethodID(cls, "position", "()I"));
    if (jbyte* e = env->GetByteArrayElements(arr, nullptr)) {
      out.insert(out.end(), reinterpret_cast<std::uint8_t*>(e) + off,
                 reinterpret_cast<std::uint8_t*>(e) + off + rem);
      env->ReleaseByteArrayElements(arr, e, JNI_ABORT);
    }
    if (arr)
      env->DeleteLocalRef(arr);
  }
  env->DeleteLocalRef(cls);
}

// Build the output MediaFormat as a HashMap<String,Object> —
// MediaCodec.getOutputFormat wraps it in a MediaFormat. A video renderer
// (ExoPlayer) reads width/height/crop to size the output Surface.
jobject build_format_map(JNIEnv* env, CodecCtx* ctx) {
  if (g_hashmap_cls == nullptr || g_hashmap_ctor == nullptr)
    return nullptr;
  jobject m = env->NewObject(g_hashmap_cls, g_hashmap_ctor);
  if (m == nullptr)
    return nullptr;
  auto put_str = [&](const char* k, const char* v) {
    jstring jk = env->NewStringUTF(k), jv = env->NewStringUTF(v);
    env->CallObjectMethod(m, g_hashmap_put, jk, jv);
    env->DeleteLocalRef(jk);
    env->DeleteLocalRef(jv);
  };
  auto put_int = [&](const char* k, int v) {
    jstring jk = env->NewStringUTF(k);
    jobject jv = env->CallStaticObjectMethod(g_integer_cls, g_integer_valueof,
                                             static_cast<jint>(v));
    env->CallObjectMethod(m, g_hashmap_put, jk, jv);
    env->DeleteLocalRef(jk);
    if (jv)
      env->DeleteLocalRef(jv);
  };
  if (is_audio(ctx)) {
    // What a decoder of audio puts out: 16-bit PCM at the stream's own rate
    // and channel count, known once the first frame has been decoded.
    const int rate = ctx->audio ? ctx->audio->sample_rate() : 0;
    const int channels = ctx->audio ? ctx->audio->channels() : 0;
    put_str("mime", "audio/raw");
    put_int("sample-rate", rate > 0 ? rate : ctx->sample_rate);
    put_int("channel-count", channels > 0 ? channels : ctx->channels);
    put_int("pcm-encoding", 2);  // AudioFormat.ENCODING_PCM_16BIT
    return m;
  }
  put_str("mime", "video/raw");
  put_int("width", ctx->width);
  put_int("height", ctx->height);
  put_int("color-format", 2135033992);  // COLOR_FormatYUV420Flexible (the frame
                                        // is a HW YUV surface)
  put_int("crop-left", 0);
  put_int("crop-top", 0);
  put_int("crop-right", ctx->width > 0 ? ctx->width - 1 : 0);
  put_int("crop-bottom", ctx->height > 0 ? ctx->height - 1 : 0);
  return m;
}

// The async driver thread: OWNS the CodecDecoder and delivers MediaCodec
// callbacks via postEventFromNative (Java routes them to the app's mCallback on
// its handler). Input is decoupled through ctx->pending, so the decoder is only
// ever touched here — no cross-thread avcodec race.
void async_driver(CodecCtx* ctx) {
  JNIEnv* env = nullptr;
  if (ctx->vm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) !=
      JNI_OK)
    return;
  auto post = [&](jint cb, jint arg2, jobject obj) {
    env->CallVoidMethod(ctx->self, g_post_event, EVENT_CALLBACK, cb, arg2, obj);
    if (env->ExceptionCheck())
      env->ExceptionClear();
  };
  {
    jobject fmt = build_format_map(env, ctx);
    post(CB_OUTPUT_FORMAT_CHANGE, 0, fmt);
    if (fmt)
      env->DeleteLocalRef(fmt);
  }
  std::string err;
  bool paced_pending = false;  // a packet the decoder had no room for
  while (!ctx->stop.load(std::memory_order_acquire)) {
    if (ctx->await_start.load(std::memory_order_acquire)) {
      // Flushed: nothing is offered or delivered until start().
      std::unique_lock<std::mutex> lk(ctx->amu);
      ctx->acv.wait_for(lk, std::chrono::milliseconds(20));
      continue;
    }
    // One pass with the decoder. flush() takes this lock, so it finds the
    // decoder between passes and what it clears is not refilled under it.
    std::unique_lock<std::mutex> pass(ctx->dmu);
    if (ctx->await_start.load(std::memory_order_acquire))
      continue;
    const auto now = std::chrono::steady_clock::now();
    std::vector<int> offer;
    std::vector<CodecCtx::Packet> send;
    paced_pending = false;
    {
      std::unique_lock<std::mutex> lk(ctx->amu);
      // Buffers are offered while the queue is short. Frames are not paced
      // here: a player shows each when it releases it, and the fixed output
      // pool below holds the decoder back to the rate the player takes them.
      for (std::size_t i = 0;
           i < ctx->inbufs.size() &&
           ctx->pending.size() + offer.size() < kMaxPendingPackets;
           ++i)
        if (ctx->inbufs[i].free) {
          ctx->inbufs[i].free = false;
          offer.push_back(static_cast<int>(i));
        }
      while (!ctx->pending.empty()) {
        send.push_back(std::move(ctx->pending.front()));
        ctx->pending.pop_front();
      }
    }
    static const bool diag = std::getenv("ACL_MC_DIAG") != nullptr;
    for (int idx : offer)
      post(CB_INPUT_AVAILABLE, idx,
           nullptr);  // "buffer idx is free — fill + queue it"
    for (std::size_t si = 0; si < send.size(); ++si) {
      auto& p = send[si];
      int sr;
      if (p.eos)
        sr = ctx->dec->send_packet(nullptr, 0, p.pts, &err);
      else
        sr = ctx->dec->send_packet(p.data.data(), p.data.size(), p.pts, &err);
      if (sr == 1) {
        // AVERROR(EAGAIN): the decoder's output is full — the packet was NOT
        // accepted. Requeue it (and the rest) to the FRONT of pending and stop;
        // the drain loop below frees output and the next tick re-sends them.
        // Dropping them here (the old bug) lost ~1/3 of frames -> 16fps not
        // 24fps.
        {
          std::lock_guard<std::mutex> lk(ctx->amu);
          for (std::size_t j = send.size(); j-- > si;)
            ctx->pending.push_front(std::move(send[j]));
        }
        paced_pending =
            true;  // re-check promptly (input isn't truly exhausted)
        break;
      }
      if (diag && !p.eos) {
        if (++ctx->tot_sent % 30 == 1) {
          double fps = 0.0;
          if (ctx->t0_sent.time_since_epoch().count() != 0)
            fps = 30.0 /
                  std::chrono::duration<double>(now - ctx->t0_sent).count();
          ctx->t0_sent = now;
          log::i(kTag,
                 "input: sent {} packets ({:.1f} pkt/s wall), pts-delta={}us",
                 ctx->tot_sent, fps, p.pts - ctx->last_sent_pts);
        }
        ctx->last_sent_pts = p.pts;
      }
      if (diag && ctx->dbg_sent < 4) {
        log::i(kTag, "async send_packet {}B eos={} pts={} -> {} {}",
               p.data.size(), p.eos, p.pts, sr, err);
        ctx->dbg_sent++;
      }
    }
    for (;;) {  // drain ready frames, with backpressure
      // BACKPRESSURE: a real MediaCodec has a FIXED output-buffer pool; when
      // the app holds them all it stops emitting frames (the decoder stalls)
      // until releaseOutputBuffer frees one. Find a free slot in the fixed
      // pool BEFORE decoding; if none, stop until a release frees one. This
      // is also what paces a paused player: it stops releasing, the pool
      // fills, and the decoder waits.
      wm::IPresenter* vpr = video_plane_for(ctx);
      int slot = -1;
      {
        std::lock_guard<std::mutex> lk(ctx->amu);
        for (std::size_t i = 0; i < ctx->outslots.size(); ++i)
          if (!ctx->outslots[i].used) {
            slot = static_cast<int>(i);
            break;
          }
        if (slot < 0) {
          ctx->tot_poolfull++;
          break;
        }  // pool full — wait for releaseOutputBuffer
      }
      gfx::DmabufFrame f{};
      std::int64_t pts = 0;
      const auto t_rf = std::chrono::steady_clock::now();
      const int rr = ctx->dec->receive_frame(&f, &pts, &err);
      if (diag && rr > 0) {
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t_rf)
                              .count();
        ctx->recv_ms_sum += ms;
      }
      if (diag && ctx->dbg_recv < 6) {
        log::i(kTag, "async receive_frame={} pts={} {}", rr, pts, err);
        ctx->dbg_recv++;
      }
      if (rr <= 0)
        break;
      if (diag && (++ctx->tot_recv % 30 == 1)) {
        const auto now = std::chrono::steady_clock::now();
        double fps = 0.0;
        if (ctx->t0_recv.time_since_epoch().count() != 0)
          fps =
              30.0 / std::chrono::duration<double>(now - ctx->t0_recv).count();
        ctx->t0_recv = now;
        log::i(kTag,
               "decode: received {} frames ({:.1f} fps over last 30), "
               "queued-to-app {}, "
               "released render={}/drop={}, pool-full-breaks={}, avg "
               "receive_frame={:.1f}ms "
               "avg ahb_import={:.1f}ms",
               ctx->tot_recv, fps, ctx->tot_queued, ctx->tot_rel_render,
               ctx->tot_rel_drop, ctx->tot_poolfull, ctx->recv_ms_sum / 30.0,
               ctx->ahb_ms_sum / 30.0);
        ctx->recv_ms_sum = 0.0;
        ctx->ahb_ms_sum = 0.0;
      }
      if (vpr != nullptr) {
        // The video plane: the frame waits in its slot, holding its decoder
        // surface, and goes to the plane when the app releases it, at the
        // time the app gives (MC_releaseOutputBuffer).
        {
          std::lock_guard<std::mutex> lk(ctx->amu);
          CodecCtx::OutSlot& s = ctx->outslots[slot];
          s.used = true;
          s.pts = pts;
          s.plane = true;
          s.frame = f;
        }
        jobject info = env->NewObject(g_bufinfo_cls, g_bufinfo_ctor);
        env->CallVoidMethod(info, g_bufinfo_set, 0, 0, static_cast<jlong>(pts),
                            0);
        ctx->tot_queued++;
        post(CB_OUTPUT_AVAILABLE, slot, info);
        if (info)
          env->DeleteLocalRef(info);
        continue;
      }
      const auto t_ahb = std::chrono::steady_clock::now();
      ::AHardwareBuffer* ahb = gfx::ahb_import(f);
      if (diag)
        ctx->ahb_ms_sum += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t_ahb)
                               .count();
      if (ahb == nullptr)
        continue;
      {
        std::lock_guard<std::mutex> lk(ctx->amu);
        CodecCtx::OutSlot& s = ctx->outslots[slot];
        s.ahb = ahb;
        s.used = true;
        s.pts = pts;
      }
      jobject info = env->NewObject(g_bufinfo_cls, g_bufinfo_ctor);
      env->CallVoidMethod(info, g_bufinfo_set, 0, 0, static_cast<jlong>(pts),
                          0);
      ctx->tot_queued++;
      post(CB_OUTPUT_AVAILABLE, slot, info);
      if (info)
        env->DeleteLocalRef(info);
    }
    pass.unlock();
    std::unique_lock<std::mutex> lk(ctx->amu);
    if (!ctx->stop.load(std::memory_order_acquire)) {
      if (paced_pending)
        ctx->acv.wait_for(lk,
                          std::chrono::milliseconds(
                              5));  // the decoder had no room — re-check soon
      else if (ctx->pending.empty())
        ctx->acv.wait_for(lk, std::chrono::milliseconds(4));
    }
  }
  ctx->vm->DetachCurrentThread();
}

// The async driver for an audio codec. The AudioCodec holds the buffers and
// the decoder; this thread turns its state into MediaCodec callbacks: an
// input buffer the client may fill, the output format, a buffer of PCM.
void audio_async_driver(CodecCtx* ctx) {
  JNIEnv* env = nullptr;
  if (ctx->vm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) !=
      JNI_OK)
    return;
  auto post = [&](jint cb, jint arg2, jobject obj) {
    env->CallVoidMethod(ctx->self, g_post_event, EVENT_CALLBACK, cb, arg2, obj);
    if (env->ExceptionCheck())
      env->ExceptionClear();
  };
  media::AudioCodec& codec = *ctx->audio;
  while (!ctx->stop.load(std::memory_order_acquire)) {
    if (ctx->await_start.load(std::memory_order_acquire)) {
      codec.wait_for_work(20);
      continue;
    }
    bool worked = false;
    for (int idx = codec.dequeue_input(); idx >= 0;
         idx = codec.dequeue_input()) {
      post(CB_INPUT_AVAILABLE, idx, nullptr);
      worked = true;
    }
    for (;;) {
      const media::AudioCodec::Output out = codec.dequeue_output();
      if (out.kind == media::AudioCodec::Output::Kind::TryAgain)
        break;
      worked = true;
      if (out.kind == media::AudioCodec::Output::Kind::FormatChanged) {
        jobject fmt = build_format_map(env, ctx);
        post(CB_OUTPUT_FORMAT_CHANGE, 0, fmt);
        if (fmt)
          env->DeleteLocalRef(fmt);
        continue;
      }
      jobject info = env->NewObject(g_bufinfo_cls, g_bufinfo_ctor);
      if (info != nullptr) {
        env->CallVoidMethod(info, g_bufinfo_set, 0,
                            static_cast<jint>(out.bytes),
                            static_cast<jlong>(out.pts_us),
                            out.end_of_stream ? BUFFER_FLAG_END_OF_STREAM : 0);
        post(CB_OUTPUT_AVAILABLE, out.index, info);
        env->DeleteLocalRef(info);
      }
      if (ctx->stop.load(std::memory_order_acquire) ||
          ctx->await_start.load(std::memory_order_acquire))
        break;
    }
    if (!worked)
      codec.wait_for_work(20);
  }
  ctx->vm->DetachCurrentThread();
}

// A direct ByteBuffer over `size` bytes at `mem`, in native byte order: PCM
// is read from it as shorts.
jobject pcm_byte_buffer(JNIEnv* env, void* mem, std::size_t size) {
  jobject bb = env->NewDirectByteBuffer(mem, static_cast<jlong>(size));
  if (bb == nullptr)
    return nullptr;
  jclass order_cls = env->FindClass("java/nio/ByteOrder");
  jclass bb_cls = env->FindClass("java/nio/ByteBuffer");
  if (order_cls != nullptr && bb_cls != nullptr) {
    const jmethodID native_order = env->GetStaticMethodID(
        order_cls, "nativeOrder", "()Ljava/nio/ByteOrder;");
    const jmethodID set_order = env->GetMethodID(
        bb_cls, "order", "(Ljava/nio/ByteOrder;)Ljava/nio/ByteBuffer;");
    if (native_order != nullptr && set_order != nullptr) {
      jobject order = env->CallStaticObjectMethod(order_cls, native_order);
      if (order != nullptr) {
        env->DeleteLocalRef(env->CallObjectMethod(bb, set_order, order));
        env->DeleteLocalRef(order);
      }
    }
  }
  if (env->ExceptionCheck())
    env->ExceptionClear();
  if (order_cls)
    env->DeleteLocalRef(order_cls);
  if (bb_cls)
    env->DeleteLocalRef(bb_cls);
  return bb;
}

// ── android.media.MediaCodec natives
// ──────────────────────────────────────────────────────────────
void JNICALL MC_native_init(JNIEnv*, jclass) {}  // ids cached at register time

void JNICALL MC_native_setup(JNIEnv* env,
                             jobject thiz,
                             jstring name,
                             jboolean nameIsType,
                             jboolean encoder,
                             jint /*pid*/,
                             jint /*uid*/) {
  if (encoder) {
    log::w(kTag, "encoder MediaCodec not supported");
    return;
  }
  auto* ctx = new CodecCtx();
  ctx->name_is_type = nameIsType == JNI_TRUE;
  if (name != nullptr) {
    const char* s = env->GetStringUTFChars(name, nullptr);
    ctx->name = s != nullptr ? s : "";
    if (s != nullptr)
      env->ReleaseStringUTFChars(name, s);
  }
  env->SetLongField(thiz, g_mc_ctx_fid, reinterpret_cast<jlong>(ctx));
  log::i(kTag, "native_setup name='{}' nameIsType={}", ctx->name,
         ctx->name_is_type);
}

void JNICALL MC_native_configure(JNIEnv* env,
                                 jobject thiz,
                                 jobjectArray keys,
                                 jobjectArray values,
                                 jobject surface,
                                 jobject /*crypto*/,
                                 jobject /*descrambler*/,
                                 jint /*flags*/) {
  CodecCtx* ctx = ctx_of(env, thiz);
  if (ctx == nullptr)
    return;
  // Parse the flattened MediaFormat (keys[], values[]) for
  // mime/width/height/csd. csd is collected BY INDEX (a std::map, ordered) —
  // the keys[] array order is not guaranteed, and H.264/HEVC require SPS
  // (csd-0) BEFORE PPS (csd-1), else avcodec sees "PPS references non-existing
  // SPS".
  std::map<int, std::vector<std::uint8_t>> csd_parts;
  const jsize n = keys != nullptr ? env->GetArrayLength(keys) : 0;
  for (jsize i = 0; i < n; ++i) {
    jstring k = static_cast<jstring>(env->GetObjectArrayElement(keys, i));
    jobject v = env->GetObjectArrayElement(values, i);
    const char* ks =
        k != nullptr ? env->GetStringUTFChars(k, nullptr) : nullptr;
    if (ks != nullptr) {
      if (std::strcmp(ks, "mime") == 0 && v != nullptr) {
        const char* vs =
            env->GetStringUTFChars(static_cast<jstring>(v), nullptr);
        if (vs != nullptr) {
          ctx->mime = vs;
          env->ReleaseStringUTFChars(static_cast<jstring>(v), vs);
        }
      } else if (std::strcmp(ks, "width") == 0) {
        ctx->width = int_value(env, v);
      } else if (std::strcmp(ks, "height") == 0) {
        ctx->height = int_value(env, v);
      } else if (std::strcmp(ks, "sample-rate") == 0) {
        ctx->sample_rate = int_value(env, v);
      } else if (std::strcmp(ks, "channel-count") == 0) {
        ctx->channels = int_value(env, v);
      } else if (std::strncmp(ks, "csd-", 4) == 0) {
        append_bytebuffer(
            env, v, csd_parts[std::atoi(ks + 4)]);  // by index: csd-0, csd-1, …
      }
      env->ReleaseStringUTFChars(k, ks);
    }
    if (k)
      env->DeleteLocalRef(k);
    if (v)
      env->DeleteLocalRef(v);
  }
  for (auto& [idx, part] : csd_parts)
    ctx->csd.insert(ctx->csd.end(), part.begin(), part.end());
  ctx->csd_parts = std::move(csd_parts);  // audio wants them apart
  // Resolve the output Surface -> its ImageReader BufferQueue (zero-copy sink).
  // null = no surface (buffer-mode decode, not supported for the zero-copy path
  // yet).
  ctx->out_queue = surface != nullptr
                       ? image_reader_queue_from_surface(env, surface)
                       : nullptr;
  if (ctx->mime.empty() && ctx->name_is_type)
    ctx->mime = ctx->name;  // createDecoderByType(mime)
  log::i(kTag, "native_configure mime='{}' {}x{} csd={}B surface_queue={}",
         ctx->mime, ctx->width, ctx->height, ctx->csd.size(),
         ctx->out_queue != nullptr);
}

void JNICALL MC_native_start(JNIEnv* env, jobject thiz) {
  CodecCtx* ctx = ctx_of(env, thiz);
  if (ctx == nullptr)
    return;
  if (ctx->started) {
    // start() after flush() resumes an asynchronous codec.
    if (ctx->await_start.exchange(false)) {
      if (ctx->audio)
        ctx->audio->wake();
      else
        ctx->acv.notify_all();
    }
    return;
  }
  std::string err;
  if (is_audio(ctx)) {
    media::AudioDecoder::Config cfg;
    cfg.mime = ctx->mime;
    cfg.sample_rate = ctx->sample_rate;
    cfg.channels = ctx->channels;
    for (auto& [idx, part] : ctx->csd_parts)
      cfg.csd.push_back(part);
    ctx->audio = media::AudioCodec::create(cfg, &err);
    if (!ctx->audio) {
      log::w(kTag, "start: no audio decoder for '{}': {}", ctx->mime, err);
      return;
    }
    ctx->started = true;
    ctx->await_start = false;
    if (ctx->async && ctx->vm != nullptr && ctx->self != nullptr)
      ctx->driver = std::thread(audio_async_driver, ctx);
    log::i(kTag, "native_start: audio decoder up{} for {}, {} csd buffer(s)",
           ctx->async ? " (ASYNC)" : "", ctx->mime, cfg.csd.size());
    return;
  }
  ctx->dec = media::CodecDecoder::configure(
      ctx->mime, ctx->csd.empty() ? nullptr : ctx->csd.data(), ctx->csd.size(),
      ctx->width, ctx->height, &err);
  if (!ctx->dec) {
    log::w(kTag, "start: CodecDecoder::configure failed: {}", err);
    return;
  }
  // Input buffer pool: a few generous buffers (compressed access units are far
  // smaller than a frame).
  const std::size_t bufsz = std::max<std::size_t>(
      256u * 1024u, static_cast<std::size_t>(std::max(ctx->width, 1) *
                                             std::max(ctx->height, 1)));
  ctx->inbufs.resize(4);
  for (auto& b : ctx->inbufs)
    b.mem.resize(bufsz);
  ctx->outslots.resize(std::getenv("ACL_MC_BIGPOOL") != nullptr ? 64 : 8);
  ctx->started = true;
  if (ctx->async && ctx->vm != nullptr && ctx->self != nullptr) {
    ctx->driver = std::thread(
        async_driver, ctx);  // async mode: the driver delivers callbacks
    log::i(kTag, "native_start: decoder up (ASYNC), {} input buffers x {}B",
           ctx->inbufs.size(), bufsz);
  } else {
    log::i(kTag, "native_start: decoder up, {} input buffers x {}B",
           ctx->inbufs.size(), bufsz);
  }
}

// How long a synchronous dequeue with a timeout waits when it has nothing:
// the timeout, capped, so that a client polling with one does not spin.
int dequeue_wait_ms(jlong timeout_us) {
  if (timeout_us == 0)
    return 0;
  if (timeout_us < 0 || timeout_us > 20000)
    return 20;
  return static_cast<int>(timeout_us / 1000);
}

jint JNICALL MC_native_dequeueInputBuffer(JNIEnv* env,
                                          jobject thiz,
                                          jlong timeoutUs) {
  CodecCtx* ctx = ctx_of(env, thiz);
  if (ctx == nullptr || !ctx->started)
    return INFO_TRY_AGAIN_LATER;
  if (ctx->audio) {
    int idx = ctx->audio->dequeue_input();
    if (idx < 0) {
      if (const int ms = dequeue_wait_ms(timeoutUs); ms > 0) {
        ctx->audio->wait_for_work(ms);
        idx = ctx->audio->dequeue_input();
      }
    }
    return idx >= 0 ? idx : INFO_TRY_AGAIN_LATER;
  }
  for (std::size_t i = 0; i < ctx->inbufs.size(); ++i)
    if (ctx->inbufs[i].free) {
      ctx->inbufs[i].free = false;
      return static_cast<jint>(i);
    }
  return INFO_TRY_AGAIN_LATER;  // all input buffers in flight — dequeue output
                                // / retry
}

jobject JNICALL MC_getBuffer(JNIEnv* env,
                             jobject thiz,
                             jboolean input,
                             jint index) {
  CodecCtx* ctx = ctx_of(env, thiz);
  if (ctx == nullptr)
    return nullptr;
  if (ctx->audio) {
    if (input == JNI_TRUE) {
      std::uint8_t* mem = ctx->audio->input_data(index);
      return mem != nullptr
                 ? env->NewDirectByteBuffer(
                       mem, static_cast<jlong>(ctx->audio->input_bytes()))
                 : nullptr;
    }
    std::size_t bytes = 0;
    const std::int16_t* pcm = ctx->audio->output_data(index, &bytes);
    if (pcm == nullptr)
      return nullptr;
    // The memory is the codec's and is reused after releaseOutputBuffer.
    // MediaCodec.java frees the ByteBuffer it tracks for an index when the
    // index is released or the codec is flushed, so a stale one throws
    // instead of reaching the memory.
    return pcm_byte_buffer(env, const_cast<std::int16_t*>(pcm), bytes);
  }
  if (input == JNI_TRUE) {
    if (index < 0 || static_cast<std::size_t>(index) >= ctx->inbufs.size())
      return nullptr;
    auto& b = ctx->inbufs[index];
    return env->NewDirectByteBuffer(b.mem.data(),
                                    static_cast<jlong>(b.mem.size()));
  }
  return nullptr;  // output is Surface-backed — no CPU-accessible output buffer
}

void JNICALL MC_native_queueInputBuffer(JNIEnv* env,
                                        jobject thiz,
                                        jint index,
                                        jint offset,
                                        jint size,
                                        jlong pts,
                                        jint flags) {
  CodecCtx* ctx = ctx_of(env, thiz);
  if (ctx == nullptr || !ctx->started)
    return;
  if (ctx->audio) {
    // Codec configuration queued as a buffer is what the format's csd already
    // said; the buffer is taken back empty.
    const bool config = (flags & BUFFER_FLAG_CODEC_CONFIG) != 0;
    const bool eos = (flags & BUFFER_FLAG_END_OF_STREAM) != 0;
    if (!ctx->audio->queue_input(index, config ? 0 : offset, config ? 0 : size,
                                 pts, eos)) {
      static int logged = 0;
      if (logged++ < 4)
        log::w(kTag,
               "queueInputBuffer: index {} offset {} size {} is not a buffer "
               "the client holds, or runs past it; ignored",
               index, offset, size);
    }
    return;
  }
  if (ctx->dec == nullptr)
    return;
  std::string err;
  if (ctx->async) {
    // Async: copy the packet for the driver + re-free the buffer (the driver
    // re-offers it). The decoder is only touched on the driver thread, so no
    // cross-thread avcodec access.
    std::lock_guard<std::mutex> lk(ctx->amu);
    CodecCtx::Packet p;
    p.pts = pts;
    if ((flags & BUFFER_FLAG_END_OF_STREAM) != 0) {
      p.eos = true;
    } else if (index >= 0 &&
               static_cast<std::size_t>(index) < ctx->inbufs.size() &&
               offset >= 0 && size > 0 &&
               static_cast<std::size_t>(offset + size) <=
                   ctx->inbufs[index].mem.size()) {
      const auto& mem = ctx->inbufs[index].mem;
      // ExoPlayer's samples carry NO in-band SPS/PPS (they're in the
      // csd/extradata); prepend csd to the first access unit so avcodec has PPS
      // 0 before the first slice ("non-existing PPS 0" else).
      if (ctx->first_input && !ctx->csd.empty()) {
        p.data = ctx->csd;
        ctx->first_input = false;
      }
      p.data.insert(p.data.end(), mem.begin() + offset,
                    mem.begin() + offset + size);
    }
    ctx->pending.push_back(std::move(p));
    if (index >= 0 && static_cast<std::size_t>(index) < ctx->inbufs.size())
      ctx->inbufs[index].free = true;
    ctx->acv.notify_one();
    return;
  }
  if ((flags & BUFFER_FLAG_END_OF_STREAM) != 0 && size == 0) {
    ctx->dec->send_packet(nullptr, 0, pts, &err);  // drain
  } else if (index >= 0 &&
             static_cast<std::size_t>(index) < ctx->inbufs.size()) {
    const auto& b = ctx->inbufs[index];
    if (offset >= 0 && size > 0 &&
        static_cast<std::size_t>(offset + size) <= b.mem.size()) {
      // CODEC_CONFIG (SPS/PPS) buffers are sent too — avcodec parses in-band
      // NAL units harmlessly.
      const int r = ctx->dec->send_packet(
          b.mem.data() + offset, static_cast<std::size_t>(size), pts, &err);
      if (r < 0)
        log::w(kTag, "queueInputBuffer: send_packet failed: {}", err);
    }
  }
  (void)BUFFER_FLAG_CODEC_CONFIG;
  if (index >= 0 && static_cast<std::size_t>(index) < ctx->inbufs.size())
    ctx->inbufs[index].free = true;
}

jint JNICALL MC_native_dequeueOutputBuffer(JNIEnv* env,
                                           jobject thiz,
                                           jobject info,
                                           jlong timeoutUs) {
  CodecCtx* ctx = ctx_of(env, thiz);
  if (ctx == nullptr || !ctx->started)
    return INFO_TRY_AGAIN_LATER;
  if (ctx->audio) {
    using Kind = media::AudioCodec::Output::Kind;
    media::AudioCodec::Output out = ctx->audio->dequeue_output();
    if (out.kind == Kind::TryAgain) {
      if (const int ms = dequeue_wait_ms(timeoutUs); ms > 0) {
        ctx->audio->wait_for_work(ms);
        out = ctx->audio->dequeue_output();
      }
    }
    if (out.kind == Kind::TryAgain)
      return INFO_TRY_AGAIN_LATER;
    if (out.kind == Kind::FormatChanged)
      return INFO_OUTPUT_FORMAT_CHANGED;
    if (g_bufinfo_set != nullptr && info != nullptr)
      env->CallVoidMethod(info, g_bufinfo_set, 0, static_cast<jint>(out.bytes),
                          static_cast<jlong>(out.pts_us),
                          out.end_of_stream ? BUFFER_FLAG_END_OF_STREAM : 0);
    return out.index;
  }
  if (ctx->dec == nullptr)
    return INFO_TRY_AGAIN_LATER;
  // Signal the output format once before the first frame (ExoPlayer's
  // MediaCodecRenderer reads getOutputFormat() on this, then continues
  // dequeuing). Our dimensions are known from configure.
  if (!ctx->format_signaled) {
    ctx->format_signaled = true;
    return INFO_OUTPUT_FORMAT_CHANGED;
  }
  gfx::DmabufFrame f{};
  std::int64_t pts = 0;
  std::string err;
  const int r = ctx->dec->receive_frame(&f, &pts, &err);
  if (r <= 0)
    return INFO_TRY_AGAIN_LATER;  // 0 = feed more input; -1 handled as
                                  // try-again (logged)
  // Import the decoded frame zero-copy NOW (dup the fd) so the decoder may
  // reuse its surface; hold it in an output slot until releaseOutputBuffer
  // decides render (queue_imported) or drop (release). A frame for the video
  // plane is kept as it is instead.
  const bool plane = video_plane_for(ctx) != nullptr;
  ::AHardwareBuffer* ahb = plane ? nullptr : gfx::ahb_import(f);
  if (!plane && ahb == nullptr) {
    log::w(kTag, "dequeueOutputBuffer: ahb_import failed");
    return INFO_TRY_AGAIN_LATER;
  }
  std::size_t slot = ctx->outslots.size();
  for (std::size_t i = 0; i < ctx->outslots.size(); ++i)
    if (!ctx->outslots[i].used) {
      slot = i;
      break;
    }
  if (slot == ctx->outslots.size())
    ctx->outslots.push_back({});
  {
    CodecCtx::OutSlot& s = ctx->outslots[slot];
    s.ahb = ahb;
    s.used = true;
    s.pts = pts;
    s.plane = plane;
    if (plane)
      s.frame = f;
  }
  if (g_bufinfo_set != nullptr)
    env->CallVoidMethod(info, g_bufinfo_set, 0, 0, static_cast<jlong>(pts),
                        0);  // Surface mode: size 0
  return static_cast<jint>(slot);
}

void JNICALL MC_releaseOutputBuffer(JNIEnv* env,
                                    jobject thiz,
                                    jint index,
                                    jboolean render,
                                    jboolean /*updatePTS*/,
                                    jlong timeNs) {
  CodecCtx* ctx = ctx_of(env, thiz);
  if (ctx == nullptr || index < 0)
    return;
  if (ctx->audio) {
    ctx->audio->release_output(index);
    return;
  }
  ::AHardwareBuffer* ahb = nullptr;
  std::int64_t rel_pts = 0;
  bool plane = false;
  gfx::DmabufFrame plane_frame{};
  {
    std::lock_guard<std::mutex> lk(
        ctx->amu);  // outslots shared with the async driver thread
    if (static_cast<std::size_t>(index) >= ctx->outslots.size())
      return;
    auto& s = ctx->outslots[index];
    if (!s.used)
      return;
    ahb = s.ahb;
    rel_pts = s.pts;
    plane = s.plane;
    if (plane)
      plane_frame = s.frame;
    s = {};
  }
  ctx->acv.notify_one();  // a pool slot freed — resume the backpressured driver
  static const bool diag = std::getenv("ACL_MC_DIAG") != nullptr;
  if (render == JNI_TRUE)
    ctx->tot_rel_render++;
  else
    ctx->tot_rel_drop++;
  const std::uint64_t tot_rel = ctx->tot_rel_render + ctx->tot_rel_drop;
  if (diag && (tot_rel <= 40 || tot_rel % 30 == 1))
    log::i(kTag,
           "app releaseOutputBuffer idx={} render={} pts={}us timeNs={} "
           "(render-tot={} drop-tot={} recv={}) early={}us",
           index, render == JNI_TRUE, rel_pts, timeNs, ctx->tot_rel_render,
           ctx->tot_rel_drop, ctx->tot_recv,
           (timeNs - std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count()) /
               1000);
  if (plane) {
    // The video plane, at the time the app asked for, like the queue below.
    // The frame carries its hold on the decoder's surface until it has been
    // shown or dropped. A frame not rendered just lets go of it.
    if (render == JNI_TRUE) {
      static const bool now = std::getenv("ACL_VIDEO_RELEASE_NOW") != nullptr;
      ctx->present.at(now ? 0 : timeNs, [plane_frame](bool due) {
        if (!due)
          return;
        if (wm::IPresenter* p = wm::active_video_presenter())
          p->present_video(plane_frame, 0, 0,
                           static_cast<int>(plane_frame.width),
                           static_cast<int>(plane_frame.height));
      });
    }
  } else if (render == JNI_TRUE && ctx->out_queue != nullptr) {
    // Zero-copy: hand the frame to the ImageReader consumer, at the time the
    // app asked for. A player releases a frame ahead of its display time
    // (media3: up to 50 ms) and leaves the wait to the platform; shown at
    // once, the picture leads the sound by that much. ACL_VIDEO_RELEASE_NOW
    // shows it at once, for comparison.
    static const bool now = std::getenv("ACL_VIDEO_RELEASE_NOW") != nullptr;
    gfx::BufferQueue* queue = ctx->out_queue;
    ctx->present.at(now ? 0 : timeNs, [queue, ahb](bool due) {
      if (due)
        queue->queue_imported(ahb);
      else
        gfx::ahb_release(ahb);
    });
  } else {
    gfx::ahb_release(ahb);  // not rendered (or no surface) — drop
  }
}

void teardown(CodecCtx* ctx) {
  if (ctx == nullptr)
    return;
  ctx->stop.store(true,
                  std::memory_order_release);  // stop the async driver (if any)
                                               // before the decoder
  ctx->acv.notify_all();
  ctx->present.cancel_all();  // frames waiting for their time are dropped
  if (ctx->audio)
    ctx->audio->wake();
  if (ctx->driver.joinable())
    ctx->driver.join();
  ctx->audio.reset();
  ctx->stop.store(false,
                  std::memory_order_release);  // a codec may be started again
  for (auto& s : ctx->outslots)
    drop_slot(s);
  ctx->outslots.clear();
  ctx->dec.reset();
  ctx->started = false;
}

void JNICALL MC_native_stop(JNIEnv* env, jobject thiz) {
  teardown(ctx_of(env, thiz));
}
void JNICALL MC_native_flush(JNIEnv* env, jobject thiz) {
  CodecCtx* ctx = ctx_of(
      env,
      thiz);  // drop any pending imported outputs; the decoder keeps its config
  if (ctx == nullptr)
    return;
  if (ctx->audio) {
    // Asynchronous: no callbacks until start() is called again.
    if (ctx->async)
      ctx->await_start = true;
    ctx->audio->flush();
    return;
  }
  if (ctx->dec == nullptr)
    return;
  // A seek. Everything the client held is void (MediaCodec.flush has already
  // forgotten its buffer indices), so every input buffer is free again and
  // every output slot empty; what was queued, decoding or waiting for its
  // time is stale. Asynchronous: nothing is offered until start().
  std::lock_guard<std::mutex> pass(ctx->dmu);  // between the driver's passes
  ctx->present.cancel_all();
  {
    std::lock_guard<std::mutex> lk(ctx->amu);
    if (ctx->async)
      ctx->await_start = true;
    ctx->pending.clear();
    for (auto& b : ctx->inbufs)
      b.free = true;
    for (auto& s : ctx->outslots)
      drop_slot(s);
    // The decoder keeps its parameter sets across a flush, but they cost
    // nothing to send again with the key frame that follows.
    ctx->first_input = true;
  }
  ctx->dec->flush();
  ctx->acv.notify_all();
}
void JNICALL MC_native_reset(JNIEnv* env, jobject thiz) {
  teardown(ctx_of(env, thiz));
}
void JNICALL MC_native_release(JNIEnv* env, jobject thiz) {
  CodecCtx* ctx = ctx_of(env, thiz);
  if (ctx == nullptr)
    return;
  teardown(ctx);
  if (ctx->self != nullptr)
    env->DeleteGlobalRef(ctx->self);  // the async self-ref
  delete ctx;
  env->SetLongField(thiz, g_mc_ctx_fid, 0);
}
void JNICALL MC_native_finalize(JNIEnv* env, jobject thiz) {
  MC_native_release(env, thiz);
}

// Stubs for the surface / metadata / async methods a basic synchronous decode
// doesn't drive.
void JNICALL MC_native_setSurface(JNIEnv* env, jobject thiz, jobject surface) {
  CodecCtx* ctx = ctx_of(env, thiz);
  if (ctx != nullptr)
    ctx->present.cancel_all();  // they were for the old surface
  if (ctx != nullptr)
    ctx->out_queue = surface != nullptr
                         ? image_reader_queue_from_surface(env, surface)
                         : nullptr;
}
jobject JNICALL MC_getImage(JNIEnv*, jobject, jboolean, jint) {
  return nullptr;
}
jstring JNICALL MC_native_getName(JNIEnv* env, jobject thiz) {
  CodecCtx* ctx = ctx_of(env, thiz);
  return env->NewStringUTF(
      ctx != nullptr && !ctx->name.empty() ? ctx->name.c_str() : "acl.decoder");
}
jstring JNICALL MC_getCanonicalName(JNIEnv* env, jobject thiz) {
  return MC_native_getName(env, thiz);
}
void JNICALL MC_native_setCallback(JNIEnv* env, jobject thiz, jobject cb) {
  CodecCtx* ctx = ctx_of(env, thiz);
  if (ctx == nullptr)
    return;
  if (cb != nullptr) {  // a non-null callback selects async mode; native_start
                        // launches the driver
    ctx->async = true;
    if (ctx->self == nullptr)
      ctx->self = env->NewGlobalRef(thiz);
    env->GetJavaVM(&ctx->vm);
  }
}
void JNICALL MC_native_enableOnFrameRenderedListener(JNIEnv*,
                                                     jobject,
                                                     jboolean) {}
void JNICALL MC_native_enableOnFirstTunnelFrameReadyListener(JNIEnv*,
                                                             jobject,
                                                             jboolean) {}
jobject JNICALL MC_getFormatNative(JNIEnv* env,
                                   jobject thiz,
                                   jboolean /*input*/) {
  CodecCtx* ctx = ctx_of(env, thiz);
  return ctx != nullptr ? build_format_map(env, ctx) : nullptr;
}
jobject JNICALL MC_getOutputFormatNative(JNIEnv* env,
                                         jobject thiz,
                                         jint /*index*/) {
  CodecCtx* ctx = ctx_of(env, thiz);
  return ctx != nullptr ? build_format_map(env, ctx) : nullptr;
}
jobject JNICALL MC_native_getMetrics(JNIEnv*, jobject) {
  return nullptr;
}

bool bind(JNIEnv* env, jclass c, const JNINativeMethod* m, int n) {
  const bool ok = env->RegisterNatives(c, m, n) == JNI_OK;
  if (!ok)
    env->ExceptionClear();
  return ok;
}
}  // namespace

void register_media_codec_natives(JNIEnv* env) {
  if (env == nullptr)
    return;
  jclass mc = env->FindClass("android/media/MediaCodec");
  if (mc == nullptr) {
    env->ExceptionClear();
    return;
  }  // no framework MediaCodec -> nothing to do
  g_mc_cls = static_cast<jclass>(env->NewGlobalRef(mc));
  g_mc_ctx_fid = env->GetFieldID(mc, "mNativeContext", "J");
  g_post_event =
      env->GetMethodID(mc, "postEventFromNative", "(IIILjava/lang/Object;)V");
  if (env->ExceptionCheck())
    env->ExceptionClear();

  if (jclass bi = env->FindClass("android/media/MediaCodec$BufferInfo");
      bi != nullptr) {
    g_bufinfo_cls = static_cast<jclass>(env->NewGlobalRef(bi));
    g_bufinfo_ctor = env->GetMethodID(bi, "<init>", "()V");
    g_bufinfo_set = env->GetMethodID(bi, "set", "(IIJI)V");
    if (env->ExceptionCheck())
      env->ExceptionClear();
    env->DeleteLocalRef(bi);
  }
  if (jclass ic = env->FindClass("java/lang/Integer"); ic != nullptr) {
    g_integer_cls = static_cast<jclass>(env->NewGlobalRef(ic));
    g_integer_intvalue = env->GetMethodID(ic, "intValue", "()I");
    g_integer_valueof =
        env->GetStaticMethodID(ic, "valueOf", "(I)Ljava/lang/Integer;");
    env->DeleteLocalRef(ic);
  }
  if (jclass hm = env->FindClass("java/util/HashMap"); hm != nullptr) {
    g_hashmap_cls = static_cast<jclass>(env->NewGlobalRef(hm));
    g_hashmap_ctor = env->GetMethodID(hm, "<init>", "()V");
    g_hashmap_put = env->GetMethodID(
        hm, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    env->DeleteLocalRef(hm);
  }
  if (env->ExceptionCheck())
    env->ExceptionClear();

  static const JNINativeMethod m[] = {
      {const_cast<char*>("native_init"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&MC_native_init)},
      {const_cast<char*>("native_setup"),
       const_cast<char*>("(Ljava/lang/String;ZZII)V"),
       reinterpret_cast<void*>(&MC_native_setup)},
      {const_cast<char*>("native_configure"),
       const_cast<char*>(
           "([Ljava/lang/String;[Ljava/lang/Object;Landroid/view/"
           "Surface;Landroid/media/MediaCrypto;Landroid/os/IHwBinder;I)V"),
       reinterpret_cast<void*>(&MC_native_configure)},
      {const_cast<char*>("native_start"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&MC_native_start)},
      {const_cast<char*>("native_stop"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&MC_native_stop)},
      {const_cast<char*>("native_flush"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&MC_native_flush)},
      {const_cast<char*>("native_reset"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&MC_native_reset)},
      {const_cast<char*>("native_release"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&MC_native_release)},
      {const_cast<char*>("native_finalize"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&MC_native_finalize)},
      {const_cast<char*>("native_dequeueInputBuffer"),
       const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&MC_native_dequeueInputBuffer)},
      {const_cast<char*>("native_queueInputBuffer"),
       const_cast<char*>("(IIIJI)V"),
       reinterpret_cast<void*>(&MC_native_queueInputBuffer)},
      {const_cast<char*>("native_dequeueOutputBuffer"),
       const_cast<char*>("(Landroid/media/MediaCodec$BufferInfo;J)I"),
       reinterpret_cast<void*>(&MC_native_dequeueOutputBuffer)},
      {const_cast<char*>("releaseOutputBuffer"), const_cast<char*>("(IZZJ)V"),
       reinterpret_cast<void*>(&MC_releaseOutputBuffer)},
      {const_cast<char*>("getBuffer"),
       const_cast<char*>("(ZI)Ljava/nio/ByteBuffer;"),
       reinterpret_cast<void*>(&MC_getBuffer)},
      {const_cast<char*>("getImage"),
       const_cast<char*>("(ZI)Landroid/media/Image;"),
       reinterpret_cast<void*>(&MC_getImage)},
      {const_cast<char*>("native_setSurface"),
       const_cast<char*>("(Landroid/view/Surface;)V"),
       reinterpret_cast<void*>(&MC_native_setSurface)},
      // Hints a running codec may be given: a player sets the operating rate
      // when the playback speed changes. None changes what a4l's decoders
      // do, so they are taken and ignored.
      {const_cast<char*>("setParameters"),
       const_cast<char*>("([Ljava/lang/String;[Ljava/lang/Object;)V"),
       reinterpret_cast<void*>(
           +[](JNIEnv*, jobject, jobjectArray, jobjectArray) {})},
      // ExoPlayer sets the scaling mode after configure; acl scales via the
      // compositor, so no-op it (unbound, it kills the ExoPlayer:Playback
      // thread).
      {const_cast<char*>("setVideoScalingMode"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(+[](JNIEnv*, jobject, jint) {})},
      {const_cast<char*>("getCanonicalName"),
       const_cast<char*>("()Ljava/lang/String;"),
       reinterpret_cast<void*>(&MC_getCanonicalName)},
      {const_cast<char*>("native_setCallback"),
       const_cast<char*>("(Landroid/media/MediaCodec$Callback;)V"),
       reinterpret_cast<void*>(&MC_native_setCallback)},
      {const_cast<char*>("native_enableOnFrameRenderedListener"),
       const_cast<char*>("(Z)V"),
       reinterpret_cast<void*>(&MC_native_enableOnFrameRenderedListener)},
      {const_cast<char*>("native_enableOnFirstTunnelFrameReadyListener"),
       const_cast<char*>("(Z)V"),
       reinterpret_cast<void*>(
           &MC_native_enableOnFirstTunnelFrameReadyListener)},
      {const_cast<char*>("getFormatNative"),
       const_cast<char*>("(Z)Ljava/util/Map;"),
       reinterpret_cast<void*>(&MC_getFormatNative)},
      {const_cast<char*>("getOutputFormatNative"),
       const_cast<char*>("(I)Ljava/util/Map;"),
       reinterpret_cast<void*>(&MC_getOutputFormatNative)},
      {const_cast<char*>("native_getMetrics"),
       const_cast<char*>("()Landroid/os/PersistableBundle;"),
       reinterpret_cast<void*>(&MC_native_getMetrics)},
  };
  const bool ok = bind(env, mc, m, static_cast<int>(sizeof(m) / sizeof(m[0])));
  env->DeleteLocalRef(mc);
  log::write(
      ok ? log::Level::Info : log::Level::Warn, kTag,
      ok ? "MediaCodec natives bound (sync decode -> Surface over CodecDecoder)"
         : "MediaCodec native bind incomplete");
}

}  // namespace acl::runtime

#endif  // ACL_MEDIA_CODEC_JNI

#if ACL_HAVE_GL && !ACL_MEDIA_CODEC_JNI
// Host GL without one of the two halves above: either no wcs presenter and
// Ganesh Vulkan device, or no FFmpeg, so the decode-to-plane path is not built.
// The libmedia_jni load path in jvm_vm.cpp still registers the ImageReader and
// MediaCodecList natives beside this one, under ACL_HAVE_GL alone, so the entry
// point has to stay linkable.
namespace acl::runtime {
void register_media_codec_natives(JNIEnv*) {}
}  // namespace acl::runtime
#endif
