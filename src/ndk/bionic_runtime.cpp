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

#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdarg>
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
}

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

std::uintptr_t get_fs() {
  std::uintptr_t v = 0;
  syscall(SYS_arch_prctl, ARCH_GET_FS, &v);
  return v;
}
void set_fs(std::uintptr_t v) { syscall(SYS_arch_prctl, ARCH_SET_FS, v); }

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
                      void** out_dlsym, void** out_dlerror) {
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
  return true;
}

}  // namespace

// --- glue that flips %fs; must stay in this -fno-stack-protector TU ---

void ndk_set_fs(std::uintptr_t v) { set_fs(v); }
std::uintptr_t ndk_get_fs() { return get_fs(); }

bool BionicRuntime_bootstrap(const BionicConfig& cfg, std::uintptr_t* fs, void** dlo, void** dls,
                             void** dle) {
  return bootstrap_x86_64(cfg, fs, dlo, dls, dle);
}

#else  // non-x86_64: bootstrap not yet ported (arm64 swaps TPIDR_EL0 / auxv / relocs)

void ndk_set_fs(std::uintptr_t) {}
std::uintptr_t ndk_get_fs() { return 0; }
bool BionicRuntime_bootstrap(const BionicConfig&, std::uintptr_t*, void**, void**, void**) {
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
  void *dlo = nullptr, *dls = nullptr, *dle = nullptr;
  if (!BionicRuntime_bootstrap(cfg, &fs, &dlo, &dls, &dle)) {
    if (error) *error = "bionic loader: linker bootstrap failed (see log)";
    return nullptr;
  }
  rt.bionic_fs_ = fs;
  rt.dlopen_fn_ = dlo;
  rt.dlsym_fn_ = dls;
  rt.dlerror_fn_ = dle;
  g_rt = &rt;
  log::i(kTag, "bionic linker bootstrapped in-process (fs={:#x})", fs);
  return g_rt;
}

BionicRuntime::RunScope::RunScope(BionicRuntime& rt) : saved_(ndk_get_fs()) {
  ndk_set_fs(rt.bionic_fs_);
}
BionicRuntime::RunScope::~RunScope() { ndk_set_fs(saved_); }

