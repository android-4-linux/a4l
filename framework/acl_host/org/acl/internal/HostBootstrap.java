// SPDX-License-Identifier: Apache-2.0
// Provenance: acl host-ART runtime bootstrap; Android SDK behavioral (ServiceManager/Binder
// degradation for a no-Binder single-process host, ADR-3; minimal Context for View
// construction). No GPL-3.0 sources.
//
// The framework environment an unmodified app expects the runtime/ActivityThread to have set
// up before its code runs, provided by acl for the host-ART app path (ADR-1). acl injects
// this class onto the classpath and calls init() at boot; the view host builds Views with
// newContext(). Keeps apps pure (they provide only a View), unlike the earlier fixture that
// did this setup itself.
//
// The ServiceManager/Binder seed needs java.lang.reflect.Proxy + an InvocationHandler, which
// is impractical to build from pure JNI — hence this Java helper.
package org.acl.internal;

import android.content.ContextWrapper;
import android.content.res.Resources;

public final class HostBootstrap {
    private HostBootstrap() {}

    private static boolean sInited = false;
    // The app's manifest theme (style resId), applied to every app Context's theme.
    private static int appThemeResId = 0;

    /** Set the app's manifest theme resId; applied when a Context resolves its theme. */
    public static void setAppTheme(int resId) { appThemeResId = resId; }

    /** Idempotent: prepare the main Looper and seed a no-Binder ServiceManager. */
    public static synchronized void init() {
        if (sInited) return;
        sInited = true;
        try {
            if (android.os.Looper.myLooper() == null) android.os.Looper.prepareMainLooper();
        } catch (Throwable ignored) {
        }
        try {
            seedNoBinderServiceManager();
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: ServiceManager seed failed: " + t);
        }
    }

    /** A minimal Context sufficient to construct a View (getResources/getMainLooper/…). */
    public static android.content.Context newContext() {
        return new ViewContext();
    }

    /**
     * An app-bound Context: the app's own package + resources (its resources.arsc via an
     * AssetManager over the APK, plus framework-res for android.R.*), so a real Activity's
     * onCreate/setContentView/resource lookups resolve. Managers are minimal stubs. This is
     * the minimal-ContextImpl stand-in (no ActivityThread/LoadedApk).
     */
    public static android.content.Context newAppContext(String apkPath, String frameworkResPath,
                                                        String packageName) {
        return new AppContext(appResources(apkPath, frameworkResPath), packageName);
    }

    private static Resources appResources(String apkPath, String frameworkResPath) {
        try {
            android.content.res.AssetManager am =
                    android.content.res.AssetManager.class.getDeclaredConstructor().newInstance();
            java.lang.reflect.Method add =
                    android.content.res.AssetManager.class.getMethod("addAssetPath", String.class);
            if (frameworkResPath != null) add.invoke(am, frameworkResPath);  // android.R.*
            if (apkPath != null) add.invoke(am, apkPath);                    // the app's arsc
            return new Resources(am, Resources.getSystem().getDisplayMetrics(),
                                 Resources.getSystem().getConfiguration());
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: app resources failed: " + t);
            return Resources.getSystem();
        }
    }

