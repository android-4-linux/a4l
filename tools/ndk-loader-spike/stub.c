// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl spike. No GPL-3.0 sources.
//
// Minimal bionic PIE executable. The in-process bootstrap hands this to the linker as the "main
// executable" so linker_main() completes (it has no init-only mode). Its entry NEVER runs — the
// bootstrap calls __linker_init directly and keeps control — so this only needs to be a valid,
// linkable bionic exe whose NEEDED pulls in bionic libc (initialized as part of the link).
int main(void) { return 0; }