void* BionicRuntime::load(const char* abs_path, std::string* error) {
  if (!abs_path || abs_path[0] != '/') {
    if (error) *error = "bionic load: absolute path required";
    return nullptr;
  }
  RunScope scope(*this);
  void* h = reinterpret_cast<void* (*)(const char*, int, const void*)>(dlopen_fn_)(
      abs_path, 2 /*RTLD_NOW*/, dlopen_fn_);
  if (!h && error) {
    char* e = dlerror_fn_ ? reinterpret_cast<char* (*)()>(dlerror_fn_)() : nullptr;
    *error = e ? e : "dlopen failed";
  }
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
WrapEnv g_wrap_env;
WrapVM g_wrap_vm;
std::uintptr_t g_jni_glibc_fs = 0, g_jni_bionic_fs = 0;

// ── Per-thread bionic thread pointer ─────────────────────────────────────────────────────────
// A bionic TCB belongs to ONE thread (it holds errno, pthread_self, pthread TLS, the DTV), so a
// thread running bionic code needs its own. This slot holds the calling thread's bionic TCB, or 0
// when the thread has never been attached. The down-call trampoline reads it *before* swapping
// (i.e. still on the caller's glibc %fs) via a constant %fs-relative displacement — hence
// initial-exec, which guarantees the slot lives in the static TLS block at a fixed offset from the
// thread pointer for EVERY thread. (Local-exec would do as well here; initial-exec states the
// requirement explicitly and survives this TU moving into a shared library.)
__thread __attribute__((tls_model("initial-exec"))) std::uintptr_t t_bionic_fs = 0;
std::int64_t g_tls_disp = 0;  // &t_bionic_fs - glibc %fs base; constant across threads

// Called by the trampoline when a native is invoked on a thread with no bionic TCB. Runs on the
// caller's own (glibc) %fs — nothing has been swapped yet — so plain host calls are safe here.
// It must not return: continuing would run bionic code on another thread's TCB and, worse, return
// with that thread's glibc %fs installed, which detaches the caller from its own TLS. That is not
// a theoretical worry: ART's Thread::Current() is a TLS read, so the process dies later inside the
// GC ("Check failed: self == thread") with nothing pointing back at here. Fail here instead.
[[noreturn]] void acl_ndk_unattached() {
  log::e(kTag,
         "bionic native called from a thread with no bionic TCB. Per-thread attach-as-bionic is "
         "not implemented yet (docs/ndk-scope.md): only the thread that bootstrapped the loader "
         "(the main thread) may run app native code. Aborting rather than corrupting this "
         "thread's TLS.");
  std::abort();
}

// Entered under bionic %fs (from the bionic .so); restore glibc for the real call, then bionic.
// Sets the *constant* glibc/bionic %fs (not a save/restore), which composes correctly under JNI
// re-entrancy with the down-call RunScope (up-calls always originate in bionic, down-calls in glibc).
struct ToGlibc {
  ToGlibc() { ndk_set_fs(g_jni_glibc_fs); }
  ~ToGlibc() { ndk_set_fs(g_jni_bionic_fs); }
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
T1(jclass, FindClass, const char*)
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

// Down-call trampoline: ART invokes a registered native under GLIBC %fs, but the native is bionic
// code expecting bionic %fs + the wrapped JNIEnv. We emit, per native, a tiny stub that replaces
// arg0 (JNIEnv*) with the wrapper env, sets %fs=bionic (wrfsbase — no register clobber, FSGSBASE),
// calls the real native, restores %fs, returns. Signature-agnostic for register-argument natives
// (no stack args) — the JNI norm; a stack-arg native would need arg copying (TODO).
//
// Both thread pointers are per-thread, never baked in: the caller's glibc %fs is read with
// rdfsbase and stashed on the stack (restoring a *constant* main-thread value would silently
// detach any other caller from its own TLS), and the bionic side comes from this thread's
// t_bionic_fs slot, read at %fs:g_tls_disp while still on the caller's glibc %fs. A zero slot
// means the thread was never attached -> acl_ndk_unattached() (see above), never a wrong TCB.
// Only %r10/%r11 are touched: both are caller-saved and neither carries an argument (nor the
// varargs vector count in %al — JNI natives are not variadic).
void* make_downcall(void* real_native) {
  auto* p = static_cast<unsigned char*>(
      ::mmap(nullptr, kPage, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (p == MAP_FAILED) return nullptr;
  std::size_t o = 0;
  auto emit = [&](std::initializer_list<unsigned char> b) { for (unsigned char x : b) p[o++] = x; };
  auto imm64 = [&](std::uint64_t v) { std::memcpy(p + o, &v, 8); o += 8; };
  auto imm32 = [&](std::int32_t v) { std::memcpy(p + o, &v, 4); o += 4; };
  // Entry: %rsp ≡ 8 (mod 16). The push below realigns to 0, so the call sees the ABI's ≡ 8.
  emit({0xF3, 0x49, 0x0F, 0xAE, 0xC2});                 // rdfsbase %r10   (caller's glibc %fs)
  emit({0x41, 0x52});                                   // push %r10
  emit({0x64, 0x4C, 0x8B, 0x1C, 0x25});                 // mov %fs:disp32,%r11  (this thread's
  imm32(static_cast<std::int32_t>(g_tls_disp));         //   bionic TCB, or 0 = unattached)
  emit({0x4D, 0x85, 0xDB});                             // test %r11,%r11
  emit({0x75, 0x0D});                                   // jnz +13 (over the unattached call)
  emit({0x49, 0xBB}); imm64(reinterpret_cast<std::uintptr_t>(&acl_ndk_unattached));
  emit({0x41, 0xFF, 0xD3});                             // call *%r11 — [[noreturn]]
  emit({0xF3, 0x49, 0x0F, 0xAE, 0xD3});                 // wrfsbase %r11  (-> this thread's bionic)
  emit({0x48, 0xBF}); imm64(reinterpret_cast<std::uintptr_t>(&g_wrap_env));  // mov $wrap_env,%rdi
  emit({0x49, 0xBB}); imm64(reinterpret_cast<std::uintptr_t>(real_native));  // mov $real,%r11
  emit({0x41, 0xFF, 0xD3});                             // call *%r11
  emit({0x41, 0x5A});                                   // pop %r10
  emit({0xF3, 0x49, 0x0F, 0xAE, 0xD2});                 // wrfsbase %r10  (caller's OWN glibc %fs)
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

// JavaVM (invoke interface) GetEnv: hand the bionic caller the wrapped env.
jint JNICALL w_GetEnv(JavaVM* vm, void** out, jint ver) {
  WrapVM* w = reinterpret_cast<WrapVM*>(vm);
  JNIEnv* tmp = nullptr;
  {
    ToGlibc g;
    const jint rc = w->real->functions->GetEnv(w->real, reinterpret_cast<void**>(&tmp), ver);
    if (rc != JNI_OK) { *out = nullptr; return rc; }
  }
  *out = w->wrap_env;
  return JNI_OK;
}
}  // namespace
#endif  // __x86_64__

JavaVM* BionicRuntime::wrap_java_vm(JavaVM* real_vm, JNIEnv* real_env) {
#if defined(__x86_64__)
  if (!real_vm || !real_env) return nullptr;
  // Up-call thunks (ToGlibc) still use this constant pair. That is correct exactly because a
  // native can only run on an attached thread, and the only attached thread is the one that
  // bootstrapped — an up-call always originates inside a down-call on that same thread. Per-thread
  // attach must revisit this: the pair becomes per-thread state, reachable from bionic %fs.
  g_jni_glibc_fs = glibc_fs_;
  g_jni_bionic_fs = bionic_fs_;

  // Attach the bootstrapping thread: give it the bionic TCB __linker_init built, and record the
  // slot's fixed displacement from the thread pointer for the trampolines. Called on that thread,
  // under its glibc %fs (ART's nativeLoad -> load_library), so ndk_get_fs() is its glibc base.
  t_bionic_fs = bionic_fs_;
  g_tls_disp = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(&t_bionic_fs)) -
               static_cast<std::int64_t>(ndk_get_fs());
  if (g_tls_disp < INT32_MIN || g_tls_disp > INT32_MAX) {
    log::e(kTag, "bionic loader: TLS slot displacement {} does not fit a disp32", g_tls_disp);
    return nullptr;  // the trampoline encodes it as a disp32; refuse rather than emit a wrong one
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

  g_wrap_env.functions = &g_env_tbl;
  g_wrap_env.real = real_env;
  g_vm_tbl = *real_vm->functions;
  g_vm_tbl.GetEnv = w_GetEnv;
  g_wrap_vm.functions = &g_vm_tbl;
  g_wrap_vm.real = real_vm;
  g_wrap_vm.wrap_env = reinterpret_cast<JNIEnv*>(&g_wrap_env);
  wrap_env_ = reinterpret_cast<JNIEnv*>(&g_wrap_env);
  return reinterpret_cast<JavaVM*>(&g_wrap_vm);
#else
  (void)real_vm;
  (void)real_env;
  return nullptr;
#endif
}

void* BionicRuntime::load_library(const char* abs_path, JavaVM* real_vm, JNIEnv* real_env,
                                  std::string* error) {
#if defined(__x86_64__)
  void* h = load(abs_path, error);
  if (!h) return nullptr;
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
