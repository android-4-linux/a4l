// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.LinkProperties; public type surface from AOSP
// android.jar (API 33) stubs (Apache-2.0). Real LinkProperties lives in the
// com.android.tethering mainline module (absent from $BOOT), so acl provides a
// minimal one. No GPL-3.0 sources.
package android.net;

// acl connectivity shim (see android.net.Network): the properties of the single default
// network. Chromium's WebView (AndroidNetworkLibrary/DnsConfigServiceAndroid) reads DNS
// config from here; acl models no per-link DNS (the host resolver is used natively), so
// these report empty/none and Chromium falls back to its own resolution.
public final class LinkProperties {
    public LinkProperties() {}

    public java.util.List<java.net.InetAddress> getDnsServers() {
        return new java.util.ArrayList<java.net.InetAddress>();
    }
    public String getDomains() { return null; }
    public String getInterfaceName() { return null; }
    public boolean isPrivateDnsActive() { return false; }
    public String getPrivateDnsServerName() { return null; }
}
