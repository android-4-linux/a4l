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
    /**
     * The host's HTTP proxy, or null when it has none (the common desktop case, and the
     * truthful answer — apps then connect directly, exactly as host programs do).
     *
     * <p>Read from the standard proxy environment (http_proxy, falling back to
     * https_proxy, with no_proxy as the exclusion list) — the convention every Linux
     * program honors and which a launched app inherits. A desktop-only proxy set
     * solely in GNOME's gsettings and never exported to the environment is not seen
     * here; that is a known fidelity gap rather than a fabricated value.
     */
    public ProxyInfo getHttpProxy() {
        String spec = System.getenv("http_proxy");
        if (spec == null || spec.isEmpty()) spec = System.getenv("https_proxy");
        if (spec == null || spec.isEmpty()) spec = System.getenv("HTTP_PROXY");
        if (spec == null || spec.isEmpty()) return null;
        try {
            // Accept both "host:port" and a full "http://user:pass@host:port/" URL.
            String s = spec.trim();
            int scheme = s.indexOf("://");
            if (scheme >= 0) s = s.substring(scheme + 3);
            int slash = s.indexOf('/');
            if (slash >= 0) s = s.substring(0, slash);
            int at = s.lastIndexOf('@');
            if (at >= 0) s = s.substring(at + 1);          // strip credentials
            int colon = s.lastIndexOf(':');
            String h = colon > 0 ? s.substring(0, colon) : s;
            int p = colon > 0 ? Integer.parseInt(s.substring(colon + 1)) : 80;
            if (h.isEmpty() || p <= 0) return null;

            String no = System.getenv("no_proxy");
            if (no == null || no.isEmpty()) no = System.getenv("NO_PROXY");
            java.util.List<String> excl = new java.util.ArrayList<String>();
            if (no != null) {
                for (String e : no.split(",")) {
                    String t = e.trim();
                    if (!t.isEmpty()) excl.add(t);
                }
            }
            return ProxyInfo.buildDirectProxy(h, p, excl);
        } catch (Throwable ignored) {
            return null;  // malformed proxy spec: behave as if unset
        }
    }

    public String getDomains() { return null; }
    public String getInterfaceName() { return null; }
    public boolean isPrivateDnsActive() { return false; }
    public String getPrivateDnsServerName() { return null; }
}
