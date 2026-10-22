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
  void colorEdited(const doc::ObjectId& id, const doc::Color& color);
  void positionEdited(const doc::ObjectId& id, double x, double y, double z);
  void definitionParameterEdited(const std::string& definitionId, const doc::ObjectId& objectId,
                                 const std::string& key, double value);
  void definitionRenamed(const std::string& definitionId, const QString& name);
  void annotationTextEdited(const doc::ObjectId& id, const QString& text);
  void curvePointEdited(const doc::ObjectId& id, int index, double x, double y, double z);

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
};

}  // namespace sbr::app
