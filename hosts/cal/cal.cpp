// SPDX-License-Identifier: Apache-2.0
// Provenance: plan.md §A01.2 (CAL template host), §A01.3 (Gate G1.5b), ADR-7 /
// A02.3; androidx.car.app model docs (ListTemplate/ItemList/Row) +
// ConstraintManager content-limit semantics (public androidx). No GPL-3.0 sources.
//
// The native Car App Library template host: a deserialized template model rendered
// to a structural line layout, with the Tier-1 content-limit distraction rule
// enforced. Pixel-backend independent — the native-Skia renderer (A02.3) consumes
// these lines; here they are the testable substrate.

#include "hosts/cal/cal.h"

namespace acl::hosts::cal {
namespace {

std::string header_line(const Template& t) {
  std::string title = t.title.empty() ? "(untitled)" : t.title;
  return std::string("[") + template_kind_name(t.kind) + "] " + title;
}

}  // namespace

const char* template_kind_name(TemplateKind k) noexcept {
  switch (k) {
    case TemplateKind::List: return "List";
    case TemplateKind::Grid: return "Grid";
    case TemplateKind::Pane: return "Pane";
    case TemplateKind::Message: return "Message";
    case TemplateKind::LongMessage: return "LongMessage";
    case TemplateKind::Unknown: return "Unknown";
  }
  return "Unknown";
}

CalHost::CalHost(int content_limit)
    : content_limit_(content_limit > 0 ? content_limit : kDefaultContentLimit) {}

const std::vector<std::string>& CalHost::render(const Template& t) {
  lines_.clear();
  truncated_ = false;
  visible_rows_ = 0;
  lines_.push_back(header_line(t));

  switch (t.kind) {
    case TemplateKind::Message:
    case TemplateKind::LongMessage:
      if (!t.message.empty()) lines_.push_back("  " + t.message);
      break;
    default: {
      // List / Grid / Pane: enforce the ConstraintManager content limit — rows
      // beyond it are dropped (the driver-distraction rule the host owns).
      const int n = static_cast<int>(t.rows.size());
      const int shown = n > content_limit_ ? content_limit_ : n;
      truncated_ = n > shown;
      visible_rows_ = shown;
      for (int i = 0; i < shown; ++i) {
        std::string line = "  • " + t.rows[i].title;
        if (!t.rows[i].text.empty()) line += " — " + t.rows[i].text;
        lines_.push_back(std::move(line));
      }
      break;
    }
  }
  return lines_;
}

CalHost& CalHost::instance() {
  static CalHost host;
  return host;
}

const char* module_name() noexcept { return "hosts/cal"; }

}  // namespace acl::hosts::cal
