// SketchUp (.skp) export and import (docs/181). See Skp.h.
#include "sbr/geom/Skp.h"

#include <cstdio>
#include <sstream>

#ifdef SBR_WITH_SKETCHUP

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <tuple>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <ShapeFix_Face.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

#include <SketchUpAPI/sketchup.h>

#endif  // SBR_WITH_SKETCHUP

namespace sbr::geom {

std::string SkpReport::summary() const {
  std::ostringstream s;
  s << parts << " object(s) from " << faces << " face(s)";
  if (lines) s << " and " << lines << " loose line(s)";
  s << "; " << groups << " group(s), " << instances << " component instance(s)";
  if (solids) s << "; " << solids << " closed into solids";
  if (leftOut) s << "; " << leftOut << " face(s) left out (too small or not flat)";
  if (hidden) s << "; " << hidden << " hidden item(s) left out";
  if (textured) s << "; " << textured << " textured material(s) taken as their colour";
  return s.str();
}

#ifndef SBR_WITH_SKETCHUP

bool skpAvailable(std::string* why) {
  if (why) *why = "This build of SketchBRep has no SketchUp support (built without the SketchUp SDK)";
  return false;
}

Status writeSkp(const std::vector<ColladaPart>&, const std::string&, const SkpExportOptions&) {
  std::string why;
  skpAvailable(&why);
  return Status::failure(why);
}

Result<std::vector<ColladaPart>> readSkp(const std::string&, SkpReport*) {
  std::string why;
  skpAvailable(&why);
  return Result<std::vector<ColladaPart>>::failure(why);
}

#else

namespace {

// --- the SketchUp C API, loaded at run time (never linked: the ZIP carries no SDK) -------------
#define SBR_SU_FUNCTIONS(X)                                                                         \
  X(SUInitialize) X(SUGetAPIVersion) X(SUModelCreate) X(SUModelCreateFromFileWithStatus)              \
  X(SUModelSaveToFileWithVersion) X(SUModelRelease) X(SUModelGetEntities) X(SUModelAddMaterials)      \
  X(SUEntitiesFill) X(SUEntitiesAddGroup) X(SUEntitiesGetNumGroups) X(SUEntitiesGetGroups)            \
  X(SUEntitiesGetNumInstances) X(SUEntitiesGetInstances) X(SUEntitiesGetNumFaces) X(SUEntitiesGetFaces) \
  X(SUEntitiesGetNumEdges) X(SUEntitiesGetEdges) X(SUGroupCreate) X(SUGroupSetName) X(SUGroupGetName) \
  X(SUGroupGetEntities) X(SUGroupGetTransform) X(SUGroupToDrawingElement)                             \
  X(SUComponentInstanceGetTransform) X(SUComponentInstanceGetDefinition) X(SUComponentInstanceGetName) \
  X(SUComponentInstanceToDrawingElement) X(SUComponentDefinitionGetEntities)                          \
  X(SUComponentDefinitionGetName) X(SUFaceToDrawingElement) X(SUFaceGetOuterLoop)                     \
  X(SUFaceGetNumInnerLoops) X(SUFaceGetInnerLoops) X(SUFaceGetFrontMaterial) X(SULoopGetNumVertices)  \
  X(SULoopGetVertices) X(SUVertexGetPosition) X(SUEdgeGetStartVertex) X(SUEdgeGetEndVertex)          \
  X(SUEdgeToDrawingElement) X(SUDrawingElementGetMaterial) X(SUDrawingElementSetMaterial)             \
  X(SUDrawingElementGetHidden) X(SUGeometryInputCreate) X(SUGeometryInputRelease)                     \
  X(SUGeometryInputSetVertices) X(SUGeometryInputAddFace) X(SUGeometryInputFaceAddInnerLoop)          \
  X(SUGeometryInputFaceSetFrontMaterial) X(SUGeometryInputAddEdge) X(SULoopInputCreate)               \
  X(SULoopInputAddVertexIndex) X(SULoopInputEdgeSetSoft) X(SULoopInputEdgeSetSmooth)                  \
  X(SUMaterialCreate) X(SUMaterialSetName) X(SUMaterialSetColor) X(SUMaterialSetOpacity)              \
  X(SUMaterialSetUseOpacity) X(SUMaterialGetColor) X(SUMaterialGetOpacity) X(SUMaterialGetUseOpacity) \
  X(SUMaterialGetTexture) X(SUStringCreate) X(SUStringRelease) X(SUStringGetUTF8Length) X(SUStringGetUTF8)

struct Api {
#define SBR_SU_POINTER(f) decltype(&::f) f = nullptr;
  SBR_SU_FUNCTIONS(SBR_SU_POINTER)
#undef SBR_SU_POINTER
};

std::wstring wide(const std::string& s) { return std::filesystem::u8path(s).wstring(); }

// The folder holding SketchUpAPI.dll: SBR_SKETCHUP_SDK (the SDK folder or that folder itself),
// else the SDK the build was made with.
std::vector<std::wstring> dllFolders() {
  std::vector<std::wstring> out;
  auto add = [&](std::wstring root) {
    if (root.empty()) return;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    out.push_back(root + L"\\binaries\\sketchup\\x64");
    out.push_back(root);
  };
  if (const wchar_t* env = _wgetenv(L"SBR_SKETCHUP_SDK")) add(env);
#ifdef SBR_SKETCHUP_SDK_DIR
  add(wide(SBR_SKETCHUP_SDK_DIR));
#endif
  return out;
}

const Api* api(std::string* why) {
  static Api a;
  static std::string error;
  static bool ok = false;
  static std::once_flag once;
  std::call_once(once, [] {
    HMODULE module = nullptr;
    for (const std::wstring& folder : dllFolders()) {
      const std::wstring dll = folder + L"\\SketchUpAPI.dll";
      if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
      module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      if (module) break;
      error = "SketchUpAPI.dll was found but could not be loaded (Windows error " + std::to_string(GetLastError()) + ")";
    }
    if (!module) {
      if (error.empty())
        error = "The SketchUp SDK was not found: set SBR_SKETCHUP_SDK to the SDK folder (the one with binaries\\sketchup\\x64)";
      return;
    }
    bool all = true;
#define SBR_SU_LOAD(f)                                                                  \
  a.f = reinterpret_cast<decltype(a.f)>(reinterpret_cast<void*>(GetProcAddress(module, #f))); \
  if (!a.f && all) {                                                                    \
    all = false;                                                                        \
    error = std::string("The SketchUp SDK on this PC has no ") + #f + " (too old a version)"; \
  }
    SBR_SU_FUNCTIONS(SBR_SU_LOAD)
#undef SBR_SU_LOAD
    if (!all) return;
    a.SUInitialize();
    ok = true;
  });
  if (!ok) {
    if (why) *why = error;
    return nullptr;
  }
  return &a;
}

std::string text(const Api& su, SUStringRef s) {
  size_t length = 0;
  if (su.SUStringGetUTF8Length(s, &length) != SU_ERROR_NONE || length == 0) return {};
  std::string out(length + 1, '\0');
  size_t copied = 0;
  su.SUStringGetUTF8(s, out.size(), out.data(), &copied);
  out.resize(copied);
  return out;
}

constexpr double kInch = 25.4;  // mm in SketchUp's internal unit

// --- export ------------------------------------------------------------------------------------

struct Writer {
  const Api& su;
  SUModelRef model = SU_INVALID;
  std::map<ColladaColor, SUMaterialRef> materials;

  SUMaterialRef material(const ColladaColor& c) {
    const auto found = materials.find(c);
    if (found != materials.end()) return found->second;
    SUMaterialRef m = SU_INVALID;
    if (su.SUMaterialCreate(&m) != SU_ERROR_NONE) return m;
    auto byte = [](double v) { return static_cast<SUByte>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    char name[64];
    std::snprintf(name, sizeof name, "SBR_%02X%02X%02X%s", byte(c.r), byte(c.g), byte(c.b), c.a < 1.0 ? "_T" : "");
    su.SUMaterialSetName(m, name);
    const SUColor color{byte(c.r), byte(c.g), byte(c.b), byte(c.a)};
    su.SUMaterialSetColor(m, &color);
    if (c.a < 1.0) {
      su.SUMaterialSetOpacity(m, std::clamp(c.a, 0.0, 1.0));
      su.SUMaterialSetUseOpacity(m, true);
    }
    su.SUModelAddMaterials(model, 1, &m);
    materials.emplace(c, m);
    return m;
  }
};

// The points of a face's wire in order, from the mesh (shared with the neighbouring faces'
// triangles, so that SketchUp welds them), each edge as the face uses it.
std::vector<gp_Pnt> loopPoints(const TopoDS_Wire& wire, const TopoDS_Face& face, const Handle(Poly_Triangulation)& tri,
                               const TopLoc_Location& location) {
  std::vector<gp_Pnt> out;
  auto push = [&](const gp_Pnt& p) {
    if (out.empty() || out.back().Distance(p) > 1e-7) out.push_back(p);
  };
  for (BRepTools_WireExplorer e(wire, face); e.More(); e.Next()) {
    const TopoDS_Edge& edge = e.Current();
    std::vector<gp_Pnt> pts;
    TopLoc_Location edgeLocation;
    const Handle(Poly_PolygonOnTriangulation) polygon = tri.IsNull() ? nullptr : BRep_Tool::PolygonOnTriangulation(edge, tri, location);
    if (!polygon.IsNull()) {
      const gp_Trsf t = location.Transformation();
      for (int i = 1; i <= polygon->NbNodes(); ++i) pts.push_back(tri->Node(polygon->Node(i)).Transformed(t));
      // The polygon runs along the edge's own direction.
      if (edge.Orientation() == TopAbs_REVERSED) std::reverse(pts.begin(), pts.end());
    } else {
      BRepAdaptor_Curve curve(edge);
      GCPnts_TangentialDeflection d(curve, 0.26, 1.0);
      for (int i = 1; i <= d.NbPoints(); ++i) pts.push_back(d.Value(i));
      if (edge.Orientation() == TopAbs_REVERSED) std::reverse(pts.begin(), pts.end());
    }
    for (const gp_Pnt& p : pts) push(p);
  }
  if (out.size() > 1 && out.front().Distance(out.back()) < 1e-7) out.pop_back();
  return out;
}

gp_Vec newell(const std::vector<gp_Pnt>& pts) {
  gp_Vec n(0, 0, 0);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const gp_Pnt& a = pts[i];
    const gp_Pnt& b = pts[(i + 1) % pts.size()];
    n += gp_Vec((a.Y() - b.Y()) * (a.Z() + b.Z()), (a.Z() - b.Z()) * (a.X() + b.X()), (a.X() - b.X()) * (a.Y() + b.Y()));
  }
  return n;
}

// The SketchUp face geometry of one part, into `input`.
struct PartGeometry {
  std::vector<SUPoint3D> vertices;
  std::map<std::tuple<long long, long long, long long>, size_t> index;
  size_t vertex(const gp_Pnt& p) {
    auto key = std::make_tuple(std::llround(p.X() * 1e5), std::llround(p.Y() * 1e5), std::llround(p.Z() * 1e5));
    const auto found = index.find(key);
    if (found != index.end()) return found->second;
    vertices.push_back(SUPoint3D{p.X() / kInch, p.Y() / kInch, p.Z() / kInch});
    index.emplace(key, vertices.size() - 1);
    return vertices.size() - 1;
  }
};

}  // namespace

bool skpAvailable(std::string* why) { return api(why) != nullptr; }

Status writeSkp(const std::vector<ColladaPart>& parts, const std::string& path, const SkpExportOptions& options) {
  std::string why;
  const Api* loaded = api(&why);
  if (!loaded) return Status::failure(why);
  const Api& su = *loaded;
  Writer w{su};
  if (su.SUModelCreate(&w.model) != SU_ERROR_NONE) return Status::failure("SketchUp could not make a model");
  struct Release {
    const Api& su;
    SUModelRef& model;
    ~Release() {
      if (SUIsValid(model)) su.SUModelRelease(&model);
    }
  } release{su, w.model};
  SUEntitiesRef root = SU_INVALID;
  su.SUModelGetEntities(w.model, &root);
  int written = 0;
  for (const ColladaPart& part : parts) {
    if (part.shape.IsNull()) continue;
    // Faces from the mesh: flat ones whole (with their holes), curved ones as soft triangles.
    BRepMesh_IncrementalMesh mesh(part.shape, options.linearDeflection, Standard_False, options.angularDeflection, Standard_True);
    TopTools_IndexedMapOfShape faceMap;
    TopExp::MapShapes(part.shape, TopAbs_FACE, faceMap);
    std::map<int, ColladaColor> painted(part.faceColors.begin(), part.faceColors.end());
    // Some of the part's faces (by their index in it), and its loose edges, into `target`.
    auto fill = [&](SUEntitiesRef target, const std::vector<int>& faceIndices, bool withEdges) -> SUResult {
      SUGeometryInputRef input = SU_INVALID;
      if (su.SUGeometryInputCreate(&input) != SU_ERROR_NONE) return SU_ERROR_GENERIC;
      PartGeometry g;
      struct Face {
        std::vector<std::vector<size_t>> loops;  // outer, then holes
        std::vector<std::array<bool, 3>> soft;   // triangles: their soft edges (empty: a flat face)
        std::optional<ColladaColor> color;
      };
      std::vector<Face> faces;
      for (const int fi : faceIndices) {
        const TopoDS_Face& face = TopoDS::Face(faceMap(fi));
        TopLoc_Location location;
        const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, location);
        const auto colour = painted.count(fi) ? std::optional<ColladaColor>(painted.at(fi)) : std::nullopt;
        BRepAdaptor_Surface surface(face, Standard_False);
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        if (surface.GetType() == GeomAbs_Plane) {
          gp_Vec normal(surface.Plane().Axis().Direction());
          if (reversed) normal.Reverse();
          Face f;
          f.color = colour;
          const TopoDS_Wire outer = BRepTools::OuterWire(face);
          std::vector<std::vector<gp_Pnt>> loops;
          if (!outer.IsNull()) loops.push_back(loopPoints(outer, face, tri, location));
          for (TopExp_Explorer wire(face, TopAbs_WIRE); wire.More(); wire.Next())
            if (!wire.Current().IsSame(outer)) loops.push_back(loopPoints(TopoDS::Wire(wire.Current()), face, tri, location));
          bool good = !loops.empty() && loops.front().size() >= 3;
          for (std::size_t k = 0; k < loops.size() && good; ++k) {
            if (loops[k].size() < 3) {
              if (k == 0) good = false;
              continue;
            }
            // Outer loop anticlockwise about the face's normal, holes the other way.
            const bool along = newell(loops[k]).Dot(normal) > 0;
            if (along != (k == 0)) std::reverse(loops[k].begin(), loops[k].end());
            std::vector<size_t> idx;
            for (const gp_Pnt& p : loops[k]) idx.push_back(g.vertex(p));
            f.loops.push_back(std::move(idx));
          }
          if (good) {
            faces.push_back(std::move(f));
            continue;
          }
        }
        // Curved (or a flat face whose loops could not be read): its triangles.
        if (tri.IsNull()) continue;
        const gp_Trsf t = location.Transformation();
        std::map<std::pair<int, int>, int> uses;
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
          int n[3];
          tri->Triangle(i).Get(n[0], n[1], n[2]);
          for (int k = 0; k < 3; ++k) ++uses[std::minmax(n[k], n[(k + 1) % 3])];
        }
        Face f;
        f.color = colour;
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
          int n[3];
          tri->Triangle(i).Get(n[0], n[1], n[2]);
          if (reversed) std::swap(n[1], n[2]);
          std::vector<size_t> loop;
          std::array<bool, 3> soft{};
          for (int k = 0; k < 3; ++k) {
            loop.push_back(g.vertex(tri->Node(n[k]).Transformed(t)));
            soft[static_cast<std::size_t>(k)] = uses[std::minmax(n[k], n[(k + 1) % 3])] > 1;  // inside the face
          }
          if (loop[0] == loop[1] || loop[1] == loop[2] || loop[0] == loop[2]) continue;
          Face one;
          one.color = colour;
          one.loops.push_back(std::move(loop));
          one.soft.push_back(soft);
          faces.push_back(std::move(one));
        }
      }
      // Loose edges (not on any face).
      std::vector<std::pair<size_t, size_t>> edges;
      if (withEdges) {
        TopTools_IndexedMapOfShape onFaces;
        for (TopExp_Explorer fe(part.shape, TopAbs_FACE); fe.More(); fe.Next()) TopExp::MapShapes(fe.Current(), TopAbs_EDGE, onFaces);
        TopTools_IndexedMapOfShape all;
        TopExp::MapShapes(part.shape, TopAbs_EDGE, all);
        for (int i = 1; i <= all.Extent(); ++i) {
          if (onFaces.Contains(all(i))) continue;
          const TopoDS_Edge& edge = TopoDS::Edge(all(i));
          if (BRep_Tool::Degenerated(edge)) continue;
          BRepAdaptor_Curve curve(edge);
          GCPnts_TangentialDeflection d(curve, options.angularDeflection, options.linearDeflection);
          for (int k = 1; k < d.NbPoints(); ++k) {
            const size_t a = g.vertex(d.Value(k)), b = g.vertex(d.Value(k + 1));
            if (a != b) edges.emplace_back(a, b);
          }
        }
      }
      if (faces.empty() && edges.empty()) {
        su.SUGeometryInputRelease(&input);
        return SU_ERROR_NO_DATA;
      }
      su.SUGeometryInputSetVertices(input, g.vertices.size(), g.vertices.data());
      for (const Face& f : faces) {
        SULoopInputRef outer = SU_INVALID;
        su.SULoopInputCreate(&outer);
        for (const size_t v : f.loops.front()) su.SULoopInputAddVertexIndex(outer, v);
        if (!f.soft.empty())
          for (size_t k = 0; k < 3; ++k)
            if (f.soft.front()[k]) {
              su.SULoopInputEdgeSetSoft(outer, k, true);
              su.SULoopInputEdgeSetSmooth(outer, k, true);
            }
        size_t index = 0;
        if (su.SUGeometryInputAddFace(input, &outer, &index) != SU_ERROR_NONE) continue;
        for (std::size_t k = 1; k < f.loops.size(); ++k) {
          SULoopInputRef hole = SU_INVALID;
          su.SULoopInputCreate(&hole);
          for (const size_t v : f.loops[k]) su.SULoopInputAddVertexIndex(hole, v);
          su.SUGeometryInputFaceAddInnerLoop(input, index, &hole);
        }
        if (f.color) {
          SUMaterialInput m{};
          m.num_uv_coords = 0;
          m.material = w.material(*f.color);
          su.SUGeometryInputFaceSetFrontMaterial(input, index, &m);
        }
      }
      for (const auto& [a, b] : edges) {
        size_t index = 0;
        su.SUGeometryInputAddEdge(input, a, b, &index);
      }
      const SUResult filled = su.SUEntitiesFill(target, input, true);
      su.SUGeometryInputRelease(&input);
      return filled;
    };
    auto group = [&](SUEntitiesRef parent) {
      SUGroupRef g = SU_INVALID;
      su.SUGroupCreate(&g);
      su.SUEntitiesAddGroup(parent, g);
      if (!part.name.empty()) su.SUGroupSetName(g, part.name.c_str());
      return g;
    };
    const SUGroupRef outer = group(root);
    if (faceMap.Extent() > 0) su.SUDrawingElementSetMaterial(su.SUGroupToDrawingElement(outer), w.material(part.color));
    SUEntitiesRef inside = SU_INVALID;
    su.SUGroupGetEntities(outer, &inside);
    // Several solids apart (a stair's treads and strings): each in a group of its own inside the
    // part's group, with the same name, so that SketchUp does not join the faces they touch with.
    std::vector<int> rest;
    std::vector<std::vector<int>> solids;
    {
      std::vector<bool> taken(static_cast<std::size_t>(faceMap.Extent()) + 1, false);
      int count = 0;
      for (TopExp_Explorer s(part.shape, TopAbs_SOLID); s.More(); s.Next()) ++count;
      if (count > 1) {
        for (TopExp_Explorer s(part.shape, TopAbs_SOLID); s.More(); s.Next()) {
          std::vector<int> mine;
          for (TopExp_Explorer f(s.Current(), TopAbs_FACE); f.More(); f.Next()) {
            const int k = faceMap.FindIndex(f.Current());
            if (k > 0 && !taken[static_cast<std::size_t>(k)]) {
              taken[static_cast<std::size_t>(k)] = true;
              mine.push_back(k);
            }
          }
          if (!mine.empty()) solids.push_back(std::move(mine));
        }
      }
      for (int k = 1; k <= faceMap.Extent(); ++k)
        if (!taken[static_cast<std::size_t>(k)]) rest.push_back(k);
    }
    for (const std::vector<int>& solid : solids) {
      const SUGroupRef piece = group(inside);
      SUEntitiesRef pieceInside = SU_INVALID;
      su.SUGroupGetEntities(piece, &pieceInside);
      const SUResult filled = fill(pieceInside, solid, false);
      if (filled != SU_ERROR_NONE && filled != SU_ERROR_NO_DATA)
        return Status::failure("SketchUp could not take the faces of '" + part.name + "' (error " + std::to_string(filled) + ")");
    }
    const SUResult filled = fill(inside, rest, true);
    if (filled != SU_ERROR_NONE && filled != SU_ERROR_NO_DATA)
      return Status::failure("SketchUp could not take the faces of '" + part.name + "' (error " + std::to_string(filled) + ")");
    ++written;
  }
  if (written == 0) return Status::failure("Nothing to export");
  const SUResult saved = su.SUModelSaveToFileWithVersion(w.model, path.c_str(), SUModelVersion_Current);
  if (saved != SU_ERROR_NONE) return Status::failure("The SketchUp file could not be written: " + path + " (error " + std::to_string(saved) + ")");
  return Status::success();
}

namespace {

// --- import ------------------------------------------------------------------------------------
using Matrix = std::array<double, 16>;  // column-major, as SUTransformation

Matrix multiply(const Matrix& a, const Matrix& b) {
  Matrix r{};
  for (int c = 0; c < 4; ++c)
    for (int row = 0; row < 4; ++row) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
      r[c * 4 + row] = s;
    }
  return r;
}

Matrix identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

gp_Pnt place(const Matrix& m, const SUPoint3D& p) {
  const double x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
  const double y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
  const double z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
  double wv = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
  if (std::abs(wv) < 1e-12) wv = 1.0;
  return gp_Pnt(x / wv * kInch, y / wv * kInch, z / wv * kInch);
}

struct Reader {
  const Api& su;
  SkpReport& report;
  std::vector<ColladaPart> parts;
  std::vector<bool> closed;  // each part: a solid

