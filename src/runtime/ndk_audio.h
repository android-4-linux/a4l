// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l. No GPL-3.0 sources.
#ifndef ACL_RUNTIME_NDK_AUDIO_H
#define ACL_RUNTIME_NDK_AUDIO_H

namespace acl::runtime {

// Gives the bionic runtime the audio output a4l's libaaudio.so plays through
// and the input its libOpenSLES.so records from.
// Before the app's first native library is loaded.
void register_ndk_audio();

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_NDK_AUDIO_H
