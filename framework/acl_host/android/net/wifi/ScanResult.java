// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.wifi.ScanResult; public field/type surface from
// AOSP android.jar (API 33) stubs (Apache-2.0). acl performs no scans, so this type
// exists so scan-result APIs compile and iterate over an empty list. No GPL-3.0 sources.
package android.net.wifi;

/**
 * One access point from a Wi-Fi scan. acl does not drive the host's wireless device,
 * so {@link WifiManager#getScanResults()} is always empty and no instance of this type
 * is produced; it exists so apps that reference the scan API keep resolving.
 */
public class ScanResult {
    /** Network name (SSID). */
    public String SSID;
    /** Access point hardware address. */
    public String BSSID;
    /** Capability string, e.g. "[WPA2-PSK-CCMP][ESS]". */
    public String capabilities;
    /** Signal strength in dBm. */
    public int level;
    /** Channel frequency in MHz. */
    public int frequency;
    /** Microsecond timestamp of the observation. */
    public long timestamp;
    /** Channel width, one of the CHANNEL_WIDTH values. */
    public int channelWidth;

    public static final int CHANNEL_WIDTH_20MHZ = 0;
    public static final int WIFI_STANDARD_UNKNOWN = 0;
    public static final int WIFI_STANDARD_LEGACY = 1;
    public static final int WIFI_STANDARD_11N = 4;
    public static final int WIFI_STANDARD_11AC = 5;
    public static final int WIFI_STANDARD_11AX = 6;

    /** One information element of an access point's beacon. */
    public static class InformationElement {
        private final int id;
        private final int idExt;
        private final byte[] bytes;

        InformationElement(int id, int idExt, byte[] bytes) {
            this.id = id;
            this.idExt = idExt;
            this.bytes = bytes != null ? bytes.clone() : new byte[0];
        }

        public int getId() { return id; }
        public int getIdExt() { return idExt; }
        public java.nio.ByteBuffer getBytes() {
            return java.nio.ByteBuffer.wrap(bytes).asReadOnlyBuffer();
        }
    }

    public ScanResult() {}

    public String getSsid() { return SSID; }
    public String getBssid() { return BSSID; }
    /** The beacon's information elements. acl reads no beacon, so there are none. */
    public java.util.List<InformationElement> getInformationElements() {
        return java.util.Collections.emptyList();
    }
    public int getWifiStandard() { return WIFI_STANDARD_UNKNOWN; }

    @Override public String toString() {
        return "ScanResult{SSID=" + SSID + ", BSSID=" + BSSID + ", level=" + level
                + ", frequency=" + frequency + "}";
    }
}
