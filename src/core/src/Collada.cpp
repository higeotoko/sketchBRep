#include "sbr/geom/Collada.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <sstream>

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools_History.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <gp_Pln.hxx>

#include "sbr/display/DisplayMesh.h"

namespace sbr::geom {
namespace {

// --- writing ---------------------------------------------------------------------

std::string xmlEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string number(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.6g", std::abs(v) < 1e-12 ? 0.0 : v);
  return buf;
}

// Corners shared between the triangles of one part (SketchUp joins faces
// along shared corners only).
struct Welder {
  std::map<std::array<long long, 3>, int> index;
  std::vector<gp_Pnt> points;
  int add(const gp_Pnt& p) {
    const std::array<long long, 3> key{std::llround(p.X() * 1e4), std::llround(p.Y() * 1e4), std::llround(p.Z() * 1e4)};
    const auto [it, fresh] = index.emplace(key, static_cast<int>(points.size()));
    if (fresh) points.push_back(p);
    return it->second;
  }
};

}  // namespace

std::string ColladaReport::summary() const {
  std::ostringstream out;
  out << parts << " parts (" << solids << " solids) from " << triangles << " faces";
  if (lines > 0) out << " and " << lines << " lines";
  out << ", units " << (units.empty() ? std::string("unknown") : units);
  if (degenerate > 0) out << "; " << degenerate << " tiny or bent faces left out";
  if (textured > 0) out << "; " << textured << " textures shown as colours";
  for (const auto& [kind, count] : unsupported) out << "; not supported: " << kind << " " << count;
  return out.str();
}

Status writeCollada(const std::vector<ColladaPart>& parts, const std::string& path,
                    const ColladaExportOptions& options) {
  if (parts.empty()) return Status::failure("Nothing to export");
  try {
    std::vector<ColladaColor> colors;
    auto colorIndex = [&](const ColladaColor& c) {
      const auto it = std::find(colors.begin(), colors.end(), c);
      if (it != colors.end()) return static_cast<int>(it - colors.begin());
      colors.push_back(c);
      return static_cast<int>(colors.size() - 1);
    };
    struct Geometry {
      std::string name;
      Welder welder;
      std::map<int, std::vector<int>> triangles;  // colour -> corner indices
      std::vector<int> lines;                     // pairs of corner indices
    };
    std::vector<Geometry> geometries;
    for (const ColladaPart& part : parts) {
      if (part.shape.IsNull()) continue;
      Geometry g;
      g.name = part.name;
      std::map<int, int> faceColor;  // face index -> colour index
      for (const auto& [index, color] : part.faceColors) faceColor[index] = colorIndex(color);
      const int whole = colorIndex(part.color);
      TopTools_IndexedMapOfShape faces;
      TopExp::MapShapes(part.shape, TopAbs_FACE, faces);
      if (faces.Extent() > 0) {
        const TopoDS_Shape mesh = display::makeDisplayCopy(part.shape);
        BRepMesh_IncrementalMesh mesher(mesh, options.linearDeflection, Standard_False, options.angularDeflection,
                                        Standard_True);
        TopTools_IndexedMapOfShape meshed;
        TopExp::MapShapes(mesh, TopAbs_FACE, meshed);
        for (int i = 1; i <= meshed.Extent(); ++i) {
          const TopoDS_Face face = TopoDS::Face(meshed(i));
          TopLoc_Location location;
          const auto triangulation = BRep_Tool::Triangulation(face, location);
          if (triangulation.IsNull()) continue;
          const gp_Trsf t = location.Transformation();
          const bool reverse = (face.Orientation() == TopAbs_REVERSED) != t.IsNegative();
          const auto it = faceColor.find(i);
          std::vector<int>& out = g.triangles[it != faceColor.end() ? it->second : whole];
          for (int k = 1; k <= triangulation->NbTriangles(); ++k) {
            int a, b, c;
            triangulation->Triangle(k).Get(a, b, c);
            if (reverse) std::swap(b, c);
            const gp_Pnt pa = triangulation->Node(a).Transformed(t), pb = triangulation->Node(b).Transformed(t),
                         pc = triangulation->Node(c).Transformed(t);
            if (gp_Vec(pa, pb).Crossed(gp_Vec(pa, pc)).SquareMagnitude() < 1e-12) continue;
            out.push_back(g.welder.add(pa));
            out.push_back(g.welder.add(pb));
            out.push_back(g.welder.add(pc));
          }
        }
      }
      // Loose edges (lines, arcs, curves drawn on their own).
      TopTools_IndexedMapOfShape faceEdges;
      for (int i = 1; i <= faces.Extent(); ++i) TopExp::MapShapes(faces(i), TopAbs_EDGE, faceEdges);
      for (TopExp_Explorer e(part.shape, TopAbs_EDGE); e.More(); e.Next()) {
        if (faceEdges.Contains(e.Current())) continue;
        const TopoDS_Edge edge = TopoDS::Edge(e.Current());
        if (BRep_Tool::Degenerated(edge)) continue;
        BRepAdaptor_Curve curve(edge);
        GCPnts_QuasiUniformDeflection points(curve, options.linearDeflection);
        if (!points.IsDone() || points.NbPoints() < 2) continue;
        for (int k = 1; k < points.NbPoints(); ++k) {
          g.lines.push_back(g.welder.add(points.Value(k)));
          g.lines.push_back(g.welder.add(points.Value(k + 1)));
        }
      }
      if (!g.welder.points.empty()) geometries.push_back(std::move(g));
    }
    if (geometries.empty()) return Status::failure("Nothing to export (no faces or lines)");

    std::ofstream out(path, std::ios::binary);
    if (!out) return Status::failure("Cannot write " + path);
    out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        << "<COLLADA xmlns=\"http://www.collada.org/2005/11/COLLADASchema\" version=\"1.4.1\">\n"
        << " <asset><contributor><authoring_tool>SketchBRep</authoring_tool></contributor>"
        << "<unit name=\"millimeter\" meter=\"0.001\"/><up_axis>Z_UP</up_axis></asset>\n";
    out << " <library_effects>\n";
    for (std::size_t i = 0; i < colors.size(); ++i) {
      const ColladaColor& c = colors[i];
      out << "  <effect id=\"m" << i << "-fx\"><profile_COMMON><technique sid=\"common\"><lambert><diffuse><color>"
          << number(c.r) << ' ' << number(c.g) << ' ' << number(c.b) << " 1</color></diffuse>";
      if (c.a < 1.0) {
        out << "<transparent opaque=\"A_ONE\"><color>1 1 1 " << number(c.a) << "</color></transparent>"
            << "<transparency><float>1</float></transparency>";
      }
      out << "</lambert></technique></profile_COMMON></effect>\n";
    }
    out << " </library_effects>\n <library_materials>\n";
    for (std::size_t i = 0; i < colors.size(); ++i) {
      out << "  <material id=\"m" << i << "\" name=\"m" << i << "\"><instance_effect url=\"#m" << i
          << "-fx\"/></material>\n";
    }
    out << " </library_materials>\n <library_geometries>\n";
    for (std::size_t gi = 0; gi < geometries.size(); ++gi) {
      const Geometry& g = geometries[gi];
      const std::string id = "g" + std::to_string(gi);
      out << "  <geometry id=\"" << id << "\" name=\"" << xmlEscape(g.name) << "\"><mesh>\n";
      out << "   <source id=\"" << id << "-p\"><float_array id=\"" << id << "-pa\" count=\"" << g.welder.points.size() * 3
          << "\">";
      for (const gp_Pnt& p : g.welder.points) out << number(p.X()) << ' ' << number(p.Y()) << ' ' << number(p.Z()) << ' ';
      out << "</float_array><technique_common><accessor source=\"#" << id << "-pa\" count=\"" << g.welder.points.size()
          << "\" stride=\"3\"><param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/>"
          << "<param name=\"Z\" type=\"float\"/></accessor></technique_common></source>\n";
      out << "   <vertices id=\"" << id << "-v\"><input semantic=\"POSITION\" source=\"#" << id << "-p\"/></vertices>\n";
      for (const auto& [color, corners] : g.triangles) {
        if (corners.empty()) continue;
        out << "   <triangles material=\"m" << color << "\" count=\"" << corners.size() / 3
            << "\"><input semantic=\"VERTEX\" source=\"#" << id << "-v\" offset=\"0\"/><p>";
        for (int c : corners) out << c << ' ';
        out << "</p></triangles>\n";
      }
      if (!g.lines.empty()) {
        out << "   <lines count=\"" << g.lines.size() / 2 << "\"><input semantic=\"VERTEX\" source=\"#" << id
            << "-v\" offset=\"0\"/><p>";
        for (int c : g.lines) out << c << ' ';
        out << "</p></lines>\n";
      }
      out << "  </mesh></geometry>\n";
    }
    out << " </library_geometries>\n <library_visual_scenes><visual_scene id=\"scene\">\n";
    for (std::size_t gi = 0; gi < geometries.size(); ++gi) {
      const Geometry& g = geometries[gi];
      out << "  <node id=\"n" << gi << "\" name=\"" << xmlEscape(g.name) << "\"><instance_geometry url=\"#g" << gi
          << "\"><bind_material><technique_common>";
      for (const auto& [color, corners] : g.triangles) {
        if (!corners.empty()) out << "<instance_material symbol=\"m" << color << "\" target=\"#m" << color << "\"/>";
      }
      out << "</technique_common></bind_material></instance_geometry></node>\n";
    }
    out << " </visual_scene></library_visual_scenes>\n <scene><instance_visual_scene url=\"#scene\"/></scene>\n"
        << "</COLLADA>\n";
    if (!out) return Status::failure("Cannot write " + path);
    return Status::success();
  } catch (const Standard_Failure& e) {
    return Status::failure(std::string("COLLADA export failed: ") + e.GetMessageString());
  }
}

// --- reading ---------------------------------------------------------------------

namespace {

// A small XML tree: enough for COLLADA (elements, attributes, text;
// comments, processing instructions, DOCTYPE and CDATA skipped or kept as
// text). Namespace prefixes are dropped from names.
struct Xml {
  std::string name;
  std::map<std::string, std::string> attrs;
  std::string text;
  std::vector<std::unique_ptr<Xml>> children;
  const Xml* child(const std::string& n) const {
    for (const auto& c : children) {
      if (c->name == n) return c.get();
    }
    return nullptr;
  }
  std::vector<const Xml*> all(const std::string& n) const {
    std::vector<const Xml*> out;
    for (const auto& c : children) {
      if (c->name == n) out.push_back(c.get());
    }
    return out;
  }
  std::string attr(const std::string& n) const {
    const auto it = attrs.find(n);
    return it == attrs.end() ? std::string() : it->second;
  }
};

std::string unescape(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out += s[i];
      continue;
    }
    const std::size_t end = s.find(';', i);
    if (end == std::string::npos) {
      out += s[i];
      continue;
    }
    const std::string e = s.substr(i + 1, end - i - 1);
    if (e == "lt") out += '<';
    else if (e == "gt") out += '>';
    else if (e == "amp") out += '&';
    else if (e == "quot") out += '"';
    else if (e == "apos") out += '\'';
    else if (!e.empty() && e[0] == '#') {
      unsigned long code = e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? std::stoul(e.substr(2), nullptr, 16)
                                                                         : std::stoul(e.substr(1));
      // UTF-8
      if (code < 0x80) out += static_cast<char>(code);
      else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
      } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
      } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
      }
    } else {
      out += s.substr(i, end - i + 1);
    }
    i = end;
  }
  return out;
}

