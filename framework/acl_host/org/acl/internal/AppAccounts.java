// SPDX-License-Identifier: Apache-2.0
// Provenance: original; the method names and arguments are those of AOSP's
// android.accounts.IAccountManager (Apache-2.0), which android.accounts.AccountManager calls.
// No GPL-3.0 sources.
package org.acl.internal;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.AuthenticatorDescription;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The app's own accounts: what AccountManager keeps for it on Android. There is one app in the
 * process and no account service, so an account added here is seen by this app alone.
 *
 * <p>An app that signs in through its own authenticator keeps the result as an account:
 * addAccountExplicitly with a password and user data, getAccountsByType at start, and
 * removeAccountExplicitly at sign-out. Those, the password, user data and auth tokens work. The
 * calls that would run an authenticator (addAccount, getAuthToken, confirmCredentials and the
 * like) report "unsupported" to their response, so a caller's future ends and does not wait
 * forever.
 *
 * <p>The accounts are kept in a4l_accounts.json in the app's data directory, readable by the
 * owner alone: it holds the passwords and tokens as the app gave them, as Android's accounts
 * database does.
 */
final class AppAccounts {
    private static final String FILE = "a4l_accounts.json";

    private static final class Entry {
        final Account account;
        String password;
        final Map<String, String> userData = new HashMap<>();
        final Map<String, String> tokens = new HashMap<>();

        Entry(Account account) {
            this.account = account;
        }
    }

    private final Context mContext;
    private final File mFile;
    private final Map<Account, Entry> mEntries = new LinkedHashMap<>();

    private AppAccounts(Context context, File dataDir) {
        mContext = context;
        mFile = new File(dataDir, FILE);
        load();
    }

    /** An AccountManager over this app's accounts, or null if it cannot be made. */
    static AccountManager manager(Context context, File dataDir) {
        try {
            final AppAccounts store = new AppAccounts(context, dataDir);
            final Class<?> iam = Class.forName("android.accounts.IAccountManager");
            final Object service = java.lang.reflect.Proxy.newProxyInstance(iam.getClassLoader(),
                    new Class<?>[]{iam}, (proxy, m, args) -> store.call(m, args));
            final java.lang.reflect.Constructor<AccountManager> c =
                    AccountManager.class.getConstructor(Context.class, iam);
            return c.newInstance(context, service);
        } catch (Throwable t) {
            android.util.Log.w("acl", "AccountManager: " + t);
            return null;
        }
    }

