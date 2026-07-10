# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §5 (Tier B headless integration). No GPL-3.0 sources.
#
# Tier B run-path integration: build a self-contained hello-world APK and exec the
# real `aclcxx` binary on it, asserting the whole CLI -> pkg -> manifest -> dex ->
# DEX/JVM -> JVM -> app-output vertical runs. Needs python3 (fixture) and a JDK
# (have_jni, so the jvm backend can execute); otherwise the test is skipped.
py3 = find_program('python3', required : false)

# Gate G2.c: the frameworks/base subsetting tool. Pure static analysis over a
# synthetic AOSP-shaped source tree — needs only python3 (no JVM), so it runs even
# where the JDK is absent.
if py3.found()
  test(
    'fw-extract',
    py3,
    args : [files('fw_extract_test.py'), files('../../tools/fw_extract.py'),
            meson.current_source_dir() / '../fixtures/fwsrc'],
    timeout : 60,
  )
endif

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
    # The whole android.* shim tree, compiled together (inter-shim deps: Activity
    # imports Bundle; MediaBrowserService extends Service, imports Bundle).
    framework_shim = custom_target(
      'framework-shim-classes',
      input : files('../../framework/shim/android/content/Context.java',
                    '../../framework/shim/android/content/ContextWrapper.java',
                    '../../framework/shim/android/view/ContextThemeWrapper.java',
                    '../../framework/shim/android/os/Bundle.java',
                    '../../framework/shim/android/app/Service.java',
                    '../../framework/shim/android/app/Activity.java',
                    '../../framework/shim/android/media/AudioManager.java',
                    '../../framework/shim/android/media/AudioTrack.java',
                    '../../framework/shim/android/media/MediaDescription.java',
                    '../../framework/shim/android/media/MediaMetadata.java',
                    '../../framework/shim/android/media/browse/MediaBrowser.java',
                    '../../framework/shim/android/media/session/MediaSession.java',
                    '../../framework/shim/android/media/session/PlaybackState.java',
                    '../../framework/shim/android/service/media/MediaBrowserService.java',
                    '../../framework/shim/android/util/Log.java'),
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

    # Gate G2.d: the Android Activity entry point. activity.apk's launcher is
    # MainActivity extends android.app.Activity; `aclcxx run` instantiates it and
    # drives onCreate/onStart/onResume against the shimmed framework — no main().
    activity_apk = custom_target(
      'activity-apk',
      input : files('../fixtures/make_activity_apk.py'),
      output : 'activity.apk',
      command : [py3, '@INPUT@', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    test(
      'run-apk-activity',
      py3,
      args : [files('run_activity_test.py'), aclcxx_exe, activity_apk, framework_shim],
      depends : [aclcxx_exe, activity_apk, framework_shim],
      timeout : 120,
    )

    # Gate G2.e (automotive media path): host a MediaBrowserService (no Views).
    # media.apk's MusicService extends android.service.media.MediaBrowserService;
    # `aclcxx run --service=...` drives onCreate + onGetRoot in-process (no Binder).
    media_apk = custom_target(
      'media-apk',
      input : files('../fixtures/make_media_apk.py'),
      output : 'media.apk',
      command : [py3, '@INPUT@', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    test(
      'run-apk-media',
      py3,
      args : [files('run_media_test.py'), aclcxx_exe, media_apk, framework_shim],
      depends : [aclcxx_exe, media_apk, framework_shim],
      timeout : 120,
    )

    # Gate G2.e (media audio): an app writes PCM via android.media.AudioTrack; the
    # aclcxx native underside forwards it to aclcxx::audio (counting sink here;
    # ACLCXX_AUDIO=pipewire actually plays it on the daemon).
    audio_apk = custom_target(
      'audio-apk',
      input : files('../fixtures/make_audio_apk.py'),
      output : 'audio.apk',
      command : [py3, '@INPUT@', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    test(
      'run-apk-audio',
      py3,
      args : [files('run_audio_test.py'), aclcxx_exe, audio_apk, framework_shim],
      depends : [aclcxx_exe, audio_apk, framework_shim],
      timeout : 120,
    )
  else
    message('javac not found; skipping run-apk-framework integration test.')
  endif
else
  message('python3/JDK not both available; skipping run-apk integration test.')
endif
