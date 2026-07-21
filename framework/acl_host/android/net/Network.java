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

    // A stable non-zero handle identifying the default network (chromium's NetworkChangeNotifier
    // keys its network map on this; 0 means "no network" so we use 1). netId 1, encoded per the
    // platform's (netId << 32 | 0xfacade) scheme so it round-trips through any handle math.
    public long getNetworkHandle() { return (1L << 32) | 0xfacadeL; }

    // Host-resolver passthrough: acl has one network, so name resolution / connections just use the
    // default host stack (java.net), ignoring the per-network binding a real Network would apply.
    public java.net.InetAddress[] getAllByName(String host) throws java.net.UnknownHostException {
        return java.net.InetAddress.getAllByName(host);
    }
    public java.net.InetAddress getByName(String host) throws java.net.UnknownHostException {
        return java.net.InetAddress.getByName(host);
    }
    public java.net.URLConnection openConnection(java.net.URL url) throws java.io.IOException {
        return url.openConnection();
    }
    public void bindSocket(java.net.Socket socket) {}
    public void bindSocket(java.net.DatagramSocket socket) {}
    public void bindSocket(java.io.FileDescriptor fd) {}
}
