#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl build tooling. Patches AOSP Soong/make (Apache-2.0) to register a
# linux_musl_arm64 host toolchain and map the aarch64-linux-musl LLVM runtimes into module variants.
# No GPL-3.0 sources. Idempotent; pass the AOSP root as $1 (default $ACL_AOSP_ROOT or /mnt/dev/aosp).
#
# See ./README.md. Revert with:  ./apply.sh --revert [AOSP_ROOT]
set -eu
REVERT=""; [ "${1:-}" = "--revert" ] && { REVERT=1; shift; }
AOSP="${1:-${ACL_AOSP_ROOT:-/mnt/dev/aosp}}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ -n "$REVERT" ]; then
  git -C "$AOSP/build/soong" checkout -- android/arch.go cc/config/Android.bp bazel/configurability.go cc/sanitize.go cc/fuzz.go 2>/dev/null || true
  rm -f "$AOSP/build/soong/cc/config/arm64_musl_host.go"
  git -C "$AOSP/build/make" checkout -- core/envsetup.mk 2>/dev/null || true
  git -C "$AOSP/prebuilts/clang/host/linux-x86" checkout -- soong/clangprebuilts.go 2>/dev/null || true
  git -C "$AOSP/external/llvm" checkout -- Android.bp 2>/dev/null || true
  echo "reverted B2 Soong changes under $AOSP (host-graph disables: sanitize.go, fuzz.go, external/llvm)"
  exit 0
fi

S="$AOSP/build/soong"; CP="$AOSP/prebuilts/clang/host/linux-x86/soong/clangprebuilts.go"

# 2. the toolchain factory
cp "$HERE/arm64_musl_host.go" "$S/cc/config/arm64_musl_host.go"

# 1. arch.go — Arm64 into LinuxMusl
grep -q 'newOsType("linux_musl", Host, false, Arm64,' "$S/android/arch.go" || \
  perl -0pi -e 's/newOsType\("linux_musl", Host, false, X86, X86_64\)/newOsType("linux_musl", Host, false, Arm64, X86, X86_64)/' "$S/android/arch.go"

# 3. Android.bp — the factory into the srcs list
grep -q 'arm64_musl_host.go' "$S/cc/config/Android.bp" || \
  perl -0pi -e 's/(        "arm64_linux_host\.go",\n)(    \],)/$1        "arm64_musl_host.go",\n$2/' "$S/cc/config/Android.bp"

# 4. configurability.go — bp2build platform maps
B="$S/bazel/configurability.go"
grep -q 'osArchLinuxMuslArm64' "$B" || {
  perl -0pi -e 's/(\tosArchLinuxMuslX86      = "linux_musl_x86"\n)/\tosArchLinuxMuslArm64    = "linux_musl_arm64"\n$1/' "$B"
  perl -0pi -e 's{(\t\tosArchLinuxMuslX86:         "//build/bazel/platforms/os_arch:linux_musl_x86",\n)}{\t\tosArchLinuxMuslArm64:       "//build/bazel/platforms/os_arch:linux_musl_arm64",\n$1}' "$B"
  perl -0pi -e 's/\t\tosLinuxMusl:   \{archX86, archX86_64\},/\t\tosLinuxMusl:   {archArm64, archX86, archX86_64},/' "$B"
}

# 5. envsetup.mk — accept HOST_CROSS_OS=linux_musl
M="$AOSP/build/make/core/envsetup.mk"
grep -q 'HOST_CROSS_OS),linux_musl)' "$M" || \
  perl -0pi -e 's/(  else ifeq \(\$\(HOST_CROSS_OS\),linux_bionic\)\n    ifeq \(,\$\(HOST_CROSS_ARCH\)\)\n      \$\(error HOST_CROSS_ARCH missing\.\)\n    endif\n)/$1  else ifeq (\$(HOST_CROSS_OS),linux_musl)\n    ifeq (,\$(HOST_CROSS_ARCH))\n      \$(error HOST_CROSS_ARCH missing.)\n    endif\n/' "$M"

