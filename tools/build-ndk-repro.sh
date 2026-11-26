#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Provenance: original acl tooling. Uses AOSP prebuilt clang + built bionic intermediates from a
# local AOSP checkout (paths overridable via env). Grounds docs/ndk-scope.md. No GPL-3.0 sources.
#
# Builds a real x86_64-linux-android (bionic-ABI) NDK-style .so and proves the host two-libc gap:
# a glibc process cannot dlopen it (glibc ld.so can't satisfy the bionic NEEDED libc.so/liblog.so).
# This is the negative control for docs/ndk-scope.md Stage 0b: it's why the bionic linker must be
# bootstrapped in-process (ADR-5 / R-6), not called via glibc dlopen.
#
# Usage: tools/build-ndk-repro.sh [out_dir]
#   AOSP=/path/to/aosp   AOSP checkout root (default /mnt/dev/aosp)
#   out_dir              where to place libndktest.so + dltest (default: mktemp)
set -euo pipefail

AOSP="${AOSP:-/mnt/dev/aosp}"
OUT="${1:-$(mktemp -d /tmp/ndk-repro.XXXXXX)}"
mkdir -p "$OUT"

CLANG="$AOSP/prebuilts/clang/host/linux-x86/clang-r450784d/bin/clang"
INT="$AOSP/out/soong/.intermediates"
BIONIC_LIBC="$INT/bionic/libc/libc/android_x86_64_shared/libc.so"
BIONIC_LIBLOG="$INT/system/logging/liblog/liblog/android_x86_64_shared/liblog.so"
BIONIC_LIBDL="$INT/bionic/libdl/libdl/android_x86_64_shared/libdl.so"
CRTBEGIN="$INT/bionic/libc/crtbegin_so/android_x86_64/crtbegin_so.o"
CRTEND="$INT/bionic/libc/crtend_so/android_x86_64/obj/bionic/libc/arch-common/bionic/crtend_so.o"
# Headers: the prebuilt NDK sysroot is often absent in an AOSP checkout; use the in-tree sources.
INC_JNI="$AOSP/libnativehelper/include_jni"           # jni.h
INC_LOG="$AOSP/system/logging/liblog/include"          # android/log.h

for f in "$CLANG" "$BIONIC_LIBC" "$BIONIC_LIBLOG" "$BIONIC_LIBDL" "$CRTBEGIN" "$CRTEND"; do
  [ -e "$f" ] || { echo "MISSING: $f  (build bionic in \$AOSP, or set AOSP=)" >&2; exit 1; }
done

# acl's libandroid.so, if a bionic runtime has been staged ($ACL_BIONIC_SYSROOT, or the sibling
# stage a caller just built). Linking it makes the repro exercise the NDK API exactly as an app
# does — DT_NEEDED libandroid.so — instead of only bionic libc.
ACL_LIBANDROID=""
for cand in "${ACL_BIONIC_SYSROOT:-}/libandroid.so" "$OUT/../bionic/libandroid.so" \
            "$(dirname "$0")/../build/bionic-runtime/bionic/libandroid.so"; do
  [ -f "$cand" ] && { ACL_LIBANDROID="$cand"; break; }
done
if [ -z "$ACL_LIBANDROID" ]; then
  # The fixture calls ALooper, so it must link acl's libandroid.so — building without it yields a
  # .so with an unresolvable ALooper_prepare, which fails at dlopen rather than here.
  echo "MISSING: libandroid.so. Stage a bionic runtime first (tools/stage-bionic-runtime.sh)" >&2
  echo "         and point ACL_BIONIC_SYSROOT at its bionic/ dir." >&2
  exit 1
fi

