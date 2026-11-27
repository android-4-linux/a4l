// SPDX-License-Identifier: Apache-2.0
// Provenance: libsecret C API (secret_password_* / secret_service_search,
// org.freedesktop.Secret.* docs) and GLib g_base64_* (GLib docs).
// AndroidKeyStore identity model from AOSP android.jar. No GPL-3.0 sources.
#include "svc/keystore/keystore_backend.h"

#include "acl_config.h"
#include "log/log.h"

#include <cstdlib>
#include <map>
#include <mutex>

#if ACL_HAVE_SECRET
#include <libsecret/secret.h>
#endif

namespace acl::keystore {
namespace {
constexpr const char* kTag = "acl.svc.keystore";

// In-process fallback: a mutex-guarded map. Non-persistent — keys vanish on
// exit. Used when the Secret Service is unreachable (no session bus / no
// gnome-keyring) or ACL_KEYSTORE_BACKEND=memory.
class MemoryBackend final : public IKeystoreBackend {
 public:
  const char* name() const noexcept override { return "memory"; }
  bool store(const std::string& pkg,
             const std::string& alias,
             const std::vector<std::uint8_t>& blob) override {
    std::lock_guard<std::mutex> lk(mu_);
    map_[key(pkg, alias)] = blob;
    return true;
  }
  bool load(const std::string& pkg,
            const std::string& alias,
            std::vector<std::uint8_t>& out) override {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(key(pkg, alias));
    if (it == map_.end()) {
      out.clear();
      return false;
    }
    out = it->second;
    return true;
  }
  bool contains(const std::string& pkg, const std::string& alias) override {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.count(key(pkg, alias)) != 0;
  }
  bool erase(const std::string& pkg, const std::string& alias) override {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.erase(key(pkg, alias)) != 0;
  }
  std::vector<std::string> aliases(const std::string& pkg) override {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    const std::string prefix = pkg + '\x1f';
    for (const auto& [k, _] : map_)
      if (k.rfind(prefix, 0) == 0)
        out.push_back(k.substr(prefix.size()));
    return out;
  }

