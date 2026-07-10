// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/util/Log):
// public API surface + priority constants. The native underside (write0) is
// aclcxx's own bridge (RegisterNatives -> aclcxx::log). No GPL-3.0 sources.
//
// A source-compatible shim for android.util.Log — the first android.* class on the
// framework boot classpath with an aclcxx-implemented native method (Gate G2.b).
// It is a docs-derived shim, NOT the vendored AOSP file; true vendoring of real
// frameworks/base classes lands with the AOSP mirror pin (G2.c). Apps call the
// documented static methods; those funnel to println(), whose native write0()
// forwards to the aclcxx native logging core.
package android.util;

public final class Log {
  public static final int VERBOSE = 2;
  public static final int DEBUG = 3;
  public static final int INFO = 4;
  public static final int WARN = 5;
  public static final int ERROR = 6;
  public static final int ASSERT = 7;

  private Log() {}

  public static int v(String tag, String msg) { return println(VERBOSE, tag, msg); }
  public static int d(String tag, String msg) { return println(DEBUG, tag, msg); }
  public static int i(String tag, String msg) { return println(INFO, tag, msg); }
  public static int w(String tag, String msg) { return println(WARN, tag, msg); }
  public static int e(String tag, String msg) { return println(ERROR, tag, msg); }
  public static int wtf(String tag, String msg) { return println(ASSERT, tag, msg); }

  public static int v(String tag, String msg, Throwable tr) { return println(VERBOSE, tag, msg + '\n' + getStackTraceString(tr)); }
  public static int d(String tag, String msg, Throwable tr) { return println(DEBUG, tag, msg + '\n' + getStackTraceString(tr)); }
  public static int i(String tag, String msg, Throwable tr) { return println(INFO, tag, msg + '\n' + getStackTraceString(tr)); }
  public static int w(String tag, String msg, Throwable tr) { return println(WARN, tag, msg + '\n' + getStackTraceString(tr)); }
  public static int e(String tag, String msg, Throwable tr) { return println(ERROR, tag, msg + '\n' + getStackTraceString(tr)); }

  public static boolean isLoggable(String tag, int level) { return true; }

  public static String getStackTraceString(Throwable tr) {
    if (tr == null) return "";
    java.io.StringWriter sw = new java.io.StringWriter();
    tr.printStackTrace(new java.io.PrintWriter(sw));
    return sw.toString();
  }

  public static int println(int priority, String tag, String msg) {
    return write0(priority, tag, msg == null ? "null" : msg);
  }

  // Native underside provided by aclcxx (registered via RegisterNatives at VM
  // start; forwards to aclcxx::log::write).
  private static native int write0(int priority, String tag, String msg);
}
