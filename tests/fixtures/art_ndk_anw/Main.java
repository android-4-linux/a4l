// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl test fixture; Android SDK docs (android.media.ImageReader,
// android.view.Surface, android.hardware.HardwareBuffer) + the NDK ANativeWindow contract. No GPL-3.0 sources.
//
// The app half of the ANativeWindow-producer end-to-end ([[flutter-texture-bridge]] M3c): an
// Activity that creates a real android.media.ImageReader (its consumer/BufferQueue is acl's), hands
// the producer Surface to a *bionic-ABI* lib/x86_64/libanwtest.so, and that native drives one frame
// through the real NDK ANativeWindow C ABI — ANativeWindow_fromSurface -> lock (dequeue + CPU-map a
// pooled dma-buf) -> fill -> unlockAndPost (queue). The Java side then acquireLatestImage()s the
// posted frame. So lockW=64 proves the producer mapped a real 64x64 pooled buffer across the
// bionic->host %fs boundary, and image=true proves the post reached the consumer — the counterpart
// to the already-proven consume/composite path (which a real MediaCodec setVideoSurface would use).
package com.acl.anwtest;

import android.app.Activity;
import android.graphics.ImageFormat;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;

public class Main extends Activity {
    static {
        System.loadLibrary("anwtest");   // -> Runtime.nativeLoad -> acl's in-process bionic loader
    }

    // Bionic native: one ANativeWindow producer frame into the ImageReader's Surface. Returns the
    // locked buffer width (64) on success, or a negative step code (see anwtest.c).
    public static native int produce(Surface s, int color);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ImageReader r = ImageReader.newInstance(64, 64, ImageFormat.PRIVATE, 3,
                HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE | HardwareBuffer.USAGE_CPU_WRITE_OFTEN);
        Surface s = r.getSurface();
        int w = produce(s, 0xFF00FF00);          // green; the native drives ANativeWindow_*
        Image img = r.acquireLatestImage();      // the frame the producer posted
        boolean got = img != null;
        if (img != null) {
            img.close();
        }
        // lockW=64: ANativeWindow_fromSurface+lock mapped a real 64x64 pooled buffer. image=true:
        // unlockAndPost queued it and the consumer acquired it — the full producer->consumer path.
        Log.i("acl", "ANW produce: lockW=" + w + " image=" + got);
    }
}
