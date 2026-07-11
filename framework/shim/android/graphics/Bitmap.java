// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/graphics/
// Bitmap): public API subset. Native underside -> acl::gfx::Surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.graphics.Bitmap (Phase 2). Wraps a
// gfx::Surface (ARGB_8888 buffer). createBitmap takes (w,h) here; the real
// (w,h,Config) overload arrives with the Config enum. Docs-derived shim, NOT
// vendored AOSP.
package android.graphics;

public class Bitmap {
  private long nativePtr;
  private final int width;
  private final int height;

  Bitmap(long nativePtr, int width, int height) {
    this.nativePtr = nativePtr;
    this.width = width;
    this.height = height;
  }

  public static Bitmap createBitmap(int width, int height) {
    return new Bitmap(nativeCreate(width, height), width, height);
  }

  public int getWidth() { return width; }
  public int getHeight() { return height; }
  public int getPixel(int x, int y) { return nativeGetPixel(nativePtr, x, y); }

  long aclPtr() { return nativePtr; }         // for Canvas(Bitmap)
  public void aclDescribe() { nativeDescribe(nativePtr); }  // dump sample pixels

  private static native long nativeCreate(int width, int height);
  private static native int nativeGetPixel(long ptr, int x, int y);
  private static native void nativeDescribe(long ptr);
}
