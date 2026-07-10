// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/dex.md (pending), Dalvik bytecode reference & JVM Spec ch.6,
// ADR-0001. No GPL-3.0 sources.

#include "dex/dex_to_jvm.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <deque>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

#include "dex/classwriter.h"
#include "log/log.h"

namespace fs = std::filesystem;

namespace aclcxx::dex {
namespace {

constexpr std::string_view kTag = "aclcxx.dex";

constexpr std::uint32_t kAccInterface = 0x0200;
constexpr std::uint32_t kAccAnnotation = 0x2000;

// JVM opcodes used by the translator/stub.
enum : std::uint8_t {
  OP_ICONST_0 = 0x03, OP_BIPUSH = 0x10, OP_SIPUSH = 0x11, OP_LDC = 0x12,
  OP_LDC_W = 0x13, OP_ILOAD = 0x15, OP_ALOAD = 0x19, OP_ISTORE = 0x36,
  OP_ASTORE = 0x3a, OP_DUP = 0x59, OP_IADD = 0x60, OP_ISUB = 0x64,
  OP_IMUL = 0x68, OP_IRETURN = 0xac, OP_ARETURN = 0xb0, OP_RETURN = 0xb1,
  OP_NEW = 0xbb, OP_INVOKESPECIAL = 0xb7, OP_ATHROW = 0xbf, OP_GOTO = 0xa7,
  OP_POP = 0x57, OP_INVOKESTATIC = 0xb8, OP_INVOKEVIRTUAL = 0xb6,
  OP_GETSTATIC = 0xb2, OP_GETFIELD = 0xb4, OP_PUTFIELD = 0xb5,
  OP_IALOAD = 0x2e, OP_IASTORE = 0x4f, OP_NEWARRAY = 0xbc,
  OP_ANEWARRAY = 0xbd, OP_ARRAYLENGTH = 0xbe,
  OP_AALOAD = 0x32, OP_AASTORE = 0x53, OP_PUTSTATIC = 0xb3,
  OP_LCONST_0 = 0x09, OP_LDC2_W = 0x14, OP_LLOAD = 0x16, OP_LSTORE = 0x37,
  OP_LADD = 0x61, OP_LSUB = 0x65, OP_LMUL = 0x69, OP_I2L = 0x85, OP_L2I = 0x88,
  OP_LCMP = 0x94, OP_LRETURN = 0xad,
  OP_INEG = 0x74, OP_ICONST_M1 = 0x02, OP_IXOR = 0x82,
  OP_I2B = 0x91, OP_I2C = 0x92, OP_I2S = 0x93,
  OP_BALOAD = 0x33, OP_CALOAD = 0x34, OP_SALOAD = 0x35,
  OP_BASTORE = 0x54, OP_CASTORE = 0x55, OP_SASTORE = 0x56,
  OP_LNEG = 0x75, OP_LXOR = 0x83, OP_LSHL = 0x79, OP_LSHR = 0x7b,
  OP_LUSHR = 0x7d, OP_LALOAD = 0x2f, OP_LASTORE = 0x50,
  OP_CHECKCAST = 0xc0, OP_INSTANCEOF = 0xc1,
};

// A type index -> the class-name form used by checkcast/instanceof/anewarray:
// "Lfoo;" -> "foo"; "[..." -> the array descriptor itself. Empty if neither.
std::string class_operand(const std::string& desc) {
  if (desc.size() >= 2 && desc.front() == 'L' && desc.back() == ';') {
    return desc.substr(1, desc.size() - 2);
  }
  if (!desc.empty() && desc.front() == '[') return desc;
  return {};
}

// JVM opcode for a Dalvik long NON-shift binary op, index add,sub,mul,div,rem,
// and,or,xor (matches 23x 0x9b.. and /2addr 0xbb..).
std::uint8_t jvm_lbinop(int idx) {
  static const std::uint8_t ops[] = {
      0x61, 0x65, 0x69, 0x6d, 0x71, 0x7f, 0x81, 0x83,
      // ladd lsub lmul ldiv lrem land  lor  lxor
  };
  return (idx >= 0 && idx < 8) ? ops[idx] : 0;
}
// JVM long shift opcode for a shift index 0=shl,1=shr,2=ushr.
std::uint8_t jvm_lshift(int idx) {
  return idx == 0 ? OP_LSHL : idx == 1 ? OP_LSHR : OP_LUSHR;
}

// JVM opcode for a Dalvik int binary op, index in the canonical order
// add,sub,mul,div,rem,and,or,xor,shl,shr,ushr (matches 23x 0x90.. and /2addr 0xb0..).
std::uint8_t jvm_ibinop(int idx) {
  static const std::uint8_t ops[] = {
      0x60, 0x64, 0x68, 0x6c, 0x70,  // iadd isub imul idiv irem
      0x7e, 0x80, 0x82, 0x78, 0x7a, 0x7c,  // iand ior ixor ishl ishr iushr
  };
  return (idx >= 0 && idx < 11) ? ops[idx] : 0;
}

// (JVM array-op, is-object) for a Dalvik aget*/aput* opcode.
std::pair<std::uint8_t, bool> jvm_arrayop(std::uint8_t op) {
  switch (op) {
    case 0x44: return {0x2e, false};  // aget -> iaload
    case 0x46: return {0x32, true};   // aget-object -> aaload
    case 0x47: case 0x48: return {0x33, false};  // aget-boolean/byte -> baload
    case 0x49: return {0x34, false};  // aget-char -> caload
    case 0x4a: return {0x35, false};  // aget-short -> saload
    case 0x4b: return {0x4f, false};  // aput -> iastore
    case 0x4d: return {0x53, true};   // aput-object -> aastore
    case 0x4e: case 0x4f: return {0x54, false};  // aput-boolean/byte -> bastore
    case 0x50: return {0x55, false};  // aput-char -> castore
    case 0x51: return {0x56, false};  // aput-short -> sastore
    default:   return {0, false};
  }
}

// JVM newarray atype codes for primitive element descriptors.
int newarray_atype(char prim) {
  switch (prim) {
    case 'Z': return 4;  case 'C': return 5;  case 'F': return 6;
    case 'D': return 7;  case 'B': return 8;  case 'S': return 9;
    case 'I': return 10; case 'J': return 11;
    default:  return -1;
  }
}

// JVM field access flags that carry over 1:1 from Dalvik field flags.
constexpr std::uint32_t kFieldFlagsMask =
    0x0001 | 0x0002 | 0x0004 | 0x0008 | 0x0010 | 0x0040 | 0x0080 | 0x1000 | 0x4000;

std::string internal_name(const std::string& java) {
  std::string s = java;
  for (char& c : s) {
    if (c == '.') c = '/';
  }
  return s;
}

std::pair<std::string, std::string> split_package(const std::string& java_name) {
  const std::size_t dot = java_name.rfind('.');
  if (dot == std::string::npos) return {"", java_name};
  return {java_name.substr(0, dot), java_name.substr(dot + 1)};
}

bool is_ref_desc(const std::string& d) {
  return !d.empty() && (d[0] == 'L' || d[0] == '[');
}
bool is_int_family_desc(const std::string& d) {
  return d == "I" || d == "Z" || d == "B" || d == "S" || d == "C";
}
bool is_wide_desc(const std::string& d) { return d == "J" || d == "D"; }

int arg_slots(const DexMethod& m) {
  int n = m.is_static() ? 0 : 1;
  for (const std::string& d : m.param_descriptors) n += is_wide_desc(d) ? 2 : 1;
  return n;
}

bool skippable_class(const DexClass& c) {
  return (c.access_flags & (kAccInterface | kAccAnnotation)) != 0;
}

bool emittable_method(const DexMethod& m) {
  if (m.access_flags & (kAccAbstract | kAccNative)) return false;
  if (m.name == "<clinit>") return false;   // static initializer not handled yet
  if (m.name == "<init>") return true;      // constructor: a valid JVM method name
  if (m.name.empty()) return false;
  for (char c : m.name) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$')) {
      return false;
    }
  }
  return true;
}

