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
    // The theme to apply to the app Context + Activity + window decor: the manifest's app theme,
    // or a full framework default (DeviceDefault has the complete android:attr set) when the app
    // declares none. Used consistently so the Activity, its window, and inflated views agree.
    static int defaultAppTheme() {
        return appThemeResId != 0 ? appThemeResId
                                  : android.R.style.Theme_DeviceDefault_Light_DarkActionBar;
    }

    // Drain the main Looper's currently-pending messages (non-blocking). The GPU present loop
    // is a native frame loop, not Looper.loop(), so Handler-posted work never runs on its own —
    // but Jetpack Compose's recomposer (and other async framework work) posts its continuations
    // to the main Handler. Pump them each frame (after Choreographer.doFrame) so the composition
    // completes before measure/layout, which would otherwise block. Bounded; stops when idle.
    public static void pumpMainLooper(int maxIters) {
        try {
            android.os.Looper looper = android.os.Looper.getMainLooper();
            if (looper == null) return;
            android.os.MessageQueue q = looper.getQueue();
            java.lang.reflect.Method isIdle = android.os.MessageQueue.class.getMethod("isIdle");
            java.lang.reflect.Method loopOnce = android.os.Looper.class.getDeclaredMethod(
                    "loopOnce", android.os.Looper.class, long.class, int.class);
            loopOnce.setAccessible(true);
            for (int i = 0; i < maxIters; i++) {
                if (Boolean.TRUE.equals(isIdle.invoke(q))) break;  // nothing due
                Object cont = loopOnce.invoke(null, looper, 0L, -1);
                if (Boolean.FALSE.equals(cont)) break;  // looper asked to quit
            }
        } catch (Throwable ignored) {
        }
    }

    // Seed the default typefaces from acl's bundled font. The system font map is normally
    // pushed to the process by the framework at boot (SharedMemory); a standalone run has
    // none, leaving Typeface.DEFAULT/sDefaults null, so create()/text rendering would NPE.
    // Build one Typeface (via the acl graphics Font/Typeface natives) and install it as the
    // default + the common family names. Runs once, after the graphics natives are registered.
    public static void seedTypefaces() {
        try {
            Class<?> tfc = Class.forName("android.graphics.Typeface");
            // Build the default Typeface directly from acl's bundled native SkTypeface:
            // nativeCreateFromArray(empty) returns default_typeface(), then wrap it with the
            // private Typeface(long) ctor. Avoids Typeface.createFromFile's Java font-loading
            // path (Font.Builder mmap), which fails silently on this host and returns DEFAULT.
            java.lang.reflect.Method createArr = tfc.getDeclaredMethod(
                    "nativeCreateFromArray", long[].class, long.class, int.class, int.class);
            createArr.setAccessible(true);
            long ni = (Long) createArr.invoke(null, new long[0], 0L, 400, 0);
            java.lang.reflect.Constructor<?> ctor = tfc.getDeclaredConstructor(long.class);
            ctor.setAccessible(true);
            Object tf = ctor.newInstance(ni);
            if (tf == null) return;
            java.lang.reflect.Method setDefault = tfc.getDeclaredMethod("setDefault", tfc);
            setDefault.setAccessible(true);
            setDefault.invoke(null, tf);
            // Public static final Typeface fields — set via the native (bypasses final).
            java.lang.reflect.Method force =
                    tfc.getDeclaredMethod("nativeForceSetStaticFinalField", String.class, tfc);
            force.setAccessible(true);
            for (String f : new String[]{"DEFAULT", "DEFAULT_BOLD", "SANS_SERIF", "SERIF", "MONOSPACE"})
                force.invoke(null, f, tf);
            Object arr = java.lang.reflect.Array.newInstance(tfc, 4);
            for (int i = 0; i < 4; i++) java.lang.reflect.Array.set(arr, i, tf);
            java.lang.reflect.Field sDefaults = tfc.getDeclaredField("sDefaults");
            sDefaults.setAccessible(true);
            sDefaults.set(null, arr);
            // Distinct serif + monospace families so Typeface.create("serif"/"monospace") and
            // styled text (e.g. <tt>, code views) don't all render as the sans default. Probe real
            // font files on the host / AOSP tree (no in-tree font vendoring — mirrors the native
            // default_typeface() candidate list); fall back to the sans default if none is found.
            // Each is loaded via the graphics native nTypefaceFromPath (SkTypeface::MakeFromFile).
            // Per-script glyph fallback (CJK, emoji) needs a fonts.xml -> SkFontMgr collection in
            // graphics_jni; still future. See docs/font-scope.md.
            Object serif = firstTypeface(new String[]{
                    System.getenv("ACL_FONT_SERIF"),
                    "/usr/share/fonts/google-noto-vf/NotoSerif[wght].ttf",
                    "/usr/share/fonts/liberation-serif/LiberationSerif-Regular.ttf",
                    "/usr/share/fonts/liberation-serif-fonts/LiberationSerif-Regular.ttf",
                    "/usr/share/fonts/dejavu-serif-fonts/DejaVuSerif.ttf",
                    "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
            });
            if (serif == null) serif = tf;
            Object mono = firstTypeface(new String[]{
                    System.getenv("ACL_FONT_MONO"),
                    "/usr/share/fonts/adwaita-mono-fonts/AdwaitaMono-Regular.ttf",
                    "/usr/share/fonts/google-noto-vf/NotoSansMono[wght].ttf",
                    "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf",
                    "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
                    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            });
            if (mono == null) mono = tf;

            java.lang.reflect.Field smf = tfc.getDeclaredField("sSystemFontMap");
            smf.setAccessible(true);
            Object mapObj = smf.get(null);
            if (mapObj instanceof java.util.Map) {
                @SuppressWarnings("unchecked")
                java.util.Map<String, Object> m = (java.util.Map<String, Object>) mapObj;
                // sans-serif and its weight/name aliases (fonts.xml sans family).
                for (String n : new String[]{"sans-serif", "default", "arial", "helvetica",
                        "sans-serif-medium", "sans-serif-light", "sans-serif-condensed",
                        "sans-serif-condensed-light", "sans-serif-condensed-medium",
                        "sans-serif-black", "sans-serif-thin", "cursive", "casual"})
                    m.put(n, tf);
                // serif family + serif-like aliases.
                for (String n : new String[]{"serif", "georgia", "baskerville", "palatino",
                        "goudy", "fantasy", "times", "times new roman"})
                    m.put(n, serif);
                // monospace family + mono aliases.
                for (String n : new String[]{"monospace", "sans-serif-monospace", "serif-monospace",
                        "courier", "courier new", "monaco"})
                    m.put(n, mono);
            }
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: seedTypefaces failed: " + t);
        }
    }

    // Build a Typeface from the first existing font file in `candidates`, via the graphics
    // Font/FontFamily/Typeface natives (path route -> SkTypeface::MakeFromFile), bypassing the
    // framework's Font.Builder(File) mmap/validation path (which fails silently on this host and
    // yields DEFAULT). Returns null if no candidate loads. See seedTypefaces()'s NOTE.
    // acl graphics native: SkTypeface::MakeFromFile(path) -> Typeface native handle (an
    // SkTypeface*). Bound by the runtime from libacl_graphics_jni (acl_typeface_from_path). This
    // is the dedicated, non-@CriticalNative entry the framework's own font-loading path lacks on
    // this host (Typeface.createFromFile returns DEFAULT; the Font$Builder natives segfault when
    // driven reflectively). Returns 0 if unbound or the file can't be loaded. See docs/font-scope.md.
    static native long nTypefaceFromPath(String path);

    private static Object firstTypeface(String[] candidates) {
        Class<?> tfc;
        java.lang.reflect.Constructor<?> ctor;
        try {
            tfc = Class.forName("android.graphics.Typeface");
            ctor = tfc.getDeclaredConstructor(long.class);
            ctor.setAccessible(true);
        } catch (Throwable t) {
            return null;
        }
        for (String path : candidates) {
            if (path == null || path.isEmpty()) continue;
            if (!new java.io.File(path).exists()) continue;
            try {
                long handle = nTypefaceFromPath(path);
                if (handle != 0) {
                    Object tf = ctor.newInstance(handle);
                    if (tf != null) {
                        System.err.println("acl HostBootstrap: loaded typeface " + path);
                        return tf;
                    }
                }
            } catch (Throwable t) {
                // try the next candidate
            }
        }
        return null;
    }

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
        seedSqliteDefaults();
        seedContentService();
        seedVirtualInputDevice();
    }

    // KeyCharacterMap.load(VIRTUAL_KEYBOARD) → InputManager.getInputDevice(-1) → null (no input
    // service) → UnavailableException, which takes down PhoneWindow.preparePanel (menu setup) and
    // any KeyEvent character mapping. Seed InputManager's device cache with a virtual keyboard
    // whose KeyCharacterMap is an empty one built via the native parcel format — no native helper:
    // android::KeyCharacterMap::readFromParcel reads {cString name, int type, bool overlay, and
    // three int counts}; writeCString("") is four zero bytes == writeInt(0), and writeBool ==
    // writeInt32, so the whole record is a run of ints.
    private static void seedVirtualInputDevice() {
        try {
            android.os.Parcel p = android.os.Parcel.obtain();
            p.writeInt(0);  // loadFileName "" (writeCString)
            p.writeInt(4);  // KeyboardType::FULL
            p.writeInt(0);  // mLayoutOverlayApplied = false
            p.writeInt(0);  // numKeys
            p.writeInt(0);  // numKeysByScanCode
            p.writeInt(0);  // numKeysByUsageCode
            p.setDataPosition(0);
            android.view.KeyCharacterMap kcm = android.view.KeyCharacterMap.CREATOR.createFromParcel(p);
            p.recycle();
            android.view.InputDevice dev = new android.view.InputDevice(
                    android.view.KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, "acl Virtual", 0, 0,
                    "acl-virtual", false, android.view.InputDevice.SOURCE_KEYBOARD,
                    android.view.InputDevice.KEYBOARD_TYPE_ALPHABETIC, kcm,
                    false, false, false, false, false);
            Object im = Class.forName("android.hardware.input.InputManager")
                    .getMethod("getInstance").invoke(null);
            android.util.SparseArray<android.view.InputDevice> devices = new android.util.SparseArray<>();
            devices.put(android.view.KeyCharacterMap.VIRTUAL_KEYBOARD, dev);
            // A non-null mInputDevices makes populateInputDevicesLocked skip the input service.
            setField(im.getClass(), im, "mInputDevices", devices);
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: virtual input device seed skipped: " + t);
        }
    }

    // ContentResolver.getContentService() reads ServiceManager.getService("content") → null (no
    // Binder), so registerContentObserver/notifyChange NPE — and many views (ListView, TextView's
    // font-scale observer, …) register observers during inflation. Seed a no-op IContentService.
    private static void seedContentService() {
        try {
            Class<?> cr = Class.forName("android.content.ContentResolver");
            Class<?> ics = Class.forName("android.content.IContentService");
            java.lang.reflect.Field f = cr.getDeclaredField("sContentService");
            f.setAccessible(true);
            if (f.get(null) == null) f.set(null, noop(ics));
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: content service seed skipped: " + t);
        }
    }

    // SQLiteGlobal.getDefaultPageSize() does `new StatFs("/data")` unconditionally the first time
    // (before its debug.sqlite.pagesize SystemProperties fallback), which throws on the host (no
    // /data sandbox) and takes down any Room/SQLite open. Pre-seed the cached sDefaultPageSize so
    // that branch is skipped — the value then comes from the property fallback (default 4096).
    private static void seedSqliteDefaults() {
        try {
            Class<?> g = Class.forName("android.database.sqlite.SQLiteGlobal");
            java.lang.reflect.Field f = g.getDeclaredField("sDefaultPageSize");
            f.setAccessible(true);
            if (f.getInt(null) == 0) f.setInt(null, 4096);
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: SQLite default-page-size seed skipped: " + t);
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
        AppContext c = new AppContext(appResources(apkPath, frameworkResPath), packageName);
        sAppContext = c;  // the current app context, for acl-sourced broadcasts (battery/bluetooth)
        return c;
    }

    private static AppContext sAppContext;

    /** Deliver an acl-sourced Intent to the app context's registered receivers (main thread). */
    public static void deliverBroadcast(android.content.Intent intent) {
        final AppContext c = sAppContext;
        if (c == null || intent == null) return;
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) {
            c.deliverBroadcast(intent);
        } else {
            new android.os.Handler(android.os.Looper.getMainLooper())
                    .post(new Runnable() { @Override public void run() { c.deliverBroadcast(intent); } });
        }
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
        // A null-base ContextWrapper NPEs on getDisplay()/getDisplayNoVerify() (mBase == null);
        // WindowManagerImpl.addView calls mContext.getDisplayNoVerify(), so a Dialog/PopupWindow on
        // this context needs a real Display (Wayland output metrics).
        @Override public android.view.Display getDisplay() { return buildDisplay(); }
        public android.view.Display getDisplayNoVerify() { return buildDisplay(); }
        // ContextWrapper delegates this to a null base on this context; ViewRootImpl.<init>
        // (Dialog/PopupWindow add path) reads getBasePackageName(), so a null-base delegation
        // would NPE. Return the app package (getOpPackageName is already overridden below).
        public String getBasePackageName() { return pkg; }
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
        @Override public boolean isRestricted() { return false; }  // TextView.readTextAppearance
        @Override public boolean canLoadUnsafeResources() { return true; }  // same, @hide
        // Grant everything — a single-app host with no permission enforcement (ADR-3).
        @Override public int checkPermission(String perm, int pid, int uid) { return 0; }  // GRANTED
        @Override public int checkSelfPermission(String perm) { return 0; }
        @Override public int checkCallingPermission(String perm) { return 0; }
        @Override public int checkCallingOrSelfPermission(String perm) { return 0; }
        private android.content.ContentResolver resolver;  // one per context (PhoneWindow, apps)
        @Override public android.content.ContentResolver getContentResolver() {
            if (resolver == null) resolver = new AppContentResolver(this);
            return resolver;
        }
        @Override public Resources.Theme getTheme() {
            if (theme == null) {
                theme = res.newTheme();
                // Apply the app's manifest theme (or a framework default when the app declares
                // none) so decor/layout inflation resolves theme attributes. A bare newTheme()
                // has no styles → InflateException. PhoneWindow.generateLayout inflates the window
                // decor (screen_title/…) off THIS context theme, so it must be populated too.
                try { theme.applyStyle(defaultAppTheme(), true); } catch (Throwable ignored) {}
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
            if ("display".equals(name)) {
                // DisplayManager(Context); its calls route via DisplayManagerGlobal →
                // ServiceManager("display") IDisplayManager proxy (registerDisplayListener etc.
                // become no-ops). Material's NavigationBarItemView registers a DisplayListener.
                try {
                    java.lang.reflect.Constructor<?> c =
                            Class.forName("android.hardware.display.DisplayManager")
                                    .getDeclaredConstructor(android.content.Context.class);
                    c.setAccessible(true);
                    return c.newInstance(this);
                } catch (Throwable t) {
                    return null;
                }
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
            if ("clipboard".equals(name)) {
                // ClipboardManager(Context, Handler) → ServiceManager("clipboard") IClipboard
                // (seeded no-op). Compose's AndroidComposeView reads it.
                try {
                    java.lang.reflect.Constructor<?> c = android.content.ClipboardManager.class
                            .getDeclaredConstructor(android.content.Context.class,
                                    android.os.Handler.class);
                    c.setAccessible(true);
                    return c.newInstance(this, null);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("autofill".equals(name)) {
                // AutofillManager(Context, IAutoFillManager) — no-Binder proxy (Compose's
                // AndroidComposeView constructs an AndroidAutofill that requires a non-null one).
                try {
                    Class<?> iafm = Class.forName("android.view.autofill.IAutoFillManager");
                    java.lang.reflect.Constructor<?> c =
                            Class.forName("android.view.autofill.AutofillManager")
                                    .getDeclaredConstructor(android.content.Context.class, iafm);
                    c.setAccessible(true);
                    return c.newInstance(this, noop(iafm));
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("accessibility".equals(name)) {
                // AccessibilityManager.getInstance(ctx) routes via ServiceManager("accessibility")
                // → IAccessibilityManager (seeded no-op). Compose reads it during view setup.
                try {
                    return Class.forName("android.view.accessibility.AccessibilityManager")
                            .getMethod("getInstance", android.content.Context.class)
                            .invoke(null, this);
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
            if ("bluetooth".equals(name)) {
                // acl's clean-room android.bluetooth.BluetoothManager (boot classpath, backed by
                // svc/bluetooth → BlueZ). Reflective for the same reason as connectivity.
                try {
                    return Class.forName("android.bluetooth.BluetoothManager")
                            .getConstructor(android.content.Context.class).newInstance(this);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("audio".equals(name)) {
                // The real android.media.AudioManager (on the boot classpath); its stream-volume
                // calls route through the seeded IAudioService → svc/audio → host PipeWire.
                try {
                    java.lang.reflect.Constructor<?> c = android.media.AudioManager.class
                            .getDeclaredConstructor(android.content.Context.class);
                    c.setAccessible(true);
                    return c.newInstance(this);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("window".equals(name)) {
                // Dialog/PopupWindow/Toast: Window.setWindowManager casts the result to the
                // concrete (final) WindowManagerImpl and calls createLocalWindowManager, so we
                // must return a real one. Seed WindowManagerGlobal's no-Binder session first so
                // the subsequent addView -> ViewRootImpl.setView doesn't NPE on a null session.
                try {
                    seedWindowManagerGlobal();
                    return Class.forName("android.view.WindowManagerImpl")
                            .getConstructor(android.content.Context.class).newInstance(this);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("storage".equals(name)) {
                // The real android.os.storage.StorageManager(Context, Looper); its ctor resolves
                // ServiceManager.getServiceOrThrow("mount") — seeded above → getVolumeList works.
                try {
                    java.lang.reflect.Constructor<?> c = Class.forName("android.os.storage.StorageManager")
                            .getDeclaredConstructor(android.content.Context.class, android.os.Looper.class);
                    c.setAccessible(true);
                    return c.newInstance(this, android.os.Looper.getMainLooper());
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
            if (serviceClass == android.view.accessibility.AccessibilityManager.class) return "accessibility";
            if (serviceClass == android.view.autofill.AutofillManager.class) return "autofill";
            if (serviceClass == android.content.ClipboardManager.class) return "clipboard";
            if (serviceClass == android.app.NotificationManager.class) return "notification";
            if ("android.hardware.display.DisplayManager".equals(serviceClass.getName())) return "display";
            // ConnectivityManager compared by name (off this compile classpath, see above).
            if ("android.net.ConnectivityManager".equals(serviceClass.getName())) return "connectivity";
            if ("android.bluetooth.BluetoothManager".equals(serviceClass.getName())) return "bluetooth";
            if (serviceClass == android.os.storage.StorageManager.class) return "storage";
            // ViewRootImpl.<init> reads getSystemService(AudioManager.class) (navigation sound
            // effects); map it so the Dialog/PopupWindow add path gets a real AudioManager.
            if (serviceClass == android.media.AudioManager.class) return "audio";
            return null;
        }

        // No general broadcast bus yet — accept registrations as no-ops, but return the current
        // sticky Intent so sticky reads get real values (not an NPE). ACTION_BATTERY_CHANGED is
        // served from the host UPower backend (svc/battery via BatteryBridge); other actions get
        // an empty Intent. A receiver registered for the battery action is delivered the current
        // state once (sticky semantics). General broadcasts remain an svc/ concern.
        // Registered {receiver, filter} pairs — a minimal broadcast bus for the events acl
        // actually sources (battery, bluetooth). deliverBroadcast fans an Intent to matches.
        private final java.util.List<Object[]> receivers = new java.util.ArrayList<>();

        private android.content.Intent stickyFor(android.content.BroadcastReceiver r,
                                                 android.content.IntentFilter f) {
            if (r != null && f != null) {
                synchronized (receivers) { receivers.add(new Object[] { r, f }); }
            }
            android.content.Intent sticky = new android.content.Intent();
            if (f != null && f.hasAction(android.content.Intent.ACTION_BATTERY_CHANGED)) {
                try { sticky = BatteryBridge.batteryIntent(); } catch (Throwable ignored) {}
                if (r != null) {
                    try { r.onReceive(this, sticky); } catch (Throwable ignored) {}
                }
            }
            return sticky;
        }

        // Fan `intent` out to every registered receiver whose filter carries its action. Callers
        // deliver on the main thread (HostBootstrap.deliverBroadcast).
        void deliverBroadcast(android.content.Intent intent) {
            if (intent == null || intent.getAction() == null) return;
            final String action = intent.getAction();
            final java.util.List<android.content.BroadcastReceiver> targets = new java.util.ArrayList<>();
            synchronized (receivers) {
                for (Object[] e : receivers) {
                    if (((android.content.IntentFilter) e[1]).hasAction(action)) {
                        targets.add((android.content.BroadcastReceiver) e[0]);
                    }
                }
            }
            android.util.Log.i("acl.broadcast", "deliver " + action + " -> " + targets.size() + " receiver(s)");
            for (android.content.BroadcastReceiver r : targets) {
                try { r.onReceive(this, intent); } catch (Throwable ignored) {}
            }
        }
        @Override public android.content.Intent registerReceiver(
                android.content.BroadcastReceiver r, android.content.IntentFilter f) {
            return stickyFor(r, f);
        }
        @Override public android.content.Intent registerReceiver(
                android.content.BroadcastReceiver r, android.content.IntentFilter f, int flags) {
            return stickyFor(r, f);
        }
        @Override public android.content.Intent registerReceiver(
                android.content.BroadcastReceiver r, android.content.IntentFilter f,
                String perm, android.os.Handler h) {
            return stickyFor(r, f);
        }
        @Override public android.content.Intent registerReceiver(
                android.content.BroadcastReceiver r, android.content.IntentFilter f,
                String perm, android.os.Handler h, int flags) {
            return stickyFor(r, f);
        }
        @Override public void unregisterReceiver(android.content.BroadcastReceiver r) {
            synchronized (receivers) {
                for (java.util.Iterator<Object[]> it = receivers.iterator(); it.hasNext();) {
                    if (it.next()[0] == r) it.remove();
                }
            }
        }
        @Override public void sendBroadcast(android.content.Intent i) { deliverBroadcast(i); }
        @Override public void sendBroadcast(android.content.Intent i, String perm) { deliverBroadcast(i); }

        // Main-thread dispatch: ContextWrapper delegates these to the (null) base, so views that
        // post to the main thread (ListView/RecyclerView.getMainExecutor, etc.) NPE. Back them
        // with the real main Looper.
        private android.os.Handler mainHandler;
        private android.os.Handler mainHandler() {
            if (mainHandler == null) mainHandler = new android.os.Handler(android.os.Looper.getMainLooper());
            return mainHandler;
        }
        @Override public java.util.concurrent.Executor getMainExecutor() {
            final android.os.Handler h = mainHandler();
            return new java.util.concurrent.Executor() {
                @Override public void execute(Runnable r) { h.post(r); }
            };
        }

        // App service start/stop: acl has no ActivityManager service dispatch yet (ADR-3), and
        // ContextWrapper delegates to the null base (NPE). Acknowledge the start (return the
        // component) so onCreate proceeds; actually hosting the Service is a follow-on.
        @Override public android.content.ComponentName startService(android.content.Intent service) {
            return service != null ? service.getComponent() : null;
        }
        @Override public android.content.ComponentName startForegroundService(android.content.Intent service) {
            return service != null ? service.getComponent() : null;
        }
        @Override public boolean stopService(android.content.Intent service) { return true; }
        @Override public boolean bindService(android.content.Intent service,
                android.content.ServiceConnection conn, int flags) {
            return false;
        }
        @Override public void unbindService(android.content.ServiceConnection conn) {}

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
        // ── App storage, mapped onto the XDG Base Directory spec ─────────────────────────
        // Android's per-app sandbox (/data/data/<pkg>/{files,databases,cache,app_*}) has no
        // analogue on the host, so acl roots it under the freedesktop XDG dirs: persistent data
        // at $XDG_DATA_HOME/acl/<pkg>, caches at $XDG_CACHE_HOME/acl/<pkg> (defaults per spec:
        // ~/.local/share and ~/.cache). getDataDir() is that data root; the Android subdirs hang
        // off it. This replaces the null base (super(null)) that made every Context.getDir/
        // getFilesDir NPE, and keeps app data out of the (absent) /data sandbox.
        private static java.io.File xdgHome(String var, String fallbackRel) {
            String base = System.getenv(var);
            if (base == null || base.isEmpty()) {
                String home = System.getProperty("user.home");
                if (home == null || home.isEmpty()) home = System.getenv("HOME");
                if (home == null || home.isEmpty()) home = System.getProperty("java.io.tmpdir", "/tmp");
                base = home + "/" + fallbackRel;
            }
            return new java.io.File(base);
        }
        private java.io.File dataRoot() {
            return new java.io.File(xdgHome("XDG_DATA_HOME", ".local/share"), "acl/" + pkg);
        }
        private java.io.File cacheRoot() {
            return new java.io.File(xdgHome("XDG_CACHE_HOME", ".cache"), "acl/" + pkg);
        }
        private static java.io.File ensure(java.io.File d) { d.mkdirs(); return d; }

        @Override public java.io.File getDataDir() { return ensure(dataRoot()); }
        @Override public java.io.File getFilesDir() { return ensure(new java.io.File(dataRoot(), "files")); }
        @Override public java.io.File getCacheDir() { return ensure(cacheRoot()); }
        @Override public java.io.File getCodeCacheDir() { return ensure(new java.io.File(cacheRoot(), "code_cache")); }
        @Override public java.io.File getNoBackupFilesDir() { return ensure(new java.io.File(dataRoot(), "no_backup")); }
        // Context.getDir(name, mode) -> /data/data/<pkg>/app_<name>; keep the app_ prefix.
        @Override public java.io.File getDir(String name, int mode) {
            return ensure(new java.io.File(dataRoot(), "app_" + name));
        }
        @Override public java.io.File getFileStreamPath(String name) {
            return new java.io.File(getFilesDir(), name);
        }
        @Override public java.io.FileInputStream openFileInput(String name) throws java.io.FileNotFoundException {
            return new java.io.FileInputStream(getFileStreamPath(name));
        }
        @Override public java.io.FileOutputStream openFileOutput(String name, int mode) throws java.io.FileNotFoundException {
            return new java.io.FileOutputStream(getFileStreamPath(name), (mode & MODE_APPEND) != 0);
        }
        @Override public boolean deleteFile(String name) { return getFileStreamPath(name).delete(); }
        @Override public String[] fileList() {
            String[] l = getFilesDir().list();
            return l != null ? l : new String[0];
        }
        // No real external storage; map it to a subtree of the data root so apps that download
        // to getExternalFilesDir (e.g. F-Droid's APK cache) get a writable, persistent dir.
        @Override public java.io.File getExternalFilesDir(String type) {
            java.io.File d = new java.io.File(dataRoot(), "external/files");
            if (type != null && !type.isEmpty()) d = new java.io.File(d, type);
            return ensure(d);
        }
        @Override public java.io.File[] getExternalFilesDirs(String type) {
            return new java.io.File[] { getExternalFilesDir(type) };
        }
        @Override public java.io.File getExternalCacheDir() {
            return ensure(new java.io.File(dataRoot(), "external/cache"));
        }
        @Override public java.io.File[] getExternalCacheDirs() {
            return new java.io.File[] { getExternalCacheDir() };
        }
        // Room/SQLite (androidx WorkManager's WorkDatabase) back onto the host libsqlite under
        // the XDG data root's databases/ dir.
        @Override public java.io.File getDatabasePath(String name) {
            // Absolute names (Room sometimes passes a full path) are used as-is.
            if (name != null && name.startsWith("/")) return new java.io.File(name);
            return new java.io.File(ensure(new java.io.File(dataRoot(), "databases")), name);
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
        // Non-null ActivityInfo (no parent activity) so androidx NavUtils.getParentActivityName
        // and similar reads don't NPE on a null result.
        @Override public android.content.pm.ActivityInfo getActivityInfo(
                android.content.ComponentName component, int flags) {
            android.content.pm.ActivityInfo ai = new android.content.pm.ActivityInfo();
            ai.packageName = component != null ? component.getPackageName() : pkg;
            ai.name = component != null ? component.getClassName() : null;
            ai.applicationInfo = getApplicationInfo(pkg, 0);
            return ai;
        }
        @Override public android.content.pm.ApplicationInfo getApplicationInfo(String p, int flags) {
            android.content.pm.ApplicationInfo ai = new android.content.pm.ApplicationInfo();
            ai.packageName = p;
            ai.processName = p;
            return ai;
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
        // Construct the app's Application, falling back to a bare Application if the declared
        // class can't be instantiated — so getApplication() is ALWAYS a real object.
        android.app.Application app = null;
        if (appClass != null && !appClass.isEmpty()) {
            try {
                app = (android.app.Application) Class.forName(appClass).getConstructor().newInstance();
            } catch (Throwable t) {
                System.err.println("acl HostBootstrap: Application " + appClass
                        + " construct failed (" + t + "); using a bare Application");
            }
        }
        if (app == null) app = new android.app.Application();
        setField(ContextWrapper.class, app, "mBase", base);
        if (base instanceof AppContext) ((AppContext) base).setApplication(app);
        seedActivityThread(pkg);
        // ActivityThread.currentApplication() (read by e.g. AutofillManager) → mInitialApplication.
        try {
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object cur = at.getMethod("currentActivityThread").invoke(null);
            if (cur != null) setField(at, cur, "mInitialApplication", app);
        } catch (Throwable ignored) {
        }
        // Run the app's own init (sets its singleton). A failure here (unsupported API, missing
        // native lib — e.g. React Native's SoLoader) must NOT discard the Application: the
        // Activity still needs a non-null getApplication(). Log and continue with the object.
        try {
            app.onCreate();
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: Application.onCreate incomplete for "
                    + (appClass == null || appClass.isEmpty() ? "(default)" : appClass)
                    + " (" + t + "); continuing with the Application attached");
        }
        return app;  // never null → Activity.getApplication() works even if app init failed
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
            // Unsafe.allocateInstance skips field initializers, so ActivityThread's final locks/
            // singletons are null. Seed the ones View/resource code reaches: mCoreSettingsLock
            // (ViewConfiguration.getLongPressTimeout → AppGlobals.getIntCoreSetting synchronizes
            // on it — a null lock is "synchronize on null" and takes down Spinner/ForwardingListener
            // inflation) and mResourcesManager (the real singleton).
            try { setField(at, atInst, "mCoreSettingsLock", new Object()); } catch (Throwable ignored) {}
            try {
                Class<?> rm = Class.forName("android.content.res.ResourcesManager");
                setField(at, atInst, "mResourcesManager", rm.getMethod("getInstance").invoke(null));
            } catch (Throwable ignored) {}
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
        // Real Activity.attach() applies the activity/app theme from ActivityInfo.getThemeResource()
        // before onCreate; layout inflation (obtainStyledAttributes) then resolves framework + app
        // attributes off it. Without it the Activity's ContextThemeWrapper theme is bare (Themes=[])
        // → InflateException "Failed to resolve attribute". Apply the manifest app theme, or a
        // framework default (DeviceDefault has a full attribute set) when the app declares none.
        int themeRes = defaultAppTheme();
        android.content.pm.ActivityInfo ai = new android.content.pm.ActivityInfo();
        ai.theme = themeRes;
        // A valid ApplicationInfo (ComponentInfo.getIconResource reads applicationInfo.icon; the
        // ActionBar/task-description path NPEs on a null one). Reuse the app Context's.
        try {
            android.content.pm.ApplicationInfo appInfo = ctx.getApplicationInfo();
            if (appInfo == null) { appInfo = new android.content.pm.ApplicationInfo(); appInfo.packageName = ctx.getPackageName(); }
            ai.applicationInfo = appInfo;
            ai.packageName = appInfo.packageName;
        } catch (Throwable ignored) {}
        setField(android.app.Activity.class, a, "mActivityInfo", ai);
        // Set the theme on the Activity's ContextThemeWrapper (mBase is already attached), forcing
        // a re-resolve, then log what actually landed on the theme stack.
        try {
            Class<?> ctw = Class.forName("android.view.ContextThemeWrapper");
            setField(ctw, a, "mThemeResource", themeRes);
            try { setField(ctw, a, "mTheme", null); } catch (Throwable ignored) {}
            android.content.res.Resources.Theme t = a.getTheme();  // applies themeRes now
            System.err.println("acl HostBootstrap: activity theme -> 0x"
                    + Integer.toHexString(themeRes) + " => " + t);
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: activity theme apply failed: " + t);
        }
        // The launch Intent — getIntent() is read in onCreate (a null mIntent NPEs).
        try {
            setField(android.app.Activity.class, a, "mIntent",
                     new android.content.Intent(android.content.Intent.ACTION_MAIN));
        } catch (Throwable ignored) {
        }
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
            // Wire a real WindowManager onto the Activity's window (real Activity.attach() does
            // mWindow.setWindowManager(...); mWindowManager = mWindow.getWindowManager()). Without
            // it, Activity.getSystemService(WINDOW_SERVICE) returns null and any Dialog/PopupWindow/
            // Toast NPEs in Window.setWindowManager. Seed WindowManagerGlobal's no-Binder session
            // so the eventual addView -> ViewRootImpl.setView path has a session.
            seedWindowManagerGlobal();
            Object wmImpl = Class.forName("android.view.WindowManagerImpl")
                    .getConstructor(android.content.Context.class).newInstance(ctx);
            java.lang.reflect.Method setWm = android.view.Window.class.getMethod(
                    "setWindowManager", android.view.WindowManager.class,
                    android.os.IBinder.class, String.class);
            setWm.invoke(window, wmImpl, null, a.getClass().getName());
            setField(android.app.Activity.class, a, "mWindowManager",
                    ((android.view.Window) window).getWindowManager());
        } catch (Throwable t) {
            Throwable cause = (t instanceof java.lang.reflect.InvocationTargetException
                    && t.getCause() != null) ? t.getCause() : t;
            System.err.println("acl HostBootstrap: PhoneWindow deferred: " + cause);
        }
        // Attach the (framework) FragmentController host — real Activity.attach() calls
        // mFragments.attachHost(null). Activity.onCreate → mFragments.dispatchCreate() throws
        // "No activity" otherwise (the base Activity drives it even for androidx apps).
        try {
            java.lang.reflect.Field mf = android.app.Activity.class.getDeclaredField("mFragments");
            mf.setAccessible(true);
            Object fragments = mf.get(a);
            if (fragments != null) {
                fragments.getClass()
                        .getMethod("attachHost", Class.forName("android.app.Fragment"))
                        .invoke(fragments, (Object) null);
            }
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: fragment host attach deferred: " + t);
        }
    }

    // Attach the decor view tree to a window before the measure/layout/draw pass — real
    // Android does this via ViewRootImpl.setView(). We construct a ViewRootImpl (it builds a
    // valid View.AttachInfo with a Handler/Choreographer/Display) and dispatch attach directly,
    // skipping setView's Binder-heavy relayout. dispatchAttachedToWindow fires onAttachedToWindow
    // across the tree — needed by e.g. Jetpack ComposeView (it can't locate its window recomposer
    // until attached).
    public static void attachDecorToWindow(android.view.View decor) {
        if (decor == null) return;
        try {
            android.content.Context ctx = decor.getContext();
            Class<?> dmgCls = Class.forName("android.hardware.display.DisplayManagerGlobal");
            Object dmg = dmgCls.getMethod("getInstance").invoke(null);
            // Disable DisplayManagerGlobal's display-info PropertyInvalidatedCache: its nonce
            // lookup goes through a SystemProperties path that faults on this host, and any
            // Display.getRefreshRate/updateDisplayInfoLocked (e.g. RecyclerView.onAttachedToWindow)
            // would re-query through it. With the cache off, getDisplayInfo falls through to the
            // no-Binder IDisplayManager (→ null) and the Display keeps the DisplayInfo we set here.
            try { setField(dmgCls, dmg, "mDisplayCache", null); } catch (Throwable ignored) {}
            Class<?> diCls = Class.forName("android.view.DisplayInfo");
            Object di = buildDisplayInfo();  // Wayland output metrics + 60Hz mode
            Class<?> daCls = Class.forName("android.view.DisplayAdjustments");
            Object da = daCls.getConstructor().newInstance();
            android.view.Display display = (android.view.Display) android.view.Display.class
                    .getConstructor(dmgCls, int.class, diCls, daCls)
                    .newInstance(dmg, 0, di, da);
            // Build a View.AttachInfo directly rather than via ViewRootImpl (whose ctor pulls in
            // SurfaceControl + Binder relayout). No-Binder IWindowSession/IWindow + Callbacks; a
            // hollow ViewRootImpl (Unsafe-allocated, no ctor) satisfies the field without the
            // heavy setup. Enough for dispatchAttachedToWindow + a one-shot measure/layout/draw.
            Object session = noop(Class.forName("android.view.IWindowSession"));
            Object window = noop(Class.forName("android.view.IWindow"));
            Object callbacks = noop(Class.forName("android.view.View$AttachInfo$Callbacks"));
            Object vri = allocateHollow(Class.forName("android.view.ViewRootImpl"));
            // Seed the hollow ViewRootImpl fields the DecorView's attach touches: the window-
            // callback list; the running thread (checkThread); and pretend a traversal is already
            // scheduled so requestFitSystemWindows()'s scheduleTraversals() early-returns instead
            // of driving the real Choreographer/relayout path (acl does its own measure/draw).
            Class<?> vriCls = Class.forName("android.view.ViewRootImpl");
            try { setField(vriCls, vri, "mWindowCallbacks", new java.util.ArrayList<>()); } catch (Throwable ignored) {}
            try { setField(vriCls, vri, "mThread", Thread.currentThread()); } catch (Throwable ignored) {}
            try { setField(vriCls, vri, "mTraversalScheduled", Boolean.TRUE); } catch (Throwable ignored) {}
            android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());
            Class<?> aiCls = Class.forName("android.view.View$AttachInfo");
            java.lang.reflect.Constructor<?> aiCtor = aiCls.getDeclaredConstructor(
                    Class.forName("android.view.IWindowSession"),
                    Class.forName("android.view.IWindow"),
                    android.view.Display.class,
                    Class.forName("android.view.ViewRootImpl"),
                    android.os.Handler.class,
                    Class.forName("android.view.View$AttachInfo$Callbacks"),
                    android.content.Context.class);
            aiCtor.setAccessible(true);
            Object attachInfo = aiCtor.newInstance(session, window, display, vri, handler, callbacks, ctx);
            // The DecorView's attach calls back into mViewRootImpl.setOnContentApplyWindowInsets
            // Listener etc., which read the ViewRootImpl's own mAttachInfo — link them.
            try {
                setField(Class.forName("android.view.ViewRootImpl"), vri, "mAttachInfo", attachInfo);
            } catch (Throwable ignored) {
            }
            java.lang.reflect.Method dispatch = android.view.View.class.getDeclaredMethod(
                    "dispatchAttachedToWindow", aiCls, int.class);
            dispatch.setAccessible(true);
            dispatch.invoke(decor, attachInfo, 0 /* View.VISIBLE */);
            System.out.println("acl HostBootstrap: decor attached to window");
        } catch (Throwable t) {
            Throwable c = (t instanceof java.lang.reflect.InvocationTargetException
                    && t.getCause() != null) ? t.getCause() : t;
            System.err.println("acl HostBootstrap: attachDecorToWindow deferred: " + c);
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

    // IAudioService translated to the host PipeWire master volume (svc/audio → default sink):
    // stream get/set/adjust/mute route to AudioBridge; everything else defaults. The stream type
    // (arg 0) is passed through but currently maps to the one master output.
    private static Object makeAudioHandler(Class<?> iface) {
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> {
                    try {
                        switch (method.getName()) {
                            case "getStreamVolume":    return AudioBridge.getVolume((Integer) a[0]);
                            case "getStreamMaxVolume": return AudioBridge.maxVolume((Integer) a[0]);
                            case "getStreamMinVolume": return AudioBridge.minVolume((Integer) a[0]);
                            case "isStreamMute":       return AudioBridge.isMuted((Integer) a[0]);
                            case "setStreamVolume":    // (int stream, int index, int flags, String pkg)
                                AudioBridge.setVolume((Integer) a[0], (Integer) a[1]);
                                return null;
                            case "adjustStreamVolume": // (int stream, int direction, int flags, String pkg)
                                AudioBridge.adjust((Integer) a[0], (Integer) a[1]);
                                return null;
                            default:
                                return defaultValue(method.getReturnType());
                        }
                    } catch (Throwable t) {
                        // An unresolvable return type or a bad arg must not propagate out of the
                        // IAudioService proxy (the real framework only catches RemoteException).
                        android.util.Log.w("acl.audio", "IAudioService." + method.getName()
                                + " defaulted: " + t.getClass().getName());
                        return defaultForReturn(method);
                    }
                });
    }

    // IStorageManager translated to the app's StorageVolume list (svc/storage): getVolumeList →
    // the primary emulated volume (app external root) + host UDisks2 removable media; everything
    // else defaults. getVolumeList's signature is (int userId, String pkg, int flags) across the
    // levels we target, but callers vary — build the list off the current app context regardless
    // of args. Without this, StorageManager.mStorageManager stays null and getExternalStorageState
    // NPEs (a2dp.Vol).
    private static Object makeStorageHandler(Class<?> iface) {
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> {
                    final String mn = method.getName();
                    if ("getVolumeList".equals(mn)) {
                        android.content.Context ctx = sAppContext;
                        return ctx != null ? StorageBridge.getVolumeList(ctx)
                                : java.lang.reflect.Array.newInstance(
                                        Class.forName("android.os.storage.StorageVolume"), 0);
                    }
                    return defaultValue(method.getReturnType());
                });
    }

    // Seed WindowManagerGlobal.sWindowSession with a no-Binder IWindowSession so the real
    // WindowManagerGlobal.addView -> ViewRootImpl.setView path (Dialog/PopupWindow/Toast) does
    // not NPE on getWindowManagerService().openSession() (our IWindowManager returns null).
    private static boolean sWmgSeeded;
    static synchronized void seedWindowManagerGlobal() {
        if (sWmgSeeded) return;
        sWmgSeeded = true;
        try {
            Class<?> wmg = Class.forName("android.view.WindowManagerGlobal");
            Object session = noop(Class.forName("android.view.IWindowSession"));
            setStaticField(wmg, "sWindowSession", session);
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: seedWindowManagerGlobal failed: " + t);
        }
    }

    // A real android.view.Display carrying acl's Wayland output metrics (a DisplayManagerGlobal
    // Display with the buildDisplayInfo() DisplayInfo). Needed by WindowManagerImpl.addView and
    // any getDisplay()/getDisplayNoVerify() on the null-base app context.
    static android.view.Display buildDisplay() {
        try {
            Class<?> dmgCls = Class.forName("android.hardware.display.DisplayManagerGlobal");
            Object dmg = dmgCls.getMethod("getInstance").invoke(null);
            try { setField(dmgCls, dmg, "mDisplayCache", null); } catch (Throwable ignored) {}
            Class<?> diCls = Class.forName("android.view.DisplayInfo");
            Object di = buildDisplayInfo();
            Class<?> daCls = Class.forName("android.view.DisplayAdjustments");
            Object da = daCls.getConstructor().newInstance();
            return (android.view.Display) android.view.Display.class
                    .getConstructor(dmgCls, int.class, diCls, daCls)
                    .newInstance(dmg, 0, di, da);
        } catch (Throwable t) {
            return null;
        }
    }

    private static void setStaticField(Class<?> cls, String name, Object val) {
        try {
            java.lang.reflect.Field f = cls.getDeclaredField(name);
            f.setAccessible(true);
            f.set(null, val);
        } catch (Throwable ignored) {
        }
    }

    // A DisplayInfo carrying acl's output metrics (Wayland output; 60Hz mode). Used both to
    // build the attach Display and as the IDisplayManager.getDisplayInfo result so every
    // Display object (Compose's, RecyclerView's, …) is valid.
    static Object buildDisplayInfo() {
        try {
            final int dw = 1920, dh = 1080, dpi = 160;
            Class<?> diCls = Class.forName("android.view.DisplayInfo");
            Object di = diCls.getConstructor().newInstance();
            for (String[] f : new String[][]{
                    {"logicalWidth", Integer.toString(dw)}, {"logicalHeight", Integer.toString(dh)},
                    {"appWidth", Integer.toString(dw)}, {"appHeight", Integer.toString(dh)},
                    {"logicalDensityDpi", Integer.toString(dpi)}, {"rotation", "0"}, {"state", "2"}}) {
                try { setField(diCls, di, f[0], Integer.parseInt(f[1])); } catch (Throwable ignored) {}
            }
            try {
                Class<?> modeCls = Class.forName("android.view.Display$Mode");
                Object mode = modeCls.getConstructor(int.class, int.class, int.class, float.class)
                        .newInstance(1, dw, dh, 60.0f);
                Object modes = java.lang.reflect.Array.newInstance(modeCls, 1);
                java.lang.reflect.Array.set(modes, 0, mode);
                setField(diCls, di, "supportedModes", modes);
                setField(diCls, di, "modeId", 1);
                setField(diCls, di, "defaultModeId", 1);
            } catch (Throwable ignored) {}
            return di;
        } catch (Throwable t) {
            return null;
        }
    }

    // IDisplayManager translated: getDisplayInfo → a valid DisplayInfo (so Display objects have
    // metrics), getDisplayIds → {0}. Other calls default. Without this, DisplayManagerGlobal
    // hands out Displays with a null DisplayInfo and getRefreshRate() NPEs (Compose, etc.).
    private static Object makeDisplayHandler(Class<?> iface) {
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> {
                    switch (method.getName()) {
                        case "getDisplayInfo": return buildDisplayInfo();
                        case "getDisplayIds": return new int[]{0};
                        default: return defaultValue(method.getReturnType());
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

    // Safe default when a proxied method's return type can't even be resolved (getReturnType()
    // may throw NoClassDefFoundError if the return class references a missing type). An
    // unresolvable return type is always a reference type (primitives always load) → null.
    private static Object defaultForReturn(java.lang.reflect.Method method) {
        try { return defaultValue(method.getReturnType()); }
        catch (Throwable t) { return null; }
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
        svc.put("display", "android.hardware.display.IDisplayManager");
        // ViewRootImpl.<init> resolves the IME service; a no-Binder proxy suffices (no IME host).
        svc.put("input_method", "com.android.internal.view.IInputMethodManager");
        svc.put("accessibility", "android.view.accessibility.IAccessibilityManager");
        svc.put("clipboard", "android.content.IClipboard");
        // InputManager.getInstance() resolves ServiceManager.getService("input") during the first
        // traversal (input-device / ViewConfiguration queries); a no-Binder proxy suffices.
        svc.put("input", "android.hardware.input.IInputManager");
        // AudioManager routes stream volume through IAudioService; acl translates it to the host
        // PipeWire master volume (svc/audio → default sink) instead of a no-op.
        svc.put("audio", "android.media.IAudioService");
        // StorageManager / Environment.getExternalStorageState resolve ServiceManager.getService
        // ("mount") → IStorageManager; acl translates getVolumeList to the app's primary emulated
        // volume + host UDisks2 removable media (svc/storage) instead of leaving it null (NPE).
        svc.put("mount", "android.os.storage.IStorageManager");
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
                                ? makeNotificationHandler(iface)
                                : "display".equals(name) ? makeDisplayHandler(iface)
                                : "audio".equals(name) ? makeAudioHandler(iface)
                                : "mount".equals(name) ? makeStorageHandler(iface) : noop(iface);
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
