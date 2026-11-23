#include "sbr/doc/IfcExport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <nlohmann/json.hpp>

#include "sbr/OpeningSymbols.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/Schedules.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Profile.h"

namespace sbr::doc {
namespace {

// --- text ------------------------------------------------------------------------------------
std::string real(double v) {
  if (!std::isfinite(v)) v = 0.0;
  if (std::abs(v) < 5e-7) v = 0.0;
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%.6f", v);
  std::string s = buffer;
  while (s.size() > 1 && s.back() == '0') s.pop_back();  // "12.500000" -> "12.5", "3.000000" -> "3."
  return s;
}

// A STEP string: apostrophes and backslashes doubled, everything outside ASCII as \X2\....\X0\.
std::string text(const std::string& utf8) {
  std::string out = "'";
  std::string wide;  // pending UTF-16 code units, as hex
  auto flush = [&] {
    if (wide.empty()) return;
    out += "\\X2\\" + wide + "\\X0\\";
    wide.clear();
  };
  auto hex4 = [](unsigned v) {
    char b[8];
    std::snprintf(b, sizeof b, "%04X", v & 0xFFFFu);
    return std::string(b);
  };
  for (std::size_t i = 0; i < utf8.size();) {
    const unsigned char c = static_cast<unsigned char>(utf8[i]);
    std::uint32_t cp = c;
    std::size_t n = 1;
    if (c >= 0xF0 && i + 3 < utf8.size()) {
      cp = (c & 0x07u) << 18 | (utf8[i + 1] & 0x3Fu) << 12 | (utf8[i + 2] & 0x3Fu) << 6 | (utf8[i + 3] & 0x3Fu);
      n = 4;
    } else if (c >= 0xE0 && i + 2 < utf8.size()) {
      cp = (c & 0x0Fu) << 12 | (utf8[i + 1] & 0x3Fu) << 6 | (utf8[i + 2] & 0x3Fu);
      n = 3;
    } else if (c >= 0xC0 && i + 1 < utf8.size()) {
      cp = (c & 0x1Fu) << 6 | (utf8[i + 1] & 0x3Fu);
      n = 2;
    }
    i += n;
    if (cp < 0x80 && cp >= 0x20) {
      flush();
      if (cp == '\'') out += "''";
      else if (cp == '\\') out += "\\\\";
      else out += static_cast<char>(cp);
    } else if (cp < 0x20) {
      flush();
      out += ' ';
    } else if (cp >= 0x10000) {
      cp -= 0x10000;
      wide += hex4(0xD800 + (cp >> 10)) + hex4(0xDC00 + (cp & 0x3FF));
    } else {
      wide += hex4(cp);
    }
  }
  flush();
  return out + "'";
}

// An IFC GlobalId (22 characters) for a thing, always the same for the same key: other
// software recognises the element again when the model is exported again.
std::string guid(const std::string& key) {
  auto fnv = [&](std::uint64_t h) {
    for (unsigned char c : key) {
      h ^= c;
      h *= 1099511628211ull;
    }
    return h;
  };
  const std::uint64_t a = fnv(14695981039346656037ull), b = fnv(0x9E3779B97F4A7C15ull ^ key.size());
  unsigned char bytes[16];
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<unsigned char>(a >> (56 - 8 * i));
    bytes[8 + i] = static_cast<unsigned char>(b >> (56 - 8 * i));
  }
  static const char* digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$";
  std::string out;
  out += digits[bytes[0] >> 6];
  out += digits[bytes[0] & 0x3F];
  for (int i = 1; i < 16; i += 3) {
    const std::uint32_t v = static_cast<std::uint32_t>(bytes[i]) << 16 | static_cast<std::uint32_t>(bytes[i + 1]) << 8 | bytes[i + 2];
    out += digits[(v >> 18) & 0x3F];
    out += digits[(v >> 12) & 0x3F];
    out += digits[(v >> 6) & 0x3F];
    out += digits[v & 0x3F];
  }
  return "'" + out + "'";
}

std::string ref(int id) { return "#" + std::to_string(id); }

std::string refs(const std::vector<int>& ids) {
  std::string out = "(";
  for (std::size_t i = 0; i < ids.size(); ++i) out += (i ? "," : "") + ref(ids[i]);
  return out + ")";
}

// --- the file being written ------------------------------------------------------------------
struct File {
  std::vector<std::string> lines;
  std::map<std::string, int> points;
  std::map<std::string, int> directions;

