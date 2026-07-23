// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.nsd.NsdManager; public type surface from AOSP
// android.jar (API 33) stubs (Apache-2.0). Backed by the host mDNS/DNS-SD daemon
// (Avahi) through org.acl.internal.NsdBridge -> svc/nsd. The real class ships in the
// com.android.tethering mainline module, absent from an apex-split host boot
// classpath. No GPL-3.0 sources.
package android.net.nsd;

/**
 * acl service-discovery shim, backed by the host's mDNS/DNS-SD daemon. Discovery is
 * genuine: {@link #discoverServices} browses the real network, {@link #resolveService}
 * returns the host/port/TXT the daemon resolved, and {@link #registerService}
 * advertises on the LAN — so cast/DLNA/printer/IoT apps find the same devices host
 * programs do.
 *
 * <p>When no discovery daemon is reachable, listeners are failed with
 * {@link #FAILURE_INTERNAL_ERROR} rather than left hanging, which is what an app
 * expects when the platform cannot discover.
 */
public final class NsdManager {
    public static final int PROTOCOL_DNS_SD = 0x1;

    public static final int FAILURE_INTERNAL_ERROR = 0;
    public static final int FAILURE_ALREADY_ACTIVE = 3;
    public static final int FAILURE_MAX_LIMIT = 4;

    public interface DiscoveryListener {
        void onStartDiscoveryFailed(String serviceType, int errorCode);
        void onStopDiscoveryFailed(String serviceType, int errorCode);
        void onDiscoveryStarted(String serviceType);
        void onDiscoveryStopped(String serviceType);
        void onServiceFound(NsdServiceInfo serviceInfo);
        void onServiceLost(NsdServiceInfo serviceInfo);
    }

    public interface ResolveListener {
        void onResolveFailed(NsdServiceInfo serviceInfo, int errorCode);
        void onServiceResolved(NsdServiceInfo serviceInfo);
    }

    public interface RegistrationListener {
        void onRegistrationFailed(NsdServiceInfo serviceInfo, int errorCode);
        void onUnregistrationFailed(NsdServiceInfo serviceInfo, int errorCode);
        void onServiceRegistered(NsdServiceInfo serviceInfo);
        void onServiceUnregistered(NsdServiceInfo serviceInfo);
    }

    // Listener -> native handle, so stop/unregister affects exactly the right browse.
    private final java.util.Map<DiscoveryListener, Long> discoveries =
            java.util.Collections.synchronizedMap(new java.util.HashMap<DiscoveryListener, Long>());
    private final java.util.Map<RegistrationListener, Long> registrations =
            java.util.Collections.synchronizedMap(
                    new java.util.HashMap<RegistrationListener, Long>());

    public NsdManager() {}

    private static NsdServiceInfo info(String name, String type, String host, String address,
                                       int port, String[] txt) {
        NsdServiceInfo si = new NsdServiceInfo();
        si.setServiceName(name);
        si.setServiceType(type);
        si.setPort(port);
        if (address != null && !address.isEmpty()) {
            try {
                // Numeric literal from the daemon — no DNS lookup.
                si.setHost(java.net.InetAddress.getByName(address));
            } catch (Throwable ignored) {}
        }
        if (txt != null) {
            for (int i = 0; i + 1 < txt.length; i += 2) si.setAttribute(txt[i], txt[i + 1]);
        }
        return si;
    }

    public void discoverServices(final String serviceType, int protocolType,
                                 final DiscoveryListener listener) {
        if (listener == null) return;
        if (!org.acl.internal.NsdBridge.available()) {
            listener.onStartDiscoveryFailed(serviceType, FAILURE_INTERNAL_ERROR);
            return;
        }
        if (discoveries.containsKey(listener)) {
            listener.onStartDiscoveryFailed(serviceType, FAILURE_ALREADY_ACTIVE);
            return;
        }
        long h = org.acl.internal.NsdBridge.startDiscovery(
                serviceType, new org.acl.internal.NsdBridge.DiscoverySink() {
                    @Override public void onFound(String name, String type, String domain) {
                        listener.onServiceFound(info(name, type, null, null, 0, null));
                    }
                    @Override public void onLost(String name, String type, String domain) {
                        listener.onServiceLost(info(name, type, null, null, 0, null));
                    }
                });
        if (h == 0L) {
            listener.onStartDiscoveryFailed(serviceType, FAILURE_INTERNAL_ERROR);
            return;
        }
        discoveries.put(listener, h);
        listener.onDiscoveryStarted(serviceType);
    }

    public void stopServiceDiscovery(DiscoveryListener listener) {
        if (listener == null) return;
        Long h = discoveries.remove(listener);
        if (h == null) {
            listener.onStopDiscoveryFailed(null, FAILURE_INTERNAL_ERROR);
            return;
        }
        org.acl.internal.NsdBridge.stopDiscovery(h);
        listener.onDiscoveryStopped(null);
    }

    public void resolveService(final NsdServiceInfo serviceInfo, final ResolveListener listener) {
        if (listener == null) return;
        if (serviceInfo == null || !org.acl.internal.NsdBridge.available()) {
            listener.onResolveFailed(serviceInfo, FAILURE_INTERNAL_ERROR);
            return;
        }
        final boolean started = org.acl.internal.NsdBridge.resolve(
                serviceInfo.getServiceName(), serviceInfo.getServiceType(), "local",
                new org.acl.internal.NsdBridge.ResolveSink() {
                    @Override public void onResolved(boolean ok, String name, String type,
                                                     String domain, String host, String address,
                                                     int port, String[] txt) {
                        if (!ok) {
                            listener.onResolveFailed(serviceInfo, FAILURE_INTERNAL_ERROR);
                            return;
                        }
                        listener.onServiceResolved(info(name, type, host, address, port, txt));
                    }
                });
        if (!started) listener.onResolveFailed(serviceInfo, FAILURE_INTERNAL_ERROR);
    }

    public void registerService(NsdServiceInfo serviceInfo, int protocolType,
                                RegistrationListener listener) {
        if (listener == null) return;
        if (serviceInfo == null || !org.acl.internal.NsdBridge.available()) {
            listener.onRegistrationFailed(serviceInfo, FAILURE_INTERNAL_ERROR);
            return;
        }
        final java.util.Map<String, byte[]> attrs = serviceInfo.getAttributes();
        String[] txt = new String[attrs.size() * 2];
        int i = 0;
        for (java.util.Map.Entry<String, byte[]> e : attrs.entrySet()) {
            txt[i++] = e.getKey();
            txt[i++] = e.getValue() == null ? "" : new String(e.getValue());
        }
        long h = org.acl.internal.NsdBridge.registerService(
                serviceInfo.getServiceName(), serviceInfo.getServiceType(), "local",
                serviceInfo.getPort(), txt);
        if (h == 0L) {
            listener.onRegistrationFailed(serviceInfo, FAILURE_INTERNAL_ERROR);
            return;
        }
        registrations.put(listener, h);
        listener.onServiceRegistered(serviceInfo);
    }

    public void unregisterService(RegistrationListener listener) {
        if (listener == null) return;
        Long h = registrations.remove(listener);
        if (h == null) {
            listener.onUnregistrationFailed(null, FAILURE_INTERNAL_ERROR);
            return;
        }
        org.acl.internal.NsdBridge.unregisterService(h);
        listener.onServiceUnregistered(null);
    }
}
