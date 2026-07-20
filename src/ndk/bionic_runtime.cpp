// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl; promotes tools/ndk-loader-spike/bionic_bootstrap.cpp (proven Stage 0b,
// docs/ndk-scope.md) into src/. Bootstrap contract read from AOSP bionic linker source (Apache-2.0):
// linker/linker_main.cpp (__linker_init standalone path), libc/bionic/__libc_init_main_thread.cpp
// (permanent-TCB migration), private/KernelArgumentBlock.h. The two-libc %fs technique is the class
// of thing libhybris (Apache-2.0) does; not vendored. No GPL-3.0 sources.
//
// The whole TU is built -fno-stack-protector (see meson.build): the thread-pointer swap changes
// %fs, and the stack canary lives at %fs:0x28 — a mid-function change would false-fail the check.

#include "ndk/bionic_runtime.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

// Generated from the private headers of the bionic acl stages (tools/gen-bionic-offsets.sh) —
// attach_current_thread lays out a bionic TCB with these. Defines ACL_BIONIC_OFFSETS_AVAILABLE 0
// when no AOSP was present at build time, and attach then compiles out.
#include "bionic_offsets.h"

#include <atomic>
#include <cstdarg>
#include <mutex>
#include <unordered_map>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <vector>

#if defined(__x86_64__)
#include <asm/prctl.h>
#include <sys/auxv.h>
#include <sys/syscall.h>

#include <algorithm>
#include <cstdio>
#endif

#include "log/log.h"

