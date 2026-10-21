#include "sbr/geom/Progress.h"

namespace sbr::geom {
namespace {

thread_local Handle(CancellableProgress) current;

}  // namespace

void setProgressIndicator(const Handle(CancellableProgress)& indicator) { current = indicator; }

Message_ProgressRange progressRange() {
  if (current.IsNull()) return Message_ProgressRange();
  return current->Start();
}

bool cancelled() { return !current.IsNull() && current->isCancelled(); }

}  // namespace sbr::geom
