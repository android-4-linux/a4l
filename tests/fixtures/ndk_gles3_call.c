// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture. No GPL-3.0 sources.
//
// For `acl-ndk-probe --glhost`. A library that links a4l's libGLESv3.so and not
// libGLESv2.so, as an engine built for OpenGL ES 3 does, and calls GL through
// it. libGLESv3.so is the same forwarder as libGLESv2.so under another name,
// with a table pointer of its own that a4l has to fill in.
extern unsigned int glGetError(void);

// No context is current, so what glGetError returns is not the test: that the
// call reaches Mesa and comes back is. 42 is the probe's pass.
__attribute__((visibility("default"))) int acl_gl_host_call(void) {
  (void)glGetError();
  return 42;
}
