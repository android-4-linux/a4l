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

// Provenance: original a4l build tooling, modeled on Soong's own x86_linux_host.go (Apache-2.0).
// No GPL-3.0 sources.
//
// a4l carried patch: a Linux (glibc) x Arm64 host toolchain, so host ART can be built for an
// arm64 glibc system. Same shape as toolchainLinuxGlibcX8664, with an aarch64 Debian-layout
// sysroot (glibc, crt objects and libgcc) in place of the x86 gcc prebuilt.

package config

import (
	"android/soong/android"
)

var (
	linuxGlibcArm64Cflags = []string{
		"-Wa,--noexecstack",
		"-fPIC",
		"-U_FORTIFY_SOURCE",
		"-D_FORTIFY_SOURCE=2",
		"-fstack-protector-strong",
		"-D__STDC_FORMAT_MACROS",
		"-D__STDC_CONSTANT_MACROS",
		"-march=armv8-a",
		"--sysroot ${LinuxGlibcArm64Sysroot}",
		"--gcc-toolchain=${LinuxGlibcArm64Sysroot}/usr",
		"-idirafter ${LinuxGlibcArm64Overlay}/include",
	}

	linuxGlibcArm64Ldflags = []string{
		"-Wl,-z,noexecstack",
		"-Wl,-z,relro",
		"-Wl,-z,now",
		"-Wl,--no-undefined-version",
		"-Wl,--hash-style=gnu",
		"-Wl,--fix-cortex-a53-843419",
		"--sysroot ${LinuxGlibcArm64Sysroot}",
		"--gcc-toolchain=${LinuxGlibcArm64Sysroot}/usr",
	}

	linuxGlibcArm64Lldflags = append(android.CopyOf(linuxGlibcArm64Ldflags),
		"-Wl,-z,max-page-size=4096")
)

func init() {
	exportedVars.ExportSourcePathVariable("LinuxGlibcArm64Sysroot",
		"prebuilts/gcc/linux-x86/host/aarch64-linux-glibc2.31")
	exportedVars.ExportSourcePathVariable("LinuxGlibcArm64Overlay",
		"prebuilts/gcc/linux-x86/host/aarch64-linux-glibc2.31-overlay")
	exportedVars.ExportStringListStaticVariable("LinuxGlibcArm64Cflags", linuxGlibcArm64Cflags)
	exportedVars.ExportStringListStaticVariable("LinuxGlibcArm64Ldflags", linuxGlibcArm64Ldflags)
	exportedVars.ExportStringListStaticVariable("LinuxGlibcArm64Lldflags", linuxGlibcArm64Lldflags)
}

type toolchainLinuxGlibcArm64 struct {
	toolchain64Bit
	toolchainGlibc
}

func (t *toolchainLinuxGlibcArm64) Name() string         { return "arm64" }
func (t *toolchainLinuxGlibcArm64) GccRoot() string      { return "${config.LinuxGlibcArm64Sysroot}/usr" }
func (t *toolchainLinuxGlibcArm64) GccTriple() string    { return "aarch64-linux-gnu" }
func (t *toolchainLinuxGlibcArm64) GccVersion() string   { return "10" }
func (t *toolchainLinuxGlibcArm64) IncludeFlags() string { return "" }
func (t *toolchainLinuxGlibcArm64) ClangTriple() string  { return "aarch64-linux-gnu" }
func (t *toolchainLinuxGlibcArm64) Cppflags() string     { return "" }

func (t *toolchainLinuxGlibcArm64) Cflags() string {
	return "${config.LinuxGlibcArm64Cflags}"
}

func (t *toolchainLinuxGlibcArm64) Ldflags() string {
	return "${config.LinuxGlibcArm64Ldflags}"
}

func (t *toolchainLinuxGlibcArm64) Lldflags() string {
	return "${config.LinuxGlibcArm64Lldflags}"
}

func (toolchainLinuxGlibcArm64) LibclangRuntimeLibraryArch() string {
	return "aarch64"
}

func (t *toolchainLinuxGlibcArm64) AvailableLibraries() []string {
	return linuxAvailableLibraries
}

var toolchainLinuxGlibcArm64Singleton Toolchain = &toolchainLinuxGlibcArm64{}

func linuxGlibcArm64ToolchainFactory(arch android.Arch) Toolchain {
	return toolchainLinuxGlibcArm64Singleton
}

func init() {
	registerToolchainFactory(android.Linux, android.Arm64, linuxGlibcArm64ToolchainFactory)
}
