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
    /**
     * The root's ViewTreeObserver for one traversal, in ViewRootImpl.performTraversals' order:
     * global layout when the tree was laid out, pre-draw, then draw. Returns true when a
     * pre-draw listener cancelled the draw.
     */
    public static boolean dispatchTreeObserver(android.view.View root, boolean didLayout) {
        final android.view.ViewTreeObserver observer = root.getViewTreeObserver();
        if (didLayout) {
            // A view added to the tree asks for a layout, so this is when a new SurfaceView
            // can have appeared.
            dropSurfaceViewPreDraw(root, observer);
            observer.dispatchOnGlobalLayout();
        }
        if (observer.dispatchOnPreDraw()) return true;
        stampDrawingTime(root);
        observer.dispatchOnDraw();
        return false;
    }

    // ViewRootImpl.draw sets AttachInfo.mDrawingTime for the frame, and a4l draws without it.
    // View.getDrawingTime() reads it, and a view Animation (android.view.animation) takes its
    // start time and its progress from it. Left at 0, such an animation starts at 0 and stays
    // on its first frame: a fade-in holds its view at alpha 0.
    private static java.lang.reflect.Field sViewAttachInfo;
    private static java.lang.reflect.Field sAttachDrawingTime;
    private static void stampDrawingTime(android.view.View root) {
        try {
            if (sViewAttachInfo == null) {
                java.lang.reflect.Field v = android.view.View.class.getDeclaredField("mAttachInfo");
                v.setAccessible(true);
                java.lang.reflect.Field t = Class.forName("android.view.View$AttachInfo")
                        .getDeclaredField("mDrawingTime");
                t.setAccessible(true);
                sAttachDrawingTime = t;
                sViewAttachInfo = v;
            }
            Object info = sViewAttachInfo.get(root);
            if (info != null) sAttachDrawingTime.setLong(info, android.os.SystemClock.uptimeMillis());
        } catch (ReflectiveOperationException e) {
            // The fields are the platform's; without them animations keep the old behavior.
        }
    }

    // SurfaceView registers a pre-draw listener of its own that runs updateSurface(). a4l gives
    // a SurfaceView its surface itself (injectSurface, maybeInjectVideoSurface); updateSurface()
    // finds no window surface behind a4l's stand-in ViewRootImpl and tells the app its surface
    // was destroyed. Take that listener off the observer.
    private static java.lang.reflect.Field sSurfaceViewDrawListener;
    private static void dropSurfaceViewPreDraw(android.view.View v,
            android.view.ViewTreeObserver observer) {
        if (v instanceof android.view.SurfaceView) {
            try {
                if (sSurfaceViewDrawListener == null) {
                    java.lang.reflect.Field f =
                            android.view.SurfaceView.class.getDeclaredField("mDrawListener");
                    f.setAccessible(true);
                    sSurfaceViewDrawListener = f;
                }
                observer.removeOnPreDrawListener((android.view.ViewTreeObserver.OnPreDrawListener)
                        sSurfaceViewDrawListener.get(v));
            } catch (Throwable t) {
                android.util.Log.w("acl", "SurfaceView pre-draw listener: " + t);
            }
        } else if (v instanceof android.view.ViewGroup) {
            final android.view.ViewGroup g = (android.view.ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++)
                dropSurfaceViewPreDraw(g.getChildAt(i), observer);
        }
    }

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
        // An exception no thread caught is logged as Android's runtime logs it ("FATAL
        // EXCEPTION: <thread>"), from a pre-handler and again from the default handler for a
        // caller that goes to the handler directly, as kotlinx.coroutines does.
        //
        // Android then ends the process. This default handler does not: the thread ends and
        // the app goes on, as it did before there was a handler here. An exception on a
        // worker is often a4l's own gap, and an app that would have shown its screen is worth
        // more than one that is gone. An app's own handler still does what it does; a crash
        // reporter chains to this one and, finding one, does not end the process itself.
        try {
            final Throwable[] logged = {null};
            final Thread.UncaughtExceptionHandler log = (thread, error) -> {
                synchronized (logged) {
                    if (logged[0] == error) return;
                    logged[0] = error;
                }
                android.util.Log.e("AndroidRuntime",
                        "FATAL EXCEPTION: " + thread.getName(), error);
            };
            // Hidden in libcore, so it is not in the jar this compiles against.
            Thread.class.getMethod("setUncaughtExceptionPreHandler",
                    Thread.UncaughtExceptionHandler.class).invoke(null, log);
            Thread.setDefaultUncaughtExceptionHandler(log);
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: uncaught-exception logging not set: " + t);
        }
        // ACL_EXIT_STACKS=1: when the process exits, print the stack of the thread that is in
        // Runtime.exit, to name who ended it. A shutdown hook runs while that thread waits.
        if ("1".equals(System.getenv("ACL_EXIT_STACKS"))) {
            Runtime.getRuntime().addShutdownHook(new Thread(() -> {
                for (java.util.Map.Entry<Thread, StackTraceElement[]> e
                        : Thread.getAllStackTraces().entrySet()) {
                    boolean exiting = false;
                    for (StackTraceElement f : e.getValue()) {
                        exiting |= "java.lang.Runtime".equals(f.getClassName())
                                && "exit".equals(f.getMethodName());
                    }
                    if (!exiting) continue;
                    System.err.println("acl exit: called on thread " + e.getKey().getName());
                    for (StackTraceElement f : e.getValue()) {
                        System.err.println("acl exit:   at " + f);
                    }
                }
            }, "acl-exit-stacks"));
        }
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
        seedMediaFrameworkInitializer();
        seedSqliteDefaults();
        // Register the AndroidKeyStore JCE provider before app code runs: apps do
        // KeyStore.getInstance("AndroidKeyStore") / KeyGenerator.getInstance(_, "AndroidKeyStore")
        // during startup (Telegram's FingerprintController), which NPE/throw with no provider.
        AclKeyStoreProvider.install();
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
        if (!new java.io.File(apkPath).exists()) {
            System.err.println("acl WebView: ACL_WEBVIEW_APK not found: " + apkPath);
            return;
        }
        sWebViewInstalled = true;
        // LAZY arm: publish a Proxy over WebViewFactoryProvider as sProviderInstance now, but defer the
        // heavy work (extract + load the ~96MB libwebviewchromium.so, spin up chromium's browser process)
        // until the app actually TOUCHES WebView — the first provider method call (WebView.<init> ->
        // createWebView, or WebSettings/CookieManager/WebStorage). Non-WebView apps that merely reference
        // android.webkit.* (many do, via bundled libraries) never call a provider method, so they pay
        // nothing. That makes it safe for run-apk to auto-set ACL_WEBVIEW_APK for EVERY app — no manual
        // env, no cost to non-WebView apps. On the first call, loadWebViewProviderReal() builds the real
        // provider AND replaces sProviderInstance with it, so getProvider() bypasses this proxy for good.
        try {
            Class<?> ifaceCls = Class.forName("android.webkit.WebViewFactoryProvider");
            Object proxy = java.lang.reflect.Proxy.newProxyInstance(
                    ifaceCls.getClassLoader(), new Class<?>[]{ifaceCls},
                    new LazyWebViewProvider(apkPath));
            setStaticField(Class.forName("android.webkit.WebViewFactory"), "sProviderInstance", proxy);
            System.err.println("acl WebView: lazy provider armed (chromium loads on first WebView use)");
        } catch (Throwable t) {
            System.err.println("acl WebView: lazy provider arm failed: " + t);
            t.printStackTrace();
        }
    }

    // The InvocationHandler behind the lazy WebView provider proxy: on the first method call it loads the
    // real chromium provider (serialized so the ~96MB engine loads exactly once), then delegates to it.
    private static final class LazyWebViewProvider implements java.lang.reflect.InvocationHandler {
        private final String apkPath;
        private Object real;
        private boolean tried;
        LazyWebViewProvider(String apkPath) { this.apkPath = apkPath; }
        @Override public Object invoke(Object proxy, java.lang.reflect.Method m, Object[] args)
                throws Throwable {
            synchronized (this) {
                if (!tried) { tried = true; real = loadWebViewProviderReal(apkPath); }
            }
            if (real == null)
                throw new UnsupportedOperationException("acl: WebView provider failed to load");
            try {
                return m.invoke(real, args);
            } catch (java.lang.reflect.InvocationTargetException e) {
                throw e.getCause();
            }
        }
    }

    // The Android ABI directory for a JVM os.arch value, or null for a host no WebView APK has an
    // engine for.
    static String webViewAbi(String osArch) {
        if (osArch == null) return null;
        switch (osArch) {
            case "x86_64": case "amd64": return "x86_64";
            case "aarch64": case "arm64": return "arm64-v8a";
            default: return null;
        }
    }

    // Heavy body (run lazily by LazyWebViewProvider): extract the engine, load it under the WebView
    // classloader, build the chromium provider, publish it as sProviderInstance, and RETURN it (null on
    // failure). Publishing the real provider means getProvider() no longer routes through the proxy.
    private static Object loadWebViewProviderReal(final String apkPath) {
        try {
            java.io.File apk = new java.io.File(apkPath);
            if (!apk.exists()) {
                System.err.println("acl WebView: ACL_WEBVIEW_APK not found: " + apkPath);
                return null;
            }
            // The APK's lib/<abi>/ for this host: the engine is native code.
            final String abi = webViewAbi(System.getProperty("os.arch"));
            if (abi == null) {
                System.err.println("acl WebView: no engine for host architecture "
                        + System.getProperty("os.arch"));
                return null;
            }
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
                return null;
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
            //
            // Every Context of the app gets it, not the Application's alone. Chromium loads its
            // resources through the Context a WebView was made with, an Activity as a rule, and
            // each of a4l's has an AssetManager of its own. (WebViewDelegate.addWebViewAssetPath,
            // which Chromium calls for this, goes through ResourcesManager and reaches none of
            // them.) Without it an Activity's Resources do not know Chromium's ids:
            // BitmapFactory.decodeResource returns null for the default video poster, and
            // Chromium ends the process on the NullPointerException that follows.
            addWebViewAssets(apkPath);
            if (sApplication != null) addSharedLibraryAssets(sApplication.getAssets(), apkPath);
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
            return provider;
        } catch (Throwable t) {
            System.err.println("acl WebView: provider install failed: " + t);
            t.printStackTrace();
            return null;
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

    static AppContext sAppContext;  // NotifyBridge loads a notification's icon with it

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

    // Config-split APK paths for the running app (ACL_SPLIT_APKS, ':'-separated). An app-bundle
    // install is base.apk + split_config.{density,lang,abi}.apk; acl runs the base directly and takes
    // the splits here. Only existing files are returned.
    static String[] splitApkPaths() {
        String v = System.getenv("ACL_SPLIT_APKS");
        if (v == null || v.isEmpty()) return new String[0];
        java.util.List<String> out = new java.util.ArrayList<String>();
        for (String p : v.split(":")) {
            if (!p.isEmpty() && new java.io.File(p).isFile()) out.add(p);
        }
        return out.toArray(new String[0]);
    }

    // Build the app's Resources through the framework's ResourcesManager so base.apk + its config
    // splits are stacked into ONE split-aware AssetManager — the same path ActivityThread/LoadedApk
    // use on a real device (splitResDirs). This is how a density drawable / language string that
    // lives only in a split resolves. Doing it via ResourcesManager (not a hand-rolled AssetManager +
    // addAssetPath) is essential: RM owns the process-wide ApkAssets cache, so it merges the tables
    // cleanly instead of a second AssetManager polluting shared state (which broke Firebase init).
    // Reflection-only: getResources / getInstance / CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO are
    // @hide. Returns null on any failure so appResources falls back to the base-only path.
    // Open the BASE apk's AndroidManifest.xml. openXmlResourceParser("AndroidManifest.xml") with no
    // cookie returns the manifest of the FIRST apk in the AssetManager — which, with framework-res, a
    // vendored WebView apk, or config SPLITS loaded, is often NOT the app's base.apk. A config split's
    // manifest declares no <provider>/<application>/<activity>, so those parses silently came back
    // empty: for a bundle app that dropped EVERY ContentProvider, incl. FirebaseInitProvider, and the
    // app then died "Default FirebaseApp is not initialized". Pin the base apk via findCookieForPath.
    static android.content.res.XmlResourceParser openBaseManifest(
            android.content.res.AssetManager am, String baseApkPath) throws java.io.IOException {
        int cookie = 0;
        if (baseApkPath != null) {
            try {
                java.lang.reflect.Method m = am.getClass().getMethod("findCookieForPath", String.class);
                Object c = m.invoke(am, baseApkPath);
                if (c instanceof Integer) cookie = ((Integer) c).intValue();
            } catch (Throwable ignored) { }
        }
        return cookie > 0 ? am.openXmlResourceParser(cookie, "AndroidManifest.xml")
                          : am.openXmlResourceParser("AndroidManifest.xml");
    }

    private static Resources appResourcesViaSplits(String apkPath, String[] splits) {
        try {
            Class<?> rmCls = Class.forName("android.app.ResourcesManager");
            Object rm = rmCls.getMethod("getInstance").invoke(null);
            Class<?> ciCls = Class.forName("android.content.res.CompatibilityInfo");
            Object compat = ciCls.getField("DEFAULT_COMPATIBILITY_INFO").get(null);
            java.lang.reflect.Method m = rmCls.getMethod("getResources",
                    android.os.IBinder.class, String.class, String[].class, String[].class,
                    String[].class, String[].class, Integer.class,
                    android.content.res.Configuration.class, ciCls, ClassLoader.class,
                    java.util.List.class);
            Resources r = (Resources) m.invoke(rm,
                    null,                 // activityToken
                    apkPath,              // resDir (base.apk)
                    splits,               // splitResDirs (the config splits)
                    null, null, null,     // legacyOverlayDirs, overlayPaths, libDirs
                    null,                 // overrideDisplayId -> default display
                    appConfiguration(),   // overrideConfig -> acl's advertised display config
                    compat,               // CompatibilityInfo.DEFAULT
                    null,                 // classLoader -> system
                    null);                // ResourcesLoaders
            if (r != null) {
                System.out.println("acl HostBootstrap: split-aware resources via ResourcesManager (base + "
                        + splits.length + " config split(s))");
            }
            return r;
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: ResourcesManager split load failed: " + t);
            return null;
        }
    }

    // The AssetManagers made for the app's Contexts, and the WebView APK once Chromium is
    // loaded: it is added to each as a shared library, those there and those made later.
    private static final java.util.Set<android.content.res.AssetManager> sAppAssets =
            java.util.Collections.newSetFromMap(new java.util.WeakHashMap<>());
    private static String sWebViewAssetPath;

    private static void addWebViewAssets(String apkPath) {
        synchronized (sAppAssets) {
            sWebViewAssetPath = apkPath;
            for (android.content.res.AssetManager am : sAppAssets) addSharedLibraryAssets(am, apkPath);
        }
    }

    private static void addSharedLibraryAssets(android.content.res.AssetManager am, String apkPath) {
        try {
            Object cookie = android.content.res.AssetManager.class
                    .getMethod("addAssetPathAsSharedLibrary", String.class).invoke(am, apkPath);
            if (!(cookie instanceof Integer) || ((Integer) cookie).intValue() == 0) {
                System.err.println("acl WebView: " + apkPath + " not added to an AssetManager");
            }
        } catch (Throwable t) {
            System.err.println("acl WebView: addAssetPathAsSharedLibrary failed: " + t);
        }
    }

    private static Resources appResources(String apkPath, String frameworkResPath) {
        final Resources r = buildAppResources(apkPath, frameworkResPath);
        if (r != Resources.getSystem()) {
            synchronized (sAppAssets) {
                if (sAppAssets.add(r.getAssets()) && sWebViewAssetPath != null) {
                    addSharedLibraryAssets(r.getAssets(), sWebViewAssetPath);
                }
            }
        }
        return r;
    }

    private static Resources buildAppResources(String apkPath, String frameworkResPath) {
        // App-bundle install (base + config splits): route through the framework's split-aware
        // ResourcesManager so split-only resources resolve. Single-APK apps (no ACL_SPLIT_APKS) keep
        // the proven hand-rolled path below unchanged.
        String[] splits = splitApkPaths();
        sizeSystemResources();
        if (splits.length > 0) {
            Resources r = appResourcesViaSplits(apkPath, splits);
            if (r != null) return r;
        }
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
            // Notification.Builder and NotificationManager shrink a notification's large icon
            // to notification_right_icon_size, 48dp: the size of the icon in Android's shade.
            // The desktop draws it larger, so that one dimension answers with NotifyBridge's
            // limit. An app installed as splits takes the other path above and keeps 48dp.
            final int iconSize = Resources.getSystem()
                    .getIdentifier("notification_right_icon_size", "dimen", "android");
            return new Resources(am, appMetrics(), appConfiguration()) {
                @Override public int getDimensionPixelSize(int id) {
                    return id == iconSize && id != 0 ? NotifyBridge.MAX_IMAGE_EDGE
                            : super.getDimensionPixelSize(id);
                }
            };
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: app resources failed: " + t);
            return Resources.getSystem();
        }
    }

    // Give a Resources the display's size. Keeps what the Resources has that a4l does not set
    // (locale, font scale, an activity's own override) and changes only what the size decides.
    private static void applyDisplaySize(Resources r, android.content.res.Configuration cfg,
            android.util.DisplayMetrics dm) {
        android.content.res.Configuration c = new android.content.res.Configuration(r.getConfiguration());
        c.screenWidthDp = cfg.screenWidthDp;
        c.screenHeightDp = cfg.screenHeightDp;
        c.smallestScreenWidthDp = cfg.smallestScreenWidthDp;
        c.orientation = cfg.orientation;
        c.screenLayout = cfg.screenLayout;
        android.util.DisplayMetrics m = new android.util.DisplayMetrics();
        m.setTo(r.getDisplayMetrics());
        m.widthPixels = m.noncompatWidthPixels = dm.widthPixels;
        m.heightPixels = m.noncompatHeightPixels = dm.heightPixels;
        r.updateConfiguration(c, m);
    }

    // Resources.getSystem() starts with a 0x0 display. On Android it has the display's size like
    // any other Resources, and apps read Resources.getSystem().getDisplayMetrics().widthPixels
    // to size views. Called when the app's own Resources is built, after setDisplay.
    private static void sizeSystemResources() {
        try {
            applyDisplaySize(Resources.getSystem(), appConfiguration(), appMetrics());
        } catch (RuntimeException e) {
            System.err.println("acl HostBootstrap: system resources keep no display size: " + e);
        }
    }

    private static android.util.DisplayMetrics appMetrics() {
        android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
        dm.setToDefaults();
        dm.widthPixels = DISPLAY_W;   dm.heightPixels = DISPLAY_H;
        // ResourcesImpl.updateConfiguration runs CompatibilityInfo.applyToDisplayMetrics over
        // these, which for an app that supports its screen copies the noncompat sizes onto the
        // plain ones. Left at 0 they made getResources().getDisplayMetrics() say 0x0.
        dm.noncompatWidthPixels = DISPLAY_W;  dm.noncompatHeightPixels = DISPLAY_H;
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

    // android.os.VibratorManager for getSystemService("vibrator_manager"), made on first use.
    private static NoVibratorManager sVibratorManager;

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
        // Activity.attachBaseContext hands its base the autofill client. There is no autofill
        // to give it to, and no base under this context to pass it on to.
        @Override public void setAutofillClient(
                android.view.autofill.AutofillManager.AutofillClient client) {}
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
        // The APK the app's code and resources are read from; one file holds both.
        @Override public String getPackageCodePath() { return apkPath; }
        @Override public String getPackageResourcePath() { return apkPath; }
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
        // The base context is a RootContext: not a ContextWrapper, and handing every call
        // back to this context. mBase itself stays null (super(null)).
        //
        // A library that unwraps a context needs both halves. Glide's
        // RequestManagerRetriever.get() does
        // ((ContextWrapper) ctx).getBaseContext().getApplicationContext(), so a null base is a
        // NullPointerException in a RecyclerView's layout. Coil looks for a LifecycleOwner
        // with "while it is a ContextWrapper, take its base", so a base that is the
        // Application, whose own base is this context, never ends: that loop held Spotify's
        // main thread at full load. On Android the chain ends at a ContextImpl.
        private android.content.Context root;
        @Override public android.content.Context getBaseContext() {
            if (root == null) root = new RootContext(this);
            return root;
        }
        public int getDisplayId() { return 0; }
        // ContextWrapper does not forward this one, and Context's own throws "Not implemented.
        // Must override in a subclass." KeyguardManager.isDeviceLocked() asks for it.
        public int getAssociatedDisplayId() { return 0; }
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
        // The same for a context on a given display: there is one display.
        @Override public android.content.Context createDisplayContext(android.view.Display display) {
            return this;
        }
        // A null-base ContextWrapper NPEs on createConfigurationContext (delegates to mBase). androidx
        // AppCompat's ContextThemeWrapper.getResourcesInternal calls it to build config-specific
        // Resources for drawable loading; Glide's AppCompatResources.getDrawable (F-Droid's app-list
        // icons) hit this, and the uncaught NPE on acl's main Looper CRASHED the app ~1s after its
        // first frame (window "flashed and was gone"). acl has one display/config, so return self —
        // getResources() yields the app Resources, enough to resolve the drawable.
        @Override public android.content.Context createConfigurationContext(
                android.content.res.Configuration cfg) {
            return this;
        }
        // ContextWrapper delegates this to a null base on this context; ViewRootImpl.<init>
        // (Dialog/PopupWindow add path) reads getBasePackageName(), so a null-base delegation
        // would NPE. Return the app package (getOpPackageName is already overridden below).
        public String getBasePackageName() { return pkg; }
        @Override public android.os.Looper getMainLooper() {
            return android.os.Looper.getMainLooper();
        }
        // Built once and shared, like ContextImpl's (which returns LoadedApk's single instance).
        // This MUST be cached: View.getLayoutDirection() reads
        // getContext().getApplicationInfo().targetSdkVersion on every call, View.isLayoutRtl()
        // calls that, and TextView.onDraw() calls isLayoutRtl() -- so an uncached build ran a full
        // AndroidManifest.xml parse (plus File/dataDir work) once per text view per frame. Measured
        // on castlab (React Native): the UI thread spent its time in parseManifestMetaData under
        // ReactTextView.onDraw and managed 44 traversals in 70s.
        private android.content.pm.ApplicationInfo appInfo;

        @Override public synchronized android.content.pm.ApplicationInfo getApplicationInfo() {
            if (appInfo != null) return appInfo;
            android.content.pm.ApplicationInfo ai = new android.content.pm.ApplicationInfo();
            ai.packageName = pkg;
            ai.processName = pkg;   // single-process; WorkManager compares against this
            // The uid a caller of the app's own services has. A media browser service checks
            // a connecting package's uid against Binder.getCallingUid().
            ai.uid = android.os.Process.myUid();
            // Native-lib loaders (Flutter/ReLinker, System.loadLibrary internals) read these:
            // sourceDir = the APK to scan for lib/<abi>/*.so; nativeLibraryDir = where acl
            // extracted them (matches the run-time java.library.path). Absent -> new File(null) NPE.
            ai.sourceDir = apkPath;
            ai.publicSourceDir = apkPath;
            ai.dataDir = ensure(dataRoot()).getAbsolutePath();
            ai.nativeLibraryDir = new java.io.File(cacheRoot(), "lib").getAbsolutePath();
            // FLAG_EXTRACT_NATIVE_LIBS: the app's libraries are files in nativeLibraryDir, not
            // pages of the APK. A loader that finds the flag clear takes them to be loadable
            // from the APK by the system and skips nativeLibraryDir (Meta's SoLoader does).
            ai.flags = android.content.pm.ApplicationInfo.FLAG_HAS_CODE
                    | android.content.pm.ApplicationInfo.FLAG_EXTRACT_NATIVE_LIBS;
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
            // <uses-sdk>, read by the same pass. The framework branches on targetSdkVersion in
            // many places (a Notification built by an app that targets less than 24 gets its
            // views made in build()); left at 0, every app took the oldest path.
            ai.minSdkVersion = manifestMinSdk;
            ai.targetSdkVersion = manifestTargetSdk > 0 ? manifestTargetSdk : manifestMinSdk;
            appInfo = ai;
            return ai;
        }

        // Parse the <application> <meta-data name=.. value|resource=..> entries into a Bundle. A
        // meta-data value is typed (string/bool/int/float) or a resource id; put each under its
        // real type so getBoolean/getInt/getString return what the app expects. Best-effort: on any
        // parse failure return an empty (non-null) Bundle rather than null.
        // From <uses-sdk>; set by parseManifestMetaData. Android's defaults: minSdkVersion 1, and
        // a targetSdkVersion that is not given equals minSdkVersion.
        private int manifestMinSdk = 1;
        private int manifestTargetSdk;

        private android.os.Bundle parseManifestMetaData() {
            android.os.Bundle b = new android.os.Bundle();
            android.content.res.XmlResourceParser xml = null;
            try {
                xml = openBaseManifest(getResources().getAssets(), apkPath);  // apkPath: this AppContext's base
                boolean inApp = false;
                for (int ev = xml.getEventType();
                     ev != android.content.res.XmlResourceParser.END_DOCUMENT; ev = xml.next()) {
                    if (ev == android.content.res.XmlResourceParser.START_TAG) {
                        String tag = xml.getName();
                        if ("application".equals(tag)) {
                            inApp = true;
                        } else if ("uses-sdk".equals(tag)) {
                            final String NS = "http://schemas.android.com/apk/res/android";
                            manifestMinSdk = Math.max(1,
                                    xml.getAttributeIntValue(NS, "minSdkVersion", 1));
                            manifestTargetSdk = xml.getAttributeIntValue(NS, "targetSdkVersion", 0);
                        } else if (inApp && "meta-data".equals(tag)) {
                            putMetaData(b, xml, getResources());
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

        private static void putMetaData(android.os.Bundle b, android.content.res.XmlResourceParser x,
                                        android.content.res.Resources res) {
            final String NS = "http://schemas.android.com/apk/res/android";
            String name = x.getAttributeValue(NS, "name");
            if (name == null) return;
            // android:resource wins over android:value (the framework treats it as an int res id).
            int resId = x.getAttributeResourceValue(NS, "resource", 0);
            if (resId != 0) { b.putInt(name, resId); return; }
            // A reference-valued android:value (android:value="@integer/google_play_services_version")
            // comes back from getAttributeValue as null/"@id", not the resolved value, so acl used to
            // drop it -- and a missing com.google.android.gms.version makes GoogleApiAvailability throw
            // GooglePlayServicesMissingManifestValueException (a hard config error, not the graceful
            // SERVICE_MISSING), which crashed Pandora's Cast module in Main.onCreate. Resolve the
            // reference to its real typed value.
            int valResId = res != null ? x.getAttributeResourceValue(NS, "value", 0) : 0;
            if (valResId != 0) {
                try { b.putInt(name, res.getInteger(valResId)); return; } catch (Throwable ignore) {}
                try { b.putBoolean(name, res.getBoolean(valResId)); return; } catch (Throwable ignore) {}
                try { b.putString(name, res.getString(valResId)); return; } catch (Throwable ignore) {}
            }
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
        // Direct-boot / device-protected storage is a separate encrypted data dir on a device; acl
        // (a desktop host) has a single storage root, so hand back THIS context — its files/prefs land
        // in the normal app dir. The default ContextWrapper delegates to a null base and returns null,
        // which NPEs callers that immediately dereference it (Firebase's DataCollectionConfigStorage
        // and GMS measurement do createDeviceProtectedStorageContext().getSharedPreferences(...) on the
        // androidx.startup init thread, aborting that InitializationProvider).
        @Override public android.content.Context createDeviceProtectedStorageContext() { return this; }
        @Override public boolean moveSharedPreferencesFrom(android.content.Context src, String name) { return true; }
        @Override public boolean moveDatabaseFrom(android.content.Context src, String name) { return true; }
        @Override public boolean isRestricted() { return false; }  // TextView.readTextAppearance
        // Cross-app URI grants are meaningless on acl (one app, no other package to grant to) — and
        // the default ContextWrapper would NPE on the null base. No-op: an email/share intent carrying
        // a content:// attachment is serviced by the host portal, which reads the fd directly. ACRA's
        // EmailIntentSender.grantPermission calls this before startActivity for each attachment.
        @Override public void grantUriPermission(String toPackage, android.net.Uri uri, int modeFlags) {}
        @Override public void revokeUriPermission(android.net.Uri uri, int modeFlags) {}
        @Override public void revokeUriPermission(String toPackage, android.net.Uri uri, int modeFlags) {}
        // Single app, single uid: every content:// URI is this app's own, so all access is permitted.
        // The default ContextWrapper would NPE on the null base — ContentResolver.openFileDescriptor
        // checks this on its own provider's URI (ACRA opening its report attachment to email it).
        @Override public int checkUriPermission(android.net.Uri uri, int pid, int uid, int modeFlags) {
            return android.content.pm.PackageManager.PERMISSION_GRANTED;
        }
        @Override public int checkUriPermission(android.net.Uri uri, String readPermission,
                String writePermission, int pid, int uid, int modeFlags) {
            return android.content.pm.PackageManager.PERMISSION_GRANTED;
        }
        @Override public int checkCallingUriPermission(android.net.Uri uri, int modeFlags) {
            return android.content.pm.PackageManager.PERMISSION_GRANTED;
        }
        // Context.startActivity (as opposed to Activity.startActivity) does NOT route through
        // Instrumentation, so the NoopInstrumentation suppression/routing never sees it — and the
        // default ContextWrapper.startActivity would NPE on AppContext's null base (ACRA sends its
        // crash-report email via the Application context). Route the external intents acl can host
        // (email -> Email portal, http(s) VIEW -> OpenURI portal) and swallow the rest instead of
        // crashing — there is no ActivityManager to launch an arbitrary component.
        @Override public void startActivity(android.content.Intent intent) { startActivity(intent, null); }
        @Override public void startActivity(android.content.Intent intent, android.os.Bundle options) {
            if (intent == null) return;
            if (composeEmailFromIntent(intent)) return;
            try {
                android.net.Uri d = intent.getData();
                String action = intent.getAction();
                if (android.content.Intent.ACTION_VIEW.equals(action) && d != null) {
                    String sc = d.getScheme();
                    if ("http".equalsIgnoreCase(sc) || "https".equalsIgnoreCase(sc)) {
                        HostShare.openUri(d.toString());
                        return;
                    }
                }
            } catch (Throwable ignored) { }
            System.out.println("acl HostBootstrap: Context.startActivity suppressed (no external host): " + intent);
        }
        @Override public boolean canLoadUnsafeResources() { return true; }  // same, @hide
        // The single acl user (ADR-3). WorkManager's PendingIntent.getBroadcast path calls
        // Context.getUser() on a background thread; without this it delegates to the null base.
        @Override public android.os.UserHandle getUser() { return android.os.Process.myUserHandle(); }
        // A single-app host with no permission enforcement (ADR-3): everything is granted, except
        // a device the desktop keeps a decision for (HostPermissions).
        @Override public int checkPermission(String perm, int pid, int uid) {
            return HostPermissions.check(perm, pkg);
        }
        @Override public int checkSelfPermission(String perm) { return HostPermissions.check(perm, pkg); }
        @Override public int checkCallingPermission(String perm) { return HostPermissions.check(perm, pkg); }
        @Override public int checkCallingOrSelfPermission(String perm) {
            return HostPermissions.check(perm, pkg);
        }
        // The enforce forms of the same checks. ContextWrapper's forward to a base this context
        // does not have (a NullPointerException); LocationManager.getProvider() calls one.
        private void enforce(String perm, String message) {
            if (HostPermissions.check(perm, pkg)
                    != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                throw new SecurityException(
                        (message != null ? message + ": " : "") + pkg + " does not have " + perm);
            }
        }
        @Override public void enforcePermission(String perm, int pid, int uid, String message) {
            enforce(perm, message);
        }
        @Override public void enforceCallingPermission(String perm, String message) {
            enforce(perm, message);
        }
        @Override public void enforceCallingOrSelfPermission(String perm, String message) {
            enforce(perm, message);
        }
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
            if ("wallpaper".equals(name)) {
                // WallpaperManager(IWallpaperManager, Context, Handler). Apps read getWallpaperColors()
                // during Application.onCreate for dynamic (Material-You-style) theming; a null service
                // NPEs and aborts onCreate, which cascades (e.g. a later getDatabasePath on a
                // half-initialized Context). Back it with a no-op IWallpaperManager: the ctor's
                // initGlobals() stores it in the static Globals, so getWallpaperColors() → null (a
                // valid "no colors" response) instead of NPEing. No wallpaper host — set/clear drop.
                try {
                    Class<?> iwm = Class.forName("android.app.IWallpaperManager");
                    java.lang.reflect.Constructor<?> c = android.app.WallpaperManager.class
                            .getDeclaredConstructor(iwm, android.content.Context.class,
                                    android.os.Handler.class);
                    c.setAccessible(true);
                    return c.newInstance(noop(iwm), this, null);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("media_session".equals(name)) {
                // The real android.media.session.MediaSessionManager(Context) — public ctor. Its
                // ISessionManager is the seeded makeSessionManagerHandler, so MediaSession /
                // MediaSessionCompat construct successfully (the media-app entry point, and the
                // automotive media path).
                try {
                    return new android.media.session.MediaSessionManager(this);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("power".equals(name)) {
                // PowerManager(Context, IPowerManager, IThermalService, Handler) — public ctor. Its
                // IPowerManager is the seeded makePowerHandler (screen on, no battery saver, wake
                // locks no-op). Kotlin apps do getSystemService(POWER_SERVICE) as PowerManager, so a
                // null service crashes them ("null cannot be cast to non-null type
                // android.os.PowerManager" — Pandora's PandoraApp.onCreate).
                try {
                    Class<?> ipm = Class.forName("android.os.IPowerManager");
                    Class<?> its = Class.forName("android.os.IThermalService");
                    java.lang.reflect.Constructor<?> c = android.os.PowerManager.class
                            .getDeclaredConstructor(android.content.Context.class, ipm, its,
                                    android.os.Handler.class);
                    c.setAccessible(true);
                    // The 4th arg (Handler) MUST be non-null: PowerManager.WakeLock.acquire() uses
                    // the outer PowerManager's mHandler unconditionally (removeCallbacks/postDelayed
                    // for its auto-release timer). A null there NPE'd every wake-lock acquire --
                    // WorkManager acquires one for foreground work, so F-Droid's RepoUpdateWorker
                    // died and its repo list never populated. Give it the main-looper Handler.
                    android.os.Handler h = new android.os.Handler(android.os.Looper.getMainLooper());
                    return c.newInstance(this, makePowerHandler(ipm), makeThermalHandler(its), h);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("uimode".equals(name)) {
                // UiModeManager(Context) — its ctor resolves ServiceManager.getServiceOrThrow("uimode")
                // (seeded → makeUiModeHandler). Kotlin/React Native apps do getSystemService(
                // UI_MODE_SERVICE) then a non-null cast to UiModeManager; a null service crashes them
                // (RN HostFunction: "null cannot be cast to non-null type android.app.UiModeManager").
                // getCurrentModeType reports NORMAL; night mode follows the Configuration acl sets.
                try {
                    java.lang.reflect.Constructor<?> c = android.app.UiModeManager.class
                            .getDeclaredConstructor(android.content.Context.class);
                    c.setAccessible(true);
                    return c.newInstance(this);
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
            if ("servicediscovery".equals(name)) {
                // acl's clean-room android.net.nsd.NsdManager (boot classpath, backed by svc/nsd →
                // the host mDNS/DNS-SD daemon). Reflective for the same reason as connectivity/wifi.
                try {
                    return Class.forName("android.net.nsd.NsdManager").getConstructor().newInstance();
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("wifi".equals(name)) {
                // acl's clean-room android.net.wifi.WifiManager (boot classpath, backed by svc/net →
                // the host network daemon's wireless device). Reflective for the same reason as
                // connectivity: HostBootstrap compiles against the framework classes.jar, whose
                // android.net.wifi differs from acl's shim.
                try {
                    return Class.forName("android.net.wifi.WifiManager").getConstructor().newInstance();
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("vibrator".equals(name)) {
                // acl has no haptics. The framework's own android.os.NullVibrator is the canonical
                // no-vibrator instance (a complete Vibrator whose methods no-op / report unsupported),
                // returned on devices without a vibrator. Hand it back rather than null: Jetpack
                // Compose's ProvideAndroidCompositionLocals does
                // getSystemService(Vibrator.class).areAllPrimitivesSupported(...) on SDK>=31 with NO
                // null guard, so a null service NPEs inside setContent — crashing composition (the app
                // renders blank). NullVibrator is @hide, so reflect getInstance().
                try {
                    return Class.forName("android.os.NullVibrator").getMethod("getInstance").invoke(null);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("vibrator_manager".equals(name)) {
                // The API 31 way to the same vibrator. An app that asks for the manager takes
                // its default vibrator with no check for null (Sonos's setup component does,
                // and failed to load for it).
                synchronized (AppContext.class) {
                    if (sVibratorManager == null) {
                        final Object none = getSystemService("vibrator");
                        if (none instanceof android.os.Vibrator)
                            sVibratorManager = new NoVibratorManager((android.os.Vibrator) none);
                    }
                    return sVibratorManager;
                }
            }
            if ("netstats".equals(name)) {
                // acl's clean-room android.app.usage.NetworkStatsManager (boot classpath). The real
                // one lives in the connectivity apex (absent from acl's bcp), and acl has no per-uid
                // traffic accounting, so this is a no-data shim. Apps resolve it via getSystemService
                // (NETWORK_STATS_SERVICE) and often store it behind a Kotlin non-null assertion (Sonos
                // telemetry's NetworkWindowLogger does), which NPEs on a null service. Reflective for
                // the same reason as wifi/nsd: HostBootstrap compiles against the framework classes.jar
                // whose NetworkStatsManager ctor differs from acl's shim.
                try {
                    return Class.forName("android.app.usage.NetworkStatsManager")
                            .getConstructor().newInstance();
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("download".equals(name)) {
                // android.app.DownloadManager. acl has no download service/provider, but apps do
                // getSystemService(DOWNLOAD_SERVICE) then a Kotlin/DownloadManager cast — a null NPEs and
                // aborts composition (flicky's Installer.<init> does `getSystemService("download")
                // .getClass()`, blanking the app). Construct the real DownloadManager so the object
                // exists (its ctor only needs getContentResolver()/getPackageName(), which AppContext
                // provides); actual downloads need the (absent) downloads content provider. Try the
                // modern Context ctor, else the older ContentResolver+package one.
                try {
                    Class<?> dm = Class.forName("android.app.DownloadManager");
                    try {
                        java.lang.reflect.Constructor<?> c =
                                dm.getDeclaredConstructor(android.content.Context.class);
                        c.setAccessible(true);
                        return c.newInstance(this);
                    } catch (NoSuchMethodException e) {
                        java.lang.reflect.Constructor<?> c = dm.getDeclaredConstructor(
                                android.content.ContentResolver.class, String.class);
                        c.setAccessible(true);
                        return c.newInstance(getContentResolver(), getPackageName());
                    }
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
                // acl ALSO routes the real show/hide: wrap the stub's IInputMethodManager so
                // showSoftInput/hideSoftInput drive the compositor's text-input (text-input-v3) via
                // PresentDriver.setImeVisible, giving Compose/View text fields a real on-screen
                // keyboard. Both the direct showSoftInput(view) path and Compose's WindowInsets
                // requestImeShow → showSoftInput path funnel through mService.showSoftInput, so this
                // one hook catches both. Every other IPC still no-ops through the original stub.
                try {
                    java.lang.reflect.Method m = android.view.inputmethod.InputMethodManager.class
                            .getDeclaredMethod("createStubInstance", int.class, android.os.Looper.class);
                    m.setAccessible(true);
                    Object imm = m.invoke(null, 0 /* Display.DEFAULT_DISPLAY */,
                            android.os.Looper.getMainLooper());
                    try {
                        Class<?> iimm = Class.forName("com.android.internal.view.IInputMethodManager");
                        java.lang.reflect.Field svcF = android.view.inputmethod.InputMethodManager.class
                                .getDeclaredField("mService");
                        svcF.setAccessible(true);
                        final Object orig = svcF.get(imm);
                        Object wrapped = java.lang.reflect.Proxy.newProxyInstance(
                                iimm.getClassLoader(), new Class<?>[]{iimm},
                                (p, method, a) -> {
                                    final String mn = method.getName();
                                    if (mn.startsWith("showSoftInput")) PresentDriver.setImeVisible(true);
                                    else if (mn.startsWith("hideSoftInput")) PresentDriver.setImeVisible(false);
                                    // Delegate to the original stub for the return value / its state.
                                    final Object r;
                                    try {
                                        r = method.invoke(orig, a);
                                    } catch (java.lang.reflect.InvocationTargetException e) {
                                        throw e.getCause();
                                    }
                                    // No Android input method is installed: the lists of them
                                    // are empty, not null. InputMethodManager hands them to the
                                    // app as they come, and apps iterate them unguarded.
                                    if (r == null && method.getReturnType() == java.util.List.class)
                                        return new java.util.ArrayList<Object>();
                                    return r;
                                });
                        svcF.set(imm, wrapped);  // final field; setAccessible bypasses (as elsewhere)
                    } catch (Throwable ignored) {}
                    return imm;
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
            if ("media_router".equals(name)) {
                // android.media.MediaRouter over a no-op IMediaRouterService (seeded in the
                // ServiceManager): the device's own audio route and nothing to cast to. Never
                // null: androidx.mediarouter calls it unguarded, and a MediaRouteButton in a
                // layout (a Cast button) fails the whole inflate without it.
                Object made = madeManager(name);
                if (made != null) return made;
                try {
                    java.lang.reflect.Constructor<?> c = android.media.MediaRouter.class
                            .getDeclaredConstructor(android.content.Context.class);
                    c.setAccessible(true);
                    return keepManager(name, c.newInstance(this));
                } catch (Throwable t) {
                    Throwable cause = t instanceof java.lang.reflect.InvocationTargetException
                            && t.getCause() != null ? t.getCause() : t;
                    android.util.Log.w("acl", "MediaRouter: " + cause, cause);
                    return null;
                }
            }
            if ("appwidget".equals(name)) {
                // AppWidgetManager(Context, IAppWidgetService). No home screen hosts widgets:
                // the app has none placed, and updates to them are dropped. Never null, for
                // the reason given for UsageStatsManager below.
                Object made = madeManager(name);
                if (made != null) return made;
                try {
                    Class<?> iaw = Class.forName("com.android.internal.appwidget.IAppWidgetService");
                    Object svc = java.lang.reflect.Proxy.newProxyInstance(iaw.getClassLoader(),
                            new Class<?>[]{iaw}, (p, m, a) -> {
                                final Class<?> rt = m.getReturnType();
                                if (rt == int[].class) return new int[0];
                                if (rt.getName().equals("android.content.pm.ParceledListSlice"))
                                    return rt.getConstructor(java.util.List.class)
                                            .newInstance(new java.util.ArrayList<Object>());
                                return defaultValue(rt);
                            });
                    java.lang.reflect.Constructor<?> c = android.appwidget.AppWidgetManager.class
                            .getDeclaredConstructor(android.content.Context.class, iaw);
                    c.setAccessible(true);
                    return keepManager(name, c.newInstance(this, svc));
                } catch (Throwable t) {
                    android.util.Log.w("acl", "AppWidgetManager: " + t);
                    return null;
                }
            }
            if ("account".equals(name)) {
                // AccountManager over the app's own accounts (AppAccounts). One per process:
                // it holds the accounts. Never null: AccountManager.get() is declared
                // non-null and an app that injects it fails on null.
                Object made = madeManager(name);
                if (made != null) return made;
                final Object am = AppAccounts.manager(this, getDataDir());
                return am != null ? keepManager(name, am) : null;
            }
            if ("shortcut".equals(name)) {
                // ShortcutManager(Context, IShortcutService). There is no launcher to publish
                // shortcuts to: sets and updates are accepted and dropped, queries are empty.
                // Never null, for the reason given for UsageStatsManager below.
                Object made = madeManager(name);
                if (made != null) return made;
                try {
                    Class<?> iss = Class.forName("android.content.pm.IShortcutService");
                    Object svc = java.lang.reflect.Proxy.newProxyInstance(iss.getClassLoader(),
                            new Class<?>[]{iss}, (p, m, a) -> {
                                final Class<?> rt = m.getReturnType();
                                // ShortcutManager calls getList() on what the service returns.
                                if (rt.getName().equals("android.content.pm.ParceledListSlice"))
                                    return rt.getConstructor(java.util.List.class)
                                            .newInstance(new java.util.ArrayList<Object>());
                                final String mn = m.getName();
                                if (mn.equals("getMaxShortcutCountPerActivity")) return 15;
                                if (rt == boolean.class && (mn.startsWith("set")
                                        || mn.startsWith("add") || mn.startsWith("update")))
                                    return Boolean.TRUE;
                                return defaultValue(rt);
                            });
                    java.lang.reflect.Constructor<?> c = android.content.pm.ShortcutManager.class
                            .getDeclaredConstructor(android.content.Context.class, iss);
                    c.setAccessible(true);
                    return keepManager(name, c.newInstance(this, svc));
                } catch (Throwable t) {
                    android.util.Log.w("acl", "ShortcutManager: " + t);
                    return null;
                }
            }
            if ("usagestats".equals(name)) {
                // UsageStatsManager(Context, IUsageStatsManager). a4l keeps no usage history:
                // queries come back empty and the app is in the active standby bucket. A null
                // manager is worse than an empty one: an app that injects it (Pandora's Dagger
                // graph, a non-null @Provides) fails to create whatever needs it.
                Object made = madeManager(name);
                if (made != null) return made;
                try {
                    Class<?> ius = Class.forName("android.app.usage.IUsageStatsManager");
                    Object svc = java.lang.reflect.Proxy.newProxyInstance(ius.getClassLoader(),
                            new Class<?>[]{ius}, (p, m, a) -> {
                                final Class<?> rt = m.getReturnType();
                                if (m.getName().equals("getAppStandbyBucket") && rt == int.class)
                                    return android.app.usage.UsageStatsManager.STANDBY_BUCKET_ACTIVE;
                                if (rt.getName().equals("android.content.pm.ParceledListSlice"))
                                    return rt.getConstructor(java.util.List.class)
                                            .newInstance(new java.util.ArrayList<Object>());
                                return defaultValue(rt);
                            });
                    java.lang.reflect.Constructor<?> c = android.app.usage.UsageStatsManager.class
                            .getDeclaredConstructor(android.content.Context.class, ius);
                    c.setAccessible(true);
                    return keepManager(name, c.newInstance(this, svc));
                } catch (Throwable t) {
                    android.util.Log.w("acl", "UsageStatsManager: " + t);
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
                                // The host user is past first-unlock and running. Default here is false,
                                // which reads as Direct Boot Mode: FirebaseApp then POSTPONES init
                                // ("Device in Direct Boot Mode: postponing initialization"), leaving
                                // Firebase Performance's singleton null so its background gauge task NPEs
                                // -- and Pandora's StatsUncaughtExceptionHandler System.exit(0)s the
                                // process on that uncaught exception. Adobe's SDK bails the same way.
                                // Report the user unlocked/running so credential-locked-gated init runs.
                                if ((mn.equals("isUserUnlocked") || mn.equals("isUserUnlockingOrUnlocked")
                                        || mn.equals("isUserRunning") || mn.equals("isUserRunningOrStopping"))
                                        && m.getReturnType() == boolean.class)
                                    return Boolean.TRUE;
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
            if ("appops".equals(name)) {
                // AppOpsManager(Context, IAppOpsService). Settings.canDrawOverlays() ->
                // AppOpsManager.checkOpNoThrow -> IAppOpsService.checkOperation; a null service NPEs
                // (Telegram's onStart -> GroupCallPip.updateVisibility -> canDrawOverlays). Back it
                // with a proxy: checkOperation etc. return MODE_ALLOWED, which is 0 == the default int,
                // so defaultValue answers every op-mode query as allowed (a desktop app may draw
                // overlays / note ops freely). noteOp/startOp likewise default; no per-op policy here.
                try {
                    Class<?> iao = Class.forName("com.android.internal.app.IAppOpsService");
                    Object svc = java.lang.reflect.Proxy.newProxyInstance(iao.getClassLoader(),
                            new Class<?>[]{iao}, (p, m, a) -> defaultValue(m.getReturnType()));
                    java.lang.reflect.Constructor<?> c = android.app.AppOpsManager.class
                            .getDeclaredConstructor(android.content.Context.class, iao);
                    c.setAccessible(true);
                    return c.newInstance(this, svc);
                } catch (Throwable t) {
                    return null;
                }
            }
            if ("telephony_registry".equals(name)) {
                // TelephonyManager.registerTelephonyCallback() takes its registry from here and
                // throws IllegalStateException("telephony service is null.") without one, which
                // ended Waze's Application.onCreate part way. The manager's ITelephonyRegistry
                // is the no-op "telephony.registry" seeded below: a callback registers and, with
                // no telephony on the host, is never called.
                try {
                    return new android.telephony.TelephonyRegistryManager(this);
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
            if (serviceClass == android.app.usage.UsageStatsManager.class) return "usagestats";
            if (serviceClass == android.accounts.AccountManager.class) return "account";
            if (serviceClass == android.content.pm.ShortcutManager.class) return "shortcut";
            if (serviceClass == android.appwidget.AppWidgetManager.class) return "appwidget";
            if (serviceClass == android.media.MediaRouter.class) return "media_router";
            if (serviceClass == android.app.job.JobScheduler.class) return "jobscheduler";
            if (serviceClass == android.view.accessibility.AccessibilityManager.class) return "accessibility";
            if (serviceClass == android.view.autofill.AutofillManager.class) return "autofill";
            if (serviceClass == android.content.ClipboardManager.class) return "clipboard";
            if (serviceClass == android.app.NotificationManager.class) return "notification";
            if ("android.hardware.display.DisplayManager".equals(serviceClass.getName())) return "display";
            // ConnectivityManager compared by name (off this compile classpath, see above).
            if ("android.net.ConnectivityManager".equals(serviceClass.getName())) return "connectivity";
            if ("android.net.wifi.WifiManager".equals(serviceClass.getName())) return "wifi";
            if ("android.net.nsd.NsdManager".equals(serviceClass.getName())) return "servicediscovery";
            if ("android.app.usage.NetworkStatsManager".equals(serviceClass.getName())) return "netstats";
            if ("android.os.Vibrator".equals(serviceClass.getName())) return "vibrator";
            if ("android.os.VibratorManager".equals(serviceClass.getName())) return "vibrator_manager";
            if ("android.bluetooth.BluetoothManager".equals(serviceClass.getName())) return "bluetooth";
            if (serviceClass == android.os.storage.StorageManager.class) return "storage";
            if (serviceClass == android.os.UserManager.class) return "user";
            if (serviceClass == android.app.AppOpsManager.class) return "appops";
            if (serviceClass == android.hardware.SensorManager.class) return "sensor";
            if (serviceClass == android.location.LocationManager.class) return "location";
            if (serviceClass == android.view.accessibility.CaptioningManager.class) return "captioning";
            if (serviceClass == android.os.BatteryManager.class) return "batterymanager";
            // Kotlin getSystemService(UiModeManager::class.java)!! (React Native reads UI_MODE_SERVICE)
            // and getSystemService(WallpaperManager::class.java)!! (dynamic theming) — a null name NPEs.
            if (serviceClass == android.app.UiModeManager.class) return "uimode";
            if (serviceClass == android.os.PowerManager.class) return "power";
            if (serviceClass == android.media.session.MediaSessionManager.class) return "media_session";
            if (serviceClass == android.app.WallpaperManager.class) return "wallpaper";
            if (serviceClass == android.telephony.TelephonyManager.class) return "phone";
            if (serviceClass == android.telephony.TelephonyRegistryManager.class)
                return "telephony_registry";
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

        // Managers built once and handed out again, as Android hands out one per Context
        // (appwidget, shortcut, usagestats).
        private final java.util.Map<String, Object> madeManagers = new java.util.HashMap<>();
        private Object madeManager(String name) {
            synchronized (madeManagers) { return madeManagers.get(name); }
        }
        private Object keepManager(String name, Object manager) {
            synchronized (madeManagers) {
                Object first = madeManagers.get(name);
                if (first != null) return first;
                madeManagers.put(name, manager);
                return manager;
            }
        }

        // The app's own services run in this process (AppServices): a4l has no ActivityManager
        // (ADR-3), and ContextWrapper would hand these to the null base. Another app's service
        // is not found: start returns null, bind returns false.
        @Override public android.content.ComponentName startService(android.content.Intent service) {
            return AppServices.start(this, service);
        }
        @Override public android.content.ComponentName startForegroundService(android.content.Intent service) {
            return AppServices.start(this, service);
        }
        @Override public boolean stopService(android.content.Intent service) {
            return AppServices.stop(this, service);
        }
        @Override public boolean bindService(android.content.Intent service,
                android.content.ServiceConnection conn, int flags) {
            return AppServices.bind(this, service, conn, flags, null);
        }
        @Override public boolean bindService(android.content.Intent service, int flags,
                java.util.concurrent.Executor executor, android.content.ServiceConnection conn) {
            return AppServices.bind(this, service, conn, flags, executor);
        }
        @Override public boolean bindIsolatedService(android.content.Intent service, int flags,
                String instanceName, java.util.concurrent.Executor executor,
                android.content.ServiceConnection conn) {
            return AppServices.bind(this, service, conn, flags, executor);
        }
        @Override public boolean bindServiceAsUser(android.content.Intent service,
                android.content.ServiceConnection conn, int flags, android.os.UserHandle user) {
            return AppServices.bind(this, service, conn, flags, null);
        }
        @Override public void unbindService(android.content.ServiceConnection conn) {
            AppServices.unbind(conn);
        }

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
        // at $XDG_DATA_HOME/a4l/<pkg>, caches at $XDG_CACHE_HOME/a4l/<pkg> (defaults per spec:
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
            return new java.io.File(xdgHome("XDG_DATA_HOME", ".local/share"), "a4l/" + pkg);
        }
        private java.io.File cacheRoot() {
            return new java.io.File(xdgHome("XDG_CACHE_HOME", ".cache"), "a4l/" + pkg);
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
        // The same for the app's shared media and its expansion files.
        @Override public java.io.File[] getExternalMediaDirs() {
            return new java.io.File[] { ensure(new java.io.File(dataRoot(), "external/media")) };
        }
        @Override public java.io.File getObbDir() {
            return ensure(new java.io.File(dataRoot(), "external/obb"));
        }
        @Override public java.io.File[] getObbDirs() {
            return new java.io.File[] { getObbDir() };
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
        // ContextWrapper.databaseList() delegates to mBase, which is null here, so it NPEs. F-Droid's
        // ContentProviderMigrator.needsMigration() calls it during DB pre-population; the NPE is
        // uncaught and its crash reporter tears the process down before the UI. List the databases/
        // dir that getDatabasePath() writes into; empty (never null) when nothing has been created.
        @Override public String[] databaseList() {
            String[] names = new java.io.File(dataRoot(), "databases").list();
            return names != null ? names : new String[0];
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
    // Settings.Secure.ANDROID_ID: 64 random bits as 16 hex digits, made on first use and kept in
    // the app's data directory. It is per app and per user, as on Android 8 and later, and it
    // names nothing about the machine. Clearing the app's data makes a new one.
    private static String sAndroidId;
    static synchronized String androidId() {
        if (sAndroidId != null) return sAndroidId;
        java.io.File f = null;
        try {
            AppContext c = sAppContext;
            if (c != null) f = new java.io.File(c.getDataDir(), "android_id");
            if (f != null && f.isFile() && f.length() == 16) {
                String s = new String(java.nio.file.Files.readAllBytes(f.toPath()),
                        java.nio.charset.StandardCharsets.US_ASCII);
                if (s.matches("[0-9a-f]{16}")) return sAndroidId = s;
            }
        } catch (Throwable ignored) {}
        String id = String.format("%016x", new java.security.SecureRandom().nextLong());
        try {
            if (f != null) {
                java.nio.file.Files.write(f.toPath(),
                        id.getBytes(java.nio.charset.StandardCharsets.US_ASCII));
                f.setReadable(false, false);  // the owner alone reads it
                f.setReadable(true, true);
            }
        } catch (Throwable ignored) {}  // not kept: the id then lasts for this run only
        return sAndroidId = id;
    }

    // InitializationProvider re-reads its declared Initializers from getProviderInfo(cn, GET_META_DATA).
    static final java.util.Map<String, android.os.Bundle> sProviderMeta =
            new java.util.HashMap<String, android.os.Bundle>();

    // Per-<service> <meta-data>, keyed by fully-qualified service class name, parsed from the
    // manifest in installContentProviders and re-served via AppPackageManager.getServiceInfo. This
    // is how Firebase's ComponentDiscovery works: it reads getServiceInfo(ComponentDiscoveryService,
    // GET_META_DATA).metaData to find the ComponentRegistrar classes (firebase-messaging, etc.).
    static final java.util.Map<String, android.os.Bundle> sServiceMeta =
            new java.util.HashMap<String, android.os.Bundle>();

    // The app's declared services, fully-qualified class name -> its intent-filter actions, from
    // the manifest (installContentProviders). AppServices runs only these.
    static final java.util.Map<String, java.util.List<String>> sServiceActions =
            java.util.Collections.synchronizedMap(
                    new java.util.LinkedHashMap<String, java.util.List<String>>());

    // A <service> or <receiver> the manifest declares, as PackageManager's intent queries report it.
    static final class Declared {
        final boolean service;
        final String permission;
        final boolean enabled;
        final boolean exported;
        final java.util.List<String> actions = new java.util.ArrayList<String>();
        Declared(boolean service, String permission, boolean enabled, boolean exported) {
            this.service = service;
            this.permission = permission;
            this.enabled = enabled;
            this.exported = exported;
        }
    }
    // Fully-qualified class name -> the declaration. Written once by installContentProviders.
    static final java.util.Map<String, Declared> sDeclared =
            java.util.Collections.synchronizedMap(new java.util.LinkedHashMap<String, Declared>());
    // The manifest's components are in sDeclared: one that is not there is not in the app.
    static volatile boolean sManifestRead;

    // Installed providers keyed by each declared authority — AppContentResolver.acquireProvider
    // resolves content:// lookups here so query/insert/openFile reach the live provider instead of
    // the no-op stub. Populated by installContentProviders after a successful attachInfo/onCreate.
    static final java.util.Map<String, android.content.ContentProvider> sProviderByAuthority =
            new java.util.HashMap<String, android.content.ContentProvider>();

    // full activity class -> full parentActivityName, parsed once from AndroidManifest.xml. Backs
    // PackageManager.getActivityInfo(...).parentActivityName so onSupportNavigateUp / the Up arrow work.
    // full activity class -> its android:theme resource, and the <application>'s theme, parsed
    // once from AndroidManifest.xml.
    private static java.util.Map<String, Integer> sActivityTheme;
    private static int sManifestAppTheme;
    /** The theme the manifest gives this Activity: its own, else the application's, else 0. */
    static synchronized int manifestThemeFor(String activityClass) {
        if (sActivityTheme == null) {
            java.util.Map<String, Integer> m = new java.util.HashMap<>();
            AppContext c = sAppContext;
            android.content.res.XmlResourceParser xml = null;
            try {
                if (c != null) {
                    final String NS = "http://schemas.android.com/apk/res/android";
                    final String p = c.getPackageName();
                    xml = openBaseManifest(c.getResources().getAssets(),
                            c.getApplicationInfo() != null ? c.getApplicationInfo().sourceDir : null);
                    for (int ev = xml.getEventType();
                         ev != android.content.res.XmlResourceParser.END_DOCUMENT; ev = xml.next()) {
                        if (ev != android.content.res.XmlResourceParser.START_TAG) continue;
                        final int theme = xml.getAttributeResourceValue(NS, "theme", 0);
                        if ("application".equals(xml.getName())) {
                            sManifestAppTheme = theme;
                        } else if ("activity".equals(xml.getName())) {
                            String name = xml.getAttributeValue(NS, "name");
                            if (name == null) continue;
                            if (theme != 0) m.put(normalizeClass(name, p), theme);
                            if (xml.getAttributeBooleanValue(NS, "noHistory", false))
                                sNoHistory.add(normalizeClass(name, p));
                        }
                    }
                }
            } catch (Throwable t) {
                System.err.println("acl HostBootstrap: activity theme parse failed: " + t);
            } finally {
                if (xml != null) xml.close();
            }
            sActivityTheme = m;
        }
        final Integer own = sActivityTheme.get(activityClass);
        return own != null ? own : sManifestAppTheme;
    }

    // Activities declared android:noHistory="true"; filled by manifestThemeFor's manifest pass.
    private static final java.util.Set<String> sNoHistory = new java.util.HashSet<String>();
    /** True for an Activity the task does not keep once another is started over it. */
    static synchronized boolean noHistory(android.app.Activity a) {
        manifestThemeFor(a.getClass().getName());  // makes sure the manifest has been read
        if (sNoHistory.contains(a.getClass().getName())) return true;
        android.content.Intent i = a.getIntent();
        return i != null
                && (i.getFlags() & android.content.Intent.FLAG_ACTIVITY_NO_HISTORY) != 0;
    }

    // The class of the first <activity>, or the target of the first <activity-alias>, with a
    // MAIN/LAUNCHER intent filter. "" once the manifest has been read and names none.
    private static String sLauncherActivity;
    static synchronized String launcherActivityName() {
        if (sLauncherActivity != null) return sLauncherActivity.isEmpty() ? null : sLauncherActivity;
        String found = "";
        final AppContext c = sAppContext;
        if (c == null) return null;
        android.content.res.XmlResourceParser xml = null;
        try {
            final String NS = "http://schemas.android.com/apk/res/android";
            final String p = c.getPackageName();
            xml = openBaseManifest(c.getResources().getAssets(),
                    c.getApplicationInfo() != null ? c.getApplicationInfo().sourceDir : null);
            String current = null;
            boolean main = false, launcher = false;
            for (int ev = xml.getEventType();
                 ev != android.content.res.XmlResourceParser.END_DOCUMENT; ev = xml.next()) {
                if (ev != android.content.res.XmlResourceParser.START_TAG) continue;
                final String tag = xml.getName();
                if ("activity".equals(tag)) {
                    current = normalizeClass(xml.getAttributeValue(NS, "name"), p);
                } else if ("activity-alias".equals(tag)) {
                    current = xml.getAttributeBooleanValue(NS, "enabled", true)
                            ? normalizeClass(xml.getAttributeValue(NS, "targetActivity"), p)
                            : null;
                } else if ("service".equals(tag) || "receiver".equals(tag)
                        || "provider".equals(tag)) {
                    current = null;
                } else if ("intent-filter".equals(tag)) {
                    main = launcher = false;
                } else if ("action".equals(tag)) {
                    main |= android.content.Intent.ACTION_MAIN
                            .equals(xml.getAttributeValue(NS, "name"));
                } else if ("category".equals(tag)) {
                    launcher |= android.content.Intent.CATEGORY_LAUNCHER
                            .equals(xml.getAttributeValue(NS, "name"));
                }
                if (current != null && main && launcher) {
                    found = current;
                    break;
                }
            }
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: launcher activity parse failed: " + t);
        } finally {
            if (xml != null) xml.close();
        }
        sLauncherActivity = found;
        return found.isEmpty() ? null : found;
    }

    // The <application>'s android:label, a string or a string resource. "" once the manifest
    // has been read and gives none.
    private static String sAppLabel;
    static synchronized String appLabel() {
        if (sAppLabel != null) return sAppLabel.isEmpty() ? null : sAppLabel;
        final AppContext c = sAppContext;
        if (c == null) return null;
        String found = "";
        android.content.res.XmlResourceParser xml = null;
        try {
            final String NS = "http://schemas.android.com/apk/res/android";
            xml = openBaseManifest(c.getResources().getAssets(),
                    c.getApplicationInfo() != null ? c.getApplicationInfo().sourceDir : null);
            for (int ev = xml.getEventType();
                 ev != android.content.res.XmlResourceParser.END_DOCUMENT; ev = xml.next()) {
                if (ev != android.content.res.XmlResourceParser.START_TAG
                        || !"application".equals(xml.getName())) continue;
                final int res = xml.getAttributeResourceValue(NS, "label", 0);
                final CharSequence label = res != 0 ? c.getResources().getText(res)
                        : xml.getAttributeValue(NS, "label");
                if (label != null) found = label.toString();
                break;
            }
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: app label parse failed: " + t);
        } finally {
            if (xml != null) xml.close();
        }
        sAppLabel = found;
        return found.isEmpty() ? null : found;
    }

    private static java.util.Map<String, String> sParentActivity;
    private static String normalizeClass(String name, String pkg) {
        if (name == null) return null;
        if (name.startsWith(".")) return pkg + name;
        return name.indexOf('.') < 0 ? pkg + "." + name : name;
    }
    static synchronized String parentActivityName(String activityClass) {
        if (sParentActivity == null) {
            java.util.Map<String, String> m = new java.util.HashMap<>();
            AppContext c = sAppContext;
            android.content.res.XmlResourceParser xml = null;
            try {
                if (c != null) {
                    final String NS = "http://schemas.android.com/apk/res/android";
                    final String p = c.getPackageName();
                    xml = openBaseManifest(c.getResources().getAssets(),
                            c.getApplicationInfo() != null ? c.getApplicationInfo().sourceDir : null);
                    for (int ev = xml.getEventType();
                         ev != android.content.res.XmlResourceParser.END_DOCUMENT; ev = xml.next()) {
                        if (ev == android.content.res.XmlResourceParser.START_TAG
                                && "activity".equals(xml.getName())) {
                            String name = xml.getAttributeValue(NS, "name");
                            String parent = xml.getAttributeValue(NS, "parentActivityName");
                            if (name != null && parent != null)
                                m.put(normalizeClass(name, p), normalizeClass(parent, p));
                        }
                    }
                }
            } catch (Throwable t) {
                System.err.println("acl HostBootstrap: parentActivityName parse failed: " + t);
            } finally {
                if (xml != null) xml.close();
            }
            sParentActivity = m;
        }
        return sParentActivity.get(activityClass);
    }

    // True if the Intent is an email launch acl can service via the xdg Email portal: an ACTION_SENDTO
    // to a mailto: URI (the canonical compose-email intent), or an ACTION_SEND[_MULTIPLE] carrying
    // EXTRA_EMAIL recipients or a message/rfc822 type. Drives BOTH the PackageManager resolution (so an
    // app that checks resolveActivity()!=null before offering "email us" / sending a crash report finds
    // a handler) and the actual routing in NoopInstrumentation.maybeComposeEmail.
    static boolean isEmailIntent(android.content.Intent intent) {
        if (intent == null) return false;
        try {
            String action = intent.getAction();
            android.net.Uri data = intent.getData();
            if (android.content.Intent.ACTION_SENDTO.equals(action)
                    && data != null && "mailto".equalsIgnoreCase(data.getScheme())) return true;
            if (android.content.Intent.ACTION_SEND.equals(action)
                    || android.content.Intent.ACTION_SEND_MULTIPLE.equals(action)) {
                if ("message/rfc822".equals(intent.getType())) return true;
                String[] to = intent.getStringArrayExtra(android.content.Intent.EXTRA_EMAIL);
                if (to != null && to.length > 0) return true;
            }
        } catch (Throwable ignored) { }
        return false;
    }

    // Route an email-launch Intent to the host's default mail client via the xdg Email portal
    // (org.freedesktop.portal.Email.ComposeEmail). Recipient/subject/body come from the mailto: URI
    // and/or the intent extras (extras win — they are unencoded). Returns true if the intent was an
    // email launch (consumed), regardless of whether the portal succeeded; false to fall through.
    static boolean composeEmailFromIntent(android.content.Intent intent) {
        if (!isEmailIntent(intent)) return false;
        try {
            android.net.Uri data = intent.getData();
            String address = "";
            String uSubject = null, uBody = null;
            if (data != null && "mailto".equalsIgnoreCase(data.getScheme())) {
                String ssp = data.getSchemeSpecificPart();  // "addr?subject=..&body=.."
                if (ssp != null) {
                    int q = ssp.indexOf('?');
                    address = (q >= 0 ? ssp.substring(0, q) : ssp).trim();
                    if (q >= 0) {
                        for (String kv : ssp.substring(q + 1).split("&")) {
                            int eq = kv.indexOf('=');
                            if (eq < 0) continue;
                            String k = kv.substring(0, eq);
                            String v = android.net.Uri.decode(kv.substring(eq + 1));
                            if ("subject".equalsIgnoreCase(k)) uSubject = v;
                            else if ("body".equalsIgnoreCase(k)) uBody = v;
                        }
                    }
                }
            }
            if (address.isEmpty()) {
                String[] to = intent.getStringArrayExtra(android.content.Intent.EXTRA_EMAIL);
                if (to != null && to.length > 0 && to[0] != null) address = to[0];
            }
            String subject = intent.getStringExtra(android.content.Intent.EXTRA_SUBJECT);
            if (subject == null) subject = uSubject;
            CharSequence text = intent.getCharSequenceExtra(android.content.Intent.EXTRA_TEXT);
            String body = text != null ? text.toString() : uBody;
            int[] fds = openAttachmentFds(intent);  // EXTRA_STREAM content:// -> the portal's attachment_fds
            boolean ok = HostShare.composeEmail(address, subject, body, fds);
            System.out.println("acl HostBootstrap: email intent -> xdg Email portal ("
                    + (ok ? "composed" : "portal unavailable") + "): to=" + address
                    + (subject != null ? " subject=\"" + subject + "\"" : "")
                    + (fds.length > 0 ? " +" + fds.length + " attachment(s)" : ""));
            return true;
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: composeEmailFromIntent failed: " + t);
            return true;  // still an email intent; don't fall through to Share/clipboard
        }
    }

    // Open every EXTRA_STREAM attachment of a SEND/SEND_MULTIPLE email intent to a DETACHED fd, for the
    // portal's attachment_fds. ACRA ships its report as a content:// file (its AcraContentProvider), so
    // without this the composed mail is empty. Each fd's ownership is transferred to the native (which
    // adopts + closes it); openFileDescriptor resolves the content:// via the app's ContentResolver.
    private static int[] openAttachmentFds(android.content.Intent intent) {
        java.util.List<android.net.Uri> streams = new java.util.ArrayList<>();
        try {
            android.os.Parcelable one = intent.getParcelableExtra(android.content.Intent.EXTRA_STREAM);
            if (one instanceof android.net.Uri) streams.add((android.net.Uri) one);
            java.util.ArrayList<android.os.Parcelable> many =
                    intent.getParcelableArrayListExtra(android.content.Intent.EXTRA_STREAM);
            if (many != null)
                for (android.os.Parcelable p : many)
                    if (p instanceof android.net.Uri) streams.add((android.net.Uri) p);
        } catch (Throwable ignored) { }
        if (streams.isEmpty() || sAppContext == null) return new int[0];
        java.util.List<Integer> fds = new java.util.ArrayList<>();
        android.content.ContentResolver cr = sAppContext.getContentResolver();
        for (android.net.Uri u : streams) {
            try {
                android.os.ParcelFileDescriptor pfd = cr.openFileDescriptor(u, "r");
                if (pfd != null) fds.add(pfd.detachFd());  // ownership -> native
            } catch (Throwable t) {
                System.err.println("acl HostBootstrap: attachment open failed (" + u + "): " + t);
            }
        }
        int[] out = new int[fds.size()];
        for (int i = 0; i < out.length; i++) out[i] = fds.get(i);
        return out;
    }

    // A synthetic ResolveInfo naming acl's portal-backed email handler, so Intent.resolveActivity() and
    // queryIntentActivities() report a mail client for an email launch (there is no email Activity on
    // acl -- the launch is serviced by the xdg Email portal via composeEmailFromIntent).
    static android.content.pm.ResolveInfo emailResolveInfo() {
        android.content.pm.ResolveInfo ri = new android.content.pm.ResolveInfo();
        android.content.pm.ActivityInfo ai = new android.content.pm.ActivityInfo();
        ai.packageName = "org.acl.email";
        ai.name = "org.acl.email.ComposeActivity";
        ai.exported = true;
        ai.enabled = true;
        android.content.pm.ApplicationInfo app = new android.content.pm.ApplicationInfo();
        app.packageName = "org.acl.email";
        ai.applicationInfo = app;
        ri.activityInfo = ai;
        ri.resolvePackageName = "org.acl.email";
        return ri;
    }

    // An ACTION_VIEW of an http or https URI that names no package and no component: what an
    // app sends to open a link in the user's browser.
    static boolean isWebLinkIntent(android.content.Intent intent) {
        if (intent == null || !android.content.Intent.ACTION_VIEW.equals(intent.getAction())
                || intent.getComponent() != null || intent.getPackage() != null) {
            return false;
        }
        final android.net.Uri d = intent.getData();
        final String scheme = d != null ? d.getScheme() : null;
        return "http".equalsIgnoreCase(scheme) || "https".equalsIgnoreCase(scheme);
    }

    // The handler such an intent resolves to. There is no browser Activity in the process: the
    // start goes to the host's handler for the URI (NoopInstrumentation), and this entry is
    // what tells an app that asks first that there is one.
    static android.content.pm.ResolveInfo webLinkResolveInfo() {
        android.content.pm.ResolveInfo ri = new android.content.pm.ResolveInfo();
        android.content.pm.ActivityInfo ai = new android.content.pm.ActivityInfo();
        ai.packageName = "org.acl.browser";
        ai.name = "org.acl.browser.ViewActivity";
        ai.exported = true;
        ai.enabled = true;
        android.content.pm.ApplicationInfo app = new android.content.pm.ApplicationInfo();
        app.packageName = "org.acl.browser";
        ai.applicationInfo = app;
        ri.activityInfo = ai;
        ri.resolvePackageName = "org.acl.browser";
        return ri;
    }

    static final class AppPackageManager extends StubPackageManager {
        private final String pkg;
        AppPackageManager(String pkg) { this.pkg = pkg; }
        // The one package is this app; the store is not asked about a name the app made up.
        @Override public int checkPermission(String permName, String packageName) {
            if (pkg.equals(packageName)) return HostPermissions.check(permName, pkg);
            return HostPermissions.device(permName) == null
                    ? android.content.pm.PackageManager.PERMISSION_GRANTED
                    : android.content.pm.PackageManager.PERMISSION_DENIED;
        }
        // A <property> the manifest does not declare is a NameNotFoundException, which is
        // what callers catch: Environment.isExternalStorageLegacy() reads one and takes the
        // exception for "not set". The base class throws UnsupportedOperationException, which
        // nobody catches. a4l reads no <property> elements, so none is ever found.
        @Override public android.content.pm.PackageManager.Property getProperty(
                String propertyName, String packageName)
                throws android.content.pm.PackageManager.NameNotFoundException {
            throw new android.content.pm.PackageManager.NameNotFoundException(propertyName);
        }
        @Override public android.content.pm.PackageManager.Property getProperty(
                String propertyName, android.content.ComponentName component)
                throws android.content.pm.PackageManager.NameNotFoundException {
            throw new android.content.pm.PackageManager.NameNotFoundException(propertyName);
        }
        // One app in the process, so every caller is this package whatever uid
        // Binder.getCallingUid() gives. MediaBrowserService checks a connecting client's
        // package against these and takes null for a caller it cannot name.
        @Override public String[] getPackagesForUid(int uid) { return new String[]{pkg}; }
        @Override public String getNameForUid(int uid) { return pkg; }
        // The app's own launch intent, as a launcher would start it. A media app builds its
        // notification's content intent from this, and PendingIntent.getActivity throws on null.
        @Override public android.content.Intent getLaunchIntentForPackage(String packageName) {
            if (packageName == null || !packageName.equals(pkg)) return null;
            final String cls = launcherActivityName();
            if (cls == null) return null;
            return new android.content.Intent(android.content.Intent.ACTION_MAIN)
                    .addCategory(android.content.Intent.CATEGORY_LAUNCHER)
                    .setClassName(pkg, cls)
                    .setFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
        }
        // Report acl's portal-backed email handler for an email launch (Intent.resolveActivity() /
        // queryIntentActivities()) so callers that gate on a mail client being present proceed; the
        // launch itself is routed to the xdg Email portal. Everything else stays unresolved (null/empty),
        // matching StubPackageManager -- acl hosts no other external Activities.
        @Override public android.content.pm.ResolveInfo resolveActivity(android.content.Intent intent, int flags) {
            if (isEmailIntent(intent)) return emailResolveInfo();
            // A web link has a handler too, the host's: an app that asks before it opens one
            // (and does nothing when there is none) goes on to start it.
            return isWebLinkIntent(intent) ? webLinkResolveInfo() : null;
        }
        @Override public java.util.List<android.content.pm.ResolveInfo> queryIntentActivities(
                android.content.Intent intent, int flags) {
            if (isWebLinkIntent(intent)) {
                java.util.List<android.content.pm.ResolveInfo> l = new java.util.ArrayList<>(1);
                l.add(webLinkResolveInfo());
                return l;
            }
            if (isEmailIntent(intent)) {
                java.util.List<android.content.pm.ResolveInfo> l = new java.util.ArrayList<>(1);
                l.add(emailResolveInfo());
                return l;
            }
            return java.util.Collections.emptyList();
        }
        // API 33 (Tiramisu) ResolveInfoFlags overloads: ACRA's EmailIntentSender calls these directly
        // on SDK>=33. StubPackageManager leaves them throwing UnsupportedOperationException (which
        // aborted the crash-report send with a confusing "queryIntentActivities not implemented"),
        // so delegate to the int forms above.
        @Override public android.content.pm.ResolveInfo resolveActivity(android.content.Intent intent,
                android.content.pm.PackageManager.ResolveInfoFlags flags) {
            return resolveActivity(intent, 0);
        }
        @Override public java.util.List<android.content.pm.ResolveInfo> queryIntentActivities(
                android.content.Intent intent, android.content.pm.PackageManager.ResolveInfoFlags flags) {
            return queryIntentActivities(intent, 0);
        }
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
        // StubPackageManager returns null, and callers do getApplicationLabel(...).toString(): Urban
        // Airship's UAirship.getAppName() does exactly that on its takeOff worker. The NPE kills that
        // worker, so UAirship never signals takeOff -- and Pandora's main thread, blocked in
        // UAirship.waitForTakeOff() inside PandoraApp.onCreate, hangs forever and no Activity launches.
        // Resolve the real android:label when the ApplicationInfo carries it, else the package name;
        // never null.
        @Override public CharSequence getApplicationLabel(android.content.pm.ApplicationInfo info) {
            if (info != null) {
                if (info.nonLocalizedLabel != null) return info.nonLocalizedLabel;
                if (info.labelRes != 0) {
                    try {
                        CharSequence l = sAppContext != null
                                ? sAppContext.getResources().getText(info.labelRes) : null;
                        if (l != null) return l;
                    } catch (Throwable ignored) { }
                }
                if (info.name != null) return info.name;
                if (info.packageName != null) return info.packageName;
            }
            return pkg;
        }
        @Override public android.content.pm.PackageInfo getPackageInfo(String p,
                android.content.pm.PackageManager.PackageInfoFlags flags)
                throws android.content.pm.PackageManager.NameNotFoundException {
            return getPackageInfo(p, 0);
        }
        // Where the app was installed from. There is no installer/store on acl (the APK is run
        // directly), so report an unknown source — what a sideloaded app sees — rather than letting
        // the AOSP base class throw UnsupportedOperationException, which aborts Application.onCreate
        // (Pandora's PandoraApp.onCreate queries this during startup).
        @Override public android.content.pm.InstallSourceInfo getInstallSourceInfo(String p)
                throws android.content.pm.PackageManager.NameNotFoundException {
            if (p != null && !p.equals(pkg) && !"com.android.webview".equals(p))
                throw new android.content.pm.PackageManager.NameNotFoundException(p);
            return new android.content.pm.InstallSourceInfo(
                    null /* initiatingPackageName */, null /* initiatingPackageSigningInfo */,
                    null /* originatingPackageName */, null /* installingPackageName */);
        }
        @Override public String getInstallerPackageName(String p) { return null; }
        // PackageManager.buildRequestPermissionsIntent() calls this to address the permission
        // controller, and the AOSP base throws "Not implemented. Must override in a subclass." That
        // throw escapes Activity.requestPermissions() on the main thread and kills whatever called it
        // -- for castlab it canceled the coroutine expo requests permissions in, leaving the app on
        // its spinner. Name the AOSP controller package; nothing launches it (NoopInstrumentation
        // answers the request with GRANTED), it just has to be a non-throwing, non-null name.
        @Override public String getPermissionControllerPackageName() {
            return "com.android.permissioncontroller";
        }
        // StubPackageManager returns null here, and callers dereference it: F-Droid's
        // SessionInstallManager ctor calls getPackageInstaller().getMySessions() during startup, the
        // NPE is uncaught, and its crash reporter kills the process before the UI. acl cannot install
        // APKs, so hand back a real PackageInstaller wrapping an IPackageInstaller proxy that reports
        // no sessions (getMySessions/getAllSessions -> empty). Built by reflection: IPackageInstaller
        // is a hidden AIDL interface and PackageInstaller has no no-arg constructor. Best-effort --
        // on any failure fall back to the old null, i.e. no worse than before.
        private android.content.pm.PackageInstaller pkgInstaller;
        private boolean pkgInstallerTried;
        @Override public synchronized android.content.pm.PackageInstaller getPackageInstaller() {
            if (pkgInstallerTried) return pkgInstaller;
            pkgInstallerTried = true;
            try {
                Class<?> ipiCls = Class.forName("android.content.pm.IPackageInstaller");
                final Object emptySlice = Class.forName("android.content.pm.ParceledListSlice")
                        .getMethod("emptyList").invoke(null);
                Object ipi = java.lang.reflect.Proxy.newProxyInstance(
                        ipiCls.getClassLoader(), new Class<?>[]{ipiCls},
                        (p, method, a) -> {
                            String n = method.getName();
                            if ("getMySessions".equals(n) || "getAllSessions".equals(n)) return emptySlice;
                            if ("asBinder".equals(n)) return null;
                            return defaultValue(method.getReturnType());
                        });
                pkgInstaller = android.content.pm.PackageInstaller.class
                        .getConstructor(ipiCls, String.class, String.class, int.class)
                        .newInstance(ipi, pkg, null, 0);
            } catch (Throwable t) {
                android.util.Log.w("acl", "getPackageInstaller stub failed (" + t + "); returning null");
            }
            return pkgInstaller;
        }
        // Kotlin callers dereference this with a non-null assertion, so a null (the stub default)
        // crashes them. acl advertises no optional hardware features; return an empty array rather
        // than null (hasSystemFeature() answers individual queries).
        @Override public android.content.pm.FeatureInfo[] getSystemAvailableFeatures() {
            return new android.content.pm.FeatureInfo[0];
        }
        private android.content.pm.PackageInfo ownPackageInfo() {
            android.content.pm.PackageInfo pi = new android.content.pm.PackageInfo();
            pi.packageName = pkg;
            pi.applicationInfo = getApplicationInfo(pkg, 0);
            // The manifest's own version. An app shows it, sends it in a user agent, and keys
            // migrations and caches on it; a manifest that states none reads as 1 / "1.0".
            requestedPermissions();
            pi.versionName = sVersionName != null ? sVersionName : "1.0";
            long code = sVersionCode > 0 ? sVersionCode : 1L;
            try { pi.setLongVersionCode(code); } catch (Throwable ignored) { pi.versionCode = (int) code; }
            // When the APK was last written, as the time it was installed and last updated. An
            // app that unpacks something from its APK keys the copy on this: Flutter's debug-mode
            // ResourceExtractor keeps its extracted Dart code while version code and
            // lastUpdateTime are unchanged, so with 0 here a rebuilt app ran its old code.
            try {
                String src = pi.applicationInfo != null ? pi.applicationInfo.sourceDir : null;
                long written = src != null ? new java.io.File(src).lastModified() : 0L;
                pi.firstInstallTime = written;
                pi.lastUpdateTime = written;
            } catch (Throwable ignored) { }
            // The app's <uses-permission> names, each marked as checkSelfPermission() answers
            // for it. Granting at checkSelfPermission() is NOT enough: permission libraries first
            // verify the permission is DECLARED, by reading getPackageInfo(GET_PERMISSIONS).requestedPermissions. A null
            // there reads as "not in the manifest", which is a hard denial no runtime grant can undo
            // -- Expo's PermissionsService does exactly
            //   getPackageInfo(pkg, GET_PERMISSIONS).requestedPermissions?.contains(perm) ?: false
            // and castlab's first screen fails with "Missing MEDIA_LIBRARY permissions" and spins
            // forever on its loading indicator.
            String[] perms = requestedPermissions();
            if (perms.length > 0) {
                pi.requestedPermissions = perms;
                int[] flags = new int[perms.length];
                for (int i = 0; i < flags.length; i++) {
                    if (HostPermissions.check(perms[i], pkg)
                            == android.content.pm.PackageManager.PERMISSION_GRANTED)
                        flags[i] = android.content.pm.PackageInfo.REQUESTED_PERMISSION_GRANTED;
                }
                pi.requestedPermissionsFlags = flags;
            }
            return pi;
        }

        // Parsed once from AndroidManifest.xml: what the app asked for.
        private static String[] sPerms;
        // The <manifest> tag's version, read in the same pass: 0 and null when it states none.
        private static long sVersionCode;
        private static String sVersionName;
        private static synchronized String[] requestedPermissions() {
            if (sPerms != null) return sPerms;
            android.content.Context base = sAppContext;
            // Do NOT cache before the app context exists, or the empty answer sticks forever and
            // every later query reports "no permissions declared".
            if (base == null) return new String[0];
            java.util.List<String> out = new java.util.ArrayList<String>();
            android.content.res.XmlResourceParser xml = null;
            try {
                // Pin to the app's base apk (openBaseManifest): the first "AndroidManifest.xml" across
                // the AssetManager can be framework-res / the vendored WebView apk / a config split,
                // not the app — which once reported 4 unrelated permissions for an app declaring 22.
                xml = openBaseManifest(base.getResources().getAssets(),
                        base.getApplicationInfo() != null ? base.getApplicationInfo().sourceDir : null);
                final String NS = "http://schemas.android.com/apk/res/android";
                for (int ev = xml.getEventType();
                     ev != android.content.res.XmlResourceParser.END_DOCUMENT; ev = xml.next()) {
                    if (ev == android.content.res.XmlResourceParser.START_TAG
                            && "manifest".equals(xml.getName())) {
                        long lo = xml.getAttributeIntValue(NS, "versionCode", 0) & 0xffffffffL;
                        long hi = xml.getAttributeIntValue(NS, "versionCodeMajor", 0) & 0xffffffffL;
                        sVersionCode = (hi << 32) | lo;
                        // A literal, or a reference to a string resource.
                        int ref = xml.getAttributeResourceValue(NS, "versionName", 0);
                        String name = ref != 0 ? base.getResources().getString(ref)
                                : xml.getAttributeValue(NS, "versionName");
                        if (name != null && !name.isEmpty()) sVersionName = name;
                        continue;
                    }
                    if (ev == android.content.res.XmlResourceParser.START_TAG
                            && ("uses-permission".equals(xml.getName())
                                || "uses-permission-sdk-23".equals(xml.getName()))) {
                        String name = xml.getAttributeValue(NS, "name");
                        if (name != null && !out.contains(name)) out.add(name);
                    }
                }
            } catch (Throwable t) {
                android.util.Log.w("acl", "requestedPermissions: manifest parse failed (" + t
                        + "); reporting none");
            } finally {
                if (xml != null) try { xml.close(); } catch (Throwable ignored) { }
            }
            sPerms = out.toArray(new String[0]);
            android.util.Log.i("acl", "requestedPermissions: " + sPerms.length
                    + " declared in the app manifest (all reported granted); version "
                    + sVersionCode + " / " + sVersionName);
            return sPerms;
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
        // Non-null ServiceInfo carrying the service's manifest <meta-data> (parsed into sServiceMeta).
        // StubPackageManager returned null here, which NPEs a caller that reads serviceInfo.metaData:
        // Firebase's ComponentDiscovery reads getServiceInfo(ComponentDiscoveryService, GET_META_DATA)
        // .metaData to enumerate its ComponentRegistrars (firebase-messaging, installations, ...), and
        // a null service left FirebaseMessaging.getInstance() throwing "Firebase Messaging component is
        // not present". acl starts no services, so this only re-serves declared metadata.
        @Override public android.content.pm.ServiceInfo getServiceInfo(
                android.content.ComponentName component, int flags)
                throws android.content.pm.PackageManager.NameNotFoundException {
            android.content.pm.ServiceInfo si = new android.content.pm.ServiceInfo();
            si.packageName = component != null ? component.getPackageName() : pkg;
            si.name = component != null ? component.getClassName() : null;
            si.applicationInfo = getApplicationInfo(pkg, 0);
            if (si.name != null) si.metaData = sServiceMeta.get(si.name);
            Declared d = si.name != null ? sDeclared.get(si.name) : null;
            // A service the app does not declare is not found, as on Android. A library asks
            // for one of its own that the app's build may have removed, and takes "not found"
            // as the answer: appcompat reads autoStoreLocales from
            // AppLocalesMetadataHolderService and fails on an info that has no meta-data.
            if (sManifestRead && si.name != null && pkg.equals(si.packageName)
                    && (d == null || !d.service)) {
                throw new android.content.pm.PackageManager.NameNotFoundException(
                        component.flattenToShortString());
            }
            if (d != null && d.service) {
                si.permission = d.permission;
                si.enabled = d.enabled;
                si.exported = d.exported;
            }
            return si;
        }
        // The app's own services and receivers answer intent queries, as an installed package's do:
        // by class for an explicit Intent, by intent-filter action otherwise. Libraries gate on
        // this (android-job counts a scheduler API as usable only when its service resolves).
        // Other packages stay unresolved. Filters are matched on the action alone.
        private java.util.List<android.content.pm.ResolveInfo> queryOwn(
                android.content.Intent intent, int flags, boolean service) {
            java.util.List<android.content.pm.ResolveInfo> out = new java.util.ArrayList<>();
            if (intent == null) return out;
            android.content.ComponentName cn = intent.getComponent();
            String target = cn != null ? cn.getPackageName() : intent.getPackage();
            if (target != null && !target.equals(pkg)) return out;
            boolean disabledToo =
                    (flags & android.content.pm.PackageManager.MATCH_DISABLED_COMPONENTS) != 0;
            String action = intent.getAction();
            if (cn == null && action == null) return out;
            synchronized (sDeclared) {
                for (java.util.Map.Entry<String, Declared> e : sDeclared.entrySet()) {
                    Declared d = e.getValue();
                    if (d.service != service || (!d.enabled && !disabledToo)) continue;
                    if (cn != null ? !e.getKey().equals(cn.getClassName())
                                   : !d.actions.contains(action)) continue;
                    out.add(resolveInfoFor(e.getKey(), d, flags));
                }
            }
            return out;
        }
        private android.content.pm.ResolveInfo resolveInfoFor(String name, Declared d, int flags) {
            android.content.pm.ComponentInfo ci = d.service
                    ? new android.content.pm.ServiceInfo() : new android.content.pm.ActivityInfo();
            ci.packageName = pkg;
            ci.name = name;
            ci.enabled = d.enabled;
            ci.exported = d.exported;
            ci.applicationInfo = getApplicationInfo(pkg, 0);
            android.content.pm.ResolveInfo ri = new android.content.pm.ResolveInfo();
            if (d.service) {
                android.content.pm.ServiceInfo si = (android.content.pm.ServiceInfo) ci;
                si.permission = d.permission;
                if ((flags & android.content.pm.PackageManager.GET_META_DATA) != 0)
                    si.metaData = sServiceMeta.get(name);
                ri.serviceInfo = si;
            } else {
                android.content.pm.ActivityInfo ai = (android.content.pm.ActivityInfo) ci;
                ai.permission = d.permission;
                ri.activityInfo = ai;
            }
            return ri;
        }
        @Override public java.util.List queryIntentServices(android.content.Intent intent, int flags) {
            return queryOwn(intent, flags, true);
        }
        @Override public java.util.List<android.content.pm.ResolveInfo> queryIntentServices(
                android.content.Intent intent, android.content.pm.PackageManager.ResolveInfoFlags flags) {
            return queryOwn(intent, (int) flags.getValue(), true);
        }
        @Override public android.content.pm.ResolveInfo resolveService(
                android.content.Intent intent, int flags) {
            java.util.List<android.content.pm.ResolveInfo> l = queryOwn(intent, flags, true);
            return l.isEmpty() ? null : l.get(0);
        }
        @Override public android.content.pm.ResolveInfo resolveService(
                android.content.Intent intent, android.content.pm.PackageManager.ResolveInfoFlags flags) {
            return resolveService(intent, (int) flags.getValue());
        }
        @Override public java.util.List queryBroadcastReceivers(android.content.Intent intent, int flags) {
            return queryOwn(intent, flags, false);
        }
        @Override public java.util.List<android.content.pm.ResolveInfo> queryBroadcastReceivers(
                android.content.Intent intent, android.content.pm.PackageManager.ResolveInfoFlags flags) {
            return queryOwn(intent, (int) flags.getValue(), false);
        }
        // Non-null ActivityInfo (no parent activity) so androidx NavUtils.getParentActivityName
        // and similar reads don't NPE on a null result.
        @Override public android.content.pm.ActivityInfo getActivityInfo(
                android.content.ComponentName component, int flags) {
            android.content.pm.ActivityInfo ai = new android.content.pm.ActivityInfo();
            ai.packageName = component != null ? component.getPackageName() : pkg;
            ai.name = component != null ? component.getClassName() : null;
            ai.applicationInfo = getApplicationInfo(pkg, 0);
            // The manifest's android:parentActivityName — NavUtils.getParentActivityName reads it here
            // (via PackageManager, not the Activity), so AppCompatActivity.onSupportNavigateUp (the
            // toolbar Up arrow) can navigate up. Without it Up-nav finds no parent and no-ops.
            if (ai.name != null) ai.parentActivityName = parentActivityName(ai.name);
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
            if ("settings".equals(authority)) return settingsProvider();
            return stubProvider();
        }
        // The settings provider, answering one key: Settings.Secure.ANDROID_ID. Android always has
        // a value there and apps hash it unchecked (Firebase Performance does). Every other
        // setting stays unset, so its reader takes the default.
        private static android.content.IContentProvider sSettings;
        private static synchronized android.content.IContentProvider settingsProvider() {
            if (sSettings == null) {
                final Class<?> iface = android.content.IContentProvider.class;
                sSettings = (android.content.IContentProvider) java.lang.reflect.Proxy.newProxyInstance(
                        iface.getClassLoader(), new Class<?>[]{iface}, (p, method, a) -> {
                            // call(AttributionSource, authority, method, arg, extras)
                            if ("call".equals(method.getName()) && a != null && a.length == 5
                                    && "GET_secure".equals(a[2]) && "android_id".equals(a[3])) {
                                android.os.Bundle b = new android.os.Bundle();
                                b.putString("value", androidId());
                                return b;
                            }
                            return defaultValue(method.getReturnType());
                        });
            }
            return sSettings;
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
        // Activity.attach calls attachBaseContext, which an Activity overrides to wrap the
        // context or to set itself up before onCreate (a base Activity that fills its injected
        // fields there finds them null in onCreate otherwise). A failure there is the app's
        // and is reported; the base is set either way.
        try {
            final java.lang.reflect.Method attach =
                    ContextWrapper.class.getDeclaredMethod("attachBaseContext",
                            android.content.Context.class);
            attach.setAccessible(true);
            attach.invoke(a, ctx);
        } catch (Throwable t) {
            final Throwable cause = t instanceof java.lang.reflect.InvocationTargetException
                    && t.getCause() != null ? t.getCause() : t;
            System.err.println("acl HostBootstrap: Activity.attachBaseContext incomplete for "
                    + a.getClass().getName() + " (" + cause + ")");
            cause.printStackTrace();
        }
        if (a.getBaseContext() == null) setField(ContextWrapper.class, a, "mBase", ctx);
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
        if (base instanceof AppContext) ((AppContext) base).setApplication(app);
        // Application.attach calls attachBaseContext, which an app overrides to set itself up
        // before its providers and onCreate run. A failure there leaves the base set, as a
        // failure in onCreate leaves the Application attached.
        try {
            final java.lang.reflect.Method attach =
                    ContextWrapper.class.getDeclaredMethod("attachBaseContext",
                            android.content.Context.class);
            attach.setAccessible(true);
            attach.invoke(app, base);
        } catch (Throwable t) {
            final Throwable cause = t instanceof java.lang.reflect.InvocationTargetException
                    && t.getCause() != null ? t.getCause() : t;
            System.err.println("acl HostBootstrap: Application.attachBaseContext incomplete for "
                    + app.getClass().getName() + " (" + cause + ")");
            cause.printStackTrace();
        }
        if (app.getBaseContext() == null) setField(ContextWrapper.class, app, "mBase", base);
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
            t.printStackTrace();
        }
        // DIAG (ACL_FORCE_STATIC_TRUE="pkg.Class:FIELD[,pkg.Class:FIELD]"): reflectively set boolean
        // static field(s) to true after the app's classloader is up — e.g. to force an app's debug/
        // logging flag on (org.telegram.messenger.BuildVars:LOGS_ENABLED routes Telegram's FileLog
        // through android.util.Log into acl's stderr, so a network/sign-in response is visible).
        try {
            String spec = System.getenv("ACL_FORCE_STATIC_TRUE");
            if (spec != null && !spec.isEmpty()) {
                for (String one : spec.split(",")) {
                    int c = one.lastIndexOf(':');
                    if (c <= 0) continue;
                    String cls = one.substring(0, c).trim(), fld = one.substring(c + 1).trim();
                    try {
                        java.lang.reflect.Field f = Class.forName(cls, true, app.getClassLoader())
                                .getDeclaredField(fld);
                        f.setAccessible(true);
                        f.setBoolean(null, true);
                        System.out.println("acl HostBootstrap: forced " + cls + "." + fld + " = true");
                    } catch (Throwable t) {
                        System.err.println("acl HostBootstrap: ACL_FORCE_STATIC_TRUE " + one + " -> " + t);
                    }
                }
            }
        } catch (Throwable ignored) {}
        return app;  // never null → Activity.getApplication() works even if app init failed
    }

    // Parse <provider> entries straight from AndroidManifest.xml (no PMS/Binder, ADR-3),
    // instantiate each via the app classloader, and attachInfo() — which runs ContentProvider.
    // onCreate(). ActivityThread does this before Application.onCreate. The per-provider
    // <meta-data> is stashed in sProviderMeta so getProviderInfo() can re-serve it (the whole
    // reason androidx.startup's InitializationProvider works). Best-effort per provider: a
    // failing onCreate is logged and skipped so one bad provider doesn't sink app startup.
    private static Declared declare(android.content.res.XmlResourceParser xml, String ns,
            String name, boolean service) {
        Declared d = new Declared(service, xml.getAttributeValue(ns, "permission"),
                xml.getAttributeBooleanValue(ns, "enabled", true),
                xml.getAttributeBooleanValue(ns, "exported", false));
        sDeclared.put(name, d);
        return d;
    }

    static void installContentProviders(android.content.Context base, String pkg) {
        // {className, authorities, exported, grantUriPermissions} — the last two decide whether
        // FileProvider (exported=false + grantUriPermissions=true, else it throws in attachInfo).
        java.util.List<String[]> providers = new java.util.ArrayList<String[]>();
        android.content.res.XmlResourceParser xml = null;
        try {
            xml = openBaseManifest(base.getResources().getAssets(),
                    base.getApplicationInfo() != null ? base.getApplicationInfo().sourceDir : null);
            final String NS = "http://schemas.android.com/apk/res/android";
            String cur = null;
            android.os.Bundle curMeta = null;
            // <service> parsing runs in parallel: each declared service and its intent-filter
            // actions (AppServices starts and binds them), and its <meta-data>, re-servable via
            // getServiceInfo — Firebase's ComponentDiscoveryService is a metadata-only holder
            // listing the ComponentRegistrars.
            String curSvc = null;
            android.os.Bundle curSvcMeta = null;
            Declared curDecl = null;
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
                            // The authorities may be a string resource, which reads back
                            // as "@<id>": the provider is then reached by a name no URI has.
                            String authorities = xml.getAttributeValue(NS, "authorities");
                            final int authRes =
                                    xml.getAttributeResourceValue(NS, "authorities", 0);
                            if (authRes != 0) {
                                try {
                                    authorities = base.getResources().getString(authRes);
                                } catch (RuntimeException e) {
                                    // Keep what the attribute gave.
                                }
                            }
                            providers.add(new String[]{name, authorities,
                                    Boolean.toString(xml.getAttributeBooleanValue(NS, "exported", false)),
                                    Boolean.toString(xml.getAttributeBooleanValue(NS, "grantUriPermissions", false))});
                        }
                    } else if ("service".equals(tag)) {
                        String name = xml.getAttributeValue(NS, "name");
                        if (name != null) {
                            if (name.startsWith(".")) name = pkg + name;
                            else if (name.indexOf('.') < 0) name = pkg + "." + name;
                            curSvc = name;
                            curSvcMeta = new android.os.Bundle();
                            sServiceActions.put(name, new java.util.ArrayList<String>());
                            curDecl = declare(xml, NS, name, true);
                        }
                    } else if ("receiver".equals(tag)) {
                        String name = xml.getAttributeValue(NS, "name");
                        if (name != null) {
                            if (name.startsWith(".")) name = pkg + name;
                            else if (name.indexOf('.') < 0) name = pkg + "." + name;
                            curDecl = declare(xml, NS, name, false);
                        }
                    } else if ("action".equals(tag) && curDecl != null) {
                        String action = xml.getAttributeValue(NS, "name");
                        if (action != null) {
                            curDecl.actions.add(action);
                            java.util.List<String> actions =
                                    curSvc != null ? sServiceActions.get(curSvc) : null;
                            if (actions != null) actions.add(action);
                        }
                    } else if ("meta-data".equals(tag)) {
                        // Attribute to whichever component encloses it (service takes precedence when
                        // both are open, though they never nest); a bare <application> meta-data with
                        // no open component is handled separately by parseManifestMetaData.
                        if (curSvc != null) AppContext.putMetaData(curSvcMeta, xml, base.getResources());
                        else if (cur != null) AppContext.putMetaData(curMeta, xml, base.getResources());
                    }
                } else if (ev == android.content.res.XmlResourceParser.END_TAG) {
                    String end = xml.getName();
                    if ("provider".equals(end)) {
                        if (cur != null) { sProviderMeta.put(cur, curMeta); cur = null; curMeta = null; }
                    } else if ("service".equals(end)) {
                        if (curSvc != null) { sServiceMeta.put(curSvc, curSvcMeta); curSvc = null; curSvcMeta = null; }
                        curDecl = null;
                    } else if ("receiver".equals(end)) {
                        curDecl = null;
                    }
                }
            }
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: <provider> parse failed (" + t + "); "
                    + "no ContentProviders installed");
            return;
        } finally {
            if (xml != null) xml.close();
        }
        sManifestRead = true;
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
        // Process.myProcessName() returns sArgV0, which ActivityThread sets with
        // Process.setArgV0 when it binds the application. It is declared non-null, and
        // Kotlin code that reads it throws on null.
        try {
            java.lang.reflect.Field argv0 = android.os.Process.class.getDeclaredField("sArgV0");
            argv0.setAccessible(true);
            if (argv0.get(null) == null) argv0.set(null, pkg);
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: process name not set: " + t);
        }
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

    /** Best-effort MIME for an ACL_INTENT_VIEW_FILE path (extension-based); null if unknown. */
    private static String guessViewMime(String path) {
        String p = path.toLowerCase(java.util.Locale.ROOT);
        if (p.endsWith(".epub")) return "application/epub+zip";
        if (p.endsWith(".pdf")) return "application/pdf";
        if (p.endsWith(".txt")) return "text/plain";
        if (p.endsWith(".fb2")) return "application/x-fictionbook+xml";
        if (p.endsWith(".html") || p.endsWith(".htm")) return "text/html";
        return null;
    }

    // Lightweight Activity attach: a real Activity is normally attach()'d by ActivityThread
    // (mActivityInfo, mWindow=PhoneWindow, mApplication, …) before onCreate. We can't stand up
    // ActivityThread, so seed the minimum onCreate/setContentView need via reflection: a
    // default ActivityInfo and a PhoneWindow bound to the context. Returns the Activity so the
    // caller can drive onCreate then read getWindow().getDecorView() as the content view.
    // ── In-process Activity task stack (acl plays the ActivityManager role; no Binder, ADR-3) ──
    // Android tracks activities in AMS as a token-keyed task; startActivity pushes, finish() pops and
    // resumes the one beneath. acl already owns both seams (Instrumentation.execStartActivity for the
    // launch, and the IActivityClientController stub for finish), so it maintains the SAME model here:
    // one task (a stack of ActivityRecord), each activity carrying a real Binder token so finish()'s
    // ActivityClient.finishActivity(mToken,...) resolves to its record. This is what makes the toolbar
    // back arrow, the BACK key, and programmatic finish() all navigate back — they converge on finish()
    // -> finishActivity(token). Single-threaded: every activity lifecycle transition, the finish post,
    // and the present loop all run on the main Looper, so no synchronization is needed.
    static final class ActivityRecord {
        final android.app.Activity activity;
        final android.os.IBinder token;
        final android.os.IBinder callerToken;  // the record that startActivityForResult'd us, or null
        final int requestCode;                 // -1 unless started for a result
        int resultCode = android.app.Activity.RESULT_CANCELED;
        android.content.Intent resultData;
        // Its theme has windowIsTranslucent or windowIsFloating: the Activity under it stays
        // on screen, as ActivityInfo.isTranslucentOrFloating decides on Android.
        final boolean translucent;
        boolean stopped;  // onStop has been called, and onRestart has not been since
        ActivityRecord(android.app.Activity a, android.os.IBinder t, android.os.IBinder caller, int rc) {
            activity = a; token = t; callerToken = caller; requestCode = rc;
            translucent = translucentOrFloating(a);
        }
    }

    private static boolean translucentOrFloating(android.app.Activity a) {
        android.content.res.TypedArray t = null;
        try {
            // In ascending order, as obtainStyledAttributes wants its attributes.
            t = a.getTheme().obtainStyledAttributes(new int[] {
                    android.R.attr.windowIsFloating, android.R.attr.windowIsTranslucent});
            return t.getBoolean(0, false) || t.getBoolean(1, false);
        } catch (Throwable e) {
            return false;  // no theme to read: an ordinary, opaque Activity
        } finally {
            if (t != null) t.recycle();
        }
    }
    static final java.util.ArrayDeque<ActivityRecord> sTask = new java.util.ArrayDeque<>();  // head = top
    static final java.util.Map<android.os.IBinder, ActivityRecord> sByToken = new java.util.HashMap<>();
    // Set by execStartActivity just before an internal launch so the record the new activity registers
    // (in attachActivity) can remember who to return a result to. Consumed once, then cleared.
    static android.os.IBinder sPendingCallerToken = null;
    static int sPendingRequestCode = -1;

    // The current top-of-stack activity's decor view (null if none). The runtime presents this at
    // nOpen instead of the entry activity's own decor: a launcher/trampoline Activity (e.g. Pandora's
    // LauncherActivity) can synchronously start the real Activity and finish() itself DURING its
    // onCreate — before the presenter opens — so the entry activity is already gone and its decor
    // would show a frozen splash. Presenting the live top shows the real screen.
    public static android.view.View currentTopDecorView() {
        return decorOf(sTask.peek());
    }

    private static android.view.View decorOf(ActivityRecord r) {
        if (r == null || r.activity == null) return null;
        try {
            android.view.Window w = r.activity.getWindow();
            return w != null ? w.getDecorView() : null;
        } catch (Throwable t) { return null; }
    }

    // ── Activities on screen under a translucent one ──────────────────────────────────────────
    // An Activity whose theme is translucent, or floating, does not hide the one under it: a
    // sign-in card over a map, a dialog-themed Activity over a list. On Android the one under
    // it is paused and not stopped, its window stays on screen with any SurfaceView surface it
    // has, and it is resumed with no restart when the one over it finishes. a4l draws every
    // Activity into the one surface, so the windows on screen are drawn bottom one first, each
    // a full traversal (layoutUnderlays, drawUnderlays), and the top one over them.

    // How many Activities are on screen, counted from the top of the task: the top one, and
    // each one that has only translucent ones over it.
    private static int shownCount() {
        int n = 0;
        for (ActivityRecord r : sTask) {  // top first
            n++;
            if (!r.translucent) break;
        }
        return n;
    }

    private static final java.util.List<android.view.View> NO_VIEWS =
            java.util.Collections.emptyList();

    // The windows of the Activities on screen under the top one, bottom one first. Empty
    // unless the top one is translucent, which is nearly always.
    private static java.util.List<android.view.View> decorsUnderTop() {
        final ActivityRecord top = sTask.peek();
        if (top == null || !top.translucent || sTask.size() < 2) return NO_VIEWS;
        final java.util.ArrayList<android.view.View> out = new java.util.ArrayList<>(2);
        for (ActivityRecord r : sTask) {
            if (r == top) continue;
            final android.view.View decor = decorOf(r);
            if (decor != null) out.add(decor);
            if (!r.translucent) break;
        }
        java.util.Collections.reverse(out);
        return out;
    }

    private static boolean onScreen(android.view.View v) {
        final android.view.View root = v.getRootView();
        int left = shownCount();
        for (ActivityRecord r : sTask) {
            if (left-- <= 0) break;
            if (decorOf(r) == root) return true;
        }
        return false;
    }

    // After the task changed: stops each Activity an opaque one now covers, and starts again
    // each one that is back on screen. Resuming the top one is the caller's.
    private static void syncShown() {
        final int shown = shownCount();
        int i = 0;
        // A copy: an Activity may start or finish another from the callback.
        for (ActivityRecord r : new java.util.ArrayList<ActivityRecord>(sTask)) {
            final boolean visible = i++ < shown;
            if (!sByToken.containsKey(r.token)) continue;
            if (!visible && !r.stopped) {
                r.stopped = true;
                safeLifecycle(r.activity, "onStop");
            } else if (visible && r.stopped) {
                r.stopped = false;
                safeLifecycle(r.activity, "onRestart");
                safeLifecycle(r.activity, "onStart");
            }
        }
    }

    // The windows under the top one that layoutUnderlays laid out for this frame and whose
    // draw no pre-draw listener cancelled, bottom one first. Main thread.
    private static java.util.List<android.view.View> sUnderlays = NO_VIEWS;
    private static int sUnderlayFailures;

    /**
     * Lays out the window of each Activity on screen under the top one, at the size of the
     * screen, and runs its tree observer, as the present loop does for the top one. Returns
     * whether there are any to draw. Called by the present loop once a frame.
     */
    public static boolean layoutUnderlays(int w, int h) {
        final java.util.List<android.view.View> under = decorsUnderTop();
        if (under.isEmpty()) {
            sUnderlays = NO_VIEWS;
            return false;
        }
        final java.util.ArrayList<android.view.View> drawn =
                new java.util.ArrayList<>(under.size());
        final int wspec = android.view.View.MeasureSpec.makeMeasureSpec(
                w, android.view.View.MeasureSpec.EXACTLY);
        final int hspec = android.view.View.MeasureSpec.makeMeasureSpec(
                h, android.view.View.MeasureSpec.EXACTLY);
        for (android.view.View decor : under) {
            try {
                // One the app covered before its first frame has not been attached yet.
                if (!decor.isAttachedToWindow()) attachDecorToWindow(decor);
                final boolean didLayout = decor.isLayoutRequested();
                decor.measure(wspec, hspec);
                decor.layout(0, 0, decor.getMeasuredWidth(), decor.getMeasuredHeight());
                if (!dispatchTreeObserver(decor, didLayout)) drawn.add(decor);
            } catch (Throwable t) {
                if (sUnderlayFailures++ < 3) {
                    System.err.println("acl HostBootstrap: layoutUnderlays: " + t);
                    t.printStackTrace();
                }
            }
        }
        sUnderlays = drawn;
        return true;
    }

    /**
     * Draws the windows layoutUnderlays laid out, bottom one first, then the dim the top
     * window asks for behind itself (FLAG_DIM_BEHIND). The present loop draws the top one
     * over them.
     */
    public static void drawUnderlays(android.graphics.Canvas canvas) {
        if (canvas == null) return;
        try {
            for (android.view.View decor : sUnderlays) decor.draw(canvas);
            final ActivityRecord top = sTask.peek();
            final android.view.Window w = top != null ? top.activity.getWindow() : null;
            final android.view.WindowManager.LayoutParams lp = w != null ? w.getAttributes() : null;
            if (lp != null
                    && (lp.flags & android.view.WindowManager.LayoutParams.FLAG_DIM_BEHIND) != 0) {
                final int a = (int) (Math.max(0f, Math.min(1f, lp.dimAmount)) * 255f);
                if (a > 0) canvas.drawColor(a << 24);
            }
        } catch (Throwable t) {
            if (sUnderlayFailures++ < 3) {
                System.err.println("acl HostBootstrap: drawUnderlays: " + t);
                t.printStackTrace();
            }
        }
    }

    /**
     * Before a frame: gives the SurfaceView on screen its surface and the TextureViews
     * theirs, in the top Activity's window and in each one on screen under it. There is one
     * plane: it goes to the SurfaceView nearest the top that can take it.
     */
    static void prepareWindows(android.view.View top) {
        boolean surfaced = maybeInjectVideoSurface(top);
        maybeLogVideoPlayer(top);
        prepareTextureViews(top);
        final java.util.List<android.view.View> under = decorsUnderTop();
        for (int i = under.size() - 1; i >= 0; i--) {
            final android.view.View decor = under.get(i);
            if (!surfaced) surfaced = maybeInjectVideoSurface(decor);
            prepareTextureViews(decor);
        }
    }

    // Give an activity a real token and push it as the task's new top. Called from attachActivity, so
    // BOTH the jvm_vm-launched ROOT (via attachActivityContext) and internal startActivity launches are
    // adopted uniformly — the first push is the root (finishing it ends the task -> process exit).
    private static void registerActivity(android.app.Activity a) {
        try {
            android.os.IBinder token = new android.os.Binder();
            setField(android.app.Activity.class, a, "mToken", token);
            final android.os.IBinder caller = sPendingCallerToken;
            final int rc = sPendingRequestCode;
            sPendingCallerToken = null; sPendingRequestCode = -1;
            ActivityRecord rec = new ActivityRecord(a, token, caller, rc);
            sTask.push(rec);
            sByToken.put(token, rec);
            seedBackFallback(a);  // so onBackPressed()/the Up arrow finish when the app doesn't handle back
            System.out.println("acl activity-stack: pushed " + a.getClass().getName()
                    + " (depth=" + sTask.size() + (rc >= 0 ? ", forResult rc=" + rc : "") + ")");
        } catch (Throwable t) {
            System.err.println("acl activity-stack: register failed: " + t);
        }
    }

    // Provide the platform's "finish on unhandled back" default to a ComponentActivity's
    // OnBackPressedDispatcher. On API 33+androidx, onBackPressed() (called by app code — e.g. a
    // toolbar Up arrow wired to onBackPressed(), or a plain back) no longer finishes on its own; the
    // default moved to the OnBackInvokedDispatcher, which acl doesn't drive. So acl seeds the
    // dispatcher's fallback Runnable to finish() the activity (-> finishActivityByToken -> pop). Field
    // is mFallbackOnBackPressed in AOSP; obfuscated apps rename it, so fall back to the sole
    // Runnable-typed field. Best-effort (plain Activities finish natively; non-androidx apps skip this).
    private static void seedBackFallback(android.app.Activity a) {
        try {
            Object d = a.getClass().getMethod("getOnBackPressedDispatcher").invoke(a);
            if (d == null) return;
            final Runnable finish = () -> {
                System.out.println("acl activity-stack: back fallback -> finish " + a.getClass().getName());
                try { a.finish(); } catch (Throwable ignored) {}
            };
            java.lang.reflect.Field target = null;
            for (Class<?> k = d.getClass(); k != null && target == null; k = k.getSuperclass())
                for (java.lang.reflect.Field f : k.getDeclaredFields())
                    if ("mFallbackOnBackPressed".equals(f.getName())) { target = f; break; }
            if (target == null)
                for (Class<?> k = d.getClass(); k != null && target == null; k = k.getSuperclass())
                    for (java.lang.reflect.Field f : k.getDeclaredFields())
                        if (Runnable.class.equals(f.getType())) { target = f; break; }
            if (target != null) { target.setAccessible(true); target.set(d, finish); }
        } catch (Throwable ignored) {}
    }

    // Best-effort call of a no-arg Activity lifecycle method (onPause/onStop/onDestroy/onRestart/…).
    private static void safeLifecycle(android.app.Activity a, String name) {
        try { invokeActivityLifecycle(a, name, null, null); }
        catch (Throwable t) { System.err.println("acl activity-stack: " + name + " failed: " + t); }
    }
    // Deliver a for-result callback to the caller being returned to (Activity.onActivityResult is protected).
    private static void deliverActivityResult(android.app.Activity a, int requestCode, int resultCode,
                                              android.content.Intent data) {
        try {
            java.lang.reflect.Method m = android.app.Activity.class.getDeclaredMethod(
                    "onActivityResult", int.class, int.class, android.content.Intent.class);
            m.setAccessible(true);
            m.invoke(a, requestCode, resultCode, data);
        } catch (Throwable t) { System.err.println("acl activity-stack: onActivityResult failed: " + t); }
    }

    // The real finish() seam: ActivityClient.finishActivity(token,…) routes here (see makeActivityTask
    // Handler). Deferred to the next main-loop tick because finish() is still unwinding when it calls in
    // (matching Android's async finish) — running the teardown inline would re-enter a half-finished
    // activity. Records the result first so the caller sees it on the way back.
    static boolean finishActivityByToken(android.os.IBinder token, int resultCode,
                                         android.content.Intent resultData) {
        System.out.println("acl activity-stack: finishActivity(token) -> "
                + (token != null && sByToken.get(token) != null
                        ? sByToken.get(token).activity.getClass().getName() : "UNKNOWN/null token"));
        // ACL_TRACE_FINISH: who called finish(). The call arrives on the caller's own stack.
        if (System.getenv("ACL_TRACE_FINISH") != null)
            new Throwable("acl activity-stack: finish() called from").printStackTrace();
        final ActivityRecord rec = token != null ? sByToken.get(token) : null;
        if (rec == null) return false;
        rec.resultCode = resultCode;
        rec.resultData = resultData;
        new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> finishRecord(rec));
        return true;
    }
    /**
     * ACL_INPUT_DIAG: the views the current gesture is going to, outermost first, read from each
     * ViewGroup's touch target after the DOWN was dispatched. Names the view that took a click
     * when nothing visible reacts.
     */
    public static String touchTargetChain(android.view.View root) {
        StringBuilder sb = new StringBuilder();
        try {
            java.lang.reflect.Field first =
                    android.view.ViewGroup.class.getDeclaredField("mFirstTouchTarget");
            first.setAccessible(true);
            android.view.View v = root;
            for (int depth = 0; v != null && depth < 64; depth++) {
                if (sb.length() > 0) sb.append(" > ");
                sb.append(v.getClass().getSimpleName());
                if (v.getId() != android.view.View.NO_ID) {
                    try {
                        sb.append('#').append(v.getResources().getResourceEntryName(v.getId()));
                    } catch (Throwable ignored) {}
                }
                sb.append('[').append(v.getWidth()).append('x').append(v.getHeight()).append(']');
                if (!(v instanceof android.view.ViewGroup)) break;
                Object target = first.get(v);
                if (target == null) break;
                java.lang.reflect.Field child = target.getClass().getDeclaredField("child");
                child.setAccessible(true);
                v = (android.view.View) child.get(target);
            }
        } catch (Throwable t) {
            sb.append(" (").append(t).append(')');
        }
        return sb.toString();
    }

    // The system's BACK action. A real ViewRootImpl routes the BACK key to the top window's back
    // handling; acl owns that system side, so it invokes the top activity's onBackPressed() — which
    // consults the OnBackPressedDispatcher (app interception: closing a drawer/sheet, WebView history,
    // a registered callback) and otherwise falls through to finish(), popping the task stack via
    // finishActivityByToken. This is why it is invoked here rather than reconstructed from raw-key
    // onKeyUp tracking, which acl's direct (no-ViewRootImpl-input-stage) dispatch doesn't carry. Posted
    // to the main Looper because the caller is mid key-dispatch and onBackPressed may finish+re-present.
    public static void dispatchBack() {
        final ActivityRecord top = sTask.peek();
        if (top == null) return;
        final android.app.Activity a = top.activity;
        new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
            try {
                // Mirror the platform's decision: if the app has an enabled OnBackPressedCallback (a
                // drawer/bottom-sheet/WebView-history handler), dispatch to it via onBackPressed();
                // otherwise the SYSTEM finishes the top activity. On API 33 + androidx, an unhandled
                // onBackPressed() no longer finishes on its own (the legacy fallback is gone in favor of
                // the OnBackInvokedDispatcher acl doesn't drive), so acl must finish() here — which pops
                // the task stack via ActivityClient.finishActivity(mToken) -> finishActivityByToken.
                boolean appHandles = false;
                try {
                    Object d = a.getClass().getMethod("getOnBackPressedDispatcher").invoke(a);
                    if (d != null)
                        appHandles = Boolean.TRUE.equals(
                                d.getClass().getMethod("hasEnabledCallbacks").invoke(d));
                } catch (Throwable ignored) {}
                if (appHandles) a.onBackPressed();  // app intercepts back
                else a.finish();                    // system default: finish -> pop
            } catch (Throwable t) {
                System.err.println("acl activity-stack: dispatchBack failed: " + t);
            }
        });
    }

    // Tear down a finishing activity and, if it was the visible top, resume + re-present the one beneath.
    // Bring a previously-shown decor back to the foreground. Swapping s->view alone is not enough:
    // the present loop (nTraverse) re-runs measure/layout/draw every frame, but draw replays each
    // view's CACHED RenderNode display list, and acl's default present path relies on those cached
    // lists rather than re-recording every frame. A decor that was backgrounded while another activity
    // ran comes back with a fully measured/laid-out tree but STALE display lists for its content (only
    // the chrome, which the restore's forceLayout touches, repaints) — so the content area draws
    // nothing and, on a live compositor, the previous activity's frame bleeds through it, reading as a
    // "page overlay". View.invalidate() does not help: acl's re-owned RenderNode replay only re-records
    // a node when its parent re-records (stock hwui semantics), and a cached parent never re-traverses
    // its children. So drive acl's force-record for a short pulse of frames — the whole tree re-records
    // top-down once, refreshing the restored content — then the cheap cache path resumes.
    private static final int RESTORE_RECORD_FRAMES = 4;
    private static void restoreDecor(android.view.View decor) {
        PresentDriver.setContentView(decor);
        clearOverlays(decor);   // drop any GhostView left by an unfinished shared-element transition
        forceFullRelayout(decor);
        decor.requestLayout();
        PresentDriver.pulseForceRecord(RESTORE_RECORD_FRAMES);
    }

    // Clear leftover ViewOverlay content across the restored tree. A shared-element activity
    // transition (F-Droid list<->detail app icon) parks a GhostView in the decor's overlay; acl has no
    // transition coordinator to end the transition, so on a pop the ghost stays and draws on top of the
    // restored content (a stray icon floating over the list). Nothing legitimate lives in an overlay
    // right after a restore, so clearing them removes the abandoned ghost without side effects.
    private static java.lang.reflect.Field sViewOverlayField;
    private static void clearOverlays(android.view.View v) {
        if (v == null) return;
        try {
            if (sViewOverlayField == null) {
                sViewOverlayField = android.view.View.class.getDeclaredField("mOverlay");
                sViewOverlayField.setAccessible(true);
            }
            Object overlay = sViewOverlayField.get(v);
            if (overlay instanceof android.view.ViewOverlay) ((android.view.ViewOverlay) overlay).clear();
        } catch (Throwable ignored) {}
        if (v instanceof android.view.ViewGroup) {
            android.view.ViewGroup vg = (android.view.ViewGroup) v;
            for (int i = 0, n = vg.getChildCount(); i < n; i++) clearOverlays(vg.getChildAt(i));
        }
    }

    private static void forceFullRelayout(android.view.View v) {
        if (v == null) return;
        v.forceLayout();
        if (v instanceof android.view.ViewGroup) {
            android.view.ViewGroup g = (android.view.ViewGroup) v;
            for (int i = 0, n = g.getChildCount(); i < n; i++) forceFullRelayout(g.getChildAt(i));
        }
    }

    // Set while finishRecord takes an Activity off the task and brings back the one under it.
    // An Activity that one starts meanwhile, from onActivityResult, onRestart, onStart or
    // onResume, is started when finishRecord is done. On Android startActivity only asks:
    // the Activity that asked is resumed first, then paused under the new one. Started at
    // once, the new one was on top before the one under it had been brought back, and that
    // one was then resumed over it and given the screen. Main thread.
    private static boolean sFinishing;

    private static void finishRecord(ActivityRecord rec) {
        if (rec == null || !sByToken.containsKey(rec.token)) return;  // already gone
        sFinishing = true;
        try {
            final boolean wasTop = (sTask.peek() == rec);
            safeLifecycle(rec.activity, "onPause");
            safeLifecycle(rec.activity, "onStop");
            safeLifecycle(rec.activity, "onDestroy");
            sTask.remove(rec);
            sByToken.remove(rec.token);
            if (!wasTop) {  // a below-top finish: no foreground change
                // Unless it was on screen under a translucent top one: then what is under
                // the top one has changed, and a plane it had goes with it.
                final ActivityRecord top = sTask.peek();
                final android.view.View topDecor = decorOf(top);
                if (top != null && top.translucent && topDecor != null) {
                    syncShown();
                    PresentDriver.setContentView(topDecor);
                }
                return;
            }
            final ActivityRecord next = sTask.peek();
            if (next == null) {  // the root finished -> the task (and this single-app process) is done
                System.out.println("acl activity-stack: task empty (root finished) -> exit");
                System.exit(0);
                return;
            }
            // Started again if it was stopped, which one under a translucent Activity was
            // not; and with it the ones on screen under it, if it is translucent itself.
            syncShown();
            // The result comes after onStart and before onResume, where
            // ActivityThread.performResumeActivity delivers it.
            if (rec.requestCode >= 0 && rec.callerToken == next.token)
                deliverActivityResult(next.activity, rec.requestCode, rec.resultCode, rec.resultData);
            // Paused when the finished one was started over it, so resumed now, which
            // brings onPostResume with it: a FragmentActivity resumes its fragments there.
            safeLifecycle(next.activity, "onResume");
            ensureResumed(next.activity);
            android.view.Window w = next.activity.getWindow();
            android.view.View decor = w != null ? w.getDecorView() : null;
            if (decor != null) restoreDecor(decor);
            System.out.println("acl activity-stack: popped to " + next.activity.getClass().getName()
                    + " (depth=" + sTask.size() + ")");
        } catch (Throwable t) {
            System.err.println("acl activity-stack: finishRecord failed: " + t);
        } finally {
            sFinishing = false;
        }
    }

    public static void attachActivity(android.app.Activity a, android.content.Context ctx)
            throws Exception {
        seedCompatCallbacks();  // before any Dialog: keep opt-in compat changes off (crash-free)
        // Real Activity.attach() applies the activity/app theme from ActivityInfo.getThemeResource()
        // before onCreate; layout inflation (obtainStyledAttributes) then resolves framework + app
        // attributes off it. Without it the Activity's ContextThemeWrapper theme is bare (Themes=[])
        // → InflateException "Failed to resolve attribute". Apply the manifest app theme, or a
        // framework default (DeviceDefault has a full attribute set) when the app declares none.
        // The Activity's own android:theme, else the application's, as ActivityInfo
        // .getThemeResource() gives on Android. The default is what the launch was given,
        // which is the launcher Activity's theme: right for the launcher, wrong for the
        // Activities it starts (a home screen inflated under a splash theme cannot resolve
        // the attributes its layouts name).
        int themeRes = manifestThemeFor(a.getClass().getName());
        if (themeRes == 0) themeRes = defaultAppTheme();
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
            // The Activity's theme is its own style over the system default, as on Android,
            // where an Activity's base context carries no app theme. Left to
            // ContextThemeWrapper it would start as a copy of the app Context's theme, which
            // is the launcher Activity's: an attribute this Activity's theme leaves unset
            // then takes the launcher's value (Pandora's home screen read hasToolbar=false
            // from its splash theme, and removed its own toolbar).
            android.content.res.Resources.Theme t = a.getResources().newTheme();
            t.applyStyle(android.R.style.Theme_DeviceDefault_Light_DarkActionBar, true);
            t.applyStyle(themeRes, true);
            setField(ctw, a, "mTheme", t);
            System.err.println("acl HostBootstrap: activity theme -> 0x"
                    + Integer.toHexString(themeRes) + " => " + t);
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: activity theme apply failed: " + t);
        }
        // The launch Intent — getIntent() is read in onCreate (a null mIntent NPEs). Give it the
        // launcher component so getIntent().getComponent() also resolves (not just getComponentName()).
        // ACL_INTENT_VIEW_FILE overrides ACTION_MAIN with an ACTION_VIEW at a file:// URI, so an
        // activity whose real content (e.g. a reader's book WebView) is reachable only via a VIEW
        // intent can be driven straight there without the library/import UI. The app's manifest
        // VIEW intent-filter (file:// + mime) matches; onCreate reads getIntent().getData()/getType().
        // acl sets mIntent directly (no ActivityManager IPC), so no FileUriExposedException fires, and
        // getContentResolver().openInputStream(file://) is plain FileInputStream (no provider needed).
        try {
            android.content.Intent launch;
            String viewFile = System.getenv("ACL_INTENT_VIEW_FILE");
            if (viewFile != null && !viewFile.isEmpty()) {
                android.net.Uri data = android.net.Uri.fromFile(new java.io.File(viewFile));
                String type = System.getenv("ACL_INTENT_VIEW_TYPE");
                if (type == null || type.isEmpty()) type = guessViewMime(viewFile);
                launch = new android.content.Intent(android.content.Intent.ACTION_VIEW)
                        .addCategory(android.content.Intent.CATEGORY_DEFAULT);
                if (type != null) launch.setDataAndType(data, type); else launch.setData(data);
                launch.addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION);
                System.err.println("acl HostBootstrap: ACTION_VIEW intent -> " + data + " (" + type + ")");
            } else {
                launch = new android.content.Intent(android.content.Intent.ACTION_MAIN)
                        .addCategory(android.content.Intent.CATEGORY_LAUNCHER);
            }
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
            // The window (and thus its LayoutInflater) MUST be created with the Activity as its
            // context, not the app Context: a View's getContext() returns the inflater's context, and
            // apps cast it to their Activity type — e.g. F-Droid's SettingsView.onAttachedToWindow does
            // (AppCompatActivity) getContext(), which ClassCastExceptions on the AppContext and aborts
            // the bottom-nav page attach (the tab appears not to respond). The Activity's base context
            // is already set (attachActivityContext) so it is a usable Context here.
            Object window = Class.forName("com.android.internal.policy.PhoneWindow")
                    .getConstructor(android.content.Context.class).newInstance(a);
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
                    .getConstructor(android.content.Context.class).newInstance(a);
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
        // Adopt this activity into the task stack with a token (see ActivityRecord). The first push is
        // the root; each internal startActivity pushes on top. finish()/BACK/the toolbar arrow pop it.
        registerActivity(a);
    }

    // Launch an Activity of THIS app internally and make it the presented decor. Many apps run a
    // splash/router Activity that immediately startActivity()s the real main Activity and finish()es
    // (Pandora: LauncherActivity -> com.pandora.android.Main). acl launches only the first Activity
    // natively; NoopInstrumentation suppresses the rest (it exists to drop EXTERNAL launches acl
    // can't host), so the app would stall on its splash. Mirror jvm_vm's native launch here for an
    // OWN-package component: instantiate -> attach -> onCreate/onStart/onResume -> attach the decor
    // and swap it into the present window. Returns true if it launched and presented.
    static boolean launchInternalActivity(android.content.Intent intent) {
        android.content.ComponentName cmp = intent != null ? intent.getComponent() : null;
        if (cmp == null || sAppContext == null) return false;
        final String targetCls = cmp.getClassName();
        final int flags = intent.getFlags();
        android.app.Activity a = null;
        ActivityRecord prev = null;
        try {
            // FLAG_ACTIVITY_SINGLE_TOP: relaunching the current top delivers onNewIntent, no new instance.
            ActivityRecord top = sTask.peek();
            if (top != null && (flags & android.content.Intent.FLAG_ACTIVITY_SINGLE_TOP) != 0
                    && targetCls.equals(top.activity.getClass().getName())) {
                try { setField(android.app.Activity.class, top.activity, "mIntent", intent); } catch (Throwable ignored) {}
                invokeActivityLifecycle(top.activity, "onNewIntent", android.content.Intent.class, intent);
                invokeActivityLifecycle(top.activity, "onResume", null, null);
                android.util.Log.i("acl", "launchInternalActivity: " + targetCls + " singleTop -> onNewIntent");
                return true;
            }
            // FLAG_ACTIVITY_CLEAR_TOP: an existing instance is brought forward, finishing everything above it.
            if ((flags & android.content.Intent.FLAG_ACTIVITY_CLEAR_TOP) != 0) {
                ActivityRecord existing = null;
                for (ActivityRecord r : sTask) if (targetCls.equals(r.activity.getClass().getName())) { existing = r; break; }
                if (existing != null) {
                    while (sTask.peek() != null && sTask.peek() != existing) {
                        ActivityRecord r = sTask.pop(); sByToken.remove(r.token);
                        safeLifecycle(r.activity, "onPause"); safeLifecycle(r.activity, "onStop");
                        safeLifecycle(r.activity, "onDestroy");
                    }
                    try { setField(android.app.Activity.class, existing.activity, "mIntent", intent); } catch (Throwable ignored) {}
                    invokeActivityLifecycle(existing.activity, "onNewIntent", android.content.Intent.class, intent);
                    syncShown();  // onRestart and onStart, if it was stopped
                    invokeActivityLifecycle(existing.activity, "onResume", null, null); ensureResumed(existing.activity);
                    android.view.Window ew = existing.activity.getWindow();
                    android.view.View ed = ew != null ? ew.getDecorView() : null;
                    if (ed != null) restoreDecor(ed);
                    android.util.Log.i("acl", "launchInternalActivity: " + targetCls + " clearTop -> reused");
                    return true;
                }
            }
            // Standard launch: instantiate, pause the current top, attach (pushes the record), resume, present.
            Class<?> cls = sAppContext.getClassLoader().loadClass(targetCls);
            if (!android.app.Activity.class.isAssignableFrom(cls)) return false;
            a = (android.app.Activity) cls.getConstructor().newInstance();
            prev = sTask.peek();
            if (prev != null) safeLifecycle(prev.activity, "onPause");  // pause the outgoing before the new resumes
            // attachActivityContext (not attachActivity) also sets mApplication, so getApplication()
            // is non-null before onCreate -- androidx ComponentActivity.getDefaultViewModelProvider
            // Factory() throws "activity is not yet attached to the Application instance" otherwise.
            attachActivityContext(a, sAppContext, sApplication);  // sets token + pushes record (top = a)
            try { setField(android.app.Activity.class, a, "mIntent", intent); } catch (Throwable ignored) {}
            invokeActivityLifecycle(a, "onCreate", android.os.Bundle.class, null);
            invokeActivityLifecycle(a, "onStart", null, null);
            invokeActivityLifecycle(a, "onPostCreate", android.os.Bundle.class, null);
            invokeActivityLifecycle(a, "onResume", null, null);
            ensureResumed(a);
            android.view.Window w = a.getWindow();
            android.view.View decor = w != null ? w.getDecorView() : null;
            if (decor == null) { android.util.Log.w("acl", "launchInternalActivity: no decor for " + targetCls); return false; }
            attachDecorToWindow(decor);
            boolean shown = PresentDriver.setContentView(decor);
            // The outgoing one is stopped once the new one shows, unless the new one is
            // translucent: then it stays on screen under it, paused.
            syncShown();
            // FLAG_ACTIVITY_NEW_TASK | FLAG_ACTIVITY_CLEAR_TASK: the Activities already in the task
            // are finished and the new one becomes its root. An app's router screen starts its
            // home screen this way; kept underneath, the router would come back when home finishes.
            // android:noHistory (or FLAG_ACTIVITY_NO_HISTORY): the Activity the new one covers is
            // finished rather than kept. Pandora's startup screen is one.
            final int clearTask = android.content.Intent.FLAG_ACTIVITY_NEW_TASK
                    | android.content.Intent.FLAG_ACTIVITY_CLEAR_TASK;
            final boolean clear = (flags & clearTask) == clearTask;
            final java.util.List<ActivityRecord> gone = new java.util.ArrayList<ActivityRecord>();
            for (ActivityRecord r : sTask) {
                if (r.activity == a) continue;
                if (clear || (r == prev && noHistory(r.activity))) gone.add(r);
            }
            // Each is finished the way its own finish() would: on a later turn of the main loop.
            // The Activity that called startActivity is often still inside its onCreate (a
            // router screen), and its remaining lifecycle calls must not land on a destroyed one.
            for (ActivityRecord r : gone) {
                System.out.println("acl activity-stack: clearing "
                        + r.activity.getClass().getName()
                        + (clear ? " (clear task)" : " (no history)"));
                finishActivityByToken(r.token, android.app.Activity.RESULT_CANCELED, null);
            }
            android.util.Log.i("acl", "launchInternalActivity: " + targetCls + (shown ? " presented" : " launched (no present)"));
            return shown;
        } catch (Throwable t) {
            // The launch failed. If the new activity was already pushed (attach happens before onCreate),
            // pop it and resume the previous top so the stack + presented screen stay consistent.
            if (a != null && sTask.peek() != null && sTask.peek().activity == a) {
                ActivityRecord bad = sTask.pop(); sByToken.remove(bad.token);
                if (prev != null) {
                    try {
                        invokeActivityLifecycle(prev.activity, "onResume", null, null);
                        android.view.Window pw = prev.activity.getWindow();
                        android.view.View pd = pw != null ? pw.getDecorView() : null;
                        if (pd != null) restoreDecor(pd);
                    } catch (Throwable ignored) {}
                }
            }
            Throwable cause = (t instanceof java.lang.reflect.InvocationTargetException && t.getCause() != null)
                    ? t.getCause() : t;
            android.util.Log.w("acl", "launchInternalActivity " + cmp.getClassName() + " failed: " + cause);
            for (StackTraceElement e : cause.getStackTrace()) {
                if (e.getClassName().startsWith("com.pandora") || e.getClassName().startsWith("android.")
                        || e.getClassName().startsWith("androidx.")) {
                    android.util.Log.w("acl", "    at " + e);
                }
            }
            return false;
        }
    }
    // Invoke a protected Activity lifecycle method (declared on android.app.Activity) reflectively;
    // Method.invoke virtual-dispatches to the app's override. onCreate takes a Bundle, the rest none.
    private static void invokeActivityLifecycle(android.app.Activity a, String name,
                                                Class<?> argType, Object arg) throws Exception {
        java.lang.reflect.Method m = argType != null
                ? android.app.Activity.class.getDeclaredMethod(name, argType)
                : android.app.Activity.class.getDeclaredMethod(name);
        m.setAccessible(true);
        if (argType != null) m.invoke(a, arg); else m.invoke(a);
        // Activity.performResume calls onPostResume after onResume. androidx FragmentActivity
        // resumes its fragments there, not in onResume.
        if ("onResume".equals(name)) invokeActivityLifecycle(a, "onPostResume", null, null);
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
        // Then check the registry itself. An app that renames androidx.lifecycle keeps neither
        // getLifecycle() nor Lifecycle.Event by name, so the registry is told from its shape:
        // handleLifecycleEvent(Event) takes an enum of 7 constants and getCurrentState() returns
        // one of 5. Events go by ordinal, which renaming leaves alone.
        Object reg = null;
        try {
            Object lc = activity.getClass().getMethod("getLifecycle").invoke(activity);
            if (isLifecycleRegistry(lc)) reg = lc;
        } catch (Throwable ignored) {}
        if (reg == null) {
            for (Class<?> c = activity.getClass(); c != null && c != Object.class && reg == null;
                    c = c.getSuperclass()) {
                for (java.lang.reflect.Field f : c.getDeclaredFields()) {
                    if (java.lang.reflect.Modifier.isStatic(f.getModifiers())) continue;
                    Object val;
                    try { f.setAccessible(true); val = f.get(activity); } catch (Throwable ignored) { continue; }
                    if (isLifecycleRegistry(val)) { reg = val; break; }
                }
            }
        }
        if (reg == null) return;
        try {
            java.lang.reflect.Method handle = findEventHandle(reg.getClass());
            java.lang.reflect.Method state = findStateGetter(reg.getClass());
            handle.setAccessible(true);
            state.setAccessible(true);
            Object cur = state.invoke(reg);
            // State ordinals: DESTROYED, INITIALIZED, CREATED, STARTED, RESUMED.
            int at = ((Enum<?>) cur).ordinal();
            if (at == 0 || at == 4) return;
            Object[] events = handle.getParameterTypes()[0].getEnumConstants();
            // Event ordinals: ON_CREATE=0, ON_START=1, ON_RESUME=2. Send only the ones still owed.
            for (int i = at - 1; i <= 2; i++) {
                try { handle.invoke(reg, events[i]); } catch (Throwable ignored) {}
            }
            System.out.println("acl HostBootstrap: lifecycle " + cur + " -> " + state.invoke(reg));
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: ensureResumed skipped: " + t);
        }
    }

    private static boolean isLifecycleRegistry(Object o) {
        return o != null && findEventHandle(o.getClass()) != null
                && findStateGetter(o.getClass()) != null;
    }

    // getCurrentState(): the no-argument method that returns an enum of the 5 Lifecycle.State
    // constants.
    private static java.lang.reflect.Method findStateGetter(Class<?> cls) {
        for (java.lang.reflect.Method m : cls.getMethods()) {
            if (m.getParameterCount() != 0) continue;
            Class<?> rt = m.getReturnType();
            if (rt.isEnum() && rt.getEnumConstants() != null && rt.getEnumConstants().length == 5) return m;
        }
        return null;
    }

    // A LifecycleRegistry exposes exactly one method taking a single enum whose type has the 7
    // Lifecycle.Event constants (ON_CREATE..ON_ANY): that's handleLifecycleEvent(Event).
    private static java.lang.reflect.Method findEventHandle(Class<?> cls) {
        for (java.lang.reflect.Method m : cls.getMethods()) {
            Class<?>[] p = m.getParameterTypes();
            if (p.length == 1 && p[0].isEnum()) {
                Object[] consts = p[0].getEnumConstants();
                if (consts != null && consts.length == 7) return m;
            }
        }
        return null;
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
        // An intent targeting THIS app's own component is internal navigation (a splash routing to
        // the main Activity) -- launch it and present it, don't drop it. Everything else (ACTION_VIEW
        // to a browser, another package) acl can't host, so suppress. Returns true if handled.
        private boolean routeInternal(android.content.Intent intent) {
            if (intent == null || sAppContext == null) return false;
            android.content.ComponentName cmp = intent.getComponent();
            String pkg = sAppContext.getPackageName();
            boolean ownComponent = cmp != null && pkg != null && pkg.equals(cmp.getPackageName());
            if (!ownComponent) return false;
            return launchInternalActivity(intent);
        }
        // `callerToken` is the starting activity's token and `requestCode` its startActivityForResult
        // code (-1 for a plain startActivity). Stash them so the launched activity's record can remember
        // who to return a result to (consumed in registerActivity). Cleared if the launch is suppressed.
        private android.app.Instrumentation.ActivityResult suppress(android.content.Intent intent,
                android.os.IBinder callerToken, int requestCode) {
            // startActivity may be called from any thread, and the Activity is made on the main
            // one: an Activity cannot be constructed on a thread that has no Looper.
            if (android.os.Looper.myLooper() != android.os.Looper.getMainLooper()) {
                new android.os.Handler(android.os.Looper.getMainLooper()).post(
                        () -> suppress(intent, callerToken, requestCode));
                return null;
            }
            // Asked for by an Activity that is being brought back: started once it has been.
            if (sFinishing) {
                new android.os.Handler(android.os.Looper.getMainLooper()).post(
                        () -> suppress(intent, callerToken, requestCode));
                return null;
            }
            sPendingCallerToken = callerToken;
            sPendingRequestCode = requestCode;
            if (routeInternal(intent)) return null;  // internal transition handled
            sPendingCallerToken = null; sPendingRequestCode = -1;  // not launched; don't leak to the next
            // An email launch (ACTION_SENDTO mailto:, or a SEND with recipients) -> the host's default
            // mail client via the xdg Email portal. Checked before Share so an email-share isn't
            // mistaken for a plain text share. Consumes the intent whether or not the portal succeeds.
            if (composeEmailFromIntent(intent)) return null;
            // A Share (ACTION_SEND, or ACTION_CHOOSER wrapping it) has no other app to target on acl,
            // but its text/URL is still useful — route it to the host clipboard so it can be pasted.
            String shared = extractShareText(intent);
            if (shared != null && !shared.isEmpty()) {
                PresentDriver.setClipboard(shared);      // always available to paste
                String url = firstUrl(shared);
                boolean opened = url != null && HostShare.openUri(url);  // + open in the host handler
                System.out.println("acl HostBootstrap: Share -> host clipboard (" + shared.length()
                        + " chars)" + (opened ? " + portal OpenURI " + url : ""));
                return null;
            }
            // A web link an Activity opens goes to the host's handler, as one opened from a
            // Context does.
            try {
                final android.net.Uri d = intent.getData();
                if (android.content.Intent.ACTION_VIEW.equals(intent.getAction()) && d != null
                        && ("http".equalsIgnoreCase(d.getScheme())
                                || "https".equalsIgnoreCase(d.getScheme()))
                        && HostShare.openUri(d.toString())) {
                    return null;
                }
            } catch (Throwable ignored) { }
            System.out.println("acl HostBootstrap: startActivity suppressed (no external-activity host): " + intent);
            return null;
        }

        // Pull the shared text from a Share intent: EXTRA_TEXT of an ACTION_SEND, or of the wrapped
        // EXTRA_INTENT inside an ACTION_CHOOSER (Intent.createChooser). Returns null if not a text share.
        private String extractShareText(android.content.Intent intent) {
            if (intent == null) return null;
            try {
                android.content.Intent send = intent;
                if (android.content.Intent.ACTION_CHOOSER.equals(intent.getAction())) {
                    android.os.Parcelable p = intent.getParcelableExtra(android.content.Intent.EXTRA_INTENT);
                    if (p instanceof android.content.Intent) send = (android.content.Intent) p;
                }
                if (android.content.Intent.ACTION_SEND.equals(send.getAction())) {
                    CharSequence t = send.getCharSequenceExtra(android.content.Intent.EXTRA_TEXT);
                    if (t != null) return t.toString();
                }
            } catch (Throwable ignored) {}
            return null;
        }

        // The first http(s) URL token in a shared string (the whole text is usually just the URL, but
        // some apps prefix "Check out ...:"). Returns null if none — then only the clipboard is used.
        private String firstUrl(String text) {
            if (text == null) return null;
            for (String tok : text.split("\\s+")) {
                if (tok.startsWith("http://") || tok.startsWith("https://")) return tok;
            }
            return null;
        }
        @Override public android.app.Instrumentation.ActivityResult execStartActivity(
                android.content.Context who, android.os.IBinder contextThread, android.os.IBinder token,
                android.app.Activity target, android.content.Intent intent, int requestCode,
                android.os.Bundle options) {
            return suppress(intent, token, requestCode);
        }
        @Override public android.app.Instrumentation.ActivityResult execStartActivity(
                android.content.Context who, android.os.IBinder contextThread, android.os.IBinder token,
                String target, android.content.Intent intent, int requestCode, android.os.Bundle options) {
            if (grantPermissionRequest(who, target, intent, requestCode)) return null;
            return suppress(intent, token, requestCode);
        }

        // Activity.requestPermissions() does not call a service — it builds an ACTION_REQUEST_PERMISSIONS
        // intent and startActivityForResult()s the permission controller, then waits for the result to
        // come back through onRequestPermissionsResult(). acl has no permission controller to launch, so
        // suppressing it silently leaves the app waiting forever: castlab's first screen requests
        // permissions inside a coroutine and sits on its loading spinner. Answer the request
        // directly with what checkSelfPermission returns for each name asked: there is no dialog, so
        // a permission the desktop has not allowed stays denied. Posted rather than called inline
        // because the framework finishes bookkeeping in requestPermissions() after this returns.
        //
        // The answer goes in through Activity.dispatchActivityResult, as the permission
        // controller's does: that is what ends the request. Activity refuses a second request
        // while one is open ("Can request only one set of permissions at a time") and answers it
        // with no names at all, so a result handed straight to onRequestPermissionsResult left
        // every later request refused. `target` says whose request it is, the activity's or one
        // of its fragments'.
        private boolean grantPermissionRequest(android.content.Context who, final String target,
                                               android.content.Intent intent, final int requestCode) {
            if (intent == null || !(who instanceof android.app.Activity)) return false;
            if (!"android.content.pm.action.REQUEST_PERMISSIONS".equals(intent.getAction())) return false;
            final String[] names = intent.getStringArrayExtra(
                    "android.content.pm.extra.REQUEST_PERMISSIONS_NAMES");
            if (names == null || names.length == 0) return false;
            final android.app.Activity a = (android.app.Activity) who;
            final int[] grants = new int[names.length];
            int granted = 0;
            for (int i = 0; i < names.length; i++) {
                grants[i] = HostPermissions.check(names[i], who.getPackageName());
                if (grants[i] == android.content.pm.PackageManager.PERMISSION_GRANTED) granted++;
            }
            final int grantedCount = granted;
            final android.content.Intent result =
                    new android.content.Intent("android.content.pm.action.REQUEST_PERMISSIONS");
            result.putExtra("android.content.pm.extra.REQUEST_PERMISSIONS_NAMES", names);
            result.putExtra("android.content.pm.extra.REQUEST_PERMISSIONS_RESULTS", grants);
            new android.os.Handler(android.os.Looper.getMainLooper()).post(new Runnable() {
                @Override public void run() {
                    java.lang.reflect.Method dispatch = null;
                    try {
                        dispatch = android.app.Activity.class.getDeclaredMethod(
                                "dispatchActivityResult", String.class, int.class, int.class,
                                android.content.Intent.class, String.class);
                        dispatch.setAccessible(true);
                    } catch (Throwable t) {
                        android.util.Log.w("acl", "no Activity.dispatchActivityResult: " + t);
                    }
                    try {
                        if (dispatch != null && target != null) {
                            dispatch.invoke(a, target, requestCode,
                                    android.app.Activity.RESULT_OK, result, "permissions");
                        } else {
                            a.onRequestPermissionsResult(requestCode, names, grants);
                        }
                    } catch (Throwable t) {
                        android.util.Log.w("acl", "onRequestPermissionsResult failed: "
                                + (t instanceof java.lang.reflect.InvocationTargetException
                                        ? t.getCause() : t));
                    }
                }
            });
            android.util.Log.i("acl", "requestPermissions: granted " + grantedCount + " of "
                    + names.length + " permission(s)");
            return true;
        }
        @Override public android.app.Instrumentation.ActivityResult execStartActivity(
                android.content.Context who, android.os.IBinder contextThread, android.os.IBinder token,
                String target, android.content.Intent intent, int requestCode, android.os.Bundle options,
                android.os.UserHandle user) {
            return suppress(intent, token, requestCode);
        }
    }

    // ── Overlay windows (Dialog / PopupWindow / Toast) ────────────────────────────────────────────
    // acl runs a single Wayland surface for the Activity. A Dialog/PopupWindow adds its decor via
    // WindowManager.addView. AclWindowManager (below) intercepts that and records the decor here
    // instead of building a real ViewRootImpl; the present loop composites the list over the
    // Activity. Add order == Z order (last = topmost). removeView drops it (stops drawing).
    // WindowManagerImpl is final and WindowManagerGlobal's ctor private, so acl can't intercept
    // addView — it reads WindowManagerGlobal.mViews (where addView lands the decor) live each frame.
    //
    // A window belongs to the Activity it was opened from, and is shown only while that Activity
    // is the top one: on Android a Dialog left open by an Activity that starts another is under
    // the new Activity, with it. Drawn regardless, it sat over the new screen and took its
    // touches. WhatsApp opens its transcript language screen from a bottom sheet and dismisses
    // the sheet when the Activity under it restarts: the sheet stayed over the language list,
    // and each tap on it opened the list again.
    private static final java.util.WeakHashMap<android.view.View, Boolean> sOverlayAttached =
            new java.util.WeakHashMap<>();

    // The Activity a window was opened from, or null for one opened from no Activity (a Toast,
    // a Service's window). A Dialog's window carries its Activity's token; a PopupWindow's
    // view has its anchor's context, which leads to the Activity.
    private static android.app.Activity overlayOwner(android.view.View v,
            android.view.WindowManager.LayoutParams lp) {
        if (lp.token != null) {
            for (ActivityRecord r : sTask) {
                if (r.token == lp.token) return r.activity;
            }
        }
        android.content.Context c = v.getContext();
        for (int depth = 0; c != null && depth < 16; depth++) {
            if (c instanceof android.app.Activity) return (android.app.Activity) c;
            if (!(c instanceof android.content.ContextWrapper)) break;
            c = ((android.content.ContextWrapper) c).getBaseContext();
        }
        return null;
    }

    public static void presentOverlays(android.graphics.Canvas canvas, int w, int h) {
        if (canvas == null) return;
        try {
            Class<?> wmgCls = Class.forName("android.view.WindowManagerGlobal");
            Object wmg = wmgCls.getMethod("getInstance").invoke(null);
            java.util.List<?> views = new java.util.ArrayList<>((java.util.List<?>) getField(wmgCls, wmg, "mViews"));
            java.util.List<?> params = new java.util.ArrayList<>((java.util.List<?>) getField(wmgCls, wmg, "mParams"));
            java.util.List<?> roots = new java.util.ArrayList<>((java.util.List<?>) getField(wmgCls, wmg, "mRoots"));
            if (OVERLAY_DIAG) {
                android.view.View md = currentTopDecorView();
                StringBuilder sig = new StringBuilder();
                for (int i = 0; i < views.size(); i++) {
                    android.view.View vv = (android.view.View) views.get(i);
                    android.view.WindowManager.LayoutParams pp = i < params.size()
                            ? (android.view.WindowManager.LayoutParams) params.get(i) : null;
                    sig.append(i).append(':').append(vv == null ? "null" : vv.getClass().getSimpleName());
                    sig.append('#').append(vv == null ? 0 : System.identityHashCode(vv));
                    if (vv != null && vv == md) sig.append("=TOPDECOR");
                    if (pp != null) sig.append('[').append(pp.width).append('x').append(pp.height)
                            .append(" type=").append(pp.type).append(" fl=0x").append(Integer.toHexString(pp.flags)).append(']');
                    sig.append(' ');
                }
                String s = sig.toString();
                if (!s.equals(sOverlayRosterSig)) {
                    sOverlayRosterSig = s;
                    System.err.println("acl OVERLAY_ROSTER (" + views.size() + " windows; topDecor#"
                            + (md == null ? 0 : System.identityHashCode(md)) + "): " + s);
                }
            }
            final java.util.ArrayList<ShownOverlay> shown = new java.util.ArrayList<>();
            final ActivityRecord topRecord = sTask.peek();
            final android.app.Activity topActivity = topRecord != null ? topRecord.activity : null;
            for (int i = 0; i < views.size() && i < params.size(); i++) {
                android.view.View v = (android.view.View) views.get(i);
                android.view.WindowManager.LayoutParams lp =
                        (android.view.WindowManager.LayoutParams) params.get(i);
                if (v == null || lp == null) continue;
                // Another Activity's window: under the top Activity, so not drawn and not touched.
                final android.app.Activity owner = overlayOwner(v, lp);
                if (owner != null && owner != topActivity) continue;
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
                if (r != null) shown.add(new ShownOverlay(v, r, lp.flags));
            }
            sShownOverlays = shown;
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: presentOverlays: " + t);
            if (sOverlayFailures++ < 3) t.printStackTrace();
        }
    }

    // The windows drawn in the last frame, bottom to top, with where each was placed, for input
    // routing. Written by presentOverlays and read by routeOverlayTouch, both on the main thread.
    private static final class ShownOverlay {
        final android.view.View view;
        final android.graphics.Rect rect;
        final boolean touchable;     // not FLAG_NOT_TOUCHABLE
        final boolean modal;         // not FLAG_NOT_TOUCH_MODAL: takes touches outside itself too
        final boolean watchOutside;  // FLAG_WATCH_OUTSIDE_TOUCH: told of a touch that lands elsewhere
        ShownOverlay(android.view.View v, android.graphics.Rect r, int flags) {
            view = v; rect = r;
            touchable = (flags & 0x10) == 0;
            modal = (flags & 0x20) == 0;
            watchOutside = (flags & 0x40000) != 0;
        }
    }
    private static int sOverlayFailures;
    private static volatile java.util.List<ShownOverlay> sShownOverlays =
            java.util.Collections.emptyList();
    // The window the gesture under way went down in, and whether it is an overlay's gesture at
    // all: its moves and its up follow the down.
    private static android.view.View sTouchTarget;
    private static boolean sTouchInOverlay;

    // Overlay drop diagnostic (ACL_OVERLAY_DIAG=1): log only on a per-view DRAWN<->DROPPED transition
    // (the flicker signature) so it stays cheap even at the present loop's frame rate.
    private static final boolean OVERLAY_DIAG = System.getenv("ACL_OVERLAY_DIAG") != null
            && !System.getenv("ACL_OVERLAY_DIAG").isEmpty() && !"0".equals(System.getenv("ACL_OVERLAY_DIAG"));
    private static final java.util.Map<android.view.View, Integer> sOverlayDiagState =
            new java.util.WeakHashMap<>();
    private static volatile String sOverlayRosterSig = "";

    // Route a pointer event to the Dialog/PopupWindow windows over the Activity, as the window
    // manager does. A touch goes to the topmost window it lands in; failing that, to the topmost
    // modal window, which takes touches outside itself (that is how a touch beside a PopupWindow
    // dismisses it). Every window above the one that takes it, and every one when none does, is
    // sent ACTION_OUTSIDE if it asked to watch for that. The event is translated into the
    // window's own space, where acl placed it. Returns true if a window took the gesture, so
    // the caller must NOT also deliver it to the Activity.
    //
    // Only the topmost window used to be asked. With a submenu open, a touch on the menu under
    // it went to the Activity, nothing told either popup of it, and both stayed up.
    public static boolean routeOverlayTouch(android.view.MotionEvent me) {
        final java.util.List<ShownOverlay> shown = sShownOverlays;
        final int action = me.getActionMasked();
        if (action != android.view.MotionEvent.ACTION_DOWN) {
            if (!sTouchInOverlay) return false;
            final android.view.View target = sTouchTarget;
            if (action == android.view.MotionEvent.ACTION_UP
                    || action == android.view.MotionEvent.ACTION_CANCEL) {
                sTouchInOverlay = false;
                sTouchTarget = null;
            }
            // A window that went away mid-gesture: the rest of the gesture is dropped.
            for (ShownOverlay s : shown) {
                if (s.view == target) { sendOverlayTouch(s, me, action); break; }
            }
            return true;
        }
        sTouchInOverlay = false;
        sTouchTarget = null;
        final int x = (int) me.getX(), y = (int) me.getY();
        int taker = -1;
        for (int i = shown.size() - 1; i >= 0 && taker < 0; i--) {
            final ShownOverlay s = shown.get(i);
            if (s.touchable && (s.rect.contains(x, y) || s.modal)) taker = i;
        }
        for (int i = shown.size() - 1; i > taker; i--) {
            final ShownOverlay s = shown.get(i);
            if (s.touchable && s.watchOutside)
                sendOverlayTouch(s, me, android.view.MotionEvent.ACTION_OUTSIDE);
        }
        if (taker < 0) return false;
        final ShownOverlay s = shown.get(taker);
        sTouchInOverlay = true;
        sTouchTarget = s.view;
        sendOverlayTouch(s, me, action);
        return true;
    }

    private static void sendOverlayTouch(ShownOverlay s, android.view.MotionEvent me, int action) {
        final android.view.MotionEvent local = android.view.MotionEvent.obtain(me);
        try {
            local.setAction(action);
            local.offsetLocation(-s.rect.left, -s.rect.top);
            s.view.dispatchTouchEvent(local);
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: routeOverlayTouch: " + t);
        } finally {
            local.recycle();
        }
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
    /**
     * Cancels the traversal each added window's ViewRootImpl has scheduled. The present loop
     * calls this every frame after the animation callbacks, which is where a view's
     * postInvalidateOnAnimation schedules one, and before the traversal callbacks are taken
     * up. A traversal that got to run would attach the window's views a second time with
     * ViewRootImpl's own AttachInfo: not hardware accelerated, and in a window that is not
     * visible, so a WebView there stops drawing.
     */
    static void cancelOverlayTraversals() {
        try {
            Class<?> wmgCls = Class.forName("android.view.WindowManagerGlobal");
            Object wmg = wmgCls.getMethod("getInstance").invoke(null);
            java.util.List<?> roots = (java.util.List<?>) getField(wmgCls, wmg, "mRoots");
            if (roots == null || roots.isEmpty()) return;
            for (Object root : new java.util.ArrayList<>(roots)) neuterViewRoot(root);
        } catch (Throwable ignored) {}
    }

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
                    rm.invoke(choreo, 3 /*Choreographer.CALLBACK_TRAVERSAL*/, traversalRunnable, null);
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
        final boolean didLayout = v.isLayoutRequested();
        v.measure(wspec, hspec);
        int mw = v.getMeasuredWidth(), mh = v.getMeasuredHeight();
        if (OVERLAY_DIAG) {
            int st = (mw <= 0 || mh <= 0) ? 0 : 1;
            Integer prev = sOverlayDiagState.get(v);
            if (prev == null || prev.intValue() != st) {
                sOverlayDiagState.put(v, Integer.valueOf(st));
                System.err.println("acl OVERLAY_DIAG: " + v.getClass().getName()
                        + " -> " + (st == 1 ? "DRAWN " + mw + "x" + mh : "DROPPED (measured " + mw + "x" + mh + ")")
                        + " wspec=" + android.view.View.MeasureSpec.toString(wspec)
                        + " hspec=" + android.view.View.MeasureSpec.toString(hspec)
                        + " lp=" + lp.width + "x" + lp.height);
            }
        }
        if (mw <= 0 || mh <= 0) return null;
        v.layout(0, 0, mw, mh);
        int gravity = lp.gravity == 0 ? android.view.Gravity.CENTER : lp.gravity;
        android.graphics.Rect out = new android.graphics.Rect();
        // A PopupWindow's x and y are in the window it was opened from. For one opened from
        // the Activity that is the screen; for one opened from another popup it is that popup.
        android.graphics.Rect frame = popupParentFrame(v);
        if (frame == null) frame = new android.graphics.Rect(0, 0, w, h);
        android.view.Gravity.apply(gravity, mw, mh, frame, lp.x, lp.y, out);
        noteWindowPosition(v, out.left, out.top);
        // The window's tree observer, as for the Activity's decor: a window that starts its
        // opening animation from a pre-draw listener never starts it otherwise. A listener that
        // cancels the draw leaves the window out of this frame.
        if (dispatchTreeObserver(v, didLayout)) return out;
        prepareTextureViews(v);
        int save = canvas.save();
        canvas.translate(out.left, out.top);
        v.draw(canvas);
        canvas.restoreToCount(save);
        return out;
    }

    // Where the window is on the screen, for View.getLocationOnScreen: ViewRootImpl writes it
    // to the AttachInfo from the frame the window manager gives it, and here there is neither.
    // Left at 0,0 a view in a popup reported its place in the popup as its place on the
    // screen, and a PopupWindow anchored to it (a submenu, a dropdown in a dialog) opened
    // there: as far up and left of its anchor as the first popup is from the corner.
    private static void noteWindowPosition(android.view.View decor, int left, int top) {
        try {
            final Object info = getField(android.view.View.class, decor, "mAttachInfo");
            if (info == null) return;
            final Class<?> aiCls = info.getClass();
            if (!Integer.valueOf(left).equals(getField(aiCls, info, "mWindowLeft")))
                setField(aiCls, info, "mWindowLeft", left);
            if (!Integer.valueOf(top).equals(getField(aiCls, info, "mWindowTop")))
                setField(aiCls, info, "mWindowTop", top);
        } catch (Throwable t) {
            if (sOverlayFailures++ < 3) System.err.println("acl HostBootstrap: window position: " + t);
        }
    }

    // The frame of the window a PopupWindow was opened from, when that is another overlay
    // window: the root of its anchor (showAsDropDown) or of its parent view (showAtLocation),
    // where noteWindowPosition last put it. Null for anything else, the Activity's own window
    // among them, which is the screen. PopupWindow works its position out against that root
    // because WindowManagerGlobal knows no window for the token it asks about.
    private static android.graphics.Rect popupParentFrame(android.view.View decor) {
        try {
            if (!"android.widget.PopupWindow$PopupDecorView".equals(decor.getClass().getName()))
                return null;
            final Object popup = getField(decor.getClass(), decor, "this$0");
            android.view.View root = null;
            for (String name : new String[] {"mAnchorRoot", "mParentRootView"}) {
                final Object ref = getField(android.widget.PopupWindow.class, popup, name);
                if (ref instanceof java.lang.ref.WeakReference)
                    root = (android.view.View) ((java.lang.ref.WeakReference<?>) ref).get();
                if (root != null) break;
            }
            if (root == null || root == decor || !Boolean.TRUE.equals(sOverlayAttached.get(root)))
                return null;
            final Object info = getField(android.view.View.class, root, "mAttachInfo");
            if (info == null) return null;
            final int left = (Integer) getField(info.getClass(), info, "mWindowLeft");
            final int top = (Integer) getField(info.getClass(), info, "mWindowTop");
            return new android.graphics.Rect(left, top, left + root.getWidth(), top + root.getHeight());
        } catch (Throwable t) {
            if (sOverlayFailures++ < 3) System.err.println("acl HostBootstrap: popup parent: " + t);
            return null;
        }
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
        // Once only. A second attach makes a new AttachInfo and moves every view to it, and
        // what the first one holds is lost: the tree observer's listeners (a view registers
        // OnGlobalLayoutListener or OnPreDrawListener from its constructor or onAttachedToWindow)
        // are never called again. An Activity started from another's onCreate is attached when
        // it is launched, and was attached again when the present loop opened.
        if (decor.isAttachedToWindow()) return;
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
            // Key dispatch: DecorView.superDispatchKeyEvent -> ViewRootImpl.dispatchUnhandledKeyEvent
            // -> mUnhandledKeyManager.dispatch(mView, event). The ctor-created final mUnhandledKeyManager
            // is skipped by the hollow allocate -> NPE that aborts the WHOLE key dispatch before the
            // focused view (an EditText) ever sees onKeyDown -> physical keyboard typing does nothing.
            // Seed a real one (a no-arg static inner class).
            try {
                Class<?> ukmCls = Class.forName("android.view.ViewRootImpl$UnhandledKeyManager");
                java.lang.reflect.Constructor<?> ukmCtor = ukmCls.getDeclaredConstructor();
                ukmCtor.setAccessible(true);
                setField(vriCls, vri, "mUnhandledKeyManager", ukmCtor.newInstance());
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
            // A text-input view (EditText) registers a stylus-handwriting area on attach:
            // View.updateHandwritingArea -> getViewRootImpl().getHandwritingInitiator()
            // .updateHandwritingAreasForView(this). The hollow ViewRootImpl's final mHandwritingInitiator
            // is null (ctor-created, skipped) -> NPE on attach. Seed it (API 33+; absent on older
            // frameworks -> caught): HandwritingInitiator(ViewConfiguration, InputMethodManager).
            try {
                Class<?> hiCls = Class.forName("android.view.HandwritingInitiator");
                java.lang.reflect.Constructor<?> hiCtor = hiCls.getDeclaredConstructor(
                        android.view.ViewConfiguration.class,
                        android.view.inputmethod.InputMethodManager.class);
                hiCtor.setAccessible(true);
                Object imm = ctx.getSystemService(android.view.inputmethod.InputMethodManager.class);
                setField(vriCls, vri, "mHandwritingInitiator",
                        hiCtor.newInstance(android.view.ViewConfiguration.get(ctx), imm));
            } catch (Throwable ignored) {}
            // SurfaceView.onAttachedToWindow calls getViewRootImpl().addSurfaceChangedCallback ->
            // mSurfaceChangedCallbacks.add, a final ArrayList the hollow ViewRootImpl leaves null.
            // Seed it so GLSurfaceView apps (halma, doodledudette) attach their decor.
            try {
                setField(vriCls, vri, "mSurfaceChangedCallbacks", new java.util.ArrayList<>());
            } catch (Throwable ignored) {}
            // Chromium's WindowAndroid.<init> (API-33 path) calls ViewRootImpl
            // .addOnBufferTransformHintChangedListener -> mTransformHintListeners.contains(), a
            // final ArrayList (added in Android 13) the hollow ViewRootImpl leaves null -> NPE in
            // AwContents.<init>, so a WebView hosted in a Compose AndroidView (e.g. Areada's book
            // reader) never initializes. Seed it (field absent on pre-T frameworks -> caught).
            try {
                setField(vriCls, vri, "mTransformHintListeners", new java.util.ArrayList<>());
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
                // When the overlay requests the IME (a focusable Compose field / a sheet grabbing focus),
                // InsetsController.show(ime) -> ImeInsetsSourceConsumer.requestShow calls
                // host.getInputMethodManager().requestImeShow(...) — a null IMM here throws an UNCAUGHT
                // NPE inside the Choreographer doFrame callback, killing the Looper thread and the whole
                // app (the areada settings sheet "opens then vanishes"). Provide a PLAIN stub IMM: its
                // stub mService no-ops requestImeShow, so the null-check is satisfied WITHOUT actually
                // showing the IME. (Using the getSystemService IMM instead routes requestImeShow ->
                // setImeVisible, whose IME-inset animation makes the sheet toggle in/out — a slow flicker.)
                Object hostImm = null;
                try {
                    java.lang.reflect.Method csi = android.view.inputmethod.InputMethodManager.class
                            .getDeclaredMethod("createStubInstance", int.class, android.os.Looper.class);
                    csi.setAccessible(true);
                    hostImm = csi.invoke(null, 0 /* Display.DEFAULT_DISPLAY */, android.os.Looper.getMainLooper());
                } catch (Throwable ignored) {}
                if (hostImm == null) {  // fallback: any non-null IMM beats an NPE
                    try { hostImm = ctx.getSystemService(android.view.inputmethod.InputMethodManager.class); }
                    catch (Throwable ignored) {}
                }
                final Object fHostImm = hostImm;
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
                                case "getInputMethodManager": return fHostImm;
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
            // A non-null window token. View.getWindowToken() returns mAttachInfo.mWindowToken, and
            // several views GATE on it: ViewPager.populate() early-returns while getWindowToken()==null,
            // so the pager never instantiates its pages — the Telegram intro's ViewPager sat at
            // childViews=0 (adapter had 6 pages) and could not be swiped. acl has no Binder window, but
            // a plain Binder is a valid IBinder and satisfies the non-null gate; populate() then runs on
            // the next measure and the pages (and swipe) come alive.
            try { setField(aiCls, attachInfo, "mWindowToken", new android.os.Binder()); } catch (Throwable ignored) {}
            // KeyEvent tracking state. The BACK key is delivered as a down+up pair; Activity.onKeyDown
            // (KEYCODE_BACK) calls event.startTracking() and onKeyUp fires onBackPressed() only if
            // event.isTracking() — which needs a non-null DispatcherState here (View.dispatchKeyEvent
            // passes mAttachInfo.mKeyDispatchState). Without it, startTracking is a no-op, the up isn't
            // tracked, onBackPressed never fires, and BACK/finish() → the task stack never pops.
            try {
                setField(aiCls, attachInfo, "mKeyDispatchState", new android.view.KeyEvent.DispatcherState());
            } catch (Throwable ignored) {}
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
                // HardwareRenderer.registerAnimatingRenderNode reads mRootNode.mNativeRenderNode; the
                // hollow renderer (no ctor) leaves mRootNode null, so a RippleDrawable / reveal / AVD
                // animation NPEs mid-View.draw and aborts the whole frame (buttons stop responding
                // visually while a ripple is pending). Seed a real RootRenderNode so the register call
                // resolves to acl's no-op native instead of dereferencing null.
                try {
                    Object rootNode = Class.forName("android.graphics.RenderNode")
                            .getConstructor(String.class).newInstance("acl$rt$root");
                    setField(Class.forName("android.graphics.HardwareRenderer"), tr, "mRootNode", rootNode);
                } catch (Throwable ignored) {}
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
          .append(" enabled=").append(v.isEnabled())
          // hscroll/vscroll = canScroll{Horizontally,Vertically}(-1),(+1): a ViewPager/scroller only
          // intercepts a drag in a direction it reports it CAN scroll — false,false means it won't
          // page/scroll no matter how far you drag (empty adapter / single page / wrong bounds).
          .append(" hscroll=").append(v.canScrollHorizontally(-1)).append(',').append(v.canScrollHorizontally(1))
          .append(" vscroll=").append(v.canScrollVertically(-1)).append(',').append(v.canScrollVertically(1))
          .append(" attached=").append(v.isAttachedToWindow());
        if (v.getId() != android.view.View.NO_ID) {
            try {
                sb.append(" id=").append(v.getResources().getResourceEntryName(v.getId()));
            } catch (RuntimeException e) {
                // An id set in code has no resource name.
            }
        }
        if (v.getClass().getName().contains("RecyclerView")) {
            try {
                Object ad = v.getClass().getMethod("getAdapter").invoke(v);
                sb.append(" [LIST adapter=").append(ad == null ? "null" : ad.getClass().getName());
                if (ad != null) {
                    sb.append(" count=").append(ad.getClass().getMethod("getItemCount").invoke(ad));
                }
                sb.append(']');
            } catch (ReflectiveOperationException e) {
                // getAdapter or getItemCount renamed: nothing to add.
            }
        }
        // Only when set: a view can be laid out and visible and still show nothing.
        if (v.getAlpha() != 1f) sb.append(" alpha=").append(v.getAlpha());
        if (v.getTranslationX() != 0f || v.getTranslationY() != 0f) {
            sb.append(" trans=").append(v.getTranslationX()).append(',').append(v.getTranslationY());
        }
        if (v.getScaleX() != 1f || v.getScaleY() != 1f) {
            sb.append(" scale=").append(v.getScaleX()).append(',').append(v.getScaleY());
        }
        if (v instanceof android.widget.ImageView) {
            android.graphics.drawable.Drawable d = ((android.widget.ImageView) v).getDrawable();
            sb.append(" drawable=").append(d == null ? "null" : d.getClass().getSimpleName()
                    + " " + d.getIntrinsicWidth() + "x" + d.getIntrinsicHeight());
        }
        if (v instanceof android.widget.TextView) {
            sb.append(" textLen=").append(((android.widget.TextView) v).length());
        }
        // A ViewPager with an adapter but no child views never populated its pages (nothing to swipe).
        if (v.getClass().getName().contains("ViewPager")) {
            try {
                Object ad = v.getClass().getMethod("getAdapter").invoke(v);
                int cnt = ad == null ? -1 : (Integer) ad.getClass().getMethod("getCount").invoke(ad);
                sb.append(" [PAGER adapter=").append(ad == null ? "null" : ad.getClass().getSimpleName())
                  .append(" count=").append(cnt)
                  .append(" childViews=").append(((android.view.ViewGroup) v).getChildCount())
                  .append(" curItem=").append(v.getClass().getMethod("getCurrentItem").invoke(v)).append(']');
            } catch (Throwable t) { sb.append(" [PAGER reflect-fail: ").append(t).append(']'); }
        }
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
        public int getAssociatedDisplayId() { return 0; }
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
                (p, method, a) -> "getWindowId".equals(method.getName())
                        ? newWindowId(method.getReturnType())
                        : defaultValue(method.getReturnType()));
    }

    // IWindowSession.getWindowId: an IWindowId with a binder of its own. View.getWindowId asks
    // once per window and keeps the answer, so one made per call is one per window, and two
    // WindowIds are equal exactly when they are the same window's. The default stand-in's
    // asBinder() is null, and WindowId.equals calls equals on it: a PopupWindow that ended its
    // exit transition while another window's was running died in Transition.forceToEnd with a
    // NullPointerException, half removed, and stayed on the screen.
    private static Object newWindowId(Class<?> iface) {
        final android.os.IBinder binder = new android.os.Binder();
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> "asBinder".equals(method.getName())
                        ? binder : defaultValue(method.getReturnType()));
    }

    // IBinders an app has written to a Parcel, by the handle written in their place (jvm_vm's
    // Parcel natives). A Parcel does not leave the process, so the handle reads back as the
    // same object. Kept for the life of the process: the bytes of a Parcel can be read long
    // after they were written, and an app writes few distinct binders.
    private static final java.util.IdentityHashMap<android.os.IBinder, Long> sBinderHandles =
            new java.util.IdentityHashMap<android.os.IBinder, Long>();
    private static final java.util.ArrayList<android.os.IBinder> sBindersByHandle =
            new java.util.ArrayList<android.os.IBinder>();

    /** The handle for a binder, never 0. Called from Parcel.nativeWriteStrongBinder. */
    public static long binderHandle(android.os.IBinder binder) {
        synchronized (sBinderHandles) {
            Long handle = sBinderHandles.get(binder);
            if (handle == null) {
                sBindersByHandle.add(binder);
                handle = Long.valueOf(sBindersByHandle.size());  // 1-based; 0 is null
                sBinderHandles.put(binder, handle);
            }
            return handle;
        }
    }

    /** The binder a handle was given for, or null. Called from Parcel.nativeReadStrongBinder. */
    public static android.os.IBinder binderForHandle(long handle) {
        synchronized (sBinderHandles) {
            return handle >= 1 && handle <= sBindersByHandle.size()
                    ? sBindersByHandle.get((int) (handle - 1)) : null;
        }
    }

    // One PendingIntent the app made: what to send and how.
    private static final class IntentSenderRecord {
        int type;              // ActivityManager.INTENT_SENDER_*
        int requestCode;
        android.content.Intent intent;
        boolean immutable;     // made with FLAG_IMMUTABLE
        Object sender;         // the IIntentSender handed to the app
    }
    // Guarded by itself. An app makes these from any thread.
    private static final java.util.List<IntentSenderRecord> sIntentSenders =
            new java.util.ArrayList<IntentSenderRecord>();
    private static final int INTENT_SENDER_BROADCAST = 1, INTENT_SENDER_ACTIVITY = 2,
            INTENT_SENDER_SERVICE = 4, INTENT_SENDER_FOREGROUND_SERVICE = 5;

    // The notification id each foreground Service gave startForeground.
    private static final java.util.Map<android.content.ComponentName, Integer> sForegroundIds =
            new java.util.concurrent.ConcurrentHashMap<android.content.ComponentName, Integer>();

    private static IntentSenderRecord intentSenderFor(Object sender) {
        synchronized (sIntentSenders) {
            for (IntentSenderRecord r : sIntentSenders) if (r.sender == sender) return r;
        }
        return null;
    }

    // PendingIntent.getActivity/getBroadcast/getService. Two requests of the same kind, request
    // code and Intent (by filterEquals) are one PendingIntent, as on Android; the flags decide
    // whether the existing one is returned, updated, replaced or required.
    private static Object getIntentSender(Class<?> senderIface, int type, int requestCode,
            android.content.Intent[] intents, int flags) {
        if (intents == null || intents.length == 0 || intents[intents.length - 1] == null) {
            return null;
        }
        final android.content.Intent intent = intents[intents.length - 1];
        synchronized (sIntentSenders) {
            IntentSenderRecord found = null;
            for (IntentSenderRecord r : sIntentSenders) {
                if (r.type == type && r.requestCode == requestCode
                        && r.intent.filterEquals(intent)) {
                    found = r;
                    break;
                }
            }
            if ((flags & android.app.PendingIntent.FLAG_NO_CREATE) != 0) {
                return found != null ? found.sender : null;
            }
            if (found != null && (flags & android.app.PendingIntent.FLAG_CANCEL_CURRENT) != 0) {
                sIntentSenders.remove(found);
                found = null;
            }
            if (found != null) {
                if ((flags & android.app.PendingIntent.FLAG_UPDATE_CURRENT) != 0) {
                    found.intent = new android.content.Intent(intent);
                }
                return found.sender;
            }
            final IntentSenderRecord r = new IntentSenderRecord();
            r.type = type;
            r.requestCode = requestCode;
            r.intent = new android.content.Intent(intent);
            r.immutable = (flags & android.app.PendingIntent.FLAG_IMMUTABLE) != 0;
            // PendingIntent compares and hashes the sender's binder, so each has its own.
            final android.os.IBinder binder = new android.os.Binder();
            r.sender = java.lang.reflect.Proxy.newProxyInstance(
                    senderIface.getClassLoader(), new Class<?>[]{senderIface},
                    (sp, sm, sa) -> {
                        switch (sm.getName()) {
                            case "asBinder": return binder;
                            case "send":  // (code, intent, resolvedType, token, receiver, ...)
                                sendIntentSender(sp, (android.content.Intent) sa[1]);
                                return null;
                            case "hashCode": return System.identityHashCode(sp);
                            case "equals": return sp == sa[0];
                            case "toString": return "IntentSender{" + binder + "}";
                            default: return defaultValue(sm.getReturnType());
                        }
                    });
            sIntentSenders.add(r);
            return r.sender;
        }
    }

    // PendingIntent.send. The Intent goes out on the main thread, to this app only: a4l runs one
    // app, and every PendingIntent here is one the app made for itself. Returns false for a
    // sender that is unknown or cancelled.
    private static boolean sendIntentSender(Object sender, android.content.Intent fillIn) {
        final IntentSenderRecord r = intentSenderFor(sender);
        final android.content.Context ctx = sAppContext;
        if (r == null || ctx == null) return false;
        final android.content.Intent intent;
        final int type;
        synchronized (sIntentSenders) {
            intent = new android.content.Intent(r.intent);
            type = r.type;
        }
        if (fillIn != null) intent.fillIn(fillIn, 0);
        new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
            try {
                switch (type) {
                    case INTENT_SENDER_BROADCAST:
                        ctx.sendBroadcast(intent);
                        break;
                    case INTENT_SENDER_ACTIVITY:
                        intent.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
                        ctx.startActivity(intent);
                        break;
                    case INTENT_SENDER_SERVICE:
                    case INTENT_SENDER_FOREGROUND_SERVICE:
                        ctx.startService(intent);
                        break;
                    default:
                        System.err.println("acl HostBootstrap: PendingIntent kind " + type
                                + " not sent");
                }
            } catch (RuntimeException e) {
                System.err.println("acl HostBootstrap: PendingIntent send failed: " + e);
            }
        });
        return true;
    }

    // ActivityManager.getMemoryInfo: the host's memory, from /proc/meminfo. Left to the default
    // every field was 0, and an app that checks for room before a large allocation found none:
    // WhatsApp sizes its speech model against availMem and gave up on every transcription
    // ("transcription failed status=17"). The low-memory threshold is a sixteenth of the total;
    // the host has no figure of its own for it.
    private static void fillMemoryInfo(android.app.ActivityManager.MemoryInfo info) {
        if (info == null) return;
        long total = 0, avail = 0;
        try (java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.FileReader("/proc/meminfo"))) {
            String line;
            while ((line = r.readLine()) != null && (total == 0 || avail == 0)) {
                final boolean isTotal = line.startsWith("MemTotal:");
                if (!isTotal && !line.startsWith("MemAvailable:")) continue;
                // "MemTotal:       63365608 kB"
                final String[] f = line.trim().split("\\s+");
                if (f.length < 2) continue;
                final long bytes = Long.parseLong(f[1]) * 1024L;
                if (isTotal) total = bytes; else avail = bytes;
            }
        } catch (java.io.IOException | NumberFormatException e) {
            android.util.Log.w("acl", "getMemoryInfo: /proc/meminfo: " + e);
        }
        info.totalMem = total;
        info.availMem = avail;
        info.threshold = total / 16;
        info.lowMemory = avail < info.threshold;
    }

    // IActivityManager. PendingIntents are kept and sent in this process, and a foreground
    // Service's notification goes to the desktop; everything else defaults (an empty process
    // list and the like, enough for the probes apps make at start).
    // IWindowManager. The animation scales are 1: WindowManagerGlobal hands
    // getCurrentAnimatorScale() to ValueAnimator.setDurationScale the first time anything asks
    // for the window manager, and the default 0 made every animator in the app end on its
    // first frame. A window drew the first frame of its opening transition and then its last:
    // WhatsApp's overflow menu showed for one frame as a grey sliver over the toolbar, where
    // its reveal starts from, before it appeared whole. Everything else defaults as before.
    private static Object makeWindowManagerHandler(Class<?> iface) {
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> {
                    switch (method.getName()) {
                        case "getCurrentAnimatorScale":
                        case "getAnimationScale":   // (which)
                            return 1.0f;
                        case "getAnimationScales":
                            return new float[] {1.0f, 1.0f, 1.0f};  // window, transition, animator
                        default:
                            return defaultValue(method.getReturnType());
                    }
                });
    }

    private static Object makeActivityManagerHandler(Class<?> iface) {
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> {
                    switch (method.getName()) {
                        case "getMemoryInfo":
                            fillMemoryInfo((android.app.ActivityManager.MemoryInfo) a[0]);
                            return null;
                        case "getIntentSender":
                            // (type, pkg, token, resultWho, requestCode, intents, types, flags, ...)
                            return getIntentSender(method.getReturnType(), (Integer) a[0],
                                    (Integer) a[4], (android.content.Intent[]) a[5],
                                    (Integer) a[7]);
                        case "getIntentSenderWithFeature":
                            // (type, pkg, feature, token, resultWho, requestCode, intents, types,
                            //  flags, ...)
                            return getIntentSender(method.getReturnType(), (Integer) a[0],
                                    (Integer) a[5], (android.content.Intent[]) a[6],
                                    (Integer) a[8]);
                        case "sendIntentSender":
                            // (target, token, code, intent, ...). 0 is ActivityManager's
                            // START_SUCCESS, -96 its START_CANCELED.
                            return sendIntentSender(a[0], (android.content.Intent) a[3]) ? 0 : -96;
                        case "cancelIntentSender": {
                            final IntentSenderRecord r = intentSenderFor(a[0]);
                            if (r != null) {
                                synchronized (sIntentSenders) { sIntentSenders.remove(r); }
                            }
                            return null;
                        }
                        case "getPackageForIntentSender":
                            return intentSenderFor(a[0]) != null && sAppContext != null
                                    ? sAppContext.getPackageName() : null;
                        case "getInfoForIntentSender": {
                            // What PendingIntent.getCreatorPackage, getCreatorUid, isImmutable
                            // and isActivity/isBroadcast/isService read. Every PendingIntent
                            // here is the app's own, so one that was cancelled still names the
                            // app; its kind is no longer known (0, INTENT_SENDER_UNKNOWN).
                            final IntentSenderRecord r = intentSenderFor(a[0]);
                            final String pkg =
                                    sAppContext != null ? sAppContext.getPackageName() : null;
                            return new android.app.ActivityManager.PendingIntentInfo(pkg,
                                    android.os.Process.myUid(), r != null && r.immutable,
                                    r != null ? r.type : 0);
                        }
                        case "isIntentSenderAnActivity": {
                            final IntentSenderRecord r = intentSenderFor(a[0]);
                            return r != null && r.type == INTENT_SENDER_ACTIVITY;
                        }
                        case "setServiceForeground": {
                            // (component, token, id, notification, flags, type).
                            // stopForeground sends id 0 and no notification; flag 1
                            // (STOP_FOREGROUND_REMOVE) takes the notification down too.
                            final android.content.ComponentName cn =
                                    (android.content.ComponentName) a[0];
                            final android.app.Notification n = (android.app.Notification) a[3];
                            if (n != null) {
                                sForegroundIds.put(cn, (Integer) a[2]);
                                NotifyBridge.post(cn.getPackageName(), (Integer) a[2], n);
                            } else if (((Integer) a[4] & 1) != 0) {
                                final Integer id = sForegroundIds.remove(cn);
                                if (id != null) NotifyBridge.cancel(id);
                            }
                            return null;
                        }
                        default:
                            return defaultValue(method.getReturnType());
                    }
                });
    }

    // IActivityTaskManager: getActivityClientController() hands back a controller whose finishActivity
    // (token, code, data, finishTask) drives acl's Activity task stack — Activity.finish() ->
    // ActivityClient.finishActivity(mToken,...) -> here. The toolbar Up arrow, the BACK key
    // (onBackPressed -> finish), and programmatic finish() all converge on this one seam. Every other
    // IActivityTaskManager/controller method still defaults (no-op), preserving prior behavior.
    private static Object makeActivityTaskHandler(Class<?> iface) {
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> {
                    if ("getActivityClientController".equals(method.getName())) {
                        final Class<?> ctrl = method.getReturnType();  // IActivityClientController
                        return java.lang.reflect.Proxy.newProxyInstance(
                                ctrl.getClassLoader(), new Class<?>[]{ctrl},
                                (cp, cm, ca) -> {
                                    // Up navigation (AppCompatActivity.onSupportNavigateUp -> Activity
                                    // .navigateUpTo). target=ca[1]; go to it with CLEAR_TOP so acl reuses
                                    // the existing parent (MainActivity) and finishes everything above it
                                    // (the detail). Posted to avoid reentrancy: this runs inside the Up
                                    // arrow's onClick, and launchInternalActivity destroys the source.
                                    if ("navigateUpTo".equals(cm.getName()) && ca != null && ca.length >= 2
                                            && ca[1] instanceof android.content.Intent) {
                                        final android.content.Intent target = (android.content.Intent) ca[1];
                                        target.addFlags(android.content.Intent.FLAG_ACTIVITY_CLEAR_TOP);
                                        new android.os.Handler(android.os.Looper.getMainLooper())
                                                .post(() -> launchInternalActivity(target));
                                        return cm.getReturnType() == boolean.class ? Boolean.TRUE
                                                : defaultValue(cm.getReturnType());
                                    }
                                    if ("finishActivity".equals(cm.getName()) && ca != null && ca.length >= 1
                                            && ca[0] instanceof android.os.IBinder) {
                                        int code = ca.length > 1 && ca[1] instanceof Integer
                                                ? (Integer) ca[1] : android.app.Activity.RESULT_CANCELED;
                                        android.content.Intent data = ca.length > 2 && ca[2] instanceof android.content.Intent
                                                ? (android.content.Intent) ca[2] : null;
                                        boolean handled = finishActivityByToken((android.os.IBinder) ca[0], code, data);
                                        return cm.getReturnType() == boolean.class ? handled : defaultValue(cm.getReturnType());
                                    }
                                    return defaultValue(cm.getReturnType());
                                });
                    }
                    return defaultValue(method.getReturnType());
                });
    }

    // INotificationManager translated to the host notification daemon (svc/notify).
    // enqueue → NotifyBridge.post (host Notify), cancel → NotifyBridge.cancel. The app's
    // channels are kept, so it reads back what it created (an app checks a channel's
    // importance before startForeground); nothing else is done with them. Everything else
    // defaults.
    private static Object makeNotificationHandler(Class<?> iface) {
        final java.util.Map<String, android.app.NotificationChannel> channels =
                java.util.Collections.synchronizedMap(
                        new java.util.LinkedHashMap<String, android.app.NotificationChannel>());
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> {
                    switch (method.getName()) {
                        case "enqueueNotificationWithTag":
                            // (String pkg, String opPkg, String tag, int id, Notification n, int user)
                            NotifyBridge.post((String) a[0], (Integer) a[3],
                                    (android.app.Notification) a[4]);
                            return null;
                        case "cancelNotificationWithTag":
                            NotifyBridge.cancel((Integer) a[3]);
                            return null;
                        case "cancelAllNotifications":
                            NotifyBridge.cancelAll();
                            return null;
                        case "createNotificationChannels": {
                            // (String pkg, ParceledListSlice channels). Creating one that
                            // exists keeps the first, as Android keeps the user's settings.
                            java.util.List<?> list = (java.util.List<?>) a[1].getClass()
                                    .getMethod("getList").invoke(a[1]);
                            for (Object c : list) {
                                if (c instanceof android.app.NotificationChannel) {
                                    android.app.NotificationChannel ch =
                                            (android.app.NotificationChannel) c;
                                    synchronized (channels) {
                                        if (!channels.containsKey(ch.getId()))
                                            channels.put(ch.getId(), ch);
                                    }
                                }
                            }
                            return null;
                        }
                        case "getNotificationChannel":              // (callingPkg, user, pkg, id)
                        case "getConversationNotificationChannel":  // (callingPkg, user, pkg, id, ...)
                            return channels.get((String) a[3]);
                        case "deleteNotificationChannel":           // (pkg, id)
                            channels.remove((String) a[1]);
                            return null;
                        case "getNotificationChannels": {
                            // A ParceledListSlice, never null: NotificationManager calls
                            // getList() on it.
                            java.util.List<android.app.NotificationChannel> all;
                            synchronized (channels) {
                                all = new java.util.ArrayList<>(channels.values());
                            }
                            return method.getReturnType().getConstructor(java.util.List.class)
                                    .newInstance(all);
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
                            // AudioManager.getAvailableCommunicationDevices() walks this array
                            // without a null check. Chromium calls it when a page starts media
                            // (AudioManagerAndroid.getAudioInputDeviceNames), and ends the
                            // process on the NullPointerException. No devices, not null.
                            case "getAvailableCommunicationDeviceIds":
                                return new int[0];
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
                    if ("getWindowId".equals(n)) return newWindowId(method.getReturnType());
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
                        // Never null on Android. No Wi-Fi display here: the default status is
                        // "feature unavailable". android.media.MediaRouter reads it unguarded.
                        case "getWifiDisplayStatus":
                            return method.getReturnType().getConstructor().newInstance();
                        default: return defaultValue(method.getReturnType());
                    }
                });
    }

    // Live logind inhibitor cookies for the wake locks the app currently holds, keyed by the
    // IBinder token PowerManager.WakeLock passes to acquire/releaseWakeLock.
    private static final java.util.Map<android.os.IBinder, Long> sWakeLocks =
            java.util.Collections.synchronizedMap(new java.util.HashMap<>());

    private static Object makePowerHandler(Class<?> iface) {
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> {
                    // Backed by svc/power (systemd-logind session state + power-profiles), not
                    // hardcoded: isInteractive follows the session being active/unlocked/non-idle,
                    // isPowerSaveMode follows the host power profile.
                    switch (method.getName()) {
                        case "isInteractive":
                        case "isDisplayInteractive":
                            return PowerBridge.isInteractive();
                        case "isPowerSaveMode":
                            return PowerBridge.isPowerSaveMode();
                        case "isDeviceIdleMode":
                        case "isLightDeviceIdleMode":
                            return PowerBridge.isDeviceIdleMode();
                        // A held Android wake lock becomes a real logind inhibitor, so playback
                        // genuinely keeps the host awake. acquireWakeLock(IBinder lock, int flags,
                        // String tag, ...) / releaseWakeLock(IBinder lock, int flags).
                        case "acquireWakeLock":
                        case "acquireWakeLockWithUid": {
                            android.os.IBinder tok = (a != null && a.length > 0
                                    && a[0] instanceof android.os.IBinder) ? (android.os.IBinder) a[0] : null;
                            int flags = (a != null && a.length > 1 && a[1] instanceof Integer)
                                    ? (Integer) a[1] : 1 /* PARTIAL_WAKE_LOCK */;
                            String tag = (a != null && a.length > 2 && a[2] instanceof String)
                                    ? (String) a[2] : "";
                            long cookie = PowerBridge.acquireWakeLock(flags, tag);
                            if (tok != null && cookie != 0L) sWakeLocks.put(tok, cookie);
                            return defaultValue(method.getReturnType());
                        }
                        case "releaseWakeLock": {
                            android.os.IBinder tok = (a != null && a.length > 0
                                    && a[0] instanceof android.os.IBinder) ? (android.os.IBinder) a[0] : null;
                            Long cookie = tok != null ? sWakeLocks.remove(tok) : null;
                            if (cookie != null) PowerBridge.releaseWakeLock(cookie);
                            return defaultValue(method.getReturnType());
                        }
                        default: return defaultValue(method.getReturnType());
                    }
                });
    }

    // ISessionManager: the app's media sessions are kept by MediaSessions, which answers
    // ISession and ISessionController too.
    private static Object makeSessionManagerHandler(Class<?> iface) {
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> MediaSessions.managerCall(method, a));
    }

    // A session's MediaController.PlaybackInfo. A session always has one on Android
    // (MediaController.getPlaybackInfo is never null there), and MediaControllerCompat callers
    // read getPlaybackInfo().getPlaybackType() unguarded. Local playback on the music stream,
    // with the host's master volume.
    static Object sessionVolumeAttributes(Class<?> type) {
        try {
            final int music = android.media.AudioManager.STREAM_MUSIC;
            java.lang.reflect.Constructor<?> c = type.getDeclaredConstructor(int.class,
                    int.class, int.class, int.class, android.media.AudioAttributes.class,
                    String.class);
            c.setAccessible(true);
            return c.newInstance(
                    android.media.session.MediaController.PlaybackInfo.PLAYBACK_TYPE_LOCAL,
                    android.media.VolumeProvider.VOLUME_CONTROL_ABSOLUTE,
                    AudioBridge.maxVolume(music), AudioBridge.getVolume(music),
                    new android.media.AudioAttributes.Builder()
                            .setLegacyStreamType(music).build(),
                    null);
        } catch (Throwable t) {
            android.util.Log.w("acl", "session volume attributes: " + t);
            return null;
        }
    }

    // IThermalService: the kernel thermal zones, via svc/power.
    private static Object makeThermalHandler(Class<?> iface) {
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> {
                    String n = method.getName();
                    if ("getCurrentThermalStatus".equals(n)) return PowerBridge.thermalStatus();
                    // IThermalService's (un)register* return boolean, and the framework reads false
                    // as failure: PowerManager.addThermalStatusListener throws
                    // RuntimeException("Listener failed to set") on it. That escapes into callers
                    // that do not expect it -- chromium's PowerMonitor.isBatteryPower() takes it
                    // during WebView startup, which aborts the renderer
                    // ([FATAL:jni_android.cc] uncaught Java exception) and kills every WebView app.
                    // acl has no thermal callbacks to deliver, but accepting the registration is the
                    // honest answer: the listener is held, it simply never fires.
                    if ((n.startsWith("register") || n.startsWith("unregister"))
                            && method.getReturnType() == boolean.class)
                        return Boolean.TRUE;
                    return defaultValue(method.getReturnType());
                });
    }

    private static Object makeUiModeHandler(Class<?> iface) {
        return java.lang.reflect.Proxy.newProxyInstance(
                iface.getClassLoader(), new Class<?>[]{iface},
                (p, method, a) -> {
                    // Desktop host: normal UI mode, not car/desk/tv/watch. (Automotive car-mode is a
                    // separate concern — forcing UI_MODE_TYPE_CAR here would flip apps to car UI.) The
                    // day/night uiMode is carried on the Configuration (setColorScheme/sNightMode), so
                    // getNightMode and the rest default; enable/disableCarMode etc. no-op.
                    if ("getCurrentModeType".equals(method.getName()))
                        return android.content.res.Configuration.UI_MODE_TYPE_NORMAL;
                    return defaultValue(method.getReturnType());
                });
    }

    // Read android.title / android.text out of a Notification's extras Bundle (reflectively —
    // HostBootstrap compiles against the framework classes.jar).
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

    // MediaSessionManager(Context) resolves ISessionManager via
    // MediaFrameworkPlatformInitializer.getMediaServiceManager(), which the framework normally seeds
    // during boot but acl does not — so it is null and the constructor NPEs, leaving
    // getSystemService(MEDIA_SESSION_SERVICE) null and every MediaSession/MediaSessionCompat
    // construction to NPE on it (the entry point for every media app, and the automotive media
    // path). Seed a real MediaServiceManager: its per-service ServiceRegisterers resolve through the
    // (no-Binder) ServiceManager, where "media_session" is seeded, so the manager builds and its
    // createSession() reaches acl's no-op ISession. Idempotent (the setter throws if called twice).
    private static void seedMediaFrameworkInitializer() {
        try {
            Class<?> mfpi = Class.forName("android.media.MediaFrameworkPlatformInitializer");
            Class<?> msm = Class.forName("android.media.MediaServiceManager");
            if (mfpi.getMethod("getMediaServiceManager").invoke(null) == null)
                mfpi.getMethod("setMediaServiceManager", msm)
                    .invoke(null, msm.getConstructor().newInstance());
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: media framework init seed skipped: " + t);
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
        // IActivityTaskManager.getActivityClientController() → a controller that drives acl's Activity
        // task stack: finishActivity(token,…) pops+restores (makeActivityTaskHandler), everything else
        // (setTaskDescription during theme init, activityIdle/Paused/…) defaults harmlessly.
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
        // TelephonyRegistryManager (getSystemService("telephony_registry")) registers telephony
        // callbacks and listeners with ITelephonyRegistry; a no-op one takes them and never calls.
        svc.put("telephony.registry", "com.android.internal.telephony.ITelephonyRegistry");
        // LocationManager finds its service by name for what it does not do through the
        // manager object: isLocationEnabled() (its cache) and the GNSS callbacks. Unpublished,
        // those throw "No service published for: location". It is the service the manager has.
        svc.put("location", "android.location.ILocationManager");
        // WebView provider init reads IWebViewUpdateService.isMultiProcessEnabled() etc.; a no-op proxy
        // (multiprocess=false, no updates) is enough — acl installs the provider directly (see
        // installWebViewProvider), so the update-service is only queried for these secondary flags.
        svc.put("webviewupdate", "android.webkit.IWebViewUpdateService");
        // WallpaperManager.Globals resolves ServiceManager.getService("wallpaper") → IWallpaperManager
        // (getWallpaperColors for dynamic theming); a no-op proxy (colors → null, a valid "no colors"
        // response) is enough. No wallpaper host — set/clear are dropped. Single desktop surface.
        svc.put("wallpaper", "android.app.IWallpaperManager");
        // UiModeManager(Context) resolves ServiceManager.getServiceOrThrow("uimode") → IUiModeManager.
        // getCurrentModeType → UI_MODE_TYPE_NORMAL (desktop host, not car/desk/tv); night mode follows
        // the Configuration.uiMode acl already sets (setColorScheme). React Native / AppCompat read it.
        svc.put("uimode", "android.app.IUiModeManager");
        // PowerManager routes isInteractive()/isPowerSaveMode()/wake locks through IPowerManager.
        // makePowerHandler answers the display-state queries truthfully for a desktop host (screen
        // on, no battery saver); wake-lock acquire/release no-op.
        svc.put("power", "android.os.IPowerManager");
        // MediaSession routes through ISessionManager; makeSessionManagerHandler returns a no-op
        // ISession (with a no-op ISessionController) so MediaSession construction completes.
        svc.put("media_session", "android.media.session.ISessionManager");
        // android.media.MediaRouter asks for IMediaRouterService when it is made; a no-op one
        // (no remote routes, no client state kept) is enough for the local route.
        svc.put("media_router", "android.media.IMediaRouterService");
        // An Activity makes its SearchManager when a menu holds a SearchView; a no-op service
        // answers that no Activity is searchable.
        svc.put("search", "android.app.ISearchManager");
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
                                : "mount".equals(name) ? makeStorageHandler(iface)
                                : "uimode".equals(name) ? makeUiModeHandler(iface)
                                : "power".equals(name) ? makePowerHandler(iface)
                                : "media_session".equals(name) ? makeSessionManagerHandler(iface)
                                : "activity_task".equals(name) ? makeActivityTaskHandler(iface)
                                : "activity".equals(name) ? makeActivityManagerHandler(iface)
                                : "window".equals(name) ? makeWindowManagerHandler(iface)
                                : "location".equals(name) ? AclLocationManager.newService()
                                : noop(iface);
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

    // Does the view tree contain a SurfaceView the app renders GL into? Such an app is given
    // the GL present, where the whole window is the GL surface. Any other SurfaceView is a
    // video surface: the app stays on the View-tree present and the SurfaceView gets a decoder
    // Surface when it is laid out (maybeInjectVideoSurface).
    public static boolean hasGlSurfaceView(android.view.View v) {
        android.view.SurfaceView sv = findSurfaceView(v);
        if (sv == null) return false;
        boolean gl = isGlSurfaceView(sv);
        android.util.Log.i("acl", "SurfaceView at launch: " + sv.getClass().getName()
                + (gl ? " -> GL present" : " -> video surface on the View-tree present"));
        return gl;
    }

    // Nothing at launch says what an app will do with a SurfaceView, so the class decides.
    // Every engine that renders GL brings its own subclass: GLSurfaceView and what extends it
    // (libGDX, cocos), Flutter's FlutterSurfaceView, SDL's SDLSurface. A player lays out the
    // platform class itself, or VideoView. So android.view.SurfaceView exactly, and VideoView,
    // are video; any other subclass is GL. An app that renders GL into a plain SurfaceView
    // from native code is the case this gets wrong; ACL_SURFACEVIEW=gl (or =video) overrides.
    static boolean isGlSurfaceView(android.view.SurfaceView sv) {
        String mode = System.getenv("ACL_SURFACEVIEW");
        if ("gl".equals(mode)) return true;
        if ("video".equals(mode)) return false;
        if (sv instanceof android.widget.VideoView) return false;
        return sv.getClass() != android.view.SurfaceView.class;
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

    // The compositor resized a GL app's window (nDispatch -> gl_session_resize). The Surface is
    // the same one at a new size, so the holder's callbacks get surfaceChanged and not a
    // destroy and create: a GLSurfaceView hands it to its renderer as onSurfaceChanged, and
    // Flutter's FlutterSurfaceView to its engine. Main thread.
    public static void resizeGlSurface(android.view.View decor, int w, int h) {
        try {
            android.view.SurfaceView sv = findSurfaceView(decor);
            if (sv == null) return;
            Class<?> c = android.view.SurfaceView.class;
            setIntField(c, sv, "mSurfaceWidth", w);
            setIntField(c, sv, "mSurfaceHeight", h);
            int fmt = getIntField(c, sv, "mFormat", android.graphics.PixelFormat.RGBX_8888);
            android.view.SurfaceHolder holder = sv.getHolder();
            for (android.view.SurfaceHolder.Callback cb : snapshotCallbacks(c, sv)) {
                if (cb != null) cb.surfaceChanged(holder, fmt, w, h);
            }
            android.util.Log.i("acl", "resizeGlSurface: surfaceChanged (" + w + "x" + h + ")");
        } catch (Throwable t) {
            android.util.Log.w("acl", "resizeGlSurface failed: " + t);
        }
    }

    // The compositor resized a View-tree app's window (nDispatch -> view_session_resize). On
    // Android a window that changes size is a configuration change: the resources take the new
    // screen size, the application and each activity get onConfigurationChanged, and the view
    // tree is told and laid out again. a4l's window is its display (DISPLAY_W/H), so that moves
    // too. Every activity is treated as if it declared screenSize|smallestScreenSize|
    // screenLayout|orientation in configChanges: none is destroyed and created again, which a4l
    // cannot do. An app that needs a relaunch to pick up, say, a layout-sw600dp resource keeps
    // the layout it inflated. Main thread.
    public static void resizeWindow(android.view.View decor, int w, int h) {
        if (w <= 0 || h <= 0) return;
        try {
            DISPLAY_W = w;
            DISPLAY_H = h;
            final android.content.res.Configuration cfg = appConfiguration();
            final android.util.DisplayMetrics dm = appMetrics();
            // Every Resources an app can reach, once each. They may share one implementation.
            java.util.Set<Resources> seen =
                    java.util.Collections.newSetFromMap(new java.util.IdentityHashMap<Resources, Boolean>());
            if (sAppContext != null) seen.add(sAppContext.getResources());
            if (sApplication != null) seen.add(sApplication.getResources());
            for (ActivityRecord r : sTask) seen.add(r.activity.getResources());
            seen.add(Resources.getSystem());
            seen.remove(null);
            for (Resources r : seen) applyDisplaySize(r, cfg, dm);
            // The frames a hollow ViewRootImpl answers getWindowVisibleDisplayFrame from.
            java.util.List<android.view.View> decors = new java.util.ArrayList<android.view.View>();
            if (decor != null) decors.add(decor);
            for (ActivityRecord r : sTask) {
                android.view.Window win = r.activity.getWindow();
                android.view.View d = win != null ? win.peekDecorView() : null;
                if (d != null && !decors.contains(d)) decors.add(d);
            }
            for (android.view.View d : decors) resizeViewRootFrames(d, w, h);
            if (sApplication != null) sApplication.onConfigurationChanged(cfg);
            // Bottom of the task first, as each would have seen it had it been on top.
            java.util.Iterator<ActivityRecord> it = sTask.descendingIterator();
            while (it.hasNext()) {
                android.app.Activity a = it.next().activity;
                try {
                    android.content.res.Configuration ac = a.getResources().getConfiguration();
                    try { setField(android.app.Activity.class, a, "mCurrentConfig",
                            new android.content.res.Configuration(ac)); } catch (Throwable ignored) {}
                    a.onConfigurationChanged(new android.content.res.Configuration(ac));
                } catch (Throwable t) {
                    android.util.Log.w("acl", "resizeWindow: " + a.getClass().getName()
                            + ".onConfigurationChanged threw: " + t);
                }
            }
            for (android.view.View d : decors) {
                d.dispatchConfigurationChanged(d.getResources().getConfiguration());
                d.requestLayout();
            }
            android.util.Log.i("acl", "resizeWindow: " + w + "x" + h + " (" + cfg.screenWidthDp + "x"
                    + cfg.screenHeightDp + " dp), " + sTask.size() + " activity(ies) told");
        } catch (Throwable t) {
            android.util.Log.w("acl", "resizeWindow failed: " + t);
        }
    }

    private static void resizeViewRootFrames(android.view.View decor, int w, int h) {
        try {
            Object vri = android.view.View.class.getMethod("getViewRootImpl").invoke(decor);
            if (vri == null) return;
            Class<?> vriCls = Class.forName("android.view.ViewRootImpl");
            Object frames = getField(vriCls, vri, "mTmpFrames");
            if (frames != null) {
                for (String fld : new String[]{"frame", "displayFrame", "parentFrame"}) {
                    try {
                        Object r = frames.getClass().getField(fld).get(frames);
                        if (r instanceof android.graphics.Rect) ((android.graphics.Rect) r).set(0, 0, w, h);
                    } catch (Throwable ignored) {}
                }
            }
            Object win = getField(vriCls, vri, "mWinFrame");
            if (win instanceof android.graphics.Rect) ((android.graphics.Rect) win).set(0, 0, w, h);
        } catch (Throwable ignored) {}
    }

    // A video SurfaceView we have already backed with a real Surface (so we fire surfaceCreated once).
    private static java.lang.ref.WeakReference<android.view.SurfaceView> sVideoSv;
    // The size the app was last told its video surface has.
    private static int sVideoW, sVideoH;

    /**
     * An Activity transition: forgets the video SurfaceView and releases the GL plane of an
     * Activity that is no longer on screen, so the next screen is given its own. Returns
     * true when one is still in use, by an Activity that is on screen under a translucent
     * one or that the translucent one has just left.
     */
    public static boolean dropHiddenPlanes() {
        final android.view.SurfaceView video = sVideoSv != null ? sVideoSv.get() : null;
        final boolean keepVideo = video != null && onScreen(video);
        if (!keepVideo) sVideoSv = null;
        final android.view.SurfaceView gl = sGlPlaneSv != null ? sGlPlaneSv.get() : null;
        final boolean keepGl = gl != null && onScreen(gl);
        if (!keepGl) releaseGlPlane();
        return keepVideo || keepGl;
    }

    // A SurfaceView that an engine draws GL into, in an app that is on the View-tree present:
    // a map under the app's own views (Waze), a game board beside its menus. The whole window
    // is not the GL surface, as it is for an app whose first Activity is one (runGl), and the
    // view is not a video. Its EGL window is an offscreen one (PresentDriver.setGlPlane, and
    // eglCreateWindowSurface's hook in the runtime) whose frames are shown on the plane under
    // the view tree, through a hole punched where the view is.
    private static java.lang.ref.WeakReference<android.view.SurfaceView> sGlPlaneSv;
    private static int sGlPlaneW, sGlPlaneH;

    // The Activity that holds the view is no longer the one shown: its surface goes, as it
    // does on Android when an Activity stops, and comes back with surfaceCreated when the
    // view is found again (maybeInjectGlPlane). Main thread.
    private static void releaseGlPlane() {
        final android.view.SurfaceView sv = sGlPlaneSv != null ? sGlPlaneSv.get() : null;
        sGlPlaneSv = null;
        if (sv == null) return;
        try {
            Class<?> c = android.view.SurfaceView.class;
            android.view.SurfaceHolder holder = sv.getHolder();
            for (android.view.SurfaceHolder.Callback cb : snapshotCallbacks(c, sv)) {
                if (cb != null) cb.surfaceDestroyed(holder);
            }
            setField(c, sv, "mSurfaceCreated", Boolean.FALSE);
        } catch (Throwable t) {
            android.util.Log.w("acl", "releaseGlPlane failed: " + t);
        }
        PresentDriver.setGlPlane(0, 0, 0, 0);
    }

    private static void maybeInjectGlPlane(android.view.SurfaceView sv, int[] loc, int w, int h)
            throws Exception {
        Class<?> c = android.view.SurfaceView.class;
        if (sGlPlaneSv != null && sGlPlaneSv.get() == sv) {  // backed: keep the plane where it is
            PresentDriver.setGlPlane(loc[0], loc[1], w, h);
            if (w != sGlPlaneW || h != sGlPlaneH) {
                sGlPlaneW = w;
                sGlPlaneH = h;
                setIntField(c, sv, "mSurfaceWidth", w);
                setIntField(c, sv, "mSurfaceHeight", h);
                int fmt = getIntField(c, sv, "mFormat", android.graphics.PixelFormat.RGBX_8888);
                android.view.SurfaceHolder holder = sv.getHolder();
                for (android.view.SurfaceHolder.Callback cb : snapshotCallbacks(c, sv)) {
                    if (cb != null) cb.surfaceChanged(holder, fmt, w, h);
                }
            }
            return;
        }
        releaseGlPlane();  // another view's, in an Activity that is still on top
        PresentDriver.setGlPlane(loc[0], loc[1], w, h);
        int fmt = getIntField(c, sv, "mRequestedFormat", android.graphics.PixelFormat.RGBX_8888);
        // mHaveFrame stays false, as for a video view (maybeInjectVideoSurface).
        setField(c, sv, "mVisible", Boolean.TRUE);
        setField(c, sv, "mRequestedVisible", Boolean.TRUE);
        setField(c, sv, "mSurfaceCreated", Boolean.TRUE);
        setIntField(c, sv, "mSurfaceWidth", w);
        setIntField(c, sv, "mSurfaceHeight", h);
        setIntField(c, sv, "mFormat", fmt);
        sGlPlaneSv = new java.lang.ref.WeakReference<>(sv);
        sGlPlaneW = w;
        sGlPlaneH = h;
        android.view.SurfaceHolder holder = sv.getHolder();
        for (android.view.SurfaceHolder.Callback cb : snapshotCallbacks(c, sv)) {
            if (cb == null) continue;
            cb.surfaceCreated(holder);
            cb.surfaceChanged(holder, fmt, w, h);
        }
        final int punched = punchPlaneHole(sv);
        android.util.Log.i("acl", "maybeInjectGlPlane: " + sv.getClass().getName()
                + " surfaceCreated (" + w + "x" + h + " @" + loc[0] + "," + loc[1]
                + ") punchHole nodes=" + punched);
    }

    // A TextureView makes its SurfaceTexture, and tells its listener the surface is available,
    // the first time it is drawn (TextureView.draw -> getTextureLayer). On Android that draw
    // comes from the display-list pass, which visits every visible child of a ViewGroup whether
    // or not the group's own drawChild draws it (ViewGroup.dispatchGetDisplayList). a4l draws a
    // tree with View.draw, which only reaches the children drawChild lets through. A group
    // that keeps its TextureView out of drawChild until the first frame is up never got a
    // surface, so the first frame never came.
    //
    // Called for each window before it is drawn: a visible, attached TextureView that has no
    // surface yet has its display list updated, as that pass would.
    static void prepareTextureViews(android.view.View root) {
        if (!(root instanceof android.view.ViewGroup)) {
            if (root instanceof android.view.TextureView) prepareTextureView((android.view.TextureView) root);
            return;
        }
        final android.view.ViewGroup group = (android.view.ViewGroup) root;
        for (int i = 0, n = group.getChildCount(); i < n; i++) {
            final android.view.View child = group.getChildAt(i);
            if (child == null || child.getVisibility() != android.view.View.VISIBLE) continue;
            if (child instanceof android.view.TextureView) {
                prepareTextureView((android.view.TextureView) child);
            } else if (child instanceof android.view.ViewGroup) {
                prepareTextureViews(child);
            }
        }
    }

    private static java.lang.reflect.Method sUpdateDisplayList;

    private static void prepareTextureView(android.view.TextureView tv) {
        if (tv.isAvailable() || !tv.isAttachedToWindow() || tv.getWidth() <= 0 || tv.getHeight() <= 0) {
            return;
        }
        try {
            if (sUpdateDisplayList == null) {
                sUpdateDisplayList = android.view.View.class.getMethod("updateDisplayListIfDirty");
            }
            tv.invalidate();  // dirty, so that the update records (and so draws) it
            sUpdateDisplayList.invoke(tv);
        } catch (Throwable t) {
            System.err.println("acl HostBootstrap: TextureView not prepared: " + t);
        }
    }

    // Back a video SurfaceView (media3 PlayerView's content SurfaceView) with a REAL,
    // MediaCodec-decodable Surface in the Skia/Compose present path, and fire surfaceCreated so the
    // app calls player.setVideoSurface(it). Without this, ExoPlayer keeps rendering to a 64x64
    // PlaceholderSurface and SKIPS every frame (render=false) → the video never plays and the decoder
    // stalls. Unlike injectGlSurface (GL path, EGL-window holder, mNativeObject stays 0), here we set
    // the SurfaceView's Surface.mNativeObject to an acl ReaderCtx whose queue the codec decodes into,
    // and route those frames to the wl_subsurface HW plane. Called each present frame (view appears
    // lazily via AndroidView after navigation); injects once, then just refreshes the plane bounds.
    // Returns whether the window has a SurfaceView that the plane now backs.
    public static boolean maybeInjectVideoSurface(android.view.View decor) {
        final android.view.SurfaceView sv = findSurfaceView(decor);
        if (sv == null) return false;
        injectVideoSurface(sv);
        return (sVideoSv != null && sVideoSv.get() == sv)
                || (sGlPlaneSv != null && sGlPlaneSv.get() == sv);
    }

    private static void injectVideoSurface(android.view.SurfaceView sv) {
        try {
            final int w = sv.getWidth(), h = sv.getHeight();
            if (w <= 0 || h <= 0) return;  // not laid out yet — wait for real bounds
            final int[] loc = new int[2];
            sv.getLocationInWindow(loc);
            if (isGlSurfaceView(sv)) {  // an engine's view, not a player's
                maybeInjectGlPlane(sv, loc, w, h);
                return;
            }
            if (sVideoSv != null && sVideoSv.get() == sv) {  // already backed — keep the plane aligned
                PresentDriver.setVideoBounds(loc[0], loc[1], w, h);
                if (w != sVideoW || h != sVideoH) {
                    // The app laid its video View out again at another size: a media3 PlayerView
                    // does once it knows the picture's aspect ratio. The surface is the same one;
                    // the app is told its new size, as SurfaceView.updateSurface would.
                    sVideoW = w;
                    sVideoH = h;
                    Class<?> c = android.view.SurfaceView.class;
                    setIntField(c, sv, "mSurfaceWidth", w);
                    setIntField(c, sv, "mSurfaceHeight", h);
                    int fmt = getIntField(c, sv, "mFormat", android.graphics.PixelFormat.RGBX_8888);
                    android.view.SurfaceHolder holder = sv.getHolder();
                    for (android.view.SurfaceHolder.Callback cb : snapshotCallbacks(c, sv)) {
                        if (cb != null) cb.surfaceChanged(holder, fmt, w, h);
                    }
                }
                return;
            }
            if (System.getenv("ACL_MC_DIAG") != null) {  // one-shot: what region does the app lay the video into?
                StringBuilder chain = new StringBuilder();
                for (Object v = sv; v instanceof android.view.View; v = ((android.view.View) v).getParent()) {
                    android.view.View vv = (android.view.View) v;
                    int[] l = new int[2];
                    try { vv.getLocationInWindow(l); } catch (Throwable t) {}
                    chain.append("\n  ").append(vv.getClass().getName())
                         .append(" ").append(vv.getWidth()).append("x").append(vv.getHeight())
                         .append(" @").append(l[0]).append(",").append(l[1]);
                }
                android.util.Log.i("acl.mc", "video view chain:" + chain);
            }
            final long handle = PresentDriver.makeVideoSurface(w, h);
            if (handle == 0L) return;
            Class<?> c = android.view.SurfaceView.class;
            android.view.Surface surf = (android.view.Surface) getField(c, sv, "mSurface");
            if (surf == null) return;
            setField(android.view.Surface.class, surf, "mNativeObject", Long.valueOf(handle));
            int fmt = getIntField(c, sv, "mRequestedFormat", android.graphics.PixelFormat.RGBX_8888);
            // mHaveFrame stays false. With it set, every later layout of the view runs
            // SurfaceView.updateSurface, which finds no window surface under a4l (there is no
            // ViewRootImpl surface), tells the app its surface is destroyed and throws on a
            // native a4l does not have. The player then has nowhere to show its frames. Resizes
            // are handled above instead.
            setField(c, sv, "mVisible", Boolean.TRUE);
            setField(c, sv, "mRequestedVisible", Boolean.TRUE);
            setField(c, sv, "mSurfaceCreated", Boolean.TRUE);
            setIntField(c, sv, "mSurfaceWidth", w);
            setIntField(c, sv, "mSurfaceHeight", h);
            setIntField(c, sv, "mFormat", fmt);
            PresentDriver.setVideoBounds(loc[0], loc[1], w, h);
            android.view.SurfaceHolder holder = sv.getHolder();
            for (android.view.SurfaceHolder.Callback cb : snapshotCallbacks(c, sv)) {
                if (cb == null) continue;
                cb.surfaceCreated(holder);
                cb.surfaceChanged(holder, fmt, w, h);
            }
            final int punched = punchPlaneHole(sv);
            sVideoSv = new java.lang.ref.WeakReference<>(sv);
            sVideoW = w;
            sVideoH = h;
            android.util.Log.i("acl", "maybeInjectVideoSurface: video SurfaceView surfaceCreated ("
                    + w + "x" + h + " @" + loc[0] + "," + loc[1] + ") punchHole nodes=" + punched);
        } catch (Throwable t) {
            android.util.Log.w("acl", "maybeInjectVideoSurface failed: " + t);
        }
    }

    // Punch a transparent hole where a SurfaceView is, so the plane under the ARGB toplevel
    // (a video's, or a GL view's) shows through while the app's views composite on top.
    // Returns how many render nodes were marked.
    private static int punchPlaneHole(android.view.SurfaceView sv) throws Exception {
        Class<?> c = android.view.SurfaceView.class;
        // Compose does NOT draw the SurfaceView's own RenderNode into the walked tree (it draws
        // the hosting Android view as a unit), so register the punch on the SurfaceView AND each
        // Android-view ancestor up to the Compose boundary — whichever node the walk actually
        // draws gets cleared. For a player these are the PlayerView subtree (video only, no UI),
        // so clearing their rect is safe.
        int punched = 0;
        for (Object v = sv; v instanceof android.view.View; v = ((android.view.View) v).getParent()) {
            String cn = v.getClass().getName();
            if (cn.contains("androidx.compose.ui.platform")) break;  // reached the Compose host
            long rn = renderNodeHandle((android.view.View) v);
            if (rn != 0L) { PresentDriver.setVideoPunchHole(rn, true); punched++; }
        }
        // And let the SurfaceView punch its own hole, as it does on Android once its surface
        // has drawn (mDrawFinished): SurfaceView.draw/dispatchDraw then calls
        // Canvas.punchHole() over its bounds. That hole lands after the ancestors' backgrounds,
        // which the node punches above do not survive: an ancestor's background is drawn after
        // its node is cleared. Without this a plane under an opaque background stays hidden.
        setField(c, sv, "mDrawFinished", Boolean.TRUE);
        sv.invalidate();
        // The parent recorded this child while it had nothing to draw, and a4l records such
        // a child inline instead of by reference. Record the parent again so that it refers
        // to the SurfaceView's node, which now holds the hole.
        if (sv.getParent() instanceof android.view.View)
            ((android.view.View) sv.getParent()).invalidate();
        return punched;
    }

    // ACL_MC_DIAG: poll the media3 Player's state/position so we can see WHY video frames render vs
    // drop (BUFFERING vs READY, playWhenReady, suppression, and whether the position clock advances).
    // Reflection-only (media3 is app-bundled, not on the framework classpath). No-op without the flag.
    private static boolean sPlayerDiag = System.getenv("ACL_MC_DIAG") != null;
    private static int sPlayerDiagN = 0;
    public static void maybeLogVideoPlayer(android.view.View decor) {
        if (!sPlayerDiag) return;
        if ((sPlayerDiagN++ % 60) != 1) return;  // ~once/sec at 60fps
        try {
            android.view.SurfaceView sv = findSurfaceView(decor);
            if (sv == null) return;
            // Walk up to the media3 PlayerView, then getPlayer().
            Object playerView = null;
            for (android.view.ViewParent p = sv.getParent(); p != null; p = p.getParent()) {
                if (p.getClass().getName().contains("media3.ui.PlayerView")) { playerView = p; break; }
            }
            if (playerView == null) return;
            Object player = playerView.getClass().getMethod("getPlayer").invoke(playerView);
            if (player == null) { android.util.Log.i("acl.mc", "player=null on PlayerView"); return; }
            Class<?> pc = player.getClass();
            Object st  = tryCall(pc, player, "getPlaybackState");
            Object pwr = tryCall(pc, player, "getPlayWhenReady");
            Object sup = tryCall(pc, player, "getPlaybackSuppressionReason");
            Object pos = tryCall(pc, player, "getCurrentPosition");
            Object dur = tryCall(pc, player, "getDuration");
            Object playing = tryCall(pc, player, "isPlaying");
            Object err = tryCall(pc, player, "getPlayerError");
            android.util.Log.i("acl.mc", "player state=" + st + " (1=IDLE 2=BUF 3=READY 4=END)"
                    + " playWhenReady=" + pwr + " suppress=" + sup + " isPlaying=" + playing
                    + " pos=" + pos + "/" + dur + "ms err=" + err);
        } catch (Throwable t) {
            android.util.Log.i("acl.mc", "maybeLogVideoPlayer: " + t);
        }
    }
    private static Object tryCall(Class<?> c, Object o, String m) {
        try { java.lang.reflect.Method mm = c.getMethod(m); mm.setAccessible(true); return mm.invoke(o); }
        catch (Throwable t) { return "?"; }
    }

    // The native AclRenderNode* behind a View's RenderNode: View.mRenderNode -> RenderNode.mNativeRenderNode.
    // Used to mark the video SurfaceView's node as a hole-punch. 0 if the fields aren't found.
    private static long renderNodeHandle(android.view.View v) {
        try {
            java.lang.reflect.Field rnf = android.view.View.class.getDeclaredField("mRenderNode");
            rnf.setAccessible(true);
            Object rn = rnf.get(v);
            if (rn == null) return 0L;
            java.lang.reflect.Field nf = rn.getClass().getDeclaredField("mNativeRenderNode");
            nf.setAccessible(true);
            return nf.getLong(rn);
        } catch (Throwable t) {
            android.util.Log.w("acl", "renderNodeHandle: " + t);
            return 0L;
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
