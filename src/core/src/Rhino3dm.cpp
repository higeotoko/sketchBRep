#include "sbr/geom/Rhino3dm.h"

#ifndef SBR_WITH_OPENNURBS

namespace sbr::geom {

bool rhino3dmAvailable() { return false; }

Status write3dm(const std::vector<NamedShape>&, const std::string&) {
  return Status::failure("This build has no 3DM support (SBR_WITH_OPENNURBS=OFF)");
}

Result<std::vector<NamedShape>> read3dm(const std::string&) {
  return Result<std::vector<NamedShape>>::failure("This build has no 3DM support (SBR_WITH_OPENNURBS=OFF)");
}

}  // namespace sbr::geom

#else

#include <cmath>
#include <map>
#include <mutex>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_NurbsConvert.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepLib.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom2dConvert.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <GeomConvert.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_Solid.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_Array2OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_Array1OfPnt2d.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>

#include "opennurbs.h"

namespace sbr::geom {
namespace {

void initOpenNurbs() {
  static std::once_flag once;
  std::call_once(once, [] { ON::Begin(); });
}

// --- OCCT -> openNURBS --------------------------------------------------------------

// openNURBS stores cv_count + order - 2 knots: OCCT's flat sequence
// without its first and last knot.
ON_NurbsCurve* toOn(const Handle(Geom_BSplineCurve)& source) {
  Handle(Geom_BSplineCurve) c = Handle(Geom_BSplineCurve)::DownCast(source->Copy());
  if (c->IsPeriodic()) c->SetNotPeriodic();
  const int n = c->NbPoles();
  const int order = c->Degree() + 1;
  auto* out = new ON_NurbsCurve(3, c->IsRational() == Standard_True, order, n);
  for (int i = 1; i <= n; ++i) {
    const gp_Pnt p = c->Pole(i);
    const double w = c->Weight(i);
    if (c->IsRational()) {
      out->SetCV(i - 1, ON_4dPoint(p.X() * w, p.Y() * w, p.Z() * w, w));
    } else {
      out->SetCV(i - 1, ON_3dPoint(p.X(), p.Y(), p.Z()));
    }
  }
  TColStd_Array1OfReal flat(1, n + order);
  c->KnotSequence(flat);
  for (int k = 2; k <= n + order - 1; ++k) out->SetKnot(k - 2, flat(k));
  return out;
}

ON_NurbsCurve* toOn(const Handle(Geom2d_BSplineCurve)& source) {
  Handle(Geom2d_BSplineCurve) c = Handle(Geom2d_BSplineCurve)::DownCast(source->Copy());
  if (c->IsPeriodic()) c->SetNotPeriodic();
  const int n = c->NbPoles();
  const int order = c->Degree() + 1;
  auto* out = new ON_NurbsCurve(2, c->IsRational() == Standard_True, order, n);
  for (int i = 1; i <= n; ++i) {
    const gp_Pnt2d p = c->Pole(i);
    const double w = c->Weight(i);
    if (c->IsRational()) {
      out->SetCV(i - 1, ON_4dPoint(p.X() * w, p.Y() * w, 0.0, w));
    } else {
      out->SetCV(i - 1, ON_3dPoint(p.X(), p.Y(), 0.0));
    }
  }
  TColStd_Array1OfReal flat(1, n + order);
  c->KnotSequence(flat);
  for (int k = 2; k <= n + order - 1; ++k) out->SetKnot(k - 2, flat(k));
  return out;
}

ON_NurbsSurface* toOn(const Handle(Geom_BSplineSurface)& source) {
  Handle(Geom_BSplineSurface) s = Handle(Geom_BSplineSurface)::DownCast(source->Copy());
  if (s->IsUPeriodic()) s->SetUNotPeriodic();
  if (s->IsVPeriodic()) s->SetVNotPeriodic();
  const int nu = s->NbUPoles(), nv = s->NbVPoles();
  const int ou = s->UDegree() + 1, ov = s->VDegree() + 1;
  const bool rational = s->IsURational() || s->IsVRational();
  auto* out = new ON_NurbsSurface(3, rational, ou, ov, nu, nv);
  for (int i = 1; i <= nu; ++i) {
    for (int j = 1; j <= nv; ++j) {
      const gp_Pnt p = s->Pole(i, j);
      const double w = s->Weight(i, j);
      if (rational) {
        out->SetCV(i - 1, j - 1, ON_4dPoint(p.X() * w, p.Y() * w, p.Z() * w, w));
      } else {
        out->SetCV(i - 1, j - 1, ON_3dPoint(p.X(), p.Y(), p.Z()));
      }
    }
  }
  TColStd_Array1OfReal uflat(1, nu + ou), vflat(1, nv + ov);
  s->UKnotSequence(uflat);
  s->VKnotSequence(vflat);
  for (int k = 2; k <= nu + ou - 1; ++k) out->SetKnot(0, k - 2, uflat(k));
  for (int k = 2; k <= nv + ov - 1; ++k) out->SetKnot(1, k - 2, vflat(k));
  return out;
}

Handle(Geom_BSplineCurve) edgeCurve(const TopoDS_Edge& edge) {
  double f = 0, l = 0;
  TopLoc_Location loc;
  Handle(Geom_Curve) c = BRep_Tool::Curve(edge, loc, f, l);
  if (c.IsNull()) return nullptr;
  if (!loc.IsIdentity()) c = Handle(Geom_Curve)::DownCast(c->Transformed(loc.Transformation()));
  return GeomConvert::CurveToBSplineCurve(new Geom_TrimmedCurve(c, f, l));
}

ON_Brep* toOnBrep(const TopoDS_Shape& input, std::string& error) {
  // Every surface and curve as NURBS (exact for planes, cylinders, cones...).
  BRepBuilderAPI_NurbsConvert convert(input, Standard_True);
  const TopoDS_Shape shape = convert.Shape();

  auto brep = std::make_unique<ON_Brep>();
  TopTools_IndexedMapOfShape vertices, edges;
  TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  for (int i = 1; i <= vertices.Extent(); ++i) {
    const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vertices(i)));
    brep->NewVertex(ON_3dPoint(p.X(), p.Y(), p.Z()), BRep_Tool::Tolerance(TopoDS::Vertex(vertices(i))));
  }
  std::vector<int> edgeIndex(edges.Extent() + 1, -1);
  for (int i = 1; i <= edges.Extent(); ++i) {
    const TopoDS_Edge e = TopoDS::Edge(edges(i).Oriented(TopAbs_FORWARD));
    if (BRep_Tool::Degenerated(e)) continue;
    const Handle(Geom_BSplineCurve) c = edgeCurve(e);
    if (c.IsNull()) continue;
    TopoDS_Vertex v0, v1;
    TopExp::Vertices(e, v0, v1);
    const int c3 = brep->AddEdgeCurve(toOn(c));
    ON_BrepEdge& oe = brep->NewEdge(brep->m_V[vertices.FindIndex(v0) - 1], brep->m_V[vertices.FindIndex(v1) - 1], c3,
                                    nullptr, BRep_Tool::Tolerance(e));
    edgeIndex[i] = oe.m_edge_index;
  }

