// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl design; mechanism proven in tools/ndk-loader-spike/ (docs/ndk-scope.md
// Stage 0b). Bootstrap contract from AOSP bionic linker source (Apache-2.0). libhybris (Apache-2.0)
// referenced for the two-libc %fs technique, not vendored. No GPL-3.0 sources.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "acl_config.h"
#if ACL_HAVE_JNI
#include <jni.h>
#include <type_traits>
#endif

namespace acl::ndk {

// True if the ELF at `path` is bionic-linked (DT_NEEDED "libc.so", vs glibc's "libc.so.6"). Used to
// route app native libs (bionic → BionicRuntime) apart from acl's own glibc supplemental libs.
bool is_bionic_elf(const char* path);

// Phase 3 surface binding: the runtime (which owns the GL session — acl's wl_display-bound GlDevice
// + the app's wl_surface) supplies these so the bionic EGL forwarders can back the app's NATIVE EGL
// (Flutter/Impeller) with acl's device, exactly as the Java GLES path (egl10) does. Each callback runs
// under the GL donor's glibc %fs (a valid glibc TCB) — keep them lock-light and TLS-free where they
// may also be reached from bionic %fs (window_size). All fields optional; unset => forwarders pass the
// slot straight to host Mesa (which opens its own Wayland connection — wrong for windowing, fine for
// the headless probe).
struct NativeGlHooks {
  void* (*get_display)(void* user) = nullptr;                    // -> EGLDisplay (acl's, waits for ready)
  void* (*create_window_surface)(void* user, void* egl_config) = nullptr;  // -> EGLSurface on wl_surface
  void (*window_size)(void* user, int* w, int* h) = nullptr;     // the session's current pixel size
  // Translate the engine's eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID, ahb) into a dma-buf EGLImage
  // (host Mesa can't consume the Android target). Set by the runtime to gfx::egl_image_from_ahb;
  // `ahb` is the AHardwareBuffer*, `real` is host Mesa's eglCreateImageKHR. Runs under a donor %fs.
  void* (*egl_image_from_ahb)(void* dpy, const void* ahb, const int* attribs, void* real) = nullptr;
  void* user = nullptr;
};
// Register the hooks (idempotent; last wins). Called by the runtime at EGL setup. Also wires the
// window-size callback into a loaded libandroid.so so ANativeWindow_getWidth/Height report real sizes.
void set_native_gl_hooks(const NativeGlHooks& hooks);

// Install the asset provider backing libandroid's NDK AAssetManager. `open_cb` inflates a whole
// asset (named relative to the APK's assets/ dir) into a freshly-allocated buffer, returning 1 and
// filling *out_buf/*out_len on hit or 0 on miss; `free_cb` releases such a buffer. Both run acl-side
// (glibc) code — the runtime restores the caller's glibc %fs around them. Called once by the runtime
// after the app APK is known, before the app's native code opens any asset.
// Install the host AHardwareBuffer describe/acquire/release ops (gfx::ahb_*). The loader points
// libandroid's AHardwareBuffer_{describe,acquire,release} at %fs-aware trampolines that call these,
// so libacl_ndk keeps no gfx link dependency (set from the runtime, which links gfx). release frees,
// so its trampoline restores glibc %fs; describe/acquire are TLS-free.
void set_ahb_ops(void (*describe)(const void* ahb, void* desc), void (*acquire)(void* ahb),
                 void (*release)(void* ahb));

// ANativeWindow producer over the ImageReader BufferQueue (Flutter external textures; ultimately
// MediaCodec setVideoSurface). A native producer resolves the producer Surface via
// ANativeWindow_fromSurface -> `from_native_obj`(Surface.mNativeObject), then cycles a frame with
// lock()/unlock_post(): lock dequeues a free pooled buffer and CPU-maps it (fills *out); unlock_post
// queues it (fires onImageAvailable so the engine composites). Host ops set by the runtime
// (image_reader_jni, which owns the ReaderCtx/BufferQueue); libandroid's ANativeWindow_* call
// %fs-aware trampolines that restore glibc %fs (the ops touch gfx/gbm + the allocator).
// AnwBuffer mirrors the ANativeWindow_Buffer fields a producer writes.
struct AnwBuffer {
  void* bits;          // mapped base pointer
  std::int32_t width;
  std::int32_t height;
  std::int32_t stride;  // in PIXELS (ANativeWindow_Buffer.stride convention)
  std::int32_t format;  // WINDOW_FORMAT_* (1 = RGBA_8888)
};
void set_anw_ops(void* (*from_native_obj)(void* native_obj),
                 int (*lock)(void* anw, AnwBuffer* out), int (*unlock_post)(void* anw));

void set_asset_provider(int (*open_cb)(void* user, const char* name, void** out_buf, long* out_len),
                        void (*free_cb)(void* user, void* buf), void* user);

// Bridge the calling thread's NDK ALooper into a Java Looper. `main_looper_bridge_fd` returns an fd
// (the ALooper's epoll set) that becomes readable whenever a task/timer registered on the ALooper is
// ready; watch it from acl's Java MessageQueue and call `main_looper_bridge_pump` to dispatch. This
// lets acl's main Looper.loop() service NDK-ALooper users on that thread — notably Flutter's merged
// platform+UI task runner, whose warm-up-frame timer otherwise never fires. Call on the target
// (main) thread, after that thread's ALooper exists. Returns -1 if unavailable.
int main_looper_bridge_fd();
void main_looper_bridge_pump();

#if ACL_HAVE_JNI
// The JNI function-table struct types, derived from the `functions` member so this is portable
// across jni.h variants (OpenJDK names them JNINativeInterface_/JNIInvokeInterface_; Android drops
// the trailing underscore). Same ABI layout either way.
using JniEnvTable = std::remove_const_t<std::remove_pointer_t<decltype(JNIEnv::functions)>>;
using JniVmTable = std::remove_const_t<std::remove_pointer_t<decltype(JavaVM::functions)>>;
#endif
}  // namespace acl::ndk

