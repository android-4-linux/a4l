// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/content/
// SharedPreferences + its Editor): public API surface. The native underside is
// acl's own bridge to the svc/ PreferencesService (no Binder). No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.content.SharedPreferences + Editor
// (Gate G2.e, svc hub). Returned by Context.getSharedPreferences(name, mode); its
// natives call the in-process acl::svc::PreferencesService. Real SharedPreferences
// is an interface (backed by a hidden impl over per-package XML files); this is a
// concrete shim over the in-process store. Docs-derived shim, NOT vendored AOSP.
package android.content;

public class SharedPreferences {
  private final String name;

  public SharedPreferences(String name) { this.name = name; }

  public String getString(String key, String defValue) {
    String v = nativeGetString(name, key);
    return v != null ? v : defValue;
  }
  public int getInt(String key, int defValue) { return nativeGetInt(name, key, defValue); }
  public boolean contains(String key) { return nativeGetString(name, key) != null; }
  public Editor edit() { return new Editor(name); }

  private static native String nativeGetString(String name, String key);
  private static native int nativeGetInt(String name, String key, int defValue);

  public static class Editor {
    private final String name;
    Editor(String name) { this.name = name; }

    public Editor putString(String key, String value) {
      nativePutString(name, key, value);
      return this;
    }
    public Editor putInt(String key, int value) {
      nativePutInt(name, key, value);
      return this;
    }
    public void apply() {}          // the store commits eagerly here
    public boolean commit() { return true; }

    private static native void nativePutString(String name, String key, String value);
    private static native void nativePutInt(String name, String key, int value);
  }
}
