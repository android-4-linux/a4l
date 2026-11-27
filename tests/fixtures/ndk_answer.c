// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture. No GPL-3.0 sources.
//
// A bionic library for run_ndk_bootstrap_test.py. answer() goes through what a
// library's first call touches: the allocator, stdio formatting, errno and a
// thread-local of its own, all of which live behind the thread pointer the
// loader has to have put in place. 42 if they all worked.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
