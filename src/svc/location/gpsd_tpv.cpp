// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l code; gpsd's JSON protocol as gpsd_json(5)
// documents it, and JSON as RFC 8259 defines it. No gpsd code. No GPL-3.0
// sources.
#include "svc/location/gpsd_tpv.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>

namespace acl::svc::location {
namespace {

// The longest string or number kept from a line; a longer one is read past.
constexpr std::size_t kMaxToken = 64;
// How deep a value that is skipped may nest. gpsd's objects nest two deep.
constexpr int kMaxDepth = 16;

// Reads one JSON object's members without building a tree: the members a4l
// wants are numbers and strings at the top level, and the rest is skipped.
class Scanner {
 public:
  explicit Scanner(std::string_view text) : s_(text) {}

  void skip_space() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' ||
                              s_[i_] == '\r' || s_[i_] == '\n'))
      ++i_;
  }
  bool take(char c) {
    if (i_ < s_.size() && s_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }
  [[nodiscard]] char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
  [[nodiscard]] bool at_end() const { return i_ >= s_.size(); }

  // A string, to `out` (its first kMaxToken characters). An escape that is
  // not one character of ASCII becomes '?': nothing a4l reads has one.
  bool string(std::string* out) {
    if (!take('"'))
      return false;
    if (out != nullptr)
      out->clear();
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"')
        return true;
      if (static_cast<unsigned char>(c) < 0x20)
        return false;  // a control character is not allowed in a string
      if (c == '\\') {
        if (i_ >= s_.size())
          return false;
        const char e = s_[i_++];
        switch (e) {
          case '"':
          case '\\':
          case '/':
            c = e;
            break;
          case 'b':
          case 'f':
          case 'n':
          case 'r':
          case 't':
            c = ' ';
            break;
          case 'u': {
            if (s_.size() - i_ < 4)
              return false;
            for (int k = 0; k < 4; ++k) {
              const char h = s_[i_ + static_cast<std::size_t>(k)];
              const bool hex = (h >= '0' && h <= '9') ||
                               (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F');
              if (!hex)
                return false;
            }
            i_ += 4;
            c = '?';
            break;
          }
          default:
            return false;
        }
      }
      if (out != nullptr && out->size() < kMaxToken)
        out->push_back(c);
    }
    return false;  // no closing quote
  }

  // A number. Empty when the text here is not one, or is not finite.
  std::optional<double> number() {
    const std::size_t start = i_;
    while (i_ < s_.size() &&
           ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '-' ||
            s_[i_] == '+' || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E'))
      ++i_;
    const std::size_t len = i_ - start;
    if (len == 0 || len > kMaxToken)
      return std::nullopt;
    const std::string token(s_.substr(start, len));
    char* end = nullptr;
    const double v = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size() || !std::isfinite(v))
      return std::nullopt;
    return v;
  }

  // Any value, read past.
  bool skip_value(int depth = 0) {
    if (depth > kMaxDepth)
      return false;
    skip_space();
    const char c = peek();
    if (c == '"')
      return string(nullptr);
    if (c == '{' || c == '[') {
      const char close = c == '{' ? '}' : ']';
      ++i_;
      skip_space();
      if (take(close))
        return true;
      for (;;) {
        if (c == '{') {
          skip_space();
          if (!string(nullptr))
            return false;
          skip_space();
          if (!take(':'))
            return false;
        }
        if (!skip_value(depth + 1))
          return false;
        skip_space();
        if (take(close))
          return true;
        if (!take(','))
          return false;
      }
    }
    if (literal("true") || literal("false") || literal("null"))
      return true;
    return number().has_value();
  }

 private:
  bool literal(std::string_view word) {
    if (s_.substr(i_, word.size()) != word)
      return false;
    i_ += word.size();
    return true;
  }

  std::string_view s_;
  std::size_t i_ = 0;
};

// Days from 1970-01-01 to a date of the proleptic Gregorian calendar.
std::int64_t days_from_civil(std::int64_t y, int m, int d) {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<std::int64_t>(y - era * 400);
  const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// "YYYY-MM-DDTHH:MM:SS[.fff]Z" as milliseconds since the epoch.
std::optional<std::int64_t> epoch_ms_from_iso8601(std::string_view t) {
  auto digits = [&t](std::size_t at, std::size_t n) -> std::optional<int> {
    if (at + n > t.size())
      return std::nullopt;
    int v = 0;
    for (std::size_t k = 0; k < n; ++k) {
      const char c = t[at + k];
      if (c < '0' || c > '9')
        return std::nullopt;
      v = v * 10 + (c - '0');
    }
    return v;
  };
  if (t.size() < 20 || t[4] != '-' || t[7] != '-' || t[10] != 'T' ||
      t[13] != ':' || t[16] != ':' || t.back() != 'Z')
    return std::nullopt;
  const auto year = digits(0, 4), month = digits(5, 2), day = digits(8, 2);
  const auto hour = digits(11, 2), minute = digits(14, 2);
  const auto second = digits(17, 2);
  if (!year || !month || !day || !hour || !minute || !second)
    return std::nullopt;
  if (*month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 ||
      *minute > 59 || *second > 60)
    return std::nullopt;
  std::int64_t ms = 0;
  if (t.size() > 20) {  // a fraction between the seconds and the Z
    if (t[19] != '.')
      return std::nullopt;
    std::int64_t scale = 100;
    for (std::size_t k = 20; k + 1 < t.size(); ++k) {
      if (t[k] < '0' || t[k] > '9')
        return std::nullopt;
      ms += (t[k] - '0') * scale;
      scale /= 10;
    }
  }
  const std::int64_t days = days_from_civil(*year, *month, *day);
  return ((days * 24 + *hour) * 60 + *minute) * 60 * 1000 +
         static_cast<std::int64_t>(*second) * 1000 + ms;
}

}  // namespace

