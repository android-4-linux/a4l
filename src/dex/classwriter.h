// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/dex.md (pending), JVM Specification chapter 4 (class file
// format), public documentation. No GPL-3.0 sources.
#ifndef ACL_DEX_CLASSWRITER_H
#define ACL_DEX_CLASSWRITER_H

// Minimal writer for a single JVM .class file: constant pool, interfaces,
// fields, and methods (each with a Code attribute + optional StackMapTable).
// Emits class-file version 51 (Java 7) — only what the DEX->JVM translator
// needs.
//
// NOTE: the class file format is big-endian (unlike dex/zip, which are
// little-endian).

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace acl::dex {

class ClassWriter {
 public:
  // Internal ('/'-separated) names, e.g. "com/example/Foo", "java/lang/Object".
  ClassWriter(std::string this_internal, std::string super_internal);

  // Override the class access_flags (default ACC_PUBLIC|ACC_SUPER for a normal
  // class). Interfaces pass ACC_PUBLIC|ACC_INTERFACE|ACC_ABSTRACT (no
  // ACC_SUPER), plus ACC_ANNOTATION for @interface types.
  void set_access_flags(std::uint16_t flags) { access_flags_ = flags; }

  // Constant-pool interning; each returns a 1-based constant-pool index.
  std::uint16_t utf8(std::string_view s);
  std::uint16_t class_ref(std::string_view internal_name);
  std::uint16_t string_const(std::string_view s);
  std::uint16_t integer_const(std::int32_t v);
  std::uint16_t float_const(std::uint32_t raw_bits);   // CONSTANT_Float
  std::uint16_t long_const(std::int64_t v);            // occupies two cp slots
  std::uint16_t double_const(std::uint64_t raw_bits);  // occupies two cp slots
  std::uint16_t methodref(std::string_view cls,
                          std::string_view name,
                          std::string_view desc);
  std::uint16_t interface_methodref(std::string_view cls,
                                    std::string_view name,
                                    std::string_view desc);
  std::uint16_t fieldref(std::string_view cls,
                         std::string_view name,
                         std::string_view desc);

  // Declare a directly-implemented interface (internal name), so casts/invoke-
  // interface against it verify. Emitted in the class file's interfaces array.
  void add_interface(std::string_view internal_name);

  // Add a field declaration.
  void add_field(std::uint16_t access_flags,
                 std::string_view name,
                 std::string_view descriptor);

  // One Code-attribute exception_table entry (catch_type = 0 for catch-all).
  struct ExceptionEntry {
    std::uint16_t start_pc;
    std::uint16_t end_pc;
    std::uint16_t handler_pc;
    std::uint16_t catch_type;
  };

  // Add a method with pre-assembled bytecode. `stackmaptable` is the
  // StackMapTable attribute body (number_of_entries + frames), or empty.
  void add_method(std::uint16_t access_flags,
                  std::string_view name,
                  std::string_view descriptor,
                  std::uint16_t max_stack,
                  std::uint16_t max_locals,
                  std::vector<std::uint8_t> code,
                  std::vector<ExceptionEntry> exceptions = {},
                  std::vector<std::uint8_t> stackmaptable = {});

  // Add an abstract or native method: a declaration with NO Code attribute (for
  // interface methods and abstract classes). `access_flags` must include
  // ACC_ABSTRACT (0x0400) or ACC_NATIVE (0x0100) so build() omits the Code
  // attribute.
  void add_abstract_method(std::uint16_t access_flags,
                           std::string_view name,
                           std::string_view descriptor);

  // Serialize the complete class file.
  std::vector<std::uint8_t> build() const;

 private:
  std::uint16_t add_entry(std::vector<std::uint8_t> cp_info,
                          const std::string& key);
  std::uint16_t name_and_type(std::string_view name, std::string_view desc);

  struct Field {
    std::uint16_t access_flags;
    std::uint16_t name_index;
    std::uint16_t descriptor_index;
  };

  struct Method {
    std::uint16_t access_flags;
    std::uint16_t name_index;
    std::uint16_t descriptor_index;
    std::uint16_t max_stack;
    std::uint16_t max_locals;
    std::vector<std::uint8_t> code;
    std::vector<ExceptionEntry> exceptions;
    std::vector<std::uint8_t> stackmaptable;
  };

  std::vector<std::vector<std::uint8_t>> pool_;  // cp_info entries (1-based)
  std::map<std::string, std::uint16_t> interned_;
  std::uint16_t this_class_;
  std::uint16_t super_class_;
  std::uint16_t access_flags_ =
      0x0021;                        // ACC_PUBLIC | ACC_SUPER (normal class)
  std::uint16_t code_attr_name_;     // utf8 "Code"
  std::uint16_t stackmap_name_ = 0;  // utf8 "StackMapTable" (lazily interned)
  std::vector<std::uint16_t> interfaces_;  // cp Class indices
  std::vector<Field> fields_;
  std::vector<Method> methods_;
};

}  // namespace acl::dex

#endif  // ACL_DEX_CLASSWRITER_H
