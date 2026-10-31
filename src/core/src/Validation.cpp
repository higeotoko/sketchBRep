#include "sbr/geom/Validation.h"

#include <sstream>

#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Shell.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS.hxx>

#include "sbr/Types.h"
#include "sbr/geom/Measure.h"

namespace sbr::geom {

std::string ValidationReport::summary() const {
  std::ostringstream out;
  out << (valid ? "valid" : "INVALID") << " solids=" << solids << " shells=" << shells
      << " faces=" << faces << " edges=" << edges << " vertices=" << vertices
      << " closed=" << (closed ? "yes" : "no") << " free_edges=" << freeEdges << " volume=" << volume;
  for (const std::string& issue : issues) out << "; " << issue;
  return out.str();
}

namespace {
int countOf(const TopoDS_Shape& shape, TopAbs_ShapeEnum type) {
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(shape, type, map);
  return map.Extent();
}
}  // namespace

ValidationReport validateShape(const TopoDS_Shape& shape, bool expectSolid, bool exactVolume) {
  ValidationReport report;
  if (shape.IsNull()) {
    report.issues.push_back("shape is null");
    return report;
  }
  report.solids = countOf(shape, TopAbs_SOLID);
  report.shells = countOf(shape, TopAbs_SHELL);
  report.faces = countOf(shape, TopAbs_FACE);
  report.edges = countOf(shape, TopAbs_EDGE);
  report.vertices = countOf(shape, TopAbs_VERTEX);

  bool ok = true;
  BRepCheck_Analyzer analyzer(shape);
  if (!analyzer.IsValid()) {
    ok = false;
    report.issues.push_back("BRepCheck_Analyzer reports errors");
  }

  report.closed = report.shells > 0;
  for (TopExp_Explorer it(shape, TopAbs_SHELL); it.More(); it.Next()) {
    BRepCheck_Shell shellCheck(TopoDS::Shell(it.Current()));
    if (shellCheck.Closed() != BRepCheck_NoError) report.closed = false;
  }

  if (report.solids > 0) report.volume = exactVolume ? volume(shape) : roughVolume(shape);
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
  for (int i = 1; i <= edgeFaces.Extent(); ++i) {
    if (edgeFaces(i).Extent() == 1 && !BRep_Tool::Degenerated(TopoDS::Edge(edgeFaces.FindKey(i)))) {
      ++report.freeEdges;
    }
  }
  report.drawingValid = analyzer.IsValid();
  report.solidValid = report.drawingValid && report.solids == 1 && report.closed &&
                      report.volume > tolerance::kMinDimension * tolerance::kMinDimension * tolerance::kMinDimension;

  if (expectSolid) {
    if (report.solids != 1) {
      ok = false;
      report.issues.push_back("expected exactly one solid");
    }
    if (!report.closed) {
      ok = false;
      report.issues.push_back("shell is not closed");
    }
    const double minVolume =
        tolerance::kMinDimension * tolerance::kMinDimension * tolerance::kMinDimension;
    if (report.volume <= minVolume) {
      ok = false;
      report.issues.push_back("volume is not positive");
    }
  }
  report.valid = ok;
  return report;
}

}  // namespace sbr::geom
