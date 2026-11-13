// Copyright 2026 Contributors to a4l
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// acl carried patch (B2): register a LinuxMusl x Arm64 host toolchain so ART can be built as a
// single-libc arm64 host runtime. Stock AOSP has only a bionic arm64 host (linux_bionic-arm64),
// and a glibc acl process cannot dlopen a bionic libart.so (bionic libc cannot cold-load under a
// foreign loader -> SIGTRAP; proven on a Pi 4, docs/pi-plan.md P-B). A musl arm64 host ART links
// only musl, coexisting with the Pi's glibc Wayland/Vulkan/PipeWire exactly as today's x86 host.
//
// Composes the device arm64 arch flags (toolchainArm64) with the arch-agnostic musl libc
// specialization (toolchainMusl), the same shape as toolchainLinuxMuslX8664 on x86 -- except the
// only arm64 toolchain base (toolchainArm64) embeds toolchainBionic, so the bionic-ness it inherits
// (Bionic(), the bionic crt objects, the bionic default shared libs) is overridden with musl below.

package config

import (
	"strings"

	"android/soong/android"
)

type toolchainLinuxMuslArm64 struct {
	toolchainArm64
	toolchainMusl
}

// Override the bionic-ness inherited via toolchainArm64 -> toolchainBionic.
func (toolchainLinuxMuslArm64) Bionic() bool { return false }

func (t *toolchainLinuxMuslArm64) ClangTriple() string {
	// No "-android" suffix (no __ANDROID__); no "-gnu" (musl, not glibc).
	return "aarch64-linux-musl"
}

func (t *toolchainLinuxMuslArm64) Cflags() string {
	return t.toolchainArm64.Cflags() + " " + t.toolchainMusl.Cflags()
}

func (t *toolchainLinuxMuslArm64) Ldflags() string {
	return t.toolchainArm64.Ldflags() + " " + t.toolchainMusl.Ldflags()
}

func (t *toolchainLinuxMuslArm64) Lldflags() string {
	return t.toolchainArm64.Lldflags() + " " + t.toolchainMusl.Lldflags()
}

// toolchainArm64 (via toolchainBionic) and toolchainMusl both define these; resolve the ambiguity
// in favor of musl so binaries link the musl crt objects and libc_musl.
func (t *toolchainLinuxMuslArm64) DefaultSharedLibraries() []string {
	return t.toolchainMusl.DefaultSharedLibraries()
}
func (t *toolchainLinuxMuslArm64) CrtBeginStaticBinary() []string {
	return t.toolchainMusl.CrtBeginStaticBinary()
}
func (t *toolchainLinuxMuslArm64) CrtBeginSharedBinary() []string {
	return t.toolchainMusl.CrtBeginSharedBinary()
}
func (t *toolchainLinuxMuslArm64) CrtBeginSharedLibrary() []string {
	return t.toolchainMusl.CrtBeginSharedLibrary()
}
func (t *toolchainLinuxMuslArm64) CrtEndStaticBinary() []string {
	return t.toolchainMusl.CrtEndStaticBinary()
}
func (t *toolchainLinuxMuslArm64) CrtEndSharedBinary() []string {
	return t.toolchainMusl.CrtEndSharedBinary()
}
func (t *toolchainLinuxMuslArm64) CrtEndSharedLibrary() []string {
	return t.toolchainMusl.CrtEndSharedLibrary()
}

func linuxMuslArm64ToolchainFactory(arch android.Arch) Toolchain {
	archVariant := "armv8-a" // for host, default to armv8-a (matches linuxArm64ToolchainFactory)
	toolchainCflags := []string{arm64ArchVariantCflagsVar[archVariant]}

	// Conservatively assume the host CPU needs the erratum fix (same as the bionic arm64 host).
	extraLdflags := "-Wl,--fix-cortex-a53-843419"

	ret := toolchainLinuxMuslArm64{}
	ret.toolchainArm64.ldflags = strings.Join([]string{
		"${config.Arm64Ldflags}",
		extraLdflags,
	}, " ")
	ret.toolchainArm64.lldflags = strings.Join([]string{
		"${config.Arm64Lldflags}",
		extraLdflags,
	}, " ")
	ret.toolchainArm64.toolchainCflags = strings.Join(toolchainCflags, " ")
	return &ret
}

func init() {
	registerToolchainFactory(android.LinuxMusl, android.Arm64, linuxMuslArm64ToolchainFactory)
}
