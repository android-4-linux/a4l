// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl. API surface + semantics from the public Android NDK docs and AOSP's
// public headers (frameworks/native/include/android/looper.h, Apache-2.0) — the contract only, no
// AOSP implementation copied. Behavior cross-checked against a real Flutter engine's use of it
// (docs/ndk-scope.md). No GPL-3.0 sources.
//
// acl's libandroid.so — TARGET code: this is cross-compiled for bionic and staged into the bionic
// sysroot by tools/stage-bionic-runtime.sh, NOT built by meson into the host binary. An app's
// native code (Flutter's libflutter.so, React Native's libfbjni.so, …) has DT_NEEDED on
// libandroid.so, so without it those libraries cannot even load — the loader is only ever handed a
// library whose every symbol resolves (RTLD_NOW).
//
// This first slice implements the ALooper family for real and declares the rest of what a real
// engine links, so the missing pieces announce themselves by name at the moment they are called
// rather than failing as a mystery.
//
// Why ALooper needs no bridge: it is epoll over real fds. bionic's own libc has epoll, eventfd and
// pthreads, so this runs entirely on the bionic side of the boundary — no %fs swap, no callback
// into acl, nothing to get wrong. Flutter's engine threads each run their own ALooper over real
// fds (docs/ndk-scope.md), and that is exactly what this serves. The MAIN thread's looper is a
// different question: it must eventually be acl's own reactor, so an app's main-thread callbacks
// and acl's Wayland/input fds share one epoll. Until that lands, a main-thread ALooper here is a
// private loop that acl does not pump.

#include <errno.h>
#include <fcntl.h>       // fcntl F_ADD_SEALS / F_SEAL_* (ASharedMemory_setProt)
#include <pthread.h>
#include <sys/mman.h>    // memfd_create, PROT_* (ASharedMemory_create/setProt)
#include <sys/stat.h>    // fstat (ASharedMemory_getSize)
#include <sys/timerfd.h> // timerfd_create/settime (AChoreographer frame clock)
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/types.h>  // off64_t (AAsset_getLength's return type)
#include <time.h>       // clock_gettime (ASurfaceTransactionStats_getLatchTime)
#include <unistd.h>

#include <android/log.h>
#include <android/hardware_buffer.h>       // AHardwareBuffer + AHardwareBuffer_Desc (NDK contract)
#include <android/hardware_buffer_jni.h>   // AHardwareBuffer_{from,to}HardwareBuffer
#include <jni.h>     // AHardwareBuffer_fromHardwareBuffer reads the Java HardwareBuffer's nativePtr
#include <stdio.h>   // snprintf
#include <string.h>  // strlen
#include <sys/uio.h>  // writev
#include <netdb.h>    // getaddrinfo / struct addrinfo (android_getaddrinfofornetwork)

#define LOG_TAG "acl.libandroid"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// ── native log routing ────────────────────────────────────────────────────────────────────────
// A native app's logs — libflutter's engine messages, Dart print(), the debug VM-service URL, any
// liblog user — go through bionic liblog, whose default logd sink acl doesn't run, so they vanish.
// Route them into acl's log stream (stderr, the same fd acl's own log writes to) in a recognizable
// "<level>/<tag>: <message>" form. acl calls acl_ndk_log_init() explicitly when the first bionic app
// lib loads (a .init_array constructor here would NOT run under acl's in-process bionic loader).
static void acl_ndk_logger(const struct __android_log_message* m) {
  if (m == NULL || m->message == NULL) return;
  char lvl;
  switch (m->priority) {
    case ANDROID_LOG_VERBOSE: lvl = 'V'; break;
    case ANDROID_LOG_DEBUG: lvl = 'D'; break;
    case ANDROID_LOG_WARN: lvl = 'W'; break;
    case ANDROID_LOG_ERROR: lvl = 'E'; break;
    case ANDROID_LOG_FATAL: lvl = 'F'; break;
    default: lvl = 'I'; break;
  }
  char hdr[128];
  int hn = snprintf(hdr, sizeof hdr, "%c/%s: ", lvl, m->tag != NULL ? m->tag : "");
  const size_t mlen = strlen(m->message);
  const int nl = (mlen == 0 || m->message[mlen - 1] != '\n');
  struct iovec iov[3] = {
      {hdr, hn > 0 ? (size_t)hn : 0},
      {(void*)m->message, mlen},
      {(void*)"\n", (size_t)nl},
  };
  (void)!writev(2, iov, 3);
}
// The Flutter engine gates its AHardwareBuffer external-texture import on
// android_get_device_api_level() >= 26/29. bionic's returns -1 here (the __system_property area is
// uninitialized and unwritable in acl's sandbox), silently disabling the WHOLE external-texture
// path. acl vendors API 33, so export our own — libandroid precedes libc in the engine's DT_NEEDED,
// so the engine binds THESE — reporting 33. See [[flutter-texture-bridge]] M3c.
__attribute__((visibility("default"))) int android_get_device_api_level(void) { return 33; }
__attribute__((visibility("default"))) int android_get_application_target_sdk_version(void) { return 33; }

__attribute__((visibility("default"))) void acl_ndk_log_init(void) {
  __android_log_set_logger(acl_ndk_logger);
}

// ── the public contract (android/looper.h) ───────────────────────────────────────────────────
enum {
  ALOOPER_PREPARE_ALLOW_NON_CALLBACKS = 1 << 0,
};
enum {
  ALOOPER_POLL_WAKE = -1,
  ALOOPER_POLL_CALLBACK = -2,
  ALOOPER_POLL_TIMEOUT = -3,
  ALOOPER_POLL_ERROR = -4,
};
enum {
  ALOOPER_EVENT_INPUT = 1 << 0,
  ALOOPER_EVENT_OUTPUT = 1 << 1,
  ALOOPER_EVENT_ERROR = 1 << 2,
  ALOOPER_EVENT_HANGUP = 1 << 3,
  ALOOPER_EVENT_INVALID = 1 << 4,
};
typedef struct ALooper ALooper;
typedef int (*ALooper_callbackFunc)(int fd, int events, void* data);

#define ACL_EXPORT __attribute__((visibility("default")))

// ── ALooper ──────────────────────────────────────────────────────────────────────────────────
// One per thread, refcounted, owning an epoll set and a wake eventfd. Registrations are a flat
// array: a looper carries a handful of fds, so a linear scan beats a hash table and keeps the
// whole thing allocation-light on the poll path.
#define ACL_MAX_FDS 64

typedef struct {
  int fd;
  int ident;
  int events;
  ALooper_callbackFunc callback;
  void* data;
} AclFdEntry;

struct ALooper {
  int refs;            // ALooper_acquire/release; the owning thread holds one
  int epoll_fd;
  int wake_fd;         // eventfd, so ALooper_wake can break a poll from any thread
  int allow_non_callbacks;
  pthread_mutex_t lock;  // guards the table (addFd/removeFd may race pollOnce from other threads)
  AclFdEntry fds[ACL_MAX_FDS];
  int fd_count;
};

static pthread_key_t g_looper_key;
static pthread_once_t g_looper_key_once = PTHREAD_ONCE_INIT;

static void acl_looper_free(ALooper* l) {
  if (l == NULL) return;
  if (l->epoll_fd >= 0) close(l->epoll_fd);
  if (l->wake_fd >= 0) close(l->wake_fd);
  pthread_mutex_destroy(&l->lock);
  free(l);
}

// Runs when a thread that called ALooper_prepare exits: drop the reference that thread held.
static void acl_looper_thread_exit(void* p) {
  ALooper* l = (ALooper*)p;
  if (l == NULL) return;
  if (__atomic_sub_fetch(&l->refs, 1, __ATOMIC_ACQ_REL) <= 0) acl_looper_free(l);
}

static void acl_looper_key_init(void) { pthread_key_create(&g_looper_key, acl_looper_thread_exit); }

ACL_EXPORT ALooper* ALooper_forThread(void) {
  pthread_once(&g_looper_key_once, acl_looper_key_init);
  return (ALooper*)pthread_getspecific(g_looper_key);
}

