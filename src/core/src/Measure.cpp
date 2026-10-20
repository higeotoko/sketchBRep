#include "sbr/geom/Measure.h"

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

namespace sbr::geom {

double volume(const TopoDS_Shape& shape) {
  if (shape.IsNull()) return 0.0;
  GProp_GProps props;
  BRepGProp::VolumeProperties(shape, props);
  return props.Mass();
}

double area(const TopoDS_Shape& shape) {
  if (shape.IsNull()) return 0.0;
  GProp_GProps props;
  BRepGProp::SurfaceProperties(shape, props);
  return props.Mass();
}

Bnd_Box exactBounds(const TopoDS_Shape& shape) {
  Bnd_Box box;
  if (!shape.IsNull()) {
    BRepBndLib::AddOptimal(shape, box, Standard_False, Standard_False);
  }
  return box;
}

}  // namespace sbr::geom