Result<std::unique_ptr<Xml>> parseXml(const std::string& s) {
  using R = Result<std::unique_ptr<Xml>>;
  auto root = std::make_unique<Xml>();
  std::vector<Xml*> stack{root.get()};
  std::size_t i = 0;
  auto stripPrefix = [](std::string n) {
    const auto colon = n.find(':');
    return colon == std::string::npos ? n : n.substr(colon + 1);
  };
  while (i < s.size()) {
    if (s[i] != '<') {
      const std::size_t next = s.find('<', i);
      const std::size_t end = next == std::string::npos ? s.size() : next;
      stack.back()->text += unescape(s.substr(i, end - i));
      i = end;
      continue;
    }
    if (s.compare(i, 4, "<!--") == 0) {
      const std::size_t end = s.find("-->", i);
      if (end == std::string::npos) return R::failure("Broken XML (comment)");
      i = end + 3;
    } else if (s.compare(i, 9, "<![CDATA[") == 0) {
      const std::size_t end = s.find("]]>", i);
      if (end == std::string::npos) return R::failure("Broken XML (CDATA)");
      stack.back()->text += s.substr(i + 9, end - i - 9);
      i = end + 3;
    } else if (s.compare(i, 2, "<?") == 0 || s.compare(i, 2, "<!") == 0) {
      const std::size_t end = s.find('>', i);
      if (end == std::string::npos) return R::failure("Broken XML");
      i = end + 1;
    } else if (s.compare(i, 2, "</") == 0) {
      const std::size_t end = s.find('>', i);
      if (end == std::string::npos || stack.size() < 2) return R::failure("Broken XML (closing tag)");
      stack.pop_back();
      i = end + 1;
    } else {
      std::size_t j = i + 1;
      while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != '>' && s[j] != '/') ++j;
      auto node = std::make_unique<Xml>();
      node->name = stripPrefix(s.substr(i + 1, j - i - 1));
      bool selfClosing = false;
      while (j < s.size()) {
        while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) ++j;
        if (j >= s.size()) return R::failure("Broken XML (tag)");
        if (s[j] == '>') {
          ++j;
          break;
        }
        if (s[j] == '/') {
          selfClosing = true;
          j = s.find('>', j);
          if (j == std::string::npos) return R::failure("Broken XML (tag)");
          ++j;
          break;
        }
        std::size_t k = j;
        while (k < s.size() && s[k] != '=' && !std::isspace(static_cast<unsigned char>(s[k])) && s[k] != '>') ++k;
        const std::string key = stripPrefix(s.substr(j, k - j));
        while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
        if (k >= s.size() || s[k] != '=') {
          j = k;
          continue;
        }
        ++k;
        while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
        if (k >= s.size() || (s[k] != '"' && s[k] != '\'')) return R::failure("Broken XML (attribute)");
        const char quote = s[k];
        const std::size_t end = s.find(quote, k + 1);
        if (end == std::string::npos) return R::failure("Broken XML (attribute)");
        node->attrs[key] = unescape(s.substr(k + 1, end - k - 1));
        j = end + 1;
      }
      Xml* raw = node.get();
      stack.back()->children.push_back(std::move(node));
      if (!selfClosing) stack.push_back(raw);
      i = j;
    }
  }
  return root;
}

