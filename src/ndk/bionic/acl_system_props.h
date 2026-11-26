// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l. No GPL-3.0 sources.
//
// The system properties a4l reports. Read by the runtime, which sets them for
// android.os.SystemProperties, and by a4l's libandroid.so, which answers an
// app's native __system_property_* calls from them. One table, so Java and
// native code in the same app read the same Build.VERSION.SDK_INT.
//
// Plain C: this is compiled for the host (C++) and for bionic (C). The values
// are placeholders, not a claim of a real device.
#ifndef ACL_NDK_BIONIC_ACL_SYSTEM_PROPS_H_
#define ACL_NDK_BIONIC_ACL_SYSTEM_PROPS_H_

struct acl_system_prop {
  const char* key;
  const char* value;
};

static const struct acl_system_prop kAclSystemProps[] = {
    {"ro.product.cpu.abilist", "x86_64,x86"},
    {"ro.product.cpu.abilist64", "x86_64"},
    {"ro.product.cpu.abilist32", "x86"},
    {"ro.build.version.sdk", "33"},
    {"ro.build.version.release", "13"},
    {"ro.build.version.codename", "REL"},
    {"ro.build.version.incremental", "acl"},
    {"ro.build.version.all_codenames", "REL"},
    {"ro.build.version.known_codenames", "REL"},
    {"ro.build.version.preview_sdk", "0"},
    {"ro.build.version.preview_sdk_fingerprint", "REL"},
    {"ro.build.version.min_supported_target_sdk", "0"},
    {"ro.build.version.security_patch", "2022-01-01"},
    {"ro.build.id", "acl"},
    {"ro.build.display.id", "acl"},
    {"ro.build.type", "eng"},
    {"ro.build.tags", "test-keys"},
    {"ro.build.flavor", "acl-eng"},
    {"ro.build.date.utc", "0"},
    {"ro.build.host", "acl"},
    {"ro.build.user", "acl"},
    {"ro.build.fingerprint", "acl/acl/acl:13/acl/acl:eng/test-keys"},
    {"ro.product.name", "acl"},
    {"ro.product.device", "acl"},
    {"ro.product.board", "acl"},
    {"ro.product.brand", "acl"},
    {"ro.product.model", "acl"},
    {"ro.product.manufacturer", "acl"},
    {"ro.hardware", "acl"},
    {"ro.bootloader", "acl"},
    {"ro.secure", "0"},
    {"ro.debuggable", "1"},
    {"ro.dalvik.vm.native.bridge", "0"},
};

#endif  // ACL_NDK_BIONIC_ACL_SYSTEM_PROPS_H_
