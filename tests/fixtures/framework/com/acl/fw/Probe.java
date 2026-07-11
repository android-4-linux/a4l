// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture. No GPL-3.0 sources.
//
// A stand-in for a vendored framework class: it lives ONLY on the boot classpath
// (supplied via --framework / VmConfig.boot_classpath), never inside the APK. The
// run-apk-framework integration test proves an app resolves it from the boot
// classpath — the seam the real android.* framework will use (Gate G2.a). This is
// a plumbing probe, NOT a real framework class (hence the neutral package).
package com.acl.fw;

public final class Probe {
  private Probe() {}

  public static String tag() {
    return "acl: framework probe on boot classpath";
  }
}