  for (TopExp_Explorer fx(shape, TopAbs_FACE); fx.More(); fx.Next()) {
    const TopoDS_Face original = TopoDS::Face(fx.Current());
    const TopoDS_Face face = TopoDS::Face(original.Oriented(TopAbs_FORWARD));
    TopLoc_Location loc;
    Handle(Geom_Surface) surface = BRep_Tool::Surface(face, loc);
    if (!loc.IsIdentity()) surface = Handle(Geom_Surface)::DownCast(surface->Transformed(loc.Transformation()));
    Handle(Geom_BSplineSurface) bs = Handle(Geom_BSplineSurface)::DownCast(surface);
    if (bs.IsNull()) bs = GeomConvert::SurfaceToBSplineSurface(surface);
    if (bs.IsNull()) {
      error = "a face could not be converted to NURBS";
      return nullptr;
    }
    const int si = brep->AddSurface(toOn(bs));
    const int fi = brep->NewFace(si).m_face_index;
    brep->m_F[fi].m_bRev = original.Orientation() == TopAbs_REVERSED;

    const TopoDS_Wire outer = BRepTools::OuterWire(face);
    std::vector<TopoDS_Wire> wires = {outer};
    for (TopExp_Explorer wx(face, TopAbs_WIRE); wx.More(); wx.Next()) {
      if (!wx.Current().IsSame(outer)) wires.push_back(TopoDS::Wire(wx.Current()));
    }
    for (std::size_t w = 0; w < wires.size(); ++w) {
      const int li = brep->NewLoop(w == 0 ? ON_BrepLoop::outer : ON_BrepLoop::inner, brep->m_F[fi]).m_loop_index;
      for (BRepTools_WireExplorer ex(wires[w], face); ex.More(); ex.Next()) {
        const TopoDS_Edge e = ex.Current();
        double f = 0, l = 0;
        Handle(Geom2d_Curve) pc = BRep_Tool::CurveOnSurface(e, face, f, l);
        if (pc.IsNull()) {
          error = "an edge has no curve on its face";
          return nullptr;
        }
        Handle(Geom2d_BSplineCurve) c2 = Geom2dConvert::CurveToBSplineCurve(new Geom2d_TrimmedCurve(pc, f, l));
        const bool reversed = e.Orientation() == TopAbs_REVERSED;
        if (reversed) c2->Reverse();  // trims run along the loop
        const int c2i = brep->AddTrimCurve(toOn(c2));
        if (BRep_Tool::Degenerated(e)) {
          TopoDS_Vertex v0, v1;
          TopExp::Vertices(e, v0, v1);
          brep->NewSingularTrim(brep->m_V[vertices.FindIndex(v0) - 1], brep->m_L[li], ON_Surface::not_iso, c2i);
          continue;
        }
        const int ei = edgeIndex[edges.FindIndex(e)];
        if (ei < 0) {
          error = "an edge could not be converted";
          return nullptr;
        }
        ON_BrepTrim& trim = brep->NewTrim(brep->m_E[ei], reversed, brep->m_L[li], c2i);
        trim.m_tolerance[0] = trim.m_tolerance[1] = 1e-4;
      }
    }
  }
  brep->SetTolerancesBoxesAndFlags();
  // Tolerances openNURBS could not compute stay "unset": use OCCT's.
  for (int i = 0; i < brep->m_E.Count(); ++i) {
    if (!(brep->m_E[i].m_tolerance >= 0.0)) brep->m_E[i].m_tolerance = 1e-4;
  }
  for (int i = 0; i < brep->m_V.Count(); ++i) {
    if (!(brep->m_V[i].m_tolerance >= 0.0)) brep->m_V[i].m_tolerance = 1e-4;
  }
  for (int i = 0; i < brep->m_T.Count(); ++i) {
    for (double& t : brep->m_T[i].m_tolerance) {
      if (!(t >= 0.0)) t = 1e-4;
    }
  }
  ON_wString log;
  ON_TextLog textLog(log);
  if (!brep->IsValid(&textLog)) {
    error = "the converted Brep is not valid: " + std::string(ON_String(log).Array() ? ON_String(log).Array() : "");
    if (error.size() > 4000) error.resize(4000);
    return nullptr;
  }
  return brep.release();
}

