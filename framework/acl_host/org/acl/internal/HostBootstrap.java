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
        // Report a normal app uid from Process.myUid() (= Os.getuid()) before any framework code
        // runs. On acl the app runs as the host user; a uid-1000 user collides with SYSTEM_UID, so
        // WebViewFactory and other myUid()==SYSTEM_UID gates misfire. See AclOs.
        AclOs.install();
        try {
            if (android.os.Looper.myLooper() == null) android.os.Looper.prepareMainLooper();
        } catch (Throwable ignored) {
        }
        try {
            seedNoBinderServiceManager();
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: ServiceManager seed failed: " + t);
        }
        seedTelephonyFrameworkInitializer();
        seedSqliteDefaults();
        seedContentService();
        seedVirtualInputDevice();
    }

    // ── WebView (Stage 1): install a vendored Chromium WebView provider ───────────────────────────
    // acl has no WebViewUpdateService/Binder, so the normal WebViewFactory.getProvider() path (ask the
    // update service which package to use -> ClassLoader over its APK -> load native -> Class.forName
    // the factory -> create) cannot run. Instead, when ACL_WEBVIEW_APK points at a Chromium WebView APK
    // (e.g. AOSP external/chromium-webview/prebuilt/x86_64/webview.apk), build the provider here and set
    // WebViewFactory.sProviderInstance directly — getProvider() then returns it and never touches the
    // update service. The 96MB libwebviewchromium.so loads through acl's bionic linker (ACL_BIONIC_LINKER).
    // Opt-in (unset -> no-op), and eager: an app that wants WebView pays the load; others are untouched.
    private static boolean sWebViewInstalled;
    static android.content.pm.PackageInfo sWebViewPackageInfo;  // the vendored WebView pkg (AppPackageManager)
    static synchronized void installWebViewProvider() {
        if (sWebViewInstalled) return;
        final String apkPath = System.getenv("ACL_WEBVIEW_APK");
        if (apkPath == null || apkPath.isEmpty()) return;  // not configured
        sWebViewInstalled = true;
        try {
            java.io.File apk = new java.io.File(apkPath);
            if (!apk.exists()) {
                System.err.println("acl WebView: ACL_WEBVIEW_APK not found: " + apkPath);
                return;
            }
            // Host is x86_64; match the APK's lib/<abi>/.
            final String abi = "x86_64";
            java.io.File libDir = new java.io.File(System.getProperty("java.io.tmpdir"), "acl-webview-lib");
            libDir.mkdirs();
            // Extract the native engine (+ chromium linker/crashpad, best-effort) next to it.
            java.io.File soFile = null;
            try (java.util.zip.ZipFile zf = new java.util.zip.ZipFile(apk)) {
                for (String so : new String[]{"libwebviewchromium.so", "libchromium_android_linker.so",
                                              "libcrashpad_handler_trampoline.so"}) {
                    java.util.zip.ZipEntry e = zf.getEntry("lib/" + abi + "/" + so);
                    if (e == null) continue;
                    java.io.File out = new java.io.File(libDir, so);
                    if (!out.exists() || out.length() != e.getSize()) {
                        try (java.io.InputStream in = zf.getInputStream(e);
                             java.io.OutputStream os = new java.io.FileOutputStream(out)) {
                            byte[] buf = new byte[1 << 16]; int n;
                            while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                        }
                    }
                    if (so.equals("libwebviewchromium.so")) soFile = out;
                }
            }
            if (soFile == null) {
                System.err.println("acl WebView: lib/" + abi + "/libwebviewchromium.so not in APK");
                return;
            }
            // ClassLoader over the WebView APK's classes.dex; parent = the framework (boot) loader so
            // android.webkit.* resolves. Native search path = the extracted lib dir.
            ClassLoader parent = android.webkit.WebView.class.getClassLoader();
            // Native search path: the extracted engine dir + acl's bionic sysroot (where
            // stage-bionic-runtime.sh builds libwebviewchromium_plat_support.so, the draw-functor glue
            // the provider loads via System.loadLibrary).
            String sysroot = System.getenv("ACL_BIONIC_SYSROOT");
            String libPath = libDir.getAbsolutePath() + (sysroot != null ? ":" + sysroot : "");
            dalvik.system.DexClassLoader cl = new dalvik.system.DexClassLoader(
                    apkPath, libPath, libPath, parent);
            // Resolve the factory class WITHOUT running its <clinit> (loadClass, not Class.forName), so
            // we can load the native engine first.
            Class<?> factoryCls = cl.loadClass("com.android.webview.chromium.WebViewChromiumFactoryProviderForT");
            // Chromium flags come via ACL_WEBVIEW_ARGS: HostBootstrap marks the app debuggable (see
            // getApplicationInfo) so WebView's CommandLineUtil reads /data/local/tmp/webview-command-line,
            // which run-apk binds with those flags. (Initing org.chromium.base.CommandLine directly is not
            // possible — it is obfuscated in the release APK.)
            // Load the native engine ASSOCIATED WITH the WebView classloader. Runtime.load0(fromClass,
            // path) -> nativeLoad(path, fromClass.getClassLoader(), null), so acl's bionic-load hook sets
            // the FindClass loader to the WebView loader — Chromium's JNI_OnLoad resolves org.chromium.*
            // (a plain System.load would bind the lib to HostBootstrap's loader and JNI_OnLoad CHECK-fails).
            System.err.println("acl WebView: loading " + soFile.getAbsolutePath() + " (" + soFile.length() + "B)");
            java.lang.reflect.Method load0 = Runtime.class.getDeclaredMethod("load0", Class.class, String.class);
            load0.setAccessible(true);
            load0.invoke(Runtime.getRuntime(), factoryCls, soFile.getAbsolutePath());
            // The provider ctor reads WebViewFactory.getLoadedPackageInfo() (= sPackageInfo), which the
            // bypassed getWebViewContextAndSetProvider() would have set. Build one for the WebView APK.
            android.content.pm.PackageInfo pi = new android.content.pm.PackageInfo();
            pi.packageName = "com.android.webview";
            pi.versionName = "120.0.0.0";
            try { pi.setLongVersionCode(541412306L); } catch (Throwable ignored) { pi.versionCode = 541412306; }
            android.content.pm.ApplicationInfo ai = new android.content.pm.ApplicationInfo();
            ai.packageName = "com.android.webview";
            // AwBrowserProcess reads className.toLowerCase().contains("monochrome") to pick the WebView
            // variant; the standalone SystemWebView's Application class (no "monochrome") => standalone.
            ai.className = "org.chromium.android_webview.nonembedded.WebViewApkApplication";
            ai.sourceDir = apkPath;
            ai.publicSourceDir = apkPath;
            ai.nativeLibraryDir = libDir.getAbsolutePath();
            ai.metaData = new android.os.Bundle();
            ai.metaData.putString("com.android.webview.WebViewLibrary", "libwebviewchromium.so");
            pi.applicationInfo = ai;
            sWebViewPackageInfo = pi;  // so AppPackageManager can resolve "com.android.webview"
            Class<?> wvf0 = Class.forName("android.webkit.WebViewFactory");
            setStaticField(wvf0, "sPackageInfo", pi);
            // Add the WebView APK to the app's AssetManager as a shared library so it gets a resource
            // package id — WebViewDelegate.getPackageId("com.android.webview") reads that (chromium needs
            // its own resources). getProviderClass() normally does this; we bypassed it.
            if (sApplication != null) {
                try {
                    android.content.res.AssetManager am = sApplication.getAssets();
                    java.lang.reflect.Method addLib = android.content.res.AssetManager.class
                            .getMethod("addAssetPathAsSharedLibrary", String.class);
                    Object cookie = addLib.invoke(am, apkPath);
                    System.err.println("acl WebView: added WebView APK to app assets (cookie=" + cookie + ")");
                } catch (Throwable t) {
                    System.err.println("acl WebView: addAssetPathAsSharedLibrary failed: " + t);
                }
            }
            // WebViewDelegate has a package-private no-arg ctor.
            java.lang.reflect.Constructor<?> dctor =
                    Class.forName("android.webkit.WebViewDelegate").getDeclaredConstructor();
            dctor.setAccessible(true);
            Object delegate = dctor.newInstance();
            // WebViewChromiumFactoryProviderForT.create(WebViewDelegate) -> the provider instance.
            java.lang.reflect.Method create = factoryCls.getMethod(
                    "create", Class.forName("android.webkit.WebViewDelegate"));
            Object provider = create.invoke(null, delegate);
            // Publish it so WebViewFactory.getProvider() returns it (skips the update-service path).
            Class<?> wvf = Class.forName("android.webkit.WebViewFactory");
            setStaticField(wvf, "sProviderInstance", provider);
            System.err.println("acl WebView: provider installed (" + factoryCls.getName() + ")");
        } catch (Throwable t) {
            System.err.println("acl WebView: provider install failed: " + t);
            t.printStackTrace();
        }
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
        AppContext c = new AppContext(appResources(apkPath, frameworkResPath), packageName, apkPath);
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

    // acl's advertised display — the single source of truth. jvm_vm pushes it from svc::window()
    // (the wl_output mode + density) via setDisplay() before any Context/Resources is built, so
    // android.view.Display (buildDisplayInfo), the Resources Configuration (appConfiguration), and the
    // content-view measure all agree on one size. Defaults cover headless/test runs (no compositor).
    static volatile int DISPLAY_W = 1920, DISPLAY_H = 1080, DISPLAY_DPI = 160;
    public static void setDisplay(int w, int h, int densityDpi) {
        if (w > 0) DISPLAY_W = w;
        if (h > 0) DISPLAY_H = h;
        if (densityDpi > 0) DISPLAY_DPI = densityDpi;
    }

    // Configuration.uiMode night bits, from the desktop's color-scheme preference (jvm_vm pushes the
    // XDG Settings-portal value via setColorScheme before any Resources is built). 0 = leave unset
    // (portal unavailable → keep the light default); AppCompat FOLLOW_SYSTEM / apps read this to pick
    // the day/night theme.
    static volatile int sNightMode = 0;  // 0 unset, else UI_MODE_NIGHT_YES / _NO
    public static void setColorScheme(int scheme) {
        // XDG color-scheme: 1 = prefer dark, 2 = prefer light. Only an explicit desktop preference
        // overrides uiMode; 0 (no preference) / -1 (portal unavailable) leave it unset so the app's
        // own theme default applies (which is light, as before).
        if (scheme == 1) sNightMode = android.content.res.Configuration.UI_MODE_NIGHT_YES;
        else if (scheme == 2) sNightMode = android.content.res.Configuration.UI_MODE_NIGHT_NO;
    }

    private static Resources appResources(String apkPath, String frameworkResPath) {
        try {
            android.content.res.AssetManager am =
                    android.content.res.AssetManager.class.getDeclaredConstructor().newInstance();
            java.lang.reflect.Method add =
                    android.content.res.AssetManager.class.getMethod("addAssetPath", String.class);
            if (frameworkResPath != null) add.invoke(am, frameworkResPath);  // android.R.*
            if (apkPath != null) add.invoke(am, apkPath);                    // the app's arsc
            // A Configuration consistent with the advertised display, NOT the bare
            // Resources.getSystem() one (orientation=UNDEFINED, screen dims 0). A config-qualified
            // resource with no default variant — e.g. a layout that exists only as -port/-land/-sw600dp
            // (foss4g2024.schedule's main_layout) — matches nothing under an undefined config, so
            // getLayout() throws Resources$NotFoundException. A real orientation + screen size + density
            // lets the selector pick a variant.
            return new Resources(am, appMetrics(), appConfiguration());
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: app resources failed: " + t);
            return Resources.getSystem();
        }
    }

    private static android.util.DisplayMetrics appMetrics() {
        android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
        dm.setToDefaults();
        dm.widthPixels = DISPLAY_W;   dm.heightPixels = DISPLAY_H;
        dm.densityDpi = DISPLAY_DPI;  dm.density = DISPLAY_DPI / 160f;
        dm.scaledDensity = dm.density;
        dm.xdpi = DISPLAY_DPI;        dm.ydpi = DISPLAY_DPI;
        return dm;
    }

    private static android.content.res.Configuration appConfiguration() {
        android.content.res.Configuration cfg = new android.content.res.Configuration();
        cfg.setToDefaults();
        cfg.densityDpi = DISPLAY_DPI;
        float density = DISPLAY_DPI / 160f;
        int wdp = Math.round(DISPLAY_W / density), hdp = Math.round(DISPLAY_H / density);
        cfg.screenWidthDp = wdp;
        cfg.screenHeightDp = hdp;
        cfg.smallestScreenWidthDp = Math.min(wdp, hdp);
        cfg.orientation = DISPLAY_W >= DISPLAY_H
                ? android.content.res.Configuration.ORIENTATION_LANDSCAPE
                : android.content.res.Configuration.ORIENTATION_PORTRAIT;
        int sw = cfg.smallestScreenWidthDp;
        int size = sw >= 720 ? android.content.res.Configuration.SCREENLAYOUT_SIZE_XLARGE
                 : sw >= 480 ? android.content.res.Configuration.SCREENLAYOUT_SIZE_LARGE
                 : sw >= 320 ? android.content.res.Configuration.SCREENLAYOUT_SIZE_NORMAL
                 : android.content.res.Configuration.SCREENLAYOUT_SIZE_SMALL;
        cfg.screenLayout = (cfg.screenLayout & ~android.content.res.Configuration.SCREENLAYOUT_SIZE_MASK)
                | size | android.content.res.Configuration.SCREENLAYOUT_LONG_NO;
        // Day/night from the desktop color-scheme (setColorScheme); left unset → light default.
        if (sNightMode != 0) {
            cfg.uiMode = (cfg.uiMode & ~android.content.res.Configuration.UI_MODE_NIGHT_MASK) | sNightMode;
        }
        return cfg;
    }

    // The process Application, set once createApplication builds it. AppContext.getApplicationContext()
    // falls back to this when its own instance wasn't wired (the Activity's separate AppContext).
    static volatile android.app.Application sApplication;

    static final class AppContext extends ContextWrapper {
        private static AclSensorManager sSensorManager;  // one per app (native events dispatch to it)
        private final Resources res;
        private final String pkg;
        private final String apkPath;   // the app's APK (ApplicationInfo.sourceDir); may be null
        private Resources.Theme theme;
        private android.app.Application application;  // set once createApplication wraps this base
        AppContext(Resources res, String pkg, String apkPath) {
            super(null); this.res = res; this.pkg = pkg; this.apkPath = apkPath;
        }
        void setApplication(android.app.Application a) { this.application = a; }
        @Override public Resources getResources() { return res; }
        // A null-base ContextWrapper's getAutofillClient() delegates to mBase -> NPE. acl has no
        // autofill; AutofillManager.getClient() tolerates a null client (tryAddServiceClientIfNeeded
        // returns false). A Compose focus-exit (AutofillManager.notifyViewExited) hits this and, with
        // the composition aborted, blanks the content — seen as app.sterna's white screen.
        @Override public android.view.autofill.AutofillManager.AutofillClient getAutofillClient() {
            return null;
        }
        // Another null-base ContextWrapper delegate: createPackageContext NPEs on mBase. acl runs one
        // app in-process; return self (own-package resources/classloader) as a best effort — enough for
        // e.g. audio_service (agersant.polaris) building a context for its notification/PendingIntent.
        @Override public android.content.Context createPackageContext(String packageName, int flags) {
            return this;
        }
        // A null-base ContextWrapper's getActivityToken() NPEs on mBase. Flutter's WindowManagerImpl.
        // getMaximumWindowMetrics -> computeWindowInsets -> Context.getToken -> getActivityToken hits
        // this in the FlutterEngine ctor. acl has no activity token (no WindowManagerService); the
        // insets path tolerates a null token.
        @Override public android.os.IBinder getActivityToken() {
            return null;
        }
        // A null-base ContextWrapper throws from register/unregisterComponentCallbacks. Compose's OWN
        // obtainImageVectorCache DisposableEffect calls registerComponentCallbacks during the initial
        // composition (via the activity's ContextThemeWrapper, which delegates here) — that throw
        // aborted the whole composition, so no later effect ran (DisposableEffect/LaunchedEffect/
        // rememberLauncherForActivityResult), the attach was interrupted, and the WindowRecomposer
        // never activated. acl has no config-change/low-memory callback bus, so accept + drop them.
        @Override public void registerComponentCallbacks(android.content.ComponentCallbacks callback) {}
        @Override public void unregisterComponentCallbacks(android.content.ComponentCallbacks callback) {}
        // Null-base ContextWrapper -> getAssets() NPEs; return the app Resources' AssetManager
        // (APK assets + framework-res). Apps read assets in Application.onCreate (e.g. pdfinspector
        // InspectorApp.getAssets()), which otherwise leaves the app uninitialized -> blank window.
        @Override public android.content.res.AssetManager getAssets() { return res.getAssets(); }
        @Override public String getPackageName() { return pkg; }
        @Override public String getOpPackageName() { return pkg; }  // ContentResolver.<init> reads it
        @Override public int getUserId() { return 0; }              // single-user (system user)
        @Override public String getAttributionTag() { return null; }
        @Override public android.content.AttributionSource getAttributionSource() {
            return new android.content.AttributionSource.Builder(0).setPackageName(pkg).build();
        }
        // Return the Application (not the base) per Android semantics: framework code keys off
        // it — e.g. WorkManager's on-demand init does getApplicationContext() instanceof
        // Configuration.Provider, which only holds if the Application flows back here. The Activity
        // gets a SEPARATE AppContext (jvm_vm's second newAppContext) that never had setApplication,
        // so fall back to the process Application (Hilt's EntryPointAccessors calls
        // getApplicationContext() then walks the result for an Application — a bare AppContext throws
        // "Could not find an Application in the given context").
        @Override public android.content.Context getApplicationContext() {
            if (application != null) return application;
            if (sApplication != null) return sApplication;
            return this;
        }
        public int getDisplayId() { return 0; }
        // A null-base ContextWrapper NPEs on getDisplay()/getDisplayNoVerify() (mBase == null);
        // WindowManagerImpl.addView calls mContext.getDisplayNoVerify(), so a Dialog/PopupWindow on
        // this context needs a real Display (Wayland output metrics).
        @Override public android.view.Display getDisplay() { return buildDisplay(); }
        public android.view.Display getDisplayNoVerify() { return buildDisplay(); }
        // Chromium's DisplayAndroidManager creates a window context for display metrics; a null-base
        // ContextWrapper NPEs. acl has one display, so return self (own resources/display).
        @Override public android.content.Context createWindowContext(int type, android.os.Bundle options) {
            return this;
        }
        @Override public android.content.Context createWindowContext(android.view.Display display,
                int type, android.os.Bundle options) {
            return this;
        }
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
            // Native-lib loaders (Flutter/ReLinker, System.loadLibrary internals) read these:
            // sourceDir = the APK to scan for lib/<abi>/*.so; nativeLibraryDir = where acl
            // extracted them (matches the run-time java.library.path). Absent -> new File(null) NPE.
            ai.sourceDir = apkPath;
            ai.publicSourceDir = apkPath;
            ai.dataDir = ensure(dataRoot()).getAbsolutePath();
            ai.nativeLibraryDir = new java.io.File(cacheRoot(), "lib").getAbsolutePath();
            ai.flags = android.content.pm.ApplicationInfo.FLAG_HAS_CODE;
            // With ACL_WEBVIEW_ARGS, mark the app debuggable so WebView's CommandLineUtil reads
            // /data/local/tmp/webview-command-line (run-apk binds it) — the only way to pass Chromium
            // flags (--disable-gpu / --enable-logging) on the obfuscated release APK.
            if (System.getenv("ACL_WEBVIEW_ARGS") != null)
                ai.flags |= android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE;
            // The manifest's <application> <meta-data> as a Bundle (PackageManager.GET_META_DATA).
            // Libraries read config from here — Expo Updates, Firebase, WorkManager — and call
            // metaData.containsKey/getBoolean on it, so a NULL metaData is an NPE before the app's
            // own code runs. Populate from AndroidManifest.xml; never leave it null.
            ai.metaData = parseManifestMetaData();
            return ai;
        }

        // Parse the <application> <meta-data name=.. value|resource=..> entries into a Bundle. A
        // meta-data value is typed (string/bool/int/float) or a resource id; put each under its
        // real type so getBoolean/getInt/getString return what the app expects. Best-effort: on any
        // parse failure return an empty (non-null) Bundle rather than null.
        private android.os.Bundle parseManifestMetaData() {
            android.os.Bundle b = new android.os.Bundle();
            android.content.res.XmlResourceParser xml = null;
            try {
                xml = getResources().getAssets().openXmlResourceParser("AndroidManifest.xml");
                boolean inApp = false;
                for (int ev = xml.getEventType();
                     ev != android.content.res.XmlResourceParser.END_DOCUMENT; ev = xml.next()) {
                    if (ev == android.content.res.XmlResourceParser.START_TAG) {
                        String tag = xml.getName();
                        if ("application".equals(tag)) {
                            inApp = true;
                        } else if (inApp && "meta-data".equals(tag)) {
                            putMetaData(b, xml);
                        }
                    } else if (ev == android.content.res.XmlResourceParser.END_TAG
                            && "application".equals(xml.getName())) {
                        inApp = false;
                    }
                }
            } catch (Throwable t) {
                System.err.println("acl HostBootstrap: meta-data parse failed (" + t + "); "
                        + "using empty metaData");
            } finally {
                if (xml != null) xml.close();
            }
            return b;
        }

        private static void putMetaData(android.os.Bundle b, android.content.res.XmlResourceParser x) {
            final String NS = "http://schemas.android.com/apk/res/android";
            String name = x.getAttributeValue(NS, "name");
            if (name == null) return;
            // android:resource wins over android:value (the framework treats it as an int res id).
            int resId = x.getAttributeResourceValue(NS, "resource", 0);
            if (resId != 0) { b.putInt(name, resId); return; }
            // XmlResourceParser exposes no typed-value kind, so find the android:value attribute and
            // recover the type from its resolved string (binary XML already resolved any ref), with
            // a boolean/int probe for a pure-typed value that has no string form.
            int vi = -1;
            for (int i = 0; i < x.getAttributeCount(); i++) {
                if ("value".equals(x.getAttributeName(i))) { vi = i; break; }
            }
            if (vi < 0) return;
            String s = x.getAttributeValue(vi);
            if (s != null && !s.isEmpty()) {
                if ("true".equalsIgnoreCase(s) || "false".equalsIgnoreCase(s)) {
                    b.putBoolean(name, Boolean.parseBoolean(s));
                    return;
                }
                try { b.putInt(name, Integer.parseInt(s.trim())); return; }
                catch (NumberFormatException ignore) {}
                try { b.putFloat(name, Float.parseFloat(s.trim())); return; }
                catch (NumberFormatException ignore) {}
                b.putString(name, s);
                return;
            }
            // No string form: getAttributeBooleanValue returns the real value for a boolean type and
            // the default otherwise, so two calls with opposite defaults agreeing means it IS a
            // boolean; otherwise treat it as an int.
            boolean t = x.getAttributeBooleanValue(vi, false);
            boolean f = x.getAttributeBooleanValue(vi, true);
            if (t == f) b.putBoolean(name, t);
            else b.putInt(name, x.getAttributeIntValue(vi, 0));
        }
        // AppContext has a null base (super(null)), so Context methods not overridden here
        // would NPE. Device-protected (direct-boot) storage isn't modeled → plain storage.
        @Override public boolean isDeviceProtectedStorage() { return false; }
        @Override public boolean isRestricted() { return false; }  // TextView.readTextAppearance
        @Override public boolean canLoadUnsafeResources() { return true; }  // same, @hide
        // The single acl user (ADR-3). WorkManager's PendingIntent.getBroadcast path calls
        // Context.getUser() on a background thread; without this it delegates to the null base.
        @Override public android.os.UserHandle getUser() { return android.os.Process.myUserHandle(); }
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
            if ("sensor".equals(name)) {
                // android.hardware.SensorManager over acl's native sensor hub (Linux IIO /
                // synthetic). Cached: one instance per app (native events dispatch to it).
                if (sSensorManager == null) sSensorManager = new AclSensorManager(this);
                return sSensorManager;
            }
            if ("input".equals(name)) {
                // InputManager.getInstance() (the process singleton; acl seeded its device cache in
                // seedVirtualInputDevice). Chromium's WebView registers an InputDeviceListener; a null
                // service NPEs. registerInputDeviceListener no-ops on acl (no hotplug bus).
                try {
                    return Class.forName("android.hardware.input.InputManager")
                            .getMethod("getInstance").invoke(null);
                } catch (Throwable t) {
                    return null;
                }
            }
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
            if ("keyguard".equals(name)) {
                // KeyguardManager(Context) (package-private, throws ServiceNotFoundException). Its
                // ctor resolves window/activity/trust/notification binders — all seeded — so it
                // builds; its queries (isKeyguardLocked/isDeviceLocked) route through the no-op
                // proxies and return false, which is correct (no lockscreen on the host).
                try {
                    java.lang.reflect.Constructor<?> c = android.app.KeyguardManager.class
                            .getDeclaredConstructor(android.content.Context.class);
                    c.setAccessible(true);
                    return c.newInstance(this);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("alarm".equals(name)) {
                // AlarmManager(IAlarmManager, Context); a no-op IAlarmManager is enough (set/cancel
                // no-op — no alarm host). EventFahrplan's AlarmServices grabs it in Fragment.onAttach.
                try {
                    Class<?> iam = Class.forName("android.app.IAlarmManager");
                    java.lang.reflect.Constructor<?> c = android.app.AlarmManager.class
                            .getDeclaredConstructor(iam, android.content.Context.class);
                    c.setAccessible(true);
                    return c.newInstance(noop(iam), this);
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
                // Dialog/PopupWindow/Toast: Window.setWindowManager casts to the final WindowManagerImpl
                // (can't subclass) + calls createLocalWindowManager, so return a real one. Its addView
                // -> WindowManagerGlobal.addView lands the decor in mViews; acl composites from there
                // (presentOverlays) and the compat-delegate seed keeps ViewRootImpl.setView from
                // crashing on device-only compat paths.
                try {
                    seedWindowManagerGlobal();
                    seedCompatCallbacks();
                    return Class.forName("android.view.WindowManagerImpl")
                            .getConstructor(android.content.Context.class).newInstance(this);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("input_method".equals(name)) {
                // No IME and no Binder (ADR-3), but ViewRootImpl/ImeFocusController dereference the
                // InputMethodManager on the Dialog/PopupWindow add + remove paths. Use the framework's
                // OWN headless path: InputMethodManager.createStubInstance() (built for layoutlib) wires
                // a Proxy-stubbed IInputMethodManager (all IPCs return false/0/null) into the real ctor,
                // so getDelegate() is non-null and IME calls no-op. Reflective: createStubInstance is
                // private static.
                try {
                    java.lang.reflect.Method m = android.view.inputmethod.InputMethodManager.class
                            .getDeclaredMethod("createStubInstance", int.class, android.os.Looper.class);
                    m.setAccessible(true);
                    return m.invoke(null, 0 /* Display.DEFAULT_DISPLAY */, android.os.Looper.getMainLooper());
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
            if ("user".equals(name)) {
                // UserManager(Context, IUserManager). Chromium's WebView reads
                // getApplicationRestrictions() (managed-config); a null service NPEs, and a default
                // (null) Bundle NPEs its caller — so back it with a proxy returning an EMPTY Bundle for
                // the *Restrictions reads and defaults otherwise. Single-user host (ADR-3).
                try {
                    Class<?> ium = Class.forName("android.os.IUserManager");
                    Object svc = java.lang.reflect.Proxy.newProxyInstance(ium.getClassLoader(),
                            new Class<?>[]{ium}, (p, m, a) -> {
                                String mn = m.getName();
                                if (mn.endsWith("Restrictions") && m.getReturnType() == android.os.Bundle.class)
                                    return new android.os.Bundle();
                                return defaultValue(m.getReturnType());
                            });
                    java.lang.reflect.Constructor<?> c = android.os.UserManager.class
                            .getDeclaredConstructor(android.content.Context.class, ium);
                    c.setAccessible(true);
                    return c.newInstance(this, svc);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("phone".equals(name)) {
                // TelephonyManager(Context): Chromium's AndroidNetworkLibrary /
                // NetworkChangeNotifierAutoDetect classify the connection via TelephonyManager
                // (getSignalStrength/getNetworkType); a null service NPEs and chromium's JNI
                // CheckException aborts the process. The real manager with no telephony binder
                // returns null/defaults (no host radio), which chromium handles.
                try {
                    return new android.telephony.TelephonyManager(this);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("batterymanager".equals(name)) {
                // BatteryManager(Context, IBatteryStats, IBatteryPropertiesRegistrar), backed by the
                // host UPower daemon via svc/battery (BatteryBridge.snapshot()). Chromium's
                // AndroidBatteryMetrics calls PowerMonitor.getRemainingBatteryCapacity() ->
                // getIntProperty(BATTERY_PROPERTY_CAPACITY); a null service NPEs and chromium's JNI
                // CheckException aborts the process. getIntProperty -> queryProperty -> the properties
                // registrar's getProperty(id, BatteryProperty) — so supply a registrar that fills the
                // property from the real UPower snapshot [level,scale,temp,mV,status,health,plugged,
                // present]. Capacity/status are answered; other BATTERY_PROPERTY_* (charge/current/
                // energy counters, which UPower's DisplayDevice doesn't expose) return non-zero so
                // BatteryManager yields the "unknown" sentinel rather than a fabricated value.
                // AclBatteryManager extends BatteryManager and answers getIntProperty from the UPower
                // snapshot; allocateHollow skips BatteryManager's ctor (which resolves batterystats/
                // batteryproperties Binder services + android.os.BatteryProperty, none on acl's boot
                // classpath — the real queryProperty path can't run here).
                try {
                    return allocateHollow(AclBatteryManager.class);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("captioning".equals(name)) {
                // CaptioningManager(Context): the WebView's onAttachedToWindow (via its media/
                // accessibility setup) registers a CaptioningChangeListener; a null service NPEs and
                // aborts dispatchAttachedToWindow — leaving the tree not-attached-to-window (which
                // forces WebView's software draw path). The real manager reads captioning Settings
                // (all defaults/off on the host) and its listener registration is inert.
                try {
                    return new android.view.accessibility.CaptioningManager(this);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("location".equals(name)) {
                // Real android.location.LocationManager with acl's bridging ILocationManager, which
                // drives the native acl::svc::location hub (host GeoClue2 over D-Bus) instead of
                // system_server's LocationManagerService. openmaps' onCreate → permissionsAPI →
                // requestLocationUpdates() NPE'd on a null service; a real, host-backed one fixes it.
                try {
                    return new android.location.LocationManager(this, AclLocationManager.newService());
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
            if (serviceClass == android.os.UserManager.class) return "user";
            if (serviceClass == android.hardware.SensorManager.class) return "sensor";
            if (serviceClass == android.location.LocationManager.class) return "location";
            if (serviceClass == android.view.accessibility.CaptioningManager.class) return "captioning";
            if (serviceClass == android.os.BatteryManager.class) return "batterymanager";
            if (serviceClass == android.telephony.TelephonyManager.class) return "phone";
            if (serviceClass == android.hardware.input.InputManager.class) return "input";
            // ViewRootImpl.<init> reads getSystemService(AudioManager.class) (navigation sound
            // effects); map it so the Dialog/PopupWindow add path gets a real AudioManager.
            if (serviceClass == android.media.AudioManager.class) return "audio";
            // ImeFocusController.getImmDelegate() does getSystemService(InputMethodManager.class)
            // .getDelegate(); a Dialog add/remove NPEs here if it returns null. Map it.
            if (serviceClass == android.view.inputmethod.InputMethodManager.class) return "input_method";
            // These are reached as getSystemService(SomeManager::class.java)!! (Kotlin non-null), so a
            // null name NPEs — EventFahrplan hits all three (onCreate/onAttach/onViewCreated). acl backs
            // each by string name below. (The real SystemServiceRegistry.getSystemServiceName is not a
            // usable fallback here — its class init is unreliable on acl's headless image and returns
            // null for these — so map them explicitly.)
            if (serviceClass == android.app.KeyguardManager.class) return "keyguard";
            if (serviceClass == android.app.AlarmManager.class) return "alarm";
            if (serviceClass == android.view.LayoutInflater.class) return "layout_inflater";
            // Flutter's FlutterEngine ctor reads getSystemService(WindowManager.class) via
            // ViewUtils.calculateMaximumDisplayMetrics; an unmapped null WindowManager NPEs the ctor
            // (half-built engine -> the raster thread then crashes). acl backs "window" (a real
            // WindowManagerImpl, line ~735), so map the class too.
            if (serviceClass == android.view.WindowManager.class) return "window";
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

    // <provider> meta-data keyed by fully-qualified class name, parsed once in
    // installContentProviders and re-served via AppPackageManager.getProviderInfo — androidx.startup's
    // InitializationProvider re-reads its declared Initializers from getProviderInfo(cn, GET_META_DATA).
    static final java.util.Map<String, android.os.Bundle> sProviderMeta =
            new java.util.HashMap<String, android.os.Bundle>();

    // Installed providers keyed by each declared authority — AppContentResolver.acquireProvider
    // resolves content:// lookups here so query/insert/openFile reach the live provider instead of
    // the no-op stub. Populated by installContentProviders after a successful attachInfo/onCreate.
    static final java.util.Map<String, android.content.ContentProvider> sProviderByAuthority =
            new java.util.HashMap<String, android.content.ContentProvider>();

    static final class AppPackageManager extends StubPackageManager {
        private final String pkg;
        AppPackageManager(String pkg) { this.pkg = pkg; }
        // An app can always query its OWN package (it is installed); other packages resolve as
        // not-installed. Returning null/throwing for the own package broke callers that read their
        // own version/appInfo on a worker (forkyz) — including the API 33 PackageInfoFlags overload,
        // which StubPackageManager leaves throwing UnsupportedOperationException ("not implemented in
        // subclass"), an uncaught crash rather than the NameNotFoundException apps guard against.
        @Override public android.content.pm.PackageInfo getPackageInfo(String p, int flags)
                throws android.content.pm.PackageManager.NameNotFoundException {
            if (p != null && p.equals(pkg)) return ownPackageInfo();
            // The vendored WebView package (chromium queries it during provider init).
            if (sWebViewPackageInfo != null && "com.android.webview".equals(p)) return sWebViewPackageInfo;
            throw new android.content.pm.PackageManager.NameNotFoundException(p);
        }
        @Override public android.content.pm.PackageInfo getPackageInfo(String p,
                android.content.pm.PackageManager.PackageInfoFlags flags)
                throws android.content.pm.PackageManager.NameNotFoundException {
            return getPackageInfo(p, 0);
        }
        private android.content.pm.PackageInfo ownPackageInfo() {
            android.content.pm.PackageInfo pi = new android.content.pm.PackageInfo();
            pi.packageName = pkg;
            pi.applicationInfo = getApplicationInfo(pkg, 0);
            pi.versionName = "1.0";
            try { pi.setLongVersionCode(1L); } catch (Throwable ignored) { pi.versionCode = 1; }
            return pi;
        }
        // A non-null ProviderInfo carrying the provider's <meta-data> (installContentProviders parsed
        // it). androidx.startup calls this on its own InitializationProvider to discover Initializers;
        // a null return (StubPackageManager default) would NPE it before any app code runs.
        @Override public android.content.pm.ProviderInfo getProviderInfo(
                android.content.ComponentName component, int flags) {
            android.content.pm.ProviderInfo pi = new android.content.pm.ProviderInfo();
            pi.packageName = component != null ? component.getPackageName() : pkg;
            pi.name = component != null ? component.getClassName() : null;
            pi.applicationInfo = getApplicationInfo(pkg, 0);
            if (pi.name != null) pi.metaData = sProviderMeta.get(pi.name);
            return pi;
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
            // Delegate to the app Context's ApplicationInfo, which carries the manifest <meta-data>
            // Bundle (plus sourceDir/nativeLibraryDir/dataDir). Callers read app meta-data through
            // PackageManager, not the Context: Flutter's ApplicationInfoLoader reads
            // io.flutter.embedding.android.EnableImpeller here — a bare info with null metaData made
            // it default to Impeller, whose GL context init fails on acl ("Bad Display" -> raster
            // SIGSEGV); acl's supported Flutter backend is Skia/GLES, which the app's manifest
            // selects via EnableImpeller=false. Firebase/WorkManager read their config the same way.
            if (sWebViewPackageInfo != null && "com.android.webview".equals(p))
                return sWebViewPackageInfo.applicationInfo;
            AppContext c = sAppContext;
            if (c != null && (p == null || p.equals(pkg))) {
                try {
                    android.content.pm.ApplicationInfo ai = c.getApplicationInfo();
                    if (ai != null) return ai;
                } catch (Throwable ignored) {
                }
            }
            android.content.pm.ApplicationInfo ai = new android.content.pm.ApplicationInfo();
            ai.packageName = p;
            ai.processName = p;
            return ai;
        }
        @Override public android.content.pm.ApplicationInfo getApplicationInfo(String p,
                android.content.pm.PackageManager.ApplicationInfoFlags flags) {
            return getApplicationInfo(p, 0);  // API 33 typed-flags overload (StubPackageManager throws)
        }
        // WebView support gate (WebViewFactory.isWebViewSupported): acl provides a vendored Chromium
        // WebView provider (installWebViewProvider), so advertise the feature when it is configured.
        @Override public boolean hasSystemFeature(String name) {
            if (android.content.pm.PackageManager.FEATURE_WEBVIEW.equals(name))
                return System.getenv("ACL_WEBVIEW_APK") != null;
            return false;
        }
        @Override public boolean hasSystemFeature(String name, int version) {
            return hasSystemFeature(name);
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
        // Route to an installed provider (its in-process Transport IContentProvider) when the
        // authority matches; otherwise the default no-op provider so unresolved lookups still
        // return null/defaults rather than NPE.
        private static android.content.IContentProvider resolve(String authority) {
            android.content.ContentProvider cp =
                    authority == null ? null : sProviderByAuthority.get(authority);
            if (cp != null) {
                try { return cp.getIContentProvider(); } catch (Throwable ignore) {}
            }
            return stubProvider();
        }
        @Override protected android.content.IContentProvider acquireProvider(
                android.content.Context c, String name) { return resolve(name); }
        @Override protected android.content.IContentProvider acquireUnstableProvider(
                android.content.Context c, String name) { return resolve(name); }
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
        sApplication = app;  // process Application — the fallback for any unwired AppContext
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
        // WebView needs the full app environment (process name from seedActivityThread above, initial
        // Application, app Context), so install the provider here — not in init() (too early: chromium's
        // AwBrowserProcess reads ActivityThread.currentProcessName() and NPEs on a null). Before
        // app.onCreate so an app using WebView in its Application still finds it.
        installWebViewProvider();
        // ActivityThread.installContentProviders runs every <provider>'s onCreate BEFORE
        // Application.onCreate — androidx.startup's InitializationProvider bootstraps WorkManager/
        // Firebase from there, so it must run first.
        installContentProviders(base, pkg);
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

    // Parse <provider> entries straight from AndroidManifest.xml (no PMS/Binder, ADR-3),
    // instantiate each via the app classloader, and attachInfo() — which runs ContentProvider.
    // onCreate(). ActivityThread does this before Application.onCreate. The per-provider
    // <meta-data> is stashed in sProviderMeta so getProviderInfo() can re-serve it (the whole
    // reason androidx.startup's InitializationProvider works). Best-effort per provider: a
    // failing onCreate is logged and skipped so one bad provider doesn't sink app startup.
    static void installContentProviders(android.content.Context base, String pkg) {
        // {className, authorities, exported, grantUriPermissions} — the last two decide whether
        // FileProvider (exported=false + grantUriPermissions=true, else it throws in attachInfo).
        java.util.List<String[]> providers = new java.util.ArrayList<String[]>();
        android.content.res.XmlResourceParser xml = null;
        try {
            xml = base.getResources().getAssets().openXmlResourceParser("AndroidManifest.xml");
            final String NS = "http://schemas.android.com/apk/res/android";
            String cur = null;
            android.os.Bundle curMeta = null;
            for (int ev = xml.getEventType();
                 ev != android.content.res.XmlResourceParser.END_DOCUMENT; ev = xml.next()) {
                if (ev == android.content.res.XmlResourceParser.START_TAG) {
                    String tag = xml.getName();
                    if ("provider".equals(tag)) {
                        String name = xml.getAttributeValue(NS, "name");
                        if (name != null) {
                            if (name.startsWith(".")) name = pkg + name;
                            else if (name.indexOf('.') < 0) name = pkg + "." + name;
                            cur = name;
                            curMeta = new android.os.Bundle();
                            providers.add(new String[]{name, xml.getAttributeValue(NS, "authorities"),
                                    Boolean.toString(xml.getAttributeBooleanValue(NS, "exported", false)),
                                    Boolean.toString(xml.getAttributeBooleanValue(NS, "grantUriPermissions", false))});
                        }
                    } else if (cur != null && "meta-data".equals(tag)) {
                        AppContext.putMetaData(curMeta, xml);
                    }
                } else if (ev == android.content.res.XmlResourceParser.END_TAG
                        && "provider".equals(xml.getName())) {
                    if (cur != null) { sProviderMeta.put(cur, curMeta); cur = null; curMeta = null; }
                }
            }
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: <provider> parse failed (" + t + "); "
                    + "no ContentProviders installed");
            return;
        } finally {
            if (xml != null) xml.close();
        }
        for (String[] p : providers) {
            String cls = p[0], auth = p[1];
            try {
                Class<?> c = Class.forName(cls, false, base.getClassLoader());
                android.content.ContentProvider cp =
                        (android.content.ContentProvider) c.getConstructor().newInstance();
                android.content.pm.ProviderInfo info = new android.content.pm.ProviderInfo();
                info.name = cls;
                info.authority = auth;
                info.packageName = pkg;
                info.applicationInfo = base.getApplicationInfo();
                info.metaData = sProviderMeta.get(cls);
                info.exported = Boolean.parseBoolean(p[2]);
                info.grantUriPermissions = Boolean.parseBoolean(p[3]);
                cp.attachInfo(base, info);  // → ContentProvider.onCreate()
                // android:authorities is a ';'-separated list; register each so the resolver routes.
                if (auth != null) {
                    for (String a : auth.split(";")) {
                        if (!a.isEmpty()) sProviderByAuthority.put(a, cp);
                    }
                }
                System.out.println("acl HostBootstrap: ContentProvider installed: " + cls
                        + (auth != null ? " (authorities=" + auth + ")" : ""));
            } catch (Throwable t) {
                System.err.println("acl HostBootstrap: ContentProvider " + cls
                        + " install failed (" + t + "); continuing");
            }
        }
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
            // Activity.recreate() (AppCompat applyDayNight, etc.) checks
            // Looper.myLooper() == mMainThread.getLooper(), then calls mMainThread.
            // scheduleRelaunchActivity(mToken), which does mActivities.get(token). Seed the main
            // Looper (getLooper() was null → "Must be called from main thread") and an EMPTY
            // mActivities so the relaunch is a clean no-op (get(null) == null). acl can't relaunch an
            // Activity, but recreate() must not throw and abort the app — forkyz's theme flow calls
            // it on the main thread and was left with a blank screen.
            try { setField(at, atInst, "mLooper", android.os.Looper.getMainLooper()); } catch (Throwable ignored) {}
            try { setField(at, atInst, "mActivities", new android.util.ArrayMap<Object, Object>()); } catch (Throwable ignored) {}
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
        seedCompatCallbacks();  // before any Dialog: keep opt-in compat changes off (crash-free)
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
        // ComponentInfo.name is the activity's own class; getComponentName()/ActivityInfo readers use it.
        String actClass = a.getClass().getName();
        ai.name = actClass;
        setField(android.app.Activity.class, a, "mActivityInfo", ai);
        // Activity.getComponentName() reads mComponent; real attach() sets it. Left null, any
        // getComponentName().getClassName() NPEs — androidx NavUtils.getParentActivityName does exactly
        // this, and so does e.g. octalsuntime's ClockActivity.onCreate.
        android.content.ComponentName cn = null;
        try {
            cn = new android.content.ComponentName(
                    ai.packageName != null ? ai.packageName : ctx.getPackageName(), actClass);
            setField(android.app.Activity.class, a, "mComponent", cn);
        } catch (Throwable ignored) {}
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
        // The launch Intent — getIntent() is read in onCreate (a null mIntent NPEs). Give it the
        // launcher component so getIntent().getComponent() also resolves (not just getComponentName()).
        try {
            android.content.Intent launch = new android.content.Intent(android.content.Intent.ACTION_MAIN)
                    .addCategory(android.content.Intent.CATEGORY_LAUNCHER);
            if (cn != null) launch.setComponent(cn);
            setField(android.app.Activity.class, a, "mIntent", launch);
        } catch (Throwable ignored) {
        }
        try {
            setField(android.app.Activity.class, a, "mUiThread", Thread.currentThread());
        } catch (Throwable ignored) {
        }
        // Activity.startActivityForResult (used by rememberLauncherForActivityResult and any
        // startActivity) reads mMainThread.getApplicationThread() then mInstrumentation.execStartActivity
        // -> the real path needs an ActivityManager (Binder) acl has no v1. Seed the hollow
        // ActivityThread (so mMainThread isn't null) and a no-op Instrumentation that suppresses the
        // launch: acl can't host external activities (file pickers, browsers), so a tap on such a
        // button becomes a safe no-op instead of crashing the app on an uncaught NPE.
        try {
            Object at = Class.forName("android.app.ActivityThread")
                    .getMethod("currentActivityThread").invoke(null);
            if (at != null) setField(android.app.Activity.class, a, "mMainThread", at);
        } catch (Throwable ignored) {}
        try {
            setField(android.app.Activity.class, a, "mInstrumentation", new NoopInstrumentation());
        } catch (Throwable ignored) {}
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
            // Real Activity.attach() (Activity.java:8242) sets the Activity as the Window inflater's
            // private factory. LayoutInflater then routes a <fragment> tag (and other tags the
            // Activity claims via onCreateView) to Activity.onCreateView -> mFragments.onCreateView.
            // Without it, inflating a layout XML containing <fragment> falls through to
            // createView("fragment","android.view.") -> ClassNotFoundException: android.view.fragment.
            // (androidx FragmentActivity overrides onCreateView to use its own FragmentController.)
            try {
                ((android.view.Window) window).getLayoutInflater()
                        .setPrivateFactory((android.view.LayoutInflater.Factory2) a);
            } catch (Throwable t) {
                System.err.println("acl HostBootstrap: setPrivateFactory deferred: " + t);
            }
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
    // Drive an androidx ComponentActivity's Lifecycle to RESUMED. The framework normally advances it
    // via Application.dispatchActivityResumed -> registered ActivityLifecycleCallbacks (ReportFragment
    // / LifecycleCallbacks), a path acl's stub Application/Instrumentation doesn't complete. Compose
    // content, LaunchedEffect, collectAsStateWithLifecycle and ViewModels gate on RESUMED, so force
    // the LifecycleRegistry there directly. Reflection-only (no compile-time androidx dep). Best effort.
    public static void ensureResumed(Object activity) {
        if (activity == null) return;
        // Robust path first: run the Activity's registered ActivityLifecycleCallbacks via the
        // framework's own dispatch methods. androidx ComponentActivity registers a ReportFragment/
        // LifecycleCallbacks there that advances its Lifecycle to RESUMED. acl calls onCreate/onStart/
        // onResume directly but never the dispatch, so the Lifecycle stays CREATED and Compose's
        // WindowRecomposer never activates → blank content. Unlike the androidx-name reflection below,
        // this works even when the app obfuscates androidx.lifecycle (forkyz renames all of it).
        dispatchLifecycleCallbacks(activity);
        try {
            java.lang.reflect.Method getLifecycle = activity.getClass().getMethod("getLifecycle");
            Object lifecycle = getLifecycle.invoke(activity);
            if (lifecycle == null) return;
            Object cur = null;
            try {
                cur = lifecycle.getClass().getMethod("getCurrentState").invoke(lifecycle);
            } catch (Throwable ignored) {}
            // LifecycleRegistry.handleLifecycleEvent(Lifecycle.Event) walks CREATED->STARTED->RESUMED,
            // firing every observer in between (what the missing framework dispatch would have done).
            Class<?> eventCls = Class.forName("androidx.lifecycle.Lifecycle$Event");
            java.lang.reflect.Method handle = null;
            for (java.lang.reflect.Method m : lifecycle.getClass().getMethods()) {
                if (m.getName().equals("handleLifecycleEvent") && m.getParameterCount() == 1) { handle = m; break; }
            }
            if (handle != null) {
                for (String ev : new String[]{"ON_CREATE", "ON_START", "ON_RESUME"}) {
                    Object e = eventCls.getField(ev).get(null);
                    try { handle.invoke(lifecycle, e); } catch (Throwable ignored) {}
                }
            }
            Object now = null;
            try { now = lifecycle.getClass().getMethod("getCurrentState").invoke(lifecycle); } catch (Throwable ignored) {}
            System.out.println("acl HostBootstrap: lifecycle " + cur + " -> " + now);
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: ensureResumed (androidx path) skipped: " + t);
        }
    }

    // Run the Activity's registered ActivityLifecycleCallbacks in lifecycle order, via the
    // framework Activity's own (private) dispatch methods. Those method names are the framework's,
    // never renamed, and they invoke the app's OWN callbacks (androidx ReportFragment, obfuscated
    // but self-consistent) — so ComponentActivity's Lifecycle advances INITIALIZED→…→RESUMED without
    // acl referencing any androidx name. Best-effort per step.
    private static void dispatchLifecycleCallbacks(Object activity) {
        final String[] noArg = {
            "dispatchActivityStarted", "dispatchActivityPostStarted",
            "dispatchActivityResumed", "dispatchActivityPostResumed",
        };
        try {
            for (String name : new String[]{"dispatchActivityCreated", "dispatchActivityPostCreated"}) {
                try {
                    java.lang.reflect.Method m = android.app.Activity.class
                            .getDeclaredMethod(name, android.os.Bundle.class);
                    m.setAccessible(true);
                    m.invoke(activity, (android.os.Bundle) null);
                } catch (Throwable ignored) {}
            }
            for (String name : noArg) {
                try {
                    java.lang.reflect.Method m = android.app.Activity.class.getDeclaredMethod(name);
                    m.setAccessible(true);
                    m.invoke(activity);
                } catch (Throwable ignored) {}
            }
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: lifecycle-callback dispatch skipped: " + t);
        }
    }

    // Instrumentation that suppresses activity launches. acl has no ActivityManager to start another
    // activity (external file picker, browser, or a second in-app Activity), so execStartActivity
    // returning null makes startActivityForResult a no-op — no result is delivered and the app
    // survives the tap instead of NPE-ing on the missing ApplicationThread/ActivityManager.
    static final class NoopInstrumentation extends android.app.Instrumentation {
        private android.app.Instrumentation.ActivityResult suppress(android.content.Intent intent) {
            System.out.println("acl HostBootstrap: startActivity suppressed (no external-activity host): " + intent);
            return null;
        }
        @Override public android.app.Instrumentation.ActivityResult execStartActivity(
                android.content.Context who, android.os.IBinder contextThread, android.os.IBinder token,
                android.app.Activity target, android.content.Intent intent, int requestCode,
                android.os.Bundle options) {
            return suppress(intent);
        }
        @Override public android.app.Instrumentation.ActivityResult execStartActivity(
                android.content.Context who, android.os.IBinder contextThread, android.os.IBinder token,
                String target, android.content.Intent intent, int requestCode, android.os.Bundle options) {
            return suppress(intent);
        }
        @Override public android.app.Instrumentation.ActivityResult execStartActivity(
                android.content.Context who, android.os.IBinder contextThread, android.os.IBinder token,
                String target, android.content.Intent intent, int requestCode, android.os.Bundle options,
                android.os.UserHandle user) {
            return suppress(intent);
        }
    }

    // ── Overlay windows (Dialog / PopupWindow / Toast) ────────────────────────────────────────────
    // acl runs a single Wayland surface for the Activity. A Dialog/PopupWindow adds its decor via
    // WindowManager.addView. AclWindowManager (below) intercepts that and records the decor here
    // instead of building a real ViewRootImpl; the present loop composites the list over the
    // Activity. Add order == Z order (last = topmost). removeView drops it (stops drawing).
    // WindowManagerImpl is final and WindowManagerGlobal's ctor private, so acl can't intercept
    // addView — it reads WindowManagerGlobal.mViews (where addView lands the decor) live each frame.
    private static final java.util.WeakHashMap<android.view.View, Boolean> sOverlayAttached =
            new java.util.WeakHashMap<>();

    public static void presentOverlays(android.graphics.Canvas canvas, int w, int h) {
        if (canvas == null) return;
        try {
            Class<?> wmgCls = Class.forName("android.view.WindowManagerGlobal");
            Object wmg = wmgCls.getMethod("getInstance").invoke(null);
            java.util.List<?> views = new java.util.ArrayList<>((java.util.List<?>) getField(wmgCls, wmg, "mViews"));
            java.util.List<?> params = new java.util.ArrayList<>((java.util.List<?>) getField(wmgCls, wmg, "mParams"));
            java.util.List<?> roots = new java.util.ArrayList<>((java.util.List<?>) getField(wmgCls, wmg, "mRoots"));
            android.view.View top = null; android.graphics.Rect topRect = null; boolean topModal = false;
            for (int i = 0; i < views.size() && i < params.size(); i++) {
                android.view.View v = (android.view.View) views.get(i);
                android.view.WindowManager.LayoutParams lp =
                        (android.view.WindowManager.LayoutParams) params.get(i);
                if (v == null || lp == null) continue;
                if (i < roots.size()) neuterViewRoot(roots.get(i));
                if (!Boolean.TRUE.equals(sOverlayAttached.get(v))) {
                    if (!v.isAttachedToWindow()) attachDecorToWindow(v);
                    sOverlayAttached.put(v, Boolean.TRUE);
                }
                if ((lp.flags & 2 /*FLAG_DIM_BEHIND*/) != 0) {
                    int a = (int) (Math.max(0f, Math.min(1f, lp.dimAmount)) * 255f);
                    canvas.drawColor(a << 24);  // black scrim behind the dialog
                }
                android.graphics.Rect r = drawOverlayView(canvas, v, lp, w, h);
                if (r != null) {  // the last drawn overlay is topmost — it receives input
                    top = v; topRect = r; topModal = (lp.flags & 0x20 /*FLAG_NOT_TOUCH_MODAL*/) == 0;
                }
            }
            sTopOverlay = top; sTopOverlayRect = topRect; sTopOverlayModal = topModal;
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: presentOverlays: " + t);
        }
    }

    // The topmost drawn overlay + where acl placed it on screen, for input routing. Written by
    // presentOverlays and read by routeOverlayTouch — both run on the present/main thread.
    private static volatile android.view.View sTopOverlay;
    private static volatile android.graphics.Rect sTopOverlayRect;
    private static volatile boolean sTopOverlayModal;

    // Route a pointer event to the topmost Dialog/PopupWindow overlay. acl gravity-places the decor
    // at sTopOverlayRect, so translate the event into the decor's local space before dispatch.
    // Returns true if the overlay consumed it (so the caller must NOT also deliver it to the Activity):
    // inside the overlay -> dispatched to the overlay; outside a modal overlay -> swallowed (the
    // dialog blocks the window behind, as on device); outside a non-modal one -> falls through.
    public static boolean routeOverlayTouch(android.view.MotionEvent me) {
        android.view.View top = sTopOverlay;
        android.graphics.Rect r = sTopOverlayRect;
        if (top == null || r == null) return false;
        if (r.contains((int) me.getX(), (int) me.getY())) {
            me.offsetLocation(-r.left, -r.top);
            try { top.dispatchTouchEvent(me); } catch (Throwable t) {
                System.err.println("acl HostBootstrap: routeOverlayTouch: " + t);
            }
            return true;
        }
        return sTopOverlayModal;
    }

    // Stop a Dialog/Popup's real ViewRootImpl from running its own traversal (acl drives render via
    // presentOverlays — measure/layout/draw the decor directly each frame). When the overlay's
    // ViewRootImpl schedules a traversal it (a) posts a Choreographer CALLBACK_TRAVERSAL that runs
    // performTraversals (which would fight acl / draw to a surface acl never gave it) and (b) posts a
    // main-looper SYNC BARRIER (mTraversalBarrier) that holds until doTraversal runs. So each frame we
    // CANCEL the pending traversal exactly the way doTraversal's cleanup would — remove the
    // Choreographer callback, remove the sync barrier, clear mTraversalScheduled — but WITHOUT running
    // performTraversals. Crucially this leaves NO lasting sync barrier: an earlier version set
    // mTraversalScheduled=TRUE and left a barrier posted for the overlay's whole lifetime, which
    // stranded every synchronous main-looper message — including a PopupWindow list item's postDelayed
    // performClick/pressed-state runnables, so menu items dispatched (consumed=true) but never
    // selected. And because mTraversalScheduled ends up FALSE, doDie -> unscheduleTraversals skips its
    // removeSyncBarrier(mTraversalBarrier) (no stale-token throw), so overlay removal still works.
    private static void neuterViewRoot(Object vri) {
        if (vri == null) return;
        try {
            Class<?> vriCls = Class.forName("android.view.ViewRootImpl");
            if (!Boolean.TRUE.equals(getField(vriCls, vri, "mTraversalScheduled"))) return;  // nothing pending
            // Cancel the pending Choreographer traversal callback so performTraversals never runs.
            Object choreo = getField(vriCls, vri, "mChoreographer");
            Object traversalRunnable = getField(vriCls, vri, "mTraversalRunnable");
            if (choreo != null && traversalRunnable instanceof Runnable) {
                try {
                    java.lang.reflect.Method rm = choreo.getClass().getMethod(
                            "removeCallbacks", int.class, Runnable.class, Object.class);
                    rm.invoke(choreo, 2 /*Choreographer.CALLBACK_TRAVERSAL*/, traversalRunnable, null);
                } catch (Throwable ignored) {}
            }
            // Remove the sync barrier scheduleTraversals posted (else it strands sync messages).
            Object barrier = getField(vriCls, vri, "mTraversalBarrier");
            if (barrier instanceof Integer) {
                try {
                    android.os.MessageQueue mq = android.os.Looper.getMainLooper().getQueue();
                    java.lang.reflect.Method remove = android.os.MessageQueue.class.getDeclaredMethod(
                            "removeSyncBarrier", int.class);
                    remove.setAccessible(true);
                    remove.invoke(mq, ((Integer) barrier).intValue());
                } catch (Throwable ignored) {}
            }
            setField(vriCls, vri, "mTraversalScheduled", Boolean.FALSE);
        } catch (Throwable ignored) {}
    }

    // Measure/layout the overlay decor per its LayoutParams (MATCH/WRAP/exact + gravity) and draw it.
    // Measure/lay out/draw the overlay decor; returns the screen Rect it was placed at (for input
    // routing), or null if it had nothing to draw.
    private static android.graphics.Rect drawOverlayView(android.graphics.Canvas canvas, android.view.View v,
            android.view.WindowManager.LayoutParams lp, int w, int h) {
        final int MATCH = -1, WRAP = -2;
        int wspec = measureSpec(lp.width, w, MATCH, WRAP);
        int hspec = measureSpec(lp.height, h, MATCH, WRAP);
        v.measure(wspec, hspec);
        int mw = v.getMeasuredWidth(), mh = v.getMeasuredHeight();
        if (mw <= 0 || mh <= 0) return null;
        v.layout(0, 0, mw, mh);
        int gravity = lp.gravity == 0 ? android.view.Gravity.CENTER : lp.gravity;
        android.graphics.Rect out = new android.graphics.Rect();
        android.view.Gravity.apply(gravity, mw, mh, new android.graphics.Rect(0, 0, w, h), lp.x, lp.y, out);
        int save = canvas.save();
        canvas.translate(out.left, out.top);
        v.draw(canvas);
        canvas.restoreToCount(save);
        return out;
    }

    private static int measureSpec(int lpDim, int avail, int match, int wrap) {
        if (lpDim == match) return android.view.View.MeasureSpec.makeMeasureSpec(avail, android.view.View.MeasureSpec.EXACTLY);
        // WRAP_CONTENT, or an unsized window (<= 0): on device the WindowManagerService relayout
        // sizes the window; acl's no-Binder session returns a 0 frame, so a Dialog's LayoutParams
        // stay 0x0. Measure the decor against the screen (AT_MOST) so the Compose/View content
        // reports its natural size instead of collapsing to EXACTLY(0).
        if (lpDim == wrap || lpDim <= 0) return android.view.View.MeasureSpec.makeMeasureSpec(avail, android.view.View.MeasureSpec.AT_MOST);
        return android.view.View.MeasureSpec.makeMeasureSpec(lpDim, android.view.View.MeasureSpec.EXACTLY);
    }

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
            // Touch dispatch goes through View.removeCallbacks -> mViewRootImpl.mChoreographer
            // .removeCallbacks (e.g. Compose's AndroidComposeView.handleMotionEvent); the hollow
            // ViewRootImpl's mChoreographer is null -> NPE before hit-testing. Seed the main
            // thread's Choreographer so pointer events reach the Compose hit-test / onClick.
            try {
                setField(vriCls, vri, "mChoreographer",
                        android.view.Choreographer.getInstance());
            } catch (Throwable ignored) {}
            // The ViewRootImpl ctor creates mImeFocusController (final); the hollow one skips it, so
            // a focus change during dispatchAttachedToWindow — View.onFocusChanged reaches
            // mViewRootImpl.getImeFocusController().onViewFocusChanged (View.java) — NPEs on the null
            // controller. A fullscreen GLSurfaceView decor (halma, doodledudette) takes focus on
            // attach, so it hits this. Seed a real one: onViewFocusChanged's getImmDelegate() resolves
            // (acl maps getSystemService(InputMethodManager.class)) and isCurrentRootView() is false
            // (acl never setCurrentRootView), so it early-returns without touching IME/Binder.
            try {
                Class<?> imefcCls = Class.forName("android.view.ImeFocusController");
                java.lang.reflect.Constructor<?> imefcCtor = imefcCls.getDeclaredConstructor(vriCls);
                imefcCtor.setAccessible(true);
                setField(vriCls, vri, "mImeFocusController", imefcCtor.newInstance(vri));
            } catch (Throwable ignored) {}
            // SurfaceView.onAttachedToWindow calls getViewRootImpl().addSurfaceChangedCallback ->
            // mSurfaceChangedCallbacks.add, a final ArrayList the hollow ViewRootImpl leaves null.
            // Seed it so GLSurfaceView apps (halma, doodledudette) attach their decor.
            try {
                setField(vriCls, vri, "mSurfaceChangedCallbacks", new java.util.ArrayList<>());
            } catch (Throwable ignored) {}
            // A classic (XML-inflated / fragment) View hierarchy drives the real ViewRootImpl during
            // attach + traversal, unlike Compose (which draws through its own layers). These fields are
            // field-initialized in the ViewRootImpl ctor acl skips, so a null one NPEs the traversal:
            // mHandler (ViewRootHandler.removeMessages), mInvalidateOnAnimationRunnable (.addView), and
            // the three ViewRootRectTracker fields (updateRectsForView). Seed them.
            // ViewRootImpl.getConfiguration() (reached from getWindowInsets → View.getRootWindowInsets,
            // e.g. androidx DrawerLayout.onLayout) dereferences mContext.getResources(); seed it.
            try { setField(vriCls, vri, "mContext", ctx); } catch (Throwable ignored) {}
            // ViewRootImpl.getDisplayId() dereferences mDisplay; the hollow ViewRootImpl leaves it
            // null. A WebView loadUrl triggers chromium's ImeAdapterImpl.onConnectedToRenderProcess ->
            // InputMethodManager.isActive() -> getFallbackInputMethodManagerIfNecessary() ->
            // ViewRootImpl.getDisplayId() -> NPE on the null mDisplay, which chromium's jni_android
            // CheckException turns into a FATAL abort (the WebView never loads). Seed the Display.
            try { setField(vriCls, vri, "mDisplay", buildDisplay()); } catch (Throwable ignored) {}
            // getWindowInsets() reads mWindowAttributes.type; seed a default LayoutParams.
            try {
                android.view.WindowManager.LayoutParams wa = new android.view.WindowManager.LayoutParams();
                wa.type = android.view.WindowManager.LayoutParams.TYPE_APPLICATION;
                setField(vriCls, vri, "mWindowAttributes", wa);
            } catch (Throwable ignored) {}
            // A Popup/DropdownMenu positions itself against the parent window's visible frame:
            // ViewRootImpl.getWindowVisibleDisplayFrame reads mTmpFrames.displayFrame. The hollow
            // ViewRootImpl has a null mTmpFrames (ctor-initialized, skipped by Unsafe) → NPE in
            // presentOverlays, so overflow menus / dropdowns / spinners never open. Seed a
            // ClientWindowFrames covering the display so the frame reads resolve.
            try {
                Class<?> cwf = Class.forName("android.window.ClientWindowFrames");
                Object frames = cwf.getConstructor().newInstance();
                for (String fld : new String[]{"frame", "displayFrame", "parentFrame"}) {
                    try {
                        Object r = cwf.getField(fld).get(frames);
                        if (r instanceof android.graphics.Rect)
                            ((android.graphics.Rect) r).set(0, 0, DISPLAY_W, DISPLAY_H);
                    } catch (Throwable ignored) {}
                }
                setField(vriCls, vri, "mTmpFrames", frames);
            } catch (Throwable ignored) {}
            // The visible-insets/frame Rects some paths read directly off the ViewRootImpl.
            for (String rf : new String[]{"mWinFrame"}) {
                try { setField(vriCls, vri, rf, new android.graphics.Rect(0, 0, DISPLAY_W, DISPLAY_H)); }
                catch (Throwable ignored) {}
            }
            try {
                setField(vriCls, vri, "mHandler",
                        innerInstance("android.view.ViewRootImpl$ViewRootHandler", vriCls, vri));
            } catch (Throwable ignored) {}
            try {
                setField(vriCls, vri, "mInvalidateOnAnimationRunnable",
                        innerInstance("android.view.ViewRootImpl$InvalidateOnAnimationRunnable", vriCls, vri));
            } catch (Throwable ignored) {}
            try {
                Class<?> rtCls = Class.forName("android.view.ViewRootRectTracker");
                java.lang.reflect.Constructor<?> rtCtor =
                        rtCls.getDeclaredConstructor(java.util.function.Function.class);
                rtCtor.setAccessible(true);
                java.util.function.Function<Object, Object> emptyRects = v -> new java.util.ArrayList<>();
                for (String f : new String[]{"mGestureExclusionTracker", "mKeepClearRectsTracker",
                                             "mUnrestrictedKeepClearRectsTracker"}) {
                    try { setField(vriCls, vri, f, rtCtor.newInstance(emptyRects)); } catch (Throwable ignored) {}
                }
            } catch (Throwable ignored) {}
            // View.getWindowInsetsController() -> mViewRootImpl.getInsetsController() (mInsetsController);
            // null on the hollow ViewRootImpl -> WindowInsetsControllerCompat.show() NPEs (apps drive
            // edge-to-edge / IME visibility from a LaunchedEffect on startup). Seed a real
            // InsetsController over a no-op InsetsController.Host so show/hide are safe no-ops (acl has
            // no system bars/IME insets to animate).
            try {
                Class<?> hostCls = Class.forName("android.view.InsetsController$Host");
                Object host = java.lang.reflect.Proxy.newProxyInstance(
                        hostCls.getClassLoader(), new Class<?>[]{hostCls},
                        (p, m, a) -> {
                            switch (m.getName()) {
                                case "getHandler": return new android.os.Handler(android.os.Looper.getMainLooper());
                                case "hasAnimationCallbacks": return Boolean.FALSE;
                                case "getSystemBarsAppearance":
                                case "getSystemBarsBehavior": return 0;
                                case "dipToPx": return (a != null && a.length > 0) ? a[0] : Integer.valueOf(0);
                                case "getRootViewTitle": return "acl";
                                default: return defaultValue(m.getReturnType());
                            }
                        });
                Object ic = Class.forName("android.view.InsetsController")
                        .getConstructor(hostCls).newInstance(host);
                setField(vriCls, vri, "mInsetsController", ic);
            } catch (Throwable ignored) {}
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
            // acl's present canvas is a hardware-accelerated RecordingCanvas over a GPU (Skia/Ganesh
            // → dma-buf) surface, so the view tree must believe it is hardware-accelerated. This is
            // what makes View.isHardwareAccelerated() true, so Jetpack Compose picks RenderNodeLayer
            // (recorded display lists replayed via Canvas.drawRenderNode) instead of a software
            // ViewLayer — acl re-owns that RenderNode record/replay path in graphics_jni.
            try { setField(aiCls, attachInfo, "mHardwareAccelerated", Boolean.TRUE); } catch (Throwable ignored) {}
            // View.canHaveDisplayList() gates the hardware display-list path on
            // mAttachInfo.mThreadedRenderer != null. Without it, View.updateDisplayListIfDirty()
            // never records a RenderNode (Compose layers + classic View children draw empty), so
            // seed a hollow ThreadedRenderer purely to satisfy the non-null check — acl drives its
            // own present loop and never calls into it (its record/replay is re-owned in graphics_jni).
            try {
                Object tr = allocateHollow(Class.forName("android.view.ThreadedRenderer"));
                setField(aiCls, attachInfo, "mThreadedRenderer", tr);
            } catch (Throwable ignored) {}
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
            // AwContents (WebView) tracks window visibility via onWindowVisibilityChanged and DEFERS
            // its navigation's network fetch until the window is visible — dispatchAttachedToWindow
            // alone does not deliver that. Without it a WebView loadUrl sticks at progress=10
            // (navigation started, request never dispatched). Dispatch VISIBLE to the decor tree.
            try { decor.dispatchWindowVisibilityChanged(android.view.View.VISIBLE); }
            catch (Throwable ignored) {}
            System.out.println("acl HostBootstrap: decor attached to window");
        } catch (Throwable t) {
            Throwable c = (t instanceof java.lang.reflect.InvocationTargetException
                    && t.getCause() != null) ? t.getCause() : t;
            System.err.println("acl HostBootstrap: attachDecorToWindow deferred: " + c);
        }
    }

    // DIAG (ACL_VIEWTREE_DIAG): dump the decor view tree — class, LTRB bounds, visibility,
    // clickable/enabled — so an input-routing gap (a custom View never getting onTouchEvent) can
    // be pinned to layout (0-size view => hit-test misses) vs. tree structure.
    public static void dumpViewTree(android.view.View v, int depth) {
        if (v == null) return;
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < depth; i++) sb.append("  ");
        sb.append(v.getClass().getName())
          .append(" [").append(v.getLeft()).append(',').append(v.getTop()).append(',')
          .append(v.getRight()).append(',').append(v.getBottom()).append(']')
          .append(" vis=").append(v.getVisibility())
          .append(" shown=").append(v.isShown())
          .append(" click=").append(v.isClickable())
          .append(" enabled=").append(v.isEnabled());
        System.out.println("acl viewtree: " + sb);
        if (v instanceof android.view.ViewGroup) {
            android.view.ViewGroup g = (android.view.ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) dumpViewTree(g.getChildAt(i), depth + 1);
        }
    }

    private static Object getField(Class<?> c, Object obj, String name) throws Exception {
        java.lang.reflect.Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        return f.get(obj);
    }

    private static void setField(Class<?> c, Object obj, String name, Object val) throws Exception {
        java.lang.reflect.Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        f.set(obj, val);
    }

    // Instantiate a non-static inner class (its synthetic constructor takes the enclosing instance).
    private static Object innerInstance(String innerName, Class<?> outerCls, Object outer) throws Exception {
        java.lang.reflect.Constructor<?> c = Class.forName(innerName).getDeclaredConstructor(outerCls);
        c.setAccessible(true);
        return c.newInstance(outer);
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
                            // Audio-focus arbitration. acl runs one app in-process with nothing else
                            // competing for the output (ADR-3), so always grant. A FAILED (0) here —
                            // the default — makes media3/ExoPlayer treat focus as denied and SUPPRESS
                            // playback (position frozen, playWhenReady honored but paused), which is
                            // why a video_player app decoded one frame and never advanced. Return
                            // AUDIOFOCUS_REQUEST_GRANTED (1) so playback proceeds. We never call the
                            // IAudioFocusDispatcher back (no LOSS/GAIN events) — correct, since no
                            // other stream ever takes focus away. abandon likewise reports granted.
                            case "requestAudioFocus":
                            case "abandonAudioFocus":
                                return 1;  // AudioManager.AUDIOFOCUS_REQUEST_GRANTED
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
            setStaticField(wmg, "sWindowSession", makeWindowSession());
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: seedWindowManagerGlobal failed: " + t);
        }
    }

    // A no-Binder IWindowSession for the addView -> ViewRootImpl.setView path (Dialog/PopupWindow).
    // addToDisplayAsUser/relayout must return ADD_OKAY (0) AND fill the outSizeCompatScale float[]
    // with 1.0 — a default-0 leaves ViewRootImpl.mInvCompatScale = 1/0 = Infinity, corrupting layout.
    private static Object makeWindowSession() throws Exception {
        Class<?> iface = Class.forName("android.view.IWindowSession");
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> {
                    String n = method.getName();
                    if (n.startsWith("addToDisplay") || n.startsWith("relayout")) {
                        if (a != null) for (Object arg : a) {
                            if (arg instanceof float[] && ((float[]) arg).length > 0) ((float[]) arg)[0] = 1.0f;
                            // ViewRootImpl.setView passes an empty InputChannel expecting WMS to fill it;
                            // an unfilled one makes WindowInputEventReceiver's nativeInit throw
                            // "InputChannel is not initialized." Give it a real host socketpair channel.
                            if (arg instanceof android.view.InputChannel) initInputChannel((android.view.InputChannel) arg);
                        }
                        return method.getReturnType() == void.class ? null : Integer.valueOf(0);
                    }
                    return defaultValue(method.getReturnType());
                });
    }

    // Server ends of the host InputChannel socketpairs, kept alive so the client ends handed to
    // ViewRootImpl stay open (acl never dispatches through them — the pair just satisfies the
    // framework's "channel must be initialized" invariant on the no-WMS Dialog/PopupWindow path).
    private static final java.util.List<android.view.InputChannel> sChannelServerEnds =
            new java.util.ArrayList<>();

    private static void initInputChannel(android.view.InputChannel out) {
        try {
            android.view.InputChannel[] pair = android.view.InputChannel.openInputChannelPair("acl-host-window");
            synchronized (sChannelServerEnds) { sChannelServerEnds.add(pair[0]); }
            android.os.Parcel parcel = android.os.Parcel.obtain();
            pair[1].writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            out.readFromParcel(parcel);
            parcel.recycle();
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: initInputChannel failed: " + t);
        }
    }

    // Install a compat-change delegate so android.compat.Compatibility.isChangeEnabled(id) returns
    // FALSE (old behavior) instead of the framework default TRUE ("No Compatibility callbacks set!").
    // The default-TRUE drives device-only paths off-device — e.g. ViewRootImpl.getViewBoundsSandboxing
    // Enabled() then calls PackageManager.queryApplicationProperty and throws a boot-classloader
    // NoClassDefFoundError during a Dialog's setView, which was silently killing every Dialog. acl is
    // not a real device, so disabling all opt-in compat changes is the correct + crash-free default.
    private static boolean sCompatSeeded;
    static synchronized void seedCompatCallbacks() {
        if (sCompatSeeded) return;
        sCompatSeeded = true;
        try {
            Class<?> compat = Class.forName("android.compat.Compatibility");
            Class<?> delegateCls = Class.forName("android.compat.Compatibility$BehaviorChangeDelegate");
            Object delegate = java.lang.reflect.Proxy.newProxyInstance(
                    delegateCls.getClassLoader(), new Class<?>[]{delegateCls},
                    (p, m, a) -> m.getName().equals("isChangeEnabled")
                            ? Boolean.FALSE : defaultValue(m.getReturnType()));
            compat.getMethod("setBehaviorChangeDelegate", delegateCls).invoke(null, delegate);
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: seedCompatCallbacks failed: " + t);
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
            final int dw = DISPLAY_W, dh = DISPLAY_H, dpi = DISPLAY_DPI;  // the advertised display
            Class<?> diCls = Class.forName("android.view.DisplayInfo");
            Object di = diCls.getConstructor().newInstance();
            for (String[] f : new String[][]{
                    {"logicalWidth", Integer.toString(dw)}, {"logicalHeight", Integer.toString(dh)},
                    {"appWidth", Integer.toString(dw)}, {"appHeight", Integer.toString(dh)},
                    {"logicalDensityDpi", Integer.toString(dpi)}, {"rotation", "0"}, {"state", "2"}}) {
                try { setField(diCls, di, f[0], Integer.parseInt(f[1])); } catch (Throwable ignored) {}
            }
            // physical DPI (float): Display.getMetrics() fills DisplayMetrics.xdpi/ydpi from these.
            // Left 0, any app sizing from the physical DPI gets 0 — libGDX's getPpiX()/getDensity()
            // path does exactly this, and doodledudette sizes an SVG-icon Bitmap by (int)xdpi, so a
            // 0 xdpi meant createBitmap(0,0) → IllegalArgumentException in its GLThread.
            try { setField(diCls, di, "physicalXDpi", Float.valueOf((float) dpi)); } catch (Throwable ignored) {}
            try { setField(diCls, di, "physicalYDpi", Float.valueOf((float) dpi)); } catch (Throwable ignored) {}
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

    // TelephonyManager methods (getSignalStrength/getNetworkType/…) resolve ITelephony via
    // TelephonyFrameworkInitializer.getTelephonyServiceManager(), which the framework normally seeds
    // during boot but acl does not — so getTelephonyServiceManager() is null and every telephony call
    // NPEs. Chromium's NetworkChangeNotifierAutoDetect / AndroidNetworkLibrary up-call these, and its
    // JNI CheckException turns the NPE into a fatal process abort. Seed a real TelephonyServiceManager:
    // its per-service ServiceRegisterers resolve through the (no-Binder) ServiceManager → null binders,
    // so telephony calls degrade to null/defaults instead of throwing. Idempotent (the setter throws if
    // called twice).
    private static void seedTelephonyFrameworkInitializer() {
        try {
            Class<?> tfi = Class.forName("android.telephony.TelephonyFrameworkInitializer");
            Class<?> tsm = Class.forName("android.os.TelephonyServiceManager");
            if (tfi.getMethod("getTelephonyServiceManager").invoke(null) == null)
                tfi.getMethod("setTelephonyServiceManager", tsm)
                   .invoke(null, tsm.getConstructor().newInstance());
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: telephony framework init seed skipped: " + t);
        }
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
        // KeyguardManager's ctor does ServiceManager.getServiceOrThrow("trust") (throws without it);
        // a no-op ITrustManager proxy is enough (isDeviceLocked/isKeyguardLocked → false).
        svc.put("trust", "android.app.trust.ITrustManager");
        // WebView provider init reads IWebViewUpdateService.isMultiProcessEnabled() etc.; a no-op proxy
        // (multiprocess=false, no updates) is enough — acl installs the provider directly (see
        // installWebViewProvider), so the update-service is only queried for these secondary flags.
        svc.put("webviewupdate", "android.webkit.IWebViewUpdateService");
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

    // ── GLSurfaceView (Stage 3) ────────────────────────────────────────────────────────────
    // acl has no Binder/ViewRootImpl-backed Surface, so a SurfaceView's updateSurface() never
    // creates a Surface and surfaceCreated never fires — its GLThread blocks forever in
    // eglCreateWindowSurface. These helpers let acl route a GLSurfaceView app to the GL present
    // and hand its SurfaceView a ready Surface (backed natively by acl's toplevel wl_surface).

    // Does the view tree contain an android.view.SurfaceView (e.g. a GLSurfaceView)?
    public static boolean hasSurfaceView(android.view.View v) {
        return findSurfaceView(v) != null;
    }

    private static android.view.SurfaceView findSurfaceView(android.view.View v) {
        if (v instanceof android.view.SurfaceView) return (android.view.SurfaceView) v;
        if (v instanceof android.view.ViewGroup) {
            android.view.ViewGroup g = (android.view.ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) {
                android.view.SurfaceView sv = findSurfaceView(g.getChildAt(i));
                if (sv != null) return sv;
            }
        }
        return null;
    }

    // Inject a ready Surface into the SurfaceView, bypassing updateSurface() entirely: mark the
    // state fields and invoke the registered SurfaceHolder.Callbacks (a GLSurfaceView registers
    // itself). The GLThread then unblocks and calls eglCreateWindowSurface(holder); acl's EGLImpl
    // hook backs it with the toplevel wl_surface, so mNativeObject stays 0 (the GL path never
    // dereferences it). Runs on the main thread.
    public static void injectGlSurface(android.view.View decor, int w, int h) {
        try {
            android.view.SurfaceView sv = findSurfaceView(decor);
            if (sv == null) return;
            Class<?> c = android.view.SurfaceView.class;
            int fmt = getIntField(c, sv, "mRequestedFormat", android.graphics.PixelFormat.RGBX_8888);
            setField(c, sv, "mHaveFrame", Boolean.TRUE);
            setField(c, sv, "mVisible", Boolean.TRUE);
            setField(c, sv, "mRequestedVisible", Boolean.TRUE);
            setField(c, sv, "mSurfaceCreated", Boolean.TRUE);
            setIntField(c, sv, "mSurfaceWidth", w);
            setIntField(c, sv, "mSurfaceHeight", h);
            setIntField(c, sv, "mFormat", fmt);
            android.view.SurfaceHolder holder = sv.getHolder();
            for (android.view.SurfaceHolder.Callback cb : snapshotCallbacks(c, sv)) {
                if (cb == null) continue;
                cb.surfaceCreated(holder);
                cb.surfaceChanged(holder, fmt, w, h);
            }
            android.util.Log.i("acl", "injectGlSurface: SurfaceView surfaceCreated (" + w + "x" + h + ")");
        } catch (Throwable t) {
            android.util.Log.w("acl", "injectGlSurface failed: " + t);
        }
    }

    @SuppressWarnings("unchecked")
    private static android.view.SurfaceHolder.Callback[] snapshotCallbacks(Class<?> c, Object sv)
            throws Exception {
        java.lang.reflect.Field f = c.getDeclaredField("mCallbacks");
        f.setAccessible(true);
        java.util.ArrayList<android.view.SurfaceHolder.Callback> list =
                (java.util.ArrayList<android.view.SurfaceHolder.Callback>) f.get(sv);
        return list.toArray(new android.view.SurfaceHolder.Callback[0]);
    }

    private static int getIntField(Class<?> c, Object obj, String name, int dflt) {
        try {
            java.lang.reflect.Field f = c.getDeclaredField(name);
            f.setAccessible(true);
            return f.getInt(obj);
        } catch (Throwable t) {
            return dflt;
        }
    }

    private static void setIntField(Class<?> c, Object obj, String name, int val) throws Exception {
        java.lang.reflect.Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        f.setInt(obj, val);
    }
}
