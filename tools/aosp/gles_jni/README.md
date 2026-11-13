<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: a4l GLSurfaceView Stage 2b build tooling; bundles AOSP glgen
     android_opengl_GLES*.cpp (frameworks/base, Apache-2.0). No GPL-3.0 sources. -->

# libacl_gles_jni — a4l's android.opengl.GLES20/GLES30 natives (GLSurfaceView Stage 2b)

The native underside of `android.opengl.GLES20` / `GLES30`. libGDX (and every `GLSurfaceView`
app) issues its GL calls through these Java classes (`AndroidGL20`/`AndroidGL30` →
`android.opengl.GLES20`), which JNI down to host **Mesa `libGLESv2`** — the same GL context the
`com.google.android.gles_jni.EGLImpl` JNI makes current (Stage 2a) on a4l's `gfx::GlDevice`.

## What is a4l-authored vs vendored AOSP

AOSP's `android_opengl_GLES20.cpp` / `GLES30.cpp` are **glgen-generated** (thousands of lines of
pure `glXxx(...)` passthrough). They are Apache-2.0 and used **verbatim** — not worth re-authoring,
and not checked into a4l's repo (size). This directory holds only the a4l-authored glue:

- `gles_shim.cpp` — implements `android::AndroidRuntime::registerNativeMethods` (a thin
  `JNIEnv::RegisterNatives`, the one symbol the glgen files use from that header) and exposes
  `register_acl_gles` — the single entry a4l (`jvm_vm.cpp` `load_acl_gles`) dlsym's.
- `shim/android_runtime/AndroidRuntime.h`, `shim/utils/misc.h` — minimal stand-ins so the glgen
  files compile without dragging in `libandroid_runtime` / `libutils` (only `registerNativeMethods`
  and `NELEM` are needed).
- `Android.bp` — the Soong module. `gl*` symbols are left **undefined** (`allow_undefined_symbols`;
  the AOSP host sysroot has no Mesa) and bind at dlopen to the system `libGLESv2` a4l loads
  `RTLD_GLOBAL` (`load_acl_gles` dlopens `libEGL.so.1`/`libGLESv2.so.2` first).

## Build (Soong), mirroring libacl_graphics_jni

```sh
# 1. sync this dir into the AOSP vendor tree
cp -r tools/aosp/gles_jni /mnt/dev/aosp/vendor/acl/
# 2. copy the verbatim AOSP glgen passthrough (NOT vendored into acl's repo)
cp /mnt/dev/aosp/frameworks/base/core/jni/android_opengl_GLES20.cpp /mnt/dev/aosp/vendor/acl/gles_jni/
cp /mnt/dev/aosp/frameworks/base/core/jni/android_opengl_GLES30.cpp /mnt/dev/aosp/vendor/acl/gles_jni/
# 3. build (2-part lunch on this branch)
cd /mnt/dev/aosp && source build/envsetup.sh && lunch aosp_x86_64-eng && m libacl_gles_jni
# -> out/host/linux-x86/lib64/libacl_gles_jni.so
```

`tools/run-apk.sh` passes it as `ACL_ART_GLES_LIB` when present; `jvm_vm.cpp load_acl_gles`
dlopens it and calls `register_acl_gles` at VM boot. See also `tools/aosp/graphics_jni/`.
