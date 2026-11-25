#pragma once

#include <QByteArray>

class QWidget;

namespace sbr::doc {
class Document;
}

namespace sbr::app {

struct PermitFormOptions {
  bool summary = false;
  bool transferAreas = false;
  bool includeSitePlan = false;
  bool includeVicinityMap = false;
};
struct PermitFormDraft {
  QByteArray docx;
  bool hasSitePlan = false;
  bool hasVicinityMap = false;
};

PermitFormDraft buildPermitForm(const doc::Document& document, const PermitFormOptions& options);
void showPermitForms(const doc::Document& document, QWidget* parent);

}  // namespace sbr::app