  std::string groupName(SUGroupRef g) {
    SUStringRef s = SU_INVALID;
    su.SUStringCreate(&s);
    su.SUGroupGetName(g, &s);
    std::string out = text(su, s);
    su.SUStringRelease(&s);
    return out;
  }

  // A group made only of groups of its own name: the pieces of one object (its solids apart,
  // as writeSkp puts them).
  bool piecesOnly(SUEntitiesRef ents, const std::string& name) {
    size_t faces = 0, edges = 0, instances = 0, groups = 0;
    su.SUEntitiesGetNumFaces(ents, &faces);
    su.SUEntitiesGetNumEdges(ents, true, &edges);
    su.SUEntitiesGetNumInstances(ents, &instances);
    su.SUEntitiesGetNumGroups(ents, &groups);
    if (faces || edges || instances || groups < 2) return false;
    std::vector<SUGroupRef> children(groups, SUGroupRef SU_INVALID);
    su.SUEntitiesGetGroups(ents, groups, children.data(), &groups);
    for (size_t i = 0; i < groups; ++i)
      if (groupName(children[i]) != name) return false;
    return true;
  }

  // The face parts from `from` on as one part named `name` (a compound of their shapes, each
  // one's colour kept on its faces).
  void combine(std::size_t from, const std::string& name, int pieces) {
    std::vector<ColladaPart> kept;
    std::vector<bool> keptClosed;
    ColladaPart whole;
    whole.name = name;
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    int offset = 0, made = 0, solids = 0;
    for (std::size_t i = from; i < parts.size(); ++i) {
      ColladaPart& p = parts[i];
      if (!TopExp_Explorer(p.shape, TopAbs_FACE).More()) {
        kept.push_back(std::move(p));
        keptClosed.push_back(false);
        continue;
      }
      if (made == 0) whole.color = p.color;
      builder.Add(compound, p.shape);
      TopTools_IndexedMapOfShape faces;
      TopExp::MapShapes(p.shape, TopAbs_FACE, faces);
      const std::map<int, ColladaColor> own(p.faceColors.begin(), p.faceColors.end());
      for (int k = 1; k <= faces.Extent(); ++k) {
        const ColladaColor c = own.count(k) ? own.at(k) : p.color;
        if (!(c == whole.color)) whole.faceColors.emplace_back(offset + k, c);
      }
      offset += faces.Extent();
      ++made;
      if (closed[i]) ++solids;
    }
    parts.resize(from);
    closed.resize(from);
    if (made) {
      whole.shape = compound;
      parts.push_back(std::move(whole));
      closed.push_back(solids == made);
      report.solids += (solids == made ? 1 : 0) - solids;
    }
    for (std::size_t i = 0; i < kept.size(); ++i) {
      parts.push_back(std::move(kept[i]));
      closed.push_back(keptClosed[i]);
    }
    report.groups -= pieces;
  }

