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
    static final String TAG = "acl";
    static Main sMain;
    final Handler main = new Handler(Looper.getMainLooper());
    int step;

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

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
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