// --- openNURBS -> OCCT --------------------------------------------------------------

Handle(Geom_BSplineCurve) toOcct3d(const ON_NurbsCurve& c) {
  const int n = c.CVCount(), order = c.Order();
  TColgp_Array1OfPnt poles(1, n);
  TColStd_Array1OfReal weights(1, n);
  for (int i = 0; i < n; ++i) {
    ON_3dPoint p;
    c.GetCV(i, p);
    poles(i + 1) = gp_Pnt(p.x, p.y, p.z);
    weights(i + 1) = c.IsRational() ? c.Weight(i) : 1.0;
  }
  // openNURBS knots lack the outer ones: rebuild the full flat sequence.
  std::vector<double> flat;
  flat.push_back(c.Knot(0));
  for (int k = 0; k < c.KnotCount(); ++k) flat.push_back(c.Knot(k));
  flat.push_back(c.Knot(c.KnotCount() - 1));
  std::vector<double> knots;
  std::vector<int> mults;
  for (double k : flat) {
    if (!knots.empty() && std::abs(k - knots.back()) < 1e-12) {
      ++mults.back();
    } else {
      knots.push_back(k);
      mults.push_back(1);
    }
  }
  TColStd_Array1OfReal K(1, static_cast<int>(knots.size()));
  TColStd_Array1OfInteger M(1, static_cast<int>(mults.size()));
  for (std::size_t i = 0; i < knots.size(); ++i) {
    K(static_cast<int>(i) + 1) = knots[i];
    M(static_cast<int>(i) + 1) = mults[i];
  }
  return c.IsRational() ? new Geom_BSplineCurve(poles, weights, K, M, order - 1)
                        : new Geom_BSplineCurve(poles, K, M, order - 1);
}

