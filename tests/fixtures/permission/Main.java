// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; AOSP android.jar API stubs (Apache-2.0).
// No GPL-3.0 sources.
package com.acl.permission;

import android.app.Activity;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

import java.io.File;
import java.io.FileOutputStream;

/**
 * Reads the microphone permission every way an app does, before and after the desktop allows
 * it. The test's stand-in permission store allows the microphone once the file "grant" exists
 * in this app's files directory, as a user changing the setting would.
 */
public final class Main extends Activity {
    private static final String MIC = "android.permission.RECORD_AUDIO";
    private static final String CAMERA = "android.permission.CAMERA";
    private int round;

    private static void log(String s) { System.out.println("PERM: " + s); }

    private String state(String when) throws Exception {
        final PackageManager pm = getPackageManager();
        final PackageInfo pi = pm.getPackageInfo(getPackageName(), PackageManager.GET_PERMISSIONS);
        String flags = "";
        for (int i = 0; pi.requestedPermissions != null && i < pi.requestedPermissions.length; i++) {
            final boolean granted = (pi.requestedPermissionsFlags[i]
                    & PackageInfo.REQUESTED_PERMISSION_GRANTED) != 0;
            final String name = pi.requestedPermissions[i];
            flags += " " + name.substring(name.lastIndexOf('.') + 1) + "=" + granted;
        }
        return when + " self " + checkSelfPermission(MIC)
                + " uid " + checkPermission(MIC, android.os.Process.myPid(), android.os.Process.myUid())
                + " pm " + pm.checkPermission(MIC, getPackageName())
                + " camera " + checkSelfPermission(CAMERA) + " declared" + flags;
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        try {
            log(state("before"));
        } catch (Throwable t) {
            log("FAIL " + t);
            System.exit(1);
        }
        requestPermissions(new String[] {MIC, CAMERA}, 7);
    }

    @Override public void onRequestPermissionsResult(int code, String[] names, int[] grants) {
        log("request " + code + " " + names.length + " names mic " + grants[0] + " camera " + grants[1]);
        if (++round == 2) {
            log("PASS");
            System.exit(0);
        }
        try {
            new File(getFilesDir(), "grant").createNewFile();
        } catch (Throwable t) {
            log("FAIL " + t);
            System.exit(1);
        }
        // Past the second a4l keeps the store's last answer for.
        new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
            @Override public void run() {
                try {
                    log(state("after"));
                } catch (Throwable t) {
                    log("FAIL " + t);
                    System.exit(1);
                }
                requestPermissions(new String[] {MIC, CAMERA}, 8);
            }
        }, 1500);
    }
}
