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

    public ScanResult() {}

    public String getSsid() { return SSID; }
    public String getBssid() { return BSSID; }

    @Override public String toString() {
        return "ScanResult{SSID=" + SSID + ", BSSID=" + BSSID + ", level=" + level
                + ", frequency=" + frequency + "}";
    }
}
