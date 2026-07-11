// SPDX-License-Identifier: Apache-2.0
// Provenance: plan.md §A01.2 (CAL template host), §A01.3 (Gate G1.5b), ADR-7 /
// A02.3 (native-Skia renderer primary); androidx.car.app model docs (ListTemplate,
// ItemList, Row) + CarUxRestrictions/ConstraintManager content-limit semantics
// (public androidx). No GPL-3.0 sources.
#ifndef ACL_HOSTS_CAL_CAL_H
#define ACL_HOSTS_CAL_CAL_H

// The Car App Library template host (Gate G1.5b). androidx.car.app apps never draw;
// they return a Bundleable template tree to a platform host that renders it and
// enforces driver-distraction rules. Per ADR-3 the app<->host boundary collapses to
// direct calls in-process (no Binder). This is the native, pixel-backend-independent
// core: a deserialized template model + Tier-1 rendering to a structural line layout
// (the substrate the native-Skia renderer, A02.3, will draw) + the content-limit
// distraction rule (ConstraintManager list limit).

#include <string>
#include <vector>

namespace acl::hosts::cal {

// Tier-1 template set (plan.md §A01.2). Unknown covers templates not yet modeled.
enum class TemplateKind { List, Grid, Pane, Message, LongMessage, Unknown };
const char* template_kind_name(TemplateKind k) noexcept;

// A single item/row of a List/Grid/Pane template.
struct Row {
  std::string title;
  std::string text;  // optional secondary line
};

// A deserialized template returned by a Screen (the Bundleable tree, flattened).
struct Template {
  TemplateKind kind = TemplateKind::Unknown;
  std::string title;
  std::vector<Row> rows;    // List / Grid / Pane items
  std::string message;      // Message / LongMessage body
};

class CalHost {
 public:
  // CAL's default ConstraintManager content limit for list templates.
  static constexpr int kDefaultContentLimit = 6;

  explicit CalHost(int content_limit = kDefaultContentLimit);

  // Render `t` into the structural line layout: a header line plus up to
  // content_limit row lines (or the message body). Rows beyond the limit are
  // dropped to satisfy the distraction rule — truncated() then reports it. Returns
  // the rendered lines (also available via lines()).
  const std::vector<std::string>& render(const Template& t);

  const std::vector<std::string>& lines() const { return lines_; }
  bool truncated() const { return truncated_; }   // last render dropped rows
  int visible_rows() const { return visible_rows_; }
  int content_limit() const { return content_limit_; }

  static CalHost& instance();

 private:
  int content_limit_;
  std::vector<std::string> lines_;
  bool truncated_ = false;
  int visible_rows_ = 0;
};

const char* module_name() noexcept;

}  // namespace acl::hosts::cal

#endif  // ACL_HOSTS_CAL_CAL_H
