// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/pkg.md (pending), AOSP androidfw ResourceTypes.h chunk layout
// (public Apache-2.0 headers), Android manifest component semantics. No GPL-3.0 sources.

#include "pkg/manifest.h"

#include <charconv>
#include <cstring>
#include <string_view>
#include <utility>

namespace acl::pkg {
namespace {

// ResChunk_header.type values (androidfw ResourceTypes.h).
constexpr std::uint16_t kResStringPool = 0x0001;
constexpr std::uint16_t kResXml = 0x0003;
constexpr std::uint16_t kResXmlResourceMap = 0x0180;
constexpr std::uint16_t kResXmlStartElement = 0x0102;
constexpr std::uint16_t kResXmlEndElement = 0x0103;

// ResStringPool_header.flags
constexpr std::uint32_t kUtf8Flag = 1u << 8;

// Res_value.dataType
constexpr std::uint8_t kTypeString = 0x03;
constexpr std::uint8_t kTypeFirstInt = 0x10;  // TYPE_INT_DEC
constexpr std::uint8_t kTypeLastInt = 0x1f;   // TYPE_INT_COLOR_* etc.

constexpr std::uint32_t kNoEntry = 0xffffffffu;

std::uint16_t rd16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1]) << 8;
}
std::uint32_t rd32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) |
         static_cast<std::uint32_t>(p[1]) << 8 |
         static_cast<std::uint32_t>(p[2]) << 16 |
         static_cast<std::uint32_t>(p[3]) << 24;
}

// Length prefixes in a string pool are variable-width. Returns the length and
// advances `consumed` by the number of prefix bytes/units read.
std::uint32_t decode_len8(const std::uint8_t* p, std::size_t& consumed) noexcept {
  if (p[0] & 0x80) {
    consumed = 2;
    return (static_cast<std::uint32_t>(p[0] & 0x7f) << 8) | p[1];
  }
  consumed = 1;
  return p[0];
}
std::uint32_t decode_len16(const std::uint8_t* p, std::size_t& consumed) noexcept {
  const std::uint16_t u0 = rd16(p);
  if (u0 & 0x8000) {
    consumed = 4;
    return (static_cast<std::uint32_t>(u0 & 0x7fff) << 16) | rd16(p + 2);
  }
  consumed = 2;
  return u0;
}

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
}

std::string utf16le_to_utf8(const std::uint8_t* p, std::uint32_t units) {
  std::string out;
  for (std::uint32_t i = 0; i < units; ++i) {
    std::uint32_t c = rd16(p + 2 * i);
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < units) {
      const std::uint32_t lo = rd16(p + 2 * (i + 1));
      if (lo >= 0xdc00 && lo <= 0xdfff) {
        c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
        ++i;
      }
    }
    append_utf8(out, c);
  }
  return out;
}

// A parsed attribute value.
struct Attr {
  std::string name;
  std::string string_value;  // resolved string, if the value is/refs a string
  std::uint8_t type = 0;
  std::uint32_t data = 0;

  bool is_int() const noexcept {
    return type >= kTypeFirstInt && type <= kTypeLastInt;
  }
  std::optional<std::int32_t> as_int() const {
    if (is_int()) return static_cast<std::int32_t>(data);
    std::int32_t v{};
    const char* b = string_value.data();
    const char* e = b + string_value.size();
    if (auto [p, ec] = std::from_chars(b, e, v); ec == std::errc{} && p == e) {
      return v;
    }
    return std::nullopt;
  }
  std::optional<std::uint32_t> as_uint() const {
    if (is_int()) return data;
    if (auto v = as_int()) return static_cast<std::uint32_t>(*v);
    return std::nullopt;
  }
};

// Map a framework attribute resource id to its canonical name — used only when
// the compiled string pool stored an empty attribute name (some toolchains do).
std::string_view canonical_attr(std::uint32_t res_id) noexcept {
  switch (res_id) {
    case 0x01010003: return "name";
    case 0x01010001: return "label";
    case 0x0101020c: return "minSdkVersion";
    case 0x01010270: return "targetSdkVersion";
    case 0x0101021b: return "versionCode";
    case 0x0101021c: return "versionName";
    case 0x01010572: return "compileSdkVersion";
    default: return {};
  }
}

class AxmlParser {
 public:
  AxmlParser(const std::uint8_t* data, std::size_t size, Manifest& out)
      : data_(data), size_(size), out_(out) {}

