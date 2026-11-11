// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/pkg.md (pending), AOSP androidfw ResourceTypes.h binary-XML
// layout (ResChunk_header/ResStringPool/ResXMLTree), Android manifest
// semantics. No GPL-3.0 sources.
#ifndef ACL_PKG_MANIFEST_H
#define ACL_PKG_MANIFEST_H

// Parse the compiled binary-XML AndroidManifest.xml (AXML) extracted from an
// APK and expose the fields the runtime needs to launch: package, version, SDK
// levels, and the activity components (with launcher detection). This is a
// focused Phase 1 reader for AndroidManifest.xml only; general resources.arsc /
// arbitrary AXML move to AOSP libandroidfw when it is vendored.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace acl::pkg {

enum class AxmlStatus {
  Ok,
  BadFormat,    // not AXML / inconsistent chunk structure
  Truncated,    // a read ran past the buffer
  Unsupported,  // a construct this reader does not handle
};

const char* axml_status_name(AxmlStatus status) noexcept;

struct AxmlError {
  AxmlStatus status = AxmlStatus::Ok;
  std::string message;
  bool is_ok() const noexcept { return status == AxmlStatus::Ok; }
  explicit operator bool() const noexcept { return status != AxmlStatus::Ok; }
};

struct Activity {
  std::string name;  // component name, FQ-resolved (for <activity-alias> this
                     // is the alias)
  std::string target_activity;  // <activity-alias android:targetActivity>,
                                // FQ-resolved (else empty)
  bool launcher = false;        // has an intent-filter with MAIN + LAUNCHER
  bool enabled =
      true;  // android:enabled (default true); disabled ones aren't launchable
  std::uint32_t theme = 0;  // android:theme style resId (0 if none); overrides
                            // the application theme

  // The class to actually instantiate: an <activity-alias> names a target class
  // that is the real Activity (the alias name is not a class), so follow it; a
  // plain <activity> is itself.
  const std::string& launch_class() const noexcept {
    return target_activity.empty() ? name : target_activity;
  }
};

struct Manifest {
  std::string package;
  std::optional<std::uint32_t> version_code;
  std::string version_name;
  std::optional<std::int32_t> min_sdk;
  std::optional<std::int32_t> target_sdk;
  std::optional<std::int32_t> compile_sdk;
  std::string application_name;  // <application android:name>, FQ-resolved
                                 // (empty if none)
  std::uint32_t application_theme =
      0;  // <application android:theme> style resId (0 if none)
  std::vector<Activity> activities;

  // First activity marked as a launcher, or nullptr.
  const Activity* launcher() const noexcept;
};

// Resolve an android:name component to a fully-qualified class name, applying
// Android's shorthand rules relative to `package`:
//   ".Foo"     -> package + ".Foo"
//   "Foo"      -> package + ".Foo"
//   "a.b.Foo"  -> "a.b.Foo"  (already qualified)
std::string resolve_class_name(std::string_view package, std::string_view name);

// Parse AXML bytes into `out`. On failure returns a non-ok error and leaves
// `out` partially populated.
AxmlError parse_manifest(const std::uint8_t* data,
                         std::size_t size,
                         Manifest& out);
AxmlError parse_manifest(const std::vector<std::uint8_t>& bytes, Manifest& out);

}  // namespace acl::pkg

#endif  // ACL_PKG_MANIFEST_H
