#pragma once
// What one undo step changes, read from its recorded deltas (review B).
//
// Preview, AI proposals and commits all summarize the Command that the
// edit produced, instead of each comparing documents with its own rules,
// so a rotation, a hidden object or a tag toggle cannot be missed by one
// path and shown by another.

#include <string>
#include <vector>

#include "sbr/doc/CommandStack.h"

namespace sbr::doc {

enum ChangeAspect : unsigned {
  kGeometry = 1u << 0,    // shape (recipe / parameters)
  kPlacement = 1u << 1,   // full transform, rotation included
  kVisibility = 1u << 2,  // hidden / shown
  kAttributes = 1u << 3,  // name, tag, colour, meaning attributes
  kHierarchy = 1u << 4,   // parent group
};

struct ChangeSummary {
  struct Modified {
    ObjectId id;
    unsigned aspects = 0;  // ChangeAspect bits
  };
  std::vector<ObjectId> added;
  std::vector<ObjectId> removed;
  std::vector<Modified> modified;
  std::vector<std::string> definitions;  // component definitions changed
  std::vector<std::string> settings;     // "scenes", "sections", "tags"

  bool empty() const {
    return added.empty() && removed.empty() && modified.empty() && definitions.empty() && settings.empty();
  }
};

// Aspects in which two states of the same object differ.
unsigned compareObjects(const Object& before, const Object& after);
// Several deltas of one object in a step are combined (first before, last
// after): created-then-changed is "added", created-then-deleted is nothing.
ChangeSummary summarize(const Command& command);
// Names of the set bits, in a fixed order ("geometry", "placement", ...).
std::vector<std::string> aspectNames(unsigned aspects);

}  // namespace sbr::doc
