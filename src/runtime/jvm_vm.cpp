// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/runtime.md (pending), JNI Invocation API & JNI Functions docs
// (Oracle/OpenJDK), ADR-0001. No GPL-3.0 sources.
//
// OpenJDK-backed VM (ADR-0001 fallback). Embeds HotSpot via the JNI Invocation
// API. This runs stock JVM bytecode (.class/.jar); DEX bytecode is not loaded by
// a stock JVM — a d8-reversal/dex2jar step feeds this path later. HotSpot permits
// exactly one VM per process and does not support re-creation after
// DestroyJavaVM, so tests spawn a dedicated process.

#include "runtime/jvm_vm.h"

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <cstdlib>
#include <ctime>

#include <dlfcn.h>
#include <jni.h>

#include "acl_config.h"
#if ACL_HAVE_SDBUS
#include "svc/battery/battery_jni.h"
#include "svc/bluetooth/bt_jni.h"
#include "svc/net/connectivity_jni.h"
#include "svc/notify/notify_jni.h"
#include "svc/storage/storage_jni.h"
#endif
#if ACL_HAVE_PIPEWIRE
#include "audio/audio_jni.h"
#endif
#include "audio/audio.h"
#include "gfx/gfx.h"
#if ACL_HAVE_VULKAN
#include "gfx/gfx_vk.h"  // S3b: VkRenderTarget import_pixels_to_dmabuf
#endif
#include "log/log.h"
#include "runtime/framework_natives.h"
#include "svc/svc.h"
#include "wm/present.h"
#if ACL_PROFILE_AUTOMOTIVE
#include "hosts/cal/cal.h"             // Gate G1.5b: Car App Library template host
#include "hosts/media/media_bridge.h"  // Gate G1.5a: headless media bridge (automotive)
#endif

namespace acl::runtime {
namespace {

constexpr std::string_view kTag = "acl.runtime.jvm";

// StrictMode thread policy slot (android.os.Binder.set/getThreadStrictModePolicy). Per-thread,
// like the real Binder thread-local; no Binder, so it just round-trips in-process.
thread_local jint g_strict_mode_policy = 0;

#if ACL_HAVE_VULKAN && ACL_HAVE_WCS
// ── Looper-driven present (approach A): native seam for org.acl.internal.PresentDriver ──
// The Java driver owns Looper.loop() and the Choreographer; these natives expose the wm
// presenter primitives, the GPU begin/end-frame, and the vsync-delivery hooks so a real
// Android frame loop drives dma-buf submission. See docs/looper-driven-present.md.

// Mirror of libacl_graphics_jni's AclDmabuf (the GPU end-frame output descriptor).
struct PresentDmabuf {
  int fd;
  std::uint32_t width, height, plane_count;
  std::uint32_t offsets[4], strides[4];
  std::uint64_t modifier;
  std::uint32_t drm_format;
};

// Map a Linux evdev scancode to an Android KeyEvent keycode (a small practical subset;
// unmapped keys become KEYCODE_UNKNOWN=0). Mirrors Android's Generic.kl.
jint acl_evdev_to_android_keycode(std::uint32_t evdev) {
  switch (evdev) {
    case 30: return 29;  case 48: return 30;  case 46: return 31;  case 32: return 32;  // A B C D
    case 18: return 33;  case 33: return 34;  case 34: return 35;  case 35: return 36;  // E F G H
    case 23: return 37;  case 36: return 38;  case 37: return 39;  case 38: return 40;  // I J K L
    case 50: return 41;  case 49: return 42;  case 24: return 43;  case 25: return 44;  // M N O P
    case 16: return 45;  case 19: return 46;  case 31: return 47;  case 20: return 48;  // Q R S T
    case 22: return 49;  case 47: return 50;  case 17: return 51;  case 45: return 52;  // U V W X
    case 21: return 53;  case 44: return 54;                                            // Y Z
    case 2: return 8;   case 3: return 9;   case 4: return 10;  case 5: return 11;       // 1 2 3 4
    case 6: return 12;  case 7: return 13;  case 8: return 14;  case 9: return 15;       // 5 6 7 8
    case 10: return 16; case 11: return 7;                                               // 9 0
    case 28: return 66;  case 57: return 62;  case 1: return 111;  case 14: return 67;   // ENT SPC ESC DEL
    case 15: return 61;  case 105: return 21; case 106: return 22;                       // TAB LEFT RIGHT
    case 103: return 19; case 108: return 20;                                            // UP DOWN
    default: return 0;
  }
}

// A monotonic timestamp (ns) for MotionEvent/KeyEvent times.
std::uint64_t present_now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

// Per-window native state for PresentDriver; its address is the jlong `s` handed to Java.
struct PresentSession {
  std::unique_ptr<wm::IPresenter> presenter;
  JavaVM* vm = nullptr;
  jobject view = nullptr;    // global ref to the decor View
  jobject canvas = nullptr;  // global ref to the android.graphics.Canvas over the GPU SkCanvas
  int w = 0, h = 0;
  jint wspec = 0, hspec = 0;  // MeasureSpec.EXACTLY(w/h)
  long (*gpu_begin)(int, int) = nullptr;
  bool (*gpu_end)(int, PresentDmabuf*) = nullptr;
  void (*deliver_vsync)(JNIEnv*, jlong) = nullptr;
  int (*vsync_pending)() = nullptr;
  void (*vsync_set_immediate)(int) = nullptr;
  jmethodID m_measure = nullptr, m_gmw = nullptr, m_gmh = nullptr, m_layout = nullptr, m_draw = nullptr;
  jclass motionevent_cls = nullptr, keyevent_cls = nullptr;  // global refs
  jmethodID me_obtain = nullptr, me_recycle = nullptr, ke_obtain = nullptr;
  jmethodID dispatch_touch = nullptr, dispatch_key = nullptr;
  std::uint64_t down_time = 0;
  std::uint64_t frames = 0;  // presented-frame count (progress evidence)
};

// The current thread's JNIEnv (input callbacks run on the main thread during nDispatch, but
// were created in nOpen — re-fetch rather than capture a possibly-stale env).
JNIEnv* present_env(PresentSession* s) {
  JNIEnv* e = nullptr;
  if (s != nullptr && s->vm != nullptr) s->vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6);
  return e;
}

// Deliver a compositor pointer/key event to the decor View (native side of MotionEvent/
// KeyEvent). Runs synchronously inside presenter->dispatch() on the main thread.
void present_deliver_input(PresentSession* s, const wm::InputEvent& ev) {
  JNIEnv* env = present_env(s);
  if (env == nullptr) return;
  if (const auto* p = std::get_if<wm::PointerInput>(&ev)) {
    if (s->motionevent_cls == nullptr || s->me_obtain == nullptr || s->dispatch_touch == nullptr) return;
    const jint action = p->action == wm::PointerInput::Action::Down ? 0
                      : p->action == wm::PointerInput::Action::Up   ? 1 : 2;
    if (action == 0) s->down_time = present_now_ns();
    jobject me = env->CallStaticObjectMethod(
        s->motionevent_cls, s->me_obtain, static_cast<jlong>(s->down_time),
        static_cast<jlong>(present_now_ns()), action, static_cast<jfloat>(p->x),
        static_cast<jfloat>(p->y), 0);
    if (me != nullptr) {
      env->CallBooleanMethod(s->view, s->dispatch_touch, me);
      if (s->me_recycle != nullptr) env->CallVoidMethod(me, s->me_recycle);
      env->DeleteLocalRef(me);
    }
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
  } else if (const auto* k = std::get_if<wm::KeyInput>(&ev)) {
    if (s->keyevent_cls == nullptr || s->ke_obtain == nullptr || s->dispatch_key == nullptr) return;
    const jint action = k->action == wm::KeyInput::Action::Down ? 0 : 1;
    jobject ke = env->CallStaticObjectMethod(s->keyevent_cls, s->ke_obtain, action,
                                             acl_evdev_to_android_keycode(k->keycode));
    if (ke != nullptr) {
      env->CallBooleanMethod(s->view, s->dispatch_key, ke);
      env->DeleteLocalRef(ke);
    }
    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
  }
}

jlong JNICALL PresentDriver_nOpen(JNIEnv* env, jclass, jobject view, jint w, jint h,
                                  jstring title, jstring appId) {
  if (!wm::wayland_available()) {
    log::i(kTag, "PresentDriver.nOpen: no Wayland compositor reachable");
    return 0;
  }
  auto begin = reinterpret_cast<long (*)(int, int)>(
      ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_begin_frame"));
  auto finish = reinterpret_cast<bool (*)(int, PresentDmabuf*)>(
      ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_end_frame"));
  if (begin == nullptr || finish == nullptr) {
    log::w(kTag, "PresentDriver.nOpen: GPU begin/end frame missing (libacl_graphics_jni?)");
    return 0;
  }
  auto* s = new PresentSession();
  env->GetJavaVM(&s->vm);
  s->w = w;
  s->h = h;
  const jint ms_exactly = 1 << 30;  // MeasureSpec.EXACTLY
  const jint ms_mask = 0x3 << 30;
  s->wspec = (w & ~ms_mask) | ms_exactly;
  s->hspec = (h & ~ms_mask) | ms_exactly;
  s->gpu_begin = begin;
  s->gpu_end = finish;
  s->deliver_vsync = reinterpret_cast<void (*)(JNIEnv*, jlong)>(::dlsym(RTLD_DEFAULT, "acl_deliver_vsync"));
  s->vsync_pending = reinterpret_cast<int (*)()>(::dlsym(RTLD_DEFAULT, "acl_vsync_pending"));
  s->vsync_set_immediate = reinterpret_cast<void (*)(int)>(::dlsym(RTLD_DEFAULT, "acl_vsync_set_immediate"));
  s->view = env->NewGlobalRef(view);

  // Resolve the View traversal methods + input dispatch entry points.
  if (jclass vc = env->GetObjectClass(view)) {
    s->m_measure = env->GetMethodID(vc, "measure", "(II)V");
    s->m_gmw = env->GetMethodID(vc, "getMeasuredWidth", "()I");
    s->m_gmh = env->GetMethodID(vc, "getMeasuredHeight", "()I");
    s->m_layout = env->GetMethodID(vc, "layout", "(IIII)V");
    s->m_draw = env->GetMethodID(vc, "draw", "(Landroid/graphics/Canvas;)V");
    s->dispatch_touch = env->GetMethodID(vc, "dispatchTouchEvent", "(Landroid/view/MotionEvent;)Z");
    s->dispatch_key = env->GetMethodID(vc, "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z");
    env->DeleteLocalRef(vc);
  }
  if (jclass mecls = env->FindClass("android/view/MotionEvent")) {
    s->me_obtain = env->GetStaticMethodID(mecls, "obtain", "(JJIFFI)Landroid/view/MotionEvent;");
    s->me_recycle = env->GetMethodID(mecls, "recycle", "()V");
    s->motionevent_cls = static_cast<jclass>(env->NewGlobalRef(mecls));
    env->DeleteLocalRef(mecls);
  } else { env->ExceptionClear(); }
  if (jclass kecls = env->FindClass("android/view/KeyEvent")) {
    s->ke_obtain = env->GetStaticMethodID(kecls, "obtain", "(II)Landroid/view/KeyEvent;");
    s->keyevent_cls = static_cast<jclass>(env->NewGlobalRef(kecls));
    env->DeleteLocalRef(kecls);
  } else { env->ExceptionClear(); }
  if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();

  auto to_utf8 = [&](jstring js) -> std::string {
    if (js == nullptr) return {};
    const char* c = env->GetStringUTFChars(js, nullptr);
    std::string out(c != nullptr ? c : "");
    if (c != nullptr) env->ReleaseStringUTFChars(js, c);
    return out;
  };
  const std::string t = to_utf8(title);
  const std::string a = to_utf8(appId);

  const wm::InputCallback input = [s](const wm::InputEvent& ev) { present_deliver_input(s, ev); };
  s->presenter = wm::presenter_open(w, h, t.empty() ? "acl" : t, a.empty() ? "org.acl.app" : a,
                                    input, /*want_ime=*/false);
  if (!s->presenter) {
    log::w(kTag, "PresentDriver.nOpen: presenter_open failed");
    if (s->view != nullptr) env->DeleteGlobalRef(s->view);
    if (s->motionevent_cls != nullptr) env->DeleteGlobalRef(s->motionevent_cls);
    if (s->keyevent_cls != nullptr) env->DeleteGlobalRef(s->keyevent_cls);
    delete s;
    return 0;
  }
  // Wrap the persistent GPU SkCanvas once (begin_frame clears it each tick, same handle).
  const jlong ch = s->gpu_begin(w, h);
  if (ch != 0) {
    if (jclass canvasC = env->FindClass("android/graphics/Canvas")) {
      if (jmethodID cctor = env->GetMethodID(canvasC, "<init>", "(J)V")) {
        jobject c = env->NewObject(canvasC, cctor, ch);
        if (c != nullptr) { s->canvas = env->NewGlobalRef(c); env->DeleteLocalRef(c); }
      }
      env->DeleteLocalRef(canvasC);
    }
  }
  if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
  log::i(kTag, "PresentDriver.nOpen: {}x{} presenter ready (looper-driven)", w, h);
  return reinterpret_cast<jlong>(s);
}

jint JNICALL PresentDriver_nFd(JNIEnv*, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  return (s != nullptr && s->presenter) ? s->presenter->fd() : -1;
}
jint JNICALL PresentDriver_nRepeatFd(JNIEnv*, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  return (s != nullptr && s->presenter) ? s->presenter->repeat_fd() : -1;
}
jboolean JNICALL PresentDriver_nDispatch(JNIEnv*, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s == nullptr || !s->presenter) return JNI_FALSE;
  return s->presenter->dispatch() ? JNI_TRUE : JNI_FALSE;  // runs input callbacks
}
void JNICALL PresentDriver_nDispatchRepeat(JNIEnv*, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s != nullptr && s->presenter) s->presenter->dispatch_repeat();
}
void JNICALL PresentDriver_nSetVsyncDelivered(JNIEnv*, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s != nullptr && s->vsync_set_immediate != nullptr) s->vsync_set_immediate(0);
}
void JNICALL PresentDriver_nMaybeDeliverVsync(JNIEnv* env, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s == nullptr || !s->presenter || s->deliver_vsync == nullptr) return;
  const bool rdy = s->presenter->ready();
  const bool pend = (s->vsync_pending == nullptr || s->vsync_pending() != 0);
  static int diag = 0;
  if (diag < 8) { log::i(kTag, "PresentDriver.vsync: ready={} pending={}", rdy, pend); ++diag; }
  if (rdy && pend) s->deliver_vsync(env, static_cast<jlong>(s->presenter->frame_time_ns()));
}
void JNICALL PresentDriver_nTraverse(JNIEnv* env, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s == nullptr || !s->presenter) return;
  static int tdiag = 0;
  if (tdiag < 4) { log::i(kTag, "PresentDriver.nTraverse: entered (frame {})", s->frames); ++tdiag; }
  if (s->gpu_begin(s->w, s->h) == 0) { log::w(kTag, "PresentDriver.nTraverse: gpu_begin=0"); return; }  // clear + re-arm
  const bool diag = tdiag <= 4;
  if (s->m_measure != nullptr && s->m_layout != nullptr) {
    env->CallVoidMethod(s->view, s->m_measure, s->wspec, s->hspec);
    const jint mw = env->CallIntMethod(s->view, s->m_gmw);
    const jint mh = env->CallIntMethod(s->view, s->m_gmh);
    env->CallVoidMethod(s->view, s->m_layout, 0, 0, mw, mh);
    if (diag) log::i(kTag, "PresentDriver.nTraverse: measured {}x{}", mw, mh);
  }
  if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionDescribe(); env->ExceptionClear(); return; }
  if (s->m_draw != nullptr && s->canvas != nullptr) env->CallVoidMethod(s->view, s->m_draw, s->canvas);
  if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionDescribe(); env->ExceptionClear(); return; }
  if (diag) log::i(kTag, "PresentDriver.nTraverse: drawn");
  const int bufidx = s->presenter->next_buffer();
  if (bufidx < 0) { if (diag) log::w(kTag, "PresentDriver.nTraverse: no free buffer"); return; }
  PresentDmabuf f{};
  if (!s->gpu_end(bufidx, &f) || f.fd < 0) { if (diag) log::w(kTag, "PresentDriver.nTraverse: gpu_end failed"); return; }
  gfx::DmabufFrame fr{};
  fr.fd = f.fd;
  fr.width = f.width;
  fr.height = f.height;
  fr.plane_count = f.plane_count;
  for (std::uint32_t i = 0; i < f.plane_count && i < 4; ++i) {
    fr.planes[i].offset = f.offsets[i];
    fr.planes[i].stride = f.strides[i];
  }
  fr.modifier = f.modifier;
  fr.drm_format = f.drm_format;
  fr.modifier_tiled = f.modifier != 0;
  if (s->presenter->submit(fr)) {
    if (s->frames == 0 || s->frames % 60 == 0)
      log::i(kTag, "PresentDriver: frame {} presented ({}x{}) — looper-driven, Compose composed",
             s->frames, f.width, f.height);
    ++s->frames;
  }
}
void JNICALL PresentDriver_nClose(JNIEnv* env, jclass, jlong sp) {
  auto* s = reinterpret_cast<PresentSession*>(sp);
  if (s == nullptr) return;
  s->presenter.reset();  // tears down the surface + disconnects
  if (s->view != nullptr) env->DeleteGlobalRef(s->view);
  if (s->canvas != nullptr) env->DeleteGlobalRef(s->canvas);
  if (s->motionevent_cls != nullptr) env->DeleteGlobalRef(s->motionevent_cls);
  if (s->keyevent_cls != nullptr) env->DeleteGlobalRef(s->keyevent_cls);
  delete s;
}

