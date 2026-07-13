// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.NetworkInfo (+ State); public method + enum
// surface from AOSP android.jar (API 33) stubs (Apache-2.0). Legacy API kept for
// apps that predate NetworkCapabilities. No GPL-3.0 sources.
package android.net;

public class NetworkInfo {
    public enum State { CONNECTING, CONNECTED, SUSPENDED, DISCONNECTING, DISCONNECTED, UNKNOWN }

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
}
