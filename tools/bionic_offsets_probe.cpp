// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl tooling. Compiled against the private headers of the
// AOSP bionic (Apache-2.0) that tools/stage-bionic-runtime.sh stages, to emit
// that build's struct offsets. The marker technique is the Linux kernel's
// asm-offsets.c trick. No GPL-3.0 sources.
//
// Attaching an ART (glibc) thread to bionic means building a bionic TCB by
// hand, which means knowing bionic's INTERNAL layout (pthread_internal_t,
// StaticTlsLayout, TlsModules, TlsDtv) — none of it exported, and all of it
// free to change between bionic versions. Rather than hand-copying those
// structs into acl (where they would silently drift out of sync and corrupt
// memory), this probe asks the compiler for the real offsets of the very bionic
// we stage.
//
// It is never linked or run: gen-bionic-offsets.sh compiles it to ASSEMBLY, and
// each DEF below emits one line of it. `#` starts a comment in x86 gas, so the
// emitted line passes through the integrated assembler untouched while already
// being a valid C preprocessor directive — the generated header is literally
// the grep of those lines. %c0 prints the constant bare (no '$').
//
// Anything read here must be paired with the STAGED bionic at run time; see
// gen-bionic-offsets.sh.
#include <stddef.h>

// StaticTlsLayout keeps its offsets private and exposes them only through
// accessors, which are not constant expressions. This probe is a build-time
// measuring tool, not a consumer of the API.
#define private public
#include "private/bionic_elf_tls.h"
#include "private/bionic_globals.h"
#undef private

#include "pthread_internal.h"

#define DEF(sym, val) \
  asm volatile("#define ACL_BIONIC_" #sym " %c0" : : "i"(val))

void acl_bionic_offsets() {
  // libc_shared_globals: reached at run time via the linker's exported
  // __loader_shared_globals.
  DEF(OFF_SG_STATIC_TLS_LAYOUT,
      offsetof(libc_shared_globals, static_tls_layout));
  DEF(OFF_SG_TLS_MODULES, offsetof(libc_shared_globals, tls_modules));

  // StaticTlsLayout: how big a static TLS block is and where bionic's structs
  // sit inside it.
  DEF(OFF_LAYOUT_SIZE,
      offsetof(StaticTlsLayout, offset_));  // size() returns offset_
  DEF(OFF_LAYOUT_ALIGNMENT, offsetof(StaticTlsLayout, alignment_));
  DEF(OFF_LAYOUT_BIONIC_TCB, offsetof(StaticTlsLayout, offset_bionic_tcb_));
  DEF(OFF_LAYOUT_BIONIC_TLS, offsetof(StaticTlsLayout, offset_bionic_tls_));

  // TlsModules + TlsModule[]: the static modules whose init images seed a new
  // block.
  DEF(OFF_TLSMODULES_MODULE_COUNT, offsetof(TlsModules, module_count));
  DEF(OFF_TLSMODULES_MODULE_TABLE, offsetof(TlsModules, module_table));
  DEF(SIZEOF_TLSMODULE, sizeof(TlsModule));
  DEF(OFF_TLSMODULE_STATIC_OFFSET, offsetof(TlsModule, static_offset));
  DEF(OFF_TLSMODULE_SEG_SIZE,
      offsetof(TlsModule, segment) + offsetof(TlsSegment, size));
  DEF(OFF_TLSMODULE_SEG_INIT_PTR,
      offsetof(TlsModule, segment) + offsetof(TlsSegment, init_ptr));
  DEF(OFF_TLSMODULE_SEG_INIT_SIZE,
      offsetof(TlsModule, segment) + offsetof(TlsSegment, init_size));

  // TlsDtv: acl hands a new thread an all-zero one; bionic lazily builds the
  // real DTV.
  DEF(SIZEOF_TLSDTV, sizeof(TlsDtv));
  DEF(OFF_TLSDTV_GENERATION, offsetof(TlsDtv, generation));

  // pthread_internal_t: acl allocates and fills the few fields a thread needs.
  DEF(SIZEOF_PTHREAD_INTERNAL, sizeof(pthread_internal_t));
  DEF(OFF_PTHREAD_TID, offsetof(pthread_internal_t, tid));
  DEF(OFF_PTHREAD_BIONIC_TLS, offsetof(pthread_internal_t, bionic_tls));
  DEF(OFF_PTHREAD_ATTR, offsetof(pthread_internal_t, attr));
  DEF(OFF_PTHREAD_STACK_TOP, offsetof(pthread_internal_t, stack_top));
  DEF(SIZEOF_BIONIC_TLS, sizeof(bionic_tls));

  // pthread_attr_t's stack fields (acl points them at the glibc thread's real
  // stack).
  DEF(OFF_ATTR_STACK_BASE, offsetof(pthread_attr_t, stack_base));
  DEF(OFF_ATTR_STACK_SIZE, offsetof(pthread_attr_t, stack_size));

  // The TCB itself: BIONIC_TLS_SLOTS void* slots, indexed by the TLS_SLOT_*
  // numbers.
  DEF(SIZEOF_BIONIC_TCB, sizeof(bionic_tcb));
  DEF(TLS_SLOTS, BIONIC_TLS_SLOTS);
  DEF(TLS_SLOT_SELF_, TLS_SLOT_SELF);
  DEF(TLS_SLOT_THREAD_ID_, TLS_SLOT_THREAD_ID);
  DEF(TLS_SLOT_STACK_GUARD_, TLS_SLOT_STACK_GUARD);
  DEF(TLS_SLOT_DTV_, TLS_SLOT_DTV);
  DEF(TLS_SLOT_BIONIC_TLS_, TLS_SLOT_BIONIC_TLS);
  // %fs == static_tls + offset_bionic_tcb only while MIN_TLS_SLOT is 0
  // (x86_64). arm64's is negative — StaticTlsLayout::offset_thread_pointer() is
  // offset_bionic_tcb_ + -MIN_TLS_SLOT*8 — so the port must use this rather
  // than assume the TCB offset.
  DEF(MIN_TLS_SLOT_, MIN_TLS_SLOT);
}
