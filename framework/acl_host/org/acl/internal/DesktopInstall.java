// SPDX-License-Identifier: Apache-2.0
// Provenance: original; the Desktop Entry Specification and the Icon Theme Specification
// (freedesktop.org) for the file formats. No GPL-3.0 sources.
package org.acl.internal;

import android.content.Intent;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.List;

/**
 * Writes or removes an APK's desktop entry and icon, so the desktop has a name and an icon for
 * the app's window: tools/desktop-entry.sh runs it as the static main in place of the app. No
 * code from the APK runs; its manifest and resources are read.
 *
 * <p>Environment: ACL_DESKTOP_ACTION (install or remove), ACL_DESKTOP_APK, and for install
 * ACL_DESKTOP_EXEC, the launch command with one argument per line. ACL_ART_FRAMEWORK_RES
 * resolves a reference to an android: resource.
 *
 * <p>The entry is $XDG_DATA_HOME/applications/&lt;package&gt;.desktop and the icon is
 * $XDG_DATA_HOME/icons/hicolor/256x256/apps/&lt;package&gt;.png. The window's app id is the
 * package, which is how the desktop pairs the two. A file at the entry's path that this class
 * did not write is left alone.
 */
public final class DesktopInstall {
    private static final String NS = "http://schemas.android.com/apk/res/android";
    private static final String MARK = "X-A4L-Package=";
    private static final int ICON_EDGE = 256;
    private static final int MAX_LABEL = 128;

    private DesktopInstall() {}

    public static void main(String[] args) {
        int rc;
        try {
            rc = run();
        } catch (Throwable t) {
            System.out.println("DESKTOP: FAIL " + t);
            rc = 1;
        }
        System.out.flush();
        System.exit(rc);
    }

    private static int run() throws Exception {
        final String action = System.getenv("ACL_DESKTOP_ACTION");
        final String apk = System.getenv("ACL_DESKTOP_APK");
        if (apk == null || !new File(apk).isFile()) return fail("ACL_DESKTOP_APK names no file");

        final AssetManager am = AssetManager.class.getDeclaredConstructor().newInstance();
        final java.lang.reflect.Method add =
                AssetManager.class.getMethod("addAssetPath", String.class);
        final String fres = System.getenv("ACL_ART_FRAMEWORK_RES");
        if (fres != null && !fres.isEmpty()) add.invoke(am, fres);
        add.invoke(am, apk);
        final Resources sys = Resources.getSystem();
        final Resources res = new Resources(am, sys.getDisplayMetrics(), sys.getConfiguration());

        final App app = readManifest(am, apk, res);
        if (!validPackage(app.pkg)) return fail("the manifest's package is not a package name");

        final File data = dataHome();
        if (data == null) return fail("neither XDG_DATA_HOME nor HOME gives a data directory");
        final File entry = new File(data, "applications/" + app.pkg + ".desktop");
        final File icon = new File(data, "icons/hicolor/" + ICON_EDGE + "x" + ICON_EDGE
                + "/apps/" + app.pkg + ".png");

        if (entry.exists() && !ours(entry, app.pkg)) {
            return fail(entry + " exists and was not written by a4l");
        }
        if ("remove".equals(action)) {
            // Only what an install wrote: the icon goes with an entry that is ours.
            if (entry.exists()) {
                Files.deleteIfExists(icon.toPath());
                Files.delete(entry.toPath());
                System.out.println("DESKTOP: removed " + entry);
            } else {
                System.out.println("DESKTOP: nothing to remove for " + app.pkg);
            }
            return 0;
        }
        if (!"install".equals(action)) return fail("ACL_DESKTOP_ACTION is not install or remove");

        final String exec = execLine(System.getenv("ACL_DESKTOP_EXEC"));
        if (exec == null) return fail("ACL_DESKTOP_EXEC is empty or has a control character");

        boolean haveIcon = false;
        if (app.icon != 0) {
            try {
                writeIcon(res, app.icon, icon);
                haveIcon = true;
                System.out.println("DESKTOP: wrote " + icon);
            } catch (Throwable t) {
                // An entry with a name and no icon is still better than none.
                System.out.println("DESKTOP: no icon (" + t + ")");
            }
        }
        final StringBuilder sb = new StringBuilder();
        sb.append("[Desktop Entry]\n");
        sb.append("Type=Application\n");
        sb.append("Name=").append(value(app.label != null ? app.label : app.pkg, app.pkg))
                .append('\n');
        sb.append("Exec=").append(exec).append('\n');
        if (haveIcon) sb.append("Icon=").append(app.pkg).append('\n');
        sb.append("StartupWMClass=").append(app.pkg).append('\n');
        sb.append("Terminal=false\n");
        sb.append(MARK).append(app.pkg).append('\n');
        writeAtomically(entry, sb.toString().getBytes(StandardCharsets.UTF_8));
        System.out.println("DESKTOP: wrote " + entry);
        return 0;
    }