  AxmlError run() {
    if (size_ < 8 || rd16(data_) != kResXml) {
      return {AxmlStatus::BadFormat, "not a binary XML (RES_XML) chunk"};
    }
    const std::uint16_t root_header = rd16(data_ + 2);
    std::size_t pos = root_header < 8 ? 8 : root_header;

    while (pos + 8 <= size_) {
      const std::uint16_t type = rd16(data_ + pos);
      const std::uint16_t header = rd16(data_ + pos + 2);
      const std::uint32_t csize = rd32(data_ + pos + 4);
      if (csize < 8 || header < 8 || pos + csize > size_) {
        return {AxmlStatus::Truncated, "chunk exceeds buffer"};
      }
      AxmlError err;
      switch (type) {
        case kResStringPool:      err = parse_string_pool(pos, csize); break;
        case kResXmlResourceMap:  parse_resource_map(pos, header, csize); break;
        case kResXmlStartElement: err = handle_start(pos, header, csize); break;
        case kResXmlEndElement:   err = handle_end(pos, header, csize); break;
        default: break;  // namespaces, cdata: ignored
      }
      if (err) return err;
      pos += csize;
    }
    return {};
  }

 private:
  std::string str(std::uint32_t idx) const {
    return idx < pool_.size() ? pool_[idx] : std::string{};
  }

  std::string attr_name(std::uint32_t idx) const {
    std::string s = str(idx);
    if (!s.empty()) return s;
    if (idx < resmap_.size()) return std::string(canonical_attr(resmap_[idx]));
    return s;
  }

  AxmlError parse_string_pool(std::size_t pos, std::uint32_t csize) {
    const std::uint8_t* base = data_ + pos;
    const std::uint32_t count = rd32(base + 8);
    const std::uint32_t flags = rd32(base + 16);
    const std::uint32_t strings_start = rd32(base + 20);
    const bool utf8 = (flags & kUtf8Flag) != 0;

    if (28 + static_cast<std::uint64_t>(count) * 4 > csize) {
      return {AxmlStatus::Truncated, "string offset table exceeds chunk"};
    }
    pool_.clear();
    pool_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint32_t off = rd32(base + 28 + 4 * i);
      const std::uint64_t sp = static_cast<std::uint64_t>(strings_start) + off;
      if (sp + 2 > csize) return {AxmlStatus::Truncated, "string offset out of range"};
      const std::uint8_t* p = base + sp;
      std::size_t c1 = 0;
      if (utf8) {
        const std::uint32_t nchars = decode_len8(p, c1);  // char count (unused)
        (void)nchars;
        std::size_t c2 = 0;
        const std::uint32_t nbytes = decode_len8(p + c1, c2);
        if (sp + c1 + c2 + nbytes > csize) {
          return {AxmlStatus::Truncated, "utf8 string exceeds chunk"};
        }
        pool_.emplace_back(reinterpret_cast<const char*>(p + c1 + c2), nbytes);
      } else {
        const std::uint32_t nunits = decode_len16(p, c1);
        if (sp + c1 + static_cast<std::uint64_t>(nunits) * 2 > csize) {
          return {AxmlStatus::Truncated, "utf16 string exceeds chunk"};
        }
        pool_.emplace_back(utf16le_to_utf8(p + c1, nunits));
      }
    }
    return {};
  }

  void parse_resource_map(std::size_t pos, std::uint16_t header,
                          std::uint32_t csize) {
    const std::uint32_t n = (csize - header) / 4;
    resmap_.clear();
    resmap_.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      resmap_.push_back(rd32(data_ + pos + header + 4 * i));
    }
  }

  AxmlError handle_start(std::size_t pos, std::uint16_t header,
                         std::uint32_t csize) {
    const std::uint8_t* ext = data_ + pos + header;
    const std::uint32_t name_ref = rd32(ext + 4);
    const std::uint16_t attr_start = rd16(ext + 8);
    const std::uint16_t attr_size = rd16(ext + 10);
    const std::uint16_t attr_count = rd16(ext + 12);

    const std::uint64_t attrs_base = header + attr_start;
    if (attrs_base + static_cast<std::uint64_t>(attr_count) * attr_size > csize) {
      return {AxmlStatus::Truncated, "attributes exceed element chunk"};
    }

    std::vector<Attr> attrs;
    attrs.reserve(attr_count);
    for (std::uint16_t a = 0; a < attr_count; ++a) {
      const std::uint8_t* ab = data_ + pos + attrs_base + a * attr_size;
      const std::uint32_t a_name = rd32(ab + 4);
      const std::uint32_t a_raw = rd32(ab + 8);
      const std::uint8_t a_type = ab[15];    // Res_value.dataType
      const std::uint32_t a_data = rd32(ab + 16);
      Attr attr;
      attr.name = attr_name(a_name);
      attr.type = a_type;
      attr.data = a_data;
      if (a_raw != kNoEntry) {
        attr.string_value = str(a_raw);
      } else if (a_type == kTypeString) {
        attr.string_value = str(a_data);
      }
      attrs.push_back(std::move(attr));
    }

    dispatch_start(str(name_ref), attrs);
    return {};
  }

  AxmlError handle_end(std::size_t pos, std::uint16_t header,
                       std::uint32_t csize) {
    if (header + 8u > csize) return {AxmlStatus::Truncated, "end element too small"};
    const std::uint32_t name_ref = rd32(data_ + pos + header + 4);
    const std::string name = str(name_ref);
    if (name == "intent-filter") {
      if (in_intent_filter_ && if_main_ && if_launcher_ && have_activity_) {
        current_.launcher = true;
      }
      in_intent_filter_ = false;
    } else if (name == "activity" || name == "activity-alias") {
      if (have_activity_) {
        out_.activities.push_back(std::move(current_));
        have_activity_ = false;
      }
    }
    return {};
  }

  static const Attr* find(const std::vector<Attr>& attrs, std::string_view name) {
    for (const Attr& a : attrs) {
      if (a.name == name) return &a;
    }
    return nullptr;
  }

  void dispatch_start(const std::string& elem, const std::vector<Attr>& attrs) {
    if (elem == "manifest") {
      if (const Attr* a = find(attrs, "package")) out_.package = a->string_value;
      if (const Attr* a = find(attrs, "versionCode")) out_.version_code = a->as_uint();
      if (const Attr* a = find(attrs, "versionName")) out_.version_name = a->string_value;
      if (const Attr* a = find(attrs, "compileSdkVersion")) out_.compile_sdk = a->as_int();
    } else if (elem == "uses-sdk") {
      if (const Attr* a = find(attrs, "minSdkVersion")) out_.min_sdk = a->as_int();
      if (const Attr* a = find(attrs, "targetSdkVersion")) out_.target_sdk = a->as_int();
    } else if (elem == "application") {
      if (const Attr* a = find(attrs, "name"))
        out_.application_name = resolve_class_name(out_.package, a->string_value);
      // android:theme is a resource reference; its typed data IS the style resId.
      if (const Attr* a = find(attrs, "theme")) out_.application_theme = a->data;
    } else if (elem == "activity" || elem == "activity-alias") {
      current_ = Activity{};
      have_activity_ = true;
      if (const Attr* a = find(attrs, "name")) {
        current_.name = resolve_class_name(out_.package, a->string_value);
      }
      // <activity-alias android:targetActivity> names the real Activity class the alias stands
      // in for (the alias name itself is not a class). Resolved so launch_class() follows it.
      if (const Attr* a = find(attrs, "targetActivity")) {
        current_.target_activity = resolve_class_name(out_.package, a->string_value);
      }
      // android:enabled=false (e.g. F-Droid's panic/disguise launcher aliases) —
      // a disabled component isn't launchable. Absent → default true.
      if (const Attr* a = find(attrs, "enabled")) {
        current_.enabled = a->as_int().value_or(1) != 0;
      }
      // Per-activity android:theme overrides the application theme (e.g. a NoActionBar theme so
      // the activity can setSupportActionBar its own Toolbar). Resource ref: typed data = style resId.
      if (const Attr* a = find(attrs, "theme")) current_.theme = a->data;
    } else if (elem == "intent-filter") {
      in_intent_filter_ = true;
      if_main_ = false;
      if_launcher_ = false;
    } else if (elem == "action") {
      const Attr* a = find(attrs, "name");
      if (in_intent_filter_ && a && a->string_value == "android.intent.action.MAIN") {
        if_main_ = true;
      }
    } else if (elem == "category") {
      const Attr* a = find(attrs, "name");
      if (in_intent_filter_ && a &&
          a->string_value == "android.intent.category.LAUNCHER") {
        if_launcher_ = true;
      }
    }
  }

  const std::uint8_t* data_;
  std::size_t size_;
  Manifest& out_;
  std::vector<std::string> pool_;
  std::vector<std::uint32_t> resmap_;

  bool have_activity_ = false;
  Activity current_;
  bool in_intent_filter_ = false;
  bool if_main_ = false;
  bool if_launcher_ = false;
};

}  // namespace

