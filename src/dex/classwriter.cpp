// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/dex.md (pending), JVM Specification ch.4 (class file). No GPL-3.0 sources.

#include "dex/classwriter.h"

#include <utility>

namespace aclcxx::dex {
namespace {

// Constant pool tags (JVMS 4.4).
constexpr std::uint8_t kUtf8 = 1;
constexpr std::uint8_t kInteger = 3;
constexpr std::uint8_t kLong = 5;
constexpr std::uint8_t kClass = 7;
constexpr std::uint8_t kString = 8;
constexpr std::uint8_t kNameAndType = 12;
constexpr std::uint8_t kMethodref = 10;
constexpr std::uint8_t kFieldref = 9;

void be16(std::vector<std::uint8_t>& v, std::uint16_t x) {
  v.push_back(static_cast<std::uint8_t>(x >> 8));
  v.push_back(static_cast<std::uint8_t>(x & 0xff));
}
void be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  v.push_back(static_cast<std::uint8_t>(x >> 24));
  v.push_back(static_cast<std::uint8_t>(x >> 16));
  v.push_back(static_cast<std::uint8_t>(x >> 8));
  v.push_back(static_cast<std::uint8_t>(x & 0xff));
}

}  // namespace

std::uint16_t ClassWriter::add_entry(std::vector<std::uint8_t> cp_info,
                                     const std::string& key) {
  if (auto it = interned_.find(key); it != interned_.end()) return it->second;
  pool_.push_back(std::move(cp_info));
  const auto index = static_cast<std::uint16_t>(pool_.size());  // 1-based
  interned_.emplace(key, index);
  return index;
}

std::uint16_t ClassWriter::utf8(std::string_view s) {
  std::vector<std::uint8_t> e;
  e.push_back(kUtf8);
  be16(e, static_cast<std::uint16_t>(s.size()));
  e.insert(e.end(), s.begin(), s.end());
  return add_entry(std::move(e), "U:" + std::string(s));
}

std::uint16_t ClassWriter::class_ref(std::string_view internal_name) {
  const std::uint16_t name = utf8(internal_name);
  std::vector<std::uint8_t> e;
  e.push_back(kClass);
  be16(e, name);
  return add_entry(std::move(e), "C:" + std::string(internal_name));
}

std::uint16_t ClassWriter::string_const(std::string_view s) {
  const std::uint16_t idx = utf8(s);
  std::vector<std::uint8_t> e;
  e.push_back(kString);
  be16(e, idx);
  return add_entry(std::move(e), "S:" + std::string(s));
}

std::uint16_t ClassWriter::integer_const(std::int32_t v) {
  std::vector<std::uint8_t> e;
  e.push_back(kInteger);
  be32(e, static_cast<std::uint32_t>(v));
  return add_entry(std::move(e), "I:" + std::to_string(v));
}

std::uint16_t ClassWriter::long_const(std::int64_t v) {
  const std::string key = "L:" + std::to_string(v);
  if (auto it = interned_.find(key); it != interned_.end()) return it->second;
  std::vector<std::uint8_t> e;
  e.push_back(kLong);
  be32(e, static_cast<std::uint32_t>(static_cast<std::uint64_t>(v) >> 32));
  be32(e, static_cast<std::uint32_t>(v & 0xffffffff));
  const auto index = static_cast<std::uint16_t>(pool_.size() + 1);
  pool_.push_back(std::move(e));
  pool_.push_back({});  // CONSTANT_Long occupies two cp slots; second is unusable
  interned_.emplace(key, index);
  return index;
}

std::uint16_t ClassWriter::name_and_type(std::string_view name,
                                         std::string_view desc) {
  const std::uint16_t n = utf8(name);
  const std::uint16_t d = utf8(desc);
  std::vector<std::uint8_t> e;
  e.push_back(kNameAndType);
  be16(e, n);
  be16(e, d);
  return add_entry(std::move(e),
                   "N:" + std::string(name) + ":" + std::string(desc));
}

std::uint16_t ClassWriter::methodref(std::string_view cls, std::string_view name,
                                     std::string_view desc) {
  const std::uint16_t c = class_ref(cls);
  const std::uint16_t nt = name_and_type(name, desc);
  std::vector<std::uint8_t> e;
  e.push_back(kMethodref);
  be16(e, c);
  be16(e, nt);
  return add_entry(std::move(e), "M:" + std::string(cls) + ":" +
                                     std::string(name) + ":" + std::string(desc));
}

