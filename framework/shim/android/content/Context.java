// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/content/
// Context): the commonly-used public surface + service-name constants. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.content.Context — the root of the
// Service/Activity hierarchy that real app entry points extend, and the type their
// constructors (MediaSession(Context,...), AudioTrack via Builder, ...) require.
// Real Context is abstract with ~100 methods bound to SystemServer (the 535-class
// closure fw_extract measured); this carries only what the app corpus calls,
// returning sane defaults. getSystemService() returns null until the aclcxx svc/
// hub is wired to the Manager boundary (ADR-2/ADR-3). Docs-derived shim, NOT
// vendored AOSP.
package android.content;

public class Context {
  public static final String AUDIO_SERVICE = "audio";
  public static final String MEDIA_SESSION_SERVICE = "media_session";
  public static final String ACTIVITY_SERVICE = "activity";
  public static final String WINDOW_SERVICE = "window";
  public static final String LAYOUT_INFLATER_SERVICE = "layout_inflater";
  public static final String NOTIFICATION_SERVICE = "notification";

  private String packageName = "com.aclcxx.app";

  public Context() {}

  public String getPackageName() { return packageName; }
  public Context getApplicationContext() { return this; }

  public static final int MODE_PRIVATE = 0;

  // The Manager boundary (ADR-2/ADR-3): return the Java Manager shim, whose natives
  // call the in-process aclcxx svc/ hub. More Managers land as apps need them.
  public Object getSystemService(String name) {
    if (AUDIO_SERVICE.equals(name)) return new android.media.AudioManager();
    if (ACTIVITY_SERVICE.equals(name)) return new android.app.ActivityManager();
    if (WINDOW_SERVICE.equals(name)) return new android.view.WindowManager();
    if (MEDIA_SESSION_SERVICE.equals(name)) return new android.media.session.MediaSessionManager();
    return null;
  }

  public SharedPreferences getSharedPreferences(String name, int mode) {
    return new SharedPreferences(name);
  }
  public String getSystemServiceName(Class<?> serviceClass) { return null; }
  public ClassLoader getClassLoader() { return getClass().getClassLoader(); }

  // aclcxx: the host sets the real manifest package after instantiating the entry
  // point (a stand-in for attachBaseContext of a ContextImpl carrying it).
  public void aclcxxSetPackage(String pkg) { if (pkg != null) packageName = pkg; }
}