# --- The bionic-ABI NDK library (what an app ships in lib/x86_64/) ---
cat >"$OUT/ndktest.c" <<'EOF'
// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl spike fixture (generated). No GPL-3.0 sources.
#include <jni.h>
#include <unistd.h>
#include <android/log.h>
typedef struct ALooper ALooper;
JNIEXPORT jint JNICALL
Java_com_acl_ndktest_Main_nativeAnswer(JNIEnv* e, jclass c) {
  __android_log_print(ANDROID_LOG_INFO, "ndktest", "nativeAnswer called");
  return 42;
}
// Bidirectional %fs-boundary self-test. Writes a bionic thread-local (bionic's dynamic TLS, via
// %fs), calls back into glibc via cb (which must be trampolined to glibc %fs), then confirms the
// bionic thread-local survived the round trip. __thread avoids any libc header (the repro compiles
// against host headers). Expected via BionicRuntime::run_boundary_selftest with cb(x)=x*2: cb(x)+1000.
static __thread volatile int t_marker;
int acl_ndk_boundary(int (*cb)(int), int x) {
  t_marker = 7;                               // bionic TLS write (needs bionic %fs)
  int r = cb(x);                              // up-call into glibc (needs glibc %fs)
  return r + (t_marker == 7 ? 1000 : 0);      // bionic TLS intact after the round trip?
}
// A native bound via RegisterNatives (not name resolution) — the real JNI_OnLoad path. It makes a
// VALID varargs up-call: GetStaticMethodID + CallStaticIntMethod(helper, 20, 30) on its own class.
// Valid JNI, so it runs against a real JVM as well as the fake ART. Exercises the va_list->...V thunk.
static jint the_registered(JNIEnv* env, jclass clazz) {
  t_marker = 55;                              // bionic TLS under the down-call (bionic %fs)
  jmethodID mid = (*env)->GetStaticMethodID(env, clazz, "helper", "(II)I");
  jint sum = (*env)->CallStaticIntMethod(env, clazz, mid, 20, 30);  // varargs up-call -> 50
  return 4200 + sum;                          // expect 4250
}
// Two native threads, one after the other. Each attaches to the VM, calls Main.callBack(),
// which calls the native answer(), and detaches. callBack() is a call down from Java on a
// native thread, which is when a4l notes the thread's bionic TCB against the glibc-side donor
// the thread borrowed at attach. The first thread's detach gives that donor back to the pool
// and the second thread is given the same one. answer() calls back up (helper), so the second
// thread's round trip only works if its call down ran on ITS bionic TCB and not on the one
// the first thread left noted in the donor. The first thread stays alive, detached, until the
// second is done: were it to exit, the second thread's TCB could land at the address the
// first one's had, and the stale note would be right by accident. Returns what the second
// callBack() returned, 4250, or a negative number for the step that failed.
extern int pthread_create(long* t, const void* attr, void* (*fn)(void*), void* arg);
extern int pthread_join(long t, void** ret);
static JavaVM* g_reuse_vm;
static jclass g_reuse_class;
static volatile int g_reuse_first_done, g_reuse_release;
static void* reuse_thread(void* first) {
  JNIEnv* env = 0;
  if ((*g_reuse_vm)->AttachCurrentThread(g_reuse_vm, (void**)&env, 0) != JNI_OK) return (void*)-2;
  jmethodID mid = (*env)->GetStaticMethodID(env, g_reuse_class, "callBack", "()I");
  long r = mid ? (*env)->CallStaticIntMethod(env, g_reuse_class, mid) : -3;
  (*g_reuse_vm)->DetachCurrentThread(g_reuse_vm);
  if (first) {
    g_reuse_first_done = 1;
    while (!g_reuse_release) usleep(1000);
  }
  return (void*)r;
}
JNIEXPORT jint JNICALL
Java_com_acl_ndktest_Main_donorReuse(JNIEnv* env, jclass clazz) {
  if ((*env)->GetJavaVM(env, &g_reuse_vm) != JNI_OK) return -10;
  g_reuse_class = (jclass)(*env)->NewGlobalRef(env, clazz);
  long t = 0;
  void* ret = 0;
  long first = 0;
  void* first_ret = 0;
  if (pthread_create(&first, 0, reuse_thread, (void*)1) != 0) return -11;
  while (!g_reuse_first_done) usleep(1000);
  const int second = pthread_create(&t, 0, reuse_thread, 0) != 0 || pthread_join(t, &ret) != 0;
  g_reuse_release = 1;
  if (pthread_join(first, &first_ret) != 0 || (long)first_ret != 4250) return -13;
  if (second) return -12;
  (*env)->DeleteGlobalRef(env, g_reuse_class);
  return (jint)(long)ret;
}
// Installs a SIGSEGV handler of its own, as a crash reporter's native part does, and looks
// again a moment later. a4l's handler has to be back: ART takes faults it recovers from (the
// null checks in compiled code), and a reporter's handler in front of ART's treats the first
// of them as a crash and ends the process. The kernel's rt_sigaction is called directly, so
// the answer does not depend on which libc's struct sigaction this is compiled against.
// Returns 1 when the handler that was there before is there again, 0 when this library's is
// still in place, 2 for anything else, negative for a call that failed.
extern long syscall(long number, ...);
struct ndktest_ksigaction { void* handler; unsigned long flags; void* restorer; unsigned long mask; };
static void ndktest_own_handler(int sig) { (void)sig; }
JNIEXPORT jint JNICALL
Java_com_acl_ndktest_Main_handlerKept(JNIEnv* e, jclass c) {
  enum { kRtSigaction = 13, kSigsegv = 11 };
  struct ndktest_ksigaction before = {0}, mine, now = {0};
  if (syscall(kRtSigaction, kSigsegv, (void*)0, &before, 8L) != 0) return -1;
  mine = before;                       // keeps the restorer and flags the kernel needs
  mine.handler = (void*)ndktest_own_handler;
  if (syscall(kRtSigaction, kSigsegv, &mine, (void*)0, 8L) != 0) return -2;
  usleep(300000);
  if (syscall(kRtSigaction, kSigsegv, (void*)0, &now, 8L) != 0) return -3;
  if (now.handler == (void*)ndktest_own_handler) return 0;
  return now.handler == before.handler ? 1 : 2;
}
// Bound by name. Reads ro.build.version.sdk the way native code does. The symbol is libc's by
// link; a4l's libacl_sysprops.so comes ahead of libc in the lookup, and answers from the table
// the runtime gives android.os.SystemProperties. libc's own has no property area: 0.
extern int __system_property_get(const char* name, char* value);
JNIEXPORT jint JNICALL
Java_com_acl_ndktest_Main_sdkProperty(JNIEnv* e, jclass c) {
  char value[92] = "";
  int n = __system_property_get("ro.build.version.sdk", value), sdk = 0;
  for (int i = 0; i < n && value[i] >= '0' && value[i] <= '9'; i++) sdk = sdk * 10 + (value[i] - '0');
  return sdk;
}
// Bound by name. Plays through a4l's libaaudio.so the way an engine does: opens the library
// itself, makes a 48 kHz stereo 16-bit output with a data callback, runs it for 300 ms and
// stops it. The callback fills silence and counts. Returns the number of callbacks, which is
// positive only if the stream opened, started and was fed; a negative step code otherwise.
extern void* dlopen(const char* name, int flags);
extern void* dlsym(void* handle, const char* name);
static volatile int ndktest_aaudio_calls;
static volatile int ndktest_aaudio_frames;
static int ndktest_aaudio_cb(void* stream, void* user, void* data, int frames) {
  (void)stream; (void)user;
  short* pcm = (short*)data;
  for (int i = 0; i < frames * 2; i++) pcm[i] = 0;
  ndktest_aaudio_calls++;
  ndktest_aaudio_frames += frames;
  return 0;                                   // AAUDIO_CALLBACK_RESULT_CONTINUE
}
JNIEXPORT jint JNICALL
Java_com_acl_ndktest_Main_aaudioPlay(JNIEnv* e, jclass c) {
  void* lib = dlopen("libaaudio.so", 2);
  if (!lib) return -1;
#define NDKTEST_SYM(name) void* name = dlsym(lib, "AAudio" #name); if (!name) return -2
  NDKTEST_SYM(_createStreamBuilder); NDKTEST_SYM(StreamBuilder_setSampleRate);
  NDKTEST_SYM(StreamBuilder_setChannelCount); NDKTEST_SYM(StreamBuilder_setFormat);
  NDKTEST_SYM(StreamBuilder_setDataCallback); NDKTEST_SYM(StreamBuilder_openStream);
  NDKTEST_SYM(StreamBuilder_delete); NDKTEST_SYM(Stream_requestStart);
  NDKTEST_SYM(Stream_requestStop); NDKTEST_SYM(Stream_getState);
  NDKTEST_SYM(Stream_getFramesWritten); NDKTEST_SYM(Stream_close);
#undef NDKTEST_SYM
  void* builder = 0; void* stream = 0;
  if (((int (*)(void**))_createStreamBuilder)(&builder) != 0) return -3;
  ((void (*)(void*, int))StreamBuilder_setSampleRate)(builder, 48000);
  ((void (*)(void*, int))StreamBuilder_setChannelCount)(builder, 2);
  ((void (*)(void*, int))StreamBuilder_setFormat)(builder, 1);          // AAUDIO_FORMAT_PCM_I16
  ((void (*)(void*, void*, void*))StreamBuilder_setDataCallback)(builder, ndktest_aaudio_cb, 0);
  if (((int (*)(void*, void**))StreamBuilder_openStream)(builder, &stream) != 0) return -4;
  ((int (*)(void*))StreamBuilder_delete)(builder);
  if (((int (*)(void*))Stream_requestStart)(stream) != 0) return -5;
  if (((int (*)(void*))Stream_getState)(stream) != 4) return -6;        // STARTED
  usleep(300000);
  if (((int (*)(void*))Stream_requestStop)(stream) != 0) return -7;
  if (((int (*)(void*))Stream_getState)(stream) != 10) return -8;       // STOPPED
  const int calls = ndktest_aaudio_calls;
  // Everything the callback supplied was queued, and nothing came after the stop.
  if (((long long (*)(void*))Stream_getFramesWritten)(stream) != ndktest_aaudio_frames) return -9;
  usleep(50000);
  if (ndktest_aaudio_calls != calls) return -10;
  if (((int (*)(void*))Stream_close)(stream) != 0) return -11;
  return calls;
}
// Decoy for the bind-by-name path: an EXPORTED Java_<class>_answer, while JNI_OnLoad below ALSO
// RegisterNatives-binds answer() to the_registered (4250). A RegisterNatives binding is the app's
// explicit choice and must win, so acl's name binding has to skip methods JNI_OnLoad already
// registered (BionicRuntime::was_registered). If it ever stops doing so, this 9999 silently
// replaces the real implementation and the end-to-end test's answer()=4250 catches it.
JNIEXPORT jint JNICALL
Java_com_acl_ndktest_Main_answer(JNIEnv* e, jclass c) { return 9999; }
// Exercises acl's libandroid.so (staged by tools/stage-bionic-runtime.sh) the way a real engine
// does: prepare a looper, register a pipe on it, wake it through that pipe, and poll. Flutter's
// engine threads each run exactly this pattern over real fds. Returns 55 iff pollOnce reported OUR
// registration back (the ident + the fd we added), which is what makes it a real epoll round trip
// and not just a linkable symbol.
extern ALooper* ALooper_prepare(int opts);
extern ALooper* ALooper_forThread(void);
extern int ALooper_addFd(ALooper* l, int fd, int ident, int events,
                         int (*cb)(int, int, void*), void* data);
