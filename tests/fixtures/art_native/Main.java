// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK internals (android.os.SystemProperties,
// a @hide native-backed API). No GPL-3.0 sources.
//
// Probe for the ART native floor (P2.2, ADR-0008): a static main that calls a
// framework native method (android.os.SystemProperties.get, backed by native_get in
// libandroid_runtime). Reached via reflection so it needs no compile-time framework
// dependency. run_native_floor_art runs this on host ART with the real framework +
// ACL_ART_NATIVE and asserts the native executed (no UnsatisfiedLinkError) — i.e. acl
// registered the real framework natives from the host libandroid_runtime.
package com.acl.hello;

import java.lang.reflect.Method;

public class Main {
    public static void main(String[] args) throws Exception {
        Class<?> sp = Class.forName("android.os.SystemProperties");
        Method get = sp.getMethod("get", String.class, String.class);
        Object v = get.invoke(null, "ro.build.version.sdk", "unknown");
        System.out.println("NATIVE-FLOOR PROBE: SystemProperties.get(ro.build.version.sdk)=" + v);
    }
}
