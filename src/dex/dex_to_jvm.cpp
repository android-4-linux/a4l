// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/dex.md (pending), Dalvik bytecode reference & JVM Spec ch.6,
// ADR-0001. No GPL-3.0 sources.

#include "dex/dex_to_jvm.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
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
};

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
  if (m.name == "<init>" || m.name == "<clinit>") return false;
  if (m.access_flags & (kAccAbstract | kAccNative)) return false;
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
};

struct Translated {
  std::vector<std::uint8_t> code;
  std::uint16_t max_stack;
  std::uint16_t max_locals;
};

// Translate the supported linear integer subset of a static method's Dalvik
// bytecode. Returns nullopt (caller stubs the method) on any unsupported
// opcode, type, or register width.
std::optional<Translated> translate_method(ClassWriter& cw, const DexFile& dex,
                                           const DexMethod& m) {
  if (!m.is_static() || !m.has_code()) return std::nullopt;
  const DexCode& c = *m.code;
  const int ins = c.ins_size;
  const int regs = c.registers_size;
  const int max_locals = ins + regs;
  if (max_locals > 255) return std::nullopt;

  // Only int-family / reference params and returns are handled.
  for (const std::string& d : m.param_descriptors) {
    if (!is_int_family_desc(d) && !is_ref_desc(d)) return std::nullopt;
  }
  const std::string& ret = m.return_descriptor;
  const bool ret_void = ret == "V";
  const bool ret_int = is_int_family_desc(ret);
  const bool ret_ref = is_ref_desc(ret);
  if (!ret_void && !ret_int && !ret_ref) return std::nullopt;

  Emitter e(cw);
  const auto local = [&](int reg) { return ins + reg; };  // Dalvik reg -> JVM local

  // Prologue: incoming JVM args (locals 0..ins-1) -> Dalvik param registers
  // (which live at JVM locals regs..regs+ins-1). Copy in declaration order.
  for (int i = 0; i < ins; ++i) {
    const std::string& d = m.param_descriptors[static_cast<std::size_t>(i)];
    const int dst = regs + i;
    if (is_ref_desc(d)) { e.load_a(i); e.store_a(dst); }
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

  const std::vector<std::uint16_t>& in = c.insns;
  std::size_t pc = 0;
  while (pc < in.size()) {
    pc_to_jvm[static_cast<std::uint32_t>(pc)] = e.code.size();
    const std::uint16_t u = in[pc];
    const std::uint8_t opcode = u & 0xff;
    const auto rel_target = [&](std::int64_t off) {
      return static_cast<std::uint32_t>(static_cast<std::int64_t>(pc) + off);
    };
    switch (opcode) {
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
      case 0x90:    // add-int (23x)
      case 0x91:    // sub-int
      case 0x92: {  // mul-int
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const int bb = in[pc + 1] & 0xff;
        const int cc = (in[pc + 1] >> 8) & 0xff;
        e.load_i(local(bb)); e.load_i(local(cc));
        e.binop(opcode == 0x90 ? OP_IADD : opcode == 0x91 ? OP_ISUB : OP_IMUL);
        e.store_i(local(a));
        pc += 2;
        break;
      }
      case 0xd8: {  // add-int/lit8 vAA, vBB, #+CC (22b)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xff;
        const int b = in[pc + 1] & 0xff;
        const std::int32_t lit = static_cast<std::int8_t>((in[pc + 1] >> 8) & 0xff);
        e.load_i(local(b)); e.push_int(lit); e.binop(OP_IADD); e.store_i(local(a));
        pc += 2;
        break;
      }
      case 0xd0: {  // add-int/lit16 vA, vB, #+CCCC (22s)
        if (pc + 1 >= in.size()) return std::nullopt;
        const int a = (u >> 8) & 0xf, b = (u >> 12) & 0xf;
        const std::int32_t lit = static_cast<std::int16_t>(in[pc + 1]);
        e.load_i(local(b)); e.push_int(lit); e.binop(OP_IADD); e.store_i(local(a));
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
      case 0x6e:    // invoke-virtual {vC..vG}, meth@BBBB (35c)
      case 0x71: {  // invoke-static {vC..vG}, meth@BBBB (35c)
        if (pc + 2 >= in.size()) return std::nullopt;
        const bool is_virtual = opcode == 0x6e;
        const int argc = (u >> 12) & 0xf;  // includes receiver for invoke-virtual
        if (is_virtual && argc < 1) return std::nullopt;
        const std::uint32_t midx = in[pc + 1];
        const std::uint16_t rw = in[pc + 2];
        const int arg_reg[5] = {rw & 0xf, (rw >> 4) & 0xf, (rw >> 8) & 0xf,
                                (rw >> 12) & 0xf, (u >> 8) & 0xf};
        const MethodRef* mr = dex.method_ref(midx);
        if (mr == nullptr) return std::nullopt;
        const int nparams = static_cast<int>(mr->param_descriptors.size());
        if (nparams != argc - (is_virtual ? 1 : 0)) return std::nullopt;

        int reg = 0;
        if (is_virtual) e.load_a(local(arg_reg[reg++]));  // receiver
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
        e.op(is_virtual ? OP_INVOKEVIRTUAL : OP_INVOKESTATIC);
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

  return Translated{std::move(e.code), static_cast<std::uint16_t>(e.max_stack),
                    static_cast<std::uint16_t>(max_locals)};
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
          static_cast<std::uint16_t>(locals < 1 ? 0 : locals)};
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
    ClassWriter cw(internal_name(cls.java_name), "java/lang/Object");

    const auto emit_all = [&](const std::vector<DexMethod>& ms) {
      for (const DexMethod& m : ms) {
        if (!emittable_method(m)) continue;
        const std::uint16_t access =
            0x0001 | (m.is_static() ? 0x0008 : 0);  // public [static]
        if (auto t = translate_method(cw, dex, m)) {
          cw.add_method(access, m.name, m.jvm_descriptor(), t->max_stack,
                        t->max_locals, std::move(t->code));
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