std::vector<double> numbers(const std::string& text) {
  std::vector<double> out;
  const char* p = text.c_str();
  char* end = nullptr;
  while (*p) {
    const double v = std::strtod(p, &end);
    if (end == p) {
      ++p;
      continue;
    }
    out.push_back(v);
    p = end;
  }
  return out;
}

std::vector<int> integers(const std::string& text) {
  std::vector<int> out;
  const char* p = text.c_str();
  char* end = nullptr;
  while (*p) {
    const long v = std::strtol(p, &end, 10);
    if (end == p) {
      ++p;
      continue;
    }
    out.push_back(static_cast<int>(v));
    p = end;
  }
  return out;
}

// 4 x 4, row-major (COLLADA's order).
using Matrix = std::array<double, 16>;
Matrix identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
Matrix multiply(const Matrix& a, const Matrix& b) {
  Matrix m{};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      double v = 0;
      for (int k = 0; k < 4; ++k) v += a[r * 4 + k] * b[k * 4 + c];
      m[r * 4 + c] = v;
    }
  }
  return m;
}
gp_Pnt apply(const Matrix& m, double x, double y, double z) {
  return gp_Pnt(m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7],
                m[8] * x + m[9] * y + m[10] * z + m[11]);
}
double determinant3(const Matrix& m) {
  return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
}

