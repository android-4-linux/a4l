// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam to the svc/storage host UDisks2 backend; builds the
// android.os.storage.StorageVolume list an app's StorageManager/IStorageManager reads.
// The primary (emulated) volume is synthesized from the app Context; removable volumes come
// from UDisks2 via the natives. No GPL-3.0 sources.
package org.acl.internal;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;

import java.io.File;

/**
 * The native bridge from the host-ART StorageManager/IStorageManager path to the acl
 * svc/storage host UDisks2 backend. {@link #getVolumeList} answers
 * {@code IStorageManager.getVolumeList(int, String, int)} (and, transitively,
 * {@code StorageManager.getStorageVolumes()} / {@code Environment.getExternalStorageState()}):
 * the app's primary emulated volume (rooted at its XDG external storage) plus any mounted
 * removable filesystems UDisks2 reports. Removable natives are bound by acl at host-ART boot
 * under ACL_HAVE_SDBUS; absent -> just the primary volume.
 */
public final class StorageBridge {
    private StorageBridge() {}

    // Removable volumes from svc/storage (UDisks2). Absent natives -> zero removable volumes.
    static native int nVolumeCount();
    static native String nVolumeId(int i);
    static native String nVolumePath(int i);
    static native String nVolumeDescription(int i);
    static native String nVolumeFsUuid(int i);
    static native String nVolumeState(int i);

    private static int removableCount() {
        try { return nVolumeCount(); } catch (Throwable t) { return 0; }
    }
    private static String str(String s) { return s != null ? s : ""; }

    /**
     * Build the StorageVolume[] for IStorageManager.getVolumeList. Index 0 is always the
     * primary emulated volume (rooted at the app's external storage), so getExternalStorageState
     * and getPrimaryStorageVolume resolve; the rest are mounted removable media.
     */
    public static Object[] getVolumeList(Context ctx) {
        java.util.ArrayList<Object> out = new java.util.ArrayList<>();

        // Primary emulated volume — the app's shared external root (…/external, the parent of
        // getExternalFilesDir(null) = …/external/files). Non-removable, emulated, always mounted.
        File primaryPath = primaryRoot(ctx);
        Object primary = newStorageVolume("emulated", primaryPath, primaryPath,
                "Internal shared storage", true /*primary*/, false /*removable*/,
                true /*emulated*/, null /*fsUuid*/, "mounted");
        if (primary != null) out.add(primary);

        // Removable volumes from UDisks2 (mounted USB sticks / SD cards).
        int n = removableCount();
        for (int i = 0; i < n; i++) {
            try {
                String path = str(nVolumePath(i));
                if (path.isEmpty()) continue;
                File dir = new File(path);
                Object v = newStorageVolume(str(nVolumeId(i)), dir, dir,
                        str(nVolumeDescription(i)), false /*primary*/, true /*removable*/,
                        false /*emulated*/, emptyToNull(nVolumeFsUuid(i)), str(nVolumeState(i)));
                if (v != null) out.add(v);
            } catch (Throwable ignored) {
            }
        }

        // Return a typed StorageVolume[] (callers cast the array element type).
        try {
            Class<?> sv = Class.forName("android.os.storage.StorageVolume");
            Object arr = java.lang.reflect.Array.newInstance(sv, out.size());
            for (int i = 0; i < out.size(); i++) java.lang.reflect.Array.set(arr, i, out.get(i));
            return (Object[]) arr;
        } catch (Throwable t) {
            return out.toArray();
        }
    }

    private static File primaryRoot(Context ctx) {
        try {
            File ext = ctx.getExternalFilesDir(null);           // …/external/files
            if (ext != null) {
                File up = ext.getParentFile();                  // …/external
                if (up != null) { up.mkdirs(); return up; }
            }
        } catch (Throwable ignored) {
        }
        File fallback = new File(System.getProperty("java.io.tmpdir", "/tmp"), "acl-storage");
        fallback.mkdirs();
        return fallback;
    }

