#pragma once
#include <QString>
#include "sbr/Types.h"
class QWidget;
namespace sbr::app {
// Output is written to the caller's temporary directory, never beside the source.
Result<QString> convertDwgToDxf(const QString& input, const QString& executable,
                               const QString& outputDirectory, QWidget* parent = nullptr,
                               int timeoutMs = 120000);
// The other way (drawings, docs/50): the DXF written to `output` as DWG
// (AutoCAD 2018 format) by ODA File Converter. The DXF is copied to a
// temporary folder first; `output` is replaced only when it worked.
Status convertDxfToDwg(const QString& dxf, const QString& executable, const QString& output,
                       QWidget* parent = nullptr, int timeoutMs = 120000);
}
