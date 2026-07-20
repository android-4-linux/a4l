// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/hardware/HardwareBuffer):
// public API subset (FORMAT_*/USAGE_* constants, create/getWidth/getHeight/getFormat/getLayers/
// getUsage/close). Native underside -> acl::gfx AHardwareBuffer over a gbm dma-buf. The producer
// side of the Flutter external-texture bridge. Docs-derived shim, NOT vendored AOSP. No GPL-3.0 sources.
package android.hardware;

// A source-compatible shim for android.hardware.HardwareBuffer. nativePtr is an acl gfx
// AHardwareBuffer* (a refcounted dma-buf); AHardwareBuffer_fromHardwareBuffer reads it back.
public final class HardwareBuffer implements AutoCloseable {
  // Formats (values match the SDK and acl::gfx::HbFormat).
  public static final int RGBA_8888 = 1;
  public static final int RGBX_8888 = 2;
  public static final int RGB_888 = 3;
  public static final int RGB_565 = 4;

  // Usage bits (subset; advisory to acl's allocator, which is always CPU+GPU capable).
  public static final long USAGE_CPU_READ_RARELY = 2L;
  public static final long USAGE_CPU_WRITE_RARELY = 32L;
  public static final long USAGE_GPU_SAMPLED_IMAGE = 256L;
  public static final long USAGE_GPU_COLOR_OUTPUT = 512L;

  private long nativePtr;
  private final int width;
  private final int height;
  private final int format;
  private final int layers;
  private final long usage;

  private HardwareBuffer(long nativePtr, int width, int height, int format, int layers, long usage) {
    this.nativePtr = nativePtr;
    this.width = width;
    this.height = height;
    this.format = format;
    this.layers = layers;
    this.usage = usage;
  }

  public static HardwareBuffer create(int width, int height, int format, int layers, long usage) {
    long ptr = nCreate(width, height, format, layers, usage);
    if (ptr == 0L) throw new IllegalArgumentException("HardwareBuffer.create failed");
    return new HardwareBuffer(ptr, width, height, format, layers, usage);
  }

  public int getWidth() { return width; }
  public int getHeight() { return height; }
  public int getFormat() { return format; }
  public int getLayers() { return layers; }
  public long getUsage() { return usage; }
  public boolean isClosed() { return nativePtr == 0L; }

  // The acl AHardwareBuffer* — read by the native AHardwareBuffer_fromHardwareBuffer and by acl's
  // own producer/JNI. Not part of the public SDK.
  public long aclNativePtr() { return nativePtr; }

  @Override
  public void close() {
    if (nativePtr != 0L) { nRelease(nativePtr); nativePtr = 0L; }
  }

  private static native long nCreate(int width, int height, int format, int layers, long usage);
  private static native void nRelease(long ptr);
}
