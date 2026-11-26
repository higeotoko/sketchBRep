#pragma once

#include <QByteArray>
#include <QStringList>
#include <functional>

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

// docs/287: 横浜市 建築協定等手続状況届出書 (第１号様式), first face filled from the model; `unfilled`
// names the items left for the person.
QByteArray buildAgreementForm(const doc::Document& document, QStringList* unfilled = nullptr);
PermitFormDraft buildPermitForm(const doc::Document& document, const PermitFormOptions& options);
void showPermitForms(const doc::Document& document, QWidget* parent, const std::function<void()>& editOutline);

}  // namespace sbr::app
