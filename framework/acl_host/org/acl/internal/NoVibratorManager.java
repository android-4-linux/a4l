// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l; android.os.VibratorManager's public and abstract surface (AOSP,
// Apache-2.0). No GPL-3.0 sources.
package org.acl.internal;

/**
 * android.os.VibratorManager for a host that has no vibrator. An app asks for the manager from
 * API 31 where it once asked for the vibrator, and takes the manager's default vibrator with
 * no check for null. There are no vibrators to list, and the default one is the vibrator a4l
 * already hands out: the framework's own NullVibrator, which says there is none and takes any
 * effect without doing anything.
 */
final class NoVibratorManager extends android.os.VibratorManager {
    private final android.os.Vibrator mNone;

    NoVibratorManager(android.os.Vibrator none) {
        mNone = none;
    }

    @Override public int[] getVibratorIds() { return new int[0]; }

    @Override public android.os.Vibrator getVibrator(int vibratorId) { return mNone; }

    @Override public android.os.Vibrator getDefaultVibrator() { return mNone; }

    @Override public void vibrate(int uid, String opPkg, android.os.CombinedVibration effect,
            String reason, android.os.VibrationAttributes attributes) { }

    @Override public void cancel() { }

    @Override public void cancel(int usageFilter) { }
}
