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

    /** The launch callbacks in the order they came: c, s, C (post create), r, R (post resume). */
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
        expect("csCrR".contentEquals(launch), "launch order " + launch);
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
            default:
                log("PASS");
                System.exit(0);
        }
    }

    void later(Runnable r) {
        main.postDelayed(r, 100);
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
