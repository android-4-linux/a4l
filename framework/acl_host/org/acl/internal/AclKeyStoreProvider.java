// SPDX-License-Identifier: Apache-2.0
// Provenance: java.security.Provider/KeyStoreSpi/KeyGeneratorSpi SPI contracts (OpenJDK/AOSP
// android.jar, Apache-2.0/GPL-CE — interface only); AndroidKeyStore behavior from AOSP
// android.security.keystore docs. Key material persisted via [[KeystoreBridge]] -> svc/keystore.
// No GPL-3.0 sources.
package org.acl.internal;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.Key;
import java.security.KeyStore;
import java.security.KeyStoreSpi;
import java.security.KeyStoreException;
import java.security.NoSuchAlgorithmException;
import java.security.Provider;
import java.security.SecureRandom;
import java.security.UnrecoverableKeyException;
import java.security.cert.Certificate;
import java.security.spec.AlgorithmParameterSpec;
import java.util.Collections;
import java.util.Date;
import java.util.Enumeration;

import javax.crypto.KeyGenerator;
import javax.crypto.KeyGeneratorSpi;
import javax.crypto.SecretKey;
import javax.crypto.spec.SecretKeySpec;

/**
 * acl's stand-in for the Android Keystore JCE provider. A device backs "AndroidKeyStore" with
 * secure hardware (keystore2/Keymaster) and non-exportable keys; acl has no such hardware, so this
 * provider generates REAL keys with the default JCA provider (conscrypt) and persists the key
 * MATERIAL via {@link KeystoreBridge} into the login keyring (svc/keystore -> Secret Service).
 *
 * <p>Honest limits: the material is readable in-process (a security downgrade from hardware), and
 * key ATTESTATION is impossible. setUserAuthenticationRequired is accepted and ignored — the
 * keyring unlock is the effective at-rest gate. This exists so apps that use AndroidKeyStore for
 * local key wrapping (Telegram's passcode, many others) run, not to reproduce hardware guarantees.
 *
 * <p>Scope so far: symmetric keys (KeyGenerator AES / HmacSHA*). Asymmetric (KeyPairGenerator +
 * self-signed cert) is a follow-up. Keys are held as ordinary JCA keys, so Cipher/Mac from the
 * default provider use them directly.
 */
public final class AclKeyStoreProvider extends Provider {
    private static final String TAG = "acl.keystore";

    public AclKeyStoreProvider() {
        super("AndroidKeyStore", 1.0, "acl AndroidKeyStore (Secret Service-backed, no hardware)");
        put("KeyStore.AndroidKeyStore", Spi.class.getName());
        // Symmetric key generation under the AndroidKeyStore provider name. JCA instantiates the SPI
        // no-arg and never tells it the requested algorithm, so each algorithm needs its own subclass.
        put("KeyGenerator.AES", AesKeyGenSpi.class.getName());
        put("KeyGenerator.HmacSHA1", HmacSha1KeyGenSpi.class.getName());
        put("KeyGenerator.HmacSHA256", HmacSha256KeyGenSpi.class.getName());
        put("KeyGenerator.HmacSHA384", HmacSha384KeyGenSpi.class.getName());
        put("KeyGenerator.HmacSHA512", HmacSha512KeyGenSpi.class.getName());
    }

    /** Register the provider once, before any app touches KeyStore.getInstance("AndroidKeyStore"). */
    public static void install() {
        try {
            if (java.security.Security.getProvider("AndroidKeyStore") == null) {
                java.security.Security.addProvider(new AclKeyStoreProvider());
                android.util.Log.i(TAG, "AndroidKeyStore provider installed (Secret Service-backed)");
            }
        } catch (Throwable t) {
            android.util.Log.w(TAG, "AndroidKeyStore provider install failed: " + t);
        }
    }

    // The app's package = the keystore namespace (acl runs one app per process).
    static String appPkg() {
        try {
            java.lang.reflect.Field f = HostBootstrap.class.getDeclaredField("sAppContext");
            f.setAccessible(true);
            Object ctx = f.get(null);
            if (ctx != null) return ((android.content.Context) ctx).getPackageName();
        } catch (Throwable ignored) {}
        return "org.acl.app";
    }

    // Blob format v1: [byte ver=1][byte kind=1 secret][UTF algo][int len][key bytes].
    static byte[] serializeSecret(SecretKey key) {
        try {
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            DataOutputStream out = new DataOutputStream(bos);
            out.writeByte(1);
            out.writeByte(1);  // secret
            out.writeUTF(key.getAlgorithm());
            byte[] enc = key.getEncoded();
            out.writeInt(enc.length);
            out.write(enc);
            out.flush();
            return bos.toByteArray();
        } catch (Exception e) {
            return null;
        }
    }
    static Key deserialize(byte[] blob) {
        try {
            DataInputStream in = new DataInputStream(new ByteArrayInputStream(blob));
            int ver = in.readByte();
            int kind = in.readByte();
            if (ver != 1 || kind != 1) return null;
            String algo = in.readUTF();
            byte[] enc = new byte[in.readInt()];
            in.readFully(enc);
            return new SecretKeySpec(enc, algo);
        } catch (Exception e) {
            return null;
        }
    }

    // --- KeyStore.AndroidKeyStore -------------------------------------------------------------
    public static final class Spi extends KeyStoreSpi {
        @Override public void engineLoad(InputStream stream, char[] password) { /* load(null) */ }
        @Override public void engineLoad(KeyStore.LoadStoreParameter param) { }
        @Override public void engineStore(OutputStream stream, char[] password) { }

