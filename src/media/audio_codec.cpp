// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design; the buffer model of android.media.MediaCodec
// as the SDK documents it. No GPL-3.0 sources.

#include "media/audio_codec.h"

#include <chrono>
#include <cstring>
#include <utility>

namespace acl::media {

std::unique_ptr<AudioCodec> AudioCodec::create(
    const AudioDecoder::Config& config,
    std::string* error) {
  std::shared_ptr<AudioDecoder> dec = AudioDecoder::configure(config, error);
  if (!dec)
    return nullptr;
  Decoder d;
  d.send = [dec](const std::uint8_t* data, std::size_t size, std::int64_t pts) {
    return dec->send_packet(data, size, pts, nullptr);
  };
  d.receive = [dec](AudioDecoder::Frame* out) {
    return dec->receive(out, nullptr);
  };
  d.flush = [dec] { dec->flush(); };
  return std::make_unique<AudioCodec>(std::move(d), Limits{});
}

AudioCodec::AudioCodec(Decoder decoder, const Limits& limits)
    : decoder_(std::move(decoder)), limits_(limits) {
  inputs_.resize(static_cast<std::size_t>(limits_.input_buffers));
  for (auto& b : inputs_)
    b.resize(limits_.input_bytes);
  input_held_.assign(inputs_.size(), false);
  input_queued_.assign(inputs_.size(), false);
  slots_.resize(static_cast<std::size_t>(limits_.output_buffers));
}

std::uint8_t* AudioCodec::input_data(int index) noexcept {
  if (index < 0 || static_cast<std::size_t>(index) >= inputs_.size())
    return nullptr;
  return inputs_[static_cast<std::size_t>(index)].data();
}

int AudioCodec::dequeue_input() {
  std::lock_guard<std::mutex> lock(mu_);
  if (queued_eos_)
    return -1;  // nothing follows the end of the stream until a flush
  for (std::size_t i = 0; i < inputs_.size(); ++i) {
    if (!input_held_[i] && !input_queued_[i]) {
      input_held_[i] = true;
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool AudioCodec::queue_input(int index,
                             int offset,
                             int size,
                             std::int64_t pts_us,
                             bool end_of_stream) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (index < 0 || static_cast<std::size_t>(index) >= inputs_.size())
      return false;
    const auto i = static_cast<std::size_t>(index);
    if (!input_held_[i])
      return false;  // not one the client was given
    if (offset < 0 || size < 0 ||
        static_cast<std::size_t>(offset) > inputs_[i].size() ||
        static_cast<std::size_t>(size) >
            inputs_[i].size() - static_cast<std::size_t>(offset))
      return false;
    input_held_[i] = false;
    input_queued_[i] = true;
    pending_.push_back({index, offset, size, pts_us, end_of_stream});
    if (end_of_stream)
      queued_eos_ = true;
    work_flag_ = true;
  }
  work_.notify_all();
  return true;
}

int AudioCodec::free_slot_locked() const {
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (!slots_[i].held)
      return static_cast<int>(i);
  }
  return -1;
}

AudioCodec::Output AudioCodec::dequeue_output() {
  std::lock_guard<std::mutex> lock(mu_);
  Output out;
  if (delivered_eos_)
    return out;
  // A decoder that asks for input and then refuses it is broken; it is asked
  // twice and then left for the next call instead of spun on.
  int refused = 0;
  for (;;) {
    // A frame in hand goes out first: after its format has been announced,
    // and when a buffer is free to carry it.
    if (have_held_frame_) {
      if (held_frame_.sample_rate != rate_ ||
          held_frame_.channels != channels_) {
        rate_ = held_frame_.sample_rate;
        channels_ = held_frame_.channels;
        out.kind = Output::Kind::FormatChanged;
        return out;
      }
      const int slot = free_slot_locked();
      if (slot < 0)
        return out;  // the client holds every output buffer
      Slot& s = slots_[static_cast<std::size_t>(slot)];
      s.pcm.swap(held_frame_.pcm);
      s.held = true;
      have_held_frame_ = false;
      out.kind = Output::Kind::Buffer;
      out.index = slot;
      out.bytes = static_cast<int>(s.pcm.size() * sizeof(std::int16_t));
      out.pts_us = held_frame_.pts_us;
      return out;
    }
    if (decoder_drained_) {
      // The last buffer: empty, flagged.
      const int slot = free_slot_locked();
      if (slot < 0)
        return out;
      Slot& s = slots_[static_cast<std::size_t>(slot)];
      s.pcm.clear();
      s.held = true;
      delivered_eos_ = true;
      out.kind = Output::Kind::Buffer;
      out.index = slot;
      out.end_of_stream = true;
      return out;
    }

    const AudioDecoder::Result r = decoder_.receive(&held_frame_);
    if (r == AudioDecoder::Result::Frame) {
      have_held_frame_ = true;
      refused = 0;
      continue;
    }
    if (r == AudioDecoder::Result::EndOfStream) {
      decoder_drained_ = true;
      continue;
    }
    if (r == AudioDecoder::Result::Error) {
      // A decoder that cannot go on ends the stream; the client is told by
      // the end-of-stream buffer and does not wait forever.
      decoder_drained_ = true;
      continue;
    }
    // The decoder wants input.
    if (pending_.empty())
      return out;
    const Pending p = pending_.front();
    const auto i = static_cast<std::size_t>(p.index);
    int rc = 0;
    if (p.size > 0)
      rc = decoder_.send(inputs_[i].data() + p.offset,
                         static_cast<std::size_t>(p.size), p.pts_us);
    if (rc == 1) {
      if (++refused > 2)
        return out;
      continue;  // it has output after all; read that first
    }
    if (rc == 0 && p.eos) {
      if (decoder_.send(nullptr, 0, p.pts_us) == 1) {
        // The access unit went in; only the end of the stream is still owed.
        pending_.front().size = 0;
        if (++refused > 2)
          return out;
        continue;
      }
      sent_eos_ = true;
    }
    pending_.pop_front();
    input_queued_[i] = false;
    if (rc < 0) {
      sent_eos_ = true;
      decoder_drained_ = true;
    }
  }
}

const std::int16_t* AudioCodec::output_data(int index,
                                            std::size_t* bytes) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (index < 0 || static_cast<std::size_t>(index) >= slots_.size() ||
      !slots_[static_cast<std::size_t>(index)].held)
    return nullptr;
  const Slot& s = slots_[static_cast<std::size_t>(index)];
  if (bytes != nullptr)
    *bytes = s.pcm.size() * sizeof(std::int16_t);
  return s.pcm.data();
}

