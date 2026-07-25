// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl design (docs/split-process-art.md, S1); AOSP JNI Invocation API docs
// (JNI_CreateJavaVM), ADR-0001. No GPL-3.0 sources.
//
// acl-art-host: the render/VM side of the split-process arm64 design. A small, VM-agnostic
// executable that boots a Java runtime via the JNI Invocation API (dlopen a libjvm.so / libart.so),
// runs `main`, and forwards the VM's stdout/stderr to the acl process over acl_ipc. It links only
// standard POSIX + jni.h + acl_ipc, so the *same source* builds as a glibc binary (x86 dev/CI, using
// OpenJDK) and as a bionic linux_bionic-arm64 binary (the Pi, using the stock bionic host ART) —
// where, being bionic itself, it boots libart.so without the two-libc SIGTRAP that killed in-process
// dlopen (docs/pi-plan.md §P-B/M3).
//
// Invoked by acl's split backend with an inherited SEQPACKET fd:
//   acl-art-host --ipc-fd=N --vm-lib=<libjvm.so|libart.so> --main=<Class> [--opt=<jvm-option>]...

#include <dlfcn.h>
#include <jni.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ipc/ipc.h"

namespace {

using acl::ipc::Channel;
using acl::ipc::Message;
using acl::ipc::MsgType;

// Forward everything written to `read_fd` (the captured stdout/stderr pipe) as Log messages until
// EOF. Runs on its own thread so the VM's prints stream to acl in real time.
void pump_output(int read_fd, Channel* ch) {
  char buf[4096];
  for (;;) {
    ssize_t n = ::read(read_fd, buf, sizeof(buf));
    if (n <= 0) break;
    Message m;
    m.type = MsgType::Log;
    m.extra_len = static_cast<std::uint32_t>(n);
    ch->send(m, std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf), n));
  }
}

const char* opt_val(const char* arg, const char* key) {
  size_t k = std::strlen(key);
  return (std::strncmp(arg, key, k) == 0) ? arg + k : nullptr;
}

// When driving the bionic host ART (libart.so), mirror ArtVm::create_java_vm's boot setup: point the
// ANDROID_* roots at ACL_ART_HOME and build the core boot classpath from the apex javalib jars. ART's
// JNI FindClass resolves against the BOOT classloader, so the app DEX (and any framework) must also
// go on the boot classpath — passed in via --bcp and appended here. Returns the -Xbootclasspath /
// -Xint option strings; sets the env as a side effect.
std::vector<std::string> setup_art(const std::string& home, const std::string& extra_bcp) {
  auto set = [](const char* k, const std::string& v) { ::setenv(k, v.c_str(), 1); };
  set("ANDROID_ROOT", home);
  set("ANDROID_ART_ROOT", home + "/apex/com.android.art");
  set("ANDROID_I18N_ROOT", home + "/com.android.i18n");
  set("ANDROID_TZDATA_ROOT", home + "/com.android.tzdata");
  if (!std::getenv("ANDROID_DATA")) set("ANDROID_DATA", home + "/acl-android-data");

  struct BcpJar { const char* module; const char* apex; };
  static const BcpJar kBcp[] = {
      {"core-oj", "com.android.art"},    {"core-libart", "com.android.art"},
      {"okhttp", "com.android.art"},     {"bouncycastle", "com.android.art"},
      {"apache-xml", "com.android.art"}, {"core-icu4j", "com.android.i18n"},
      {"conscrypt", "com.android.conscrypt"}};
  std::string bcp;
  for (const BcpJar& j : kBcp) {
    if (!bcp.empty()) bcp.push_back(':');
    bcp += home + "/apex/" + j.apex + "/javalib/" + j.module + ".jar";
  }
  if (!extra_bcp.empty()) { bcp.push_back(':'); bcp += extra_bcp; }

  std::vector<std::string> opts;
  opts.push_back("-Xbootclasspath:" + bcp);
  opts.push_back("-Xbootclasspath-locations:" + bcp);
  if (const char* img = std::getenv("ACL_ART_BOOT_IMAGE"); img && *img)
    opts.push_back(std::string("-Ximage:") + img);
  else if (std::getenv("ACL_ART_INT") != nullptr)
    opts.push_back("-Xint");  // opt-out: imageless interpret; no runtime dex2oat
  else
    opts.push_back("-Xusejit:true");  // default: imageless JIT (~23x on hot compute; see jvm_vm)
  return opts;
}

