// SPDX-License-Identifier: Apache-2.0
// Provenance: acl; Android SDK behavioral reference (android.app.Service lifecycle,
// Context.startService/bindService/stopService, Service.stopSelf/startForeground). No GPL-3.0
// sources.
package org.acl.internal;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executor;

/**
 * The app's own Services, run in its process. a4l has no ActivityManager (ADR-0003), so
 * Context.startService and bindService land here: a Service the app declares is made, started,
 * bound, unbound and destroyed on the main thread, as Android does, and a client gets the object
 * onBind returned. In one process that is the Service's own Binder, so a local Binder subclass, an
 * AIDL stub (queryLocalInterface) and a Messenger all work as they do in-process on Android.
 *
 * Other apps' services are not reachable: there is one app (see issue #155). One binder is kept
 * per Service, from the first onBind, where Android keeps one per distinct intent.
 */
final class AppServices {
    private AppServices() {}

    private static final String TAG = "acl";

    /** One client binding. */
    private static final class Connection {
        final Intent intent;
        final ServiceConnection conn;
        final Executor executor;  // null: the main thread
        final boolean autoCreate;
        boolean connected;

        Connection(Intent intent, ServiceConnection conn, Executor executor, boolean autoCreate) {
            this.intent = intent;
            this.conn = conn;
            this.executor = executor;
            this.autoCreate = autoCreate;
        }
    }

    /** One declared Service and its state. Fields are guarded by AppServices.class. */
    private static final class Record {
        final String name;
        final ComponentName component;
        final IBinder token = new Binder();
        Service service;
        boolean failed;
        boolean started;
        int lastStartId;
        boolean bound;      // onBind has run
        Intent bindIntent;  // the intent onBind was given
        IBinder binder;     // what onBind returned
        boolean unbound;    // onUnbind has run since the last bind
        boolean rebind;     // what onUnbind returned
        int foregroundId;   // the notification startForeground posted, 0 for none
        final Map<ServiceConnection, Connection> conns = new LinkedHashMap<>();

        Record(String name, String pkg) {
            this.name = name;
            this.component = new ComponentName(pkg, name);
        }
    }

    private static final Map<String, Record> sRecords = new HashMap<>();
    private static Handler sMain;
    private static boolean sToldImplicit;

    private static synchronized Handler main() {
        if (sMain == null) sMain = new Handler(Looper.getMainLooper());
        return sMain;
    }

    private static String appPackage(Context ctx) {
        return ctx != null ? ctx.getPackageName() : null;
    }

    /**
     * The class of the app's own declared Service that `intent` names, or null: an explicit
     * component in this package, or an action with this package set. An intent with neither
     * a component nor a package is refused, as Android 5 and later refuse it.
     */
    static String resolve(Context ctx, Intent intent) {
        final String pkg = appPackage(ctx);
        if (intent == null || pkg == null) return null;
        final Map<String, List<String>> declared = HostBootstrap.sServiceActions;
        final ComponentName cn = intent.getComponent();
        if (cn != null) {
            if (!pkg.equals(cn.getPackageName())) return null;
            return declared.containsKey(cn.getClassName()) ? cn.getClassName() : null;
        }
        final String action = intent.getAction();
        final String target = intent.getPackage();
        if (target == null) {
            synchronized (AppServices.class) {
                if (!sToldImplicit)
                    android.util.Log.w(TAG, "services: an intent with no component or package "
                            + "names no service: " + intent);
                sToldImplicit = true;
            }
            return null;
        }
        if (action == null || !pkg.equals(target)) return null;
        synchronized (declared) {  // a synchronized map is iterated under its own lock
            for (Map.Entry<String, List<String>> e : declared.entrySet()) {
                if (e.getValue().contains(action)) return e.getKey();
            }
        }
        return null;
    }

    private static Record record(Context ctx, String name) {
        Record r = sRecords.get(name);
        if (r == null) {
            r = new Record(name, appPackage(ctx));
            sRecords.put(name, r);
        }
        return r;
    }

    // ── Context entry points ─────────────────────────────────────────────────────────────────

    static ComponentName start(final Context ctx, final Intent intent) {
        final String name = resolve(ctx, intent);
        if (name == null) return null;
        final Record r;
        final int startId;
        synchronized (AppServices.class) {
            r = record(ctx, name);
            r.started = true;
            startId = ++r.lastStartId;
        }
        main().post(() -> {
            if (!ensureCreated(ctx, r)) return;
            try {
                r.service.onStartCommand(intent, 0, startId);
            } catch (Throwable t) {
                android.util.Log.e(TAG, "services: " + name + ".onStartCommand threw", t);
            }
        });
        return r.component;
    }