Matrix nodeTransform(const Xml& node) {
  Matrix m = identity();
  for (const auto& c : node.children) {
    if (c->name == "matrix") {
      const auto v = numbers(c->text);
      if (v.size() >= 16) {
        Matrix t;
        std::copy(v.begin(), v.begin() + 16, t.begin());
        m = multiply(m, t);
      }
    } else if (c->name == "translate") {
      const auto v = numbers(c->text);
      if (v.size() >= 3) m = multiply(m, Matrix{1, 0, 0, v[0], 0, 1, 0, v[1], 0, 0, 1, v[2], 0, 0, 0, 1});
    } else if (c->name == "scale") {
      const auto v = numbers(c->text);
      if (v.size() >= 3) m = multiply(m, Matrix{v[0], 0, 0, 0, 0, v[1], 0, 0, 0, 0, v[2], 0, 0, 0, 0, 1});
    } else if (c->name == "rotate") {
      const auto v = numbers(c->text);
      if (v.size() >= 4) {
        const gp_Vec axis(v[0], v[1], v[2]);
        if (axis.Magnitude() < 1e-12) continue;
        gp_Trsf r;
        r.SetRotation(gp_Ax1(gp::Origin(), gp_Dir(axis)), v[3] * M_PI / 180.0);
        Matrix t = identity();
        for (int row = 1; row <= 3; ++row) {
          for (int col = 1; col <= 3; ++col) t[(row - 1) * 4 + (col - 1)] = r.Value(row, col);
        }
        m = multiply(m, t);
      }
    }
  }
  return m;
}

