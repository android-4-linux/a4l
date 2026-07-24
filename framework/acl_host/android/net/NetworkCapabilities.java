// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.NetworkCapabilities; public constants +
// hasCapability/hasTransport signatures from AOSP android.jar (API 33) stubs
// (Apache-2.0). No GPL-3.0 sources.
package android.net;

// The capability/transport bits apps (and androidx WorkManager) read off the active
// network. Built by ConnectivityManager from the svc/net snapshot.
public final class NetworkCapabilities {
    // Transports (values per AOSP android.jar API 33).
    public static final int TRANSPORT_CELLULAR = 0;
    public static final int TRANSPORT_WIFI = 1;
    public static final int TRANSPORT_ETHERNET = 3;
    // Capabilities.
    public static final int NET_CAPABILITY_NOT_METERED = 11;
    public static final int NET_CAPABILITY_INTERNET = 12;
    public static final int NET_CAPABILITY_VALIDATED = 16;
    public static final int NET_CAPABILITY_NOT_ROAMING = 18;

    private final int transports;    // bitmask: (1 << TRANSPORT_*)
    private final int capabilities;  // bitmask: (1 << NET_CAPABILITY_*)

    NetworkCapabilities(int transports, int capabilities) {
        this.transports = transports;
        this.capabilities = capabilities;
    }

    public boolean hasTransport(int transportType) {
        return (transports & (1 << transportType)) != 0;
    }
    public boolean hasCapability(int capability) {
        return (capabilities & (1 << capability)) != 0;
    }
    // Link bandwidth estimates (Kbps). Apps read these for network-quality decisions -- Pandora's
    // PandoraApp.onCreate calls getLinkUpstreamBandwidthKbps(), and a missing method threw
    // NoSuchMethodError that aborted onCreate. acl has no per-link bandwidth estimate from the host,
    // so report LINK_BANDWIDTH_UNSPECIFIED (0), which is exactly what AOSP returns when unknown.
    public int getLinkUpstreamBandwidthKbps() { return 0; }
    public int getLinkDownstreamBandwidthKbps() { return 0; }
    // Signal strength is likewise unknown; AOSP's sentinel is SIGNAL_STRENGTH_UNSPECIFIED.
    public int getSignalStrength() { return Integer.MIN_VALUE; }

    // Build from ConnectivityManager's packed native state word (see ConnectivityManager).
    static NetworkCapabilities fromPacked(int st) {
        int caps = 0, trans = 0;
        boolean connected = (st & 0x1) != 0;
        boolean validated = (st & 0x2) != 0;
        boolean metered = (st & 0x4) != 0;
        int t = (st >> 3) & 0x7;   // 0 none, 1 wifi, 2 ethernet, 3 cellular
        if (connected) caps |= (1 << NET_CAPABILITY_INTERNET);
        if (validated) caps |= (1 << NET_CAPABILITY_VALIDATED);
        if (!metered)  caps |= (1 << NET_CAPABILITY_NOT_METERED);
        if (t != 3)    caps |= (1 << NET_CAPABILITY_NOT_ROAMING);  // only cellular roams
        if (t == 1)      trans |= (1 << TRANSPORT_WIFI);
        else if (t == 2) trans |= (1 << TRANSPORT_ETHERNET);
        else if (t == 3) trans |= (1 << TRANSPORT_CELLULAR);
        return new NetworkCapabilities(trans, caps);
    }
}
