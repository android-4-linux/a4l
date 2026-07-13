// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.Network; public type surface from AOSP
// android.jar (API 33) stubs (Apache-2.0). Real Network wraps a netId; acl models
// the single default network as an opaque token. No GPL-3.0 sources.
package android.net;

// acl connectivity shim: on an apex-split host image the real android.net.* live in
// the com.android.tethering mainline module (absent from $BOOT). These minimal
// clean-room classes sit on the boot classpath and are backed by svc/net (a host
// D-Bus connectivity daemon) via ConnectivityManager's native methods.
public final class Network {
    // The one default network acl exposes. getActiveNetwork() returns this (or null).
    static final Network DEFAULT = new Network();
    private Network() {}
}
