// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.LinkProperties; public type surface from AOSP
// android.jar (API 33) stubs (Apache-2.0). Real LinkProperties lives in the
// com.android.tethering mainline module (absent from $BOOT), so acl provides a
// minimal one. No GPL-3.0 sources.
package android.net;

// acl connectivity shim (see android.net.Network): the properties of the single default
// network. Chromium's WebView (AndroidNetworkLibrary/DnsConfigServiceAndroid) reads DNS
// config from here to seed its built-in async resolver. acl models a single default
// network mirroring the host, so report the host's resolv.conf nameservers — otherwise
// the WebView's DnsConfig is empty ("Failed to read DnsConfig") and every hostname (e.g.
// map tile servers) fails to resolve, leaving remote content blank.
public final class LinkProperties {
    public LinkProperties() {}

    public java.util.List<java.net.InetAddress> getDnsServers() {
        java.util.List<java.net.InetAddress> out = new java.util.ArrayList<java.net.InetAddress>();
        try (java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.FileReader("/etc/resolv.conf"))) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.startsWith("#") || !line.startsWith("nameserver")) continue;
                String[] parts = line.split("\\s+");
                if (parts.length < 2) continue;
                try {
                    // Numeric literal only — no DNS lookup (would recurse / need the servers).
                    out.add(java.net.InetAddress.getByName(parts[1]));
                } catch (Throwable ignored) {}
            }
        } catch (Throwable ignored) {}
        return out;
    }
    public String getDomains() { return null; }
    public String getInterfaceName() { return null; }
    public boolean isPrivateDnsActive() { return false; }
    public String getPrivateDnsServerName() { return null; }
}
