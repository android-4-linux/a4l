// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/service/
// media/MediaBrowserService and its BrowserRoot/Result nested types): the public
// media-browse API surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.service.media.MediaBrowserService
// (Gate G2.e, automotive media path). A media app subclasses it and implements
// onGetRoot (browse-tree root) and onLoadChildren (children of a node). The real
// class exposes these over a MediaSession/Binder connection; per ADR-3 the aclcxx
// media host calls them in-process, no Binder — constructing a Result, calling
// onLoadChildren, and reading back what the app sent. Docs-derived shim, NOT
// vendored AOSP.
package android.service.media;

import android.media.browse.MediaBrowser;
import android.os.Bundle;

import java.util.List;

public abstract class MediaBrowserService extends android.app.Service {
  public MediaBrowserService() {}

  // Return the browse-tree root for a connecting client, or null to reject.
  public abstract BrowserRoot onGetRoot(String clientPackageName, int clientUid,
                                        Bundle rootHints);

  // Load the children of `parentId`; the app calls result.sendResult(children).
  public abstract void onLoadChildren(String parentId,
                                      Result<List<MediaBrowser.MediaItem>> result);

  // The receiver an app calls sendResult() on. The real Result is delivered over a
  // MediaSession connection; here it holds the value so the in-process host reads it
  // (aclcxxValue/aclcxxSent are aclcxx accessors, not Android API).
  public static class Result<T> {
    private T value;
    private boolean sent;

    public Result() {}

    public void sendResult(T result) { this.value = result; this.sent = true; }
    public void detach() {}

    public T aclcxxValue() { return value; }
    public boolean aclcxxSent() { return sent; }
  }

  // The root of a browse tree returned by onGetRoot.
  public static final class BrowserRoot {
    private final String rootId;
    private final Bundle extras;

    public BrowserRoot(String rootId, Bundle extras) {
      if (rootId == null) {
        throw new IllegalArgumentException("rootId cannot be null");
      }
      this.rootId = rootId;
      this.extras = extras;
    }

    public String getRootId() { return rootId; }
    public Bundle getExtras() { return extras; }
  }
}
