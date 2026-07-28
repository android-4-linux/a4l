// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.HostShare to xdg-desktop-portal's
// org.freedesktop.portal.OpenURI.OpenURI (session bus). Freedesktop portal spec. No GPL-3.0 sources.
#include "svc/share/share_jni.h"

#include <map>
#include <string>
#include <string_view>

#include <sdbus-c++/sdbus-c++.h>

#include "log/log.h"

namespace acl::share {
namespace {

constexpr std::string_view kTag = "svc.share";

// Ask the host's desktop portal to open a URI. The portal shows the user's default handler (a
// browser for http/https) — the sandbox-correct alternative to the (unavailable) Android chooser.
// One-shot: create a session-bus connection, call OpenURI, return. OpenURI returns immediately with
// a request handle; the actual open is the portal's job, so this does not block on the browser.
bool open_uri(const std::string& uri) {
  if (uri.empty()) return false;
  try {
    auto conn = sdbus::createSessionBusConnection();
    auto proxy = sdbus::createProxy(*conn, sdbus::ServiceName{"org.freedesktop.portal.Desktop"},
                                    sdbus::ObjectPath{"/org/freedesktop/portal/desktop"});
    // OpenURI(s parent_window, s uri, a{sv} options) -> o handle. No parent window (empty), no options.
    sdbus::ObjectPath handle;
    proxy->callMethod("OpenURI")
        .onInterface(sdbus::InterfaceName{"org.freedesktop.portal.OpenURI"})
        .withArguments(std::string{} /*parent_window*/, uri,
                       std::map<std::string, sdbus::Variant>{} /*options*/)
        .storeResultsTo(handle);
    log::i(kTag, "OpenURI -> host portal ({})", uri);
    return true;
  } catch (const sdbus::Error& e) {
    log::w(kTag, "OpenURI failed ({}); URI not opened", e.getMessage());
    return false;
  } catch (...) {
    log::w(kTag, "OpenURI failed (unknown); URI not opened");
    return false;
  }
}

jboolean JNICALL n_open_uri(JNIEnv* env, jclass, jstring juri) {
  if (juri == nullptr) return JNI_FALSE;
  const char* c = env->GetStringUTFChars(juri, nullptr);
  std::string uri(c != nullptr ? c : "");
  if (c != nullptr) env->ReleaseStringUTFChars(juri, c);
  return open_uri(uri) ? JNI_TRUE : JNI_FALSE;
}

}  // namespace

void register_share_natives(JNIEnv* env) {
  jclass cls = env->FindClass("org/acl/internal/HostShare");
  if (cls == nullptr) {
    env->ExceptionClear();
    log::w(kTag, "org.acl.internal.HostShare not on the classpath; OpenURI native not registered");
    return;
  }
  const JNINativeMethod m[] = {
      {const_cast<char*>("nOpenUri"), const_cast<char*>("(Ljava/lang/String;)Z"),
       reinterpret_cast<void*>(&n_open_uri)},
  };
  if (env->RegisterNatives(cls, m, 1) != JNI_OK) {
    env->ExceptionClear();
    log::w(kTag, "RegisterNatives(HostShare) failed");
    return;
  }
  env->DeleteLocalRef(cls);
  log::i(kTag, "HostShare.nOpenUri bound (xdg-desktop-portal OpenURI)");
}

}  // namespace acl::share
