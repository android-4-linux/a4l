// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l; the format words and what each means are those
// android.os.Process documents (AOSP, Apache-2.0). No GPL-3.0 sources.
//
// android.os.Process.readProcFile and parseProcLine: a /proc file, or a line
// of one, split into fields by a format array. An app reads its own
// /proc/self/stat this way (start time, CPU time), and so does the framework.
//
// getElapsedCpuTime, and sendSignal with its quiet form: what killProcess
// calls. An app ends itself with killProcess(myPid()).
#include "runtime/process_proc_jni.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <string>

#include "log/log.h"

namespace acl::runtime {
namespace {

// android.os.Process.PROC_*
constexpr jint kTermMask = 0xff;
constexpr jint kCombine = 0x100;
constexpr jint kParens = 0x200;
constexpr jint kQuotes = 0x400;
constexpr jint kChar = 0x800;
constexpr jint kOutString = 0x1000;
constexpr jint kOutLong = 0x2000;
constexpr jint kOutFloat = 0x4000;

// A /proc file is small. One that is not is cut here, not read to the end.
constexpr std::size_t kMaxFile = 1U << 20;

// Splits [pos, end) of `data` by `format`, one field per format word, and
// stores the fields that ask for it in the out arrays, in order. False if the
// data ran out before the format did; what was parsed up to there is stored.
bool parse(JNIEnv* env,
           const std::string& data,
           std::size_t pos,
           std::size_t end,
           jintArray format,
           jobjectArray out_strings,
           jlongArray out_longs,
           jfloatArray out_floats) {
  const jsize nf = env->GetArrayLength(format);
  const jsize ns =
      out_strings != nullptr ? env->GetArrayLength(out_strings) : 0;
  const jsize nl = out_longs != nullptr ? env->GetArrayLength(out_longs) : 0;
  const jsize nr = out_floats != nullptr ? env->GetArrayLength(out_floats) : 0;
  jsize out = 0;  // the next out slot, shared by the three arrays
  for (jsize f = 0; f < nf; ++f) {
    jint mode = 0;
    env->GetIntArrayRegion(format, f, 1, &mode);
    const char term = static_cast<char>(mode & kTermMask);
    // A field in parentheses or quotes starts after the opening one and runs
    // to the closing one, whatever is inside. A field said to be quoted that
    // does not open with a quote is an ordinary field.
    char close = '\0';
    if ((mode & kParens) != 0) {
      close = ')';
      ++pos;
    } else if ((mode & kQuotes) != 0 && pos < end && data[pos] == '"') {
      close = '"';
      ++pos;
    }
    if (pos >= end)
      return false;
    const std::size_t first = pos;
    std::size_t last = std::string::npos;  // one past the field
    if (close != '\0') {
      while (pos < end && data[pos] != close)
        ++pos;
      last = pos;
      if (pos < end)
        ++pos;
    }
    while (pos < end && data[pos] != term)
      ++pos;
    if (last == std::string::npos)
      last = pos;
    if (pos < end) {
      ++pos;  // the terminator
      if ((mode & kCombine) != 0) {
        while (pos < end && data[pos] == term)
          ++pos;
      }
    }
    if ((mode & (kOutString | kOutLong | kOutFloat)) == 0)
      continue;
    const std::string field = data.substr(first, last - first);
    if ((mode & kOutFloat) != 0 && out < nr) {
      const jfloat v = std::strtof(field.c_str(), nullptr);
      env->SetFloatArrayRegion(out_floats, out, 1, &v);
    }
    if ((mode & kOutLong) != 0 && out < nl) {
      // PROC_CHAR: the field's first character as a number, not its digits.
      const jlong v =
          (mode & kChar) != 0
              ? static_cast<jlong>(field.empty() ? 0 : field[0])
              : static_cast<jlong>(std::strtoll(field.c_str(), nullptr, 10));
      env->SetLongArrayRegion(out_longs, out, 1, &v);
    }
    if ((mode & kOutString) != 0 && out < ns) {
      jstring s = env->NewStringUTF(field.c_str());
      if (s == nullptr)
        return false;  // OutOfMemoryError is pending
      env->SetObjectArrayElement(out_strings, out, s);
      env->DeleteLocalRef(s);
    }
    ++out;
  }
  return true;
}

jboolean JNICALL read_proc_file(JNIEnv* env,
                                jclass,
                                jstring file,
                                jintArray format,
                                jobjectArray out_strings,
                                jlongArray out_longs,
                                jfloatArray out_floats) {
  if (file == nullptr || format == nullptr) {
    if (jclass npe = env->FindClass("java/lang/NullPointerException"))
      env->ThrowNew(npe, nullptr);
    return JNI_FALSE;
  }
  const char* path = env->GetStringUTFChars(file, nullptr);
  if (path == nullptr)
    return JNI_FALSE;
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  env->ReleaseStringUTFChars(file, path);
  if (fd < 0)
    return JNI_FALSE;
  std::string data;
  char chunk[4096];
  while (data.size() < kMaxFile) {
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    data.append(chunk, static_cast<std::size_t>(n));
  }
  ::close(fd);
  return parse(env, data, 0, data.size(), format, out_strings, out_longs,
               out_floats)
             ? JNI_TRUE
             : JNI_FALSE;
}

jboolean JNICALL parse_proc_line(JNIEnv* env,
                                 jclass,
                                 jbyteArray buffer,
                                 jint start,
                                 jint end,
                                 jintArray format,
                                 jobjectArray out_strings,
                                 jlongArray out_longs,
                                 jfloatArray out_floats) {
  if (buffer == nullptr || format == nullptr) {
    if (jclass npe = env->FindClass("java/lang/NullPointerException"))
      env->ThrowNew(npe, nullptr);
    return JNI_FALSE;
  }
  // The caller's indices are not trusted: only what is in the array is read.
  const jsize len = env->GetArrayLength(buffer);
  if (end > len)
    end = len;
  if (start < 0 || start >= end)
    return JNI_FALSE;
  std::string data(static_cast<std::size_t>(end - start), '\0');
  env->GetByteArrayRegion(buffer, start, end - start,
                          reinterpret_cast<jbyte*>(data.data()));
  return parse(env, data, 0, data.size(), format, out_strings, out_longs,
               out_floats)
             ? JNI_TRUE
             : JNI_FALSE;
}

// CPU time this process has used, in milliseconds.
jlong JNICALL elapsed_cpu_time(JNIEnv*, jclass) {
  struct timespec ts{};
  if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
    return 0;
  return static_cast<jlong>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// On Android an app can signal the processes of its own uid, which are its
// own. Here the app has one process, this one, and its uid is the user's: a
// signal for another pid is not sent. SIGKILL for this process ends it the
// way System.exit does, less the Java shutdown hooks a kill does not run,
// with the status a shell gives a process that died of the signal. Any other
// signal is dropped: SIGQUIT and SIGUSR1 ask Android's runtime for a stack
// dump and a collection, and neither is something an app waits on.
void JNICALL send_signal(JNIEnv* env, jclass, jint pid, jint sig) {
  if (pid != static_cast<jint>(::getpid())) {
    log::i("acl.runtime.process",
           "sendSignal({}, {}): not this process; not sent", pid, sig);
    return;
  }
  if (sig != SIGKILL) {
    log::i("acl.runtime.process", "sendSignal: signal {} to itself dropped",
           sig);
    return;
  }
  log::i("acl.runtime.process", "the app killed its own process");
  constexpr jint kStatus = 128 + SIGKILL;
  jclass runtime = env->FindClass("java/lang/Runtime");
  jmethodID get = runtime != nullptr
                      ? env->GetStaticMethodID(runtime, "getRuntime",
                                               "()Ljava/lang/Runtime;")
                      : nullptr;
  jmethodID halt =
      runtime != nullptr ? env->GetMethodID(runtime, "halt", "(I)V") : nullptr;
  jobject rt =
      get != nullptr ? env->CallStaticObjectMethod(runtime, get) : nullptr;
  if (rt != nullptr && halt != nullptr)
    env->CallVoidMethod(rt, halt, kStatus);  // does not return
  env->ExceptionClear();
  ::_exit(kStatus);
}

}  // namespace

void register_process_proc_natives(JNIEnv* env) {
  jclass c = env->FindClass("android/os/Process");
  if (c == nullptr) {
    env->ExceptionClear();
    return;
  }
  const JNINativeMethod m[] = {
      {const_cast<char*>("readProcFile"),
       const_cast<char*>("(Ljava/lang/String;[I[Ljava/lang/String;[J[F)Z"),
       reinterpret_cast<void*>(&read_proc_file)},
      {const_cast<char*>("parseProcLine"),
       const_cast<char*>("([BII[I[Ljava/lang/String;[J[F)Z"),
       reinterpret_cast<void*>(&parse_proc_line)},
      {const_cast<char*>("getElapsedCpuTime"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&elapsed_cpu_time)},
      {const_cast<char*>("sendSignal"), const_cast<char*>("(II)V"),
       reinterpret_cast<void*>(&send_signal)},
      {const_cast<char*>("sendSignalQuiet"), const_cast<char*>("(II)V"),
       reinterpret_cast<void*>(&send_signal)},
  };
  if (env->RegisterNatives(c, m, sizeof(m) / sizeof(m[0])) != JNI_OK)
    env->ExceptionClear();
  env->DeleteLocalRef(c);
}

}  // namespace acl::runtime
