// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/os/Bundle):
// public API surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.os.Bundle — enough to pass a saved
// instance-state object through the Activity lifecycle (Gate G2.d). The real
// Bundle's Parcelable machinery is out of scope until it is needed; methods are
// added as the app corpus requires them. Docs-derived shim, NOT vendored AOSP —
// real frameworks/base vendoring is gated on the AOSP mirror pin (G2.c) and the
// closure being cut-or-shimmed at the SystemServer boundary (see
// docs/framework-bootstrap.md "Measured").
package android.os;

import java.util.HashMap;
import java.util.Map;

public final class Bundle {
  private final Map<String, Object> map = new HashMap<>();

  public Bundle() {}

  public boolean isEmpty() { return map.isEmpty(); }
  public int size() { return map.size(); }
  public void clear() { map.clear(); }
  public boolean containsKey(String key) { return map.containsKey(key); }

  public void putString(String key, String value) { map.put(key, value); }
  public String getString(String key) {
    Object v = map.get(key);
    return v instanceof String ? (String) v : null;
  }
  public void putInt(String key, int value) { map.put(key, value); }
  public int getInt(String key) {
    Object v = map.get(key);
    return v instanceof Integer ? (Integer) v : 0;
  }
  public void putBoolean(String key, boolean value) { map.put(key, value); }
  public boolean getBoolean(String key) {
    Object v = map.get(key);
    return v instanceof Boolean && (Boolean) v;
  }
}
