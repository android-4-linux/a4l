// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK internals (android.os.SystemProperties,
// a @hide native-backed API). No GPL-3.0 sources.
//
// Probe for the ART framework floor (P2.1 + P2.2, ADR-0008): a static main that (1)
// confirms a real framework class is loaded by the BOOT classloader — i.e. the real
// framework is on the bootclasspath (P2.1), as on device — and (2) calls a framework
// native method (android.os.SystemProperties.get, backed by native_get in
// libandroid_runtime) to confirm acl registered the real native floor (P2.2). Reached
// via reflection so it needs no compile-time framework dependency. run_native_floor_art
// runs this on host ART with the real framework on the bootclasspath + ACL_ART_NATIVE.
package com.acl.hello;

import java.lang.reflect.Method;

public class Main {
    public static void main(String[] args) throws Exception {
        Class<?> sp = Class.forName("android.os.SystemProperties");
        // A framework class on the bootclasspath is loaded by the boot classloader — on
        // the JVM that is null; on ART it is a java.lang.BootClassLoader instance. Both
        // mean "on the bootclasspath" (P2.1), so normalize to BOOT.
        ClassLoader cl = sp.getClassLoader();
        boolean boot = (cl == null) || cl.getClass().getName().endsWith("BootClassLoader");
        System.out.println("FRAMEWORK-FLOOR PROBE: android.os.SystemProperties classloader="
                + (boot ? "BOOT" : cl.getClass().getName()));
        Method get = sp.getMethod("get", String.class, String.class);
        Object v = get.invoke(null, "ro.build.version.sdk", "unknown");
        System.out.println("FRAMEWORK-FLOOR PROBE: SystemProperties.get(ro.build.version.sdk)=" + v);
    }
}
