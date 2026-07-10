// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/dex.md (pending), JVM Specification ch.4 (class file). No GPL-3.0 sources.

#include "dex/classwriter.h"

#include <utility>

namespace aclcxx::dex {
namespace {

// Constant pool tags (JVMS 4.4).
constexpr std::uint8_t kUtf8 = 1;
constexpr std::uint8_t kInteger = 3;
constexpr std::uint8_t kClass = 7;
constexpr std::uint8_t kString = 8;
constexpr std::uint8_t kNameAndType = 12;
constexpr std::uint8_t kMethodref = 10;

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

ClassWriter::ClassWriter(std::string this_internal, std::string super_internal) {
  this_class_ = class_ref(this_internal);
  super_class_ = class_ref(super_internal);
  code_attr_name_ = utf8("Code");
}

void ClassWriter::add_method(std::uint16_t access_flags, std::string_view name,
                             std::string_view descriptor, std::uint16_t max_stack,
                             std::uint16_t max_locals,
                             std::vector<std::uint8_t> code) {
  methods_.push_back({access_flags, utf8(name), utf8(descriptor), max_stack,
                      max_locals, std::move(code)});
}

std::vector<std::uint8_t> ClassWriter::build() const {
  std::vector<std::uint8_t> out;
  be32(out, 0xCAFEBABE);
  be16(out, 0);   // minor
  // Major 49 (Java 5): the JVM verifies these with the type-inference verifier,
  // which needs no StackMapTable — so the translator can emit branches without
  // computing stack-map frames. The translation keeps the operand stack empty at
  // every instruction boundary, which that verifier accepts.
  be16(out, 49);

  be16(out, static_cast<std::uint16_t>(pool_.size() + 1));  // cp count
  for (const auto& e : pool_) out.insert(out.end(), e.begin(), e.end());

  be16(out, 0x0021);  // ACC_PUBLIC | ACC_SUPER
  be16(out, this_class_);
  be16(out, super_class_);
  be16(out, 0);  // interfaces_count
  be16(out, 0);  // fields_count

  be16(out, static_cast<std::uint16_t>(methods_.size()));
  for (const Method& m : methods_) {
    be16(out, m.access_flags);
    be16(out, m.name_index);
    be16(out, m.descriptor_index);
    be16(out, 1);  // attributes_count: just Code
    be16(out, code_attr_name_);
    be32(out, static_cast<std::uint32_t>(12 + m.code.size()));  // attr length
    be16(out, m.max_stack);
    be16(out, m.max_locals);
    be32(out, static_cast<std::uint32_t>(m.code.size()));
    out.insert(out.end(), m.code.begin(), m.code.end());
    be16(out, 0);  // exception_table_length
    be16(out, 0);  // code attributes_count
  }

  be16(out, 0);  // class attributes_count
  return out;
}

}  // namespace aclcxx::dex
