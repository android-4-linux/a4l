// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture. No GPL-3.0 sources.
//
// Main, with the SurfaceView added half a second after launch.
package com.acl.videoplane;

public class MainLate extends Main {
    @Override
    protected boolean addLate() {
        return true;
    }
}
