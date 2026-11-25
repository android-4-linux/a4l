// SPDX-License-Identifier: Apache-2.0
// Provenance: original; public Android API only (android.media.browse.MediaBrowser,
// android.media.session.MediaController, android.widget). No GPL-3.0 sources.
package org.acl.internal;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Typeface;
import android.media.MediaDescription;
import android.media.MediaMetadata;
import android.media.browse.MediaBrowser;
import android.media.session.MediaController;
import android.media.session.PlaybackState;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.TextUtils;
import android.util.LruCache;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import java.io.InputStream;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * A window for an app that has none: a media app made for a car has a media browser service
 * and expects the car's media app to draw it. This is that media app, for a desktop.
 *
 * <p>{@code a4l run --service=<class> --present} starts it with ACL_MEDIA_SERVICE naming the
 * service. It binds the service with a MediaBrowser, shows the root's children as tabs and a
 * tab's children as a list, opens a browsable item and plays a playable one through the
 * session's transport controls. A bar at the bottom shows the track and the transport from
 * the session. When the session reports an error that names a way to resolve it (signing in,
 * as a rule), a button runs it.
 */
public final class MediaBrowseActivity extends Activity {
    private static final int BG = 0xFF121212, BAR = 0xFF1E1E1E, TEXT = 0xFFFFFFFF,
            DIM = 0xFFB3B3B3, ACCENT = 0xFF1DB954;
    // What a car's media app passes; an app reads these to size what it sends.
    private static final String ERROR_LABEL =
            "android.media.extras.ERROR_RESOLUTION_ACTION_LABEL";
    private static final String ERROR_INTENT =
            "android.media.extras.ERROR_RESOLUTION_ACTION_INTENT";

    private final Handler mMain = new Handler(Looper.getMainLooper());
    private final ExecutorService mArt = Executors.newFixedThreadPool(2);
    private final LruCache<String, Bitmap> mArtCache = new LruCache<>(64);
    private final List<MediaBrowser.MediaItem> mItems = new ArrayList<>();
    private final ArrayDeque<String[]> mStack = new ArrayDeque<>();  // {id, title}

    private MediaBrowser mBrowser;
    private MediaController mController;
    private String mSubscribed;
    private String mTab;
    private float mDp;

    private LinearLayout mTabs;
    private TextView mPath;
    private TextView mStatus;
    private Button mResolve;
    private ItemAdapter mAdapter;
    private ImageView mNowArt;
    private TextView mNowTitle;
    private TextView mNowArtist;
    private ImageView mPrev;
    private ImageView mPlay;
    private ImageView mNext;

    @Override protected void onCreate(Bundle saved) {
        setTheme(android.R.style.Theme_Material_NoActionBar);
        super.onCreate(saved);
        mDp = getResources().getDisplayMetrics().density;
        final String label = HostBootstrap.appLabel();
        setTitle(label != null ? label : getPackageName());
        setContentView(build());

        final String service = System.getenv("ACL_MEDIA_SERVICE");
        if (service == null || service.isEmpty()) {
            status("No media service was named.");
            return;
        }
        status("Connecting\u2026");
        final Bundle hints = new Bundle();
        hints.putInt("android.media.extras.MEDIA_ART_SIZE_HINT_PIXELS", px(96));
        mBrowser = new MediaBrowser(this, new ComponentName(getPackageName(), service),
                new MediaBrowser.ConnectionCallback() {
                    @Override public void onConnected() {
                        connected();
                    }
                    @Override public void onConnectionFailed() {
                        status("The app refused the connection.");
                    }
                    @Override public void onConnectionSuspended() {
                        status("The connection to the app was lost.");
                    }
                }, hints);
        mBrowser.connect();
    }

    @Override protected void onResume() {
        super.onResume();
        // Back from the app's own screen, signing in as a rule: ask again.
        if (mBrowser != null && mBrowser.isConnected() && mSubscribed != null) {
            final String id = mSubscribed;
            mSubscribed = null;
            open(id);
        }
    }

