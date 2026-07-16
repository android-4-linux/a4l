<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original acl spike. Grounds docs/ndk-scope.md Stage 0b. Reads AOSP bionic linker
     source (Apache-2.0) for the bootstrap contract; libhybris (Apache-2.0) referenced for the
     two-libc TLS-swap technique, not vendored. No GPL-3.0 sources. -->

# ndk-loader-spike — in-process bionic linker bootstrap (Stage 0b)

The make-or-break experiment for host NDK support (docs/ndk-scope.md): from a **glibc** program,
drive the **bionic** dynamic linker *in-process* to `dlopen` a bionic-ABI `.so` and call one of its
functions — proving app native libs can load inside the glibc host-ART process. x86_64 first (the
dev host; Stage 0 already proved the substrate here); the mechanism ports to arm64 unchanged except
the TLS register / auxv / relocation details.

## Why it's hard (the two crux points, from the bionic source)

1. **No "init-only" linker mode.** `linker_main()` always loads a *main executable*. So we hand it a
   tiny bionic stub exe and call `__linker_init` **directly** (not through `_start`, whose tail
   `jmp *%rax` would jump into the stub) — we take its return value and keep control.
2. **TLS / thread-pointer collision.** `__linker_init` → `__libc_init_main_thread_early` sets the
   thread pointer (`%fs` on x86_64 via `arch_prctl`) to bionic's TCB, clobbering glibc's. So we
   `ARCH_GET_FS` before and `ARCH_SET_FS` back after, and only ever run bionic code (linker, loader,
   app `.so`) with `%fs`=bionic, glibc code (this harness) with `%fs`=glibc. The orchestration is
   built `-fno-stack-protector` (the canary lives at `%fs:0x28`; a mid-function `%fs` swap would
   fail the check).

## Bootstrap steps (bionic_bootstrap.cpp)

1. `map_pie()` — mmap the linker64 PT_LOAD segments at a chosen base (kernel-style).
2. `build_kab()` — synthesize `[argc][argv][NULL][envp][NULL][auxv][AT_NULL]`; copy AT_PAGESZ/
   HWCAP/SYSINFO_EHDR/RANDOM/PLATFORM from our *own* process auxv (valid pointers in our space),
   override AT_BASE/PHDR/PHNUM/ENTRY to describe the mapped linker (standalone mode: AT_ENTRY =
   linker `_start`, argv[1] = the stub exe).
3. `bootstrap()` — save `%fs`; call `__linker_init(kab)`; restore `%fs`; resolve `__loader_dlopen`/
   `__loader_dlsym` from the linker's dynsym.
4. `load()/sym()/call` — with `%fs`=bionic: `__loader_dlopen` the target, `__loader_dlsym` the
   symbol, call it; restore `%fs`; return the result to glibc.

## Run

```sh
tools/ndk-loader-spike/build.sh   # builds the repro .so, the bionic stub exe, and the spike
tools/ndk-loader-spike/out/spike  # expect: nativeAnswer() = 42
```

`AOSP=/path` overrides the AOSP checkout (default /mnt/dev/aosp). Success = `42` printed from the
glibc side after an in-process bionic `dlopen`. Milestone ladder + current rung: see STATUS below.

## STATUS

Rung 1 (substrate, Stage 0): PROVEN (docs/ndk-scope.md).
**Rung 2 (in-process bootstrap): PROVEN (2026-07-16).** `SPIKE: nativeAnswer() = 42 (PASS)`,
deterministic. A glibc process maps linker64, calls `__linker_init` in-process (synthetic KAB,
`%fs` save/restore), then `__loader_dlopen`s the bionic `libndktest.so` — resolving its full dep
graph (bionic `libc`, `liblog`→`libc++`, `libdl`) — `__loader_dlsym`s `nativeAnswer`, and calls it
under `%fs`=bionic (its `__android_log_print` runs, no crash) → 42, then restores `%fs`=glibc.

Findings: (1) `__libc_init_main_thread_late` migrates `%fs` to a permanent mmap'd TCB, so calling
`__linker_init` as a function and returning is safe. (2) The `%fs` swap must bracket every bionic
call; the harness is `-fno-stack-protector`. (3) Bionic **linker input errors are fatal** — a
relative exe path makes the linker `__linker_error`+abort the whole process, so real integration
must pre-validate/abs-path all inputs (and ideally sandbox the bootstrap). (4) The bionic dep graph
must be reachable via `LD_LIBRARY_PATH` (no `ld.config.txt` on host → default namespace).

NEXT (Rung 3+, docs/ndk-scope.md): promote to `src/ndk/` as a real loader; thread the TLS-swap
seam through JNI up/down calls (app native thread ↔ glibc ART); resolve the NDK trampoline `.so`
(`liblog`→acl `log`, `libandroid`/`ANativeWindow`→`wm`) on the app's search path; then the arm64
port (same code, swap `%fs`→`TPIDR_EL0` / auxv / reloc details).
