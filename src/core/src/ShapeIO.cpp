#include "sbr/geom/ShapeIO.h"

#include <locale>
#include <sstream>

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_CLocaleSentry.hxx>
#include <Standard_Failure.hxx>

namespace sbr::geom {

std::string shapeToBRepString(const TopoDS_Shape& shape) {
  Standard_CLocaleSentry localeSentry;
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  // Geometry only: triangulations belong to the display layer.
  BRepTools::Write(shape, stream, Standard_False, Standard_False, TopTools_FormatVersion_CURRENT);
  return stream.str();
}

Result<TopoDS_Shape> shapeFromBRepString(const std::string& text) {
  Standard_CLocaleSentry localeSentry;
  try {
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    TopoDS_Shape shape;
    BRep_Builder builder;
    BRepTools::Read(shape, stream, builder);
    if (shape.IsNull()) return Result<TopoDS_Shape>::failure("Empty or unreadable BRep data");
    return shape;
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("BRep read failed: ") + e.GetMessageString());
  }
}

Status writeStep(const TopoDS_Shape& shape, const std::string& path) {
  if (shape.IsNull()) return Status::failure("Nothing to export");
  try {
    STEPControl_Writer writer;
    Interface_Static::SetCVal("write.step.unit", "MM");
    Interface_Static::SetCVal("write.step.schema", "AP214IS");
    if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone) {
      return Status::failure("STEP transfer failed");
    }
    if (writer.Write(path.c_str()) != IFSelect_RetDone) {
      return Status::failure("Could not write STEP file: " + path);
    }
    return Status::success();
  } catch (const Standard_Failure& e) {
    return Status::failure(std::string("STEP export failed: ") + e.GetMessageString());
  }
}

Result<TopoDS_Shape> readStep(const std::string& path) {
  try {
    STEPControl_Reader reader;
    Interface_Static::SetCVal("xstep.cascade.unit", "MM");
    if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) {
      return Result<TopoDS_Shape>::failure("Could not read STEP file: " + path);
    }
    if (reader.TransferRoots() == 0) {
      return Result<TopoDS_Shape>::failure("STEP file contains no transferable shapes");
    }
    TopoDS_Shape shape = reader.OneShape();
    if (shape.IsNull()) return Result<TopoDS_Shape>::failure("STEP file produced an empty shape");
    return shape;
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("STEP import failed: ") +
                                         e.GetMessageString());
  }
}

}  // namespace sbr::geom
