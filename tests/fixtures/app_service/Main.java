// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK public API (android.app.Service,
// Context.startService/bindService/stopService, Messenger). No GPL-3.0 sources.
//
// The app's own Services, run by a4l in the app's process. Main walks through the cases on the
// main thread, one after another, and logs a "SERVICE:" line for each; the test reads them. The
// last line is "SERVICE: PASS", after which the process exits 0. A case that fails logs
// "SERVICE: FAIL <why>" and exits 1; one that never finishes is caught by a 30 s limit (exit 3).
package com.acl.svc;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.RemoteException;
import android.util.Log;

public class Main extends Activity {
    /** The manifest's Application: records that attachBaseContext came before onCreate. */
    public static class App extends android.app.Application {
        static final StringBuilder calls = new StringBuilder();

        @Override protected void attachBaseContext(Context base) {
            super.attachBaseContext(base);
            calls.append('a');
        }

        @Override public void onCreate() {
            super.onCreate();
            calls.append('c');
        }
    }

    static final String TAG = "acl";
    static Main sMain;
    final Handler main = new Handler(Looper.getMainLooper());
    int step;

    // True when the file's group and others have no permission on it.
    private static boolean ownerOnly(java.io.File f) {
        try {
            return (android.system.Os.stat(f.getPath()).st_mode & 077) == 0;
        } catch (android.system.ErrnoException e) {
            return false;
        }
    }

    static void log(String s) {
        Log.i(TAG, "SERVICE: " + s);
    }

    void fail(String why) {
        log("FAIL " + why);
        System.exit(1);
    }

    void expect(boolean ok, String what) {
        if (!ok) fail(what);
    }

    /** The launch callbacks in the order they came: a (attachBaseContext), c, s, C (post create), r, R (post resume). */
    private final StringBuilder launch = new StringBuilder();

    @Override
    protected void onStart() {
        super.onStart();
        launch.append('s');
    }

    @Override
    protected void onPostCreate(Bundle b) {
        super.onPostCreate(b);
        launch.append('C');
    }

    @Override
    protected void onResume() {
        super.onResume();
        launch.append('r');
    }

