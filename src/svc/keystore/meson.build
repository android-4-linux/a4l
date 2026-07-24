# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2 (svc/ in-process hub), ADR-3; Android AndroidKeyStore mapped
# onto the freedesktop Secret Service (libsecret). No GPL-3.0 sources.
#
# svc/keystore — the STORAGE half of acl's AndroidKeyStore. acl's AndroidKeyStore JCE
# provider (Java) generates real keys with conscrypt and persists the key MATERIAL here:
# in the login keyring via the Secret Service (libsecret -> gnome-keyring / KWallet),
# encrypted at rest and gated on the keyring unlock. The libsecret half is gated inside
# the module (ACL_HAVE_SECRET); without it the in-memory Null backend keeps working
# (non-persistent). NOT hardware-equivalent and no attestation — a desktop tradeoff.
keystore_deps = [fmt_dep, jni_dep]
if have_secret
  keystore_deps += secret_dep
endif

keystore_lib = static_library(
  'acl_svc_keystore',
  files('keystore_backend.cpp', 'keystore_jni.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : keystore_deps,
)
module_libs += keystore_lib

# Dev tool: round-trip a blob through the selected backend (store/contains/load/list/
# erase). Verifies the Secret Service (or memory fallback) works before the JCE provider
# is wired on top.
executable(
  'acl-keystore-probe',
  files('keystore_probe_main.cpp'),
  include_directories : [config_inc, core_inc],
  link_with : [keystore_lib, acl_log],
  dependencies : keystore_deps,
  install : false,
)
