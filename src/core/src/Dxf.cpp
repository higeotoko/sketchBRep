#include "sbr/geom/Dxf.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <Precision.hxx>
#include <gp.hxx>
#include <locale>
#include <sstream>
#include <stdexcept>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <Standard_Failure.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Compound.hxx>
#include <BRep_Builder.hxx>

namespace sbr::geom {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr std::size_t maxPairs = 2000000;
constexpr std::size_t maxEntities = 100000;
struct Pair { int code; std::string value; };
struct Entity {
  std::string type;
  std::vector<Pair> data;
  std::vector<Entity> vertices;
  std::string text(int code, const std::string& fallback = {}) const {
    for (const auto& p : data) if (p.code == code) return p.value;
    return fallback;
  }
  double number(int code, double fallback = 0) const;
};
struct Block { gp_Pnt base; std::vector<Entity> entities; };

std::string trim(const std::string& s) {
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
double numeric(const std::string& s) {
  std::istringstream in(s);
  in.imbue(std::locale::classic());
  double value;
  if (!(in >> value) || !std::isfinite(value)) throw std::runtime_error("Invalid DXF number");
  in >> std::ws;
  if (!in.eof()) throw std::runtime_error("Invalid DXF number suffix");
  return value;
}
int integer(const std::string& s) {
  const double value = numeric(s);
  if (std::floor(value) != value || value < -2147483647.0 || value > 2147483647.0)
    throw std::runtime_error("Invalid DXF integer");
  return static_cast<int>(value);
}
double Entity::number(int code, double fallback) const {
  for (const auto& p : data) if (p.code == code) return numeric(p.value);
  return fallback;
}
gp_Pnt point(const Entity& e, int code = 10) {
  if (e.text(code).empty() || e.text(code + 10).empty()) throw std::runtime_error("Missing DXF coordinate");
  return {e.number(code), e.number(code + 10), e.number(code + 20)};
}
// DXF Unicode escapes also occur in older ASCII files, even for layer names.
std::string name(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s.compare(i, 3, "\\U+") == 0 && i + 7 <= s.size()) {
      const std::string hex = s.substr(i + 3, 4);
      if (hex.find_first_not_of("0123456789ABCDEFabcdef") != std::string::npos)
        throw std::runtime_error("Invalid DXF Unicode escape");
      const auto c = std::stoul(hex, nullptr, 16);
      if (c >= 0xd800 && c <= 0xdfff) throw std::runtime_error("Unsupported DXF surrogate name");
      if (c < 128) out += static_cast<char>(c);
      else if (c < 2048) { out += static_cast<char>(0xc0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 63)); }
      else { out += static_cast<char>(0xe0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 63)); out += static_cast<char>(0x80 | (c & 63)); }
      i += 6;
    } else out += s[i];
  }
  return out;
}

double unitScale(int unit) {
  switch (unit) {
    case 1: return 25.4; case 2: return 304.8; case 3: return 1609344;
    case 4: return 1; case 5: return 10; case 6: return 1000; case 7: return 1000000;
    case 8: return 0.0000254; case 9: return 0.0254; case 10: return 914.4;
    case 11: return 1e-7; case 12: return 1e-6; case 13: return 0.001;
    case 14: return 100; case 15: return 10000; case 16: return 100000;
    case 21: return 1200000.0 / 3937;
    default: throw std::runtime_error("Unsupported DXF $INSUNITS; convert the drawing to mm first");
  }
}

TopoDS_Edge segment(const gp_Pnt& a, const gp_Pnt& b, double bulge) {
  if (a.Distance(b) <= Precision::Confusion()) throw std::runtime_error("Zero-length DXF segment");
  if (bulge == 0) return BRepBuilderAPI_MakeEdge(a, b).Edge();
  // bulge = tan(sweep/4); the sagitta point preserves major and clockwise arcs.
  const gp_Pnt mid((a.X() + b.X()) / 2 + (b.Y() - a.Y()) * bulge / 2,
                   (a.Y() + b.Y()) / 2 - (b.X() - a.X()) * bulge / 2, a.Z());
  GC_MakeArcOfCircle arc(a, mid, b);
  if (!arc.IsDone()) throw std::runtime_error("Invalid DXF bulge arc");
  return BRepBuilderAPI_MakeEdge(arc.Value()).Edge();
}

