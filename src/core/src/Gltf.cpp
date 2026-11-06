#include "sbr/geom/Gltf.h"

#include <algorithm>
#include <map>

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <Message_ProgressRange.hxx>
#include <RWGltf_CafWriter.hxx>
#include <RWMesh_CoordinateSystem.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_IndexedDataMapOfStringString.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_VisMaterial.hxx>
#include <XCAFDoc_VisMaterialTool.hxx>

#include "sbr/KernelError.h"

namespace sbr::geom {

Status writeGltf(const std::vector<GltfPart>& parts, const std::string& path, const GltfExportOptions& options) {
  if (parts.empty()) return Status::failure("Nothing to export");
  std::string ext = path.size() >= 5 ? path.substr(path.size() - 5) : std::string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const bool binary = ext == ".gltf" ? false : true;
  if (ext != ".gltf" && (path.size() < 4 || [&] {
        std::string e = path.substr(path.size() - 4);
        std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return e != ".glb";
      }())) {
    return Status::failure("glTF files end in .glb or .gltf");
  }
  try {
    Handle(TDocStd_Document) document;
    XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", document);
    Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    Handle(XCAFDoc_VisMaterialTool) materials = XCAFDoc_DocumentTool::VisMaterialTool(document->Main());
    std::map<std::string, TDF_Label> made;  // material name -> label
    int count = 0;
    for (const GltfPart& part : parts) {
      if (part.shape.IsNull()) continue;
      // Meshed on a copy: the model's shapes keep no triangulation from here.
      TopoDS_Shape shape = BRepBuilderAPI_Copy(part.shape, Standard_True, Standard_False).Shape();
      BRepMesh_IncrementalMesh(shape, options.linearDeflection, Standard_False, options.angularDeflection, Standard_True);
      if (!shape.Location().IsIdentity()) {  // a plain mesh, not an instance
        TopoDS_Compound wrapper;
        BRep_Builder builder;
        builder.MakeCompound(wrapper);
        builder.Add(wrapper, shape);
        shape = wrapper;
      }
      const TDF_Label label = shapes->AddShape(shape, Standard_False);
      TDataStd_Name::Set(label, TCollection_ExtendedString(part.name.c_str(), Standard_True));
      const std::string key = part.materialName.empty() ? "SketchBRep " + std::to_string(++count) : part.materialName;
      auto found = made.find(key);
      if (found == made.end()) {
        Handle(XCAFDoc_VisMaterial) material = new XCAFDoc_VisMaterial();
        XCAFDoc_VisMaterialPBR pbr;
        pbr.BaseColor = Quantity_ColorRGBA(Quantity_Color(part.color[0], part.color[1], part.color[2], Quantity_TOC_RGB),
                                           static_cast<float>(part.color[3]));
        pbr.Metallic = static_cast<float>(std::clamp(part.metallic, 0.0, 1.0));
        pbr.Roughness = static_cast<float>(std::clamp(part.roughness, 0.0, 1.0));
        pbr.IsDefined = Standard_True;
        material->SetPbrMaterial(pbr);
        material->SetRawName(new TCollection_HAsciiString(key.c_str()));  // the name the renderer shows
        if (part.color[3] < 1.0) material->SetAlphaMode(Graphic3d_AlphaMode_Blend, 0.5f);
        found = made.emplace(key, materials->AddMaterial(material, TCollection_AsciiString(key.c_str()))).first;
      }
      materials->SetShapeMaterial(label, found->second);
    }
    RWGltf_CafWriter writer(TCollection_AsciiString(path.c_str()), binary ? Standard_True : Standard_False);
    // The model: mm, Z up. glTF: metres, Y up (the writer turns it).
    writer.ChangeCoordinateSystemConverter().SetInputLengthUnit(0.001);
    writer.ChangeCoordinateSystemConverter().SetInputCoordinateSystem(RWMesh_CoordinateSystem_Zup);
    writer.SetMergeFaces(true);  // one primitive per object: fewer draw calls in the renderer
    TColStd_IndexedDataMapOfStringString info;
    info.Add("Author", "SketchBRep");
    if (!writer.Perform(document, info, Message_ProgressRange())) return Status::failure("Could not write " + path);
    return Status::success();
  } catch (const Standard_Failure& e) {
    return Status::failure(std::string("glTF export failed: ") + sbr::kernelMessage(e));
  }
}

}  // namespace sbr::geom
