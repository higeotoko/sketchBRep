#pragma once

#include <cstdint>
#include <map>
// Right-hand panel: object list and parameter editor.

#include <vector>

#include <QWidget>

#include "sbr/doc/Modeler.h"

class QFormLayout;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

namespace sbr::app {

class PropertiesPanel : public QWidget {
  Q_OBJECT
 public:
  explicit PropertiesPanel(QWidget* parent = nullptr);

  // `elementSummary` describes a face/edge selection ("面 1 / エッジ 4").
  void refresh(const doc::Modeler& modeler, const std::vector<doc::ObjectId>& selection,
               const QString& elementSummary = {});

 signals:
  void selectionChanged(const std::vector<doc::ObjectId>& ids);
  void parameterEdited(const doc::ObjectId& id, const std::string& key, double value);
  void renameRequested(const doc::ObjectId& id, const QString& name);
  void visibilityToggled(const doc::ObjectId& id, bool visible);
  void tagEdited(const doc::ObjectId& id, const QString& tag);
  // A meaning attribute set ("" value: removed), e.g. kind = column.
  void attributeEdited(const doc::ObjectId& id, const std::string& key, const std::string& value);
  void colorEdited(const doc::ObjectId& id, const doc::Color& color);
  void positionEdited(const doc::ObjectId& id, double x, double y, double z);
  void definitionParameterEdited(const std::string& definitionId, const doc::ObjectId& objectId,
                                 const std::string& key, double value);
  void definitionRenamed(const std::string& definitionId, const QString& name);
  void annotationTextEdited(const doc::ObjectId& id, const QString& text);
  void curvePointEdited(const doc::ObjectId& id, int index, double x, double y, double z);
  // Opening (window/door cut) `index` of a host resized.
  void openingSizeEdited(const doc::ObjectId& id, int index, double width, double height);
  // Door / window kind of one opening (plan symbol): a fitting JSON (OpeningSymbols.h).
  void openingFittingEdited(const doc::ObjectId& id, int index, const QString& fitting);
  // One opening put at `offset` from the wall's left end (seen from the side it was cut from)
  // and `sill` above the wall's bottom.
  void openingPlaced(const doc::ObjectId& id, int index, double offset, double sill);
  // One opening removed (the wall is whole again there).
  void openingRemoved(const doc::ObjectId& id, int index);
  // A wall's build-up (WallTypes.h id) chosen in the panel.
  void wallTypeEdited(const doc::ObjectId& id, const QString& type);
  // A wall's foot (top = false) or a wall's / slab's top put on the level `level` (its id;
  // empty: let go of its level).
  void levelAttachmentEdited(const doc::ObjectId& id, bool top, const QString& level);
  // A wall's or a slab's own size (length / thickness / height) typed in the part section.
  void partSizeEdited(const doc::ObjectId& id, const QString& key, double value);
  // A roof's size (docs/90 G-08): key = kind (then `kind`), pitch, eave, gable_eave, thickness, height.
  void roofEdited(const doc::ObjectId& id, const QString& key, double value, const QString& kind);
  // A stair's size (docs/90 G-09): key = width, risers (0 = automatic), riser_limit, turn,
  // thickness, rise, or style (then `text`).
  void stairEdited(const doc::ObjectId& id, const QString& key, double value, const QString& text);
  // The floors over a stair opened for it, or closed again.
  void stairFloorRequested(const doc::ObjectId& id, bool open);
  // Walls up to a roof (docs/97 N-05): `id` is a roof (fit / release the walls under it) or a
  // wall (release it).
  void roofWallsRequested(const doc::ObjectId& id, bool fit);
  void lockToggled(const doc::ObjectId& id, bool locked);
  // The object's material picked from the library ("" = inherit).
  void materialAssigned(const doc::ObjectId& id, const std::string& materialId);
  // Edit that material ("" = make a new one).
  void materialEditRequested(const std::string& materialId);
  // A component placement's own dimension (only this placement changes).
  void placementParameterEdited(const doc::ObjectId& instanceId, const doc::ObjectId& objectId,
                                const std::string& key, double value);
  // "unique", "reset", "mirror0".."mirror2" or "axes".
  void placementAction(const doc::ObjectId& instanceId, const QString& action);

 private:
  void rebuildForm(const doc::Modeler& modeler, const std::vector<doc::ObjectId>& selection,
                   const QString& elementSummary);

  QTreeWidget* tree_ = nullptr;
  // The tree is rebuilt only when the document changed (sequence); a
  // selection change just moves the highlight.
  std::map<doc::ObjectId, QTreeWidgetItem*> treeItems_;
  std::uint64_t treeSequence_ = 0;
  const doc::Document* treeDocument_ = nullptr;
  QWidget* formHost_ = nullptr;
  QFormLayout* form_ = nullptr;
  bool updating_ = false;
  bool detailsExpanded_ = false;  // remembered across selections
  bool placementOnly_ = false;    // component dimensions: this placement only
};

}  // namespace sbr::app