ACL_EXPORT ALooper* ALooper_prepare(int opts) {
  pthread_once(&g_looper_key_once, acl_looper_key_init);
  ALooper* l = (ALooper*)pthread_getspecific(g_looper_key);
  if (l != NULL) return l;  // idempotent per thread, as on Android

  l = (ALooper*)calloc(1, sizeof(ALooper));
  if (l == NULL) return NULL;
  l->refs = 1;  // the thread's own reference; released by acl_looper_thread_exit
  l->allow_non_callbacks = (opts & ALOOPER_PREPARE_ALLOW_NON_CALLBACKS) != 0;
  pthread_mutex_init(&l->lock, NULL);
  l->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  l->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (l->epoll_fd < 0 || l->wake_fd < 0) {
    ALOGE("ALooper_prepare: epoll/eventfd failed (%s)", strerror(errno));
    acl_looper_free(l);
    return NULL;
  }
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN;
  ev.data.fd = l->wake_fd;
  if (epoll_ctl(l->epoll_fd, EPOLL_CTL_ADD, l->wake_fd, &ev) != 0) {
    ALOGE("ALooper_prepare: epoll_ctl(wake) failed (%s)", strerror(errno));
    acl_looper_free(l);
    return NULL;
  }
  pthread_setspecific(g_looper_key, l);
  return l;
}

ACL_EXPORT void ALooper_acquire(ALooper* looper) {
  if (looper != NULL) __atomic_add_fetch(&looper->refs, 1, __ATOMIC_ACQ_REL);
}

ACL_EXPORT void ALooper_release(ALooper* looper) {
  if (looper == NULL) return;
  if (__atomic_sub_fetch(&looper->refs, 1, __ATOMIC_ACQ_REL) <= 0) acl_looper_free(looper);
}

static int acl_to_epoll(int events) {
  int e = 0;
  if (events & ALOOPER_EVENT_INPUT) e |= EPOLLIN;
  if (events & ALOOPER_EVENT_OUTPUT) e |= EPOLLOUT;
  return e;  // ERROR/HANGUP are reported by the kernel regardless; they are not requestable
}

static int acl_from_epoll(uint32_t events) {
  int e = 0;
  if (events & EPOLLIN) e |= ALOOPER_EVENT_INPUT;
  if (events & EPOLLOUT) e |= ALOOPER_EVENT_OUTPUT;
  if (events & EPOLLERR) e |= ALOOPER_EVENT_ERROR;
  if (events & EPOLLHUP) e |= ALOOPER_EVENT_HANGUP;
  return e;
}

ACL_EXPORT int ALooper_addFd(ALooper* looper, int fd, int ident, int events,
                             ALooper_callbackFunc callback, void* data) {
  if (looper == NULL || fd < 0) return -1;
  // Android's contract: with no callback the caller must have allowed non-callbacks, and ident
  // must be >= 0 — that is how pollOnce can report the fd back to it.
  if (callback == NULL) {
    if (!looper->allow_non_callbacks || ident < 0) {
      ALOGW("ALooper_addFd(fd=%d): no callback and ident=%d without ALLOW_NON_CALLBACKS", fd, ident);
      return -1;
    }
  } else {
    ident = ALOOPER_POLL_CALLBACK;  // as documented: a callback registration reports as CALLBACK
  }

  pthread_mutex_lock(&looper->lock);
  int slot = -1;
  for (int i = 0; i < looper->fd_count; ++i)
    if (looper->fds[i].fd == fd) { slot = i; break; }  // re-adding an fd replaces it
  const int adding = slot < 0;
  if (adding) {
    if (looper->fd_count >= ACL_MAX_FDS) {
      pthread_mutex_unlock(&looper->lock);
      ALOGE("ALooper_addFd(fd=%d): more than %d fds on one looper", fd, ACL_MAX_FDS);
      return -1;
    }
    slot = looper->fd_count;
  }
  looper->fds[slot].fd = fd;
  looper->fds[slot].ident = ident;
  looper->fds[slot].events = events;
  looper->fds[slot].callback = callback;
  looper->fds[slot].data = data;

  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = acl_to_epoll(events);
  ev.data.fd = fd;
  const int op = adding ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
  if (epoll_ctl(looper->epoll_fd, op, fd, &ev) != 0) {
    pthread_mutex_unlock(&looper->lock);
    ALOGE("ALooper_addFd(fd=%d): epoll_ctl failed (%s)", fd, strerror(errno));
    return -1;
  }
  if (adding) looper->fd_count++;
  pthread_mutex_unlock(&looper->lock);
  return 1;
}