  std::optional<ColladaColor> colorOf(SUMaterialRef m) {
    if (SUIsInvalid(m)) return std::nullopt;
    SUColor c{};
    if (su.SUMaterialGetColor(m, &c) != SU_ERROR_NONE) return std::nullopt;
    ColladaColor out{c.red / 255.0, c.green / 255.0, c.blue / 255.0, 1.0};
    bool useOpacity = false;
    double alpha = 1.0;
    if (su.SUMaterialGetUseOpacity(m, &useOpacity) == SU_ERROR_NONE && useOpacity &&
        su.SUMaterialGetOpacity(m, &alpha) == SU_ERROR_NONE)
      out.a = std::clamp(alpha, 0.0, 1.0);
    SUTextureRef texture = SU_INVALID;
    if (su.SUMaterialGetTexture(m, &texture) == SU_ERROR_NONE && SUIsValid(texture)) ++report.textured;
    return out;
  }

  std::vector<gp_Pnt> loop(SULoopRef l, const Matrix& m) {
    size_t n = 0;
    su.SULoopGetNumVertices(l, &n);
    std::vector<SUVertexRef> vs(n, SUVertexRef SU_INVALID);
    su.SULoopGetVertices(l, n, vs.data(), &n);
    std::vector<gp_Pnt> out;
    for (size_t i = 0; i < n; ++i) {
      SUPoint3D p{};
      su.SUVertexGetPosition(vs[i], &p);
      out.push_back(place(m, p));
    }
    return out;
  }