extern int ALooper_pollOnce(int timeoutMillis, int* outFd, int* outEvents, void** outData);
extern int ALooper_removeFd(ALooper* l, int fd);
extern void ALooper_wake(ALooper* l);
JNIEXPORT jint JNICALL
Java_com_acl_ndktest_Main_looperTest(JNIEnv* e, jclass c) {
  ALooper* l = ALooper_prepare(1 /*ALOOPER_PREPARE_ALLOW_NON_CALLBACKS*/);
  if (!l || ALooper_forThread() != l) return -1;   // prepare must be idempotent per thread
  int fds[2];
  if (pipe(fds) != 0) return -2;
  if (ALooper_addFd(l, fds[0], 7 /*ident*/, 1 /*ALOOPER_EVENT_INPUT*/, NULL, (void*)0x1234) != 1)
    return -3;
  // Nothing is readable yet: the looper must time out rather than invent an event.
  if (ALooper_pollOnce(0, NULL, NULL, NULL) != -3 /*ALOOPER_POLL_TIMEOUT*/) return -4;
  // ALooper_wake must break a poll and report WAKE, distinctly from our fd.
  ALooper_wake(l);
  if (ALooper_pollOnce(1000, NULL, NULL, NULL) != -1 /*ALOOPER_POLL_WAKE*/) return -5;
  if (write(fds[1], "x", 1) != 1) return -6;
  int out_fd = -1, out_events = 0;
  void* out_data = NULL;
  const int r = ALooper_pollOnce(1000, &out_fd, &out_events, &out_data);
  ALooper_removeFd(l, fds[0]);
  close(fds[0]);
  close(fds[1]);
  if (r != 7 || out_fd != fds[0] || !(out_events & 1) || out_data != (void*)0x1234) return -7;
  return 55;
}
// Exercises the JNIEnv/JavaVM boundary: GetEnv (VM up-call) then GetVersion/FindClass/
// RegisterNatives (Env up-calls), each crossing bionic->glibc. Returns JNI_VERSION_1_6.
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  JNIEnv* env = 0;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return -1;
  (void)(*env)->GetVersion(env);
  jclass c = (*env)->FindClass(env, "com/acl/ndktest/Main");
  JNINativeMethod m = { "answer", "()I", (void*)the_registered };
  if ((*env)->RegisterNatives(env, c, &m, 1) != JNI_OK) return -1;
  t_marker = 33;                              // bionic TLS after the up-calls returned
  return JNI_VERSION_1_6;
}
EOF