const char* axml_status_name(AxmlStatus status) noexcept {
  switch (status) {
    case AxmlStatus::Ok:          return "ok";
    case AxmlStatus::BadFormat:   return "bad-format";
    case AxmlStatus::Truncated:   return "truncated";
    case AxmlStatus::Unsupported: return "unsupported";
  }
  return "?";
}

const Activity* Manifest::launcher() const noexcept {
  // First ENABLED launcher: a disabled MAIN/LAUNCHER component (e.g. a panic-mode
  // disguise) must not shadow the app's real launcher.
  for (const Activity& a : activities) {
    if (a.launcher && a.enabled) return &a;
  }
  return nullptr;
}

std::string resolve_class_name(std::string_view package, std::string_view name) {
  if (name.empty()) return std::string(name);
  if (name.front() == '.') return std::string(package) + std::string(name);
  if (name.find('.') == std::string_view::npos) {
    return std::string(package) + "." + std::string(name);
  }
  return std::string(name);
}

AxmlError parse_manifest(const std::uint8_t* data, std::size_t size, Manifest& out) {
  return AxmlParser(data, size, out).run();
}

AxmlError parse_manifest(const std::vector<std::uint8_t>& bytes, Manifest& out) {
  return parse_manifest(bytes.data(), bytes.size(), out);
}

}  // namespace acl::pkg
