// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; public Android API only. No GPL-3.0 sources.
//
// A content provider whose android:authorities is a string resource. The Activity asks the
// ContentResolver for the type of a URI under the authority the string holds, which reaches
// the provider only if it was installed under that name and not under the reference to it.
package com.acl.authres;

import android.app.Activity;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;

public class Main extends Activity {
    public static class Files extends ContentProvider {
        @Override public boolean onCreate() { return true; }
        @Override public String getType(Uri uri) { return "text/x-fixture"; }
        @Override public Cursor query(Uri u, String[] p, String s, String[] a, String o) {
            return null;
        }
        @Override public Uri insert(Uri u, ContentValues v) { return null; }
        @Override public int delete(Uri u, String s, String[] a) { return 0; }
        @Override public int update(Uri u, ContentValues v, String s, String[] a) { return 0; }
    }

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        final String authority = getString(R.string.authority);
        final String type = getContentResolver().getType(
                Uri.parse("content://" + authority + "/x"));
        Log.i("acl", "AUTHORITY: " + authority + " type=" + type);
        System.exit("text/x-fixture".equals(type) ? 0 : 1);
    }
}
