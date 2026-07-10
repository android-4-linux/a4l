// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/dex.md (pending), public Dalvik .dex format spec. No GPL-3.0 sources.

#include "dex/dexfile.h"

#include <cstring>
#include <string_view>

namespace aclcxx::dex {
namespace {

constexpr std::uint32_t kNoIndex = 0xffffffffu;

std::uint16_t rd16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1]) << 8;
}
std::uint32_t rd32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) |
         static_cast<std::uint32_t>(p[1]) << 8 |
         static_cast<std::uint32_t>(p[2]) << 16 |
         static_cast<std::uint32_t>(p[3]) << 24;
}

// Cursor over the dex buffer with bounds-checked reads.
class Reader {
 public:
  Reader(const std::uint8_t* base, std::size_t size) : base_(base), size_(size) {}

  bool ok() const noexcept { return ok_; }
  const std::string& error() const noexcept { return error_; }

  bool in_bounds(std::uint64_t off, std::uint64_t len) const noexcept {
    return off + len <= size_ && off + len >= off;
  }

  std::uint32_t u32(std::uint64_t off) {
    if (!require(off, 4)) return 0;
    return rd32(base_ + off);
  }
  std::uint16_t u16(std::uint64_t off) {
    if (!require(off, 2)) return 0;
    return rd16(base_ + off);
  }

  // uleb128 starting at `off`; advances `off` past the value.
  std::uint32_t uleb(std::uint64_t& off) {
    std::uint32_t result = 0;
    int shift = 0;
    for (int i = 0; i < 5; ++i) {
      if (!require(off, 1)) return 0;
      const std::uint8_t b = base_[off++];
      result |= static_cast<std::uint32_t>(b & 0x7f) << shift;
      if ((b & 0x80) == 0) return result;
      shift += 7;
    }
    fail("uleb128 too long");
    return result;
  }

  // MUTF-8 string_data_item at `off`: uleb128 utf16 length, then bytes to NUL.
  std::string string_data(std::uint64_t off) {
    uleb(off);  // utf16 char count (unused; we read to the NUL terminator)
    std::string out;
    while (require(off, 1)) {
      const std::uint8_t b = base_[off++];
      if (b == 0) break;
      out.push_back(static_cast<char>(b));  // ASCII/MUTF-8 passthrough
    }
    return out;
  }

  const std::uint8_t* base() const noexcept { return base_; }
  std::size_t size() const noexcept { return size_; }

  void fail(std::string msg) {
    if (ok_) {
      ok_ = false;
      error_ = std::move(msg);
    }
  }

 private:
  bool require(std::uint64_t off, std::uint64_t len) {
    if (!in_bounds(off, len)) {
      fail("read out of bounds");
      return false;
    }
    return true;
  }

  const std::uint8_t* base_;
  std::size_t size_;
  bool ok_ = true;
  std::string error_;
};

}  // namespace

const char* dex_status_name(DexStatus status) noexcept {
  switch (status) {
    case DexStatus::Ok:          return "ok";
    case DexStatus::BadFormat:   return "bad-format";
    case DexStatus::Truncated:   return "truncated";
    case DexStatus::Unsupported: return "unsupported";
  }
  return "?";
}

std::string descriptor_to_java(std::string_view d) {
  int dims = 0;
  while (!d.empty() && d.front() == '[') {
    ++dims;
    d.remove_prefix(1);
  }
  std::string base;
  if (!d.empty() && d.front() == 'L') {
    base = std::string(d.substr(1, d.size() >= 2 ? d.size() - 2 : 0));  // strip L..;
    for (char& c : base) {
      if (c == '/') c = '.';
    }
  } else if (d.size() == 1) {
    switch (d.front()) {
      case 'V': base = "void"; break;
      case 'Z': base = "boolean"; break;
      case 'B': base = "byte"; break;
      case 'S': base = "short"; break;
      case 'C': base = "char"; break;
      case 'I': base = "int"; break;
      case 'J': base = "long"; break;
      case 'F': base = "float"; break;
      case 'D': base = "double"; break;
      default:  base = std::string(d); break;
    }
  } else {
    base = std::string(d);
  }
  for (int i = 0; i < dims; ++i) base += "[]";
  return base;
}

