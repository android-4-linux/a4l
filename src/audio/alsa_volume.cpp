// SPDX-License-Identifier: Apache-2.0
// Provenance: the ALSA simple mixer API (alsa-lib, LGPL-2.1, linked
// dynamically): snd_mixer_open/attach/load, snd_mixer_selem_*. After
// pw_volume.cpp. No GPL-3.0 sources.
//
// The master volume where audio goes to ALSA: android.media.AudioManager's
// volume is the playback volume of one mixer control of the card that
// ACL_ALSA_DEVICE (default "default") plays through. The control is the one
// ACL_ALSA_MIXER names, or the first of Master, PCM, Speaker, Headphone and
// Digital that has a playback volume. A device with no such control has no
// volume to set: it reports full volume and takes no change.
//
// Volume is a linear amplitude in [0,1], as the PipeWire controller's is. A
// control that states its range in decibels is set in decibels, with 1.0 at
// the top of the range; one that does not is set as a fraction of its raw
// range.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <alsa/asoundlib.h>

#include "audio/alsa_device.h"
#include "audio/pw_volume.h"
#include "log/log.h"

namespace acl::audio {

float alsa_linear_from_db(long db_hundredths, long max_hundredths) {
  if (db_hundredths >= max_hundredths)
    return 1.0f;
  return static_cast<float>(std::pow(
      10.0, static_cast<double>(db_hundredths - max_hundredths) / 2000.0));
}

long alsa_db_from_linear(float linear,
                         long min_hundredths,
                         long max_hundredths) {
  if (!(linear > 0.0f))
    return min_hundredths;
  if (linear >= 1.0f)
    return max_hundredths;
  const long db = max_hundredths +
                  std::lround(2000.0 * std::log10(static_cast<double>(linear)));
  return std::clamp(db, min_hundredths, max_hundredths);
}

namespace {

constexpr std::string_view kTag = "acl.audio.vol";

class AlsaVolume final : public IPwVolume {
 public:
  ~AlsaVolume() override {
    if (mixer_ != nullptr)
      snd_mixer_close(mixer_);
  }

  // False when the device has no mixer or no control with a playback volume.
  bool open() {
    const std::string device = alsa_device_name();
    if (snd_mixer_open(&mixer_, 0) < 0) {
      mixer_ = nullptr;
      return false;
    }
    if (snd_mixer_attach(mixer_, device.c_str()) < 0 ||
        snd_mixer_selem_register(mixer_, nullptr, nullptr) < 0 ||
        snd_mixer_load(mixer_) < 0) {
      log::i(kTag, "alsa volume: '{}' has no mixer", device);
      return false;
    }
    const char* named = std::getenv("ACL_ALSA_MIXER");
    if (named != nullptr && named[0] != '\0') {
      elem_ = find(named);
      if (elem_ == nullptr)
        log::w(kTag, "alsa volume: no playback control '{}' on '{}'", named,
               device);
    } else {
      for (const char* name :
           {"Master", "PCM", "Speaker", "Headphone", "Digital"}) {
        elem_ = find(name);
        if (elem_ != nullptr)
          break;
      }
    }
    if (elem_ == nullptr)
      return false;
    has_db_ =
        snd_mixer_selem_get_playback_dB_range(elem_, &db_min_, &db_max_) == 0 &&
        db_max_ > db_min_;
    snd_mixer_selem_get_playback_volume_range(elem_, &raw_min_, &raw_max_);
    log::i(kTag, "alsa volume controller up (control '{}' on '{}')",
           snd_mixer_selem_get_name(elem_), device);
    return true;
  }

  bool available() const override { return elem_ != nullptr; }

  float volume() const override {
    const std::lock_guard<std::mutex> lk(mu_);
    if (elem_ == nullptr)
      return 1.0f;
    snd_mixer_handle_events(mixer_);  // another program may have moved it
    long v = 0;
    if (has_db_) {
      if (snd_mixer_selem_get_playback_dB(elem_, kChannel, &v) < 0)
        return 1.0f;
      // The bottom of the range is "off" whatever decibel figure it carries.
      return v <= db_min_ ? 0.0f : alsa_linear_from_db(v, db_max_);
    }
    if (raw_max_ <= raw_min_ ||
        snd_mixer_selem_get_playback_volume(elem_, kChannel, &v) < 0)
      return 1.0f;
    return static_cast<float>(v - raw_min_) /
           static_cast<float>(raw_max_ - raw_min_);
  }

  bool muted() const override {
    const std::lock_guard<std::mutex> lk(mu_);
    if (elem_ == nullptr || snd_mixer_selem_has_playback_switch(elem_) == 0)
      return false;
    snd_mixer_handle_events(mixer_);
    int on = 1;
    if (snd_mixer_selem_get_playback_switch(elem_, kChannel, &on) < 0)
      return false;
    return on == 0;
  }

  bool set_volume(float linear) override {
    const std::lock_guard<std::mutex> lk(mu_);
    if (elem_ == nullptr)
      return false;
    linear = std::clamp(linear, 0.0f, 1.0f);
    if (has_db_)
      return snd_mixer_selem_set_playback_dB_all(
                 elem_, alsa_db_from_linear(linear, db_min_, db_max_), 0) == 0;
    if (raw_max_ <= raw_min_)
      return false;
    const long raw =
        raw_min_ + std::lround(static_cast<double>(linear) *
                               static_cast<double>(raw_max_ - raw_min_));
    return snd_mixer_selem_set_playback_volume_all(elem_, raw) == 0;
  }

  bool set_muted(bool mute) override {
    const std::lock_guard<std::mutex> lk(mu_);
    if (elem_ == nullptr || snd_mixer_selem_has_playback_switch(elem_) == 0)
      return false;
    return snd_mixer_selem_set_playback_switch_all(elem_, mute ? 0 : 1) == 0;
  }

  int channels() const override {
    const std::lock_guard<std::mutex> lk(mu_);
    if (elem_ == nullptr)
      return 0;
    return snd_mixer_selem_is_playback_mono(elem_) != 0 ? 1 : 2;
  }

 private:
  static constexpr snd_mixer_selem_channel_id_t kChannel =
      SND_MIXER_SCHN_FRONT_LEFT;  // also the one channel of a mono control

  snd_mixer_elem_t* find(const char* name) {
    snd_mixer_selem_id_t* id = nullptr;
    snd_mixer_selem_id_alloca(&id);
    snd_mixer_selem_id_set_index(id, 0);
    snd_mixer_selem_id_set_name(id, name);
    snd_mixer_elem_t* e = snd_mixer_find_selem(mixer_, id);
    return e != nullptr && snd_mixer_selem_has_playback_volume(e) != 0
               ? e
               : nullptr;
  }

  mutable std::mutex mu_;  // alsa-lib's mixer calls are not thread-safe
  snd_mixer_t* mixer_ = nullptr;
  snd_mixer_elem_t* elem_ = nullptr;
  bool has_db_ = false;
  long db_min_ = 0;
  long db_max_ = 0;
  long raw_min_ = 0;
  long raw_max_ = 0;
};

}  // namespace

std::unique_ptr<IPwVolume> make_alsa_volume() {
  auto v = std::make_unique<AlsaVolume>();
  if (!v->open())
    log::i(kTag,
           "alsa volume: no playback volume control; the volume is fixed at "
           "full");
  return v;
}

}  // namespace acl::audio
