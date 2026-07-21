// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK android.os.BatteryManager public surface (AOSP android.jar stubs,
// Apache-2.0 — signatures only). Backed by the host UPower daemon via acl::svc::battery
// (BatteryBridge). No AOSP BatteryService/battery HAL code, no the predecessor.
package org.acl.internal;

import android.os.BatteryManager;

// acl's BatteryManager: the real android.os.BatteryManager reads its BATTERY_PROPERTY_* values
// through an IBatteryPropertiesRegistrar + android.os.BatteryProperty, neither of which is on acl's
// framework boot classpath (they live in the batteryservice, a system_server jar acl doesn't load) —
// so queryProperty() can't run. Subclass it and answer the property reads directly from the host
// UPower snapshot (acl::svc::battery via BatteryBridge), bypassing that Binder/Parcelable path.
//
// Instantiated with Unsafe.allocateInstance (HostBootstrap.allocateHollow), which SKIPS
// BatteryManager's constructors — those resolve the batterystats/batteryproperties Binder services
// acl has not. The overrides below never touch the (null) inherited fields, so a hollow instance is
// safe. Chromium's PowerMonitor.getRemainingBatteryCapacity() reads BATTERY_PROPERTY_CAPACITY here;
// apps also read STATUS. Counters UPower's DisplayDevice doesn't expose return the unknown sentinel.
public final class AclBatteryManager extends BatteryManager {

    @Override
    public int getIntProperty(int id) {
        int[] s = BatteryBridge.snapshot();  // [level,scale,temp,mV,status,health,plugged,present]
        switch (id) {
            case BATTERY_PROPERTY_CAPACITY:
                return s[0];                 // current charge percent (scale is 100)
            case BATTERY_PROPERTY_STATUS:
                return s[4];                 // BATTERY_STATUS_*
            default:
                return Integer.MIN_VALUE;    // charge/current/energy counters: unknown
        }
    }

    @Override
    public long getLongProperty(int id) {
        int v = getIntProperty(id);
        return v == Integer.MIN_VALUE ? Long.MIN_VALUE : (long) v;
    }

    @Override
    public boolean isCharging() {
        int status = BatteryBridge.snapshot()[4];
        return status == BATTERY_STATUS_CHARGING || status == BATTERY_STATUS_FULL;
    }
}
