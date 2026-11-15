// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture. No GPL-3.0 sources.
//
// Main, driving the codec with a MediaCodec.Callback.
package com.acl.audiocodec;

public class MainAsync extends Main {
    @Override
    protected boolean async() {
        return true;
    }
}
