// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl spike. Bootstrap contract read from AOSP bionic
// linker source (Apache-2.0): linker/linker_main.cpp (__linker_init, standalone
// AT_ENTRY==&_start path), libc/bionic/__libc_init_main_thread.cpp (permanent
// TCB migration), private/KernelArgumentBlock.h. The two-libc %fs save/restore
// technique is the class of thing libhybris (Apache-2.0) does; not vendored. No
// GPL-3.0 sources.
//
// In-process bionic linker bootstrap (docs/ndk-scope.md Stage 0b). From a glibc
// process: map the bionic linker64, hand it a synthetic KernelArgumentBlock
// (standalone mode, a minimal bionic stub exe as argv[1]), call __linker_init
// DIRECTLY (not via _start's `jmp *%rax`) so we keep control, then use the
// linker's exported __loader_dlopen/__loader_dlsym to load a bionic .so and
// resolve a symbol. Every bionic call runs with %fs=bionic; we restore
// %fs=glibc around each so glibc/ART code keeps its own thread pointer.
// Compiled -fno-stack-protector (canary lives at %fs:0x28).

#include <asm/prctl.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bionic_bootstrap.h"

namespace acl::ndk::spike {
namespace {

constexpr std::size_t kPage = 4096;
inline std::uintptr_t page_down(std::uintptr_t v) {
  return v & ~(std::uintptr_t{kPage} - 1);
}
inline std::uintptr_t page_up(std::uintptr_t v) {
  return page_down(v + kPage - 1);
}

// --- raw, TLS-free logging (safe under either %fs) ---
void raw(const char* s) {
  syscall(SYS_write, 2, s, std::strlen(s));
}
void rawx(const char* label, std::uintptr_t v) {
  char buf[32];
  char* p = buf + sizeof(buf);
  *--p = '\n';
  if (v == 0)
    *--p = '0';
  for (; v; v >>= 4)
    *--p = "0123456789abcdef"[v & 0xf];
  *--p = 'x';
  *--p = '0';
  raw(label);
  syscall(SYS_write, 2, p, buf + sizeof(buf) - p);
}

// --- %fs (thread-pointer) save/restore ---
std::uintptr_t get_fs() {
  std::uintptr_t v = 0;
  syscall(SYS_arch_prctl, ARCH_GET_FS, &v);
  return v;
}
void set_fs(std::uintptr_t v) {
  syscall(SYS_arch_prctl, ARCH_SET_FS, v);
}

// --- map a PIE ELF's PT_LOAD segments kernel-style; return load base (== bias
// for a PIE) ---
struct Mapped {
  std::uintptr_t base = 0;
  std::uintptr_t entry = 0;  // base + e_entry (the linker's _start)
  std::uintptr_t phdr = 0;   // base + e_phoff
  std::uint16_t phnum = 0;
};

bool map_pie(const char* path, Mapped& out) {
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    raw("map_pie: open failed\n");
    return false;
  }
  Elf64_Ehdr eh{};
  if (::pread(fd, &eh, sizeof(eh), 0) != (ssize_t)sizeof(eh)) {
    ::close(fd);
    return false;
  }
  Elf64_Phdr ph[64];
  if (eh.e_phnum > 64) {
    ::close(fd);
    return false;
  }
  ::pread(fd, ph, eh.e_phnum * sizeof(Elf64_Phdr), eh.e_phoff);

  std::uintptr_t maxv = 0;
  for (int i = 0; i < eh.e_phnum; ++i)
    if (ph[i].p_type == PT_LOAD)
      maxv = std::max<std::uintptr_t>(maxv, ph[i].p_vaddr + ph[i].p_memsz);
  const std::size_t span = page_up(maxv);

  // Reserve the whole span (PIE min vaddr is 0 → base is the load bias).
  void* base =
      ::mmap(nullptr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) {
    ::close(fd);
    return false;
  }
  const auto b = reinterpret_cast<std::uintptr_t>(base);

