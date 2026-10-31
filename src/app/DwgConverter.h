#pragma once
#include <QString>
#include "sbr/Types.h"
class QWidget;
namespace sbr::app {
// Output is written to the caller's temporary directory, never beside the source.
Result<QString> convertDwgToDxf(const QString& input, const QString& executable,
                               const QString& outputDirectory, QWidget* parent = nullptr,
                               int timeoutMs = 120000);
}
