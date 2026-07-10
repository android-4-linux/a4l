# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §5 (Tier B headless integration). No GPL-3.0 sources.
#
# Tier B run-path integration: build a self-contained hello-world APK and exec the
# real `aclcxx` binary on it, asserting the whole CLI -> pkg -> manifest -> dex ->
# DEX/JVM -> JVM -> app-output vertical runs. Needs python3 (fixture) and a JDK
# (have_jni, so the jvm backend can execute); otherwise the test is skipped.
py3 = find_program('python3', required : false)
if py3.found() and have_jni
  hello_apk = custom_target(
    'hello-apk',
    input : files('../fixtures/make_hello_apk.py'),
    output : 'hello.apk',
    command : [py3, '@INPUT@', '@OUTPUT@'],
    depend_files : files('../fixtures/make_axml.py'),
  )
  test(
    'run-apk',
    py3,
    args : [files('run_apk_test.py'), aclcxx_exe, hello_apk],
    depends : [aclcxx_exe, hello_apk],
    timeout : 120,
  )
else
  message('python3/JDK not both available; skipping run-apk integration test.')
endif