namespace acl::ndk {
namespace {
constexpr std::string_view kTag = "acl.ndk";

// Reached when bionic code is about to run on a thread that has no bionic TCB — from the down-call
// trampoline (which checks the slot before swapping, so it is still on the caller's glibc %fs, and
// plain host calls are safe here) or from RunScope. It must not return. Continuing would run bionic
// code on another thread's TCB and, worse, return with THAT thread's glibc %fs installed, detaching
// the caller from its own TLS. Not a theoretical worry: ART's Thread::Current() is a TLS read, so
// the process dies later inside the GC ("Check failed: self == thread") with nothing pointing back
// here. Fail at the call instead.
[[noreturn]] void acl_ndk_unattached() {
  log::e(kTag,
         "bionic code reached on a thread with no bionic TCB. Threads must be attached first "
         "(BionicRuntime::attach_current_thread; see docs/ndk-scope.md). Aborting rather than "
         "corrupting this thread's TLS.");
  std::abort();
}
}  // namespace

bool is_bionic_elf(const char* path) {
  // Bionic libs DT_NEEDED "libc.so"; glibc libs DT_NEEDED "libc.so.6". That soname is the reliable
  // discriminator for routing app libs (bionic → BionicRuntime) vs acl's own glibc supplemental libs.
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const off_t sz = ::lseek(fd, 0, SEEK_END);
  if (sz < static_cast<off_t>(sizeof(Elf64_Ehdr))) { ::close(fd); return false; }
  void* m = ::mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (m == MAP_FAILED) return false;
  const auto* f = static_cast<const std::uint8_t*>(m);
  const auto& eh = *reinterpret_cast<const Elf64_Ehdr*>(f);
  bool bionic = false;
  if (std::memcmp(f, ELFMAG, SELFMAG) == 0 && eh.e_ident[EI_CLASS] == ELFCLASS64 && eh.e_shoff &&
      eh.e_shoff + std::uint64_t(eh.e_shnum) * sizeof(Elf64_Shdr) <= std::uint64_t(sz)) {
    const auto* sh = reinterpret_cast<const Elf64_Shdr*>(f + eh.e_shoff);
    for (int i = 0; i < eh.e_shnum && !bionic; ++i) {
      if (sh[i].sh_type != SHT_DYNAMIC || sh[i].sh_link >= eh.e_shnum) continue;
      const auto* dyn = reinterpret_cast<const Elf64_Dyn*>(f + sh[i].sh_offset);
      const char* str = reinterpret_cast<const char*>(f + sh[sh[i].sh_link].sh_offset);
      for (; dyn->d_tag != DT_NULL; ++dyn)
        if (dyn->d_tag == DT_NEEDED && std::strcmp(str + dyn->d_un.d_val, "libc.so") == 0) {
          bionic = true;
          break;
        }
    }
  }
  ::munmap(m, sz);
  return bionic;
}

BionicConfig BionicConfig::from_env() {
  BionicConfig c;
  if (const char* v = std::getenv("ACL_BIONIC_LINKER")) c.linker = v;
  if (const char* v = std::getenv("ACL_BIONIC_SYSROOT")) c.sysroot = v;
  if (const char* v = std::getenv("ACL_BIONIC_STUB")) c.stub = v;
  // The app's own lib/<abi> dir, exported by `acl run` (and settable by hand for the probe): a real
  // app's libs mostly NEED each other — React Native's libjsi.so needs the libc++_shared.so shipped
  // beside it — so without this the sysroot alone cannot resolve them.
  if (const char* v = std::getenv("ACL_BIONIC_APP_LIBS"); v != nullptr && *v != '\0')
    c.extra_lib_paths.emplace_back(v);
  return c;
}

bool BionicConfig::valid(std::string* why) const {
  auto bad = [&](std::string m) { if (why) *why = std::move(m); return false; };
  namespace fs = std::filesystem;
  for (const auto* p : {&linker, &sysroot, &stub}) {
    if (p->empty()) return bad("bionic runtime path unset (ACL_BIONIC_LINKER/_SYSROOT/_STUB)");
    if (!fs::path(*p).is_absolute()) return bad("bionic runtime path not absolute: " + *p);
    std::error_code ec;
    if (!fs::exists(fs::path(*p), ec)) return bad("bionic runtime path missing: " + *p);
  }
  return true;
}

#if defined(__x86_64__)

// The x86_64 bootstrap. Kept in an anonymous namespace so nothing here is inlined into callers
// compiled with a stack protector.
namespace {

constexpr std::size_t kPage = 4096;
std::uintptr_t page_down(std::uintptr_t v) { return v & ~(std::uintptr_t{kPage} - 1); }
std::uintptr_t page_up(std::uintptr_t v) { return page_down(v + kPage - 1); }

// ── Per-thread bionic thread pointer ─────────────────────────────────────────────────────────
// A bionic TCB belongs to ONE thread (it holds errno, pthread_self, pthread TLS, the DTV), so a
// thread running bionic code needs its own — see BionicRuntime::attach_current_thread. This slot
// holds the calling thread's bionic TCB, or 0 when the thread has never been attached. The
// down-call trampoline reads it *before* swapping (i.e. still on the caller's glibc %fs) via a
// constant %fs-relative displacement — hence initial-exec, which guarantees the slot lives in the
// static TLS block at a fixed offset from the thread pointer for EVERY thread. (Local-exec would
// do as well here; initial-exec states the requirement explicitly and survives this TU moving
// into a shared library.)
__thread __attribute__((tls_model("initial-exec"))) std::uintptr_t t_bionic_fs = 0;
std::int64_t g_tls_disp = 0;  // &t_bionic_fs - glibc %fs base; constant across threads

// bionic %fs -> glibc %fs, for the up-call thunks. An up-call happens under bionic %fs, where a
// glibc __thread slot is unreachable (that is the whole point of the boundary), so the pairing
// cannot live in glibc TLS like t_bionic_fs does. It is registered per attached thread and only
// ever appended to, so a reader needs no lock; there are as many entries as threads that run
// native code, and a linear scan of those is cheaper than the %fs swaps around it.
struct FsPair {
  std::atomic<std::uintptr_t> bionic_fs{0};
  std::uintptr_t glibc_fs = 0;
};
constexpr int kMaxAttached = 256;
FsPair g_fs_pairs[kMaxAttached];
// The down-call trampoline's epilogue scans g_fs_pairs in hand-written asm (16-byte stride,
// glibc_fs at +8) to recover the caller's glibc %fs — keep the layout it assumes.
static_assert(sizeof(FsPair) == 16, "trampoline scan assumes a 16-byte FsPair");
static_assert(offsetof(FsPair, glibc_fs) == 8, "trampoline scan assumes glibc_fs at +8");
std::atomic<int> g_fs_pair_count{0};

void register_fs_pair(std::uintptr_t bionic_fs, std::uintptr_t glibc_fs) {
  // Reuse a slot a detached thread left behind before growing: threads come and go, and an
  // append-only table would run out under any thread churn. Only the owning thread clears its own
  // entry, so a reader that matched `bionic_fs` cannot have it freed underneath — it is that
  // thread doing the reading.
  const int n = g_fs_pair_count.load(std::memory_order_acquire);
  for (int i = 0; i < n && i < kMaxAttached; ++i) {
    std::uintptr_t expected = 0;
    if (g_fs_pairs[i].bionic_fs.load(std::memory_order_relaxed) != 0) continue;
    g_fs_pairs[i].glibc_fs = glibc_fs;  // must be visible before the entry is published
    if (g_fs_pairs[i].bionic_fs.compare_exchange_strong(expected, bionic_fs,
                                                        std::memory_order_release,
                                                        std::memory_order_relaxed))
      return;
  }
  const int i = g_fs_pair_count.fetch_add(1, std::memory_order_relaxed);
  if (i >= kMaxAttached) {
    log::e(kTag, "more than {} threads attached to bionic at once; cannot track this one's glibc %fs",
           kMaxAttached);
    std::abort();  // an untracked thread would up-call onto another thread's TLS
  }
  g_fs_pairs[i].glibc_fs = glibc_fs;
  g_fs_pairs[i].bionic_fs.store(bionic_fs, std::memory_order_release);  // publishes the entry
}

// Called from the exiting thread's own destructor, before its TCB is unmapped — so the address
// cannot be recycled by a later mmap while a stale entry still names it.
void unregister_fs_pair(std::uintptr_t bionic_fs) {
  const int n = g_fs_pair_count.load(std::memory_order_acquire);
  for (int i = 0; i < n && i < kMaxAttached; ++i)
    if (g_fs_pairs[i].bionic_fs.load(std::memory_order_acquire) == bionic_fs) {
      g_fs_pairs[i].bionic_fs.store(0, std::memory_order_release);  // frees the slot for reuse
      return;
    }
}

std::uintptr_t glibc_fs_for(std::uintptr_t bionic_fs) {
  const int n = g_fs_pair_count.load(std::memory_order_acquire);
  for (int i = 0; i < n && i < kMaxAttached; ++i)
    if (g_fs_pairs[i].bionic_fs.load(std::memory_order_acquire) == bionic_fs)
      return g_fs_pairs[i].glibc_fs;
  return 0;
}


// Entered under bionic %fs (from the bionic .so); restore glibc for the real call, then bionic.
// Sets the *constant* glibc/bionic %fs (not a save/restore), which composes correctly under JNI
// re-entrancy with the down-call RunScope (up-calls always originate in bionic, down-calls in glibc).
// ── Releasing an attached thread ─────────────────────────────────────────────────────────────
// An attached thread must give its TCB back when it exits, or a process with any thread churn
// (ART's pools) grows without bound. Bionic cannot tell us: this thread was never one of its own,
// so it will not run bionic's thread-exit path, and __libc_register_thread_exit_callback only
// fires for threads bionic created. glibc can: a pthread key destructor runs ON the exiting
// thread, which is exactly where its bionic TCB has to be dismantled.
//
// What this CANNOT free — bionic's own per-thread dynamic-TLS blocks. If the thread's native code
// touched a dlopen'ed module's __thread variable, bionic's allocator has blocks hanging off the
// DTV, and __free_dynamic_tls (the only thing that knows how to release them) is __LIBC_HIDDEN__.
// See docs/ndk-scope.md.
struct AttachedThread {
  void* tls_block = nullptr;
  std::size_t tls_bytes = 0;
  void* thread_struct = nullptr;
  std::size_t thread_bytes = 0;
  std::uintptr_t bionic_fs = 0;
};

void release_attached_thread(void* p) {
  auto* a = static_cast<AttachedThread*>(p);
  // Order matters: drop the pairing first, so no up-call can be routed to a TCB that is about to
  // be unmapped — and so a later mmap reusing the address cannot collide with a stale entry.
  unregister_fs_pair(a->bionic_fs);
  t_bionic_fs = 0;  // a native reaching this thread again would now attach afresh, not use freed memory
  if (a->tls_block != nullptr) ::munmap(a->tls_block, a->tls_bytes);
  if (a->thread_struct != nullptr) ::munmap(a->thread_struct, a->thread_bytes);
  delete a;
}

pthread_key_t g_attached_key;
std::once_flag g_attached_key_once;

// The DTV every attached thread starts with: all-zero, so its generation is kTlsGenerationNone.
// That makes the first dynamic-TLS access build the thread's real DTV, and stops
// __free_dynamic_tls's chain-walk here rather than passing acl's memory to bionic's allocator.
// One process-wide instance, exactly as bionic's own `static const TlsDtv zero_dtv`. Bytes, not a
// struct: the layout belongs to bionic, and its size is measured, not assumed.
#if ACL_BIONIC_OFFSETS_AVAILABLE
alignas(16) char g_zero_dtv[ACL_BIONIC_SIZEOF_TLSDTV] = {};
#endif

std::uintptr_t get_fs() {
  std::uintptr_t v = 0;
  syscall(SYS_arch_prctl, ARCH_GET_FS, &v);
  return v;
}
void set_fs(std::uintptr_t v) { syscall(SYS_arch_prctl, ARCH_SET_FS, v); }

// ── Signal %fs shim ──────────────────────────────────────────────────────────────────────────
// A thread running a bionic .so's native code holds the bionic %fs. If a signal lands then — ART's
// own thread-management signals (suspend/checkpoint), or a genuine fault — the glibc signal handler
// ART installs (libsigchain) runs with the WRONG %fs and crashes on its first glibc TLS access
// (pthread_getspecific). Interpose the KERNEL handler (raw rt_sigaction, below libsigchain's own
// sigaction interposition): restore this thread's glibc %fs via the pairing BEFORE delegating to
// ART's handler, then restore bionic on return. If ART's handler services the signal it returns and
// the thread resumes native code; if it's a real crash, ART now produces its proper report instead
// of the pthread_getspecific double-fault. Signals under glibc %fs (the normal case) pass straight
// through (glibc_fs_for returns 0). All of this is TLS-free until the swap, which the whole TU's
// -fno-stack-protector allows.
struct KSigaction {  // the KERNEL's layout (differs from glibc's: field order + 8-byte mask)
  void* handler = nullptr;            // sa_handler / sa_sigaction
  unsigned long flags = 0;            // sa_flags (SA_RESTORER, SA_SIGINFO, …)
  void (*restorer)(void) = nullptr;   // sa_restorer — required on x86_64
  unsigned long mask = 0;             // sa_mask (first 64 signals)
};
int raw_rt_sigaction(int sig, const KSigaction* act, KSigaction* old) {
  return static_cast<int>(syscall(SYS_rt_sigaction, sig, act, old, sizeof(unsigned long)));
}
struct SavedSig {
  void* handler = nullptr;
  unsigned long flags = 0;
};
SavedSig g_saved_sig[65];  // indexed by signo (1..64)
std::atomic<bool> g_shims_installed{false};

// True if `fs` is the glibc %fs of a thread acl attached (i.e. an ART thread). Scans the pairing
// table's glibc side. Lets the signal shim tell an attached ART thread already on its glibc %fs
// (leave it alone, so ART's own fault handler runs) from a bionic-spawned thread ART never owned.
bool is_known_glibc_fs(std::uintptr_t fs) {
  const int n = g_fs_pair_count.load(std::memory_order_acquire);
  for (int i = 0; i < n && i < kMaxAttached; ++i) {
    if (g_fs_pairs[i].bionic_fs.load(std::memory_order_acquire) == 0) continue;
    if (g_fs_pairs[i].glibc_fs == fs) return true;
  }
  return false;
}

// A glibc %fs whose libart Thread::self_tls_ reads null, donated by a real glibc thread that parks
// forever and never attaches to ART. A signal delivered to a bionic-spawned engine thread (one ART
// does not own — Flutter/Skia/Dart spawn ~18) runs ART's chained fault handler; without a valid
// glibc %fs it dereferences a garbage Thread and SEGV_ACCERRs, and ART's all-thread crash dump
// (rt_tgsigqueueinfo) cascades the same fault onto every such thread. Under this %fs,
// Thread::Current() is null, so ART's FaultManager::IsInGeneratedCode returns false and HandleFault
// declines cleanly instead of crashing. See docs/ndk-scope.md.
std::atomic<std::uintptr_t> g_null_art_fs{0};
void* null_art_fs_donor(void*) {
  g_null_art_fs.store(get_fs(), std::memory_order_release);  // this thread's glibc %fs; self_tls_ == null
  for (;;) pause();  // park forever: the %fs must outlive the process, and the thread must never run ART
  return nullptr;
}
void ensure_null_art_fs() {
  if (g_null_art_fs.load(std::memory_order_acquire) != 0) return;
  pthread_t t;
  if (pthread_create(&t, nullptr, null_art_fs_donor, nullptr) != 0) return;
  pthread_detach(t);
  for (int i = 0; i < 10000 && g_null_art_fs.load(std::memory_order_acquire) == 0; ++i)
    usleep(100);  // wait for the donor to publish its %fs before any signal can need it
}

// --- Phase 2: bionic->host-Mesa GL forwarding -------------------------------------------------
// Flutter/Impeller renders GL on bionic-spawned engine threads, but host Mesa (glibc libglvnd) keeps
// the current context in per-thread TLS (_glapi_tls_Current). The generated bionic libEGL.so /
// libGLESv2.so (acl_egl_fwd.c / acl_gles_fwd.c) are thin trampolines: each entrypoint reads the
// bridge below, wrfsbase's to a *glibc* %fs that is Mesa's TLS home, calls the host Mesa symbol, and
// swaps back. The bridge is { donor_fs; fns[] } — populated here and its address written into the
// forwarder libs' acl_gl_{egl,gles}_bridge globals after they load. See docs/ndk-scope.md.
#include "gl_forward_syms.inc"  // kEglForwardSyms[20], kGlesForwardSyms[106] — ABI-ordered
struct GlBridge {
  std::uintptr_t donor_fs;  // offset 0: the donor glibc %fs (Mesa's per-thread TLS lives here)
  void* fns[128];           // offset 8: host-Mesa function pointers, indexed to match the forwarders
};

// A POOL of parked glibc threads whose %fs the app's GL threads borrow (one donor per GL thread), so
// host Mesa's per-thread state (current context, driver TLS) is never shared between two GL threads.
// Flutter drives GL from a raster thread AND an IO thread; sharing one donor makes Mesa treat them as
// one and write through stale per-thread pointers, corrupting memory. Each donor publishes its %fs
// then parks forever. Created AFTER host Mesa is dlopen'd, so Mesa's initial-exec _glapi_tls_Current
// lands in each donor's static TLS. The forwarders map caller-thread -> donor via acl_gl_pick_donor
// (in libEGL.so); acl only fills the pool here.
constexpr int kGlDonorPool = 8;
std::atomic<std::uintptr_t> g_gl_donor_pool[kGlDonorPool];
std::atomic<int> g_gl_donor_published{0};
void* g_gl_pick_donor = nullptr;  // address of libEGL.so's acl_gl_pick_donor, for the JIT stubs
void* gl_fs_donor(void* arg) {
  const int i = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  g_gl_donor_pool[i].store(get_fs(), std::memory_order_release);
  g_gl_donor_published.fetch_add(1, std::memory_order_release);
  for (;;) pause();  // park forever: this %fs must outlive the process; the thread never runs itself
  return nullptr;
}
void ensure_gl_donor_pool() {
  static std::atomic<bool> done{false};
  if (done.exchange(true)) return;
  for (int i = 0; i < kGlDonorPool; ++i) {
    pthread_t t;
    if (pthread_create(&t, nullptr, gl_fs_donor, reinterpret_cast<void*>(intptr_t(i))) == 0)
      pthread_detach(t);
  }
  for (int k = 0; k < 20000 && g_gl_donor_published.load(std::memory_order_acquire) < kGlDonorPool; ++k)
    usleep(100);  // wait for every donor to publish its %fs before any GL call can need one
}

// Attach-donor pool: parked glibc threads whose %fs a bionic-spawned thread borrows to host its ART
// Thread when it calls AttachCurrentThread (the reverse of attach_current_thread, which gives a glibc
// thread a bionic TCB). ART keeps per-thread state -- Thread::Current, the JNIEnv -- in this %fs's
// glibc TLS, so a real AttachCurrentThreadInternal can run instead of SEGV'ing under bionic %fs. One
// donor per attaching thread (never shared: two threads sharing one %fs would overwrite each other's
// Thread), assigned once and kept for the thread's life. Same parked-forever pattern as the GL
// donors. Known limitation: ART reads this thread's stack bounds from the DONOR's pthread, not the
// bionic thread's -- fine for short JNI up-calls; a GC that scans the stack mid-call could miss roots.
constexpr int kAttachDonorPool = 16;
std::atomic<std::uintptr_t> g_attach_donor_pool[kAttachDonorPool];
std::atomic<int> g_attach_donor_published{0};
std::atomic<int> g_attach_donor_next{0};
void* attach_fs_donor(void* arg) {
  const int i = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  g_attach_donor_pool[i].store(get_fs(), std::memory_order_release);
  g_attach_donor_published.fetch_add(1, std::memory_order_release);
  for (;;) pause();  // park forever: this %fs must outlive the process; the thread never runs ART
  return nullptr;
}
void ensure_attach_donor_pool() {
  static std::atomic<bool> done{false};
  if (done.exchange(true)) return;
  for (int i = 0; i < kAttachDonorPool; ++i) {
    pthread_t t;
    if (pthread_create(&t, nullptr, attach_fs_donor, reinterpret_cast<void*>(intptr_t(i))) == 0)
      pthread_detach(t);
  }
  for (int k = 0; k < 20000 && g_attach_donor_published.load(std::memory_order_acquire) < kAttachDonorPool; ++k)
    usleep(100);
}
// Claim the next free donor slot, or -1 if the pool is exhausted.
int claim_attach_donor() {
  const int i = g_attach_donor_next.fetch_add(1, std::memory_order_acq_rel);
  return i < g_attach_donor_published.load(std::memory_order_acquire) ? i : -1;
}

// A borrowing bionic thread's REAL stack, recorded per donor slot so the interposed
// pthread_getattr_np (below) can report it to ART. Without this, ART reads the DONOR's (tiny, parked)
// stack via pthread_getattr_np and its GC / stack-overflow logic scans the wrong range.
struct BorrowerStack {
  std::atomic<std::uintptr_t> donor_fs{0};  // published last; 0 = slot unused
  std::uintptr_t low = 0;
  std::uintptr_t size = 0;
};
BorrowerStack g_borrower_stacks[kAttachDonorPool];

static bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static unsigned hexv(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

// TLS-free (raw syscalls, no malloc/stdio): find the /proc/self/maps region containing `sp` -- the
// calling thread's stack. Runs under bionic %fs at pairing time, so it must touch no glibc TLS.
bool stack_region_for(std::uintptr_t sp, std::uintptr_t* out_low, std::uintptr_t* out_high) {
  const long fd = syscall(SYS_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY | O_CLOEXEC, 0L);
  if (fd < 0) return false;
  char buf[8192];
  char line[512];
  std::size_t ll = 0;
  bool found = false;
  for (;;) {
    const long n = syscall(SYS_read, fd, buf, sizeof buf);
    if (n <= 0) break;
    for (long i = 0; i < n; ++i) {
      const char c = buf[i];
      if (c != '\n') { if (ll < sizeof line - 1) line[ll++] = c; continue; }
      line[ll] = '\0';
      ll = 0;
      const char* p = line;
      std::uintptr_t a = 0, b = 0;
      while (is_hex(*p)) a = a * 16 + hexv(*p++);
      if (*p != '-') continue;
      ++p;
      while (is_hex(*p)) b = b * 16 + hexv(*p++);
      if (a <= sp && sp < b) { *out_low = a; *out_high = b; found = true; break; }
    }
    if (found) break;
  }
  syscall(SYS_close, fd);
  return found;
}

// The current thread's glibc %fs, pairing an unpaired bionic-spawned thread with an attach donor on
// demand so it can cross into acl's glibc code (ART attach, or a pkg asset read). Once paired the
// thread keeps its donor for the process's life. Records the thread's real stack against the donor
// slot so pthread_getattr_np reports it. Returns 0 only when the donor pool is exhausted.
std::uintptr_t ensure_paired_glibc_fs() {
  const std::uintptr_t bionic_fs = get_fs();
  const std::uintptr_t glibc_fs = glibc_fs_for(bionic_fs);
  if (glibc_fs != 0) return glibc_fs;
  const int idx = claim_attach_donor();
  if (idx < 0) return 0;
  const std::uintptr_t donor = g_attach_donor_pool[idx].load(std::memory_order_acquire);
  std::uintptr_t low = 0, high = 0;
  if (stack_region_for(reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)), &low, &high)) {
    g_borrower_stacks[idx].low = low;
    g_borrower_stacks[idx].size = high - low;
    g_borrower_stacks[idx].donor_fs.store(donor, std::memory_order_release);  // publish last
  }
  register_fs_pair(bionic_fs, donor);
  return donor;
}

// eglGetProcAddress is special: the pointers it returns are called DIRECTLY by the app (Impeller
// resolves its whole GL proc table this way), bypassing the static forwarders — so they must be
// %fs-swapping trampolines too, or Mesa's libGLdispatch reads _glapi_tls_Current under the app's
// bionic %fs and faults. real Mesa eglGetProcAddress, captured at install time.
void* (*g_mesa_egl_gpa)(const char*) = nullptr;

// A bump-allocated executable page pool + a cache of Mesa-ptr -> JIT trampoline, so each proc is
// wrapped once. Guarded by a mutex (eglGetProcAddress can be called from several threads).
std::mutex g_gl_jit_mu;
std::unordered_map<void*, void*> g_gl_jit_cache;
uint8_t* g_gl_jit_page = nullptr;
size_t g_gl_jit_used = 0, g_gl_jit_cap = 0;

// Emit a trampoline that resolves THIS thread's donor %fs (acl_gl_pick_donor, which clobbers only
// rax/r10/r11 so the live args survive), wrfsbase's to it, calls `mesa`, and restores the bionic %fs.
// Same per-thread-donor shape as the static forwarders; cached by `mesa`.
void* gl_jit_wrap(void* mesa) {
  if (mesa == nullptr || g_gl_pick_donor == nullptr) return nullptr;
  std::lock_guard<std::mutex> lk(g_gl_jit_mu);
  auto it = g_gl_jit_cache.find(mesa);
  if (it != g_gl_jit_cache.end()) return it->second;

  static constexpr size_t kStub = 176;  // frame + 8-slot arg copy + swap/call/restore
  if (g_gl_jit_page == nullptr || g_gl_jit_used + kStub > g_gl_jit_cap) {
    g_gl_jit_cap = 64 * 1024;
    g_gl_jit_page = static_cast<uint8_t*>(mmap(nullptr, g_gl_jit_cap, PROT_READ | PROT_WRITE | PROT_EXEC,
                                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    g_gl_jit_used = 0;
    if (g_gl_jit_page == MAP_FAILED) { g_gl_jit_page = nullptr; return nullptr; }
  }
  uint8_t* p = g_gl_jit_page + g_gl_jit_used;
  size_t n = 0;
  auto b = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t x : bytes) p[n++] = x; };
  auto imm64 = [&](std::uintptr_t v) { for (int i = 0; i < 8; ++i) p[n++] = uint8_t(v >> (8 * i)); };
  auto imm32 = [&](int32_t v) { for (int i = 0; i < 4; ++i) p[n++] = uint8_t(uint32_t(v) >> (8 * i)); };
  // Same frame + stack-arg copy as the static forwarders (see gen-gl-forwarders.py): a bare push
  // would shift a GL entry's stack-passed args and make Mesa read the saved %fs as an arg pointer.
  b({0x55});                                  // push %rbp
  b({0x48, 0x89, 0xE5});                      // mov %rsp,%rbp   ([rbp+16+8k] = app stack args)
  b({0x49, 0xBB}); imm64(reinterpret_cast<std::uintptr_t>(g_gl_pick_donor));  // movabs pick_donor,%r11
  b({0x41, 0xFF, 0xD3});                      // call *%r11      (rax = this thread's donor; args live)
  b({0xF3, 0x49, 0x0F, 0xAE, 0xC3});          // rdfsbase %r11   (caller bionic %fs)
  b({0x49, 0x89, 0xC2});                      // mov %rax,%r10   (donor)
  b({0x41, 0x53});                            // push %r11       ([rbp-8]  = bionic %fs)
  b({0x41, 0x52});                            // push %r10       ([rbp-16] = donor)
  for (int k = 7; k >= 0; --k) {              // copy 8 stack slots (slot7..slot0)
    b({0x48, 0x8B, 0x85}); imm32(16 + 8 * k); // mov 0x10+8k(%rbp),%rax
    b({0x50});                                // push %rax
  }
  b({0xF3, 0x49, 0x0F, 0xAE, 0xD2});          // wrfsbase %r10   (%fs = donor)
  b({0x49, 0xBA}); imm64(reinterpret_cast<std::uintptr_t>(mesa));  // movabs mesa, %r10
  b({0x41, 0xFF, 0xD2});                      // call *%r10      (Mesa; copied stack args in place)
  b({0x4C, 0x8B, 0x5D, 0xF8});                // mov -8(%rbp),%r11  (caller bionic %fs)
  b({0xF3, 0x49, 0x0F, 0xAE, 0xD3});          // wrfsbase %r11   (%fs = bionic)
  b({0x48, 0x89, 0xEC});                      // mov %rbp,%rsp
  b({0x5D});                                  // pop %rbp
  b({0xC3});                                  // ret
  g_gl_jit_used += kStub;
  g_gl_jit_cache.emplace(mesa, p);
  return p;
}

// Replacement for eglGetProcAddress installed into the EGL bridge slot: resolve `name` in host Mesa,
// then hand back a %fs-swapping trampoline instead of the raw dispatch pointer. Runs under the donor
// %fs (the forwarder swapped before calling this), so calling Mesa here is safe.
// The external-texture import shims (defined after g_native_gl_hooks, which they read). Host Mesa
// has neither entry point, so acl_egl_get_proc_address hands these back — wrapped like any Mesa ptr
// so they run under a donor %fs — for the engine's Texture/ImageExternalTexture path.
void* acl_egl_get_native_client_buffer_android(void* ahb);
void* acl_egl_create_image_khr(void* dpy, void* ctx, unsigned target, void* buffer, const int* attribs);

void* acl_egl_get_proc_address(const char* name) {
  if (g_mesa_egl_gpa == nullptr || name == nullptr) return nullptr;
  // The engine imports an AHardwareBuffer as an EGLImage via these two; translate to a dma-buf import.
  if (std::strcmp(name, "eglGetNativeClientBufferANDROID") == 0)
    return gl_jit_wrap(reinterpret_cast<void*>(&acl_egl_get_native_client_buffer_android));
  if (std::strcmp(name, "eglCreateImageKHR") == 0)
    return gl_jit_wrap(reinterpret_cast<void*>(&acl_egl_create_image_khr));
  void* mesa = g_mesa_egl_gpa(name);
  void* w = gl_jit_wrap(mesa);
  if (getenv("ACL_GL_DIAG") != nullptr)
    log::i(kTag, "eglGetProcAddress({}) -> mesa={} wrap={}", name, mesa, w);
  return w;
}

// Phase 3: acl-supplied hooks to back the app's NATIVE EGL with acl's GL device (see header). Set by
// the runtime; read by the two overrides below and by install_gl_forwarders. Run under the donor %fs.
NativeGlHooks g_native_gl_hooks;

// eglGetNativeClientBufferANDROID: host Mesa lacks it. The engine passes the returned EGLClientBuffer
// straight back into eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID, ...), so the AHardwareBuffer* IS
// the client buffer our create-image shim expects.
void* acl_egl_get_native_client_buffer_android(void* ahb) { return ahb; }

// eglCreateImageKHR: translate the engine's AHardwareBuffer import (EGL_NATIVE_BUFFER_ANDROID, value
// 0x3140) into a dma-buf EGLImage via the runtime-installed hook; forward every other target to Mesa
// unchanged. Runs under a donor %fs (the gpa wrapper swapped), so calling Mesa here is safe.
void* acl_egl_create_image_khr(void* dpy, void* ctx, unsigned target, void* buffer, const int* attribs) {
  constexpr unsigned kEglNativeBufferAndroid = 0x3140;
  static void* (*mesa_create_image)(void*, void*, unsigned, void*, const int*) = nullptr;
  if (mesa_create_image == nullptr && g_mesa_egl_gpa != nullptr)
    mesa_create_image = reinterpret_cast<decltype(mesa_create_image)>(g_mesa_egl_gpa("eglCreateImageKHR"));
  if (mesa_create_image == nullptr) return nullptr;
  if (target == kEglNativeBufferAndroid && g_native_gl_hooks.egl_image_from_ahb != nullptr)
    return g_native_gl_hooks.egl_image_from_ahb(dpy, buffer, attribs, reinterpret_cast<void*>(mesa_create_image));
  return mesa_create_image(dpy, ctx, target, buffer, attribs);
}

// eglGetDisplay override: hand back acl's EGLDisplay so the app's EGL shares acl's one Wayland
// connection (its window surface will live on acl's wl_surface). The display_id arg is ignored.
void* acl_native_egl_get_display(void*) {
  return g_native_gl_hooks.get_display != nullptr ? g_native_gl_hooks.get_display(g_native_gl_hooks.user)
                                                  : nullptr;
}
// eglCreateWindowSurface override: a wl_egl_window-backed EGLSurface on acl's session wl_surface,
// matching the app's chosen config. Ignores the ANativeWindow identity (one GL surface = the
// toplevel), exactly as the egl10 create_window_surface hook does. Args: dpy, config, win, attribs.
void* acl_native_egl_create_window_surface(void*, void* config, void*, const void*) {
  return g_native_gl_hooks.create_window_surface != nullptr
             ? g_native_gl_hooks.create_window_surface(g_native_gl_hooks.user, config)
             : nullptr;
}

// dlopen host Mesa, resolve the ABI-ordered symbols into two bridges, and write their addresses into
// the loaded bionic forwarder libs. `rt` is used only to reach the already-loaded forwarder libs via
// the bionic linker (load() of an already-open lib returns its handle). No-op unless ACL_GL_FORWARD.
void install_gl_forwarders(BionicRuntime* rt) {
#if defined(__x86_64__)
  if (getenv("ACL_GL_FORWARD") == nullptr) return;
  const char* sysroot = getenv("ACL_BIONIC_SYSROOT");
  if (sysroot == nullptr) return;
  static std::atomic<bool> done{false};
  if (done.exchange(true)) return;

  // Host Mesa first, so the donor thread's static TLS includes Mesa's initial-exec TLS.
  void* megl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  void* mgles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
  if (megl == nullptr || mgles == nullptr) {
    log::w(kTag, "GL forward: host Mesa dlopen failed (EGL={} GLESv2={})",
           megl != nullptr, mgles != nullptr);
    return;
  }
  ensure_gl_donor_pool();
  const int npub = g_gl_donor_published.load(std::memory_order_acquire);
  if (npub == 0) { log::w(kTag, "GL forward: no donor %fs"); return; }

  auto fill = [&](GlBridge& b, void* mh, const char* const* syms, int n) {
    b.donor_fs = 0;  // unused: forwarders resolve a per-thread donor via acl_gl_pick_donor
    int missing = 0;
    for (int i = 0; i < n; ++i) {
      b.fns[i] = dlsym(mh, syms[i]);
      if (b.fns[i] == nullptr) ++missing;
    }
    return missing;
  };
  static GlBridge egl_b{}, gles_b{};
  const int em = fill(egl_b, megl, kEglForwardSyms, 20);
  const int gm = fill(gles_b, mgles, kGlesForwardSyms, 106);

  // Publish the donor pool into libEGL.so (where acl_gl_pick_donor + the pool live) and grab
  // pick_donor's address for the JIT stubs. Written before the bridge is wired, so it is ready before
  // any GL call can reach a forwarder.
  {
    std::string err;
    void* he = rt->load((std::string(sysroot) + "/libEGL.so").c_str(), &err);
    if (he != nullptr) {
      g_gl_pick_donor = rt->symbol(he, "acl_gl_pick_donor");
      if (auto* pool = static_cast<std::uintptr_t*>(rt->symbol(he, "acl_gl_donor_pool"))) {
        for (int i = 0; i < kGlDonorPool; ++i)
          pool[i] = g_gl_donor_pool[i].load(std::memory_order_acquire);
      }
    }
    if (g_gl_pick_donor == nullptr)
      log::w(kTag, "GL forward: libEGL.so acl_gl_pick_donor not found ({})", err);
  }

  // Redirect the eglGetProcAddress slot to acl_egl_get_proc_address, which wraps the pointers Mesa
  // returns so the app can call them directly (see above). Keep Mesa's real one for it to delegate to.
  g_mesa_egl_gpa = reinterpret_cast<void* (*)(const char*)>(dlsym(megl, "eglGetProcAddress"));
  for (int i = 0; i < 20; ++i) {
    if (std::strcmp(kEglForwardSyms[i], "eglGetProcAddress") == 0) {
      egl_b.fns[i] = reinterpret_cast<void*>(&acl_egl_get_proc_address);
      break;
    }
  }

  // Phase 3: if the runtime supplied GL-session hooks, back the app's native EGL with acl's device —
  // eglGetDisplay -> acl's EGLDisplay, eglCreateWindowSurface -> acl's wl_surface. Otherwise these
  // slots stay pointed at host Mesa (the headless-probe path, which opens its own Wayland connection).
  if (g_native_gl_hooks.get_display != nullptr) {
    for (int i = 0; i < 20; ++i) {
      if (std::strcmp(kEglForwardSyms[i], "eglGetDisplay") == 0)
        egl_b.fns[i] = reinterpret_cast<void*>(&acl_native_egl_get_display);
      else if (std::strcmp(kEglForwardSyms[i], "eglCreateWindowSurface") == 0)
        egl_b.fns[i] = reinterpret_cast<void*>(&acl_native_egl_create_window_surface);
    }
    // Let ANativeWindow_getWidth/Height (in the loaded libandroid.so) report the live session size.
    // The callback is captureless and TLS-free, safe to invoke from libandroid under bionic %fs.
    if (g_native_gl_hooks.window_size != nullptr) {
      std::string err;
      void* h = rt->load((std::string(sysroot) + "/libandroid.so").c_str(), &err);
      if (h != nullptr) {
        if (void** s = static_cast<void**>(rt->symbol(h, "acl_anw_size_hook")))
          *s = reinterpret_cast<void*>(g_native_gl_hooks.window_size);
        if (void** u = static_cast<void**>(rt->symbol(h, "acl_anw_size_user"))) *u = g_native_gl_hooks.user;
      }
    }
  }

  auto wire = [&](const char* lib, const char* sym, GlBridge* b) -> bool {
    std::string err, path = std::string(sysroot) + "/" + lib;
    void* h = rt->load(path.c_str(), &err);  // already loaded as a libflutter dep -> same handle
    if (h == nullptr) { log::w(kTag, "GL forward: {} not loaded ({})", lib, err); return false; }
    void** slot = static_cast<void**>(rt->symbol(h, sym));
    if (slot == nullptr) { log::w(kTag, "GL forward: {} missing {}", lib, sym); return false; }
    *slot = b;
    return true;
  };
  const bool e = wire("libEGL.so", "acl_gl_egl_bridge", &egl_b);
  const bool g = wire("libGLESv2.so", "acl_gl_gles_bridge", &gles_b);
  log::i(kTag, "GL forwarders wired: EGL {}/{} GLES {}/{} donor-pool={} pick={} (egl={} gles={})",
         20 - em, 20, 106 - gm, 106, npub, g_gl_pick_donor != nullptr, e, g);
#else
  (void)rt;
#endif
}

void acl_signal_trampoline(int sig, siginfo_t* info, void* uctx) {
  const std::uintptr_t cur = get_fs();
  std::uintptr_t target = glibc_fs_for(cur);  // attached thread on its bionic %fs -> its glibc %fs
  if (target == 0 && !is_known_glibc_fs(cur)) {
    // Not a tracked bionic %fs, and not a known ART-glibc %fs => a bionic-spawned thread ART never
    // owned. Give it the null-ART %fs so ART's fault handler sees Thread::Current()==null and
    // declines. (A pure glibc non-ART thread also lands here; its self_tls_ is already null, so the
    // swap is harmless.) When the donor isn't up yet, fall through to the old behavior.
    target = g_null_art_fs.load(std::memory_order_acquire);
  }
  const bool swap = target != 0 && target != cur;
  if (swap) set_fs(target);
  const SavedSig s = (sig > 0 && sig <= 64) ? g_saved_sig[sig] : SavedSig{};
  if (s.handler != nullptr) {
    if ((s.flags & SA_SIGINFO) != 0)
      reinterpret_cast<void (*)(int, siginfo_t*, void*)>(s.handler)(sig, info, uctx);
    else
      reinterpret_cast<void (*)(int)>(s.handler)(sig);
  }
  if (swap) set_fs(cur);
}

// Wrap the kernel handlers ART/libsigchain installed for the fault + management signals. Idempotent;
// runs once (on the first bionic lib load, when ART is fully up). SIG_DFL/SIG_IGN are left alone —
// they touch no glibc TLS.
void install_signal_shims() {
  if (g_shims_installed.exchange(true)) return;
  ensure_null_art_fs();  // the shim needs the null-ART %fs ready before any bionic-thread signal
  static const int sigs[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGABRT, SIGQUIT, SIGSYS};
  int wrapped = 0;
  for (const int sig : sigs) {
    KSigaction old{};
    if (raw_rt_sigaction(sig, nullptr, &old) != 0) continue;
    if (old.handler == nullptr || old.handler == reinterpret_cast<void*>(1)) continue;  // SIG_DFL/IGN
    if (old.handler == reinterpret_cast<void*>(&acl_signal_trampoline)) continue;        // already
    g_saved_sig[sig].handler = old.handler;
    g_saved_sig[sig].flags = old.flags;
    KSigaction neu = old;  // keep the kernel restorer + mask + flags
    neu.handler = reinterpret_cast<void*>(&acl_signal_trampoline);
    neu.flags |= SA_SIGINFO;  // our trampoline is a 3-arg sigaction
    if (raw_rt_sigaction(sig, &neu, nullptr) == 0) ++wrapped;
  }
  log::i(kTag, "signal fs-shim installed over {} ART handler(s)", wrapped);
}

struct Mapped {
  std::uintptr_t base = 0, entry = 0, phdr = 0;
  std::uint16_t phnum = 0;
};

bool map_pie(const char* path, Mapped& out) {
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  Elf64_Ehdr eh{};
  if (::pread(fd, &eh, sizeof(eh), 0) != (ssize_t)sizeof(eh)) { ::close(fd); return false; }
  Elf64_Phdr ph[64];
  if (eh.e_phnum > 64) { ::close(fd); return false; }
  ::pread(fd, ph, eh.e_phnum * sizeof(Elf64_Phdr), eh.e_phoff);
  std::uintptr_t maxv = 0;
  for (int i = 0; i < eh.e_phnum; ++i)
    if (ph[i].p_type == PT_LOAD) maxv = std::max<std::uintptr_t>(maxv, ph[i].p_vaddr + ph[i].p_memsz);
  void* base = ::mmap(nullptr, page_up(maxv), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) { ::close(fd); return false; }
  const auto b = reinterpret_cast<std::uintptr_t>(base);
  for (int i = 0; i < eh.e_phnum; ++i) {
    const Elf64_Phdr& p = ph[i];
    if (p.p_type != PT_LOAD) continue;
    const std::uintptr_t start = b + p.p_vaddr, astart = page_down(start);
    const off_t aoff = static_cast<off_t>(page_down(p.p_offset));
    const std::size_t fmap = (start - astart) + p.p_filesz;
    const int prot = (p.p_flags & PF_R ? PROT_READ : 0) | (p.p_flags & PF_W ? PROT_WRITE : 0) |
                     (p.p_flags & PF_X ? PROT_EXEC : 0);
    if (fmap > 0 && ::mmap(reinterpret_cast<void*>(astart), fmap, prot | PROT_WRITE,
                           MAP_PRIVATE | MAP_FIXED, fd, aoff) == MAP_FAILED) {
      ::close(fd); return false;
    }
    const std::uintptr_t file_end = start + p.p_filesz, mem_end = start + p.p_memsz;
    if (mem_end > file_end) {
      const std::uintptr_t tail = std::min(page_up(file_end), mem_end);
      if (tail > file_end) std::memset(reinterpret_cast<void*>(file_end), 0, tail - file_end);
      if (page_up(file_end) < page_up(mem_end) &&
          ::mmap(reinterpret_cast<void*>(page_up(file_end)), page_up(mem_end) - page_up(file_end),
                 prot | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) {
        ::close(fd); return false;
      }
    }
    if (!(prot & PROT_WRITE) && fmap > 0)
      ::mprotect(reinterpret_cast<void*>(astart), page_up(fmap), prot);
  }
  ::close(fd);
  out.base = b;
  out.entry = b + eh.e_entry;
  out.phdr = b + eh.e_phoff;
  out.phnum = eh.e_phnum;
  return true;
}

std::uintptr_t find_sym(const char* path, std::uintptr_t base, const char* name) {
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  const off_t sz = ::lseek(fd, 0, SEEK_END);
  void* m = ::mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (m == MAP_FAILED) return 0;
  const auto* f = reinterpret_cast<const std::uint8_t*>(m);
  const auto& eh = *reinterpret_cast<const Elf64_Ehdr*>(f);
  const auto* sh = reinterpret_cast<const Elf64_Shdr*>(f + eh.e_shoff);
  std::uintptr_t result = 0;
  for (int i = 0; i < eh.e_shnum && result == 0; ++i) {
    if (sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM) continue;
    const auto* sym = reinterpret_cast<const Elf64_Sym*>(f + sh[i].sh_offset);
    const char* str = reinterpret_cast<const char*>(f + sh[sh[i].sh_link].sh_offset);
    for (std::size_t s = 0, n = sh[i].sh_size / sizeof(Elf64_Sym); s < n; ++s)
      if (sym[s].st_value && std::strcmp(str + sym[s].st_name, name) == 0) {
        result = base + sym[s].st_value;
        break;
      }
  }
  ::munmap(m, sz);
  return result;
}

// Synthetic KernelArgumentBlock + LD_LIBRARY_PATH storage (single-bootstrap → static is fine).
std::uintptr_t g_block[256];
char g_argv0[] = "acl-linker64";
char g_ldpath[4096];

void* build_kab(const Mapped& lk, const char* stub_path, const std::string& ld) {
  std::snprintf(g_ldpath, sizeof(g_ldpath), "LD_LIBRARY_PATH=%s", ld.c_str());
  std::size_t k = 0;
  g_block[k++] = 2;
  g_block[k++] = reinterpret_cast<std::uintptr_t>(g_argv0);
  g_block[k++] = reinterpret_cast<std::uintptr_t>(const_cast<char*>(stub_path));
  g_block[k++] = 0;
  g_block[k++] = reinterpret_cast<std::uintptr_t>(g_ldpath);
  g_block[k++] = 0;
  auto aux = [&](std::uint64_t t, std::uint64_t v) { g_block[k++] = t; g_block[k++] = v; };
  aux(AT_PAGESZ, kPage);
  aux(AT_CLKTCK, getauxval(AT_CLKTCK));
  aux(AT_HWCAP, getauxval(AT_HWCAP));
  aux(AT_HWCAP2, getauxval(AT_HWCAP2));
  aux(AT_SYSINFO_EHDR, getauxval(AT_SYSINFO_EHDR));
  aux(AT_RANDOM, getauxval(AT_RANDOM));
  aux(AT_PLATFORM, getauxval(AT_PLATFORM));
  aux(AT_SECURE, 0);
  aux(AT_UID, getuid());
  aux(AT_EUID, geteuid());
  aux(AT_GID, getgid());
  aux(AT_EGID, getegid());
  aux(AT_FLAGS, 0);
  aux(AT_EXECFN, reinterpret_cast<std::uintptr_t>(g_argv0));
  aux(AT_BASE, lk.base);
  aux(AT_PHDR, lk.phdr);
  aux(AT_PHNUM, lk.phnum);
  aux(AT_ENTRY, lk.entry);
  aux(AT_NULL, 0);
  return g_block;
}

// The signals bionic's crash handler takes over during the bootstrap. linker_main calls
// linker_debuggerd_init(), whose debuggerd_register_handlers() (system/core/debuggerd/include/
// debuggerd/handler.h) sigaction()s exactly this set. BIONIC_SIGNAL_DEBUGGER is bionic's
// __SIGRTMIN + 3; __SIGRTMIN is the kernel's 32, so the raw number is 35 — glibc's SIGRTMIN
// macro is 34 (it reserves 32/33 for NPTL) and would restore the wrong signal, hence the literal.
constexpr int kBionicSignals[] = {SIGABRT, SIGBUS, SIGFPE,  SIGILL, SIGSEGV,
                                  SIGSTKFLT, SIGSYS, SIGTRAP, 32 + 3};

// Bootstrap the linker; on success fill the loader fn ptrs and bionic %fs. Returns true/false.
// (%fs — and the host's signal state, see below — are restored to glibc/host before returning.)
bool bootstrap_x86_64(const BionicConfig& cfg, std::uintptr_t* out_bionic_fs, void** out_dlopen,
                      void** out_dlsym, void** out_dlerror, void** out_shared_globals) {
  Mapped lk;
  if (!map_pie(cfg.linker.c_str(), lk)) return false;
  const std::uintptr_t init_addr = find_sym(cfg.linker.c_str(), lk.base, "__dl___linker_init");
  void* dlopen_a = reinterpret_cast<void*>(find_sym(cfg.linker.c_str(), lk.base, "__dl___loader_dlopen"));
  void* dlsym_a = reinterpret_cast<void*>(find_sym(cfg.linker.c_str(), lk.base, "__dl___loader_dlsym"));
  void* dlerror_a = reinterpret_cast<void*>(find_sym(cfg.linker.c_str(), lk.base, "__dl___loader_dlerror"));
  if (!init_addr || !dlopen_a || !dlsym_a) return false;

  std::string ld = cfg.sysroot;
  for (const std::string& p : cfg.extra_lib_paths) { ld += ':'; ld += p; }
  void* kab = build_kab(lk, cfg.stub.c_str(), ld);

  // Signal state is HOST state, and the linker's init claims two pieces of it for bionic:
  //   1. the crash handlers (linker_debuggerd_init -> kBionicSignals above), and
  //   2. THIS thread's sigaltstack — __libc_init_main_thread_late -> __init_additional_stacks ->
  //      __init_alternate_signal_stack (bionic/libc/bionic/pthread_create.cpp) mmaps a guarded
  //      SIGNAL_STACK_SIZE region and sigaltstack()s it in.
  // acl must keep both. ART installs its own SIGSEGV handler (implicit null checks are SEGVs it
  // fixes up), and on host art::Thread::TearDownAlternateSignalStack (art/runtime/thread_linux.cc)
  // reads back whatever sigaltstack is installed and `delete[]`s it — with bionic's mmap there,
  // ART shutdown free()s a bionic guard-page address and SIGSEGVs. So snapshot the host's signal
  // state, let the linker do as it likes, then put it back: acl stays the crash handler and the
  // owner of its own alt stack. Bionic's handlers/stack stay mapped, just unreferenced — a bionic
  // crash is then reported by the host, which is what we want from a translation layer anyway.
  stack_t saved_ss{};
  const bool have_ss = ::sigaltstack(nullptr, &saved_ss) == 0;
  struct sigaction saved_sa[std::size(kBionicSignals)];
  bool have_sa[std::size(kBionicSignals)];
  for (std::size_t i = 0; i < std::size(kBionicSignals); ++i)
    have_sa[i] = ::sigaction(kBionicSignals[i], nullptr, &saved_sa[i]) == 0;

  const std::uintptr_t glibc_fs = get_fs();
  reinterpret_cast<std::uintptr_t (*)(void*)>(init_addr)(kab);  // sets %fs -> permanent bionic TCB
  *out_bionic_fs = get_fs();
  set_fs(glibc_fs);

  // Back on glibc %fs, so these libc calls are safe again.
  for (std::size_t i = 0; i < std::size(kBionicSignals); ++i)
    if (have_sa[i]) ::sigaction(kBionicSignals[i], &saved_sa[i], nullptr);
  if (have_ss) ::sigaltstack(&saved_ss, nullptr);  // SS_DISABLE when the host had none: still correct

  *out_dlopen = dlopen_a;
  *out_dlsym = dlsym_a;
  *out_dlerror = dlerror_a;
  // The linker's view of libc's shared globals — the static TLS layout + module table that
  // attach_current_thread needs. libc itself imports this symbol from the linker.
  *out_shared_globals = reinterpret_cast<void*>(
      find_sym(cfg.linker.c_str(), lk.base, "__loader_shared_globals"));
  return true;
}

}  // namespace

// --- glue that flips %fs; must stay in this -fno-stack-protector TU ---

void ndk_set_fs(std::uintptr_t v) { set_fs(v); }
std::uintptr_t ndk_get_fs() { return get_fs(); }

// Interpose pthread_getattr_np so ART sees a donor-attached bionic thread's REAL stack, not the
// parked donor's. libart calls pthread_getattr_np(pthread_self(), &attr) inside
// AttachCurrentThreadInternal, under the donor %fs acl swapped to; for a %fs recorded as an attach
// donor we return the borrowing thread's stack (captured at pairing), otherwise delegate to glibc's
// real implementation. The acl executable's definition interposes libart's undefined reference to
// pthread_getattr_np@GLIBC_2.2.5 (an unversioned definition satisfies a versioned reference). Only
// touches globals + the caller's (valid glibc) %fs; lives in this -fno-stack-protector TU.
extern "C" __attribute__((visibility("default"))) int pthread_getattr_np(pthread_t thread,
                                                                          pthread_attr_t* attr) {
  const std::uintptr_t cur = get_fs();
  if (cur != 0 && attr != nullptr) {
    for (int i = 0; i < kAttachDonorPool; ++i) {
      if (g_borrower_stacks[i].donor_fs.load(std::memory_order_acquire) != cur) continue;
      const std::uintptr_t low = g_borrower_stacks[i].low;
      const std::uintptr_t size = g_borrower_stacks[i].size;
      if (low != 0 && size != 0) {
        pthread_attr_init(attr);
        pthread_attr_setstack(attr, reinterpret_cast<void*>(low), size);
        pthread_attr_setguardsize(attr, 0);
        return 0;
      }
      break;
    }
  }
  static int (*real)(pthread_t, pthread_attr_t*) = nullptr;
  if (real == nullptr) {
    void* p = dlvsym(RTLD_NEXT, "pthread_getattr_np", "GLIBC_2.2.5");
    if (p == nullptr) p = dlsym(RTLD_NEXT, "pthread_getattr_np");
    real = reinterpret_cast<int (*)(pthread_t, pthread_attr_t*)>(p);
  }
  return real != nullptr ? real(thread, attr) : -1;
}

bool BionicRuntime_bootstrap(const BionicConfig& cfg, std::uintptr_t* fs, void** dlo, void** dls,
                             void** dle, void** sg) {
  return bootstrap_x86_64(cfg, fs, dlo, dls, dle, sg);
}

#else  // non-x86_64: bootstrap not yet ported (arm64 swaps TPIDR_EL0 / auxv / relocs)

void ndk_set_fs(std::uintptr_t) {}
std::uintptr_t ndk_get_fs() { return 0; }
bool BionicRuntime_bootstrap(const BionicConfig&, std::uintptr_t*, void**, void**, void**, void**) {
  return false;
}

#endif

// --- portable class surface (compiled in the same TU; no stack-protector) ---

namespace {
BionicRuntime* g_rt = nullptr;
}

BionicRuntime* BionicRuntime::get() noexcept { return g_rt; }

BionicRuntime* BionicRuntime::instance(const BionicConfig& cfg, std::string* error) {
  if (g_rt) return g_rt;
  std::string why;
  if (!cfg.valid(&why)) { if (error) *error = why; return nullptr; }
#if !defined(__x86_64__)
  if (error) *error = "bionic loader: only x86_64 host is supported so far (arm64 TLS port pending)";
  return nullptr;
#endif
  static BionicRuntime rt;
  rt.glibc_fs_ = ndk_get_fs();  // this (main) thread's glibc thread pointer, before bootstrap
  std::uintptr_t fs = 0;
  void *dlo = nullptr, *dls = nullptr, *dle = nullptr, *sg = nullptr;
  if (!BionicRuntime_bootstrap(cfg, &fs, &dlo, &dls, &dle, &sg)) {
    if (error) *error = "bionic loader: linker bootstrap failed (see log)";
    return nullptr;
  }
  rt.bionic_fs_ = fs;
  rt.dlopen_fn_ = dlo;
  rt.dlsym_fn_ = dls;
  rt.dlerror_fn_ = dle;
  rt.shared_globals_fn_ = sg;
  g_rt = &rt;
#if defined(__x86_64__)
  // The bootstrapping thread is attached by construction: __linker_init built it a real bionic TCB
  // and migrated it to a permanent mapping. Record that as this thread's, and measure the slot's
  // fixed displacement from the thread pointer for the trampolines — done here, on this thread,
  // under its glibc %fs (ndk_get_fs() is its glibc base, == rt.glibc_fs_).
  t_bionic_fs = fs;
  register_fs_pair(fs, rt.glibc_fs_);  // so this thread's up-calls find their way back to glibc
  g_tls_disp = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(&t_bionic_fs)) -
               static_cast<std::int64_t>(rt.glibc_fs_);
  if (g_tls_disp < INT32_MIN || g_tls_disp > INT32_MAX) {
    if (error) *error = "bionic loader: TLS slot displacement does not fit a disp32";
    g_rt = nullptr;
    return nullptr;  // the trampoline encodes it as a disp32; refuse rather than emit a wrong one
  }
#endif
  log::i(kTag, "bionic linker bootstrapped in-process (fs={:#x})", fs);
  return g_rt;
}

