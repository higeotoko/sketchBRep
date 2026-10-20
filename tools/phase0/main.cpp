// Phase 0 feasibility check: rectangle -> solid -> validate -> STEP -> reload.
//
// Usage: sbr_phase0 [width height depth] [output.step]

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/Profile.h"
#include "sbr/geom/ShapeIO.h"
#include "sbr/geom/Validation.h"

using namespace sbr;

namespace {

bool check(bool condition, const char* what) {
  std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", what);
  return condition;
}

}  // namespace

int main(int argc, char** argv) {
  double w = 1000.0, h = 2000.0, d = 3000.0;
  std::string out = "phase0_box.step";
  if (argc >= 4) {
    w = std::atof(argv[1]);
    h = std::atof(argv[2]);
    d = std::atof(argv[3]);
  }
  if (argc == 2) out = argv[1];
  if (argc >= 5) out = argv[4];

  std::printf("SketchBRep Phase 0: box %.3f x %.3f x %.3f mm -> %s\n", w, h, d, out.c_str());
  bool ok = true;

  const auto face = geom::makeRectangleFace(geom::SketchPlane::ground(), w, h);
  ok &= check(face.ok(), "rectangle profile face");
  if (!face) {
    std::printf("    %s\n", face.error().c_str());
    return 1;
  }

  const auto solid = geom::extrudeFace(face.value(), d);
  ok &= check(solid.ok(), "extrude to solid");
  if (!solid) {
    std::printf("    %s\n", solid.error().c_str());
    return 1;
  }

  const geom::ValidationReport report = geom::validateShape(solid.value(), true);
  std::printf("    %s\n", report.summary().c_str());
  ok &= check(report.valid, "B-rep validity (BRepCheck_Analyzer, closed shell)");
  ok &= check(report.faces == 6 && report.edges == 12 && report.vertices == 8,
              "topology 6 faces / 12 edges / 8 vertices");

  const double expected = std::abs(w * h * d);
  const double vol = geom::volume(solid.value());
  std::printf("    volume = %.9f mm3 (expected %.9f)\n", vol, expected);
  ok &= check(std::abs(vol - expected) <= expected * 1e-12, "exact volume (rel. 1e-12)");

  const Status written = geom::writeStep(solid.value(), out);
  ok &= check(written.ok(), "STEP export");
  if (!written) {
    std::printf("    %s\n", written.message().c_str());
    return 1;
  }

  const auto reread = geom::readStep(out);
  ok &= check(reread.ok(), "STEP re-import");
  if (!reread) {
    std::printf("    %s\n", reread.error().c_str());
    return 1;
  }
  const geom::ValidationReport report2 = geom::validateShape(reread.value(), true);
  std::printf("    %s\n", report2.summary().c_str());
  ok &= check(report2.valid, "re-imported solid is valid");

  const double vol2 = geom::volume(reread.value());
  std::printf("    re-imported volume = %.9f mm3 (diff %.3e)\n", vol2, vol2 - expected);
  ok &= check(std::abs(vol2 - expected) <= expected * 1e-9, "volume preserved (rel. 1e-9)");

  const Bnd_Box box = geom::exactBounds(reread.value());
  const gp_Pnt mn = box.CornerMin();
  const gp_Pnt mx = box.CornerMax();
  std::printf("    bounds = (%.6f, %.6f, %.6f) - (%.6f, %.6f, %.6f)\n", mn.X(), mn.Y(), mn.Z(),
              mx.X(), mx.Y(), mx.Z());
  const double tol = 1e-6;
  ok &= check(std::abs(mx.X() - mn.X() - std::abs(w)) < tol &&
                  std::abs(mx.Y() - mn.Y() - std::abs(h)) < tol &&
                  std::abs(mx.Z() - mn.Z() - std::abs(d)) < tol,
              "dimensions preserved (1e-6 mm)");

  std::printf("%s\n", ok ? "PHASE 0: PASS" : "PHASE 0: FAIL");
  return ok ? 0 : 1;
}
