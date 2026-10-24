#include <gtest/gtest.h>

#include "sbr/Units.h"

using namespace sbr::units;

TEST(Units, ParsesPlainMillimetres) {
  EXPECT_DOUBLE_EQ(*parseLength("3000"), 3000.0);
  EXPECT_DOUBLE_EQ(*parseLength("  12.5 "), 12.5);
  EXPECT_DOUBLE_EQ(*parseLength("-450"), -450.0);
  EXPECT_DOUBLE_EQ(*parseLength("250mm"), 250.0);
}

TEST(Units, ParsesUnitSuffixes) {
  EXPECT_DOUBLE_EQ(*parseLength("4.5m"), 4500.0);
  EXPECT_DOUBLE_EQ(*parseLength("30 cm"), 300.0);
  EXPECT_DOUBLE_EQ(*parseLength("1E3"), 1000.0);
}

TEST(Units, RejectsGarbage) {
  EXPECT_FALSE(parseLength(""));
  EXPECT_FALSE(parseLength("abc"));
  EXPECT_FALSE(parseLength("12yd"));  // (feet and inches are read since W8)
  EXPECT_FALSE(parseLength("1.2.3"));
}

TEST(Units, ParsesLists) {
  const auto values = parseLengthList("3000, 4.5m");
  ASSERT_TRUE(values);
  ASSERT_EQ(values->size(), 2u);
  EXPECT_DOUBLE_EQ((*values)[0], 3000.0);
  EXPECT_DOUBLE_EQ((*values)[1], 4500.0);
  EXPECT_FALSE(parseLengthList("3000,"));
}

TEST(Units, FormatsIndependentlyOfModelPrecision) {
  EXPECT_EQ(formatLength(3000.04, 1), "3000.0 mm");
  EXPECT_EQ(formatLength(1234.5678, 2), "1234.57 mm");
}

TEST(Units, SpacingListsRepeatBays) {
  using sbr::units::parseSpacingList;
  auto v = parseSpacingList("3600*3, 2.7m");
  ASSERT_TRUE(v);
  EXPECT_EQ(*v, (std::vector<double>{3600, 3600, 3600, 2700}));
  v = parseSpacingList("910x2");
  ASSERT_TRUE(v);
  EXPECT_EQ(*v, (std::vector<double>{910, 910}));
  ASSERT_TRUE(parseSpacingList("  "));
  EXPECT_TRUE(parseSpacingList("")->empty());
  EXPECT_FALSE(parseSpacingList("3600*0"));
  EXPECT_FALSE(parseSpacingList("3600*a"));
  EXPECT_FALSE(parseSpacingList("3600,,"));
}

// NUM-02: one side only; an empty item keeps that value.
TEST(Units, LengthSlotsMayBeEmpty) {
  auto a = parseLengthSlots("3000,");
  ASSERT_TRUE(a);
  ASSERT_EQ(a->size(), 2u);
  EXPECT_DOUBLE_EQ(*(*a)[0], 3000.0);
  EXPECT_FALSE((*a)[1]);
  auto b = parseLengthSlots(" , 1.5m");
  ASSERT_TRUE(b);
  EXPECT_FALSE((*b)[0]);
  EXPECT_DOUBLE_EQ(*(*b)[1], 1500.0);
  auto c = parseLengthSlots("-300cm,1000");
  ASSERT_TRUE(c);
  EXPECT_DOUBLE_EQ(*(*c)[0], -3000.0);
  EXPECT_FALSE(parseLengthSlots(","));        // nothing given
  EXPECT_FALSE(parseLengthSlots("3000,abc"));  // garbage is not "empty"
  EXPECT_FALSE(parseLengthSlots(""));
}

// W8: inches and feet as in SketchUp - 12", 12in, 3', 3ft, 3'6", 3' 6",
// 3'6 (inches), -3'6" (all negative); still refused: nonsense after them.
TEST(Units, InchesAndFeet) {
  EXPECT_NEAR(*parseLength("12\""), 304.8, 1e-9);
  EXPECT_NEAR(*parseLength("12in"), 304.8, 1e-9);
  EXPECT_NEAR(*parseLength("3'"), 914.4, 1e-9);
  EXPECT_NEAR(*parseLength("3ft"), 914.4, 1e-9);
  EXPECT_NEAR(*parseLength("3'6\""), 1066.8, 1e-9);
  EXPECT_NEAR(*parseLength("3' 6\""), 1066.8, 1e-9);
  EXPECT_NEAR(*parseLength("3'6"), 1066.8, 1e-9);
  EXPECT_NEAR(*parseLength("3'6.5\""), 1079.5, 1e-9);
  EXPECT_NEAR(*parseLength("-3'6\""), -1066.8, 1e-9);
  EXPECT_FALSE(parseLength("3'x").has_value());
  EXPECT_FALSE(parseLength("3'-6\"").has_value());
  EXPECT_FALSE(parseLength("'6\"").has_value());
  EXPECT_NEAR(*parseLength("2000"), 2000, 1e-9);  // mm as before
}