namespace acl::ndk {

// Where the bionic runtime lives on the host: the AOSP dynamic linker, a directory of bionic
// runtime libs (libc/libm/libdl/liblog/libc++/ld-android.so), and a minimal bionic PIE exe the
// linker "loads" during bootstrap (its entry never runs). See docs/ndk-scope.md.
struct BionicConfig {
  std::string linker;                        // absolute path to linker64
  std::string sysroot;                       // dir of bionic runtime libs (LD_LIBRARY_PATH root)
  std::string stub;                          // absolute path to a minimal bionic PIE exe
  std::vector<std::string> extra_lib_paths;  // app native-lib dir(s), appended to LD_LIBRARY_PATH

  // Populated from ACL_BIONIC_LINKER / ACL_BIONIC_SYSROOT / ACL_BIONIC_STUB.
  static BionicConfig from_env();
  // True iff linker/sysroot/stub are set, absolute, and exist. `why` gets the first problem.
  bool valid(std::string* why) const;
};

// The bionic linker bootstrapped in-process. Process-singleton: __linker_init can run only once,
// and a bad bootstrap input makes the linker abort the process, so inputs are validated first.
//
// Threading: bootstrap sets up the calling (main) thread's bionic TCB. Every call that runs bionic
// code (dlopen/dlsym/loaded app functions) must hold the thread pointer as bionic — use RunScope.
// Calling bionic code from other host threads is Rung 4 (attach-as-bionic); not yet supported.
class BionicRuntime {
 public:
  // Bootstrap (or return the already-bootstrapped singleton). Returns nullptr and sets `error` if
  // the config is invalid, the arch is unsupported, or the bootstrap fails. Idempotent.
  static BionicRuntime* instance(const BionicConfig& cfg, std::string* error);
  // The bootstrapped singleton, or nullptr if instance() has not succeeded.
  static BionicRuntime* get() noexcept;

  // dlopen a bionic .so (absolute path required; pre-validated). Returns a bionic handle or nullptr
  // + sets `error` (dlopen failure is graceful, not fatal). Runs with the thread pointer as bionic.
  // global=true adds the library to bionic's global group (RTLD_GLOBAL), so its symbols are
  // visible to dlsym(dlopen(nullptr)) — needed for e.g. Chromium's AHardwareBuffer compat shim,
  // which resolves AHardwareBuffer_* out of the main executable's global namespace.
  void* load(const char* abs_path, std::string* error, bool global = false);
  // dlsym in a loaded handle. Runs with the thread pointer as bionic.
  void* symbol(void* handle, const char* name);

  // Give the CALLING thread its own bionic TCB, so it may run bionic code. Returns the thread's
  // bionic thread pointer (idempotent — returns the existing one if already attached), or 0 +
  // sets `error`.
  //
  // A bionic TCB belongs to one thread: it holds errno, pthread_self, the pthread TLS keys and the
  // DTV. Bionic creates one per thread it clones, but ART's threads are created by glibc, and
  // bionic exports nothing that adopts a running thread (pthread_create always clones a new one,
  // and its TCB builders are hidden), so acl builds the TCB the way bionic does — see
  // docs/ndk-scope.md, and src/ndk/bionic_runtime.cpp for the steps. The internal layout comes
  // from the generated bionic_offsets.h; when that is unavailable this fails rather than guessing.
  //
  // Must be called ON the thread being attached (it reads that thread's tid and stack).
  std::uintptr_t attach_current_thread(std::string* error);

  // This thread's bionic thread pointer, or 0 if it was never attached. What the down-call
  // trampolines read to find the right TCB.
  static std::uintptr_t current_thread_fs() noexcept;

  // The bootstrapping thread's bionic thread pointer (the TCB __linker_init built) and its
  // pthread_internal_t. Exposed so a test can assert an attached thread got its OWN of each:
  // sharing either is the bug attach exists to prevent.
  std::uintptr_t bootstrap_fs() const noexcept { return bionic_fs_; }
  void* bootstrap_thread_struct() const noexcept;

