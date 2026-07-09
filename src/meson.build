# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, §8. No GPL-3.0 sources.
#
# Native core modules. Each subdir appends its static_library to module_libs
# (shared scope via subdir). src/shell builds the `aclcxx` executable last,
# linking everything. Order below is dependency-agnostic for the Phase 0 stubs.

subdir('log')       # logging facade (fmt backend; spdlog/DLT sinks later)
subdir('runtime')   # ART embedding, class-loader, DEX/OAT cache, JNI tables
subdir('pkg')       # APK/zip parse, resources.arsc, manifest, split/ABI select
subdir('gfx')       # Skia canvases (shm/egl/vulkan-dmabuf), text stack
subdir('wm')        # window/Activity surface mgmt over wcs
subdir('input')     # wl_seat -> MotionEvent/KeyEvent, IME via text-input-v3
subdir('audio')     # AAudio/OpenSL -> PipeWire
subdir('media')     # MediaCodec -> V4L2 M2M, FFmpeg fallback
subdir('svc')       # in-process service hub (+ svc/car under automotive)
subdir('ndk')       # libandroid, ANativeWindow, ALooper (Phase 4)
subdir('shell')     # CLI launcher + aclcxx executable
