#include "sbr/geom/ShapeIO.h"

#include <locale>
#include <sstream>

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDF_ChildIterator.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
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

namespace {

Handle(TDocStd_Document) newXcafDocument() {
  Handle(TDocStd_Document) document;
  XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", document);
  return document;
}

std::string labelName(const TDF_Label& label) {
  Handle(TDataStd_Name) attr;
  if (!label.FindAttribute(TDataStd_Name::GetID(), attr)) return {};
  const TCollection_ExtendedString& ext = attr->Get();
  std::string out(static_cast<std::size_t>(ext.LengthOfCString()), '\0');
  Standard_PCharacter buffer = out.data();
  ext.ToUTF8CString(buffer);
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

std::optional<std::array<double, 3>> labelColor(const Handle(XCAFDoc_ColorTool)& colors,
                                                const TDF_Label& label,
                                                const TopoDS_Shape& shape) {
  Quantity_Color c;
  if (colors->GetColor(label, XCAFDoc_ColorSurf, c) || colors->GetColor(label, XCAFDoc_ColorGen, c) ||
      colors->GetColor(shape, XCAFDoc_ColorSurf, c) || colors->GetColor(shape, XCAFDoc_ColorGen, c)) {
    double r, g, b;
    c.Values(r, g, b, Quantity_TOC_RGB);
    return std::array<double, 3>{r, g, b};
  }
  return std::nullopt;
}

void collectParts(const Handle(XCAFDoc_ShapeTool)& shapes, const Handle(XCAFDoc_ColorTool)& colors,
                  const TDF_Label& label, const TopLoc_Location& location,
                  const std::string& inheritedName,
                  std::optional<std::array<double, 3>> inheritedColor,
                  std::vector<NamedShape>& out) {
  std::string name = labelName(label);
  if (name.empty()) name = inheritedName;
  TopoDS_Shape own = XCAFDoc_ShapeTool::GetShape(label);
  auto color = labelColor(colors, label, own);
  if (!color) color = inheritedColor;

  if (XCAFDoc_ShapeTool::IsAssembly(label)) {
    TDF_LabelSequence components;
    XCAFDoc_ShapeTool::GetComponents(label, components);
    for (int i = 1; i <= components.Length(); ++i) {
      const TDF_Label component = components.Value(i);
      TDF_Label referred;
      if (!XCAFDoc_ShapeTool::GetReferredShape(component, referred)) continue;
      const std::string componentName = labelName(component);
      const auto componentColor = labelColor(colors, component, TopoDS_Shape());
      collectParts(shapes, colors, referred, location * XCAFDoc_ShapeTool::GetLocation(component),
                   componentName.empty() ? name : componentName,
                   componentColor ? componentColor : color, out);
    }
    return;
  }
  if (own.IsNull()) return;
  const TopoDS_Shape placed = location.IsIdentity() ? own : own.Moved(location);
  std::vector<TopoDS_Shape> solids;
  for (TopExp_Explorer it(placed, TopAbs_SOLID); it.More(); it.Next()) solids.push_back(it.Current());
  if (solids.empty()) {
    out.push_back({placed, name, color});
    return;
  }
  for (std::size_t i = 0; i < solids.size(); ++i) {
    const std::string partName =
        solids.size() == 1 ? name : name + (name.empty() ? "" : " ") + std::to_string(i + 1);
    out.push_back({solids[i], partName, color});
  }
}

}  // namespace

Status writeStepParts(const std::vector<NamedShape>& parts, const std::string& path) {
  if (parts.empty()) return Status::failure("Nothing to export");
  try {
    Handle(TDocStd_Document) document = newXcafDocument();
    Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    Handle(XCAFDoc_ColorTool) colors = XCAFDoc_DocumentTool::ColorTool(document->Main());
    for (const NamedShape& part : parts) {
      if (part.shape.IsNull()) continue;
      // Wrap located shapes so that the placement is kept as geometry of a
      // plain product instead of turning into an assembly instance.
      TopoDS_Shape shape = part.shape;
      if (!shape.Location().IsIdentity()) {
        TopoDS_Compound wrapper;
        BRep_Builder builder;
        builder.MakeCompound(wrapper);
        builder.Add(wrapper, shape);
        shape = wrapper;
      }
      const TDF_Label label = shapes->AddShape(shape, Standard_False);
      TDataStd_Name::Set(label, TCollection_ExtendedString(part.name.c_str(), Standard_True));
      if (part.color) {
        colors->SetColor(label,
                         Quantity_Color((*part.color)[0], (*part.color)[1], (*part.color)[2],
                                        Quantity_TOC_RGB),
                         XCAFDoc_ColorGen);
      }
    }
    STEPCAFControl_Writer writer;
    writer.SetNameMode(Standard_True);
    writer.SetColorMode(Standard_True);
    Interface_Static::SetCVal("write.step.unit", "MM");
    Interface_Static::SetCVal("write.step.schema", "AP214IS");
    if (!writer.Transfer(document, STEPControl_AsIs)) return Status::failure("STEP transfer failed");
    if (writer.Write(path.c_str()) != IFSelect_RetDone) {
      return Status::failure("Could not write STEP file: " + path);
    }
    return Status::success();
  } catch (const Standard_Failure& e) {
    return Status::failure(std::string("STEP export failed: ") + e.GetMessageString());
  }
}

Result<std::vector<NamedShape>> readStepParts(const std::string& path) {
  using R = Result<std::vector<NamedShape>>;
  try {
    STEPCAFControl_Reader reader;
    reader.SetNameMode(Standard_True);
    reader.SetColorMode(Standard_True);
    Interface_Static::SetCVal("xstep.cascade.unit", "MM");
    if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) {
      return R::failure("Could not read STEP file: " + path);
    }
    Handle(TDocStd_Document) document = newXcafDocument();
    if (!reader.Transfer(document)) return R::failure("STEP transfer failed");
    Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    Handle(XCAFDoc_ColorTool) colors = XCAFDoc_DocumentTool::ColorTool(document->Main());
    TDF_LabelSequence roots;
    shapes->GetFreeShapes(roots);
    std::vector<NamedShape> parts;
    for (int i = 1; i <= roots.Length(); ++i) {
      collectParts(shapes, colors, roots.Value(i), TopLoc_Location(), std::string(), std::nullopt,
                   parts);
    }
    if (parts.empty()) return R::failure("STEP file contains no shapes");
    return parts;
  } catch (const Standard_Failure& e) {
    return R::failure(std::string("STEP import failed: ") + e.GetMessageString());
  }
}

}  // namespace sbr::geom