# Link against the real bionic libs by full path so NEEDED sonames are bionic (libc.so, not
# libc.so.6) and no glibc crt/loader leaks in. -nostdlib + explicit crt = a clean bionic .so.
# -fno-emulated-tls: use native ELF TLS (__tls_get_addr, provided by bionic) for __thread rather
# than clang's android-default emulated-TLS (__emutls_get_address, a compiler-rt symbol not in bionic).
"$CLANG" --target=x86_64-linux-android31 \
  -fPIC -shared -nostdlib -fno-emulated-tls -Wl,-soname,libndktest.so \
  -I"$INC_JNI" -I"$INC_LOG" \
  -o "$OUT/libndktest.so" \
  "$CRTBEGIN" "$OUT/ndktest.c" "$CRTEND" \
  "$BIONIC_LIBC" "$BIONIC_LIBLOG" "$BIONIC_LIBDL" $ACL_LIBANDROID \
  -fuse-ld=lld 2>/dev/null \
|| "$CLANG" --target=x86_64-linux-android31 -fPIC -shared -nostdlib -fno-emulated-tls \
     -Wl,-soname,libndktest.so -I"$INC_JNI" -I"$INC_LOG" -o "$OUT/libndktest.so" \
     "$CRTBEGIN" "$OUT/ndktest.c" "$CRTEND" \
     "$BIONIC_LIBC" "$BIONIC_LIBLOG" "$BIONIC_LIBDL" $ACL_LIBANDROID -fuse-ld=lld