  static std::optional<TopoDS_Wire> wire(const std::vector<gp_Pnt>& pts) {
    if (pts.size() < 3) return std::nullopt;
    BRepBuilderAPI_MakePolygon polygon;
    for (const gp_Pnt& p : pts) polygon.Add(p);
    polygon.Close();
    if (!polygon.IsDone()) return std::nullopt;
    return polygon.Wire();
  }

  bool hidden(SUDrawingElementRef e) {
    bool h = false;
    return su.SUDrawingElementGetHidden(e, &h) == SU_ERROR_NONE && h;
  }

  // One container's own faces and loose edges as a part; its groups and instances as their own.
  void entities(SUEntitiesRef ents, const Matrix& m, const std::string& name, const std::optional<ColladaColor>& inherited, int depth) {
    if (depth > 64) return;
    std::vector<std::pair<TopoDS_Face, ColladaColor>> made;
    size_t nFaces = 0;
    su.SUEntitiesGetNumFaces(ents, &nFaces);
    std::vector<SUFaceRef> faces(nFaces, SUFaceRef SU_INVALID);
    su.SUEntitiesGetFaces(ents, nFaces, faces.data(), &nFaces);
    for (size_t i = 0; i < nFaces; ++i) {
      if (hidden(su.SUFaceToDrawingElement(faces[i]))) {
        ++report.hidden;
        continue;
      }
      ++report.faces;
      SULoopRef outer = SU_INVALID;
      su.SUFaceGetOuterLoop(faces[i], &outer);
      const auto outerWire = wire(loop(outer, m));
      if (!outerWire) {
        ++report.leftOut;
        continue;
      }
      BRepBuilderAPI_MakeFace face(*outerWire, Standard_True);
      if (!face.IsDone()) {
        ++report.leftOut;
        continue;
      }
      size_t nInner = 0;
      su.SUFaceGetNumInnerLoops(faces[i], &nInner);
      std::vector<SULoopRef> inner(nInner, SULoopRef SU_INVALID);
      su.SUFaceGetInnerLoops(faces[i], nInner, inner.data(), &nInner);
      for (size_t k = 0; k < nInner; ++k)
        if (const auto hole = wire(loop(inner[k], m))) face.Add(*hole);
      ShapeFix_Face fix(face.Face());
      fix.FixOrientation();
      fix.Perform();
      SUMaterialRef front = SU_INVALID;
      su.SUFaceGetFrontMaterial(faces[i], &front);
      const auto colour = colorOf(front);
      made.emplace_back(fix.Face(), colour ? *colour : inherited ? *inherited : ColladaColor{});
    }
    if (!made.empty()) {
      bool solid = false;
      parts.push_back(joinFaces(made, name, &solid));
      closed.push_back(solid);
      if (solid) ++report.solids;
    }
    size_t nEdges = 0;
    su.SUEntitiesGetNumEdges(ents, true, &nEdges);
    if (nEdges) {
      std::vector<SUEdgeRef> edges(nEdges, SUEdgeRef SU_INVALID);
      su.SUEntitiesGetEdges(ents, true, nEdges, edges.data(), &nEdges);
      BRep_Builder builder;
      TopoDS_Compound lines;
      builder.MakeCompound(lines);
      int count = 0;
      for (size_t i = 0; i < nEdges; ++i) {
        if (hidden(su.SUEdgeToDrawingElement(edges[i]))) continue;
        SUVertexRef a = SU_INVALID, b = SU_INVALID;
        su.SUEdgeGetStartVertex(edges[i], &a);
        su.SUEdgeGetEndVertex(edges[i], &b);
        SUPoint3D pa{}, pb{};
        su.SUVertexGetPosition(a, &pa);
        su.SUVertexGetPosition(b, &pb);
        const gp_Pnt p = place(m, pa), q = place(m, pb);
        if (p.Distance(q) < 1e-6) continue;
        builder.Add(lines, BRepBuilderAPI_MakeEdge(p, q).Edge());
        ++count;
      }
      if (count) {
        report.lines += count;
        ColladaPart part;
        part.shape = lines;
        part.name = name.empty() ? std::string("Lines") : name + " lines";
        parts.push_back(std::move(part));
        closed.push_back(false);
      }
    }
    size_t nGroups = 0;
    su.SUEntitiesGetNumGroups(ents, &nGroups);
    std::vector<SUGroupRef> groups(nGroups, SUGroupRef SU_INVALID);
    su.SUEntitiesGetGroups(ents, nGroups, groups.data(), &nGroups);
    for (size_t i = 0; i < nGroups; ++i) {
      const SUDrawingElementRef element = su.SUGroupToDrawingElement(groups[i]);
      if (hidden(element)) {
        ++report.hidden;
        continue;
      }
      ++report.groups;
      SUTransformation t{};
      su.SUGroupGetTransform(groups[i], &t);
      Matrix local;
      std::copy(std::begin(t.values), std::end(t.values), local.begin());
      const std::string named = groupName(groups[i]);
      const std::string label = named.empty() ? std::string("Group") : named;
      SUMaterialRef material = SU_INVALID;
      su.SUDrawingElementGetMaterial(element, &material);
      const auto own = colorOf(material);
      SUEntitiesRef inside = SU_INVALID;
      su.SUGroupGetEntities(groups[i], &inside);
      const bool pieces = piecesOnly(inside, named);
      size_t count = 0;
      if (pieces) su.SUEntitiesGetNumGroups(inside, &count);
      const std::size_t from = parts.size();
      entities(inside, multiply(m, local), label, own ? own : inherited, depth + 1);
      if (pieces) combine(from, label, static_cast<int>(count));
    }
    size_t nInstances = 0;
    su.SUEntitiesGetNumInstances(ents, &nInstances);
    std::vector<SUComponentInstanceRef> instances(nInstances, SUComponentInstanceRef SU_INVALID);
    su.SUEntitiesGetInstances(ents, nInstances, instances.data(), &nInstances);
    for (size_t i = 0; i < nInstances; ++i) {
      const SUDrawingElementRef element = su.SUComponentInstanceToDrawingElement(instances[i]);
      if (hidden(element)) {
        ++report.hidden;
        continue;
      }
      ++report.instances;
      SUTransformation t{};
      su.SUComponentInstanceGetTransform(instances[i], &t);
      Matrix local;
      std::copy(std::begin(t.values), std::end(t.values), local.begin());
      SUComponentDefinitionRef definition = SU_INVALID;
      su.SUComponentInstanceGetDefinition(instances[i], &definition);
      SUStringRef s = SU_INVALID;
      su.SUStringCreate(&s);
      su.SUComponentInstanceGetName(instances[i], &s);
      std::string instanceName = text(su, s);
      if (instanceName.empty()) {
        su.SUComponentDefinitionGetName(definition, &s);
        instanceName = text(su, s);
      }
      su.SUStringRelease(&s);
      if (instanceName.empty()) instanceName = "Component";
      SUMaterialRef material = SU_INVALID;
      su.SUDrawingElementGetMaterial(element, &material);
      const auto own = colorOf(material);
      SUEntitiesRef inside = SU_INVALID;
      su.SUComponentDefinitionGetEntities(definition, &inside);
      entities(inside, multiply(m, local), instanceName, own ? own : inherited, depth + 1);
    }
  }
};

}  // namespace