// Bind org.acl.internal.PresentDriver's native seam. RegisterNatives is atomic per class, so
// every signature must match; a miss leaves PresentDriver.run to fail fast via nOpen=0.
void register_present_driver_natives(JNIEnv* env) {
  jclass c = env->FindClass("org/acl/internal/PresentDriver");
  if (c == nullptr) { env->ExceptionClear(); return; }
  const JNINativeMethod m[] = {
      {const_cast<char*>("nOpen"),
       const_cast<char*>("(Landroid/view/View;IILjava/lang/String;Ljava/lang/String;)J"),
       reinterpret_cast<void*>(PresentDriver_nOpen)},
      {const_cast<char*>("nFd"), const_cast<char*>("(J)I"), reinterpret_cast<void*>(PresentDriver_nFd)},
      {const_cast<char*>("nRepeatFd"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(PresentDriver_nRepeatFd)},
      {const_cast<char*>("nDispatch"), const_cast<char*>("(J)Z"),
       reinterpret_cast<void*>(PresentDriver_nDispatch)},
      {const_cast<char*>("nDispatchRepeat"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(PresentDriver_nDispatchRepeat)},
      {const_cast<char*>("nSetVsyncDelivered"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(PresentDriver_nSetVsyncDelivered)},
      {const_cast<char*>("nMaybeDeliverVsync"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(PresentDriver_nMaybeDeliverVsync)},
      {const_cast<char*>("nTraverse"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(PresentDriver_nTraverse)},
      {const_cast<char*>("nClose"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(PresentDriver_nClose)},
  };
  if (env->RegisterNatives(c, m, sizeof(m) / sizeof(m[0])) != JNI_OK) {
    log::w(kTag, "PresentDriver: RegisterNatives failed");
    env->ExceptionClear();
  } else {
    log::i(kTag, "PresentDriver native seam registered (looper-driven present)");
  }
  env->DeleteLocalRef(c);
}
#endif  // ACL_HAVE_VULKAN && ACL_HAVE_WCS

#if ACL_PROFILE_AUTOMOTIVE
// The composition-root audio route for the media bridge: renders the now-playing
// track to acl::audio -> PipeWire. The demo MediaBrowserService publishes no PCM
// stream (real apps feed decoded PCM via AudioTrack, which already routes to
// acl::audio), so this plays a short representative tone — audible with
// ACL_AUDIO=pipewire, silent/deterministic with ACL_AUDIO=null.
class ToneAudioRoute : public hosts::media::AudioRoute {
 public:
  std::uint64_t play(const hosts::media::NowPlaying& now) override {
    audio::ToneSpec spec;
    spec.seconds = 0.4;
    spec.freq_hz = now.state == hosts::media::Playback::Playing ? 523 : 440;  // C5/A4
    return audio::play_tone(spec);
  }
};
#endif

std::string join(const std::vector<std::string>& parts, char sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) out.push_back(sep);
    out += parts[i];
  }
  return out;
}

// Convert a dotted or slashed class name to the slashed JNI form.
std::string to_jni_name(std::string_view name) {
  std::string out(name);
  for (char& c : out) {
    if (c == '.') c = '/';
  }
  return out;
}

// Base for the two JNI-Invocation-API backends. Both embed a Java runtime via
// JNI_CreateJavaVM and drive it through identical JNI (call_static_main, the
// Activity/media/CAL lifecycles, the present path). Subclasses differ ONLY in
// create_java_vm(): JvmVm links HotSpot's JNI_CreateJavaVM (ADR-0001 fallback);
// ArtVm dlopens a host ART build's libart.so and loads DEX natively (the ADR-0001
// target). Everything below create_java_vm() is runtime-agnostic and shared.
class JniVm : public Vm {
 public:
  JniVm(Backend backend, VmConfig config)
      : Vm(backend), config_(std::move(config)) {}

  ~JniVm() override { shutdown(); }

  Error start() override {
    if (jvm_ != nullptr) return Error::ok();
    JNIEnv* env = nullptr;
    if (Error e = create_java_vm(env)) return e;
    env_ = env;
    log::i(kTag, "{} started: boot=[{}] classpath=[{}]", backend_name(backend()),
           join(config_.boot_classpath, ':'), join(config_.classpath, ':'));
    // Bind the android.* native methods on the classpath.
    register_natives(env_);
    return Error::ok();
  }

 protected:
  // Create and configure the underlying VM, returning its main-thread JNIEnv and
  // setting jvm_. Implemented per backend.
  virtual Error create_java_vm(JNIEnv*& env) = 0;

  // Bind the android.* native methods. Default: acl's C++ framework natives (the
  // shim floor). ArtVm can instead register the real framework natives from a host
  // libandroid_runtime (ADR-0008) — the two are alternatives, not stacked (acl's
  // shims target the shim classes and would mis-bind against the real framework).
  virtual void register_natives(JNIEnv* env) { register_framework_natives(env); }

 public:

  Error call_static_main(std::string_view main_class,
                         const std::vector<std::string>& args) override {
    if (env_ == nullptr) {
      return Error::of(Status::NotStarted, "call_static_main before start()");
    }

    const std::string jni_name = to_jni_name(main_class);
    jclass cls = env_->FindClass(jni_name.c_str());
    if (cls == nullptr) {
      clear_exception();
      return Error::of(Status::NotFound, "class not found: " + std::string(main_class));
    }
    jmethodID mid =
        env_->GetStaticMethodID(cls, "main", "([Ljava/lang/String;)V");
    if (mid == nullptr) {
      clear_exception();
      env_->DeleteLocalRef(cls);
      return Error::of(Status::NotFound,
                       "no static void main(String[]) in " + std::string(main_class));
    }

    jobjectArray jargs = make_string_array(args);
    if (jargs == nullptr) {
      env_->DeleteLocalRef(cls);
      return Error::of(Status::RuntimeError, "failed to build String[] args");
    }

    env_->CallStaticVoidMethod(cls, mid, jargs);
    Error result = Error::ok();
    if (env_->ExceptionCheck() == JNI_TRUE) {
      env_->ExceptionDescribe();  // prints the stack trace to stderr
      env_->ExceptionClear();
      result = Error::of(Status::RuntimeError,
                         "uncaught exception in " + std::string(main_class) + ".main");
    }
    env_->DeleteLocalRef(jargs);
    env_->DeleteLocalRef(cls);
    return result;
  }

  Error call_activity_lifecycle(std::string_view activity_class) override {
    if (env_ == nullptr) {
      return Error::of(Status::NotStarted, "call_activity_lifecycle before start()");
    }
    const std::string name = to_jni_name(activity_class);
    jclass cls = env_->FindClass(name.c_str());
    if (cls == nullptr) return class_load_error("class", activity_class);
    // Only drive the lifecycle for real Activity subclasses; otherwise the caller
    // falls back to a static main() (self-contained apps).
    jclass activity_cls = env_->FindClass("android/app/Activity");
    if (activity_cls == nullptr || env_->IsAssignableFrom(cls, activity_cls) != JNI_TRUE) {
      clear_exception();
      env_->DeleteLocalRef(cls);
      if (activity_cls != nullptr) env_->DeleteLocalRef(activity_cls);
      return Error::of(Status::NotApplicable,
                       std::string(activity_class) + " is not an android.app.Activity");
    }

    // Seed android.view.Display from the compositor's wl_output before onCreate, so
    // the app's early getWindowManager()/getDefaultDisplay() metrics are real. A
    // throwaway connection; a no-op when not presenting or no compositor is reachable.
    if (config_.present) {
      int dw = 0, dh = 0, dscale = 1;
      if (wm::query_display(dw, dh, dscale)) {
        svc::window().set_display(dw, dh, dscale);
        log::i(kTag, "display from wl_output: {}x{} scale {}", dw, dh, dscale);
      }
    }

    Error result = Error::ok();
    jmethodID ctor = env_->GetMethodID(cls, "<init>", "()V");
    jobject activity = ctor != nullptr ? env_->NewObject(cls, ctor) : nullptr;
    if (activity == nullptr) {
      result = fail_with_exception("instantiate " + std::string(activity_class));
    } else {
      log::i(kTag, "launching Activity {}", activity_class);
      attach_app_context(activity);  // host ART: give the Activity a real app Context
      log_context(activity);
      // onCreate(null) -> onStart() -> onResume(), the documented launch order.
      result = call_lifecycle(cls, activity, "onCreate", "(Landroid/os/Bundle;)V", true);
      if (result.is_ok()) probe_managers(activity);
      if (result.is_ok())
        result = call_lifecycle(cls, activity, "onStart", "()V", false);
      if (result.is_ok())
        result = call_lifecycle(cls, activity, "onResume", "()V", false);
      if (result.is_ok()) render_content_view(activity);
      env_->DeleteLocalRef(activity);
    }
    env_->DeleteLocalRef(activity_cls);
    env_->DeleteLocalRef(cls);
    return result;
  }

  // Host ART: give the Activity a real app Context (its package + resources.arsc) and attach
  // it (mBase + mActivityInfo + PhoneWindow), via acl's runtime bootstrap. No-op on the shim
  // path (HostBootstrap absent) or if the app class/resources aren't available.
  void attach_app_context(jobject activity) {
    jclass boot = env_->FindClass("org/acl/internal/HostBootstrap");
    if (boot == nullptr) { clear_exception(); return; }
    const std::string apk = config_.classpath.empty() ? std::string() : config_.classpath.front();
    const char* fwres = std::getenv("ACL_ART_FRAMEWORK_RES");
    jstring j_apk = env_->NewStringUTF(apk.c_str());
    jstring j_fw = env_->NewStringUTF(fwres != nullptr ? fwres : "");
    jstring j_pkg = env_->NewStringUTF(config_.package_name.c_str());

    // The app's manifest theme, applied to the app Context (so decor/layout inflation
    // resolves framework + app theme attributes). Set before any Context is built.
    if (jmethodID set_theme = env_->GetStaticMethodID(boot, "setAppTheme", "(I)V")) {
      env_->CallStaticVoidMethod(boot, set_theme,
                                 static_cast<jint>(config_.application_theme));
    }
    clear_exception();

    // The app's Application, created once (its onCreate sets the app's singleton).
    if (app_application_ == nullptr) {
      jstring j_app = env_->NewStringUTF(config_.application_name.c_str());
      jobject app = env_->CallStaticObjectMethod(
          boot,
          env_->GetStaticMethodID(boot, "createApplication",
                                  "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                                  "Ljava/lang/String;)Landroid/app/Application;"),
          j_app, j_apk, j_fw, j_pkg);
      if (env_->ExceptionCheck() == JNI_TRUE) {
        env_->ExceptionDescribe();
        env_->ExceptionClear();
        log::w(kTag, "app Application {} onCreate hit a wall (continuing without singleton)",
               config_.application_name);
      } else if (app != nullptr) {
        app_application_ = env_->NewGlobalRef(app);
        log::i(kTag, "app Application ready: {}",
               config_.application_name.empty() ? "(default)" : config_.application_name);
      }
      if (app != nullptr) env_->DeleteLocalRef(app);
      env_->DeleteLocalRef(j_app);
    }

    jobject ctx = env_->CallStaticObjectMethod(
        boot,
        env_->GetStaticMethodID(
            boot, "newAppContext",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Landroid/content/Context;"),
        j_apk, j_fw, j_pkg);
    if (ctx != nullptr && env_->ExceptionCheck() != JNI_TRUE) {
      env_->CallStaticVoidMethod(
          boot,
          env_->GetStaticMethodID(
              boot, "attachActivityContext",
              "(Landroid/app/Activity;Landroid/content/Context;Landroid/app/Application;)V"),
          activity, ctx, app_application_);
    }
    if (env_->ExceptionCheck() == JNI_TRUE) {
      env_->ExceptionDescribe();
      env_->ExceptionClear();
      log::w(kTag, "attach app context: failed (continuing)");
    } else {
      log::i(kTag, "attach app context: package={} resources bound", config_.package_name);
    }
    env_->DeleteLocalRef(j_apk);
    env_->DeleteLocalRef(j_fw);
    env_->DeleteLocalRef(j_pkg);
    if (ctx != nullptr) env_->DeleteLocalRef(ctx);
    env_->DeleteLocalRef(boot);
  }
  jobject app_application_ = nullptr;  // the app's Application, created once (host ART)

  // The media-browse API family a hosted service uses: the framework
  // android.service.media.MediaBrowserService, or the support-library
  // androidx.media.MediaBrowserServiceCompat (what most real Auto apps extend). The
  // methods are identical bar the package of the nested BrowserRoot/Result/Description.
  struct MbsApi {
    const char* label;
    bool framework;
    std::string browser_root;  // internal name of the BrowserRoot nested class
    std::string result_cls;    // internal name of the Result nested class
    std::string media_desc;    // internal name of MediaDescription(Compat)
  };
  bool media_browser_api(jclass cls, MbsApi& api) {
    struct Cand { const char* base; const char* label; bool fw;
                  const char* root; const char* res; const char* desc; };
    static const Cand cands[] = {
        {"android/service/media/MediaBrowserService", "MediaBrowserService", true,
         "android/service/media/MediaBrowserService$BrowserRoot",
         "android/service/media/MediaBrowserService$Result", "android/media/MediaDescription"},
        {"androidx/media/MediaBrowserServiceCompat", "MediaBrowserServiceCompat", false,
         "androidx/media/MediaBrowserServiceCompat$BrowserRoot",
         "androidx/media/MediaBrowserServiceCompat$Result", "androidx/media/MediaDescriptionCompat"},
    };
    for (const Cand& c : cands) {
      jclass base = env_->FindClass(c.base);
      if (base == nullptr) { clear_exception(); continue; }
      const bool ok = env_->IsAssignableFrom(cls, base) == JNI_TRUE;
      env_->DeleteLocalRef(base);
      if (ok) { api = {c.label, c.fw, c.root, c.res, c.desc}; return true; }
    }
    return false;
  }

  Error call_media_browser(std::string_view service_class) override {
    if (env_ == nullptr) {
      return Error::of(Status::NotStarted, "call_media_browser before start()");
    }
    const std::string name = to_jni_name(service_class);
    jclass cls = env_->FindClass(name.c_str());
    if (cls == nullptr) return class_load_error("MediaBrowserService", service_class);
    MbsApi api;
    if (!media_browser_api(cls, api)) {
      clear_exception();
      env_->DeleteLocalRef(cls);
      return Error::of(Status::NotApplicable,
                       std::string(service_class) + " is not a MediaBrowserService(Compat)");
    }

    Error result = Error::ok();
    jmethodID ctor = env_->GetMethodID(cls, "<init>", "()V");
    jobject svc = ctor != nullptr ? env_->NewObject(cls, ctor) : nullptr;
    if (svc == nullptr) {
      result = fail_with_exception("instantiate " + std::string(service_class));
    } else {
      log::i(kTag, "hosting {} {}", api.label, service_class);
      log_context(svc);
#if ACL_PROFILE_AUTOMOTIVE
      hosts::media::MediaBridge::instance().set_audio_route(&audio_route_);
#endif
      result = call_lifecycle(cls, svc, "onCreate", "()V", false);
      if (result.is_ok()) probe_managers(svc);  // the app's MediaSession now shows up
      // Session read uses the framework MediaSession; androidx uses MediaSessionCompat
      // (a separate follow-on) — skip it there.
      if (result.is_ok() && api.framework) result = read_session(cls, svc);
      if (result.is_ok()) result = browse_root(cls, svc, api);
      env_->DeleteLocalRef(svc);
    }
    env_->DeleteLocalRef(cls);
    return result;
  }

  Error call_car_app(std::string_view service_class) override {
#if !ACL_PROFILE_AUTOMOTIVE
    (void)service_class;
    return Error::of(Status::NotApplicable, "CAL host is automotive-profile only");
#else
    if (env_ == nullptr) {
      return Error::of(Status::NotStarted, "call_car_app before start()");
    }
    const std::string name = to_jni_name(service_class);
    jclass cls = env_->FindClass(name.c_str());
    if (cls == nullptr) return class_load_error("CarAppService", service_class);
    jclass cas_cls = env_->FindClass("androidx/car/app/CarAppService");
    if (cas_cls == nullptr || env_->IsAssignableFrom(cls, cas_cls) != JNI_TRUE) {
      clear_exception();
      env_->DeleteLocalRef(cls);
      if (cas_cls != nullptr) env_->DeleteLocalRef(cas_cls);
      return Error::of(Status::NotApplicable,
                       std::string(service_class) + " is not a CarAppService");
    }
    env_->DeleteLocalRef(cas_cls);

    Error result = Error::ok();
    jmethodID ctor = env_->GetMethodID(cls, "<init>", "()V");
    jobject svc = ctor != nullptr ? env_->NewObject(cls, ctor) : nullptr;
    if (svc == nullptr) {
      result = fail_with_exception("instantiate " + std::string(service_class));
    } else {
      log::i(kTag, "hosting CarAppService {}", service_class);
      result = render_car_app(cls, svc);
      env_->DeleteLocalRef(svc);
    }
    env_->DeleteLocalRef(cls);
    return result;
#endif
  }

  Error shutdown() override {
    if (jvm_ != nullptr) {
      jvm_->DestroyJavaVM();  // blocks until non-daemon threads finish
      jvm_ = nullptr;
      env_ = nullptr;
      log::i(kTag, "{} shutdown", backend_name(backend()));
    }
    return Error::ok();
  }

 private:
  void clear_exception() {
    if (env_ != nullptr && env_->ExceptionCheck() == JNI_TRUE) {
      env_->ExceptionClear();
    }
  }

  // If an exception is pending, return its toString() (e.g. "java.lang.
  // NoClassDefFoundError: androidx/media/MediaBrowserServiceCompat") and clear it;
  // else "". Lets a FindClass failure distinguish a genuinely-absent class from one
  // that is present but cannot be linked (a missing dependency in its hierarchy).
  std::string pending_exception_text() {
    if (env_ == nullptr || env_->ExceptionCheck() != JNI_TRUE) return {};
    jthrowable ex = env_->ExceptionOccurred();
    env_->ExceptionClear();
    if (ex == nullptr) return {};
    std::string out;
    jclass ex_cls = env_->GetObjectClass(ex);
    jmethodID to_string = env_->GetMethodID(ex_cls, "toString", "()Ljava/lang/String;");
    if (to_string != nullptr) {
      auto s = static_cast<jstring>(env_->CallObjectMethod(ex, to_string));
      if (env_->ExceptionCheck() == JNI_TRUE) env_->ExceptionClear();  // toString threw
      if (s != nullptr) { out = utf8(s); env_->DeleteLocalRef(s); }
    }
    env_->DeleteLocalRef(ex_cls);
    env_->DeleteLocalRef(ex);
    return out;
  }

  // Report a failed entry-class lookup: a link error (present-but-unloadable) is a
  // RuntimeError with the underlying exception; a truly absent class is NotFound.
  Error class_load_error(std::string_view kind, std::string_view name) {
    const std::string exc = pending_exception_text();
    if (!exc.empty())
      return Error::of(Status::RuntimeError,
                       "could not load " + std::string(kind) + " " + std::string(name) + ": " + exc);
    return Error::of(Status::NotFound, "class not found: " + std::string(name));
  }

  // If `obj` is an android.content.Context (Service/Activity now are), inject the
  // manifest package and log getPackageName() — proof the entry point functions as
  // a Context carrying the app's real identity (a stand-in for attachBaseContext of
  // a ContextImpl).
  void log_context(jobject obj) {
    jclass ctx_cls = env_->FindClass("android/content/Context");
    if (ctx_cls == nullptr) { clear_exception(); return; }
    if (env_->IsInstanceOf(obj, ctx_cls) == JNI_TRUE) {
      jclass obj_cls = env_->GetObjectClass(obj);
      if (!config_.package_name.empty()) {
        jmethodID setter =
            env_->GetMethodID(obj_cls, "aclSetPackage", "(Ljava/lang/String;)V");
        if (setter != nullptr) {
          jstring pkg = env_->NewStringUTF(config_.package_name.c_str());
          env_->CallVoidMethod(obj, setter, pkg);
          env_->DeleteLocalRef(pkg);
        } else {
          clear_exception();
        }
      }
      jmethodID gpn = env_->GetMethodID(obj_cls, "getPackageName", "()Ljava/lang/String;");
      if (gpn != nullptr) {
        auto p = static_cast<jstring>(env_->CallObjectMethod(obj, gpn));
        log::i(kTag, "entry point is a Context: getPackageName()={}",
               p != nullptr ? utf8(p) : "?");
        if (p != nullptr) env_->DeleteLocalRef(p);
      }
      env_->DeleteLocalRef(obj_cls);
    }
    env_->DeleteLocalRef(ctx_cls);
  }

  // Exercise the svc/-backed Managers via getSystemService on the app's Context —
  // the framework itself calls these. Proves ActivityManager / WindowManager+Display
  // / MediaSessionManager are wired; the MediaSession the app created shows up.
  void probe_managers(jobject ctx) {
    jclass ctx_cls = env_->GetObjectClass(ctx);
    jmethodID gss = env_->GetMethodID(ctx_cls, "getSystemService",
                                      "(Ljava/lang/String;)Ljava/lang/Object;");
    env_->DeleteLocalRef(ctx_cls);
    if (gss == nullptr) { clear_exception(); return; }

    const auto svc = [&](const char* name) -> jobject {
      jstring s = env_->NewStringUTF(name);
      jobject o = env_->CallObjectMethod(ctx, gss, s);
      env_->DeleteLocalRef(s);
      if (env_->ExceptionCheck() == JNI_TRUE) env_->ExceptionClear();
      return o;
    };
    const auto call_int = [&](jobject o, const char* method) -> int {
      jclass c = env_->GetObjectClass(o);
      jmethodID m = env_->GetMethodID(c, method, "()I");
      const int v = m != nullptr ? env_->CallIntMethod(o, m) : -1;
      if (m == nullptr) clear_exception();
      env_->DeleteLocalRef(c);
      return v;
    };

    if (jobject am = svc("activity")) {
      log::i(kTag, "ActivityManager: memoryClass={} MB", call_int(am, "getMemoryClass"));
      env_->DeleteLocalRef(am);
    }
    if (jobject wm = svc("window")) {
      jclass c = env_->GetObjectClass(wm);
      jmethodID gdd = env_->GetMethodID(c, "getDefaultDisplay", "()Landroid/view/Display;");
      jobject d = gdd != nullptr ? env_->CallObjectMethod(wm, gdd) : nullptr;
      if (d != nullptr) {
        log::i(kTag, "WindowManager: display={}x{}", call_int(d, "getWidth"),
               call_int(d, "getHeight"));
        env_->DeleteLocalRef(d);
      } else { clear_exception(); }
      env_->DeleteLocalRef(c);
      env_->DeleteLocalRef(wm);
    }
    if (jobject msm = svc("media_session")) {
      log::i(kTag, "MediaSessionManager: activeSessions={}",
             call_int(msm, "aclActiveSessionCount"));
      env_->DeleteLocalRef(msm);
    }
  }

  // Phase-2 ViewRoot traversal. If the Activity set a content View (setContentView),
  // run the measure -> layout -> draw pass ViewRootImpl.performTraversals drives —
  // against the WindowManager display metrics, into a Bitmap-backed Canvas — and
  // describe the resulting pixels. Proves View(Context) + a real measure/layout/draw
  // traversal over the app's view tree renders verifiable output (no compositor).
  void render_content_view(jobject activity) {
    jclass acls = env_->GetObjectClass(activity);
    jobject view = nullptr;
    // Prefer acl's helper (view-host demo); real Activities lack it — fall back to the
    // standard decor view: activity.getWindow().getDecorView().
    if (jmethodID gcv = env_->GetMethodID(acls, "aclContentView", "()Landroid/view/View;");
        gcv != nullptr) {
      view = env_->CallObjectMethod(activity, gcv);
    }
    clear_exception();
    if (view == nullptr) {
      if (jmethodID gw = env_->GetMethodID(acls, "getWindow", "()Landroid/view/Window;");
          gw != nullptr) {
        if (jobject window = env_->CallObjectMethod(activity, gw); window != nullptr) {
          jclass wcls = env_->GetObjectClass(window);
          if (jmethodID gdv =
                  env_->GetMethodID(wcls, "getDecorView", "()Landroid/view/View;");
              gdv != nullptr) {
            view = env_->CallObjectMethod(window, gdv);
          }
          env_->DeleteLocalRef(wcls);
          env_->DeleteLocalRef(window);
        }
      }
      clear_exception();
    }
    env_->DeleteLocalRef(acls);
    if (view == nullptr) { clear_exception(); return; }  // no content view set

    const int w = svc::window().display_width();
    const int h = svc::window().display_height();
    // MeasureSpec.makeMeasureSpec(size, EXACTLY): size in the low 30 bits, mode 1.
    const jint mask = 0x3 << 30;
    const jint wspec = (w & ~mask) | (1 << 30);
    const jint hspec = (h & ~mask) | (1 << 30);

    jclass vcls = env_->GetObjectClass(view);
    jmethodID measure = env_->GetMethodID(vcls, "measure", "(II)V");
    jmethodID gmw = env_->GetMethodID(vcls, "getMeasuredWidth", "()I");
    jmethodID gmh = env_->GetMethodID(vcls, "getMeasuredHeight", "()I");
    jmethodID layout = env_->GetMethodID(vcls, "layout", "(IIII)V");
    jmethodID draw = env_->GetMethodID(vcls, "draw", "(Landroid/graphics/Canvas;)V");
    env_->DeleteLocalRef(vcls);
    if (measure == nullptr || gmw == nullptr || gmh == nullptr || layout == nullptr ||
        draw == nullptr) {
      clear_exception();
      env_->DeleteLocalRef(view);
      return;
    }

    // Attach the tree to a window (real Android's ViewRootImpl.setView) before traversal —
    // dispatchAttachedToWindow across the tree, needed by views like Jetpack ComposeView.
    if (jclass boot = env_->FindClass("org/acl/internal/HostBootstrap"); boot != nullptr) {
      if (jmethodID att =
              env_->GetStaticMethodID(boot, "attachDecorToWindow", "(Landroid/view/View;)V"))
        env_->CallStaticVoidMethod(boot, att, view);
      if (env_->ExceptionCheck() == JNI_TRUE) env_->ExceptionClear();
      env_->DeleteLocalRef(boot);
    }

    // GPU present: drive the Choreographer-pumped present loop directly — its per-frame
    // doFrame pumps the looper so async content composes (Jetpack Compose composes on the
    // frame clock). The loop measures/lays out/draws each frame, so do NOT measure/layout
    // here first: the ComposeView's measure/layout blocks on a composition that only runs
    // once the loop's doFrame pumps it (chicken-and-egg).
    if (config_.present) {
      if (const char* gpu = std::getenv("ACL_PRESENT_GPU"); gpu != nullptr && *gpu != '\0') {
        // Default: the Looper-driven present (approach A). ACL_PRESENT_LEGACY selects the old
        // native poll loop (kept for comparison; it starves the Android Looper so Compose hangs).
        const char* legacy = std::getenv("ACL_PRESENT_LEGACY");
        if (legacy != nullptr && *legacy != '\0') present_content_loop_gpu(view, w, h);
        else present_content_looper(view, w, h);
        env_->DeleteLocalRef(view);
        return;
      }
    }

    // Headless / non-GPU path: one-shot measure → layout → draw into a Bitmap.
    env_->CallVoidMethod(view, measure, wspec, hspec);
    const int mw = env_->CallIntMethod(view, gmw);
    const int mh = env_->CallIntMethod(view, gmh);
    log::i(kTag, "ViewRoot: performTraversals measured={}x{}, laying out {}x{}", mw, mh, w, h);
    env_->CallVoidMethod(view, layout, 0, 0, mw, mh);

    // Bitmap b = Bitmap.createBitmap(w, h); Canvas c = new Canvas(b); view.draw(c);
    jclass bmp_cls = env_->FindClass("android/graphics/Bitmap");
    jclass canvas_cls = env_->FindClass("android/graphics/Canvas");
    jobject bmp = nullptr;
    if (bmp_cls != nullptr && canvas_cls != nullptr) {
      jmethodID create = env_->GetStaticMethodID(bmp_cls, "createBitmap",
                                                 "(II)Landroid/graphics/Bitmap;");
      jmethodID cctor = env_->GetMethodID(canvas_cls, "<init>",
                                          "(Landroid/graphics/Bitmap;)V");
      if (create != nullptr && cctor != nullptr) {
        bmp = env_->CallStaticObjectMethod(bmp_cls, create, w, h);
        jobject canvas = bmp != nullptr ? env_->NewObject(canvas_cls, cctor, bmp) : nullptr;
        if (canvas != nullptr) {
          env_->CallVoidMethod(view, draw, canvas);
          jmethodID describe = env_->GetMethodID(bmp_cls, "aclDescribe", "()V");
          if (describe != nullptr) env_->CallVoidMethod(bmp, describe);
          // Present the content view in a Wayland window (--present), driving a
          // Choreographer loop: each vsync tick fires the app's frame callbacks and
          // re-draws. No-op if no compositor / built without wcs. ACL_PRESENT_GPU routes
          // to the GPU-native dma-buf loop (View.draw on Ganesh Vulkan); default is the
          // mature shm loop (which also carries input/IME).
          if (config_.present) {
            const char* gpu = std::getenv("ACL_PRESENT_GPU");
            if (gpu != nullptr && *gpu != '\0') present_content_loop_gpu(view, w, h);
            else present_content_loop(view, bmp, canvas, w, h);
          }
          env_->DeleteLocalRef(canvas);
        }
      }
    }
    if (env_->ExceptionCheck() == JNI_TRUE) fail_with_exception("render content view");
    if (bmp != nullptr) env_->DeleteLocalRef(bmp);
    if (bmp_cls != nullptr) env_->DeleteLocalRef(bmp_cls);
    if (canvas_cls != nullptr) env_->DeleteLocalRef(canvas_cls);
    env_->DeleteLocalRef(view);
  }

  // Present the content view in a Wayland window via wm/ (wcs), driven by a
  // Choreographer-style loop: on each vsync tick the ViewRoot fires the app's
  // frame callbacks (android.view.Choreographer.aclDoFrame), re-runs the View
  // draw, and blits into the presenter's frame — the ViewRootImpl.performTraversals
  // + Choreographer path. A static app (no frame callbacks) shows one frame and
  // stops; an animating one runs to the frame budget (--present-frames) or close.
  // The Bitmap's native pointer IS the gfx::Surface the View draws into.
  void present_content_loop(jobject view, jobject bmp, jobject canvas, int w, int h) {
    if (!wm::wayland_available()) {
      log::i(kTag, "--present: no Wayland compositor reachable; frame not shown");
      return;
    }
    jclass bmp_cls = env_->GetObjectClass(bmp);
    jmethodID ptr_m = env_->GetMethodID(bmp_cls, "aclPtr", "()J");
    env_->DeleteLocalRef(bmp_cls);
    if (ptr_m == nullptr) { clear_exception(); return; }
    auto* surface = reinterpret_cast<const gfx::Surface*>(env_->CallLongMethod(bmp, ptr_m));
    if (surface == nullptr) return;

    jclass vcls = env_->GetObjectClass(view);
    jmethodID draw = env_->GetMethodID(vcls, "draw", "(Landroid/graphics/Canvas;)V");
    env_->DeleteLocalRef(vcls);

    // Choreographer.getInstance() + the per-frame pump. Optional: if the shim isn't
    // on the classpath we present a single static frame.
    jobject choreo = nullptr;
    jmethodID do_frame = nullptr;
    if (jclass ch_cls = env_->FindClass("android/view/Choreographer")) {
      if (jmethodID gi = env_->GetStaticMethodID(ch_cls, "getInstance",
                                                 "()Landroid/view/Choreographer;"))
        choreo = env_->CallStaticObjectMethod(ch_cls, gi);
      do_frame = env_->GetMethodID(ch_cls, "aclDoFrame", "(J)J");
      env_->DeleteLocalRef(ch_cls);
    }
    // android.view.MotionEvent.obtain + the view's dispatchTouchEvent, for pointer
    // input. Optional: absent shims => input is simply not delivered.
    jobject motionevent_cls = nullptr;
    jmethodID me_obtain = nullptr, me_recycle = nullptr, dispatch_touch = nullptr;
    if (jclass mecls = env_->FindClass("android/view/MotionEvent")) {
      me_obtain = env_->GetStaticMethodID(mecls, "obtain", "(JJIFFI)Landroid/view/MotionEvent;");
      me_recycle = env_->GetMethodID(mecls, "recycle", "()V");
      motionevent_cls = env_->NewGlobalRef(mecls);
      env_->DeleteLocalRef(mecls);
    }
    // android.view.KeyEvent.obtain + the view's dispatchKeyEvent, for key input.
    jobject keyevent_cls = nullptr;
    jmethodID ke_obtain = nullptr, dispatch_key = nullptr;
    if (jclass kecls = env_->FindClass("android/view/KeyEvent")) {
      ke_obtain = env_->GetStaticMethodID(kecls, "obtain", "(II)Landroid/view/KeyEvent;");
      keyevent_cls = env_->NewGlobalRef(kecls);
      env_->DeleteLocalRef(kecls);
    }
    if (jclass vc = env_->GetObjectClass(view)) {
      dispatch_touch = env_->GetMethodID(vc, "dispatchTouchEvent", "(Landroid/view/MotionEvent;)Z");
      dispatch_key = env_->GetMethodID(vc, "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z");
      env_->DeleteLocalRef(vc);
    }
    clear_exception();

    // IME: if the focused view produces an InputConnection, enable Wayland text
    // input; committed strings from the compositor's IME are delivered to it via
    // InputConnection.commitText.
    jobject input_conn = nullptr;
    jmethodID commit_text = nullptr, set_composing = nullptr, delete_surrounding = nullptr;
    bool want_ime = false;
    {
      jclass vc = env_->GetObjectClass(view);
      jmethodID create_ic = vc != nullptr ? env_->GetMethodID(
          vc, "onCreateInputConnection",
          "(Landroid/view/inputmethod/EditorInfo;)Landroid/view/inputmethod/InputConnection;") : nullptr;
      jclass ei_cls = env_->FindClass("android/view/inputmethod/EditorInfo");
      if (create_ic != nullptr && ei_cls != nullptr) {
        jmethodID ei_ctor = env_->GetMethodID(ei_cls, "<init>", "()V");
        jobject ei = ei_ctor != nullptr ? env_->NewObject(ei_cls, ei_ctor) : nullptr;
        jobject ic = ei != nullptr ? env_->CallObjectMethod(view, create_ic, ei) : nullptr;
        if (ic != nullptr) {
          input_conn = env_->NewGlobalRef(ic);
          if (jclass ic_cls = env_->GetObjectClass(ic)) {
            commit_text = env_->GetMethodID(ic_cls, "commitText", "(Ljava/lang/CharSequence;I)Z");
            set_composing = env_->GetMethodID(ic_cls, "setComposingText", "(Ljava/lang/CharSequence;I)Z");
            delete_surrounding = env_->GetMethodID(ic_cls, "deleteSurroundingText", "(II)Z");
            env_->DeleteLocalRef(ic_cls);
          }
          want_ime = commit_text != nullptr;
          env_->DeleteLocalRef(ic);
        }
        if (ei != nullptr) env_->DeleteLocalRef(ei);
      }
      if (vc != nullptr) env_->DeleteLocalRef(vc);
      if (ei_cls != nullptr) env_->DeleteLocalRef(ei_cls);
      clear_exception();
    }

    const std::string& pkg = config_.package_name;
    const auto render = [&](gfx::Surface& out, std::uint64_t frame_time_ns,
                            std::uint64_t /*idx*/) -> bool {
      if (choreo != nullptr && do_frame != nullptr)
        env_->CallLongMethod(choreo, do_frame, static_cast<jlong>(frame_time_ns));
      if (env_->ExceptionCheck() == JNI_TRUE) { fail_with_exception("Choreographer.doFrame"); return false; }
      if (draw != nullptr) env_->CallVoidMethod(view, draw, canvas);  // re-draw with updated state
      if (env_->ExceptionCheck() == JNI_TRUE) { fail_with_exception("View.draw"); return false; }
      out.blit(surface->data());
      return true;  // present until the frame budget / window close
    };
    // Deliver a pointer as a MotionEvent and a key as a KeyEvent into the content view.
    std::uint64_t down_time = 0;
    const auto on_pointer = [&, this](const wm::PointerInput& ev) {
      if (motionevent_cls == nullptr || me_obtain == nullptr || dispatch_touch == nullptr) return;
      const jint action = ev.action == wm::PointerInput::Action::Down ? 0
                        : ev.action == wm::PointerInput::Action::Up   ? 1 : 2;  // DOWN/UP/MOVE
      if (action == 0) down_time = frame_index_now();
      const jlong t = static_cast<jlong>(frame_index_now());
      jobject me = env_->CallStaticObjectMethod(
          static_cast<jclass>(motionevent_cls), me_obtain, static_cast<jlong>(down_time), t,
          action, static_cast<jfloat>(ev.x), static_cast<jfloat>(ev.y), 0);
      if (me != nullptr) {
        env_->CallBooleanMethod(view, dispatch_touch, me);
        if (me_recycle != nullptr) env_->CallVoidMethod(me, me_recycle);
        env_->DeleteLocalRef(me);
      }
      if (env_->ExceptionCheck() == JNI_TRUE) clear_exception();
    };
    const auto on_key = [&, this](const wm::KeyInput& ev) {
      if (keyevent_cls == nullptr || ke_obtain == nullptr || dispatch_key == nullptr) return;
      const jint action = ev.action == wm::KeyInput::Action::Down ? 0 : 1;  // ACTION_DOWN/UP
      jobject ke = env_->CallStaticObjectMethod(static_cast<jclass>(keyevent_cls), ke_obtain,
                                                action, android_keycode(ev.keycode));
      if (ke != nullptr) {
        env_->CallBooleanMethod(view, dispatch_key, ke);
        env_->DeleteLocalRef(ke);
      }
      if (env_->ExceptionCheck() == JNI_TRUE) clear_exception();
    };
    // Route IME text through the app's InputConnection: preedit -> setComposingText
    // (composing), a final commit -> commitText.
    const auto on_text = [&, this](const wm::TextInput& ev) {
      const jmethodID m = ev.preedit ? set_composing : commit_text;
      log::i(kTag, "IME {}: \"{}\"", ev.preedit ? "preedit" : "committed", ev.text);
      if (input_conn == nullptr || m == nullptr) return;
      jstring s = env_->NewStringUTF(ev.text.c_str());
      env_->CallBooleanMethod(input_conn, m, s, ev.cursor);
      if (s != nullptr) env_->DeleteLocalRef(s);
      if (env_->ExceptionCheck() == JNI_TRUE) clear_exception();
    };
    const auto on_delete = [&, this](const wm::DeleteSurroundingText& ev) {
      log::i(kTag, "IME delete-surrounding: before={} after={}", ev.before, ev.after);
      if (input_conn == nullptr || delete_surrounding == nullptr) return;
      env_->CallBooleanMethod(input_conn, delete_surrounding,
                              static_cast<jint>(ev.before), static_cast<jint>(ev.after));
      if (env_->ExceptionCheck() == JNI_TRUE) clear_exception();
    };
    const wm::InputCallback input = [&](const wm::InputEvent& ev) {
      if (const auto* p = std::get_if<wm::PointerInput>(&ev)) on_pointer(*p);
      else if (const auto* k = std::get_if<wm::KeyInput>(&ev)) on_key(*k);
      else if (const auto* t = std::get_if<wm::TextInput>(&ev)) on_text(*t);
      else if (const auto* d = std::get_if<wm::DeleteSurroundingText>(&ev)) on_delete(*d);
    };
    wm::present_loop(w, h, pkg.empty() ? "acl" : pkg,
                     pkg.empty() ? "org.acl.app" : pkg, render, input, want_ime,
                     config_.present_frames);
    if (choreo != nullptr) env_->DeleteLocalRef(choreo);
    if (motionevent_cls != nullptr) env_->DeleteGlobalRef(motionevent_cls);
    if (keyevent_cls != nullptr) env_->DeleteGlobalRef(keyevent_cls);
    if (input_conn != nullptr) env_->DeleteGlobalRef(input_conn);
  }

  // S4b step 2: present the content view GPU-native. Each vsync tick, the app's
  // View.draw runs on a real android.graphics.Canvas backed by the Ganesh Vulkan
  // surface (begin_frame → Choreographer.doFrame → View.draw → end_frame), and the
  // resulting modifier-tiled dma-buf is presented via zwp_linux_dmabuf_v1 — no CPU
  // upload. Pointer/key input is delivered to the View (via present_dmabuf_loop's
  // wl_seat wiring); IME on this path is a follow-up. Gated by ACL_PRESENT_GPU so the
  // mature interactive shm loop stays the default.
 protected:  // ArtVm's view-host demo drives this directly
  // Looper-driven present (approach A, default): hand the decor View to Java's PresentDriver,
  // which runs Looper.loop() so the Android main thread owns Wayland dispatch and the real
  // Choreographer drives frames — unblocking Handler-posted async work (Jetpack Compose).
  void present_content_looper(jobject view, int w, int h) {
#if ACL_HAVE_VULKAN && ACL_HAVE_WCS
    jclass pd = env_->FindClass("org/acl/internal/PresentDriver");
    if (pd == nullptr) {
      clear_exception();
      log::w(kTag, "--present(gpu): PresentDriver not on the boot classpath; falling back to loop");
      present_content_loop_gpu(view, w, h);
      return;
    }
    jmethodID run = env_->GetStaticMethodID(
        pd, "run", "(Landroid/view/View;IILjava/lang/String;Ljava/lang/String;)V");
    if (run == nullptr) {
      clear_exception();
      env_->DeleteLocalRef(pd);
      present_content_loop_gpu(view, w, h);
      return;
    }
    const std::string& pkg = config_.package_name;
    jstring title = env_->NewStringUTF(pkg.empty() ? "acl" : pkg.c_str());
    jstring appId = env_->NewStringUTF(pkg.empty() ? "org.acl.app" : pkg.c_str());
    log::i(kTag, "--present(gpu): handing off to PresentDriver (looper owns dispatch)");
    env_->CallStaticVoidMethod(pd, run, view, w, h, title, appId);
    if (env_->ExceptionCheck() == JNI_TRUE) fail_with_exception("PresentDriver.run");
    if (title != nullptr) env_->DeleteLocalRef(title);
    if (appId != nullptr) env_->DeleteLocalRef(appId);
    env_->DeleteLocalRef(pd);
#else
    (void)view; (void)w; (void)h;
#endif
  }

  void present_content_loop_gpu(jobject view, int w, int h) {
#if ACL_HAVE_VULKAN && ACL_HAVE_WCS
    log::i(kTag, "--present(gpu): entered {}x{}, checking compositor…", w, h);
    if (!wm::wayland_available()) {
      log::i(kTag, "--present(gpu): no Wayland compositor reachable; frame not shown");
      return;
    }
    struct AclDmabuf {
      int fd;
      std::uint32_t width, height, plane_count;
      std::uint32_t offsets[4], strides[4];
      std::uint64_t modifier;
      std::uint32_t drm_format;
    };
    auto begin = reinterpret_cast<long (*)(int, int)>(
        ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_begin_frame"));
    auto finish = reinterpret_cast<bool (*)(int, AclDmabuf*)>(
        ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_end_frame"));
    if (begin == nullptr || finish == nullptr) {
      log::w(kTag, "--present(gpu): begin/end frame missing (libacl_graphics_jni?)");
      return;
    }
    // Wrap the persistent GPU SkSurface canvas once; begin_frame clears it each tick.
    log::i(kTag, "--present(gpu): compositor ok, begin_frame…");
    const jlong ch = begin(w, h);
    if (ch == 0) { log::w(kTag, "--present(gpu): begin_frame failed"); return; }
    log::i(kTag, "--present(gpu): begin_frame ok (ch={}), setting up loop…", ch);
    jclass canvasC = env_->FindClass("android/graphics/Canvas");
    jmethodID cctor = canvasC ? env_->GetMethodID(canvasC, "<init>", "(J)V") : nullptr;
    jobject canvas = cctor ? env_->NewObject(canvasC, cctor, ch) : nullptr;
    if (canvas == nullptr) { clear_exception(); log::w(kTag, "--present(gpu): Canvas(long) failed"); return; }
    jclass vcls = env_->GetObjectClass(view);
    jmethodID draw = env_->GetMethodID(vcls, "draw", "(Landroid/graphics/Canvas;)V");
    // Measure/layout each frame (after the doFrame pump) so async content — Jetpack Compose
    // composes on the frame clock — is sized before it draws. MeasureSpec EXACTLY(w/h).
    jmethodID measure = env_->GetMethodID(vcls, "measure", "(II)V");
    jmethodID gmw = env_->GetMethodID(vcls, "getMeasuredWidth", "()I");
    jmethodID gmh = env_->GetMethodID(vcls, "getMeasuredHeight", "()I");
    jmethodID layout = env_->GetMethodID(vcls, "layout", "(IIII)V");
    env_->DeleteLocalRef(vcls);
    const jint ms_mask = 0x3 << 30;
    const jint wspec = (w & ~ms_mask) | (1 << 30);
    const jint hspec = (h & ~ms_mask) | (1 << 30);
    // Pump the main Looper each frame — async framework work (Jetpack Compose's recomposer)
    // posts to the Handler queue, which this native frame loop otherwise never runs.
    jclass boot_cls = env_->FindClass("org/acl/internal/HostBootstrap");
    jmethodID pump = boot_cls ? env_->GetStaticMethodID(boot_cls, "pumpMainLooper", "(I)V") : nullptr;
    if (boot_cls == nullptr) clear_exception();

    // Optional Choreographer pump (acl's shim exposes aclDoFrame). On host ART the REAL
    // Choreographer is present: its getInstance() needs a vsync native (DisplayEventReceiver)
    // and has no aclDoFrame — so tolerate both (clear the exception, skip it). The loop still
    // re-draws the View each vsync tick; a static View just repaints identically.
    jobject choreo = nullptr;
    jmethodID do_frame = nullptr;
    if (jclass ch_cls = env_->FindClass("android/view/Choreographer")) {
      if (jmethodID gi = env_->GetStaticMethodID(ch_cls, "getInstance",
                                                 "()Landroid/view/Choreographer;")) {
        choreo = env_->CallStaticObjectMethod(ch_cls, gi);
        if (env_->ExceptionCheck() == JNI_TRUE) { env_->ExceptionClear(); choreo = nullptr; }
      }
      if (choreo != nullptr) do_frame = env_->GetMethodID(ch_cls, "aclDoFrame", "(J)J");
      env_->DeleteLocalRef(ch_cls);
    }
    clear_exception();

    // Input: pointer -> MotionEvent.dispatchTouchEvent, key -> KeyEvent.dispatchKeyEvent
    // (same mapping as the shm present_content_loop; IME on this path is a follow-up).
    jobject motionevent_cls = nullptr, keyevent_cls = nullptr;
    jmethodID me_obtain = nullptr, me_recycle = nullptr, ke_obtain = nullptr;
    jmethodID dispatch_touch = nullptr, dispatch_key = nullptr;
    if (jclass mecls = env_->FindClass("android/view/MotionEvent")) {
      me_obtain = env_->GetStaticMethodID(mecls, "obtain", "(JJIFFI)Landroid/view/MotionEvent;");
      me_recycle = env_->GetMethodID(mecls, "recycle", "()V");
      motionevent_cls = env_->NewGlobalRef(mecls);
      env_->DeleteLocalRef(mecls);
    }
    if (jclass kecls = env_->FindClass("android/view/KeyEvent")) {
      ke_obtain = env_->GetStaticMethodID(kecls, "obtain", "(II)Landroid/view/KeyEvent;");
      keyevent_cls = env_->NewGlobalRef(kecls);
      env_->DeleteLocalRef(kecls);
    }
    if (jclass vc = env_->GetObjectClass(view)) {
      dispatch_touch = env_->GetMethodID(vc, "dispatchTouchEvent", "(Landroid/view/MotionEvent;)Z");
      dispatch_key = env_->GetMethodID(vc, "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z");
      env_->DeleteLocalRef(vc);
    }
    clear_exception();
    std::uint64_t down_time = 0;
    const wm::InputCallback input = [&, this](const wm::InputEvent& ev) {
      if (const auto* p = std::get_if<wm::PointerInput>(&ev)) {
        if (motionevent_cls == nullptr || me_obtain == nullptr || dispatch_touch == nullptr) return;
        const jint action = p->action == wm::PointerInput::Action::Down ? 0
                          : p->action == wm::PointerInput::Action::Up   ? 1 : 2;
        if (action == 0) down_time = frame_index_now();
        jobject me = env_->CallStaticObjectMethod(
            static_cast<jclass>(motionevent_cls), me_obtain, static_cast<jlong>(down_time),
            static_cast<jlong>(frame_index_now()), action, static_cast<jfloat>(p->x),
            static_cast<jfloat>(p->y), 0);
        if (me != nullptr) {
          env_->CallBooleanMethod(view, dispatch_touch, me);
          if (me_recycle != nullptr) env_->CallVoidMethod(me, me_recycle);
          env_->DeleteLocalRef(me);
        }
        if (env_->ExceptionCheck() == JNI_TRUE) clear_exception();
      } else if (const auto* k = std::get_if<wm::KeyInput>(&ev)) {
        if (keyevent_cls == nullptr || ke_obtain == nullptr || dispatch_key == nullptr) return;
        const jint action = k->action == wm::KeyInput::Action::Down ? 0 : 1;
        jobject ke = env_->CallStaticObjectMethod(static_cast<jclass>(keyevent_cls), ke_obtain,
                                                  action, android_keycode(k->keycode));
        if (ke != nullptr) {
          env_->CallBooleanMethod(view, dispatch_key, ke);
          env_->DeleteLocalRef(ke);
        }
        if (env_->ExceptionCheck() == JNI_TRUE) clear_exception();
      }
    };

    const wm::DmabufFrameFn render = [&](int bufidx, const std::vector<std::uint64_t>&,
                                         std::uint64_t frame_time_ns, std::uint64_t idx,
                                         gfx::DmabufFrame* out) -> bool {
      if (idx > 0 && begin(w, h) == 0) return false;  // re-arm + clear (frame 0 pre-armed)
      if (choreo != nullptr && do_frame != nullptr)
        env_->CallLongMethod(choreo, do_frame, static_cast<jlong>(frame_time_ns));
      if (env_->ExceptionCheck() == JNI_TRUE) { fail_with_exception("Choreographer.doFrame"); return false; }
      // Run pending Handler work (Compose recomposition) posted by doFrame, before measure.
      if (pump != nullptr && boot_cls != nullptr) {
        env_->CallStaticVoidMethod(boot_cls, pump, static_cast<jint>(256));
        if (env_->ExceptionCheck() == JNI_TRUE) clear_exception();
      }
      // measure → layout → draw (post-doFrame, so Compose has composed before it lays out).
      if (measure != nullptr && layout != nullptr) {
        env_->CallVoidMethod(view, measure, wspec, hspec);
        const int mw = env_->CallIntMethod(view, gmw);
        const int mh = env_->CallIntMethod(view, gmh);
        env_->CallVoidMethod(view, layout, 0, 0, mw, mh);
        if (idx == 0) log::i(kTag, "--present(gpu): frame0 measured {}x{}", mw, mh);
      }
      if (draw != nullptr) env_->CallVoidMethod(view, draw, canvas);
      if (env_->ExceptionCheck() == JNI_TRUE) { fail_with_exception("View.draw"); return false; }
      AclDmabuf f{};
      if (!finish(bufidx, &f) || f.fd < 0) return false;
      gfx::DmabufFrame fr{};
      fr.fd = f.fd;
      fr.width = f.width;
      fr.height = f.height;
      fr.plane_count = f.plane_count;
      for (std::uint32_t i = 0; i < f.plane_count && i < 4; ++i) {
        fr.planes[i].offset = f.offsets[i];
        fr.planes[i].stride = f.strides[i];
      }
      fr.modifier = f.modifier;
      fr.drm_format = f.drm_format;
      fr.modifier_tiled = f.modifier != 0;
      *out = fr;
      return true;
    };
    const std::string& pkg = config_.package_name;
    log::i(kTag, "--present(gpu): starting present_dmabuf_loop (budget={})", config_.present_frames);
    const bool ok = wm::present_dmabuf_loop(w, h, pkg.empty() ? "acl" : pkg,
                                            pkg.empty() ? "org.acl.app" : pkg, render, input,
                                            /*want_ime=*/false, config_.present_frames);
    log::i(kTag, "--present(gpu): View.draw → Ganesh Vulkan → dma-buf → Wayland {} (zero-copy)",
           ok ? "presented" : "did not present");
    if (choreo != nullptr) env_->DeleteLocalRef(choreo);
    if (canvas != nullptr) env_->DeleteLocalRef(canvas);
    if (canvasC != nullptr) env_->DeleteLocalRef(canvasC);
    if (motionevent_cls != nullptr) env_->DeleteGlobalRef(motionevent_cls);
    if (keyevent_cls != nullptr) env_->DeleteGlobalRef(keyevent_cls);
#else
    (void)view; (void)w; (void)h;
#endif
  }

 private:
  // Map a Linux evdev scancode to an Android KeyEvent keycode (a small practical
  // subset; unmapped keys become KEYCODE_UNKNOWN=0). Mirrors Android's Generic.kl.
  static jint android_keycode(std::uint32_t evdev) {
    switch (evdev) {
      case 30: return 29;  case 48: return 30;  case 46: return 31;  case 32: return 32;  // A B C D
      case 18: return 33;  case 33: return 34;  case 34: return 35;  case 35: return 36;  // E F G H
      case 23: return 37;  case 36: return 38;  case 37: return 39;  case 38: return 40;  // I J K L
      case 50: return 41;  case 49: return 42;  case 24: return 43;  case 25: return 44;  // M N O P
      case 16: return 45;  case 19: return 46;  case 31: return 47;  case 20: return 48;  // Q R S T
      case 22: return 49;  case 47: return 50;  case 17: return 51;  case 45: return 52;  // U V W X
      case 21: return 53;  case 44: return 54;                                            // Y Z
      case 2: return 8;   case 3: return 9;   case 4: return 10;  case 5: return 11;       // 1 2 3 4
      case 6: return 12;  case 7: return 13;  case 8: return 14;  case 9: return 15;       // 5 6 7 8
      case 10: return 16; case 11: return 7;                                               // 9 0
      case 28: return 66;  // ENTER
      case 57: return 62;  // SPACE
      case 1: return 111;  // ESC -> KEYCODE_ESCAPE
      case 14: return 67;  // BACKSPACE -> KEYCODE_DEL
      case 15: return 61;  // TAB
      case 105: return 21; case 106: return 22;  // LEFT RIGHT
      case 103: return 19; case 108: return 20;  // UP DOWN
      default: return 0;   // KEYCODE_UNKNOWN
    }
  }

  // A monotonic timestamp (ms-ish) for MotionEvent times; wall-clock isn't needed.
  static std::uint64_t frame_index_now() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ull;
  }

  // Print + clear a pending exception and return a RuntimeError describing `what`.
  Error fail_with_exception(const std::string& what) {
    if (env_->ExceptionCheck() == JNI_TRUE) {
      env_->ExceptionDescribe();
      env_->ExceptionClear();
    }
    return Error::of(Status::RuntimeError, "failed to " + what);
  }

  // Call onGetRoot(pkg, uid, null) and read BrowserRoot.getRootId(). `api` supplies
  // the framework-vs-androidx nested-class names.
  Error browse_root(jclass cls, jobject svc, const MbsApi& api) {
    const std::string sig =
        "(Ljava/lang/String;ILandroid/os/Bundle;)L" + api.browser_root + ";";
    jmethodID on_get_root = env_->GetMethodID(cls, "onGetRoot", sig.c_str());
    if (on_get_root == nullptr) {
      clear_exception();
      return Error::of(Status::NotFound, "no onGetRoot(String,int,Bundle)");
    }
    jstring pkg = env_->NewStringUTF("acl");
    jobject root = env_->CallObjectMethod(svc, on_get_root, pkg, static_cast<jint>(1000),
                                          nullptr);
    env_->DeleteLocalRef(pkg);
    if (env_->ExceptionCheck() == JNI_TRUE) return fail_with_exception("onGetRoot()");
    if (root == nullptr) {
      return Error::of(Status::RuntimeError, "onGetRoot returned null (browse rejected)");
    }
    jclass root_cls = env_->GetObjectClass(root);
    jmethodID get_root_id =
        env_->GetMethodID(root_cls, "getRootId", "()Ljava/lang/String;");
    Error result = Error::ok();
    if (get_root_id == nullptr) {
      clear_exception();
      result = Error::of(Status::NotFound, "no BrowserRoot.getRootId()");
    } else {
      auto id = static_cast<jstring>(env_->CallObjectMethod(root, get_root_id));
      const char* c = id != nullptr ? env_->GetStringUTFChars(id, nullptr) : nullptr;
      log::i(kTag, "media browse root: {}", c != nullptr ? c : "?");
      if (c != nullptr) env_->ReleaseStringUTFChars(id, c);
      result = load_children(cls, svc, id, api);  // walk the root's children
      if (id != nullptr) env_->DeleteLocalRef(id);
    }
    env_->DeleteLocalRef(root_cls);
    env_->DeleteLocalRef(root);
    return result;
  }

  // Call onLoadChildren(parentId, new Result()) and enumerate the MediaItems the
  // app sent — their titles (in-process, no Binder). A String utf8 accessor.
  std::string utf8(jstring s) {
    if (s == nullptr) return {};
    const char* c = env_->GetStringUTFChars(s, nullptr);
    std::string out(c != nullptr ? c : "");
    if (c != nullptr) env_->ReleaseStringUTFChars(s, c);
    return out;
  }

  // MediaMetadata.getString(key), or `dflt` when the key is absent/null.
  std::string metadata_string(jobject md, jmethodID get_string, const char* key,
                              const char* dflt) {
    jstring k = env_->NewStringUTF(key);
    auto v = static_cast<jstring>(env_->CallObjectMethod(md, get_string, k));
    std::string out = v != nullptr ? utf8(v) : std::string(dflt);
    if (v != nullptr) env_->DeleteLocalRef(v);
    env_->DeleteLocalRef(k);
    return out;
  }

  Error load_children(jclass cls, jobject svc, jstring parent_id, const MbsApi& api) {
    const std::string on_load_sig = "(Ljava/lang/String;L" + api.result_cls + ";)V";
    jmethodID on_load = env_->GetMethodID(cls, "onLoadChildren", on_load_sig.c_str());
    if (on_load == nullptr) { clear_exception(); return Error::ok(); }  // optional

    jclass result_cls = env_->FindClass(api.result_cls.c_str());
    jmethodID result_ctor =
        result_cls != nullptr ? env_->GetMethodID(result_cls, "<init>", "()V") : nullptr;
    jobject result = result_ctor != nullptr ? env_->NewObject(result_cls, result_ctor) : nullptr;
    if (result == nullptr) { clear_exception(); return fail_with_exception("build Result"); }

    env_->CallVoidMethod(svc, on_load, parent_id, result);
    if (env_->ExceptionCheck() == JNI_TRUE) {
      env_->DeleteLocalRef(result); env_->DeleteLocalRef(result_cls);
      return fail_with_exception("onLoadChildren()");
    }

    // list = result.aclValue()
    jobject list = env_->CallObjectMethod(
        result, env_->GetMethodID(result_cls, "aclValue", "()Ljava/lang/Object;"));
    Error result_err = Error::ok();
    if (list == nullptr) {
      log::i(kTag, "media children: (none sent)");
    } else {
      jclass list_cls = env_->FindClass("java/util/List");
      jmethodID size_m = env_->GetMethodID(list_cls, "size", "()I");
      jmethodID get_m = env_->GetMethodID(list_cls, "get", "(I)Ljava/lang/Object;");
      const jint n = env_->CallIntMethod(list, size_m);
      const std::string parent = utf8(parent_id);
      log::i(kTag, "media children of '{}': {} item(s)", parent, n);
      std::vector<std::string> titles;
      for (jint i = 0; i < n; ++i) {
        jobject item = env_->CallObjectMethod(list, get_m, i);  // MediaBrowser.MediaItem
        jclass item_cls = env_->GetObjectClass(item);
        const std::string desc_sig = "()L" + api.media_desc + ";";
        jobject desc = env_->CallObjectMethod(
            item, env_->GetMethodID(item_cls, "getDescription", desc_sig.c_str()));
        jclass desc_cls = env_->GetObjectClass(desc);
        auto title = env_->CallObjectMethod(  // CharSequence
            desc, env_->GetMethodID(desc_cls, "getTitle", "()Ljava/lang/CharSequence;"));
        std::string title_s = "(no title)";
        if (title != nullptr) {  // CharSequence.toString()
          jclass cs_cls = env_->GetObjectClass(title);
          auto s = static_cast<jstring>(env_->CallObjectMethod(
              title, env_->GetMethodID(cs_cls, "toString", "()Ljava/lang/String;")));
          title_s = utf8(s);
          if (s != nullptr) env_->DeleteLocalRef(s);
          env_->DeleteLocalRef(cs_cls);
          env_->DeleteLocalRef(title);
        }
        log::i(kTag, "  media item: {}", title_s);
        titles.push_back(title_s);
        env_->DeleteLocalRef(desc_cls); env_->DeleteLocalRef(desc);
        env_->DeleteLocalRef(item_cls); env_->DeleteLocalRef(item);
      }
      env_->DeleteLocalRef(list_cls);
      env_->DeleteLocalRef(list);
#if ACL_PROFILE_AUTOMOTIVE
      // Publish the browse tree snapshot northbound (Gate G1.5a).
      auto& bridge = hosts::media::MediaBridge::instance();
      bridge.publish_browse(parent, titles);
      log::i(kTag, "MediaBridge: browse snapshot root='{}' {} item(s)",
             bridge.browse_root(), bridge.browse_items().size());
#endif
    }
    env_->DeleteLocalRef(result);
    env_->DeleteLocalRef(result_cls);
    return result_err;
  }

#if ACL_PROFILE_AUTOMOTIVE
  // Walk a CarAppService -> Session -> Screen -> Template and render it through the
  // native CAL host (Gate G1.5b). In-process, no Binder (ADR-3).
  Error render_car_app(jclass cls, jobject svc) {
    jmethodID m_sess = env_->GetMethodID(cls, "onCreateSession",
                                         "()Landroidx/car/app/Session;");
    if (m_sess == nullptr) { clear_exception(); return Error::of(Status::NotFound, "no onCreateSession()"); }
    jobject session = env_->CallObjectMethod(svc, m_sess);
    if (env_->ExceptionCheck() == JNI_TRUE) return fail_with_exception("onCreateSession()");
    if (session == nullptr) return Error::of(Status::RuntimeError, "onCreateSession returned null");

    jclass sess_cls = env_->GetObjectClass(session);
    jmethodID m_screen = env_->GetMethodID(sess_cls, "onCreateScreen",
                                           "()Landroidx/car/app/Screen;");
    jobject screen = m_screen != nullptr ? env_->CallObjectMethod(session, m_screen) : nullptr;
    if (env_->ExceptionCheck() == JNI_TRUE) { env_->DeleteLocalRef(sess_cls); env_->DeleteLocalRef(session); return fail_with_exception("onCreateScreen()"); }
    if (screen == nullptr) { env_->DeleteLocalRef(sess_cls); env_->DeleteLocalRef(session); return Error::of(Status::RuntimeError, "onCreateScreen returned null"); }

    jclass screen_cls = env_->GetObjectClass(screen);
    jmethodID m_tmpl = env_->GetMethodID(screen_cls, "onGetTemplate",
                                         "()Landroidx/car/app/model/Template;");
    jobject tmpl = m_tmpl != nullptr ? env_->CallObjectMethod(screen, m_tmpl) : nullptr;
    Error result = Error::ok();
    if (env_->ExceptionCheck() == JNI_TRUE) {
      result = fail_with_exception("onGetTemplate()");
    } else if (tmpl == nullptr) {
      result = Error::of(Status::RuntimeError, "onGetTemplate returned null");
    } else {
      const hosts::cal::Template t = read_template(tmpl);
      auto& host = hosts::cal::CalHost::instance();
      host.render(t);
      log::i(kTag, "CalHost: rendered [{}] '{}' — {} row(s), limit {}, truncated={}",
             hosts::cal::template_kind_name(t.kind), t.title, host.visible_rows(),
             host.content_limit(), host.truncated());
      for (const std::string& line : host.lines()) log::i(kTag, "  {}", line);
      env_->DeleteLocalRef(tmpl);
    }
    env_->DeleteLocalRef(screen_cls); env_->DeleteLocalRef(screen);
    env_->DeleteLocalRef(sess_cls); env_->DeleteLocalRef(session);
    return result;
  }

  // Deserialize a Template into the native model (Tier-1: ListTemplate). The shim's
  // acl* accessors substitute for the Bundleable read the host would do over Binder.
  hosts::cal::Template read_template(jobject tmpl) {
    hosts::cal::Template t;
    jclass lt_cls = env_->FindClass("androidx/car/app/model/ListTemplate");
    if (lt_cls == nullptr || env_->IsInstanceOf(tmpl, lt_cls) != JNI_TRUE) {
      clear_exception();
      if (lt_cls != nullptr) env_->DeleteLocalRef(lt_cls);
      t.kind = hosts::cal::TemplateKind::Unknown;
      log::w(kTag, "CAL template is not a Tier-1 ListTemplate; rendering header only");
      return t;
    }
    t.kind = hosts::cal::TemplateKind::List;
    jclass tcls = env_->GetObjectClass(tmpl);
    auto title = static_cast<jstring>(env_->CallObjectMethod(
        tmpl, env_->GetMethodID(tcls, "aclTitle", "()Ljava/lang/String;")));
    if (title != nullptr) { t.title = utf8(title); env_->DeleteLocalRef(title); }
    jobject list = env_->CallObjectMethod(
        tmpl, env_->GetMethodID(tcls, "aclList", "()Landroidx/car/app/model/ItemList;"));
    if (list != nullptr) {
      jclass il_cls = env_->GetObjectClass(list);
      jobject rows = env_->CallObjectMethod(
          list, env_->GetMethodID(il_cls, "aclRows", "()Ljava/util/List;"));
      if (rows != nullptr) {
        jclass list_cls = env_->FindClass("java/util/List");
        jmethodID size_m = env_->GetMethodID(list_cls, "size", "()I");
        jmethodID get_m = env_->GetMethodID(list_cls, "get", "(I)Ljava/lang/Object;");
        const jint n = env_->CallIntMethod(rows, size_m);
        for (jint i = 0; i < n; ++i) {
          jobject row = env_->CallObjectMethod(rows, get_m, i);
          jclass row_cls = env_->GetObjectClass(row);
          hosts::cal::Row r;
          auto rt = static_cast<jstring>(env_->CallObjectMethod(
              row, env_->GetMethodID(row_cls, "aclTitle", "()Ljava/lang/String;")));
          if (rt != nullptr) { r.title = utf8(rt); env_->DeleteLocalRef(rt); }
          auto rx = static_cast<jstring>(env_->CallObjectMethod(
              row, env_->GetMethodID(row_cls, "aclText", "()Ljava/lang/String;")));
          if (rx != nullptr) { r.text = utf8(rx); env_->DeleteLocalRef(rx); }
          t.rows.push_back(std::move(r));
          env_->DeleteLocalRef(row_cls); env_->DeleteLocalRef(row);
        }
        env_->DeleteLocalRef(list_cls); env_->DeleteLocalRef(rows);
      }
      env_->DeleteLocalRef(il_cls); env_->DeleteLocalRef(list);
    }
    env_->DeleteLocalRef(tcls); env_->DeleteLocalRef(lt_cls);
    return t;
  }
#endif  // ACL_PROFILE_AUTOMOTIVE

  // After onCreate, read the published MediaSession via getSessionToken() ->
  // Token -> MediaSession, and log its now-playing metadata + playback state
  // (in-process, no controller/Binder). No session is not an error.
  Error read_session(jclass cls, jobject svc) {
    jmethodID get_token = env_->GetMethodID(
        cls, "getSessionToken", "()Landroid/media/session/MediaSession$Token;");
    if (get_token == nullptr) { clear_exception(); return Error::ok(); }
    jobject token = env_->CallObjectMethod(svc, get_token);
    if (env_->ExceptionCheck() == JNI_TRUE) return fail_with_exception("getSessionToken()");
    if (token == nullptr) { return Error::ok(); }  // app published no session

    jclass token_cls = env_->GetObjectClass(token);
    jobject session = env_->CallObjectMethod(
        token, env_->GetMethodID(token_cls, "aclSession",
                                 "()Landroid/media/session/MediaSession;"));
    jclass ses_cls = session != nullptr ? env_->GetObjectClass(session) : nullptr;

    std::string title = "(none)";
    std::string artist;
    int state = -1;
    if (session != nullptr) {
      jobject md = env_->CallObjectMethod(
          session, env_->GetMethodID(ses_cls, "aclMetadata",
                                     "()Landroid/media/MediaMetadata;"));
      if (md != nullptr) {
        jclass md_cls = env_->GetObjectClass(md);
        jmethodID get_string = env_->GetMethodID(
            md_cls, "getString", "(Ljava/lang/String;)Ljava/lang/String;");
        title = metadata_string(md, get_string, "android.media.metadata.TITLE", "(none)");
        artist = metadata_string(md, get_string, "android.media.metadata.ARTIST", "");
        env_->DeleteLocalRef(md_cls); env_->DeleteLocalRef(md);
      }
      jobject ps = env_->CallObjectMethod(
          session, env_->GetMethodID(ses_cls, "aclPlaybackState",
                                     "()Landroid/media/session/PlaybackState;"));
      if (ps != nullptr) {
        jclass ps_cls = env_->GetObjectClass(ps);
        state = env_->CallIntMethod(ps, env_->GetMethodID(ps_cls, "getState", "()I"));
        env_->DeleteLocalRef(ps_cls); env_->DeleteLocalRef(ps);
      }
    }
    log::i(kTag, "media session: title='{}' state={}", title, state);
#if ACL_PROFILE_AUTOMOTIVE
    // Publish northbound into the media bridge (Gate G1.5a) and report the MPRIS
    // projection a Linux desktop / OEM shell controller would consume.
    {
      auto& bridge = hosts::media::MediaBridge::instance();
      hosts::media::NowPlaying np;
      np.title = title;
      np.artist = artist;
      np.state = hosts::media::playback_from_state(state);
      bridge.publish(std::move(np));
      const auto m = bridge.mpris();
      log::i(kTag, "MediaBridge: published now-playing gen={} MPRIS PlaybackStatus={} title='{}'",
             bridge.generation(), m.playback_status, m.title);
    }
#endif
    if (session != nullptr) invoke_transport(ses_cls, session);
    if (ses_cls != nullptr) env_->DeleteLocalRef(ses_cls);
    if (session != nullptr) env_->DeleteLocalRef(session);
    env_->DeleteLocalRef(token_cls);
    env_->DeleteLocalRef(token);
    return Error::ok();
  }

  // Invoke one MediaSession.Callback transport method by name (no-op if absent).
  void dispatch_callback(jobject cb, jclass cb_cls, const char* method) {
    jmethodID m = env_->GetMethodID(cb_cls, method, "()V");
    if (m == nullptr) { clear_exception(); return; }
    env_->CallVoidMethod(cb, m);
    if (env_->ExceptionCheck() == JNI_TRUE) {
      env_->ExceptionDescribe();
      env_->ExceptionClear();
    }
  }

  // Drive transport controls in-process (no controller/Binder). Under the automotive
  // profile a controller (MPRIS / OEM shell) posts commands into the media bridge;
  // we drain that channel and dispatch each to the app's MediaSession.Callback —
  // the app's overrides react. Otherwise a fixed Play/Pause pair is invoked directly.
  void invoke_transport(jclass ses_cls, jobject session) {
    jobject cb = env_->CallObjectMethod(
        session, env_->GetMethodID(ses_cls, "aclCallback",
                                   "()Landroid/media/session/MediaSession$Callback;"));
    if (cb == nullptr) { clear_exception(); return; }  // app set no callback
    jclass cb_cls = env_->GetObjectClass(cb);
#if ACL_PROFILE_AUTOMOTIVE
    auto& bridge = hosts::media::MediaBridge::instance();
    bridge.post_command(hosts::media::Command::Play);   // as an MPRIS controller would
    bridge.post_command(hosts::media::Command::Pause);
    hosts::media::Command c;
    while (bridge.next_command(c)) {
      const char* method = hosts::media::command_callback(c);
      log::i(kTag, "MediaBridge: dispatching command {} -> {}",
             hosts::media::command_name(c), method);
      dispatch_callback(cb, cb_cls, method);
      if (c == hosts::media::Command::Play) {
        // Playback started: the bridge routes the now-playing audio to the device.
        const std::uint64_t frames = bridge.route_now_playing();
        log::i(kTag, "MediaBridge: routing now-playing '{}' to PipeWire ({} frames)",
               bridge.snapshot().title, frames);
      }
    }
#else
    for (const char* action : {"onPlay", "onPause"}) dispatch_callback(cb, cb_cls, action);
#endif
    env_->DeleteLocalRef(cb_cls);
    env_->DeleteLocalRef(cb);
  }

  // Invoke one lifecycle callback (found on `cls` or a superclass). `pass_bundle`
  // passes a null Bundle (a fresh launch); absence of the method is not an error
  // (a subclass may not override every callback beyond the ones Activity defines).
  Error call_lifecycle(jclass cls, jobject activity, const char* method,
                       const char* sig, bool pass_bundle) {
    jmethodID mid = env_->GetMethodID(cls, method, sig);
    if (mid == nullptr) {  // shouldn't happen (Activity defines them), but be safe
      clear_exception();
      return Error::ok();
    }
    if (pass_bundle) {
      env_->CallVoidMethod(activity, mid, nullptr);  // savedInstanceState = null
    } else {
      env_->CallVoidMethod(activity, mid);
    }
    if (env_->ExceptionCheck() == JNI_TRUE) {
      return fail_with_exception(std::string(method) + "()");
    }
    return Error::ok();
  }

  jobjectArray make_string_array(const std::vector<std::string>& args) {
    jclass str_cls = env_->FindClass("java/lang/String");
    if (str_cls == nullptr) {
      clear_exception();
      return nullptr;
    }
    jobjectArray arr =
        env_->NewObjectArray(static_cast<jsize>(args.size()), str_cls, nullptr);
    if (arr != nullptr) {
      for (std::size_t i = 0; i < args.size(); ++i) {
        jstring s = env_->NewStringUTF(args[i].c_str());
        env_->SetObjectArrayElement(arr, static_cast<jsize>(i), s);
        env_->DeleteLocalRef(s);
      }
    }
    env_->DeleteLocalRef(str_cls);
    return arr;
  }

 protected:
  VmConfig config_;
  JavaVM* jvm_ = nullptr;
  JNIEnv* env_ = nullptr;
#if ACL_PROFILE_AUTOMOTIVE
  ToneAudioRoute audio_route_;  // media bridge -> acl::audio -> PipeWire
#endif
};

// OpenJDK backend (ADR-0001 fallback): links HotSpot's JNI_CreateJavaVM. A stock
// JVM has no separately repopulatable boot classpath, so the vendored framework
// (boot_classpath) is folded into java.class.path ahead of the app classes. Stock
// JVM bytecode only — DEX is fed via the dex_to_jvm translator (--convert-dex).
class JvmVm final : public JniVm {
 public:
  explicit JvmVm(VmConfig config) : JniVm(Backend::Jvm, std::move(config)) {}

 protected:
  Error create_java_vm(JNIEnv*& env) override {
    std::vector<std::string> cp = config_.boot_classpath;
    cp.insert(cp.end(), config_.classpath.begin(), config_.classpath.end());

    std::vector<std::string> option_strings;
    option_strings.push_back("-Djava.class.path=" + join(cp, ':'));
    for (const std::string& opt : config_.options) option_strings.push_back(opt);

    std::vector<JavaVMOption> options;
    options.reserve(option_strings.size());
    for (std::string& s : option_strings) {
      options.push_back(JavaVMOption{s.data(), nullptr});
    }

    JavaVMInitArgs args{};
    args.version = JNI_VERSION_1_8;
    args.nOptions = static_cast<jint>(options.size());
    args.options = options.data();
    args.ignoreUnrecognized = JNI_FALSE;

    const jint rc =
        JNI_CreateJavaVM(&jvm_, reinterpret_cast<void**>(&env), &args);
    if (rc != JNI_OK || jvm_ == nullptr) {
      jvm_ = nullptr;
      return Error::of(Status::RuntimeError,
                       "JNI_CreateJavaVM failed (rc=" + std::to_string(rc) + ")");
    }
    return Error::ok();
  }
};

// Host ART backend (ADR-0001 target): dlopens a prebuilt host ART's libart.so and
// boots it via the JNI Invocation API, so the app's DEX runs natively — no
// translation. ACL_ART_HOME points at the host ART tree's out/host/linux-x86 (holds
// lib64/libart.so with RUNPATH=$ORIGIN so its deps resolve, plus apex/com.android.*
// with the core bootclasspath jars). M2 milestone: imageless + interpreter (-Xint);
// the dex2oat'd framework boot image and app OAT cache are M3.
class ArtVm final : public JniVm {
 public:
  explicit ArtVm(VmConfig config) : JniVm(Backend::Art, std::move(config)) {}

 protected:
  Error create_java_vm(JNIEnv*& env) override {
    const char* home_c = std::getenv("ACL_ART_HOME");
    if (home_c == nullptr || *home_c == '\0') {
      return Error::of(
          Status::InvalidConfig,
          "ACL_ART_HOME not set: point it at a host ART build's out/host/linux-x86 "
          "(lib64/libart.so + apex/com.android.art). See docs/art-host-spike.md");
    }
    const std::string home = home_c;

    // Host ART locates its runtime/data modules via these roots. Note the host
    // apex layout is mixed: the javalib bootclasspath jars + boot image live under
    // $HOME/apex/com.android.*/, but the ICU/tzdata *data* roots are the non-apex
    // $HOME/com.android.{i18n,tzdata} (that is where etc/icu/icudt*.dat lives, and
    // libicu_jni aborts at startup if ANDROID_I18N_ROOT does not point at it).
    ::setenv("ANDROID_ROOT", home.c_str(), 1);
    ::setenv("ANDROID_ART_ROOT", (home + "/apex/com.android.art").c_str(), 1);
    ::setenv("ANDROID_I18N_ROOT", (home + "/com.android.i18n").c_str(), 1);
    ::setenv("ANDROID_TZDATA_ROOT", (home + "/com.android.tzdata").c_str(), 1);
    if (std::getenv("ANDROID_DATA") == nullptr) {
      ::setenv("ANDROID_DATA", (home + "/acl-android-data").c_str(), 1);
    }

    // libart.so carries RUNPATH=$ORIGIN, so its lib64/ siblings resolve without
    // LD_LIBRARY_PATH; RTLD_GLOBAL exposes ART's JNI symbols to loaded natives.
    const std::string libart = home + "/lib64/libart.so";
    void* handle = ::dlopen(libart.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
      const char* e = ::dlerror();
      return Error::of(Status::Unavailable,
                       "dlopen(" + libart + ") failed: " + (e ? e : "?"));
    }
    using CreateVmFn = jint (*)(JavaVM**, void**, void*);
    auto create_vm =
        reinterpret_cast<CreateVmFn>(::dlsym(handle, "JNI_CreateJavaVM"));
    if (create_vm == nullptr) {
      return Error::of(Status::Unavailable,
                       "JNI_CreateJavaVM not found in " + libart);
    }

    // Core boot classpath (java.* + a few libs), in ART image order. ART loads
    // these DEX jars natively. config_.boot_classpath (acl's android.jar stubs, for
    // the JVM backend) does not apply here — the real framework is an M3 boot image.
    struct BcpJar { const char* module; const char* apex; };
    static const BcpJar kBcp[] = {
        {"core-oj", "com.android.art"},     {"core-libart", "com.android.art"},
        {"okhttp", "com.android.art"},      {"bouncycastle", "com.android.art"},
        {"apache-xml", "com.android.art"},  {"core-icu4j", "com.android.i18n"},
        {"conscrypt", "com.android.conscrypt"}};
    std::vector<std::string> bcp;
    for (const BcpJar& j : kBcp) {
      bcp.push_back(home + "/apex/" + j.apex + "/javalib/" + j.module + ".jar");
    }
    // P2.1 (ADR-0002): append the real framework to the BOOT classpath (not the app
    // classpath) so framework classes get boot-classloader identity as on device —
    // needed for framework internals that assume bootclasspath residency. Colon-
    // separated jars (framework-minus-apex, framework-graphics, ext, ...).
    if (const char* ext = std::getenv("ACL_ART_BOOTCLASSPATH");
        ext != nullptr && *ext != '\0') {
      bcp.emplace_back(ext);
    }
    const std::string bcp_joined = join(bcp, ':');

    // Application classpath: the vendored android.* framework DEX (ADR-0002) ahead
    // of the app, so real framework classes (android.app.Activity, ...) resolve —
    // the JVM backend folds these into java.class.path from .class dirs, but ART
    // needs DEX, so ACL_ART_FRAMEWORK carries the d8'd framework (colon-separated;
    // shim dex before the android.jar-stub dex). Absent it, only java.* is available
    // and apps that touch the framework fail to load — a static-main DEX still runs.
    std::vector<std::string> cp;
    if (const char* fw = std::getenv("ACL_ART_FRAMEWORK"); fw != nullptr && *fw != '\0') {
      cp.emplace_back(fw);
    }
    // acl's host-ART runtime bootstrap DEX (org.acl.internal.HostBootstrap): the framework
    // environment (main Looper, no-Binder ServiceManager, minimal Context) an app expects
    // the runtime to provide. Injected ahead of the app so a pure app needs no setup.
    if (const char* hb = std::getenv("ACL_ART_HOST_BOOTSTRAP"); hb != nullptr && *hb != '\0') {
      cp.emplace_back(hb);
    }
    cp.insert(cp.end(), config_.classpath.begin(), config_.classpath.end());

    std::vector<std::string> option_strings;
    option_strings.push_back("-Xbootclasspath:" + bcp_joined);
    option_strings.push_back("-Xbootclasspath-locations:" + bcp_joined);
    // P2.1: a dex2oat'd framework boot image (ACL_ART_BOOT_IMAGE) makes the framework
    // AOT-compiled on the bootclasspath; without one, interpret imageless (-Xint). The
    // image's baked bootclasspath must match ACL_ART_BOOTCLASSPATH or ART falls back.
    if (const char* img = std::getenv("ACL_ART_BOOT_IMAGE");
        img != nullptr && *img != '\0') {
      option_strings.push_back(std::string("-Ximage:") + img);
    } else {
      option_strings.push_back("-Xint");  // imageless: interpret; no runtime dex2oat
    }
    option_strings.push_back("-Djava.class.path=" + join(cp, ':'));
    for (const std::string& opt : config_.options) option_strings.push_back(opt);

    std::vector<JavaVMOption> options;
    options.reserve(option_strings.size());
    for (std::string& s : option_strings) {
      options.push_back(JavaVMOption{s.data(), nullptr});
    }

    JavaVMInitArgs args{};
    args.version = JNI_VERSION_1_6;
    args.nOptions = static_cast<jint>(options.size());
    args.options = options.data();
    args.ignoreUnrecognized = JNI_FALSE;

    const jint rc = create_vm(&jvm_, reinterpret_cast<void**>(&env), &args);
    if (rc != JNI_OK || jvm_ == nullptr) {
      jvm_ = nullptr;
      return Error::of(Status::RuntimeError,
                       "ART JNI_CreateJavaVM failed (rc=" + std::to_string(rc) +
                           "); ACL_ART_HOME=" + home);
    }
    return Error::ok();
  }

  // ADR-0008: register the real framework's native methods from the host
  // libandroid_runtime so android.* natives (SystemProperties, Parcel, resources, …)
  // execute. Opt-in via ACL_ART_NATIVE, and only meaningful with the real framework
  // (framework-minus-apex) on the classpath so each register_*'s FindClass resolves.
  // Without it, fall back to acl's shim natives (the M2/M3 path). The host lib strips
  // AndroidRuntime::startReg, but the per-component register_android_*(env) functions
  // are exported, so acl calls a curated set of the pure-framework ones directly (no
  // AndroidRuntime/gCurRuntime dependency).
  void register_natives(JNIEnv* env) override {
    const char* opt = std::getenv("ACL_ART_NATIVE");
    const char* home_c = std::getenv("ACL_ART_HOME");
    if (opt == nullptr || *opt == '\0' || home_c == nullptr || *home_c == '\0') {
      register_framework_natives(env);  // shim floor (M2/M3)
      return;
    }
    const std::string lib = std::string(home_c) + "/lib64/libandroid_runtime.so";
    void* h = ::dlopen(lib.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (h == nullptr) {
      const char* e = ::dlerror();
      log::w(kTag, "ART native floor: dlopen({}) failed: {}; using shim natives", lib,
             e ? e : "?");
      register_framework_natives(env);  // fall back to the shim floor
      return;
    }
    // The framework native registrars the host libandroid_runtime exports (its
    // LayoutlibLoader host build compiles these device-independent-enough natives —
    // resources, input, os, db, animation). Each register_*(env) FindClass'es its
    // framework class and RegisterNatives; missing ones are cleared and counted. This
    // clears the real-Activity frontier (Resources -> AssetManager native). Natives
    // NOT here (View, Bitmap/Canvas, Configuration) stay target-only — the P2.4 render
    // subsystem (docs/resources-host-scope.md, ADR-0008).
    static constexpr const char* kReg[] = {
        // resources
        "_ZN7android37register_android_content_AssetManagerEP7_JNIEnv",
        "_ZN7android38register_android_content_res_ApkAssetsEP7_JNIEnv",
        "_ZN7android33register_android_content_XmlBlockEP7_JNIEnv",
        "_ZN7android36register_android_content_StringBlockEP7_JNIEnv",
        // os / looper / props
        "_ZN7android36register_android_os_SystemPropertiesEP7_JNIEnv",
        "_ZN7android31register_android_os_SystemClockEP7_JNIEnv",
        "_ZN7android25register_android_os_TraceEP7_JNIEnv",
        "_ZN7android26register_android_os_ParcelEP7_JNIEnv",
        "_ZN7android32register_android_os_MessageQueueEP7_JNIEnv",
        "_ZN7android32register_android_os_FileObserverEP7_JNIEnv",
        // util
        "_ZN7android25register_android_util_LogEP7_JNIEnv",
        "_ZN7android30register_android_util_EventLogEP7_JNIEnv",
        "_ZN7android38register_android_text_AndroidCharacterEP7_JNIEnv",
        "_ZN7android39register_android_util_jar_StrictJarFileEP7_JNIEnv",
        // input / view (event plumbing; not the View/graphics render natives)
        "_ZN7android30register_android_view_KeyEventEP7_JNIEnv",
        "_ZN7android33register_android_view_MotionEventEP7_JNIEnv",
        "_ZN7android33register_android_view_InputDeviceEP7_JNIEnv",
        "_ZN7android34register_android_view_InputChannelEP7_JNIEnv",
        "_ZN7android37register_android_view_KeyCharacterMapEP7_JNIEnv",
        "_ZN7android37register_android_view_VelocityTrackerEP7_JNIEnv",
        "_ZN7android38register_android_view_InputEventSenderEP7_JNIEnv",
        "_ZN7android40register_android_view_InputEventReceiverEP7_JNIEnv",
        "_ZN7android44register_android_view_InputApplicationHandleEP7_JNIEnv",
        // database / animation
        "_ZN7android38register_android_database_CursorWindowEP7_JNIEnv",
        "_ZN7android38register_android_database_SQLiteGlobalEP7_JNIEnv",
        "_ZN7android37register_android_database_SQLiteDebugEP7_JNIEnv",
        "_ZN7android42register_android_database_SQLiteConnectionEP7_JNIEnv",
        "_ZN7android47register_android_animation_PropertyValuesHolderEP7_JNIEnv",
    };
    using RegFn = jint (*)(JNIEnv*);
    int ok = 0, unavailable = 0;
    for (const char* sym : kReg) {
      auto fn = reinterpret_cast<RegFn>(::dlsym(h, sym));
      if (fn == nullptr) { ++unavailable; continue; }
      const jint rc = fn(env);
      if (env->ExceptionCheck()) { env->ExceptionClear(); ++unavailable; continue; }
      if (rc == JNI_OK) ++ok; else ++unavailable;
    }
    log::i(kTag,
           "ART native floor: {} framework native group(s) from libandroid_runtime "
           "({} unavailable)",
           ok, unavailable);
    populate_system_properties(env);
    load_system_resources(env);
    register_no_binder(env);
    bootstrap_host_env(env);
    load_acl_graphics(env);
    // Seed default typefaces now that the graphics Font/Typeface natives are registered
    // (the system font map isn't pushed to a standalone run).
    if (jclass boot = env->FindClass("org/acl/internal/HostBootstrap"); boot != nullptr) {
      if (jmethodID m = env->GetStaticMethodID(boot, "seedTypefaces", "()V"))
        env->CallStaticVoidMethod(boot, m);
      if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
      env->DeleteLocalRef(boot);
    }
#if ACL_HAVE_SDBUS
    // Bind android.net.ConnectivityManager (acl clean-room shim on the boot classpath)
    // to the svc/net host connectivity backend (systemd-networkd/NetworkManager/ConnMan).
    acl::net::register_connectivity_natives(env);
    // Bind org.acl.internal.NotifyBridge (INotificationManager translation) to the
    // svc/notify host backend (org.freedesktop.Notifications).
    acl::notify::register_notify_natives(env);
    // Bind org.acl.internal.BatteryBridge (ACTION_BATTERY_CHANGED extras) to the svc/battery
    // host power backend (org.freedesktop.UPower).
    acl::battery::register_battery_natives(env);
    // Bind org.acl.internal.BluetoothBridge (android.bluetooth.* shims) to the svc/bluetooth
    // host BlueZ backend (org.bluez).
    acl::bt::register_bt_natives(env);
    // Bind org.acl.internal.StorageBridge (StorageManager/IStorageManager.getVolumeList) to the
    // svc/storage host UDisks2 backend (removable media).
    acl::storage::register_storage_natives(env);
#endif
#if ACL_HAVE_PIPEWIRE
    // Bind org.acl.internal.AudioBridge (AudioManager master volume) to the host PipeWire
    // default sink.
    acl::audio::register_audio_natives(env);
#endif
#if ACL_HAVE_VULKAN && ACL_HAVE_WCS
    // Bind org.acl.internal.PresentDriver's native seam (the Looper-driven present, approach A).
    register_present_driver_natives(env);
#endif
  }

  // Run acl's host-ART runtime bootstrap (org.acl.internal.HostBootstrap.init) if injected on
  // the classpath: prepare the main Looper (any Activity/Handler needs it) and seed a
  // no-Binder ServiceManager. Runs on EVERY host-ART boot, before app code, so the normal
  // launcher-Activity flow (not just the view-host demo) gets the environment.
  void bootstrap_host_env(JNIEnv* env) {
    jclass boot = env->FindClass("org/acl/internal/HostBootstrap");
    if (boot == nullptr) { env->ExceptionClear(); return; }
    if (jmethodID init = env->GetStaticMethodID(boot, "init", "()V"))
      env->CallStaticVoidMethod(boot, init);
    if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionDescribe(); env->ExceptionClear(); }
    env->DeleteLocalRef(boot);
    log::i(kTag, "host bootstrap: main Looper + no-Binder ServiceManager ready");
  }

  // acl has no Binder (ADR-3): stub the ServiceManager entry point so the framework's
  // Binder path degrades gracefully instead of crashing with UnsatisfiedLinkError.
  // BinderInternal.getContextObject() → null makes ServiceManager.getService() return
  // null, and framework callers (ViewConfiguration, WindowManagerGlobal, …) fall back to
  // their no-service defaults. The Java-visible Manager APIs are served by svc/, not Binder.
  void register_no_binder(JNIEnv* env) {
    if (jclass c = env->FindClass("com/android/internal/os/BinderInternal")) {
      JNINativeMethod m[] = {
          {const_cast<char*>("getContextObject"), const_cast<char*>("()Landroid/os/IBinder;"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jobject { return nullptr; })},
      };
      if (env->RegisterNatives(c, m, 1) != JNI_OK) env->ExceptionClear();
      env->DeleteLocalRef(c);
    } else {
      env->ExceptionClear();
    }
    // android.os.Binder identity statics: no Binder, so a single fixed in-process identity.
    if (jclass c = env->FindClass("android/os/Binder")) {
      JNINativeMethod m[] = {
          {const_cast<char*>("getCallingUid"), const_cast<char*>("()I"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jint { return 1000; })},
          {const_cast<char*>("getCallingPid"), const_cast<char*>("()I"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jint { return 0; })},
          {const_cast<char*>("clearCallingIdentity"), const_cast<char*>("()J"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jlong { return 0; })},
          {const_cast<char*>("restoreCallingIdentity"), const_cast<char*>("(J)V"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jlong) {})},
          // Binder object construction (getNativeBBinderHolder) → a non-null sentinel (the
          // NativeAllocationRegistry rejects null). A Binder with no native backing: framework
          // client stubs (e.g. IAccessibilityManagerClient) construct fine; they only fail if
          // actually transacted, which has no service to reach anyway. The no-op finalizer
          // below never frees the shared sentinel.
          {const_cast<char*>("getNativeBBinderHolder"), const_cast<char*>("()J"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jlong {
             static int holder = 0;
             return reinterpret_cast<jlong>(&holder);
           })},
          // The NativeAllocationRegistry finalizer for a Binder holder → a no-op free (the
          // holder is 0, so there is nothing to free).
          {const_cast<char*>("getNativeFinalizer"), const_cast<char*>("()J"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jlong {
             return reinterpret_cast<jlong>(static_cast<void (*)(void*)>(+[](void*) {}));
           })},
          // Called from MessageQueue.next on background looper threads (no Binder to flush).
          {const_cast<char*>("flushPendingCommands"), const_cast<char*>("()V"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) {})},
          // StrictMode thread policy rides in the Binder thread-local (device-side it maps to
          // the driver). No Binder → keep it in a process-local slot so set/get round-trips.
          {const_cast<char*>("setThreadStrictModePolicy"), const_cast<char*>("(I)V"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jint policy) { g_strict_mode_policy = policy; })},
          {const_cast<char*>("getThreadStrictModePolicy"), const_cast<char*>("()I"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass) -> jint { return g_strict_mode_policy; })},
      };
      if (env->RegisterNatives(c, m, sizeof(m) / sizeof(m[0])) != JNI_OK) env->ExceptionClear();
      env->DeleteLocalRef(c);
    } else {
      env->ExceptionClear();
    }
    // android.os.Process thread ops touch device-only natives on background HandlerThreads
    // (e.g. SharedPreferences' queued-work looper). No-op them so those threads don't crash.
    if (jclass c = env->FindClass("android/os/Process")) {
      JNINativeMethod m[] = {
          {const_cast<char*>("setThreadPriority"), const_cast<char*>("(I)V"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jint) {})},
          {const_cast<char*>("setThreadPriority"), const_cast<char*>("(II)V"),
           reinterpret_cast<void*>(+[](JNIEnv*, jclass, jint, jint) {})},
      };
      if (env->RegisterNatives(c, m, 2) != JNI_OK) env->ExceptionClear();
      env->DeleteLocalRef(c);
    } else {
      env->ExceptionClear();
    }
    log::i(kTag, "ART native floor: Binder/BinderInternal/Process stubbed (no Binder, ADR-3)");
  }

  // P2.4/V3 (render): load acl's own android.graphics natives (libacl_graphics_jni, built
  // by Soong against Android's host libskia) and register them. Unlike the host
  // libandroid_runtime graphics umbrella (whose layoutlib build compiles @CriticalNative
  // natives with the wrong (env,clazz) signature → ABI crash), acl's natives use the DEVICE
  // convention, matching the framework's @CriticalNative that host ART honors. So real
  // android.graphics.Bitmap/Canvas/Paint execute on our (Android's) Skia. Opt-in via
  // ACL_ART_GRAPHICS_LIB (path to libacl_graphics_jni.so); best-effort.
  void load_acl_graphics(JNIEnv* env) {
    const char* lib = std::getenv("ACL_ART_GRAPHICS_LIB");
    if (lib == nullptr || *lib == '\0') return;
    void* h = ::dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (h == nullptr) {
      const char* e = ::dlerror();
      log::w(kTag, "acl graphics: dlopen({}) failed: {}", lib, e ? e : "?");
      return;
    }
    auto reg = reinterpret_cast<jint (*)(JNIEnv*)>(::dlsym(h, "register_acl_graphics"));
    if (reg == nullptr) {
      log::w(kTag, "acl graphics: register_acl_graphics not found in {}", lib);
      return;
    }
    const jint rc = reg(env);
    if (env->ExceptionCheck() || rc != JNI_OK) {
      env->ExceptionClear();
      log::w(kTag, "acl graphics: registration failed (rc={})", rc);
      return;
    }
    log::i(kTag,
           "acl graphics: android.graphics natives registered on Android host Skia "
           "(Bitmap/Canvas/Paint)");
    graphics_ops_check(env);
    graphics_present_demo(env);
    graphics_view_demo(env);
  }

  // App path (GPU): host a REAL android.view.View and present it GPU-native. The app
  // provides ONLY a View subclass (ACL_ART_VIEW_HOST) — like a real app's custom View. acl's
  // runtime bootstrap (org.acl.internal.HostBootstrap) supplies the framework environment
  // (main Looper, no-Binder ServiceManager, minimal Context); acl constructs the View,
  // measures/lays it out, and drives it through the Ganesh Vulkan present loop (begin_frame →
  // View.draw on the GPU Canvas → end_frame → tiled dma-buf → Wayland). No software path.
  void graphics_view_demo([[maybe_unused]] JNIEnv* env) {
#if ACL_HAVE_VULKAN && ACL_HAVE_WCS
    const char* host = std::getenv("ACL_ART_VIEW_HOST");
    if (host == nullptr || *host == '\0') return;
    constexpr int W = 480, H = 320;

    // Runtime bootstrap: main Looper + no-Binder ServiceManager + a Context factory.
    jclass boot = env->FindClass("org/acl/internal/HostBootstrap");
    if (boot == nullptr) {
      env->ExceptionClear();
      log::w(kTag, "view host: HostBootstrap not on classpath (ACL_ART_HOST_BOOTSTRAP?)");
      return;
    }
    env->CallStaticVoidMethod(boot, env->GetStaticMethodID(boot, "init", "()V"));
    jobject ctx = env->CallStaticObjectMethod(
        boot, env->GetStaticMethodID(boot, "newContext", "()Landroid/content/Context;"));
    if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionDescribe(); env->ExceptionClear(); }

    jclass vcls = env->FindClass(to_jni_name(host).c_str());
    if (vcls == nullptr) { env->ExceptionClear(); log::w(kTag, "view host: {} not found", host); env->DeleteLocalRef(boot); return; }

    // Activity probe: real apps enter via an Activity (onCreate → setContentView), not a
    // directly-constructible View. Map the first wall of the Activity/Window frontier.
    if (jclass actC = env->FindClass("android/app/Activity")) {
      const bool is_activity = env->IsAssignableFrom(vcls, actC) == JNI_TRUE;
      env->DeleteLocalRef(actC);
      if (is_activity) {
        log::i(kTag, "view host: {} is an Activity — attach + onCreate", host);
        jobject act = env->NewObject(vcls, env->GetMethodID(vcls, "<init>", "()V"));
        // Lightweight attach (HostBootstrap: mActivityInfo + PhoneWindow), then onCreate.
        env->CallStaticVoidMethod(
            boot,
            env->GetStaticMethodID(boot, "attachActivity",
                                   "(Landroid/app/Activity;Landroid/content/Context;)V"),
            act, ctx);
        if (act != nullptr && env->ExceptionCheck() != JNI_TRUE) {
          env->CallVoidMethod(act, env->GetMethodID(vcls, "onCreate", "(Landroid/os/Bundle;)V"),
                              static_cast<jobject>(nullptr));
        }
        if (env->ExceptionCheck() == JNI_TRUE) {
          env->ExceptionDescribe();
          env->ExceptionClear();
          log::w(kTag, "view host: Activity attach/onCreate hit a wall (Window/attach frontier; "
                       "docs/app-path-scope.md)");
          if (act != nullptr) env->DeleteLocalRef(act);
          env->DeleteLocalRef(vcls);
          env->DeleteLocalRef(boot);
          return;
        }
        // Content view = the Window's decor view (root of the installed hierarchy).
        jclass actClass = env->GetObjectClass(act);
        jobject window = env->CallObjectMethod(
            act, env->GetMethodID(actClass, "getWindow", "()Landroid/view/Window;"));
        jobject decor = window != nullptr
            ? env->CallObjectMethod(window, env->GetMethodID(env->GetObjectClass(window),
                                    "getDecorView", "()Landroid/view/View;"))
            : nullptr;
        if (decor == nullptr || env->ExceptionCheck() == JNI_TRUE) {
          if (env->ExceptionCheck()) env->ExceptionDescribe();
          env->ExceptionClear();
          log::w(kTag, "view host: Activity has no decor view to present");
          env->DeleteLocalRef(act);
          env->DeleteLocalRef(vcls);
          env->DeleteLocalRef(boot);
          return;
        }
        jclass dcls = env->GetObjectClass(decor);
        constexpr jint kExactlyA = 1 << 30;
        env->CallVoidMethod(decor, env->GetMethodID(dcls, "measure", "(II)V"),
                            static_cast<jint>((W & 0x3FFFFFFF) | kExactlyA),
                            static_cast<jint>((H & 0x3FFFFFFF) | kExactlyA));
        env->CallVoidMethod(decor, env->GetMethodID(dcls, "layout", "(IIII)V"), 0, 0, W, H);
        if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionDescribe(); env->ExceptionClear(); }
        log::i(kTag, "view host: Activity {} content view ready; presenting GPU-native", host);
        present_content_loop_gpu(decor, W, H);
        env->DeleteLocalRef(act);
        env->DeleteLocalRef(vcls);
        env->DeleteLocalRef(boot);
        return;
      }
    } else {
      env->ExceptionClear();
    }

    jmethodID vctor = vcls ? env->GetMethodID(vcls, "<init>", "(Landroid/content/Context;)V")
                           : nullptr;
    jobject view = vctor ? env->NewObject(vcls, vctor, ctx) : nullptr;
    if (view == nullptr || env->ExceptionCheck() == JNI_TRUE) {
      if (env->ExceptionCheck()) env->ExceptionDescribe();
      env->ExceptionClear();
      log::w(kTag, "view host: could not construct View {}", host);
      env->DeleteLocalRef(boot);
      return;
    }
    // measure(EXACTLY W, EXACTLY H) + layout(0,0,W,H) — MeasureSpec.EXACTLY == 1<<30.
    constexpr jint kExactly = 1 << 30;
    jmethodID measure = env->GetMethodID(vcls, "measure", "(II)V");
    jmethodID layout = env->GetMethodID(vcls, "layout", "(IIII)V");
    env->CallVoidMethod(view, measure, static_cast<jint>((W & 0x3FFFFFFF) | kExactly),
                        static_cast<jint>((H & 0x3FFFFFFF) | kExactly));
    env->CallVoidMethod(view, layout, 0, 0, W, H);
    if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionDescribe(); env->ExceptionClear(); }

    log::i(kTag, "view host: {} constructed + laid out; presenting GPU-native", host);
    present_content_loop_gpu(view, W, H);
    env->DeleteLocalRef(view);
    env->DeleteLocalRef(vcls);
    env->DeleteLocalRef(boot);