namespace {

// Bundles the header table offsets and resolves ids to strings/types/methods.
struct Tables {
  Reader& r;
  std::uint64_t string_ids_off, type_ids_off, proto_ids_off, method_ids_off;
  std::uint32_t string_ids_size, type_ids_size, proto_ids_size, method_ids_size;

  std::string str(std::uint32_t idx) {
    if (idx >= string_ids_size) { r.fail("string index out of range"); return {}; }
    return r.string_data(r.u32(string_ids_off + idx * 4));
  }
  std::string type_desc(std::uint32_t idx) {
    if (idx >= type_ids_size) { r.fail("type index out of range"); return {}; }
    return str(r.u32(type_ids_off + idx * 4));
  }
  // Fill a method's name/return/params from a method_id index.
  void method(std::uint32_t idx, DexMethod& out) {
    if (idx >= method_ids_size) { r.fail("method index out of range"); return; }
    const std::uint64_t base = method_ids_off + idx * 8;
    const std::uint16_t proto_idx = r.u16(base + 2);
    const std::uint32_t name_idx = r.u32(base + 4);
    out.name = str(name_idx);
    if (proto_idx >= proto_ids_size) { r.fail("proto index out of range"); return; }
    const std::uint64_t p = proto_ids_off + proto_idx * 12;
    out.return_descriptor = type_desc(r.u32(p + 4));
    out.return_type = descriptor_to_java(out.return_descriptor);
    const std::uint32_t params_off = r.u32(p + 8);
    if (params_off != 0) {
      const std::uint32_t count = r.u32(params_off);
      for (std::uint32_t i = 0; i < count; ++i) {
        std::string pd = type_desc(r.u16(params_off + 4 + i * 2));
        out.param_types.push_back(descriptor_to_java(pd));
        out.param_descriptors.push_back(std::move(pd));
      }
    }
  }
};

// Parse a code_item at `off` into `out`.
void parse_code_item(Reader& r, std::uint64_t off, DexCode& out) {
  out.registers_size = r.u16(off + 0);
  out.ins_size = r.u16(off + 2);
  out.outs_size = r.u16(off + 4);
  const std::uint32_t insns_size = r.u32(off + 12);
  const std::uint64_t insns_off = off + 16;
  if (!r.in_bounds(insns_off, static_cast<std::uint64_t>(insns_size) * 2)) {
    r.fail("code_item insns out of bounds");
    return;
  }
  out.insns.reserve(insns_size);
  for (std::uint32_t k = 0; k < insns_size; ++k) {
    out.insns.push_back(r.u16(insns_off + k * 2));
  }
}

// Parse a class_data_item: field and method encoded arrays.
void parse_class_data(Reader& r, Tables& t, std::uint64_t off, DexClass& cls) {
  const std::uint32_t static_fields = r.uleb(off);
  const std::uint32_t instance_fields = r.uleb(off);
  const std::uint32_t direct_methods = r.uleb(off);
  const std::uint32_t virtual_methods = r.uleb(off);

  // Skip encoded_field arrays (idx_diff, access_flags) — fields not modeled yet.
  for (std::uint32_t i = 0; i < static_fields + instance_fields; ++i) {
    r.uleb(off);
    r.uleb(off);
  }

  auto read_methods = [&](std::uint32_t n, std::vector<DexMethod>& into) {
    std::uint32_t method_idx = 0;
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
      method_idx += r.uleb(off);            // cumulative diff
      const std::uint32_t access = r.uleb(off);
      const std::uint32_t code_off = r.uleb(off);
      DexMethod m;
      m.access_flags = access;
      t.method(method_idx, m);
      if (code_off != 0) {
        DexCode dc;
        parse_code_item(r, code_off, dc);
        m.code = std::move(dc);
      }
      into.push_back(std::move(m));
    }
  };
  read_methods(direct_methods, cls.direct_methods);
  read_methods(virtual_methods, cls.virtual_methods);
}

}  // namespace

