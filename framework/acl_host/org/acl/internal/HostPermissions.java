// SPDX-License-Identifier: Apache-2.0
// Provenance: acl host-integration seam; Android runtime permissions (AOSP android.jar stubs,
// Apache-2.0) mapped to the XDG permission store's "devices" table. No GPL-3.0 sources.
package org.acl.internal;

/**
 * Which permissions the app holds. Every permission is granted except the ones that name a
 * device the desktop keeps a decision for: those are granted when the desktop's permission
 * store ({@code org.freedesktop.impl.portal.PermissionStore}, table {@code devices}) says
 * {@code yes} for the app's package name, or when {@code ACL_ALLOW_DEVICES} names the device.
 * a4l has no dialog to ask the user with, so anything else is a denial.
 */
public final class HostPermissions {
    private HostPermissions() {}

    // Bound by svc/permission (register_permission_natives).
    private static native boolean nDeviceAllowed(String device, String app);

    /** The store's name for the device a permission guards, or null when it guards none. */
    static String device(String permission) {
        if ("android.permission.RECORD_AUDIO".equals(permission)) return "microphone";
        return null;
    }

    /** PackageManager.PERMISSION_GRANTED or PERMISSION_DENIED for the app {@code pkg}. */
    public static int check(String permission, String pkg) {
        String device = device(permission);
        if (device == null) return android.content.pm.PackageManager.PERMISSION_GRANTED;
        boolean allowed;
        try {
            allowed = pkg != null && nDeviceAllowed(device, pkg);
        } catch (Throwable t) {
            allowed = false;  // native not registered
        }
        return allowed ? android.content.pm.PackageManager.PERMISSION_GRANTED
                       : android.content.pm.PackageManager.PERMISSION_DENIED;
    }
}
