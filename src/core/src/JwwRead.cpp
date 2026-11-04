// Reading Jw_cad's .jww (docs/50): the layout from Jw_cad's own description
// of its format (jwdatafmt.txt), the same reading checked against the
// drawings shipped with Jw_cad 8.25a.

#include "sbr/geom/Jww.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRep_Builder.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_Circle.hxx>
#include <Geom_Ellipse.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <gp_GTrsf.hxx>

#include "sbr/TextEncoding.h"

namespace sbr::geom {
namespace {

class Reader {
 public:
  explicit Reader(std::vector<std::uint8_t> d) : d_(std::move(d)) {}
  void need(std::size_t n) const {
    if (p_ + n > d_.size()) throw std::runtime_error("JWW file ends early");
  }
  std::uint8_t u8() { need(1); return d_[p_++]; }
  std::uint16_t u16() { need(2); std::uint16_t v; std::memcpy(&v, &d_[p_], 2); p_ += 2; return v; }
  std::uint32_t u32() { need(4); std::uint32_t v; std::memcpy(&v, &d_[p_], 4); p_ += 4; return v; }
  double f64() { need(8); double v; std::memcpy(&v, &d_[p_], 8); p_ += 8; return v; }
  void skip(std::size_t n) { need(n); p_ += n; }
  std::string str() {
    std::size_t n = u8();
    if (n == 0xFF) {
      n = u16();
      if (n == 0xFFFE) throw std::runtime_error("JWW text in Unicode is not supported");
      if (n == 0xFFFF) n = u32();
    }
    need(n);
    std::string s(reinterpret_cast<const char*>(&d_[p_]), n);
    p_ += n;
    const auto utf8 = fromShiftJis(s);
    return utf8 ? *utf8 : std::string("?");
  }
  std::size_t at() const { return p_; }
  void seek(std::size_t p) { p_ = p; }
  std::size_t size() const { return d_.size(); }

 private:
  std::vector<std::uint8_t> d_;
  std::size_t p_ = 0;
};

// One element as read (paper coordinates).
struct Element {
  enum Kind { Line, Arc, Block, Other } kind = Other;
  std::string what;  // for the report
  int group = 0, layer = 0;
  double v[8] = {};  // line: x0 y0 x1 y1; arc: cx cy r start sweep tilt flat full
  // block: x y sx sy rotation number
};

class Archive {
 public:
  Archive(Reader& r, unsigned version) : r_(r), version_(version) {}
  std::vector<Element> list() {
    std::size_t n = r_.u16();
    if (n == 0xFFFF) n = r_.u32();
    std::vector<Element> out;
    for (std::size_t i = 0; i < n; ++i) object(out);
    return out;
  }
  std::map<std::uint32_t, std::vector<Element>> blocks;