void AudioCodec::release_output(int index) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (index < 0 || static_cast<std::size_t>(index) >= slots_.size())
      return;
    slots_[static_cast<std::size_t>(index)].held = false;
    work_flag_ = true;
  }
  work_.notify_all();
}

int AudioCodec::sample_rate() const {
  std::lock_guard<std::mutex> lock(mu_);
  return rate_;
}

int AudioCodec::channels() const {
  std::lock_guard<std::mutex> lock(mu_);
  return channels_;
}

void AudioCodec::flush() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    decoder_.flush();
    pending_.clear();
    input_held_.assign(inputs_.size(), false);
    input_queued_.assign(inputs_.size(), false);
    for (Slot& s : slots_)
      s.held = false;
    have_held_frame_ = false;
    queued_eos_ = false;
    sent_eos_ = false;
    decoder_drained_ = false;
    delivered_eos_ = false;
    work_flag_ = true;
  }
  work_.notify_all();
}

void AudioCodec::wait_for_work(int ms) {
  std::unique_lock<std::mutex> lock(mu_);
  work_.wait_for(lock, std::chrono::milliseconds(ms),
                 [this] { return work_flag_; });
  work_flag_ = false;
}

void AudioCodec::wake() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    work_flag_ = true;
  }
  work_.notify_all();
}

}  // namespace acl::media
