#pragma once
// Progress and cancellation of long geometric operations (spec 07 AC-19).
//
// The host (GUI) installs a progress indicator; heavy operations (Booleans,
// push/pull, curved offsets) report to it and stop when it says the user
// cancelled. A cancelled operation fails like any other failure, so the
// model is left unchanged.

#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressRange.hxx>

namespace sbr::geom {

// OCCT indicator whose cancellation state the core can query.
class CancellableProgress : public Message_ProgressIndicator {
 public:
  bool isCancelled() { return UserBreak() == Standard_True; }
};

// Indicator for the operations on this thread; null = none.
void setProgressIndicator(const Handle(CancellableProgress)& indicator);
// Range for the next heavy operation (empty when there is no indicator).
Message_ProgressRange progressRange();
// True if the user asked to stop the current operation.
bool cancelled();

// Message of operations stopped by the user.
inline constexpr const char* kCancelledMessage = "Cancelled by the user";

}  // namespace sbr::geom
