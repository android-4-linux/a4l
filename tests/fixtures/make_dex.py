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
    "Ljava/lang/RuntimeException;",  # 30 type: thrown class
    "Ljava/lang/Exception;",         # 31 type: caught class
    "tryCatch",              # 32 method name (try/throw/catch/move-exception)
    "ab",                    # 33 const-string (object-array demo)
    "cde",                   # 34 const-string (object-array demo)
    "objArray",              # 35 method name (aget-object/aput-object)
    "total",                 # 36 static field name
    "staticInt",             # 37 method name (sget/sput int static)
    "label",                 # 38 Counter instance object field
    "tagLen",                # 39 method name (iget-object/iput-object)
    "wideSum",               # 40 method name (long arithmetic)
    "wideLoop",              # 41 method name (long at a stack-map frame point)
    "intArith",              # 42 method name (div/rem/and/or/xor/shl-int)
    "intLit",                # 43 method name (/2addr, /lit8, neg, not, int-to-byte)
    "[B",                    # 44 type: byte[]
    "byteArray",             # 45 method name (new-array byte, aput-byte, aget-byte)
    "longBits",              # 46 method name (long div/rem/shl/shr)
    "[J",                    # 47 type: long[]
    "wideArray",             # 48 method name (new-array long, aput-wide, aget-wide)
    "wf",                    # 49 static long field name
    "wideField",             # 50 method name (sget-wide/sput-wide)
    "checkCast",             # 51 method name (check-cast)
    "instTest",              # 52 method name (instance-of)
    "J",                     # 53 type: long (field descriptor)
    "Ljava/lang/CharSequence;",  # 54 type: interface for invoke-interface
    "ifaceTest",             # 55 method name (invoke-interface)
    "switchTest",            # 56 method name (packed-switch)
    "Ljava/lang/Class;",     # 57 type: for const-class
    "ci",                    # 58 static field name (<clinit> demo)
    "filledArray",           # 59 method name (filled-new-array)
    "constClass",            # 60 method name (const-class)
    "<clinit>",              # 61 static-initializer method name
    "clinitTest",            # 62 method name (reads <clinit>-set field)
    "IIIIIII",               # 63 shorty for (IIIIII)I
    "sum6",                  # 64 method name (6-int target for invoke/range)
    "rangeTest",             # 65 method name (invoke-static/range)
    "fillData",              # 66 method name (fill-array-data)
]
S = {s: i for i, s in enumerate(STRINGS)}

# type_ids -> string index for each descriptor.
TYPES = ["LHelloDex;", "Ljava/lang/Object;", "V", "I", "[Ljava/lang/String;",
         "Ljava/lang/String;", "Ljava/lang/System;", "Ljava/io/PrintStream;",
         "LCounter;", "[I", "Ljava/lang/RuntimeException;", "Ljava/lang/Exception;",
         "[B", "[J", "J", "Ljava/lang/CharSequence;", "Ljava/lang/Class;"]
T = {t: i for i, t in enumerate(TYPES)}


def code_item(regs, ins, outs, insns_u16):
    # registers_size, ins_size, outs_size, tries_size, debug_off, insns_size,
    # insns[], then padding to 4.
    body = struct.pack("<HHHHII", regs, ins, outs, 0, 0, len(insns_u16))
    for u in insns_u16:
        body += struct.pack("<H", u)
    return align4(bytearray(body))


def code_item_try(regs, ins, outs, insns_u16, tries, handlers_bytes):
    # tries: list of (start_addr, insn_count, handler_off) ; handlers_bytes is the
    # encoded_catch_handler_list (uleb size + handlers).
    body = struct.pack("<HHHHII", regs, ins, outs, len(tries), 0, len(insns_u16))
    for u in insns_u16:
        body += struct.pack("<H", u)
    if len(insns_u16) % 2 == 1:   # pad so try_item[] is 4-byte aligned
        body += b"\x00\x00"
    for (sa, ic, ho) in tries:
        body += struct.pack("<IHH", sa, ic, ho)
    body += handlers_bytes
    return align4(bytearray(body))