std::uint16_t ClassWriter::fieldref(std::string_view cls, std::string_view name,
                                    std::string_view desc) {
  const std::uint16_t c = class_ref(cls);
  const std::uint16_t nt = name_and_type(name, desc);
  std::vector<std::uint8_t> e;
  e.push_back(kFieldref);
  be16(e, c);
  be16(e, nt);
  return add_entry(std::move(e), "F:" + std::string(cls) + ":" +
                                     std::string(name) + ":" + std::string(desc));
}

ClassWriter::ClassWriter(std::string this_internal, std::string super_internal) {
  this_class_ = class_ref(this_internal);
  super_class_ = class_ref(super_internal);
  code_attr_name_ = utf8("Code");
}

void ClassWriter::add_field(std::uint16_t access_flags, std::string_view name,
                            std::string_view descriptor) {
  fields_.push_back({access_flags, utf8(name), utf8(descriptor)});
}

void ClassWriter::add_method(std::uint16_t access_flags, std::string_view name,
                             std::string_view descriptor, std::uint16_t max_stack,
                             std::uint16_t max_locals,
                             std::vector<std::uint8_t> code,
                             std::vector<ExceptionEntry> exceptions,
                             std::vector<std::uint8_t> stackmaptable) {
  if (!stackmaptable.empty() && stackmap_name_ == 0) {
    stackmap_name_ = utf8("StackMapTable");
  }
  methods_.push_back({access_flags, utf8(name), utf8(descriptor), max_stack,
                      max_locals, std::move(code), std::move(exceptions),
                      std::move(stackmaptable)});
}

std::vector<std::uint8_t> ClassWriter::build() const {
  std::vector<std::uint8_t> out;
  be32(out, 0xCAFEBABE);
  be16(out, 0);   // minor
  // Major 51 (Java 7): the type-checking verifier is used, so branch/exception
  // methods carry a StackMapTable (computed by the translator).
  be16(out, 51);

  be16(out, static_cast<std::uint16_t>(pool_.size() + 1));  // cp count
  for (const auto& e : pool_) out.insert(out.end(), e.begin(), e.end());

  be16(out, 0x0021);  // ACC_PUBLIC | ACC_SUPER
  be16(out, this_class_);
  be16(out, super_class_);
  be16(out, 0);  // interfaces_count

  be16(out, static_cast<std::uint16_t>(fields_.size()));
  for (const Field& f : fields_) {
    be16(out, f.access_flags);
    be16(out, f.name_index);
    be16(out, f.descriptor_index);
    be16(out, 0);  // field attributes_count
  }

  be16(out, static_cast<std::uint16_t>(methods_.size()));
  for (const Method& m : methods_) {
    be16(out, m.access_flags);
    be16(out, m.name_index);
    be16(out, m.descriptor_index);
    be16(out, 1);  // attributes_count: just Code
    be16(out, code_attr_name_);
    const bool has_smt = !m.stackmaptable.empty();
    const std::uint32_t smt_attr_size =
        has_smt ? static_cast<std::uint32_t>(6 + m.stackmaptable.size()) : 0;
    const std::uint32_t code_attr_len = static_cast<std::uint32_t>(
        12 + m.code.size() + m.exceptions.size() * 8) + smt_attr_size;
    be32(out, code_attr_len);
    be16(out, m.max_stack);
    be16(out, m.max_locals);
    be32(out, static_cast<std::uint32_t>(m.code.size()));
    out.insert(out.end(), m.code.begin(), m.code.end());
    be16(out, static_cast<std::uint16_t>(m.exceptions.size()));
    for (const ExceptionEntry& ex : m.exceptions) {
      be16(out, ex.start_pc);
      be16(out, ex.end_pc);
      be16(out, ex.handler_pc);
      be16(out, ex.catch_type);
    }
    be16(out, has_smt ? 1 : 0);  // code attributes_count
    if (has_smt) {
      be16(out, stackmap_name_);
      be32(out, static_cast<std::uint32_t>(m.stackmaptable.size()));
      out.insert(out.end(), m.stackmaptable.begin(), m.stackmaptable.end());
    }
  }

  be16(out, 0);  // class attributes_count
  return out;
}

}  // namespace aclcxx::dex
