// SPDX-License-Identifier: Apache-2.0
// Provenance: acl host-ART runtime bootstrap; libcore.io.Os/ForwardingOs is the Java syscall seam
// (AOSP core-libart, Apache-2.0). Behavioral: Android app-uid model (apps run as 10000+, never a
// privileged uid). No GPL-3.0 sources.
//
// A libcore.io.Os wrapper that reports a normal Android APP uid from getuid()/geteuid() instead of
// the host user's real uid. On acl the app runs as the invoking host user; when that user's uid is
// a privileged Android uid — 0 (root), 1000 (SYSTEM), 1001 (PHONE), ... — framework code that gates
// on Process.myUid() (which is Os.getuid()) misbehaves. The most visible case: WebViewFactory throws
// "For security reasons, WebView is not allowed in privileged processes" when myUid()==SYSTEM_UID
// (1000), which is exactly a uid-1000 desktop user. Other framework paths compare myUid() to
// SYSTEM_UID / <FIRST_APPLICATION_UID too. Reporting a fixed application uid fixes them all.
//
// Only the Java identity (Process.myUid, getuid/geteuid) is remapped; the REAL syscall uid used for
// actual file I/O (open/stat/chown via bionic/glibc) is untouched, so on-disk ownership is unchanged.
// Everything except getuid/geteuid forwards to the real Os (ForwardingOs).
package org.acl.internal;

public final class AclOs extends libcore.io.ForwardingOs {
    // Android's first application uid (Process.FIRST_APPLICATION_UID). Any value >= this reads as a
    // normal, non-privileged app to the framework.
    private static final int ACL_APP_UID = 10000;

    private AclOs(libcore.io.Os delegate) { super(delegate); }

    @Override public int getuid() { return ACL_APP_UID; }
    @Override public int geteuid() { return ACL_APP_UID; }

    /** Install the app-uid wrapper over the current Libcore.os, once. Idempotent + best-effort. */
    public static void install() {
        try {
            libcore.io.Os cur = libcore.io.Libcore.os;
            if (cur instanceof AclOs) return;                 // already installed
            if (cur.getuid() >= ACL_APP_UID) return;          // host uid already a normal app uid
            libcore.io.Libcore.compareAndSetOs(cur, new AclOs(cur));
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: app-uid (Libcore.os) wrap skipped: " + t);
        }
    }
}