struct Reader {
  const Xml& root;
  ColladaReport& report;
  std::map<std::string, const Xml*> ids;
  std::map<std::string, ColladaColor> materialColors;  // material id -> colour
  Matrix base = identity();  // units and up axis
  std::vector<ColladaPart> parts;

  void index(const Xml& n) {
    const std::string id = n.attr("id");
    if (!id.empty()) ids[id] = &n;
    for (const auto& c : n.children) index(*c);
  }
  const Xml* byUrl(const std::string& url) const {
    if (url.empty() || url[0] != '#') return nullptr;
    const auto it = ids.find(url.substr(1));
    return it == ids.end() ? nullptr : it->second;
  }

  ColladaColor effectColor(const Xml* effect) {
    ColladaColor c;
    if (!effect) return c;
    // profile_COMMON / technique / (lambert | phong | blinn | constant)
    std::function<const Xml*(const Xml&, const std::string&)> find = [&](const Xml& n, const std::string& name) -> const Xml* {
      for (const auto& ch : n.children) {
        if (ch->name == name) return ch.get();
        if (const Xml* f = find(*ch, name)) return f;
      }
      return nullptr;
    };
    if (const Xml* diffuse = find(*effect, "diffuse")) {
      if (const Xml* color = diffuse->child("color")) {
        const auto v = numbers(color->text);
        if (v.size() >= 3) c = {v[0], v[1], v[2], 1.0};
      } else if (diffuse->child("texture")) {
        ++report.textured;
        c = {0.7, 0.7, 0.7, 1.0};
      }
    }
    // Transparency (A_ONE: alpha of <transparent> times <transparency>).
    double alpha = 1.0;
    const Xml* transparent = find(*effect, "transparent");
    const Xml* transparency = find(*effect, "transparency");
    if (transparent) {
      if (const Xml* color = transparent->child("color")) {
        const auto v = numbers(color->text);
        const std::string mode = transparent->attr("opaque");
        if (mode == "RGB_ZERO" && v.size() >= 3) alpha = 1.0 - (v[0] + v[1] + v[2]) / 3.0;
        else if (v.size() >= 4) alpha = v[3];
      }
    }
    if (transparency) {
      if (const Xml* f = transparency->child("float")) {
        const auto v = numbers(f->text);
        if (!v.empty()) alpha = transparent ? alpha * v[0] : v[0];
      }
    }
    // Files that store transparency the other way round would make every
    // material invisible: an invisible colour is taken as opaque.
    c.a = alpha > 0.02 && alpha <= 1.0 ? alpha : 1.0;
    return c;
  }

  void materials() {
    for (const auto& [id, n] : ids) {
      if (n->name != "material") continue;
      const Xml* ie = n->child("instance_effect");
      materialColors[id] = effectColor(ie ? byUrl(ie->attr("url")) : nullptr);
    }
  }

