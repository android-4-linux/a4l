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

namespace acl::dex {
namespace {

constexpr std::string_view kTag = "acl.dex";

constexpr std::uint32_t kAccInterface = 0x0200;
constexpr std::uint32_t kAccAnnotation = 0x2000;

// JVM opcodes used by the translator/stub.
enum : std::uint8_t {
  OP_ACONST_NULL = 0x01,
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
  OP_CHECKCAST = 0xc0, OP_INSTANCEOF = 0xc1, OP_INVOKEINTERFACE = 0xb9,
  // Float / double.
  OP_FLOAD = 0x17, OP_DLOAD = 0x18, OP_FSTORE = 0x38, OP_DSTORE = 0x39,
  OP_FCONST_0 = 0x0b, OP_DCONST_0 = 0x0e,
  OP_FRETURN = 0xae, OP_DRETURN = 0xaf,
  OP_FALOAD = 0x30, OP_DALOAD = 0x31, OP_FASTORE = 0x51, OP_DASTORE = 0x52,
  OP_FCMPL = 0x95, OP_FCMPG = 0x96, OP_DCMPL = 0x97, OP_DCMPG = 0x98,
  OP_FNEG = 0x76, OP_DNEG = 0x77,
  OP_I2F = 0x86, OP_I2D = 0x87, OP_L2F = 0x89, OP_L2D = 0x8a,
  OP_F2I = 0x8b, OP_F2L = 0x8c, OP_F2D = 0x8d,
  OP_D2I = 0x8e, OP_D2L = 0x8f, OP_D2F = 0x90,
};

// JVM opcode for a Dalvik float non-shift binary op (add,sub,mul,div,rem).
std::uint8_t jvm_fbinop(int idx) {
  static const std::uint8_t ops[] = {0x62, 0x66, 0x6a, 0x6e, 0x72};  // f{add,sub,mul,div,rem}
  return (idx >= 0 && idx < 5) ? ops[idx] : 0;
}
// JVM opcode for a Dalvik double non-shift binary op (add,sub,mul,div,rem).
std::uint8_t jvm_dbinop(int idx) {
  static const std::uint8_t ops[] = {0x63, 0x67, 0x6b, 0x6f, 0x73};  // d{add,sub,mul,div,rem}
  return (idx >= 0 && idx < 5) ? ops[idx] : 0;
}

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

bool emittable_method(const DexMethod& m) {
  if (m.access_flags & (kAccAbstract | kAccNative)) return false;
  if (m.name == "<clinit>") return true;    // static initializer: valid JVM method name
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
  // Float (1 slot) and double (2 slots) helpers.
  void load_f(int slot) { op(OP_FLOAD); u1(static_cast<std::uint8_t>(slot)); adj(+1); }
  void store_f(int slot) { op(OP_FSTORE); u1(static_cast<std::uint8_t>(slot)); adj(-1); }
  void load_d(int slot) { op(OP_DLOAD); u1(static_cast<std::uint8_t>(slot)); adj(+2); }
  void store_d(int slot) { op(OP_DSTORE); u1(static_cast<std::uint8_t>(slot)); adj(-2); }
  void push_float(std::uint32_t bits) {
    if (bits == 0x00000000) { op(OP_FCONST_0); }        // 0.0f
    else if (bits == 0x3f800000) { op(OP_FCONST_0 + 1); }  // 1.0f
    else if (bits == 0x40000000) { op(OP_FCONST_0 + 2); }  // 2.0f
    else {
      const std::uint16_t idx = cw.float_const(bits);
      if (idx <= 255) { op(OP_LDC); u1(static_cast<std::uint8_t>(idx)); }
      else { op(OP_LDC_W); u2(idx); }
    }
    adj(+1);
  }
  void push_double(std::uint64_t bits) {
    if (bits == 0x0000000000000000ULL) { op(OP_DCONST_0); }        // 0.0
    else if (bits == 0x3ff0000000000000ULL) { op(OP_DCONST_0 + 1); }  // 1.0
    else { op(OP_LDC2_W); u2(cw.double_const(bits)); }
    adj(+2);
  }
  void fbinop(std::uint8_t o) { op(o); adj(-1); }  // 2 floats -> 1 float
  void dbinop(std::uint8_t o) { op(o); adj(-2); }  // 2 doubles -> 1 double
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
// Instruction width in 16-bit code units, per the Dalvik instruction-format table
// (source.android.com/docs/core/runtime/instruction-formats). This MUST be correct
// for EVERY opcode — not only the ones translated — because the register-kind and
// stackmap passes walk the whole method linearly; a wrong width desyncs the walk and
// reads garbage (real apps carry const-string/jumbo, goto/32, invoke-custom for
// lambdas, invoke-polymorphic, etc., even in methods that ultimately stub).
int dalvik_insn_units(std::uint8_t op) {
  if (op == 0x18) return 5;                                        // const-wide (51l)
  if (op == 0xfa || op == 0xfb) return 4;    // invoke-polymorphic{,/range} (45cc/4rcc)
  if (op == 0x14 || op == 0x17 || op == 0x1b ||   // const/32, const-wide/32, const-string/jumbo
      op == 0x24 || op == 0x25 || op == 0x26 ||   // filled-new-array{,/range}, fill-array-data
      op == 0x2a || op == 0x2b || op == 0x2c ||   // goto/32, packed-switch, sparse-switch
      (op >= 0x6e && op <= 0x72) || (op >= 0x74 && op <= 0x78) ||  // invoke-{,/range}
      op == 0xfc || op == 0xfd) {                 // invoke-custom{,/range}
    return 3;
  }
  if (op == 0x13 || op == 0x15 || op == 0x16 || op == 0x19 || op == 0x1a ||
      op == 0x1c ||                  // const-class
      op == 0x1f || op == 0x20 ||    // check-cast, instance-of
      op == 0x22 || op == 0x23 || op == 0x29 ||
      (op >= 0x2d && op <= 0x31) ||   // cmpl/cmpg-float/double, cmp-long
      (op >= 0x32 && op <= 0x3d) ||   // if-test / if-testz
      (op >= 0x44 && op <= 0x6d) ||   // aget/aput, iget/iput, sget/sput (all typed)
      (op >= 0x90 && op <= 0xaf) ||   // int/long/float/double binary ops (23x)
      (op >= 0xd0 && op <= 0xe2) ||   // int op/lit16, op/lit8
      op == 0xfe || op == 0xff) {     // const-method-handle, const-method-type
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
    case 0x0c: {  // move-result-object -> Obj(preceding invoke return / filled-new-array)
      std::string cls;
      if (pc >= 3) {
        if ((in[pc - 3] & 0xff) == 0x24) {  // filled-new-array: result is the array type
          const std::string* td = dex.type_at(in[pc - 2]);
          if (td) cls = class_operand(*td);
        } else {
          const MethodRef* mr = dex.method_ref(in[pc - 2]);
          if (mr) cls = obj_internal(mr->return_descriptor);
        }
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
    case 0x82: case 0x85: case 0x87: case 0x8c:             // ..-to-float (1 slot)
      set(L((u >> 8) & 0xf), kInt); break;
    case 0x83: case 0x86: case 0x89:                        // ..-to-double (2 slots)
      set_long(L((u >> 8) & 0xf)); break;
    case 0x88: set_long(L((u >> 8) & 0xf)); break;          // float-to-long
    case 0x8a: set(L((u >> 8) & 0xf), kInt); break;         // double-to-int
    case 0x8b: set_long(L((u >> 8) & 0xf)); break;          // double-to-long
    case 0x7f: set(L((u >> 8) & 0xf), kInt); break;         // neg-float (1 slot)
    case 0x80: set_long(L((u >> 8) & 0xf)); break;          // neg-double (2 slots)
    case 0x2d: case 0x2e: case 0x2f: case 0x30:             // cmp*-float/double -> int
    case 0x31: set(L((u >> 8) & 0xff), kInt); break;        // cmp-long -> int
    case 0xa6: case 0xa7: case 0xa8: case 0xa9: case 0xaa:  // float binary 23x -> float (1)
      set(L((u >> 8) & 0xff), kInt); break;
    case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:  // double binary 23x -> double (2)
      set_long(L((u >> 8) & 0xff)); break;
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
    case 0x1c:  // const-class -> Obj(java/lang/Class)
      set(L((u >> 8) & 0xff), {VType::Obj, "java/lang/Class"}); break;
    case 0x7b: case 0x7c: case 0x8d: case 0x8e: case 0x8f:  // neg/not/int-to-{b,c,s}
      set(L((u >> 8) & 0xf), kInt); break;
    case 0x7d: case 0x7e:                                   // neg/not-long -> long
      set_long(L((u >> 8) & 0xf)); break;
    default:
      if (op >= 0xb0 && op <= 0xba) {                       // int op/2addr -> int
        set(L((u >> 8) & 0xf), kInt);
      } else if (op >= 0xbb && op <= 0xc5) {                // long op/2addr -> long
        set_long(L((u >> 8) & 0xf));
      } else if (op >= 0xc6 && op <= 0xca) {                // float op/2addr -> float (1)
        set(L((u >> 8) & 0xf), kInt);
      } else if (op >= 0xcb && op <= 0xcf) {                // double op/2addr -> double (2)
        set_long(L((u >> 8) & 0xf));
      } else if (op >= 0xd0 && op <= 0xe2) {                // int op/lit -> int
        set(L(op >= 0xd8 ? (u >> 8) & 0xff : (u >> 8) & 0xf), kInt);
      }
      break;  // otherwise no local write
  }
}

// Per-register numeric kind, so the type-ambiguous const/move opcodes can pick the
// correctly-typed JVM load/store/const. Float occupies one slot (like int), double
// two (like long); their arithmetic/convert/compare opcodes are unambiguous, so we
// pin registers from those and propagate through moves via union-find.
enum class RKind { Unknown, Int, Float, Long, Double, Ref };

// Returns nullopt if a register is used as two incompatible types (method stubbed).
std::optional<std::vector<RKind>> infer_reg_kinds(const DexFile& dex,
                                                  const DexMethod& m) {
  const DexCode& c = *m.code;
  const int nregs = c.registers_size;
  // Per-register kind bitmask. Int/Long are the defaults (disambiguated by opcode
  // width), so we track only which of {Int,Float,Long,Double} a register is used
  // as, and flag a genuine ambiguity: Float-vs-Int or Double-vs-Long on one
  // register (the const/move opcode can't tell those apart). Int-vs-Long reuse is
  // fine (add-int vs add-long already differ).
  // kZ: written by const-0 (null candidate). kRD: written by a ref-producing op
  // (const-string/new-instance/...-object/...) — so its ref-ness is NOT a null.
  constexpr int kI = 1, kF = 2, kL = 4, kD = 8, kR = 16, kZ = 32, kRD = 64;
  const auto bit = [](RKind k) {
    switch (k) {
      case RKind::Int: return kI; case RKind::Float: return kF;
      case RKind::Long: return kL; case RKind::Double: return kD;
      case RKind::Ref: return kR;
      default: return 0;
    }
  };
  std::vector<int> parent(static_cast<std::size_t>(nregs));
  std::vector<int> mask(static_cast<std::size_t>(nregs), 0);
  for (int i = 0; i < nregs; ++i) parent[static_cast<std::size_t>(i)] = i;
  const auto find = [&](int x) {
    while (parent[static_cast<std::size_t>(x)] != x) {
      parent[static_cast<std::size_t>(x)] =
          parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(x)])];
      x = parent[static_cast<std::size_t>(x)];
    }
    return x;
  };
  const auto pin = [&](int r, RKind k) {
    if (r < 0 || r >= nregs) return;
    mask[static_cast<std::size_t>(find(r))] |= bit(k);
  };
  const auto pin_zero = [&](int r) {  // register written by a const whose value is 0
    if (r >= 0 && r < nregs) mask[static_cast<std::size_t>(find(r))] |= kZ;
  };
  const auto pin_refdef = [&](int r) {  // register written by a ref-producing op
    if (r >= 0 && r < nregs) mask[static_cast<std::size_t>(find(r))] |= kRD;
  };
  const auto unite = [&](int a, int b) {
    if (a < 0 || b < 0 || a >= nregs || b >= nregs) return;
    const int ra = find(a), rb = find(b);
    if (ra == rb) return;
    parent[static_cast<std::size_t>(ra)] = rb;
    mask[static_cast<std::size_t>(rb)] |= mask[static_cast<std::size_t>(ra)];
  };

  // Parameter registers (high registers): kind from the signature.
  int pr = nregs - c.ins_size;
  if (!m.is_static()) pr += 1;  // 'this' is a ref (no float/double bearing)
  for (const std::string& d : m.param_descriptors) {
    if (d == "F") { pin(pr, RKind::Float); pr += 1; }
    else if (d == "D") { pin(pr, RKind::Double); pr += 2; }
    else if (d == "J") { pin(pr, RKind::Long); pr += 2; }
    else if (is_int_family_desc(d)) { pin(pr, RKind::Int); pr += 1; }
    else pr += 1;  // ref
  }

  const std::vector<std::uint16_t>& in = c.insns;
  std::size_t pc = 0;
  const MethodRef* pending_ret = nullptr;  // invoke return type -> next move-result
  while (pc < in.size()) {
    const std::uint16_t u0 = in[pc];
    if (u0 == 0x0100) { pc += 4 + (pc + 1 < in.size() ? in[pc + 1] * 2 : 0); continue; }
    if (u0 == 0x0200) { pc += 2 + (pc + 1 < in.size() ? in[pc + 1] * 4 : 0); continue; }
    if (u0 == 0x0300) {
      const std::uint32_t sz = pc + 3 < in.size()
          ? (static_cast<std::uint32_t>(in[pc + 2]) |
             (static_cast<std::uint32_t>(in[pc + 3]) << 16)) : 0;
      const int w = pc + 1 < in.size() ? in[pc + 1] : 1;
      pc += 4 + (static_cast<std::size_t>(sz) * w + 1) / 2;
      continue;
    }
    const std::uint8_t op = u0 & 0xff;
    const int A4 = (u0 >> 8) & 0xf, B4 = (u0 >> 12) & 0xf, A8 = (u0 >> 8) & 0xff;
    // Bounds-safe code-unit read: real-world DEX can desync this linear walk (an
    // opcode length we don't advance), so a misread operand must not overrun `in`
    // — the method stubs during translation instead. Returns 0 past the end.
    const auto at = [&](std::size_t k) -> std::uint16_t {
      return pc + k < in.size() ? in[pc + k] : std::uint16_t{0};
    };
    const int BB = pc + 1 < in.size() ? (in[pc + 1] & 0xff) : 0;
    const int CC = pc + 1 < in.size() ? ((in[pc + 1] >> 8) & 0xff) : 0;
    const MethodRef* this_invoke = nullptr;

    if (op >= 0x90 && op <= 0x9a) { pin(A8, RKind::Int); pin(BB, RKind::Int); pin(CC, RKind::Int); }  // int 23x
    else if (op >= 0x9b && op <= 0xa2) { pin(A8, RKind::Long); pin(BB, RKind::Long); pin(CC, RKind::Long); }  // long non-shift
    else if (op >= 0xa3 && op <= 0xa5) { pin(A8, RKind::Long); pin(BB, RKind::Long); pin(CC, RKind::Int); }   // long shift: CC int
    else if (op >= 0xa6 && op <= 0xaa) { pin(A8, RKind::Float); pin(BB, RKind::Float); pin(CC, RKind::Float); }  // float 23x
    else if (op >= 0xab && op <= 0xaf) { pin(A8, RKind::Double); pin(BB, RKind::Double); pin(CC, RKind::Double); }  // double 23x
    else if (op >= 0xb0 && op <= 0xba) { pin(A4, RKind::Int); pin(B4, RKind::Int); }    // int /2addr
    else if (op >= 0xbb && op <= 0xc2) { pin(A4, RKind::Long); pin(B4, RKind::Long); }  // long non-shift /2addr
    else if (op >= 0xc3 && op <= 0xc5) { pin(A4, RKind::Long); pin(B4, RKind::Int); }   // long shift /2addr
    else if (op >= 0xc6 && op <= 0xca) { pin(A4, RKind::Float); pin(B4, RKind::Float); }    // float /2addr
    else if (op >= 0xcb && op <= 0xcf) { pin(A4, RKind::Double); pin(B4, RKind::Double); }  // double /2addr
    else if (op >= 0xd0 && op <= 0xd7) { pin(A4, RKind::Int); pin(B4, RKind::Int); }    // int /lit16 (22s: nibbles)
    else if (op >= 0xd8 && op <= 0xe2) { pin(A8, RKind::Int); pin(BB, RKind::Int); }    // int /lit8 (22b: bytes)
    // const-0: a null candidate for a ref register (const 0 -> aconst_null).
    if (op == 0x12) { if (((u0 >> 12) & 0xf) == 0) pin_zero(A4); }
    else if (op == 0x13 && pc + 1 < in.size()) { if (in[pc + 1] == 0) pin_zero(A8); }
    else if (op == 0x14 && pc + 2 < in.size()) { if ((in[pc + 1] | in[pc + 2]) == 0) pin_zero(A8); }
    else if (op == 0x15 && pc + 1 < in.size()) { if (in[pc + 1] == 0) pin_zero(A8); }

    // Moves unify (32-bit and 64-bit), so a const/move-linked register inherits
    // the type pinned elsewhere.
    if (op == 0x01) unite(A4, B4);                       // move
    else if (op == 0x02) unite(A8, at(1));               // move/from16
    else if (op == 0x03) unite(at(1), at(2));            // move/16
    else if (op == 0x04) unite(A4, B4);                  // move-wide
    else if (op == 0x05) unite(A8, at(1));               // move-wide/from16
    else if (op == 0x06) unite(at(1), at(2));            // move-wide/16
    else if (op == 0x07) unite(A4, B4);                  // move-object
    else if (op == 0x08) unite(A8, at(1));               // move-object/from16
    else if (op == 0x09) unite(at(1), at(2));            // move-object/16

    // Ref-producing writes (value is a real object, never a bare null literal).
    if (op == 0x1a || op == 0x1c || op == 0x22 || op == 0x0c || op == 0x0d) pin_refdef(A8);
    else if (op == 0x23 || op == 0x07) pin_refdef(A4);
    else if (op == 0x08) pin_refdef(A8);
    else if (op == 0x09) pin_refdef(at(1));

    switch (op) {
      case 0x11: pin(A8, RKind::Ref); break;   // return-object
      case 0x27: pin(A8, RKind::Ref); break;   // throw
      case 0x1f: pin(A8, RKind::Ref); pin_refdef(A8); break;   // check-cast (dst is a real ref)
      case 0x20: pin(B4, RKind::Ref); break;   // instance-of source
      case 0x46: pin(A8, RKind::Ref); pin_refdef(A8); break;   // aget-object dst
      case 0x4d: pin(A8, RKind::Ref); break;   // aput-object value (a null is const 0)
      case 0x62: pin(A8, RKind::Ref); pin_refdef(A8); break;   // sget-object dst
      case 0x69: pin(A8, RKind::Ref); break;   // sput-object value
      case 0x7b: case 0x7c: pin(A4, RKind::Int); pin(B4, RKind::Int); break;      // neg/not-int
      case 0x7d: case 0x7e: pin(A4, RKind::Long); pin(B4, RKind::Long); break;    // neg/not-long
      case 0x7f: pin(A4, RKind::Float); pin(B4, RKind::Float); break;             // neg-float
      case 0x80: pin(A4, RKind::Double); pin(B4, RKind::Double); break;           // neg-double
      case 0x81: pin(A4, RKind::Long); pin(B4, RKind::Int); break;                // int-to-long
      case 0x82: pin(A4, RKind::Float); pin(B4, RKind::Int); break;               // int-to-float
      case 0x83: pin(A4, RKind::Double); pin(B4, RKind::Int); break;              // int-to-double
      case 0x84: pin(A4, RKind::Int); pin(B4, RKind::Long); break;                // long-to-int
      case 0x85: pin(A4, RKind::Float); pin(B4, RKind::Long); break;              // long-to-float
      case 0x86: pin(A4, RKind::Double); pin(B4, RKind::Long); break;             // long-to-double
      case 0x87: pin(A4, RKind::Int); pin(B4, RKind::Float); break;               // float-to-int
      case 0x88: pin(A4, RKind::Long); pin(B4, RKind::Float); break;              // float-to-long
      case 0x89: pin(A4, RKind::Double); pin(B4, RKind::Float); break;            // float-to-double
      case 0x8a: pin(A4, RKind::Int); pin(B4, RKind::Double); break;              // double-to-int
      case 0x8b: pin(A4, RKind::Long); pin(B4, RKind::Double); break;             // double-to-long
      case 0x8c: pin(A4, RKind::Float); pin(B4, RKind::Double); break;            // double-to-float
      case 0x8d: case 0x8e: case 0x8f: pin(A4, RKind::Int); pin(B4, RKind::Int); break;  // int-to-b/c/s
      case 0x2d: case 0x2e: pin(A8, RKind::Int); pin(BB, RKind::Float); pin(CC, RKind::Float); break;   // cmp*-float
      case 0x2f: case 0x30: pin(A8, RKind::Int); pin(BB, RKind::Double); pin(CC, RKind::Double); break; // cmp*-double
      case 0x31: pin(A8, RKind::Int); pin(BB, RKind::Long); pin(CC, RKind::Long); break;                // cmp-long
      case 0x0f: pin(A8, m.return_descriptor == "F" ? RKind::Float : RKind::Int); break;  // return
      case 0x10: pin(A8, m.return_descriptor == "D" ? RKind::Double : RKind::Long); break;  // return-wide
      case 0x0a:  // move-result
        if (pending_ret) {
          pin(A8, pending_ret->return_descriptor == "F" ? RKind::Float : RKind::Int);
        }
        break;
      case 0x0b:  // move-result-wide
        if (pending_ret) {
          pin(A8, pending_ret->return_descriptor == "D" ? RKind::Double : RKind::Long);
        }
        break;
      case 0x52: case 0x53: case 0x54: {  // iget / iget-wide / iget-object
        if (op == 0x54) pin_refdef(A4);  // iget-object dst is a real ref
        const FieldRef* fr = dex.field_ref(at(1));
        if (fr) pin(A4, fr->type_descriptor == "F" ? RKind::Float
                        : fr->type_descriptor == "D" ? RKind::Double
                        : is_wide_desc(fr->type_descriptor) ? RKind::Long
                        : is_ref_desc(fr->type_descriptor) ? RKind::Ref : RKind::Int);
        break;
      }
      case 0x59: case 0x5a: case 0x5b: {  // iput / iput-wide / iput-object
        const FieldRef* fr = dex.field_ref(at(1));
        if (fr) pin(A4, fr->type_descriptor == "F" ? RKind::Float
                        : fr->type_descriptor == "D" ? RKind::Double
                        : is_wide_desc(fr->type_descriptor) ? RKind::Long
                        : is_ref_desc(fr->type_descriptor) ? RKind::Ref : RKind::Int);
        break;
      }
      case 0x60: case 0x61: {  // sget / sget-wide
        const FieldRef* fr = dex.field_ref(at(1));
        if (fr) pin(A8, fr->type_descriptor == "F" ? RKind::Float
                        : fr->type_descriptor == "D" ? RKind::Double
                        : is_wide_desc(fr->type_descriptor) ? RKind::Long
                        : is_ref_desc(fr->type_descriptor) ? RKind::Ref : RKind::Int);
        break;
      }
      case 0x67: case 0x68: {  // sput / sput-wide
        const FieldRef* fr = dex.field_ref(at(1));
        if (fr) pin(A8, fr->type_descriptor == "F" ? RKind::Float
                        : fr->type_descriptor == "D" ? RKind::Double
                        : is_wide_desc(fr->type_descriptor) ? RKind::Long
                        : is_ref_desc(fr->type_descriptor) ? RKind::Ref : RKind::Int);
        break;
      }
      case 0x44: case 0x47: case 0x48: case 0x49: case 0x4a:  // aget int/narrow: dst Int, idx Int
      case 0x4b: case 0x4e: case 0x4f: case 0x50: case 0x51:  // aput int/narrow
        pin(A8, RKind::Int); pin(CC, RKind::Int); break;
      case 0x45: case 0x4c: pin(A8, RKind::Long); pin(CC, RKind::Int); break;  // aget/aput-wide (long)
      case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72:  // invoke 35c
      case 0x74: case 0x75: case 0x76: case 0x77: case 0x78:  // invoke /range
        this_invoke = dex.method_ref(at(1));
        if (this_invoke) {
          const bool is_static = (op == 0x71 || op == 0x77);
          const bool range = op >= 0x74;
          const int argc = range ? A8 : ((u0 >> 12) & 0xf);
          const std::uint16_t rw = pc + 2 < in.size() ? in[pc + 2] : 0;
          const int nib[5] = {rw & 0xf, (rw >> 4) & 0xf, (rw >> 8) & 0xf,
                              (rw >> 12) & 0xf, A8};
          if (!is_static) pin(range ? rw : nib[0], RKind::Ref);  // receiver
          int idx = is_static ? 0 : 1;
          for (const std::string& d : this_invoke->param_descriptors) {
            const int r = range ? (rw + idx) : nib[idx];
            if (d == "F") pin(r, RKind::Float);
            else if (d == "D") { pin(r, RKind::Double); }
            else if (d == "J") pin(r, RKind::Long);
            else if (is_int_family_desc(d)) pin(r, RKind::Int);
            else if (is_ref_desc(d)) pin(r, RKind::Ref);  // a null arg is const 0 -> aconst_null
            idx += is_wide_desc(d) ? 2 : 1;
            (void)argc;
          }
        }
        break;
      default: break;
    }
    pending_ret = this_invoke;  // a move-result immediately after reads this
    pc += dalvik_insn_units(op);
  }

  std::vector<RKind> out(static_cast<std::size_t>(nregs), RKind::Unknown);
  for (int i = 0; i < nregs; ++i) {
    const int mk = mask[static_cast<std::size_t>(find(i))];
    // Genuine ambiguity a whole-method kind cannot resolve -> stub the method: the
    // const/move opcode can't tell Float from Int or Double from Long.
    if (((mk & kF) && (mk & kI)) || ((mk & kD) && (mk & kL)) ||
        ((mk & kF) && (mk & kD))) {
      return std::nullopt;
    }
    // Ref only for a *pure null literal*: used as a ref, written by a const-0, and
    // never used numerically nor written by a ref-producing op (whose const-0s
    // would be int). This is the `f(null)` pattern; other ref/int reuse stays
    // opcode-driven (const-string astore, aload, etc. don't consult this kind).
    const int numeric = mk & (kI | kF | kL | kD);
    out[static_cast<std::size_t>(i)] =
        (mk & kD) ? RKind::Double
      : (mk & kF) ? RKind::Float
      : ((mk & kR) && (mk & kZ) && !numeric && !(mk & kRD)) ? RKind::Ref
                                                            : RKind::Unknown;
  }
  return out;
}

// Serialize one full_frame's verification_type_info list for `slots`, trimming
// trailing Top entries. Returns (bytes, entry_count).
// `rk`/`ins` refine a width-typed local (Int/Long) to Float/Double per the
// inferred register kind (JVM slot i maps to Dalvik register i-ins).
std::pair<std::vector<std::uint8_t>, std::uint16_t> encode_vtypes(
    ClassWriter& cw, const std::vector<VType>& slots,
    const std::vector<RKind>& rk, int ins) {
  const auto slot_kind = [&](int i) -> RKind {
    const int reg = i - ins;
    return (reg >= 0 && reg < static_cast<int>(rk.size()))
               ? rk[static_cast<std::size_t>(reg)] : RKind::Unknown;
  };
  int last = -1;
  for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
    if (slots[i].kind != VType::Top && slots[i].kind != VType::LongHi) last = i;
  }
  std::vector<std::uint8_t> out;
  std::uint16_t count = 0;
  for (int i = 0; i <= last; ++i) {
    ++count;
    switch (slots[i].kind) {
      case VType::Int:
        out.push_back(slot_kind(i) == RKind::Float ? 2 : 1);  // Float : Integer
        break;
      case VType::Long:
        out.push_back(slot_kind(i) == RKind::Double ? 3 : 4);  // Double : Long
        ++i;  // skip the high slot
        break;
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
                                           const DexMethod& m,
                                           const std::string& this_internal) {
  if (!m.has_code()) return std::nullopt;
  const DexCode& c = *m.code;
  const int ins = c.ins_size;
  const int regs = c.registers_size;
  const int max_locals = ins + regs;
  if (max_locals > 255) return std::nullopt;

  const std::string& ret = m.return_descriptor;
  // All scalar return kinds are supported (void/int/long/float/double/ref).

  // Register-kind inference disambiguates the type-ambiguous const/move opcodes
  // (int-vs-float, long-vs-double). A conflict -> stub.
  auto reg_kinds = infer_reg_kinds(dex, m);
  if (!reg_kinds) return std::nullopt;
  const std::vector<RKind>& rk = *reg_kinds;
  const auto rkind = [&](int reg) -> RKind {
    return (reg >= 0 && reg < regs) ? rk[static_cast<std::size_t>(reg)]
                                    : RKind::Unknown;
  };

  Emitter e(cw);
  const auto local = [&](int reg) { return ins + reg; };  // Dalvik reg -> JVM local

  // Prologue: incoming JVM args (locals 0..ins-1) -> Dalvik param registers
  // (JVM locals regs..regs+ins-1). Copy with the correctly-typed load/store, and
  // seed the frame fixpoint's entry state so params read-before-write are typed.
  std::vector<VType> entry_locals(static_cast<std::size_t>(max_locals));  // all Top
  {
    int islot = 0;  // incoming JVM slot cursor
    const auto do_arg = [&](RKind k, const std::string& obj_cls) {
      const int dst = regs + islot;
      auto seed = [&](int slot, VType v) {
        if (slot >= 0 && slot < max_locals) {
          entry_locals[static_cast<std::size_t>(slot)] = std::move(v);
        }
      };
      switch (k) {
        case RKind::Long: case RKind::Double:
          e.load_l(islot); e.store_l(dst);  // wide load/store move both slots
          seed(dst, {VType::Long, {}}); seed(dst + 1, {VType::LongHi, {}});
          islot += 2; break;
        case RKind::Float:
          e.load_f(islot); e.store_f(dst); seed(dst, {VType::Int, {}}); islot += 1; break;
        case RKind::Ref:
          e.load_a(islot); e.store_a(dst); seed(dst, {VType::Obj, obj_cls});
          islot += 1; break;
        default:  // Int
          e.load_i(islot); e.store_i(dst); seed(dst, {VType::Int, {}}); islot += 1; break;
      }
    };
    if (!m.is_static()) do_arg(RKind::Ref, this_internal);  // 'this'
    for (const std::string& d : m.param_descriptors) {
      if (d == "J" || d == "D") do_arg(d == "J" ? RKind::Long : RKind::Double, "");
      else if (d == "F") do_arg(RKind::Float, "");
      else if (is_ref_desc(d)) do_arg(RKind::Ref, obj_internal(d));
      else if (is_int_family_desc(d)) do_arg(RKind::Int, "");
      else return std::nullopt;
    }
    if (islot != ins) return std::nullopt;
  }

  // Const/move emit helpers: pick the float/double/ref form when the destination
  // register's inferred kind demands it (the raw bit pattern is reinterpreted).
  // A const-0 into a ref register is a null reference -> aconst_null. Such a null
  // at a StackMapTable frame point needs verification-type handling we don't do
  // yet, so a null-const in a method with branches/handlers stubs (guard below).
  bool emitted_null_const = false;
  const auto emit_const32 = [&](int a, std::int32_t lit) -> bool {
    switch (rkind(a)) {
      case RKind::Float:
        e.push_float(static_cast<std::uint32_t>(lit)); e.store_f(local(a));
        return true;
      case RKind::Ref:
        if (lit != 0) return false;  // only null is representable via const
        e.op(OP_ACONST_NULL); e.adj(+1); e.store_a(local(a));
        emitted_null_const = true;
        return true;
      default:
        e.push_int(lit); e.store_i(local(a)); return true;
    }
  };
  const auto emit_const64 = [&](int a, std::int64_t lit) {
    if (rkind(a) == RKind::Double) {
      e.push_double(static_cast<std::uint64_t>(lit)); e.store_d(local(a));
    } else { e.push_long(lit); e.store_l(local(a)); }
  };

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
  // 4-byte switch offsets (relative to the switch opcode), patched below.
  struct SwitchFixup { std::size_t operand_at; std::uint32_t target_pc;
                       std::size_t switch_op_at; };
  std::vector<SwitchFixup> switch_fixups;
  // Element descriptor last written to a register by new-array, so a following
  // fill-array-data can pick the correct typed array-store opcode. Conservative:
  // any other write to the register clears the entry (see clobber() below).
  std::map<int, std::string> reg_array_elem;

  // Catch-handler entry points: the JVM enters these with the exception on the
  // operand stack, so reset the tracked height to 1 (move-exception then pops it).
  std::set<std::uint32_t> handler_pcs;
  for (const DexTryHandler& h : c.handlers) handler_pcs.insert(h.handler_addr);

  const std::vector<std::uint16_t>& in = c.insns;
  // Per-register type recorded by new-instance, used to fix <init> calls: Dalvik's
  // `new-instance vX, N; invoke-direct M.<init>` (N inherits M's constructor) must
  // become JVM `new N; invokespecial N.<init>` — the invokespecial class must match
  // the new type, not the constructor's declaring class (else VerifyError).
  std::map<int, std::string> reg_new_type;
  std::size_t pc = 0;
  while (pc < in.size()) {
    // Skip inline switch/array payloads (data, not instructions).
    if (in[pc] == 0x0100) { pc += 4 + (pc + 1 < in.size() ? in[pc + 1] * 2 : 0); continue; }
    if (in[pc] == 0x0200) { pc += 2 + (pc + 1 < in.size() ? in[pc + 1] * 4 : 0); continue; }
    if (in[pc] == 0x0300) {  // fill-array-data-payload: 4u header + ceil(size*width/2)
      const std::uint32_t sz = pc + 3 < in.size()
          ? (static_cast<std::uint32_t>(in[pc + 2]) |
             (static_cast<std::uint32_t>(in[pc + 3]) << 16)) : 0;
      const int w = pc + 1 < in.size() ? in[pc + 1] : 1;
      pc += 4 + (static_cast<std::size_t>(sz) * w + 1) / 2;
      continue;
    }
    pc_to_jvm[static_cast<std::uint32_t>(pc)] = e.code.size();
    if (handler_pcs.count(static_cast<std::uint32_t>(pc))) {
      e.stack = 1;
      if (e.max_stack < 1) e.max_stack = 1;
    }
    const std::uint16_t u = in[pc];
    const std::uint8_t opcode = u & 0xff;
    // Bounds-safe code-unit read (see infer_reg_kinds): a misdecoded operand must
    // not overrun `in`; returns 0 past the end so the method stubs rather than aborts.
    const auto at = [&](std::size_t k) -> std::uint16_t {
      return pc + k < in.size() ? in[pc + k] : std::uint16_t{0};
    };
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
        if (rkind(a) == RKind::Float) { e.load_f(local(b)); e.store_f(local(a)); }
        else if (rkind(a) == RKind::Ref) { e.load_a(local(b)); e.store_a(local(a)); }
        else { e.load_i(local(b)); e.store_i(local(a)); }
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
      case 0x0f: {  // return vAA (11x): int or float, per the return descriptor
        const int reg = (u >> 8) & 0xff;
        if (ret == "F") { e.load_f(local(reg)); e.op(OP_FRETURN); e.adj(-1); }
        else { e.load_i(local(reg)); e.op(OP_IRETURN); e.adj(-1); }
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
        if (!emit_const32(a, lit)) return std::nullopt;
        pc += 1;
        break;
      }
      case 0x13: {  // const/16 vAA, #+BBBB (21s)
        if (pc + 1 >= in.size()) return std::nullopt;
        if (!emit_const32((u >> 8) & 0xff, static_cast<std::int16_t>(in[pc + 1])))
          return std::nullopt;
        pc += 2;
        break;
      }
      case 0x14: {  // const vAA, #+BBBBBBBB (31i)
        if (pc + 2 >= in.size()) return std::nullopt;
        if (!emit_const32((u >> 8) & 0xff, static_cast<std::int32_t>(
                static_cast<std::uint32_t>(in[pc + 1]) |
                (static_cast<std::uint32_t>(in[pc + 2]) << 16))))
          return std::nullopt;
        pc += 3;
        break;
      }
      case 0x15: {  // const/high16 vAA, #+BBBB0000 (21h)
        if (pc + 1 >= in.size()) return std::nullopt;
        if (!emit_const32((u >> 8) & 0xff,
                static_cast<std::int32_t>(static_cast<std::uint32_t>(in[pc + 1]) << 16)))
          return std::nullopt;
        pc += 2;
        break;
      }
      case 0x16: {  // const-wide/16 vAA, #+BBBB (21s) - long/double
        if (pc + 1 >= in.size()) return std::nullopt;
        emit_const64((u >> 8) & 0xff, static_cast<std::int16_t>(in[pc + 1]));
        pc += 2;
        break;
      }
      case 0x17: {  // const-wide/32 vAA, #+BBBBBBBB (31i) - long/double
        if (pc + 2 >= in.size()) return std::nullopt;
        emit_const64((u >> 8) & 0xff, static_cast<std::int32_t>(
            static_cast<std::uint32_t>(in[pc + 1]) |
            (static_cast<std::uint32_t>(in[pc + 2]) << 16)));
        pc += 3;
        break;
      }
      case 0x18: {  // const-wide vAA, #+BBBB...(64) (51l) - long/double
        if (pc + 4 >= in.size()) return std::nullopt;
        std::uint64_t lit = 0;
        for (int k = 0; k < 4; ++k) {
          lit |= static_cast<std::uint64_t>(in[pc + 1 + k]) << (16 * k);
        }
        emit_const64((u >> 8) & 0xff, static_cast<std::int64_t>(lit));
        pc += 5;
        break;
      }
      case 0x19: {  // const-wide/high16 vAA, #+BBBB000000000000 (21h) - long/double
        if (pc + 1 >= in.size()) return std::nullopt;
        emit_const64((u >> 8) & 0xff, static_cast<std::int64_t>(in[pc + 1]) << 48);
        pc += 2;
        break;
      }
      case 0x04: {  // move-wide vA, vB (12x) - long/double
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        if (rkind(a) == RKind::Double) { e.load_d(local(b)); e.store_d(local(a)); }
        else { e.load_l(local(b)); e.store_l(local(a)); }
        pc += 1;
        break;
      }
      case 0x10: {  // return-wide vAA (11x): long or double, per return descriptor
        const int a = (u >> 8) & 0xff;
        if (ret == "D") { e.load_d(local(a)); e.op(OP_DRETURN); e.adj(-2); }
        else { e.load_l(local(a)); e.op(OP_LRETURN); e.adj(-2); }
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
      case 0xa6: case 0xa7: case 0xa8: case 0xa9: case 0xaa: {  // float binary (23x)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const int bb = in[pc + 1] & 0xff, cc = (in[pc + 1] >> 8) & 0xff;
        e.load_f(local(bb)); e.load_f(local(cc));
        e.fbinop(jvm_fbinop(opcode - 0xa6));
        e.store_f(local(a));
        pc += 2;
        break;
      }
      case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf: {  // double binary (23x)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const int bb = in[pc + 1] & 0xff, cc = (in[pc + 1] >> 8) & 0xff;
        e.load_d(local(bb)); e.load_d(local(cc));
        e.dbinop(jvm_dbinop(opcode - 0xab));
        e.store_d(local(a));
        pc += 2;
        break;
      }
      case 0xc6: case 0xc7: case 0xc8: case 0xc9: case 0xca: {  // float op/2addr (12x)
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_f(local(a)); e.load_f(local(b));
        e.fbinop(jvm_fbinop(opcode - 0xc6));
        e.store_f(local(a));
        pc += 1;
        break;
      }
      case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf: {  // double op/2addr (12x)
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_d(local(a)); e.load_d(local(b));
        e.dbinop(jvm_dbinop(opcode - 0xcb));
        e.store_d(local(a));
        pc += 1;
        break;
      }
      case 0x7f: {  // neg-float (12x)
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_f(local(b)); e.op(OP_FNEG); e.store_f(local(a));
        pc += 1;
        break;
      }
      case 0x80: {  // neg-double (12x)
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        e.load_d(local(b)); e.op(OP_DNEG); e.store_d(local(a));
        pc += 1;
        break;
      }
      case 0x82: case 0x83: case 0x85: case 0x86:  // int/long -> float/double
      case 0x87: case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8c: {  // float/double -> ..
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        switch (opcode) {
          case 0x82: e.load_i(local(b)); e.op(OP_I2F); e.store_f(local(a)); break;
          case 0x83: e.load_i(local(b)); e.op(OP_I2D); e.adj(+1); e.store_d(local(a)); break;
          case 0x85: e.load_l(local(b)); e.op(OP_L2F); e.adj(-1); e.store_f(local(a)); break;
          case 0x86: e.load_l(local(b)); e.op(OP_L2D); e.store_d(local(a)); break;
          case 0x87: e.load_f(local(b)); e.op(OP_F2I); e.store_i(local(a)); break;
          case 0x88: e.load_f(local(b)); e.op(OP_F2L); e.adj(+1); e.store_l(local(a)); break;
          case 0x89: e.load_f(local(b)); e.op(OP_F2D); e.adj(+1); e.store_d(local(a)); break;
          case 0x8a: e.load_d(local(b)); e.op(OP_D2I); e.adj(-1); e.store_i(local(a)); break;
          case 0x8b: e.load_d(local(b)); e.op(OP_D2L); e.store_l(local(a)); break;
          case 0x8c: e.load_d(local(b)); e.op(OP_D2F); e.adj(-1); e.store_f(local(a)); break;
          default: return std::nullopt;
        }
        pc += 1;
        break;
      }
      case 0x2d: case 0x2e: {  // cmpl-float / cmpg-float (23x) -> int
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const int bb = in[pc + 1] & 0xff, cc = (in[pc + 1] >> 8) & 0xff;
        e.load_f(local(bb)); e.load_f(local(cc));
        e.op(opcode == 0x2d ? OP_FCMPL : OP_FCMPG); e.adj(-1);  // 2 floats -> int
        e.store_i(local(a));
        pc += 2;
        break;
      }
      case 0x2f: case 0x30: {  // cmpl-double / cmpg-double (23x) -> int
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const int bb = in[pc + 1] & 0xff, cc = (in[pc + 1] >> 8) & 0xff;
        e.load_d(local(bb)); e.load_d(local(cc));
        e.op(opcode == 0x2f ? OP_DCMPL : OP_DCMPG); e.adj(-3);  // 2 doubles -> int
        e.store_i(local(a));
        pc += 2;
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
      case 0x0a: {  // move-result vAA (11x): capture preceding invoke's int/float result
        const int a = (u >> 8) & 0xff;
        if (rkind(a) == RKind::Float) e.store_f(local(a));
        else e.store_i(local(a));  // pops the value the invoke left on the stack
        pc += 1;
        break;
      }
      case 0x0b: {  // move-result-wide vAA: capture a long/double result
        const int a = (u >> 8) & 0xff;
        if (rkind(a) == RKind::Double) e.store_d(local(a));
        else e.store_l(local(a));  // pops the two-slot value
        pc += 1;
        break;
      }
      case 0x0c: {  // move-result-object vAA: capture an object/array result
        e.store_a(local((u >> 8) & 0xff));  // pops the reference
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
        const FieldRef* fr = dex.field_ref(at(1));
        if (fr == nullptr) return std::nullopt;
        const bool is_object = opcode == 0x62 || opcode == 0x69;
        const bool is_float = fr->type_descriptor == "F";
        const bool is_get = opcode == 0x62 || (opcode >= 0x60 && opcode <= 0x66);
        if (is_object ? !is_ref_desc(fr->type_descriptor)
                      : !(is_float || is_int_family_desc(fr->type_descriptor))) {
          return std::nullopt;
        }
        const std::uint16_t ref =
            cw.fieldref(fr->owner_internal, fr->name, fr->type_descriptor);
        if (is_get) {
          e.op(OP_GETSTATIC); e.u2(ref); e.adj(+1);
          if (is_object) e.store_a(local(a));
          else if (is_float) e.store_f(local(a)); else e.store_i(local(a));
        } else {
          if (is_object) e.load_a(local(a));
          else if (is_float) e.load_f(local(a)); else e.load_i(local(a));
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
        reg_new_type[a] = td->substr(1, td->size() - 2);  // for the <init> owner fix
        pc += 2;
        break;
      }
      case 0x1c: {  // const-class vAA, type@BBBB (21c)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const std::string* td = dex.type_at(in[pc + 1]);
        if (td == nullptr) return std::nullopt;
        const std::string cls = class_operand(*td);
        if (cls.empty()) return std::nullopt;
        e.ldc_string(cw.class_ref(cls));  // ldc of a Class constant
        e.store_a(local(a));
        pc += 2;
        break;
      }
      case 0x24: {  // filled-new-array {vC..vG}, type@BBBB (35c)
        if (pc + 2 >= in.size()) return std::nullopt;
        const int argc = (u >> 12) & 0xf;
        const std::string* td = dex.type_at(in[pc + 1]);
        if (td == nullptr || td->size() < 2 || td->front() != '[') return std::nullopt;
        const std::string elem = td->substr(1);
        const std::uint16_t rw = in[pc + 2];
        const int arg_reg[5] = {rw & 0xf, (rw >> 4) & 0xf, (rw >> 8) & 0xf,
                                (rw >> 12) & 0xf, (u >> 8) & 0xf};
        // Element store op + ref-ness.
        std::uint8_t store_op;
        bool is_ref = false;
        if (elem == "I") { store_op = OP_IASTORE; }
        else if (elem[0] == 'L' || elem[0] == '[') { store_op = OP_AASTORE; is_ref = true; }
        else if (elem == "Z" || elem == "B") { store_op = OP_BASTORE; }
        else if (elem == "C") { store_op = OP_CASTORE; }
        else if (elem == "S") { store_op = OP_SASTORE; }
        else return std::nullopt;
        e.push_int(argc);
        if (elem.size() == 1) {  // primitive element
          const int at = newarray_atype(elem[0]);
          if (at < 0) return std::nullopt;
          e.op(OP_NEWARRAY); e.u1(static_cast<std::uint8_t>(at));  // net 0
        } else {                 // reference element
          e.op(OP_ANEWARRAY);
          e.u2(cw.class_ref(elem[0] == 'L' ? elem.substr(1, elem.size() - 2) : elem));
        }
        for (int i = 0; i < argc; ++i) {
          e.op(OP_DUP); e.adj(+1);
          e.push_int(i);
          if (is_ref) e.load_a(local(arg_reg[i])); else e.load_i(local(arg_reg[i]));
          e.op(store_op); e.adj(-3);
        }
        // The array is left on the stack for the following move-result-object.
        pc += 3;
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
      case 0x53: case 0x5a: {  // iget-wide / iput-wide (22c) - long or double field
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        const FieldRef* fr = dex.field_ref(at(1));
        if (fr == nullptr || !is_wide_desc(fr->type_descriptor)) return std::nullopt;
        const bool dbl = fr->type_descriptor == "D";
        const std::uint16_t ref =
            cw.fieldref(fr->owner_internal, fr->name, fr->type_descriptor);
        if (opcode == 0x53) {  // vA(wide) = vB.field
          e.load_a(local(b));
          e.op(OP_GETFIELD); e.u2(ref); e.adj(+1);  // obj(1) -> wide(2)
          if (dbl) e.store_d(local(a)); else e.store_l(local(a));
        } else {               // vB.field = vA(wide)
          e.load_a(local(b));
          if (dbl) e.load_d(local(a)); else e.load_l(local(a));
          e.op(OP_PUTFIELD); e.u2(ref); e.adj(-3);
        }
        pc += 2;
        break;
      }
      case 0x61: case 0x68: {  // sget-wide / sput-wide (21c) - long or double field
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const FieldRef* fr = dex.field_ref(at(1));
        if (fr == nullptr || !is_wide_desc(fr->type_descriptor)) return std::nullopt;
        const bool dbl = fr->type_descriptor == "D";
        const std::uint16_t ref =
            cw.fieldref(fr->owner_internal, fr->name, fr->type_descriptor);
        if (opcode == 0x61) {
          e.op(OP_GETSTATIC); e.u2(ref); e.adj(+2);
          if (dbl) e.store_d(local(a)); else e.store_l(local(a));
        } else {
          if (dbl) e.load_d(local(a)); else e.load_l(local(a));
          e.op(OP_PUTSTATIC); e.u2(ref); e.adj(-2);
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
        reg_array_elem[a] = elem;  // remember for a following fill-array-data
        pc += 2;
        break;
      }
      case 0x26: {  // fill-array-data vAA, +offset (31t) - init an array from a payload
        if (pc + 2 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const auto it = reg_array_elem.find(a);
        if (it == reg_array_elem.end() || it->second.size() != 1) {
          return std::nullopt;  // element type unknown / not primitive -> stub
        }
        const char elem = it->second[0];
        const std::int32_t poff = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(in[pc + 1]) |
            (static_cast<std::uint32_t>(in[pc + 2]) << 16));
        const std::size_t ppc = static_cast<std::size_t>(
            static_cast<std::int64_t>(pc) + poff);
        if (ppc + 3 >= in.size() || in[ppc] != 0x0300) return std::nullopt;
        const int width = in[ppc + 1];
        const std::uint32_t size = static_cast<std::uint32_t>(in[ppc + 2]) |
                                   (static_cast<std::uint32_t>(in[ppc + 3]) << 16);
        // Element store op + how to read a value of `width` bytes from the payload.
        std::uint8_t store_op;
        bool wide = false;
        switch (elem) {
          case 'Z': case 'B': store_op = OP_BASTORE; break;
          case 'C': store_op = OP_CASTORE; break;
          case 'S': store_op = OP_SASTORE; break;
          case 'I': store_op = OP_IASTORE; break;
          case 'J': store_op = OP_LASTORE; wide = true; break;
          default: return std::nullopt;  // F/D unsupported (no type inference yet)
        }
        const std::size_t data = (ppc + 4) * 2;  // payload data byte offset
        const auto raw = [&](std::size_t byte) -> std::uint64_t {
          const std::size_t unit = byte / 2;
          return unit < in.size()
                     ? (byte % 2 == 0 ? (in[unit] & 0xff) : (in[unit] >> 8)) : 0;
        };
        for (std::uint32_t i = 0; i < size; ++i) {
          std::uint64_t v = 0;
          for (int b = 0; b < width; ++b) {
            v |= raw(data + static_cast<std::size_t>(i) * width + b) << (8 * b);
          }
          e.load_a(local(a));   // arrayref
          e.push_int(static_cast<std::int32_t>(i));
          if (wide) {
            e.push_long(static_cast<std::int64_t>(v));  // sign of full 64-bit value
            e.op(store_op); e.adj(-4);  // array+index+long(2) popped
          } else {
            // Sign-extend from the element width so byte/short literals round-trip.
            const int bits = width * 8;
            std::int64_t sv = static_cast<std::int64_t>(v);
            if (bits < 64 && (v & (1ULL << (bits - 1)))) {
              sv |= -(1LL << bits);
            }
            e.push_int(static_cast<std::int32_t>(sv));
            e.op(store_op); e.adj(-3);  // array+index+value popped
          }
        }
        pc += 3;
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
        const FieldRef* fr = dex.field_ref(at(1));
        if (fr == nullptr) return std::nullopt;
        const bool is_object = opcode == 0x54 || opcode == 0x5b;
        const bool is_float = fr->type_descriptor == "F";
        const bool is_get = opcode == 0x54 || (opcode >= 0x52 && opcode <= 0x58);
        if (is_object ? !is_ref_desc(fr->type_descriptor)
                      : !(is_float || is_int_family_desc(fr->type_descriptor))) {
          return std::nullopt;
        }
        const std::uint16_t ref =
            cw.fieldref(fr->owner_internal, fr->name, fr->type_descriptor);
        if (is_get) {                    // vA = vB.field
          e.load_a(local(b));
          e.op(OP_GETFIELD); e.u2(ref);  // pop obj, push value (net 0)
          if (is_object) e.store_a(local(a));
          else if (is_float) e.store_f(local(a)); else e.store_i(local(a));
        } else {                         // vB.field = vA
          e.load_a(local(b));
          if (is_object) e.load_a(local(a));
          else if (is_float) e.load_f(local(a)); else e.load_i(local(a));
          e.op(OP_PUTFIELD); e.u2(ref); e.adj(-2);
        }
        pc += 2;
        break;
      }
      case 0x6e:    // invoke-virtual   {vC..vG}, meth@BBBB (35c)
      case 0x6f:    // invoke-super     {vC..vG}, meth@BBBB (35c)
      case 0x70:    // invoke-direct    {vC..vG}, meth@BBBB (35c) - <init>/private
      case 0x71:    // invoke-static    {vC..vG}, meth@BBBB (35c)
      case 0x72:    // invoke-interface {vC..vG}, meth@BBBB (35c)
      case 0x74:    // invoke-virtual/range   {vCCCC..vNNNN}, meth@BBBB (3rc)
      case 0x75:    // invoke-super/range
      case 0x76:    // invoke-direct/range
      case 0x77:    // invoke-static/range
      case 0x78: {  // invoke-interface/range
        if (pc + 2 >= in.size()) return std::nullopt;
        const bool is_range = opcode >= 0x74;
        // Map /range opcodes (0x74..0x78) onto their 35c kin (0x6e..0x72).
        const std::uint8_t kind = is_range
            ? static_cast<std::uint8_t>(opcode - (0x74 - 0x6e)) : opcode;
        const bool is_static_call = kind == 0x71;
        const bool is_interface = kind == 0x72;
        const bool has_receiver = !is_static_call;
        const std::uint8_t invoke_op =
            is_static_call ? OP_INVOKESTATIC
            : (kind == 0x70 || kind == 0x6f) ? OP_INVOKESPECIAL
                                             : OP_INVOKEVIRTUAL;
        // 35c: arg count in the high nibble, registers nibble-packed.
        // 3rc: arg count in AA, registers contiguous from CCCC.
        const int argc = is_range ? ((u >> 8) & 0xff) : ((u >> 12) & 0xf);
        if (has_receiver && argc < 1) return std::nullopt;
        const std::uint32_t midx = in[pc + 1];
        const std::uint16_t rw = in[pc + 2];
        const int nibble_reg[5] = {rw & 0xf, (rw >> 4) & 0xf, (rw >> 8) & 0xf,
                                   (rw >> 12) & 0xf, (u >> 8) & 0xf};
        const auto arg_reg = [&](int i) {
          return is_range ? (rw + i) : nibble_reg[i];
        };
        const MethodRef* mr = dex.method_ref(midx);
        if (mr == nullptr) return std::nullopt;
        const int nparams = static_cast<int>(mr->param_descriptors.size());
        if (nparams != argc - (has_receiver ? 1 : 0)) return std::nullopt;

        int reg = 0;
        if (has_receiver) e.load_a(local(arg_reg(reg++)));  // receiver
        for (int i = 0; i < nparams; ++i) {
          const std::string& d = mr->param_descriptors[static_cast<std::size_t>(i)];
          if (d == "J") { e.load_l(local(arg_reg(reg))); reg += 2; }
          else if (d == "D") { e.load_d(local(arg_reg(reg))); reg += 2; }
          else if (d == "F") { e.load_f(local(arg_reg(reg))); reg += 1; }
          else if (is_ref_desc(d)) { e.load_a(local(arg_reg(reg))); reg += 1; }
          else if (is_int_family_desc(d)) { e.load_i(local(arg_reg(reg))); reg += 1; }
          else return std::nullopt;
        }
        const std::string& rd = mr->return_descriptor;
        const int retw = rd == "V" ? 0 : is_wide_desc(rd) ? 2 : 1;
        if (is_interface) {
          e.op(OP_INVOKEINTERFACE);
          e.u2(cw.interface_methodref(mr->owner_internal, mr->name, mr->descriptor));
          e.u1(static_cast<std::uint8_t>(argc));  // outgoing arg word count
          e.u1(0);
        } else {
          // For a constructor call on a freshly new'd object, the invokespecial class
          // must be the new type (not the constructor's declaring superclass) — the
          // Dalvik inherited-constructor pattern. super()/this() on uninitialized-this
          // has no recorded new-type and keeps the ref's owner.
          std::string owner = mr->owner_internal;
          if (kind == 0x70 && mr->name == "<init>" && has_receiver) {
            auto it = reg_new_type.find(arg_reg(0));
            if (it != reg_new_type.end()) {
              owner = it->second;
              reg_new_type.erase(it);  // initialized now
            }
          }
          e.op(invoke_op);
          e.u2(cw.methodref(owner, mr->name, mr->descriptor));
        }
        e.adj(-argc + retw);  // pops receiver+params, pushes return
        // Discard the result if not captured by a following move-result{,-wide}.
        if (retw > 0) {
          const std::uint8_t nxt = pc + 3 < in.size() ? (in[pc + 3] & 0xff) : 0;
          const bool captured = nxt == 0x0a || nxt == 0x0b || nxt == 0x0c;
          if (!captured) { e.op(retw == 2 ? 0x58 : OP_POP); e.adj(-retw); }  // pop2/pop
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
      case 0x2b:    // packed-switch vAA, +offset (31t)
      case 0x2c: {  // sparse-switch vAA, +offset (31t)
        if (pc + 2 >= in.size()) return std::nullopt;
        const int reg = (u >> 8) & 0xff;
        const std::int32_t poff = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(in[pc + 1]) |
            (static_cast<std::uint32_t>(in[pc + 2]) << 16));
        const std::size_t ppc = static_cast<std::size_t>(
            static_cast<std::int64_t>(pc) + poff);
        if (ppc + 1 >= in.size()) return std::nullopt;
        const std::uint16_t ident = in[ppc];
        const int size = in[ppc + 1];
        if ((opcode == 0x2b && ident != 0x0100) ||
            (opcode == 0x2c && ident != 0x0200)) {
          return std::nullopt;
        }
        const auto put_s4 = [&](std::int32_t v) {
          e.code.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
          e.code.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
          e.code.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
          e.code.push_back(static_cast<std::uint8_t>(v & 0xff));
        };
        const auto s4_at = [&](std::size_t p) {
          return static_cast<std::int32_t>(
              static_cast<std::uint32_t>(in[p]) |
              (static_cast<std::uint32_t>(in[p + 1]) << 16));
        };
        const auto switch_target = [&](std::uint32_t target) {
          switch_fixups.push_back({e.code.size(), target,
                                   /*switch_op_at set below*/ 0});
          put_s4(0);  // placeholder
        };

        e.load_i(local(reg));
        const std::size_t sw_at = e.code.size();
        e.op(opcode == 0x2b ? 0xaa : 0xab);  // tableswitch / lookupswitch
        while (e.code.size() % 4 != 0) e.code.push_back(0);  // align operands
        const std::size_t first_fixup = switch_fixups.size();
        switch_target(static_cast<std::uint32_t>(pc + 3));  // default -> fall-through
        if (opcode == 0x2b) {  // tableswitch: low, high, targets
          const std::int32_t first_key = s4_at(ppc + 2);
          put_s4(first_key);
          put_s4(first_key + size - 1);
          for (int i = 0; i < size; ++i) {
            switch_target(static_cast<std::uint32_t>(
                static_cast<std::int64_t>(pc) + s4_at(ppc + 4 + 2 * i)));
          }
        } else {  // lookupswitch: npairs, then (key, offset)*
          put_s4(size);
          for (int i = 0; i < size; ++i) {
            put_s4(s4_at(ppc + 2 + 2 * i));  // key
            switch_target(static_cast<std::uint32_t>(
                static_cast<std::int64_t>(pc) + s4_at(ppc + 2 + 2 * size + 2 * i)));
          }
        }
        for (std::size_t i = first_fixup; i < switch_fixups.size(); ++i) {
          switch_fixups[i].switch_op_at = sw_at;
        }
        e.adj(-1);  // the switch pops the int value
        pc += 3;
        break;
      }
      default:
        return std::nullopt;  // unsupported opcode -> stub the method
    }
  }

  // A null reference (const 0 -> aconst_null) live across a StackMapTable frame
  // needs verification-type-info we don't compute yet; stub such methods (safe)
  // rather than risk a class-load VerifyError. Straight-line null use is fine.
  if (emitted_null_const &&
      (!fixups.empty() || !switch_fixups.empty() || !c.handlers.empty())) {
    return std::nullopt;
  }

  // Resolve switch offsets (4 bytes, relative to the switch opcode).
  for (const SwitchFixup& sf : switch_fixups) {
    const auto it = pc_to_jvm.find(sf.target_pc);
    if (it == pc_to_jvm.end()) return std::nullopt;
    const std::int32_t rel = static_cast<std::int32_t>(
        static_cast<std::int64_t>(it->second) - static_cast<std::int64_t>(sf.switch_op_at));
    e.code[sf.operand_at + 0] = static_cast<std::uint8_t>((rel >> 24) & 0xff);
    e.code[sf.operand_at + 1] = static_cast<std::uint8_t>((rel >> 16) & 0xff);
    e.code[sf.operand_at + 2] = static_cast<std::uint8_t>((rel >> 8) & 0xff);
    e.code[sf.operand_at + 3] = static_cast<std::uint8_t>(rel & 0xff);
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
  for (const SwitchFixup& sf : switch_fixups) frame_stack[sf.target_pc];  // empty stack
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
    in_state[0] = entry_locals;  // seeded with incoming param types (see prologue)
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
      } else if (op == 0x2b || op == 0x2c) {  // packed/sparse-switch
        succ.push_back(pc + 3);  // default = fall-through
        const std::int32_t poff = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(in[pc + 1]) |
            (static_cast<std::uint32_t>(in[pc + 2]) << 16));
        const std::size_t ppc =
            static_cast<std::size_t>(static_cast<std::int64_t>(pc) + poff);
        const int size = in[ppc + 1];
        const auto s4 = [&](std::size_t p) {
          return static_cast<std::int32_t>(static_cast<std::uint32_t>(in[p]) |
                                           (static_cast<std::uint32_t>(in[p + 1]) << 16));
        };
        for (int i = 0; i < size; ++i) {
          const std::size_t toff =
              op == 0x2b ? ppc + 4 + 2 * i : ppc + 2 + 2 * size + 2 * i;
          succ.push_back(static_cast<std::uint32_t>(
              static_cast<std::int64_t>(pc) + s4(toff)));
        }
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
      auto [lbytes, lcount] = encode_vtypes(cw, f.locals, rk, ins);
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
      cw.string_const("acl: DEX method not translated: " + m.name);
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
                            ? fs::temp_directory_path(ec) / "acl-dex2jvm"
                            : fs::path(opts.work_dir);
  const fs::path classes = work / "classes";
  fs::create_directories(classes, ec);
  if (ec) return {DexStatus::Unsupported, "cannot create work dir: " + ec.message()};

  for (const DexClass& cls : dex.classes()) {
    const std::string super =
        cls.super_name ? internal_name(*cls.super_name) : "java/lang/Object";
    ClassWriter cw(internal_name(cls.java_name), super);
    // Class access flags. Interfaces/annotations must NOT set ACC_SUPER and MUST set
    // ACC_INTERFACE|ACC_ABSTRACT — without this the converter dropped every interface,
    // so any class implementing one failed to load (NoClassDefFoundError).
    const bool is_interface = (cls.access_flags & kAccInterface) != 0;
    std::uint16_t class_flags = 0x0001;  // ACC_PUBLIC
    if (is_interface) {
      class_flags |= 0x0200 | 0x0400;                      // ACC_INTERFACE|ACC_ABSTRACT
      if (cls.access_flags & kAccAnnotation) class_flags |= 0x2000;  // ACC_ANNOTATION
    } else {
      class_flags |= 0x0020;                               // ACC_SUPER
    }
    cw.set_access_flags(class_flags);
    for (const std::string& iface : cls.interfaces) cw.add_interface(internal_name(iface));

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
        const std::uint16_t access =
            0x0001 | (m.is_static() ? 0x0008 : 0);  // public [static]
        // Abstract/native methods (incl. interface methods) are declarations only —
        // emit them with no Code so the interface/abstract class is complete and its
        // methods resolve (invoke-interface / invoke-virtual).
        if (m.access_flags & (kAccAbstract | kAccNative)) {
          const std::uint16_t abstract_flag =
              (m.access_flags & kAccAbstract) ? 0x0400 : 0x0100;
          cw.add_abstract_method(static_cast<std::uint16_t>(access | abstract_flag),
                                 m.name, m.jvm_descriptor());
          continue;
        }
        if (!emittable_method(m)) continue;
        if (auto t = translate_method(cw, dex, m, internal_name(cls.java_name))) {
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

}  // namespace acl::dex
