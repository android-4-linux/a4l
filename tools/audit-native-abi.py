#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l tooling; the JNI calling conventions for @FastNative and
# @CriticalNative as documented in AOSP (dalvik.annotation.optimization). No GPL-3.0 sources.
"""Check a4l's native method tables against the framework's Java declarations.

  audit-native-abi.py [--all] [source files or directories ...]

ART calls a native method by its Java annotation:

  (none), @FastNative   fn(JNIEnv*, jclass or jobject, declared arguments...)
  @CriticalNative       fn(declared arguments...)   -- no JNIEnv, no class

A table entry that binds a function of the other shape still registers and still runs. Its
arguments are read one or two registers off: a "handle" is then whatever the caller left
there. That has crashed a4l more than once (the font natives, HardwareBuffer.nEstimateSize)
and quietly corrupted a value (Binder.setThreadStrictModePolicy). It cannot be seen by
reading the C++ alone, so this reads both sides.

For every JNINativeMethod entry in the given sources (default: src/ and tools/aosp/) it finds
the class the table is registered on, looks the method up in the framework's class files with
javap, and reports:

  BAD      the function takes or lacks a JNIEnv* against the annotation, and the method has
           declared arguments, so they are misread
  benign   the same mismatch on a method with no declared arguments: nothing to misread.
           Left as it is where one function serves both ART and the JVM backend
  unknown  the class or the function could not be found; check by hand

Entries on a4l's own Java classes (org.acl.internal.*, and its shims of framework classes
for the JVM backend) are counted apart: their Java is in framework/ and uses neither
annotation.

--all also lists the entries that agree. Exits 1 if anything is BAD.

Needs a host ART build: ACL_ART_HOME (out/host/linux-x86), from which the framework's
classes.jar files and a JDK's javap are found. It is not part of CI, which has no AOSP tree;
run it after adding or changing a native table.
"""
import glob
import os
import re
import subprocess
import sys

JARS = ("framework-minus-apex_intermediates", "framework-graphics_intermediates",
        "ext_intermediates", "core-oj.com.android.art.debug_intermediates",
        "core-libart.com.android.art.debug_intermediates")
STRING = r'"(?:[^"\\]|\\.)*"'
# A table: `JNINativeMethod name[] = {` or `std::array<JNINativeMethod, N> name{{`.
TABLE = re.compile(r'JNINativeMethod(?:\s*,\s*\d+\s*>)?\s+(\w+)\s*(?:\[[^\]]*\])?\s*(?:=\s*)?\{')
CLASS_LIT = re.compile(r'"((?:android|com|java|javax|dalvik|libcore|org|sun)/[\w/$]+)"')
LOOKUP = re.compile(r'(?:FindClass|find_class_optional)\s*\((?:[^()"]|\([^()]*\))*?' + STRING)
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def unquote(s):
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', s))


