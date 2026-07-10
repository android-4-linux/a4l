// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/media/
// MediaDescription and its Builder): public API surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.media.MediaDescription + its Builder
// (Gate G2.e, automotive media path). Media apps build browse items through the
// Builder; the aclcxx media host reads title/mediaId back. Fields beyond title +
// mediaId are added as the app corpus needs them. Docs-derived shim, NOT vendored.
package android.media;

public final class MediaDescription {
  private final String mediaId;
  private final CharSequence title;

  MediaDescription(String mediaId, CharSequence title) {
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
    public MediaDescription build() { return new MediaDescription(mediaId, title); }
  }
}
