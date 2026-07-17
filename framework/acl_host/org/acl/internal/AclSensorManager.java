// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK android.hardware.SensorManager (the abstract Manager-boundary API,
// ADR-2 shim rule) + Sensor/SensorEvent field shapes. Backed natively by acl::svc::sensor
// (Linux IIO / synthetic). No AOSP SystemSensorManager code, no the predecessor.
package org.acl.internal;

import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.hardware.TriggerEventListener;
import android.os.Handler;
import android.os.Looper;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

// acl's concrete SensorManager: real android.hardware.SensorManager (abstract) implemented over
// the native acl::svc::sensor hub. AOSP's SystemSensorManager talks to sensorservice via Binder,
// which acl has not — so this is the shim-at-Manager-boundary substitute (ADR-2). Enumeration
// and enable/disable are native; samples arrive via onSensorEvent (native poll thread) and are
// posted to each listener's Handler as SensorEvents.
public final class AclSensorManager extends SensorManager {
    private static volatile AclSensorManager INSTANCE;

    private final List<Sensor> sensors = new ArrayList<>();

    private static final class Reg {
        final SensorEventListener listener;
        final Sensor sensor;
        final Handler handler;
        Reg(SensorEventListener l, Sensor s, Handler h) { listener = l; sensor = s; handler = h; }
    }
    private final CopyOnWriteArrayList<Reg> regs = new CopyOnWriteArrayList<>();

    public AclSensorManager(android.content.Context ctx) {
        INSTANCE = this;
        final int n = nCount();
        for (int i = 0; i < n; i++) {
            Sensor s = makeSensor(i);
            if (s != null) sensors.add(s);
        }
    }

    // Build a real android.hardware.Sensor from the native descriptor (its ctor + fields are
    // package-private, so reflect). Only the fields apps read are set.
    private static Sensor makeSensor(int i) {
        try {
            java.lang.reflect.Constructor<Sensor> c = Sensor.class.getDeclaredConstructor();
            c.setAccessible(true);
            Sensor s = c.newInstance();
            setI(s, "mHandle", nHandle(i));
            setI(s, "mType", nType(i));
            setI(s, "mVersion", nVersion(i));
            setI(s, "mMinDelay", nMinDelayUs(i));
            setI(s, "mFlags", 0);              // REPORTING_MODE_CONTINUOUS, not wake-up
            setF(s, "mMaxRange", nMaxRange(i));
            setF(s, "mResolution", nResolution(i));
            setF(s, "mPower", nPower(i));
            setO(s, "mName", nName(i));
            setO(s, "mVendor", nVendor(i));
            return s;
        } catch (Throwable t) {
            android.util.Log.w("acl", "AclSensorManager.makeSensor failed: " + t);
            return null;
        }
    }

    private static void setI(Object o, String f, int v) throws Exception {
        java.lang.reflect.Field fl = Sensor.class.getDeclaredField(f); fl.setAccessible(true); fl.setInt(o, v);
    }
    private static void setF(Object o, String f, float v) throws Exception {
        java.lang.reflect.Field fl = Sensor.class.getDeclaredField(f); fl.setAccessible(true); fl.setFloat(o, v);
    }
    private static void setO(Object o, String f, Object v) throws Exception {
        java.lang.reflect.Field fl = Sensor.class.getDeclaredField(f); fl.setAccessible(true); fl.set(o, v);
    }

    // ── native seam (acl::svc::sensor, registered by register_sensor_natives) ────────────────
    private static native int nCount();
    private static native int nHandle(int i);
    private static native int nType(int i);
    private static native int nVersion(int i);
    private static native int nMinDelayUs(int i);
    private static native int nValueCount(int i);
    private static native float nMaxRange(int i);
    private static native float nResolution(int i);
    private static native float nPower(int i);
    private static native String nName(int i);
    private static native String nVendor(int i);
    private static native boolean nEnable(int handle, int periodUs);
    private static native void nDisable(int handle);

    // Called from the native hub poll thread with one sample; post it to each matching listener.
    static void onSensorEvent(int handle, float[] values, int accuracy, long timestampNs) {
        AclSensorManager self = INSTANCE;
        if (self == null) return;
        for (Reg r : self.regs) {
            if (r.sensor.getHandle() != handle) continue;
            final SensorEvent ev = new SensorEvent(r.sensor, accuracy, timestampNs, values.clone());
            final SensorEventListener l = r.listener;
            Handler h = r.handler != null ? r.handler : self.mainHandler();
            h.post(() -> l.onSensorChanged(ev));
        }
    }

    private Handler main;
    private synchronized Handler mainHandler() {
        if (main == null) main = new Handler(Looper.getMainLooper());
        return main;
    }

    // ── SensorManager abstract surface ───────────────────────────────────────────────────────
    @Override protected List<Sensor> getFullSensorList() { return sensors; }
    @Override protected List<Sensor> getFullDynamicSensorList() { return new ArrayList<>(); }

    @Override
    protected boolean registerListenerImpl(SensorEventListener listener, Sensor sensor, int delayUs,
                                           Handler handler, int maxReportLatencyUs, int reservedFlags) {
        if (listener == null || sensor == null) return false;
        regs.add(new Reg(listener, sensor, handler));
        listener.onAccuracyChanged(sensor, SensorManager.SENSOR_STATUS_ACCURACY_HIGH);
        return nEnable(sensor.getHandle(), delayUs);
    }

    @Override
    protected void unregisterListenerImpl(SensorEventListener listener, Sensor sensor) {
        for (Reg r : regs)
            if (r.listener == listener && (sensor == null || r.sensor == sensor)) regs.remove(r);
        // Disable a sensor once nothing listens to it any more.
        if (sensor != null) {
            boolean stillUsed = false;
            for (Reg r : regs) if (r.sensor.getHandle() == sensor.getHandle()) { stillUsed = true; break; }
            if (!stillUsed) nDisable(sensor.getHandle());
        }
    }

    @Override protected boolean flushImpl(SensorEventListener listener) { return false; }

    // The rest of the abstract surface is not backed by acl (no direct channels, dynamic sensors,
    // trigger sensors, data injection) — safe no-ops so the class is concrete.
    @Override protected android.hardware.SensorDirectChannel createDirectChannelImpl(
            android.os.MemoryFile memoryFile, android.hardware.HardwareBuffer hardwareBuffer) { return null; }
    @Override protected void destroyDirectChannelImpl(android.hardware.SensorDirectChannel channel) {}
    @Override protected int configureDirectChannelImpl(
            android.hardware.SensorDirectChannel channel, Sensor s, int rate) { return 0; }
    @Override protected void registerDynamicSensorCallbackImpl(
            DynamicSensorCallback callback, Handler handler) {}
    @Override protected void unregisterDynamicSensorCallbackImpl(DynamicSensorCallback callback) {}
    @Override protected boolean requestTriggerSensorImpl(TriggerEventListener listener, Sensor sensor) { return false; }
    @Override protected boolean cancelTriggerSensorImpl(TriggerEventListener listener, Sensor sensor, boolean disable) { return false; }
    @Override protected boolean initDataInjectionImpl(boolean enable) { return false; }
    @Override protected boolean injectSensorDataImpl(Sensor sensor, float[] values, int accuracy, long timestamp) { return false; }
    @Override protected boolean setOperationParameterImpl(android.hardware.SensorAdditionalInfo parameter) { return false; }
}
