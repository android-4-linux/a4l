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

  # Gate G2.a: the framework boot-classpath seam. probe.apk's main() calls a class
  # (com.aclcxx.fw.Probe) that is NOT bundled; it must come from --framework. Uses
  # the javac from the SAME java_home as the detected libjvm (class-file version).
  javac = find_program(java_home / 'bin' / 'javac', required : false)
  if javac.found()
    probe_apk = custom_target(
      'probe-apk',
      input : files('../fixtures/make_hello_apk.py'),
      output : 'probe.apk',
      command : [py3, '@INPUT@', '--probe', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    framework_classes = custom_target(
      'framework-probe-classes',
      input : files('../fixtures/framework/com/aclcxx/fw/Probe.java'),
      output : 'framework',
      command : [javac, '-d', '@OUTPUT@', '@INPUT@'],
    )
    test(
      'run-apk-framework',
      py3,
      args : [files('run_framework_test.py'), aclcxx_exe, probe_apk,
              framework_classes],
      depends : [aclcxx_exe, probe_apk, framework_classes],
      timeout : 120,
    )

    # Gate G2.b: the JNI native underside. log.apk's main() calls
    # android.util.Log.i(...); the framework Log shim's native write0() is bound by
    # aclcxx (RegisterNatives) and forwards to aclcxx::log.
    log_apk = custom_target(
      'log-apk',
      input : files('../fixtures/make_hello_apk.py'),
      output : 'log.apk',
      command : [py3, '@INPUT@', '--log', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    framework_shim = custom_target(
      'framework-shim-classes',
      input : files('../../framework/shim/android/util/Log.java'),
      output : 'framework-shim',
      command : [javac, '-d', '@OUTPUT@', '@INPUT@'],
    )
    test(
      'run-apk-log',
      py3,
      args : [files('run_log_test.py'), aclcxx_exe, log_apk, framework_shim],
      depends : [aclcxx_exe, log_apk, framework_shim],
      timeout : 120,
    )
  else
    message('javac not found; skipping run-apk-framework integration test.')
  endif
else
  message('python3/JDK not both available; skipping run-apk integration test.')
endif
