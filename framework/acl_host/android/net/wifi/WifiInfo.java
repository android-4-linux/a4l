// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.wifi.WifiInfo; public type surface from AOSP
// android.jar (API 33) stubs (Apache-2.0). Values are a point-in-time snapshot of
// the host wireless association, taken from svc/net (NetworkManager's active
// access point) via WifiManager's native seam. No GPL-3.0 sources.
package android.net.wifi;

/**
 * A snapshot of the current Wi-Fi association, as {@link WifiManager#getConnectionInfo}
 * returns it. Backed by the host network daemon's view of the wireless device — SSID,
 * BSSID, signal strength, frequency and negotiated rate all come from the access point
 * the host is actually associated with.
 *
 * <p>When the host has no wireless device, the radio is off, or nothing is associated,
 * the accessors return the same "not connected" values a device reports in that state
 * ({@code <unknown ssid>}, a null BSSID, network id -1).
 */
public class WifiInfo {
    /** Android's sentinel for an unavailable SSID (matches the platform constant). */
    public static final String SSID_NONE = "<unknown ssid>";

    private final boolean connected;
    private final String ssid;      // unquoted, "" when unknown
    private final String bssid;     // "" when unknown
    private final int rssi;
    private final int linkSpeed;
    private final int frequency;

    WifiInfo(boolean connected, String ssid, String bssid, int rssi, int linkSpeed, int frequency) {
        this.connected = connected;
        this.ssid = ssid != null ? ssid : "";
        this.bssid = bssid != null ? bssid : "";
        this.rssi = rssi;
        this.linkSpeed = linkSpeed;
        this.frequency = frequency;
    }

    /** The SSID, double-quoted as the platform returns it, or {@code <unknown ssid>}. */
    public String getSSID() {
        if (!connected || ssid.isEmpty()) return SSID_NONE;
        return "\"" + ssid + "\"";
    }

    /** The associated access point's hardware address, or null when not associated. */
    public String getBSSID() { return (connected && !bssid.isEmpty()) ? bssid : null; }

    /** Signal strength in dBm (-127 when unknown). */
    public int getRssi() { return rssi; }

    /** Negotiated link speed in Mbps, or -1 when unknown. */
    public int getLinkSpeed() { return linkSpeed; }

    /** Channel frequency in MHz (0 when unknown). */
    public int getFrequency() { return frequency; }

    /** acl exposes a single association; 0 when connected, -1 (NONE) otherwise. */
    public int getNetworkId() { return connected ? 0 : -1; }

    public boolean getHiddenSSID() { return false; }

    /**
     * Android returns this anonymised address to apps that lack location permission
     * (the platform stopped handing out the real MAC in API 23); acl always does, since
     * exposing the host's hardware address to an app would be worse than a device.
     */
    public String getMacAddress() { return "02:00:00:00:00:00"; }

    /** Deprecated IPv4-as-int accessor; acl does not surface the address here. */
    public int getIpAddress() { return 0; }

    public SupplicantState getSupplicantState() {
        return connected ? SupplicantState.COMPLETED : SupplicantState.DISCONNECTED;
    }

    @Override public String toString() {
        return "WifiInfo{SSID=" + getSSID() + ", BSSID=" + getBSSID() + ", rssi=" + rssi
                + ", linkSpeed=" + linkSpeed + ", frequency=" + frequency + "}";
    }
}
