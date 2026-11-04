#pragma once
// Progress for long geometric operations: after 0.4 s a small dialog with a
// "中止" (cancel) button appears; cancelling makes the operation fail, so
// the model stays as it was (spec 07 AC-19).

#include <QElapsedTimer>
#include <QPointer>

#include "sbr/geom/Progress.h"

class QProgressDialog;
class QWidget;

namespace sbr::app {

class GuiProgress : public geom::CancellableProgress {
 public:
  explicit GuiProgress(QWidget* parent) : parent_(parent) {}
  // Closes the dialog once the operation is over.
  void finish();
  // True while a long operation is handing events out: whatever runs now
  // runs inside it.
  static bool busy() { return pumping_ > 0; }
  // Marks events handed out from inside an operation (public for tests).
  class Pumping {
   public:
    Pumping() { ++pumping_; }
    ~Pumping() { --pumping_; }
    Pumping(const Pumping&) = delete;
    Pumping& operator=(const Pumping&) = delete;
  };

 protected:
  void Show(const Message_ProgressScope& scope, const Standard_Boolean force) override;
  Standard_Boolean UserBreak() override;
  void Reset() override;

 private:
  QWidget* parent_;
  QPointer<QProgressDialog> dialog_;
  QElapsedTimer elapsed_;
  bool cancelled_ = false;
  static int pumping_;
};

}  // namespace sbr::app
