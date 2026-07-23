// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.ProxyInfo; public type surface from AOSP
// android.jar (API 33) stubs (Apache-2.0). Real ProxyInfo lives in the
// com.android.tethering mainline module (absent from $BOOT), so acl provides a
// minimal one, populated from the host's proxy configuration. No GPL-3.0 sources.
package android.net;

/**
 * An HTTP proxy configuration, as {@link LinkProperties#getHttpProxy()} reports it.
 * acl derives this from the host's proxy settings rather than inventing one, so an app
 * that honors the platform proxy reaches the network the same way host programs do.
 */
public class ProxyInfo {
    private final String host;
    private final int port;
    private final String exclusionList;

    private ProxyInfo(String host, int port, String exclusionList) {
        this.host = host;
        this.port = port;
        this.exclusionList = exclusionList != null ? exclusionList : "";
    }

    public static ProxyInfo buildDirectProxy(String host, int port) {
        return new ProxyInfo(host, port, "");
    }

    public static ProxyInfo buildDirectProxy(String host, int port,
                                             java.util.List<String> exclList) {
        return new ProxyInfo(host, port, exclList == null ? "" : String.join(",", exclList));
    }

    public String getHost() { return host; }

    public int getPort() { return port; }

    /** Hosts that bypass the proxy, comma-separated (the platform's encoding). */
    public String getExclusionList() { return exclusionList; }

    public String[] getExclusionListAsStringArray() {
        if (exclusionList == null || exclusionList.isEmpty()) return new String[0];
        return exclusionList.split(",");
    }

    /** acl derives proxies from host settings, which carry no PAC script. */
    public Uri getPacFileUrl() { return Uri.EMPTY; }

    public boolean isValid() { return host != null && !host.isEmpty() && port > 0; }

    @Override public String toString() {
        return "ProxyInfo{" + host + ":" + port
                + (exclusionList.isEmpty() ? "" : " exclude=" + exclusionList) + "}";
    }
}
