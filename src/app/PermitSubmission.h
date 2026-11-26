#pragma once

class QWidget;

namespace sbr::doc {
class Document;
struct DrawingSetSpec;
}

namespace sbr::app {

void showPermitSubmissionReview(const doc::Document& document, const doc::DrawingSetSpec& spec, QWidget* parent);

}  // namespace sbr::app