  int add(const std::string& body) {
    lines.push_back(body);
    return static_cast<int>(lines.size());
  }
  int point(double x, double y, double z) {
    const std::string key = real(x) + "," + real(y) + "," + real(z);
    const auto it = points.find(key);
    if (it != points.end()) return it->second;
    return points[key] = add("IFCCARTESIANPOINT((" + key + "))");
  }
  int point2(double x, double y) { return add("IFCCARTESIANPOINT((" + real(x) + "," + real(y) + "))"); }
  int direction(double x, double y, double z) {
    const std::string key = real(x) + "," + real(y) + "," + real(z);
    const auto it = directions.find(key);
    if (it != directions.end()) return it->second;
    return directions[key] = add("IFCDIRECTION((" + key + "))");
  }
  // A placement relative to `relTo` (0: the world), with a horizontal X axis.
  int placement(int relTo, double x, double y, double z, const gp_Dir& along = gp::DX()) {
    const int axis = add("IFCAXIS2PLACEMENT3D(" + ref(point(x, y, z)) + "," + ref(direction(0, 0, 1)) + "," +
                         ref(direction(along.X(), along.Y(), 0)) + ")");
    return add("IFCLOCALPLACEMENT(" + (relTo ? ref(relTo) : std::string("$")) + "," + ref(axis) + ")");
  }
};

struct Loop {
  std::vector<gp_Pnt> points;
};

gp_Vec newell(const std::vector<gp_Pnt>& p) {
  gp_Vec n(0, 0, 0);
  for (std::size_t i = 0; i < p.size(); ++i) {
    const gp_Pnt& a = p[i];
    const gp_Pnt& b = p[(i + 1) % p.size()];
    n += gp_Vec((a.Y() - b.Y()) * (a.Z() + b.Z()), (a.Z() - b.Z()) * (a.X() + b.X()), (a.X() - b.X()) * (a.Y() + b.Y()));
  }
  return n;
}

// One solid as an IfcFacetedBrep, z lowered by `dz`. Flat faces with straight sides go as
// they are (outline and holes); others as triangles.
int brep(File& f, const TopoDS_Shape& solid, double dz, bool& meshed) {
  std::vector<int> faces;
  bool triangulated = false;
  auto loopOf = [&](std::vector<gp_Pnt> points, bool reversed) {
    if (reversed) std::reverse(points.begin(), points.end());
    std::vector<int> ids;
    for (const gp_Pnt& p : points) {
      const int id = f.point(p.X(), p.Y(), p.Z() - dz);
      if (ids.empty() || ids.back() != id) ids.push_back(id);  // no side of no length
    }
    if (ids.size() > 1 && ids.front() == ids.back()) ids.pop_back();
    return ids.size() >= 3 ? f.add("IFCPOLYLOOP(" + refs(ids) + ")") : 0;
  };
  for (TopExp_Explorer fe(solid, TopAbs_FACE); fe.More(); fe.Next()) {
    const TopoDS_Face face = TopoDS::Face(fe.Current());
    const bool reversed = face.Orientation() == TopAbs_REVERSED;
    BRepAdaptor_Surface surface(face);
    bool flat = surface.GetType() == GeomAbs_Plane;
    for (TopExp_Explorer ee(face, TopAbs_EDGE); flat && ee.More(); ee.Next())
      flat = BRepAdaptor_Curve(TopoDS::Edge(ee.Current())).GetType() == GeomAbs_Line;
    if (flat) {
      gp_Vec outward(surface.Plane().Axis().Direction());
      if (reversed) outward.Reverse();
      const TopoDS_Wire outer = BRepTools::OuterWire(face);
      std::vector<int> bounds;
      bool ok = true;
      for (TopExp_Explorer we(face, TopAbs_WIRE); we.More(); we.Next()) {
        const TopoDS_Wire wire = TopoDS::Wire(we.Current());
        std::vector<gp_Pnt> points;
        for (BRepTools_WireExplorer ex(wire, face); ex.More(); ex.Next()) points.push_back(BRep_Tool::Pnt(ex.CurrentVertex()));
        if (points.size() < 3) {
          ok = false;
          break;
        }
        const bool isOuter = wire.IsSame(outer);
        // Outline: counter-clockwise seen from outside. Holes: the other way.
        const bool ccw = newell(points).Dot(outward) > 0;
        const int loop = loopOf(points, isOuter ? !ccw : ccw);
        if (!loop) {
          ok = false;
          break;
        }
        const int bound = f.add(std::string(isOuter ? "IFCFACEOUTERBOUND(" : "IFCFACEBOUND(") + ref(loop) + ",.T.)");
        if (isOuter) bounds.insert(bounds.begin(), bound);
        else bounds.push_back(bound);
      }
      if (ok && !bounds.empty()) {
        faces.push_back(f.add("IFCFACE(" + refs(bounds) + ")"));
        continue;
      }
    }
    // Curved, or with curved sides: triangles.
    if (!triangulated) {
      BRepMesh_IncrementalMesh mesh(solid, 2.0, Standard_False, 0.3, Standard_True);
      triangulated = true;
      meshed = true;
    }
    TopLoc_Location location;
    const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, location);
    if (tri.IsNull()) continue;
    const gp_Trsf t = location.Transformation();
    for (int i = 1; i <= tri->NbTriangles(); ++i) {
      int a = 0, b = 0, c = 0;
      tri->Triangle(i).Get(a, b, c);
      if (reversed) std::swap(b, c);
      const int loop = loopOf({tri->Node(a).Transformed(t), tri->Node(b).Transformed(t), tri->Node(c).Transformed(t)}, false);
      if (!loop) continue;
      faces.push_back(f.add("IFCFACE((" + ref(f.add("IFCFACEOUTERBOUND(" + ref(loop) + ",.T.)")) + "))"));
    }
  }
  if (faces.empty()) return 0;
  return f.add("IFCFACETEDBREP(" + ref(f.add("IFCCLOSEDSHELL(" + refs(faces) + ")")) + ")");
}

std::string attribute(const Object& o, const char* key) {
  const auto it = o.attributes.find(key);
  return it == o.attributes.end() ? std::string() : it->second;
}

struct Storey {
  std::string id, name;
  double elevation = 0.0;
  int entity = 0, placement = 0;
  std::vector<int> elements, spaces;
};

}  // namespace

