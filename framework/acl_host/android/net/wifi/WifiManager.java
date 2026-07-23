// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.wifi.WifiManager; public type surface from AOSP
// android.jar (API 33) stubs (Apache-2.0). Backed by svc/net (the host connectivity
// daemon — NetworkManager's WirelessEnabled + the wireless device's active access
// point) through the native seam below. No GPL-3.0 sources.
package android.net.wifi;

/**
 * acl Wi-Fi shim: the real android.net.wifi.* live in the com.android.wifi mainline
 * module, absent from an apex-split host image, so this minimal clean-room class sits
 * on the boot classpath and answers from the host network daemon via svc/net.
 *
 * <p>Reads are genuine: {@link #isWifiEnabled} follows the host's wireless radio
 * switch, and {@link #getConnectionInfo} reports the access point the host is actually
 * associated with (SSID, BSSID, signal, frequency, negotiated rate).
 *
 * <p>Mutations are not: acl will not reconfigure the host's networking on an app's
 * behalf, so {@link #setWifiEnabled} refuses (returning false, exactly as the platform
 * does for non-privileged apps since API 29) and scans are not started. Multicast and
 * Wi-Fi locks are accepted and tracked but hold nothing — a desktop host has no
 * multicast filter or Wi-Fi power-save state to suspend, so there is nothing to
 * release; apps that acquire them (mDNS/SSDP discovery, react-native-udp) work
 * unchanged.
 */
public class WifiManager {
    public static final int WIFI_STATE_DISABLING = 0;
    public static final int WIFI_STATE_DISABLED = 1;
    public static final int WIFI_STATE_ENABLING = 2;
    public static final int WIFI_STATE_ENABLED = 3;
    public static final int WIFI_STATE_UNKNOWN = 4;

    // Wi-Fi lock modes (android.net.wifi.WifiManager).
    public static final int WIFI_MODE_FULL = 1;
    public static final int WIFI_MODE_SCAN_ONLY = 2;
    public static final int WIFI_MODE_FULL_HIGH_PERF = 3;
    public static final int WIFI_MODE_FULL_LOW_LATENCY = 4;

    // Broadcast actions apps filter on (declared so filters/constants resolve).
    public static final String WIFI_STATE_CHANGED_ACTION = "android.net.wifi.WIFI_STATE_CHANGED";
    public static final String NETWORK_STATE_CHANGED_ACTION =
            "android.net.wifi.STATE_CHANGE";
    public static final String SCAN_RESULTS_AVAILABLE_ACTION =
            "android.net.wifi.SCAN_RESULTS";
    public static final String EXTRA_WIFI_STATE = "wifi_state";

    // svc/net seam: [enabled, connected, rssi(dBm), linkSpeed(Mbps), frequency(MHz)].
    private static native int[] nWifiSnapshot();
    private static native String nSsid();
    private static native String nBssid();

    public WifiManager() {}

    private static int[] snapshot() {
        try {
            int[] s = nWifiSnapshot();
            if (s != null && s.length >= 5) return s;
        } catch (Throwable ignored) { /* natives absent */ }
        return new int[]{0, 0, -127, -1, 0};  // no wireless device visible
    }

    private static String ssid() {
        try { String s = nSsid(); return s != null ? s : ""; }
        catch (Throwable ignored) { return ""; }
    }

    private static String bssid() {
        try { String s = nBssid(); return s != null ? s : ""; }
        catch (Throwable ignored) { return ""; }
    }

    /** True when the host's wireless radio is switched on. */
    public boolean isWifiEnabled() { return snapshot()[0] != 0; }

    public int getWifiState() {
        return isWifiEnabled() ? WIFI_STATE_ENABLED : WIFI_STATE_DISABLED;
    }

    /**
     * acl does not reconfigure host networking for an app. The platform likewise
     * refuses for ordinary apps from API 29 onward, so returning false is the same
     * answer a modern device gives.
     */
    public boolean setWifiEnabled(boolean enabled) { return false; }

