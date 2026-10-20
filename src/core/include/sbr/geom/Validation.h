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
  std::vector<std::string> issues;

  std::string summary() const;
};

// Runs BRepCheck_Analyzer and topological sanity checks. When `expectSolid`
// is set, the shape must consist of exactly one closed solid with positive
// volume.
ValidationReport validateShape(const TopoDS_Shape& shape, bool expectSolid);

}  // namespace sbr::geom