ACL_EXPORT int ALooper_removeFd(ALooper* looper, int fd) {
  if (looper == NULL || fd < 0) return -1;
  pthread_mutex_lock(&looper->lock);
  int slot = -1;
  for (int i = 0; i < looper->fd_count; ++i)
    if (looper->fds[i].fd == fd) { slot = i; break; }
  if (slot < 0) {
    pthread_mutex_unlock(&looper->lock);
    return 0;  // not registered: 0, not an error
  }
  epoll_ctl(looper->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
  looper->fds[slot] = looper->fds[looper->fd_count - 1];
  looper->fd_count--;
  pthread_mutex_unlock(&looper->lock);
  return 1;
}

ACL_EXPORT void ALooper_wake(ALooper* looper) {
  if (looper == NULL || looper->wake_fd < 0) return;
  const uint64_t one = 1;
  ssize_t n;
  do {
    n = write(looper->wake_fd, &one, sizeof(one));
  } while (n < 0 && errno == EINTR);
}

ACL_EXPORT int ALooper_pollOnce(int timeoutMillis, int* outFd, int* outEvents, void** outData) {
  ALooper* l = ALooper_forThread();
  if (l == NULL) return ALOOPER_POLL_ERROR;  // pollOnce is only meaningful on a prepared thread

  for (;;) {
    struct epoll_event evs[ACL_MAX_FDS + 1];
    int n;
    do {
      n = epoll_wait(l->epoll_fd, evs, ACL_MAX_FDS + 1, timeoutMillis);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
      ALOGE("ALooper_pollOnce: epoll_wait failed (%s)", strerror(errno));
      return ALOOPER_POLL_ERROR;
    }
    if (n == 0) return ALOOPER_POLL_TIMEOUT;

    int fired_callback = 0;
    for (int i = 0; i < n; ++i) {
      const int fd = evs[i].data.fd;
      if (fd == l->wake_fd) {
        uint64_t counter;
        while (read(l->wake_fd, &counter, sizeof(counter)) > 0) {  // drain; it is EFD_NONBLOCK
        }
        return ALOOPER_POLL_WAKE;
      }
      // Copy the registration out from under the lock: the callback may add/remove fds.
      pthread_mutex_lock(&l->lock);
      AclFdEntry e;
      memset(&e, 0, sizeof(e));
      int found = 0;
      for (int j = 0; j < l->fd_count; ++j)
        if (l->fds[j].fd == fd) { e = l->fds[j]; found = 1; break; }
      pthread_mutex_unlock(&l->lock);
      if (!found) continue;  // removed while we were polling

      const int events = acl_from_epoll(evs[i].events);
      if (e.callback != NULL) {
        if (e.callback(e.fd, events, e.data) == 0) ALooper_removeFd(l, e.fd);  // 0 = unregister
        fired_callback = 1;
        continue;
      }
      if (outFd != NULL) *outFd = e.fd;
      if (outEvents != NULL) *outEvents = events;
      if (outData != NULL) *outData = e.data;
      return e.ident;
    }
    if (fired_callback) return ALOOPER_POLL_CALLBACK;
    // Only fds we no longer know about fired: poll again rather than report a bogus wake.
  }
}

ACL_EXPORT int ALooper_pollAll(int timeoutMillis, int* outFd, int* outEvents, void** outData) {
  // Documented as pollOnce, minus the WAKE returns: keep polling until something the caller can
  // act on happens.
  for (;;) {
    const int r = ALooper_pollOnce(timeoutMillis, outFd, outEvents, outData);
    if (r != ALOOPER_POLL_CALLBACK && r != ALOOPER_POLL_WAKE) return r;
  }
}

// ── ALooper <-> Java Looper bridge ────────────────────────────────────────────────────────────
// On a real device the NDK ALooper and android.os.MessageQueue on a thread ARE the same native
// Looper, so an app that adds an fd via ALooper_addFd is serviced by the same loop the Java
// Handler runs on. acl reimplements the NDK ALooper here (epoll) separately from the Java
// MessageQueue's native looper, so the two diverge: on acl's main thread, code that posts to the
// NDK ALooper (notably Flutter's merged platform+UI task runner — its MessageLoopAndroid registers
// a timerfd via ALooper_addFd) is NOT serviced by acl's main Java Looper.loop(). Bridge them: the
// ALooper's epoll set is itself pollable via its epoll_fd (readable whenever any registered fd is
// ready), so acl's main Java Looper watches that fd (PresentDriver) and drains the ALooper when it
// signals. Without this, Dart's warm-up-frame timer never fires and no frame is ever drawn.
ACL_EXPORT int acl_looper_bridge_fd(void) {
  ALooper* l = ALooper_prepare(0);  // idempotent: returns this thread's existing looper if any
  if (l == NULL) return -1;
  return l->epoll_fd;
}
// Non-blocking drain of the current thread's ALooper: dispatch every currently-ready fd callback.
// One ALooper_pollOnce(0) handles a whole epoll batch; loop so a callback that immediately re-arms
// its fd (a zero-delay follow-up task) runs in this wake too, bounded so a busy animation can't
// starve the Java Looper's other fds — the epoll_fd stays readable and re-wakes us if work remains.
ACL_EXPORT void acl_looper_bridge_pump(void) {
  if (ALooper_forThread() == NULL) return;
  for (int i = 0; i < 64; ++i) {
    int outFd = 0, outEvents = 0;
    void* outData = NULL;
    if (ALooper_pollOnce(0, &outFd, &outEvents, &outData) != ALOOPER_POLL_CALLBACK) return;
  }
}

// ── AChoreographer (android/choreographer.h) ─────────────────────────────────────────────────
// The NDK Choreographer is a per-thread frame clock: a consumer (e.g. Chromium WebView's
// RenderThread, whose MessagePumpAndroid pumps this same ALooper) calls AChoreographer_getInstance()
// then posts frame/vsync callbacks that must fire ~once per display frame on that thread's ALooper.
// Without it Chromium logs "Unable to load function AChoreographer_postVsyncCallback" and its
// compositor has no vsync source, so it never produces frames (a WebView paints nothing). acl has
// no per-thread hardware vsync at this layer, so drive the callbacks from a 60 Hz timerfd registered
// on the thread's ALooper — enough to advance the consumer's compositor; acl's present loop then
// composites the frames it produces. Provenance: NDK android/choreographer.h public contract
// (developer.android.com, Apache-2.0). No GPL-3.0 sources.
typedef struct AChoreographer AChoreographer;
typedef struct AChoreographerFrameCallbackData AChoreographerFrameCallbackData;
typedef void (*AChoreographer_frameCallback)(long frameTimeNanos, void* data);
typedef void (*AChoreographer_frameCallback64)(int64_t frameTimeNanos, void* data);
typedef void (*AChoreographer_vsyncCallback)(const AChoreographerFrameCallbackData* cbData, void* data);

#define ACL_CHOREO_MAX_CBS 128
enum { ACL_CB_FRAME = 0, ACL_CB_FRAME64 = 1, ACL_CB_VSYNC = 2 };
typedef struct { int kind; void* fn; void* data; } AclFrameCb;

struct AChoreographer {
  ALooper* looper;
  int timer_fd;
  int armed;
  pthread_mutex_t lock;
  AclFrameCb pending[ACL_CHOREO_MAX_CBS];
  int pending_count;
  int64_t vsync_id;
};
// The data a vsync callback receives — one synthetic frame timeline (index 0).
struct AChoreographerFrameCallbackData {
  int64_t frame_time_ns;
  int64_t vsync_id;
  int64_t expected_present_ns;
  int64_t deadline_ns;
};

static pthread_key_t g_choreo_key;
static pthread_once_t g_choreo_once = PTHREAD_ONCE_INIT;

static int64_t acl_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void acl_choreo_free(void* p) {
  AChoreographer* c = (AChoreographer*)p;
  if (c == NULL) return;
  if (c->looper != NULL && c->timer_fd >= 0) ALooper_removeFd(c->looper, c->timer_fd);
  if (c->timer_fd >= 0) close(c->timer_fd);
  pthread_mutex_destroy(&c->lock);
  free(c);
}
static void acl_choreo_key_init(void) { pthread_key_create(&g_choreo_key, acl_choreo_free); }

static void acl_choreo_arm(AChoreographer* c, int64_t delay_ns) {
  if (delay_ns < 1000000LL) delay_ns = 1000000LL;  // >=1ms
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  its.it_value.tv_sec = delay_ns / 1000000000LL;
  its.it_value.tv_nsec = delay_ns % 1000000000LL;
  timerfd_settime(c->timer_fd, 0, &its, NULL);
  c->armed = 1;
}

// timerfd fired: drain it, snapshot the pending callbacks, invoke each with the current frame time.
static int acl_choreo_on_frame(int fd, int events, void* data) {
  (void)events;
  AChoreographer* c = (AChoreographer*)data;
  uint64_t exp;
  while (read(fd, &exp, sizeof(exp)) == (ssize_t)sizeof(exp)) { /* drain */ }
  AclFrameCb snap[ACL_CHOREO_MAX_CBS];
  int n;
  pthread_mutex_lock(&c->lock);
  n = c->pending_count;
  memcpy(snap, c->pending, (size_t)n * sizeof(AclFrameCb));
  c->pending_count = 0;
  c->armed = 0;
  pthread_mutex_unlock(&c->lock);
  const int64_t now = acl_now_ns();
  struct AChoreographerFrameCallbackData fcd = {
      now, ++c->vsync_id, now + 16666667LL, now + 16666667LL};
  // Invoke outside the lock — a callback re-posts (re-arms the timer) for the next frame.
  for (int i = 0; i < n; i++) {
    if (snap[i].fn == NULL) continue;
    if (snap[i].kind == ACL_CB_VSYNC)
      ((AChoreographer_vsyncCallback)snap[i].fn)(&fcd, snap[i].data);
    else if (snap[i].kind == ACL_CB_FRAME64)
      ((AChoreographer_frameCallback64)snap[i].fn)(now, snap[i].data);
    else
      ((AChoreographer_frameCallback)snap[i].fn)((long)now, snap[i].data);
  }
  return 1;  // keep the fd registered
}

ACL_EXPORT AChoreographer* AChoreographer_getInstance(void) {
  pthread_once(&g_choreo_once, acl_choreo_key_init);
  AChoreographer* c = (AChoreographer*)pthread_getspecific(g_choreo_key);
  if (c != NULL) return c;
  ALooper* looper = ALooper_forThread();
  if (looper == NULL) {
    ALOGE("AChoreographer_getInstance: no ALooper on this thread");
    return NULL;
  }
  c = (AChoreographer*)calloc(1, sizeof(*c));
  if (c == NULL) return NULL;
  c->looper = looper;
  c->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  pthread_mutex_init(&c->lock, NULL);
  if (c->timer_fd < 0 ||
      ALooper_addFd(looper, c->timer_fd, 0, ALOOPER_EVENT_INPUT, acl_choreo_on_frame, c) != 1) {
    ALOGE("AChoreographer_getInstance: timerfd/addFd failed (%s)", strerror(errno));
    acl_choreo_free(c);
    return NULL;
  }
  pthread_setspecific(g_choreo_key, c);
  return c;
}

static void acl_choreo_post(AChoreographer* c, int kind, void* fn, void* data, int64_t delay_ns) {
  if (c == NULL || fn == NULL) return;
  pthread_mutex_lock(&c->lock);
  if (c->pending_count < ACL_CHOREO_MAX_CBS) {
    c->pending[c->pending_count].kind = kind;
    c->pending[c->pending_count].fn = fn;
    c->pending[c->pending_count].data = data;
    c->pending_count++;
  }
  if (!c->armed) acl_choreo_arm(c, delay_ns > 0 ? delay_ns : 16666667LL);
  pthread_mutex_unlock(&c->lock);
}

ACL_EXPORT void AChoreographer_postFrameCallback(AChoreographer* c, AChoreographer_frameCallback cb,
                                                 void* data) {
  acl_choreo_post(c, ACL_CB_FRAME, (void*)cb, data, 0);
}
ACL_EXPORT void AChoreographer_postFrameCallbackDelayed(AChoreographer* c,
                                                        AChoreographer_frameCallback cb, void* data,
                                                        long delayMillis) {
  acl_choreo_post(c, ACL_CB_FRAME, (void*)cb, data, (int64_t)delayMillis * 1000000LL);
}
ACL_EXPORT void AChoreographer_postFrameCallback64(AChoreographer* c,
                                                   AChoreographer_frameCallback64 cb, void* data) {
  acl_choreo_post(c, ACL_CB_FRAME64, (void*)cb, data, 0);
}
ACL_EXPORT void AChoreographer_postFrameCallbackDelayed64(AChoreographer* c,
                                                          AChoreographer_frameCallback64 cb,
                                                          void* data, uint32_t delayMillis) {
  acl_choreo_post(c, ACL_CB_FRAME64, (void*)cb, data, (int64_t)delayMillis * 1000000LL);
}
ACL_EXPORT void AChoreographer_postVsyncCallback(AChoreographer* c,
                                                 AChoreographer_vsyncCallback cb, void* data) {
  acl_choreo_post(c, ACL_CB_VSYNC, (void*)cb, data, 0);
}

// AChoreographerFrameCallbackData accessors — one synthetic timeline, so `index` is ignored.
ACL_EXPORT int64_t
AChoreographerFrameCallbackData_getFrameTimeNanos(const AChoreographerFrameCallbackData* d) {
  return d ? d->frame_time_ns : 0;
}
ACL_EXPORT size_t
AChoreographerFrameCallbackData_getFrameTimelinesLength(const AChoreographerFrameCallbackData* d) {
  (void)d;
  return 1;
}
ACL_EXPORT size_t AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(
    const AChoreographerFrameCallbackData* d) {
  (void)d;
  return 0;
}
ACL_EXPORT int64_t AChoreographerFrameCallbackData_getFrameTimelineVsyncId(
    const AChoreographerFrameCallbackData* d, size_t index) {
  (void)index;
  return d ? d->vsync_id : 0;
}
ACL_EXPORT int64_t AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(
    const AChoreographerFrameCallbackData* d, size_t index) {
  (void)index;
  return d ? d->expected_present_ns : 0;
}
ACL_EXPORT int64_t AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(
    const AChoreographerFrameCallbackData* d, size_t index) {
  (void)index;
  return d ? d->deadline_ns : 0;
}

// ── Not implemented yet ──────────────────────────────────────────────────────────────────────
// Declared so that a library linking them still loads (RTLD_NOW resolves every symbol), and so
// that the gap announces itself by name when an app actually calls it, instead of surfacing as a
// mystery crash. Each returns the API's documented failure value.
//
// ANativeWindow_* belongs to acl's wm (a surface is acl's, on the glibc side, so these must cross
// the %fs boundary); AAsset* belongs to acl's pkg (the APK is already open there). Both are the
// next slices — see docs/ndk-scope.md.
#define ACL_TODO(fn) ALOGE("%s is not implemented yet (acl libandroid slice; docs/ndk-scope.md)", fn)

typedef struct ANativeWindow ANativeWindow;
// The NDK ANativeWindow_Buffer ABI (android/native_window.h) — a producer writes `bits` after lock().
typedef struct ANativeWindow_Buffer {
  int32_t width;
  int32_t height;
  int32_t stride;
  int32_t format;
  void* bits;
  uint32_t reserved[6];
} ANativeWindow_Buffer;
// Matches acl::ndk::AnwBuffer (bionic_runtime.h) — what the lock hook fills for us to copy across.
typedef struct AclAnwBuf {
  void* bits;
  int32_t width;
  int32_t height;
  int32_t stride;
  int32_t format;
} AclAnwBuf;
typedef struct AAssetManager AAssetManager;
typedef struct AAsset AAsset;
typedef struct ARect ARect;
struct _JNIEnv;
struct _jobject;

// Phase 3 surface binding: acl backs the app's native EGL with its own GL device (the wl_surface is
// acl's, on the glibc side). So ANativeWindow_fromSurface need not build a real window — the forwarded
// eglCreateWindowSurface is overridden to make the surface on acl's session toplevel and ignores the
// ANativeWindow identity. We return one stable non-null token so the engine's surface bookkeeping
// proceeds; the size getters report acl's live session size via a callback acl installs after load
// (acl_anw_size_hook; captureless + TLS-free, so it is safe to call from here under bionic %fs).
static int g_anw_token;  // a stable, non-null ANativeWindow handle (identity only; the GL-window path)
// ANativeWindow producer over an acl ImageReader's BufferQueue. When a Surface belongs to an acl
// ImageReader (its mNativeObject is a live ReaderCtx), fromSurface returns that ctx as the window and
// lock/unlockAndPost cycle it. These call the runtime's %fs-aware trampolines (bionic_runtime), which
// resolve the Surface (host JNI), CPU-map a pooled buffer, and queue it. Set by the loader; a plain
// global (this lib is -fvisibility=hidden, so acl resolves them by name to install).
ACL_EXPORT void* acl_anw_from_hook = 0;         // void* (*)(void* surface) -> ctx or NULL
ACL_EXPORT void* acl_anw_lock_hook = 0;         // int (*)(void* anw, AclAnwBuf* out)
ACL_EXPORT void* acl_anw_unlock_post_hook = 0;  // int (*)(void* anw)
ACL_EXPORT void* acl_anw_size_hook = 0;  // void (*)(void* user, int* w, int* h), set by acl
ACL_EXPORT void* acl_anw_size_user = 0;
static void anw_size(int* w, int* h) {
  if (acl_anw_size_hook) {
    ((void (*)(void*, int*, int*))acl_anw_size_hook)(acl_anw_size_user, w, h);
  } else {
    if (w) *w = 0;
    if (h) *h = 0;
  }
}
ACL_EXPORT ANativeWindow* ANativeWindow_fromSurface(struct _JNIEnv* env, struct _jobject* surface) {
  (void)env;
  // If this Surface is an acl ImageReader producer (mNativeObject is a live ReaderCtx), back the
  // window with that reader's BufferQueue — the host hook reads mNativeObject (JNI) + validates. Any
  // other surface (the app's GL window) keeps the identity token: eglCreateWindowSurface is overridden
  // to acl's session toplevel and ignores the ANativeWindow identity.
  if (acl_anw_from_hook != 0 && surface != NULL) {
    void* anw = ((void* (*)(void*))acl_anw_from_hook)(surface);
    if (anw != NULL) return (ANativeWindow*)anw;
  }
  return (ANativeWindow*)&g_anw_token;
}
ACL_EXPORT void ANativeWindow_acquire(ANativeWindow* w) { (void)w; }  // refcount is a no-op (see release)
ACL_EXPORT void ANativeWindow_release(ANativeWindow* w) { (void)w; }
ACL_EXPORT int32_t ANativeWindow_getFormat(ANativeWindow* w) {
  (void)w;
  return 1;  // WINDOW_FORMAT_RGBA_8888
}
ACL_EXPORT int32_t ANativeWindow_getWidth(ANativeWindow* w) {
  (void)w;
  int ww = 0, hh = 0;
  anw_size(&ww, &hh);
  return ww;
}
ACL_EXPORT int32_t ANativeWindow_getHeight(ANativeWindow* w) {
  (void)w;
  int ww = 0, hh = 0;
  anw_size(&ww, &hh);
  return hh;
}
ACL_EXPORT int32_t ANativeWindow_lock(ANativeWindow* w, ANativeWindow_Buffer* b, ARect* dirty) {
  (void)dirty;
  if (w == NULL || b == NULL) return -EINVAL;
  if (w == (ANativeWindow*)&g_anw_token || acl_anw_lock_hook == 0) {
    ACL_TODO("ANativeWindow_lock (non-ImageReader window)");  // GL-window path has no CPU producer
    return -EINVAL;
  }
  AclAnwBuf ab;
  memset(&ab, 0, sizeof(ab));
  int rc = ((int (*)(void*, void*))acl_anw_lock_hook)(w, &ab);  // dequeue + CPU-map the next buffer
  if (rc != 0) return rc;
  b->width = ab.width;
  b->height = ab.height;
  b->stride = ab.stride;
  b->format = ab.format;
  b->bits = ab.bits;
  return 0;
}
ACL_EXPORT int32_t ANativeWindow_unlockAndPost(ANativeWindow* w) {
  if (w == NULL) return -EINVAL;
  if (w == (ANativeWindow*)&g_anw_token || acl_anw_unlock_post_hook == 0) {
    ACL_TODO("ANativeWindow_unlockAndPost (non-ImageReader window)");
    return -EINVAL;
  }
  return ((int (*)(void*))acl_anw_unlock_post_hook)(w);  // queue -> onImageAvailable -> engine composites
}

// ── AAssetManager (NDK) ──────────────────────────────────────────────────────────────────────
// Flutter's engine reads its Dart kernel + snapshots + flutter_assets through the NDK
// AAssetManager (AAssetManager_open + AAsset_getBuffer/getLength). Those live in the APK, and the
// APK — with the zlib inflate needed for its DEFLATE'd asset entries — is acl's pkg, on the glibc
// side of the %fs boundary. So the read crosses: acl_asset_open_hook reads+inflates a whole asset
// into a buffer on acl's side (the hook does the glibc %fs swap itself), and libandroid then serves
// getBuffer/getLength/read from that buffer with plain, %fs-agnostic memory access. acl_asset_free_
// hook releases it (acl's allocator owns it). Both hooks are installed by acl right after this lib
// loads (bionic_runtime.cpp), mirroring acl_anw_size_hook.
//
//   acl_asset_open_hook: int (*)(void* user, const char* name, void** out_buf, long* out_len)
//                        -> 1 and fills *out_buf/*out_len on hit, 0 on miss. `name` is the asset
//                        path Android passes (relative to the APK's assets/ dir).
//   acl_asset_free_hook: void (*)(void* user, void* buf)  — frees a buffer from the open hook.
// Default visibility: acl resolves these by name (bionic dlsym) to install the provider, and this
// lib is built -fvisibility=hidden, so a plain global would be unreachable.
ACL_EXPORT void* acl_asset_open_hook = 0;
ACL_EXPORT void* acl_asset_free_hook = 0;
ACL_EXPORT void* acl_asset_user = 0;
static int g_am_token;  // a stable, non-null AAssetManager handle (identity only; one APK)

struct AAsset {
  char* data;    // acl-owned buffer holding the whole (decompressed) asset
  off64_t len;
  off64_t pos;
};

ACL_EXPORT AAssetManager* AAssetManager_fromJava(struct _JNIEnv* env, struct _jobject* am) {
  // acl has a single app APK; the Java AssetManager identity is irrelevant. Hand back one stable
  // non-null token so the engine's bookkeeping proceeds — open() routes to acl's pkg regardless.
  (void)env; (void)am;
  return (AAssetManager*)&g_am_token;
}
ACL_EXPORT AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename, int mode) {
  (void)mgr; (void)mode;
  if (acl_asset_open_hook == 0 || filename == NULL) { ACL_TODO("AAssetManager_open"); return NULL; }
  void* buf = NULL;
  long len = 0;
  const int ok = ((int (*)(void*, const char*, void**, long*))acl_asset_open_hook)(
      acl_asset_user, filename, &buf, &len);
  if (!ok || buf == NULL) return NULL;  // no such asset — a normal miss, not an error
  AAsset* a = (AAsset*)malloc(sizeof(AAsset));
  if (a == NULL) {
    if (acl_asset_free_hook != 0) ((void (*)(void*, void*))acl_asset_free_hook)(acl_asset_user, buf);
    return NULL;
  }
  a->data = (char*)buf;
  a->len = (off64_t)len;
  a->pos = 0;
  return a;
}
ACL_EXPORT void AAsset_close(AAsset* a) {
  if (a == NULL) return;
  if (a->data != NULL && acl_asset_free_hook != 0)
    ((void (*)(void*, void*))acl_asset_free_hook)(acl_asset_user, a->data);
  free(a);
}
ACL_EXPORT int AAsset_read(AAsset* a, void* buf, size_t count) {
  if (a == NULL || buf == NULL) return -1;
  off64_t remaining = a->len - a->pos;
  if (remaining <= 0) return 0;  // EOF
  size_t n = count < (size_t)remaining ? count : (size_t)remaining;
  memcpy(buf, a->data + a->pos, n);
  a->pos += (off64_t)n;
  return (int)n;
}
ACL_EXPORT off_t AAsset_seek(AAsset* a, off_t offset, int whence) {
  if (a == NULL) return (off_t)-1;
  off64_t base = whence == SEEK_CUR ? a->pos : (whence == SEEK_END ? a->len : 0);
  off64_t np = base + offset;
  if (np < 0 || np > a->len) return (off_t)-1;
  a->pos = np;
  return (off_t)a->pos;
}
ACL_EXPORT off64_t AAsset_seek64(AAsset* a, off64_t offset, int whence) {
  if (a == NULL) return (off64_t)-1;
  off64_t base = whence == SEEK_CUR ? a->pos : (whence == SEEK_END ? a->len : 0);
  off64_t np = base + offset;
  if (np < 0 || np > a->len) return (off64_t)-1;
  a->pos = np;
  return a->pos;
}
ACL_EXPORT const void* AAsset_getBuffer(AAsset* a) { return a != NULL ? a->data : NULL; }
ACL_EXPORT off64_t AAsset_getLength(AAsset* a) { return a != NULL ? a->len : 0; }
ACL_EXPORT off64_t AAsset_getLength64(AAsset* a) { return a != NULL ? a->len : 0; }
ACL_EXPORT off64_t AAsset_getRemainingLength(AAsset* a) { return a != NULL ? a->len - a->pos : 0; }
ACL_EXPORT off64_t AAsset_getRemainingLength64(AAsset* a) { return a != NULL ? a->len - a->pos : 0; }
// The asset is decompressed into a heap buffer (getBuffer is that buffer), never an mmap of the
// APK — so it is "allocated" and there is no backing file descriptor to hand out.
ACL_EXPORT int AAsset_isAllocated(AAsset* a) { (void)a; return 1; }
ACL_EXPORT int AAsset_openFileDescriptor(AAsset* a, off_t* outStart, off_t* outLength) {
  (void)a; (void)outStart; (void)outLength;
  return -1;  // DEFLATE'd assets have no contiguous file range; callers fall back to getBuffer
}
ACL_EXPORT int AAsset_openFileDescriptor64(AAsset* a, off64_t* outStart, off64_t* outLength) {
  (void)a; (void)outStart; (void)outLength;
  return -1;
}

