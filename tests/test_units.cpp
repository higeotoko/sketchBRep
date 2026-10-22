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
  EXPECT_FALSE(parseLength("12ft"));
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
