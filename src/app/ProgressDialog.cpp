#include "ProgressDialog.h"

#include <QCoreApplication>
#include <QProgressDialog>

namespace sbr::app {

void GuiProgress::Reset() {
  Message_ProgressIndicator::Reset();
  elapsed_.start();
  cancelled_ = false;
}

void GuiProgress::Show(const Message_ProgressScope&, const Standard_Boolean) {
  if (!elapsed_.isValid()) elapsed_.start();
  const double position = GetPosition();
  if (position >= 1.0) {
    finish();
    return;
  }
  if (!dialog_ && elapsed_.elapsed() > 400) {
    dialog_ = new QProgressDialog(QObject::tr("形状を計算しています…"), QObject::tr("中止"), 0, 100, parent_);
    dialog_->setWindowModality(Qt::WindowModal);
    dialog_->setMinimumDuration(0);
    dialog_->setAutoClose(false);
  }
  if (dialog_) {
    dialog_->setValue(static_cast<int>(position * 100));
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  }
}

Standard_Boolean GuiProgress::UserBreak() {
  if (dialog_) {
    QCoreApplication::processEvents();  // the dialog is modal: only "中止" reacts
    cancelled_ = cancelled_ || dialog_->wasCanceled();
  }
  return cancelled_;
}

void GuiProgress::finish() {
  if (dialog_) dialog_->deleteLater();
  dialog_ = nullptr;
  elapsed_.invalidate();
}

}  // namespace sbr::app
