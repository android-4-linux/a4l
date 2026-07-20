// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/media/Image): public API
// subset used by Flutter's ImageExternalTexture — getHardwareBuffer(), close(), getWidth/getHeight/
// getFormat/getTimestamp. On the SDK Image is abstract; this is a concrete acl-producible instance
// backed by an android.hardware.HardwareBuffer (a dma-buf) so the engine's texture import can pull
// its AHardwareBuffer. Docs-derived shim, NOT vendored AOSP. No GPL-3.0 sources.
package android.media;

import android.hardware.HardwareBuffer;

public class Image implements AutoCloseable {
  private HardwareBuffer buffer;
  private final int width;
  private final int height;
  private final int format;
  private long timestamp;
  private boolean closed;

  // acl producers wrap a HardwareBuffer as an Image (the SDK creates Images via ImageReader; here a
  // producer hands one straight to TextureRegistry.ImageConsumer.pushImage).
  public Image(HardwareBuffer buffer, int format, long timestamp) {
    this.buffer = buffer;
    this.width = buffer.getWidth();
    this.height = buffer.getHeight();
    this.format = format;
    this.timestamp = timestamp;
  }

  public int getWidth() { return width; }
  public int getHeight() { return height; }
  public int getFormat() { return format; }
  public long getTimestamp() { return timestamp; }
  public void setTimestamp(long ts) { this.timestamp = ts; }

  // The engine's ImageExternalTexture path: getHardwareBuffer() then close(). Returns null once closed.
  public HardwareBuffer getHardwareBuffer() { return closed ? null : buffer; }

  @Override
  public void close() {
    if (!closed) { closed = true; buffer = null; }
  }
}