    private synchronized Object call(Method m, Object[] a) throws Exception {
        final Class<?> rt = m.getReturnType();
        switch (m.getName()) {
            case "getAccountsAsUser":
            case "getAccountsByTypeForPackage":
                return accounts((String) a[0]);
            case "getAccountsForPackage":
                return accounts(null);
            case "getAccountsAndVisibilityForPackage": {
                final Map<Account, Integer> out = new LinkedHashMap<>();
                for (Account acct : accounts((String) a[1])) {
                    out.put(acct, AccountManager.VISIBILITY_VISIBLE);
                }
                return out;
            }
            case "getPackagesAndVisibilityForAccount":
                return new HashMap<String, Integer>();
            case "addAccountExplicitly":
            case "addAccountExplicitlyWithVisibility":
                return add((Account) a[0], (String) a[1], (Bundle) a[2]);
            case "removeAccountExplicitly":
                return remove((Account) a[0]);
            case "removeAccountAsUser": {
                final Bundle result = new Bundle();
                result.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, remove((Account) a[1]));
                respond(a[0], result);
                return null;
            }
            case "getPassword": {
                final Entry e = mEntries.get(a[0]);
                return e != null ? e.password : null;
            }
            case "setPassword":
            case "clearPassword": {
                final Entry e = mEntries.get(a[0]);
                if (e != null) {
                    e.password = a.length > 1 ? (String) a[1] : null;
                    // Android drops an account's tokens when its password changes.
                    e.tokens.clear();
                    save();
                }
                return null;
            }
            case "getUserData": {
                final Entry e = mEntries.get(a[0]);
                return e != null ? e.userData.get((String) a[1]) : null;
            }
            case "setUserData": {
                final Entry e = mEntries.get(a[0]);
                if (e != null && a[1] != null) {
                    put(e.userData, (String) a[1], (String) a[2]);
                    save();
                }
                return null;
            }
            case "peekAuthToken": {
                final Entry e = mEntries.get(a[0]);
                return e != null ? e.tokens.get((String) a[1]) : null;
            }
            case "setAuthToken": {
                final Entry e = mEntries.get(a[0]);
                if (e != null && a[1] != null) {
                    put(e.tokens, (String) a[1], (String) a[2]);
                    save();
                }
                return null;
            }
            case "invalidateAuthToken": {
                // (accountType, authToken): the token goes from every account of the type.
                boolean changed = false;
                for (Entry e : mEntries.values()) {
                    if (a[1] != null && e.account.type.equals(a[0])) {
                        changed |= e.tokens.values().remove((String) a[1]);
                    }
                }
                if (changed) save();
                return null;
            }
            case "accountAuthenticated":
            case "someUserHasAccount":
            case "hasAccountAccess":
                return mEntries.containsKey(a[0]);
            case "getAccountVisibility":
                return mEntries.containsKey(a[0]) ? AccountManager.VISIBILITY_VISIBLE
                        : AccountManager.VISIBILITY_NOT_VISIBLE;
            case "setAccountVisibility":
                return mEntries.containsKey(a[0]);
            case "getAuthenticatorTypes":
                return new AuthenticatorDescription[0];
            default:
                break;
        }
        // The rest that take a response would run an authenticator.
        if (a != null && a.length > 0 && a[0] != null
                && m.getParameterTypes()[0].getName()
                        .equals("android.accounts.IAccountManagerResponse")) {
            a[0].getClass().getMethod("onError", int.class, String.class).invoke(a[0],
                    AccountManager.ERROR_CODE_UNSUPPORTED_OPERATION,
                    m.getName() + " is not supported");
            return null;
        }
        if (rt == boolean.class) return Boolean.FALSE;
        if (rt == int.class) return 0;
        return null;
    }

    private Account[] accounts(String type) {
        final List<Account> out = new ArrayList<>();
        for (Account acct : mEntries.keySet()) {
            if (type == null || type.equals(acct.type)) out.add(acct);
        }
        return out.toArray(new Account[0]);
    }

    private boolean add(Account account, String password, Bundle extras) {
        if (account == null || mEntries.containsKey(account)) return false;
        final Entry e = new Entry(account);
        e.password = password;
        if (extras != null) {
            for (String key : extras.keySet()) {
                // AccountManager documents the values as strings.
                final String value = extras.getString(key);
                if (value != null) e.userData.put(key, value);
            }
        }
        mEntries.put(account, e);
        save();
        return true;
    }

    private boolean remove(Account account) {
        if (account == null || mEntries.remove(account) == null) return false;
        save();
        // What a receiver of android.accounts.action.ACCOUNT_REMOVED in the app is sent.
        try {
            mContext.sendBroadcast(new Intent(AccountManager.ACTION_ACCOUNT_REMOVED)
                    .setPackage(mContext.getPackageName())
                    .putExtra(AccountManager.KEY_ACCOUNT_NAME, account.name)
                    .putExtra(AccountManager.KEY_ACCOUNT_TYPE, account.type));
        } catch (RuntimeException ex) {
            android.util.Log.w("acl", "ACCOUNT_REMOVED not sent: " + ex);
        }
        return true;
    }

    private static void put(Map<String, String> map, String key, String value) {
        if (value == null) map.remove(key);
        else map.put(key, value);
    }

    private static void respond(Object response, Bundle result) throws Exception {
        if (response == null) return;
        response.getClass().getMethod("onResult", Bundle.class).invoke(response, result);
    }

    private void load() {
        if (!mFile.isFile()) return;
        try {
            final JSONArray all = new JSONArray(new String(Files.readAllBytes(mFile.toPath()),
                    StandardCharsets.UTF_8));
            for (int i = 0; i < all.length(); i++) {
                final JSONObject o = all.getJSONObject(i);
                final Entry e = new Entry(new Account(o.getString("name"), o.getString("type")));
                e.password = o.isNull("password") ? null : o.getString("password");
                read(o.optJSONObject("userData"), e.userData);
                read(o.optJSONObject("tokens"), e.tokens);
                mEntries.put(e.account, e);
            }
        } catch (Exception ex) {
            // A file that does not parse is the same as no accounts: the app signs in again.
            mEntries.clear();
            android.util.Log.w("acl", "accounts not read from " + mFile + ": " + ex);
        }
    }

    private static void read(JSONObject from, Map<String, String> into) throws Exception {
        if (from == null) return;
        for (java.util.Iterator<String> it = from.keys(); it.hasNext(); ) {
            final String key = it.next();
            into.put(key, from.getString(key));
        }
    }

    // Written beside the file and renamed over it. The temporary file is made readable by the
    // owner alone before anything is written to it.
    private void save() {
        File tmp = null;
        try {
            final JSONArray all = new JSONArray();
            for (Entry e : mEntries.values()) {
                final JSONObject o = new JSONObject();
                o.put("name", e.account.name);
                o.put("type", e.account.type);
                o.put("password", e.password != null ? e.password : JSONObject.NULL);
                o.put("userData", new JSONObject(e.userData));
                o.put("tokens", new JSONObject(e.tokens));
                all.put(o);
            }
            tmp = File.createTempFile(".a4l_accounts", ".tmp", mFile.getParentFile());
            tmp.setReadable(false, false);
            tmp.setReadable(true, true);
            tmp.setWritable(false, false);
            tmp.setWritable(true, true);
            try (FileOutputStream os = new FileOutputStream(tmp)) {
                os.write(all.toString().getBytes(StandardCharsets.UTF_8));
            }
            Files.move(tmp.toPath(), mFile.toPath(), StandardCopyOption.REPLACE_EXISTING,
                    StandardCopyOption.ATOMIC_MOVE);
            tmp = null;
        } catch (Exception ex) {
            android.util.Log.w("acl", "accounts not saved to " + mFile + ": " + ex);
        } finally {
            if (tmp != null) tmp.delete();
        }
    }
}
