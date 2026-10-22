#pragma once
// Materials panel: the document's material library as swatches. Picking
// one makes it the paint material (and switches to the paint tool); the
// first entry, "inherit", takes faces' / objects' own materials away.

#include <string>

#include <QWidget>

#include "sbr/doc/Document.h"

class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace sbr::app {

class MaterialsPanel : public QWidget {
  Q_OBJECT
 public:
  explicit MaterialsPanel(QWidget* parent = nullptr);

  // Rebuilds the list (when the library changed) and marks `current`.
  void refresh(const doc::Document& document, const std::string& current);
  QListWidget* list() const { return list_; }

  // Ready-made materials offered for one-click adding.
  struct Preset {
    const char* name;
    doc::Color color;
  };
  static const std::vector<Preset>& presets();

 signals:
  void materialChosen(const std::string& id);  // "" = inherit
  void createRequested();
  void presetRequested(int index);
  void editRequested(const std::string& id);
  void deleteRequested(const std::string& id);
  void applyRequested(const std::string& id);  // to the selection

 private:
  std::string currentId() const;
  QListWidget* list_ = nullptr;
  QPushButton* edit_ = nullptr;
  QPushButton* remove_ = nullptr;
  QPushButton* apply_ = nullptr;
  std::vector<doc::Material> shown_;
  bool updating_ = false;
};

}  // namespace sbr::app
