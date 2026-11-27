// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture. No GPL-3.0 sources.
//
// A bionic library for run_ndk_bootstrap_test.py. answer() goes through what a
// library's first call touches: the allocator, stdio formatting, errno and a
// thread-local of its own, all of which live behind the thread pointer the
// loader has to have put in place. 42 if they all worked.
//
// acl_ndk_heap_path makes a system call with a path from the allocator. On
// arm64 bionic tags that pointer's top byte, and the kernel refuses it with
// EFAULT on a thread that does not have the tagged address ABI on.
//
// Java_com_acl_ndktest_Main_nativeAnswer is the name `acl-ndk-probe --thread`
// calls on a second thread it has attached; it is the same function.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static __thread int t_counter = 40;

int answer(void* a, void* b) {
  (void)a;
  (void)b;
  char* p = malloc(64);
  if (!p)
    return -1;
  snprintf(p, 64, "%d", 1);
  errno = 0;
  t_counter += atoi(p) + (int)strlen(p);
  free(p);
  return t_counter + errno;
}

int acl_ndk_heap_path(unsigned* top_byte) {
  char* p = strdup("/");
  if (!p)
    return -1;
  *top_byte = (unsigned)((unsigned long)p >> 56);
  int r = access(p, F_OK) == 0 ? 0 : errno;
  free(p);
  return r;
}

int Java_com_acl_ndktest_Main_nativeAnswer(void* env, void* cls) {
  // A thread that was just attached starts from the thread-local's initial
  // value, 40, not from what another thread left in it.
  return answer(env, cls);
}
