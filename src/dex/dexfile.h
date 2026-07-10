// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/dex.md (pending), public Dalvik .dex format specification
// (source.android.com/docs/core/runtime/dex-format). No GPL-3.0 sources.
#ifndef ACLCXX_DEX_DEXFILE_H
#define ACLCXX_DEX_DEXFILE_H

// Read-only parser for the Dalvik .dex container: header + string/type/proto/
// method/class tables, exposing the class and method metadata the DEX->JVM
// converter needs. This is the front-end of the runtime's DEX path; it parses
// structure and signatures, not method bytecode (instruction translation is a
// separate, larger step). Bounds-checked throughout.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aclcxx::dex {

enum class DexStatus {
  Ok,
  BadFormat,    // bad magic / not a dex
  Truncated,    // a table or item ran past the buffer
  Unsupported,  // a construct this reader does not handle
};

const char* dex_status_name(DexStatus status) noexcept;

struct DexError {
  DexStatus status = DexStatus::Ok;
  std::string message;
  bool is_ok() const noexcept { return status == DexStatus::Ok; }
  explicit operator bool() const noexcept { return status != DexStatus::Ok; }
};

// Dalvik/JVM access flags (subset).
enum : std::uint32_t {
  kAccPublic = 0x0001,
  kAccStatic = 0x0008,
  kAccFinal = 0x0010,
  kAccAbstract = 0x0400,
  kAccNative = 0x0100,
};

// A method's Dalvik code_item (bytecode), when present.
struct DexCode {
  std::uint16_t registers_size = 0;   // total registers used by the method
  std::uint16_t ins_size = 0;         // registers occupied by incoming args
  std::uint16_t outs_size = 0;        // registers for outgoing-arg area
  std::vector<std::uint16_t> insns;   // instruction stream (16-bit code units)
};

struct DexMethod {
  std::string name;                     // e.g. "main", "<init>"
  std::string return_type;              // Java type, e.g. "void", "int" (display)
  std::vector<std::string> param_types; // Java types (display)
  // Raw type descriptors (JVM == Dalvik form): "V", "I", "[Ljava/lang/String;".
  std::string return_descriptor;
  std::vector<std::string> param_descriptors;
  std::uint32_t access_flags = 0;
  std::optional<DexCode> code;          // present iff code_off != 0

  bool is_static() const noexcept { return access_flags & kAccStatic; }
  bool has_code() const noexcept { return code.has_value(); }

  // JVM method descriptor, e.g. "([Ljava/lang/String;)V".
  std::string jvm_descriptor() const;
};

struct DexField {
  std::string name;              // e.g. "value"
  std::string type_descriptor;  // e.g. "I", "Ljava/lang/String;"
  std::uint32_t access_flags = 0;
};

struct DexClass {
  std::string descriptor;                   // "Lcom/example/Foo;"
  std::string java_name;                    // "com.example.Foo"
  std::optional<std::string> super_name;    // "java.lang.Object"
  std::uint32_t access_flags = 0;
  std::vector<DexField> static_fields;
  std::vector<DexField> instance_fields;
  std::vector<DexMethod> direct_methods;    // static/private/ctor
  std::vector<DexMethod> virtual_methods;
};

// A resolved method reference (from the method_ids table), for translating
// invoke-* instructions.
struct MethodRef {
  std::string owner_internal;             // e.g. "java/lang/Integer"
  std::string name;                       // e.g. "parseInt"
  std::string descriptor;                 // e.g. "(Ljava/lang/String;)I"
  std::string return_descriptor;          // e.g. "I"
  std::vector<std::string> param_descriptors;
};

// A resolved field reference (from the field_ids table), for sget/sput/etc.
struct FieldRef {
  std::string owner_internal;    // e.g. "java/lang/System"
  std::string name;              // e.g. "out"
  std::string type_descriptor;   // e.g. "Ljava/io/PrintStream;"
};

class DexFile {
 public:
  // Parse `data` (copied in). On failure returns nullopt and fills `out_error`.
  static std::optional<DexFile> parse(const std::uint8_t* data, std::size_t size,
                                      DexError& out_error);
  static std::optional<DexFile> parse(const std::vector<std::uint8_t>& bytes,
                                      DexError& out_error);

  const std::vector<DexClass>& classes() const noexcept { return classes_; }

  // Resolve a method_ids index (as referenced by invoke-* operands), or nullptr.
  const MethodRef* method_ref(std::uint32_t index) const noexcept {
    return index < method_refs_.size() ? &method_refs_[index] : nullptr;
  }

  // Resolve a string_ids index (as referenced by const-string operands), or null.
  const std::string* string_at(std::uint32_t index) const noexcept {
    return index < strings_.size() ? &strings_[index] : nullptr;
  }

  // Resolve a field_ids index (as referenced by sget/sput/etc.), or nullptr.
  const FieldRef* field_ref(std::uint32_t index) const noexcept {
    return index < field_refs_.size() ? &field_refs_[index] : nullptr;
  }

  // Resolve a type_ids index (as referenced by new-instance/etc.), or nullptr.
  const std::string* type_at(std::uint32_t index) const noexcept {
    return index < type_descriptors_.size() ? &type_descriptors_[index] : nullptr;
  }

 private:
  DexFile() = default;
  std::vector<std::uint8_t> data_;
  std::vector<DexClass> classes_;
  std::vector<MethodRef> method_refs_;
  std::vector<FieldRef> field_refs_;
  std::vector<std::string> strings_;
  std::vector<std::string> type_descriptors_;
};

// Convert a type descriptor ("I", "V", "[I", "Lcom/example/Foo;") to a Java
// type name ("int", "void", "int[]", "com.example.Foo").
std::string descriptor_to_java(std::string_view descriptor);

}  // namespace aclcxx::dex

#endif  // ACLCXX_DEX_DEXFILE_H