    private static int fail(String why) {
        System.out.println("DESKTOP: FAIL " + why);
        return 1;
    }

    private static final class App {
        String pkg;
        String label;
        int icon;
    }

    // The package, and the label and icon a launcher would show: those of the first
    // MAIN/LAUNCHER activity where it gives them, else the application's.
    private static App readManifest(AssetManager am, String apk, Resources res) throws Exception {
        final App app = new App();
        String appLabel = null, actLabel = null;
        int appIcon = 0, actIcon = 0;
        boolean inActivity = false, main = false, launcher = false, found = false;
        final XmlResourceParser xml = HostBootstrap.openBaseManifest(am, apk);
        try {
            for (int ev = xml.getEventType(); ev != XmlResourceParser.END_DOCUMENT && !found;
                 ev = xml.next()) {
                if (ev != XmlResourceParser.START_TAG) continue;
                final String tag = xml.getName();
                if ("manifest".equals(tag)) {
                    app.pkg = xml.getAttributeValue(null, "package");
                } else if ("application".equals(tag)) {
                    appLabel = label(xml, res);
                    appIcon = xml.getAttributeResourceValue(NS, "icon", 0);
                } else if ("activity".equals(tag) || "activity-alias".equals(tag)) {
                    inActivity = xml.getAttributeBooleanValue(NS, "enabled", true);
                    actLabel = label(xml, res);
                    actIcon = xml.getAttributeResourceValue(NS, "icon", 0);
                } else if ("service".equals(tag) || "receiver".equals(tag)
                        || "provider".equals(tag)) {
                    inActivity = false;
                } else if ("intent-filter".equals(tag)) {
                    main = launcher = false;
                } else if ("action".equals(tag)) {
                    main |= Intent.ACTION_MAIN.equals(xml.getAttributeValue(NS, "name"));
                } else if ("category".equals(tag)) {
                    launcher |= Intent.CATEGORY_LAUNCHER.equals(xml.getAttributeValue(NS, "name"));
                }
                found = inActivity && main && launcher;
            }
        } finally {
            xml.close();
        }
        app.label = found && actLabel != null ? actLabel : appLabel;
        app.icon = found && actIcon != 0 ? actIcon : appIcon;
        return app;
    }

    private static String label(XmlResourceParser xml, Resources res) {
        try {
            final int id = xml.getAttributeResourceValue(NS, "label", 0);
            final CharSequence cs = id != 0 ? res.getText(id) : xml.getAttributeValue(NS, "label");
            return cs != null && cs.length() > 0 ? cs.toString() : null;
        } catch (RuntimeException e) {
            return null;  // a reference that does not resolve
        }
    }

    // A Java package name: the file names are made from it, so nothing else gets through.
    private static boolean validPackage(String pkg) {
        return pkg != null && pkg.length() <= 255
                && pkg.matches("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+");
    }

