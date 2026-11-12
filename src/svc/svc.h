// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/svc.md (pending), Android SDK Manager APIs (AudioManager),
// ADR-3 (single-process, Binder-less service hub). No GPL-3.0 sources.
#ifndef ACL_SVC_SVC_H
#define ACL_SVC_SVC_H

// The in-process service hub (ADR-3): the Java-visible Manager APIs
// (android.media.AudioManager, ActivityManager, ...) are thin shims whose
// native methods call directly into these C++ services — no Binder, no
// SystemServer. This is the "shim-at-Manager-boundary" seam (ADR-2).
// Context.getSystemService(name) returns the Manager shim; the shim's natives
// reach the service singletons here.

#include <array>
#include <cstdlib>
#include <map>
#include <string>

namespace acl::svc {

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
  std::array<int, kMaxStreams>
      volume_{};  // 0-initialized; defaults set in ctor
  bool music_active_ = false;

 public:
  AudioService();
};

// Backs android.app.ActivityManager: process/memory facts.
class ActivityService {
 public:
  int memory_class() const { return 192; }  // MB per-app heap (a typical value)
  bool low_ram_device() const { return false; }
};

// Backs android.view.WindowManager / Display: display metrics. Defaults to a
// 1080p logical display; wm::query_display() overrides it from the compositor's
// wl_output (active mode + integer scale) before the app queries
// android.view.Display.
class WindowService {
 public:
  int display_width() const { return width_; }
  int display_height() const { return height_; }
  int density_dpi() const {
    return 160 * scale_;
  }  // Android baseline 160 dpi * scale
  int rotation() const { return 0; }  // Surface.ROTATION_0
  void set_display(int width, int height, int scale) {
    // ACL_DISPLAY_SIZE="WxH" pins the logical display to a fixed size (e.g. an
    // average tablet, 1280x800) instead of the compositor's wl_output.
    // Everything downstream derives from here: DisplayMetrics/Configuration,
    // the present toplevel/buffer size, and the window insets.
    if (const char* env = std::getenv("ACL_DISPLAY_SIZE")) {
      char* end = nullptr;
      const long ew = std::strtol(env, &end, 10);
      const long eh = (end != nullptr && (*end == 'x' || *end == 'X'))
                          ? std::strtol(end + 1, nullptr, 10)
                          : 0;
      if (ew > 0 && eh > 0) {
        width_ = static_cast<int>(ew);
        height_ = static_cast<int>(eh);
        if (scale > 0)
          scale_ = scale;
        return;
      }
    }
    if (width > 0)
      width_ = width;
    if (height > 0)
      height_ = height;
    if (scale > 0)
      scale_ = scale;
  }

 private:
  int width_ = 1920;
  int height_ = 1080;
  int scale_ = 1;
};

// Backs android.media.session.MediaSessionManager: tracks active sessions. A
// MediaSession registers on construction, so it surfaces in getActiveSessions.
class MediaSessionService {
 public:
  void register_session() { ++active_; }
  void unregister_session() {
    if (active_ > 0)
      --active_;
  }
  int active_sessions() const { return active_; }

 private:
  int active_ = 0;
};

// Backs android.content.SharedPreferences: named key/value stores, in-process.
// (A real hub persists these to per-package files; in-memory for now.)
class PreferencesService {
 public:
  void put_string(const std::string& file,
                  const std::string& key,
                  const std::string& value);
  const std::string* find_string(const std::string& file,
                                 const std::string& key) const;
  void put_int(const std::string& file, const std::string& key, int value);
  int get_int(const std::string& file,
              const std::string& key,
              int def_value) const;

 private:
  std::map<std::string, std::map<std::string, std::string>> strings_;
  std::map<std::string, std::map<std::string, int>> ints_;
};

// Process singletons for the hub's services.
AudioService& audio();
ActivityService& activity();
WindowService& window();
MediaSessionService& media_sessions();
PreferencesService& prefs();

const char* module_name() noexcept;

}  // namespace acl::svc

#endif  // ACL_SVC_SVC_H