    static final class AppContext extends ContextWrapper {
        private final Resources res;
        private final String pkg;
        private Resources.Theme theme;
        private android.app.Application application;  // set once createApplication wraps this base
        AppContext(Resources res, String pkg) { super(null); this.res = res; this.pkg = pkg; }
        void setApplication(android.app.Application a) { this.application = a; }
        @Override public Resources getResources() { return res; }
        @Override public String getPackageName() { return pkg; }
        @Override public String getOpPackageName() { return pkg; }  // ContentResolver.<init> reads it
        @Override public int getUserId() { return 0; }              // single-user (system user)
        @Override public String getAttributionTag() { return null; }
        @Override public android.content.AttributionSource getAttributionSource() {
            return new android.content.AttributionSource.Builder(0).setPackageName(pkg).build();
        }
        // Return the Application (not the base) per Android semantics: framework code keys off
        // it — e.g. WorkManager's on-demand init does getApplicationContext() instanceof
        // Configuration.Provider, which only holds if the Application flows back here.
        @Override public android.content.Context getApplicationContext() {
            return application != null ? application : this;
        }
        public int getDisplayId() { return 0; }
        @Override public android.os.Looper getMainLooper() {
            return android.os.Looper.getMainLooper();
        }
        @Override public android.content.pm.ApplicationInfo getApplicationInfo() {
            android.content.pm.ApplicationInfo ai = new android.content.pm.ApplicationInfo();
            ai.packageName = pkg;
            ai.processName = pkg;   // single-process; WorkManager compares against this
            return ai;
        }
        // AppContext has a null base (super(null)), so Context methods not overridden here
        // would NPE. Device-protected (direct-boot) storage isn't modeled → plain storage.
        @Override public boolean isDeviceProtectedStorage() { return false; }
        private android.content.ContentResolver resolver;  // one per context (PhoneWindow, apps)
        @Override public android.content.ContentResolver getContentResolver() {
            if (resolver == null) resolver = new AppContentResolver(this);
            return resolver;
        }
        @Override public Resources.Theme getTheme() {
            if (theme == null) {
                theme = res.newTheme();
                // Apply the app's manifest theme so decor/layout inflation resolves theme
                // attributes (a bare newTheme() has no styles → InflateException).
                if (appThemeResId != 0) {
                    try { theme.applyStyle(appThemeResId, true); } catch (Throwable ignored) {}
                }
            }
            return theme;
        }
        @Override public void setTheme(int resId) {
            if (resId != 0) { appThemeResId = resId; theme = null; }  // re-resolve on next getTheme
        }
        @Override public Object getSystemService(String name) {
            if ("layout_inflater".equals(name)) {
                try {
                    return Class.forName("com.android.internal.policy.PhoneLayoutInflater")
                            .getConstructor(android.content.Context.class).newInstance(this);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("activity".equals(name)) {
                // ActivityManager's ctor is package-private; construct via reflection. Its
                // methods route through the seeded ServiceManager("activity") IActivityManager
                // proxy, so getRunningAppProcesses() etc. return defaults instead of NPEing.
                try {
                    java.lang.reflect.Constructor<?> c = android.app.ActivityManager.class
                            .getDeclaredConstructor(android.content.Context.class,
                                    android.os.Handler.class);
                    c.setAccessible(true);
                    return c.newInstance(this, null);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("jobscheduler".equals(name)) {
                return new AppJobScheduler();  // androidx WorkManager's SystemJobScheduler
            }
            if ("notification".equals(name)) {
                // NotificationManager(Context, Handler); its calls route through the seeded
                // ServiceManager("notification") → INotificationManager proxy (no-op), so
                // createNotificationChannels etc. are accepted and dropped (no notif host yet).
                try {
                    java.lang.reflect.Constructor<?> c = android.app.NotificationManager.class
                            .getDeclaredConstructor(android.content.Context.class,
                                    android.os.Handler.class);
                    c.setAccessible(true);
                    return c.newInstance(this, null);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("connectivity".equals(name)) {
                // acl's clean-room android.net.ConnectivityManager (on the boot classpath via
                // this bootstrap jar, backed by svc/net) has a public no-arg ctor. Reflective
                // so HostBootstrap need not compile against the shim's signature (it compiles
                // against the framework classes.jar, whose ConnectivityManager differs).
                try {
                    return Class.forName("android.net.ConnectivityManager")
                            .getConstructor().newInstance();
                } catch (Throwable t) {
                    return null;
                }
            }
            return null;
        }
        // getSystemService(Class) → getSystemServiceName(Class) → getSystemService(String).
        // Map the classes we back; unknown → null name → getSystemService(Class) yields null
        // (remaining Managers are svc/-routed later, ADR-2). @hide → compiled vs classes.jar.
        @Override public String getSystemServiceName(Class<?> serviceClass) {
            if (serviceClass == android.app.ActivityManager.class) return "activity";
            if (serviceClass == android.app.job.JobScheduler.class) return "jobscheduler";
            if (serviceClass == android.app.NotificationManager.class) return "notification";
            // ConnectivityManager compared by name (off this compile classpath, see above).
            if ("android.net.ConnectivityManager".equals(serviceClass.getName())) return "connectivity";
            return null;
        }

        // No broadcast infrastructure yet — accept registrations as no-ops. Return an EMPTY
        // Intent (not null) so sticky-broadcast reads (receiver==null, e.g. WorkManager's
        // battery/storage trackers reading ACTION_BATTERY_CHANGED) get defaults instead of an
        // NPE. Broadcasts are an svc/ concern later.
        @Override public android.content.Intent registerReceiver(
                android.content.BroadcastReceiver r, android.content.IntentFilter f) {
            return new android.content.Intent();
        }
        @Override public android.content.Intent registerReceiver(
                android.content.BroadcastReceiver r, android.content.IntentFilter f, int flags) {
            return new android.content.Intent();
        }
        @Override public android.content.Intent registerReceiver(
                android.content.BroadcastReceiver r, android.content.IntentFilter f,
                String perm, android.os.Handler h) {
            return new android.content.Intent();
        }
        @Override public android.content.Intent registerReceiver(
                android.content.BroadcastReceiver r, android.content.IntentFilter f,
                String perm, android.os.Handler h, int flags) {
            return new android.content.Intent();
        }
        @Override public void unregisterReceiver(android.content.BroadcastReceiver r) {}
        @Override public void sendBroadcast(android.content.Intent i) {}
        @Override public void sendBroadcast(android.content.Intent i, String perm) {}

        // File-backed SharedPreferences (real android.app.SharedPreferencesImpl over a temp
        // dir) — apps read/write prefs in Application.onCreate. No app data dir yet, so a temp.
        private final java.util.Map<String, android.content.SharedPreferences> prefs =
                new java.util.HashMap<>();
        private java.io.File prefsDir;
        @Override public android.content.SharedPreferences getSharedPreferences(String name,
                int mode) {
            android.content.SharedPreferences sp = prefs.get(name);
            if (sp != null) return sp;
            try {
                if (prefsDir == null) {
                    prefsDir = new java.io.File(System.getProperty("java.io.tmpdir"),
                                                "acl-prefs-" + pkg);
                    prefsDir.mkdirs();
                }
                java.io.File f = new java.io.File(prefsDir, name + ".xml");
                java.lang.reflect.Constructor<?> ctor =
                        Class.forName("android.app.SharedPreferencesImpl")
                                .getDeclaredConstructor(java.io.File.class, int.class);
                ctor.setAccessible(true);
                sp = (android.content.SharedPreferences) ctor.newInstance(f, mode);
                prefs.put(name, sp);
            } catch (Throwable t) {
                System.err.println("acl HostBootstrap: getSharedPreferences failed: " + t);
            }
            return sp;
        }
        @Override public java.io.File getFilesDir() {
            java.io.File d = new java.io.File(System.getProperty("java.io.tmpdir"),
                                              "acl-files-" + pkg);
            d.mkdirs();
            return d;
        }
        @Override public java.io.File getCacheDir() {
            java.io.File d = new java.io.File(System.getProperty("java.io.tmpdir"),
                                              "acl-cache-" + pkg);
            d.mkdirs();
            return d;
        }
        @Override public java.io.File getNoBackupFilesDir() {
            java.io.File d = new java.io.File(System.getProperty("java.io.tmpdir"),
                                              "acl-nobackup-" + pkg);
            d.mkdirs();
            return d;
        }
        // Room/SQLite (androidx WorkManager's WorkDatabase) — back onto the host libsqlite
        // in a temp databases dir. No app sandbox yet; a per-package dir suffices.
        @Override public java.io.File getDatabasePath(String name) {
            java.io.File d = new java.io.File(System.getProperty("java.io.tmpdir"),
                                              "acl-db-" + pkg);
            d.mkdirs();
            return new java.io.File(d, name);
        }
        @Override public android.database.sqlite.SQLiteDatabase openOrCreateDatabase(
                String name, int mode, android.database.sqlite.SQLiteDatabase.CursorFactory f) {
            return android.database.sqlite.SQLiteDatabase.openOrCreateDatabase(
                    getDatabasePath(name).getPath(), f);
        }
        @Override public android.database.sqlite.SQLiteDatabase openOrCreateDatabase(
                String name, int mode, android.database.sqlite.SQLiteDatabase.CursorFactory f,
                android.database.DatabaseErrorHandler eh) {
            return android.database.sqlite.SQLiteDatabase.openOrCreateDatabase(
                    getDatabasePath(name).getPath(), f, eh);
        }
        @Override public boolean deleteDatabase(String name) {
            return getDatabasePath(name).delete();
        }
        // The app classloader (HostBootstrap is injected on the app classpath, so its loader
        // sees the app classes) — resource/XML inflation resolves app classes through this.
        @Override public ClassLoader getClassLoader() {
            return getClass().getClassLoader();
        }
        // No Binder/PMS (ADR-3) → a defaulted PackageManager (StubPackageManager) with the
        // app-meaningful bits: getPackageInfo throws NameNotFoundException (queries resolve as
        // "not installed"), getPackageName is the app's.
        private android.content.pm.PackageManager pm;
        @Override public android.content.pm.PackageManager getPackageManager() {
            if (pm == null) pm = new AppPackageManager(pkg);
            return pm;
        }
    }

    static final class AppPackageManager extends StubPackageManager {
        private final String pkg;
        AppPackageManager(String pkg) { this.pkg = pkg; }
        @Override public android.content.pm.PackageInfo getPackageInfo(String p, int flags)
                throws android.content.pm.PackageManager.NameNotFoundException {
            throw new android.content.pm.PackageManager.NameNotFoundException(p);
        }
    }

    // acl has no ContentProvider host yet (ADR-3): a ContentResolver whose provider lookups
    // return null, so ContentResolver's concrete query/insert/… yield null/no-op and callers
    // (e.g. PhoneWindow reading Settings.System) fall back to defaults. Compiled against the
    // framework classes.jar (the abstract methods + IContentProvider are hidden).
    static final class AppContentResolver extends android.content.ContentResolver {
        AppContentResolver(android.content.Context c) { super(c); }
        // A no-Binder IContentProvider whose calls return defaults (null Cursor/Bundle).
        // Not null: ContentResolver.call() derefs the provider without a null check, and
        // PhoneWindow reads Settings via call() — a null provider would NPE.
        private static android.content.IContentProvider stubProvider() {
            return (android.content.IContentProvider) noop(android.content.IContentProvider.class);
        }
        @Override protected android.content.IContentProvider acquireProvider(
                android.content.Context c, String name) { return stubProvider(); }
        @Override protected android.content.IContentProvider acquireUnstableProvider(
                android.content.Context c, String name) { return stubProvider(); }
        @Override public boolean releaseProvider(android.content.IContentProvider p) { return false; }
        @Override public boolean releaseUnstableProvider(android.content.IContentProvider p) { return false; }
        @Override public void unstableProviderDied(android.content.IContentProvider p) {}
    }

    // acl has no JobScheduler service (ADR-3). Accept schedules (RESULT_SUCCESS) and
    // report no pending jobs so androidx WorkManager's SystemJobScheduler initializes;
    // the jobs are dropped (WorkManager also runs an in-process GreedyScheduler).
    static final class AppJobScheduler extends StubJobScheduler {
        @Override public int schedule(android.app.job.JobInfo job) {
            return android.app.job.JobScheduler.RESULT_SUCCESS;
        }
        @Override public java.util.List getAllPendingJobs() {
            return new java.util.ArrayList();
        }
    }

    /** Attach a real Activity: base context + mActivityInfo + PhoneWindow + the app Application. */
    public static void attachActivityContext(android.app.Activity a, android.content.Context ctx,
            android.app.Application app) throws Exception {
        setField(ContextWrapper.class, a, "mBase", ctx);
        if (app != null) {
            try { setField(android.app.Activity.class, a, "mApplication", app); }
            catch (Throwable ignored) {}
        }
        attachActivity(a, ctx);
    }

    // Create + run the app's Application (the singleton real apps set in Application.onCreate,
    // e.g. FDroidApp.INSTANCE). ActivityThread normally does this; we construct it, bind an app
    // Context, and call onCreate. Returns the Application (acl also sets it as Activity.mApplication
    // so getApplication()/getApplicationContext() work). appClass empty → a bare Application.
    public static android.app.Application createApplication(String appClass, String apkPath,
            String frameworkResPath, String pkg) throws Exception {
        android.content.Context base = newAppContext(apkPath, frameworkResPath, pkg);
        android.app.Application app = (appClass != null && !appClass.isEmpty())
                ? (android.app.Application) Class.forName(appClass).getConstructor().newInstance()
                : new android.app.Application();
        setField(ContextWrapper.class, app, "mBase", base);
        if (base instanceof AppContext) ((AppContext) base).setApplication(app);
        seedActivityThread(pkg);
        app.onCreate();  // sets the app's own singleton; may throw on a not-yet-supported API
        return app;
    }

    // Allocate an instance without running its constructor (no side effects) — for seeding
    // hollow framework singletons acl doesn't fully stand up.
    private static Object allocateHollow(Class<?> c) throws Exception {
        Class<?> u = Class.forName("sun.misc.Unsafe");
        java.lang.reflect.Field tf = u.getDeclaredField("theUnsafe");
        tf.setAccessible(true);
        Object unsafe = tf.get(null);
        return u.getMethod("allocateInstance", Class.class).invoke(unsafe, c);
    }

    // Seed a minimal ActivityThread so Application.getProcessName() (→ ActivityThread.
    // currentProcessName() → mBoundApplication.processName) is non-null — androidx
    // WorkManager requires it. acl has no real ActivityThread (ADR-2/3); this is a hollow
    // instance (Unsafe-allocated, no ctor) carrying only the process name + appInfo.
    static void seedActivityThread(String pkg) {
        try {
            Class<?> at = Class.forName("android.app.ActivityThread");
            if (at.getMethod("currentActivityThread").invoke(null) != null) return;
            Object atInst = allocateHollow(at);
            Class<?> abd = Class.forName("android.app.ActivityThread$AppBindData");
            Object abdInst = allocateHollow(abd);
            setField(abd, abdInst, "processName", pkg);
            android.content.pm.ApplicationInfo ai = new android.content.pm.ApplicationInfo();
            ai.packageName = pkg;
            ai.processName = pkg;
            setField(abd, abdInst, "appInfo", ai);
            setField(at, atInst, "mBoundApplication", abdInst);
            java.lang.reflect.Field s = at.getDeclaredField("sCurrentActivityThread");
            s.setAccessible(true);
            s.set(null, atInst);
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: seedActivityThread failed: " + t);
        }
    }

    // Lightweight Activity attach: a real Activity is normally attach()'d by ActivityThread
    // (mActivityInfo, mWindow=PhoneWindow, mApplication, …) before onCreate. We can't stand up
    // ActivityThread, so seed the minimum onCreate/setContentView need via reflection: a
    // default ActivityInfo and a PhoneWindow bound to the context. Returns the Activity so the
    // caller can drive onCreate then read getWindow().getDecorView() as the content view.
    public static void attachActivity(android.app.Activity a, android.content.Context ctx)
            throws Exception {
        setField(android.app.Activity.class, a, "mActivityInfo",
                 new android.content.pm.ActivityInfo());
        try {
            setField(android.app.Activity.class, a, "mUiThread", Thread.currentThread());
        } catch (Throwable ignored) {
        }
        // PhoneWindow is best-effort: its ctor reads settings via getContentResolver (which our
        // minimal Context lacks). Needed for setContentView but not for early onCreate; don't
        // let it fail the attach.
        try {
            Object window = Class.forName("com.android.internal.policy.PhoneWindow")
                    .getConstructor(android.content.Context.class).newInstance(ctx);
            setField(android.app.Activity.class, a, "mWindow", window);
            // Activity implements Window.Callback; real Activity.attach() wires it so the
            // window routes events to the Activity (AppCompat requires a non-null callback).
            ((android.view.Window) window).setCallback(a);
        } catch (Throwable t) {
            Throwable cause = (t instanceof java.lang.reflect.InvocationTargetException
                    && t.getCause() != null) ? t.getCause() : t;
            System.err.println("acl HostBootstrap: PhoneWindow deferred: " + cause);
        }
    }

    private static void setField(Class<?> c, Object obj, String name, Object val) throws Exception {
        java.lang.reflect.Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        f.set(obj, val);
    }

    static final class ViewContext extends ContextWrapper {
        private Resources.Theme theme;
        ViewContext() { super(null); }
        @Override public Resources getResources() { return Resources.getSystem(); }
        // getDisplayId() is @hide; overrides at runtime by signature. DEFAULT_DISPLAY = 0.
        public int getDisplayId() { return 0; }
        @Override public Object getSystemService(String name) {
            // PhoneWindow/setContentView needs a LayoutInflater (LayoutInflater.from →
            // getSystemService(LAYOUT_INFLATER_SERVICE)). Concrete impl is internal.
            if ("layout_inflater".equals(name)) {
                try {
                    return Class.forName("com.android.internal.policy.PhoneLayoutInflater")
                            .getConstructor(android.content.Context.class).newInstance(this);
                } catch (Throwable t) {
                    return null;
                }
            }
            return null;
        }
        // A real (empty) Theme from the system Resources — View/ViewGroup constructors call
        // context.obtainStyledAttributes(), which delegates to getTheme().
        @Override public Resources.Theme getTheme() {
            if (theme == null) theme = Resources.getSystem().newTheme();
            return theme;
        }
        @Override public android.os.Looper getMainLooper() {
            return android.os.Looper.getMainLooper();
        }
        @Override public android.content.pm.ApplicationInfo getApplicationInfo() {
            return new android.content.pm.ApplicationInfo();  // defaults (targetSdk etc.)
        }
    }

    // A no-op InvocationHandler returning type-appropriate defaults (false/0/null).
    private static Object noop(Class<?> iface) {
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> defaultValue(method.getReturnType()));
    }

    // INotificationManager translated to the host notification daemon (svc/notify).
    // enqueue → NotifyBridge.nNotify (host Notify), cancel → nClose; per-app-id → server-id
    // map keeps replace/cancel coherent. Channel ops and everything else default.
    private static Object makeNotificationHandler(Class<?> iface) {
        final java.util.Map<Integer, Long> ids = new java.util.concurrent.ConcurrentHashMap<>();
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> {
                    switch (method.getName()) {
                        case "enqueueNotificationWithTag": {
                            // (String pkg, String opPkg, String tag, int id, Notification n, int user)
                            String pkg = (String) a[0];
                            int id = (Integer) a[3];
                            String[] tt = notificationTitleText(a[4]);
                            long prev = ids.getOrDefault(id, 0L);
                            long server = NotifyBridge.nNotify(pkg, (int) prev, tt[0], tt[1]);
                            if (server != 0) ids.put(id, server);
                            return null;
                        }
                        case "cancelNotificationWithTag": {
                            Long server = ids.remove((Integer) a[3]);
                            if (server != null) NotifyBridge.nClose(server);
                            return null;
                        }
                        case "cancelAllNotifications": {
                            for (Long s : ids.values()) NotifyBridge.nClose(s);
                            ids.clear();
                            return null;
                        }
                        default:
                            return defaultValue(method.getReturnType());
                    }
                });
    }

    // Read android.title / android.text out of a Notification's extras Bundle (reflectively —
    // HostBootstrap compiles against the framework classes.jar).
    private static String[] notificationTitleText(Object notification) {
        String title = "", text = "";
        try {
            Object extras = notification.getClass().getField("extras").get(notification);
            if (extras != null) {
                java.lang.reflect.Method getCs =
                        extras.getClass().getMethod("getCharSequence", String.class);
                Object t = getCs.invoke(extras, "android.title");
                Object x = getCs.invoke(extras, "android.text");
                if (t != null) title = t.toString();
                if (x != null) text = x.toString();
            }
        } catch (Throwable ignored) {
        }
        return new String[]{title, text};
    }

    private static Object defaultValue(Class<?> rt) {
        if (rt == boolean.class) return false;
        if (rt == int.class || rt == long.class || rt == short.class || rt == byte.class
                || rt == char.class) return 0;
        if (rt == float.class || rt == double.class) return 0;
        // Empty collections (not null) so callers that iterate a Binder result don't NPE.
        if (java.util.List.class.isAssignableFrom(rt)) return new java.util.ArrayList<>();
        if (java.util.Map.class.isAssignableFrom(rt)) return new java.util.HashMap<>();
        if (java.util.Set.class.isAssignableFrom(rt)) return new java.util.HashSet<>();
        // A Binder method that returns another Binder interface (e.g. IActivityTaskManager.
        // getActivityClientController → IActivityClientController) → a nested no-op, so the
        // caller's follow-up call doesn't NPE. IBinder stays null (callers null-check it).
        if (rt.isInterface() && rt != android.os.IBinder.class) return noop(rt);
        return null;
    }

    // acl has no Binder (ADR-3): seed a no-op ServiceManager. getService(name) returns a
    // local IBinder whose queryLocalInterface hands back a no-op service interface, so
    // framework callers (ViewConfiguration → IWindowManager, getSystemService, …) get a stub
    // that returns defaults instead of transacting over Binder. Unknown services → null.
    private static void seedNoBinderServiceManager() throws Exception {
        final java.util.Map<String, String> svc = new java.util.HashMap<>();
        svc.put("window", "android.view.IWindowManager");
        // A real ActivityManager (constructed in AppContext.getSystemService) routes its calls
        // through ServiceManager.getService("activity") → IActivityManager; a defaulting proxy
        // (empty process list, etc.) is enough for boot-time probes like isAcraProcess.
        svc.put("activity", "android.app.IActivityManager");
        // NotificationManager routes through INotificationManager; acl translates it to the
        // host notification daemon (svc/notify → org.freedesktop.Notifications) instead of a
        // no-op proxy.
        svc.put("notification", "android.app.INotificationManager");
        // Activity.setTaskDescription (called during theme init) → ActivityClient →
        // IActivityTaskManager.getActivityClientController() → a nested no-op.
        svc.put("activity_task", "android.app.IActivityTaskManager");
        Class<?> ism = Class.forName("android.os.IServiceManager");
        Object proxy = java.lang.reflect.Proxy.newProxyInstance(
                ism.getClassLoader(), new Class<?>[]{ism},
                (p, method, a) -> {
                    final String mn = method.getName();
                    if ("getService".equals(mn) || "checkService".equals(mn)) {
                        String name = (String) a[0];
                        String ifaceName = svc.get(name);
                        if (ifaceName == null) return null;
                        Class<?> iface = Class.forName(ifaceName);
                        final Object localIface = "notification".equals(name)
                                ? makeNotificationHandler(iface) : noop(iface);
                        final String descriptor = ifaceName;  // AIDL DESCRIPTOR == interface FQN
                        // Proxy IBinder (an interface) rather than new Binder() — the latter
                        // needs Binder natives. asInterface() calls queryLocalInterface and
                        // uses the local stub directly.
                        return java.lang.reflect.Proxy.newProxyInstance(
                                iface.getClassLoader(), new Class<?>[]{android.os.IBinder.class},
                                (bp, bm, ba) -> {
                                    if ("queryLocalInterface".equals(bm.getName()))
                                        return descriptor.equals(ba[0]) ? localIface : null;
                                    Class<?> brt = bm.getReturnType();
                                    if (brt == boolean.class) return false;
                                    if (brt == int.class) return 0;
                                    return null;
                                });
                    }
                    Class<?> rt = method.getReturnType();
                    if (rt == boolean.class) return false;
                    if (rt == int.class) return 0;
                    return null;
                });
        java.lang.reflect.Field f =
                Class.forName("android.os.ServiceManager").getDeclaredField("sServiceManager");
        f.setAccessible(true);
        f.set(null, proxy);
    }
}