    private static File dataHome() {
        final String xdg = System.getenv("XDG_DATA_HOME");
        if (xdg != null && xdg.startsWith("/")) return new File(xdg);
        final String home = System.getenv("HOME");
        return home != null && home.startsWith("/") ? new File(home, ".local/share") : null;
    }

    private static boolean ours(File entry, String pkg) throws IOException {
        if (entry.length() > 64 * 1024) return false;
        final List<String> lines = Files.readAllLines(entry.toPath(), StandardCharsets.UTF_8);
        return lines.contains(MARK + pkg);
    }

    // A desktop entry string value: one line, with a backslash escaped. The label is the
    // APK's, so a newline in it must not start a key of its own.
    private static String value(String s, String fallback) {
        final StringBuilder sb = new StringBuilder();
        for (int i = 0; i < s.length() && sb.length() < MAX_LABEL; ) {
            final int cp = s.codePointAt(i);
            i += Character.charCount(cp);
            if (Character.isISOControl(cp) || cp == 0x2028 || cp == 0x2029) sb.append(' ');
            else if (cp == '\\') sb.append("\\\\");
            else sb.appendCodePoint(cp);
        }
        final String out = sb.toString().trim();
        return out.isEmpty() ? fallback : out;
    }

    // The Exec key from one argument per line. Each argument is quoted, with the characters
    // the specification reserves inside quotes escaped; then the backslash is escaped again as
    // in any string value, and a percent sign is doubled so it is not a field code.
    private static String execLine(String args) {
        if (args == null) return null;
        final StringBuilder sb = new StringBuilder();
        for (String arg : args.split("\n")) {
            if (arg.isEmpty()) continue;
            final StringBuilder q = new StringBuilder("\"");
            for (int i = 0; i < arg.length(); i++) {
                final char c = arg.charAt(i);
                if (Character.isISOControl(c)) return null;
                if (c == '"' || c == '`' || c == '$' || c == '\\') q.append('\\');
                q.append(c);
            }
            q.append('"');
            if (sb.length() > 0) sb.append(' ');
            sb.append(q.toString().replace("\\", "\\\\").replace("%", "%%"));
        }
        return sb.length() > 0 ? sb.toString() : null;
    }

    private static void writeIcon(Resources res, int id, File out) throws IOException {
        // The densest variant the APK has; it is drawn at the icon's size either way.
        final Drawable d = res.getDrawableForDensity(id, 640, null);
        if (d == null) throw new IOException("the icon resource has no drawable");
        final Bitmap bmp = Bitmap.createBitmap(ICON_EDGE, ICON_EDGE, Bitmap.Config.ARGB_8888);
        try {
            d.setBounds(0, 0, ICON_EDGE, ICON_EDGE);
            d.draw(new Canvas(bmp));
            final java.io.ByteArrayOutputStream png = new java.io.ByteArrayOutputStream();
            if (!bmp.compress(Bitmap.CompressFormat.PNG, 100, png)) {
                throw new IOException("PNG encoding failed");
            }
            writeAtomically(out, png.toByteArray());
        } finally {
            bmp.recycle();
        }
    }

    // Written beside the target and renamed over it, so a reader never sees half a file.
    private static void writeAtomically(File out, byte[] bytes) throws IOException {
        final File dir = out.getParentFile();
        Files.createDirectories(dir.toPath());
        final File tmp = File.createTempFile(".a4l-", ".tmp", dir);
        try {
            try (FileOutputStream os = new FileOutputStream(tmp)) {
                os.write(bytes);
            }
            // createTempFile makes it private to the owner; the desktop only needs to read it.
            tmp.setReadable(true, false);
            Files.move(tmp.toPath(), out.toPath(), StandardCopyOption.REPLACE_EXISTING,
                    StandardCopyOption.ATOMIC_MOVE);
        } finally {
            Files.deleteIfExists(tmp.toPath());
        }
    }
}
