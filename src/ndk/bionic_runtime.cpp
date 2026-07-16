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

#include <cstdlib>
#include <filesystem>
#include <string_view>

#if defined(__x86_64__)
#include <asm/prctl.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#endif

#include "log/log.h"

namespace acl::ndk {
namespace {
constexpr std::string_view kTag = "acl.ndk";
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

// Bootstrap the linker; on success fill the loader fn ptrs and bionic %fs. Returns true/false.
// (%fs is restored to glibc before returning.)
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

  const std::uintptr_t glibc_fs = get_fs();
  reinterpret_cast<std::uintptr_t (*)(void*)>(init_addr)(kab);  // sets %fs -> permanent bionic TCB
  *out_bionic_fs = get_fs();
  set_fs(glibc_fs);
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

// Entered under bionic %fs (from the bionic .so); restore glibc for the real call, then bionic.
struct ToGlibc {
  ToGlibc() { ndk_set_fs(g_jni_glibc_fs); }
  ~ToGlibc() { ndk_set_fs(g_jni_bionic_fs); }
};

jint JNICALL w_GetVersion(JNIEnv* e) {
  JNIEnv* r = reinterpret_cast<WrapEnv*>(e)->real;
  ToGlibc g;
  return r->functions->GetVersion(r);
}
jclass JNICALL w_FindClass(JNIEnv* e, const char* n) {
  JNIEnv* r = reinterpret_cast<WrapEnv*>(e)->real;
  ToGlibc g;
  return r->functions->FindClass(r, n);
}
jint JNICALL w_RegisterNatives(JNIEnv* e, jclass c, const JNINativeMethod* m, jint n) {
  JNIEnv* r = reinterpret_cast<WrapEnv*>(e)->real;
  ToGlibc g;
  return r->functions->RegisterNatives(r, c, m, n);
}
jint JNICALL w_GetEnv(JavaVM* vm, void** out, jint ver) {
  WrapVM* w = reinterpret_cast<WrapVM*>(vm);
  JNIEnv* tmp = nullptr;
  {
    ToGlibc g;
    const jint rc = w->real->functions->GetEnv(w->real, reinterpret_cast<void**>(&tmp), ver);
    if (rc != JNI_OK) { *out = nullptr; return rc; }
  }
  *out = w->wrap_env;  // hand the bionic caller the wrapped env, not the raw ART one
  return JNI_OK;
}
}  // namespace
#endif  // __x86_64__

JavaVM* BionicRuntime::wrap_java_vm(JavaVM* real_vm, JNIEnv* real_env) {
#if defined(__x86_64__)
  if (!real_vm || !real_env) return nullptr;
  g_jni_glibc_fs = glibc_fs_;
  g_jni_bionic_fs = bionic_fs_;
  g_env_tbl = *real_env->functions;  // copy every slot, then thunk the ones we support
  g_env_tbl.GetVersion = w_GetVersion;
  g_env_tbl.FindClass = w_FindClass;
  g_env_tbl.RegisterNatives = w_RegisterNatives;
  g_wrap_env.functions = &g_env_tbl;
  g_wrap_env.real = real_env;
  g_vm_tbl = *real_vm->functions;
  g_vm_tbl.GetEnv = w_GetEnv;
  g_wrap_vm.functions = &g_vm_tbl;
  g_wrap_vm.real = real_vm;
  g_wrap_vm.wrap_env = reinterpret_cast<JNIEnv*>(&g_wrap_env);
  return reinterpret_cast<JavaVM*>(&g_wrap_vm);
#else
  (void)real_vm;
  (void)real_env;
  return nullptr;
#endif
}
#endif  // ACL_HAVE_JNI

}  // namespace acl::ndk
