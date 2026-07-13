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
        AppContext(Resources res, String pkg) { super(null); this.res = res; this.pkg = pkg; }
        @Override public Resources getResources() { return res; }
        @Override public String getPackageName() { return pkg; }
        @Override public android.content.Context getApplicationContext() { return this; }
        public int getDisplayId() { return 0; }
        @Override public android.os.Looper getMainLooper() {
            return android.os.Looper.getMainLooper();
        }
        @Override public android.content.pm.ApplicationInfo getApplicationInfo() {
            android.content.pm.ApplicationInfo ai = new android.content.pm.ApplicationInfo();
            ai.packageName = pkg;
            return ai;
        }
        @Override public Resources.Theme getTheme() {
            if (theme == null) theme = res.newTheme();
            return theme;
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
            return null;
        }
        // getSystemService(Class) → getSystemServiceName(Class) → getSystemService(String).
        // No service map, so return null name → getSystemService(Class) yields null (Managers
        // are svc/-routed later, ADR-2). @hide, so compiled against the framework classes.jar.
        @Override public String getSystemServiceName(Class<?> serviceClass) { return null; }

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
        app.onCreate();  // sets the app's own singleton; may throw on a not-yet-supported API
        return app;
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
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: PhoneWindow deferred: " + t);
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
                (p, method, a) -> {
                    Class<?> rt = method.getReturnType();
                    if (rt == boolean.class) return false;
                    if (rt == int.class || rt == long.class || rt == short.class
                            || rt == byte.class) return 0;
                    if (rt == float.class || rt == double.class) return 0;
                    return null;
                });
    }

    // acl has no Binder (ADR-3): seed a no-op ServiceManager. getService(name) returns a
    // local IBinder whose queryLocalInterface hands back a no-op service interface, so
    // framework callers (ViewConfiguration → IWindowManager, getSystemService, …) get a stub
    // that returns defaults instead of transacting over Binder. Unknown services → null.
    private static void seedNoBinderServiceManager() throws Exception {
        final java.util.Map<String, String> svc = new java.util.HashMap<>();
        svc.put("window", "android.view.IWindowManager");
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
                        final Object localIface = noop(iface);
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
