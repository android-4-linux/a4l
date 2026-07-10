// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/content/
// ContextWrapper): public API surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.content.ContextWrapper — the
// delegating Context that Service (and, via ContextThemeWrapper, Activity) extend.
// When a base Context is attached it delegates; otherwise it uses Context's own
// defaults, so a framework-constructed subclass works before attachBaseContext.
// Docs-derived shim, NOT vendored AOSP.
package android.content;

public class ContextWrapper extends Context {
  private Context base;

  public ContextWrapper(Context base) { this.base = base; }
  protected ContextWrapper() {}  // subclasses attach the base later (attachBaseContext)

  protected void attachBaseContext(Context newBase) { this.base = newBase; }
  public Context getBaseContext() { return base; }

  @Override public String getPackageName() {
    return base != null ? base.getPackageName() : super.getPackageName();
  }
  @Override public Context getApplicationContext() {
    return base != null ? base.getApplicationContext() : this;
  }
  @Override public Object getSystemService(String name) {
    return base != null ? base.getSystemService(name) : super.getSystemService(name);
  }
}