// --- DNS resolver seeding -----------------------------------------------------------------------
// Android resolves DNS through netd's /dev/socket/dnsproxyd; acl runs no netd, so bionic's
// getaddrinfo tries that socket (ENOENT) and falls back to its in-libc resolver — which has NO
// nameservers configured, so it queries 0.0.0.0 and every lookup fails. That is why an app's
// network image/HTTP fetches never resolve a host. netd normally seeds the resolver via the libc
// entry point _resolv_set_nameservers_for_net(); do the same, mirroring the host's
// /etc/resolv.conf so the fallback resolver has real servers. netid 0 (NETID_UNSET) is the default
// context getaddrinfo() uses. Runs in bionic context, driven explicitly by acl (like
// acl_ndk_log_init) because .init_array constructors do not run under acl's in-process loader.
// Provenance: bionic public resolver contract (_resolv_set_nameservers_for_net is the same symbol
// netd links, from bionic/libc/dns, Apache-2.0); resolv.conf format is standard. No GPL-3.0 sources.
extern int _resolv_set_nameservers_for_net(unsigned netid, const char** servers,
                                           unsigned numservers, const char* domains,
                                           const void* params);

#define ACL_RESOLV_MAXNS 4

ACL_EXPORT void acl_seed_dns_resolv(void) {
  // Run bionic's resolver in "local" mode. Without netd, getaddrinfo falls back to the in-libc
  // resolver, whose DNS cache is created with zero entries UNLESS ANDROID_DNS_MODE=local (on real
  // Android only netd's proxy uses the local cache). A zero-sized cache makes _cache_lookup_p do a
  // `hash % 0` and take SIGFPE. Set it before the first getaddrinfo/cache creation below.
  setenv("ANDROID_DNS_MODE", "local", 1);

  char store[ACL_RESOLV_MAXNS][64];
  const char* servers[ACL_RESOLV_MAXNS];
  unsigned n = 0;

  FILE* f = fopen("/etc/resolv.conf", "re");
  if (f != NULL) {
    char line[256];
    while (n < ACL_RESOLV_MAXNS && fgets(line, sizeof(line), f) != NULL) {
      char* p = line;
      while (*p == ' ' || *p == '\t') p++;
      if (strncmp(p, "nameserver", 10) != 0) continue;
      p += 10;
      while (*p == ' ' || *p == '\t') p++;
      unsigned k = 0;
      while (p[k] != '\0' && p[k] != '\n' && p[k] != '\r' && p[k] != ' ' && p[k] != '\t'
             && k + 1 < sizeof(store[0])) { store[n][k] = p[k]; k++; }
      if (k == 0) continue;
      store[n][k] = '\0';
      servers[n] = store[n];
      n++;
    }
    fclose(f);
  }

  // If the host config yielded nothing (no systemd-resolved, unreadable file, …), fall back to
  // public resolvers so DNS still works out of the box.
  if (n == 0) {
    static const char* const kFallback[] = { "1.1.1.1", "8.8.8.8" };
    for (unsigned i = 0; i < 2 && n < ACL_RESOLV_MAXNS; i++) servers[n++] = kFallback[i];
  }

  int rc = _resolv_set_nameservers_for_net(0u, servers, n, "", NULL);
  __android_log_print(rc == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, LOG_TAG,
                      "seeded %u DNS nameserver(s) for default net (rc=%d, first=%s)",
                      n, rc, n > 0 ? servers[0] : "(none)");
}