void* BionicRuntime::bootstrap_thread_struct() const noexcept {
#if defined(__x86_64__) && ACL_BIONIC_OFFSETS_AVAILABLE
  if (bionic_fs_ == 0) return nullptr;
  return reinterpret_cast<void**>(bionic_fs_)[ACL_BIONIC_TLS_SLOT_THREAD_ID_];
#else
  return nullptr;
#endif
}

std::uintptr_t BionicRuntime::current_thread_fs() noexcept {
#if defined(__x86_64__)
  return t_bionic_fs;
#else
  return 0;
#endif
}

// Run bionic code as THIS thread — never as the bootstrapping thread. Using rt.bionic_fs_ here
// would hand a second thread the main thread's TCB, which is the whole hazard attach exists to
// remove (shared errno / pthread_self / TLS keys / DTV). Attaching is explicit
// (attach_current_thread), so an unattached thread here is a bug in acl, not something to paper
// over by borrowing another thread's TCB.
BionicRuntime::RunScope::RunScope(BionicRuntime&) : saved_(ndk_get_fs()) {
  const std::uintptr_t fs = BionicRuntime::current_thread_fs();
  if (fs == 0) acl_ndk_unattached();  // [[noreturn]]
  ndk_set_fs(fs);
}
BionicRuntime::RunScope::~RunScope() { ndk_set_fs(saved_); }