  // One instance of a geometry, placed by `m`, its materials bound.
  void geometry(const Xml& instance, const Matrix& m, const std::string& name) {
    const Xml* geometry = byUrl(instance.attr("url"));
    if (!geometry) return;
    const Xml* mesh = geometry->child("mesh");
    if (!mesh) {
      ++report.unsupported[geometry->children.empty() ? "empty geometry" : geometry->children.front()->name];
      return;
    }
    std::map<std::string, std::string> bound;  // symbol -> material id
    if (const Xml* bm = instance.child("bind_material")) {
      if (const Xml* tc = bm->child("technique_common")) {
        for (const Xml* im : tc->all("instance_material")) {
          const std::string target = im->attr("target");
          bound[im->attr("symbol")] = target.empty() || target[0] != '#' ? target : target.substr(1);
        }
      }
    }
    auto colorOf = [&](const std::string& symbol) {
      const auto b = bound.find(symbol);
      const std::string id = b != bound.end() ? b->second : symbol;
      const auto it = materialColors.find(id);
      return it != materialColors.end() ? it->second : ColladaColor{};
    };
    // Positions: <vertices> -> POSITION source -> float_array (accessor stride).
    std::map<std::string, std::vector<gp_Pnt>> sources;
    auto positions = [&](const std::string& url) -> const std::vector<gp_Pnt>* {
      const Xml* n = byUrl(url);
      if (!n) return nullptr;
      if (n->name == "vertices") {
        for (const Xml* in : n->all("input")) {
          if (in->attr("semantic") == "POSITION") n = byUrl(in->attr("source"));
        }
        if (!n || n->name != "source") return nullptr;
      }
      const std::string id = n->attr("id");
      if (auto it = sources.find(id); it != sources.end()) return &it->second;
      const Xml* array = n->child("float_array");
      if (!array) return nullptr;
      const auto v = numbers(array->text);
      int stride = 3;
      if (const Xml* tc = n->child("technique_common")) {
        if (const Xml* acc = tc->child("accessor")) stride = std::max(3, std::atoi(acc->attr("stride").c_str()));
      }
      std::vector<gp_Pnt> pts;
      for (std::size_t i = 0; i + 2 < v.size(); i += stride) pts.push_back(apply(m, v[i], v[i + 1], v[i + 2]));
      return &(sources[id] = std::move(pts));
    };
    const bool mirrored = determinant3(m) < 0;
    struct Tri {
      std::vector<gp_Pnt> corners;
      ColladaColor color;
    };
    std::vector<Tri> faces;
    std::vector<std::pair<gp_Pnt, gp_Pnt>> segments;
    for (const auto& prim : mesh->children) {
      const std::string kind = prim->name;
      if (kind == "source" || kind == "vertices" || kind == "extra") continue;
      if (kind != "triangles" && kind != "polylist" && kind != "polygons" && kind != "lines" && kind != "linestrips") {
        ++report.unsupported[kind];
        continue;
      }
      int vertexOffset = 0, stride = 1;
      const std::vector<gp_Pnt>* pts = nullptr;
      for (const Xml* in : prim->all("input")) {
        const int offset = std::atoi(in->attr("offset").c_str());
        stride = std::max(stride, offset + 1);
        if (in->attr("semantic") == "VERTEX") {
          vertexOffset = offset;
          pts = positions(in->attr("source"));
        }
      }
      if (!pts) continue;
      auto corner = [&](const std::vector<int>& p, std::size_t i) -> std::optional<gp_Pnt> {
        const std::size_t at = i * stride + vertexOffset;
        if (at >= p.size() || p[at] < 0 || static_cast<std::size_t>(p[at]) >= pts->size()) return std::nullopt;
        return (*pts)[p[at]];
      };
      const ColladaColor color = colorOf(prim->attr("material"));
      std::vector<std::vector<int>> lists;
      for (const Xml* p : prim->all("p")) lists.push_back(integers(p->text));
      if (kind == "lines" || kind == "linestrips") {
        for (const auto& p : lists) {
          const std::size_t n = p.size() / stride;
          for (std::size_t i = 0; i + 1 < n; i += kind == "lines" ? 2 : 1) {
            const auto a = corner(p, i), b = corner(p, i + 1);
            if (a && b && a->Distance(*b) > 1e-9) segments.emplace_back(*a, *b);
          }
        }
        continue;
      }
      std::vector<int> counts;
      if (kind == "polylist") {
        if (const Xml* vc = prim->child("vcount")) counts = integers(vc->text);
      }
      for (const auto& p : lists) {
        const std::size_t n = p.size() / stride;
        std::vector<int> sizes;
        if (kind == "triangles") sizes.assign(n / 3, 3);
        else if (kind == "polylist") sizes = counts;
        else sizes = {static_cast<int>(n)};  // polygons: one per <p>
        std::size_t at = 0;
        for (int size : sizes) {
          Tri f;
          for (int k = 0; k < size; ++k) {
            if (const auto c = corner(p, at + k)) f.corners.push_back(*c);
          }
          at += size;
          if (mirrored) std::reverse(f.corners.begin(), f.corners.end());
          f.color = color;
          if (f.corners.size() >= 3) faces.push_back(std::move(f));
        }
      }
    }
    build(faces, segments, name.empty() ? geometry->attr("name") : name);
  }