    @Override protected void onDestroy() {
        mArt.shutdownNow();
        if (mController != null) mController.unregisterCallback(mSession);
        if (mBrowser != null) mBrowser.disconnect();
        super.onDestroy();
    }

    @Override public void onBackPressed() {
        if (!up()) super.onBackPressed();
    }

    // ---- the tree ----
    private void connected() {
        System.out.println("acl media: browse root: " + mBrowser.getRoot());
        mController = new MediaController(this, mBrowser.getSessionToken());
        mController.registerCallback(mSession, mMain);
        showSession();
        mBrowser.subscribe(mBrowser.getRoot(), new MediaBrowser.SubscriptionCallback() {
            @Override public void onChildrenLoaded(String parent,
                    List<MediaBrowser.MediaItem> children) {
                tabs(children);
            }
            @Override public void onError(String parent) {
                status("The app gave no content.");
                showSession();
            }
        });
    }

    // The root's browsable children are the tabs; playable ones go in the list.
    private void tabs(List<MediaBrowser.MediaItem> children) {
        System.out.println("acl media: children of '" + mBrowser.getRoot() + "': "
                + children.size() + " item(s)");
        mTabs.removeAllViews();
        final List<MediaBrowser.MediaItem> playable = new ArrayList<>();
        String first = null;
        boolean keep = false;
        for (final MediaBrowser.MediaItem item : children) {
            if (!item.isBrowsable()) {
                playable.add(item);
                continue;
            }
            final String id = item.getMediaId();
            final String title = text(item.getDescription().getTitle());
            if (first == null) first = id;
            keep |= id != null && id.equals(mTab);
            final TextView tab = label(title, 15, TEXT);
            tab.setTag(id);
            tab.setGravity(Gravity.CENTER);
            tab.setPadding(px(18), 0, px(18), 0);
            tab.setOnClickListener(v -> selectTab(id, title));
            mTabs.addView(tab, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, px(48)));
        }
        if (first == null) {
            show(mBrowser.getRoot(), playable);
        } else if (!keep) {
            selectTab(first, text(((TextView) mTabs.getChildAt(0)).getText()));
        } else {
            markTab();
        }
    }

    private void selectTab(String id, String title) {
        mTab = id;
        mStack.clear();
        mStack.push(new String[]{id, title});
        markTab();
        open(id);
    }

    private void markTab() {
        for (int i = 0; i < mTabs.getChildCount(); i++) {
            final TextView tab = (TextView) mTabs.getChildAt(i);
            final boolean on = tab.getTag() != null && tab.getTag().equals(mTab);
            tab.setTextColor(on ? ACCENT : DIM);
            tab.setTypeface(null, on ? Typeface.BOLD : Typeface.NORMAL);
        }
    }

    private void open(final String id) {
        if (mSubscribed != null) mBrowser.unsubscribe(mSubscribed);
        mSubscribed = id;
        path();
        status("Loading\u2026");
        mBrowser.subscribe(id, new MediaBrowser.SubscriptionCallback() {
            @Override public void onChildrenLoaded(String parent,
                    List<MediaBrowser.MediaItem> children) {
                if (parent.equals(mSubscribed)) show(parent, children);
            }
            @Override public void onError(String parent) {
                if (parent.equals(mSubscribed)) {
                    show(parent, new ArrayList<MediaBrowser.MediaItem>());
                    status("Nothing to show here.");
                    showSession();
                }
            }
        });
    }

    private void show(String parent, List<MediaBrowser.MediaItem> children) {
        System.out.println("acl media: children of '" + parent + "': " + children.size()
                + " item(s)");
        mItems.clear();
        mItems.addAll(children);
        mAdapter.notifyDataSetChanged();
        status(children.isEmpty() ? "Nothing to show here." : null);
    }

    private void click(MediaBrowser.MediaItem item) {
        final String id = item.getMediaId();
        if (id == null) return;
        if (item.isPlayable() && mController != null) {
            System.out.println("acl media: play " + id);
            mController.getTransportControls().playFromMediaId(id,
                    item.getDescription().getExtras());
        } else if (item.isBrowsable()) {
            mStack.push(new String[]{id, text(item.getDescription().getTitle())});
            open(id);
        }
    }

    private boolean up() {
        if (mStack.size() <= 1) return false;
        mStack.pop();
        open(mStack.peek()[0]);
        return true;
    }

    private void path() {
        final boolean deep = mStack.size() > 1;
        mPath.setVisibility(deep ? View.VISIBLE : View.GONE);
        if (deep) mPath.setText("\u2039  " + mStack.peek()[1]);
    }

    // ---- the session ----
    private final MediaController.Callback mSession = new MediaController.Callback() {
        @Override public void onMetadataChanged(MediaMetadata metadata) {
            showSession();
        }
        @Override public void onPlaybackStateChanged(PlaybackState state) {
            showSession();
        }
        @Override public void onSessionDestroyed() {
            status("The app ended its session.");
        }
    };

    private void showSession() {
        if (mController == null) return;
        final MediaMetadata md = mController.getMetadata();
        final PlaybackState ps = mController.getPlaybackState();
        final String title = md == null ? "" : first(md,
                MediaMetadata.METADATA_KEY_DISPLAY_TITLE, MediaMetadata.METADATA_KEY_TITLE);
        mNowTitle.setText(title.isEmpty() ? "Nothing playing" : title);
        mNowArtist.setText(md == null ? "" : first(md, MediaMetadata.METADATA_KEY_ARTIST,
                MediaMetadata.METADATA_KEY_DISPLAY_SUBTITLE));
        Bitmap art = null;
        String artUri = "";
        if (md != null) {
            art = md.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART);
            if (art == null) art = md.getBitmap(MediaMetadata.METADATA_KEY_ART);
            if (art == null) art = md.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON);
            artUri = first(md, MediaMetadata.METADATA_KEY_ALBUM_ART_URI,
                    MediaMetadata.METADATA_KEY_ART_URI,
                    MediaMetadata.METADATA_KEY_DISPLAY_ICON_URI);
        }
        picture(mNowArt, art, artUri.isEmpty() ? null : Uri.parse(artUri));

        final long actions = ps != null ? ps.getActions() : 0;
        final boolean playing = ps != null && ps.getState() == PlaybackState.STATE_PLAYING;
        ((Glyph) mPlay.getDrawable()).set(playing ? Glyph.PAUSE : Glyph.PLAY, true);
        enable(mPrev, (actions & PlaybackState.ACTION_SKIP_TO_PREVIOUS) != 0);
        enable(mNext, (actions & PlaybackState.ACTION_SKIP_TO_NEXT) != 0);

        // An error that names its remedy: a label and a PendingIntent, as a car is given.
        PendingIntent resolve = null;
        CharSequence resolveLabel = null;
        if (ps != null && ps.getState() == PlaybackState.STATE_ERROR) {
            final Bundle extras = ps.getExtras();
            if (extras != null) {
                resolve = extras.getParcelable(ERROR_INTENT);
                resolveLabel = extras.getCharSequence(ERROR_LABEL);
            }
            if (ps.getErrorMessage() != null) status(ps.getErrorMessage().toString());
        }
        final PendingIntent run = resolve;
        mResolve.setVisibility(run != null ? View.VISIBLE : View.GONE);
        if (run != null) {
            mResolve.setText(resolveLabel != null ? resolveLabel : "Continue");
            mResolve.setOnClickListener(v -> {
                try {
                    run.send();
                } catch (PendingIntent.CanceledException e) {
                    status("That is no longer available.");
                }
            });
        }
    }

    private static String first(MediaMetadata md, String... keys) {
        for (String k : keys) {
            final CharSequence v = md.getText(k);
            if (v != null && v.length() > 0) return v.toString();
        }
        return "";
    }

    // ---- pictures ----
    // A Bitmap is shown as it is. A URI is read off the main thread: a content URI from the
    // app's own provider, an http or https one from the network. The view is tagged with what
    // it is waiting for, so a row that has been reused does not get another row's picture.
    private void picture(final ImageView view, Bitmap bitmap, final Uri uri) {
        if (bitmap != null) {
            view.setTag(null);
            view.setImageBitmap(bitmap);
            return;
        }
        if (uri == null) {
            view.setTag(null);
            view.setImageDrawable(null);
            return;
        }
        final String key = uri.toString();
        final Bitmap cached = mArtCache.get(key);
        if (cached != null) {
            view.setTag(null);
            view.setImageBitmap(cached);
            return;
        }
        if (key.equals(view.getTag())) return;  // already on its way
        view.setTag(key);
        view.setImageDrawable(null);
        final int edge = px(96);
        try {
            mArt.execute(() -> {
                final Bitmap loaded = load(uri, edge);
                if (loaded == null) return;
                mMain.post(() -> {
                    mArtCache.put(key, loaded);
                    if (key.equals(view.getTag())) {
                        view.setTag(null);
                        view.setImageBitmap(loaded);
                    }
                });
            });
        } catch (java.util.concurrent.RejectedExecutionException e) {
            // The window is closing.
        }
    }

    private Bitmap load(Uri uri, int edge) {
        final String scheme = uri.getScheme();
        try {
            final InputStream in;
            if ("content".equals(scheme) || "android.resource".equals(scheme)
                    || "file".equals(scheme)) {
                in = getContentResolver().openInputStream(uri);
            } else if ("https".equals(scheme) || "http".equals(scheme)) {
                final java.net.URLConnection c = new java.net.URL(uri.toString())
                        .openConnection();
                c.setConnectTimeout(8000);
                c.setReadTimeout(8000);
                in = c.getInputStream();
            } else {
                return null;
            }
            if (in == null) return null;
            try {
                final Bitmap full = BitmapFactory.decodeStream(in);
                if (full == null) return null;
                final int longer = Math.max(full.getWidth(), full.getHeight());
                if (longer <= edge * 2) return full;
                final float scale = (float) (edge * 2) / longer;
                final Bitmap small = Bitmap.createScaledBitmap(full,
                        Math.max(1, Math.round(full.getWidth() * scale)),
                        Math.max(1, Math.round(full.getHeight() * scale)), true);
                if (small != full) full.recycle();
                return small;
            } finally {
                in.close();
            }
        } catch (Exception e) {
            return null;
        }
    }

    // ---- the views ----
    private View build() {
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(BG);

        mTabs = new LinearLayout(this);
        mTabs.setOrientation(LinearLayout.HORIZONTAL);
        final HorizontalScrollView scroll = new HorizontalScrollView(this);
        scroll.setHorizontalScrollBarEnabled(false);
        scroll.setBackgroundColor(BAR);
        scroll.addView(mTabs);
        root.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, px(48)));

        mPath = label("", 15, TEXT);
        mPath.setPadding(px(16), 0, px(16), 0);
        mPath.setGravity(Gravity.CENTER_VERTICAL);
        mPath.setVisibility(View.GONE);
        mPath.setOnClickListener(v -> up());
        root.addView(mPath, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, px(40)));

        mStatus = label("", 14, DIM);
        mStatus.setPadding(px(16), px(12), px(16), px(4));
        mStatus.setVisibility(View.GONE);
        root.addView(mStatus);

        mResolve = new Button(this);
        mResolve.setVisibility(View.GONE);
        final LinearLayout.LayoutParams rp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        rp.setMargins(px(16), px(4), px(16), px(8));
        root.addView(mResolve, rp);

        final ListView list = new ListView(this);
        list.setDividerHeight(0);
        list.setBackgroundColor(BG);
        mAdapter = new ItemAdapter();
        list.setAdapter(mAdapter);
        list.setOnItemClickListener((parent, view, position, id) -> {
            if (position >= 0 && position < mItems.size()) click(mItems.get(position));
        });
        root.addView(list, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        root.addView(nowPlaying(), new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, px(72)));
        return root;
    }

    private View nowPlaying() {
        final LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setBackgroundColor(BAR);
        bar.setPadding(px(12), 0, px(12), 0);

        mNowArt = new ImageView(this);
        mNowArt.setScaleType(ImageView.ScaleType.CENTER_CROP);
        mNowArt.setBackgroundColor(0xFF2A2A2A);
        bar.addView(mNowArt, new LinearLayout.LayoutParams(px(48), px(48)));

        final LinearLayout text = new LinearLayout(this);
        text.setOrientation(LinearLayout.VERTICAL);
        text.setPadding(px(12), 0, px(12), 0);
        mNowTitle = label("Nothing playing", 15, TEXT);
        mNowArtist = label("", 13, DIM);
        text.addView(mNowTitle);
        text.addView(mNowArtist);
        bar.addView(text, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        mPrev = transport(Glyph.PREVIOUS, "Previous");
        mPlay = transport(Glyph.PLAY, "Play or pause");
        mNext = transport(Glyph.NEXT, "Next");
        mPrev.setOnClickListener(v -> {
            if (mController != null) mController.getTransportControls().skipToPrevious();
        });
        mNext.setOnClickListener(v -> {
            if (mController != null) mController.getTransportControls().skipToNext();
        });
        mPlay.setOnClickListener(v -> {
            if (mController == null) return;
            final PlaybackState ps = mController.getPlaybackState();
            if (ps != null && ps.getState() == PlaybackState.STATE_PLAYING) {
                mController.getTransportControls().pause();
            } else {
                mController.getTransportControls().play();
            }
        });
        for (ImageView b : new ImageView[]{mPrev, mPlay, mNext}) {
            bar.addView(b, new LinearLayout.LayoutParams(px(56), px(48)));
        }
        return bar;
    }

    private ImageView transport(int shape, String description) {
        final ImageView b = new ImageView(this);
        b.setImageDrawable(new Glyph(shape));
        b.setScaleType(ImageView.ScaleType.FIT_XY);
        b.setContentDescription(description);
        b.setClickable(true);
        b.setFocusable(true);
        return b;
    }

    private static void enable(ImageView button, boolean on) {
        button.setEnabled(on);
        ((Glyph) button.getDrawable()).set(-1, on);
    }

    // The transport symbols, drawn: the fonts a desktop has do not all carry them.
    private static final class Glyph extends android.graphics.drawable.Drawable {
        static final int PLAY = 0, PAUSE = 1, PREVIOUS = 2, NEXT = 3;
        private final android.graphics.Paint mPaint =
                new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        private final android.graphics.Path mPath = new android.graphics.Path();
        private int mShape;
        private boolean mOn = true;

        Glyph(int shape) {
            mShape = shape;
        }

        // shape < 0 keeps the shape.
        void set(int shape, boolean on) {
            if (shape >= 0) mShape = shape;
            mOn = on;
            invalidateSelf();
        }

        @Override public void draw(android.graphics.Canvas canvas) {
            final android.graphics.Rect r = getBounds();
            final float s = Math.min(r.width(), r.height()) * 0.36f;  // the symbol's edge
            final float cx = r.exactCenterX(), cy = r.exactCenterY();
            final float l = cx - s / 2, t = cy - s / 2, rt = cx + s / 2, bt = cy + s / 2;
            mPaint.setColor(mOn ? TEXT : 0xFF5A5A5A);
            mPath.reset();
            switch (mShape) {
                case PAUSE:
                    canvas.drawRect(l, t, l + s * 0.34f, bt, mPaint);
                    canvas.drawRect(rt - s * 0.34f, t, rt, bt, mPaint);
                    return;
                case PREVIOUS:
                    canvas.drawRect(l, t, l + s * 0.16f, bt, mPaint);
                    mPath.moveTo(rt, t);
                    mPath.lineTo(l + s * 0.16f, cy);
                    mPath.lineTo(rt, bt);
                    break;
                case NEXT:
                    canvas.drawRect(rt - s * 0.16f, t, rt, bt, mPaint);
                    mPath.moveTo(l, t);
                    mPath.lineTo(rt - s * 0.16f, cy);
                    mPath.lineTo(l, bt);
                    break;
                default:
                    mPath.moveTo(l + s * 0.08f, t);
                    mPath.lineTo(rt, cy);
                    mPath.lineTo(l + s * 0.08f, bt);
                    break;
            }
            mPath.close();
            canvas.drawPath(mPath, mPaint);
        }

        @Override public void setAlpha(int alpha) {}
        @Override public void setColorFilter(android.graphics.ColorFilter filter) {}
        @Override public int getOpacity() {
            return android.graphics.PixelFormat.TRANSLUCENT;
        }
    }

    private TextView label(String text, int sp, int color) {
        final TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(sp);
        t.setTextColor(color);
        t.setSingleLine(true);
        t.setEllipsize(TextUtils.TruncateAt.END);
        return t;
    }

    private void status(String text) {
        mStatus.setVisibility(text != null ? View.VISIBLE : View.GONE);
        if (text != null) mStatus.setText(text);
    }

    private static String text(CharSequence cs) {
        return cs != null ? cs.toString() : "";
    }

    private int px(int dp) {
        return Math.round(dp * mDp);
    }

    private final class ItemAdapter extends BaseAdapter {
        @Override public int getCount() {
            return mItems.size();
        }
        @Override public Object getItem(int position) {
            return mItems.get(position);
        }
        @Override public long getItemId(int position) {
            return position;
        }
        @Override public View getView(int position, View convert, ViewGroup parent) {
            final LinearLayout row;
            if (convert instanceof LinearLayout) {
                row = (LinearLayout) convert;
            } else {
                row = new LinearLayout(MediaBrowseActivity.this);
                row.setOrientation(LinearLayout.HORIZONTAL);
                row.setGravity(Gravity.CENTER_VERTICAL);
                row.setPadding(px(16), 0, px(16), 0);
                row.setLayoutParams(new ListView.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, px(64)));
                final ImageView icon = new ImageView(MediaBrowseActivity.this);
                icon.setScaleType(ImageView.ScaleType.CENTER_CROP);
                icon.setBackgroundColor(0xFF2A2A2A);
                row.addView(icon, new LinearLayout.LayoutParams(px(48), px(48)));
                final LinearLayout text = new LinearLayout(MediaBrowseActivity.this);
                text.setOrientation(LinearLayout.VERTICAL);
                text.setPadding(px(12), 0, px(12), 0);
                text.addView(label("", 15, TEXT));
                text.addView(label("", 13, DIM));
                row.addView(text, new LinearLayout.LayoutParams(0,
                        ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
                row.addView(label("", 18, DIM));
            }
            final MediaBrowser.MediaItem item = mItems.get(position);
            final MediaDescription d = item.getDescription();
            final LinearLayout text = (LinearLayout) row.getChildAt(1);
            ((TextView) text.getChildAt(0)).setText(text(d.getTitle()));
            final TextView sub = (TextView) text.getChildAt(1);
            sub.setText(text(d.getSubtitle()));
            sub.setVisibility(TextUtils.isEmpty(d.getSubtitle()) ? View.GONE : View.VISIBLE);
            ((TextView) row.getChildAt(2)).setText(
                    item.isBrowsable() && !item.isPlayable() ? "\u203A" : "");
            picture((ImageView) row.getChildAt(0), d.getIconBitmap(), d.getIconUri());
            return row;
        }
    }
}