Handle(Geom2d_BSplineCurve) toOcct2d(const ON_NurbsCurve& c) {
  const Handle(Geom_BSplineCurve) c3 = toOcct3d(c);
  TColgp_Array1OfPnt2d poles(1, c3->NbPoles());
  TColStd_Array1OfReal weights(1, c3->NbPoles());
  for (int i = 1; i <= c3->NbPoles(); ++i) {
    poles(i) = gp_Pnt2d(c3->Pole(i).X(), c3->Pole(i).Y());
    weights(i) = c3->Weight(i);
  }
  TColStd_Array1OfReal K(1, c3->NbKnots());
  TColStd_Array1OfInteger M(1, c3->NbKnots());
  c3->Knots(K);
  c3->Multiplicities(M);
  return c3->IsRational() ? new Geom2d_BSplineCurve(poles, weights, K, M, c3->Degree())
                          : new Geom2d_BSplineCurve(poles, K, M, c3->Degree());
}

void knotsOf(const ON_NurbsSurface& s, int dir, std::vector<double>& knots, std::vector<int>& mults) {
  const int count = s.KnotCount(dir);
  std::vector<double> flat = {s.Knot(dir, 0)};
  for (int k = 0; k < count; ++k) flat.push_back(s.Knot(dir, k));
  flat.push_back(s.Knot(dir, count - 1));
  for (double k : flat) {
    if (!knots.empty() && std::abs(k - knots.back()) < 1e-12) {
      ++mults.back();
    } else {
      knots.push_back(k);
      mults.push_back(1);
    }
  }
}

Handle(Geom_BSplineSurface) toOcct(const ON_NurbsSurface& s) {
  const int nu = s.CVCount(0), nv = s.CVCount(1);
  TColgp_Array2OfPnt poles(1, nu, 1, nv);
  TColStd_Array2OfReal weights(1, nu, 1, nv);
  for (int i = 0; i < nu; ++i) {
    for (int j = 0; j < nv; ++j) {
      ON_3dPoint p;
      s.GetCV(i, j, p);
      poles(i + 1, j + 1) = gp_Pnt(p.x, p.y, p.z);
      weights(i + 1, j + 1) = s.IsRational() ? s.Weight(i, j) : 1.0;
    }
  }
  std::vector<double> uk, vk;
  std::vector<int> um, vm;
  knotsOf(s, 0, uk, um);
  knotsOf(s, 1, vk, vm);
  TColStd_Array1OfReal UK(1, static_cast<int>(uk.size())), VK(1, static_cast<int>(vk.size()));
  TColStd_Array1OfInteger UM(1, static_cast<int>(um.size())), VM(1, static_cast<int>(vm.size()));
  for (std::size_t i = 0; i < uk.size(); ++i) {
    UK(static_cast<int>(i) + 1) = uk[i];
    UM(static_cast<int>(i) + 1) = um[i];
  }
  for (std::size_t i = 0; i < vk.size(); ++i) {
    VK(static_cast<int>(i) + 1) = vk[i];
    VM(static_cast<int>(i) + 1) = vm[i];
  }
  return s.IsRational()
             ? new Geom_BSplineSurface(poles, weights, UK, VK, UM, VM, s.Order(0) - 1, s.Order(1) - 1)
             : new Geom_BSplineSurface(poles, UK, VK, UM, VM, s.Order(0) - 1, s.Order(1) - 1);
}

