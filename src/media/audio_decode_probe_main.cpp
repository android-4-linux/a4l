// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test tool. Container framing from the public
// specifications: ISO/IEC 13818-7 (ADTS header), ISO/IEC 11172-3 (MPEG audio
// frame header), RFC 3533 (Ogg pages and lacing), RFC 7845 (Opus in Ogg), the
// Vorbis I specification (header packets) and the FLAC-in-Ogg mapping. No
// GPL-3.0 sources.
//
// acl-audio-decode-probe <mime> <file>
//
// Decodes an elementary audio stream with media::AudioDecoder the way an
// Android app's player would drive MediaCodec: it cuts the file into access
// units, builds the codec-specific data a MediaFormat would carry (csd-0,
// csd-1), and feeds them. Then it says what came out: frames, rate, channels,
// the frequency of the left channel by zero crossings, and its peak. The
// test (tests/integration/run_audio_decode_test.py) encodes a known tone and
// reads these back.
//
//   audio/mp4a-latm   ADTS             csd-0 = AudioSpecificConfig, from the
//   header audio/mpeg        MPEG audio frames audio/opus        Ogg csd-0 =
//   OpusHead audio/vorbis      Ogg              csd-0 = identification, csd-1 =
//   setup audio/flac        Ogg (FLAC mapping) csd-0 = "fLaC" + STREAMINFO
//   block
//
// Exit 0 with a report, 77 if the decoder is not available here, 1 on error.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "media/audio_decoder.h"