// Bytecode assembler with conservative stack-height tracking.
struct Emitter {
  ClassWriter& cw;
  std::vector<std::uint8_t> code;
  int stack = 0;
  int max_stack = 0;

  explicit Emitter(ClassWriter& c) : cw(c) {}

  void adj(int d) {
    stack += d;
    if (stack > max_stack) max_stack = stack;
  }
  void op(std::uint8_t o) { code.push_back(o); }
  void u1(std::uint8_t x) { code.push_back(x); }
  void u2(std::uint16_t x) {
    code.push_back(static_cast<std::uint8_t>(x >> 8));
    code.push_back(static_cast<std::uint8_t>(x & 0xff));
  }

  void push_int(std::int32_t v) {
    if (v >= -1 && v <= 5) {
      op(static_cast<std::uint8_t>(OP_ICONST_0 + v));  // iconst_m1..iconst_5
    } else if (v >= -128 && v <= 127) {
      op(OP_BIPUSH); u1(static_cast<std::uint8_t>(v));
    } else if (v >= -32768 && v <= 32767) {
      op(OP_SIPUSH); u2(static_cast<std::uint16_t>(v));
    } else {
      const std::uint16_t idx = cw.integer_const(v);
      if (idx <= 255) { op(OP_LDC); u1(static_cast<std::uint8_t>(idx)); }
      else { op(OP_LDC_W); u2(idx); }
    }
    adj(+1);
  }
  void ldc_string(std::uint16_t idx) {
    if (idx <= 255) { op(OP_LDC); u1(static_cast<std::uint8_t>(idx)); }
    else { op(OP_LDC_W); u2(idx); }
    adj(+1);
  }
  void load_i(int slot) { op(OP_ILOAD); u1(static_cast<std::uint8_t>(slot)); adj(+1); }
  void store_i(int slot) { op(OP_ISTORE); u1(static_cast<std::uint8_t>(slot)); adj(-1); }
  void load_a(int slot) { op(OP_ALOAD); u1(static_cast<std::uint8_t>(slot)); adj(+1); }
  void store_a(int slot) { op(OP_ASTORE); u1(static_cast<std::uint8_t>(slot)); adj(-1); }
  void binop(std::uint8_t o) { op(o); adj(-1); }  // 2 operands -> 1 result
  // Wide (long) helpers: a long occupies two stack/local slots.
  void push_long(std::int64_t v) {
    if (v == 0 || v == 1) { op(static_cast<std::uint8_t>(OP_LCONST_0 + v)); }
    else { op(OP_LDC2_W); u2(cw.long_const(v)); }
    adj(+2);
  }
  void load_l(int slot) { op(OP_LLOAD); u1(static_cast<std::uint8_t>(slot)); adj(+2); }
  void store_l(int slot) { op(OP_LSTORE); u1(static_cast<std::uint8_t>(slot)); adj(-2); }
  void lbinop(std::uint8_t o) { op(o); adj(-2); }  // 2 longs -> 1 long
};

struct Translated {
  std::vector<std::uint8_t> code;
  std::uint16_t max_stack;
  std::uint16_t max_locals;
  std::vector<ClassWriter::ExceptionEntry> exceptions;
  std::vector<std::uint8_t> stackmaptable;  // StackMapTable attribute body (v51)
};

// A JVM verification type for one local slot. Long occupies two slots: the low
// slot is Long, the high slot LongHi (skipped when serialized).
struct VType {
  enum Kind { Top, Int, Long, LongHi, Obj } kind = Top;
  std::string cls;  // internal name when kind == Obj
  bool operator==(const VType& o) const { return kind == o.kind && cls == o.cls; }
};

// Object-typed field/element/return descriptor -> internal class name (or "" if
// not a plain object type).
std::string obj_internal(const std::string& desc) {
  if (desc.size() >= 2 && desc.front() == 'L' && desc.back() == ';') {
    return desc.substr(1, desc.size() - 2);
  }
  if (!desc.empty() && desc.front() == '[') return desc;  // array class name
  return {};
}

// Number of 16-bit code units of a supported Dalvik instruction (used by the
// stack-map forward pass, which only ever sees already-translatable opcodes).
int dalvik_insn_units(std::uint8_t op) {
  if (op == 0x18) return 5;                                        // const-wide
  if (op == 0x14 || op == 0x17 || op == 0x6e || op == 0x70 || op == 0x71) {
    return 3;  // const, const-wide/32, invoke-{virtual,direct,static}
  }
  if (op == 0x13 || op == 0x15 || op == 0x16 || op == 0x19 || op == 0x1a ||
      op == 0x1f || op == 0x20 ||    // check-cast, instance-of
      op == 0x22 || op == 0x23 || op == 0x29 || op == 0x31 ||
      (op >= 0x32 && op <= 0x3d) ||   // if-test / if-testz
      (op >= 0x44 && op <= 0x6d) ||   // aget/aput, iget/iput, sget/sput (all typed)
      (op >= 0x90 && op <= 0xa5) ||   // int/long binary ops (23x)
      (op >= 0xd0 && op <= 0xe2)) {   // int op/lit16, op/lit8
    return 2;
  }
  return 1;  // moves, const/4, returns, gotos, /2addr, unary, nop, etc.
}

