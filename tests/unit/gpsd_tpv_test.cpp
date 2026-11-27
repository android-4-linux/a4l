// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test code; report lines in the form gpsd_json(5)
// documents, with made-up positions. No GPL-3.0 sources.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "svc/location/gpsd_tpv.h"

using acl::svc::location::fix_from_gpsd_line;
using acl::svc::location::LocationFix;
using acl::svc::location::parse_gpsd_endpoint;
using Catch::Approx;

TEST_CASE("a gpsd TPV report with a 3D fix is a location", "[location][gpsd]") {
  const std::string line =
      R"({"class":"TPV","device":"/dev/ttyACM0","mode":3,)"
      R"("time":"2026-11-27T20:50:41.250Z","ept":0.005,"lat":48.117300,)"
      R"("lon":11.516670,"altHAE":545.4,"altMSL":499.6,"alt":499.6,)"
      R"("epx":4.2,"epy":6.1,"epv":9.0,"track":84.4,"magtrack":80.9,)"
      R"("speed":11.52,"climb":0.0,"eps":0.3,"eph":7.5,"sep":12.0})"
      "\r";
  LocationFix f{};
  int mode = -1;
  REQUIRE(fix_from_gpsd_line(line, &f, &mode));
  CHECK(mode == 3);
  CHECK(f.latitude == Approx(48.1173));
  CHECK(f.longitude == Approx(11.51667));
  CHECK(f.accuracy_m == Approx(7.5));
  CHECK(f.has_altitude);
  CHECK(f.altitude_m == Approx(545.4));  // above the ellipsoid, not sea level
  CHECK(f.has_speed);
  CHECK(f.speed_mps == Approx(11.52));
  CHECK(f.has_bearing);
  CHECK(f.bearing_deg == Approx(84.4));
  // 2026-11-27T20:50:41.250Z
  CHECK(f.timestamp_ms == 1795812641250LL);
}

TEST_CASE("a 2D fix has a position and no height", "[location][gpsd]") {
  const std::string line =
      R"({"class":"TPV","mode":2,"time":"2026-11-27T20:50:42.000Z",)"
      R"("lat":-33.8568,"lon":151.2153,"altHAE":30.0,"epx":12.0,"epy":9.0})";
  LocationFix f{};
  int mode = -1;
  REQUIRE(fix_from_gpsd_line(line, &f, &mode));
  CHECK(mode == 2);
  CHECK(f.latitude == Approx(-33.8568));
  CHECK(f.longitude == Approx(151.2153));
  CHECK(f.accuracy_m == Approx(12.0));  // no eph: the larger of epx and epy
  CHECK_FALSE(f.has_altitude);
  CHECK_FALSE(f.has_speed);
  CHECK_FALSE(f.has_bearing);
  CHECK(f.timestamp_ms == 1795812642000LL);
}

TEST_CASE("what is not a fix is not a location", "[location][gpsd]") {
  LocationFix f{};
  int mode = 99;
  // The receiver has not found itself yet: a TPV, and no position.
  CHECK_FALSE(fix_from_gpsd_line(
      R"({"class":"TPV","device":"/dev/ttyACM0","mode":1,"time":"2026-11-27T20:50:40.000Z"})",
      &f, &mode));
  CHECK(mode == 1);
  CHECK_FALSE(fix_from_gpsd_line(R"({"class":"TPV","mode":0})", &f, &mode));
  CHECK(mode == 0);
  // A fix mode and no coordinates, or coordinates that are not on the globe.
  CHECK_FALSE(
      fix_from_gpsd_line(R"({"class":"TPV","mode":3,"lat":1.0})", &f, &mode));
  CHECK_FALSE(fix_from_gpsd_line(
      R"({"class":"TPV","mode":3,"lat":91.0,"lon":0.0})", &f, &mode));
  CHECK_FALSE(fix_from_gpsd_line(
      R"({"class":"TPV","mode":3,"lat":0.0,"lon":-181.0})", &f, &mode));
  // gpsd's other lines.
  CHECK_FALSE(fix_from_gpsd_line(
      R"({"class":"VERSION","release":"3.25","rev":"3.25","proto_major":3,"proto_minor":15})",
      &f, &mode));
  CHECK(mode == -1);
  CHECK_FALSE(fix_from_gpsd_line(
      R"({"class":"SKY","device":"/dev/ttyACM0","hdop":1.2,"satellites":[)"
      R"({"PRN":5,"el":31.0,"az":86.0,"ss":41.0,"used":true},)"
      R"({"PRN":13,"el":12.0,"az":310.0,"ss":20.0,"used":false}]})",
      &f, &mode));
  CHECK_FALSE(fix_from_gpsd_line(
      R"({"class":"DEVICES","devices":[{"class":"DEVICE","path":"/dev/ttyACM0","driver":"u-blox","activated":"2026-11-27T20:50:00.000Z"}]})",
      &f, &mode));
  CHECK_FALSE(fix_from_gpsd_line(
      R"({"class":"WATCH","enable":true,"json":true,"nmea":false,"raw":0,"scaled":false,"timing":false,"split24":false,"pps":false})",
      &f, &mode));
}