// --- AHardwareBuffer (NDK libandroid) -----------------------------------------------------------
// The Flutter engine dlopen("libandroid.so") + dlsym's these to import a Java HardwareBuffer as a
// texture (see [[flutter-texture-bridge]] M3b). The Java android.hardware.HardwareBuffer's nativePtr
// (aclNativePtr()) IS an acl gfx AHardwareBuffer*, so fromHardwareBuffer just reads it back — pure
// JNI, no host bridge. describe/acquire/release operate on that host object, so they call through
// hooks the loader installs (bionic_runtime.cpp): describe/acquire are TLS-free; release frees
// (glibc malloc) and its trampoline restores glibc %fs first. Provenance: NDK
// android/hardware_buffer.h + hardware_buffer_jni.h contract. No GPL-3.0 sources.
ACL_EXPORT void* acl_ahb_describe_hook = 0;  // void (*)(const void* ahb, void* desc)
ACL_EXPORT void* acl_ahb_acquire_hook = 0;   // void (*)(void* ahb)
ACL_EXPORT void* acl_ahb_release_hook = 0;   // void (*)(void* ahb)

ACL_EXPORT AHardwareBuffer* AHardwareBuffer_fromHardwareBuffer(JNIEnv* env, jobject hb) {
  if (env == NULL || hb == NULL) return NULL;
  // The REAL android.hardware.HardwareBuffer stashes the native AHardwareBuffer* in its private
  // `long mNativeObject` field (AOSP: "do not rename, this field is used by native code"). acl's
  // nCreateHardwareBuffer put a gfx::AHardwareBuffer* there; read it back.
  jclass c = (*env)->GetObjectClass(env, hb);
  if (c == NULL) { (*env)->ExceptionClear(env); return NULL; }
  jfieldID f = (*env)->GetFieldID(env, c, "mNativeObject", "J");
  (*env)->DeleteLocalRef(env, c);
  if (f == NULL) { (*env)->ExceptionClear(env); return NULL; }
  jlong ptr = (*env)->GetLongField(env, hb, f);
  return (AHardwareBuffer*)(intptr_t)ptr;
}

