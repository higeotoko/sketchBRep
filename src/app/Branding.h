#pragma once

#include <QIcon>

namespace sbr::app {
// The same individually rendered sizes are used by Qt and the Windows EXE.
inline QIcon applicationIcon() {
  return QIcon(QStringLiteral(":/branding/app-icon.ico"));
}
}  // namespace sbr::app