  // Bidirectional-boundary self-test (docs/ndk-scope.md Rung 4). Calls a loaded bionic function
  // `int f(int(*cb)(int), int)` under bionic %fs, passing an internal up-call trampoline that
  // restores glibc %fs around `cb` — proving a bionic .so can call back into glibc (JNI) and
  // return with both sides' TLS intact. Returns f's result, or -1 on unsupported arch.
  // (Main-thread only for now — uses the glibc %fs captured at bootstrap.)
  int run_boundary_selftest(void* bionic_fn, int (*cb)(int), int x);

#if ACL_HAVE_JNI
  // Wrap a real (glibc/ART) JavaVM so a bionic .so's JNI_OnLoad can drive it across the %fs
  // boundary: the returned JavaVM's GetEnv hands back a wrapped JNIEnv whose entry points are
  // %fs-restoring thunks over `real_env`'s. `real_env` is the ART JNIEnv for this thread. Returns
  // nullptr on unsupported arch. NOTE: only the slots a JNI_OnLoad/RegisterNatives path needs are
  // thunked so far (GetEnv/GetVersion/FindClass/RegisterNatives); the rest are copied through
  // unwrapped and will misbehave if called under bionic %fs — the full table is the next step.
  JavaVM* wrap_java_vm(JavaVM* real_vm, JNIEnv* real_env);
  // The wrapped JNIEnv produced by the last wrap_java_vm() — what a loaded bionic native should be
  // handed so its own JNI up-calls trampoline. Null before wrap_java_vm() succeeds.
  JNIEnv* wrapped_env() const { return wrap_env_; }

  // The operation the ART Runtime.nativeLoad hook calls: dlopen the app's bionic .so in-process,
  // and if it exports JNI_OnLoad, wrap ART's real JavaVM and drive JNI_OnLoad across the %fs
  // boundary (its RegisterNatives'd methods get down-call trampolines so ART can invoke them).
  // Returns the bionic handle, or nullptr + sets `error`. `real_vm`/`real_env` are ART's.
  void* load_library(const char* abs_path, JavaVM* real_vm, JNIEnv* real_env, std::string* error);

  // Set the class loader that the wrapped FindClass falls back to when the plain (system) lookup
  // misses. A bionic lib's JNI_OnLoad does FindClass on app classes (e.g. FlutterJNI) that live in
  // the app's class loader, not the boot loader FindClass searches; pass Runtime.nativeLoad's
  // ClassLoader arg here (before load_library) so those resolve, the way ART's own
  // LoadNativeLibrary installs a class-loader override around JNI_OnLoad. `real_env` is ART's.
  void set_app_class_loader(JNIEnv* real_env, jobject loader);

  // dlsym `sym` in `handle` and wrap it in a down-call trampoline ART can invoke directly. The raw
  // symbol must never be called by ART: it is bionic code expecting bionic %fs + the wrapped
  // JNIEnv, which is exactly what the trampoline substitutes. Null if the symbol is absent (the
  // normal "this native isn't exported by name" case) or on unsupported arch. load_library must
  // have run first (the trampoline needs the wrapped env). Used to bind natives by NAME —
  // JNI_OnLoad's RegisterNatives path gets its trampolines inside the wrapped RegisterNatives.
  void* downcall_symbol(void* handle, const char* sym);

  // True if a JNI_OnLoad already RegisterNatives'd (name, sig) on `klass`. Those bindings are the
  // app's explicit choice, so name resolution must not overwrite them (a .so may both export
  // Java_pkg_Class_m and register a *different* implementation for m).
  bool was_registered(JNIEnv* env, jclass klass, const char* name, const char* sig) const;
#endif

  // RAII: set the thread pointer to bionic for the scope's lifetime, restore glibc on exit. Wrap
  // any direct call into loaded app code in one of these. The swap itself is out-of-line in the
  // -fno-stack-protector TU (a mid-function %fs change would false-fail the caller's canary).
  class RunScope {
   public:
    explicit RunScope(BionicRuntime& rt);
    ~RunScope();
    RunScope(const RunScope&) = delete;
    RunScope& operator=(const RunScope&) = delete;

   private:
    std::uintptr_t saved_;
  };

 private:
  BionicRuntime() = default;
  std::uintptr_t bionic_fs_ = 0;
  std::uintptr_t glibc_fs_ = 0;  // the bootstrapping (main) thread's glibc thread pointer
#if ACL_HAVE_JNI
  JNIEnv* wrap_env_ = nullptr;
#endif
  void* dlopen_fn_ = nullptr;
  void* dlsym_fn_ = nullptr;
  void* dlerror_fn_ = nullptr;
  // The linker's __loader_shared_globals: libc's static TLS layout + module table, which
  // attach_current_thread reads to lay out a new thread's block. Null if the linker lacks it.
  void* shared_globals_fn_ = nullptr;
};

}  // namespace acl::ndk