    /** The current association, from the host's active access point. */
    public WifiInfo getConnectionInfo() {
        int[] s = snapshot();
        final boolean connected = s[1] != 0;
        return new WifiInfo(connected, connected ? ssid() : "", connected ? bssid() : "",
                            s[2], s[3], s[4]);
    }

    /** No scan is initiated (acl does not drive the host's wireless device). */
    public boolean startScan() { return false; }

    /** acl surfaces no scan results; callers must tolerate an empty list. */
    public java.util.List<ScanResult> getScanResults() {
        return java.util.Collections.emptyList();
    }

    public boolean is5GHzBandSupported() { return true; }
    public boolean isScanAlwaysAvailable() { return false; }
    public boolean isP2pSupported() { return false; }

    /**
     * Map an RSSI to a bucket in {@code [0, numLevels)}, using the platform's linear
     * fit over the usable range (-100 dBm .. -55 dBm).
     */
    public static int calculateSignalLevel(int rssi, int numLevels) {
        if (numLevels <= 1) return 0;
        final int min = -100, max = -55;
        if (rssi <= min) return 0;
        if (rssi >= max) return numLevels - 1;
        final float range = max - min;
        return (int) ((float) (rssi - min) * (numLevels - 1) / range);
    }

    /** Signed difference of two RSSI values, as the platform defines it. */
    public static int compareSignalLevel(int rssiA, int rssiB) { return rssiA - rssiB; }

    public MulticastLock createMulticastLock(String tag) { return new MulticastLock(tag); }

    public WifiLock createWifiLock(int lockType, String tag) { return new WifiLock(tag); }

    public WifiLock createWifiLock(String tag) { return new WifiLock(tag); }

    /**
     * On a device this suspends the Wi-Fi multicast filter so the app can receive
     * multicast/broadcast packets (mDNS, SSDP). A Linux host applies no such filter to
     * its wireless interface, so there is nothing to suspend: the lock is tracked for
     * correct {@link #isHeld} semantics and otherwise does nothing. Apps that rely on
     * multicast reception keep working because the packets were never filtered.
     */
    public static class MulticastLock {
        private final String tag;
        private boolean referenceCounted = true;
        private int refCount;

        MulticastLock(String tag) { this.tag = tag; }

        public void acquire() {
            synchronized (this) { if (referenceCounted) refCount++; else refCount = 1; }
        }

        public void release() {
            synchronized (this) {
                if (referenceCounted) {
                    if (refCount > 0) refCount--;
                } else {
                    refCount = 0;
                }
            }
        }

        public void setReferenceCounted(boolean value) {
            synchronized (this) { referenceCounted = value; }
        }

        public boolean isHeld() { synchronized (this) { return refCount > 0; } }

        @Override public String toString() { return "MulticastLock{" + tag + " held=" + isHeld() + "}"; }
    }

    /**
     * On a device this keeps the Wi-Fi radio out of power-save while held. The host's
     * wireless power management is the operating system's business, not an app's, so
     * acl tracks the lock without changing host state. (A wake lock — which acl does
     * honor — is the mechanism that actually keeps the machine awake; see svc/power.)
     */
    public static class WifiLock {
        private final String tag;
        private boolean referenceCounted = true;
        private int refCount;

        WifiLock(String tag) { this.tag = tag; }

        public void acquire() {
            synchronized (this) { if (referenceCounted) refCount++; else refCount = 1; }
        }

        public void release() {
            synchronized (this) {
                if (referenceCounted) {
                    if (refCount > 0) refCount--;
                } else {
                    refCount = 0;
                }
            }
        }

        public void setReferenceCounted(boolean value) {
            synchronized (this) { referenceCounted = value; }
        }

        public boolean isHeld() { synchronized (this) { return refCount > 0; } }

        @Override public String toString() { return "WifiLock{" + tag + " held=" + isHeld() + "}"; }
    }
}
