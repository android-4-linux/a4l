# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §5 (Tier A unit). No GPL-3.0 sources.
#
# Tier A unit tests (Catch2), one growing test binary. Catch2 is pulled via
# subprojects/catch2.wrap and falls back gracefully if unavailable so the
# skeleton still configures.
catch2_dep = dependency(
  'catch2-with-main',
  fallback : ['catch2', 'catch2_with_main_dep'],
  required : false,
)

unit_test_sources = files(
  'smoke_test.cpp',
  'log_test.cpp',
  'runtime_test.cpp',
  'pkg_test.cpp',
)

# Build-time APK fixture for the pkg/ reader tests (no binary committed to git).
# If python3 is unavailable the pkg tests SKIP at runtime (env var unset).
python3 = find_program('python3', required : false)
test_env = environment()
fixture_deps = []
if python3.found()
  fixture_apk = custom_target(
    'fixture-apk',
    input : files('../fixtures/make_fixture_apk.py'),
    output : 'fixture.apk',
    command : [python3, '@INPUT@', '@OUTPUT@'],
  )
  test_env.set('ACLCXX_FIXTURE_APK', fixture_apk.full_path())
  fixture_deps = [fixture_apk]
else
  warning('python3 not found; pkg/ fixture tests will SKIP.')
endif

if catch2_dep.found()
  unit_tests = executable(
    'aclcxx_unit_tests',
    unit_test_sources,
    include_directories : [config_inc, core_inc],
    link_with : module_libs,
    dependencies : [catch2_dep, fmt_dep],
  )
  # `depends` ensures the fixture is built before the test runs.
  test('unit', unit_tests, env : test_env, depends : fixture_deps)
else
  warning('Catch2 not available; skipping Tier A unit tests. ' +
          'Provide subprojects/catch2 (meson wrap install catch2) to enable.')
endif
