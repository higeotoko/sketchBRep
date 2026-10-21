#include "sbr/doc/CommandStack.h"

namespace sbr::doc {

void CommandStack::applyAfter(const Command& command) {
  for (const DefinitionDelta& d : command.definitionDeltas) {
    if (d.after) {
      document_.putDefinition(*d.after);
    } else {
      document_.removeDefinition(d.id);
    }
  }
  for (const ObjectDelta& d : command.deltas) {
    if (!d.after) {
      document_.remove(d.id);
    } else if (document_.find(d.id)) {
      document_.replace(*d.after);
    } else {
      document_.insert(*d.after);
    }
  }
}

void CommandStack::applyBefore(const Command& command) {
  for (auto it = command.deltas.rbegin(); it != command.deltas.rend(); ++it) {
    const ObjectDelta& d = *it;
    if (!d.before) {
      document_.remove(d.id);
    } else if (document_.find(d.id)) {
      document_.replace(*d.before);
    } else {
      document_.insert(*d.before, d.indexBefore);
    }
  }
  for (auto it = command.definitionDeltas.rbegin(); it != command.definitionDeltas.rend(); ++it) {
    if (it->before) {
      document_.putDefinition(*it->before);
    } else {
      document_.removeDefinition(it->id);
    }
  }
}

void CommandStack::execute(Command command) {
  if (groupDepth_ > 0) {
    applyAfter(command);
    if (!group_) group_ = Command{groupName_, {}, {}};
    group_->deltas.insert(group_->deltas.end(), command.deltas.begin(), command.deltas.end());
    group_->definitionDeltas.insert(group_->definitionDeltas.end(), command.definitionDeltas.begin(),
                                    command.definitionDeltas.end());
    return;
  }
  applyAfter(command);
  undo_.push_back(std::move(command));
  revision_ = nextRevision_++;
  undoRevisions_.push_back(revision_);
  redo_.clear();
  redoRevisions_.clear();
  if (undo_.size() > limit_) {
    undo_.erase(undo_.begin());
    undoRevisions_.erase(undoRevisions_.begin());
  }
}

void CommandStack::beginGroup(const std::string& name) {
  if (groupDepth_++ == 0) {
    groupName_ = name;
    group_.reset();
  }
}

void CommandStack::endGroup() {
  if (groupDepth_ == 0 || --groupDepth_ > 0) return;
  if (!group_ || (group_->deltas.empty() && group_->definitionDeltas.empty())) {
    group_.reset();
    return;
  }
  Command command = std::move(*group_);
  group_.reset();
  // Already applied: record it without applying again.
  undo_.push_back(std::move(command));
  revision_ = nextRevision_++;
  undoRevisions_.push_back(revision_);
  redo_.clear();
  redoRevisions_.clear();
  if (undo_.size() > limit_) {
    undo_.erase(undo_.begin());
    undoRevisions_.erase(undoRevisions_.begin());
  }
}

void CommandStack::abortGroup() {
  if (groupDepth_ == 0) return;
  groupDepth_ = 0;
  if (group_) applyBefore(*group_);
  group_.reset();
}

bool CommandStack::undo() {
  if (undo_.empty() || groupDepth_ > 0) return false;
  Command command = std::move(undo_.back());
  undo_.pop_back();
  const long long rev = undoRevisions_.back();
  undoRevisions_.pop_back();
  applyBefore(command);
  redo_.push_back(std::move(command));
  redoRevisions_.push_back(rev);
  revision_ = undoRevisions_.empty() ? 0 : undoRevisions_.back();
  return true;
}

bool CommandStack::redo() {
  if (redo_.empty() || groupDepth_ > 0) return false;
  Command command = std::move(redo_.back());
  redo_.pop_back();
  const long long rev = redoRevisions_.back();
  redoRevisions_.pop_back();
  applyAfter(command);
  undo_.push_back(std::move(command));
  undoRevisions_.push_back(rev);
  revision_ = rev;
  return true;
}

void CommandStack::clear() {
  groupDepth_ = 0;
  group_.reset();
  undo_.clear();
  redo_.clear();
  undoRevisions_.clear();
  redoRevisions_.clear();
  revision_ = 0;
  cleanMarker_ = 0;
}

}  // namespace sbr::doc