bool fix_from_gpsd_line(std::string_view line, LocationFix* fix, int* mode) {
  if (mode != nullptr)
    *mode = -1;
  Scanner in(line);
  in.skip_space();
  if (!in.take('{'))
    return false;

  std::string cls, time;
  std::optional<double> tpv_mode, lat, lon, alt_hae, speed, track, eph, epx,
      epy;
  in.skip_space();
  if (!in.take('}')) {
    for (;;) {
      std::string key;
      in.skip_space();
      if (!in.string(&key))
        return false;
      in.skip_space();
      if (!in.take(':'))
        return false;
      in.skip_space();
      std::optional<double>* number = nullptr;
      if (key == "mode")
        number = &tpv_mode;
      else if (key == "lat")
        number = &lat;
      else if (key == "lon")
        number = &lon;
      else if (key == "altHAE")
        number = &alt_hae;
      else if (key == "speed")
        number = &speed;
      else if (key == "track")
        number = &track;
      else if (key == "eph")
        number = &eph;
      else if (key == "epx")
        number = &epx;
      else if (key == "epy")
        number = &epy;
      const bool text = key == "class" || key == "time";
      const char c = in.peek();
      const bool numeric = (c >= '0' && c <= '9') || c == '-';
      if (number != nullptr && numeric) {
        *number = in.number();
        if (!number->has_value())
          return false;
      } else if (text && c == '"') {
        if (!in.string(key == "class" ? &cls : &time))
          return false;
      } else if (!in.skip_value()) {
        return false;
      }
      in.skip_space();
      if (in.take('}'))
        break;
      if (!in.take(','))
        return false;
    }
  }
  in.skip_space();
  if (!in.at_end())
    return false;  // something after the object

  if (cls != "TPV")
    return false;
  const int m = tpv_mode && *tpv_mode >= 0 && *tpv_mode <= 3
                    ? static_cast<int>(*tpv_mode)
                    : 0;
  if (mode != nullptr)
    *mode = m;
  if (m < 2 || !lat || !lon || fix == nullptr)
    return false;
  if (*lat < -90.0 || *lat > 90.0 || *lon < -180.0 || *lon > 180.0)
    return false;

  LocationFix f{};
  f.latitude = *lat;
  f.longitude = *lon;
  if (eph && *eph > 0)
    f.accuracy_m = *eph;
  else if ((epx && *epx > 0) || (epy && *epy > 0))
    f.accuracy_m = std::max(epx.value_or(0.0), epy.value_or(0.0));
  // The height is the receiver's only with a 3D fix.
  if (m == 3 && alt_hae) {
    f.altitude_m = *alt_hae;
    f.has_altitude = true;
  }
  if (speed && *speed >= 0) {
    f.speed_mps = *speed;
    f.has_speed = true;
  }
  if (track && *track >= 0 && *track <= 360.0) {
    f.bearing_deg = *track == 360.0 ? 0.0 : *track;
    f.has_bearing = true;
  }
  const auto when = epoch_ms_from_iso8601(time);
  f.timestamp_ms =
      when ? *when : static_cast<std::int64_t>(std::time(nullptr)) * 1000;
  *fix = f;
  return true;
}

bool parse_gpsd_endpoint(std::string_view text,
                         std::string* host,
                         std::string* port) {
  std::string_view h, p;
  if (!text.empty() && text.front() == '[') {  // [ipv6] or [ipv6]:port
    const std::size_t close = text.find(']');
    if (close == std::string_view::npos)
      return false;
    h = text.substr(1, close - 1);
    const std::string_view rest = text.substr(close + 1);
    if (!rest.empty()) {
      if (rest.front() != ':')
        return false;
      p = rest.substr(1);
    }
    if (h.empty())
      return false;
  } else {
    const std::size_t colon = text.find(':');
    if (colon != std::string_view::npos &&
        text.find(':', colon + 1) != std::string_view::npos)
      return false;  // a bare IPv6 address: which colon is the port's?
    h = text.substr(0, colon);
    if (colon != std::string_view::npos)
      p = text.substr(colon + 1);
  }
  for (const char c : p)
    if (c < '0' || c > '9')
      return false;
  if (p.size() > 5 || (!p.empty() && std::atoi(std::string(p).c_str()) > 65535))
    return false;
  for (const char c : h)
    if (static_cast<unsigned char>(c) <= 0x20 || c == '/')
      return false;
  *host = h.empty() ? "localhost" : std::string(h);
  *port = p.empty() ? "2947" : std::string(p);
  return true;
}

}  // namespace acl::svc::location