// Not on the consumer/texture path (acl producers allocate via android.hardware.HardwareBuffer.create,
// which is JNI-backed). Exported so the engine's dlsym succeeds; returns null.
ACL_EXPORT jobject AHardwareBuffer_toHardwareBuffer(JNIEnv* env, AHardwareBuffer* ahb) {
  (void)env; (void)ahb; return NULL;
}

ACL_EXPORT void AHardwareBuffer_describe(const AHardwareBuffer* ahb, AHardwareBuffer_Desc* desc) {
  if (acl_ahb_describe_hook != 0 && ahb != NULL && desc != NULL)
    ((void (*)(const void*, void*))acl_ahb_describe_hook)(ahb, desc);
}
ACL_EXPORT void AHardwareBuffer_acquire(AHardwareBuffer* ahb) {
  if (acl_ahb_acquire_hook != 0 && ahb != NULL) ((void (*)(void*))acl_ahb_acquire_hook)(ahb);
}
ACL_EXPORT void AHardwareBuffer_release(AHardwareBuffer* ahb) {
  if (acl_ahb_release_hook != 0 && ahb != NULL) ((void (*)(void*))acl_ahb_release_hook)(ahb);
}
ACL_EXPORT int AHardwareBuffer_isSupported(const AHardwareBuffer_Desc* desc) { (void)desc; return 1; }
// The engine keys its EGLImage LRU cache on this (impeller GetSystemUniqueID). The AHardwareBuffer*
// is already a stable per-buffer id here. Returns 0 on success (NDK contract).
ACL_EXPORT int AHardwareBuffer_getId(const AHardwareBuffer* ahb, uint64_t* out_id) {
  if (out_id != NULL) *out_id = (uint64_t)(uintptr_t)ahb;
  return 0;
}

// Producer-side NDK allocation is not used by acl (producers go through the JNI HardwareBuffer.create
// path); exported so the engine's dlsym resolves. Returns an error (non-zero).
ACL_EXPORT int AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** out) {
  (void)desc; if (out != NULL) *out = NULL; return -1;
}
ACL_EXPORT int AHardwareBuffer_lock(AHardwareBuffer* ahb, uint64_t usage, int32_t fence, const ARect* rect,
                         void** out) {
  (void)ahb; (void)usage; (void)fence; (void)rect; if (out != NULL) *out = NULL; return -1;
}
ACL_EXPORT int AHardwareBuffer_unlock(AHardwareBuffer* ahb, int32_t* fence) {
  (void)ahb; if (fence != NULL) *fence = -1; return 0;
}
// Cross-process handle transfer over a unix socket. acl is single-process, so WebView's Vulkan
// compositor should never actually marshal a buffer across the socket — but Chromium's
// AHardwareBuffer compat shim DCHECKs all eight AHardwareBuffer_* pointers are non-null at load
// (base/android/android_hardware_buffer_compat.cc). Exported so that dlsym resolves; return an
// error if ever invoked (the caller then falls back rather than corrupting state).
ACL_EXPORT int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer* ahb, int fd) {
  (void)ahb; (void)fd; return -1;
}
ACL_EXPORT int AHardwareBuffer_recvHandleFromUnixSocket(int fd, AHardwareBuffer** out) {
  (void)fd; if (out != NULL) *out = NULL; return -1;
}

// ── ASharedMemory (NDK, API 26+) — memfd-backed ─────────────────────────────────────────────────
// Chromium's base shared memory (ashmem-dev.c) dlsyms these from libandroid on SDK>=26; the fallback
// is /dev/ashmem, which the host lacks (so region creation fails -> WebView font-table DCHECK). Back
// them with memfd: create+size via memfd_create/ftruncate; read-only conversion (setProt without
// PROT_WRITE) via write-seal, so Chromium's mmap-probe get_prot_region reports PROT_READ.
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010  // Linux 5.1+
#endif
ACL_EXPORT int ASharedMemory_create(const char* name, size_t size) {
  int fd = memfd_create(name != NULL ? name : "acl-shmem", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) return -errno;
  if (ftruncate(fd, (off_t)size) < 0) { int e = errno; close(fd); return -e; }
  return fd;
}
ACL_EXPORT size_t ASharedMemory_getSize(int fd) {
  struct stat st;
  return fstat(fd, &st) == 0 ? (size_t)st.st_size : 0;
}
ACL_EXPORT int ASharedMemory_setProt(int fd, int prot) {
  // ashmem restricts the region's MAX protection; emulate the read-only case by sealing the memfd so
  // it can no longer be mapped writable (Chromium's ashmem-dev.c get_prot_region probes writability
  // with an RW mmap, so the seal must make that mmap fail). Prefer F_SEAL_FUTURE_WRITE: Chromium's
  // ReadOnlySharedMemoryRegion::Create converts a region to read-only WHILE it is still mapped
  // writable, and a full F_SEAL_WRITE EBUSYs whenever a writable mapping is outstanding (leaving the
  // region unsealed -> the CheckPlatformHandlePermissionsCorrespondToMode CHECK aborts). FUTURE_WRITE
  // blocks only new writable mmaps, so it applies regardless of existing mappings. Fall back to
  // F_SEAL_WRITE on kernels without FUTURE_WRITE (< 5.1).
  if (!(prot & PROT_WRITE))
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE | F_SEAL_SHRINK | F_SEAL_GROW) < 0)
      fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW);  // best-effort
  return 0;
}

