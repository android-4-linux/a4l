// SPDX-License-Identifier: Apache-2.0
// Provenance: android.os.BatteryManager / Intent.ACTION_BATTERY_CHANGED extras (AOSP
// android.jar stubs, Apache-2.0); values sourced from the acl svc/battery host UPower
// backend via the native seam. No GPL-3.0 sources.
package org.acl.internal;

import android.content.Intent;
import android.os.BatteryManager;

/**
 * Builds the sticky {@code ACTION_BATTERY_CHANGED} broadcast Intent from the host power
 * daemon (UPower), via the svc/battery native seam. acl's Context.registerReceiver returns
 * this for a battery filter, so apps that read battery level/temperature/status (e.g. via
 * {@code registerReceiver(null, filter).getIntExtra(EXTRA_TEMPERATURE, …)} or a receiver)
 * see real host values instead of stubbed defaults. No Binder, no BatteryService.
 */
public final class BatteryBridge {
    private BatteryBridge() {}

    // svc/battery: [level, scale, temperature(dC), voltage(mV), status, health, plugged,
    // present(0/1)] and the technology string. Bound by acl at host-ART boot (ACL_HAVE_SDBUS);
    // absent → the defaults below.
    private static native int[] nSnapshot();
    private static native String nTechnology();

    /** The current battery state as an ACTION_BATTERY_CHANGED Intent (never null). */
    public static Intent batteryIntent() {
        Intent i = new Intent(Intent.ACTION_BATTERY_CHANGED);
        int[] s = null;
        try { s = nSnapshot(); } catch (Throwable ignored) { /* natives absent */ }
        if (s == null || s.length < 8) s = new int[] { 100, 100, 0, 0, 3, 2, 0, 1 };
        i.putExtra(BatteryManager.EXTRA_LEVEL, s[0]);
        i.putExtra(BatteryManager.EXTRA_SCALE, s[1]);
        i.putExtra(BatteryManager.EXTRA_TEMPERATURE, s[2]);
        i.putExtra(BatteryManager.EXTRA_VOLTAGE, s[3]);
        i.putExtra(BatteryManager.EXTRA_STATUS, s[4]);
        i.putExtra(BatteryManager.EXTRA_HEALTH, s[5]);
        i.putExtra(BatteryManager.EXTRA_PLUGGED, s[6]);
        i.putExtra(BatteryManager.EXTRA_PRESENT, s[7] != 0);
        String tech = "Li-ion";
        try { tech = nTechnology(); } catch (Throwable ignored) { /* keep default */ }
        i.putExtra(BatteryManager.EXTRA_TECHNOLOGY, tech);
        i.putExtra(BatteryManager.EXTRA_ICON_SMALL, 0);
        return i;
    }
}
