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
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/types.h>  // off64_t (AAsset_getLength's return type)
#include <unistd.h>

#include <android/log.h>
#include <stdio.h>   // snprintf
#include <string.h>  // strlen
#include <sys/uio.h>  // writev

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
typedef struct ANativeWindow_Buffer ANativeWindow_Buffer;
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
static int g_anw_token;  // a stable, non-null ANativeWindow handle (identity only)
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
  (void)env; (void)surface;
  return (ANativeWindow*)&g_anw_token;
}
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
  (void)w; (void)b; (void)dirty;
  ACL_TODO("ANativeWindow_lock");
  return -EINVAL;
}
ACL_EXPORT int32_t ANativeWindow_unlockAndPost(ANativeWindow* w) {
  (void)w;
  ACL_TODO("ANativeWindow_unlockAndPost");
  return -EINVAL;
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