// ── ASurfaceControl (NDK, API 29+) over acl's compositor ─────────────────────────────────────────
// Chromium WebView composites its rendered content (an AHardwareBuffer per frame) through
// SurfaceControl: it creates surface controls (a tree rooted at the app window) and, per frame,
// batches setBuffer/setGeometry/... into a transaction and applies it; the platform latches the
// buffers and fires an OnComplete callback with a present fence. There is no SurfaceFlinger on acl,
// so this backs surface controls with logical nodes and (Stage: wiring) presents each frame's
// AHardwareBuffer via acl's dma-buf path. Chromium's SurfaceControl::IsSupported() CHECKs that these
// symbols resolve, so they MUST exist in libandroid; apply() must run the OnComplete callback or
// Chromium blocks its render thread waiting for the frame to latch.
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

struct ASurfaceControl {
  int refs;
  struct ASurfaceControl* parent;
  char name[64];
  AHardwareBuffer* buffer;   // latest latched buffer
};
typedef struct { struct ASurfaceControl* sc; AHardwareBuffer* buffer; int fence_fd; int has_buffer; } AclScOp;
#define ACL_TXN_MAX_OPS 128
struct ASurfaceTransaction {
  AclScOp ops[ACL_TXN_MAX_OPS];
  int n_ops;
  void* on_complete_ctx; void (*on_complete)(void* ctx, void* stats);
  void* on_commit_ctx;   void (*on_commit)(void* ctx, void* stats);
};
struct ASurfaceTransactionStats {
  struct ASurfaceControl* scs[ACL_TXN_MAX_OPS];
  size_t n;
};

ACL_EXPORT struct ASurfaceControl* ASurfaceControl_createFromWindow(ANativeWindow* parent, const char* name) {
  (void)parent;
  struct ASurfaceControl* sc = (struct ASurfaceControl*)calloc(1, sizeof(*sc));
  if (sc) { sc->refs = 1; if (name) snprintf(sc->name, sizeof(sc->name), "%s", name); }
  ALOGI("ASurfaceControl_createFromWindow(%s) -> %p", name ? name : "", (void*)sc);
  return sc;
}
ACL_EXPORT struct ASurfaceControl* ASurfaceControl_create(struct ASurfaceControl* parent, const char* name) {
  struct ASurfaceControl* sc = (struct ASurfaceControl*)calloc(1, sizeof(*sc));
  if (sc) { sc->refs = 1; sc->parent = parent; if (name) snprintf(sc->name, sizeof(sc->name), "%s", name); }
  return sc;
}
ACL_EXPORT void ASurfaceControl_acquire(struct ASurfaceControl* sc) {
  if (sc) __atomic_add_fetch(&sc->refs, 1, __ATOMIC_ACQ_REL);
}
ACL_EXPORT void ASurfaceControl_release(struct ASurfaceControl* sc) {
  if (sc && __atomic_sub_fetch(&sc->refs, 1, __ATOMIC_ACQ_REL) <= 0) free(sc);
}

ACL_EXPORT struct ASurfaceTransaction* ASurfaceTransaction_create(void) {
  return (struct ASurfaceTransaction*)calloc(1, sizeof(struct ASurfaceTransaction));
}
ACL_EXPORT void ASurfaceTransaction_delete(struct ASurfaceTransaction* t) { free(t); }

// The render seam: Chromium hands us the frame's content as an AHardwareBuffer + an acquire fence.
ACL_EXPORT void ASurfaceTransaction_setBuffer(struct ASurfaceTransaction* t, struct ASurfaceControl* sc,
                                              AHardwareBuffer* buffer, int acquire_fence_fd) {
  if (!t || t->n_ops >= ACL_TXN_MAX_OPS) { if (acquire_fence_fd >= 0) close(acquire_fence_fd); return; }
  AclScOp* op = &t->ops[t->n_ops++];
  op->sc = sc; op->buffer = buffer; op->fence_fd = acquire_fence_fd; op->has_buffer = 1;
}
ACL_EXPORT void ASurfaceTransaction_setOnComplete(struct ASurfaceTransaction* t, void* ctx,
                                                  void (*func)(void*, void*)) {
  if (t) { t->on_complete_ctx = ctx; t->on_complete = func; }
}
ACL_EXPORT void ASurfaceTransaction_setOnCommit(struct ASurfaceTransaction* t, void* ctx,
                                                void (*func)(void*, void*)) {
  if (t) { t->on_commit_ctx = ctx; t->on_commit = func; }
}

ACL_EXPORT void ASurfaceTransaction_apply(struct ASurfaceTransaction* t) {
  if (!t) return;
  struct ASurfaceTransactionStats stats; stats.n = 0;
  for (int i = 0; i < t->n_ops; ++i) {
    AclScOp* op = &t->ops[i];
    if (!op->has_buffer) continue;
    if (op->sc) { op->sc->buffer = op->buffer; if (stats.n < ACL_TXN_MAX_OPS) stats.scs[stats.n++] = op->sc; }
    if (op->fence_fd >= 0) close(op->fence_fd);          // acl "acquired" the buffer
    // TODO(webview render bridge): import op->buffer (AHardwareBuffer -> dma-buf) and composite it as
    // a wl_subsurface at the surface control's geometry via acl's present loop.
    ALOGI("ASurfaceControl apply: setBuffer sc=%p buffer=%p (present TODO)", (void*)op->sc, (void*)op->buffer);
  }
  // Commit + complete must fire or Chromium's render thread blocks waiting for the frame to latch.
  if (t->on_commit) t->on_commit(t->on_commit_ctx, &stats);
  if (t->on_complete) t->on_complete(t->on_complete_ctx, &stats);
}

