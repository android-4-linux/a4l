// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.AudioManager stream-volume surface (AOSP android.jar stubs,
// Apache-2.0); backed by the acl svc/audio host PipeWire master-volume controller (default
// sink). Consumed by the seeded IAudioService shim (HostBootstrap.makeAudioHandler). No GPL-3.0 sources.
package org.acl.internal;

/**
 * The native bridge from the seeded IAudioService shim to the acl PipeWire master-volume
 * controller. Android's index-based stream volume maps onto the host default sink's volume;
 * the stream type is ignored for now (one master output). Bound at host-ART boot under
 * ACL_HAVE_PIPEWIRE; absent → a fixed silent default.
 */
public final class AudioBridge {
    private AudioBridge() {}

    static native boolean nAvailable();
    static native int nMaxVolume(int stream);
    static native int nMinVolume(int stream);
    static native int nGetVolume(int stream);
    static native void nSetVolume(int stream, int index);
    static native boolean nIsMuted(int stream);
    static native void nSetMuted(int stream, boolean mute);

    public static int maxVolume(int stream) {
        try { return nMaxVolume(stream); } catch (Throwable t) { return 15; }
    }
    public static int minVolume(int stream) {
        try { return nMinVolume(stream); } catch (Throwable t) { return 0; }
    }
    public static int getVolume(int stream) {
        try { return nGetVolume(stream); } catch (Throwable t) { return 0; }
    }
    public static void setVolume(int stream, int index) {
        try { nSetVolume(stream, index); } catch (Throwable ignored) {}
    }
    public static boolean isMuted(int stream) {
        try { return nIsMuted(stream); } catch (Throwable t) { return false; }
    }
    public static void setMuted(int stream, boolean mute) {
        try { nSetMuted(stream, mute); } catch (Throwable ignored) {}
    }

    // adjustStreamVolume(direction): RAISE=1, LOWER=-1, MUTE=-100, UNMUTE=100, TOGGLE_MUTE=101.
    public static void adjust(int stream, int direction) {
        if (direction == -100) { setMuted(stream, true); return; }
        if (direction == 100) { setMuted(stream, false); return; }
        if (direction == 101) { setMuted(stream, !isMuted(stream)); return; }
        int v = getVolume(stream);
        if (direction > 0) v++; else if (direction < 0) v--;
        int max = maxVolume(stream);
        if (v < 0) v = 0; else if (v > max) v = max;
        setVolume(stream, v);
    }
}
