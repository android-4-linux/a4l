// SPDX-License-Identifier: Apache-2.0
// Provenance: PipeWire pw_registry / pw_metadata / SPA_PARAM_Props (channelVolumes/mute)
// public API + examples (MIT); default-sink-tracking pattern after jwinarske/pw_dart
// (Apache-2.0). Maps android.media.AudioManager master-volume onto the host default sink.
// No GPL-3.0 sources.
//
// PwVolume follows the current default audio sink and reads/writes its volume. The "master
// output" is not a fixed device: it is whichever node the "default" metadata names as
// default.audio.sink, which changes at runtime on desktop Linux (output switch, headphones
// plugged in, monitor swap, device removed → WirePlumber re-picks). We track that metadata
// key, resolve it to a live Audio/Sink node, bind it, and re-target when it changes.
#ifndef ACL_AUDIO_PW_VOLUME_H
#define ACL_AUDIO_PW_VOLUME_H

#include <memory>

namespace acl::audio {

// The master-volume controller over the host default sink. Synchronous getters answer off a
// cached snapshot kept live by the PipeWire thread loop; setters push to the bound sink node.
class IPwVolume {
 public:
  virtual ~IPwVolume() = default;

  // A default sink is currently bound (a master output exists to control).
  virtual bool available() const = 0;

  // Master volume as a linear amplitude in [0,1] (the SPA channelVolumes convention, what
  // wpctl/pavucontrol show as the raw value), and mute.
  virtual float volume() const = 0;
  virtual bool muted() const = 0;

  // Set the bound default sink's volume/mute. No-op (returns false) when nothing is bound.
  virtual bool set_volume(float linear) = 0;
  virtual bool set_muted(bool mute) = 0;

  // The current default sink's channel count (for building the channelVolumes array).
  virtual int channels() const = 0;
};

// Connect to PipeWire and start tracking the default sink. Returns a Null controller
// (available()==false) when PipeWire is absent or the connection fails. Never null.
std::unique_ptr<IPwVolume> make_pw_volume();

}  // namespace acl::audio

#endif  // ACL_AUDIO_PW_VOLUME_H