Result<std::string> ifcText(const Modeler& modeler, const std::string& fileName, IfcSummary* summaryOut) {
  using R = Result<std::string>;
  const Document& document = modeler.document();
  IfcSummary summary;
  File f;
  // --- project, units, contexts ---
  const int origin = f.add("IFCAXIS2PLACEMENT3D(" + ref(f.point(0, 0, 0)) + "," + ref(f.direction(0, 0, 1)) + "," + ref(f.direction(1, 0, 0)) + ")");
  const int context = f.add("IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,1.E-5," + ref(origin) + ",$)");
  const int body = f.add("IFCGEOMETRICREPRESENTATIONSUBCONTEXT('Body','Model',*,*,*,*," + ref(context) + ",$,.MODEL_VIEW.,$)");
  const int axisContext = f.add("IFCGEOMETRICREPRESENTATIONSUBCONTEXT('Axis','Model',*,*,*,*," + ref(context) + ",$,.GRAPH_VIEW.,$)");
  const int mm = f.add("IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.)");
  const int m2 = f.add("IFCSIUNIT(*,.AREAUNIT.,$,.SQUARE_METRE.)");
  const int m3 = f.add("IFCSIUNIT(*,.VOLUMEUNIT.,$,.CUBIC_METRE.)");
  const int rad = f.add("IFCSIUNIT(*,.PLANEANGLEUNIT.,$,.RADIAN.)");
  const int units = f.add("IFCUNITASSIGNMENT(" + refs({mm, m2, m3, rad}) + ")");
  const int project = f.add("IFCPROJECT(" + guid("project") + ",$," + text(fileName) + ",$,$,$,$," + refs({context}) + "," + ref(units) + ")");
  const int sitePlacement = f.placement(0, 0, 0, 0);
  const int site = f.add("IFCSITE(" + guid("site") + ",$,'Site',$,$," + ref(sitePlacement) + ",$,$,.ELEMENT.,$,$,$,$,$)");
  const int buildingPlacement = f.placement(sitePlacement, 0, 0, 0);
  const int building = f.add("IFCBUILDING(" + guid("building") + ",$,'Building',$,$," + ref(buildingPlacement) + ",$,$,.ELEMENT.,$,$,$)");
  f.add("IFCRELAGGREGATES(" + guid("rel:project-site") + ",$,$,$," + ref(project) + "," + refs({site}) + ")");
  f.add("IFCRELAGGREGATES(" + guid("rel:site-building") + ",$,$,$," + ref(site) + "," + refs({building}) + ")");

  // --- storeys from the levels ---
  std::vector<Storey> storeys;
  for (const FloorLevel& l : modeler.levels()) storeys.push_back({l.id, l.name, l.elevation});
  if (storeys.empty()) {
    storeys.push_back({"ground", "GL", 0.0});
    summary.notes.push_back("No levels in the model: everything is on one storey 'GL' at height 0");
  }
  std::stable_sort(storeys.begin(), storeys.end(), [](const Storey& a, const Storey& b) { return a.elevation < b.elevation; });
  for (Storey& s : storeys) {
    s.placement = f.placement(buildingPlacement, 0, 0, s.elevation);
    s.entity = f.add("IFCBUILDINGSTOREY(" + guid("storey:" + s.id) + ",$," + text(s.name) + ",$,$," + ref(s.placement) + ",$,$,.ELEMENT.," +
                     real(s.elevation) + ")");
  }
  {
    std::vector<int> all;
    for (const Storey& s : storeys) all.push_back(s.entity);
    f.add("IFCRELAGGREGATES(" + guid("rel:building-storeys") + ",$,$,$," + ref(building) + "," + refs(all) + ")");
  }
  summary.storeys = static_cast<int>(storeys.size());
  auto storeyAt = [&](double z) -> Storey& {
    std::size_t found = 0;
    for (std::size_t i = 0; i < storeys.size(); ++i)
      if (storeys[i].elevation <= z + 1.0) found = i;
    return storeys[found];
  };
  auto storeyOf = [&](const std::string& levelId, double z) -> Storey& {
    for (Storey& s : storeys)
      if (!levelId.empty() && s.id == levelId) return s;
    return storeyAt(z);
  };

  // --- properties and quantities ---
  auto relate = [&](const std::string& key, int element, int definition) {
    f.add("IFCRELDEFINESBYPROPERTIES(" + guid("rel:" + key) + ",$,$,$," + refs({element}) + "," + ref(definition) + ")");
  };
  auto quantities = [&](const std::string& key, int element, const char* name, const std::vector<int>& items) {
    if (items.empty()) return;
    relate(key, element, f.add("IFCELEMENTQUANTITY(" + guid("qto:" + key) + ",$," + text(name) + ",$,$," + refs(items) + ")"));
  };
  auto length = [&](const char* name, double v) { return f.add("IFCQUANTITYLENGTH(" + text(name) + ",$,$," + real(v) + ",$)"); };
  auto area = [&](const char* name, double mm2) { return f.add("IFCQUANTITYAREA(" + text(name) + ",$,$," + real(mm2 / 1e6) + ",$)"); };
  auto volumeOf = [&](const char* name, double mm3) { return f.add("IFCQUANTITYVOLUME(" + text(name) + ",$,$," + real(mm3 / 1e9) + ",$)"); };

  // The marks and product numbers of the openings, as in the schedule (docs/117).
  std::map<std::string, const OpeningRow*> marks;
  const OpeningSchedule schedule = openingSchedule(document);
  for (const OpeningRow& r : schedule.rows) marks[r.host + ":" + r.openingId] = &r;
  std::map<std::pair<std::string, std::string>, TopoDS_Compound> fittingBodies;
  BRep_Builder builder;
  for (const draw::FittingSolid& part : draw::fittingSolids(document)) {
    auto& compound = fittingBodies[{part.host, part.openingId}];
    if (compound.IsNull()) builder.MakeCompound(compound);
    builder.Add(compound, part.shape);
  }

  // --- elements ---
  bool meshedAny = false;
  auto shapeOf = [&](const TopoDS_Shape& world, double dz, int axisRep = 0) {
    std::vector<int> items;
    for (TopExp_Explorer e(world, TopAbs_SOLID); e.More(); e.Next())
      if (const int item = brep(f, e.Current(), dz, meshedAny)) items.push_back(item);
    if (items.empty()) return 0;
    const int rep = f.add("IFCSHAPEREPRESENTATION(" + ref(body) + ",'Body','Brep'," + refs(items) + ")");
    return f.add("IFCPRODUCTDEFINITIONSHAPE($,$," + refs(axisRep ? std::vector{rep, axisRep} : std::vector{rep}) + ")");
  };
  std::set<std::string> columnRoles = {"column", "through_column", "roof_post", "floor_post"};
  std::set<std::string> beamRoles = {"beam", "girder", "floor_beam", "rim_beam", "eave_beam", "roof_beam", "sill",
                                     "ridge", "purlin", "floor_girder", "lintel"};
  std::set<std::string> memberRoles = {"joist", "rafter", "stud", "brace"};
  std::vector<std::pair<std::string, int>> ceilings;  // (room id, covering): related to the room's space below
  std::vector<std::pair<std::string, int>> canopies;  // (wall id, canopy): connected to their walls (docs/155)
  std::map<std::string, int> written;                 // object id -> its element
  std::map<std::string, int> materials;               // layer name -> IfcMaterial (docs/170)
  for (const Object& o : document.objects()) {
    if (o.isContainer() || !document.isVisible(o.id)) continue;
    const std::string kind = attribute(o, "kind");
    if (kind == "room") continue;  // spaces, below
    if (modeler.gridSpec(o.id) || o.layer == "Grid") continue;  // grids (their lines and labels) go as IfcGrid, below
    const TopoDS_Shape world = o.shape.IsNull() ? TopoDS_Shape() : document.worldShape(o);
    if (world.IsNull() || !TopExp_Explorer(world, TopAbs_SOLID).More()) {
      if (!std::holds_alternative<GuideLine>(o.recipe.base)) ++summary.skipped;
      continue;
    }
    const Bnd_Box box = geom::exactBounds(world);
    const double z0 = box.CornerMin().Z(), z1 = box.CornerMax().Z();
    Storey& storey = kind == "wall"                     ? storeyOf(attribute(o, "base_level"), z0)
                     : (kind == "slab" || kind == "roof") ? storeyOf(attribute(o, "top_level"), kind == "slab" ? z1 : z0)
                                                          : storeyAt(z0);
    int placement = 0;
    int axisRep = 0;
    double bodyElevation = storey.elevation;
    TopLoc_Location wallLocal;
    if (const auto line = Modeler::wallReferenceLine(document, o)) {
      const auto& [start, end] = *line;
      const gp_Vec run(start, end);
      const gp_Dir along(run.X(), run.Y(), 0);
      placement = f.placement(storey.placement, start.X(), start.Y(), start.Z() - storey.elevation, along);
      gp_Trsf toLocal;
      toLocal.SetTransformation(gp_Ax3(start, gp::DZ(), along));
      wallLocal = TopLoc_Location(toLocal);
      bodyElevation = 0;
      const int axis = f.add("IFCPOLYLINE(" + refs({f.point2(0, 0), f.point2(std::hypot(run.X(), run.Y()), 0)}) + ")");
      axisRep = f.add("IFCSHAPEREPRESENTATION(" + ref(axisContext) + ",'Axis','Curve2D'," + refs({axis}) + ")");
    } else {
      placement = f.placement(storey.placement, 0, 0, 0);
    }
    const int shape = shapeOf(world.Moved(wallLocal), bodyElevation, axisRep);
    if (!shape) {
      ++summary.skipped;
      continue;
    }
    const std::string head = guid("object:" + o.id) + ",$," + text(o.name) + ",$,";
    const std::string tail = "," + ref(placement) + "," + ref(shape) + "," + text(o.id);
    int element = 0;
    if (kind == "wall") {
      const std::string type = attribute(o, "wall_type_name");
      element = f.add("IFCWALL(" + head + (type.empty() ? std::string("$") : text(type)) + tail + ",.NOTDEFINED.)");
      ++summary.walls;
      if (!type.empty()) {
        const bool external = attribute(o, "wall_type").rfind("exterior", 0) == 0;
        const int reference = f.add("IFCPROPERTYSINGLEVALUE('Reference',$,IFCIDENTIFIER(" + text(type) + "),$)");
        const int isExternal = f.add(std::string("IFCPROPERTYSINGLEVALUE('IsExternal',$,IFCBOOLEAN(") + (external ? ".T." : ".F.") + "),$)");
        relate("pset:" + o.id, element,
               f.add("IFCPROPERTYSET(" + guid("pset:" + o.id) + ",$,'Pset_WallCommon',$," + refs({reference, isExternal}) + ")"));
      }
      std::vector<int> q;
      if (const auto info = modeler.memberInfo(o.id)) {
        for (const auto& field : info->fields) {
          if (field.key == "length") q.push_back(length("Length", field.value));
          if (field.key == "thickness") q.push_back(length("Width", field.value));
          if (field.key == "height") q.push_back(length("Height", z1 - z0));
        }
      }
      q.push_back(volumeOf("NetVolume", geom::volume(world)));
      quantities(o.id, element, "Qto_WallBaseQuantities", q);
      // Its openings: voids of the wall, each filled by a window or a door when it has a fitting.
      nlohmann::json fittings = nlohmann::json::object();
      if (const std::string stored = attribute(o, "opening_fittings"); !stored.empty()) {
        const auto parsed = nlohmann::json::parse(stored, nullptr, false);
        if (parsed.is_object()) fittings = parsed;
      }
      if (const auto openings = modeler.openings(o.id)) {
        for (const Modeler::OpeningInfo& op : *openings) {
          if (std::abs(op.normal.Z()) > 1e-6 || op.depth < 1.0) continue;
          const gp_Dir along = gp::DZ().Crossed(op.normal);
          geom::SketchPlane plane;
          plane.normal = op.normal;
          plane.xDir = along;
          plane.origin = op.center.Translated(gp_Vec(along) * (-op.along / 2));
          plane.origin.SetZ(op.bottom);
          const auto outline = geom::makeRectangleFace(plane, op.along, op.height);
          if (!outline) continue;
          BRepPrimAPI_MakePrism hole(outline.value(), gp_Vec(op.normal) * -op.depth);
          if (!hole.IsDone()) continue;
          const int holeShape = shapeOf(hole.Shape().Moved(wallLocal), bodyElevation);
          if (!holeShape) continue;
          const std::string key = "opening:" + o.id + ":" + op.id;
          const int holePlacement = f.placement(placement, 0, 0, 0);
          const int opening = f.add("IFCOPENINGELEMENT(" + guid(key) + ",$," + text(op.name.empty() ? "Opening" : op.name) + ",$,$," +
                                    ref(holePlacement) + "," + ref(holeShape) + "," + text(op.id) + ",.OPENING.)");
          f.add("IFCRELVOIDSELEMENT(" + guid("void:" + key) + ",$,$,$," + ref(element) + "," + ref(opening) + ")");
          ++summary.openings;
          const auto fitting = fittings.find(op.id);
          const std::string type = fitting != fittings.end() && fitting->is_object() ? fitting->value("type", std::string()) : std::string();
          if (type.empty() || type == "none") continue;
          // A fitting whose sill is on the floor is a door; above it, a window.
          const bool door = type != "window" && op.sill < 1.0;
          const gp_Pnt at(op.center.X(), op.center.Y(), op.bottom - storey.elevation);
          const int fillPlacement = f.placement(storey.placement, at.X(), at.Y(), at.Z());
          int fillShape = 0;
          if (const auto part = fittingBodies.find({o.id, op.id}); part != fittingBodies.end()) {
            gp_Trsf toLocal;
            toLocal.SetTranslation(gp_Vec(-at.X(), -at.Y(), -op.bottom));
            fillShape = shapeOf(part->second.Moved(TopLoc_Location(toLocal)), 0);
          }
          // Name: its mark (AW1, WD2, AW-1, ...); Description: its product number when given (docs/117).
          const auto marked = marks.find(o.id + ":" + op.id);
          const std::string name = marked != marks.end() && !marked->second->mark.empty() ? marked->second->mark : (door ? "Door" : "Window");
          const std::string product = marked != marks.end() ? marked->second->product : std::string();
          const std::string common = guid("fitting:" + key) + ",$," + text(name) + "," + (product.empty() ? std::string("$") : text(product)) + "," + text(type) + "," +
                                     ref(fillPlacement) + "," + (fillShape ? ref(fillShape) : "$") + "," + text(op.id) + "," + real(op.height) + "," + real(op.width);
          int fill = 0;
          if (door) {
            const std::string hinge = fitting->value("hinge", std::string("left")), slide = fitting->value("slide", std::string("left"));
            const std::string operation = type == "swing_single"     ? (hinge == "right" ? "SINGLE_SWING_RIGHT" : "SINGLE_SWING_LEFT")
                                          : type == "swing_double"   ? "DOUBLE_DOOR_SINGLE_SWING"
                                          : type == "sliding_single" ? (slide == "right" ? "SLIDING_TO_RIGHT" : "SLIDING_TO_LEFT")
                                                                     : "DOUBLE_DOOR_SLIDING";
            fill = f.add("IFCDOOR(" + common + ",.DOOR.,." + operation + ".,$)");
            ++summary.doors;
          } else {
            const char* panels = type == "sliding_double" || type == "sliding_bi" || type == "swing_double" ? "DOUBLE_PANEL_VERTICAL" : "SINGLE_PANEL";
            fill = f.add("IFCWINDOW(" + common + ",.WINDOW.,." + panels + ".,$)");
            ++summary.windows;
          }
          f.add("IFCRELFILLSELEMENT(" + guid("fill:" + key) + ",$,$,$," + ref(opening) + "," + ref(fill) + ")");
          storey.elements.push_back(fill);
        }
      }
    } else if (kind == "slab") {
      element = f.add("IFCSLAB(" + head + "$" + tail + ",.FLOOR.)");
      ++summary.slabs;
      quantities(o.id, element, "Qto_SlabBaseQuantities", {length("Width", z1 - z0), volumeOf("NetVolume", geom::volume(world))});
    } else if (kind == "roof") {
      const auto roof = modeler.roofInfo(o.id);
      const std::string type = !roof ? "NOTDEFINED" : roof->spec.kind == "hip" ? "HIP_ROOF" : roof->spec.kind == "shed" ? "SHED_ROOF" : "GABLE_ROOF";
      element = f.add("IFCROOF(" + head + "$" + tail + ",." + type + ".)");
      ++summary.roofs;
      if (roof) quantities(o.id, element, "Qto_RoofBaseQuantities", {area("GrossArea", roof->slopeArea), area("ProjectedArea", roof->planArea)});
    } else if (kind == "stair") {
      // Its shape by its turns: winders or landings, a quarter turn or a half.
      const auto stair = modeler.stairInfo(o.id);
      std::string type = "NOTDEFINED";
      if (stair) {
        const int turns = stair->winders > 0 && stair->landings > 0 ? -1 : static_cast<int>(stair->spec.path.size()) - 2;
        const bool winding = stair->winders > 0;
        if (turns == 0) type = "STRAIGHT_RUN_STAIR";
        else if (turns == 1) type = winding ? "QUARTER_WINDING_STAIR" : "QUARTER_TURN_STAIR";
        else if (turns == 2) type = stair->flights == 2 ? (winding ? "HALF_WINDING_STAIR" : "HALF_TURN_STAIR")
                                                         : (winding ? "TWO_QUARTER_WINDING_STAIR" : "TWO_QUARTER_TURN_STAIR");
      }
      element = f.add("IFCSTAIR(" + head + "$" + tail + ",." + type + ".)");
      ++summary.stairs;
    } else if (kind == "canopy") {
      // A canopy (docs/147): a slab of its own kind, connected to the wall it stands out from (docs/155).
      element = f.add("IFCSLAB(" + head + text("CANOPY") + tail + ",.USERDEFINED.)");
      ++summary.canopies;
      quantities(o.id, element, "Qto_SlabBaseQuantities", {length("Width", z1 - z0), volumeOf("NetVolume", geom::volume(world))});
      if (const std::string host = attribute(o, "host"); !host.empty()) canopies.emplace_back(host, element);
    } else if (kind == "balcony") {
      // A balcony (docs/163, docs/167): its floor a slab of its own kind, its rail a railing, its
      // posts columns, connected to the wall it stands out from. Moved or reshaped by hand: one slab.
      const auto parts = modeler.balconyParts(o.id);
      const int floorShape = parts ? shapeOf(parts->floor, storey.elevation) : shape;
      element = f.add("IFCSLAB(" + head + text("BALCONY") + "," + ref(placement) + "," + ref(floorShape ? floorShape : shape) + "," + text(o.id) +
                      ",.USERDEFINED.)");
      ++summary.balconies;
      if (!parts) summary.notes.push_back("Balcony '" + o.name + "': " + parts.error() + " - written as one slab");
      std::vector<int> others;
      if (parts && !parts->rail.IsNull())
        if (const int railShape = shapeOf(parts->rail, storey.elevation)) {
          const int rail = f.add("IFCRAILING(" + guid("rail:" + o.id) + ",$," + text(o.name + " rail") + ",$,$," + ref(placement) + "," + ref(railShape) +
                                 "," + text(o.id + ":rail") + ",.GUARDRAIL.)");
          ++summary.railings;
          storey.elements.push_back(rail);
          others.push_back(rail);
        }
      if (parts && !parts->posts.IsNull()) {
        int n = 0;
        for (TopExp_Explorer e(parts->posts, TopAbs_SOLID); e.More(); e.Next(), ++n) {
          const int postShape = shapeOf(e.Current(), storey.elevation);
          if (!postShape) continue;
          const int post = f.add("IFCCOLUMN(" + guid("post:" + o.id + ":" + std::to_string(n)) + ",$," + text(o.name + " post " + std::to_string(n + 1)) +
                                 ",$," + text("balcony_post") + "," + ref(placement) + "," + ref(postShape) + "," + text(o.id + ":post") + ",.COLUMN.)");
          ++summary.columns;
          storey.elements.push_back(post);
          others.push_back(post);
        }
      }
      for (const int other : others)
        f.add("IFCRELCONNECTSELEMENTS(" + guid("balcony:" + o.id + ":" + std::to_string(other)) + ",$,$,$,$," + ref(element) + "," + ref(other) + ")");
      quantities(o.id, element, "Qto_SlabBaseQuantities", {volumeOf("NetVolume", geom::volume(parts ? parts->floor : world))});
      if (const std::string host = attribute(o, "host"); !host.empty()) canopies.emplace_back(host, element);
    } else if (kind == "ceiling") {
      // A ceiling board (docs/123): a covering of its room (docs/127).
      element = f.add("IFCCOVERING(" + head + "$" + tail + ",.CEILING.)");
      ++summary.coverings;
      if (const std::string roomId = attribute(o, "room"); !roomId.empty()) ceilings.emplace_back(roomId, element);
    } else if (columnRoles.count(kind)) {
      element = f.add("IFCCOLUMN(" + head + text(kind) + tail + ",.COLUMN.)");
      ++summary.columns;
    } else if (beamRoles.count(kind)) {
      element = f.add("IFCBEAM(" + head + text(kind) + tail + ",.BEAM.)");
      ++summary.beams;
    } else if (memberRoles.count(kind)) {
      const char* type = kind == "rafter" ? "RAFTER" : kind == "stud" ? "STUD" : kind == "brace" ? "BRACE" : "MEMBER";
      element = f.add("IFCMEMBER(" + head + text(kind) + tail + ",." + type + ".)");
      ++summary.members;
    } else {
      element = f.add("IFCBUILDINGELEMENTPROXY(" + head + (kind.empty() ? std::string("$") : text(kind)) + tail + ",.NOTDEFINED.)");
      ++summary.proxies;
    }
    storey.elements.push_back(element);
    written[o.id] = element;
    // docs/170: its layers as materials (a wall from its drawn line, a floor / roof from its top).
    if (const auto usage = Modeler::layerUsage(document, o)) {
      std::vector<int> layers;
      for (const Modeler::TypeLayer& l : usage->layers) {
        int& material = materials[l.name];
        if (!material) material = f.add("IFCMATERIAL(" + text(l.name) + ",$,$)");
        layers.push_back(f.add("IFCMATERIALLAYER(" + ref(material) + "," + real(l.thickness) + ",$," + text(l.name) + ",$,$,$)"));
      }
      const int set = f.add("IFCMATERIALLAYERSET(" + refs(layers) + "," + (usage->setName.empty() ? std::string("$") : text(usage->setName)) + ",$)");
      const std::string direction = usage->wall ? ".AXIS2." : ".AXIS3.";
      const std::string sense = usage->wall ? (usage->towardsLeft ? ".POSITIVE." : ".NEGATIVE.") : ".NEGATIVE.";
      const int use = f.add("IFCMATERIALLAYERSETUSAGE(" + ref(set) + "," + direction + "," + sense + "," + real(usage->wall ? usage->offset : 0.0) + ",$)");
      f.add("IFCRELASSOCIATESMATERIAL(" + guid("material:" + o.id) + ",$,$,$," + refs({element}) + "," + ref(use) + ")");
      ++summary.layerSets;
    }
  }
  for (const auto& [host, canopy] : canopies) {
    const auto wall = written.find(host);
    if (wall == written.end()) continue;
    f.add("IFCRELCONNECTSELEMENTS(" + guid("canopy:" + host + ":" + std::to_string(canopy)) + ",$,$,$,$," + ref(wall->second) + "," +
          ref(canopy) + ")");
  }

  // --- rooms as spaces ---
  for (const RoomInfo& room : modeler.rooms()) {
    if (!room.closed || !room.problem.empty()) {
      summary.notes.push_back("Room '" + room.name + "' is not exported: " + (room.closed ? room.problem : "not enclosed by walls"));
      continue;
    }
    Storey& storey = storeyOf(room.level, room.at.Z());
    // Up to the next level; without one above, 2400 (said in the notes).
    double height = 2400.0;
    bool assumed = true;
    for (const Storey& s : storeys)
      if (s.elevation > room.at.Z() + 1.0) {
        height = s.elevation - room.at.Z();
        assumed = false;
        break;
      }
    if (assumed) summary.notes.push_back("Room '" + room.name + "': no level above it, its space is 2400 high");
    const std::vector<gp_Pnt>& outline = room.innerKnown ? room.inner : room.centre;
    std::vector<int> points;
    for (const gp_Pnt& p : outline) points.push_back(f.point2(p.X(), p.Y()));
    points.push_back(points.front());
    const int profile = f.add("IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$," + ref(f.add("IFCPOLYLINE(" + refs(points) + ")")) + ")");
    const int at = f.add("IFCAXIS2PLACEMENT3D(" + ref(f.point(0, 0, room.at.Z() - storey.elevation)) + "," + ref(f.direction(0, 0, 1)) + "," +
                         ref(f.direction(1, 0, 0)) + ")");
    const int solid = f.add("IFCEXTRUDEDAREASOLID(" + ref(profile) + "," + ref(at) + "," + ref(f.direction(0, 0, 1)) + "," + real(height) + ")");
    const int rep = f.add("IFCSHAPEREPRESENTATION(" + ref(body) + ",'Body','SweptSolid'," + refs({solid}) + ")");
    const int shape = f.add("IFCPRODUCTDEFINITIONSHAPE($,$," + refs({rep}) + ")");
    const int placement = f.placement(storey.placement, 0, 0, 0);
    // Its use and finishes (docs/115, docs/117): the use as its object type, the finishes in
    // Pset_SpaceCoveringRequirements, the ceiling height as a quantity.
    const Object* marker = document.find(room.id);
    const RoomFinish finish = marker ? roomFinishOf(*marker) : RoomFinish{};
    const int space = f.add("IFCSPACE(" + guid("object:" + room.id) + ",$," + text(room.name) + ",$," +
                            (finish.use.empty() ? std::string("$") : text(finish.use)) + "," + ref(placement) + "," + ref(shape) + "," +
                            text(room.name) + ",.ELEMENT.,.SPACE.,$)");
    std::vector<int> q = {area("GrossFloorArea", room.centreArea), length("Height", height)};
    if (room.innerKnown) {
      q.push_back(area("NetFloorArea", room.innerArea));
      q.push_back(length("NetPerimeter", room.innerPerimeter));
    }
    if (finish.ceilingHeight > 0) q.push_back(length("FinishCeilingHeight", finish.ceilingHeight));
    quantities(room.id, space, "Qto_SpaceBaseQuantities", q);
    std::vector<int> coverings;
    auto label = [&](const char* key, const std::string& value) {
      if (!value.empty()) coverings.push_back(f.add(std::string("IFCPROPERTYSINGLEVALUE('") + key + "',$,IFCLABEL(" + text(value) + "),$)"));
    };
    label("FloorCovering", finish.floor);
    label("WallCovering", finish.wall);
    label("CeilingCovering", finish.ceiling);
    if (!coverings.empty())
      relate("finish:" + room.id, space,
             f.add("IFCPROPERTYSET(" + guid("pset-finish:" + room.id) + ",$,'Pset_SpaceCoveringRequirements',$," + refs(coverings) + ")"));
    storey.spaces.push_back(space);
    ++summary.spaces;
    std::vector<int> covered;
    for (const auto& [roomId, covering] : ceilings)
      if (roomId == room.id) covered.push_back(covering);
    if (!covered.empty()) f.add("IFCRELCOVERSSPACES(" + guid("rel:covers:" + room.id) + ",$,$,$," + ref(space) + "," + refs(covered) + ")");
  }

  // --- grids ---
  for (const Object& o : document.objects()) {
    const GridSpec* grid = modeler.gridSpec(o.id);
    if (!grid || grid->x.empty() || grid->y.empty() || !document.isVisible(o.id)) continue;
    const gp_Trsf world = document.worldTransform(o);
    auto range = [](const std::vector<GridAxis>& axes) {
      double lo = 1e300, hi = -1e300;
      for (const GridAxis& a : axes) {
        lo = std::min(lo, a.offset);
        hi = std::max(hi, a.offset);
      }
      return std::make_pair(lo - 1500.0, hi + 1500.0);
    };
    const auto [x0, x1] = range(grid->x);
    const auto [y0, y1] = range(grid->y);
    Storey& storey = storeys.front();
    auto axis = [&](const GridAxis& a, bool alongY) {
      const gp_Pnt p = (alongY ? gp_Pnt(a.offset, y0, 0) : gp_Pnt(x0, a.offset, 0)).Transformed(world);
      const gp_Pnt q = (alongY ? gp_Pnt(a.offset, y1, 0) : gp_Pnt(x1, a.offset, 0)).Transformed(world);
      const int line = f.add("IFCPOLYLINE(" + refs({f.point2(p.X(), p.Y()), f.point2(q.X(), q.Y())}) + ")");
      return f.add("IFCGRIDAXIS(" + text(a.name) + "," + ref(line) + ",.T.)");
    };
    std::vector<int> us, vs;
    for (const GridAxis& a : grid->x) us.push_back(axis(a, true));
    for (const GridAxis& a : grid->y) vs.push_back(axis(a, false));
    const int placement = f.placement(storey.placement, 0, 0, -storey.elevation);
    storey.elements.push_back(f.add("IFCGRID(" + guid("object:" + o.id) + ",$," + text(o.name) + ",$,$," + ref(placement) + ",$," + refs(us) + "," +
                                    refs(vs) + ",$,.RECTANGULAR.)"));
    ++summary.grids;
  }

  // --- what is in each storey ---
  for (const Storey& s : storeys) {
    if (!s.elements.empty())
      f.add("IFCRELCONTAINEDINSPATIALSTRUCTURE(" + guid("rel:contains:" + s.id) + ",$,$,$," + refs(s.elements) + "," + ref(s.entity) + ")");
    if (!s.spaces.empty()) f.add("IFCRELAGGREGATES(" + guid("rel:spaces:" + s.id) + ",$,$,$," + ref(s.entity) + "," + refs(s.spaces) + ")");
  }
  if (meshedAny) summary.notes.push_back("Curved faces are written as triangles");
  if (summary.skipped > 0)
    summary.notes.push_back(std::to_string(summary.skipped) + " visible objects without volume (lines, faces, notes) are not exported");
  const int elements = summary.stairs + summary.walls + summary.slabs + summary.roofs + summary.columns + summary.beams + summary.members + summary.proxies + summary.coverings +
                       summary.spaces + summary.grids;
  if (elements == 0) return R::failure("Nothing to export: the model has no solids, rooms or grids");

  // --- the file ---
  std::time_t now = std::time(nullptr);
  char stamp[32] = "1970-01-01T00:00:00";
  if (const std::tm* utc = std::gmtime(&now)) std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", utc);
  std::ostringstream out;
  out << "ISO-10303-21;\nHEADER;\n"
      << "FILE_DESCRIPTION(('ViewDefinition [ReferenceView_V1.2]'),'2;1');\n"
      << "FILE_NAME(" << text(fileName) << ",'" << stamp << "',(''),(''),'SketchBRep','SketchBRep','');\n"
      << "FILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n";
  for (std::size_t i = 0; i < f.lines.size(); ++i) out << '#' << (i + 1) << '=' << f.lines[i] << ";\n";
  out << "ENDSEC;\nEND-ISO-10303-21;\n";
  if (summaryOut) *summaryOut = summary;
  return out.str();
}

Result<IfcSummary> exportIfc(const Modeler& modeler, const std::string& path) {
  using R = Result<IfcSummary>;
  std::string name = path;
  if (const auto slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
  IfcSummary summary;
  const auto content = ifcText(modeler, name, &summary);
  if (!content) return R::failure(content.error());
  std::ofstream file(std::filesystem::u8path(path), std::ios::binary);
  if (!file) return R::failure("Could not write the file");
  file << content.value();
  file.close();
  if (!file) return R::failure("Could not write the file");
  return summary;
}

}  // namespace sbr::doc
