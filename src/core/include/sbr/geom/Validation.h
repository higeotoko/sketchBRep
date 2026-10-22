#pragma once
// B-rep validity checks. Invalid solids must never be reported as success.

#include <string>
#include <vector>

#include <TopoDS_Shape.hxx>

namespace sbr::geom {

struct ValidationReport {
  bool valid = false;
  int solids = 0;
  int shells = 0;
  int faces = 0;
  int edges = 0;
  int vertices = 0;
  bool closed = false;  // every edge shared by exactly two faces
  double volume = 0.0;
  // Two levels, whatever was expected: sound as drawing geometry (B-rep
  // topology and geometry check out; open shapes may be) and sound as a
  // solid (that, plus one closed solid with positive volume).
  bool drawingValid = false;
  bool solidValid = false;
  int freeEdges = 0;  // edges on an open border (one face only)
  std::vector<std::string> issues;

  std::string summary() const;
};

// Runs BRepCheck_Analyzer and topological sanity checks. When `expectSolid`
// is set, the shape must consist of exactly one closed solid with positive
// volume.
ValidationReport validateShape(const TopoDS_Shape& shape, bool expectSolid);

}  // namespace sbr::geom
