#pragma once
// What was being done when SketchBRep stopped (docs/51): the last actions
// (tools, edits, files, warnings) kept in memory, written with the version
// and the kind of failure to a report file if the program crashes; the next
// start says where it is, to send with a bug report. Nothing is sent
// anywhere by the program.

#include <QString>
#include <QStringList>

namespace sbr::app {

// Once, early in main: where reports go (created if needed).
void installCrashLog(const QString& directory);
QString crashLogDirectory();
// A line in the log of recent actions (kept: the last 64).
void noteActivity(const QString& what);
// The last actions, oldest first (for the bug report dialog and checks).
QStringList recentActivity();
// Reports written since the user was last told; marks them told.
QStringList takeNewCrashReports();
// For checks: write the report as a crash would (without crashing).
QString writeCrashReportForTest(const char* reason);

}  // namespace sbr::app
