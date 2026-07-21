// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.NetworkInfo (+ State); public method + enum
// surface from AOSP android.jar (API 33) stubs (Apache-2.0). Legacy API kept for
// apps that predate NetworkCapabilities. No GPL-3.0 sources.
package android.net;

public class NetworkInfo {
    public enum State { CONNECTING, CONNECTED, SUSPENDED, DISCONNECTING, DISCONNECTED, UNKNOWN }
    public enum DetailedState {
        IDLE, SCANNING, CONNECTING, AUTHENTICATING, OBTAINING_IPADDR, CONNECTED, SUSPENDED,
        DISCONNECTING, DISCONNECTED, FAILED, BLOCKED, VERIFYING_POOR_LINK, CAPTIVE_PORTAL_CHECK
    }

    private final boolean connected;
    private final int type;  // legacy ConnectivityManager.TYPE_*

    NetworkInfo(boolean connected, int type) {
        this.connected = connected;
        this.type = type;
    }

    public boolean isConnected() { return connected; }
    public boolean isConnectedOrConnecting() { return connected; }
    public boolean isAvailable() { return connected; }
    public State getState() { return connected ? State.CONNECTED : State.DISCONNECTED; }
    public int getType() { return type; }
    public String getTypeName() {
        switch (type) {
            case ConnectivityManager.TYPE_WIFI: return "WIFI";
            case ConnectivityManager.TYPE_MOBILE: return "MOBILE";
            case ConnectivityManager.TYPE_ETHERNET: return "ETHERNET";
            default: return "NONE";
        }
    }
    // acl has no mobile-network subtype; chromium's NetworkChangeNotifier reads these for the
    // connection-type/subtype it reports to WebView. Report unknown subtype + a plain connected state.
    public int getSubtype() { return 0; }                 // TelephonyManager.NETWORK_TYPE_UNKNOWN
    public String getSubtypeName() { return ""; }
    public DetailedState getDetailedState() {
        return connected ? DetailedState.CONNECTED : DetailedState.DISCONNECTED;
    }
    public String getReason() { return null; }
    public String getExtraInfo() { return null; }
    public boolean isRoaming() { return false; }
    public boolean isFailover() { return false; }
}
