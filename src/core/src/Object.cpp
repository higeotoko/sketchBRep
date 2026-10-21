#include "sbr/doc/Object.h"

#include <cstdio>
#include <random>

#include <TopExp_Explorer.hxx>

namespace sbr::doc {

ObjectId generateObjectId() {
  static thread_local std::mt19937_64 engine{std::random_device{}()};
  const std::uint64_t hi = engine();
  const std::uint64_t lo = engine();
  char buffer[37];
  std::snprintf(buffer, sizeof(buffer), "%08x-%04x-%04x-%04x-%012llx",
                static_cast<unsigned>(hi >> 32), static_cast<unsigned>((hi >> 16) & 0xffff),
                static_cast<unsigned>((hi & 0x0fff) | 0x4000),
                static_cast<unsigned>(((lo >> 48) & 0x3fff) | 0x8000),
                static_cast<unsigned long long>(lo & 0xffffffffffffULL));
  return buffer;
}

const char* toString(ObjectKind kind) {
  switch (kind) {
    case ObjectKind::Empty: return "Empty";
    case ObjectKind::Curve: return "Curve";
    case ObjectKind::Face: return "Face";
    case ObjectKind::Solid: return "Solid";
    case ObjectKind::Group: return "Group";
    case ObjectKind::Instance: return "Instance";
    case ObjectKind::Annotation: return "Annotation";
    case ObjectKind::Guide: return "Guide";
    case ObjectKind::Other: return "Other";
  }
  return "Other";
}

ObjectKind Object::kind() const {
  if (std::holds_alternative<GroupNode>(recipe.base)) return ObjectKind::Group;
  if (std::holds_alternative<ComponentInstance>(recipe.base)) return ObjectKind::Instance;
  if (isAnnotation(recipe)) return ObjectKind::Annotation;
  if (std::holds_alternative<GuideLine>(recipe.base)) return ObjectKind::Guide;
  if (shape.IsNull()) return ObjectKind::Empty;
  if (TopExp_Explorer(shape, TopAbs_SOLID).More()) return ObjectKind::Solid;
  if (TopExp_Explorer(shape, TopAbs_FACE).More()) return ObjectKind::Face;
  if (TopExp_Explorer(shape, TopAbs_EDGE).More()) return ObjectKind::Curve;
  return ObjectKind::Other;
}

Status rebuild(Object& object, const RegenContext& context) {
  auto result = regenerate(object.recipe, context);
  if (!result) return result.status();
  object.shape = result.value();
  return Status::success();
}

}  // namespace sbr::doc