    static boolean stop(Context ctx, Intent intent) {
        final String name = resolve(ctx, intent);
        if (name == null) return false;
        final Record r;
        synchronized (AppServices.class) {
            r = sRecords.get(name);
            if (r == null || !r.started) return false;
            r.started = false;
        }
        main().post(() -> maybeDestroy(r));
        return true;
    }

    static boolean bind(final Context ctx, Intent intent, final ServiceConnection conn, int flags,
            Executor executor) {
        if (conn == null) throw new IllegalArgumentException("connection is null");
        final String name = resolve(ctx, intent);
        if (name == null) return false;
        final Record r;
        final Connection c = new Connection(intent, conn, executor,
                (flags & Context.BIND_AUTO_CREATE) != 0);
        synchronized (AppServices.class) {
            r = record(ctx, name);
            r.conns.put(conn, c);
        }
        main().post(() -> connect(ctx, r, c));
        return true;
    }

    static void unbind(ServiceConnection conn) {
        Record found = null;
        synchronized (AppServices.class) {
            for (Record r : sRecords.values()) {
                if (r.conns.remove(conn) != null) {
                    found = r;
                    break;
                }
            }
        }
        if (found == null) {
            android.util.Log.w(TAG, "services: unbindService for a connection that is not bound");
            return;
        }
        final Record r = found;
        main().post(() -> {
            boolean callUnbind;
            Intent last;
            synchronized (AppServices.class) {
                callUnbind = r.service != null && r.bound && !r.unbound && r.conns.isEmpty();
                last = r.bindIntent;
            }
            if (callUnbind) {
                boolean rebind = false;
                try {
                    rebind = r.service.onUnbind(last);
                } catch (Throwable t) {
                    android.util.Log.e(TAG, "services: " + r.name + ".onUnbind threw", t);
                }
                synchronized (AppServices.class) {
                    r.unbound = true;
                    r.rebind = rebind;
                }
            }
            maybeDestroy(r);
        });
    }

    // ── Lifecycle, on the main thread ────────────────────────────────────────────────────────

    private static boolean ensureCreated(Context ctx, Record r) {
        synchronized (AppServices.class) {
            if (r.service != null) return true;
            if (r.failed) return false;
        }
        Service s;
        try {
            s = (Service) ctx.getClassLoader().loadClass(r.name).getDeclaredConstructor()
                    .newInstance();
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object thread = at.getMethod("currentActivityThread").invoke(null);
            java.lang.reflect.Method attach = Service.class.getDeclaredMethod("attach",
                    Context.class, at, String.class, IBinder.class, android.app.Application.class,
                    Object.class);
            attach.setAccessible(true);
            attach.invoke(s, ctx, thread, r.name, r.token, HostBootstrap.sApplication,
                    activityManager(ctx));
            s.onCreate();
        } catch (Throwable t) {
            if (t instanceof java.lang.reflect.InvocationTargetException && t.getCause() != null)
                t = t.getCause();
            android.util.Log.e(TAG, "services: cannot create " + r.name, t);
            synchronized (AppServices.class) {
                r.failed = true;
            }
            return false;
        }
        android.util.Log.i(TAG, "services: " + r.name + " created");
        List<Connection> waiting = new ArrayList<>();
        synchronized (AppServices.class) {
            r.service = s;
            for (Connection c : r.conns.values()) {
                if (!c.connected) waiting.add(c);
            }
        }
        // Bindings made without BIND_AUTO_CREATE connect once the Service exists.
        for (Connection c : waiting) connect(ctx, r, c);
        return true;
    }

    private static void connect(Context ctx, Record r, Connection c) {
        boolean create;
        synchronized (AppServices.class) {
            if (r.conns.get(c.conn) != c || c.connected) return;  // unbound or replaced
            create = r.service == null && (c.autoCreate || r.started);
        }
        if (create && !ensureCreated(ctx, r)) return;
        IBinder binder;
        boolean callBind;
        boolean callRebind;
        synchronized (AppServices.class) {
            if (r.service == null || r.conns.get(c.conn) != c || c.connected) return;
            callBind = !r.bound;
            callRebind = r.bound && r.unbound && r.rebind;
            r.unbound = false;
        }
        try {
            if (callBind) {
                binder = r.service.onBind(c.intent);
                synchronized (AppServices.class) {
                    r.binder = binder;
                    r.bindIntent = c.intent;
                    r.bound = true;
                }
            } else if (callRebind) {
                r.service.onRebind(c.intent);
            }
        } catch (Throwable t) {
            android.util.Log.e(TAG, "services: " + r.name + ".onBind threw", t);
            return;
        }
        synchronized (AppServices.class) {
            c.connected = true;
            binder = r.binder;
        }
        final IBinder b = binder;
        final ComponentName cn = r.component;
        final Runnable deliver = () -> {
            if (b != null) c.conn.onServiceConnected(cn, b);
            else c.conn.onNullBinding(cn);
        };
        if (c.executor != null) c.executor.execute(deliver);
        else deliver.run();
    }