  for (int i = 0; i < eh.e_phnum; ++i) {
    const Elf64_Phdr& p = ph[i];
    if (p.p_type != PT_LOAD)
      continue;
    const std::uintptr_t start = b + p.p_vaddr;
    const std::uintptr_t astart = page_down(start);
    const off_t aoff = static_cast<off_t>(page_down(p.p_offset));
    const std::size_t fmap = (start - astart) + p.p_filesz;
    int prot = (p.p_flags & PF_R ? PROT_READ : 0) |
               (p.p_flags & PF_W ? PROT_WRITE : 0) |
               (p.p_flags & PF_X ? PROT_EXEC : 0);
    // Map file-backed with WRITE forced on so we can zero the bss tail;
    // re-protect after.
    if (fmap > 0) {
      void* r = ::mmap(reinterpret_cast<void*>(astart), fmap, prot | PROT_WRITE,
                       MAP_PRIVATE | MAP_FIXED, fd, aoff);
      if (r == MAP_FAILED) {
        raw("map_pie: seg map failed\n");
        ::close(fd);
        return false;
      }
    }
    // Zero-fill the remainder of the last file page, then anon-map full bss
    // pages.
    const std::uintptr_t file_end = start + p.p_filesz;
    const std::uintptr_t mem_end = start + p.p_memsz;
    if (mem_end > file_end) {
      const std::uintptr_t tail = std::min(page_up(file_end), mem_end);
      if (tail > file_end)
        std::memset(reinterpret_cast<void*>(file_end), 0, tail - file_end);
      if (page_up(file_end) < page_up(mem_end)) {
        void* r =
            ::mmap(reinterpret_cast<void*>(page_up(file_end)),
                   page_up(mem_end) - page_up(file_end), prot | PROT_WRITE,
                   MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
        if (r == MAP_FAILED) {
          raw("map_pie: bss map failed\n");
          return false;
        }
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

// --- resolve a symbol's runtime address from .symtab or .dynsym (file section
// headers) ---
std::uintptr_t find_sym(const char* path,
                        std::uintptr_t base,
                        const char* name) {
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  const off_t sz = ::lseek(fd, 0, SEEK_END);
  void* m = ::mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (m == MAP_FAILED)
    return 0;
  const auto* f = reinterpret_cast<const std::uint8_t*>(m);
  const auto& eh = *reinterpret_cast<const Elf64_Ehdr*>(f);
  const auto* sh = reinterpret_cast<const Elf64_Shdr*>(f + eh.e_shoff);
  std::uintptr_t result = 0;
  for (int i = 0; i < eh.e_shnum && result == 0; ++i) {
    if (sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM)
      continue;
    const auto* sym = reinterpret_cast<const Elf64_Sym*>(f + sh[i].sh_offset);
    const char* str =
        reinterpret_cast<const char*>(f + sh[sh[i].sh_link].sh_offset);
    const std::size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
    for (std::size_t s = 0; s < n; ++s)
      if (sym[s].st_value && std::strcmp(str + sym[s].st_name, name) == 0) {
        result = base + sym[s].st_value;
        break;
      }
  }
  ::munmap(m, sz);
  return result;
}

// --- synthetic KernelArgumentBlock:
// [argc][argv..][0][envp..][0][auxv..][AT_NULL] ---
std::uintptr_t g_block[256];
char g_argv0[] = "acl-linker64";
char g_ldpath[512];

void* build_kab(const Mapped& lk,
                const char* stub_path,
                const char* bionic_libdir) {
  std::snprintf(g_ldpath, sizeof(g_ldpath), "LD_LIBRARY_PATH=%s",
                bionic_libdir);
  std::size_t k = 0;
  g_block[k++] = 2;                                          // argc
  g_block[k++] = reinterpret_cast<std::uintptr_t>(g_argv0);  // argv[0]
  g_block[k++] = reinterpret_cast<std::uintptr_t>(
      stub_path);    // argv[1] = stub exe to "load"
  g_block[k++] = 0;  // argv NULL
  g_block[k++] = reinterpret_cast<std::uintptr_t>(g_ldpath);  // envp[0]
  g_block[k++] = 0;                                           // envp NULL
  auto aux = [&](std::uint64_t type, std::uint64_t val) {
    g_block[k++] = type;
    g_block[k++] = val;
  };
  // Copy pointer/scalar auxv from our own process (valid in this address
  // space).
  aux(AT_PAGESZ, kPage);
  aux(AT_CLKTCK, getauxval(AT_CLKTCK));
  aux(AT_HWCAP, getauxval(AT_HWCAP));
  aux(AT_HWCAP2, getauxval(AT_HWCAP2));
  aux(AT_SYSINFO_EHDR, getauxval(AT_SYSINFO_EHDR));  // vdso
  aux(AT_RANDOM, getauxval(AT_RANDOM));              // 16 valid random bytes
  aux(AT_PLATFORM, getauxval(AT_PLATFORM));
  aux(AT_SECURE, 0);  // honor LD_LIBRARY_PATH
  aux(AT_UID, getuid());
  aux(AT_EUID, geteuid());
  aux(AT_GID, getgid());
  aux(AT_EGID, getegid());
  aux(AT_FLAGS, 0);
  aux(AT_EXECFN, reinterpret_cast<std::uintptr_t>(g_argv0));
  // Describe the mapped linker; AT_ENTRY == linker _start triggers standalone
  // mode.
  aux(AT_BASE, lk.base);
  aux(AT_PHDR, lk.phdr);
  aux(AT_PHNUM, lk.phnum);
  aux(AT_ENTRY, lk.entry);
  aux(AT_NULL, 0);
  return g_block;
}

}  // namespace

int run(const Config& cfg) {
  Mapped lk;
  if (!map_pie(cfg.linker_path, lk)) {
    raw("bootstrap: map linker failed\n");
    return -1;
  }
  rawx("bootstrap: linker base ", lk.base);

  const std::uintptr_t init_addr =
      find_sym(cfg.linker_path, lk.base, "__dl___linker_init");
  const std::uintptr_t dlopen_addr =
      find_sym(cfg.linker_path, lk.base, "__dl___loader_dlopen");
  const std::uintptr_t dlsym_addr =
      find_sym(cfg.linker_path, lk.base, "__dl___loader_dlsym");
  const std::uintptr_t dlerror_addr =
      find_sym(cfg.linker_path, lk.base, "__dl___loader_dlerror");
  if (!init_addr || !dlopen_addr || !dlsym_addr) {
    raw("bootstrap: missing linker symbols\n");
    return -2;
  }
  rawx("bootstrap: __linker_init  ", init_addr);
  rawx("bootstrap: __loader_dlopen", dlopen_addr);

  void* kab = build_kab(lk, cfg.stub_path, cfg.bionic_libdir);

  using init_fn = std::uintptr_t (*)(void*);
  using dlopen_fn = void* (*)(const char*, int, const void*);
  using dlsym_fn = void* (*)(void*, const char*, const void*);
  auto linker_init = reinterpret_cast<init_fn>(init_addr);
  auto loader_dlopen = reinterpret_cast<dlopen_fn>(dlopen_addr);
  auto loader_dlsym = reinterpret_cast<dlsym_fn>(dlsym_addr);

  const std::uintptr_t glibc_fs = get_fs();
  raw("bootstrap: calling __linker_init (%fs -> bionic)\n");
  linker_init(
      kab);  // runs the whole linker init; sets %fs to a permanent bionic TCB
  const std::uintptr_t bionic_fs = get_fs();
  set_fs(glibc_fs);
  raw("bootstrap: __linker_init returned; linker up\n");
  rawx("bootstrap: bionic %fs ", bionic_fs);

  // Load the bionic .so + resolve the symbol, all under %fs=bionic.
  auto loader_dlerror = reinterpret_cast<char* (*)()>(dlerror_addr);
  set_fs(bionic_fs);
  void* h = loader_dlopen(cfg.so_path, RTLD_NOW,
                          reinterpret_cast<void*>(dlopen_addr));
  char* err = (!h && dlerror_addr) ? loader_dlerror() : nullptr;
  void* fn =
      h ? loader_dlsym(h, cfg.symbol, reinterpret_cast<void*>(dlopen_addr))
        : nullptr;
  set_fs(glibc_fs);

  if (!h) {
    raw("bootstrap: dlopen returned NULL: ");
    raw(err ? err : "(no dlerror)");
    raw("\n");
    return -3;
  }
  if (!fn) {
    raw("bootstrap: dlsym returned NULL\n");
    return -4;
  }
  rawx("bootstrap: symbol ", reinterpret_cast<std::uintptr_t>(fn));

  // Call it under %fs=bionic. The repro nativeAnswer(JNIEnv*,jclass) ignores
  // both args.
  set_fs(bionic_fs);
  int result = reinterpret_cast<int (*)(void*, void*)>(fn)(nullptr, nullptr);
  set_fs(glibc_fs);
  return result;
}

}  // namespace acl::ndk::spike