namespace {

using Bytes = std::vector<std::uint8_t>;
using acl::media::AudioDecoder;

struct Stream {
  std::vector<Bytes> csd;
  std::vector<Bytes> units;
};

bool fail(const char* why) {
  std::fprintf(stderr, "acl-audio-decode-probe: %s\n", why);
  return false;
}

// ADTS: a 7-byte header (9 with a CRC) before each raw AAC frame.
bool cut_adts(const Bytes& d, Stream* out) {
  std::size_t at = 0;
  while (at + 7 <= d.size()) {
    if (d[at] != 0xff || (d[at + 1] & 0xf0) != 0xf0)
      return fail("not an ADTS sync word");
    const bool crc = (d[at + 1] & 1) == 0;
    const std::size_t header = crc ? 9 : 7;
    const std::size_t length = (static_cast<std::size_t>(d[at + 3] & 3) << 11) |
                               (static_cast<std::size_t>(d[at + 4]) << 3) |
                               (d[at + 5] >> 5);
    if (length < header || at + length > d.size())
      return fail("ADTS frame runs past the file");
    if (out->csd.empty()) {
      // AudioSpecificConfig: 5 bits object type, 4 bits frequency index,
      // 4 bits channel configuration, 3 bits zero.
      const int object_type = (d[at + 2] >> 6) + 1;
      const int freq_index = (d[at + 2] >> 2) & 0xf;
      const int channel_config = ((d[at + 2] & 1) << 2) | (d[at + 3] >> 6);
      out->csd.push_back(
          {static_cast<std::uint8_t>((object_type << 3) | (freq_index >> 1)),
           static_cast<std::uint8_t>(((freq_index & 1) << 7) |
                                     (channel_config << 3))});
    }
    out->units.emplace_back(d.begin() + static_cast<long>(at + header),
                            d.begin() + static_cast<long>(at + length));
    at += length;
  }
  return !out->units.empty();
}

// MPEG-1/2/2.5 Layer III frames. An ID3v2 tag at the start is skipped.
bool cut_mp3(const Bytes& d, Stream* out) {
  std::size_t at = 0;
  if (d.size() > 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3')
    at = 10 + ((static_cast<std::size_t>(d[6] & 0x7f) << 21) |
               (static_cast<std::size_t>(d[7] & 0x7f) << 14) |
               (static_cast<std::size_t>(d[8] & 0x7f) << 7) | (d[9] & 0x7f));
  static const int kBitrateV1[16] = {0,   32,  40,  48,  56,  64,  80,  96,
                                     112, 128, 160, 192, 224, 256, 320, 0};
  static const int kBitrateV2[16] = {0,  8,  16, 24,  32,  40,  48,  56,
                                     64, 80, 96, 112, 128, 144, 160, 0};
  static const int kRate[4][3] = {{11025, 12000, 8000},    // 2.5
                                  {0, 0, 0},               // reserved
                                  {22050, 24000, 16000},   // 2
                                  {44100, 48000, 32000}};  // 1
  while (at + 4 <= d.size()) {
    if (d[at] != 0xff || (d[at + 1] & 0xe0) != 0xe0) {
      ++at;  // tags and padding between frames
      continue;
    }
    const int version = (d[at + 1] >> 3) & 3;
    const int layer = (d[at + 1] >> 1) & 3;
    const int bitrate_index = d[at + 2] >> 4;
    const int rate_index = (d[at + 2] >> 2) & 3;
    const int padding = (d[at + 2] >> 1) & 1;
    if (version == 1 || layer != 1 || bitrate_index == 0 ||
        bitrate_index == 15 || rate_index == 3) {
      ++at;
      continue;
    }
    const int bitrate =
        (version == 3 ? kBitrateV1 : kBitrateV2)[bitrate_index] * 1000;
    const int rate = kRate[version][rate_index];
    const std::size_t length = static_cast<std::size_t>(
        (version == 3 ? 144 : 72) * bitrate / rate + padding);
    if (length < 4 || at + length > d.size())
      break;
    out->units.emplace_back(d.begin() + static_cast<long>(at),
                            d.begin() + static_cast<long>(at + length));
    at += length;
  }
  return !out->units.empty();
}

// Ogg: pages of segments; a packet is a run of segments ending in one shorter
// than 255, and may span pages.
bool ogg_packets(const Bytes& d, std::vector<Bytes>* packets) {
  std::size_t at = 0;
  Bytes packet;
  while (at + 27 <= d.size()) {
    if (d[at] != 'O' || d[at + 1] != 'g' || d[at + 2] != 'g' ||
        d[at + 3] != 'S')
      return fail("not an Ogg page");
    const std::size_t segments = d[at + 26];
    if (at + 27 + segments > d.size())
      return fail("Ogg page header runs past the file");
    std::size_t body = at + 27 + segments;
    for (std::size_t s = 0; s < segments; ++s) {
      const std::size_t n = d[at + 27 + s];
      if (body + n > d.size())
        return fail("Ogg page body runs past the file");
      packet.insert(packet.end(), d.begin() + static_cast<long>(body),
                    d.begin() + static_cast<long>(body + n));
      body += n;
      if (n < 255) {
        packets->push_back(packet);
        packet.clear();
      }
    }
    at = body;
  }
  return !packets->empty();
}

bool starts_with(const Bytes& b, const char* s, std::size_t n) {
  if (b.size() < n)
    return false;
  for (std::size_t i = 0; i < n; ++i)
    if (b[i] != static_cast<std::uint8_t>(s[i]))
      return false;
  return true;
}

bool cut_ogg(const std::string& mime, const Bytes& d, Stream* out) {
  std::vector<Bytes> packets;
  if (!ogg_packets(d, &packets))
    return false;
  std::size_t first_audio = 0;
  if (mime == "audio/opus") {
    if (packets.size() < 3 || !starts_with(packets[0], "OpusHead", 8) ||
        !starts_with(packets[1], "OpusTags", 8))
      return fail("not Opus in Ogg");
    out->csd.push_back(packets[0]);
    first_audio = 2;
  } else if (mime == "audio/vorbis") {
    if (packets.size() < 4 || !starts_with(packets[0], "\x01vorbis", 7) ||
        !starts_with(packets[1], "\x03vorbis", 7) ||
        !starts_with(packets[2], "\x05vorbis", 7))
      return fail("not Vorbis in Ogg");
    out->csd.push_back(packets[0]);
    out->csd.push_back(packets[2]);
    first_audio = 3;
  } else {  // FLAC: 0x7F "FLAC", version, header count, "fLaC", STREAMINFO
    if (packets.empty() || packets[0].size() < 13 + 38 - 4 ||
        !starts_with(packets[0],
                     "\x7f"
                     "FLAC",
                     5) ||
        packets[0][9] != 'f' || packets[0][10] != 'L')
      return fail("not FLAC in Ogg");
    const std::size_t headers =
        (static_cast<std::size_t>(packets[0][7]) << 8) | packets[0][8];
    out->csd.emplace_back(packets[0].begin() + 9, packets[0].end());
    first_audio = 1 + headers;
    // A header count of 0 means "unknown": skip metadata blocks by their
    // first byte, which for an audio frame is the 0xFF of the sync code.
    while (first_audio < packets.size() && !packets[first_audio].empty() &&
           packets[first_audio][0] != 0xff)
      ++first_audio;
  }
  for (std::size_t i = first_audio; i < packets.size(); ++i)
    out->units.push_back(std::move(packets[i]));
  return !out->units.empty();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: acl-audio-decode-probe <mime> <file>\n");
    return 2;
  }
  const std::string mime = argv[1];
  if (!AudioDecoder::known(mime)) {
    std::fprintf(stderr, "acl-audio-decode-probe: %s is not a type a4l names\n",
                 mime.c_str());
    return 1;
  }
  if (!AudioDecoder::available(mime)) {
    std::printf("SKIP: no decoder for %s on this host\n", mime.c_str());
    return 77;
  }
  std::ifstream in(argv[2], std::ios::binary);
  const Bytes data((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  if (data.empty()) {
    fail("cannot read the file");
    return 1;
  }
  Stream stream;
  const bool cut = mime == "audio/mp4a-latm" ? cut_adts(data, &stream)
                   : mime == "audio/mpeg"    ? cut_mp3(data, &stream)
                                             : cut_ogg(mime, data, &stream);
  if (!cut) {
    fail("no access units found");
    return 1;
  }

  AudioDecoder::Config config;
  config.mime = mime;
  config.csd = stream.csd;
  std::string error;
  auto decoder = AudioDecoder::configure(config, &error);
  if (!decoder) {
    std::fprintf(stderr, "acl-audio-decode-probe: configure: %s\n",
                 error.c_str());
    return 1;
  }

  std::vector<std::int16_t> left;
  int rate = 0, channels = 0, format_changes = 0;
  bool ended = false;
  AudioDecoder::Frame frame;
  const auto drain = [&]() -> bool {
    for (;;) {
      const AudioDecoder::Result r = decoder->receive(&frame, &error);
      if (r == AudioDecoder::Result::Again)
        return true;
      if (r == AudioDecoder::Result::EndOfStream) {
        ended = true;
        return true;
      }
      if (r == AudioDecoder::Result::Error) {
        std::fprintf(stderr, "acl-audio-decode-probe: receive: %s\n",
                     error.c_str());
        return false;
      }
      if (frame.sample_rate != rate || frame.channels != channels) {
        rate = frame.sample_rate;
        channels = frame.channels;
        ++format_changes;
      }
      for (std::size_t i = 0; i < frame.pcm.size();
           i += static_cast<std::size_t>(frame.channels))
        left.push_back(frame.pcm[i]);
    }
  };
  std::int64_t pts = 0;
  for (std::size_t i = 0; i <= stream.units.size(); ++i) {
    const bool last = i == stream.units.size();
    for (;;) {
      const int rc =
          last ? decoder->send_packet(nullptr, 0, pts, &error)
               : decoder->send_packet(stream.units[i].data(),
                                      stream.units[i].size(), pts, &error);
      if (rc < 0) {
        std::fprintf(stderr, "acl-audio-decode-probe: send: %s\n",
                     error.c_str());
        return 1;
      }
      if (!drain())
        return 1;
      if (rc == 0)
        break;
    }
    pts += 20000;
  }
  while (!ended) {
    if (!drain())
      return 1;
    if (!ended && decoder->send_packet(nullptr, 0, pts, &error) < 0)
      break;
  }

  // The tone, measured where it is steady: the middle half.
  int peak = 0;
  double hz = 0;
  if (left.size() >= 8 && rate > 0) {
    const std::size_t a = left.size() / 4, b = left.size() * 3 / 4;
    int crossings = 0;
    for (std::size_t i = a + 1; i < b; ++i) {
      if ((left[i - 1] < 0) != (left[i] < 0))
        ++crossings;
      peak = std::max(peak, std::abs(static_cast<int>(left[i])));
    }
    hz = crossings / 2.0 / (static_cast<double>(b - a) / rate);
  }
  std::printf(
      "units=%zu csd=%zu frames=%zu rate=%d channels=%d "
      "format_changes=%d seconds=%.3f hz=%.1f peak=%d eos=%d\n",
      stream.units.size(), stream.csd.size(), left.size(), rate, channels,
      format_changes, rate > 0 ? static_cast<double>(left.size()) / rate : 0.0,
      hz, peak, ended ? 1 : 0);
  return 0;
}
