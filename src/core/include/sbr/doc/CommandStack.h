#pragma once
// Undo/redo built on object snapshots.
//
// A command records the before/after state of each object it touches. Shapes
// are reference counted by OCCT, so snapshots are cheap and undo restores the
// exact previous B-rep rather than re-running modelling operations.

#include <optional>
#include <string>
#include <vector>

#include "sbr/doc/Document.h"

namespace sbr::doc {

struct ObjectDelta {
  ObjectId id;
  std::optional<Object> before;  // nullopt: object did not exist
  std::optional<Object> after;   // nullopt: object is removed
  std::optional<std::size_t> indexBefore;
};

struct DefinitionDelta {
  std::string id;
  std::optional<ComponentDefinition> before;
  std::optional<ComponentDefinition> after;
};

struct SettingsDelta {
  PersistentSettings before;
  PersistentSettings after;
};

struct Command {
  std::string name;
  std::vector<ObjectDelta> deltas;
  // Applied before object deltas on execute/redo and after them on undo, so
  // instances never refer to a missing definition.
  std::vector<DefinitionDelta> definitionDeltas;
  // Tag visibility, sections, scenes (review A: part of the undo step and
  // of atomic rollback like geometry).
  std::optional<SettingsDelta> settings;
};

class CommandStack {
 public:
  explicit CommandStack(Document& document, std::size_t limit = 500)
      : document_(document), limit_(limit) {}

  // Applies the command's "after" states and records it. Inside a group the
  // command is merged into the group's single undo step.
  void execute(Command command);

  // Groups subsequent commands into one undo step (e.g. an AI batch).
  // Nested calls are counted; the outermost endGroup() records the step.
  // Persistent settings changed in between (also directly, e.g. a saved
  // scene) become part of that step, and abortGroup() restores them.
  void beginGroup(const std::string& name);
  void endGroup();
  // Reverts everything executed since the outermost beginGroup() and
  // closes the group (all-or-nothing batches).
  void abortGroup();
  bool inGroup() const { return groupDepth_ > 0; }
  // While amending, the next recorded step (a command, or a whole group)
  // is merged into the most recent undo step instead of becoming its own:
  // revising what was just done ("type right after to change it") stays
  // one undo step. Each object keeps its state from before the original
  // step and gets the newest one after.
  void beginAmend() { amending_ = true; }
  void endAmend() { amending_ = false; }
  bool undo();
  bool redo();
  void clear();

  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  std::string undoName() const { return canUndo() ? undo_.back().name : std::string(); }
  std::string redoName() const { return canRedo() ? redo_.back().name : std::string(); }
  std::size_t undoCount() const { return undo_.size(); }
  // The most recent undo step (e.g. what a batch just committed).
  const Command* lastCommand() const { return undo_.empty() ? nullptr : &undo_.back(); }

  // Modification tracking for "unsaved changes" prompts.
  void markClean() { cleanMarker_ = revision_; }
  // For edits kept outside the undo history (e.g. saved scenes).
  void markDirty() { cleanMarker_ = -1; }
  bool isClean() const { return cleanMarker_ == revision_; }
  // Changes with every executed, undone or redone command.
  long long revision() const { return revision_; }

 private:
  void applyAfter(const Command& command);
  void applyBefore(const Command& command);
  // Records an applied command: new undo step, or merged while amending.
  void record(Command command);

  Document& document_;
  std::size_t limit_;
  std::vector<Command> undo_;
  std::vector<Command> redo_;
  long long revision_ = 0;
  long long cleanMarker_ = 0;
  long long nextRevision_ = 1;
  std::vector<long long> undoRevisions_;
  std::vector<long long> redoRevisions_;
  int groupDepth_ = 0;
  bool amending_ = false;
  std::string groupName_;
  std::optional<Command> group_;
  PersistentSettings groupSettings_;  // at the outermost beginGroup()
};

}  // namespace sbr::doc