// Faces from trims (2D curves on the face's surface), then sewn into a shell
// or solid; scale converts the file's units to millimetres.
Result<TopoDS_Shape> fromOnBrep(const ON_Brep& brep, double scale) {
  BRepBuilderAPI_Sewing sewing(1e-3);
  int faces = 0;
  for (int fi = 0; fi < brep.m_F.Count(); ++fi) {
    const ON_BrepFace& of = brep.m_F[fi];
    ON_NurbsSurface ns;
    if (!of.SurfaceOf() || !of.SurfaceOf()->GetNurbForm(ns)) continue;
    const Handle(Geom_BSplineSurface) surface = toOcct(ns);
    TopoDS_Face face;
    BRep_Builder builder;
    bool first = true;
    for (int l = 0; l < of.LoopCount(); ++l) {
      const ON_BrepLoop* loop = of.Loop(l);
      if (!loop) continue;
      BRepBuilderAPI_MakeWire wire;
      for (int t = 0; t < loop->TrimCount(); ++t) {
        const ON_BrepTrim* trim = loop->Trim(t);
        if (!trim) continue;
        ON_NurbsCurve nc;
        if (!trim->TrimCurveOf() || !trim->TrimCurveOf()->GetNurbForm(nc)) continue;
        const ON_Interval d = trim->Domain();
        Handle(Geom2d_BSplineCurve) c2 = toOcct2d(nc);
        if (trim->m_type == ON_BrepTrim::singular) {
          // A pole (sphere, cone apex): a degenerated edge at one point.
          const gp_Pnt2d uv = c2->Value(d[0]);
          const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(surface->Value(uv.X(), uv.Y()));
          TopoDS_Edge e;
          builder.MakeEdge(e);
          builder.UpdateEdge(e, c2, surface, TopLoc_Location(), 1e-4);
          builder.Range(e, d[0], d[1]);
          builder.Degenerated(e, Standard_True);
          builder.Add(e, v.Oriented(TopAbs_FORWARD));
          builder.Add(e, v.Oriented(TopAbs_REVERSED));
          wire.Add(e);
          continue;
        }
        BRepBuilderAPI_MakeEdge edge(c2, surface, d[0], d[1]);
        if (edge.IsDone()) wire.Add(edge.Edge());
      }
      if (!wire.IsDone()) continue;
      if (first) {
        BRepBuilderAPI_MakeFace make(surface, wire.Wire(), Standard_True);
        if (!make.IsDone()) break;
        face = make.Face();
        first = false;
      } else {
        builder.Add(face, wire.Wire());
      }
    }
    if (face.IsNull()) continue;
    BRepLib::BuildCurves3d(face);
    ShapeFix_Face fix(face);
    fix.Perform();
    face = fix.Face();
    if (of.m_bRev) face.Reverse();
    sewing.Add(face);
    ++faces;
  }
  if (faces == 0) return Result<TopoDS_Shape>::failure("Brep without usable faces");
  sewing.Perform();
  TopoDS_Shape result = sewing.SewedShape();
  // A single sewn face (sphere, closed by its own seam) comes back bare.
  if (result.ShapeType() == TopAbs_FACE) {
    TopoDS_Shell shell;
    BRep_Builder shellBuilder;
    shellBuilder.MakeShell(shell);
    shellBuilder.Add(shell, result);
    shell.Closed(BRep_Tool::IsClosed(shell));
    result = shell;
  }
  // Closed shells become solids.
  TopoDS_Compound compound;
  BRep_Builder builder;
  builder.MakeCompound(compound);
  int parts = 0;
  for (TopExp_Explorer sx(result, TopAbs_SHELL); sx.More(); sx.Next()) {
    const TopoDS_Shell shell = TopoDS::Shell(sx.Current());
    if (BRep_Tool::IsClosed(shell)) {
      BRepBuilderAPI_MakeSolid solid(shell);
      ShapeFix_Solid fix(solid.Solid());
      fix.Perform();
      builder.Add(compound, fix.Solid());
    } else {
      builder.Add(compound, shell);
    }
    ++parts;
  }
  if (parts == 0) {
    for (TopExp_Explorer fx(result, TopAbs_FACE); fx.More(); fx.Next()) builder.Add(compound, fx.Current());
  }
  TopoDS_Shape shape = compound;
  TopExp_Explorer single(compound, TopAbs_SOLID);
  if (single.More()) {
    TopoDS_Shape only = single.Current();
    single.Next();
    if (!single.More() && parts == 1) shape = only;
  }
  if (std::abs(scale - 1.0) > 1e-12) {
    gp_Trsf t;
    t.SetScale(gp_Pnt(0, 0, 0), scale);
    shape = BRepBuilderAPI_Transform(shape, t, Standard_True).Shape();
  }
  return shape;
}

std::string toUtf8(const ON_wString& s) {
  const ON_String utf8(s);
  return utf8.Array() ? std::string(utf8.Array()) : std::string();
}

}  // namespace

bool rhino3dmAvailable() { return true; }

