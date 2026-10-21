#pragma once
// User-facing wording of failures (spec 07 §10): what could not be done,
// that the model is unchanged, and what to try next. The core reports in
// English; the GUI shows these Japanese explanations.

#include <QString>

namespace sbr::app {

// Replaces known core messages inside 'text' with explanations. Returns
// 'text' unchanged when nothing is known.
QString userMessage(const QString& text);

}  // namespace sbr::app
