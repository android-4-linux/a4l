// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l. The functions and their contracts are those of
// bionic's public sys/system_properties.h (Apache-2.0); no implementation was
// copied. No GPL-3.0 sources.
//
// System properties for an app's native code. bionic maps them from
// /dev/__properties__, which only init can make: the files have to be owned by
// root. Without it every native read comes back empty, and a library that
// reads ro.build.version.sdk sees 0. Chromium then takes /dev/ashmem over
// ASharedMemory and no page loads; another app's start-up check aborts on
// "Device SDK version should have been at least 21 but was 0".
//
// TARGET code, compiled for bionic and staged as libacl_sysprops.so. It is
// linked -z global and loaded before any library of the app, which puts it
// ahead of libc in every later library's symbol lookup: these are the
// functions an app library binds. libc's own callers are not reached; they
// were bound when libc was linked.
//
// The values are the table the runtime sets for android.os.SystemProperties
// (acl_system_props.h). They do not change: nothing here plays init, so a set
// is refused and a wait runs out its time.
//
// A prop_info is an entry of the table. Its serial carries the value's length
// in the top byte, where callers of the older API look for it.
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include "acl_system_props.h"

#define ACL_EXPORT __attribute__((visibility("default")))

#define ACL_PROP_COUNT (sizeof(kAclSystemProps) / sizeof(kAclSystemProps[0]))

static const struct acl_system_prop* acl_prop_of(const prop_info* pi) {
  return (const struct acl_system_prop*)pi;
}

// Copies at most cap-1 bytes and ends the string. Returns the length copied.
static size_t acl_prop_copy(char* dst, const char* src, size_t cap) {
  size_t n = strlen(src);
  if (n >= cap)
    n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
  return n;
}

ACL_EXPORT const prop_info* __system_property_find(const char* name) {
  if (name == NULL)
    return NULL;
  for (size_t i = 0; i < ACL_PROP_COUNT; i++) {
    if (strcmp(kAclSystemProps[i].key, name) == 0)
      return (const prop_info*)&kAclSystemProps[i];
  }
  return NULL;
}

ACL_EXPORT const prop_info* __system_property_find_nth(unsigned n) {
  return n < ACL_PROP_COUNT ? (const prop_info*)&kAclSystemProps[n] : NULL;
}

// value has room for PROP_VALUE_MAX bytes; name, where given, for
// PROP_NAME_MAX.
ACL_EXPORT int __system_property_read(const prop_info* pi,
                                      char* name,
                                      char* value) {
  if (pi == NULL)
    return -1;
  const struct acl_system_prop* p = acl_prop_of(pi);
  if (name != NULL)
    acl_prop_copy(name, p->key, PROP_NAME_MAX);
  if (value == NULL)
    return (int)strlen(p->value);
  return (int)acl_prop_copy(value, p->value, PROP_VALUE_MAX);
}

ACL_EXPORT int __system_property_get(const char* name, char* value) {
  const prop_info* pi = __system_property_find(name);
  if (pi != NULL)
    return __system_property_read(pi, NULL, value);
  if (value != NULL)
    value[0] = '\0';
  return 0;
}

ACL_EXPORT uint32_t __system_property_serial(const prop_info* pi) {
  if (pi == NULL)
    return 0;
  size_t n = strlen(acl_prop_of(pi)->value);
  if (n >= PROP_VALUE_MAX)
    n = PROP_VALUE_MAX - 1;
  return (uint32_t)n << 24;
}

ACL_EXPORT uint32_t __system_property_area_serial(void) {
  return 0;
}

ACL_EXPORT void __system_property_read_callback(
    const prop_info* pi,
    void (*callback)(void* cookie,
                     const char* name,
                     const char* value,
                     uint32_t serial),
    void* cookie) {
  if (pi == NULL || callback == NULL)
    return;
  const struct acl_system_prop* p = acl_prop_of(pi);
  callback(cookie, p->key, p->value, __system_property_serial(pi));
}

ACL_EXPORT int __system_property_foreach(void (*callback)(const prop_info* pi,
                                                          void* cookie),
                                         void* cookie) {
  if (callback == NULL)
    return -1;
  for (size_t i = 0; i < ACL_PROP_COUNT; i++)
    callback((const prop_info*)&kAclSystemProps[i], cookie);
  return 0;
}

ACL_EXPORT int __system_property_set(const char* key, const char* value) {
  (void)key;
  (void)value;
  return -1;
}

// No property changes, so this never has a new serial to report: it sleeps for
// the timeout and returns false, or does not return where there is none.
ACL_EXPORT bool __system_property_wait(
    const prop_info* pi,
    uint32_t old_serial,
    uint32_t* new_serial_ptr,
    const struct timespec* relative_timeout) {
  (void)pi;
  (void)old_serial;
  (void)new_serial_ptr;
  if (relative_timeout == NULL) {
    for (;;)
      pause();
  }
  struct timespec left = *relative_timeout;
  while (nanosleep(&left, &left) != 0 && errno == EINTR) {
  }
  return false;
}

// android_get_device_api_level() reads ro.build.version.sdk inside libc, which
// has no property area to read. The Flutter engine gates its AHardwareBuffer
// external-texture import on this being 26 or more.
ACL_EXPORT int android_get_device_api_level(void) {
  char value[PROP_VALUE_MAX];
  if (__system_property_get("ro.build.version.sdk", value) <= 0)
    return -1;
  return atoi(value);
}
