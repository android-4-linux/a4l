// SPDX-License-Identifier: Apache-2.0
// Provenance: plan.md §A01.3 (Gate G1.5b), §5 (Tier A unit); androidx.car.app
// model docs + ConstraintManager content-limit semantics (public androidx). No
// GPL-3.0 sources.
//
// Tier-A unit tests for the native Car App Library template host (hosts/cal).
// Automotive-profile only — the module is compiled out under the phone profile.

#include <catch2/catch_test_macros.hpp>

#include "hosts/cal/cal.h"

using namespace acl::hosts::cal;

TEST_CASE("list template renders a header and bulleted rows",
          "[cal][automotive]") {
  Template t;
  t.kind = TemplateKind::List;
  t.title = "Media";
  t.rows = {{"Play Music", ""}, {"Browse Library", "12 albums"}};

  CalHost host;
  const auto& lines = host.render(t);
  REQUIRE(lines.size() == 3);
  CHECK(lines[0] == "[List] Media");
  CHECK(lines[1] == "  • Play Music");
  CHECK(lines[2] ==
        "  • Browse Library — 12 albums");  // secondary text appended
  CHECK(host.visible_rows() == 2);
  CHECK_FALSE(host.truncated());
}

TEST_CASE("content limit drops rows beyond the distraction cap",
          "[cal][automotive]") {
  Template t;
  t.kind = TemplateKind::List;
  t.title = "Long";
  for (int i = 0; i < 10; ++i)
    t.rows.push_back({"Row " + std::to_string(i), ""});

  CalHost host;  // default limit 6
  CHECK(host.content_limit() == CalHost::kDefaultContentLimit);
  const auto& lines = host.render(t);
  CHECK(host.visible_rows() == 6);
  CHECK(host.truncated());
  CHECK(lines.size() == 1 + 6);        // header + 6 rows
  CHECK(lines.back() == "  • Row 5");  // 7th..10th dropped

  CalHost roomy(20);  // a host with a higher limit shows them all
  roomy.render(t);
  CHECK(roomy.visible_rows() == 10);
  CHECK_FALSE(roomy.truncated());
}

TEST_CASE("message templates render the body, not rows", "[cal][automotive]") {
  Template t;
  t.kind = TemplateKind::Message;
  t.title = "Signed out";
  t.message = "Please sign in to continue.";

  CalHost host;
  const auto& lines = host.render(t);
  REQUIRE(lines.size() == 2);
  CHECK(lines[0] == "[Message] Signed out");
  CHECK(lines[1] == "  Please sign in to continue.");
  CHECK(host.visible_rows() == 0);
}

TEST_CASE("template kind names cover the Tier-1 set", "[cal][automotive]") {
  CHECK(std::string(template_kind_name(TemplateKind::List)) == "List");
  CHECK(std::string(template_kind_name(TemplateKind::Grid)) == "Grid");
  CHECK(std::string(template_kind_name(TemplateKind::Pane)) == "Pane");
  CHECK(std::string(template_kind_name(TemplateKind::Message)) == "Message");
  CHECK(std::string(template_kind_name(TemplateKind::LongMessage)) ==
        "LongMessage");
}

TEST_CASE("an untitled template still renders a header", "[cal][automotive]") {
  Template t;
  t.kind = TemplateKind::Pane;
  CalHost host;
  const auto& lines = host.render(t);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "[Pane] (untitled)");
}
