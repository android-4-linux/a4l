// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam to svc/keystore (Secret Service). Method contracts are acl's own.
// No GPL-3.0 sources.
package org.acl.internal;

/**
 * Native seam onto acl's svc/keystore: persists opaque AndroidKeyStore key blobs (keyed by
 * package + alias) in the login keyring via the freedesktop Secret Service. jvm_vm registers the
 * natives (acl.keystore.register_keystore_natives). {@link AclKeyStoreProvider} is the only caller;
 * it owns the blob format (a serialized JCA key), this layer just stores bytes.
 *
 * <p>All calls are best-effort and synchronous. When the natives are unregistered (svc/keystore
 * absent), every method returns the not-available default, and the provider reports an empty store.
 */
public final class KeystoreBridge {
    private KeystoreBridge() {}

    // Whether the natives are wired (jvm_vm called RegisterNatives). Set false by the fallback so a
    // build without svc/keystore doesn't hit UnsatisfiedLinkError on first use.
    static volatile boolean available = true;

    static native boolean nStore(String pkg, String alias, byte[] blob);
    static native byte[] nLoad(String pkg, String alias);      // null if absent
    static native boolean nContains(String pkg, String alias);
    static native boolean nErase(String pkg, String alias);
    static native String[] nAliases(String pkg);               // never null (may be empty)

    static boolean store(String pkg, String alias, byte[] blob) {
        if (!available) return false;
        try { return nStore(pkg, alias, blob); }
        catch (UnsatisfiedLinkError e) { available = false; return false; }
    }
    static byte[] load(String pkg, String alias) {
        if (!available) return null;
        try { return nLoad(pkg, alias); }
        catch (UnsatisfiedLinkError e) { available = false; return null; }
    }
    static boolean contains(String pkg, String alias) {
        if (!available) return false;
        try { return nContains(pkg, alias); }
        catch (UnsatisfiedLinkError e) { available = false; return false; }
    }
    static boolean erase(String pkg, String alias) {
        if (!available) return false;
        try { return nErase(pkg, alias); }
        catch (UnsatisfiedLinkError e) { available = false; return false; }
    }
    static String[] aliases(String pkg) {
        if (!available) return new String[0];
        try { String[] a = nAliases(pkg); return a != null ? a : new String[0]; }
        catch (UnsatisfiedLinkError e) { available = false; return new String[0]; }
    }
}