    // Construct the host-ART android.os.storage.StorageVolume via its real (hidden) constructor,
    // located by shape (14 args: String,File,File,String, then flags, long, UserHandle, UUID,
    // String,String) so a minor framework revision doesn't break an exact-type lookup. Falls back
    // to no-arg + private-field set. Returns null if neither works (caller drops the volume).
    private static Object newStorageVolume(String id, File path, File internalPath,
            String description, boolean primary, boolean removable, boolean emulated,
            String fsUuid, String state) {
        try {
            Class<?> sv = Class.forName("android.os.storage.StorageVolume");
            Object owner = android.os.Process.myUserHandle();
            java.lang.reflect.Constructor<?> best = null;
            for (java.lang.reflect.Constructor<?> c : sv.getDeclaredConstructors()) {
                Class<?>[] pt = c.getParameterTypes();
                if (pt.length == 14 && pt[0] == String.class && pt[1] == File.class) {
                    best = c;
                    break;
                }
            }
            if (best != null) {
                best.setAccessible(true);
                // id, path, internalPath, description, primary, removable, emulated,
                // externallyManaged, allowMassStorage, maxFileSize, owner, uuid, fsUuid, state
                return best.newInstance(id, path, internalPath, description, primary, removable,
                        emulated, false, false, 0L, owner, null, fsUuid, state);
            }
            // Fallback: no-arg + reflective field set (best-effort).
            java.lang.reflect.Constructor<?> noArg = sv.getDeclaredConstructor();
            noArg.setAccessible(true);
            Object v = noArg.newInstance();
            setField(sv, v, "mId", id);
            setField(sv, v, "mPath", path);
            setField(sv, v, "mInternalPath", internalPath);
            setField(sv, v, "mDescription", description);
            setField(sv, v, "mPrimary", primary);
            setField(sv, v, "mRemovable", removable);
            setField(sv, v, "mEmulated", emulated);
            setField(sv, v, "mOwner", owner);
            setField(sv, v, "mFsUuid", fsUuid);
            setField(sv, v, "mState", state);
            return v;
        } catch (Throwable t) {
            return null;
        }
    }

    private static void setField(Class<?> cls, Object o, String name, Object val) {
        try {
            java.lang.reflect.Field f = cls.getDeclaredField(name);
            f.setAccessible(true);
            f.set(o, val);
        } catch (Throwable ignored) {
        }
    }

    private static String emptyToNull(String s) {
        return (s == null || s.isEmpty()) ? null : s;
    }

    // ── Media mount/unmount broadcasts ───────────────────────────────────────────────────────
    // svc/storage fires onStorageChanged() on the D-Bus thread when removable media appears or
    // vanishes; diff against the last-seen mount points and emit ACTION_MEDIA_MOUNTED /
    // ACTION_MEDIA_UNMOUNTED through the app Context's receiver bus (like the bluetooth path).
    private static android.os.Handler sMain;
    private static boolean sInitialized;
    private static java.util.HashSet<String> sSeen = new java.util.HashSet<>();

    /** Called from the native change sink (D-Bus thread). */
    public static void onStorageChanged() {
        android.os.Handler h;
        synchronized (StorageBridge.class) {
            if (sMain == null) sMain = new android.os.Handler(android.os.Looper.getMainLooper());
            h = sMain;
        }
        h.post(new Runnable() { @Override public void run() { deliverChanges(); } });
    }

    private static void deliverChanges() {  // main thread
        int n = removableCount();
        java.util.HashSet<String> now = new java.util.HashSet<>();
        for (int i = 0; i < n; i++) {
            String p = str(nVolumePath(i));
            if (!p.isEmpty()) now.add(p);
        }
        if (!sInitialized) {  // baseline; don't replay the current world as "changes"
            sInitialized = true;
            sSeen = now;
            return;
        }
        for (String p : now) {
            if (!sSeen.contains(p)) broadcastMedia(Intent.ACTION_MEDIA_MOUNTED, p);
        }
        for (String p : sSeen) {
            if (!now.contains(p)) broadcastMedia(Intent.ACTION_MEDIA_UNMOUNTED, p);
        }
        sSeen = now;
    }

    private static void broadcastMedia(String action, String path) {
        Intent i = new Intent(action, Uri.fromFile(new File(path)));
        HostBootstrap.deliverBroadcast(i);
    }
}