Status write3dm(const std::vector<NamedShape>& parts, const std::string& path) {
  initOpenNurbs();
  try {
    ONX_Model model;
    model.m_properties.m_Application.m_application_name = L"SketchBRep";
    model.m_settings.m_ModelUnitsAndTolerances.m_unit_system = ON_UnitSystem(ON::LengthUnitSystem::Millimeters);
    model.m_settings.m_ModelUnitsAndTolerances.m_absolute_tolerance = 0.001;
    model.AddDefaultLayer(nullptr, ON_Color::UnsetColor);
    std::map<std::string, int> layers;
    int written = 0;
    for (const NamedShape& part : parts) {
      std::string error;
      ON_Brep* brep = toOnBrep(part.shape, error);
      if (!brep) return Status::failure("'" + part.name + "': " + error);
      auto* attributes = new ON_3dmObjectAttributes();
      attributes->m_name = ON_wString(ON_String(part.name.c_str()));
      if (!part.layer.empty()) {
        auto it = layers.find(part.layer);
        if (it == layers.end()) {
          const int index = model.AddLayer(ON_wString(ON_String(part.layer.c_str())), ON_Color::Black);
          it = layers.emplace(part.layer, index).first;
        }
        attributes->m_layer_index = it->second;
      }
      if (part.color) {
        attributes->m_color = ON_Color(static_cast<int>(std::lround((*part.color)[0] * 255)),
                                       static_cast<int>(std::lround((*part.color)[1] * 255)),
                                       static_cast<int>(std::lround((*part.color)[2] * 255)));
        attributes->SetColorSource(ON::color_from_object);
      }
      model.AddManagedModelGeometryComponent(brep, attributes);
      ++written;
    }
    if (written == 0) return Status::failure("Nothing to export");
    ON_wString log;
    ON_TextLog textLog(log);
    if (!model.Write(path.c_str(), 0, &textLog)) {
      return Status::failure("Could not write 3DM file: " + toUtf8(log));
    }
    return Status::success();
  } catch (const Standard_Failure& e) {
    return Status::failure(std::string("3DM export failed: ") + e.GetMessageString());
  }
}

Result<std::vector<NamedShape>> read3dm(const std::string& path) {
  initOpenNurbs();
  try {
    ONX_Model model;
    ON_wString log;
    ON_TextLog textLog(log);
    if (!model.Read(path.c_str(), &textLog)) {
      return Result<std::vector<NamedShape>>::failure("Could not read 3DM file: " + path);
    }
    const double metersPerUnit = model.m_settings.m_ModelUnitsAndTolerances.m_unit_system.MetersPerUnit(1.0);
    const double scale = std::isfinite(metersPerUnit) && metersPerUnit > 0 ? metersPerUnit * 1000.0 : 1.0;
    std::vector<NamedShape> out;
    ONX_ModelComponentIterator it(model, ON_ModelComponent::Type::ModelGeometry);
    for (const ON_ModelComponent* c = it.FirstComponent(); c; c = it.NextComponent()) {
      const auto* component = ON_ModelGeometryComponent::Cast(c);
      if (!component) continue;
      const ON_Geometry* geometry = component->Geometry(nullptr);
      const ON_3dmObjectAttributes* attributes = component->Attributes(nullptr);
      if (!geometry) continue;
      std::unique_ptr<ON_Brep> owned;
      const ON_Brep* brep = ON_Brep::Cast(geometry);
      if (!brep) {
        // Extrusions and single surfaces have a Brep form.
        owned.reset(geometry->BrepForm());
        brep = owned.get();
      }
      if (!brep) continue;  // meshes, points, annotations: not B-rep geometry
      auto shape = fromOnBrep(*brep, scale);
      if (!shape) continue;
      NamedShape part;
      part.shape = shape.value();
      if (attributes) {
        part.name = toUtf8(attributes->m_name);
        if (attributes->ColorSource() == ON::color_from_object) {
          const ON_Color col = attributes->m_color;
          part.color = std::array<double, 3>{col.Red() / 255.0, col.Green() / 255.0, col.Blue() / 255.0};
        }
        const ON_ModelComponentReference layerRef = model.LayerFromIndex(attributes->m_layer_index);
        if (const ON_Layer* layer = ON_Layer::FromModelComponentRef(layerRef, nullptr)) {
          const std::string name = toUtf8(layer->Name());
          if (!name.empty() && name != "Default") part.layer = name;
        }
      }
      out.push_back(std::move(part));
    }
    if (out.empty()) return Result<std::vector<NamedShape>>::failure("The 3DM file contains no surfaces or solids");
    return out;
  } catch (const Standard_Failure& e) {
    return Result<std::vector<NamedShape>>::failure(std::string("3DM import failed: ") + e.GetMessageString());
  }
}

}  // namespace sbr::geom

#endif