 private:
  struct Base {
    std::uint8_t style;
    std::uint16_t color, layer, group;
  };
  Base base() {
    Base b;
    r_.u32();
    b.style = r_.u8();
    b.color = r_.u16();
    if (version_ >= 351) r_.u16();
    b.layer = r_.u16();
    b.group = r_.u16();
    r_.u16();
    return b;
  }
  void line(Element& e) {
    for (int k = 0; k < 4; ++k) e.v[k] = r_.f64();
  }
  void text() {
    base();
    for (int k = 0; k < 4; ++k) r_.f64();
    r_.u32();
    for (int k = 0; k < 4; ++k) r_.f64();
    r_.str();
    r_.str();
  }
  void point() {
    const std::size_t at = r_.at();
    r_.u32();
    const std::uint8_t style = r_.u8();
    r_.seek(at);
    base();
    r_.f64(); r_.f64(); r_.u32();
    if (style == 100) { r_.u32(); r_.f64(); r_.f64(); }
  }
  void body(const std::string& name, std::vector<Element>& out) {
    Element e;
    if (name == "CDataSen") {
      const Base b = base();
      e.kind = Element::Line;
      e.group = b.group;
      e.layer = b.layer;
      line(e);
      out.push_back(e);
    } else if (name == "CDataEnko") {
      const Base b = base();
      e.kind = Element::Arc;
      e.group = b.group;
      e.layer = b.layer;
      for (int k = 0; k < 7; ++k) e.v[k] = r_.f64();
      e.v[7] = r_.u32();
      out.push_back(e);
    } else if (name == "CDataTen") {
      point();
      out.push_back({Element::Other, "points"});
    } else if (name == "CDataMoji") {
      text();
      out.push_back({Element::Other, "text"});
    } else if (name == "CDataSolid") {
      const std::size_t at = r_.at();
      r_.u32(); r_.u8();
      const std::uint16_t color = r_.u16();
      r_.seek(at);
      base();
      for (int k = 0; k < 8; ++k) r_.f64();
      if (color == 10) r_.u32();
      out.push_back({Element::Other, "solid fills"});
    } else if (name == "CDataSunpou") {
      // A dimension: its line kept; its text and helpers counted.
      const Base b = base();
      Element dim;
      dim.kind = Element::Line;
      dim.group = b.group;
      dim.layer = b.layer;
      const Base lb = base();
      dim.group = lb.group;
      dim.layer = lb.layer;
      line(dim);
      out.push_back(dim);
      text();
      out.push_back({Element::Other, "dimension values"});
      if (version_ >= 420) {
        r_.u16();
        for (int k = 0; k < 2; ++k) {
          Element helper;
          helper.kind = Element::Line;
          const Base hb = base();
          helper.group = hb.group;
          helper.layer = hb.layer;
          line(helper);
          out.push_back(helper);
        }
        for (int k = 0; k < 4; ++k) point();
      }
    } else if (name == "CDataBlock") {
      const Base b = base();
      e.kind = Element::Block;
      e.group = b.group;
      e.layer = b.layer;
      for (int k = 0; k < 5; ++k) e.v[k] = r_.f64();
      e.v[5] = r_.u32();
      out.push_back(e);
    } else if (name == "CDataList") {
      base();
      const std::uint32_t number = r_.u32();
      r_.u32();
      r_.u32();  // time
      r_.str();
      blocks[number] = list();
    } else {
      throw std::runtime_error("Unknown JWW element '" + name + "'");
    }
  }
  void object(std::vector<Element>& out) {
    const std::uint16_t tag = r_.u16();
    std::string name;
    if (tag == 0xFFFF) {
      r_.u16();
      const std::size_t len = r_.u16();
      for (std::size_t k = 0; k < len; ++k) name.push_back(static_cast<char>(r_.u8()));
      classes_[count_++] = name;
    } else if (tag == 0x7FFF) {
      name = classes_.at(r_.u32() & 0x7FFFFFFF);
    } else if (tag & 0x8000) {
      const auto c = classes_.find(tag & 0x7FFF);
      if (c == classes_.end()) throw std::runtime_error("Broken JWW element list");
      name = c->second;
    } else {
      throw std::runtime_error("Broken JWW element list");
    }
    ++count_;
    body(name, out);
  }
  Reader& r_;
  unsigned version_;
  std::map<std::uint32_t, std::string> classes_;
  std::uint32_t count_ = 1;
};

struct Header {
  unsigned version = 0;
  double scale[16] = {};
  std::string layer[16][16];
  std::string group[16];
};

Header header(Reader& r) {
  Header h;
  char magic[8];
  for (char& c : magic) c = static_cast<char>(r.u8());
  if (std::string(magic, 8) != "JwwData.") throw std::runtime_error("Not a Jw_cad (.jww) file");
  h.version = r.u32();
  if (h.version < 300 || h.version > 9999) throw std::runtime_error("Unsupported JWW version");
  r.str();   // memo
  r.u32();   // paper
  r.u32();   // layer group written to
  for (int g = 0; g < 16; ++g) {
    r.u32(); r.u32();
    h.scale[g] = r.f64();
    r.u32();
    for (int l = 0; l < 32; ++l) r.u32();
  }
  for (int i = 0; i < 14 + 5 + 1 + 1; ++i) r.u32();
  r.f64(); r.f64(); r.f64(); r.u32();
  r.u32(); for (int i = 0; i < 5; ++i) r.f64();
  for (int g = 0; g < 16; ++g) for (int l = 0; l < 16; ++l) h.layer[g][l] = r.str();
  for (int g = 0; g < 16; ++g) h.group[g] = r.str();
  r.f64(); r.f64(); r.u32(); r.f64();
  r.f64(); r.f64();
  r.u32();
  for (int i = 0; i < 6; ++i) r.f64();
  for (int n = 0; n < 8; ++n) { r.f64(); r.f64(); r.f64(); r.u32(); }
  for (int i = 0; i < 3; ++i) r.f64();
  r.u32(); r.f64(); r.f64(); r.f64(); r.u32();
  for (int n = 0; n < 11; ++n) r.f64();
  for (int n = 0; n < 10; ++n) { r.u32(); r.u32(); }
  for (int n = 0; n < 10; ++n) { r.u32(); r.u32(); r.f64(); }
  for (int n = 0; n < 8 * 4 + 5 * 5 + 4 * 4; ++n) r.u32();
  for (int i = 0; i < 10 + 1 + 1 + 4; ++i) r.u32();
  for (int i = 0; i < 9; ++i) r.f64();
  r.u32(); r.u32();
  for (int n = 0; n <= 256; ++n) { r.u32(); r.u32(); }
  for (int n = 0; n <= 256; ++n) { r.str(); r.u32(); r.u32(); r.f64(); }
  for (int n = 0; n <= 32; ++n) { r.u32(); r.u32(); r.u32(); r.u32(); }
  for (int n = 0; n <= 32; ++n) { r.str(); r.u32(); for (int j = 0; j < 10; ++j) r.f64(); }
  for (int i = 0; i < 10; ++i) { r.f64(); r.f64(); r.f64(); r.u32(); }
  r.f64(); r.f64(); r.f64(); r.u32(); r.u32();
  r.f64(); r.f64(); r.u32();
  for (int i = 0; i < 6; ++i) r.f64();
  return h;
}

std::string layerName(const Header& h, int g, int l) {
  static const char hex[] = "0123456789ABCDEF";
  auto trim = [](std::string s) {
    while (!s.empty() && (s.front() == ' ')) s.erase(s.begin());
    // A full-width space Jw uses to indent names.
    while (s.size() >= 3 && s.compare(0, 3, "\xE3\x80\x80") == 0) s.erase(0, 3);
    return s;
  };
  g &= 15;
  l &= 15;
  const std::string gn = trim(h.group[g]).empty() ? std::string("G") + hex[g] : trim(h.group[g]);
  const std::string ln = trim(h.layer[g][l]).empty() ? std::string(1, hex[g]) + "-" + hex[l] : trim(h.layer[g][l]);
  return "Jw " + gn + "/" + ln;
}

}  // namespace

std::string JwwImportReport::summary() const {
  std::ostringstream out;
  out << imported << " curves (Jw_cad data " << version << ")";
  for (const auto& [what, n] : omitted) out << "; not read: " << what << " " << n;
  return out.str();
}

Result<std::vector<NamedShape>> readJww(const std::string& path, const JwwImportOptions& options,
                                        JwwImportReport* output) {
  using R = Result<std::vector<NamedShape>>;
  JwwImportReport local;
  JwwImportReport& report = output ? *output : local;
  report = {};
  try {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open the JWW file");
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() > 256u * 1024 * 1024) throw std::runtime_error("JWW exceeds the 256 MiB import limit");
    Reader r(std::move(data));
    Header h;
    std::vector<Element> elements;
    std::unique_ptr<Archive> archive;
    try {
      h = header(r);
      archive = std::make_unique<Archive>(r, h.version);
      elements = archive->list();
      archive->list();  // block definitions (CDataList: kept in blocks)
    } catch (const std::exception& e) {
      throw std::runtime_error(std::string("This JWW could not be read (") + e.what() +
                               "). Save it as DXF from Jw_cad and import that");
    }
    report.version = h.version;
    std::vector<NamedShape> result;
    auto add = [&](const TopoDS_Shape& edge, int group, int layer) {
      result.push_back({edge, "", std::nullopt, layerName(h, group, layer)});
      ++report.imported;
    };
    // Each element: paper mm x its layer group's scale = real mm; in a
    // block, placed by the instance (offset, scales, rotation) first.
    std::function<void(const Element&, const gp_GTrsf&, int, int)> place;
    place = [&](const Element& e, const gp_GTrsf& toPaper, int groupOverride, int depth) {
      const int group = groupOverride >= 0 ? groupOverride : e.group;
      const double scale = h.scale[group & 15] > 0 ? h.scale[group & 15] : 1.0;
      gp_GTrsf toReal;
      toReal.SetValue(1, 1, scale);
      toReal.SetValue(2, 2, scale);
      toReal.SetValue(3, 3, 1.0);
      const gp_GTrsf all = toReal * toPaper;
      try {
        TopoDS_Shape edge;
        if (e.kind == Element::Line) {
          const gp_Pnt a(e.v[0], e.v[1], 0), b(e.v[2], e.v[3], 0);
          if (a.Distance(b) < 1e-9) return;
          edge = BRepBuilderAPI_MakeEdge(a, b).Edge();
        } else if (e.kind == Element::Arc) {
          const double r0 = e.v[2], start = e.v[3], sweep = e.v[4], tilt = e.v[5], flat = e.v[6];
          const bool full = e.v[7] != 0 || std::abs(sweep) >= 2 * M_PI - 1e-9;
          if (!(r0 > 0)) return;
          const gp_Ax2 axis(gp_Pnt(e.v[0], e.v[1], 0), gp::DZ(), gp_Dir(std::cos(tilt), std::sin(tilt), 0));
          Handle(Geom_Curve) curve;
          if (std::abs(flat - 1.0) < 1e-9 || !(flat > 0)) {
            curve = new Geom_Circle(gp_Circ(axis, r0));
          } else if (flat < 1) {
            curve = new Geom_Ellipse(gp_Elips(axis, r0, r0 * flat));
          } else {
            // Taller than wide: the major axis turned a quarter.
            const gp_Ax2 turned(axis.Location(), gp::DZ(), gp_Dir(-std::sin(tilt), std::cos(tilt), 0));
            curve = new Geom_Ellipse(gp_Elips(turned, r0 * flat, r0));
          }
          if (full) {
            edge = BRepBuilderAPI_MakeEdge(curve).Edge();
          } else {
            double s = start, t = start + sweep;
            if (flat > 1) { s -= M_PI / 2; t -= M_PI / 2; }
            if (sweep < 0) std::swap(s, t);
            edge = BRepBuilderAPI_MakeEdge(curve, s, t).Edge();
          }
        } else if (e.kind == Element::Block) {
          if (depth > 16) return;
          const auto def = archive->blocks.find(static_cast<std::uint32_t>(e.v[5]));
          if (def == archive->blocks.end()) {
            ++report.omitted["blocks without a definition"];
            return;
          }
          gp_GTrsf local;
          const double c = std::cos(e.v[4]), s = std::sin(e.v[4]);
          local.SetValue(1, 1, c * e.v[2]);
          local.SetValue(1, 2, -s * e.v[3]);
          local.SetValue(2, 1, s * e.v[2]);
          local.SetValue(2, 2, c * e.v[3]);
          local.SetValue(3, 3, 1.0);
          local.SetTranslationPart(gp_XYZ(e.v[0], e.v[1], 0));
          for (const Element& inner : def->second) place(inner, toPaper * local, group, depth + 1);
          return;
        } else {
          ++report.omitted[e.what];
          return;
        }
        if (all.Form() == gp_Other) edge = BRepBuilderAPI_GTransform(edge, all, Standard_True).Shape();
        else edge = BRepBuilderAPI_Transform(edge, all.Trsf(), Standard_True).Shape();
        add(edge, group, e.layer);
      } catch (const Standard_Failure&) {
        ++report.omitted["invalid curves"];
      }
    };
    for (const Element& e : elements) place(e, gp_GTrsf(), -1, 0);
    if (result.empty()) throw std::runtime_error("No lines or arcs in this JWW; " + report.summary());
    if (report.incomplete() && !options.allowPartial) {
      throw std::runtime_error("The JWW has parts that are not read (" + report.summary() + "): nothing imported");
    }
    if (options.groupByLayer) {
      BRep_Builder builder;
      std::map<std::string, TopoDS_Compound> layers;
      for (const NamedShape& part : result) {
        auto [it, inserted] = layers.try_emplace(part.layer);
        if (inserted) builder.MakeCompound(it->second);
        builder.Add(it->second, part.shape);
      }
      result.clear();
      for (const auto& [layer, shape] : layers) result.push_back({shape, layer, std::nullopt, layer});
    }
    return result;
  } catch (const std::exception& e) {
    return R::failure(e.what());
  } catch (const Standard_Failure& e) {
    return R::failure(std::string("JWW: ") + (e.GetMessageString() ? e.GetMessageString() : "geometry error"));
  }
}

}  // namespace sbr::geom
