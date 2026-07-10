// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/svc.md (pending), Android SDK Manager APIs, ADR-3, CTS. No GPL-3.0 sources.
//
// The in-process (Binder-less) service hub. Managers reached via
// Context.getSystemService() are thin Java shims whose natives call these C++
// services directly. AudioService is the first (backs android.media.AudioManager);
// ActivityManager/WindowManager/PackageManager join as the app corpus needs them.

#include "svc/svc.h"

#include <algorithm>
#include <string_view>

#include "log/log.h"

namespace aclcxx::svc {
namespace {
constexpr std::string_view kTag = "aclcxx.svc.audio";
}  // namespace

AudioService::AudioService() {
  // Sensible defaults: half volume on every stream.
  volume_.fill(kMaxVolume / 2);
}

int AudioService::stream_volume(int stream) const {
  if (stream < 0 || stream >= kMaxStreams) return 0;
  return volume_[static_cast<std::size_t>(stream)];
}

void AudioService::set_stream_volume(int stream, int index) {
  if (stream < 0 || stream >= kMaxStreams) return;
  const int clamped = std::clamp(index, 0, kMaxVolume);
  volume_[static_cast<std::size_t>(stream)] = clamped;
  log::i(kTag, "set stream {} volume -> {}", stream, clamped);
}

AudioService& audio() {
  static AudioService instance;
  return instance;
}

// --- PreferencesService --------------------------------------------------------
namespace {
constexpr std::string_view kPrefsTag = "aclcxx.svc.prefs";
}  // namespace

void PreferencesService::put_string(const std::string& file, const std::string& key,
                                    const std::string& value) {
  strings_[file][key] = value;
  log::i(kPrefsTag, "prefs[{}] {} -> {}", file, key, value);
}

const std::string* PreferencesService::find_string(const std::string& file,
                                                   const std::string& key) const {
  auto f = strings_.find(file);
  if (f == strings_.end()) return nullptr;
  auto k = f->second.find(key);
  return k == f->second.end() ? nullptr : &k->second;
}

void PreferencesService::put_int(const std::string& file, const std::string& key, int value) {
  ints_[file][key] = value;
  log::i(kPrefsTag, "prefs[{}] {} -> {}", file, key, value);
}

int PreferencesService::get_int(const std::string& file, const std::string& key,
                                int def_value) const {
  auto f = ints_.find(file);
  if (f == ints_.end()) return def_value;
  auto k = f->second.find(key);
  return k == f->second.end() ? def_value : k->second;
}

PreferencesService& prefs() {
  static PreferencesService instance;
  return instance;
}

const char* module_name() noexcept { return "svc"; }

}  // namespace aclcxx::svc