// Update the local-variable types for one instruction's writes.
void sm_transfer(std::vector<VType>& locals, const DexFile& dex, const DexMethod& m,
                 const std::vector<std::uint16_t>& in, std::size_t pc, int ins) {
  const auto L = [&](int reg) { return ins + reg; };
  const auto set = [&](int slot, VType t) {
    if (slot >= 0 && slot < static_cast<int>(locals.size())) locals[slot] = t;
  };
  const auto set_long = [&](int slot) {
    set(slot, {VType::Long, {}});
    set(slot + 1, {VType::LongHi, {}});
  };
  const std::uint16_t u = in[pc];
  const std::uint8_t op = u & 0xff;
  const VType kInt{VType::Int, {}};
  switch (op) {
    case 0x12: case 0x13: case 0x14: case 0x15:  // const* -> int
      set(L((u >> 8) & (op == 0x12 ? 0xf : 0xff)), kInt); break;
    case 0x16: case 0x17: case 0x18: case 0x19:  // const-wide* -> long
      set_long(L((u >> 8) & 0xff)); break;
    case 0x1a:  // const-string -> String
      set(L((u >> 8) & 0xff), {VType::Obj, "java/lang/String"}); break;
    case 0x01: case 0x07: {  // move / move-object: copy
      const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
      set(L(a), locals[L(b)]); break;
    }
    case 0x04: {  // move-wide
      const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
      (void)b; set_long(L(a)); break;
    }
    case 0x22: {  // new-instance -> Obj (uninitialized elided; no frame between new/<init>)
      const std::string* td = dex.type_at(in[pc + 1]);
      set(L((u >> 8) & 0xff), {VType::Obj, td ? obj_internal(*td) : ""}); break;
    }
    case 0x23: {  // new-array vA, vB, type -> vA = Obj(array)
      const std::string* td = dex.type_at(in[pc + 1]);
      set(L((u >> 8) & 0xf), {VType::Obj, td ? *td : ""}); break;
    }
    case 0x21: set(L((u >> 8) & 0xf), kInt); break;         // array-length -> int
    case 0x0a: set(L((u >> 8) & 0xff), kInt); break;        // move-result -> int
    case 0x0b: set_long(L((u >> 8) & 0xff)); break;         // move-result-wide
    case 0x0c: {  // move-result-object -> Obj(return type of the preceding invoke)
      std::string cls;
      if (pc >= 3) {
        const MethodRef* mr = dex.method_ref(in[pc - 2]);
        if (mr) cls = obj_internal(mr->return_descriptor);
      }
      set(L((u >> 8) & 0xff), {VType::Obj, cls}); break;
    }
    case 0x0d: {  // move-exception -> Obj(caught type at this handler)
      std::string cls = "java/lang/Throwable";
      for (const DexTryHandler& h : m.code->handlers) {
        if (h.handler_addr == pc && !h.catch_all) {
          const std::string* td = dex.type_at(h.type_idx);
          if (td) cls = obj_internal(*td);
        }
      }
      set(L((u >> 8) & 0xff), {VType::Obj, cls}); break;
    }
    case 0x52: case 0x55: case 0x56: case 0x57: case 0x58:  // iget int/narrow -> int
      set(L((u >> 8) & 0xf), kInt); break;
    case 0x60: case 0x63: case 0x64: case 0x65: case 0x66:  // sget int/narrow -> int
      set(L((u >> 8) & 0xff), kInt); break;
    case 0x54: case 0x62: {  // iget-object / sget-object -> Obj(field type)
      const FieldRef* fr = dex.field_ref(in[pc + 1]);
      set(L((u >> 8) & (op == 0x54 ? 0xf : 0xff)),
          {VType::Obj, fr ? obj_internal(fr->type_descriptor) : ""}); break;
    }
    case 0x44: case 0x47: case 0x48: case 0x49: case 0x4a:  // aget int/narrow -> int
      set(L((u >> 8) & 0xff), kInt); break;
    case 0x46: {  // aget-object -> Obj(element of the array local)
      const int bb = in[pc + 1] & 0xff;
      const VType& arr = locals[L(bb)];
      std::string cls;
      if (arr.kind == VType::Obj && !arr.cls.empty() && arr.cls.front() == '[') {
        cls = obj_internal(arr.cls.substr(1));
      }
      set(L((u >> 8) & 0xff), {VType::Obj, cls}); break;
    }
    case 0x81: set_long(L((u >> 8) & 0xf)); break;          // int-to-long
    case 0x84: set(L((u >> 8) & 0xf), kInt); break;         // long-to-int
    case 0x31: set(L((u >> 8) & 0xff), kInt); break;        // cmp-long -> int
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94:  // int binary 23x -> int
    case 0x95: case 0x96: case 0x97: case 0x98: case 0x99: case 0x9a:
      set(L((u >> 8) & 0xff), kInt); break;
    case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f:  // long binary 23x -> long
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5:
    case 0x45:                                              // aget-wide -> long
    case 0x61:                                              // sget-wide -> long
      set_long(L((u >> 8) & 0xff)); break;
    case 0x53: set_long(L((u >> 8) & 0xf)); break;          // iget-wide -> long
    case 0x20: set(L((u >> 8) & 0xf), kInt); break;         // instance-of -> int
    case 0x1f: {  // check-cast -> Obj(cast type)
      const std::string* td = dex.type_at(in[pc + 1]);
      set(L((u >> 8) & 0xff), {VType::Obj, td ? class_operand(*td) : ""}); break;
    }
    case 0x7b: case 0x7c: case 0x8d: case 0x8e: case 0x8f:  // neg/not/int-to-{b,c,s}
      set(L((u >> 8) & 0xf), kInt); break;
    case 0x7d: case 0x7e:                                   // neg/not-long -> long
      set_long(L((u >> 8) & 0xf)); break;
    default:
      if (op >= 0xb0 && op <= 0xba) {                       // int op/2addr -> int
        set(L((u >> 8) & 0xf), kInt);
      } else if (op >= 0xbb && op <= 0xc5) {                // long op/2addr -> long
        set_long(L((u >> 8) & 0xf));
      } else if (op >= 0xd0 && op <= 0xe2) {                // int op/lit -> int
        set(L(op >= 0xd8 ? (u >> 8) & 0xff : (u >> 8) & 0xf), kInt);
      }
      break;  // otherwise no local write
  }
}

// Serialize one full_frame's verification_type_info list for `slots`, trimming
// trailing Top entries. Returns (bytes, entry_count).
std::pair<std::vector<std::uint8_t>, std::uint16_t> encode_vtypes(
    ClassWriter& cw, const std::vector<VType>& slots) {
  int last = -1;
  for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
    if (slots[i].kind != VType::Top && slots[i].kind != VType::LongHi) last = i;
  }
  std::vector<std::uint8_t> out;
  std::uint16_t count = 0;
  for (int i = 0; i <= last; ++i) {
    ++count;
    switch (slots[i].kind) {
      case VType::Int:    out.push_back(1); break;
      case VType::Long:   out.push_back(4); ++i; break;  // skip the LongHi slot
      case VType::Obj: {
        out.push_back(7);
        const std::uint16_t ci = cw.class_ref(slots[i].cls);
        out.push_back(static_cast<std::uint8_t>(ci >> 8));
        out.push_back(static_cast<std::uint8_t>(ci & 0xff));
        break;
      }
      default:            out.push_back(0); break;  // Top
    }
  }
  return {out, count};
}