int fail(Channel& ch, const std::string& why) {
  std::string msg = "acl-art-host: " + why + "\n";
  Message m;
  m.type = MsgType::Log;
  m.extra_len = static_cast<std::uint32_t>(msg.size());
  ch.send(m, std::span<const std::byte>(reinterpret_cast<const std::byte*>(msg.data()), msg.size()));
  Message bye;
  bye.type = MsgType::Bye;
  bye.u32 = 1;  // nonzero = failure
  ch.send(bye);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  int ipc_fd = -1;
  std::string vm_lib, main_class, home, bcp, cp;
  std::vector<std::string> jvm_opts;
  for (int i = 1; i < argc; ++i) {
    if (const char* v = opt_val(argv[i], "--ipc-fd=")) ipc_fd = std::atoi(v);
    else if (const char* v = opt_val(argv[i], "--vm-lib=")) vm_lib = v;
    else if (const char* v = opt_val(argv[i], "--main=")) main_class = v;
    else if (const char* v = opt_val(argv[i], "--home=")) home = v;
    else if (const char* v = opt_val(argv[i], "--bcp=")) bcp = v;      // ART: extra boot classpath (app dex + framework)
    else if (const char* v = opt_val(argv[i], "--cp=")) cp = v;        // OpenJDK: app class path
    else if (const char* v = opt_val(argv[i], "--opt=")) jvm_opts.emplace_back(v);
  }
  if (home.empty()) if (const char* h = std::getenv("ACL_ART_HOME")) home = h;
  if (ipc_fd < 0) { std::fprintf(stderr, "acl-art-host: --ipc-fd required\n"); return 2; }
  Channel ch(ipc_fd);

  { Message hello; hello.type = MsgType::Hello; hello.u32 = 1; ch.send(hello); }
  if (vm_lib.empty() || main_class.empty()) return fail(ch, "--vm-lib and --main required");

  // Capture stdout+stderr to a pipe so the VM's System.out reaches acl over IPC.
  int pipefd[2];
  if (::pipe(pipefd) != 0) return fail(ch, "pipe() failed");
  ::dup2(pipefd[1], STDOUT_FILENO);
  ::dup2(pipefd[1], STDERR_FILENO);
  ::close(pipefd[1]);
  std::thread pump(pump_output, pipefd[0], &ch);

  auto finish = [&](int code) {
    std::fflush(stdout);
    std::fflush(stderr);
    ::close(STDOUT_FILENO);
    ::close(STDERR_FILENO);  // EOF -> pump thread exits
    if (pump.joinable()) pump.join();
    Message bye;
    bye.type = MsgType::Bye;
    bye.u32 = static_cast<std::uint32_t>(code);
    ch.send(bye);
    return code;
  };

  // dlopen the runtime and grab the JNI Invocation entrypoint (identical for OpenJDK and ART).
  void* h = ::dlopen(vm_lib.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (!h) return finish(fail(ch, std::string("dlopen ") + vm_lib + ": " + dlerror()));
  using CreateFn = jint (*)(JavaVM**, void**, void*);
  auto create = reinterpret_cast<CreateFn>(::dlsym(h, "JNI_CreateJavaVM"));
  if (!create) return finish(fail(ch, "JNI_CreateJavaVM not found in " + vm_lib));

  // Build the VM option list, backend-appropriate. ART (bionic libart.so): set the ANDROID_* roots
  // and the core+app boot classpath (FindClass uses the boot loader). OpenJDK (x86 dev/CI): the app
  // goes on -Djava.class.path. Passthrough --opt entries follow.
  const bool is_art = vm_lib.find("libart") != std::string::npos;
  std::vector<std::string> all_opts;
  if (is_art) {
    if (home.empty()) return finish(fail(ch, "ART requires --home or ACL_ART_HOME"));
    auto art = setup_art(home, bcp);
    all_opts.insert(all_opts.end(), art.begin(), art.end());
  } else if (!cp.empty()) {
    all_opts.push_back("-Djava.class.path=" + cp);
  }
  all_opts.insert(all_opts.end(), jvm_opts.begin(), jvm_opts.end());

  std::vector<JavaVMOption> opts;
  opts.reserve(all_opts.size());
  for (auto& o : all_opts) opts.push_back(JavaVMOption{const_cast<char*>(o.c_str()), nullptr});
  JavaVMInitArgs args{};
  args.version = JNI_VERSION_1_6;
  args.nOptions = static_cast<jint>(opts.size());
  args.options = opts.empty() ? nullptr : opts.data();
  args.ignoreUnrecognized = JNI_FALSE;

  JavaVM* vm = nullptr;
  JNIEnv* env = nullptr;
  if (create(&vm, reinterpret_cast<void**>(&env), &args) != JNI_OK || !env)
    return finish(fail(ch, "JNI_CreateJavaVM failed"));

  // Resolve and call `public static void main(String[])`.
  std::string bin_name = main_class;
  for (char& c : bin_name) if (c == '.') c = '/';
  jclass cls = env->FindClass(bin_name.c_str());
  if (!cls) { if (env->ExceptionCheck()) env->ExceptionDescribe(); return finish(fail(ch, "class not found: " + main_class)); }
  jmethodID mid = env->GetStaticMethodID(cls, "main", "([Ljava/lang/String;)V");
  if (!mid) { if (env->ExceptionCheck()) env->ExceptionDescribe(); return finish(fail(ch, "no static main([String]) in " + main_class)); }
  jobjectArray jargs = env->NewObjectArray(0, env->FindClass("java/lang/String"), nullptr);
  env->CallStaticVoidMethod(cls, mid, jargs);
  int code = 0;
  if (env->ExceptionCheck()) { env->ExceptionDescribe(); code = 3; }

  vm->DestroyJavaVM();
  return finish(code);
}
