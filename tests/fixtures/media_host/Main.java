// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; public Android API only. No GPL-3.0 sources.
//
// A media browser service that is not a MediaBrowserService by class: Wrapped extends Service
// and hands out the binder of a MediaBrowserService it keeps inside, which is how androidx's
// MediaBrowserServiceCompat is built. With R8, that androidx class has another name, so a host
// that looks for the base class by name finds neither. a4l binds it through the framework's
// interface instead (org.acl.internal.MediaHost).
//
// The service logs "MEDIAHOST: onGetRoot ..." with whether the caller it was given is this
// package with this process's uid, which is what a service checks before it answers.
package com.acl.mediahost;

import android.app.Activity;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.media.MediaDescription;
import android.media.browse.MediaBrowser.MediaItem;
import android.media.session.MediaSession;
import android.os.Bundle;
import android.os.IBinder;
import android.service.media.MediaBrowserService;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

public class Main extends Activity {
    public static class Wrapped extends Service {
        private final Inner inner = new Inner();

        @Override public void onCreate() {
            super.onCreate();
            inner.attach(this);
            inner.onCreate();
        }

        @Override public IBinder onBind(Intent intent) {
            return inner.onBind(intent);
        }
    }

    static class Inner extends MediaBrowserService {
        void attach(Context base) {
            attachBaseContext(base);
        }

        @Override public void onCreate() {
            super.onCreate();
            // A browser's connection completes once the service has a session token.
            setSessionToken(new MediaSession(this, "media-host").getSessionToken());
        }

        @Override public BrowserRoot onGetRoot(String clientPackage, int clientUid, Bundle hints) {
            boolean known = false;
            try {
                known = getPackageName().equals(clientPackage)
                        && clientUid == android.os.Process.myUid()
                        && getPackageManager().getPackageInfo(clientPackage, 0)
                                .applicationInfo.uid == clientUid;
            } catch (android.content.pm.PackageManager.NameNotFoundException e) {
                // known stays false
            }
            Log.i("acl", "MEDIAHOST: onGetRoot client known=" + known);
            return known ? new BrowserRoot("root", null) : null;
        }

        @Override public void onLoadChildren(String parentId, Result<List<MediaItem>> result) {
            final List<MediaItem> items = new ArrayList<>();
            if ("root".equals(parentId)) {
                items.add(new MediaItem(new MediaDescription.Builder().setMediaId("albums")
                        .setTitle("Albums").build(), MediaItem.FLAG_BROWSABLE));
                items.add(new MediaItem(new MediaDescription.Builder().setMediaId("track-1")
                        .setTitle("Track 1").build(), MediaItem.FLAG_PLAYABLE));
            }
            result.sendResult(items);
        }
    }
}