// ── attach-as-bionic ─────────────────────────────────────────────────────────────────────────
// Build this thread a bionic TCB the way bionic's own __allocate_thread does, using the layout
// measured from the staged bionic (bionic_offsets.h — never hand-copied structs; see
// tools/gen-bionic-offsets.sh). Deliberately NOT a copy of the bootstrapping thread's block: that
// block holds bionic_tls (pthread key_data, locale, the cleanup stack), which must not be
// inherited, and its module TLS holds that thread's current values where a new thread needs the
// pristine init images. So: zero, then re-init.
std::uintptr_t BionicRuntime::attach_current_thread(std::string* error) {
#if !defined(__x86_64__)
  if (error) *error = "attach: only x86_64 is supported so far (arm64 TLS port pending)";
  return 0;
#elif !ACL_BIONIC_OFFSETS_AVAILABLE
  if (error) *error =
      "attach: built without bionic offsets (no AOSP at build time; see tools/gen-bionic-offsets.sh)";
  return 0;
#else
  if (t_bionic_fs != 0) return t_bionic_fs;  // idempotent
  auto fail = [&](const char* why) -> std::uintptr_t {
    if (error) *error = why;
    return 0;
  };
  if (shared_globals_fn_ == nullptr) return fail("attach: linker exposed no __loader_shared_globals");

  // 1. The linker owns the authoritative layout of a static TLS block; libc reads it through the
  //    same pointer. Reading it needs no bionic %fs — it is plain memory.
  const auto* sg = static_cast<const char*>(
      reinterpret_cast<void* (*)()>(shared_globals_fn_)());
  if (sg == nullptr) return fail("attach: __loader_shared_globals returned null");
  auto sz_at = [](const char* p, std::size_t off) { return *reinterpret_cast<const std::size_t*>(p + off); };
  const char* layout = sg + ACL_BIONIC_OFF_SG_STATIC_TLS_LAYOUT;
  const std::size_t tls_size = sz_at(layout, ACL_BIONIC_OFF_LAYOUT_SIZE);
  const std::size_t tls_align = sz_at(layout, ACL_BIONIC_OFF_LAYOUT_ALIGNMENT);
  const std::size_t off_tcb = sz_at(layout, ACL_BIONIC_OFF_LAYOUT_BIONIC_TCB);
  const std::size_t off_tls = sz_at(layout, ACL_BIONIC_OFF_LAYOUT_BIONIC_TLS);
  // If the offsets we were built with do not describe the bionic we loaded, these are garbage.
  // Refuse: a bad TCB corrupts memory somewhere else rather than failing here.
  if (tls_size == 0 || tls_size > (1u << 24) || off_tcb >= tls_size || off_tls >= tls_size ||
      tls_align == 0 || tls_align > kPage)
    return fail("attach: implausible static TLS layout — the staged bionic does not match the "
                "offsets acl was built with (rebuild with the same $AOSP)");

  // 2. A fresh, zeroed block (mmap is zero-filled), page-aligned, which covers any bionic
  //    alignment up to a page.
  void* block = ::mmap(nullptr, page_up(tls_size), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (block == MAP_FAILED) return fail("attach: mmap of the static TLS block failed");
  auto* blk = static_cast<char*>(block);

  // 3. Seed each static module's TLS from its init image (bionic's __init_static_tls). The module
  //    table is read without bionic's rwlock: taking it would mean calling bionic code, and this
  //    thread has no TCB yet — that is the chicken-and-egg attach exists to break. Safe in
  //    practice because the static modules are fixed after startup; a dlopen racing an attach
  //    could still move the table, which is a known narrow race (docs/ndk-scope.md).
  const char* mods = sg + ACL_BIONIC_OFF_SG_TLS_MODULES;
  const std::size_t module_count = sz_at(mods, ACL_BIONIC_OFF_TLSMODULES_MODULE_COUNT);
  const char* table = *reinterpret_cast<const char* const*>(mods + ACL_BIONIC_OFF_TLSMODULES_MODULE_TABLE);
  for (std::size_t i = 0; table != nullptr && i < module_count; ++i) {
    const char* m = table + i * ACL_BIONIC_SIZEOF_TLSMODULE;
    const std::size_t static_offset = sz_at(m, ACL_BIONIC_OFF_TLSMODULE_STATIC_OFFSET);
    if (static_offset == SIZE_MAX) break;  // all static modules precede the dynamic ones
    const std::size_t init_size = sz_at(m, ACL_BIONIC_OFF_TLSMODULE_SEG_INIT_SIZE);
    const void* init_ptr = *reinterpret_cast<const void* const*>(m + ACL_BIONIC_OFF_TLSMODULE_SEG_INIT_PTR);
    if (init_size == 0 || init_ptr == nullptr) continue;  // .tbss: stays zero
    if (static_offset + init_size > tls_size) {
      ::munmap(block, page_up(tls_size));
      return fail("attach: a TLS module's init image does not fit the block (layout mismatch)");
    }
    std::memcpy(blk + static_offset, init_ptr, init_size);
  }

  // 4. The thread struct. Bionic carves this out of the stack it allocates; acl did not allocate
  //    this thread's stack, so give it its own mapping (zeroed, and never freed — see the
  //    thread-exit gap in docs/ndk-scope.md).
  void* thread = ::mmap(nullptr, page_up(ACL_BIONIC_SIZEOF_PTHREAD_INTERNAL), PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (thread == MAP_FAILED) {
    ::munmap(block, page_up(tls_size));
    return fail("attach: mmap of pthread_internal_t failed");
  }
  auto* thr = static_cast<char*>(thread);
  *reinterpret_cast<pid_t*>(thr + ACL_BIONIC_OFF_PTHREAD_TID) =
      static_cast<pid_t>(::syscall(SYS_gettid));
  *reinterpret_cast<void**>(thr + ACL_BIONIC_OFF_PTHREAD_BIONIC_TLS) = blk + off_tls;
  // Describe the stack this code is REALLY on — the glibc thread's — so bionic's unwinder and
  // pthread_getattr_np see the truth rather than a stack acl never allocated.
  void* stack_base = nullptr;
  std::size_t stack_size = 0;
  pthread_attr_t gattr;
  if (::pthread_getattr_np(::pthread_self(), &gattr) == 0) {
    ::pthread_attr_getstack(&gattr, &stack_base, &stack_size);
    ::pthread_attr_destroy(&gattr);
  }
  if (stack_base != nullptr) {
    *reinterpret_cast<void**>(thr + ACL_BIONIC_OFF_PTHREAD_ATTR + ACL_BIONIC_OFF_ATTR_STACK_BASE) = stack_base;
    *reinterpret_cast<std::size_t*>(thr + ACL_BIONIC_OFF_PTHREAD_ATTR + ACL_BIONIC_OFF_ATTR_STACK_SIZE) = stack_size;
    *reinterpret_cast<std::uintptr_t*>(thr + ACL_BIONIC_OFF_PTHREAD_STACK_TOP) =
        reinterpret_cast<std::uintptr_t>(stack_base) + stack_size;
  }

  // 5. The TCB slots. On x86_64 the thread pointer IS the TCB (MIN_TLS_SLOT == 0, asserted by the
  //    bionic-offsets test); an arm64 port must use StaticTlsLayout::offset_thread_pointer().
  auto** tcb = reinterpret_cast<void**>(blk + off_tcb);
  tcb[ACL_BIONIC_TLS_SLOT_SELF_] = tcb;        // x86_64 reads TLS vars through %fs:0
  tcb[ACL_BIONIC_TLS_SLOT_THREAD_ID_] = thread;
  tcb[ACL_BIONIC_TLS_SLOT_BIONIC_TLS_] = blk + off_tls;
  // The stack canary is process-wide; take it from the bootstrapping thread's TCB rather than
  // dlsym'ing __stack_chk_guard. A wrong value here would fail every bionic function's epilogue.
  tcb[ACL_BIONIC_TLS_SLOT_STACK_GUARD_] =
      reinterpret_cast<void**>(bionic_fs_)[ACL_BIONIC_TLS_SLOT_STACK_GUARD_];
  // An all-zero DTV: generation 0 is kTlsGenerationNone, so the first dynamic-TLS access lazily
  // builds this thread's real DTV, and __free_dynamic_tls's chain-walk stops here instead of
  // handing our memory to bionic's allocator. One process-wide instance is what bionic itself
  // does (static const TlsDtv zero_dtv). The slot points at the *generation field*, not the base.
  tcb[ACL_BIONIC_TLS_SLOT_DTV_] = g_zero_dtv + ACL_BIONIC_OFF_TLSDTV_GENERATION;

  t_bionic_fs = reinterpret_cast<std::uintptr_t>(tcb);
  // The up-call thunks run under bionic %fs, where this thread's glibc TLS is unreachable, so the
  // pairing has to be looked up by bionic %fs — register it before any native can up-call.
  register_fs_pair(t_bionic_fs, ndk_get_fs());

  // Hand everything this thread owns to a glibc key destructor, which runs on this thread when it
  // exits (see release_attached_thread). Failing to arm that is a leak, not a correctness problem,
  // so it is reported rather than fatal.
  std::call_once(g_attached_key_once, [] {
    if (::pthread_key_create(&g_attached_key, &release_attached_thread) != 0)
      log::w(kTag, "pthread_key_create failed; attached threads will leak their TCB at exit");
  });
  auto* owned = new AttachedThread{block, page_up(tls_size), thread,
                                   page_up(ACL_BIONIC_SIZEOF_PTHREAD_INTERNAL), t_bionic_fs};
  if (::pthread_setspecific(g_attached_key, owned) != 0) {
    log::w(kTag, "pthread_setspecific failed; this thread will leak its bionic TCB at exit");
    delete owned;
  }
  log::i(kTag, "attached thread {} to bionic (tcb={:#x})", ::syscall(SYS_gettid), t_bionic_fs);
  return t_bionic_fs;
#endif
}

void* BionicRuntime::load(const char* abs_path, std::string* error) {
  if (!abs_path || abs_path[0] != '/') {
    if (error) *error = "bionic load: absolute path required";
    return nullptr;
  }
  void* h = nullptr;
  const char* why = nullptr;
  {
    RunScope scope(*this);
    h = reinterpret_cast<void* (*)(const char*, int, const void*)>(dlopen_fn_)(
        abs_path, 2 /*RTLD_NOW*/, dlopen_fn_);
    // Take only the POINTER here. Building the std::string inside this scope would call glibc's
    // malloc under bionic %fs, and malloc reads its tcache out of glibc TLS — so the failure path
    // would segfault inside the allocator instead of reporting why the load failed. (dlerror's
    // buffer is bionic's, but reading it from glibc %fs is just a memory read.)
    if (h == nullptr && dlerror_fn_ != nullptr)
      why = reinterpret_cast<const char* (*)()>(dlerror_fn_)();
  }
  if (h == nullptr && error != nullptr) *error = why != nullptr ? why : "dlopen failed";
  return h;
}

void* BionicRuntime::symbol(void* handle, const char* name) {
  if (!handle || !name) return nullptr;
  RunScope scope(*this);
  return reinterpret_cast<void* (*)(void*, const char*, const void*)>(dlsym_fn_)(handle, name,
                                                                                 dlsym_fn_);
}

// --- bidirectional-boundary self-test (the JNI up-call mechanism, in miniature) ---
#if defined(__x86_64__)
namespace {
int (*g_glibc_cb)(int) = nullptr;
std::uintptr_t g_up_glibc_fs = 0, g_up_bionic_fs = 0;
// Entered under bionic %fs (called from the bionic .so). Restore glibc for the callback, then
// bionic. Lives in this -fno-stack-protector TU. Models a JNIEnv function-pointer trampoline.
int upcall_trampoline(int x) {
  ndk_set_fs(g_up_glibc_fs);
  int r = g_glibc_cb(x);
  ndk_set_fs(g_up_bionic_fs);
  return r;
}
}  // namespace
#endif

int BionicRuntime::run_boundary_selftest(void* bionic_fn, int (*cb)(int), int x) {
#if defined(__x86_64__)
  if (!bionic_fn || !cb) return -1;
  g_glibc_cb = cb;
  g_up_glibc_fs = glibc_fs_;
  g_up_bionic_fs = bionic_fs_;
  RunScope scope(*this);  // %fs -> bionic for the down-call
  return reinterpret_cast<int (*)(int (*)(int), int)>(bionic_fn)(&upcall_trampoline, x);
#else
  (void)bionic_fn;
  (void)cb;
  (void)x;
  return -1;
#endif
}

// --- JNIEnv/JavaVM boundary trampolines (Rung 4b): each thunk runs the real (glibc/ART) function
// under glibc %fs, so a bionic .so's JNI_OnLoad can call GetEnv/FindClass/RegisterNatives across
// the boundary. Only the JNI_OnLoad/RegisterNatives slots are thunked so far. ---
#if ACL_HAVE_JNI
#if defined(__x86_64__)
namespace {
// Wrapper handles: first member is the fn table (JNIEnv/JavaVM ABI), then the real object.
struct WrapEnv {
  const JniEnvTable* functions;
  JNIEnv* real;
};
struct WrapVM {
  const JniVmTable* functions;
  JavaVM* real;
  JNIEnv* wrap_env;
};
JniEnvTable g_env_tbl;
JniVmTable g_vm_tbl;
WrapVM g_wrap_vm;

// The wrapped JNIEnv is PER-THREAD: a JNIEnv belongs to one thread, so wrapping ART's env in a
// single global would hand every thread the env of whichever thread loaded the library. The
// down-call trampoline fills this in for the calling thread (functions + the real env ART just
// passed it) before swapping %fs, so every up-call out of a native lands on that thread's env.
// initial-exec for the same reason as t_bionic_fs: the trampoline addresses it off the thread
// pointer with a fixed displacement.
__thread __attribute__((tls_model("initial-exec"))) WrapEnv t_wrap_env;
std::int64_t g_wrap_disp = 0;  // &t_wrap_env - glibc %fs base; constant across threads

// Entered under the CALLING thread's bionic %fs (from its native code); restore that same thread's
// glibc %fs for the real call, then put its bionic one back. Never a constant pair: restoring the
// bootstrapping thread's glibc %fs here would detach whatever thread is actually up-calling from
// its own TLS — the same corruption the down-call trampoline avoids (ART's Thread::Current() is a
// TLS read). Saving what we entered with also composes with re-entrancy exactly as before.
struct ToGlibc {
  std::uintptr_t bionic_fs;
  ToGlibc() : bionic_fs(ndk_get_fs()) {
    const std::uintptr_t glibc_fs = glibc_fs_for(bionic_fs);
    if (glibc_fs == 0) acl_ndk_unattached();  // [[noreturn]]: no pairing => whose TLS is this?
    ndk_set_fs(glibc_fs);
  }
  ~ToGlibc() { ndk_set_fs(bionic_fs); }
};

#define R() JNIEnv* r = reinterpret_cast<WrapEnv*>(e)->real
#define RF r->functions

// Fixed-signature thunks: run the real slot under glibc %fs, forwarding args unchanged.
#define T0(RET, N)              RET JNICALL w_##N(JNIEnv* e){ R(); ToGlibc g; return RF->N(r); }
#define T0V(N)                  void JNICALL w_##N(JNIEnv* e){ R(); ToGlibc g; RF->N(r); }
#define T1(RET, N, A1)          RET JNICALL w_##N(JNIEnv* e, A1 a1){ R(); ToGlibc g; return RF->N(r,a1); }
#define T1V(N, A1)              void JNICALL w_##N(JNIEnv* e, A1 a1){ R(); ToGlibc g; RF->N(r,a1); }
#define T2(RET, N, A1, A2)      RET JNICALL w_##N(JNIEnv* e, A1 a1, A2 a2){ R(); ToGlibc g; return RF->N(r,a1,a2); }
#define T2V(N, A1, A2)          void JNICALL w_##N(JNIEnv* e, A1 a1, A2 a2){ R(); ToGlibc g; RF->N(r,a1,a2); }
#define T3(RET, N, A1, A2, A3)  RET JNICALL w_##N(JNIEnv* e, A1 a1, A2 a2, A3 a3){ R(); ToGlibc g; return RF->N(r,a1,a2,a3); }
#define T3V(N, A1, A2, A3)      void JNICALL w_##N(JNIEnv* e, A1 a1, A2 a2, A3 a3){ R(); ToGlibc g; RF->N(r,a1,a2,a3); }
#define T4(RET, N, A1,A2,A3,A4) RET JNICALL w_##N(JNIEnv* e, A1 a1,A2 a2,A3 a3,A4 a4){ R(); ToGlibc g; return RF->N(r,a1,a2,a3,a4); }
#define T4V(N, A1,A2,A3,A4)     void JNICALL w_##N(JNIEnv* e, A1 a1,A2 a2,A3 a3,A4 a4){ R(); ToGlibc g; RF->N(r,a1,a2,a3,a4); }

T0(jint, GetVersion)
T4(jclass, DefineClass, const char*, jobject, const jbyte*, jsize)
// The class loader of the app that last loaded a bionic lib (set by the nativeLoad hook). A JNI
// FindClass from that lib's JNI_OnLoad / native threads searches the system class loader, so app
// classes (io/flutter/embedding/engine/FlutterJNI, ...) miss — on real Android ART sets a
// class-loader override around JNI_OnLoad from Runtime.nativeLoad's ClassLoader arg. We do the
// equivalent as a FindClass fallback via this loader. A global ref; one live at a time is enough.
jobject g_app_class_loader = nullptr;

jclass JNICALL w_FindClass(JNIEnv* e, const char* a1) {
  R(); ToGlibc g;
  jclass c = RF->FindClass(r, a1);
  bool via_loader = false;
  if (c == nullptr && g_app_class_loader != nullptr && a1 != nullptr) {
    RF->ExceptionClear(r);  // FindClass left a pending ClassNotFound/NoClassDefFound; retry cleanly
    std::string dotted(a1);
    for (char& ch : dotted) if (ch == '/') ch = '.';  // JNI 'a/B' -> dotted 'a.B' for loadClass
    jstring jn = RF->NewStringUTF(r, dotted.c_str());
    jclass loaderCls = RF->GetObjectClass(r, g_app_class_loader);
    jmethodID lc = loaderCls != nullptr
        ? RF->GetMethodID(r, loaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;")
        : nullptr;
    if (lc != nullptr) {
      c = static_cast<jclass>(RF->CallObjectMethod(r, g_app_class_loader, lc, jn));
      if (RF->ExceptionCheck(r)) { RF->ExceptionClear(r); c = nullptr; } else if (c != nullptr) via_loader = true;
    }
    if (jn != nullptr) RF->DeleteLocalRef(r, jn);
    if (loaderCls != nullptr) RF->DeleteLocalRef(r, loaderCls);
  }
  static const bool diag = std::getenv("ACL_NDK_JNI_DIAG") != nullptr;
  if (diag) log::i(kTag, "jni FindClass({}) -> {}{}", a1 ? a1 : "?", c ? "ok" : "NULL",
                   via_loader ? " (via app loader)" : "");
  return c;
}
T1(jmethodID, FromReflectedMethod, jobject)
T1(jfieldID, FromReflectedField, jobject)
T3(jobject, ToReflectedMethod, jclass, jmethodID, jboolean)
T1(jclass, GetSuperclass, jclass)
T2(jboolean, IsAssignableFrom, jclass, jclass)
T3(jobject, ToReflectedField, jclass, jfieldID, jboolean)
T1(jint, Throw, jthrowable)
T2(jint, ThrowNew, jclass, const char*)
T0(jthrowable, ExceptionOccurred)
T0V(ExceptionDescribe)
T0V(ExceptionClear)
T1V(FatalError, const char*)
T1(jint, PushLocalFrame, jint)
T1(jobject, PopLocalFrame, jobject)
T1(jobject, NewGlobalRef, jobject)
T1V(DeleteGlobalRef, jobject)
T1V(DeleteLocalRef, jobject)
T2(jboolean, IsSameObject, jobject, jobject)
T1(jobject, NewLocalRef, jobject)
T1(jint, EnsureLocalCapacity, jint)
T1(jobject, AllocObject, jclass)
T1(jclass, GetObjectClass, jobject)
T2(jboolean, IsInstanceOf, jobject, jclass)
T3(jmethodID, GetMethodID, jclass, const char*, const char*)
T3(jfieldID, GetFieldID, jclass, const char*, const char*)
T3(jmethodID, GetStaticMethodID, jclass, const char*, const char*)
T3(jfieldID, GetStaticFieldID, jclass, const char*, const char*)
T2(jstring, NewString, const jchar*, jsize)
T1(jsize, GetStringLength, jstring)
T2(const jchar*, GetStringChars, jstring, jboolean*)
T2V(ReleaseStringChars, jstring, const jchar*)
T1(jstring, NewStringUTF, const char*)
T1(jsize, GetStringUTFLength, jstring)
T2(const char*, GetStringUTFChars, jstring, jboolean*)
T2V(ReleaseStringUTFChars, jstring, const char*)
T1(jsize, GetArrayLength, jarray)
T3(jobjectArray, NewObjectArray, jsize, jclass, jobject)
T2(jobject, GetObjectArrayElement, jobjectArray, jsize)
T3V(SetObjectArrayElement, jobjectArray, jsize, jobject)
T1(jint, UnregisterNatives, jclass)
T1(jint, MonitorEnter, jobject)
T1(jint, MonitorExit, jobject)
T4V(GetStringRegion, jstring, jsize, jsize, jchar*)
T4V(GetStringUTFRegion, jstring, jsize, jsize, char*)
T2(void*, GetPrimitiveArrayCritical, jarray, jboolean*)
T3V(ReleasePrimitiveArrayCritical, jarray, void*, jint)
T2(const jchar*, GetStringCritical, jstring, jboolean*)
T2V(ReleaseStringCritical, jstring, const jchar*)
T1(jweak, NewWeakGlobalRef, jobject)
T1V(DeleteWeakGlobalRef, jweak)
T0(jboolean, ExceptionCheck)
T2(jobject, NewDirectByteBuffer, void*, jlong)
T1(void*, GetDirectBufferAddress, jobject)
T1(jlong, GetDirectBufferCapacity, jobject)
T1(jobjectRefType, GetObjectRefType, jobject)

// Attach this thread on first use, from the down-call trampoline. Runs on the caller's own glibc
// %fs (nothing swapped yet), so the host allocator and log are safe here. Returns the thread's
// bionic thread pointer; never returns 0 — a native that cannot be given a TCB must not run.
extern "C" std::uintptr_t acl_ndk_attach_now() {
  BionicRuntime* rt = BionicRuntime::get();
  std::string why;
  const std::uintptr_t fs = rt != nullptr ? rt->attach_current_thread(&why) : 0;
  if (fs == 0) {
    log::e(kTag, "cannot attach this thread to bionic: {}", why.empty() ? "no runtime" : why);
    acl_ndk_unattached();  // [[noreturn]]
  }
  return fs;
}

// The trampoline's slow path, called once per thread. It must preserve every argument register
// across the attach call, because the trampoline is signature-agnostic — it cannot know which of
// rdi/rsi/rdx/rcx/r8/r9 or xmm0-7 carry this native's arguments, so it saves all of them. Written
// as real assembly rather than emitted bytes: the assembler checks it, and it is identical for
// every native, so make_downcall only has to call it. Returns the bionic %fs in %rax (the ABI's
// return register, and not an argument register).
//
// Alignment: entered with %rsp ≡ 8 (mod 16) after the trampoline's call, so subtracting 0xC8
// (≡ 8) lands %rsp 16-byte aligned — which both movaps and the ABI's "%rsp ≡ 0 at a call" require.
asm(R"(
    .text
    .globl acl_ndk_attach_thunk
    .hidden acl_ndk_attach_thunk
    .type acl_ndk_attach_thunk,@function
acl_ndk_attach_thunk:
    subq  $0xC8, %rsp
    movaps %xmm0, 0x00(%rsp)
    movaps %xmm1, 0x10(%rsp)
    movaps %xmm2, 0x20(%rsp)
    movaps %xmm3, 0x30(%rsp)
    movaps %xmm4, 0x40(%rsp)
    movaps %xmm5, 0x50(%rsp)
    movaps %xmm6, 0x60(%rsp)
    movaps %xmm7, 0x70(%rsp)
    movq  %rdi, 0x80(%rsp)
    movq  %rsi, 0x88(%rsp)
    movq  %rdx, 0x90(%rsp)
    movq  %rcx, 0x98(%rsp)
    movq  %r8,  0xA0(%rsp)
    movq  %r9,  0xA8(%rsp)
    call  acl_ndk_attach_now
    movq  0x80(%rsp), %rdi
    movq  0x88(%rsp), %rsi
    movq  0x90(%rsp), %rdx
    movq  0x98(%rsp), %rcx
    movq  0xA0(%rsp), %r8
    movq  0xA8(%rsp), %r9
    movaps 0x00(%rsp), %xmm0
    movaps 0x10(%rsp), %xmm1
    movaps 0x20(%rsp), %xmm2
    movaps 0x30(%rsp), %xmm3
    movaps 0x40(%rsp), %xmm4
    movaps 0x50(%rsp), %xmm5
    movaps 0x60(%rsp), %xmm6
    movaps 0x70(%rsp), %xmm7
    addq  $0xC8, %rsp
    ret
    .size acl_ndk_attach_thunk,.-acl_ndk_attach_thunk
)");
extern "C" std::uintptr_t acl_ndk_attach_thunk();

// Down-call trampoline: ART invokes a registered native under GLIBC %fs, but the native is bionic
// code expecting bionic %fs + a wrapped JNIEnv. We emit, per native, a tiny stub that gives the
// calling thread its wrapped env, swaps to that thread's bionic %fs (wrfsbase — no register
// clobber, FSGSBASE), calls the real native, swaps back, returns. Signature-agnostic for
// register-argument natives (no stack args) — the JNI norm; a stack-arg native would need arg
// copying (TODO).
//
// Everything it touches is per-thread, and read while still on the caller's glibc %fs (the only
// point where that thread's TLS is reachable):
//   - the caller's glibc %fs, via rdfsbase, stashed on the stack. Restoring a constant would
//     detach any other caller from its own TLS.
//   - the thread's bionic TCB from t_bionic_fs. Zero means never attached: attach it now, via the
//     thunk above, rather than borrowing another thread's TCB.
//   - the thread's wrapped env (t_wrap_env), pointed at the real JNIEnv ART just passed in %rdi.
//     A JNIEnv belongs to one thread, so this cannot be a global.
// Only %rax/%r10/%r11 are clobbered: all caller-saved, none an argument register, and %al carries
// no varargs count (JNI natives are not variadic).
void* make_downcall(void* real_native) {
  auto* p = static_cast<unsigned char*>(
      ::mmap(nullptr, kPage, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (p == MAP_FAILED) return nullptr;
  std::size_t o = 0;
  auto emit = [&](std::initializer_list<unsigned char> b) { for (unsigned char x : b) p[o++] = x; };
  auto imm64 = [&](std::uint64_t v) { std::memcpy(p + o, &v, 8); o += 8; };
  auto imm32 = [&](std::int32_t v) { std::memcpy(p + o, &v, 4); o += 4; };
  // Frame-pointer trampoline that FORWARDS up to kStackSlots stack arguments: a JNI native with >6
  // integer args (env + class/this + >4 user args) passes the extras on the stack — e.g. libGDX's
  // Matrix4.prj(float[], float[], int offset, int numVecs, int stride) has `stride` on the stack, and
  // Flutter's FlutterJNI.nativeSetViewportMetrics (sig (JFIIIIIIIIIIIIIII[I[I[IIIIIIIII)V) has 29
  // params: after env+class+long+float and the first 3 ints in registers, 12 ints + 3 int[] pointers
  // + 9 trailing ints = 24 integer args sit on the stack — the last of which are the four
  // displayCornerRadius* values. Copying too few leaves those reading uninitialized stack (Flutter's
  // dart:ui then rejects the garbage corner radii as "not all set or all unset"). The trampoline's
  // own return address sits between the native's frame and those args, so we rebuild the args below a
  // fresh call. Entry: %rsp ≡ 8 (mod 16), [rsp]=ret, stack args at [rsp+8+8k]. Copying kStackSlots
  // (even, so the call stays 16-aligned) covers up to 6+kStackSlots integer args; natives with fewer
  // just copy unused slots (harmless reads of the caller's own stack, always mapped — it is ART's
  // frame). 32 covers the 24-stack-arg worst case above with margin.
  constexpr int kStackSlots = 32;
  emit({0x55});                                         // push %rbp        (save ART's rbp; rsp ≡ 0)
  emit({0x48, 0x89, 0xE5});                             // mov %rsp,%rbp    ([rbp+8]=ret, [rbp+16+8k]=args)
  emit({0x64, 0x4C, 0x8B, 0x1C, 0x25});                 // mov %fs:disp32,%r11  (this thread's
  imm32(static_cast<std::int32_t>(g_tls_disp));         //   bionic TCB, or 0 = unattached)
  emit({0x4D, 0x85, 0xDB});                             // test %r11,%r11
  emit({0x75, 0x10});                                   // jnz +16 (over the attach call)
  emit({0x49, 0xBB}); imm64(reinterpret_cast<std::uintptr_t>(&acl_ndk_attach_thunk));
  emit({0x41, 0xFF, 0xD3});                             // call *%r11 (preserves the arg regs; rsp ≡ 0)
  emit({0x49, 0x89, 0xC3});                             // mov %rax,%r11  (this thread's TCB)
  // Still on glibc %fs: point this thread's wrapped env at the real JNIEnv in %rdi, and pass it.
  emit({0xF3, 0x48, 0x0F, 0xAE, 0xC0});                 // rdfsbase %rax   (glibc thread pointer)
  emit({0x48, 0x8D, 0x80}); imm32(static_cast<std::int32_t>(g_wrap_disp));  // lea disp32(%rax),%rax
  emit({0x49, 0xBA}); imm64(reinterpret_cast<std::uintptr_t>(&g_env_tbl));  // mov $env_tbl,%r10
  emit({0x4C, 0x89, 0x10});                             // mov %r10,0(%rax)  wrap.functions
  emit({0x48, 0x89, 0x78, 0x08});                       // mov %rdi,8(%rax)  wrap.real = ART's env
  emit({0x48, 0x89, 0xC7});                             // mov %rax,%rdi     arg0 = &t_wrap_env
  // Copy the stack slots in reverse (slotN..slot0) so the native sees them just above its return
  // address. rbp-relative; plain memory, %fs-agnostic. rsp stays ≡ 0 (even push count). disp32 form
  // (ModRM 0x85), since 0x10+8k exceeds the signed disp8 range once k > 13.
  for (int k = kStackSlots - 1; k >= 0; --k) {
    emit({0x48, 0x8B, 0x85}); imm32(0x10 + 8 * k);      // mov 0x10+8k(%rbp),%rax  (disp32)
    emit({0x50});                                       // push %rax
  }
  emit({0xF3, 0x49, 0x0F, 0xAE, 0xD3});                 // wrfsbase %r11  (-> this thread's bionic)
  emit({0x49, 0xBB}); imm64(reinterpret_cast<std::uintptr_t>(real_native));  // mov $real,%r11
  emit({0x41, 0xFF, 0xD3});                             // call *%r11     (native: stack args at [rsp+8+8k])
  // Epilogue: drop the copied args, recover THIS thread's glibc %fs from the pairing (not a stack
  // save — that would have shifted the args), restore ART's rbp, return. Clobbers only r10/r11/rbp,
  // never the native's return in rax/rdx/xmm0-1.
  emit({0x48, 0x89, 0xEC});                             // mov %rbp,%rsp
  emit({0xF3, 0x49, 0x0F, 0xAE, 0xC2});                 // rdfsbase %r10  (current bionic %fs)
  emit({0x49, 0xBB}); imm64(reinterpret_cast<std::uintptr_t>(g_fs_pairs));  // mov $g_fs_pairs,%r11
  const std::size_t scan = o;
  emit({0x4D, 0x39, 0x13});                             // .scan: cmp %r10,(%r11)  (bionic_fs match?)
  emit({0x74, 0x06});                                   // je .found (+6, over add+jmp)
  emit({0x49, 0x83, 0xC3, 0x10});                       // add $0x10,%r11  (sizeof FsPair)
  const int back = static_cast<int>(scan) - static_cast<int>(o + 2);
  emit({0xEB, static_cast<unsigned char>(back)});       // jmp .scan  (thread is attached: match sure)
  emit({0x4D, 0x8B, 0x5B, 0x08});                       // .found: mov 0x8(%r11),%r11  (glibc_fs)
  emit({0xF3, 0x49, 0x0F, 0xAE, 0xD3});                 // wrfsbase %r11  (-> caller's glibc %fs)
  emit({0x5D});                                         // pop %rbp
  emit({0xC3});                                         // ret
  __builtin___clear_cache(reinterpret_cast<char*>(p), reinterpret_cast<char*>(p + o));
  return p;
}

// What a JNI_OnLoad explicitly bound, so later name resolution can leave those methods alone (see
// BionicRuntime::was_registered). Global refs: these outlive the JNI_OnLoad call's local frame.
struct Registered {
  jobject cls;  // global ref
  std::string name, sig;
};
std::vector<Registered> g_registered;

jint JNICALL w_RegisterNatives(JNIEnv* e, jclass c, const JNINativeMethod* m, jint n) {
  R();
  ToGlibc g;  // glibc %fs for the host allocator + the real RegisterNatives
  std::vector<JNINativeMethod> wrapped(m, m + n);
  for (auto& wm : wrapped) wm.fnPtr = make_downcall(wm.fnPtr);  // ART calls the trampoline, not the raw native
  const jint rc = RF->RegisterNatives(r, c, wrapped.data(), n);
  static const bool diag = std::getenv("ACL_NDK_JNI_DIAG") != nullptr;
  if (diag) {
    if (RF->ExceptionCheck != nullptr && RF->ExceptionCheck(r)) {
      log::w(kTag, "jni RegisterNatives(n={}, first={}) -> rc={} + PENDING EXCEPTION",
             n, (n > 0 && m[0].name) ? m[0].name : "?", rc);
      RF->ExceptionDescribe(r);
    } else {
      log::i(kTag, "jni RegisterNatives(n={}, first={}) -> rc={}", n,
             (n > 0 && m[0].name) ? m[0].name : "?", rc);
    }
  }
  // Record what the lib explicitly bound, so name resolution won't overwrite it (was_registered).
  // NewGlobalRef is always present on a real VM; tolerate a partial env (the probe's fake ART)
  // rather than crashing on a null slot — nothing name-binds against that env anyway.
  if (rc == JNI_OK && RF->NewGlobalRef != nullptr)
    for (jint i = 0; i < n; ++i)
      g_registered.push_back({RF->NewGlobalRef(r, c), m[i].name ? m[i].name : "",
                              m[i].signature ? m[i].signature : ""});
  return rc;
}

// NewObject varargs family (forwards varargs through NewObjectV).
jobject JNICALL w_NewObject(JNIEnv* e, jclass c, jmethodID m, ...) {
  R(); va_list a; va_start(a, m); ToGlibc g; jobject x = RF->NewObjectV(r, c, m, a); va_end(a); return x;
}
jobject JNICALL w_NewObjectV(JNIEnv* e, jclass c, jmethodID m, va_list a) { R(); ToGlibc g; return RF->NewObjectV(r, c, m, a); }
jobject JNICALL w_NewObjectA(JNIEnv* e, jclass c, jmethodID m, const jvalue* a) { R(); ToGlibc g; return RF->NewObjectA(r, c, m, a); }

// Call<T>Method value families (varargs -> ...V). One macro instantiation per return type.
#define CALL_T(RET, T) \
  RET JNICALL w_Call##T##Method(JNIEnv* e, jobject o, jmethodID m, ...) { R(); va_list a; va_start(a,m); ToGlibc g; RET x = RF->Call##T##MethodV(r,o,m,a); va_end(a); return x; } \
  RET JNICALL w_Call##T##MethodV(JNIEnv* e, jobject o, jmethodID m, va_list a) { R(); ToGlibc g; return RF->Call##T##MethodV(r,o,m,a); } \
  RET JNICALL w_Call##T##MethodA(JNIEnv* e, jobject o, jmethodID m, const jvalue* a) { R(); ToGlibc g; return RF->Call##T##MethodA(r,o,m,a); } \
  RET JNICALL w_CallNonvirtual##T##Method(JNIEnv* e, jobject o, jclass c, jmethodID m, ...) { R(); va_list a; va_start(a,m); ToGlibc g; RET x = RF->CallNonvirtual##T##MethodV(r,o,c,m,a); va_end(a); return x; } \
  RET JNICALL w_CallNonvirtual##T##MethodV(JNIEnv* e, jobject o, jclass c, jmethodID m, va_list a) { R(); ToGlibc g; return RF->CallNonvirtual##T##MethodV(r,o,c,m,a); } \
  RET JNICALL w_CallNonvirtual##T##MethodA(JNIEnv* e, jobject o, jclass c, jmethodID m, const jvalue* a) { R(); ToGlibc g; return RF->CallNonvirtual##T##MethodA(r,o,c,m,a); } \
  RET JNICALL w_CallStatic##T##Method(JNIEnv* e, jclass c, jmethodID m, ...) { R(); va_list a; va_start(a,m); ToGlibc g; RET x = RF->CallStatic##T##MethodV(r,c,m,a); va_end(a); return x; } \
  RET JNICALL w_CallStatic##T##MethodV(JNIEnv* e, jclass c, jmethodID m, va_list a) { R(); ToGlibc g; return RF->CallStatic##T##MethodV(r,c,m,a); } \
  RET JNICALL w_CallStatic##T##MethodA(JNIEnv* e, jclass c, jmethodID m, const jvalue* a) { R(); ToGlibc g; return RF->CallStatic##T##MethodA(r,c,m,a); }
CALL_T(jobject, Object) CALL_T(jboolean, Boolean) CALL_T(jbyte, Byte) CALL_T(jchar, Char)
CALL_T(jshort, Short) CALL_T(jint, Int) CALL_T(jlong, Long) CALL_T(jfloat, Float) CALL_T(jdouble, Double)

// Call void family (explicit — no return value).
void JNICALL w_CallVoidMethod(JNIEnv* e, jobject o, jmethodID m, ...) { R(); va_list a; va_start(a,m); ToGlibc g; RF->CallVoidMethodV(r,o,m,a); va_end(a); }
void JNICALL w_CallVoidMethodV(JNIEnv* e, jobject o, jmethodID m, va_list a) { R(); ToGlibc g; RF->CallVoidMethodV(r,o,m,a); }
void JNICALL w_CallVoidMethodA(JNIEnv* e, jobject o, jmethodID m, const jvalue* a) { R(); ToGlibc g; RF->CallVoidMethodA(r,o,m,a); }
void JNICALL w_CallNonvirtualVoidMethod(JNIEnv* e, jobject o, jclass c, jmethodID m, ...) { R(); va_list a; va_start(a,m); ToGlibc g; RF->CallNonvirtualVoidMethodV(r,o,c,m,a); va_end(a); }
void JNICALL w_CallNonvirtualVoidMethodV(JNIEnv* e, jobject o, jclass c, jmethodID m, va_list a) { R(); ToGlibc g; RF->CallNonvirtualVoidMethodV(r,o,c,m,a); }
void JNICALL w_CallNonvirtualVoidMethodA(JNIEnv* e, jobject o, jclass c, jmethodID m, const jvalue* a) { R(); ToGlibc g; RF->CallNonvirtualVoidMethodA(r,o,c,m,a); }
void JNICALL w_CallStaticVoidMethod(JNIEnv* e, jclass c, jmethodID m, ...) { R(); va_list a; va_start(a,m); ToGlibc g; RF->CallStaticVoidMethodV(r,c,m,a); va_end(a); }
void JNICALL w_CallStaticVoidMethodV(JNIEnv* e, jclass c, jmethodID m, va_list a) { R(); ToGlibc g; RF->CallStaticVoidMethodV(r,c,m,a); }
void JNICALL w_CallStaticVoidMethodA(JNIEnv* e, jclass c, jmethodID m, const jvalue* a) { R(); ToGlibc g; RF->CallStaticVoidMethodA(r,c,m,a); }

// Get/Set<T>Field (instance + static).
#define FIELD_T(CT, T) \
  CT JNICALL w_Get##T##Field(JNIEnv* e, jobject o, jfieldID f) { R(); ToGlibc g; return RF->Get##T##Field(r,o,f); } \
  void JNICALL w_Set##T##Field(JNIEnv* e, jobject o, jfieldID f, CT v) { R(); ToGlibc g; RF->Set##T##Field(r,o,f,v); } \
  CT JNICALL w_GetStatic##T##Field(JNIEnv* e, jclass c, jfieldID f) { R(); ToGlibc g; return RF->GetStatic##T##Field(r,c,f); } \
  void JNICALL w_SetStatic##T##Field(JNIEnv* e, jclass c, jfieldID f, CT v) { R(); ToGlibc g; RF->SetStatic##T##Field(r,c,f,v); }
FIELD_T(jobject, Object) FIELD_T(jboolean, Boolean) FIELD_T(jbyte, Byte) FIELD_T(jchar, Char)
FIELD_T(jshort, Short) FIELD_T(jint, Int) FIELD_T(jlong, Long) FIELD_T(jfloat, Float) FIELD_T(jdouble, Double)

// Primitive array families.
#define ARR_T(CT, AT, T) \
  AT JNICALL w_New##T##Array(JNIEnv* e, jsize n) { R(); ToGlibc g; return RF->New##T##Array(r,n); } \
  CT* JNICALL w_Get##T##ArrayElements(JNIEnv* e, AT a, jboolean* c) { R(); ToGlibc g; return RF->Get##T##ArrayElements(r,a,c); } \
  void JNICALL w_Release##T##ArrayElements(JNIEnv* e, AT a, CT* el, jint m) { R(); ToGlibc g; RF->Release##T##ArrayElements(r,a,el,m); } \
  void JNICALL w_Get##T##ArrayRegion(JNIEnv* e, AT a, jsize s, jsize l, CT* b) { R(); ToGlibc g; RF->Get##T##ArrayRegion(r,a,s,l,b); } \
  void JNICALL w_Set##T##ArrayRegion(JNIEnv* e, AT a, jsize s, jsize l, const CT* b) { R(); ToGlibc g; RF->Set##T##ArrayRegion(r,a,s,l,b); }
ARR_T(jboolean, jbooleanArray, Boolean) ARR_T(jbyte, jbyteArray, Byte) ARR_T(jchar, jcharArray, Char)
ARR_T(jshort, jshortArray, Short) ARR_T(jint, jintArray, Int) ARR_T(jlong, jlongArray, Long)
ARR_T(jfloat, jfloatArray, Float) ARR_T(jdouble, jdoubleArray, Double)

// GetJavaVM must hand back the WRAPPER vm so nested GetEnv keeps trampolining.
jint JNICALL w_GetJavaVM(JNIEnv*, JavaVM** vm) { *vm = reinterpret_cast<JavaVM*>(&g_wrap_vm); return JNI_OK; }

// JavaVM (invoke interface) GetEnv: hand the bionic caller ITS OWN wrapped env.
jint JNICALL w_GetEnv(JavaVM* vm, void** out, jint ver) {
  WrapVM* w = reinterpret_cast<WrapVM*>(vm);
  // A thread acl never paired to a glibc %fs is, by definition, not attached to the VM — real JNI
  // returns JNI_EDETACHED here (the caller then AttachCurrentThread's or skips its JNI work). A
  // bionic-spawned engine thread (e.g. a Flutter worker/raster thread that has not received an ART
  // down-call) hits this. Check BEFORE ToGlibc, which would otherwise abort as unattached.
  if (out != nullptr) *out = nullptr;
  if (glibc_fs_for(ndk_get_fs()) == 0) return JNI_EDETACHED;
  JNIEnv* tmp = nullptr;
  JNIEnv* wrapped = nullptr;
  {
    ToGlibc g;
    const jint rc = w->real->functions->GetEnv(w->real, reinterpret_cast<void**>(&tmp), ver);
    if (rc != JNI_OK) { *out = nullptr; return rc; }
    // &t_wrap_env is a glibc TLS access (%fs-relative), so it is only meaningful while ToGlibc
    // holds this thread's glibc %fs. Taking it outside this scope would resolve the same
    // displacement against BIONIC's TCB and hand the caller garbage — a JNIEnv that segfaults on
    // first use. Any glibc TLS read under bionic %fs is silently wrong; keep them in here.
    wrapped = reinterpret_cast<JNIEnv*>(&t_wrap_env);
  }
  *out = wrapped;
  return JNI_OK;
}

// JavaVM AttachCurrentThread[AsDaemon]. A thread already paired to a glibc %fs (an ART thread that
// borrowed a bionic TCB via a down-call) can attach for real. A bionic-spawned engine thread that
// acl never paired CANNOT yet: ART's AttachCurrentThreadInternal would run under bionic %fs and
// dereference a garbage Thread (SEGV). acl has no per-thread glibc %fs+TLS+stack for such a thread
// to host an ART Thread on (the reverse of attach_current_thread; a real fix needs a glibc attach
// donor whose pthread stack bounds match this thread, so ART's GC scans the right stack -- see
// docs/ndk-scope.md). Fail the attach cleanly so the caller skips its JNI work on this thread,
// rather than crashing. Before this, a real app whose engine attaches such a thread (e.g. the
// flutter_gallery) SIGSEGV'd inside libart.
jint JNICALL w_AttachCurrentThread(JavaVM* vm, void** p_env, void* args) {
  WrapVM* w = reinterpret_cast<WrapVM*>(vm);
  if (p_env != nullptr) *p_env = nullptr;
  // Pair an unpaired bionic thread with a glibc attach-donor %fs, so ART's AttachCurrentThreadInternal
  // has a glibc TLS to host the Thread on instead of SEGV'ing under bionic %fs. Pool exhaustion is the
  // only failure (warned once via a raw write -- log/fmt/malloc would themselves fault under bionic
  // %fs). After pairing, ToGlibc below finds the donor %fs.
  if (ensure_paired_glibc_fs() == 0) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      static const char m[] =
          "acl.ndk: W AttachCurrentThread: attach-donor pool exhausted; failing the attach\n";
      (void)syscall(SYS_write, 2, m, sizeof(m) - 1);
    }
    return JNI_ERR;
  }
  JNIEnv* wrapped = nullptr;
  {
    ToGlibc g;  // this thread's glibc %fs (its donor, or its real one if already an ART thread)
    JNIEnv* tmp = nullptr;
    const jint rc =
        w->real->functions->AttachCurrentThread(w->real, reinterpret_cast<void**>(&tmp), args);
    if (rc != JNI_OK) return rc;
    t_wrap_env.functions = &g_env_tbl;
    t_wrap_env.real = tmp;
    wrapped = reinterpret_cast<JNIEnv*>(&t_wrap_env);
  }
  if (p_env != nullptr) *p_env = wrapped;
  return JNI_OK;
}
}  // namespace
#endif  // __x86_64__

JavaVM* BionicRuntime::wrap_java_vm(JavaVM* real_vm, JNIEnv* real_env) {
#if defined(__x86_64__)
  if (!real_vm || !real_env) return nullptr;
  // Parked glibc %fs donors for bionic threads that AttachCurrentThread later. Spawned here, on the
  // main (glibc) thread, so the donors are real glibc threads with glibc TLS.
  ensure_attach_donor_pool();
  // This thread's wrapped env. JNI_OnLoad is driven directly by load_library rather than through a
  // down-call trampoline, so nothing else would have filled it in for this thread. The trampoline
  // addresses the slot off the thread pointer, so record its fixed displacement too — measured
  // here, on this thread, under its glibc %fs.
  t_wrap_env.functions = &g_env_tbl;
  t_wrap_env.real = real_env;
  g_wrap_disp = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(&t_wrap_env)) -
                static_cast<std::int64_t>(ndk_get_fs());
  if (g_wrap_disp < INT32_MIN || g_wrap_disp > INT32_MAX) {
    log::e(kTag, "wrapped-env TLS displacement does not fit a disp32");
    return nullptr;
  }

  g_env_tbl = *real_env->functions;  // start from the real table, then thunk every slot below

#define S(N) g_env_tbl.N = w_##N
#define S_CALL(T) S(Call##T##Method); S(Call##T##MethodV); S(Call##T##MethodA); \
  S(CallNonvirtual##T##Method); S(CallNonvirtual##T##MethodV); S(CallNonvirtual##T##MethodA); \
  S(CallStatic##T##Method); S(CallStatic##T##MethodV); S(CallStatic##T##MethodA)
#define S_FIELD(T) S(Get##T##Field); S(Set##T##Field); S(GetStatic##T##Field); S(SetStatic##T##Field)
#define S_ARR(T) S(New##T##Array); S(Get##T##ArrayElements); S(Release##T##ArrayElements); \
  S(Get##T##ArrayRegion); S(Set##T##ArrayRegion)

  S(GetVersion); S(DefineClass); S(FindClass); S(FromReflectedMethod); S(FromReflectedField);
  S(ToReflectedMethod); S(GetSuperclass); S(IsAssignableFrom); S(ToReflectedField); S(Throw);
  S(ThrowNew); S(ExceptionOccurred); S(ExceptionDescribe); S(ExceptionClear); S(FatalError);
  S(PushLocalFrame); S(PopLocalFrame); S(NewGlobalRef); S(DeleteGlobalRef); S(DeleteLocalRef);
  S(IsSameObject); S(NewLocalRef); S(EnsureLocalCapacity); S(AllocObject); S(NewObject);
  S(NewObjectV); S(NewObjectA); S(GetObjectClass); S(IsInstanceOf); S(GetMethodID);
  S(GetFieldID); S(GetStaticMethodID); S(GetStaticFieldID); S(NewString); S(GetStringLength);
  S(GetStringChars); S(ReleaseStringChars); S(NewStringUTF); S(GetStringUTFLength);
  S(GetStringUTFChars); S(ReleaseStringUTFChars); S(GetArrayLength); S(NewObjectArray);
  S(GetObjectArrayElement); S(SetObjectArrayElement); S(RegisterNatives); S(UnregisterNatives);
  S(MonitorEnter); S(MonitorExit); S(GetJavaVM); S(GetStringRegion); S(GetStringUTFRegion);
  S(GetPrimitiveArrayCritical); S(ReleasePrimitiveArrayCritical); S(GetStringCritical);
  S(ReleaseStringCritical); S(NewWeakGlobalRef); S(DeleteWeakGlobalRef); S(ExceptionCheck);
  S(NewDirectByteBuffer); S(GetDirectBufferAddress); S(GetDirectBufferCapacity); S(GetObjectRefType);
  S(CallVoidMethod); S(CallVoidMethodV); S(CallVoidMethodA); S(CallNonvirtualVoidMethod);
  S(CallNonvirtualVoidMethodV); S(CallNonvirtualVoidMethodA); S(CallStaticVoidMethod);
  S(CallStaticVoidMethodV); S(CallStaticVoidMethodA);
  S_CALL(Object); S_CALL(Boolean); S_CALL(Byte); S_CALL(Char); S_CALL(Short); S_CALL(Int);
  S_CALL(Long); S_CALL(Float); S_CALL(Double);
  S_FIELD(Object); S_FIELD(Boolean); S_FIELD(Byte); S_FIELD(Char); S_FIELD(Short); S_FIELD(Int);
  S_FIELD(Long); S_FIELD(Float); S_FIELD(Double);
  S_ARR(Boolean); S_ARR(Byte); S_ARR(Char); S_ARR(Short); S_ARR(Int); S_ARR(Long); S_ARR(Float);
  S_ARR(Double);
#undef S
#undef S_CALL
#undef S_FIELD
#undef S_ARR

  g_vm_tbl = *real_vm->functions;
  g_vm_tbl.GetEnv = w_GetEnv;
  g_vm_tbl.AttachCurrentThread = w_AttachCurrentThread;
  g_vm_tbl.AttachCurrentThreadAsDaemon = w_AttachCurrentThread;
  g_wrap_vm.functions = &g_vm_tbl;
  g_wrap_vm.real = real_vm;
  g_wrap_vm.wrap_env = reinterpret_cast<JNIEnv*>(&t_wrap_env);
  wrap_env_ = reinterpret_cast<JNIEnv*>(&t_wrap_env);
  return reinterpret_cast<JavaVM*>(&g_wrap_vm);
#else
  (void)real_vm;
  (void)real_env;
  return nullptr;
#endif
}

void set_native_gl_hooks(const NativeGlHooks& hooks) { g_native_gl_hooks = hooks; }

// ── AAssetManager provider bridge (libandroid AAsset* -> acl's pkg) ───────────────────────────
// libandroid's AAssetManager_open (bionic side) reads an app asset — Flutter's Dart kernel,
// snapshots, and flutter_assets — through acl's pkg (glibc side), which owns the open APK and the
// zlib inflate its DEFLATE'd entries need. The callback inflates the whole asset into a glibc-owned
// buffer; libandroid serves getBuffer/getLength from it. libandroid invokes the callback on the
// CALLER's bionic %fs, so restore this thread's glibc %fs across the call (the same bionic->glibc
// hop ToGlibc does, but reachable from a raw C hook pointer and without aborting on a thread acl
// never paired — a bionic-spawned engine thread — where running acl's allocator would corrupt TLS).
namespace {
int (*g_asset_open_cb)(void*, const char*, void**, long*) = nullptr;
void (*g_asset_free_cb)(void*, void*) = nullptr;
void* g_asset_provider_user = nullptr;

int acl_asset_open_tramp(void* user, const char* name, void** out_buf, long* out_len) {
  if (g_asset_open_cb == nullptr) return 0;
  const std::uintptr_t bionic_fs = ndk_get_fs();
  // A bionic-spawned engine thread reading an asset needs a glibc %fs for pkg + the allocator; pair
  // it with an attach donor on demand (only fails if the donor pool is exhausted). The read is
  // %fs-agnostic memory work under the donor -- no ART Thread / stack scanning involved.
  const std::uintptr_t glibc_fs = ensure_paired_glibc_fs();
  if (glibc_fs == 0) return 0;  // pool exhausted: miss rather than corrupt
  ndk_set_fs(glibc_fs);
  const int rc = g_asset_open_cb(user, name, out_buf, out_len);
  ndk_set_fs(bionic_fs);
  return rc;
}
void acl_asset_free_tramp(void* user, void* buf) {
  if (g_asset_free_cb == nullptr) return;
  const std::uintptr_t bionic_fs = ndk_get_fs();
  const std::uintptr_t glibc_fs = ensure_paired_glibc_fs();
  if (glibc_fs == 0) return;  // pool exhausted: leak rather than corrupt
  ndk_set_fs(glibc_fs);
  g_asset_free_cb(user, buf);
  ndk_set_fs(bionic_fs);
}

// AHardwareBuffer describe/acquire/release backing (Flutter external textures). The host ops are
// gfx::ahb_* (set by the runtime via set_ahb_ops, so libacl_ndk keeps no gfx link dep). describe (a
// struct read) and acquire (an atomic) are TLS-free, so their trampolines call the host op directly;
// release frees (glibc malloc), so it restores glibc %fs first, like acl_asset_free_tramp.
void (*g_ahb_describe_fn)(const void*, void*) = nullptr;
void (*g_ahb_acquire_fn)(void*) = nullptr;
void (*g_ahb_release_fn)(void*) = nullptr;
void acl_ahb_describe_tramp(const void* ahb, void* desc) {
  if (g_ahb_describe_fn != nullptr) g_ahb_describe_fn(ahb, desc);
}
void acl_ahb_acquire_tramp(void* ahb) {
  if (g_ahb_acquire_fn != nullptr) g_ahb_acquire_fn(ahb);
}
void acl_ahb_release_tramp(void* ahb) {
  if (g_ahb_release_fn == nullptr) return;
  const std::uintptr_t bionic_fs = ndk_get_fs();
  const std::uintptr_t glibc_fs = ensure_paired_glibc_fs();
  if (glibc_fs == 0) return;  // pool exhausted: leak rather than corrupt
  ndk_set_fs(glibc_fs);
  g_ahb_release_fn(ahb);
  ndk_set_fs(bionic_fs);
}
}  // namespace

void set_asset_provider(int (*open_cb)(void*, const char*, void**, long*),
                        void (*free_cb)(void*, void*), void* user) {
  g_asset_open_cb = open_cb;
  g_asset_free_cb = free_cb;
  g_asset_provider_user = user;
}

void set_ahb_ops(void (*describe)(const void*, void*), void (*acquire)(void*),
                 void (*release)(void*)) {
  g_ahb_describe_fn = describe;
  g_ahb_acquire_fn = acquire;
  g_ahb_release_fn = release;
}

// ── ALooper <-> Java Looper bridge ────────────────────────────────────────────────────────────
// libandroid's acl_looper_bridge_{fd,pump} expose the current thread's NDK ALooper so acl's main
// Java Looper can service it (see the libandroid side). These run the bionic exports as this
// thread — the main thread, which is attached — under its bionic %fs.
namespace {
int (*g_bridge_fd_fn)() = nullptr;
void (*g_bridge_pump_fn)() = nullptr;
bool ensure_looper_bridge_syms() {
  if (g_bridge_fd_fn != nullptr && g_bridge_pump_fn != nullptr) return true;
  BionicRuntime* rt = BionicRuntime::get();
  const char* sysroot = getenv("ACL_BIONIC_SYSROOT");
  if (rt == nullptr || sysroot == nullptr) return false;
  std::string err;
  void* la = rt->load((std::string(sysroot) + "/libandroid.so").c_str(), &err);
  if (la == nullptr) return false;
  g_bridge_fd_fn = reinterpret_cast<int (*)()>(rt->symbol(la, "acl_looper_bridge_fd"));
  g_bridge_pump_fn = reinterpret_cast<void (*)()>(rt->symbol(la, "acl_looper_bridge_pump"));
  return g_bridge_fd_fn != nullptr && g_bridge_pump_fn != nullptr;
}
}  // namespace

int main_looper_bridge_fd() {
#if defined(__x86_64__)
  if (!ensure_looper_bridge_syms()) return -1;
  BionicRuntime::RunScope scope(*BionicRuntime::get());
  return g_bridge_fd_fn();
#else
  return -1;
#endif
}
void main_looper_bridge_pump() {
#if defined(__x86_64__)
  if (g_bridge_pump_fn == nullptr) return;  // fd side resolves the symbols first
  BionicRuntime::RunScope scope(*BionicRuntime::get());
  g_bridge_pump_fn();
#endif
}

void BionicRuntime::set_app_class_loader(JNIEnv* real_env, jobject loader) {
#if defined(__x86_64__)
  if (real_env == nullptr) return;
  if (g_app_class_loader != nullptr) {
    real_env->DeleteGlobalRef(g_app_class_loader);
    g_app_class_loader = nullptr;
  }
  if (loader != nullptr) g_app_class_loader = real_env->NewGlobalRef(loader);
#else
  (void)real_env; (void)loader;
#endif
}

void* BionicRuntime::load_library(const char* abs_path, JavaVM* real_vm, JNIEnv* real_env,
                                  std::string* error) {
#if defined(__x86_64__)
  // Wrap ART/libsigchain's signal handlers BEFORE running this lib's constructors: a lib like
  // libflutter.so spawns engine threads in its .init_array, and a signal landing on one of those
  // bionic-spawned threads (a real fault, or ART's all-thread crash dump) must be serviced with a
  // glibc %fs — otherwise ART's handler crashes on it before the shim is even installed. Idempotent.
  install_signal_shims();
  // Route the app's native logs (libflutter engine, Dart print, the debug VM-service URL, any liblog
  // user) into acl's stream: bionic liblog's default logd sink isn't running, so they otherwise
  // vanish. Done via libandroid's acl_ndk_log_init BEFORE loading `abs_path`, so its load-time
  // constructor logs are captured too. Once; best-effort. (A .init_array in libandroid would not run
  // under this loader, so acl drives it explicitly.)
  {
    static std::atomic<bool> log_routed{false};
    if (!log_routed.exchange(true)) {
      if (const char* sysroot = getenv("ACL_BIONIC_SYSROOT")) {
        std::string lerr;
        if (void* la = load((std::string(sysroot) + "/libandroid.so").c_str(), &lerr)) {
          if (void* init = symbol(la, "acl_ndk_log_init")) {
            RunScope scope(*this);
            reinterpret_cast<void (*)()>(init)();
          }
          // Seed bionic's DNS resolver (no netd/dnsproxyd here, so getaddrinfo would otherwise have
          // no nameservers and every host lookup — image/HTTP downloads — fails). Mirrors the host
          // /etc/resolv.conf; runs in bionic %fs since it calls libc resolver internals.
          if (void* seed = symbol(la, "acl_seed_dns_resolv")) {
            RunScope scope(*this);
            reinterpret_cast<void (*)()>(seed)();
          }
          // Point libandroid's AAssetManager at acl's pkg-backed provider. Installed unconditionally
          // (this runs once, at first lib load, possibly before set_asset_provider); the trampolines
          // self-guard on g_asset_open_cb and miss until acl sets it. They restore glibc %fs before
          // entering acl's asset reader — see set_asset_provider.
          if (void** o = static_cast<void**>(symbol(la, "acl_asset_open_hook")))
            *o = reinterpret_cast<void*>(&acl_asset_open_tramp);
          if (void** f = static_cast<void**>(symbol(la, "acl_asset_free_hook")))
            *f = reinterpret_cast<void*>(&acl_asset_free_tramp);
          if (void** u = static_cast<void**>(symbol(la, "acl_asset_user"))) *u = g_asset_provider_user;
          // AHardwareBuffer describe/acquire/release hooks -> the %fs-aware trampolines above (they
          // self-guard on g_ahb_*_fn, which the runtime sets to gfx::ahb_* via set_ahb_ops).
          if (void** d = static_cast<void**>(symbol(la, "acl_ahb_describe_hook")))
            *d = reinterpret_cast<void*>(&acl_ahb_describe_tramp);
          if (void** a = static_cast<void**>(symbol(la, "acl_ahb_acquire_hook")))
            *a = reinterpret_cast<void*>(&acl_ahb_acquire_tramp);
          if (void** r = static_cast<void**>(symbol(la, "acl_ahb_release_hook")))
            *r = reinterpret_cast<void*>(&acl_ahb_release_tramp);
        }
      }
    }
  }
  void* h = load(abs_path, error);
  if (!h) return nullptr;
  // If this lib pulled in the bionic libEGL.so/libGLESv2.so forwarders (they are its NEEDED deps,
  // loaded by the line above), point them at host Mesa now — before any GL call. Gated internally.
  install_gl_forwarders(this);
  // Wrap unconditionally, not just for JNI_OnLoad: a lib whose natives are all bound by NAME has
  // no JNI_OnLoad, and its down-call trampolines still need the wrapped env + the %fs pair that
  // wrap_java_vm establishes (see downcall_symbol).
  JavaVM* wvm = wrap_java_vm(real_vm, real_env);
  if (!wvm) { if (error) *error = "wrap_java_vm failed"; return nullptr; }
  void* onload = symbol(h, "JNI_OnLoad");
  if (onload) {
    jint ver;
    { RunScope scope(*this); ver = reinterpret_cast<jint (*)(JavaVM*, void*)>(onload)(wvm, nullptr); }
    if (ver == JNI_ERR || ver < JNI_VERSION_1_2) {
      if (error) *error = "JNI_OnLoad rejected the VM";
      return nullptr;
    }
    log::i(kTag, "{}: JNI_OnLoad ok (requested JNI {:#x})", abs_path, ver);
  }
  return h;
#else
  (void)abs_path; (void)real_vm; (void)real_env;
  if (error) *error = "bionic loader: arm64 TLS port pending";
  return nullptr;
#endif
}

void* BionicRuntime::downcall_symbol(void* handle, const char* sym) {
#if defined(__x86_64__)
  void* raw = symbol(handle, sym);
  if (raw == nullptr) return nullptr;  // not exported by name — the common, non-error case
  return make_downcall(raw);
#else
  (void)handle; (void)sym;
  return nullptr;
#endif
}

bool BionicRuntime::was_registered(JNIEnv* env, jclass klass, const char* name,
                                   const char* sig) const {
#if defined(__x86_64__)
  for (const Registered& r : g_registered)
    if (r.name == name && r.sig == sig && env->IsSameObject(r.cls, klass)) return true;
  return false;
#else
  (void)env; (void)klass; (void)name; (void)sig;
  return false;
#endif
}
#endif  // ACL_HAVE_JNI

}  // namespace acl::ndk