TEST_CASE("a position after a nested member is still found",
          "[location][gpsd]") {
  LocationFix f{};
  REQUIRE(fix_from_gpsd_line(
      R"({"extra":{"lat":1.0,"deep":[1,[2,{"lon":3.0}],"x\"y"]},"class":"TPV",)"
      R"("mode":3,"lat":10.5,"lon":20.25,"note":"a é b","flag":null})",
      &f));
  CHECK(f.latitude == Approx(10.5));  // the top level's, not the nested one
  CHECK(f.longitude == Approx(20.25));
}

TEST_CASE("a line that is not a JSON object is refused", "[location][gpsd]") {
  LocationFix f{};
  const char* bad[] = {
      "",
      "garbage",
      "[1,2,3]",
      R"({"class":"TPV","mode":3,"lat":1.0,"lon":2.0)",        // not closed
      R"({"class":"TPV","mode":3,"lat":1.0,"lon":2.0} tail)",  // more after it
      R"({"class":"TPV","mode":3,"lat":1.0,"lon":})",          // no value
      R"({"class":"TPV","mode":3,"lat":1e999,"lon":2.0})",     // not finite
      R"({"class":"TPV","mode":3,"lat":"1.0","lon":2.0})",     // a string
      R"({"class":"TPV" "mode":3})",                           // no comma
      R"({"class":"TPV","mode":3,"lat":1.0,"lon":2.0,"s":"unterminated})",
      R"({class:"TPV"})",
  };
  for (const char* line : bad) {
    INFO(line);
    CHECK_FALSE(fix_from_gpsd_line(line, &f));
  }
  // Nesting deeper than gpsd's is refused, not followed.
  std::string deep = R"({"class":"TPV","mode":3,"lat":1.0,"lon":2.0,"d":)";
  deep += std::string(40, '[') + std::string(40, ']') + "}";
  CHECK_FALSE(fix_from_gpsd_line(deep, &f));
  // A time that cannot be read is the time now, not a refusal.
  REQUIRE(fix_from_gpsd_line(
      R"({"class":"TPV","mode":3,"lat":1.0,"lon":2.0,"time":"yesterday"})",
      &f));
  CHECK(f.timestamp_ms > 1700000000000LL);
}

TEST_CASE("ACL_GPSD names a host, a port, or both", "[location][gpsd]") {
  std::string host, port;
  REQUIRE(parse_gpsd_endpoint("", &host, &port));
  CHECK(host == "localhost");
  CHECK(port == "2947");
  REQUIRE(parse_gpsd_endpoint("gps.local", &host, &port));
  CHECK(host == "gps.local");
  CHECK(port == "2947");
  REQUIRE(parse_gpsd_endpoint("127.0.0.1:12345", &host, &port));
  CHECK(host == "127.0.0.1");
  CHECK(port == "12345");
  REQUIRE(parse_gpsd_endpoint(":3000", &host, &port));
  CHECK(host == "localhost");
  CHECK(port == "3000");
  REQUIRE(parse_gpsd_endpoint("[::1]:2948", &host, &port));
  CHECK(host == "::1");
  CHECK(port == "2948");
  REQUIRE(parse_gpsd_endpoint("[fe80::1]", &host, &port));
  CHECK(host == "fe80::1");
  CHECK(port == "2947");
  CHECK_FALSE(parse_gpsd_endpoint("::1", &host, &port));  // which colon?
  CHECK_FALSE(parse_gpsd_endpoint("host:port", &host, &port));
  CHECK_FALSE(parse_gpsd_endpoint("host:70000", &host, &port));
  CHECK_FALSE(parse_gpsd_endpoint("[::1", &host, &port));
  CHECK_FALSE(parse_gpsd_endpoint("[::1]x", &host, &port));
  CHECK_FALSE(parse_gpsd_endpoint("a b", &host, &port));
  CHECK_FALSE(parse_gpsd_endpoint("/run/gpsd.sock", &host, &port));
}
