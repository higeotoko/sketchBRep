#pragma once
// High level modelling API.
//
// Every user operation (GUI tools, scripts, future AI agents) goes through this
// class, so all of them share validation, rollback and undo/redo.

#include <string>
#include <vector>

#include <TopoDS_Face.hxx>

#include "sbr/doc/CommandStack.h"
#include "sbr/doc/Document.h"

namespace sbr::doc {

struct ObjectSummary {
  ObjectId id;
  std::string name;
  ObjectKind kind;
  std::string recipe;
  double volume = 0.0;
};

// Result of a push/pull: which object and parameter received the distance,
// so the caller can re-edit it (e.g. typing a value right after the drag).
struct PushPullResult {
  ObjectId id;
  std::string parameterKey;
};

class Modeler {
 public:
  Modeler();
  Modeler(const Modeler&) = delete;
  Modeler& operator=(const Modeler&) = delete;

  Document& document() { return document_; }
  const Document& document() const { return document_; }
  CommandStack& history() { return history_; }

  // --- creation -----------------------------------------------------------
  Result<ObjectId> createRectangle(const geom::SketchPlane& plane, double width, double height,
                                   const std::string& name = {});
  Result<ObjectId> createPolygon(const std::vector<gp_Pnt>& points, const std::string& name = {});
  Result<ObjectId> createPolyline(const std::vector<gp_Pnt>& points, const std::string& name = {});
  Result<ObjectId> importShape(const TopoDS_Shape& shape, const std::string& name,
                               const std::string& source);

  // --- editing ------------------------------------------------------------
  // `face` is a face of the object's world shape (or an equivalent copy).
  Result<PushPullResult> pushPull(const ObjectId& id, const TopoDS_Face& face, double distance);
  // Extrudes a face/profile object directly (convenience for scripting).
  Result<PushPullResult> extrude(const ObjectId& id, double distance);
  Status setParameter(const ObjectId& id, const std::string& key, double value);
  Status rename(const ObjectId& id, const std::string& name);
  Status setVisible(const ObjectId& id, bool visible);
  Status remove(const std::vector<ObjectId>& ids);

  bool undo() { return history_.undo(); }
  bool redo() { return history_.redo(); }

  // --- files --------------------------------------------------------------
  void newDocument();
  Status save(const std::string& path);
  Status load(const std::string& path, std::vector<std::string>* warnings = nullptr);
  // Exports the given objects (all visible objects when empty) as STEP.
  Status exportStep(const std::string& path, const std::vector<ObjectId>& ids = {}) const;
  Result<std::vector<ObjectId>> importStep(const std::string& path);

  // --- queries ------------------------------------------------------------
  std::vector<ObjectSummary> listObjects() const;
  std::vector<Parameter> parameters(const ObjectId& id) const;

 private:
  Result<ObjectId> addObject(Object object, const std::string& commandName);
  Status commitModified(Object object, const std::string& commandName);
  std::string uniqueName(const std::string& base) const;

  Document document_;
  CommandStack history_;
};

}  // namespace sbr::doc
