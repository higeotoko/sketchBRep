// docs/82: where the sun is, saved with the model, set by the AI.
#include <gtest/gtest.h>

#include <cmath>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Sun.h"

using namespace sbr;

namespace {
geom::SunSettings tokyo(int month, int day, double hour) {
  geom::SunSettings s;
  s.month = month;
  s.day = day;
  s.hour = hour;
  return s;  // Tokyo 35.68 N, 139.69 E, UTC+9
}
}  // namespace

// Reference values from the NOAA solar calculator (Tokyo); the formulas are
// good to a fraction of a degree.
TEST(Sun, PositionInTokyo) {
  // Summer solstice, 12:00 JST: high, a little east of south (solar noon ~11:43).
  const auto june = geom::sunPosition(tokyo(6, 21, 12.0));
  EXPECT_NEAR(june.altitude, 77.4, 1.0);
  EXPECT_NEAR(june.azimuth, 195.0, 6.0);
  // Winter solstice, 12:00 JST: low in the south.
  const auto december = geom::sunPosition(tokyo(12, 21, 12.0));
  EXPECT_NEAR(december.altitude, 30.8, 1.0);
  EXPECT_NEAR(december.azimuth, 183.0, 4.0);
  // Equinox morning, 9:00: south-east, rising.
  const auto march = geom::sunPosition(tokyo(3, 20, 9.0));
  EXPECT_GT(march.azimuth, 110.0);
  EXPECT_LT(march.azimuth, 140.0);
  // Hour angle -42 degrees, declination ~0: sin(alt) = cos(35.68) cos(42) -> about 37 degrees.
  EXPECT_NEAR(march.altitude, 36.8, 1.0);
  // Night.
  EXPECT_FALSE(geom::sunPosition(tokyo(6, 21, 0.5)).up());
}

TEST(Sun, DirectionFollowsNorthInTheModel) {
  // Noon in winter: the sun in the south, i.e. toward -Y when north is +Y.
  auto s = tokyo(12, 21, 11.75);
  auto p = geom::sunPosition(s);
  EXPECT_LT(p.towardSun[1], -0.5);
  EXPECT_NEAR(p.towardSun[0], 0.0, 0.1);
  EXPECT_NEAR(std::hypot(p.towardSun[0], p.towardSun[1], p.towardSun[2]), 1.0, 1e-9);
  // True north turned 90 degrees clockwise (north = +X): south is -X.
  s.northAngle = 90.0;
  p = geom::sunPosition(s);
  EXPECT_LT(p.towardSun[0], -0.5);
  EXPECT_NEAR(p.towardSun[1], 0.0, 0.1);
}

TEST(Sun, CheckedSavedUndoneAndSetByTheAi) {
  doc::Modeler m;
  geom::SunSettings bad = tokyo(2, 30, 12.0);
  EXPECT_FALSE(m.setSun(bad));
  const auto steps = m.history().undoCount();
  geom::SunSettings osaka = tokyo(8, 1, 15.5);
  osaka.latitude = 34.69;
  osaka.longitude = 135.50;
  osaka.northAngle = 12.0;
  ASSERT_TRUE(m.setSun(osaka));
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  EXPECT_EQ(m.document().settings().sun, osaka);
  const std::string path = ::testing::TempDir() + "sun.sbr";
  ASSERT_TRUE(m.save(path));
  doc::Modeler back;
  ASSERT_TRUE(back.load(path));
  EXPECT_EQ(back.document().settings().sun, osaka);
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.document().settings().sun, geom::SunSettings{});

  api::CommandApi a(m);
  auto call = [&](const std::string& command, nlohmann::json args) {
    return nlohmann::json::parse(a.execute(nlohmann::json{{"command", command}, {"args", args}}.dump()));
  };
  auto set = call("set_sun", {{"month", 12}, {"day", 21}, {"hour", 12.0}});
  ASSERT_TRUE(set.value("ok", false)) << set.dump();
  EXPECT_EQ(m.document().settings().sun.month, 12);
  EXPECT_NEAR(set.at("result").at("altitude").get<double>(), 30.8, 1.0);
  auto get = call("get_sun", nlohmann::json::object());
  ASSERT_TRUE(get.value("ok", false)) << get.dump();
  EXPECT_EQ(get.at("result").at("day").get<int>(), 21);
  EXPECT_TRUE(get.at("result").at("up").get<bool>());
  EXPECT_FALSE(call("set_sun", {{"hour", 25.0}}).value("ok", true));
  EXPECT_EQ(a.effectOf(R"({"command":"get_sun","args":{}})"), api::CommandApi::Effect::Read);
}