 private:
  static std::string key(const std::string& pkg, const std::string& alias) {
    return pkg + '\x1f' +
           alias;  // unit separator can't appear in a package name or alias
  }
  std::mutex mu_;
  std::map<std::string, std::vector<std::uint8_t>> map_;
};

#if ACL_HAVE_SECRET
// The Secret Service schema for acl keystore entries. The secret value is the
// base64 of the Java provider's opaque key blob; (package, alias) are the
// lookup attributes.
const SecretSchema* acl_schema() {
  static const SecretSchema s = {
      "org.acl.keystore",
      SECRET_SCHEMA_NONE,
      {
          {"package", SECRET_SCHEMA_ATTRIBUTE_STRING},
          {"alias", SECRET_SCHEMA_ATTRIBUTE_STRING},
          {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
      },
      // reserved fields
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
  };
  return &s;
}

// Persist key material in the login keyring. store/load/erase use the simple
// password API; aliases uses a SecretService search because the simple API
// can't enumerate.
class SecretBackend final : public IKeystoreBackend {
 public:
  const char* name() const noexcept override { return "secret-service"; }

  bool store(const std::string& pkg,
             const std::string& alias,
             const std::vector<std::uint8_t>& blob) override {
    gchar* b64 = g_base64_encode(blob.data(), blob.size());
    const std::string label = "acl keystore: " + pkg + '/' + alias;
    GError* err = nullptr;
    gboolean ok = secret_password_store_sync(
        acl_schema(), SECRET_COLLECTION_DEFAULT, label.c_str(), b64, nullptr,
        &err, "package", pkg.c_str(), "alias", alias.c_str(), nullptr);
    g_free(b64);
    if (!ok) {
      log_err("store", pkg, alias, err);
      return false;
    }
    return true;
  }

  bool load(const std::string& pkg,
            const std::string& alias,
            std::vector<std::uint8_t>& out) override {
    out.clear();
    GError* err = nullptr;
    gchar* b64 = secret_password_lookup_sync(acl_schema(), nullptr, &err,
                                             "package", pkg.c_str(), "alias",
                                             alias.c_str(), nullptr);
    if (err != nullptr) {
      log_err("load", pkg, alias, err);
      return false;
    }
    if (b64 == nullptr)
      return false;  // absent
    gsize n = 0;
    guchar* raw = g_base64_decode(b64, &n);
    out.assign(raw, raw + n);
    g_free(raw);
    secret_password_free(b64);
    return true;
  }

  bool contains(const std::string& pkg, const std::string& alias) override {
    GError* err = nullptr;
    gchar* b64 = secret_password_lookup_sync(acl_schema(), nullptr, &err,
                                             "package", pkg.c_str(), "alias",
                                             alias.c_str(), nullptr);
    if (err != nullptr) {
      log_err("contains", pkg, alias, err);
      return false;
    }
    const bool present = b64 != nullptr;
    if (b64 != nullptr)
      secret_password_free(b64);
    return present;
  }

  bool erase(const std::string& pkg, const std::string& alias) override {
    GError* err = nullptr;
    gboolean removed = secret_password_clear_sync(
        acl_schema(), nullptr, &err, "package", pkg.c_str(), "alias",
        alias.c_str(), nullptr);
    if (err != nullptr) {
      log_err("erase", pkg, alias, err);
      return false;
    }
    return removed;
  }

  std::vector<std::string> aliases(const std::string& pkg) override {
    std::vector<std::string> out;
    GError* err = nullptr;
    SecretService* svc =
        secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &err);
    if (svc == nullptr) {
      log_err("aliases/service", pkg, "", err);
      return out;
    }
    GHashTable* attrs = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(attrs, const_cast<char*>("package"),
                        const_cast<char*>(pkg.c_str()));
    GList* items = secret_service_search_sync(
        svc, acl_schema(), attrs,
        static_cast<SecretSearchFlags>(SECRET_SEARCH_ALL), nullptr, &err);
    g_hash_table_destroy(attrs);
    if (err != nullptr) {
      log_err("aliases/search", pkg, "", err);
      g_object_unref(svc);
      return out;
    }
    for (GList* l = items; l != nullptr; l = l->next) {
      auto* item = static_cast<SecretItem*>(l->data);
      GHashTable* a = secret_item_get_attributes(item);
      if (a != nullptr) {
        if (const char* al =
                static_cast<const char*>(g_hash_table_lookup(a, "alias")))
          out.emplace_back(al);
        g_hash_table_unref(a);
      }
    }
    g_list_free_full(items, g_object_unref);
    g_object_unref(svc);
    return out;
  }

 private:
  static void log_err(const char* op,
                      const std::string& pkg,
                      const std::string& alias,
                      GError*& err) {
    log::w(kTag, "{} {}/{} failed: {}", op, pkg, alias,
           err != nullptr && err->message != nullptr ? err->message : "?");
    if (err != nullptr) {
      g_error_free(err);
      err = nullptr;
    }
  }
};

// Probe: can we reach the Secret Service (a session bus + a secret backend),
// and has it a default keyring to store in? If so, use it; otherwise fall back
// to memory. Done once here so
// make_* never blocks on a dead bus twice.
bool secret_service_reachable() {
  GError* err = nullptr;
  SecretService* svc =
      secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &err);
  if (svc == nullptr) {
    log::w(kTag, "Secret Service unreachable ({}); using in-memory keystore",
           err != nullptr && err->message != nullptr ? err->message : "?");
    if (err != nullptr)
      g_error_free(err);
    return false;
  }
  // A keyring daemon with no default keyring takes the connection and then
  // refuses every store: gnome-keyring started outside a desktop login has only
  // its session collection. Keys would be lost one failed store at a time, so
  // say it once and keep them in memory.
  SecretCollection* def = secret_collection_for_alias_sync(
      svc, SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE, nullptr, &err);
  if (def == nullptr) {
    g_object_unref(svc);
    log::w(
        kTag,
        "the Secret Service has no default keyring ({}); using in-memory "
        "keystore. On a system with no desktop login, create and unlock "
        "one with: printf %s \"$PASSWORD\" | gnome-keyring-daemon --replace "
        "--unlock --components=secrets",
        err != nullptr && err->message != nullptr ? err->message : "none set");
    if (err != nullptr)
      g_error_free(err);
    return false;
  }
  // A locked keyring refuses a store too. Ask for it to be unlocked: a desktop
  // prompts for the password, here instead of at the app's first key. Where
  // nothing can prompt the keyring stays locked.
  bool locked = secret_collection_get_locked(def) != FALSE;
  if (locked) {
    GList* objects = g_list_append(nullptr, def);
    GList* unlocked = nullptr;
    secret_service_unlock_sync(svc, objects, nullptr, &unlocked, nullptr);
    locked = unlocked == nullptr;
    g_list_free_full(unlocked, g_object_unref);
    g_list_free(objects);
  }
  g_object_unref(def);
  g_object_unref(svc);
  if (locked) {
    log::w(kTag,
           "the default keyring is locked and was not unlocked; using "
           "in-memory keystore. Unlock it with: printf %s \"$PASSWORD\" | "
           "gnome-keyring-daemon --replace --unlock --components=secrets");
    return false;
  }
  return true;
}
#endif  // ACL_HAVE_SECRET

}  // namespace

std::unique_ptr<IKeystoreBackend> make_keystore_backend() {
  const char* force = std::getenv("ACL_KEYSTORE_BACKEND");
  const bool force_memory = force != nullptr && std::string(force) == "memory";
#if ACL_HAVE_SECRET
  if (!force_memory && secret_service_reachable()) {
    log::i(kTag, "keystore backend: secret-service (login keyring)");
    return std::make_unique<SecretBackend>();
  }
#else
  (void)force_memory;
#endif
  log::i(kTag, "keystore backend: memory (non-persistent)");
  return std::make_unique<MemoryBackend>();
}

}  // namespace acl::keystore