#endif
  }

  // Exercise the Canvas transform/clip + geometry natives on a REAL android.graphics.Canvas
  // (Android host Skia) and verify with pixel readback. Gated by ACL_ART_GRAPHICS_OPS.
  // Each new native is called (so a missing registration surfaces as UnsatisfiedLinkError)
  // and a few pixels are asserted (fill circle, stroke, translate+clip).
  void graphics_ops_check([[maybe_unused]] JNIEnv* env) {
    const char* on = std::getenv("ACL_ART_GRAPHICS_OPS");
    if (on == nullptr || *on == '\0') return;
    constexpr int W = 64, H = 64;
    const auto jfail = [&](const char* what) {
      if (env->ExceptionCheck()) env->ExceptionDescribe();
      env->ExceptionClear();
      log::w(kTag, "graphics ops: {} failed", what);
    };
    jclass bitmapC = env->FindClass("android/graphics/Bitmap");
    jclass configC = env->FindClass("android/graphics/Bitmap$Config");
    jclass canvasC = env->FindClass("android/graphics/Canvas");
    jclass paintC = env->FindClass("android/graphics/Paint");
    if (bitmapC == nullptr || configC == nullptr || canvasC == nullptr || paintC == nullptr)
      return jfail("FindClass");
    jobject argb = env->GetStaticObjectField(
        configC, env->GetStaticFieldID(configC, "ARGB_8888", "Landroid/graphics/Bitmap$Config;"));
    jobject bmp = env->CallStaticObjectMethod(
        bitmapC,
        env->GetStaticMethodID(bitmapC, "createBitmap",
                               "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;"),
        W, H, argb);
    if (bmp == nullptr) return jfail("createBitmap");
    jobject canvas = env->NewObject(
        canvasC, env->GetMethodID(canvasC, "<init>", "(Landroid/graphics/Bitmap;)V"), bmp);
    if (canvas == nullptr) return jfail("new Canvas");

    const jmethodID drawColor = env->GetMethodID(canvasC, "drawColor", "(I)V");
    const jmethodID drawCircle = env->GetMethodID(canvasC, "drawCircle", "(FFFLandroid/graphics/Paint;)V");
    const jmethodID drawRect = env->GetMethodID(canvasC, "drawRect", "(FFFFLandroid/graphics/Paint;)V");
    const jmethodID save = env->GetMethodID(canvasC, "save", "()I");
    const jmethodID restore = env->GetMethodID(canvasC, "restore", "()V");
    const jmethodID translate = env->GetMethodID(canvasC, "translate", "(FF)V");
    const jmethodID clipRect = env->GetMethodID(canvasC, "clipRect", "(FFFF)Z");
    const jmethodID setColor = env->GetMethodID(paintC, "setColor", "(I)V");
    const jmethodID setStyle = env->GetMethodID(paintC, "setStyle", "(Landroid/graphics/Paint$Style;)V");
    if (drawCircle == nullptr || save == nullptr || translate == nullptr || clipRect == nullptr ||
        setStyle == nullptr)
      return jfail("GetMethodID (ops)");

    // Black background; a red FILL circle at the center; then translate+clip a green rect.
    env->CallVoidMethod(canvas, drawColor, static_cast<jint>(0xFF000000));
    jobject paint = env->NewObject(paintC, env->GetMethodID(paintC, "<init>", "()V"));
    jobject fillStyle = env->GetStaticObjectField(
        env->FindClass("android/graphics/Paint$Style"),
        env->GetStaticFieldID(env->FindClass("android/graphics/Paint$Style"), "FILL",
                              "Landroid/graphics/Paint$Style;"));
    env->CallVoidMethod(paint, setStyle, fillStyle);
    env->CallVoidMethod(paint, setColor, static_cast<jint>(0xFFFF0000));  // red
    env->CallVoidMethod(canvas, drawCircle, 32.0f, 32.0f, 16.0f, paint);

    env->CallIntMethod(canvas, save);
    env->CallVoidMethod(canvas, translate, 4.0f, 4.0f);
    env->CallBooleanMethod(canvas, clipRect, 0.0f, 0.0f, 8.0f, 8.0f);  // device (4,4)-(12,12)
    env->CallVoidMethod(paint, setColor, static_cast<jint>(0xFF00FF00));  // green
    env->CallVoidMethod(canvas, drawRect, -100.0f, -100.0f, 100.0f, 100.0f, paint);  // clipped
    env->CallVoidMethod(canvas, restore);
    if (env->ExceptionCheck()) return jfail("draw ops");

    // Text: white "A" in the clear top-right region (y<16 is above the circle) — verifies
    // the SkFont/drawText path (nSetTextSize + nDrawText).
    const jmethodID setTextSize = env->GetMethodID(paintC, "setTextSize", "(F)V");
    const jmethodID drawText = env->GetMethodID(
        canvasC, "drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V");
    bool text_ok_call = false;
    if (setTextSize != nullptr && drawText != nullptr) {
      env->CallVoidMethod(paint, setColor, static_cast<jint>(0xFFFFFFFF));  // white
      env->CallVoidMethod(paint, setTextSize, 18.0f);
      jstring s = env->NewStringUTF("A");
      env->CallVoidMethod(canvas, drawText, s, 44.0f, 14.0f, paint);  // baseline (44,14)
      env->DeleteLocalRef(s);
      text_ok_call = env->ExceptionCheck() == JNI_FALSE;
    }
    env->ExceptionClear();

    // drawBitmap: an 8×8 magenta bitmap at (2,40) — clear region left of the circle.
    bool bmp_ok_call = false;
    jobject src = env->CallStaticObjectMethod(
        bitmapC,
        env->GetStaticMethodID(bitmapC, "createBitmap",
                               "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;"),
        8, 8, argb);
    if (src != nullptr) {
      env->CallVoidMethod(src, env->GetMethodID(bitmapC, "eraseColor", "(I)V"),
                          static_cast<jint>(0xFFFF00FF));  // magenta
      env->CallVoidMethod(
          canvas,
          env->GetMethodID(canvasC, "drawBitmap",
                           "(Landroid/graphics/Bitmap;FFLandroid/graphics/Paint;)V"),
          src, 2.0f, 40.0f, static_cast<jobject>(nullptr));
      bmp_ok_call = env->ExceptionCheck() == JNI_FALSE;
      env->DeleteLocalRef(src);
    }
    env->ExceptionClear();

    // measureText / getTextBounds on "Ag".
    float advance = 0.0f;
    int bounds_w = 0;
    if (jmethodID measure = env->GetMethodID(paintC, "measureText", "(Ljava/lang/String;)F")) {
      jstring t = env->NewStringUTF("Ag");
      advance = env->CallFloatMethod(paint, measure, t);
      jclass rectC = env->FindClass("android/graphics/Rect");
      jobject rb = env->NewObject(rectC, env->GetMethodID(rectC, "<init>", "()V"));
      if (jmethodID gtb = env->GetMethodID(
              paintC, "getTextBounds", "(Ljava/lang/String;IILandroid/graphics/Rect;)V")) {
        env->CallVoidMethod(paint, gtb, t, 0, 2, rb);
        bounds_w = env->CallIntMethod(rb, env->GetMethodID(rectC, "width", "()I"));
      }
      env->DeleteLocalRef(t);
      env->DeleteLocalRef(rb);
      env->DeleteLocalRef(rectC);
    }
    env->ExceptionClear();

    // Path: a filled cyan triangle in the bottom-right (clear of the circle at x<48).
    bool path_ok_call = false;
    if (jclass pathC = env->FindClass("android/graphics/Path")) {
      jobject path = env->NewObject(pathC, env->GetMethodID(pathC, "<init>", "()V"));
      const jmethodID moveTo = env->GetMethodID(pathC, "moveTo", "(FF)V");
      const jmethodID lineTo = env->GetMethodID(pathC, "lineTo", "(FF)V");
      const jmethodID close = env->GetMethodID(pathC, "close", "()V");
      const jmethodID drawPath = env->GetMethodID(
          canvasC, "drawPath", "(Landroid/graphics/Path;Landroid/graphics/Paint;)V");
      if (path != nullptr && moveTo != nullptr && drawPath != nullptr) {
        env->CallVoidMethod(path, moveTo, 50.0f, 50.0f);
        env->CallVoidMethod(path, lineTo, 62.0f, 50.0f);
        env->CallVoidMethod(path, lineTo, 56.0f, 62.0f);
        env->CallVoidMethod(path, close);
        env->CallVoidMethod(paint, setColor, static_cast<jint>(0xFF00FFFF));  // cyan
        env->CallVoidMethod(canvas, drawPath, path, paint);
        path_ok_call = env->ExceptionCheck() == JNI_FALSE;
      }
      if (path != nullptr) env->DeleteLocalRef(path);
      env->DeleteLocalRef(pathC);
    }
    env->ExceptionClear();

    // Matrix: concat a 2×-scale + translate (via setValues) and draw a rect through it —
    // rect(0,0,3,3) → device (2,52)-(8,58), a clear region below the bitmap and circle.
    bool matrix_ok_call = false;
    if (jclass matrixC = env->FindClass("android/graphics/Matrix")) {
      jobject m = env->NewObject(matrixC, env->GetMethodID(matrixC, "<init>", "()V"));
      const jmethodID setValues = env->GetMethodID(matrixC, "setValues", "([F)V");
      const jmethodID concat =
          env->GetMethodID(canvasC, "concat", "(Landroid/graphics/Matrix;)V");
      if (m != nullptr && setValues != nullptr && concat != nullptr) {
        const jfloat vals[9] = {2, 0, 2, 0, 2, 52, 0, 0, 1};  // scale 2, translate (2,52)
        jfloatArray arr = env->NewFloatArray(9);
        env->SetFloatArrayRegion(arr, 0, 9, vals);
        env->CallVoidMethod(m, setValues, arr);
        env->CallIntMethod(canvas, save);
        env->CallVoidMethod(canvas, concat, m);
        env->CallVoidMethod(paint, setColor, static_cast<jint>(0xFFFFFF00));  // yellow
        env->CallVoidMethod(canvas, drawRect, 0.0f, 0.0f, 3.0f, 3.0f, paint);
        env->CallVoidMethod(canvas, restore);
        matrix_ok_call = env->ExceptionCheck() == JNI_FALSE;
        env->DeleteLocalRef(arr);
      }
      if (m != nullptr) env->DeleteLocalRef(m);
      env->DeleteLocalRef(matrixC);
    }
    env->ExceptionClear();

    // Gradient: a horizontal red→blue LinearGradient filling a rect in the left strip
    // (x0..14, y16..40) — clear of the circle (x≥16) and bitmap (y≥40).
    bool grad_ok_call = false;
    if (jclass lgC = env->FindClass("android/graphics/LinearGradient")) {
      jclass tmC = env->FindClass("android/graphics/Shader$TileMode");
      jobject clamp = env->GetStaticObjectField(
          tmC, env->GetStaticFieldID(tmC, "CLAMP", "Landroid/graphics/Shader$TileMode;"));
      jmethodID lgCtor =
          env->GetMethodID(lgC, "<init>", "(FFFFIILandroid/graphics/Shader$TileMode;)V");
      jmethodID setShader = env->GetMethodID(
          paintC, "setShader", "(Landroid/graphics/Shader;)Landroid/graphics/Shader;");
      if (lgCtor != nullptr && setShader != nullptr) {
        jobject grad = env->NewObject(lgC, lgCtor, 0.0f, 28.0f, 14.0f, 28.0f,
                                      static_cast<jint>(0xFFFF0000), static_cast<jint>(0xFF0000FF),
                                      clamp);
        if (grad != nullptr) {
          env->CallObjectMethod(paint, setShader, grad);
          env->CallVoidMethod(canvas, drawRect, 0.0f, 16.0f, 14.0f, 40.0f, paint);
          env->CallObjectMethod(paint, setShader, static_cast<jobject>(nullptr));  // clear
          grad_ok_call = env->ExceptionCheck() == JNI_FALSE;
          env->DeleteLocalRef(grad);
        }
      }
      env->DeleteLocalRef(tmC);
      env->DeleteLocalRef(lgC);
    }
    env->ExceptionClear();

    auto read = reinterpret_cast<bool (*)(long, std::uint32_t*, int, int)>(
        ::dlsym(RTLD_DEFAULT, "acl_read_bitmap"));
    if (read == nullptr) { log::w(kTag, "graphics ops: acl_read_bitmap missing"); return; }
    std::vector<std::uint32_t> px(static_cast<std::size_t>(W) * H);
    const jlong handle = env->CallLongMethod(
        bmp, env->GetMethodID(bitmapC, "getNativeInstance", "()J"));
    if (!read(handle, px.data(), W, H)) { log::w(kTag, "graphics ops: readback failed"); return; }
    const auto at = [&](int x, int y) { return px[static_cast<std::size_t>(y) * W + x]; };
    const bool center_red = at(32, 32) == 0xFFFF0000u;
    const bool bg_black = at(2, 2) == 0xFF000000u;         // outside circle + clip
    const bool clip_green = at(6, 6) == 0xFF00FF00u;       // inside translated clip (4,4)-(12,12)
    const bool clip_edge = at(2, 2) != 0xFF00FF00u;        // outside the clip stayed black
    // Count non-black pixels in the glyph region (x 40..64, y 0..14) — above the circle
    // (top y=16) and right of the clip (x<12), so any non-black = a rasterized glyph.
    int text_px = 0;
    for (int y = 0; y < 15; ++y)
      for (int x = 40; x < W; ++x)
        if (at(x, y) != 0xFF000000u) ++text_px;
    const bool text_ok = text_ok_call && text_px > 0;
    const bool bmp_ok = bmp_ok_call && at(4, 42) == 0xFFFF00FFu;  // magenta bitmap at (2,40)
    const bool measure_ok = advance > 0.0f && bounds_w > 0;
    const bool path_ok = path_ok_call && at(55, 53) == 0xFF00FFFFu;  // cyan inside the triangle
    const bool matrix_ok = matrix_ok_call && at(5, 55) == 0xFFFFFF00u;  // yellow in scaled rect
    // Gradient: left end reddish (R>B), right end bluish (B>R).
    const auto R = [](std::uint32_t p) { return (p >> 16) & 0xffu; };
    const auto B = [](std::uint32_t p) { return p & 0xffu; };
    const bool grad_ok = grad_ok_call && R(at(1, 28)) > B(at(1, 28)) && B(at(12, 28)) > R(at(12, 28));
    log::i(kTag,
           "graphics ops: circle={} clip={} text={} ({}px) bitmap={} measure={} path={} "
           "matrix={} gradient={} (l=0x{:08x} r=0x{:08x})",
           center_red, clip_green, text_ok, text_px, bmp_ok, measure_ok, path_ok, matrix_ok,
           grad_ok, at(1, 28), at(12, 28));
    log::i(kTag,
           "graphics ops: geometry + text + bitmap + measure + path + matrix + gradient {} "
           "(real Canvas, Android Skia)",
           (center_red && bg_black && clip_green && clip_edge && text_ok && bmp_ok && measure_ok &&
            path_ok && matrix_ok && grad_ok)
               ? "PASS"
               : "MISMATCH");
    env->DeleteLocalRef(canvas);
    env->DeleteLocalRef(bmp);
  }

  // S3b (P2.4): the full loop, as a live demo. Gated by ACL_ART_GRAPHICS_DEMO. Drives a
  // real android.graphics render via JNI (Bitmap/Canvas/Paint on Android's Skia), reads
  // the Bitmap's pixels (libacl_graphics_jni acl_read_bitmap) into a gfx::Surface, imports
  // them into a dma-buf (gfx VkRenderTarget), and presents to the Wayland compositor
  // (zwp_linux_dmabuf_v1). Closes app→android.graphics→Android Skia→dma-buf→Wayland.
  void graphics_present_demo([[maybe_unused]] JNIEnv* env) {
#if ACL_HAVE_VULKAN && ACL_HAVE_WCS
    const char* on = std::getenv("ACL_ART_GRAPHICS_DEMO");
    if (on == nullptr || *on == '\0') return;
    constexpr int W = 480, H = 320;
    constexpr jint kBg = 0xFF204060, kFg = 0xFFCC8844;

    struct AclDmabuf {
      int fd;
      std::uint32_t width, height, plane_count;
      std::uint32_t offsets[4], strides[4];
      std::uint64_t modifier;
      std::uint32_t drm_format;
    };
    // Present a once-rendered dma-buf frame N times (static demo scene).
    const auto present_frame = [&](const AclDmabuf& f, const char* title) -> bool {
      wm::DmabufRenderFn present = [&](const std::vector<std::uint64_t>&,
                                       gfx::DmabufFrame* out) -> bool {
        gfx::DmabufFrame fr{};
        fr.fd = f.fd;
        fr.width = f.width;
        fr.height = f.height;
        fr.plane_count = f.plane_count;
        for (std::uint32_t i = 0; i < f.plane_count && i < 4; ++i) {
          fr.planes[i].offset = f.offsets[i];
          fr.planes[i].stride = f.strides[i];
        }
        fr.modifier = f.modifier;
        fr.drm_format = f.drm_format;
        fr.modifier_tiled = f.modifier != 0;
        *out = fr;
        return true;
      };
      return wm::present_dmabuf(present, title, "com.acl.graphics", 3);
    };

    // S4b step 2: ANIMATED GPU loop — a real android.graphics.Canvas re-draws a moving
    // rect each vsync tick onto the Ganesh Vulkan surface, presented via the per-frame
    // dma-buf loop (begin_frame → drawColor/drawRect → end_frame → present). Proves the
    // continuous GPU present path (present_dmabuf_loop) end-to-end. Gated by
    // ACL_ART_GRAPHICS_GPU_LOOP; frame count from the value (default 60).
    if (const char* gl = std::getenv("ACL_ART_GRAPHICS_GPU_LOOP");
        gl != nullptr && *gl != '\0') {
      const std::uint64_t frames = std::strtoull(gl, nullptr, 10) > 1
                                       ? std::strtoull(gl, nullptr, 10) : 60;
      auto begin = reinterpret_cast<long (*)(int, int)>(
          ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_begin_frame"));
      auto finish = reinterpret_cast<bool (*)(int, AclDmabuf*)>(
          ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_end_frame"));
      if (begin == nullptr || finish == nullptr) {
        log::w(kTag, "graphics demo(gpu-loop): begin/end frame missing");
        return;
      }
      const jlong ch = begin(W, H);
      if (ch == 0) { log::w(kTag, "graphics demo(gpu-loop): begin_frame failed"); return; }
      jclass canvasC = env->FindClass("android/graphics/Canvas");
      jclass paintC = env->FindClass("android/graphics/Paint");
      if (canvasC == nullptr || paintC == nullptr) { env->ExceptionClear(); return; }
      jobject canvas = env->NewObject(
          canvasC, env->GetMethodID(canvasC, "<init>", "(J)V"), ch);
      jobject paint = env->NewObject(paintC, env->GetMethodID(paintC, "<init>", "()V"));
      if (canvas == nullptr || paint == nullptr) { env->ExceptionClear(); return; }
      const jmethodID drawColor = env->GetMethodID(canvasC, "drawColor", "(I)V");
      const jmethodID setColor = env->GetMethodID(paintC, "setColor", "(I)V");
      const jmethodID drawRect =
          env->GetMethodID(canvasC, "drawRect", "(FFFFLandroid/graphics/Paint;)V");
      env->CallVoidMethod(paint, setColor, kFg);
      const wm::DmabufFrameFn render = [&](int bufidx, const std::vector<std::uint64_t>&,
                                           std::uint64_t, std::uint64_t idx,
                                           gfx::DmabufFrame* out) -> bool {
        if (idx > 0 && begin(W, H) == 0) return false;  // re-arm + clear
        // A rect sweeping horizontally so successive frames visibly differ.
        const float x = 20.0f + static_cast<float>((idx * 8) % 360);
        env->CallVoidMethod(canvas, drawColor, kBg);
        env->CallVoidMethod(canvas, drawRect, x, 60.0f, x + 100.0f, 260.0f, paint);
        if (env->ExceptionCheck() == JNI_TRUE) { env->ExceptionClear(); return false; }
        AclDmabuf f{};
        if (!finish(bufidx, &f) || f.fd < 0) return false;
        gfx::DmabufFrame fr{};
        fr.fd = f.fd;
        fr.width = f.width;
        fr.height = f.height;
        fr.plane_count = f.plane_count;
        for (std::uint32_t i = 0; i < f.plane_count && i < 4; ++i) {
          fr.planes[i].offset = f.offsets[i];
          fr.planes[i].stride = f.strides[i];
        }
        fr.modifier = f.modifier;
        fr.drm_format = f.drm_format;
        fr.modifier_tiled = f.modifier != 0;
        *out = fr;
        return true;
      };
      const wm::InputCallback no_input;
      const bool ok = wm::present_dmabuf_loop(W, H, "acl graphics (GPU loop)",
                                              "com.acl.graphics", render, no_input, false,
                                              frames);
      log::i(kTag, "graphics demo(gpu-loop): {} animated frame(s), real Canvas → Ganesh "
                   "Vulkan → dma-buf → Wayland {} (zero-copy)", frames,
             ok ? "presented" : "not presented");
      return;
    }

    // S4b: drive the app's REAL android.graphics.Canvas onto the GPU surface (not a
    // hardcoded C++ scene). Real Canvas ops → Android Skia Ganesh Vulkan → tiled dma-buf →
    // Wayland, zero-copy. Gated by ACL_ART_GRAPHICS_GPU_CANVAS.
    if (const char* gc = std::getenv("ACL_ART_GRAPHICS_GPU_CANVAS");
        gc != nullptr && *gc != '\0') {
      auto begin = reinterpret_cast<long (*)(int, int)>(
          ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_begin_frame"));
      auto finish = reinterpret_cast<bool (*)(int, AclDmabuf*)>(
          ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_end_frame"));
      if (begin == nullptr || finish == nullptr) {
        log::w(kTag, "graphics demo(gpu-canvas): begin/end frame missing");
        return;
      }
      const auto jfail2 = [&](const char* what) {
        if (env->ExceptionCheck()) env->ExceptionDescribe();
        env->ExceptionClear();
        log::w(kTag, "graphics demo(gpu-canvas): {} failed", what);
      };
      const jlong ch = begin(W, H);
      if (ch == 0) {
        log::w(kTag, "graphics demo(gpu-canvas): begin_frame failed");
        return;
      }
      jclass canvasC = env->FindClass("android/graphics/Canvas");
      jclass paintC = env->FindClass("android/graphics/Paint");
      if (canvasC == nullptr || paintC == nullptr) return jfail2("FindClass");
      jobject canvas = env->NewObject(
          canvasC, env->GetMethodID(canvasC, "<init>", "(J)V"), ch);
      if (canvas == nullptr) return jfail2("new Canvas(long)");
      env->CallVoidMethod(canvas, env->GetMethodID(canvasC, "drawColor", "(I)V"), kBg);
      jobject paint = env->NewObject(paintC, env->GetMethodID(paintC, "<init>", "()V"));
      env->CallVoidMethod(paint, env->GetMethodID(paintC, "setColor", "(I)V"), kFg);
      env->CallVoidMethod(
          canvas, env->GetMethodID(canvasC, "drawRect", "(FFFFLandroid/graphics/Paint;)V"),
          80.0f, 60.0f, 400.0f, 260.0f, paint);
      if (env->ExceptionCheck()) return jfail2("draw");
      AclDmabuf f{};
      if (!finish(0, &f) || f.fd < 0) {
        log::w(kTag, "graphics demo(gpu-canvas): end_frame failed");
        return;
      }
      log::i(kTag, "graphics demo(gpu-canvas): real android.graphics.Canvas → Ganesh Vulkan "
                   "→ dma-buf (modifier=0x{:x}, {} plane(s))", f.modifier, f.plane_count);
      const bool ok = present_frame(f, "acl graphics (GPU Canvas)");
      log::i(kTag, "graphics demo(gpu-canvas): real Canvas → Ganesh Vulkan → dma-buf → "
                   "Wayland {} (zero-copy)", ok ? "presented" : "not presented");
      return;
    }

    // S4a: GPU-native zero-copy path. libacl_graphics_jni renders the scene with Android's
    // Skia Ganesh Vulkan STRAIGHT into a modifier-tiled dma-buf VkImage (no CPU upload);
    // acl presents the exported fd. Gated by ACL_ART_GRAPHICS_GPU.
    if (const char* gpu = std::getenv("ACL_ART_GRAPHICS_GPU"); gpu != nullptr && *gpu != '\0') {
      auto render = reinterpret_cast<bool (*)(int, int, std::uint32_t, int, int, int, int,
                                              std::uint32_t, AclDmabuf*)>(
          ::dlsym(RTLD_DEFAULT, "acl_graphics_gpu_render"));
      if (render == nullptr) {
        log::w(kTag, "graphics demo(gpu): acl_graphics_gpu_render missing");
        return;
      }
      AclDmabuf f{};
      if (!render(W, H, static_cast<std::uint32_t>(kBg), 80, 60, 400, 260,
                  static_cast<std::uint32_t>(kFg), &f) || f.fd < 0) {
        log::w(kTag, "graphics demo(gpu): GPU render failed");
        return;
      }
      log::i(kTag, "graphics demo(gpu): Android Skia Ganesh Vulkan rendered into dma-buf "
                   "(modifier=0x{:x}, {} plane(s))", f.modifier, f.plane_count);
      const bool ok = present_frame(f, "acl graphics (GPU)");
      log::i(kTag, "graphics demo(gpu): android.graphics Skia → Ganesh Vulkan → dma-buf → "
                   "Wayland {} (zero-copy)", ok ? "presented" : "not presented");
      return;
    }

    const auto jfail = [&](const char* what) {
      if (env->ExceptionCheck()) env->ExceptionDescribe();
      env->ExceptionClear();
      log::w(kTag, "graphics demo: {} failed", what);
    };

    // ── Drive a real android.graphics render ────────────────────────────────
    jclass bitmapC = env->FindClass("android/graphics/Bitmap");
    jclass configC = env->FindClass("android/graphics/Bitmap$Config");
    jclass canvasC = env->FindClass("android/graphics/Canvas");
    jclass paintC = env->FindClass("android/graphics/Paint");
    if (bitmapC == nullptr || configC == nullptr || canvasC == nullptr || paintC == nullptr)
      return jfail("FindClass");
    jfieldID argbF = env->GetStaticFieldID(configC, "ARGB_8888",
                                           "Landroid/graphics/Bitmap$Config;");
    jobject argb = env->GetStaticObjectField(configC, argbF);
    jmethodID createBitmap = env->GetStaticMethodID(
        bitmapC, "createBitmap",
        "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    jobject bitmap = env->CallStaticObjectMethod(bitmapC, createBitmap, W, H, argb);
    if (bitmap == nullptr) return jfail("createBitmap");
    jobject canvas = env->NewObject(canvasC, env->GetMethodID(canvasC, "<init>",
                                    "(Landroid/graphics/Bitmap;)V"), bitmap);
    if (canvas == nullptr) return jfail("new Canvas");
    env->CallVoidMethod(canvas, env->GetMethodID(canvasC, "drawColor", "(I)V"), kBg);
    jobject paint = env->NewObject(paintC, env->GetMethodID(paintC, "<init>", "()V"));
    env->CallVoidMethod(paint, env->GetMethodID(paintC, "setColor", "(I)V"), kFg);
    env->CallVoidMethod(canvas,
                        env->GetMethodID(canvasC, "drawRect", "(FFFFLandroid/graphics/Paint;)V"),
                        80.0f, 60.0f, 400.0f, 260.0f, paint);
    if (env->ExceptionCheck()) return jfail("draw");

    // ── Read the rendered pixels → gfx::Surface ─────────────────────────────
    const jlong handle = env->CallLongMethod(
        bitmap, env->GetMethodID(bitmapC, "getNativeInstance", "()J"));
    auto read = reinterpret_cast<bool (*)(long, std::uint32_t*, int, int)>(
        ::dlsym(RTLD_DEFAULT, "acl_read_bitmap"));
    if (read == nullptr) { log::w(kTag, "graphics demo: acl_read_bitmap missing"); return; }
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(W) * H);
    if (!read(handle, pixels.data(), W, H)) {
      log::w(kTag, "graphics demo: acl_read_bitmap failed");
      return;
    }
    gfx::Surface surf(W, H);
    surf.blit(pixels.data());

    // ── Import to dma-buf + present to the compositor ───────────────────────
    std::string err;
    auto rt = gfx::VkRenderTarget::Create(W, H, &err);
    if (!rt) { log::w(kTag, "graphics demo: no Vulkan render target: {}", err); return; }
    wm::DmabufRenderFn render = [&](const std::vector<std::uint64_t>&,
                                    gfx::DmabufFrame* out) {
      return rt->import_pixels_to_dmabuf(surf, out, &err);
    };
    const bool ok = wm::present_dmabuf(render, "acl graphics", "com.acl.graphics", 3);
    log::i(kTag, "graphics demo: app→android.graphics→Android Skia→dma-buf→Wayland {}",
           ok ? "presented" : "did not present (no compositor?)");
#endif
  }

  // Pre-load the framework's system assets (framework-res.apk = the android:R.* resources)
  // so Resources.getSystem() resolves real framework resources. AssetManager hardcodes
  // FRAMEWORK_APK_PATH=/system/framework/framework-res.apk with no override, but its
  // createSystemAssetsInZygoteLocked(reinit, path) is public and sets sSystem — so acl
  // pre-populates from a real path (ACL_ART_FRAMEWORK_RES) before getSystem() runs.
  void load_system_resources(JNIEnv* env) {
    const char* res = std::getenv("ACL_ART_FRAMEWORK_RES");
    if (res == nullptr || *res == '\0') return;
    jclass am = env->FindClass("android/content/res/AssetManager");
    if (am == nullptr) { env->ExceptionClear(); return; }
    jmethodID create = env->GetStaticMethodID(
        am, "createSystemAssetsInZygoteLocked", "(ZLjava/lang/String;)V");
    if (create == nullptr) { env->ExceptionClear(); return; }
    jstring path = env->NewStringUTF(res);
    env->CallStaticVoidMethod(am, create, JNI_FALSE, path);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      log::w(kTag, "ART framework resources: failed to load {}", res);
    } else {
      log::i(kTag, "ART framework resources: loaded {}", res);
    }
    env->DeleteLocalRef(path);
    env->DeleteLocalRef(am);
  }

  // Populate a minimal build.prop before the app's framework classes initialize. Many
  // framework static initializers read system properties and crash on empty values
  // (e.g. android.os.Build.<clinit> indexes ro.product.cpu.abilist* — a length-0 array
  // without these). On host, properties are an in-process map that SystemProperties.set
  // writes, so acl seeds the essential set. Done after the SystemProperties native is
  // registered above; values are placeholders, not a claim of a real device.
  void populate_system_properties(JNIEnv* env) {
    jclass sp = env->FindClass("android/os/SystemProperties");
    if (sp == nullptr) { env->ExceptionClear(); return; }
    jmethodID set = env->GetStaticMethodID(
        sp, "set", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (set == nullptr) { env->ExceptionClear(); return; }
    struct Prop { const char* k; const char* v; };
    static const Prop kProps[] = {
        {"ro.product.cpu.abilist", "x86_64,x86"}, {"ro.product.cpu.abilist64", "x86_64"},
        {"ro.product.cpu.abilist32", "x86"},
        {"ro.build.version.sdk", "33"}, {"ro.build.version.release", "13"},
        {"ro.build.version.codename", "REL"}, {"ro.build.version.incremental", "acl"},
        {"ro.build.version.all_codenames", "REL"}, {"ro.build.version.known_codenames", "REL"},
        {"ro.build.version.preview_sdk", "0"}, {"ro.build.version.preview_sdk_fingerprint", "REL"},
        {"ro.build.version.min_supported_target_sdk", "0"}, {"ro.build.version.security_patch", "2022-01-01"},
        {"ro.build.id", "acl"}, {"ro.build.display.id", "acl"}, {"ro.build.type", "eng"},
        {"ro.build.tags", "test-keys"}, {"ro.build.flavor", "acl-eng"},
        {"ro.build.date.utc", "0"}, {"ro.build.host", "acl"}, {"ro.build.user", "acl"},
        {"ro.build.fingerprint", "acl/acl/acl:13/acl/acl:eng/test-keys"},
        {"ro.product.name", "acl"}, {"ro.product.device", "acl"},
        {"ro.product.board", "acl"}, {"ro.product.brand", "acl"},
        {"ro.product.model", "acl"}, {"ro.product.manufacturer", "acl"},
        {"ro.hardware", "acl"}, {"ro.bootloader", "acl"}, {"ro.secure", "0"},
        {"ro.debuggable", "1"}, {"ro.dalvik.vm.native.bridge", "0"},
    };
    int n = 0;
    for (const Prop& p : kProps) {
      jstring k = env->NewStringUTF(p.k);
      jstring v = env->NewStringUTF(p.v);
      env->CallStaticVoidMethod(sp, set, k, v);
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      } else {
        ++n;
      }
      env->DeleteLocalRef(k);
      env->DeleteLocalRef(v);
    }
    env->DeleteLocalRef(sp);
    log::i(kTag, "ART build.prop: seeded {} system propert{}", n, n == 1 ? "y" : "ies");
  }
};

}  // namespace

std::unique_ptr<Vm> make_jvm_vm(VmConfig config) {
  return std::make_unique<JvmVm>(std::move(config));
}

std::unique_ptr<Vm> make_art_vm(VmConfig config) {
  return std::make_unique<ArtVm>(std::move(config));
}

}  // namespace acl::runtime
