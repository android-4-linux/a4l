// SPDX-License-Identifier: Apache-2.0
// Provenance: FFmpeg public API (libavcodec/libavutil, LGPL, dynamic link only:
// dlopen'd RTLD_LOCAL, per plan.md policy, as media/video_decoder.cpp does);
// the Android MediaFormat conventions for audio codec-specific data (csd-N) as
// documented for android.media.MediaCodec; RFC 7845 (Opus in Ogg, the OpusHead
// layout) and the Vorbis I specification (header packets) for what those csd
// buffers contain. No GPL-3.0 sources.
//
// The engine under android.media.MediaCodec for audio: encoded access units
// in, interleaved 16-bit PCM out. Software decode through FFmpeg, in the
// process that asked for it; under --split that is the confined app process,
// which is where data an app supplies should be parsed.
#ifndef ACL_MEDIA_AUDIO_DECODER_H
#define ACL_MEDIA_AUDIO_DECODER_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace acl::media {

class AudioDecoder {
 public:
  // The MIME types a4l can name a decoder for, in MediaFormat's spelling:
  // audio/mp4a-latm (AAC), audio/mpeg (MP3), audio/opus, audio/vorbis,
  // audio/flac. Whether the FFmpeg on this host has the decoder is a separate
  // question: available().
  static bool known(std::string_view mime) noexcept;
  // True if FFmpeg loads here and has a decoder for `mime`.
  static bool available(std::string_view mime);

  struct Config {
    std::string mime;
    int sample_rate = 0;  // from the MediaFormat; a hint, the stream decides
    int channels = 0;
    // The format's csd buffers in index order: csd-0, csd-1, ... What each
    // holds depends on the codec; see audio_decoder.cpp.
    std::vector<std::vector<std::uint8_t>> csd;
  };
  // nullptr + *error if FFmpeg or the decoder is missing, or the config is
  // not one it accepts.
  static std::unique_ptr<AudioDecoder> configure(const Config& config,
                                                 std::string* error);
  ~AudioDecoder();
  AudioDecoder(const AudioDecoder&) = delete;
  AudioDecoder& operator=(const AudioDecoder&) = delete;

  // Feed one access unit. data == nullptr signals end of stream. Returns 0
  // when taken, 1 when the decoder must be read from first (receive(), then
  // send again), -1 + *error. A unit the decoder cannot make sense of is
  // dropped and reported as taken: one bad frame does not end a stream.
  int send_packet(const std::uint8_t* data,
                  std::size_t size,
                  std::int64_t pts_us,
                  std::string* error);

  enum class Result { Frame, Again, EndOfStream, Error };
  struct Frame {
    std::vector<std::int16_t> pcm;  // interleaved; replaced, not appended to
    int sample_rate = 0;
    int channels = 0;
    std::int64_t pts_us = 0;
  };
  // Pull decoded audio. Frame: *out is filled. Again: feed more input.
  // EndOfStream: everything sent before the end-of-stream has come out.
  Result receive(Frame* out, std::string* error);

  // Drop what the decoder holds, for a seek. The configuration stays.
  void flush();

 private:
  AudioDecoder();
  struct Impl;
  std::unique_ptr<Impl> d_;
};

// Bounds on what one decoded frame may be, so that a stream cannot make the
// caller allocate without limit.
inline constexpr int kMaxAudioDecodeChannels = 8;
inline constexpr int kMaxAudioDecodeFrames = 192000;  // samples per channel

}  // namespace acl::media

#endif  // ACL_MEDIA_AUDIO_DECODER_H