// Transaction stats read by the OnComplete callback. Present fence -1 = already presented (no fence);
// latch time = now; the SC list drives Chromium's per-surface release-fence handling.
ACL_EXPORT int64_t ASurfaceTransactionStats_getLatchTime(struct ASurfaceTransactionStats* s) {
  (void)s; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
ACL_EXPORT int ASurfaceTransactionStats_getPresentFenceFd(struct ASurfaceTransactionStats* s) { (void)s; return -1; }
ACL_EXPORT void ASurfaceTransactionStats_getASurfaceControls(struct ASurfaceTransactionStats* s,
                                                             struct ASurfaceControl*** out, size_t* out_n) {
  if (!s || !out || !out_n) { if (out_n) *out_n = 0; return; }
  *out_n = s->n;
  if (s->n == 0) { *out = NULL; return; }
  struct ASurfaceControl** arr = (struct ASurfaceControl**)malloc(s->n * sizeof(*arr));
  if (!arr) { *out = NULL; *out_n = 0; return; }
  for (size_t i = 0; i < s->n; ++i) { arr[i] = s->scs[i]; ASurfaceControl_acquire(arr[i]); }
  *out = arr;
}
ACL_EXPORT void ASurfaceTransactionStats_releaseASurfaceControls(struct ASurfaceControl** arr) {
  if (arr) free(arr);   // the acquire in getASurfaceControls is balanced by Chromium's own release
}
ACL_EXPORT int64_t ASurfaceTransactionStats_getAcquireTime(struct ASurfaceTransactionStats* s,
                                                           struct ASurfaceControl* sc) { (void)s; (void)sc; return -1; }
ACL_EXPORT int ASurfaceTransactionStats_getPreviousReleaseFenceFd(struct ASurfaceTransactionStats* s,
                                                                  struct ASurfaceControl* sc) { (void)s; (void)sc; return -1; }

// Per-surface transaction ops acl does not yet act on (geometry/z/visibility/color/…). The symbols
// must resolve (SurfaceControl::IsSupported); bodies are no-ops until the compositor wiring lands.
ACL_EXPORT void ASurfaceTransaction_reparent(struct ASurfaceTransaction* t, struct ASurfaceControl* sc,
                                             struct ASurfaceControl* np) { (void)t; if (sc) sc->parent = np; }
ACL_EXPORT void ASurfaceTransaction_setVisibility(void* t, void* sc, int8_t v) { (void)t; (void)sc; (void)v; }
ACL_EXPORT void ASurfaceTransaction_setZOrder(void* t, void* sc, int32_t z) { (void)t; (void)sc; (void)z; }
ACL_EXPORT void ASurfaceTransaction_setGeometry(void* t, void* sc, const void* src, const void* dst, int32_t tr) {
  (void)t; (void)sc; (void)src; (void)dst; (void)tr;
}
ACL_EXPORT void ASurfaceTransaction_setCrop(void* t, void* sc, const void* crop) { (void)t; (void)sc; (void)crop; }
ACL_EXPORT void ASurfaceTransaction_setPosition(void* t, void* sc, int32_t x, int32_t y) { (void)t; (void)sc; (void)x; (void)y; }
ACL_EXPORT void ASurfaceTransaction_setScale(void* t, void* sc, float sx, float sy) { (void)t; (void)sc; (void)sx; (void)sy; }
ACL_EXPORT void ASurfaceTransaction_setBufferTransform(void* t, void* sc, int32_t tr) { (void)t; (void)sc; (void)tr; }
ACL_EXPORT void ASurfaceTransaction_setBufferTransparency(void* t, void* sc, int8_t tp) { (void)t; (void)sc; (void)tp; }
ACL_EXPORT void ASurfaceTransaction_setBufferAlpha(void* t, void* sc, float a) { (void)t; (void)sc; (void)a; }
ACL_EXPORT void ASurfaceTransaction_setColor(void* t, void* sc, float r, float g, float b, float a, int ds) {
  (void)t; (void)sc; (void)r; (void)g; (void)b; (void)a; (void)ds;
}
ACL_EXPORT void ASurfaceTransaction_setDamageRegion(void* t, void* sc, const void* rects, uint32_t n) {
  (void)t; (void)sc; (void)rects; (void)n;
}
ACL_EXPORT void ASurfaceTransaction_setDesiredPresentTime(void* t, int64_t ns) { (void)t; (void)ns; }
ACL_EXPORT void ASurfaceTransaction_setBufferDataSpace(void* t, void* sc, uint64_t ds) { (void)t; (void)sc; (void)ds; }
ACL_EXPORT void ASurfaceTransaction_setHdrMetadata_smpte2086(void* t, void* sc, const void* m) { (void)t; (void)sc; (void)m; }
ACL_EXPORT void ASurfaceTransaction_setHdrMetadata_cta861_3(void* t, void* sc, const void* m) { (void)t; (void)sc; (void)m; }
ACL_EXPORT void ASurfaceTransaction_setFrameRate(void* t, void* sc, float fr, int8_t c) { (void)t; (void)sc; (void)fr; (void)c; }
ACL_EXPORT void ASurfaceTransaction_setFrameRateWithChangeStrategy(void* t, void* sc, float fr, int8_t c, int8_t s) {
  (void)t; (void)sc; (void)fr; (void)c; (void)s;
}
ACL_EXPORT void ASurfaceTransaction_setEnableBackPressure(void* t, void* sc, int8_t e) { (void)t; (void)sc; (void)e; }
ACL_EXPORT void ASurfaceTransaction_setFrameTimeline(void* t, int64_t v) { (void)t; (void)v; }

// ── AAssetDir listing (AAssetManager_openDir + AAssetDir iteration) ─────────────────────────────
// acl's asset hook opens a single named asset, not a directory, so openDir returns a valid but EMPTY
// AAssetDir (getNextFileName -> NULL). YouTube's libgoogle3.so references these; without the symbols
// its dlopen fails. An app enumerating an asset dir gets nothing and falls back.
typedef struct AAssetDir { int _unused; } AAssetDir;
ACL_EXPORT AAssetDir* AAssetManager_openDir(AAssetManager* mgr, const char* dirName) {
  (void)mgr; (void)dirName;
  AAssetDir* d = (AAssetDir*)malloc(sizeof(AAssetDir));
  if (d != NULL) d->_unused = 0;
  return d;  // non-NULL, empty
}
ACL_EXPORT const char* AAssetDir_getNextFileName(AAssetDir* dir) { (void)dir; return NULL; }
ACL_EXPORT void AAssetDir_rewind(AAssetDir* dir) { (void)dir; }
ACL_EXPORT void AAssetDir_close(AAssetDir* dir) { free(dir); }

// ── AImageDecoder (android/imagedecoder.h) ─ not backed: return "unsupported", keep symbols present.
// acl decodes images via the Java/graphics_jni path; the NDK AImageDecoder is not wired. Return the
// unsupported-format result so callers fall back instead of crashing; the symbols must exist so a lib
// that references them (YouTube's libgoogle3.so) loads. ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT=-6.
typedef struct AImageDecoder AImageDecoder;
ACL_EXPORT int AImageDecoder_createFromBuffer(const void* data, size_t size, AImageDecoder** out) {
  (void)data; (void)size; if (out != NULL) *out = NULL; return -6;
}
ACL_EXPORT int AImageDecoder_decodeImage(AImageDecoder* dec, void* px, size_t stride, size_t size) {
  (void)dec; (void)px; (void)stride; (void)size; return -1;
}
ACL_EXPORT int AImageDecoder_setAndroidBitmapFormat(AImageDecoder* dec, int format) {
  (void)dec; (void)format; return -1;
}
ACL_EXPORT int AImageDecoder_setTargetSize(AImageDecoder* dec, int width, int height) {
  (void)dec; (void)width; (void)height; return -1;
}
ACL_EXPORT void AImageDecoder_delete(AImageDecoder* dec) { (void)dec; }

// AHardwareBuffer_lockPlanes: delegate to the single-plane lock (the planes struct is left unfilled;
// callers that need per-plane strides for a multi-plane buffer degrade). Keeps the symbol present.
ACL_EXPORT int AHardwareBuffer_lockPlanes(AHardwareBuffer* ahb, uint64_t usage, int32_t fence,
                                          const ARect* rect, AHardwareBuffer_Planes* outPlanes) {
  (void)outPlanes;
  return AHardwareBuffer_lock(ahb, usage, fence, rect, NULL);
}

// ── NDK multi-network API (android/multinetwork.h) ─────────────────────────────────────────────
// On real Android these bind a socket/process to a specific Network; acl has a single host network,
// so per-socket/per-process binding and traffic tagging are no-ops (sockets already use the one
// route). These MUST be exported: YouTube's libgoogle3.so references android_setsocknetwork, and
// without the symbol its dlopen fails ("cannot locate symbol") and aborts Application.onCreate.
// DNS-for-network delegates to the host resolver (ignoring the handle); the raw res_* queries are
// unsupported (rarely used). net_handle_t is a uint64_t — take it by value to match the ABI.
ACL_EXPORT int android_setsocknetwork(uint64_t network, int fd) { (void)network; (void)fd; return 0; }
ACL_EXPORT int android_setprocnetwork(uint64_t network) { (void)network; return 0; }
ACL_EXPORT int android_getprocnetwork(uint64_t* network) { if (network) *network = 0; return 0; }
ACL_EXPORT int android_setprocdns(uint64_t network) { (void)network; return 0; }
ACL_EXPORT int android_getprocdns(uint64_t* network) { if (network) *network = 0; return 0; }
ACL_EXPORT int android_getaddrinfofornetwork(uint64_t network, const char* node, const char* service,
                                             const struct addrinfo* hints, struct addrinfo** res) {
  (void)network;  // single host network: resolve via the host resolver
  return getaddrinfo(node, service, hints, res);
}
ACL_EXPORT int android_res_nquery(uint64_t network, const char* dname, int ns_class, int ns_type,
                                  uint32_t flags) {
  (void)network; (void)dname; (void)ns_class; (void)ns_type; (void)flags; errno = ENOSYS; return -1;
}
ACL_EXPORT int android_res_nsend(uint64_t network, const uint8_t* msg, size_t msglen, uint32_t flags) {
  (void)network; (void)msg; (void)msglen; (void)flags; errno = ENOSYS; return -1;
}
ACL_EXPORT int android_res_nresult(int fd, int* rcode, uint8_t* answer, size_t anslen) {
  (void)fd; (void)rcode; (void)answer; (void)anslen; errno = ENOSYS; return -1;
}
ACL_EXPORT void android_res_cancel(int nsend_fd) { (void)nsend_fd; }
ACL_EXPORT int android_tag_socket_with_uid(int fd, uint32_t tag, uid_t uid) {
  (void)fd; (void)tag; (void)uid; return 0;
}
ACL_EXPORT int android_tag_socket(int fd, uint32_t tag) { (void)fd; (void)tag; return 0; }
ACL_EXPORT int android_untag_socket(int fd) { (void)fd; return 0; }