Result<std::vector<ColladaPart>> readSkp(const std::string& path, SkpReport* reportOut) {
  using R = Result<std::vector<ColladaPart>>;
  std::string why;
  const Api* loaded = api(&why);
  if (!loaded) return R::failure(why);
  const Api& su = *loaded;
  SUModelRef model = SU_INVALID;
  enum SUModelLoadStatus status = SUModelLoadStatus_Success;
  const SUResult opened = su.SUModelCreateFromFileWithStatus(&model, path.c_str(), &status);
  if (opened != SU_ERROR_NONE) return R::failure("The SketchUp file could not be read: " + path + " (error " + std::to_string(opened) + ")");
  struct Release {
    const Api& su;
    SUModelRef& model;
    ~Release() { su.SUModelRelease(&model); }
  } release{su, model};
  SkpReport local;
  SkpReport& report = reportOut ? *reportOut : local;
  report = SkpReport{};
  Reader reader{su, report, {}};
  SUEntitiesRef root = SU_INVALID;
  su.SUModelGetEntities(model, &root);
  reader.entities(root, identity(), "SketchUp", std::nullopt, 0);
  report.parts = static_cast<int>(reader.parts.size());
  if (reader.parts.empty()) return R::failure("The SketchUp file has nothing to import (" + report.summary() + ")");
  return std::move(reader.parts);
}

#endif  // SBR_WITH_SKETCHUP

}  // namespace sbr::geom
