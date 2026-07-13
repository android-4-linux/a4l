// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK android.view.View / android.graphics
// (public API). No GPL-3.0 sources.
//
// Drive the real View traversal on host ART: construct a DrawView, measure/layout it, draw
// it into a real Canvas over a Bitmap, and report a center pixel. The smallest "app path"
// step — a real android.view.View.onDraw through the real android.graphics natives.
//
// EXPLORATION PROBE (not a passing test yet): it maps the framework-native frontier. It
// clears Context/Resources, the no-Binder ServiceManager (via reflection here; acl's runtime
// provides the BinderInternal stub), and the ViewConfiguration→IWindowManager wall (an IBinder
// interface proxy). It currently STOPS at RenderNode.nCreate — the hwui wall (ADR-4). The next
// milestone is an acl RenderNode shim; see docs/app-path-scope.md. Run: /tmp/.../run_view.sh.
package com.acl.hello;

import android.content.ContextWrapper;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.view.View;

public class Main {
    // A minimal Context: a ContextWrapper with no base, returning the system Resources.
    // Enough to construct a View (which needs getResources().getDisplayMetrics()); other
    // Context calls would delegate to the null base and fail loudly if the View needs them.
    static final class ViewContext extends ContextWrapper {
        ViewContext() { super(null); }
        @Override public Resources getResources() { return Resources.getSystem(); }
        // getDisplayId() is @hide in the public SDK, so no @Override; still overrides at
        // runtime by signature (DEFAULT_DISPLAY = 0).
        public int getDisplayId() { return 0; }
        @Override public Object getSystemService(String name) { return null; }
        @Override public Resources.Theme getTheme() { return null; }
        @Override public android.os.Looper getMainLooper() {
            return android.os.Looper.getMainLooper();
        }
        @Override public android.content.pm.ApplicationInfo getApplicationInfo() {
            return new android.content.pm.ApplicationInfo();
        }
    }

    // A no-op InvocationHandler returning type-appropriate defaults (false/0/null).
    static Object noop(Class<?> iface) {
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

    // acl has no Binder: seed a no-op ServiceManager. getService(name) returns a local
    // IBinder whose queryLocalInterface hands back a no-op service interface, so callers
    // (ViewConfiguration → IWindowManager, getSystemService, …) get a working stub that
    // returns defaults instead of transacting over Binder. Known service→interface below;
    // unknown services return null. (In real acl boot this is provided by the runtime.)
    static void seedNoBinderServiceManager() throws Exception {
        final java.util.Map<String, String> svc = new java.util.HashMap<>();
        svc.put("window", "android.view.IWindowManager");
        Class<?> ism = Class.forName("android.os.IServiceManager");
        Object proxy = java.lang.reflect.Proxy.newProxyInstance(
                ism.getClassLoader(), new Class<?>[]{ism},
                (p, method, a) -> {
                    if ("getService".equals(method.getName()) || "checkService".equals(method.getName())) {
                        String name = (String) a[0];
                        String ifaceName = svc.get(name);
                        if (ifaceName == null) return null;
                        Class<?> iface = Class.forName(ifaceName);
                        final Object localIface = noop(iface);
                        final String descriptor = ifaceName;  // AIDL DESCRIPTOR == interface FQN
                        // Proxy IBinder (an interface) rather than new Binder() — the latter
                        // needs Binder natives (getNativeBBinderHolder). asInterface() calls
                        // queryLocalInterface(DESCRIPTOR) and uses the local stub directly.
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

    // Framework setup an unmodified app would get from the runtime/ActivityThread: a
    // no-Binder ServiceManager, a main Looper. Idempotent.
    static void bootstrap() throws Exception {
        seedNoBinderServiceManager();
        if (android.os.Looper.myLooper() == null) android.os.Looper.prepareMainLooper();
    }

    // acl-driven GPU path: acl calls this factory, then drives the returned View through
    // present_content_loop_gpu (begin_frame → View.draw on the Ganesh Vulkan Canvas →
    // end_frame → tiled dma-buf → Wayland). Mirrors an app's setContentView; acl presents.
    public static View aclCreateContentView(int w, int h) throws Exception {
        bootstrap();
        DrawView v = new DrawView(new ViewContext());
        v.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY),
                  View.MeasureSpec.makeMeasureSpec(h, View.MeasureSpec.EXACTLY));
        v.layout(0, 0, w, h);
        return v;
    }

    public static void main(String[] args) throws Exception {
        // Software readback path — a pixel ORACLE for the View traversal, not the product
        // path. The product path is acl-driven GPU (aclCreateContentView + present loop).
        final int W = 240, H = 160;
        View v = aclCreateContentView(W, H);
        Bitmap bmp = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bmp);
        v.draw(canvas);
        int center = bmp.getPixel(W / 2, H / 2);
        int corner = bmp.getPixel(2, 2);
        System.out.println(String.format(
                "VIEW PROBE: %dx%d center=0x%08x corner=0x%08x", W, H, center, corner));
        System.out.println("VIEW PROBE: real android.view.View.onDraw ran on host ART");
    }
}