def blank(text):
    """The text with comments and string contents replaced by spaces, same length, so that
    brackets can be counted."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
        elif two == "/*":
            j = text.find("*/", i)
            j = n if j < 0 else j + 2
        elif text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):  # keep the quotes
                out[k] = " "
            i = j + 1
            continue
        elif text[i] == "'" and i + 2 < n and (text[i + 2] == "'" or text[i + 1] == "\\"):
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            j += 1
        else:
            i += 1
            continue
        for k in range(i, min(j, n)):
            if out[k] != "\n":
                out[k] = " "
        i = j
    return "".join(out)


def close_of(code, open_pos):
    """The index of the bracket closing the one at `open_pos`."""
    depth = 0
    for i in range(open_pos, len(code)):
        ch = code[i]
        if ch in "{([":
            depth += 1
        elif ch in "})]":
            depth -= 1
            if depth == 0:
                return i
    return len(code) - 1


def split_top(code, text, start, end):
    """The pieces of text[start:end] between top-level commas, each with its position."""
    parts, depth, last = [], 0, start
    for i in range(start, end):
        ch = code[i]
        if ch in "{([":
            depth += 1
        elif ch in "})]":
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append((last, text[last:i]))
            last = i + 1
    if text[last:end].strip():
        parts.append((last, text[last:end]))
    return parts


def expand(piece, macros):
    """A signature written as adjacent string literals and string macros, joined."""
    out = []
    for tok in re.finditer(r'(' + STRING + r')|([A-Za-z_]\w*)', piece):
        if tok.group(1):
            out.append(unquote(tok.group(1)))
        elif tok.group(2) in macros:
            out.append(macros[tok.group(2)])
    return "".join(out)


def entries_of(text, code):
    """Every table's entries as (position, table name, table position, name, sig, fn)."""
    macros = {m.group(1): unquote(m.group(2))
              for m in re.finditer(r'^[ \t]*#define\s+(\w+)\s+((?:' + STRING + r'[ \t]*)+)$', text,
                                   re.M)}
    for t in TABLE.finditer(code):
        open_pos = t.end() - 1
        close = close_of(code, open_pos)
        inner_start, body_end = open_pos + 1, close
        # std::array's double brace: the inner brace spans the whole body.
        m = re.match(r"\s*\{", code[inner_start:close])
        if m:
            first = inner_start + m.end() - 1
            first_close = close_of(code, first)
            if code[first_close + 1:close].strip(" \n,") == "":
                inner_start, body_end = first + 1, first_close
        for pos, piece in split_top(code, text, inner_start, body_end):
            # Where the entry itself starts, past any comment before it.
            bare = code[pos:pos + len(piece)]
            if not bare.strip():
                continue
            lead = pos + len(bare) - len(bare.lstrip())
            if bare.strip().startswith("{"):
                inner = split_top(code, text, lead + 1, close_of(code, lead))
            else:  # NAME("name", "sig", fn)
                paren = code.find("(", lead, pos + len(piece))
                if paren < 0:
                    continue
                inner = split_top(code, text, paren + 1, close_of(code, paren))
            if len(inner) < 3:
                continue
            name, sig = unquote(inner[0][1]), expand(inner[1][1], macros)
            fn = ",".join(x[1] for x in inner[2:]).strip()
            fn = re.sub(r"^reinterpret_cast<void\*>\(\s*(.*)\)$", r"\1", fn, flags=re.S)
            fn = re.sub(r"^\(void\*\)\s*", "", fn).strip()
            yield lead, t.group(1), t.start(), name, sig, fn


def find_tools():
    home = os.environ.get("ACL_ART_HOME", "")
    if not home or not os.path.isdir(home):
        sys.exit("audit-native-abi: set ACL_ART_HOME to a host ART build (out/host/linux-x86)")
    root = os.path.abspath(os.path.join(home, "..", "..", ".."))

    # The newest JDK there, by number: "jdk9" sorts after "jdk17" as text.
    def version(path):
        m = re.search(r"/jdk(\d+)/", path)
        return int(m.group(1)) if m else 0

    javap = next((p for p in sorted(glob.glob(root + "/prebuilts/jdk/jdk*/linux-x86/bin/javap"),
                                    key=version, reverse=True) if os.access(p, os.X_OK)), None)
    jars = [j for name in JARS
            for j in glob.glob(f"{root}/out/target/common/obj/JAVA_LIBRARIES/{name}/classes.jar")]
    if not javap or not jars:
        sys.exit("audit-native-abi: no javap or no framework classes.jar under " + root)
    return javap, ":".join(jars)


