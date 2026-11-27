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
// acl_ndk_time_env reads the three variables bionic's time zone code looks
// tzdata up under, then converts a time: a bionic built for a host aborts in
// gmtime_r when one of them is not in its environment. 42 if ANDROID_DATA is
// what the test set, the other two are set, and the date is right.
//
// Java_com_acl_ndktest_Main_nativeAnswer is the name `acl-ndk-probe --thread`
// calls on a second thread it has attached; it is the same function.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

int acl_ndk_time_env(void* a, void* b) {
  (void)a;
  (void)b;
  const char* data = getenv("ANDROID_DATA");
  if (!data || strcmp(data, "/acl-ndk-test/data") != 0)
    return 1;
  if (!getenv("ANDROID_TZDATA_ROOT"))
    return 2;
  if (!getenv("ANDROID_ROOT"))
    return 3;
  const time_t day_400 = 400 * 86400;  // 1971-02-05
  struct tm tm;
  if (!gmtime_r(&day_400, &tm))
    return 4;
  return tm.tm_year == 71 && tm.tm_mon == 1 && tm.tm_mday == 5 ? 42 : 5;
}

int Java_com_acl_ndktest_Main_nativeAnswer(void* env, void* cls) {
  // A thread that was just attached starts from the thread-local's initial
  // value, 40, not from what another thread left in it.
  return answer(env, cls);
}