// Translate the supported linear integer subset of a static method's Dalvik
// bytecode. Returns nullopt (caller stubs the method) on any unsupported
// opcode, type, or register width.
std::optional<Translated> translate_method(ClassWriter& cw, const DexFile& dex,
                                           const DexMethod& m) {
  if (!m.has_code()) return std::nullopt;
  const DexCode& c = *m.code;
  const int ins = c.ins_size;
  const int regs = c.registers_size;
  const int max_locals = ins + regs;
  if (max_locals > 255) return std::nullopt;

  const std::string& ret = m.return_descriptor;
  const bool ret_void = ret == "V";
  const bool ret_int = is_int_family_desc(ret);
  const bool ret_ref = is_ref_desc(ret);
  if (!ret_void && !ret_int && !ret_ref) return std::nullopt;

  // Incoming-argument reference-ness, in order: [this?] then params. Wide args
  // are not handled.
  std::vector<bool> arg_is_ref;
  if (!m.is_static()) arg_is_ref.push_back(true);  // this
  for (const std::string& d : m.param_descriptors) {
    if (is_wide_desc(d)) return std::nullopt;
    if (is_ref_desc(d)) arg_is_ref.push_back(true);
    else if (is_int_family_desc(d)) arg_is_ref.push_back(false);
    else return std::nullopt;
  }
  if (static_cast<int>(arg_is_ref.size()) != ins) return std::nullopt;

  Emitter e(cw);
  const auto local = [&](int reg) { return ins + reg; };  // Dalvik reg -> JVM local

  // Prologue: incoming JVM args (locals 0..ins-1) -> Dalvik param registers
  // (which live at JVM locals regs..regs+ins-1). Copy in declaration order.
  for (int i = 0; i < ins; ++i) {
    const int dst = regs + i;
    if (arg_is_ref[static_cast<std::size_t>(i)]) { e.load_a(i); e.store_a(dst); }
    else { e.load_i(i); e.store_i(dst); }
  }

  // Branch handling: record every Dalvik pc -> JVM code offset, and each branch's
  // JVM operand site + Dalvik target, then patch relative offsets in a second
  // pass (targets may be forward).
  std::map<std::uint32_t, std::size_t> pc_to_jvm;
  struct Fixup { std::size_t branch_at; std::uint32_t target_pc; };
  std::vector<Fixup> fixups;
  const auto branch_to = [&](std::uint8_t jvm_op, std::uint32_t target) {
    const std::size_t at = e.code.size();
    e.op(jvm_op);
    e.u2(0);  // placeholder, patched below
    fixups.push_back({at, target});
  };

  // Catch-handler entry points: the JVM enters these with the exception on the
  // operand stack, so reset the tracked height to 1 (move-exception then pops it).
  std::set<std::uint32_t> handler_pcs;
  for (const DexTryHandler& h : c.handlers) handler_pcs.insert(h.handler_addr);

  const std::vector<std::uint16_t>& in = c.insns;
  std::size_t pc = 0;
  while (pc < in.size()) {
    pc_to_jvm[static_cast<std::uint32_t>(pc)] = e.code.size();
    if (handler_pcs.count(static_cast<std::uint32_t>(pc))) {
      e.stack = 1;
      if (e.max_stack < 1) e.max_stack = 1;
    }
    const std::uint16_t u = in[pc];
    const std::uint8_t opcode = u & 0xff;
    const auto rel_target = [&](std::int64_t off) {
      return static_cast<std::uint32_t>(static_cast<std::int64_t>(pc) + off);
    };
    switch (opcode) {
      case 0x0d: {  // move-exception vAA (11x): the handler's exception -> local
        const int a = (u >> 8) & 0xff;
        e.store_a(local(a));  // pops the exception the JVM pushed on handler entry
        pc += 1;
        break;
      }
      case 0x27: {  // throw vAA (11x)
        const int a = (u >> 8) & 0xff;
        e.load_a(local(a));
        e.op(OP_ATHROW); e.adj(-1);
        pc += 1;
        break;
      }
      case 0x00:  // nop
        pc += 1;
        break;
      case 0x01: {  // move vA, vB (12x)
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_i(local(b)); e.store_i(local(a));
        pc += 1;
        break;
      }
      case 0x07: {  // move-object vA, vB (12x)
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_a(local(b)); e.store_a(local(a));
        pc += 1;
        break;
      }
      case 0x0e:  // return-void (10x)
        e.op(OP_RETURN);
        pc += 1;
        break;
      case 0x0f: {  // return vAA (11x)
        const int reg = (u >> 8) & 0xff;
        e.load_i(local(reg)); e.op(OP_IRETURN); e.adj(-1);
        pc += 1;
        break;
      }
      case 0x11: {  // return-object vAA (11x)
        const int reg = (u >> 8) & 0xff;
        e.load_a(local(reg)); e.op(OP_ARETURN); e.adj(-1);
        pc += 1;
        break;
      }
      case 0x12: {  // const/4 vA, #+B (11n)
        const int a = (u >> 8) & 0xf;
        int lit = (u >> 12) & 0xf;
        if (lit & 0x8) lit -= 16;  // sign-extend 4 bits
        e.push_int(lit); e.store_i(local(a));
        pc += 1;
        break;
      }
      case 0x13: {  // const/16 vAA, #+BBBB (21s)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const std::int32_t lit = static_cast<std::int16_t>(in[pc + 1]);
        e.push_int(lit); e.store_i(local(a));
        pc += 2;
        break;
      }
      case 0x14: {  // const vAA, #+BBBBBBBB (31i)
        if (pc + 2 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const std::int32_t lit = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(in[pc + 1]) |
            (static_cast<std::uint32_t>(in[pc + 2]) << 16));
        e.push_int(lit); e.store_i(local(a));
        pc += 3;
        break;
      }
      case 0x15: {  // const/high16 vAA, #+BBBB0000 (21h)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const std::int32_t lit =
            static_cast<std::int32_t>(static_cast<std::uint32_t>(in[pc + 1]) << 16);
        e.push_int(lit); e.store_i(local(a));
        pc += 2;
        break;
      }
      case 0x16: {  // const-wide/16 vAA, #+BBBB (21s) - long
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        e.push_long(static_cast<std::int16_t>(in[pc + 1]));
        e.store_l(local(a));
        pc += 2;
        break;
      }
      case 0x17: {  // const-wide/32 vAA, #+BBBBBBBB (31i) - long
        if (pc + 2 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const std::int32_t lo32 = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(in[pc + 1]) |
            (static_cast<std::uint32_t>(in[pc + 2]) << 16));
        e.push_long(lo32);
        e.store_l(local(a));
        pc += 3;
        break;
      }
      case 0x18: {  // const-wide vAA, #+BBBB...(64) (51l) - long
        if (pc + 4 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        std::uint64_t lit = 0;
        for (int k = 0; k < 4; ++k) {
          lit |= static_cast<std::uint64_t>(in[pc + 1 + k]) << (16 * k);
        }
        e.push_long(static_cast<std::int64_t>(lit));
        e.store_l(local(a));
        pc += 5;
        break;
      }
      case 0x19: {  // const-wide/high16 vAA, #+BBBB000000000000 (21h) - long
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        e.push_long(static_cast<std::int64_t>(in[pc + 1]) << 48);
        e.store_l(local(a));
        pc += 2;
        break;
      }
      case 0x04: {  // move-wide vA, vB (12x) - long
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_l(local(b)); e.store_l(local(a));
        pc += 1;
        break;
      }
      case 0x10: {  // return-wide vAA (11x) - long
        const int a = (u >> 8) & 0xff;
        e.load_l(local(a)); e.op(OP_LRETURN); e.adj(-2);
        pc += 1;
        break;
      }
      case 0x81: {  // int-to-long vA, vB (12x)
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_i(local(b)); e.op(OP_I2L); e.adj(+1); e.store_l(local(a));
        pc += 1;
        break;
      }
      case 0x84: {  // long-to-int vA, vB (12x)
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_l(local(b)); e.op(OP_L2I); e.adj(-1); e.store_i(local(a));
        pc += 1;
        break;
      }
      case 0x31: {  // cmp-long vAA, vBB, vCC (23x) -> int
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const int bb = in[pc + 1] & 0xff, cc = (in[pc + 1] >> 8) & 0xff;
        e.load_l(local(bb)); e.load_l(local(cc));
        e.op(OP_LCMP); e.adj(-3);  // 2 longs (4) -> 1 int (1)
        e.store_i(local(a));
        pc += 2;
        break;
      }
      case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f:  // long binary (23x)
      case 0xa0: case 0xa1: case 0xa2:                        //   add..xor
      case 0xa3: case 0xa4: case 0xa5: {                      //   shl,shr,ushr
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const int bb = in[pc + 1] & 0xff, cc = (in[pc + 1] >> 8) & 0xff;
        if (opcode >= 0xa3) {  // shift: value is long, amount is int
          e.load_l(local(bb)); e.load_i(local(cc));
          e.op(jvm_lshift(opcode - 0xa3)); e.adj(-1);  // long(2)+int(1) -> long(2)
        } else {
          e.load_l(local(bb)); e.load_l(local(cc));
          e.lbinop(jvm_lbinop(opcode - 0x9b));
        }
        e.store_l(local(a));
        pc += 2;
        break;
      }
      case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:  // long op/2addr (12x)
      case 0xc0: case 0xc1: case 0xc2: case 0xc3: case 0xc4: case 0xc5: {
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        if (opcode >= 0xc3) {  // shift: amount is int
          e.load_l(local(a)); e.load_i(local(b));
          e.op(jvm_lshift(opcode - 0xc3)); e.adj(-1);
        } else {
          e.load_l(local(a)); e.load_l(local(b));
          e.lbinop(jvm_lbinop(opcode - 0xbb));
        }
        e.store_l(local(a));
        pc += 1;
        break;
      }
      case 0x7d: {  // neg-long (12x)
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_l(local(b)); e.op(OP_LNEG); e.store_l(local(a));
        pc += 1;
        break;
      }
      case 0x7e: {  // not-long (12x): ~x = x ^ -1L
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_l(local(b)); e.push_long(-1); e.op(OP_LXOR); e.adj(-2);
        e.store_l(local(a));
        pc += 1;
        break;
      }
      case 0x90: case 0x91: case 0x92: case 0x93: case 0x94:  // int binary op (23x)
      case 0x95: case 0x96: case 0x97: case 0x98: case 0x99: case 0x9a: {
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const int bb = in[pc + 1] & 0xff;
        const int cc = (in[pc + 1] >> 8) & 0xff;
        e.load_i(local(bb)); e.load_i(local(cc));
        e.binop(jvm_ibinop(opcode - 0x90));
        e.store_i(local(a));
        pc += 2;
        break;
      }
      case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4:  // int op/2addr (12x)
      case 0xb5: case 0xb6: case 0xb7: case 0xb8: case 0xb9: case 0xba: {
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_i(local(a)); e.load_i(local(b));
        e.binop(jvm_ibinop(opcode - 0xb0));
        e.store_i(local(a));
        pc += 1;
        break;
      }
      case 0xd0: case 0xd1: case 0xd2: case 0xd3:  // int op/lit16 (22s)
      case 0xd4: case 0xd5: case 0xd6: case 0xd7:
      case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc:  // int op/lit8 (22b)
      case 0xdd: case 0xde: case 0xdf: case 0xe0: case 0xe1: case 0xe2: {
        if (pc + 1 >= in.size()) return std::nullopt;
        const bool lit8 = opcode >= 0xd8;
        const int a = lit8 ? (u >> 8) & 0xff : (u >> 8) & 0xf;
        const int b = lit8 ? in[pc + 1] & 0xff : (u >> 12) & 0xf;
        const std::int32_t lit = lit8 ? static_cast<std::int8_t>((in[pc + 1] >> 8) & 0xff)
                                      : static_cast<std::int16_t>(in[pc + 1]);
        const int idx = lit8 ? opcode - 0xd8 : opcode - 0xd0;
        if (idx == 1) {  // rsub: vA = lit - vB
          e.push_int(lit); e.load_i(local(b)); e.binop(OP_ISUB);
        } else {
          e.load_i(local(b)); e.push_int(lit); e.binop(jvm_ibinop(idx));
        }
        e.store_i(local(a));
        pc += 2;
        break;
      }
      case 0x7b: {  // neg-int (12x)
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_i(local(b)); e.op(OP_INEG); e.store_i(local(a));
        pc += 1;
        break;
      }
      case 0x7c: {  // not-int (12x): ~x = x ^ -1
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_i(local(b)); e.op(OP_ICONST_M1); e.adj(+1);
        e.op(OP_IXOR); e.adj(-1); e.store_i(local(a));
        pc += 1;
        break;
      }
      case 0x8d: case 0x8e: case 0x8f: {  // int-to-byte/char/short (12x)
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_i(local(b));
        e.op(opcode == 0x8d ? OP_I2B : opcode == 0x8e ? OP_I2C : OP_I2S);
        e.store_i(local(a));
        pc += 1;
        break;
      }
      case 0x28: {  // goto +AA (10t)
        const std::int32_t off = static_cast<std::int8_t>((u >> 8) & 0xff);
        branch_to(OP_GOTO, rel_target(off));
        pc += 1;
        break;
      }
      case 0x29: {  // goto/16 +AAAA (20t)
        if (pc + 1 >= in.size()) return std::nullopt;
        const std::int32_t off = static_cast<std::int16_t>(in[pc + 1]);
        branch_to(OP_GOTO, rel_target(off));
        pc += 2;
        break;
      }
      case 0x32: case 0x33: case 0x34:
      case 0x35: case 0x36: case 0x37: {  // if-test vA, vB, +CCCC (22t)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        const std::int32_t off = static_cast<std::int16_t>(in[pc + 1]);
        e.load_i(local(a)); e.load_i(local(b));
        // if-eq..if-le (0x32..0x37) -> if_icmpeq..if_icmple (0x9f..0xa4)
        const std::uint8_t jvm_op = static_cast<std::uint8_t>(0x9f + (opcode - 0x32));
        e.adj(-2);
        branch_to(jvm_op, rel_target(off));
        pc += 2;
        break;
      }
      case 0x0a: {  // move-result vAA (11x): capture preceding invoke's int result
        const int a = (u >> 8) & 0xff;
        e.store_i(local(a));  // pops the value the invoke left on the stack
        pc += 1;
        break;
      }
      case 0x1a: {  // const-string vAA, string@BBBB (21c)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const std::string* s = dex.string_at(in[pc + 1]);
        if (s == nullptr) return std::nullopt;
        e.ldc_string(cw.string_const(*s));
        e.store_a(local(a));
        pc += 2;
        break;
      }
      case 0x60: case 0x63: case 0x64: case 0x65: case 0x66:  // sget/-bool/byte/char/short
      case 0x62:                                               // sget-object
      case 0x67: case 0x6a: case 0x6b: case 0x6c: case 0x6d:  // sput/-bool/byte/char/short
      case 0x69: {  // sput-object
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const FieldRef* fr = dex.field_ref(in[pc + 1]);
        if (fr == nullptr) return std::nullopt;
        const bool is_object = opcode == 0x62 || opcode == 0x69;
        const bool is_get = opcode == 0x62 || (opcode >= 0x60 && opcode <= 0x66);
        if (is_object ? !is_ref_desc(fr->type_descriptor)
                      : !is_int_family_desc(fr->type_descriptor)) {
          return std::nullopt;
        }
        const std::uint16_t ref =
            cw.fieldref(fr->owner_internal, fr->name, fr->type_descriptor);
        if (is_get) {
          e.op(OP_GETSTATIC); e.u2(ref); e.adj(+1);
          if (is_object) e.store_a(local(a)); else e.store_i(local(a));
        } else {
          if (is_object) e.load_a(local(a)); else e.load_i(local(a));
          e.op(OP_PUTSTATIC); e.u2(ref); e.adj(-1);
        }
        pc += 2;
        break;
      }
      case 0x22: {  // new-instance vAA, type@BBBB (21c)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const std::string* td = dex.type_at(in[pc + 1]);
        if (td == nullptr || td->size() < 2 || td->front() != 'L' ||
            td->back() != ';') {
          return std::nullopt;  // only plain object types
        }
        e.op(OP_NEW);
        e.u2(cw.class_ref(td->substr(1, td->size() - 2)));
        e.adj(+1);
        e.store_a(local(a));
        pc += 2;
        break;
      }
      case 0x1f: {  // check-cast vAA, type@BBBB (21c)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const std::string* td = dex.type_at(in[pc + 1]);
        if (td == nullptr) return std::nullopt;
        const std::string cls = class_operand(*td);
        if (cls.empty()) return std::nullopt;
        e.load_a(local(a));
        e.op(OP_CHECKCAST); e.u2(cw.class_ref(cls));  // net 0
        e.store_a(local(a));
        pc += 2;
        break;
      }
      case 0x20: {  // instance-of vA, vB, type@CCCC (22c)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        const std::string* td = dex.type_at(in[pc + 1]);
        if (td == nullptr) return std::nullopt;
        const std::string cls = class_operand(*td);
        if (cls.empty()) return std::nullopt;
        e.load_a(local(b));
        e.op(OP_INSTANCEOF); e.u2(cw.class_ref(cls));  // ref -> int, net 0
        e.store_i(local(a));
        pc += 2;
        break;
      }
      case 0x53: case 0x5a: {  // iget-wide / iput-wide (22c) - long field only
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        const FieldRef* fr = dex.field_ref(in[pc + 1]);
        if (fr == nullptr || fr->type_descriptor != "J") return std::nullopt;
        const std::uint16_t ref =
            cw.fieldref(fr->owner_internal, fr->name, fr->type_descriptor);
        if (opcode == 0x53) {  // vA(wide) = vB.field
          e.load_a(local(b));
          e.op(OP_GETFIELD); e.u2(ref); e.adj(+1);  // obj(1) -> long(2)
          e.store_l(local(a));
        } else {               // vB.field = vA(wide)
          e.load_a(local(b)); e.load_l(local(a));
          e.op(OP_PUTFIELD); e.u2(ref); e.adj(-3);
        }
        pc += 2;
        break;
      }
      case 0x61: case 0x68: {  // sget-wide / sput-wide (21c) - long field only
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const FieldRef* fr = dex.field_ref(in[pc + 1]);
        if (fr == nullptr || fr->type_descriptor != "J") return std::nullopt;
        const std::uint16_t ref =
            cw.fieldref(fr->owner_internal, fr->name, fr->type_descriptor);
        if (opcode == 0x61) {
          e.op(OP_GETSTATIC); e.u2(ref); e.adj(+2); e.store_l(local(a));
        } else {
          e.load_l(local(a)); e.op(OP_PUTSTATIC); e.u2(ref); e.adj(-2);
        }
        pc += 2;
        break;
      }
      case 0x21: {  // array-length vA, vB (12x)
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_a(local(b));
        e.op(OP_ARRAYLENGTH);  // pop arrayref, push int (net 0)
        e.store_i(local(a));
        pc += 1;
        break;
      }
      case 0x23: {  // new-array vA, vB, type@CCCC (22c)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        const std::string* td = dex.type_at(in[pc + 1]);
        if (td == nullptr || td->size() < 2 || td->front() != '[') {
          return std::nullopt;
        }
        const std::string elem = td->substr(1);  // one dimension off
        e.load_i(local(b));                        // size
        if (elem.size() == 1) {
          const int atype = newarray_atype(elem[0]);
          if (atype < 0) return std::nullopt;
          e.op(OP_NEWARRAY); e.u1(static_cast<std::uint8_t>(atype));
        } else if (elem[0] == 'L' && elem.back() == ';') {
          e.op(OP_ANEWARRAY);
          e.u2(cw.class_ref(elem.substr(1, elem.size() - 2)));
        } else if (elem[0] == '[') {
          e.op(OP_ANEWARRAY); e.u2(cw.class_ref(elem));  // array of arrays
        } else {
          return std::nullopt;
        }
        // size popped, arrayref pushed (net 0 on stack); then store.
        e.store_a(local(a));
        pc += 2;
        break;
      }
      case 0x45:    // aget-wide vAA, vBB, vCC (23x) - long[] (double[] stubs elsewhere)
      case 0x4c: {  // aput-wide vAA, vBB, vCC (23x)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const int bb = in[pc + 1] & 0xff, cc = (in[pc + 1] >> 8) & 0xff;
        if (opcode == 0x45) {  // vAA = vBB[vCC]
          e.load_a(local(bb)); e.load_i(local(cc));
          e.op(OP_LALOAD); e.adj(0);  // array(1)+index(1) -> long(2): net 0
          e.store_l(local(a));
        } else {               // vBB[vCC] = vAA
          e.load_a(local(bb)); e.load_i(local(cc)); e.load_l(local(a));
          e.op(OP_LASTORE); e.adj(-4);  // pop array+index+long
        }
        pc += 2;
        break;
      }
      case 0x44: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a:  // aget*
      case 0x4b: case 0x4d: case 0x4e: case 0x4f: case 0x50: case 0x51: {  // aput*
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const int bb = in[pc + 1] & 0xff;         // array
        const int cc = (in[pc + 1] >> 8) & 0xff;  // index
        const bool is_get = opcode <= 0x4a;       // aget family = 0x44..0x4a
        const auto [array_op, is_object] = jvm_arrayop(opcode);
        if (is_get) {                              // vAA = vBB[vCC]
          e.load_a(local(bb)); e.load_i(local(cc));
          e.op(array_op); e.adj(-1);
          if (is_object) e.store_a(local(a)); else e.store_i(local(a));
        } else {                                   // vBB[vCC] = vAA
          e.load_a(local(bb)); e.load_i(local(cc));
          if (is_object) e.load_a(local(a)); else e.load_i(local(a));
          e.op(array_op); e.adj(-3);
        }
        pc += 2;
        break;
      }
      case 0x52: case 0x55: case 0x56: case 0x57: case 0x58:  // iget/-bool/byte/char/short
      case 0x54:                                               // iget-object
      case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f:  // iput/-bool/byte/char/short
      case 0x5b: {  // iput-object
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        const FieldRef* fr = dex.field_ref(in[pc + 1]);
        if (fr == nullptr) return std::nullopt;
        const bool is_object = opcode == 0x54 || opcode == 0x5b;
        const bool is_get = opcode == 0x54 || (opcode >= 0x52 && opcode <= 0x58);
        if (is_object ? !is_ref_desc(fr->type_descriptor)
                      : !is_int_family_desc(fr->type_descriptor)) {
          return std::nullopt;
        }
        const std::uint16_t ref =
            cw.fieldref(fr->owner_internal, fr->name, fr->type_descriptor);
        if (is_get) {                    // vA = vB.field
          e.load_a(local(b));
          e.op(OP_GETFIELD); e.u2(ref);  // pop obj, push value (net 0)
          if (is_object) e.store_a(local(a)); else e.store_i(local(a));
        } else {                         // vB.field = vA
          e.load_a(local(b));
          if (is_object) e.load_a(local(a)); else e.load_i(local(a));
          e.op(OP_PUTFIELD); e.u2(ref); e.adj(-2);
        }
        pc += 2;
        break;
      }
      case 0x6e:    // invoke-virtual {vC..vG}, meth@BBBB (35c)
      case 0x70:    // invoke-direct  {vC..vG}, meth@BBBB (35c) - <init>/private
      case 0x71: {  // invoke-static  {vC..vG}, meth@BBBB (35c)
        if (pc + 2 >= in.size()) return std::nullopt;
        const bool is_static_call = opcode == 0x71;
        const bool has_receiver = !is_static_call;  // virtual and direct
        const std::uint8_t invoke_op = is_static_call ? OP_INVOKESTATIC
                                       : opcode == 0x70 ? OP_INVOKESPECIAL
                                                        : OP_INVOKEVIRTUAL;
        const int argc = (u >> 12) & 0xf;  // includes receiver when present
        if (has_receiver && argc < 1) return std::nullopt;
        const std::uint32_t midx = in[pc + 1];
        const std::uint16_t rw = in[pc + 2];
        const int arg_reg[5] = {rw & 0xf, (rw >> 4) & 0xf, (rw >> 8) & 0xf,
                                (rw >> 12) & 0xf, (u >> 8) & 0xf};
        const MethodRef* mr = dex.method_ref(midx);
        if (mr == nullptr) return std::nullopt;
        const int nparams = static_cast<int>(mr->param_descriptors.size());
        if (nparams != argc - (has_receiver ? 1 : 0)) return std::nullopt;

        int reg = 0;
        if (has_receiver) e.load_a(local(arg_reg[reg++]));  // receiver
        for (int i = 0; i < nparams; ++i, ++reg) {
          const std::string& d = mr->param_descriptors[static_cast<std::size_t>(i)];
          if (is_wide_desc(d)) return std::nullopt;
          if (is_ref_desc(d)) e.load_a(local(arg_reg[reg]));
          else if (is_int_family_desc(d)) e.load_i(local(arg_reg[reg]));
          else return std::nullopt;
        }
        if (is_wide_desc(mr->return_descriptor)) return std::nullopt;
        const int retw = mr->return_descriptor == "V" ? 0 : 1;
        const std::uint16_t ref =
            cw.methodref(mr->owner_internal, mr->name, mr->descriptor);
        e.op(invoke_op);
        e.u2(ref);
        e.adj(-argc + retw);  // pops receiver+params, pushes return
        // Discard the result if not captured by a following move-result.
        if (retw > 0) {
          const bool next_move_result =
              (pc + 3 < in.size()) &&
              ((in[pc + 3] & 0xff) == 0x0a || (in[pc + 3] & 0xff) == 0x0c);
          if (!next_move_result) { e.op(OP_POP); e.adj(-1); }
        }
        pc += 3;
        break;
      }
      case 0x38: case 0x39: case 0x3a:
      case 0x3b: case 0x3c: case 0x3d: {  // if-testz vAA, +BBBB (21t)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const std::int32_t off = static_cast<std::int16_t>(in[pc + 1]);
        e.load_i(local(a));
        // if-eqz..if-lez (0x38..0x3d) -> ifeq..ifle (0x99..0x9e)
        const std::uint8_t jvm_op = static_cast<std::uint8_t>(0x99 + (opcode - 0x38));
        e.adj(-1);
        branch_to(jvm_op, rel_target(off));
        pc += 2;
        break;
      }
      default:
        return std::nullopt;  // unsupported opcode -> stub the method
    }
  }

  // Resolve branch targets to relative JVM offsets (from the branch opcode).
  for (const Fixup& fx : fixups) {
    const auto it = pc_to_jvm.find(fx.target_pc);
    if (it == pc_to_jvm.end()) return std::nullopt;  // target not an instruction
    const std::int64_t rel =
        static_cast<std::int64_t>(it->second) - static_cast<std::int64_t>(fx.branch_at);
    if (rel < -32768 || rel > 32767) return std::nullopt;  // needs goto_w; stub
    e.code[fx.branch_at + 1] = static_cast<std::uint8_t>((rel >> 8) & 0xff);
    e.code[fx.branch_at + 2] = static_cast<std::uint8_t>(rel & 0xff);
  }

  // Build the JVM exception table from the Dalvik try/handler pairs.
  const auto jvm_off = [&](std::uint32_t addr) -> std::optional<std::uint32_t> {
    if (addr == in.size()) return static_cast<std::uint32_t>(e.code.size());
    const auto it = pc_to_jvm.find(addr);
    if (it == pc_to_jvm.end()) return std::nullopt;
    return static_cast<std::uint32_t>(it->second);
  };
  std::vector<ClassWriter::ExceptionEntry> exceptions;
  for (const DexTryHandler& h : c.handlers) {
    const auto s = jvm_off(h.start_addr), en = jvm_off(h.end_addr),
               hd = jvm_off(h.handler_addr);
    if (!s || !en || !hd || *s > 0xffff || *en > 0xffff || *hd > 0xffff) {
      return std::nullopt;  // ranges must map and fit u2
    }
    std::uint16_t catch_type = 0;  // 0 = catch-all
    if (!h.catch_all) {
      const std::string* td = dex.type_at(h.type_idx);
      if (td == nullptr || td->size() < 2 || td->front() != 'L') return std::nullopt;
      catch_type = cw.class_ref(td->substr(1, td->size() - 2));
    }
    exceptions.push_back({static_cast<std::uint16_t>(*s),
                          static_cast<std::uint16_t>(*en),
                          static_cast<std::uint16_t>(*hd), catch_type});
  }

  // --- StackMapTable (for class-file v51) --------------------------------------
  // Frame points: branch targets (empty stack) and exception handlers (stack =
  // [caught exception]). One forward pass over the Dalvik code computes the JVM
  // local types at each; each Dalvik register keeps an invariant type in our
  // generated code, so a single pass (no fixpoint) is exact.
  std::map<std::uint32_t, std::vector<VType>> frame_stack;  // dalvik pc -> stack
  for (const Fixup& fx : fixups) frame_stack[fx.target_pc];  // empty stack
  for (const DexTryHandler& h : c.handlers) {
    std::string cls = "java/lang/Throwable";
    if (!h.catch_all) {
      const std::string* td = dex.type_at(h.type_idx);
      if (td) cls = obj_internal(*td);
    }
    frame_stack[h.handler_addr] = {{VType::Obj, cls}};
  }

  std::vector<std::uint8_t> stackmap;
  if (!frame_stack.empty()) {
    const std::size_t nloc = static_cast<std::size_t>(max_locals);
    const auto merge1 = [](VType a, VType b) -> VType {
      if (a == b) return a;
      return {VType::Top, {}};  // no supertype lattice: disagreement -> Top
    };
    // Worklist fixpoint: in-state (JVM local types) at each Dalvik pc.
    std::map<std::uint32_t, std::vector<VType>> in_state;
    in_state[0] = std::vector<VType>(nloc);  // args omitted: frame methods are no-arg static
    const auto merge_into = [&](std::uint32_t pc, const std::vector<VType>& s) -> bool {
      auto it = in_state.find(pc);
      if (it == in_state.end()) { in_state[pc] = s; return true; }
      bool changed = false;
      for (std::size_t i = 0; i < nloc; ++i) {
        VType mg = merge1(it->second[i], s[i]);
        if (!(mg == it->second[i])) { it->second[i] = mg; changed = true; }
      }
      return changed;
    };
    std::deque<std::uint32_t> work{0};
    while (!work.empty()) {
      const std::uint32_t pc = work.front(); work.pop_front();
      if (pc >= in.size()) continue;
      std::vector<VType> out = in_state[pc];
      sm_transfer(out, dex, m, in, pc, ins);
      const std::uint8_t op = in[pc] & 0xff;
      const bool ends = op == 0x0e || op == 0x0f || op == 0x11 || op == 0x10 ||
                        op == 0x27;  // return* / throw
      std::vector<std::uint32_t> succ;
      if (op == 0x28) {  // goto
        succ.push_back(static_cast<std::uint32_t>(
            static_cast<std::int64_t>(pc) + static_cast<std::int8_t>((in[pc] >> 8) & 0xff)));
      } else if (op == 0x29) {  // goto/16
        succ.push_back(static_cast<std::uint32_t>(
            static_cast<std::int64_t>(pc) + static_cast<std::int16_t>(in[pc + 1])));
      } else if (!ends) {
        succ.push_back(pc + dalvik_insn_units(op));  // fall-through
        if ((op >= 0x32 && op <= 0x37) || (op >= 0x38 && op <= 0x3d)) {  // if-*
          succ.push_back(static_cast<std::uint32_t>(
              static_cast<std::int64_t>(pc) + static_cast<std::int16_t>(in[pc + 1])));
        }
      }
      for (std::uint32_t s : succ) {
        if (merge_into(s, out)) work.push_back(s);
      }
      for (const DexTryHandler& h : c.handlers) {
        if (pc >= h.start_addr && pc < h.end_addr) {
          if (merge_into(h.handler_addr, in_state[pc])) work.push_back(h.handler_addr);
        }
      }
    }

    struct Frame { std::uint32_t off; std::vector<VType> locals; std::vector<VType> stack; };
    std::vector<Frame> frames;
    for (const auto& [dpc, stk] : frame_stack) {
      const auto it = in_state.find(dpc);
      if (it == in_state.end()) continue;  // unreachable frame point
      frames.push_back({static_cast<std::uint32_t>(pc_to_jvm[dpc]), it->second, stk});
    }
    // frame_stack (a map) iterates dpc-ascending == jvm-offset-ascending.

    const auto be16v = [](std::vector<std::uint8_t>& v, std::uint16_t x) {
      v.push_back(static_cast<std::uint8_t>(x >> 8));
      v.push_back(static_cast<std::uint8_t>(x & 0xff));
    };
    be16v(stackmap, static_cast<std::uint16_t>(frames.size()));
    std::int32_t prev = -1;
    for (const Frame& f : frames) {
      stackmap.push_back(255);  // full_frame
      be16v(stackmap, static_cast<std::uint16_t>(f.off - prev - 1));
      auto [lbytes, lcount] = encode_vtypes(cw, f.locals);
      be16v(stackmap, lcount);
      stackmap.insert(stackmap.end(), lbytes.begin(), lbytes.end());
      be16v(stackmap, static_cast<std::uint16_t>(f.stack.size()));
      for (const VType& s : f.stack) {
        if (s.kind == VType::Obj) {
          stackmap.push_back(7);
          const std::uint16_t ci = cw.class_ref(s.cls);
          be16v(stackmap, ci);
        } else {
          stackmap.push_back(s.kind == VType::Int ? 1 : 0);
        }
      }
      prev = static_cast<std::int32_t>(f.off);
    }
  }

  return Translated{std::move(e.code), static_cast<std::uint16_t>(e.max_stack),
                    static_cast<std::uint16_t>(max_locals), std::move(exceptions),
                    std::move(stackmap)};
}

// A method body that throws UnsupportedOperationException.
Translated emit_stub(ClassWriter& cw, const DexMethod& m) {
  Emitter e(cw);
  const std::uint16_t cls =
      cw.class_ref("java/lang/UnsupportedOperationException");
  const std::uint16_t ctor = cw.methodref(
      "java/lang/UnsupportedOperationException", "<init>", "(Ljava/lang/String;)V");
  const std::uint16_t msg =
      cw.string_const("aclcxx: DEX method not translated: " + m.name);
  e.op(OP_NEW); e.u2(cls); e.adj(+1);
  e.op(OP_DUP); e.adj(+1);
  e.ldc_string(msg);
  e.op(OP_INVOKESPECIAL); e.u2(ctor); e.adj(-2);  // pops objectref + arg
  e.op(OP_ATHROW); e.adj(-1);
  const int locals = arg_slots(m);
  return {std::move(e.code), static_cast<std::uint16_t>(e.max_stack),
          static_cast<std::uint16_t>(locals < 1 ? 0 : locals), {}, {}};
}

}  // namespace