std::string DexMethod::jvm_descriptor() const {
  std::string d = "(";
  for (const std::string& p : param_descriptors) d += p;
  d += ")";
  d += return_descriptor;
  return d;
}

std::optional<DexFile> DexFile::parse(const std::uint8_t* data, std::size_t size,
                                      DexError& out_error) {
  if (size < 112 || std::memcmp(data, "dex\n", 4) != 0 || data[7] != 0) {
    out_error = {DexStatus::BadFormat, "not a dex file (bad magic)"};
    return std::nullopt;
  }

  DexFile dex;
  dex.data_.assign(data, data + size);
  Reader r(dex.data_.data(), dex.data_.size());

  if (r.u32(0x28) != 0x12345678u) {
    out_error = {DexStatus::Unsupported, "unexpected endian tag (big-endian dex?)"};
    return std::nullopt;
  }

  Tables t{r,
           /*string_ids_off*/ r.u32(0x3c), /*type_ids_off*/ r.u32(0x44),
           /*proto_ids_off*/ r.u32(0x4c),  /*method_ids_off*/ r.u32(0x5c),
           /*string_ids_size*/ r.u32(0x38), /*type_ids_size*/ r.u32(0x40),
           /*proto_ids_size*/ r.u32(0x48),  /*method_ids_size*/ r.u32(0x58)};

  // Resolve every method_id so the translator can turn invoke-* operands into
  // JVM method references.
  for (std::uint32_t i = 0; i < t.method_ids_size && r.ok(); ++i) {
    DexMethod tmp;
    t.method(i, tmp);
    MethodRef mr;
    std::string owner = t.type_desc(r.u16(t.method_ids_off + i * 8));
    if (owner.size() >= 2 && owner.front() == 'L' && owner.back() == ';') {
      owner = owner.substr(1, owner.size() - 2);
    }
    mr.owner_internal = std::move(owner);
    mr.name = std::move(tmp.name);
    mr.return_descriptor = tmp.return_descriptor;
    mr.param_descriptors = tmp.param_descriptors;
    mr.descriptor = tmp.jvm_descriptor();
    dex.method_refs_.push_back(std::move(mr));
  }

  const std::uint32_t class_defs_size = r.u32(0x60);
  const std::uint64_t class_defs_off = r.u32(0x64);

  for (std::uint32_t i = 0; i < class_defs_size && r.ok(); ++i) {
    const std::uint64_t cd = class_defs_off + i * 32;
    const std::uint32_t class_idx = r.u32(cd + 0);
    const std::uint32_t access = r.u32(cd + 4);
    const std::uint32_t super_idx = r.u32(cd + 8);
    const std::uint32_t class_data_off = r.u32(cd + 24);

    DexClass cls;
    cls.descriptor = t.type_desc(class_idx);
    cls.java_name = descriptor_to_java(cls.descriptor);
    cls.access_flags = access;
    if (super_idx != kNoIndex) {
      cls.super_name = descriptor_to_java(t.type_desc(super_idx));
    }
    if (class_data_off != 0) {
      parse_class_data(r, t, class_data_off, cls);
    }
    dex.classes_.push_back(std::move(cls));
  }

  if (!r.ok()) {
    out_error = {DexStatus::Truncated, r.error()};
    return std::nullopt;
  }
  out_error = {};
  return dex;
}

std::optional<DexFile> DexFile::parse(const std::vector<std::uint8_t>& bytes,
                                      DexError& out_error) {
  return parse(bytes.data(), bytes.size(), out_error);
}

}  // namespace aclcxx::dex
