// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture. No GPL-3.0 sources.
//
// For `acl-ndk-probe --glforward`: calls one entry point of a4l's libEGL.so
// and one of its libGLESv2.so, with more arguments than there are registers
// for. The probe stands in for Mesa and checks what arrives. The names are
// only which forwarder is entered; the prototype is the test's own.
static __thread volatile int t_marker;

typedef long (*many_fn)(long,
                        long,
                        long,
                        long,
                        long,
                        long,
                        long,
                        long,
                        long,
                        long,
                        long,
                        long,
                        double,
                        double,
                        double,
                        double,
                        double,
                        double,
                        double,
                        double,
                        double);
extern void eglChooseConfig(void);
extern void glTexImage2D(void);

static long call(void (*entry)(void)) {
  return ((many_fn)entry)(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0.5, 1.5, 2.5,
                          3.5, 4.5, 5.5, 6.5, 7.5, 8.5);
}

// The two results added, or -1 when this thread's own thread-local did not
// survive the calls.
__attribute__((visibility("default"))) long acl_gl_forward_test(void) {
  t_marker = 91;
  const long egl = call(eglChooseConfig);
  if (t_marker != 91)
    return -1;
  const long gles = call(glTexImage2D);
  if (t_marker != 91)
    return -1;
  return egl + gles;
}
