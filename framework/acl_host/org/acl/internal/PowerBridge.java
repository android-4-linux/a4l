// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room acl bridge exposing the host session/power state to the
// android.os.PowerManager surface (AOSP android.jar stubs, Apache-2.0), backed by the
// svc/power native seam (systemd-logind + power-profiles + kernel thermal zones).
// No GPL-3.0 sources.
package org.acl.internal;

/**
 * The Android-visible power state, read from the host session/power daemons via the
 * svc/power native seam: systemd-logind for session active/idle/lock, the power
 * profile daemon for battery-saver, and the kernel thermal zones for throttling.
 *
 * <p>Android wake locks are backed by real logind inhibitor fds — acquiring one
 * genuinely stops the host idling/suspending, releasing lets go — so an app holding a
 * PARTIAL_WAKE_LOCK during playback keeps the machine awake as it would on a device.
 *
 * <p>When the natives are absent (no boot classpath seam, or a build without them)
 * every read falls back to an awake, unthrottled desktop and wake locks are accepted
 * but hold nothing, which is the safe default: an app is never told the screen is off.
 */
public final class PowerBridge {
    private PowerBridge() {}

    // [interactive(0/1), powerSave(0/1), deviceIdle(0/1), thermalStatus]
    private static native int[] nSnapshot();
    private static native long nAcquireWakeLock(int flags, String tag);
    private static native void nReleaseWakeLock(long cookie);

    private static int[] snapshot() {
        try {
            int[] s = nSnapshot();
            if (s != null && s.length >= 4) return s;
        } catch (Throwable ignored) { /* natives absent */ }
        return new int[]{1, 0, 0, 0};  // awake, no saver, not idle, thermal none
    }

    /** PowerManager.isInteractive(): the session is active, unlocked and not idle. */
    public static boolean isInteractive() { return snapshot()[0] != 0; }

    /** PowerManager.isPowerSaveMode(): the host power profile is "power-saver". */
    public static boolean isPowerSaveMode() { return snapshot()[1] != 0; }

    /** PowerManager.isDeviceIdleMode(): the session carries logind's IdleHint. */
    public static boolean isDeviceIdleMode() { return snapshot()[2] != 0; }

    /** IThermalService.getCurrentThermalStatus(): PowerManager.THERMAL_STATUS_*. */
    public static int thermalStatus() { return snapshot()[3]; }

    /**
     * Take a host inhibitor for an Android wake lock. Returns a cookie for
     * {@link #releaseWakeLock}; 0 means nothing is held (acquire never fails, matching
     * Android, where a wake lock is always granted).
     */
    public static long acquireWakeLock(int flags, String tag) {
        try {
            return nAcquireWakeLock(flags, tag);
        } catch (Throwable ignored) {
            return 0L;
        }
    }

    /** Drop the inhibitor taken for {@code cookie}. */
    public static void releaseWakeLock(long cookie) {
        if (cookie == 0L) return;
        try {
            nReleaseWakeLock(cookie);
        } catch (Throwable ignored) { /* natives absent */ }
    }
}
