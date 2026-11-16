# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l build glue (docs/adr/ADR-0009 C5). No GPL-3.0 sources.
#
# libFuzzer harnesses. Off by default: they need clang and are not part of the test suite.
# CI builds and runs broker_fuzz for 120 s in the `present` job (.github/workflows/ci.yml).
#
#   CC=clang CXX=clang++ meson setup build-fuzz -Dfuzzers=true \
#       -Db_sanitize=address,undefined -Db_lundef=false \
#       -Dcpp_args=-fsanitize=fuzzer-no-link
#   meson compile -C build-fuzz broker_fuzz
#   tests/fuzz/make_broker_seeds.py corpus && build-fuzz/tests/fuzz/broker_fuzz -max_len=4096 corpus
#
# -fsanitize=fuzzer-no-link gives a4l's own libraries the coverage instrumentation the fuzzer
# steers by; without it the harness runs and finds little.
if meson.get_compiler('cpp').get_id() != 'clang'
  error('-Dfuzzers=true needs clang (libFuzzer): set CC=clang CXX=clang++')
endif
#
# The harness compiles the broker's own sources (wm/split_present.cpp, wm/split_validate.cpp)
# and links only ipc/ and log/, not the whole native core: the broker needs nothing else, and
# the rest of the tree, vendored subprojects included, then does not have to build under clang.
if have_vulkan and have_wcs
  executable(
    'broker_fuzz',
    files('broker_fuzz.cpp', '../../src/wm/split_present.cpp',
          '../../src/wm/split_validate.cpp'),
    include_directories : [config_inc, core_inc],
    link_with : [ipc_lib, acl_log],
    dependencies : [fmt_dep],
    cpp_args : ['-fsanitize=fuzzer'],
    link_args : ['-fsanitize=fuzzer'],
  )
else
  warning('broker_fuzz not built: the broker needs the GPU present path (Vulkan and wcs)')
endif
