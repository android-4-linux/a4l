// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding android.net.ConnectivityManager
// (clean-room shim) to the svc/net host connectivity backend. No GPL-3.0
// sources.
#include "svc/net/connectivity_jni.h"

#include <memory>
#include <string_view>

#include "log/log.h"
#include "svc/net/net_backend.h"

namespace acl::net {
namespace {

constexpr std::string_view kTag = "svc.net";

// The process-wide connectivity backend, created on first native registration
// and kept alive for the VM lifetime (owns the D-Bus connection + async event
// loop).
std::unique_ptr<INetBackend>& backend() {
  static std::unique_ptr<INetBackend> b;
  return b;
}

// android.net.ConnectivityManager.nGetState(): pack the current snapshot into
// one int the Java shim decodes — bit0 connected, bit1 validated, bit2 metered,
// bits3-5 transport (0 none, 1 wifi, 2 ethernet, 3 cellular).
jint JNICALL n_get_state(JNIEnv*, jclass) {
  const INetBackend* b = backend().get();
  if (b == nullptr)
    return 0;
  const AndroidNetState s = b->snapshot();
  jint v = 0;
  if (s.connected)
    v |= 0x1;
  if (s.validated)
    v |= 0x2;
  if (s.metered)
    v |= 0x4;
  int t = 0;
  switch (s.transport) {
    case AndroidNetState::Transport::kWifi:
      t = 1;
      break;
    case AndroidNetState::Transport::kEthernet:
      t = 2;
      break;
    case AndroidNetState::Transport::kCellular:
      t = 3;
      break;
    case AndroidNetState::Transport::kNone:
      t = 0;
      break;
  }
  v |= (t << 3);
  return v;
}

// android.net.wifi.WifiManager.nWifiSnapshot(): the numeric Wi-Fi slice as one
// int[], in the order the Java shim unpacks: [enabled(0/1), connected(0/1),
// rssi(dBm), linkSpeed(Mbps), frequency(MHz)].
jintArray JNICALL n_wifi_snapshot(JNIEnv* env, jclass) {
  AndroidWifiState w;
  if (const INetBackend* b = backend().get())
    w = b->wifi_snapshot();
  jint vals[5] = {w.enabled ? 1 : 0, w.connected ? 1 : 0, w.rssi_dbm,
                  w.link_speed_mbps, w.frequency_mhz};
  jintArray arr = env->NewIntArray(5);
  if (arr != nullptr)
    env->SetIntArrayRegion(arr, 0, 5, vals);
  return arr;
}

// WifiInfo.getSSID() / getBSSID() sources (empty string when not associated).
jstring JNICALL n_wifi_ssid(JNIEnv* env, jclass) {
  AndroidWifiState w;
  if (const INetBackend* b = backend().get())
    w = b->wifi_snapshot();
  return env->NewStringUTF(w.ssid.c_str());
}
jstring JNICALL n_wifi_bssid(JNIEnv* env, jclass) {
  AndroidWifiState w;
  if (const INetBackend* b = backend().get())
    w = b->wifi_snapshot();
  return env->NewStringUTF(w.bssid.c_str());
}

}  // namespace

void register_wifi_natives(JNIEnv* env) {
  if (!backend())
    backend() = make_net_backend();  // shared with connectivity

  jclass cls = env->FindClass("android/net/wifi/WifiManager");
  if (cls == nullptr) {
    env->ExceptionClear();
    log::w(kTag,
           "android.net.wifi.WifiManager not on the boot classpath; "
           "wifi natives not registered");
    return;
  }
  const JNINativeMethod m[] = {
      {const_cast<char*>("nWifiSnapshot"), const_cast<char*>("()[I"),
       reinterpret_cast<void*>(&n_wifi_snapshot)},
      {const_cast<char*>("nSsid"), const_cast<char*>("()Ljava/lang/String;"),
       reinterpret_cast<void*>(&n_wifi_ssid)},
      {const_cast<char*>("nBssid"), const_cast<char*>("()Ljava/lang/String;"),
       reinterpret_cast<void*>(&n_wifi_bssid)},
  };
  if (env->RegisterNatives(cls, m, 3) != JNI_OK) {
    env->ExceptionClear();
    log::w(kTag, "RegisterNatives(WifiManager) failed");
  } else {
    const AndroidWifiState w = backend()->wifi_snapshot();
    log::i(kTag,
           "wifi natives registered (backend: {}; enabled={} connected={} "
           "ssid=\"{}\")",
           backend()->name(), w.enabled, w.connected, w.ssid);
  }
  env->DeleteLocalRef(cls);
}

void register_connectivity_natives(JNIEnv* env) {
  if (!backend())
    backend() = make_net_backend();

  jclass cls = env->FindClass("android/net/ConnectivityManager");
  if (cls == nullptr) {
    env->ExceptionClear();
    log::w(kTag,
           "android.net.ConnectivityManager not on the boot classpath; "
           "connectivity natives not registered");
    return;
  }
  const JNINativeMethod m[] = {
      {const_cast<char*>("nGetState"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&n_get_state)},
  };
  if (env->RegisterNatives(cls, m, 1) != JNI_OK) {
    env->ExceptionClear();
    log::w(kTag, "RegisterNatives(ConnectivityManager.nGetState) failed");
  } else {
    log::i(kTag, "connectivity natives registered (backend: {})",
           backend()->name());
  }
  env->DeleteLocalRef(cls);
}

}  // namespace acl::net
