// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; Android SDK docs (PackageManager.GET_SIGNATURES,
// GET_SIGNING_CERTIFICATES, hasSigningCertificate, SigningInfo). No GPL-3.0 sources.
//
// What an app asks about the certificate it is signed with. It sends the certificate's
// digest to a server to say what it is (Firebase and Google's APIs do), and asks
// hasSigningCertificate whether it is the build its publisher signed (Spotify, in its first
// Activity). The fixture asks each way and logs one line; the test knows the certificate the
// APK was signed with.
package com.acl.signing;

import android.app.Activity;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.Bundle;
import android.util.Log;
import android.view.View;

import java.security.MessageDigest;

public class Main extends Activity {
    static void log(String s) {
        Log.i("acl", "SIGNING: " + s);
    }

    private static String hex(byte[] b) {
        final StringBuilder sb = new StringBuilder();
        for (byte x : b) sb.append(String.format("%02x", x));
        return sb.toString();
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        setContentView(new View(this));
        try {
            log(ask());
        } catch (Throwable t) {
            log("FAILED " + t);
        }
        log("done");
        System.exit(0);
    }

    private String ask() throws Exception {
        final PackageManager pm = getPackageManager();
        final String pkg = getPackageName();
        final PackageInfo plain = pm.getPackageInfo(pkg, 0);
        final PackageInfo old = pm.getPackageInfo(pkg, PackageManager.GET_SIGNATURES);
        final PackageInfo all = pm.getPackageInfo(pkg, PackageManager.GET_SIGNING_CERTIFICATES);
        final PackageInfo flags33 = pm.getPackageInfo(pkg,
                PackageManager.PackageInfoFlags.of(PackageManager.GET_SIGNING_CERTIFICATES));
        final Signature[] sigs = old.signatures;
        final boolean unasked = plain.signatures == null && plain.signingInfo == null;
        final byte[] wrong = new byte[32];
        if (sigs == null || sigs.length == 0) {
            return "signatures=0 signingInfo=" + (all.signingInfo != null) + " has="
                    + pm.hasSigningCertificate(pkg, wrong, PackageManager.CERT_INPUT_SHA256)
                    + " unasked=" + unasked;
        }
        final byte[] raw = sigs[0].toByteArray();
        final byte[] sha256 = MessageDigest.getInstance("SHA-256").digest(raw);
        final Signature[] signers =
                all.signingInfo != null ? all.signingInfo.getApkContentsSigners() : null;
        return "signatures=" + sigs.length + " sha256=" + hex(sha256)
                + " signingInfo=" + (signers != null && signers.length == sigs.length
                        && signers[0].equals(sigs[0]))
                + " flags33=" + (flags33.signingInfo != null)
                + " has=" + pm.hasSigningCertificate(pkg, sha256, PackageManager.CERT_INPUT_SHA256)
                + "/" + pm.hasSigningCertificate(pkg, raw, PackageManager.CERT_INPUT_RAW_X509)
                + "/" + pm.hasSigningCertificate(android.os.Process.myUid(), sha256,
                        PackageManager.CERT_INPUT_SHA256)
                + " wrong=" + pm.hasSigningCertificate(pkg, wrong,
                        PackageManager.CERT_INPUT_SHA256)
                + " other=" + pm.hasSigningCertificate("com.example.other", sha256,
                        PackageManager.CERT_INPUT_SHA256)
                + " unasked=" + unasked;
    }
}
