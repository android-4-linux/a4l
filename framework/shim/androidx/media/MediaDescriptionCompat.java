// SPDX-License-Identifier: Apache-2.0
// Provenance: androidx docs (developer.android.com/reference/androidx/media/... and
// android.support.v4.media.MediaDescriptionCompat): public API surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for androidx.media MediaDescriptionCompat + its
// Builder — the support-library twin of android.media.MediaDescription, used by
// MediaBrowserServiceCompat apps (the standard Android Auto base class). Media apps
// build browse items through the Builder; the acl media host reads title/mediaId
// back. Docs-derived shim, NOT vendored androidx.
package androidx.media;

public final class MediaDescriptionCompat {
  private final String mediaId;
  private final CharSequence title;

  MediaDescriptionCompat(String mediaId, CharSequence title) {
    this.mediaId = mediaId;
    this.title = title;
  }

  public String getMediaId() { return mediaId; }
  public CharSequence getTitle() { return title; }

  public static final class Builder {
    private String mediaId;
    private CharSequence title;

    public Builder() {}

    public Builder setMediaId(String mediaId) { this.mediaId = mediaId; return this; }
    public Builder setTitle(CharSequence title) { this.title = title; return this; }
    public MediaDescriptionCompat build() { return new MediaDescriptionCompat(mediaId, title); }
  }
}