DexError convert(const DexFile& dex, const ConvertOptions& opts,
                 ConvertResult& out) {
  std::error_code ec;
  const fs::path work = opts.work_dir.empty()
                            ? fs::temp_directory_path(ec) / "aclcxx-dex2jvm"
                            : fs::path(opts.work_dir);
  const fs::path classes = work / "classes";
  fs::create_directories(classes, ec);
  if (ec) return {DexStatus::Unsupported, "cannot create work dir: " + ec.message()};

  for (const DexClass& cls : dex.classes()) {
    if (skippable_class(cls)) continue;
    const std::string super =
        cls.super_name ? internal_name(*cls.super_name) : "java/lang/Object";
    ClassWriter cw(internal_name(cls.java_name), super);

    for (const DexField& f : cls.static_fields) {
      cw.add_field(static_cast<std::uint16_t>(f.access_flags & kFieldFlagsMask),
                   f.name, f.type_descriptor);
    }
    for (const DexField& f : cls.instance_fields) {
      cw.add_field(static_cast<std::uint16_t>(f.access_flags & kFieldFlagsMask),
                   f.name, f.type_descriptor);
    }

    const auto emit_all = [&](const std::vector<DexMethod>& ms) {
      for (const DexMethod& m : ms) {
        if (!emittable_method(m)) continue;
        const std::uint16_t access =
            0x0001 | (m.is_static() ? 0x0008 : 0);  // public [static]
        if (auto t = translate_method(cw, dex, m)) {
          cw.add_method(access, m.name, m.jvm_descriptor(), t->max_stack,
                        t->max_locals, std::move(t->code), std::move(t->exceptions),
                        std::move(t->stackmaptable));
          ++out.translated_methods;
        } else {
          Translated s = emit_stub(cw, m);
          cw.add_method(access, m.name, m.jvm_descriptor(), s.max_stack,
                        s.max_locals, std::move(s.code));
          ++out.stubbed_methods;
        }
      }
    };
    emit_all(cls.direct_methods);
    emit_all(cls.virtual_methods);

    const auto [pkg, simple] = split_package(cls.java_name);
    fs::path dir = classes;
    if (!pkg.empty()) {
      dir /= internal_name(pkg);
      fs::create_directories(dir, ec);
    }
    const fs::path file = dir / (simple + ".class");
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) return {DexStatus::Unsupported, "cannot write " + file.string()};
    const std::vector<std::uint8_t> bytes = cw.build();
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!f) return {DexStatus::Unsupported, "write failed: " + file.string()};
    out.classes.push_back(cls.java_name);
  }

  if (out.classes.empty()) {
    return {DexStatus::Unsupported, "no emittable classes in dex"};
  }
  out.classpath_dir = classes.string();
  log::i(kTag, "converted {} class(es) [{} methods translated, {} stubbed] -> {}",
         out.classes.size(), out.translated_methods, out.stubbed_methods,
         out.classpath_dir);
  return {};
}

}  // namespace aclcxx::dex
