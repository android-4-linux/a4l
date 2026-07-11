// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/media/
// AudioTrack): public API surface + constants. The native underside (nativeOpen/
// Write/Start/Stop/Release) is acl's own bridge to the audio sink. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.media.AudioTrack (Gate G2.e, media
// audio). Apps write 16-bit PCM here; the acl native underside forwards it to
// an acl::audio::Sink (PipeWire when built + selected, else a counting sink).
// The 2-arg constructor is an acl simplification of AudioTrack(streamType,
// sampleRate, channelConfig, format, bufSize, mode) / AudioTrack.Builder — real
// ctors arrive with the /range-invoke or Builder path. Docs-derived shim, NOT
// vendored AOSP.
package android.media;

public class AudioTrack {
  public static final int MODE_STREAM = 1;
  public static final int MODE_STATIC = 0;
  public static final int ENCODING_PCM_16BIT = 2;
  public static final int CHANNEL_OUT_MONO = 4;
  public static final int CHANNEL_OUT_STEREO = 12;
  public static final int STREAM_MUSIC = 3;
  public static final int PLAYSTATE_STOPPED = 1;
  public static final int PLAYSTATE_PLAYING = 3;

  private long nativeHandle;

  public AudioTrack(int sampleRateInHz, int channels) {
    this.nativeHandle = nativeOpen(sampleRateInHz, channels);
  }

  public void play() { nativeStart(nativeHandle); }
  public void stop() { nativeStop(nativeHandle); }
  public void release() {
    nativeRelease(nativeHandle);
    nativeHandle = 0;
  }

  // Write 16-bit PCM. Returns the number of samples written (or < 0 on error).
  public int write(short[] audioData, int offsetInShorts, int sizeInShorts) {
    if (nativeHandle == 0) return -1;
    return nativeWrite(nativeHandle, audioData, offsetInShorts, sizeInShorts);
  }

  // acl native underside (RegisterNatives -> acl::audio).
  private static native long nativeOpen(int sampleRate, int channels);
  private static native int nativeWrite(long handle, short[] data, int offset, int size);
  private static native void nativeStart(long handle);
  private static native void nativeStop(long handle);
  private static native void nativeRelease(long handle);
}
