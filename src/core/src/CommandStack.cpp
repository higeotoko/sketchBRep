#include "sbr/doc/CommandStack.h"

#include <algorithm>

namespace sbr::doc {

namespace {
void applySettings(Document& document, const PersistentSettings& settings) {
  if (PersistentSettings::of(document.settings()) == settings) return;
  settings.applyTo(document.settings());
  document.settingsChanged();
}
}  // namespace

void CommandStack::applyAfter(const Command& command) {
  for (const MaterialDelta& d : command.materialDeltas) {
    if (d.after) {
      document_.putMaterial(*d.after);
    } else {
      document_.removeMaterial(d.id);
    }
  }
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
  if (command.settings) applySettings(document_, command.settings->after);
}

void CommandStack::applyBefore(const Command& command) {
  if (command.settings) applySettings(document_, command.settings->before);
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
  for (auto it = command.materialDeltas.rbegin(); it != command.materialDeltas.rend(); ++it) {
    if (it->before) {
      document_.putMaterial(*it->before);
    } else {
      document_.removeMaterial(it->id);
    }
  }
}

namespace {

bool reshaped(const Object* before, const Object& after) {
  return !before || !before->shape.IsSame(after.shape) || before->shape.Orientation() != after.shape.Orientation();
}

// Painted faces follow shape changes, whoever made them (tools, AI,
// batches): each new state finds its faces again (docs/11_materials.md).
// Recorded steps are already consistent, so undo / redo skip this.
void reanchorMaterials(Command& command) {
  for (ObjectDelta& d : command.deltas) {
    if (d.after && !d.after->faceMaterials.empty() && reshaped(d.before ? &*d.before : nullptr, *d.after)) {
      reanchorFaceMaterials(*d.after);
    }
  }
  for (DefinitionDelta& d : command.definitionDeltas) {
    if (!d.after) continue;
    for (Object& o : d.after->objects) {
      if (o.faceMaterials.empty()) continue;
      const Object* before = nullptr;
      if (d.before) {
        for (const Object& b : d.before->objects) {
          if (b.id == o.id) before = &b;
        }
      }
      if (reshaped(before, o)) reanchorFaceMaterials(o);
    }
  }
}

}  // namespace

void CommandStack::execute(Command command) {
  reanchorMaterials(command);
  // A step that changes nothing is not worth an undo (or a new revision).
  if (command.deltas.empty() && command.definitionDeltas.empty() && command.materialDeltas.empty() &&
      !command.settings) {
    return;
  }
  if (groupDepth_ > 0) {
    applyAfter(command);
    if (!group_) group_ = Command{groupName_, {}, {}, {}, std::nullopt};
    group_->deltas.insert(group_->deltas.end(), command.deltas.begin(), command.deltas.end());
    group_->definitionDeltas.insert(group_->definitionDeltas.end(), command.definitionDeltas.begin(),
                                    command.definitionDeltas.end());
    group_->materialDeltas.insert(group_->materialDeltas.end(), command.materialDeltas.begin(),
                                  command.materialDeltas.end());
    return;
  }
  applyAfter(command);
  record(std::move(command));
}

void CommandStack::record(Command command) {
  redo_.clear();
  redoRevisions_.clear();
  revision_ = nextRevision_++;
  if (amending_ && !undo_.empty()) {
    Command& last = undo_.back();
    for (ObjectDelta& d : command.deltas) {
      auto it = std::find_if(last.deltas.begin(), last.deltas.end(), [&](const ObjectDelta& e) { return e.id == d.id; });
      if (it == last.deltas.end()) {
        last.deltas.push_back(std::move(d));
      } else {
        it->after = std::move(d.after);
      }
    }
    // Created and removed again within the step: nothing to restore.
    last.deltas.erase(std::remove_if(last.deltas.begin(), last.deltas.end(),
                                     [](const ObjectDelta& e) { return !e.before && !e.after; }),
                      last.deltas.end());
    for (DefinitionDelta& d : command.definitionDeltas) {
      auto it = std::find_if(last.definitionDeltas.begin(), last.definitionDeltas.end(),
                             [&](const DefinitionDelta& e) { return e.id == d.id; });
      if (it == last.definitionDeltas.end()) {
        last.definitionDeltas.push_back(std::move(d));
      } else {
        it->after = std::move(d.after);
      }
    }
    for (MaterialDelta& d : command.materialDeltas) {
      auto it = std::find_if(last.materialDeltas.begin(), last.materialDeltas.end(),
                             [&](const MaterialDelta& e) { return e.id == d.id; });
      if (it == last.materialDeltas.end()) {
        last.materialDeltas.push_back(std::move(d));
      } else {
        it->after = std::move(d.after);
      }
    }
    if (command.settings) {
      if (last.settings) {
        last.settings->after = command.settings->after;
      } else {
        last.settings = command.settings;
      }
    }
    if (last.deltas.empty() && last.definitionDeltas.empty() && last.materialDeltas.empty() && !last.settings) {
      undo_.pop_back();  // amended back to nothing: no step left to undo
      undoRevisions_.pop_back();
    } else {
      undoRevisions_.back() = revision_;
    }
    return;
  }
  undo_.push_back(std::move(command));
  undoRevisions_.push_back(revision_);
  if (undo_.size() > limit_) {
    undo_.erase(undo_.begin());
    undoRevisions_.erase(undoRevisions_.begin());
  }
}

void CommandStack::beginGroup(const std::string& name) {
  if (groupDepth_++ == 0) {
    groupName_ = name;
    group_.reset();
    groupSettings_ = PersistentSettings::of(document_.settings());
  }
}

void CommandStack::endGroup() {
  if (groupDepth_ == 0 || --groupDepth_ > 0) return;
  const PersistentSettings now = PersistentSettings::of(document_.settings());
  if (now != groupSettings_) {
    if (!group_) group_ = Command{groupName_, {}, {}, {}, std::nullopt};
    group_->settings = SettingsDelta{groupSettings_, now};
  }
  if (!group_ || (group_->deltas.empty() && group_->definitionDeltas.empty() && group_->materialDeltas.empty() &&
                  !group_->settings)) {
    group_.reset();
    return;
  }
  Command command = std::move(*group_);
  group_.reset();
  // Already applied: record it without applying again.
  record(std::move(command));
}

void CommandStack::abortGroup() {
  if (groupDepth_ == 0) return;
  groupDepth_ = 0;
  if (group_) {
    group_->settings.reset();  // restored below from the snapshot
    applyBefore(*group_);
  }
  group_.reset();
  applySettings(document_, groupSettings_);
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
  amending_ = false;
  group_.reset();
  undo_.clear();
  redo_.clear();
  undoRevisions_.clear();
  redoRevisions_.clear();
  revision_ = 0;
  cleanMarker_ = 0;
}

}  // namespace sbr::doc
