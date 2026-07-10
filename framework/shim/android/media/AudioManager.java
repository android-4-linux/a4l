// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/media/
// AudioManager): public API surface + stream constants. The native underside is
// aclcxx's own bridge to the svc/ AudioService (no Binder). No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.media.AudioManager (Gate G2.e, svc
// hub). Returned by Context.getSystemService(AUDIO_SERVICE); its native methods
// call the in-process aclcxx::svc::AudioService directly (ADR-3, shim-at-Manager-
// boundary). Docs-derived shim, NOT vendored AOSP.
package android.media;

public class AudioManager {
  public static final int STREAM_VOICE_CALL = 0;
  public static final int STREAM_RING = 2;
  public static final int STREAM_MUSIC = 3;
  public static final int STREAM_ALARM = 4;

  public AudioManager() {}

  public int getStreamVolume(int streamType) { return nativeGetStreamVolume(streamType); }
  public void setStreamVolume(int streamType, int index, int flags) {
    nativeSetStreamVolume(streamType, index);
  }
  public int getStreamMaxVolume(int streamType) { return nativeGetStreamMaxVolume(streamType); }
  public boolean isMusicActive() { return nativeIsMusicActive(); }

  // aclcxx native underside (RegisterNatives -> aclcxx::svc::AudioService).
  private static native int nativeGetStreamVolume(int streamType);
  private static native void nativeSetStreamVolume(int streamType, int index);
  private static native int nativeGetStreamMaxVolume(int streamType);
  private static native boolean nativeIsMusicActive();
}