        @Override public boolean engineContainsAlias(String alias) {
            return KeystoreBridge.contains(appPkg(), alias);
        }
        @Override public boolean engineIsKeyEntry(String alias) { return engineContainsAlias(alias); }
        @Override public boolean engineIsCertificateEntry(String alias) { return false; }

        @Override public Key engineGetKey(String alias, char[] password)
                throws UnrecoverableKeyException {
            byte[] blob = KeystoreBridge.load(appPkg(), alias);
            if (blob == null) return null;
            Key k = deserialize(blob);
            if (k == null) throw new UnrecoverableKeyException("acl keystore: corrupt entry " + alias);
            return k;
        }
        @Override public Certificate engineGetCertificate(String alias) { return null; }
        @Override public Certificate[] engineGetCertificateChain(String alias) { return null; }
        @Override public String engineGetCertificateAlias(Certificate cert) { return null; }
        @Override public Date engineGetCreationDate(String alias) {
            return engineContainsAlias(alias) ? new Date(0) : null;
        }

        @Override public void engineSetKeyEntry(String alias, Key key, char[] password,
                                                Certificate[] chain) throws KeyStoreException {
            if (!(key instanceof SecretKey)) {
                throw new KeyStoreException("acl keystore: only symmetric keys supported so far");
            }
            byte[] blob = serializeSecret((SecretKey) key);
            if (blob == null || !KeystoreBridge.store(appPkg(), alias, blob)) {
                throw new KeyStoreException("acl keystore: could not persist " + alias);
            }
        }
        @Override public void engineSetKeyEntry(String alias, byte[] key, Certificate[] chain)
                throws KeyStoreException {
            throw new KeyStoreException("acl keystore: raw key entries not supported");
        }
        @Override public void engineSetCertificateEntry(String alias, Certificate cert)
                throws KeyStoreException {
            throw new KeyStoreException("acl keystore: certificate entries not supported");
        }
        @Override public void engineDeleteEntry(String alias) {
            KeystoreBridge.erase(appPkg(), alias);
        }
        @Override public Enumeration<String> engineAliases() {
            return Collections.enumeration(java.util.Arrays.asList(KeystoreBridge.aliases(appPkg())));
        }
        @Override public int engineSize() { return KeystoreBridge.aliases(appPkg()).length; }
    }

    // --- KeyGenerator.{AES,HmacSHA*} under provider "AndroidKeyStore" -------------------------
    // Accepts a KeyGenParameterSpec (its keystoreAlias/keySize read reflectively so this compiles
    // without a hard dependency on android.security.keystore), generates a real key with the default
    // provider, persists it under the alias, and returns it. One public subclass per algorithm
    // because JCA instantiates the SPI no-arg and doesn't pass the requested algorithm name.
    public static final class AesKeyGenSpi extends KeyGenSpi { public AesKeyGenSpi() { setAlgorithm("AES"); } }
    public static final class HmacSha1KeyGenSpi extends KeyGenSpi { public HmacSha1KeyGenSpi() { setAlgorithm("HmacSHA1"); } }
    public static final class HmacSha256KeyGenSpi extends KeyGenSpi { public HmacSha256KeyGenSpi() { setAlgorithm("HmacSHA256"); } }
    public static final class HmacSha384KeyGenSpi extends KeyGenSpi { public HmacSha384KeyGenSpi() { setAlgorithm("HmacSHA384"); } }
    public static final class HmacSha512KeyGenSpi extends KeyGenSpi { public HmacSha512KeyGenSpi() { setAlgorithm("HmacSHA512"); } }

    public static class KeyGenSpi extends KeyGeneratorSpi {
        private String algorithm = "AES";
        private String alias;
        private int keySize = -1;
        private SecureRandom random;

        @Override protected void engineInit(SecureRandom random) {
            this.random = random;  // no alias -> generateKey() will fail like AndroidKeyStore does
        }
        @Override protected void engineInit(int keysize, SecureRandom random) {
            this.keySize = keysize; this.random = random;
        }
        @Override protected void engineInit(AlgorithmParameterSpec params, SecureRandom random) {
            this.random = random;
            if (params == null) return;
            try {
                Object a = params.getClass().getMethod("getKeystoreAlias").invoke(params);
                if (a != null) alias = a.toString();
            } catch (Throwable ignored) {}
            try {
                Object ks = params.getClass().getMethod("getKeySize").invoke(params);
                if (ks instanceof Integer && (Integer) ks > 0) keySize = (Integer) ks;
            } catch (Throwable ignored) {}
            // The purposes/auth/block-mode constraints (setUserAuthenticationRequired etc.) are
            // accepted and dropped: acl has no per-key policy engine; the keyring unlock is the gate.
        }

        void setAlgorithm(String a) { algorithm = a; }

        @Override protected SecretKey engineGenerateKey() {
            try {
                KeyGenerator kg = KeyGenerator.getInstance(algorithm);  // default provider (conscrypt)
                if (keySize > 0) {
                    if (random != null) kg.init(keySize, random); else kg.init(keySize);
                } else if (random != null) {
                    kg.init(random);
                }
                SecretKey key = kg.generateKey();
                if (alias != null) {
                    byte[] blob = serializeSecret(key);
                    if (blob != null) KeystoreBridge.store(appPkg(), alias, blob);
                }
                return key;
            } catch (NoSuchAlgorithmException e) {
                throw new IllegalStateException("acl keystore: no KeyGenerator for " + algorithm, e);
            }
        }
    }
}
