// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/media/
// MediaMetadata and its Builder): public API surface + metadata key constants.
// No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.media.MediaMetadata + Builder
// (Gate G2.e, media session). Media apps publish now-playing info (title, artist)
// through the Builder; the aclcxx media host reads it back. String keys only for
// now; long/rating/bitmap values are added as the app corpus needs them.
// Docs-derived shim, NOT vendored AOSP.
package android.media;

import java.util.HashMap;
import java.util.Map;

public final class MediaMetadata {
  public static final String METADATA_KEY_TITLE = "android.media.metadata.TITLE";
  public static final String METADATA_KEY_ARTIST = "android.media.metadata.ARTIST";
  public static final String METADATA_KEY_ALBUM = "android.media.metadata.ALBUM";

  private final Map<String, String> strings;

  MediaMetadata(Map<String, String> strings) { this.strings = strings; }

  public String getString(String key) { return strings.get(key); }

  public static final class Builder {
    private final Map<String, String> strings = new HashMap<>();

    public Builder() {}

    public Builder putString(String key, String value) {
      strings.put(key, value);
      return this;
    }

    public MediaMetadata build() { return new MediaMetadata(new HashMap<>(strings)); }
  }
}
