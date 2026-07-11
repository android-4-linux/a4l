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
                    '../../framework/shim/android/content/SharedPreferences.java',
                    '../../framework/shim/android/view/ContextThemeWrapper.java',
                    '../../framework/shim/android/os/Bundle.java',
                    '../../framework/shim/android/app/Service.java',
                    '../../framework/shim/android/app/Activity.java',
                    '../../framework/shim/android/app/ActivityManager.java',
                    '../../framework/shim/android/view/Display.java',
                    '../../framework/shim/android/view/WindowManager.java',
                    '../../framework/shim/android/media/AudioManager.java',
                    '../../framework/shim/android/media/AudioTrack.java',
                    '../../framework/shim/android/media/MediaDescription.java',
                    '../../framework/shim/android/media/MediaMetadata.java',
                    '../../framework/shim/android/media/browse/MediaBrowser.java',
                    '../../framework/shim/android/media/session/MediaSession.java',
                    '../../framework/shim/android/media/session/MediaSessionManager.java',
                    '../../framework/shim/android/media/session/PlaybackState.java',
                    '../../framework/shim/android/service/media/MediaBrowserService.java',
                    '../../framework/shim/android/graphics/Color.java',
                    '../../framework/shim/android/graphics/Paint.java',
                    '../../framework/shim/android/graphics/Bitmap.java',
                    '../../framework/shim/android/graphics/Canvas.java',
                    '../../framework/shim/android/view/View.java',
                    '../../framework/shim/android/view/ViewGroup.java',
                    '../../framework/shim/android/view/Choreographer.java',
                    '../../framework/shim/android/view/MotionEvent.java',
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

    # Phase 2 (gfx/Canvas): gfx.apk's main() builds a custom MyView extends
    # android.view.View; v.draw(c) -> onDraw(c) fills a Bitmap-backed Canvas blue and
    # draws a red rect. The Canvas/Bitmap natives raster into aclcxx::gfx::Surface and
    # aclcxxDescribe() dumps sample pixels — verifiable headlessly (no compositor).
    gfx_apk = custom_target(
      'gfx-apk',
      input : files('../fixtures/make_gfx_apk.py'),
      output : 'gfx.apk',
      command : [py3, '@INPUT@', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    test(
      'run-apk-gfx',
      py3,
      args : [files('run_gfx_test.py'), aclcxx_exe, gfx_apk, framework_shim],
      depends : [aclcxx_exe, gfx_apk, framework_shim],
      timeout : 120,
    )

    # Phase 2 (View traversal): view.apk's launcher MainActivity.onCreate does
    # setContentView(new Panel(this)); Panel extends ViewGroup holding two Tiles
    # (custom Views). After the launch lifecycle the ViewRoot driver (jvm_vm
    # render_content_view) runs measure -> layout -> draw over the tree into a
    # display-sized Bitmap: the top half rasters blue, the bottom half red —
    # verifiable headlessly. Proves View(Context) + a real measure/layout/draw pass.
    view_apk = custom_target(
      'view-apk',
      input : files('../fixtures/make_view_apk.py'),
      output : 'view.apk',
      command : [py3, '@INPUT@', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    test(
      'run-apk-view',
      py3,
      args : [files('run_view_test.py'), aclcxx_exe, view_apk, framework_shim],
      depends : [aclcxx_exe, view_apk, framework_shim],
      timeout : 120,
    )

    # Tier C (full vertical): the same app, but `--present` shows its rendered
    # content view in a real Wayland window. run_view_present_test.py launches a
    # headless Weston and asserts aclcxx reports the frame was shown. Only when the
    # real wcs presenter is built and Weston is available; SKIPs otherwise.
    weston_prog = find_program('weston', required : false)
    if have_wcs and weston_prog.found()
      test(
        'run-apk-view-present',
        py3,
        args : [files('run_view_present_test.py'), aclcxx_exe, view_apk, framework_shim],
        depends : [aclcxx_exe, view_apk, framework_shim],
        timeout : 120,
      )

      # Tier C (continuous, JVM-driven): anim.apk's AnimView implements
      # Choreographer.FrameCallback and re-posts itself each frame; `aclcxx run
      # --present-frames=N` runs the Choreographer loop so the app redraws a moving
      # band and N distinct frames are presented to a headless Weston.
      anim_apk = custom_target(
        'anim-apk',
        input : files('../fixtures/make_anim_apk.py'),
        output : 'anim.apk',
        command : [py3, '@INPUT@', '@OUTPUT@'],
        depend_files : files('../fixtures/make_axml.py'),
      )
      test(
        'run-apk-anim-present',
        py3,
        args : [files('run_anim_present_test.py'), aclcxx_exe, anim_apk, framework_shim],
        depends : [aclcxx_exe, anim_apk, framework_shim],
        timeout : 120,
      )
    endif

    # Tier C (input): touch.apk's TouchView.onTouchEvent logs + repaints. The wm/
    # test compositor (close_compositor, built under src/wm when wayland-server +
    # wayland-scanner are present) injects a pointer tap, which the ViewRoot driver
    # turns into a MotionEvent and dispatches into the view. Uses the compositor,
    # not Weston (headless Weston has no input to inject).
    if have_wcs and wayland_server_dep.found() and find_program('wayland-scanner', required : false).found()
      touch_apk = custom_target(
        'touch-apk',
        input : files('../fixtures/make_touch_apk.py'),
        output : 'touch.apk',
        command : [py3, '@INPUT@', '@OUTPUT@'],
        depend_files : files('../fixtures/make_axml.py'),
      )
      test(
        'run-apk-touch-present',
        py3,
        args : [files('run_touch_test.py'), aclcxx_exe, touch_apk, framework_shim,
                close_compositor],
        depends : [aclcxx_exe, touch_apk, framework_shim, close_compositor],
        timeout : 120,
      )
    endif
  else
    message('javac not found; skipping run-apk-framework integration test.')
  endif
else
  message('python3/JDK not both available; skipping run-apk integration test.')
endif