def build_dex():
    header_size = 112
    n_strings = len(STRINGS)
    n_types = len(TYPES)
    n_protos = 6    # + (IIIIII)I for sum6
    n_fields = 6    # + HelloDex.ci (static int, <clinit> demo)
    n_methods = 40  # + filledArray, constClass, <clinit>, clinitTest, sum6, rangeTest, fillData
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
    # type_list for (IIIIII) parameters: six ints  [sum6]
    data.extend(align4(bytearray()))
    type_list_6int_off = off_data + len(data)
    tl4 = struct.pack("<I", 6) + struct.pack("<H", T["I"]) * 6
    data.extend(align4(bytearray(tl4)))

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
    code_trycatch_off = off_data + len(data)
    # int tryCatch() { try { throw new RuntimeException(); }
    #                  catch (Exception e) { return 99; } }
    #   pc0 new-instance v0, RuntimeException(type10)
    #   pc2 invoke-direct {v0}, RuntimeException.<init>()V (m15)
    #   pc5 throw v0            <- try range [5,6)
    #   pc6 move-exception v0   <- handler (catch Exception)
    #   pc7 const/16 v0, 99 ; pc9 return v0
    trycatch_handlers = (uleb128(1)                          # 1 handler in list
                         + uleb128(1)                        # 1 typed catch, no catch-all
                         + uleb128(T["Ljava/lang/Exception;"])  # catch type
                         + uleb128(6))                       # handler addr = pc6
    data.extend(code_item_try(
        1, 0, 1,
        [0x0022, 0x000A, 0x1070, 0x000F, 0x0000, 0x0027,
         0x000D, 0x0013, 0x0063, 0x000F],
        tries=[(5, 1, 1)],              # start=pc5, count=1, handler_off=1
        handlers_bytes=trycatch_handlers))
    code_staticint_off = off_data + len(data)
    # int staticInt(){ total=7; total=total+3; return total; }   (10)  field 2
    data.extend(code_item(2, 0, 0, [
        0x7012, 0x0067, 0x0002, 0x0060, 0x0002, 0x3112,
        0x0090, 0x0100, 0x0067, 0x0002, 0x0060, 0x0002, 0x000F,
    ]))
    code_objarray_off = off_data + len(data)
    # int objArray(){ String[] a=new String[2]; a[0]="ab"; a[1]="cde";
    #                 return a[0].length()+a[1].length(); }   (2+3=5)
    #   v0=array v1=index v2=elem/result v3=elem2 ; String.length = method 6
    data.extend(code_item(4, 0, 1, [
        0x2012, 0x0023, 0x0004,          # const/4 v0,2 ; new-array v0,v0,String[]
        0x0112, 0x021A, S["ab"], 0x024D, 0x0100,   # v1=0; v2="ab"; a[0]=v2
        0x1112, 0x021A, S["cde"], 0x024D, 0x0100,  # v1=1; v2="cde"; a[1]=v2
        0x0112, 0x0246, 0x0100,          # v1=0; aget-object v2, v0, v1 (a[0])
        0x106E, 0x0006, 0x0002, 0x020A,  # invoke-virtual {v2} length; move-result v2
        0x1112, 0x0346, 0x0100,          # v1=1; aget-object v3, v0, v1 (a[1])
        0x106E, 0x0006, 0x0003, 0x030A,  # invoke-virtual {v3} length; move-result v3
        0x0090, 0x0302, 0x000F,          # add-int v0,v2,v3 ; return v0
    ]))
    code_widesum_off = off_data + len(data)
    # int wideSum(){ long a=1e12; long b=2; long c=a+b; return (int)(c-a); }  (2)
    #   v0/v1=a, v2/v3=b, v4/v5=c ; registers=6
    _WV = 1000000000000
    _wlo = [(_WV >> (16 * k)) & 0xFFFF for k in range(4)]
    data.extend(code_item(6, 0, 0, [
        0x0018, _wlo[0], _wlo[1], _wlo[2], _wlo[3],  # const-wide v0, 1e12
        0x0216, 0x0002,                # const-wide/16 v2, 2
        0x049B, 0x0200,                # add-long v4, v0, v2
        0x049C, 0x0004,                # sub-long v4, v4, v0
        0x4084,                        # long-to-int v0, v4
        0x000F,                        # return v0
    ]))
    code_wideloop_off = off_data + len(data)
    # int wideLoop(){ long sum=0; for(i=0;i<5;i++) sum+=(long)i; return (int)sum; } (10)
    #   v0/v1=sum, v2=i, v3=limit, v4/v5=(long)i ; registers=6.
    #   The loop head's frame has a long local (sum) -> exercises StackMapTable + wide.
    data.extend(code_item(6, 0, 0, [
        0x0018, 0x0000, 0x0000, 0x0000, 0x0000,  # const-wide v0, 0 (sum)
        0x0212, 0x5312,                # const/4 v2,0 (i) ; const/4 v3,5 (limit)
        0x3235, 0x0008,                # :loop if-ge v2,v3,:end
        0x2481,                        # int-to-long v4, v2
        0x009B, 0x0400,                # add-long v0,v0,v4  (sum += (long)i)
        0x02D8, 0x0102,                # add-int/lit8 v2,v2,1
        0xF928,                        # goto :loop
        0x0084, 0x000F,                # :end long-to-int v0,v0 ; return v0
    ]))
    code_intarith_off = off_data + len(data)
    # int intArith(){ (((((100/7)%9)&6)|3)^2)<<1 }  = 10   (div,rem,and,or,xor,shl)
    data.extend(code_item(4, 0, 0, [
        0x0013, 0x0064, 0x7112,        # const/16 v0,100 ; const/4 v1,7
        0x0293, 0x0100,                # div-int v2,v0,v1   -> 14
        0x0313, 0x0009, 0x0294, 0x0302,  # const/16 v3,9 ; rem-int v2,v2,v3 -> 5
        0x6312, 0x0295, 0x0302,        # const/4 v3,6 ; and-int v2,v2,v3 -> 4
        0x3312, 0x0296, 0x0302,        # const/4 v3,3 ; or-int  v2,v2,v3 -> 7
        0x2312, 0x0297, 0x0302,        # const/4 v3,2 ; xor-int v2,v2,v3 -> 5
        0x1312, 0x0298, 0x0302,        # const/4 v3,1 ; shl-int v2,v2,v3 -> 10
        0x020F,                        # return v2
    ]))
    code_intlit_off = off_data + len(data)
    # int intLit(){ v=5*3; v/=3; v&=6; v=-v; v=~v; v+=7; return (byte)v; }  = 10
    data.extend(code_item(2, 0, 0, [
        0x5012, 0x3112,                # const/4 v0,5 ; const/4 v1,3
        0x10B2,                        # mul-int/2addr v0,v1 -> 15
        0x00DB, 0x0300,                # div-int/lit8 v0,v0,3 -> 5
        0x00DD, 0x0600,                # and-int/lit8 v0,v0,6 -> 4
        0x007B,                        # neg-int v0,v0 -> -4
        0x007C,                        # not-int v0,v0 -> 3
        0x00D8, 0x0700,                # add-int/lit8 v0,v0,7 -> 10
        0x008D,                        # int-to-byte v0,v0 -> 10
        0x000F,                        # return v0
    ]))
    code_bytearray_off = off_data + len(data)
    # int byteArray(){ byte[] a=new byte[3]; a[0]=10;a[1]=20;a[2]=12;
    #                  return a[0]+a[1]+a[2]; }  = 42   (new-array byte/aput-byte/aget-byte)
    data.extend(code_item(3, 0, 0, [
        0x3112, 0x1023, T["[B"],       # const/4 v1,3 ; new-array v0,v1,byte[]
        0x0112, 0x0213, 0x000A, 0x024F, 0x0100,  # a[0]=10 (aput-byte)
        0x1112, 0x0213, 0x0014, 0x024F, 0x0100,  # a[1]=20
        0x2112, 0x0213, 0x000C, 0x024F, 0x0100,  # a[2]=12
        0x0112, 0x0148, 0x0100,        # const/4 v1,0 ; aget-byte v1,v0,v1 (a[0])
        0x1212, 0x0248, 0x0200,        # const/4 v2,1 ; aget-byte v2,v0,v2 (a[1])
        0x0190, 0x0201,                # add-int v1,v1,v2
        0x2212, 0x0248, 0x0200,        # const/4 v2,2 ; aget-byte v2,v0,v2 (a[2])
        0x0190, 0x0201,                # add-int v1,v1,v2
        0x010F,                        # return v1
    ]))
    code_longbits_off = off_data + len(data)
    # int longBits(){ long q=1000/7; q%=100; q<<=1; q>>=1; return (int)q; }  = 42
    #   v0/v1=q, v2/v3=scratch ; shift amount is an int in v2.
    data.extend(code_item(4, 0, 0, [
        0x0018, 0x03E8, 0x0000, 0x0000, 0x0000,  # const-wide v0, 1000
        0x0216, 0x0007,                # const-wide/16 v2, 7
        0x009E, 0x0200,                # div-long v0,v0,v2  -> 142
        0x0216, 0x0064,                # const-wide/16 v2, 100
        0x009F, 0x0200,                # rem-long v0,v0,v2  -> 42
        0x1212,                        # const/4 v2, 1  (int shift amount)
        0x00A3, 0x0200,                # shl-long v0,v0,v2  -> 84
        0x00A4, 0x0200,                # shr-long v0,v0,v2  -> 42
        0x0084, 0x000F,                # long-to-int v0,v0 ; return v0
    ]))
    code_widearray_off = off_data + len(data)
    # int wideArray(){ long[] a=new long[2]; a[0]=40; a[1]=2;
    #                  return (int)(a[0]+a[1]); }  = 42  (new-array long/aput-wide/aget-wide)
    data.extend(code_item(6, 0, 0, [
        0x2012, 0x0023, T["[J"],       # const/4 v0,2 ; new-array v0,v0,long[]
        0x0112, 0x0216, 0x0028, 0x024C, 0x0100,  # a[0]=40 (aput-wide)
        0x1112, 0x0216, 0x0002, 0x024C, 0x0100,  # a[1]=2
        0x0112, 0x0245, 0x0100,        # const/4 v1,0 ; aget-wide v2,v0,v1 (a[0])
        0x1112, 0x0445, 0x0100,        # const/4 v1,1 ; aget-wide v4,v0,v1 (a[1])
        0x029B, 0x0402,                # add-long v2,v2,v4
        0x2084, 0x000F,                # long-to-int v0,v2 ; return v0
    ]))
    code_widefield_off = off_data + len(data)
    # int wideField(){ wf = 40; return (int)(wf + 2); }  = 42   (sput/sget-wide, field 4)
    data.extend(code_item(4, 0, 0, [
        0x0016, 0x0028, 0x0068, 0x0004,  # const-wide/16 v0,40 ; sput-wide v0, wf
        0x0061, 0x0004,                # sget-wide v0, wf  -> 40
        0x0216, 0x0002, 0x009B, 0x0200,  # const-wide/16 v2,2 ; add-long v0,v0,v2
        0x0084, 0x000F,                # long-to-int v0,v0 ; return v0
    ]))
    code_checkcast_off = off_data + len(data)
    # int checkCast(){ Object o="hello"; return ((String)o).length(); }  = 5
    data.extend(code_item(1, 0, 1, [
        0x001A, S["hello"], 0x001F, 0x0005,  # const-string v0,"hello" ; check-cast v0,String
        0x106E, 0x0006, 0x0000, 0x000A, 0x000F,  # invoke length; move-result v0; return v0
    ]))
    code_insttest_off = off_data + len(data)
    # int instTest(){ Object o="hello"; return (o instanceof String) ? 42 : 0; }  = 42
    data.extend(code_item(1, 0, 0, [
        0x001A, S["hello"],            # const-string v0,"hello"
        0x0020, 0x0005,                # instance-of v0, v0, String  -> 1
        0x0038, 0x0005,                # if-eqz v0, :else
        0x0013, 0x002A, 0x000F,        # const/16 v0,42 ; return v0
        0x0012, 0x000F,                # :else const/4 v0,0 ; return v0
    ]))
    code_iface_off = off_data + len(data)
    # int ifaceTest(){ CharSequence cs="hello"; return cs.length(); }  = 5
    #   invoke-interface CharSequence.length (method 30)
    data.extend(code_item(1, 0, 1, [
        0x001A, S["hello"],            # const-string v0,"hello"
        0x1072, 0x001E, 0x0000,        # invoke-interface {v0}, CharSequence.length (m30)
        0x000A, 0x000F,                # move-result v0 ; return v0
    ]))
    code_switch_off = off_data + len(data)
    # int switchTest(){ int x=2; switch(x){case 0:10;case 1:20;case 2:42;default:0;} }  = 42
    data.extend(code_item(1, 0, 0, [
        0x2012,                        # pc0  const/4 v0, 2
        0x002B, 0x000E, 0x0000,        # pc1  packed-switch v0, +14 (payload @ pc15)
        0x0012, 0x000F,                # pc4  (default) const/4 v0,0 ; return v0
        0x0013, 0x000A, 0x000F,        # pc6  :c0 const/16 v0,10 ; return
        0x0013, 0x0014, 0x000F,        # pc9  :c1 const/16 v0,20 ; return
        0x0013, 0x002A, 0x000F,        # pc12 :c2 const/16 v0,42 ; return
        # pc15 packed-switch-payload: ident, size=3, first_key=0, targets to c0/c1/c2
        0x0100, 0x0003, 0x0000, 0x0000,
        0x0005, 0x0000, 0x0008, 0x0000, 0x000B, 0x0000,
    ]))
    code_filled_off = off_data + len(data)
    # int filledArray(){ int[] a={10,20,12}; return a[0]+a[1]+a[2]; }  = 42
    data.extend(code_item(4, 0, 3, [
        0x0013, 0x000A,                # const/16 v0, 10
        0x0113, 0x0014,                # const/16 v1, 20
        0x0213, 0x000C,                # const/16 v2, 12
        0x3024, T["[I"], 0x0210,       # filled-new-array {v0,v1,v2}, [I
        0x030C,                        # move-result-object v3
        0x0012, 0x0044, 0x0003,        # const/4 v0,0 ; aget v0, v3, v0   (=10)
        0x1112, 0x0144, 0x0103,        # const/4 v1,1 ; aget v1, v3, v1   (=20)
        0x0090, 0x0100,                # add-int v0, v0, v1               (=30)
        0x2112, 0x0144, 0x0103,        # const/4 v1,2 ; aget v1, v3, v1   (=12)
        0x0090, 0x0100,                # add-int v0, v0, v1               (=42)
        0x000F,                        # return v0
    ]))
    code_constclass_off = off_data + len(data)
    # int constClass(){ return (String.class instanceof Class) ? 42 : 0; }  = 42
    data.extend(code_item(2, 0, 0, [
        0x001C, T["Ljava/lang/String;"],  # const-class v0, String
        0x0020, T["Ljava/lang/Class;"],   # instance-of v0, v0, Class   (=1)
        0x0113, 0x002A,                # const/16 v1, 42
        0x0092, 0x0100,                # mul-int v0, v0, v1              (=42)
        0x000F,                        # return v0
    ]))
    code_clinit_off = off_data + len(data)
    # static { ci = 77; }
    data.extend(code_item(1, 0, 0, [
        0x0013, 0x004D,                # const/16 v0, 77
        0x0067, 0x0005,                # sput v0, HelloDex.ci (field 5)
        0x000E,                        # return-void
    ]))
    code_clinittest_off = off_data + len(data)
    # int clinitTest(){ return ci; }  = 77  (set by <clinit> at class init)
    data.extend(code_item(1, 0, 0, [
        0x0060, 0x0005,                # sget v0, HelloDex.ci (field 5)
        0x000F,                        # return v0
    ]))
    code_sum6_off = off_data + len(data)
    # int sum6(int a..f){ return a+b+c+d+e+f; }  (params in v0..v5)
    data.extend(code_item(6, 6, 0, [
        0x0090, 0x0100,                # add-int v0, v0, v1
        0x0090, 0x0200,                # add-int v0, v0, v2
        0x0090, 0x0300,                # add-int v0, v0, v3
        0x0090, 0x0400,                # add-int v0, v0, v4
        0x0090, 0x0500,                # add-int v0, v0, v5
        0x000F,                        # return v0
    ]))
    code_range_off = off_data + len(data)
    # int rangeTest(){ return sum6(1,2,3,4,5,6); }  = 21  (invoke-static/range)
    data.extend(code_item(6, 0, 6, [
        0x1012, 0x2112, 0x3212,        # const/4 v0,1 ; v1,2 ; v2,3
        0x4312, 0x5412, 0x6512,        # const/4 v3,4 ; v4,5 ; v5,6
        0x0677, 0x0025, 0x0000,        # invoke-static/range {v0..v5}, sum6 (m37)
        0x000A, 0x000F,                # move-result v0 ; return v0
    ]))
    code_filldata_off = off_data + len(data)
    # int fillData(){ int[] a={5,15,22}; return a[0]+a[1]+a[2]; }  = 42  (fill-array-data)
    data.extend(code_item(4, 0, 0, [
        0x3112,                        # pc0  const/4 v1, 3   (size)
        0x1023, T["[I"],               # pc1  new-array v0, v1, [I
        0x0026, 0x0011, 0x0000,        # pc3  fill-array-data v0, +17 (payload @ pc20)
        0x0112, 0x0244, 0x0100,        # pc6  const/4 v1,0 ; aget v2, v0, v1   (=5)
        0x1112, 0x0344, 0x0100,        # pc9  const/4 v1,1 ; aget v3, v0, v1   (=15)
        0x0290, 0x0302,                # pc12 add-int v2, v2, v3                (=20)
        0x2112, 0x0344, 0x0100,        # pc14 const/4 v1,2 ; aget v3, v0, v1   (=22)
        0x0290, 0x0302,                # pc17 add-int v2, v2, v3                (=42)
        0x020F,                        # pc19 return v2
        # pc20 fill-array-data-payload: ident, width=4, size=3, data 5/15/22
        0x0300, 0x0004, 0x0003, 0x0000,
        0x0005, 0x0000, 0x000F, 0x0000, 0x0016, 0x0000,
    ]))

    # class_data_item
    class_data_off = off_data + len(data)
    cd = bytearray()
    cd += uleb128(3)  # static_fields_size (HelloDex.total, HelloDex.wf, HelloDex.ci)
    cd += uleb128(0)  # instance_fields_size
    cd += uleb128(30)  # direct_methods_size
    cd += uleb128(0)  # virtual_methods_size
    # static fields: total=f2, wf=f4, ci=f5 (cumulative diffs from 0)
    cd += uleb128(2) + uleb128(0x9)                               # f2 total, public static
    cd += uleb128(2) + uleb128(0x9)                               # +2 -> f4 wf, public static
    cd += uleb128(1) + uleb128(0x9)                               # +1 -> f5 ci, public static
    # direct methods, sorted by ascending method index; idx diffs are cumulative.
    # m0=main..m5=strlen, m7=hello, m14=arraySum, m16=tryCatch, m17=staticInt, m18=objArray.
    cd += uleb128(0) + uleb128(0x9) + uleb128(code_main_off)      # diff 0 -> idx 0
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_answer_off)    # diff 1 -> idx 1
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_misc_off)      # diff 1 -> idx 2
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_loopsum_off)   # diff 1 -> idx 3
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_callsum_off)   # diff 1 -> idx 4
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_strlen_off)    # diff 1 -> idx 5
    cd += uleb128(2) + uleb128(0x9) + uleb128(code_hello_off)     # diff 2 -> idx 7
    cd += uleb128(7) + uleb128(0x9) + uleb128(code_arraysum_off)  # diff 7 -> idx 14
    cd += uleb128(2) + uleb128(0x9) + uleb128(code_trycatch_off)  # diff 2 -> idx 16
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_staticint_off) # diff 1 -> idx 17
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_objarray_off)  # diff 1 -> idx 18
    cd += uleb128(2) + uleb128(0x9) + uleb128(code_widesum_off)   # diff 2 -> idx 20
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_wideloop_off)  # diff 1 -> idx 21
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_intarith_off)  # diff 1 -> idx 22
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_intlit_off)    # diff 1 -> idx 23
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_bytearray_off) # diff 1 -> idx 24
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_longbits_off)  # diff 1 -> idx 25
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_widearray_off) # diff 1 -> idx 26
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_widefield_off) # diff 1 -> idx 27
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_checkcast_off) # diff 1 -> idx 28
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_insttest_off)  # diff 1 -> idx 29
    cd += uleb128(2) + uleb128(0x9) + uleb128(code_iface_off)     # diff 2 -> idx 31 (m30=CharSequence.length)
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_switch_off)    # diff 1 -> idx 32
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_filled_off)    # diff 1 -> idx 33
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_constclass_off)  # diff 1 -> idx 34
    cd += uleb128(1) + uleb128(0x10008) + uleb128(code_clinit_off)  # diff 1 -> idx 35 (<clinit>: ACC_STATIC|ACC_CONSTRUCTOR)
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_clinittest_off)  # diff 1 -> idx 36
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_sum6_off)        # diff 1 -> idx 37
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_range_off)       # diff 1 -> idx 38
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_filldata_off)    # diff 1 -> idx 39
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
    code_ctag_off = off_data + len(data)
    # int tagLen(){ label = "ab"; return label.length(); }  (2)
    #   registers=2, ins=1: v1=this, v0=temp. label = field id 3.
    #   const-string v0,"ab" ; iput-object v0,v1,label ; iget-object v0,v1,label ;
    #   invoke-virtual {v0}, String.length (m6) ; move-result v0 ; return v0
    data.extend(code_item(2, 1, 1, [
        0x001A, S["ab"], 0x105B, 0x0003, 0x1054, 0x0003,
        0x106E, 0x0006, 0x0000, 0x000A, 0x000F,
    ]))
    code_ccount_off = off_data + len(data)
    # static int useCounter(){ Counter c=new Counter(10); c.add(5);
    #                          return c.get() + c.tagLen(); }   (15 + 2 = 17)
    data.extend(code_item(2, 0, 2, [
        0x0022, 0x0008, 0x0113, 0x000A, 0x2070, 0x000A, 0x0010,   # new + <init>(10)
        0x5112, 0x206E, 0x000C, 0x0010,                          # c.add(5)
        0x106E, 0x000B, 0x0000, 0x010A,                          # c.get(); move-result v1
        0x106E, 0x0013, 0x0000, 0x000A,                          # c.tagLen(m19); move-result v0
        0x0090, 0x0001, 0x000F,                                  # add-int v0,v1,v0 ; return v0
    ]))

    counter_class_data_off = off_data + len(data)
    ccd = bytearray()
    ccd += uleb128(0)  # static_fields_size
    ccd += uleb128(2)  # instance_fields_size (value, label)
    ccd += uleb128(2)  # direct_methods_size  (<init>, useCounter)
    ccd += uleb128(3)  # virtual_methods_size (get, add, tagLen)
    # instance fields: value=f1, label=f3 (cumulative diffs from 0)
    ccd += uleb128(1) + uleb128(0x1)                             # f1 value, public
    ccd += uleb128(2) + uleb128(0x1)                             # +2 -> f3 label, public
    # direct methods: <init>=m10, useCounter=m13
    ccd += uleb128(10) + uleb128(0x1) + uleb128(code_cinit_off)  # idx 10, public
    ccd += uleb128(3) + uleb128(0x9) + uleb128(code_ccount_off)  # +3 -> idx 13, public static
    # virtual methods: get=m11, add=m12, tagLen=m19
    ccd += uleb128(11) + uleb128(0x1) + uleb128(code_cget_off)   # idx 11, public
    ccd += uleb128(1) + uleb128(0x1) + uleb128(code_cadd_off)    # +1 -> idx 12, public
    ccd += uleb128(7) + uleb128(0x1) + uleb128(code_ctag_off)    # +7 -> idx 19, public
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
        (0x2001, 35, code_main_off),
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
    # proto 5: (IIIIII)I -> shorty "IIIIIII", return I, params (6 ints)
    proto_ids += struct.pack("<III", S["IIIIIII"], T["I"], type_list_6int_off)

    # field_ids: {class_idx(u16), type_idx(u16), name_idx(u32)}
    field_ids = struct.pack("<HHI", T["Ljava/lang/System;"],
                            T["Ljava/io/PrintStream;"], S["out"])     # f0 System.out
    field_ids += struct.pack("<HHI", T["LCounter;"], T["I"], S["value"])  # f1 Counter.value
    field_ids += struct.pack("<HHI", T["LHelloDex;"], T["I"], S["total"])  # f2 HelloDex.total
    field_ids += struct.pack("<HHI", T["LCounter;"], T["Ljava/lang/String;"], S["label"])  # f3
    field_ids += struct.pack("<HHI", T["LHelloDex;"], T["J"], S["wf"])  # f4 HelloDex.wf:long
    field_ids += struct.pack("<HHI", T["LHelloDex;"], T["I"], S["ci"])  # f5 HelloDex.ci:int

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
    method_ids += struct.pack("<HHI", T["Ljava/lang/RuntimeException;"], 3, S["<init>"])  # m15 ()V
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["tryCatch"])   # m16 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["staticInt"])  # m17 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["objArray"])   # m18 ()I
    method_ids += struct.pack("<HHI", T["LCounter;"], 1, S["tagLen"])       # m19 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["wideSum"])      # m20 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["wideLoop"])     # m21 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["intArith"])     # m22 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["intLit"])       # m23 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["byteArray"])    # m24 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["longBits"])     # m25 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["wideArray"])    # m26 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["wideField"])    # m27 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["checkCast"])    # m28 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["instTest"])     # m29 ()I
    method_ids += struct.pack("<HHI", T["Ljava/lang/CharSequence;"], 1, S["length"])  # m30 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["ifaceTest"])    # m31 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["switchTest"])   # m32 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["filledArray"])  # m33 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["constClass"])   # m34 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 3, S["<clinit>"])     # m35 ()V (proto 3)
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["clinitTest"])   # m36 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 5, S["sum6"])         # m37 (IIIIII)I (proto 5)
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["rangeTest"])    # m38 ()I
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["fillData"])     # m39 ()I

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
