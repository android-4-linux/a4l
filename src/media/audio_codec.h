// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design; the buffer model of android.media.MediaCodec
// as the SDK documents it (input and output buffers by index, BufferInfo,
// INFO_OUTPUT_FORMAT_CHANGED, BUFFER_FLAG_END_OF_STREAM). No GPL-3.0 sources.
//
// android.media.MediaCodec's byte-buffer model for an audio decoder, with no
// JNI in it: a fixed set of input buffers the client fills and queues, and
// output buffers of 16-bit PCM it dequeues, reads and releases. The decoder
// itself is behind three functions, so the model is tested without one.
//
// Memory is bounded by the buffers: an input buffer stays in flight until
// its access unit has been decoded, and decoding stops when every output
// buffer is held. A client that queues without dequeuing waits; it does not
// grow a queue.
#ifndef ACL_MEDIA_AUDIO_CODEC_H
#define ACL_MEDIA_AUDIO_CODEC_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/audio_decoder.h"

namespace acl::media {

class AudioCodec {
 public:
  // The decoder, as AudioDecoder offers it.
  struct Decoder {
    // 0 taken, 1 read output first, -1 error. data == nullptr: end of stream.
    std::function<
        int(const std::uint8_t* data, std::size_t size, std::int64_t pts_us)>
        send;
    std::function<AudioDecoder::Result(AudioDecoder::Frame* out)> receive;
    std::function<void()> flush;
  };
  struct Limits {
    int input_buffers = 4;
    std::size_t input_bytes = 256 * 1024;
    int output_buffers = 8;
  };

  // A codec over a real AudioDecoder. nullptr + *error if it cannot be made.
  static std::unique_ptr<AudioCodec> create(const AudioDecoder::Config& config,
                                            std::string* error);
  // A codec over the given decoder functions.
  AudioCodec(Decoder decoder, const Limits& limits);

  // ── input ──
  int input_buffers() const noexcept { return limits_.input_buffers; }
  std::size_t input_bytes() const noexcept { return limits_.input_bytes; }
  // The memory of input buffer `index`, or nullptr.
  std::uint8_t* input_data(int index) noexcept;
  // An input buffer the client may fill, or -1 if all are in flight.
  int dequeue_input();
  // Queue `size` bytes at `offset` of buffer `index`, or (end_of_stream) the
  // end of the stream, with or without a last access unit. False if the
  // index is not one the client holds or the range is outside the buffer.
  bool queue_input(int index,
                   int offset,
                   int size,
                   std::int64_t pts_us,
                   bool end_of_stream);

  // ── output ──
  struct Output {
    enum class Kind { TryAgain, FormatChanged, Buffer };
    Kind kind = Kind::TryAgain;
    int index = -1;
    int bytes = 0;  // of 16-bit PCM in the buffer
    std::int64_t pts_us = 0;
    bool end_of_stream = false;  // the last buffer; `bytes` is 0
  };
  // Decode until there is something to hand over. TryAgain: nothing now
  // (more input is needed, or every output buffer is held).
  Output dequeue_output();
  // The PCM of a dequeued buffer; nullptr if `index` is not held.
  const std::int16_t* output_data(int index, std::size_t* bytes) const;
  void release_output(int index);
  // The format of what dequeue_output() last announced.
  int sample_rate() const;
  int channels() const;

  // Drop everything in flight (a seek). Input buffers return to the client's
  // reach, held output buffers are invalid, and the stream may start again.
  void flush();

  // Wait up to `ms` for a queue_input(), release_output() or flush(): what an
  // asynchronous driver sleeps on between passes.
  void wait_for_work(int ms);
  // Wake a wait_for_work() from outside, to stop a driver.
  void wake();

 private:
  struct Pending {
    int index;
    int offset, size;
    std::int64_t pts_us;
    bool eos;
  };
  struct Slot {
    std::vector<std::int16_t> pcm;
    bool held = false;
  };
  int free_slot_locked() const;

  Decoder decoder_;
  const Limits limits_;
  mutable std::mutex mu_;
  std::condition_variable work_;
  bool work_flag_ = false;
  std::vector<std::vector<std::uint8_t>> inputs_;
  std::vector<bool> input_held_;    // by the client, being filled
  std::vector<bool> input_queued_;  // queued, not yet decoded
  std::deque<Pending> pending_;
  std::vector<Slot> slots_;
  // A decoded frame not yet handed over: the format changed and the client
  // is told first, or no output buffer was free.
  AudioDecoder::Frame held_frame_;
  bool have_held_frame_ = false;
  int rate_ = 0, channels_ = 0;   // announced
  bool queued_eos_ = false;       // the client has queued end of stream
  bool sent_eos_ = false;         // and it has been given to the decoder
  bool decoder_drained_ = false;  // and the decoder has said so
  bool delivered_eos_ = false;    // and the client has the last buffer
};

}  // namespace acl::media

#endif  // ACL_MEDIA_AUDIO_CODEC_H