    @Override
    protected void onPostResume() {
        super.onPostResume();
        launch.append('R');
        // Activity.performCreate/Start/Resume and ActivityThread give this order.
        log("launch order " + launch);
        expect("acsCrR".contentEquals(launch), "launch order " + launch);
        // Resources.getSystem() has the display's size, as the app's own Resources does.
        final android.util.DisplayMetrics sys =
                android.content.res.Resources.getSystem().getDisplayMetrics();
        final android.util.DisplayMetrics app = getResources().getDisplayMetrics();
        final boolean sized = sys.widthPixels > 0 && sys.widthPixels == app.widthPixels
                && sys.heightPixels == app.heightPixels;
        log("system resources sized=" + sized);
        expect(sized, "system resources " + sys.widthPixels + "x" + sys.heightPixels
                + ", app " + app.widthPixels + "x" + app.heightPixels);
        // Bitmap.setPixel: an opaque color and a translucent one read back as written, and
        // the pixels beside them are untouched.
        final android.graphics.Bitmap dots = android.graphics.Bitmap.createBitmap(
                4, 4, android.graphics.Bitmap.Config.ARGB_8888);
        dots.setPixel(1, 2, 0xFF336699);
        dots.setPixel(3, 0, 0x80FF0000);
        final boolean set = dots.getPixel(1, 2) == 0xFF336699
                && dots.getPixel(3, 0) == 0x80FF0000
                && dots.getPixel(2, 2) == 0 && dots.getPixel(0, 0) == 0;
        log("bitmap pixel set=" + set);
        expect(set, String.format("setPixel read back %08x %08x %08x", dots.getPixel(1, 2),
                dots.getPixel(3, 0), dots.getPixel(2, 2)));
        // Bitmap.copyPixelsFromBuffer and copyPixelsToBuffer: ARGB_8888 is R, G, B, A in the
        // buffer. A direct buffer, an array-backed one, and an IntBuffer whose position is not
        // zero; the position moves past what was copied.
        boolean copied = true;
        final byte[] rgba = new byte[4 * 4 * 4];
        rgba[4 * (2 * 4 + 1)] = 0x11;      // (1, 2): R
        rgba[4 * (2 * 4 + 1) + 1] = 0x22;  // G
        rgba[4 * (2 * 4 + 1) + 2] = 0x33;  // B
        rgba[4 * (2 * 4 + 1) + 3] = (byte) 0xff;
        for (int pass = 0; pass < 2; pass++) {
            final java.nio.ByteBuffer in = pass == 0
                    ? java.nio.ByteBuffer.allocateDirect(rgba.length)
                    : java.nio.ByteBuffer.allocate(rgba.length);
            in.put(rgba).rewind();
            final android.graphics.Bitmap bm = android.graphics.Bitmap.createBitmap(
                    4, 4, android.graphics.Bitmap.Config.ARGB_8888);
            bm.copyPixelsFromBuffer(in);
            copied &= bm.getPixel(1, 2) == 0xFF112233 && bm.getPixel(0, 0) == 0
                    && in.position() == rgba.length;
            final java.nio.ByteBuffer out = pass == 0
                    ? java.nio.ByteBuffer.allocate(rgba.length)
                    : java.nio.ByteBuffer.allocateDirect(rgba.length);
            bm.copyPixelsToBuffer(out);
            out.rewind();
            final byte[] back = new byte[rgba.length];
            out.get(back);
            copied &= java.util.Arrays.equals(rgba, back);
        }
        final java.nio.IntBuffer ints = java.nio.IntBuffer.allocate(3 + 16);
        ints.position(3);
        dots.copyPixelsToBuffer(ints);
        // (1, 2) of dots is 0xFF336699: bytes 33 66 99 ff, a little-endian int ff996633.
        copied &= ints.position() == 19 && ints.get(3 + 2 * 4 + 1) == 0xFF996633
                && ints.get(0) == 0 && ints.get(3) == 0;
        log("bitmap copied through buffers=" + copied);
        expect(copied, "copyPixelsFromBuffer or copyPixelsToBuffer");
        // Bitmap.extractAlpha: the alpha alone, as ALPHA_8. Through a blur it spreads past
        // the source, and the offset says by how much. Apps build shadows and glows this way.
        String alphaOf;
        try {
            final android.graphics.Bitmap spot = android.graphics.Bitmap.createBitmap(8, 8,
                    android.graphics.Bitmap.Config.ARGB_8888);
            spot.setPixel(3, 4, 0x80FF0000);
            final android.graphics.Bitmap mask = spot.extractAlpha();
            final android.graphics.Paint blur = new android.graphics.Paint();
            blur.setMaskFilter(new android.graphics.BlurMaskFilter(4f,
                    android.graphics.BlurMaskFilter.Blur.NORMAL));
            final int[] at = new int[2];
            final android.graphics.Bitmap glow = spot.extractAlpha(blur, at);
            alphaOf = mask.getConfig() + " " + mask.getWidth() + "x" + mask.getHeight()
                    + " alpha " + android.graphics.Color.alpha(mask.getPixel(3, 4)) + "/"
                    + android.graphics.Color.alpha(mask.getPixel(0, 0))
                    + " blurred larger " + (glow.getWidth() > 8 && glow.getHeight() > 8)
                    + " offset negative " + (at[0] < 0 && at[1] < 0)
                    + " spread " + (android.graphics.Color.alpha(
                            glow.getPixel(3 - at[0] + 1, 4 - at[1])) > 0);
        } catch (Throwable t) {
            Log.w(TAG, "Bitmap.extractAlpha", t);
            alphaOf = t.toString();
        }
        log("extract alpha " + alphaOf);
        expect(alphaOf.equals("ALPHA_8 8x8 alpha 128/0 blurred larger true offset negative true"
                + " spread true"), "Bitmap.extractAlpha: " + alphaOf);
        // A grayscale PNG asked for as ALPHA_8 decodes to one, its gray levels the alpha: a
        // tint mask. Without the preference it is an opaque gray ARGB_8888.
        final byte[] grayPng = {-119, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
                0, 0, 0, 2, 0, 0, 0, 1, 8, 0, 0, 0, 0, -47, 73, 32, 86, 0, 0, 0, 11, 73, 68, 65,
                84, 120, -100, 99, 112, 56, 0, 0, 1, 67, 1, 1, 5, 17, 119, 124, 0, 0, 0, 0, 73,
                69, 78, 68, -82, 66, 96, -126};
        String mask;
        try {
            final android.graphics.BitmapFactory.Options asAlpha =
                    new android.graphics.BitmapFactory.Options();
            asAlpha.inPreferredConfig = android.graphics.Bitmap.Config.ALPHA_8;
            final android.graphics.Bitmap m = android.graphics.BitmapFactory.decodeByteArray(
                    grayPng, 0, grayPng.length, asAlpha);
            final android.graphics.Bitmap plain = android.graphics.BitmapFactory.decodeByteArray(
                    grayPng, 0, grayPng.length);
            mask = m.getConfig() + " " + m.getWidth() + "x" + m.getHeight() + " alpha "
                    + android.graphics.Color.alpha(m.getPixel(0, 0)) + "/"
                    + android.graphics.Color.alpha(m.getPixel(1, 0)) + " plain "
                    + plain.getConfig() + " " + Integer.toHexString(plain.getPixel(0, 0));
        } catch (Throwable t) {
            Log.w(TAG, "decode as ALPHA_8", t);
            mask = t.toString();
        }
        log("gray png as mask " + mask);
        expect(mask.equals("ALPHA_8 2x1 alpha 64/192 plain ARGB_8888 ff404040"),
                "grayscale PNG decoded as ALPHA_8: " + mask);
        // A BlurMaskFilter on the paint: a square's edge spreads past where it was drawn, the
        // middle stays opaque, and a corner well away is untouched.
        final android.graphics.Bitmap soft = android.graphics.Bitmap.createBitmap(
                48, 48, android.graphics.Bitmap.Config.ARGB_8888);
        final android.graphics.Paint blur = new android.graphics.Paint();
        blur.setColor(0xFFFF0000);
        blur.setMaskFilter(new android.graphics.BlurMaskFilter(
                4, android.graphics.BlurMaskFilter.Blur.NORMAL));
        new android.graphics.Canvas(soft).drawRect(16, 16, 32, 32, blur);
        final int edge = soft.getPixel(14, 24) >>> 24;
        final boolean blurred = edge > 0 && edge < 255 && (soft.getPixel(24, 24) >>> 24) > 200
                && soft.getPixel(1, 1) == 0;
        log("mask filter blurred=" + blurred);
        expect(blurred, String.format("blur edge alpha %d middle %08x corner %08x", edge,
                soft.getPixel(24, 24), soft.getPixel(1, 1)));
        // The app's own accounts: added with a password and user data, listed by type,
        // changed, and removed. The file they are kept in is the owner's alone.
        final android.accounts.AccountManager am = android.accounts.AccountManager.get(this);
        expect(am != null, "AccountManager.get() is null");
        final android.accounts.Account acct =
                new android.accounts.Account("user@example", "com.acl.svc.account");
        for (android.accounts.Account old : am.getAccountsByType(acct.type)) {
            am.removeAccountExplicitly(old);  // from a run that did not finish
        }
        final Bundle data = new Bundle();
        data.putString("region", "us");
        final boolean added = am.addAccountExplicitly(acct, "secret", data);
        final boolean twice = am.addAccountExplicitly(acct, "other", null);
        final android.accounts.Account[] mine = am.getAccountsByType(acct.type);
        final android.accounts.Account[] others = am.getAccountsByType("com.acl.svc.none");
        am.setUserData(acct, "tier", "premium");
        am.setAuthToken(acct, "api", "token-1");
        final java.io.File kept = new java.io.File(getDataDir(), "a4l_accounts.json");
        final boolean stored = added && !twice && mine.length == 1 && acct.equals(mine[0])
                && others.length == 0 && am.getAccounts().length == 1
                && "secret".equals(am.getPassword(acct))
                && "us".equals(am.getUserData(acct, "region"))
                && "premium".equals(am.getUserData(acct, "tier"))
                && "token-1".equals(am.peekAuthToken(acct, "api"))
                && kept.isFile() && ownerOnly(kept);
        am.invalidateAuthToken(acct.type, "token-1");
        final boolean dropped = am.peekAuthToken(acct, "api") == null;
        final boolean removed = am.removeAccountExplicitly(acct)
                && am.getAccountsByType(acct.type).length == 0
                && !am.removeAccountExplicitly(acct);
        log("accounts stored=" + stored + " token dropped=" + dropped + " removed=" + removed);
        expect(stored && dropped && removed, "accounts");
        // The app's own PackageInfo carries its uid, which is the one a caller of its own
        // services has: a media browser service checks a client this way.
        int infoUid = -1;
        String[] forUid = null;
        try {
            infoUid = getPackageManager().getPackageInfo(getPackageName(),
                    android.content.pm.PackageManager.GET_PERMISSIONS).applicationInfo.uid;
            forUid = getPackageManager().getPackagesForUid(android.os.Binder.getCallingUid());
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            // infoUid stays -1
        }
        final boolean uids = infoUid == android.os.Process.myUid()
                && infoUid == android.os.Binder.getCallingUid()
                && forUid != null && forUid.length == 1 && getPackageName().equals(forUid[0]);
        // A manifest <property> that is not declared is a NameNotFoundException, and
        // Environment.isExternalStorageLegacy(), which reads one, answers.
        boolean noProperty = false;
        try {
            getPackageManager().getProperty("com.acl.svc.absent", getPackageName());
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            noProperty = true;
        }
        final boolean legacy = android.os.Environment.isExternalStorageLegacy();
        log("absent property not found=" + noProperty + " legacy storage=" + legacy);
        expect(noProperty, "getProperty for an absent property");
        log("package uid consistent=" + uids);
        expect(uids, "uid: info " + infoUid + " process " + android.os.Process.myUid()
                + " caller " + android.os.Binder.getCallingUid());
        // ImageDecoder from a file: a PNG written here is decoded at its size with its
        // pixel, and a file that is no image is an IOException, not a crash.
        boolean decoded = false, refused = false;
        try {
            final java.io.File png = new java.io.File(getCacheDir(), "decode.png");
            final android.graphics.Bitmap src = android.graphics.Bitmap.createBitmap(
                    12, 7, android.graphics.Bitmap.Config.ARGB_8888);
            src.eraseColor(0xFF336699);
            try (java.io.FileOutputStream os = new java.io.FileOutputStream(png)) {
                src.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, os);
            }
            final android.graphics.Bitmap out = android.graphics.ImageDecoder.decodeBitmap(
                    android.graphics.ImageDecoder.createSource(png))
                    .copy(android.graphics.Bitmap.Config.ARGB_8888, false);
            decoded = out.getWidth() == 12 && out.getHeight() == 7
                    && out.getPixel(5, 3) == 0xFF336699;
            final java.io.File junk = new java.io.File(getCacheDir(), "decode.junk");
            try (java.io.FileOutputStream os = new java.io.FileOutputStream(junk)) {
                os.write(new byte[]{1, 2, 3, 4, 5, 6, 7, 8});
            }
            try {
                android.graphics.ImageDecoder.decodeBitmap(
                        android.graphics.ImageDecoder.createSource(junk));
            } catch (java.io.IOException e) {
                refused = true;
            }
        } catch (java.io.IOException e) {
            Log.w(TAG, "image decode: " + e);
        }
        log("image file decoded=" + decoded + " junk refused=" + refused);
        expect(decoded && refused, "ImageDecoder from a file");
        // A base-context chain ends: from the Activity and from the Application, taking the
        // base while it is a ContextWrapper reaches a Context that is not one, is not null,
        // and still names the Application. An image loader walks the chain this way.
        boolean ends = true;
        for (android.content.Context start : new android.content.Context[]{
                this, getApplicationContext()}) {
            android.content.Context c = start;
            int steps = 0;
            while (c instanceof android.content.ContextWrapper && steps < 20) {
                c = ((android.content.ContextWrapper) c).getBaseContext();
                steps++;
            }
            ends &= steps < 20 && c != null && c.getApplicationContext() == getApplication()
                    && getPackageName().equals(c.getPackageName());
        }
        log("base context chain ends=" + ends);
        expect(ends, "base context chain");
        // The directories and the display context an app asks any Context for.
        boolean dirs = true;
        for (android.content.Context c : new android.content.Context[]{
                this, getApplicationContext()}) {
            final java.io.File[] media = c.getExternalMediaDirs();
            dirs &= media != null && media.length == 1 && media[0].isDirectory()
                    && c.getObbDir().isDirectory() && c.getObbDirs().length == 1
                    && c.createDisplayContext(getWindowManager().getDefaultDisplay()) != null;
        }
        log("media and obb dirs, display context=" + dirs);
        expect(dirs, "getExternalMediaDirs, getObbDir or createDisplayContext");
        // What a call's network probe reads of the Wi-Fi link. The host reports one rate and
        // no standard, security type or beacon, and each getter says so instead of missing.
        final android.net.wifi.WifiInfo wifi = ((android.net.wifi.WifiManager)
                getApplicationContext().getSystemService(WIFI_SERVICE)).getConnectionInfo();
        final android.net.wifi.ScanResult scan = new android.net.wifi.ScanResult();
        String link;
        try {
            final int rate = wifi.getLinkSpeed() > 0 ? wifi.getLinkSpeed() : -1;
            link = (wifi.getTxLinkSpeedMbps() == rate)
                    + " " + wifi.getRxLinkSpeedMbps() + "/" + wifi.getMaxSupportedTxLinkSpeedMbps()
                    + "/" + wifi.getMaxSupportedRxLinkSpeedMbps() + " standard "
                    + wifi.getWifiStandard() + " security " + wifi.getCurrentSecurityType()
                    + " scan " + scan.channelWidth + "/" + scan.getInformationElements().size()
                    + "/" + scan.getWifiStandard();
        } catch (Throwable t) {
            link = t.toString();
        }
        log("wifi link " + link);
        expect("true -1/-1/-1 standard 0 security -1 scan 0/0/0".equals(link),
                "WifiInfo and ScanResult: " + link);
        // ColorMatrixColorFilter: red drawn through a matrix that moves red into blue and
        // adds 51 to green comes out (0, 51, 255). The 51 is the matrix's fifth column, which
        // Android gives on the 0..255 scale.
        final android.graphics.Bitmap tinted = android.graphics.Bitmap.createBitmap(
                4, 4, android.graphics.Bitmap.Config.ARGB_8888);
        final android.graphics.Paint matrixPaint = new android.graphics.Paint();
        matrixPaint.setColor(0xFFFF0000);
        matrixPaint.setColorFilter(new android.graphics.ColorMatrixColorFilter(new float[]{
                0, 0, 0, 0, 0,
                0, 0, 0, 0, 51,
                1, 0, 0, 0, 0,
                0, 0, 0, 1, 0}));
        new android.graphics.Canvas(tinted).drawRect(0, 0, 4, 4, matrixPaint);
        final int through = tinted.getPixel(2, 2);
        final boolean filtered = android.graphics.Color.red(through) == 0
                && Math.abs(android.graphics.Color.green(through) - 51) <= 1
                && android.graphics.Color.blue(through) == 255
                && android.graphics.Color.alpha(through) == 255;
        log("color matrix filtered=" + filtered);
        expect(filtered, String.format("ColorMatrixColorFilter gave %08x", through));
        // ActivityManager.getMemoryInfo is the host's memory, not zeros.
        final android.app.ActivityManager.MemoryInfo mem = new android.app.ActivityManager.MemoryInfo();
        ((android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE)).getMemoryInfo(mem);
        final boolean memSane = mem.availMem > (16L << 20) && mem.totalMem >= mem.availMem
                && mem.threshold > 0 && mem.threshold < mem.totalMem
                && mem.lowMemory == (mem.availMem < mem.threshold);
        log("memory info sane=" + memSane);
        expect(memSane, "MemoryInfo avail " + mem.availMem + " total " + mem.totalMem
                + " threshold " + mem.threshold + " low " + mem.lowMemory);
        // LinkProperties names the interface of the host's default route, with its addresses
        // and MTU, or says there is none. Checked against the interface itself.
        String linkProps;
        try {
            final android.net.LinkProperties lp = new android.net.LinkProperties();
            final String ifName = lp.getInterfaceName();
            final java.util.List<android.net.LinkAddress> addrs = lp.getLinkAddresses();
            if (ifName == null) {
                linkProps = "consistent=" + (addrs.isEmpty() && lp.getMtu() == 0);
            } else {
                final java.net.NetworkInterface nif = java.net.NetworkInterface.getByName(ifName);
                boolean ok = nif != null && !nif.isLoopback() && lp.getMtu() == nif.getMTU()
                        && addrs.size() == nif.getInterfaceAddresses().size();
                for (android.net.LinkAddress a : addrs) {
                    ok &= a.getAddress() != null && a.getPrefixLength() > 0
                            && a.getPrefixLength() <= 128;
                }
                linkProps = "consistent=" + ok;
                Log.i(TAG, "link " + ifName + " " + addrs + " mtu " + lp.getMtu());
            }
        } catch (Throwable t) {
            Log.w(TAG, "LinkProperties", t);
            linkProps = t.toString();
        }
        log("link properties " + linkProps);
        expect("consistent=true".equals(linkProps), "LinkProperties: " + linkProps);
        // The process has the package's name, from both places an app reads it.
        final String proc = android.os.Process.myProcessName();
        // The Application's attachBaseContext ran, before its onCreate, and the context
        // names the APK the code is in.
        final String code = getPackageCodePath();
        final boolean attached = "ac".contentEquals(App.calls) && code != null
                && new java.io.File(code).isFile() && code.equals(getPackageResourcePath())
                && code.equals(getApplicationInfo().sourceDir);
        log("application attached=" + attached);
        expect(attached, "Application calls " + App.calls + ", code path " + code);
        log("process name " + proc + " application " + android.app.Application.getProcessName());
        expect(getPackageName().equals(proc), "Process.myProcessName() is " + proc);
        // Native libraries are extracted to nativeLibraryDir, and the flags say so: a loader
        // that reads the flag clear expects to load them out of the APK instead.
        final android.content.pm.ApplicationInfo self = getApplicationInfo();
        final boolean extracted = (self.flags
                & android.content.pm.ApplicationInfo.FLAG_EXTRACT_NATIVE_LIBS) != 0
                && self.nativeLibraryDir != null;
        log("native libs extracted=" + extracted);
        expect(extracted, "ApplicationInfo.flags is 0x" + Integer.toHexString(self.flags));
        // There is no Android input method, so the lists of them are empty. They are never
        // null: an app that lists keyboard languages iterates them unguarded.
        final android.view.inputmethod.InputMethodManager imm =
                getSystemService(android.view.inputmethod.InputMethodManager.class);
        String imes;
        try {
            imes = imm.getInputMethodList().size() + "/" + imm.getEnabledInputMethodList().size()
                    + "/" + imm.getEnabledInputMethodSubtypeList(null, true).size();
        } catch (RuntimeException e) {
            imes = e.toString();
        }
        log("input methods " + imes);
        expect("0/0/0".equals(imes), "input method lists are " + imes);
        // Process.readProcFile and parseProcLine split a line by a format, as the framework
        // and apps read /proc/self/stat: a number, a name in parentheses that has a space in
        // it, a state letter, and a number after a run of spaces.
        boolean parsed = false;
        try {
            final int space = 0x20, combine = 0x100, parens = 0x200, chr = 0x800;
            final int outString = 0x1000, outLong = 0x2000;
            final int[] format = {space | outLong, space | parens | outString,
                    space | combine | outLong | chr, space | outLong, '\n' | outString};
            final byte[] line = "42 (a b) R   7 tail\n".getBytes("US-ASCII");
            final java.io.File f = new java.io.File(getCacheDir(), "stat");
            try (java.io.FileOutputStream os = new java.io.FileOutputStream(f)) {
                os.write(line);
            }
            parsed = true;
            for (int pass = 0; pass < 2; pass++) {
                final String[] strings = new String[5];
                final long[] longs = new long[5];
                final boolean ok = pass == 0
                        ? (Boolean) android.os.Process.class.getMethod("readProcFile",
                                String.class, int[].class, String[].class, long[].class,
                                float[].class).invoke(null, f.getPath(), format, strings, longs,
                                null)
                        : (Boolean) android.os.Process.class.getMethod("parseProcLine",
                                byte[].class, int.class, int.class, int[].class, String[].class,
                                long[].class, float[].class).invoke(null, line, 0, line.length,
                                format, strings, longs, null);
                parsed &= ok && longs[0] == 42 && "a b".equals(strings[1]) && longs[2] == 'R'
                        && longs[3] == 7 && "tail".equals(strings[4]);
            }
            // A format longer than the data, and a file that is not there.
            parsed &= !(Boolean) android.os.Process.class.getMethod("parseProcLine", byte[].class,
                    int.class, int.class, int[].class, String[].class, long[].class,
                    float[].class).invoke(null, line, 0, 2, format, new String[5], new long[5],
                    null);
            parsed &= !(Boolean) android.os.Process.class.getMethod("readProcFile", String.class,
                    int[].class, String[].class, long[].class, float[].class).invoke(null,
                    f.getPath() + ".none", format, null, null, null);
        } catch (Exception e) {
            parsed = false;
            Log.w(TAG, "readProcFile", e);
        }
        log("proc file parsed=" + parsed);
        expect(parsed, "Process.readProcFile or parseProcLine");
        // A PendingIntent says who made it and what it is. Apps check the creator of one
        // before trusting it, from any thread.
        String creator;
        try {
            final android.app.PendingIntent sent = android.app.PendingIntent.getBroadcast(this,
                    41, new Intent("com.acl.svc.INFO").setPackage(getPackageName()),
                    android.app.PendingIntent.FLAG_IMMUTABLE);
            final android.app.PendingIntent shown = android.app.PendingIntent.getActivity(this,
                    42, new Intent(this, Main.class), android.app.PendingIntent.FLAG_MUTABLE);
            creator = sent.getCreatorPackage() + " uid "
                    + (sent.getCreatorUid() == android.os.Process.myUid())
                    + " broadcast " + sent.isBroadcast() + "/" + sent.isActivity()
                    + " immutable " + sent.isImmutable()
                    + " activity " + shown.isActivity() + "/" + shown.isService()
                    + " immutable " + shown.isImmutable();
            sent.cancel();
            shown.cancel();
        } catch (Throwable t) {
            Log.w(TAG, "PendingIntent info", t);
            creator = t.toString();
        }
        log("pending intent " + creator);
        expect(creator.equals(getPackageName() + " uid true broadcast true/false immutable true"
                + " activity true/false immutable false"), "PendingIntent info: " + creator);
        // The native heap's sizes, which an app samples for its memory reports: what is in
        // use and what is free are both within the whole, and an allocation shows.
        final long heap = android.os.Debug.getNativeHeapSize();
        final long used = android.os.Debug.getNativeHeapAllocatedSize();
        final long free = android.os.Debug.getNativeHeapFreeSize();
        final android.graphics.Bitmap held = android.graphics.Bitmap.createBitmap(1024, 1024,
                android.graphics.Bitmap.Config.ARGB_8888);
        final long usedAfter = android.os.Debug.getNativeHeapAllocatedSize();
        final boolean heapOk = heap > 0 && used > 0 && used <= heap && free >= 0 && free <= heap
                && usedAfter - used >= held.getByteCount();
        log("native heap sane=" + heapOk);
        expect(heapOk, "native heap " + heap + " used " + used + " free " + free + " after "
                + usedAfter);
        // requestNetwork: a request that names no transport is met by the host's network when
        // there is one. One for a transport the host does not have (Bluetooth) is not, and with
        // a timeout its callback hears so.
        final android.net.ConnectivityManager cm = (android.net.ConnectivityManager)
                getSystemService(CONNECTIVITY_SERVICE);
        final boolean online = cm.getActiveNetwork() != null;
        final boolean[] got = new boolean[2];
        final java.util.concurrent.CountDownLatch gone = new java.util.concurrent.CountDownLatch(1);
        final android.os.HandlerThread netThread = new android.os.HandlerThread("net-request");
        netThread.start();
        final android.net.ConnectivityManager.NetworkCallback any =
                new android.net.ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(android.net.Network n) { got[0] = true; }
        };
        final android.net.ConnectivityManager.NetworkCallback bluetooth =
                new android.net.ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(android.net.Network n) { got[1] = true; }
            @Override public void onUnavailable() { gone.countDown(); }
        };
        boolean timedOut = false;
        try {
            cm.requestNetwork(new android.net.NetworkRequest.Builder().build(), any);
            cm.requestNetwork(new android.net.NetworkRequest.Builder()
                    .addTransportType(android.net.NetworkCapabilities.TRANSPORT_BLUETOOTH)
                    .build(), bluetooth, new Handler(netThread.getLooper()), 20);
            timedOut = gone.await(5, java.util.concurrent.TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        final boolean met = got[0] == online;
        cm.unregisterNetworkCallback(any);
        got[0] = false;
        cm.registerBestMatchingNetworkCallback(new android.net.NetworkRequest.Builder().build(),
                any, main);
        final boolean best = got[0] == online;
        cm.unregisterNetworkCallback(any);
        cm.reportNetworkConnectivity(cm.getActiveNetwork(), true);
        netThread.quitSafely();
        final boolean requested = met && best && !got[1] && timedOut;
        log("network requested=" + requested);
        expect(requested, "requestNetwork: online " + online + " met " + met + " best " + best
                + " bluetooth " + got[1] + " timed out " + timedOut);
        // A SweepGradient from red to blue around the center: red just past the positive x
        // axis, an even mix opposite it, blue just before the axis again.
        final android.graphics.Bitmap bmp = android.graphics.Bitmap.createBitmap(
                64, 64, android.graphics.Bitmap.Config.ARGB_8888);
        final android.graphics.Paint paint = new android.graphics.Paint();
        paint.setShader(new android.graphics.SweepGradient(32f, 32f, 0xFFFF0000, 0xFF0000FF));
        new android.graphics.Canvas(bmp).drawRect(0, 0, 64, 64, paint);
        final int start = bmp.getPixel(60, 33), half = bmp.getPixel(4, 32),
                end = bmp.getPixel(60, 30);
        final boolean swept = android.graphics.Color.red(start) > 240
                && android.graphics.Color.blue(start) < 16
                && Math.abs(android.graphics.Color.red(half) - 128) < 24
                && Math.abs(android.graphics.Color.blue(half) - 128) < 24
                && android.graphics.Color.blue(end) > 240
                && android.graphics.Color.red(end) < 16;
        log("sweep gradient drawn=" + swept);
        expect(swept, String.format("sweep gradient %08x %08x %08x", start, half, end));
        // android.graphics.Camera, the 3D transform a legacy Animation rotates a View with.
        // A turn of 60 degrees about Y halves the width (cos 60) and adds perspective in x;
        // restore() undoes it; the canvas takes the same matrix.
        final android.graphics.Camera camera = new android.graphics.Camera();
        final float cameraZ = camera.getLocationZ();
        camera.save();
        camera.rotateY(60f);
        final float[] turned = new float[9];
        final android.graphics.Matrix cameraMatrix = new android.graphics.Matrix();
        camera.getMatrix(cameraMatrix);
        cameraMatrix.getValues(turned);
        final android.graphics.Canvas cameraCanvas = new android.graphics.Canvas(
                android.graphics.Bitmap.createBitmap(4, 4, android.graphics.Bitmap.Config.ARGB_8888));
        camera.applyToCanvas(cameraCanvas);
        final float[] onCanvas = new float[9];
        cameraCanvas.getMatrix().getValues(onCanvas);
        final float facing = camera.dotWithNormal(0f, 0f, 1f);
        camera.restore();
        camera.getMatrix(cameraMatrix);
        final String cameraSeen = String.format(java.util.Locale.ROOT,
                "z %.1f turned %.3f/%.3f/%.5f canvas %b facing %.3f restored %b",
                cameraZ, turned[0], turned[4], turned[6],
                java.util.Arrays.equals(turned, onCanvas), facing, cameraMatrix.isIdentity());
        log("camera " + cameraSeen);
        // A ComposeShader of a red shader (the destination) and a green one (the source):
        // SRC is the green, DST the red, and MULTIPLY their product, black.
        final android.graphics.Shader redShader = new android.graphics.LinearGradient(
                0, 0, 8, 0, 0xFFFF0000, 0xFFFF0000, android.graphics.Shader.TileMode.CLAMP);
        final android.graphics.Shader greenShader = new android.graphics.LinearGradient(
                0, 0, 8, 0, 0xFF00FF00, 0xFF00FF00, android.graphics.Shader.TileMode.CLAMP);
        final int[] composed = new int[3];
        final android.graphics.PorterDuff.Mode[] modes = {
            android.graphics.PorterDuff.Mode.SRC, android.graphics.PorterDuff.Mode.DST,
            android.graphics.PorterDuff.Mode.MULTIPLY};
        for (int i = 0; i < modes.length; i++) {
            final android.graphics.Bitmap out = android.graphics.Bitmap.createBitmap(
                    8, 8, android.graphics.Bitmap.Config.ARGB_8888);
            final android.graphics.Paint both = new android.graphics.Paint();
            both.setShader(new android.graphics.ComposeShader(redShader, greenShader, modes[i]));
            new android.graphics.Canvas(out).drawRect(0, 0, 8, 8, both);
            composed[i] = out.getPixel(4, 4);
        }
        final boolean compose = composed[0] == 0xFF00FF00 && composed[1] == 0xFFFF0000
                && composed[2] == 0xFF000000;
        log("compose shader blended=" + compose);
        // Animators take the time they are given: the duration scale is 1, not the 0 that ends
        // each one on its first frame. ViewConfiguration asks the window manager about the
        // menu key, which is the first use of it here, and the scale is set from it then.
        android.view.ViewConfiguration.get(this).hasPermanentMenuKey();
        log("animators enabled=" + android.animation.ValueAnimator.areAnimatorsEnabled()
                + " scale=" + android.animation.ValueAnimator.getDurationScale());
        expect(compose, String.format("ComposeShader %08x %08x %08x", composed[0], composed[1],
                composed[2]));
        // drawPoint, drawPoints and drawLines, with a 3-pixel square stroke: each mark is
        // where it was put, and a pixel between marks is untouched. drawPoints takes its
        // offset and count in floats; the first pair here is skipped.
        final android.graphics.Bitmap marks = android.graphics.Bitmap.createBitmap(
                40, 20, android.graphics.Bitmap.Config.ARGB_8888);
        final android.graphics.Canvas markCanvas = new android.graphics.Canvas(marks);
        final android.graphics.Paint mark = new android.graphics.Paint();
        mark.setColor(0xFF00FF00);
        mark.setStrokeWidth(3);
        markCanvas.drawPoint(4, 4, mark);
        markCanvas.drawPoints(new float[]{30, 4, 12, 4, 20, 4}, 2, 4, mark);
        markCanvas.drawLines(new float[]{4, 14, 16, 14, 24, 14, 36, 14}, mark);
        final boolean marked = marks.getPixel(4, 4) == 0xFF00FF00
                && marks.getPixel(12, 4) == 0xFF00FF00 && marks.getPixel(20, 4) == 0xFF00FF00
                && marks.getPixel(30, 4) == 0 && marks.getPixel(8, 4) == 0
                && marks.getPixel(10, 14) == 0xFF00FF00 && marks.getPixel(30, 14) == 0xFF00FF00
                && marks.getPixel(20, 14) == 0;
        log("points and lines drawn=" + marked);
        expect(marked, "drawPoint, drawPoints, drawLines");
        // drawPaint fills the clip and no more; drawColor with CLEAR erases; a layer saved
        // with a half-transparent paint comes back at half alpha; skew moves a row sideways
        // by its y; a double round rect is a ring; isOpaque follows the bitmap's config.
        final android.graphics.Bitmap sheet = android.graphics.Bitmap.createBitmap(
                40, 40, android.graphics.Bitmap.Config.ARGB_8888);
        final android.graphics.Canvas sc = new android.graphics.Canvas(sheet);
        final android.graphics.Paint blue = new android.graphics.Paint();
        blue.setColor(0xFF0000FF);
        sc.save();
        sc.clipRect(0, 0, 20, 40);
        sc.drawPaint(blue);
        sc.restore();
        final boolean painted = sheet.getPixel(10, 20) == 0xFF0000FF
                && sheet.getPixel(30, 20) == 0;
        sc.drawColor(0, android.graphics.PorterDuff.Mode.CLEAR);
        final boolean cleared = sheet.getPixel(10, 20) == 0;
        final android.graphics.Paint halfPaint = new android.graphics.Paint();
        halfPaint.setAlpha(128);
        final int layer = sc.saveLayer(0, 0, 40, 40, halfPaint);
        sc.drawRect(0, 0, 40, 40, blue);
        sc.restoreToCount(layer);
        final int layered = sheet.getPixel(20, 20) >>> 24;
        sc.drawColor(0, android.graphics.PorterDuff.Mode.CLEAR);
        sc.save();
        sc.skew(1, 0);
        sc.drawRect(0, 20, 4, 24, blue);
        sc.restore();
        final boolean skewed = sheet.getPixel(23, 21) == 0xFF0000FF
                && sheet.getPixel(2, 21) == 0;
        sc.drawColor(0, android.graphics.PorterDuff.Mode.CLEAR);
        sc.drawDoubleRoundRect(new android.graphics.RectF(0, 0, 40, 40), 4, 4,
                new android.graphics.RectF(10, 10, 30, 30), 2, 2, blue);
        final boolean ring = sheet.getPixel(5, 20) == 0xFF0000FF && sheet.getPixel(20, 20) == 0;
        sc.drawColor(0, android.graphics.PorterDuff.Mode.CLEAR);
        final float[] corners = {4, 4, 4, 4, 4, 4, 4, 4};
        sc.drawDoubleRoundRect(new android.graphics.RectF(0, 0, 40, 40), corners,
                new android.graphics.RectF(10, 10, 30, 30), corners, blue);
        final boolean ringRadii = sheet.getPixel(20, 35) == 0xFF0000FF
                && sheet.getPixel(20, 20) == 0;
        final boolean opaque = !sc.isOpaque() && new android.graphics.Canvas(
                android.graphics.Bitmap.createBitmap(
                        2, 2, android.graphics.Bitmap.Config.RGB_565)).isOpaque();
        final boolean canvasOps = painted && cleared && layered > 120 && layered < 136
                && skewed && ring && ringRadii && opaque;
        log("canvas paint layer skew ring=" + canvasOps);
        expect(canvasOps, "drawPaint " + painted + " clear " + cleared + " layer alpha "
                + layered + " skew " + skewed + " ring " + ring + "/" + ringRadii
                + " opaque " + opaque);
    }

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        launch.append('a');
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        launch.append('c');
        sMain = this;
        main.postDelayed(new Runnable() {
            @Override public void run() {
                log("FAIL timed out at step " + step);
                System.exit(3);
            }
        }, 30000);
        main.post(new Runnable() {
            @Override public void run() { next(); }
        });
    }

    /** Runs the next case; each calls next() again when it is done. */
    void next() {
        switch (++step) {
            case 1: refused(); break;
            case 2: localBinder(); break;
            case 3: messenger(); break;
            case 4: startedTwice(); break;
            case 5: bindBeforeStart(); break;
            case 6: foreground(); break;
            case 7: systemServices(); break;
            case 8: components(); break;
            case 9: androidId(); break;
            case 10: job(); break;
            case 11: artwork(); break;
            case 12: keyGenerators(); break;
            case 13: permissions(); break;
            case 14: coveredDialog(); break;
            default:
                log("PASS");
                System.exit(0);
        }
    }

    void later(Runnable r) {
        main.postDelayed(r, 100);
    }

    // A Dialog left open by an Activity that starts another is under the new Activity: it is
    // not drawn while that one is on top, and is drawn again when it finishes. The dialog's
    // view counts its draws and asks for another each time, so it is drawn on every frame it
    // is shown in.
    static int sDialogDraws;
    static int sCoveredDraws = -1;

    static final class DrawCounter extends android.view.View {
        DrawCounter(Context c) { super(c); }
        @Override protected void onDraw(android.graphics.Canvas canvas) {
            sDialogDraws++;
            canvas.drawColor(0xff336699);
            invalidate();
        }
    }

    /** Covers Main for a while, counts the dialog's draws meanwhile, and finishes. */
    public static class Cover extends Activity {
        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            setContentView(new android.view.View(this));
            final Handler h = new Handler(Looper.getMainLooper());
            // After the frames that were under way when this was started.
            h.postDelayed(new Runnable() {
                @Override public void run() {
                    final int before = sDialogDraws;
                    h.postDelayed(new Runnable() {
                        @Override public void run() {
                            sCoveredDraws = sDialogDraws - before;
                            finish();
                            sMain.main.postDelayed(new Runnable() {
                                @Override public void run() { sMain.afterCover(); }
                            }, 300);
                        }
                    }, 400);
                }
            }, 300);
        }
    }

    private android.app.Dialog coverDialog;
    private int shownDraws;

    void coveredDialog() {
        coverDialog = new android.app.Dialog(this);
        coverDialog.setContentView(new DrawCounter(this),
                new android.view.ViewGroup.LayoutParams(200, 100));
        coverDialog.show();
        main.postDelayed(new Runnable() {
            @Override public void run() {
                shownDraws = sDialogDraws;
                startActivity(new Intent(Main.this, Cover.class));
            }
        }, 400);
    }

    void afterCover() {
        final int before = sDialogDraws;
        main.postDelayed(new Runnable() {
            @Override public void run() {
                log("dialog drawn=" + (shownDraws > 0) + " while covered=" + sCoveredDraws
                        + " back=" + (sDialogDraws > before));
                coverDialog.dismiss();
                next();
            }
        }, 400);
    }

    // A second requestPermissions is answered like the first. Activity refuses a request
    // while an earlier one is open, and answers it with no names.
    private String asked = "";

    void permissions() {
        requestPermissions(new String[] {"android.permission.INTERNET"}, 5);
    }

    @Override public void onRequestPermissionsResult(int code, String[] names, int[] grants) {
        asked += " " + code + "/" + names.length + "/" + (grants.length > 0 ? grants[0] : -9);
        if (code == 5) {
            requestPermissions(new String[] {"android.permission.INTERNET",
                                             "android.permission.VIBRATE"}, 6);
            return;
        }
        log("permissions asked twice" + asked);
        next();
    }

    static final ServiceConnection NONE = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName n, IBinder s) {}
        @Override public void onServiceDisconnected(ComponentName n) {}
    };

    // 1. What is not this app's declared service is not found.
    void refused() {
        Intent other = new Intent().setComponent(
                new ComponentName("com.example.other", "com.example.other.Svc"));
        boolean otherBound = bindService(other, NONE, BIND_AUTO_CREATE);
        Intent implicit = new Intent("com.acl.svc.MESSENGER");
        boolean implicitBound = bindService(implicit, NONE, BIND_AUTO_CREATE);
        ComponentName implicitStarted = startService(implicit);
        Intent undeclared = new Intent().setComponent(
                new ComponentName(getPackageName(), "com.acl.svc.NotDeclared"));
        boolean undeclaredBound = bindService(undeclared, NONE, BIND_AUTO_CREATE);
        log("refused other=" + otherBound + " implicit=" + implicitBound + "/" + implicitStarted
                + " undeclared=" + undeclaredBound);
        expect(!otherBound && !implicitBound && implicitStarted == null && !undeclaredBound,
                "a service that is not this app's was found");
        next();
    }

    // 2. A local Binder: the client calls the Service's own object; unbinding destroys it.
    final ServiceConnection localConn = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName n, IBinder s) {
            expect(Looper.myLooper() == Looper.getMainLooper(), "connected off the main thread");
            int sum = ((LocalService.LocalBinder) s).service().add(2, 3);
            log("local connected " + n.getShortClassName() + " add=" + sum);
            expect(sum == 5, "local add");
            unbindService(this);
            later(new Runnable() {
                @Override public void run() {
                    log("local after unbind created=" + LocalService.created + " unbound="
                            + LocalService.unbound + " destroyed=" + LocalService.destroyed);
                    expect(LocalService.created == 1 && LocalService.unbound == 1
                            && LocalService.destroyed == 1, "local lifecycle");
                    next();
                }
            });
        }
        @Override public void onServiceDisconnected(ComponentName n) {}
    };

    void localBinder() {
        boolean ok = bindService(new Intent(this, LocalService.class), localConn,
                BIND_AUTO_CREATE);
        expect(ok, "bind LocalService");
    }

    // 3. A Messenger (an AIDL stub underneath), bound by action and package.
    Messenger toService;
    final Messenger replies = new Messenger(new Handler(Looper.getMainLooper()) {
        @Override public void handleMessage(Message m) {
            log("messenger reply " + m.arg1);
            expect(m.arg1 == 21, "messenger reply");
            unbindService(messengerConn);
            next();
        }
    });
    final ServiceConnection messengerConn = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName n, IBinder s) {
            toService = new Messenger(s);
            Message m = Message.obtain(null, 1, 20, 0);
            m.replyTo = replies;
            try {
                toService.send(m);
            } catch (RemoteException e) {
                fail("messenger send " + e);
            }
        }
        @Override public void onServiceDisconnected(ComponentName n) {}
    };

    void messenger() {
        Intent i = new Intent("com.acl.svc.MESSENGER").setPackage(getPackageName());
        expect(bindService(i, messengerConn, BIND_AUTO_CREATE), "bind MessengerService");
    }

    // 4. Started twice: stopSelfResult(1) is refused once start 2 has come in; (2) stops it.
    void startedTwice() {
        Intent i = new Intent(this, StartedService.class);
        expect(startService(i) != null, "start StartedService");
        expect(startService(i) != null, "start StartedService again");
        later(new Runnable() {
            @Override public void run() {
                log("started ids=" + StartedService.ids + " stopped=" + StartedService.results
                        + " destroyed=" + StartedService.destroyed);
                expect("1,2".equals(StartedService.ids)
                        && "false,true".equals(StartedService.results)
                        && StartedService.destroyed == 1, "started lifecycle");
                next();
            }
        });
    }

    // 5. A binding without BIND_AUTO_CREATE connects once the Service is started.
    boolean lateConnected;
    final ServiceConnection lateConn = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName n, IBinder s) {
            lateConnected = true;
        }
        @Override public void onServiceDisconnected(ComponentName n) {}
    };

    void bindBeforeStart() {
        final Intent i = new Intent(this, LateService.class);
        expect(bindService(i, lateConn, 0), "bind LateService");
        later(new Runnable() {
            @Override public void run() {
                boolean before = lateConnected;
                startService(i);
                later(new Runnable() {
                    @Override public void run() {
                        log("late connected before=" + before + " after=" + lateConnected);
                        expect(!before && lateConnected, "late binding");
                        unbindService(lateConn);
                        boolean stopped = stopService(i);
                        later(new Runnable() {
                            @Override public void run() {
                                log("late stopped=" + stopped + " destroyed="
                                        + LateService.destroyed);
                                expect(stopped && LateService.destroyed == 1, "late stop");
                                next();
                            }
                        });
                    }
                });
            }
        });
    }

    // 6. A foreground Service: startForeground and stopForeground do not throw.
    void foreground() {
        expect(startForegroundService(new Intent(this, ForegroundService.class)) != null,
                "start ForegroundService");
        later(new Runnable() {
            @Override public void run() {
                log("foreground ran=" + ForegroundService.ran + " destroyed="
                        + ForegroundService.destroyed);
                expect(ForegroundService.ran && ForegroundService.destroyed == 1, "foreground");
                next();
            }
        });
    }

    // 7. System services that always exist on Android, and that a Service's setup reads: a null
    //    manager, or a null answer where Android gives a value, fails whatever asked for it.
    void systemServices() {
        android.app.usage.UsageStatsManager usage =
                getSystemService(android.app.usage.UsageStatsManager.class);
        android.content.pm.ShortcutManager shortcuts =
                getSystemService(android.content.pm.ShortcutManager.class);
        android.appwidget.AppWidgetManager widgets =
                android.appwidget.AppWidgetManager.getInstance(this);
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel("probe", "Probe",
                NotificationManager.IMPORTANCE_HIGH));
        NotificationChannel channel = nm.getNotificationChannel("probe");
        int listed = nm.getNotificationChannels().size();
        android.media.session.MediaSession session =
                new android.media.session.MediaSession(this, "probe");
        android.media.session.MediaController.PlaybackInfo playback =
                session.getController().getPlaybackInfo();
        android.media.MediaRouter router = getSystemService(android.media.MediaRouter.class);
        android.app.SearchManager search =
                (android.app.SearchManager) getSystemService(SEARCH_SERVICE);
        String line = "system usage=" + (usage != null ? usage.getAppStandbyBucket() : -1)
                + " shortcuts=" + (shortcuts != null ? shortcuts.getDynamicShortcuts().size() : -1)
                + " widgets=" + (widgets != null
                        ? widgets.getAppWidgetIds(new ComponentName(this, Main.class)).length : -1)
                + " channel=" + (channel != null ? channel.getImportance() : -1) + "/" + listed
                + " playback=" + (playback != null ? playback.getPlaybackType() : -1)
                + " router=" + (router != null && router.getRouteCount() > 0)
                + " search=" + (search != null
                        ? String.valueOf(search.getSearchableInfo(getComponentName())) : "none");
        session.release();
        log(line);
        // Bucket 10 is STANDBY_BUCKET_ACTIVE; channel "svc" from case 6 and "probe" are listed.
        expect(("system usage=10 shortcuts=0 widgets=0 channel=4/2 playback=1 router=true"
                        + " search=null").equals(line),
                "system services");
        next();
    }

    static String shortName(android.content.pm.ComponentInfo ci) {
        return ci == null ? "null" : ci.name.substring(ci.packageName.length());
    }

    // 8. PackageManager finds the app's own services and receivers, by class and by action, and
    //    nothing of another package. Libraries ask this before they use a component.
    void components() {
        android.content.pm.PackageManager pm = getPackageManager();
        int explicit = pm.queryIntentServices(new Intent(this, LocalService.class), 0).size();
        java.util.List<android.content.pm.ResolveInfo> byAction =
                pm.queryIntentServices(new Intent("com.acl.svc.MESSENGER"), 0);
        android.content.pm.ResolveInfo job =
                pm.resolveService(new Intent(this, JobLikeService.class), 0);
        Intent off = new Intent("com.acl.svc.OFF");
        int disabled = pm.queryIntentServices(off, 0).size();
        int disabledToo = pm.queryIntentServices(off,
                android.content.pm.PackageManager.MATCH_DISABLED_COMPONENTS).size();
        Intent other = new Intent().setComponent(
                new ComponentName("com.example.other", "com.example.other.Svc"));
        java.util.List<android.content.pm.ResolveInfo> ticks =
                pm.queryBroadcastReceivers(new Intent("com.acl.svc.TICK"), 0);
        String line = "components explicit=" + explicit
                + " action=" + (byAction.size() == 1 ? shortName(byAction.get(0).serviceInfo) : "?")
                + " permission=" + (job != null ? job.serviceInfo.permission : "?")
                + " disabled=" + disabled + "/" + disabledToo
                + " other=" + pm.queryIntentServices(other, 0).size() + "/"
                + pm.resolveService(other, 0)
                + " receiver=" + (ticks.size() == 1 ? shortName(ticks.get(0).activityInfo) : "?")
                + "/" + pm.queryBroadcastReceivers(new Intent("com.acl.svc.MESSENGER"), 0).size();
        log(line);
        expect(("components explicit=1 action=.Main$MessengerService"
                + " permission=android.permission.BIND_JOB_SERVICE disabled=0/1 other=0/null"
                + " receiver=.Main$TickReceiver/0").equals(line), "components");
        // A service the manifest does not have is not found; one it has is.
        boolean undeclared = false;
        boolean declared = false;
        try {
            getPackageManager().getServiceInfo(
                    new android.content.ComponentName(this, getPackageName() + ".NoSuchService"),
                    android.content.pm.PackageManager.GET_META_DATA);
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            undeclared = true;
        }
        try {
            declared = getPackageManager().getServiceInfo(
                    new android.content.ComponentName(this, LocalService.class), 0) != null;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            declared = false;
        }
        log("undeclared service not found=" + undeclared + " declared found=" + declared);
        expect(undeclared && declared, "getServiceInfo");
        // A web link has a handler, the host's; one aimed at a package that is not here does
        // not, and neither does a scheme nothing handles.
        final Intent link = new Intent(Intent.ACTION_VIEW,
                android.net.Uri.parse("https://example.com/a"));
        final boolean web = pm.resolveActivity(link, 0) != null
                && pm.queryIntentActivities(link, 0).size() == 1
                && link.resolveActivity(pm) != null
                && pm.resolveActivity(new Intent(link).setPackage("com.example.other"), 0) == null
                && pm.resolveActivity(new Intent(Intent.ACTION_VIEW,
                        android.net.Uri.parse("nosuch://example")), 0) == null;
        log("web link resolves=" + web);
        expect(web, "resolveActivity for a web link");
        next();
    }

    // 9. Settings.Secure.ANDROID_ID always has a value on Android: 16 hex digits, the same each
    //    time it is read.
    void androidId() {
        String id = android.provider.Settings.Secure.getString(getContentResolver(),
                android.provider.Settings.Secure.ANDROID_ID);
        String again = android.provider.Settings.Secure.getString(getContentResolver(),
                android.provider.Settings.Secure.ANDROID_ID);
        boolean hex = id != null && id.matches("[0-9a-f]{16}");
        log("settings android_id=" + (hex ? 16 : -1) + " stable=" + (id != null && id.equals(again)));
        expect(hex && id.equals(again), "android_id");
        next();
    }

    // 10. A job that needs a network: the JobInfo builds, reads its network request back, and
    //     the scheduler accepts it.
    void job() {
        android.net.NetworkRequest request = new android.net.NetworkRequest.Builder()
                .addCapability(android.net.NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .addTransportType(android.net.NetworkCapabilities.TRANSPORT_WIFI)
                .removeTransportType(android.net.NetworkCapabilities.TRANSPORT_WIFI)
                .build();
        android.app.job.JobInfo info = new android.app.job.JobInfo.Builder(1,
                new ComponentName(this, JobLikeService.class))
                .setRequiredNetwork(request)
                .build();
        android.net.NetworkRequest back = info.getRequiredNetwork();
        int scheduled = getSystemService(android.app.job.JobScheduler.class).schedule(info);
        String line = "job network="
                + back.hasCapability(android.net.NetworkCapabilities.NET_CAPABILITY_INTERNET)
                + "/" + back.hasTransport(android.net.NetworkCapabilities.TRANSPORT_WIFI)
                + " specifier=" + back.getNetworkSpecifier() + " scheduled=" + scheduled;
        log(line);
        // 1 is JobScheduler.RESULT_SUCCESS.
        expect("job network=true/false specifier=null scheduled=1".equals(line), "job");
        next();
    }

    /** The last pixel of `b`. */
    static int pixel(android.graphics.Bitmap b) {
        int[] px = new int[b.getWidth() * b.getHeight()];
        b.getPixels(px, 0, b.getWidth(), 0, 0, b.getWidth(), b.getHeight());
        return px[px.length - 1];
    }

    // 11. Artwork as a playing app hands it on: Bitmap.asShared(), a Bitmap through a Parcel,
    //     and a notification's RemoteViews, bitmap included, cloned through a Parcel.
    // 12. A key generator of the AndroidKeyStore provider makes a key, with that provider
    //     where an app has put it: last, and first. First is the order in which a generator
    //     that asks for the algorithm alone gets the keystore's own.
    void keyGenerators() {
        boolean made = true;
        final java.security.Provider ks = java.security.Security.getProvider("AndroidKeyStore");
        try {
            for (int pass = 0; pass < 2 && ks != null; pass++) {
                if (pass == 1) {
                    java.security.Security.removeProvider("AndroidKeyStore");
                    java.security.Security.insertProviderAt(ks, 1);
                }
                final javax.crypto.KeyGenerator kg =
                        javax.crypto.KeyGenerator.getInstance("AES", "AndroidKeyStore");
                kg.init(256);  // no alias: nothing is stored
                made &= kg.generateKey().getEncoded().length == 32;
                final java.security.KeyPairGenerator kpg =
                        java.security.KeyPairGenerator.getInstance("EC", "AndroidKeyStore");
                made &= kpg.generateKeyPair().getPrivate() != null;
            }
        } catch (Throwable t) {
            made = false;
            Log.w(TAG, "key generators", t);
        }
        log("keystore generators made keys=" + (made && ks != null));
        expect(made && ks != null, "AndroidKeyStore KeyGenerator or KeyPairGenerator");
        next();
    }

    void artwork() {
        android.graphics.Bitmap src = android.graphics.Bitmap.createBitmap(3, 2,
                android.graphics.Bitmap.Config.ARGB_8888);
        src.eraseColor(0xff336699);
        android.graphics.Bitmap shared = src.asShared();
        android.os.Parcel p = android.os.Parcel.obtain();
        src.writeToParcel(p, 0);
        p.writeInt(77);
        p.setDataPosition(0);
        android.graphics.Bitmap back = android.graphics.Bitmap.CREATOR.createFromParcel(p);
        int after = p.readInt();
        p.recycle();
        String views;
        try {
            android.widget.RemoteViews rv = new android.widget.RemoteViews(getPackageName(),
                    android.R.layout.simple_list_item_1);
            rv.setImageViewBitmap(android.R.id.icon, src);
            rv.setTextViewText(android.R.id.text1, "x");
            rv.setOnClickPendingIntent(android.R.id.text1, android.app.PendingIntent.getBroadcast(
                    this, 0, new Intent("com.acl.svc.TICK").setPackage(getPackageName()),
                    android.app.PendingIntent.FLAG_IMMUTABLE));
            android.os.Parcel q = android.os.Parcel.obtain();
            rv.writeToParcel(q, 0);
            q.setDataPosition(0);
            android.widget.RemoteViews copy = android.widget.RemoteViews.CREATOR.createFromParcel(q);
            q.recycle();
            views = String.valueOf(copy.getLayoutId() == android.R.layout.simple_list_item_1);
        } catch (Throwable t) {
            Log.w(TAG, "RemoteViews through a Parcel", t);
            views = t.toString();
        }
        String cloned;
        try {
            android.app.PendingIntent tick = android.app.PendingIntent.getBroadcast(this, 0,
                    new Intent("com.acl.svc.TICK").setPackage(getPackageName()),
                    android.app.PendingIntent.FLAG_IMMUTABLE);
            Notification n = new Notification.Builder(this, "probe")
                    .setSmallIcon(android.R.drawable.ic_media_play)
                    .setLargeIcon(src)
                    .setContentTitle("Title")
                    .setContentText("Text")
                    .setContentIntent(tick)
                    .addAction(new Notification.Action.Builder(
                            android.graphics.drawable.Icon.createWithResource(this,
                                    android.R.drawable.ic_media_pause), "Pause", tick).build())
                    .setStyle(new Notification.MediaStyle().setShowActionsInCompactView(0))
                    .build();
            cloned = String.valueOf(n.clone().getLargeIcon() != null);
        } catch (Throwable t) {
            Log.w(TAG, "Notification.clone", t);
            cloned = t.toString();
        }
        android.content.pm.ApplicationInfo ai = getApplicationInfo();
        String sdk = ai.minSdkVersion + "/" + ai.targetSdkVersion;
        String line = "artwork shared=" + shared.getWidth() + "x" + shared.getHeight() + "/"
                + Integer.toHexString(pixel(shared)) + "/" + shared.isMutable()
                + " parcel=" + back.getWidth() + "x" + back.getHeight() + "/"
                + Integer.toHexString(pixel(back))
                + " next=" + after + " views=" + views + " notification=" + cloned + " sdk=" + sdk;
        log(line);
        expect(("artwork shared=3x2/ff336699/false parcel=3x2/ff336699 next=77 views=true"
                + " notification=true sdk=24/33")
                .equals(line), "artwork");
        next();
    }

    public static class JobLikeService extends Service {
        @Override public IBinder onBind(Intent i) { return null; }
    }

    public static class TickReceiver extends android.content.BroadcastReceiver {
        @Override public void onReceive(Context c, Intent i) {}
    }

    public static class LocalService extends Service {
        static int created, unbound, destroyed;

        public class LocalBinder extends Binder {
            LocalService service() { return LocalService.this; }
        }

        final LocalBinder binder = new LocalBinder();

        int add(int a, int b) { return a + b; }

        @Override public void onCreate() { created++; }
        @Override public IBinder onBind(Intent i) { return binder; }
        @Override public boolean onUnbind(Intent i) {
            unbound++;
            return false;
        }
        @Override public void onDestroy() { destroyed++; }
    }

    public static class MessengerService extends Service {
        final Messenger messenger = new Messenger(new Handler(Looper.getMainLooper()) {
            @Override public void handleMessage(Message m) {
                try {
                    m.replyTo.send(Message.obtain(null, 2, m.arg1 + 1, 0));
                } catch (RemoteException e) {
                    log("FAIL messenger reply " + e);
                }
            }
        });

        @Override public IBinder onBind(Intent i) { return messenger.getBinder(); }
    }

    public static class StartedService extends Service {
        static String ids = "", results = "";
        static int destroyed;

        @Override public int onStartCommand(Intent i, int flags, int startId) {
            boolean stopped = stopSelfResult(startId);
            ids += (ids.isEmpty() ? "" : ",") + startId;
            results += (results.isEmpty() ? "" : ",") + stopped;
            return START_NOT_STICKY;
        }
        @Override public IBinder onBind(Intent i) { return null; }
        @Override public void onDestroy() { destroyed++; }
    }

    public static class LateService extends Service {
        static int destroyed;
        final Binder binder = new Binder();

        @Override public IBinder onBind(Intent i) { return binder; }
        @Override public void onDestroy() { destroyed++; }
    }

    public static class ForegroundService extends Service {
        static boolean ran;
        static int destroyed;

        @Override public int onStartCommand(Intent i, int flags, int startId) {
            NotificationManager nm = getSystemService(NotificationManager.class);
            if (nm != null)
                nm.createNotificationChannel(new NotificationChannel("svc", "Service",
                        NotificationManager.IMPORTANCE_LOW));
            Notification n = new Notification.Builder(this, "svc")
                    .setContentTitle("a4l service test")
                    .setSmallIcon(android.R.drawable.ic_media_play)
                    .build();
            startForeground(7, n);
            stopForeground(STOP_FOREGROUND_REMOVE);
            ran = true;
            stopSelf();
            return START_NOT_STICKY;
        }
        @Override public IBinder onBind(Intent i) { return null; }
        @Override public void onDestroy() { destroyed++; }
    }
}
