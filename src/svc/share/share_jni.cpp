// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.HostShare to xdg-desktop-portal's
// org.freedesktop.portal.OpenURI.OpenURI + org.freedesktop.portal.Email.ComposeEmail (session bus).
// Freedesktop portal spec. No GPL-3.0 sources.
#include "svc/share/share_jni.h"

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Ask the host's desktop portal to open a pre-filled email in the user's default mail client, the
// sandbox-correct stand-in for an Android email-app ACTION_SENDTO (mailto:). Apps' crash reporters
// (ACRA) and "email us" flows fire such intents; on acl there is no email Activity, so route them to
// org.freedesktop.portal.Email.ComposeEmail. Only non-empty fields are passed as options.
// `attachments` are adopted fds (ownership transferred by the caller) forwarded as the portal's
// attachment_fds; the portal dups them into the D-Bus message, so they are closed when the vector
// dies at return — the report file an attachment-based reporter (ACRA) ships thus reaches the mail.
bool compose_email(const std::string& address, const std::string& subject, const std::string& body,
                   std::vector<sdbus::UnixFd>&& attachments) {
  try {
    auto conn = sdbus::createSessionBusConnection();
    auto proxy = sdbus::createProxy(*conn, sdbus::ServiceName{"org.freedesktop.portal.Desktop"},
                                    sdbus::ObjectPath{"/org/freedesktop/portal/desktop"});
    // ComposeEmail(s parent_window, a{sv} options) -> o handle. Keys per the Email portal spec.
    std::map<std::string, sdbus::Variant> options;
    if (!address.empty()) options.emplace("address", sdbus::Variant{address});
    if (!subject.empty()) options.emplace("subject", sdbus::Variant{subject});
    if (!body.empty()) options.emplace("body", sdbus::Variant{body});
    const std::size_t n_att = attachments.size();
    if (n_att > 0) options.emplace("attachment_fds", sdbus::Variant{attachments});
    sdbus::ObjectPath handle;
    proxy->callMethod("ComposeEmail")
        .onInterface(sdbus::InterfaceName{"org.freedesktop.portal.Email"})
        .withArguments(std::string{} /*parent_window*/, options)
        .storeResultsTo(handle);
    log::i(kTag, "ComposeEmail -> host portal (to={}, subject={}B, body={}B, {} attachment(s))",
           address, subject.size(), body.size(), n_att);
    return true;
  } catch (const sdbus::Error& e) {
    log::w(kTag, "ComposeEmail failed ({}); email not composed", e.getMessage());
    return false;
  } catch (...) {
    log::w(kTag, "ComposeEmail failed (unknown); email not composed");
    return false;
  }
}

jboolean JNICALL n_compose_email(JNIEnv* env, jclass, jstring jaddr, jstring jsubj, jstring jbody,
                                 jintArray jfds) {
  auto str = [env](jstring j) -> std::string {
    if (j == nullptr) return {};
    const char* c = env->GetStringUTFChars(j, nullptr);
    std::string s(c != nullptr ? c : "");
    if (c != nullptr) env->ReleaseStringUTFChars(j, c);
    return s;
  };
  // The caller detached ownership of each fd (ParcelFileDescriptor.detachFd) — adopt them so each
  // UnixFd closes it after the portal call dups it into the message. adopt_fd (not the dup ctor):
  // avoids leaking the originals.
  std::vector<sdbus::UnixFd> fds;
  if (jfds != nullptr) {
    const jsize n = env->GetArrayLength(jfds);
    if (jint* els = env->GetIntArrayElements(jfds, nullptr); els != nullptr) {
      fds.reserve(static_cast<std::size_t>(n));
      for (jsize i = 0; i < n; ++i)
        if (els[i] >= 0) fds.emplace_back(els[i], sdbus::adopt_fd);
      env->ReleaseIntArrayElements(jfds, els, JNI_ABORT);
    }
  }
  return compose_email(str(jaddr), str(jsubj), str(jbody), std::move(fds)) ? JNI_TRUE : JNI_FALSE;
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
      {const_cast<char*>("nComposeEmail"),
       const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[I)Z"),
       reinterpret_cast<void*>(&n_compose_email)},
  };
  if (env->RegisterNatives(cls, m, 2) != JNI_OK) {
    env->ExceptionClear();
    log::w(kTag, "RegisterNatives(HostShare) failed");
    return;
  }
  env->DeleteLocalRef(cls);
  log::i(kTag, "HostShare.nOpenUri + nComposeEmail bound (xdg-desktop-portal OpenURI/Email)");
}

}  // namespace acl::share