# --- A second .so that links a4l's libmediandk.so, where the stage has one ---
ACL_LIBMEDIANDK="$(dirname "$ACL_LIBANDROID")/libmediandk.so"
if [ -f "$ACL_LIBMEDIANDK" ]; then
  cat >"$OUT/mediatest.c" <<'EOF'
// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l fixture (generated); calls the NDK media API as
// <media/NdkMediaFormat.h> and <media/NdkMediaCodec.h> declare it. No GPL-3.0 sources.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef struct AMediaFormat AMediaFormat;
typedef struct AMediaCodec AMediaCodec;
extern const char* AMEDIAFORMAT_KEY_MIME;
extern const char* AMEDIAFORMAT_KEY_SAMPLE_RATE;
extern const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT;
AMediaFormat* AMediaFormat_new(void);
int32_t AMediaFormat_delete(AMediaFormat*);
void AMediaFormat_setInt32(AMediaFormat*, const char*, int32_t);
void AMediaFormat_setString(AMediaFormat*, const char*, const char*);
void AMediaFormat_setBuffer(AMediaFormat*, const char*, const void*, size_t);
bool AMediaFormat_getInt32(AMediaFormat*, const char*, int32_t*);
bool AMediaFormat_getString(AMediaFormat*, const char*, const char**);
bool AMediaFormat_getBuffer(AMediaFormat*, const char*, void**, size_t*);
const char* AMediaFormat_toString(AMediaFormat*);
AMediaCodec* AMediaCodec_createDecoderByType(const char*);
int32_t AMediaCodec_start(AMediaCodec*);