def java_natives(javap, classpath, cls, cache={}):
    """{(name, descriptor): [annotation, declared argument count]} for a class, or None."""
    if cls in cache:
        return cache[cls]
    r = subprocess.run([javap, "-v", "-p", "-cp", classpath, cls.replace("/", ".")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        cache[cls] = None
        return None
    lines = r.stdout.splitlines()
    # An annotation is printed as "N: #index()", and by a newer javap also by name on the
    # next line. The constant pool names it for either.
    pool = {}
    for line in lines:
        c = re.match(r"^\s*(#\d+) = Utf8\s+(\S+)", line)
        if c:
            pool[c.group(1)] = c.group(2)
    out, cur = {}, None
    for line in lines:
        m = re.match(r"^  (?:[\w.$<>\[\], ?]+ )?([\w$<>]+)\(.*\)(?: throws .*)?;$", line)
        if m:
            cur = [m.group(1), None, " native " in " " + line]
            continue
        if cur is None or not cur[2]:
            continue
        d = re.match(r"^\s+descriptor: (\S+)", line)
        if d:
            cur[1] = d.group(1)
            args = re.findall(r"\[*(?:L[^;]+;|[ZBCSIJFD])", d.group(1)[1:d.group(1).index(")")])
            out[(cur[0], cur[1])] = ["normal", len(args)]
        elif cur[1]:
            a = re.match(r"^\s+\d+: (#\d+)\(", line)
            named = pool.get(a.group(1), "") if a else line
            if "CriticalNative" in named:
                out[(cur[0], cur[1])][0] = "critical"
            elif "FastNative" in named:
                out[(cur[0], cur[1])][0] = "fast"
    cache[cls] = out
    return out


def takes_env(text, fn):
    """Whether the bound C++ function's first parameter is a JNIEnv*. None if not found."""
    fn = fn.strip()
    if fn.startswith("+[") or fn.startswith("["):
        return "JNIEnv" in fn[fn.index("(") + 1:fn.index(")")]
    ident = re.sub(r"^&", "", fn).split("::")[-1].strip()
    if not re.fullmatch(r"\w+", ident):
        return None
    m = re.search(r"\b" + re.escape(ident) + r"\s*\(([^)]*)\)\s*(?:noexcept\s*)?\{", text)
    if m:
        return "JNIEnv" in m.group(1)
    # A lambda held in a variable: `auto ident = +[](...)`.
    m = re.search(r"\b" + re.escape(ident) + r"\s*=\s*\+?\[[^\]]*\]\s*\(([^)]*)\)", text)
    if m:
        return "JNIEnv" in m.group(1)
    # Defined by a macro: MACRO(ident, ...) at the start of a line, where the macro's body
    # declares `first_parameter(...)`.
    use = re.search(r"^[ \t]*([A-Z_][A-Z0-9_]*)\(\s*" + re.escape(ident) + r"\b", text, re.M)
    if not use:
        return None
    d = re.search(r"#define\s+" + use.group(1) + r"\((\w+)[^)]*\)((?:.*\\\n)*.*)", text)
    if not d:
        return None
    sig = re.search(r"\b" + d.group(1) + r"\s*\(([^)]*)\)", d.group(2))
    return ("JNIEnv" in sig.group(1)) if sig else None


def class_of(text, table, table_pos):
    """The class a table is registered on. In order: named just before the table in a call
    (`bind(env, "cls", table`); the class the variable passed with the table was looked up
    as (`RegisterNatives(c, table` after `c = FindClass("cls")`); the last class looked up
    before the table; the first looked up after it."""
    for m in re.finditer(r"(" + STRING + r")\s*,\s*" + re.escape(table) + r"\b", text):
        lit = CLASS_LIT.fullmatch(m.group(1))
        if lit:
            return lit.group(1)
    for m in re.finditer(r"\(\s*(?:env\s*,\s*)?(\w+)\s*,\s*" + re.escape(table) +
                         r"\s*(?:\.data\(\))?\s*[,)]", text):
        if m.start() < table_pos:
            continue  # a use of the name before this table: another table of the same name
        var, best = m.group(1), None
        for d in re.finditer(r"\b" + re.escape(var) +
                             r"\s*=\s*[^;]*?(?:FindClass|find_class_optional)"
                             r"\s*\((?:[^()\"]|\([^()]*\))*?(" + STRING + r")", text):
            if d.start() < m.start():
                best = d.group(1)
        if best and CLASS_LIT.fullmatch(best):
            return CLASS_LIT.fullmatch(best).group(1)
        break
    last = None
    for m in LOOKUP.finditer(text, max(0, table_pos - 6000), table_pos):
        lit = CLASS_LIT.findall(m.group(0))
        if lit:
            last = lit[-1]
    if last:
        return last
    # A file-scope table registered by a function further down.
    for m in LOOKUP.finditer(text, table_pos, min(len(text), table_pos + 6000)):
        lit = CLASS_LIT.findall(m.group(0))
        if lit:
            return lit[-1]
    return None


def own_java(cls, name):
    """For a class a4l has Java source for: "plain" if that source declares the native
    `name` and uses neither annotation, "annotated" if it uses one, None if there is no
    such source or no such native in it."""
    found = None
    # A nested class is in its outer class's file.
    for path in glob.glob(os.path.join(REPO, "framework", "*", cls.split("$")[0] + ".java")):
        src = open(path, encoding="utf-8", errors="replace").read()
        if not re.search(r"\bnative\b[^;(]*\b" + re.escape(name) + r"\s*\(", src):
            continue
        if "CriticalNative" in src or "FastNative" in src:
            return "annotated"
        found = "plain"
    return found


def audit(path, javap, classpath, show_all):
    text = open(path, encoding="utf-8", errors="replace").read()
    code = blank(text)
    counts = {"BAD": 0, "benign": 0, "unknown": 0, "ok": 0, "own": 0}
    for pos, table, table_pos, name, sig, fn in entries_of(text, code):
        if not re.fullmatch(r"\(.*\).+", sig):
            continue  # not a method table entry
        cls = class_of(text, table, table_pos)
        natives = java_natives(javap, classpath, cls) if cls else None
        where = f"{path}:{text.count(chr(10), 0, pos) + 1}"
        if natives is None or (name, sig) not in natives:
            # Not the framework's: a class of a4l's own, or its shim of a framework class
            # for the JVM backend. Those are plain JNI unless their Java says otherwise.
            own = own_java(cls, name) if cls else None
            if own == "plain":
                counts["own"] += 1
                if show_all:
                    print(f"own      {cls}.{name}{sig}  {where}")
                continue
            counts["unknown"] += 1
            print(f"unknown  {cls or '?'}.{name}{sig}  {where}  "
                  f"({'annotated in a4l Java: check by hand' if own else 'class not found'})")
            continue
        annotation, nargs = natives[(name, sig)]
        env = takes_env(text, fn)
        if env is None:
            counts["unknown"] += 1
            print(f"unknown  {cls}.{name}{sig}  {where}  (cannot find the function "
                  f"{' '.join(fn.split())[:40]})")
            continue
        if env != (annotation == "critical"):
            counts["ok"] += 1
            if show_all:
                print(f"ok       @{annotation:8} {cls}.{name}{sig}  {where}")
            continue
        kind = "BAD" if nargs > 0 else "benign"
        counts[kind] += 1
        want = "no JNIEnv*" if annotation == "critical" else "a JNIEnv*"
        print(f"{kind:8} @{annotation:8} {cls}.{name}{sig}  {where}  wants {want}")
    return counts


def main():
    args = [a for a in sys.argv[1:] if a != "--all"]
    show_all = "--all" in sys.argv[1:]
    roots = args or [os.path.join(REPO, "src"), os.path.join(REPO, "tools", "aosp")]
    files = []
    for r in roots:
        files += sorted(glob.glob(os.path.join(r, "**", "*.cpp"), recursive=True)) \
            if os.path.isdir(r) else [r]
    javap, classpath = find_tools()
    total = {"BAD": 0, "benign": 0, "unknown": 0, "ok": 0, "own": 0}
    for f in files:
        shown = os.path.relpath(f, REPO) if os.path.abspath(f).startswith(REPO) else f
        for k, v in audit(f, javap, classpath, show_all).items() if shown == f else \
                audit_as(f, shown, javap, classpath, show_all).items():
            total[k] += v
    sys.stderr.write(f"{sum(total.values())} entries: {total['ok']} agree with the framework, "
                     f"{total['own']} on a4l's own classes, {total['BAD']} BAD, "
                     f"{total['benign']} benign, {total['unknown']} unknown\n")
    return 1 if total["BAD"] else 0


def audit_as(path, shown, javap, classpath, show_all):
    """audit(), printing `shown` (the path from the repository root) in place of `path`."""
    cwd = os.getcwd()
    os.chdir(REPO)
    try:
        return audit(shown, javap, classpath, show_all)
    finally:
        os.chdir(cwd)


if __name__ == "__main__":
    sys.exit(main())