  template <class Faces>
  void build(const Faces& faces, const std::vector<std::pair<gp_Pnt, gp_Pnt>>& segments, const std::string& name) {
    report.triangles += static_cast<int>(faces.size());
    report.lines += static_cast<int>(segments.size());
    if (!faces.empty()) {
      // Faces made one by one (a polygon split into triangles when bent),
      // sewn along shared edges, coplanar ones joined; colours followed.
      Bnd_Box extent;
      std::vector<std::pair<TopoDS_Face, ColladaColor>> made;
      for (const auto& f : faces) {
        auto add = [&](const std::vector<gp_Pnt>& c) {
          const gp_Vec n = gp_Vec(c[0], c[1]).Crossed(gp_Vec(c[0], c[2]));
          if (n.Magnitude() < 1e-10) {
            ++report.degenerate;
            return;
          }
          BRepBuilderAPI_MakePolygon polygon;
          for (const gp_Pnt& p : c) polygon.Add(p);
          polygon.Close();
          if (!polygon.IsDone()) {
            ++report.degenerate;
            return;
          }
          BRepBuilderAPI_MakeFace face(polygon.Wire(), Standard_True);
          if (!face.IsDone()) {
            ++report.degenerate;
            return;
          }
          made.emplace_back(face.Face(), f.color);
        };
        if (f.corners.size() == 3) {
          add(f.corners);
          continue;
        }
        // Flat polygons stay whole; bent ones are cut into a fan.
        const gp_Vec n = gp_Vec(f.corners[0], f.corners[1]).Crossed(gp_Vec(f.corners[0], f.corners[2]));
        bool flat = n.Magnitude() > 1e-10;
        if (flat) {
          const gp_Pln plane(f.corners[0], gp_Dir(n));
          for (const gp_Pnt& p : f.corners) flat = flat && plane.Distance(p) < 1e-6;
        }
        if (flat) {
          add(f.corners);
        } else {
          for (std::size_t k = 1; k + 1 < f.corners.size(); ++k) add({f.corners[0], f.corners[k], f.corners[k + 1]});
        }
      }
      for (const auto& [face, color] : made) {
        for (TopExp_Explorer v(face, TopAbs_VERTEX); v.More(); v.Next()) extent.Add(BRep_Tool::Pnt(TopoDS::Vertex(v.Current())));
      }
      if (!made.empty()) {
        const double size = extent.IsVoid() ? 1.0 : std::sqrt(extent.SquareExtent());
        BRepBuilderAPI_Sewing sewing(std::max(1e-6, std::min(0.01, size * 1e-6)));
        for (const auto& fc : made) sewing.Add(fc.first);
        sewing.Perform();
        TopoDS_Shape sewn = sewing.SewedShape();
        // Closed shells become solids.
        bool solid = false;
        if (sewn.ShapeType() == TopAbs_SHELL && BRep_Tool::IsClosed(sewn)) {
          BRepBuilderAPI_MakeSolid makeSolid(TopoDS::Shell(sewn));
          if (makeSolid.IsDone()) {
            TopoDS_Shape s = makeSolid.Solid();
            BRepLib::OrientClosedSolid(TopoDS::Solid(s));
            if (BRepCheck_Analyzer(s).IsValid()) {
              sewn = s;
              solid = true;
            }
          }
        }
        ShapeUpgrade_UnifySameDomain unify(sewn, Standard_True, Standard_True, Standard_True);
        unify.Build();
        const TopoDS_Shape joined = unify.Shape();
        // Each resulting face: the colour of the triangles it was made of
        // (the one covering most of it).
        std::map<const void*, std::map<ColladaColor, double>> areas;
        for (const auto& [face, color] : made) {
          TopoDS_Shape s = sewing.Modified(face);
          if (s.IsNull()) s = face;
          for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) {
            TopoDS_Shape r = unify.History()->IsRemoved(e.Current()) ? TopoDS_Shape() : e.Current();
            const auto& modified = unify.History()->Modified(e.Current());
            if (!modified.IsEmpty()) r = modified.First();
            if (r.IsNull()) continue;
            GProp_GProps props;
            BRepGProp::SurfaceProperties(e.Current(), props);
            areas[r.TShape().get()][color] += props.Mass();
          }
        }
        ColladaPart part;
        part.shape = joined;
        part.name = name;
        std::map<ColladaColor, double> total;
        TopTools_IndexedMapOfShape result;
        TopExp::MapShapes(joined, TopAbs_FACE, result);
        std::vector<std::pair<int, ColladaColor>> perFace;
        for (int i = 1; i <= result.Extent(); ++i) {
          const auto it = areas.find(result(i).TShape().get());
          if (it == areas.end()) continue;
          const auto best = std::max_element(it->second.begin(), it->second.end(),
                                             [](const auto& a, const auto& b) { return a.second < b.second; });
          perFace.emplace_back(i, best->first);
          for (const auto& [c, a] : it->second) total[c] += a;
        }
        if (!total.empty()) {
          part.color = std::max_element(total.begin(), total.end(), [](const auto& a, const auto& b) {
                         return a.second < b.second;
                       })->first;
        }
        for (const auto& [i, c] : perFace) {
          if (!(c == part.color)) part.faceColors.emplace_back(i, c);
        }
        parts.push_back(std::move(part));
        if (solid) ++report.solids;
      }
    }
    if (!segments.empty()) {
      BRep_Builder builder;
      TopoDS_Compound edges;
      builder.MakeCompound(edges);
      for (const auto& [a, b] : segments) builder.Add(edges, BRepBuilderAPI_MakeEdge(a, b).Edge());
      ColladaPart part;
      part.shape = edges;
      part.name = name.empty() ? std::string("Lines") : name + " lines";
      parts.push_back(std::move(part));
    }
  }

  void node(const Xml& n, const Matrix& parent, int depth) {
    if (depth > 64) return;  // a node instancing itself
    const Matrix m = multiply(parent, nodeTransform(n));
    const std::string name = n.attr("name");
    for (const auto& c : n.children) {
      if (c->name == "instance_geometry") geometry(*c, m, name);
      else if (c->name == "node") node(*c, m, depth + 1);
      else if (c->name == "instance_node") {
        if (const Xml* target = byUrl(c->attr("url"))) node(*target, m, depth + 1);
      } else if (c->name == "instance_camera" || c->name == "instance_light") {
        // nothing to model
      }
    }
  }
};

}  // namespace

