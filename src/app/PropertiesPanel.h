#pragma once
// Right-hand panel: object list and parameter editor.

#include <vector>

#include <QWidget>

#include "sbr/doc/Modeler.h"

class QFormLayout;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QWidget;

namespace sbr::app {

class PropertiesPanel : public QWidget {
  Q_OBJECT
 public:
  explicit PropertiesPanel(QWidget* parent = nullptr);

  void refresh(const doc::Modeler& modeler, const std::vector<doc::ObjectId>& selection);

 signals:
  void selectionChanged(const std::vector<doc::ObjectId>& ids);
  void parameterEdited(const doc::ObjectId& id, const std::string& key, double value);
  void renameRequested(const doc::ObjectId& id, const QString& name);
  void visibilityToggled(const doc::ObjectId& id, bool visible);

 private:
  void rebuildForm(const doc::Modeler& modeler, const std::vector<doc::ObjectId>& selection);

  QTreeWidget* tree_ = nullptr;
  QWidget* formHost_ = nullptr;
  QFormLayout* form_ = nullptr;
  bool updating_ = false;
};

}  // namespace sbr::app
