// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.nsd.NsdServiceInfo; public type surface from
// AOSP android.jar (API 33) stubs (Apache-2.0). The real class ships in the
// com.android.tethering mainline module, absent from an apex-split host boot
// classpath. No GPL-3.0 sources.
package android.net.nsd;

/**
 * Identity and (once resolved) connection details of a network service. acl fills
 * these from the host mDNS/DNS-SD daemon: a browse yields the name/type, and a
 * resolve adds the host, port and TXT attributes.
 */
public final class NsdServiceInfo {
    private String serviceName;
    private String serviceType;
    private java.net.InetAddress host;
    private int port;
    private final java.util.Map<String, byte[]> attributes = new java.util.HashMap<>();

    public NsdServiceInfo() {}

    public String getServiceName() { return serviceName; }
    public void setServiceName(String s) { serviceName = s; }

    public String getServiceType() { return serviceType; }
    public void setServiceType(String s) { serviceType = s; }

    public java.net.InetAddress getHost() { return host; }
    public void setHost(java.net.InetAddress a) { host = a; }

    public int getPort() { return port; }
    public void setPort(int p) { port = p; }

    /** TXT records. Values are raw bytes, as the platform exposes them. */
    public java.util.Map<String, byte[]> getAttributes() {
        return java.util.Collections.unmodifiableMap(attributes);
    }

    public void setAttribute(String key, String value) {
        if (key == null || key.isEmpty()) return;
        attributes.put(key, value == null ? new byte[0] : value.getBytes());
    }

    public void setAttribute(String key, byte[] value) {
        if (key == null || key.isEmpty()) return;
        attributes.put(key, value == null ? new byte[0] : value);
    }

    public void removeAttribute(String key) { attributes.remove(key); }

    @Override public String toString() {
        return "NsdServiceInfo{name=" + serviceName + ", type=" + serviceType
                + ", host=" + host + ", port=" + port + ", txt=" + attributes.size() + "}";
    }
}