Result<std::vector<ColladaPart>> readCollada(const std::string& path, ColladaReport* reportOut) {
  using R = Result<std::vector<ColladaPart>>;
  ColladaReport local;
  ColladaReport& report = reportOut ? *reportOut : local;
  std::ifstream in(path, std::ios::binary);
  if (!in) return R::failure("Cannot open " + path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  try {
    auto doc = parseXml(buffer.str());
    if (!doc) return R::failure(doc.error());
    const Xml* collada = (*doc)->child("COLLADA");
    if (!collada) return R::failure("Not a COLLADA file");
    Reader reader{*collada, report};
    reader.index(*collada);
    // Units and up axis.
    if (const Xml* asset = collada->child("asset")) {
      if (const Xml* unit = asset->child("unit")) {
        report.units = unit->attr("name");
        const double meter = std::atof(unit->attr("meter").c_str());
        if (meter > 0) report.scaleToMm = meter * 1000.0;
      } else {
        report.units = "meter";
        report.scaleToMm = 1000.0;
      }
      if (const Xml* up = asset->child("up_axis")) {
        std::string axis = up->text;
        axis.erase(std::remove_if(axis.begin(), axis.end(), [](unsigned char ch) { return std::isspace(ch); }), axis.end());
        if (!axis.empty()) report.upAxis = axis;
      }
    } else {
      report.units = "meter";
      report.scaleToMm = 1000.0;
    }
    const double k = report.scaleToMm;
    Matrix base{k, 0, 0, 0, 0, k, 0, 0, 0, 0, k, 0, 0, 0, 0, 1};
    if (report.upAxis == "Y_UP") base = multiply(Matrix{1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1}, base);
    else if (report.upAxis == "X_UP") base = multiply(Matrix{0, -1, 0, 0, 0, 0, 1, 0, -1, 0, 0, 0, 0, 0, 0, 1}, base);
    reader.materials();
    const Xml* scene = collada->child("scene");
    const Xml* visual = nullptr;
    if (scene) {
      if (const Xml* ivs = scene->child("instance_visual_scene")) visual = reader.byUrl(ivs->attr("url"));
    }
    if (!visual) {
      if (const Xml* lib = collada->child("library_visual_scenes")) visual = lib->child("visual_scene");
    }
    if (!visual) return R::failure("The COLLADA file has no scene");
    for (const Xml* n : visual->all("node")) reader.node(*n, base, 0);
    report.parts = static_cast<int>(reader.parts.size());
    if (reader.parts.empty()) return R::failure("No faces or lines in the COLLADA file (" + report.summary() + ")");
    return std::move(reader.parts);
  } catch (const Standard_Failure& e) {
    return R::failure(std::string("COLLADA import failed: ") + e.GetMessageString());
  } catch (const std::exception& e) {
    return R::failure(std::string("COLLADA import failed: ") + e.what());
  }
}

}  // namespace sbr::geom
