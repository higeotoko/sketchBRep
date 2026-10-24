#pragma once
// Materials panel, as SketchUp's: "選択" lists the document's materials as
// swatches (picking one makes it the paint material and switches to the
// paint tool; the first entry, "inherit", takes faces' / objects' own
// materials away; ready-made ones follow), "編集" edits the picked one.

#include <optional>
#include <string>

#include <QWidget>

#include "sbr/doc/Document.h"

class QListWidget;
class QListWidgetItem;
class QTabWidget;
class QPushButton;

namespace sbr::app {

class MaterialsPanel : public QWidget {
  Q_OBJECT
 public:
  explicit MaterialsPanel(QWidget* parent = nullptr);

  // Rebuilds the list (when the library changed) and marks `current`
  // (nullopt: nothing picked yet). Ready-made materials not yet in the
  // library follow, one click adds and picks one.
  void refresh(const doc::Document& document, const std::optional<std::string>& current);
  QListWidget* list() const { return list_; }
  class MaterialEditor* editor() const { return editor_; }
  // Shows the "編集" tab for the material picked in the list.
  void openEditor();

  // Ready-made materials offered for one-click adding (assets/materials/
  // builtin): name, plain colour (also the fallback), picture (a Qt
  // resource) and its size per tile in mm, opacity and rendering values.
  struct Preset {
    const char* name;
    doc::Color color;
    const char* picture;
    double width, height;
    double opacity, metallic, roughness, reflectance;
    bool usePicture;  // added with its picture (glass: off, plain tint)
  };
  static const std::vector<Preset>& presets();
  // The preset of that exact name, if any.
  static std::optional<int> presetNamed(const std::string& name);
  // Thumbnail of a preset: its picture, or its colour.
  static QIcon presetIcon(int index);

 signals:
  void materialChosen(const std::string& id);  // "" = inherit
  void createRequested();
  void presetRequested(int index);
  void editRequested(const std::string& id);
  void deleteRequested(const std::string& id);
  void applyRequested(const std::string& id);  // to the selection
  // An existing material of a preset's name gets the preset's picture
  // (asked for explicitly; the material keeps its id and assignments).
  void presetPictureRequested(const std::string& id, int presetIndex);

 private:
  std::string currentId() const;
  QTabWidget* tabs_ = nullptr;
  class MaterialEditor* editor_ = nullptr;
  QListWidget* list_ = nullptr;
  QPushButton* edit_ = nullptr;
  QPushButton* remove_ = nullptr;
  QPushButton* apply_ = nullptr;
  std::vector<doc::Material> shown_;
  bool updating_ = false;
};

}  // namespace sbr::app
