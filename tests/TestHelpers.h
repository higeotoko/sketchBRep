#pragma once

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "sbr/geom/Measure.h"

namespace sbr::test {

inline std::string tempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() / ("sbr_test_" + name)).string();
}

inline void expectBounds(const TopoDS_Shape& shape, double x0, double y0, double z0, double x1,
                         double y1, double z1, double tol = 1e-6) {
  const Bnd_Box box = geom::exactBounds(shape);
  ASSERT_FALSE(box.IsVoid());
  EXPECT_NEAR(box.CornerMin().X(), x0, tol);
  EXPECT_NEAR(box.CornerMin().Y(), y0, tol);
  EXPECT_NEAR(box.CornerMin().Z(), z0, tol);
  EXPECT_NEAR(box.CornerMax().X(), x1, tol);
  EXPECT_NEAR(box.CornerMax().Y(), y1, tol);
  EXPECT_NEAR(box.CornerMax().Z(), z1, tol);
}

}  // namespace sbr::test
