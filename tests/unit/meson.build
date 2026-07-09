# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §5 (Tier A unit). No GPL-3.0 sources.
#
# Tier A unit tests (Catch2). Catch2 is pulled via subprojects/catch2.wrap and
# falls back gracefully if unavailable so the Phase 0 skeleton still configures.
catch2_dep = dependency(
  'catch2-with-main',
  fallback : ['catch2', 'catch2_with_main_dep'],
  required : false,
)

if catch2_dep.found()
  smoke = executable(
    'aclcxx_smoke_test',
    files('smoke_test.cpp'),
    include_directories : config_inc,
    link_with : module_libs,
    dependencies : catch2_dep,
  )
  test('smoke', smoke)
else
  warning('Catch2 not available; skipping Tier A unit tests. ' +
          'Provide subprojects/catch2 (meson wrap install catch2) to enable.')
endif
