// SPDX-License-Identifier: Apache-2.0
// Provenance: acl shim. Minimal stand-in for AOSP system/core/libutils/include/utils/
// misc.h — the copied glgen android_opengl_GLES*.cpp use ONLY the NELEM macro from it, so
// this avoids linking libutils. No GPL-3.0 sources.
#ifndef ACL_GLES_JNI_SHIM_UTILS_MISC_H
#define ACL_GLES_JNI_SHIM_UTILS_MISC_H

#ifndef NELEM
#define NELEM(x) ((int)(sizeof(x) / sizeof((x)[0])))
#endif

#endif  // ACL_GLES_JNI_SHIM_UTILS_MISC_H