// 77 when every step holds; a negative number names the step that did not.
__attribute__((visibility("default")))
int Java_com_acl_ndktest_Main_mediaFormatTest(void* e, void* c) {
  (void)e; (void)c;
  if (strcmp(AMEDIAFORMAT_KEY_MIME, "mime") != 0 ||
      strcmp(AMEDIAFORMAT_KEY_SAMPLE_RATE, "sample-rate") != 0) return -1;
  AMediaFormat* f = AMediaFormat_new();
  if (!f) return -2;
  AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
  AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, 44100);
  AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 2);
  const unsigned char csd[4] = {0x12, 0x10, 0x00, 0xff};
  AMediaFormat_setBuffer(f, "csd-0", csd, sizeof csd);
  const char* mime = NULL;
  int32_t rate = 0, channels = 0, none = 5;
  if (!AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) ||
      strcmp(mime, "audio/mp4a-latm") != 0) return -3;
  if (!AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate) || rate != 44100) return -4;
  if (!AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels) || channels != 2)
    return -5;
  // A key that is absent, and one of another type, are both "not there".
  if (AMediaFormat_getInt32(f, "absent", &none) || none != 5) return -6;
  if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_MIME, &none)) return -7;
  void* data = NULL;
  size_t size = 0;
  if (!AMediaFormat_getBuffer(f, "csd-0", &data, &size) || size != sizeof csd ||
      memcmp(data, csd, size) != 0 || data == (const void*)csd) return -8;
  // Setting a key again replaces its value and may change its type.
  AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, 48000);
  if (!AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate) || rate != 48000) return -9;
  AMediaFormat_setString(f, "csd-0", "now a string");
  if (AMediaFormat_getBuffer(f, "csd-0", &data, &size)) return -10;
  const char* text = AMediaFormat_toString(f);
  if (!text || !strstr(text, "sample-rate: int32(48000)") ||
      !strstr(text, "mime: string(audio/mp4a-latm)")) return -11;
  if (AMediaFormat_delete(f) != 0) return -12;
  // No codec stands behind AMediaCodec yet: NULL, and a call on it is an error, not a crash.
  AMediaCodec* codec = AMediaCodec_createDecoderByType("audio/mp4a-latm");
  if (codec != NULL) return -13;
  if (AMediaCodec_start(codec) == 0) return -14;
  return 77;
}
EOF
  "$CLANG" --target=x86_64-linux-android31 -fPIC -shared -nostdlib -fno-emulated-tls \
    -Wl,-soname,libmediatest.so -o "$OUT/libmediatest.so" \
    "$CRTBEGIN" "$OUT/mediatest.c" "$CRTEND" "$BIONIC_LIBC" "$ACL_LIBMEDIANDK" -fuse-ld=lld
  echo "== built $OUT/libmediatest.so =="
fi

echo "== built $OUT/libndktest.so =="
readelf -d "$OUT/libndktest.so" | grep -E 'NEEDED|SONAME'
echo "== exported symbol =="
readelf -sW "$OUT/libndktest.so" | grep nativeAnswer | head -1

# --- A glibc probe that tries to dlopen it (the failure) ---
cat >"$OUT/dltest.c" <<'EOF'
// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl spike fixture (generated). No GPL-3.0 sources.
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char** argv) {
  void* h = dlopen(argv[1], RTLD_NOW);
  if (!h) { printf("dlopen FAILED: %s\n", dlerror()); return 1; }
  int (*fn)(void*, void*) = (int(*)(void*,void*))dlsym(h, "Java_com_acl_ndktest_Main_nativeAnswer");
  printf("loaded; nativeAnswer=%d\n", fn ? fn(0,0) : -1);
  return 0;
}
EOF
cc -o "$OUT/dltest" "$OUT/dltest.c" -ldl   # host glibc cc

echo "== glibc dlopen of the bionic .so =="
"$OUT/dltest" "$OUT/libndktest.so" || true
echo "(expected: 'dlopen FAILED: .../libc.so: invalid ELF header' — the host NDK loader gap)"
echo "artifacts in $OUT"
