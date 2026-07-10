// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/svc.md (pending), Android SDK Manager APIs (AudioManager),
// ADR-3 (single-process, Binder-less service hub). No GPL-3.0 sources.
#ifndef ACLCXX_SVC_SVC_H
#define ACLCXX_SVC_SVC_H

// The in-process service hub (ADR-3): the Java-visible Manager APIs
// (android.media.AudioManager, ActivityManager, ...) are thin shims whose native
// methods call directly into these C++ services — no Binder, no SystemServer. This
// is the "shim-at-Manager-boundary" seam (ADR-2). Context.getSystemService(name)
// returns the Manager shim; the shim's natives reach the service singletons here.

#include <array>
#include <map>
#include <string>

namespace aclcxx::svc {

// android.media.AudioManager backing: per-stream volume state, in-process.
class AudioService {
 public:
  static constexpr int kStreamMusic = 3;
  static constexpr int kMaxStreams = 16;
  static constexpr int kMaxVolume = 15;

  int stream_volume(int stream) const;
  void set_stream_volume(int stream, int index);  // clamped to [0, max]; logged
  int stream_max_volume(int /*stream*/) const { return kMaxVolume; }
  bool music_active() const { return music_active_; }
  void set_music_active(bool active) { music_active_ = active; }

 private:
  std::array<int, kMaxStreams> volume_{};  // 0-initialized; defaults set in ctor
  bool music_active_ = false;

 public:
  AudioService();
};

// Backs android.content.SharedPreferences: named key/value stores, in-process.
// (A real hub persists these to per-package files; in-memory for now.)
class PreferencesService {
 public:
  void put_string(const std::string& file, const std::string& key, const std::string& value);
  const std::string* find_string(const std::string& file, const std::string& key) const;
  void put_int(const std::string& file, const std::string& key, int value);
  int get_int(const std::string& file, const std::string& key, int def_value) const;

 private:
  std::map<std::string, std::map<std::string, std::string>> strings_;
  std::map<std::string, std::map<std::string, int>> ints_;
};

// Process singletons for the hub's services.
AudioService& audio();
PreferencesService& prefs();

const char* module_name() noexcept;

}  // namespace aclcxx::svc

#endif  // ACLCXX_SVC_SVC_H
