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

struct Command {
  std::string name;
  std::vector<ObjectDelta> deltas;
};

class CommandStack {
 public:
  explicit CommandStack(Document& document, std::size_t limit = 500)
      : document_(document), limit_(limit) {}

  // Applies the command's "after" states and records it.
  void execute(Command command);
  bool undo();
  bool redo();
  void clear();

  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  std::string undoName() const { return canUndo() ? undo_.back().name : std::string(); }
  std::string redoName() const { return canRedo() ? redo_.back().name : std::string(); }
  std::size_t undoCount() const { return undo_.size(); }

  // Modification tracking for "unsaved changes" prompts.
  void markClean() { cleanMarker_ = revision_; }
  bool isClean() const { return cleanMarker_ == revision_; }

 private:
  void applyAfter(const Command& command);
  void applyBefore(const Command& command);

  Document& document_;
  std::size_t limit_;
  std::vector<Command> undo_;
  std::vector<Command> redo_;
  long long revision_ = 0;
  long long cleanMarker_ = 0;
  long long nextRevision_ = 1;
  std::vector<long long> undoRevisions_;
  std::vector<long long> redoRevisions_;
};

}  // namespace sbr::doc