TopoDS_Shape curve(const Entity& e) {
  // These 2D entities use +Z OCS. Refuse other planes rather than flattening them.
  if (e.number(210) != 0 || e.number(220) != 0 || e.number(230, 1) != 1 || e.number(39) != 0)
    throw std::runtime_error("non-XY extrusion/thickness");
  if (e.type == "LINE") {
    const auto a = point(e), b = point(e, 11);
    if (a.Z() != b.Z()) throw std::runtime_error("3D line");
    return segment(a, b, 0);
  }
  if (e.type == "CIRCLE" || e.type == "ARC") {
    const double radius = e.number(40);
    if (radius <= 0) throw std::runtime_error("Invalid DXF radius");
    gp_Circ circle(gp_Ax2(point(e), gp::DZ()), radius);
    if (e.type == "CIRCLE") return BRepBuilderAPI_MakeEdge(circle).Edge();
    const double start = e.number(50) * pi / 180;
    double sweep = std::fmod((e.number(51) - e.number(50)) * pi / 180, 2 * pi);
    if (sweep <= 0) sweep += 2 * pi;
    return BRepBuilderAPI_MakeEdge(circle, start, start + sweep).Edge();
  }
  if (e.type == "ELLIPSE") {
    const gp_Pnt major = point(e, 11);
    if (major.Z() != 0) throw std::runtime_error("3D ellipse");
    const double radius = std::hypot(major.X(), major.Y()), ratio = e.number(40);
    if (radius <= 0 || ratio <= 0 || ratio > 1) throw std::runtime_error("Invalid DXF ellipse");
    const gp_Elips ellipse(gp_Ax2(point(e), gp::DZ(), gp_Dir(major.X(), major.Y(), 0)), radius, radius * ratio);
    const double start = e.number(41), end = e.number(42, 2 * pi);
    if (std::abs(end - start) >= 2 * pi - 1e-12) return BRepBuilderAPI_MakeEdge(ellipse).Edge();
    double sweep = std::fmod(end - start, 2 * pi);
    if (sweep <= 0) sweep += 2 * pi;
    return BRepBuilderAPI_MakeEdge(ellipse, start, start + sweep).Edge();
  }
  if (e.type == "SPLINE") {
    const int flags = integer(e.text(70, "0")), degree = integer(e.text(71, "0"));
    if ((flags & 2) || degree < 1 || degree > 25) throw std::runtime_error("periodic/invalid spline");
    std::vector<gp_Pnt> points;
    std::vector<double> knots, weights;
    bool hasY = true;
    for (const auto& p : e.data) {
      if (p.code == 10) {
        if (!hasY) throw std::runtime_error("Missing spline Y");
        points.emplace_back(numeric(p.value), 0, 0); hasY = false;
      } else if (p.code == 20 || p.code == 30) {
        if (points.empty()) throw std::runtime_error("Invalid spline control point");
        if (p.code == 20) { points.back().SetY(numeric(p.value)); hasY = true; }
        else points.back().SetZ(numeric(p.value));
      } else if (p.code == 40) knots.push_back(numeric(p.value));
      else if (p.code == 41) weights.push_back(numeric(p.value));
    }
    if (!hasY || points.size() < 2 || points.size() != static_cast<std::size_t>(integer(e.text(73, "0"))) ||
        knots.size() != static_cast<std::size_t>(integer(e.text(72, "0"))) ||
        knots.size() != points.size() + degree + 1 || (!weights.empty() && weights.size() != points.size()))
      throw std::runtime_error("Invalid spline counts");
    for (const auto& p : points) if (p.Z() != points.front().Z()) throw std::runtime_error("3D spline");
    std::vector<double> unique;
    std::vector<int> multiplicities;
    for (double knot : knots) {
      if (!unique.empty() && knot < unique.back()) throw std::runtime_error("Unordered spline knots");
      if (!unique.empty() && knot == unique.back()) ++multiplicities.back();
      else { unique.push_back(knot); multiplicities.push_back(1); }
    }
    if (unique.size() < 2) throw std::runtime_error("Empty spline interval");
    TColgp_Array1OfPnt poles(1, static_cast<int>(points.size()));
    TColStd_Array1OfReal w(1, poles.Length()), k(1, static_cast<int>(unique.size()));
    TColStd_Array1OfInteger mult(1, k.Length());
    for (int i = 1; i <= poles.Length(); ++i) {
      poles(i) = points[i - 1]; w(i) = weights.empty() ? 1 : weights[i - 1];
      if (w(i) <= 0) throw std::runtime_error("Invalid spline weight");
    }
    for (int i = 1; i <= k.Length(); ++i) { k(i) = unique[i - 1]; mult(i) = multiplicities[i - 1]; }
    Handle(Geom_BSplineCurve) spline = new Geom_BSplineCurve(poles, w, k, mult, degree, false);
    return BRepBuilderAPI_MakeEdge(spline).Edge();
  }
  if (e.type == "LWPOLYLINE" || e.type == "POLYLINE") {
    const int flags = integer(e.text(70, "0"));
    if (flags & ~129) throw std::runtime_error("3D/fitted polyline");
    struct Vertex { gp_Pnt p; double bulge = 0; };
    std::vector<Vertex> vertices;
    if (e.type == "LWPOLYLINE") {
      bool hasY = true;
      for (const auto& p : e.data) {
        if (p.code == 10) {
          if (!hasY) throw std::runtime_error("Missing polyline Y");
          vertices.push_back({gp_Pnt(numeric(p.value), 0, e.number(38)), 0}); hasY = false;
        } else if (p.code == 20 || p.code == 42) {
          if (vertices.empty()) throw std::runtime_error("Invalid polyline vertex order");
          if (p.code == 20) { vertices.back().p.SetY(numeric(p.value)); hasY = true; }
          else vertices.back().bulge = numeric(p.value);
        } else if ((p.code == 40 || p.code == 41 || p.code == 43) && numeric(p.value) != 0)
          throw std::runtime_error("wide polyline");
      }
      if (!hasY || integer(e.text(90, "-1")) != static_cast<int>(vertices.size()))
        throw std::runtime_error("Invalid polyline vertex count");
    } else {
      if (e.number(40) != 0 || e.number(41) != 0) throw std::runtime_error("wide polyline");
      for (const auto& v : e.vertices) {
        if (v.number(70) != 0 || v.number(40) != 0 || v.number(41) != 0 || v.number(30) != 0)
          throw std::runtime_error("non-2D polyline vertex");
        auto p = point(v); p.SetZ(e.number(30));
        vertices.push_back({p, v.number(42)});
      }
    }
    if (vertices.size() < 2) throw std::runtime_error("Too few polyline vertices");
    BRepBuilderAPI_MakeWire wire;
    const std::size_t count = (flags & 1) ? vertices.size() : vertices.size() - 1;
    for (std::size_t i = 0; i < count; ++i) {
      wire.Add(segment(vertices[i].p, vertices[(i + 1) % vertices.size()].p, vertices[i].bulge));
      if (!wire.IsDone()) throw std::runtime_error("Disconnected DXF polyline");
    }
    return wire.Wire();
  }
  throw std::runtime_error("unsupported entity");
}
}  // namespace

