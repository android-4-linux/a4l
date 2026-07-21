// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.ConnectivityManager (+ NetworkCallback);
// public method surface from AOSP android.jar (API 33) stubs (Apache-2.0). Backed
// by acl's svc/net host D-Bus connectivity backend via the nGetState() native
// (see src/svc/net/connectivity_jni.cpp). No GPL-3.0 sources.
package android.net;

import java.util.concurrent.CopyOnWriteArrayList;

// acl connectivity manager: instead of transacting with a (missing) system
// ConnectivityService over Binder, it reads the host connectivity daemon
// (systemd-networkd / NetworkManager / ConnMan) that svc/net translates.
public class ConnectivityManager {
    // Legacy network types (AOSP android.jar API 33) for getActiveNetworkInfo().
    public static final int TYPE_MOBILE = 0;
    public static final int TYPE_WIFI = 1;
    public static final int TYPE_ETHERNET = 9;

    // Registered callbacks; acl delivers the current state on registration and
    // (future) live changes when the svc/net sink is wired to JNI dispatch.
    private final CopyOnWriteArrayList<NetworkCallback> callbacks = new CopyOnWriteArrayList<>();

    public ConnectivityManager() {}

    public Network getActiveNetwork() {
        return (nGetState() & 0x1) != 0 ? Network.DEFAULT : null;
    }

    public NetworkCapabilities getNetworkCapabilities(Network network) {
        if (network == null) return null;
        return NetworkCapabilities.fromPacked(nGetState());
    }

    public NetworkInfo getActiveNetworkInfo() {
        int st = nGetState();
        if ((st & 0x1) == 0) return null;
        return new NetworkInfo(true, legacyType((st >> 3) & 0x7));
    }

    public boolean isActiveNetworkMetered() {
        return (nGetState() & 0x4) != 0;
    }

    // acl models a single default network; report its info for any live Network. Chromium's
    // AndroidNetworkLibrary/NetworkChangeNotifier reads this for the WebView network state.
    public NetworkInfo getNetworkInfo(Network network) {
        if (network == null) return null;
        return getActiveNetworkInfo();
    }
    public NetworkInfo getNetworkInfo(int networkType) {
        return getActiveNetworkInfo();
    }
    public Network[] getAllNetworks() {
        return getActiveNetwork() != null ? new Network[]{ Network.DEFAULT } : new Network[0];
    }
    public NetworkInfo[] getAllNetworkInfo() {
        NetworkInfo ni = getActiveNetworkInfo();
        return ni != null ? new NetworkInfo[]{ ni } : new NetworkInfo[0];
    }
    public LinkProperties getLinkProperties(Network network) {
        return network != null ? new LinkProperties() : null;
    }

    public void registerDefaultNetworkCallback(NetworkCallback cb) { register(cb); }
    public void registerDefaultNetworkCallback(NetworkCallback cb, android.os.Handler h) { register(cb); }
    public void registerNetworkCallback(NetworkRequest req, NetworkCallback cb) { register(cb); }
    public void registerNetworkCallback(NetworkRequest req, NetworkCallback cb, android.os.Handler h) { register(cb); }
    public void unregisterNetworkCallback(NetworkCallback cb) { callbacks.remove(cb); }

    // Default-network activity listener (chromium's NetworkActiveNotifier). acl has one always-on
    // default network, so there is no idle/active transition to report — accept + drop.
    public interface OnNetworkActiveListener { void onNetworkActive(); }
    public void addDefaultNetworkActiveListener(OnNetworkActiveListener l) {}
    public void removeDefaultNetworkActiveListener(OnNetworkActiveListener l) {}
    public boolean isDefaultNetworkActive() { return getActiveNetwork() != null; }

    private void register(NetworkCallback cb) {
        callbacks.addIfAbsent(cb);
        // Deliver the current state immediately — androidx WorkManager's tracker
        // reads it here to seed its constraint state.
        int st = nGetState();
        if ((st & 0x1) != 0) {
            cb.onAvailable(Network.DEFAULT);
            cb.onCapabilitiesChanged(Network.DEFAULT, NetworkCapabilities.fromPacked(st));
        } else {
            cb.onLost(Network.DEFAULT);
        }
    }

    private static int legacyType(int transport) {  // packed transport → TYPE_*
        switch (transport) {
            case 1: return TYPE_WIFI;
            case 2: return TYPE_ETHERNET;
            case 3: return TYPE_MOBILE;
            default: return TYPE_ETHERNET;
        }
    }

    // Concrete (empty) callback apps subclass; acl invokes the overrides it delivers.
    public static class NetworkCallback {
        public NetworkCallback() {}
        public NetworkCallback(int flags) {}
        public void onAvailable(Network network) {}
        public void onLosing(Network network, int maxMsToLive) {}
        public void onLost(Network network) {}
        public void onUnavailable() {}
        public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {}
        public void onBlockedStatusChanged(Network network, boolean blocked) {}
    }

    // Packed connectivity snapshot from svc/net: bit0 connected, bit1 validated,
    // bit2 metered, bits3-5 transport (0 none,1 wifi,2 eth,3 cellular).
    private static native int nGetState();
}
