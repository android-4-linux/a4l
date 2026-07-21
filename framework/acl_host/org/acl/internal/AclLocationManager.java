// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK android.location.LocationManager / ILocationManager / ILocationListener /
// Location public+@hide surface (AOSP framework-minus-apex classes.jar, Apache-2.0 — signatures
// only). Backed natively by acl::svc::location over host GeoClue2. No AOSP LocationManagerService
// code (that lives in system_server, which acl has not), no the predecessor.
package org.acl.internal;

import android.location.ILocationListener;
import android.location.ILocationManager;
import android.location.Location;
import android.location.LocationManager;
import android.os.IRemoteCallback;
import android.os.SystemClock;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

// acl's LocationManager backing: acl keeps the REAL android.location.LocationManager (all its API
// surface, request throttling, permission plumbing) and substitutes ONLY its Binder service — the
// ILocationManager that AOSP routes to system_server's LocationManagerService, which acl has not
// (ADR-2 shim-at-Manager-boundary). This bridging ILocationManager captures the LocationListener
// transports the framework hands it and drives them from acl's native location hub
// (acl::svc::location, host GeoClue2 over D-Bus). HostBootstrap.getSystemService("location")
// constructs `new LocationManager(context, newService())`.
public final class AclLocationManager {
    private AclLocationManager() {}

    // Registered transports → the provider name each requested (so a delivered Location carries the
    // provider the app asked for). ILocationListener is an in-process Stub here, so calling it
    // directly (no Binder) hands the fix to the app's LocationListener on its own executor.
    private static final Map<ILocationListener, String> sListeners = new ConcurrentHashMap<>();
    private static volatile Location sLast;

    private static native boolean nStart();
    private static native void nStop();
    private static native boolean nAvailable();

    // The bridging ILocationManager for `new LocationManager(context, service)`.
    public static ILocationManager newService() {
        return (ILocationManager) Proxy.newProxyInstance(
                ILocationManager.class.getClassLoader(),
                new Class<?>[]{ILocationManager.class},
                new Handler());
    }

    private static final class Handler implements InvocationHandler {
        @Override public Object invoke(Object proxy, Method m, Object[] a) {
            switch (m.getName()) {
                case "registerLocationListener": {
                    // (String provider, LocationRequest, ILocationListener, String pkg, String tag,
                    //  String listenerId) — capture the transport + provider, start the host source.
                    ILocationListener l = (ILocationListener) a[2];
                    String provider = (a[0] instanceof String) ? (String) a[0]
                            : LocationManager.FUSED_PROVIDER;
                    sListeners.put(l, provider);
                    nStart();
                    Location last = sLast;
                    if (last != null) deliver(l, provider, last);  // prime with the last fix
                    return null;
                }
                case "unregisterLocationListener": {
                    if (a != null && a.length > 0 && a[0] instanceof ILocationListener)
                        sListeners.remove(a[0]);
                    if (sListeners.isEmpty()) nStop();
                    return null;
                }
                case "getLastLocation":
                    nStart();
                    return sLast;
                case "isProviderEnabled":
                case "isProviderEnabledForUser":
                case "hasProvider":
                    return nAvailable();
                case "getProviders":
                case "getAllProviders":
                    return providers();
                case "getBestProvider":
                    return nAvailable() ? LocationManager.FUSED_PROVIDER : null;
                default:
                    return defaultValue(m.getReturnType());
            }
        }
    }

    // acl reports the standard providers as enabled when the host source is available — apps request
    // GPS/NETWORK/FUSED interchangeably and GeoClue is a fused source. PASSIVE included so passive
    // listeners also see fixes.
    private static List<String> providers() {
        if (!nAvailable()) return Collections.emptyList();
        List<String> p = new ArrayList<>(4);
        p.add(LocationManager.GPS_PROVIDER);
        p.add(LocationManager.NETWORK_PROVIDER);
        p.add(LocationManager.FUSED_PROVIDER);
        p.add(LocationManager.PASSIVE_PROVIDER);
        return p;
    }

    // Native up-call (acl::svc::location hub thread): a fix arrived. Build a Location, cache it as
    // last-known, and hand it to every registered transport tagged with the provider it asked for.
    static void onLocationChanged(double lat, double lon, double accuracy, double altitude,
                                  double speed, double bearing, long timeMs, boolean hasAltitude,
                                  boolean hasSpeed, boolean hasBearing) {
        Location base = new Location(LocationManager.FUSED_PROVIDER);
        base.setLatitude(lat);
        base.setLongitude(lon);
        base.setAccuracy((float) accuracy);
        if (hasAltitude) base.setAltitude(altitude);
        if (hasSpeed) base.setSpeed((float) speed);
        if (hasBearing) base.setBearing((float) bearing);
        base.setTime(timeMs);
        base.setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos());
        sLast = base;
        for (Map.Entry<ILocationListener, String> e : sListeners.entrySet())
            deliver(e.getKey(), e.getValue(), base);
    }

    private static void deliver(ILocationListener l, String provider, Location base) {
        try {
            Location loc = new Location(base);
            loc.setProvider(provider);
            // ILocationListener.onLocationChanged(List<Location>, IRemoteCallback) — no completion
            // callback needed (the transport skips it when null).
            List<Location> one = new ArrayList<>(1);
            one.add(loc);
            l.onLocationChanged(one, (IRemoteCallback) null);
        } catch (Throwable t) {
            // A dead/removed transport throws; drop it so we stop pushing to it.
            sListeners.remove(l);
        }
    }

    private static Object defaultValue(Class<?> t) {
        if (!t.isPrimitive()) return null;
        if (t == boolean.class) return Boolean.FALSE;
        if (t == void.class) return null;
        if (t == char.class) return (char) 0;
        return 0;  // byte/short/int/long/float/double auto-box+widen
    }
}
