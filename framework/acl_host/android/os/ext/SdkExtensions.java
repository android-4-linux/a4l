// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room from the public Android SDK reference for
// android.os.ext.SdkExtensions (getExtensionVersion / getAllExtensionVersions). Not derived from
// AOSP or the predecessor sources. android.os.ext.SdkExtensions lives in the framework-sdkextensions apex
// module, which is NOT on acl's framework-minus-apex boot classpath, so an app that reads a
// mainline SDK-extension version (forkyz's storage/import path) hits NoClassDefFoundError. This
// boot-classpath shim reports "no extensions", which is the correct answer for acl (it ships none).
// No GPL-3.0 sources.
package android.os.ext;

import java.util.Collections;
import java.util.Map;

/**
 * Minimal stand-in for the apex-provided SdkExtensions. acl exposes no mainline SDK extensions, so
 * every extension version is 0 (the documented "not available" value).
 */
public class SdkExtensions {

    private SdkExtensions() {}

    /** The version of the given mainline SDK extension. acl provides none, so always 0. */
    public static int getExtensionVersion(int extension) {
        return 0;
    }

    /** All extension versions, keyed by SDK level. Empty on acl. */
    public static Map<Integer, Integer> getAllExtensionVersions() {
        return Collections.emptyMap();
    }
}
