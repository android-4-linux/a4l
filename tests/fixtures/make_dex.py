# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format). No GPL-3.0 sources.
#
# Encode a minimal but structurally-valid classes.dex holding one class
#   public class HelloDex {
#     public static void main(String[] args) { return; }
#     public static int answer() { return 42; }
#   }
# with a correct Adler-32 checksum and SHA-1 signature, for the dex/ parser
# tests. Independent of the C++ parser (both follow the documented format).
#
# Usage: make_dex.py <output.dex>
import hashlib
import struct
import sys
import zlib

NO_INDEX = 0xFFFFFFFF


def uleb128(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def align4(buf):
    while len(buf) % 4 != 0:
        buf += b"\x00"
    return buf


# Interned strings (order is arbitrary; the parser is order-independent).
STRINGS = [
    "LHelloDex;",            # 0  type: this class
    "Ljava/lang/Object;",    # 1  type: superclass
    "V",                     # 2  type/shorty: void
    "I",                     # 3  type/shorty: int
    "[Ljava/lang/String;",   # 4  type: String[]
    "VL",                    # 5  shorty: (Object-ish) -> void
    "main",                  # 6  method name
    "answer",                # 7  method name
    "HelloDex.java",         # 8  source file
    "misc",                  # 9  method name (untranslatable -> stubbed)
    "loopSum",               # 10 method name (branches: sum 1..10 = 55)
    "callSum",               # 11 method name (invoke-static: answer()+loopSum())
    "Ljava/lang/String;",    # 12 type: String
    "hello",                 # 13 const-string value / hello() method name
    "length",                # 14 method name (String.length)
    "strlen",                # 15 method name (const-string + invoke-virtual)
    "Ljava/lang/System;",    # 16 type: System
    "Ljava/io/PrintStream;", # 17 type: PrintStream
    "out",                   # 18 field name (System.out)
    "println",               # 19 method name (PrintStream.println)
    "hello aclcxx",          # 20 const-string value
    "LCounter;",             # 21 type: the object-model demo class
    "<init>",                # 22 constructor name
    "value",                 # 23 instance field name
    "get",                   # 24 method name
    "add",                   # 25 method name
    "useCounter",            # 26 method name (static, news up a Counter)
    "VI",                    # 27 shorty for (I)V
    "[I",                    # 28 type: int[]
    "arraySum",              # 29 method name (new-array/aput/aget/array-length)
]
S = {s: i for i, s in enumerate(STRINGS)}

# type_ids -> string index for each descriptor.
TYPES = ["LHelloDex;", "Ljava/lang/Object;", "V", "I", "[Ljava/lang/String;",
         "Ljava/lang/String;", "Ljava/lang/System;", "Ljava/io/PrintStream;",
         "LCounter;", "[I"]
T = {t: i for i, t in enumerate(TYPES)}


def code_item(regs, ins, outs, insns_u16):
    # registers_size, ins_size, outs_size, tries_size, debug_off, insns_size,
    # insns[], then padding to 4.
    body = struct.pack("<HHHHII", regs, ins, outs, 0, 0, len(insns_u16))
    for u in insns_u16:
        body += struct.pack("<H", u)
    return align4(bytearray(body))


def build_dex():
    header_size = 112
    n_strings = len(STRINGS)
    n_types = len(TYPES)
    n_protos = 5    # + (I)V for Counter.<init>/add
    n_fields = 2    # System.out, Counter.value
    n_methods = 15  # + arraySum
    n_classdefs = 2  # HelloDex, Counter

    off_string_ids = header_size
    off_type_ids = off_string_ids + n_strings * 4
    off_proto_ids = off_type_ids + n_types * 4
    off_field_ids = off_proto_ids + n_protos * 12
    off_method_ids = off_field_ids + n_fields * 8
    off_class_defs = off_method_ids + n_methods * 8
    off_data = off_class_defs + n_classdefs * 32

    # --- data section -------------------------------------------------------
    data = bytearray()

    def emit(chunk):
        off = off_data + len(data)
        data.extend(chunk)
        return off

    # string_data_items: uleb128(utf16 count) + MUTF-8 bytes + 0x00
    string_data_off = []
    for s in STRINGS:
        b = s.encode("utf-8")
        string_data_off.append(emit(uleb128(len(s)) + b + b"\x00"))

    # type_list for main's parameters: (String[])
    data.extend(align4(bytearray()))  # align before a 4-aligned item
    type_list_main_off = off_data + len(data)
    tl = struct.pack("<I", 1) + struct.pack("<H", T["[Ljava/lang/String;"])
    data.extend(align4(bytearray(tl)))
    # type_list for println's parameters: (String)
    data.extend(align4(bytearray()))
    type_list_println_off = off_data + len(data)
    tl2 = struct.pack("<I", 1) + struct.pack("<H", T["Ljava/lang/String;"])
    data.extend(align4(bytearray(tl2)))
    # type_list for (I)V parameters: (int)  [Counter.<init>, Counter.add]
    data.extend(align4(bytearray()))
    type_list_int_off = off_data + len(data)
    tl3 = struct.pack("<I", 1) + struct.pack("<H", T["I"])
    data.extend(align4(bytearray(tl3)))

    # code items
    code_main_off = off_data + len(data)
    # return-void (op 0x0e, format 10x) -> code unit 0x000e
    data.extend(code_item(1, 1, 0, [0x000E]))
    code_answer_off = off_data + len(data)
    # const/16 v0, #42 (op 0x13) -> 0x0013, 0x002a ; return v0 (op 0x0f) -> 0x000f
    data.extend(code_item(1, 0, 0, [0x0013, 0x002A, 0x000F]))
    code_misc_off = off_data + len(data)
    # monitor-enter v0 (op 0x1d, unsupported -> stubbed). The translator bails at
    # the first unsupported opcode; the rest is never reached (the stub throws).
    # Keeps the stub-fallback path under test.
    data.extend(code_item(1, 0, 0, [0x001D, 0x0012, 0x000F]))
    code_loopsum_off = off_data + len(data)
    # int loopSum() { int sum=0,i=1; while(!(i>10)){ sum+=i; i+=1; } return sum; }
    #   pc0  const/4    v0, 0
    #   pc1  const/4    v1, 1
    #   pc2  const/16   v2, 10
    #   pc4  if-gt      v1, v2, +7    ; if i>10 goto pc11
    #   pc6  add-int    v0, v0, v1
    #   pc8  add-int/lit8 v1, v1, 1
    #   pc10 goto       -6            ; -> pc4
    #   pc11 return     v0            ; 55
    data.extend(code_item(3, 0, 0, [
        0x0012, 0x1112, 0x0213, 0x000A, 0x2136, 0x0007,
        0x0090, 0x0100, 0x01D8, 0x0101, 0xFA28, 0x000F,
    ]))
    code_callsum_off = off_data + len(data)
    # int callSum() { return answer() + loopSum(); }   (42 + 55 = 97)
    #   invoke-static answer (method 1); move-result v0
    #   invoke-static loopSum (method 3); move-result v1
    #   add-int v0, v0, v1; return v0
    data.extend(code_item(2, 0, 0, [
        0x0071, 0x0001, 0x0000, 0x000A,   # invoke-static {} answer ; move-result v0
        0x0071, 0x0003, 0x0000, 0x010A,   # invoke-static {} loopSum; move-result v1
        0x0090, 0x0100, 0x000F,           # add-int v0,v0,v1 ; return v0
    ]))
    code_strlen_off = off_data + len(data)
    # int strlen() { return "hello".length(); }   (== 5)
    #   const-string   v0, "hello"
    #   invoke-virtual {v0}, String.length()I  (method 6) ; move-result v1
    #   return v1
    data.extend(code_item(2, 0, 1, [
        0x001A, S["hello"], 0x106E, 0x0006, 0x0000, 0x010A, 0x010F,
    ]))
    code_hello_off = off_data + len(data)
    # void hello() { System.out.println("hello aclcxx"); }
    #   sget-object    v0, System.out:PrintStream   (field 0)
    #   const-string   v1, "hello aclcxx"
    #   invoke-virtual {v0, v1}, PrintStream.println(String)V  (method 8)
    #   return-void
    data.extend(code_item(2, 0, 2, [
        0x0062, 0x0000, 0x011A, S["hello aclcxx"], 0x206E, 0x0008, 0x0010, 0x000E,
    ]))
    code_arraysum_off = off_data + len(data)
    # int arraySum() {
    #   int[] a = new int[3]; a[0]=10; a[1]=20; a[2]=12;
    #   int sum=0; for (int i=0; i<a.length; i++) sum += a[i]; return sum;   // 42
    # }  registers: v0=array, v1=index, v2=value/sum, v3=len/elem
    data.extend(code_item(4, 0, 0, [
        0x3112,                       # const/4 v1, 3
        0x1023, T["[I"],              # new-array v0, v1, int[]
        0x0112, 0x0213, 0x000A,       # const/4 v1,0 ; const/16 v2,10
        0x024B, 0x0100,               # aput v2, v0, v1     (a[0]=10)
        0x1112, 0x0213, 0x0014,       # const/4 v1,1 ; const/16 v2,20
        0x024B, 0x0100,               # aput v2, v0, v1     (a[1]=20)
        0x2112, 0x0213, 0x000C,       # const/4 v1,2 ; const/16 v2,12
        0x024B, 0x0100,               # aput v2, v0, v1     (a[2]=12)
        0x0212, 0x0112,               # const/4 v2,0 (sum) ; const/4 v1,0 (i)
        0x0321,                       # :loop  array-length v3, v0
        0x3135, 0x0009,               # if-ge v1, v3, :end
        0x0344, 0x0100,               # aget v3, v0, v1
        0x0290, 0x0302,               # add-int v2, v2, v3
        0x01D8, 0x0101,               # add-int/lit8 v1, v1, 1
        0xF728,                       # goto :loop
        0x020F,                       # :end  return v2
    ]))

    # class_data_item
    class_data_off = off_data + len(data)
    cd = bytearray()
    cd += uleb128(0)  # static_fields_size
    cd += uleb128(0)  # instance_fields_size
    cd += uleb128(8)  # direct_methods_size
    cd += uleb128(0)  # virtual_methods_size
    # direct methods, sorted by ascending method index; idx diffs are cumulative.
    # m0=main..m5=strlen, m7=hello, m14=arraySum.
    cd += uleb128(0) + uleb128(0x9) + uleb128(code_main_off)      # diff 0 -> idx 0
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_answer_off)    # diff 1 -> idx 1
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_misc_off)      # diff 1 -> idx 2
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_loopsum_off)   # diff 1 -> idx 3
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_callsum_off)   # diff 1 -> idx 4
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_strlen_off)    # diff 1 -> idx 5
    cd += uleb128(2) + uleb128(0x9) + uleb128(code_hello_off)     # diff 2 -> idx 7
    cd += uleb128(7) + uleb128(0x9) + uleb128(code_arraysum_off)  # diff 7 -> idx 14
    data.extend(cd)

    # --- Counter class code items (object model demo) -----------------------
    # field id 1 = Counter.value:I ; method ids: 9=Object.<init>, 10=Counter.<init>,
    # 11=Counter.get, 12=Counter.add, 13=Counter.useCounter. type 8 = LCounter;.
    code_cinit_off = off_data + len(data)
    # Counter(int start){ super(); value = start; }   ins=2: v0=this, v1=start
    #   invoke-direct {v0}, Object.<init>()V (m9) ; iput v1,v0,value(f1) ; return-void
    data.extend(code_item(2, 2, 1, [
        0x1070, 0x0009, 0x0000, 0x0159, 0x0001, 0x000E,
    ]))
    code_cget_off = off_data + len(data)
    # int get(){ return value; }   registers=2, ins=1: v1=this, v0=temp
    #   iget v0, v1, value(f1) ; return v0
    data.extend(code_item(2, 1, 0, [0x1052, 0x0001, 0x000F]))
    code_cadd_off = off_data + len(data)
    # void add(int n){ value = value + n; }  registers=3, ins=2: v1=this, v2=n, v0=temp
    #   iget v0,v1,value ; add-int v0,v0,v2 ; iput v0,v1,value ; return-void
    data.extend(code_item(3, 2, 0, [
        0x1052, 0x0001, 0x0090, 0x0200, 0x1059, 0x0001, 0x000E,
    ]))
    code_ccount_off = off_data + len(data)
    # static int useCounter(){ Counter c=new Counter(10); c.add(5); return c.get(); }
    #   new-instance v0, Counter(type8) ; const/16 v1,10 ;
    #   invoke-direct {v0,v1}, Counter.<init>(m10) ; const/4 v1,5 ;
    #   invoke-virtual {v0,v1}, Counter.add(m12) ;
    #   invoke-virtual {v0}, Counter.get(m11) ; move-result v1 ; return v1
    data.extend(code_item(2, 0, 2, [
        0x0022, 0x0008, 0x0113, 0x000A, 0x2070, 0x000A, 0x0010,
        0x5112, 0x206E, 0x000C, 0x0010, 0x106E, 0x000B, 0x0000,
        0x010A, 0x010F,
    ]))

    counter_class_data_off = off_data + len(data)
    ccd = bytearray()
    ccd += uleb128(0)  # static_fields_size
    ccd += uleb128(1)  # instance_fields_size
    ccd += uleb128(2)  # direct_methods_size  (<init>, useCounter)
    ccd += uleb128(2)  # virtual_methods_size (get, add)
    # instance fields: value = field id 1 (diff from 0)
    ccd += uleb128(1) + uleb128(0x1)                             # f1, public
    # direct methods: <init>=m10, useCounter=m13
    ccd += uleb128(10) + uleb128(0x1) + uleb128(code_cinit_off)  # idx 10, public
    ccd += uleb128(3) + uleb128(0x9) + uleb128(code_ccount_off)  # +3 -> idx 13, public static
    # virtual methods: get=m11, add=m12
    ccd += uleb128(11) + uleb128(0x1) + uleb128(code_cget_off)   # idx 11, public
    ccd += uleb128(1) + uleb128(0x1) + uleb128(code_cadd_off)    # +1 -> idx 12, public
    data.extend(ccd)

    # map_list (4-aligned). Parser ignores it, but emit a valid one.
    data.extend(align4(bytearray()))
    map_off = off_data + len(data)
    items = [
        (0x0000, 1, 0),
        (0x0001, n_strings, off_string_ids),
        (0x0002, n_types, off_type_ids),
        (0x0003, n_protos, off_proto_ids),
        (0x0004, n_fields, off_field_ids),
        (0x0005, n_methods, off_method_ids),
        (0x0006, n_classdefs, off_class_defs),
        (0x2002, n_strings, string_data_off[0]),
        (0x1001, 3, type_list_main_off),
        (0x2001, 12, code_main_off),
        (0x2000, 2, class_data_off),
    ]
    map_body = bytearray(struct.pack("<I", len(items) + 1))
    for mtype, size, off in items:
        map_body += struct.pack("<HHII", mtype, 0, size, off)
    map_body += struct.pack("<HHII", 0x1000, 0, 1, map_off)  # the map itself
    data.extend(align4(map_body))

    data_size = len(data)

    # --- fixed tables -------------------------------------------------------
    string_ids = b"".join(struct.pack("<I", o) for o in string_data_off)
    type_ids = b"".join(struct.pack("<I", S[t]) for t in TYPES)

    proto_ids = b""
    # proto 0: main -> shorty "VL", return V, params (String[])
    proto_ids += struct.pack("<III", S["VL"], T["V"], type_list_main_off)
    # proto 1: answer -> shorty "I", return I, no params
    proto_ids += struct.pack("<III", S["I"], T["I"], 0)
    # proto 2: println -> shorty "VL", return V, params (String)
    proto_ids += struct.pack("<III", S["VL"], T["V"], type_list_println_off)
    # proto 3: hello -> shorty "V", return V, no params
    proto_ids += struct.pack("<III", S["V"], T["V"], 0)
    # proto 4: (I)V -> shorty "VI", return V, params (int)
    proto_ids += struct.pack("<III", S["VI"], T["V"], type_list_int_off)

    # field_ids: {class_idx(u16), type_idx(u16), name_idx(u32)}
    field_ids = struct.pack("<HHI", T["Ljava/lang/System;"],
                            T["Ljava/io/PrintStream;"], S["out"])     # f0 System.out
    field_ids += struct.pack("<HHI", T["LCounter;"], T["I"], S["value"])  # f1 Counter.value

    method_ids = b""
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 0, S["main"])
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["answer"])
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["misc"])     # proto ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["loopSum"])  # proto ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["callSum"])  # proto ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["strlen"])   # m5 ()I
    method_ids += struct.pack("<HHI", T["Ljava/lang/String;"], 1, S["length"])  # m6 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 3, S["hello"])    # m7 ()V
    method_ids += struct.pack("<HHI", T["Ljava/io/PrintStream;"], 2, S["println"])  # m8
    method_ids += struct.pack("<HHI", T["Ljava/lang/Object;"], 3, S["<init>"])  # m9 Object.<init>()V
    method_ids += struct.pack("<HHI", T["LCounter;"], 4, S["<init>"])     # m10 Counter.<init>(I)V
    method_ids += struct.pack("<HHI", T["LCounter;"], 1, S["get"])        # m11 Counter.get()I
    method_ids += struct.pack("<HHI", T["LCounter;"], 4, S["add"])        # m12 Counter.add(I)V
    method_ids += struct.pack("<HHI", T["LCounter;"], 1, S["useCounter"]) # m13 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["arraySum"])   # m14 ()I

    class_defs = struct.pack(
        "<IIIIIIII",
        T["LHelloDex;"],       # class_idx
        0x1,                    # access_flags = public
        T["Ljava/lang/Object;"],  # superclass_idx
        0,                      # interfaces_off
        S["HelloDex.java"],     # source_file_idx
        0,                      # annotations_off
        class_data_off,         # class_data_off
        0,                      # static_values_off
    )
    class_defs += struct.pack(
        "<IIIIIIII",
        T["LCounter;"],           # class_idx
        0x1,                       # access_flags = public
        T["Ljava/lang/Object;"],   # superclass_idx (Counter extends Object)
        0,                         # interfaces_off
        0xFFFFFFFF,                # source_file_idx = NO_INDEX
        0,                         # annotations_off
        counter_class_data_off,    # class_data_off
        0,                         # static_values_off
    )

    # --- header -------------------------------------------------------------
    header = bytearray(header_size)
    header[0:8] = b"dex\n035\x00"
    struct.pack_into("<I", header, 0x24, header_size)
    struct.pack_into("<I", header, 0x28, 0x12345678)  # endian tag
    struct.pack_into("<I", header, 0x34, map_off)
    struct.pack_into("<I", header, 0x38, n_strings)
    struct.pack_into("<I", header, 0x3C, off_string_ids)
    struct.pack_into("<I", header, 0x40, n_types)
    struct.pack_into("<I", header, 0x44, off_type_ids)
    struct.pack_into("<I", header, 0x48, n_protos)
    struct.pack_into("<I", header, 0x4C, off_proto_ids)
    struct.pack_into("<I", header, 0x50, n_fields)       # field_ids_size
    struct.pack_into("<I", header, 0x54, off_field_ids)  # field_ids_off
    struct.pack_into("<I", header, 0x58, n_methods)
    struct.pack_into("<I", header, 0x5C, off_method_ids)
    struct.pack_into("<I", header, 0x60, n_classdefs)
    struct.pack_into("<I", header, 0x64, off_class_defs)
    struct.pack_into("<I", header, 0x68, data_size)
    struct.pack_into("<I", header, 0x6C, off_data)

    dex = bytearray(header) + string_ids + type_ids + proto_ids + \
        field_ids + method_ids + class_defs + bytes(data)
    struct.pack_into("<I", dex, 0x20, len(dex))  # file_size

    # signature = SHA-1 over everything after the signature field (offset 32).
    sig = hashlib.sha1(bytes(dex[32:])).digest()
    dex[12:32] = sig
    # checksum = Adler-32 over everything after the checksum field (offset 12).
    checksum = zlib.adler32(bytes(dex[12:])) & 0xFFFFFFFF
    struct.pack_into("<I", dex, 0x08, checksum)
    return bytes(dex)


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: make_dex.py <output.dex>\n")
        return 2
    with open(sys.argv[1], "wb") as f:
        f.write(build_dex())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