    private static void maybeDestroy(Record r) {
        Service s;
        int fg;
        synchronized (AppServices.class) {
            if (r.started || r.service == null) return;
            for (Connection c : r.conns.values()) {
                if (c.autoCreate) return;
            }
            s = r.service;
            fg = r.foregroundId;
            // Cleared at once: a second check queued behind this one (an unbind and a stop
            // together) finds nothing to destroy.
            r.service = null;
            if (r.conns.isEmpty()) {
                sRecords.remove(r.name);
            } else {
                // Bindings made without BIND_AUTO_CREATE stay, and connect again if the
                // Service is started again.
                r.bound = false;
                r.bindIntent = null;
                r.binder = null;
                r.unbound = false;
                r.rebind = false;
                r.foregroundId = 0;
                for (Connection c : r.conns.values()) c.connected = false;
            }
        }
        android.util.Log.i(TAG, "services: " + r.name + " destroyed");
        try {
            s.onDestroy();
        } catch (Throwable t) {
            android.util.Log.e(TAG, "services: " + r.name + ".onDestroy threw", t);
        }
        if (fg != 0) cancelNotification(s, fg);
    }

    // ── The Service's ActivityManager: stopSelf and startForeground ──────────────────────────

    private static Object activityManager(final Context ctx) throws Exception {
        final Class<?> iam = Class.forName("android.app.IActivityManager");
        return java.lang.reflect.Proxy.newProxyInstance(iam.getClassLoader(), new Class<?>[]{iam},
                (p, m, a) -> {
                    final String mn = m.getName();
                    if ("stopServiceToken".equals(mn) && a != null && a.length >= 3)
                        return stopByToken((IBinder) a[1], (Integer) a[2]);
                    if ("setServiceForeground".equals(mn) && a != null)
                        setForeground(ctx, (IBinder) a[1], a);
                    final Class<?> rt = m.getReturnType();
                    if (rt == boolean.class) return false;
                    if (rt == int.class) return 0;
                    if (rt == long.class) return 0L;
                    return null;
                });
    }

    private static Record byToken(IBinder token) {
        for (Record r : sRecords.values()) {
            if (r.token == token) return r;
        }
        return null;
    }

    /** Service.stopSelf(startId): stops unless a later start has come in; -1 stops anyway. */
    private static boolean stopByToken(IBinder token, int startId) {
        final Record r;
        synchronized (AppServices.class) {
            r = byToken(token);
            if (r == null || !r.started) return false;
            if (startId >= 0 && startId != r.lastStartId) return false;
            r.started = false;
        }
        main().post(() -> maybeDestroy(r));
        return true;
    }

    /**
     * setServiceForeground(component, token, id, notification, flags, type): shows the
     * notification, as Android does for a foreground Service, through a4l's notification bridge.
     * stopForeground(REMOVE) takes it down.
     */
    private static void setForeground(Context ctx, IBinder token, Object[] a) {
        int id = 0;
        Notification n = null;
        int flags = 0;
        if (a.length >= 5) {
            if (a[2] instanceof Integer) id = (Integer) a[2];
            if (a[3] instanceof Notification) n = (Notification) a[3];
            if (a[4] instanceof Integer) flags = (Integer) a[4];
        }
        int cancel = 0;
        synchronized (AppServices.class) {
            Record r = byToken(token);
            if (r == null) return;
            if (n != null && id != 0) {
                r.foregroundId = id;
            } else if ((flags & Service.STOP_FOREGROUND_REMOVE) != 0) {
                cancel = r.foregroundId;
                r.foregroundId = 0;
            }
        }
        NotificationManager nm = ctx.getSystemService(NotificationManager.class);
        if (nm == null) return;
        try {
            if (n != null && id != 0) nm.notify(id, n);
            else if (cancel != 0) nm.cancel(cancel);
        } catch (Throwable t) {
            android.util.Log.w(TAG, "services: foreground notification: " + t);
        }
    }

    private static void cancelNotification(Context ctx, int id) {
        try {
            NotificationManager nm = ctx.getSystemService(NotificationManager.class);
            if (nm != null) nm.cancel(id);
        } catch (Throwable ignored) {
        }
    }
}