std::string DxfImportReport::summary() const {
  std::ostringstream out;
  out << imported << " curves; scale to mm: " << scaleToMm;
  if (!unitsKnown) out << " (unitless override)";
  for (const auto& [type, count] : omitted) out << "; omitted " << type << ": " << count;
  if (paperSpace) out << "; paper space: " << paperSpace;
  return out.str();
}

Result<std::vector<NamedShape>> readDxf(const std::string& path, const DxfImportOptions& options,
                                      DxfImportReport* output) {
  DxfImportReport local;
  auto& report = output ? *output : local;
  report = {};
  try {
    if (!std::isfinite(options.unitlessScaleToMm) || options.unitlessScaleToMm <= 0)
      throw std::runtime_error("DXF unit scale must be positive and finite");
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open DXF file");
    file.seekg(0, std::ios::end);
    if (file.tellg() > 64 * 1024 * 1024) throw std::runtime_error("DXF exceeds the 64 MiB import limit");
    file.seekg(0);
    std::vector<Pair> pairs;
    std::string code, value;
    while (std::getline(file, code)) {
      if (pairs.empty() && code.compare(0, 3, "\xef\xbb\xbf") == 0) code.erase(0, 3);
      if (!std::getline(file, value)) throw std::runtime_error("Truncated DXF group pair");
      const int key = integer(trim(code));
      if (key < 0 || key > 1071) throw std::runtime_error("Invalid DXF group code (ASCII DXF required)");
      if (key != 999) pairs.push_back({key, trim(value)});
      if (pairs.size() > maxPairs) throw std::runtime_error("DXF has too many groups");
    }
    if (pairs.empty() || pairs.back().code != 0 || pairs.back().value != "EOF")
      throw std::runtime_error("DXF EOF marker missing");
    std::vector<Entity> entities;
    std::map<std::string, Block> blocks;
    std::string section, blockName, header, version, codepage;
    bool entitySection = false;
    int units = 0;
    for (std::size_t i = 0; i < pairs.size();) {
      const auto p = pairs[i++];
      if (p.code == 0 && p.value == "SECTION") {
        if (!section.empty() || i == pairs.size() || pairs[i].code != 2) throw std::runtime_error("Invalid DXF section");
        section = pairs[i++].value; header.clear();
        if (section == "ENTITIES") entitySection = true;
        continue;
      }
      if (p.code == 0 && p.value == "ENDSEC") { section.clear(); continue; }
      if (section == "HEADER") {
        if (p.code == 9) header = p.value;
        if (header == "$INSUNITS" && p.code == 70) units = integer(p.value);
        if (header == "$ACADVER" && p.code == 1) version = p.value;
        if (header == "$DWGCODEPAGE" && p.code == 3) codepage = p.value;
        continue;
      }
      if ((section != "ENTITIES" && section != "BLOCKS") || p.code != 0) continue;
      Entity e{p.value, {}, {}};
      while (i < pairs.size() && pairs[i].code != 0) e.data.push_back(pairs[i++]);
      if (e.type == "POLYLINE") {
        while (i < pairs.size() && pairs[i].code == 0 && pairs[i].value == "VERTEX") {
          ++i; Entity v{"VERTEX", {}, {}};
          while (i < pairs.size() && pairs[i].code != 0) v.data.push_back(pairs[i++]);
          e.vertices.push_back(std::move(v));
        }
        if (i == pairs.size() || pairs[i].value != "SEQEND") throw std::runtime_error("Unterminated DXF POLYLINE");
        ++i;
        while (i < pairs.size() && pairs[i].code != 0) ++i;
      }
      if (section == "BLOCKS") {
        if (e.type == "BLOCK") {
          blockName = e.text(2);
          if (blockName.empty() || blocks.count(blockName)) throw std::runtime_error("Invalid DXF block name");
          blocks[blockName].base = point(e);
        } else if (e.type == "ENDBLK") blockName.clear();
        else if (!blockName.empty()) blocks[blockName].entities.push_back(std::move(e));
      } else entities.push_back(std::move(e));
    }
    if (!section.empty() || !entitySection) throw std::runtime_error("DXF ENTITIES section missing or incomplete");
    // Modern DXF is UTF-8 regardless of DWGCODEPAGE. Older raw code pages
    // need explicit conversion, rather than making corrupted UTF-8 tag names.
    if (version < "AC1021" && codepage != "UTF-8") {
      for (const auto& item : pairs) for (unsigned char c : item.value)
        if (c >= 128) throw std::runtime_error("Legacy DXF text encoding: export UTF-8 (AutoCAD 2007+) DXF first");
    }
    report.unitsKnown = units != 0;
    report.scaleToMm = units ? unitScale(units) : options.unitlessScaleToMm;
    std::vector<NamedShape> result;
    std::size_t visited = 0;
    std::function<void(const Entity&, const gp_Trsf&, const std::string&, int)> add;
    add = [&](const Entity& e, const gp_Trsf& transform, const std::string& parentLayer, int depth) {
      if (++visited > maxEntities) throw std::runtime_error("DXF exceeds the 100000 entity import limit");
      if (e.number(67) == 1) { ++report.paperSpace; return; }
      std::string layer = name(e.text(8, "0"));
      if (layer == "0" && !parentLayer.empty()) layer = parentLayer;
      if (e.type == "INSERT") {
        const auto b = blocks.find(e.text(2));
        const double scale = e.number(41, 1);
        if (depth >= 16 || b == blocks.end() || scale <= 0 || scale != e.number(42, 1) || scale != e.number(43, 1) ||
            e.number(70, 1) != 1 || e.number(71, 1) != 1 || e.number(66) != 0 ||
            e.number(210) != 0 || e.number(220) != 0 || e.number(230, 1) != 1) {
          ++report.omitted["INSERT (unsupported transform/attributes or missing/recursive block)"]; return;
        }
        gp_Trsf t, rotation, scaling, origin;
        origin.SetTranslation(gp_Vec(b->second.base, gp_Pnt(0, 0, 0)));
        scaling.SetScale(gp_Pnt(0, 0, 0), scale);
        rotation.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp::DZ()), e.number(50) * pi / 180);
        t.SetTranslation(gp_Vec(gp_Pnt(0, 0, 0), point(e)));
        t = transform * t * rotation * scaling * origin;
        for (const auto& child : b->second.entities) add(child, t, layer, depth + 1);
        return;
      }
      try {
        auto shape = curve(e);
        shape = BRepBuilderAPI_Transform(shape, transform, true).Shape();
        if (!BRepCheck_Analyzer(shape).IsValid()) throw std::runtime_error("invalid shape");
        result.push_back({shape, e.type, std::nullopt, layer});
        ++report.imported;
      } catch (const Standard_Failure&) { ++report.omitted[e.type + " (invalid geometry)"]; }
        catch (const std::exception& err) { ++report.omitted[e.type + " (" + err.what() + ")"]; }
    };
    gp_Trsf scale; scale.SetScale(gp_Pnt(0, 0, 0), report.scaleToMm);
    for (const auto& e : entities) add(e, scale, {}, 0);
    if (result.empty()) throw std::runtime_error("No supported 2D curves in DXF; " + report.summary());
    if (options.groupByLayer) {
      BRep_Builder builder;
      std::map<std::string, TopoDS_Compound> layers;
      for (const auto& part : result) {
        auto [it, inserted] = layers.try_emplace(part.layer);
        if (inserted) builder.MakeCompound(it->second);
        builder.Add(it->second, part.shape);
      }
      result.clear();
      for (const auto& [layer, shape] : layers)
        result.push_back({shape, layer, std::nullopt, layer});
    }
    return result;
  } catch (const Standard_Failure& err) {
    return Result<std::vector<NamedShape>>::failure(std::string("DXF: ") + err.GetMessageString());
  } catch (const std::exception& err) {
    return Result<std::vector<NamedShape>>::failure(err.what());
  }
}
}  // namespace sbr::geom
