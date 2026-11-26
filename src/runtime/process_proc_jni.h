// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l. No GPL-3.0 sources.
#ifndef ACL_RUNTIME_PROCESS_PROC_JNI_H
#define ACL_RUNTIME_PROCESS_PROC_JNI_H

#include <jni.h>

namespace acl::runtime {

// Binds android.os.Process.readProcFile and parseProcLine.
void register_process_proc_natives(JNIEnv* env);

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_PROCESS_PROC_JNI_H