# 7. sanitize.go — (a) don't inject the bionic memtag note into host arm64 binaries;
#                  (b) the musl arm64 bring-up host ships no sanitizer runtimes -> disable them.
SAN="$S/cc/sanitize.go"
grep -q 'acl B2' "$SAN" || {
  perl -0pi -e 's/(\t// Enable Memtag for all components in the include paths \(for Aarch64 only\)\n)(\tif ctx\.Arch\(\)\.ArchType == android\.Arm64 \{)/$1\t\/\/ acl B2: MTE is an Android device feature; never for host builds (note_memtag_* is bionic-only).\n\tif ctx.Arch().ArchType == android.Arm64 \&\& !ctx.Host() \{/' "$SAN"
  perl -0pi -e 's/(\t// Disable CFI for musl\n\tif ctx\.toolchain\(\)\.Musl\(\) \{\n\t\ts\.Cfi = nil\n\t\ts\.Diag\.Cfi = nil\n\t\}\n)/$1\n\t\/\/ acl B2: musl arm64 bring-up host ships only core runtimes; disable sanitizers pulling others.\n\tif ctx.toolchain().Musl() \&\& ctx.Arch().ArchType == android.Arm64 \{\n\t\ts.Address = nil; s.Hwaddress = nil; s.Thread = nil; s.Fuzzer = nil; s.Safestack = nil\n\t\ts.Scudo = nil; s.Undefined = nil; s.All_undefined = nil; s.Misc_undefined = nil; s.Integer_overflow = nil\n\t\}\n/' "$SAN"
}

# 6. clangprebuilts.go — map the aarch64-linux-musl runtime prebuilts into module variants
grep -q 'Linux_musl_arm64' "$CP" || {
  # archInnerProps: Enabled field
  perl -0pi -e 's/(type archInnerProps struct \{\n\tSrcs \[\]string\n\tStem \*string\n)(\})/$1\tEnabled *bool \/\/ acl B2\n$2/' "$CP"
  # archProps: Linux_musl_arm64 field
  perl -0pi -e 's/(\tLinux_musl_x86_64   archInnerProps\n)/$1\tLinux_musl_arm64    archInnerProps \/\/ acl B2\n/' "$CP"
  # crt object: Arm64 arch case
  perl -0pi -e 's/(\t\t\tX86_64 struct \{\n\t\t\t\tSrcs \[\]string\n\t\t\t\}\n)(\t\t\}\n\t\tSystem_shared_libs)/$1\t\t\tArm64 struct { \/\/ acl B2\n\t\t\t\tSrcs []string\n\t\t\t}\n$2/' "$CP"
  perl -0pi -e 's{(\tp\.Arch\.X86_64\.Srcs = \[\]string\{path\.Join\(libDir, name\+"-x86_64\.o"\)\}\n)}{$1\tp.Arch.Arm64.Srcs = []string{path.Join(libDir, name+"-aarch64.o")} // acl B2\n}' "$CP"
  # libclang_rt static (builtins/profile): existence-gated musl_arm64
  perl -0pi -e 's{(\tp\.Target\.Linux_musl_x86_64\.Srcs = \[\]string\{path\.Join\(libDir, "x86_64-linux-musl/lib/linux", name\+"-x86_64"\+suffix\+"\.a"\)\}\n)}{$1\tmuslA := path.Join(libDir, "aarch64-linux-musl/lib/linux", name+"-aarch64"+suffix+".a") // acl B2\n\tif android.ExistentPathForSource(ctx, ctx.ModuleDir(), muslA).Valid() \{\n\t\tp.Target.Linux_musl_arm64.Srcs = []string\{muslA\}\n\t\} else \{\n\t\tp.Target.Linux_musl_arm64.Enabled = proptools.BoolPtr(false)\n\t\}\n}' "$CP"
  # llvm static (libunwind/libc++/libc++abi): existence-gated musl_arm64
  perl -0pi -e 's{(\tp\.Target\.Linux_musl_x86_64\.Srcs = \[\]string\{path\.Join\(libDir, "x86_64-linux-musl/lib", name\)\}\n)}{$1\tmuslA := path.Join(libDir, "aarch64-linux-musl/lib", name) // acl B2\n\tif android.ExistentPathForSource(ctx, ctx.ModuleDir(), muslA).Valid() \{\n\t\tp.Target.Linux_musl_arm64.Srcs = []string\{muslA\}\n\t\} else \{\n\t\tp.Target.Linux_musl_arm64.Enabled = proptools.BoolPtr(false)\n\t\}\n}' "$CP"
}

echo "applied B2 Soong changes under $AOSP"
for f in "$S/android/arch.go:Arm64, X86" "$S/cc/config/Android.bp:arm64_musl_host" \
         "$S/bazel/configurability.go:osArchLinuxMuslArm64" "$AOSP/build/make/core/envsetup.mk:linux_musl)" \
         "$CP:Linux_musl_arm64"; do
  p="${f%%:*}"; pat="${f##*:}"; grep -q "$pat" "$p" && echo "  ok: ${p#$AOSP/}" || echo "  MISSING: ${p#$AOSP/} ($pat)"
done
[ -f "$S/cc/config/arm64_musl_host.go" ] && echo "  ok: build/soong/cc/config/arm64_musl_host.go"
