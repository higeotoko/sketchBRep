#include "DwgConverter.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProgressDialog>
#include <QTemporaryDir>
#include <QTimer>

namespace sbr::app {
Result<QString> convertDwgToDxf(const QString& input, const QString& executable,
                               const QString& outputDirectory, QWidget* parent, int timeoutMs) {
  if (!QFileInfo(executable).isExecutable())
    return Result<QString>::failure("ODA File Converter の実行ファイルを設定してください。");
  QFile source(input);
  if (!source.open(QIODevice::ReadOnly) || !source.read(6).startsWith("AC10"))
    return Result<QString>::failure("DWG ファイルを開けないか、対応する DWG ヘッダーではありません。");
  source.close();
  QTemporaryDir staging;
  if (!staging.isValid() || !QDir().mkpath(outputDirectory) ||
      !QFile::copy(input, staging.filePath(QStringLiteral("drawing.dwg"))))
    return Result<QString>::failure("DWG 変換用の一時ファイルを作れません。");
  const QString output = QDir(outputDirectory).filePath(QStringLiteral("drawing.dxf"));
  if (QFileInfo::exists(output))
    return Result<QString>::failure("DWG 変換先に既存のファイルがあります。");

  QProcess process;
  // Passing arguments directly also handles spaces/Japanese paths without a shell.
  process.setProgram(executable);
  process.setArguments({staging.path(), QDir(outputDirectory).absolutePath(),
                        QStringLiteral("ACAD2018"), QStringLiteral("DXF"),
                        QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("drawing.dwg")});
  process.setStandardOutputFile(QProcess::nullDevice());
  process.setStandardErrorFile(QProcess::nullDevice());
  QProgressDialog progress(QStringLiteral("DWG を DXF に変換しています…"), QStringLiteral("中止"), 0, 0, parent);
  progress.setWindowModality(Qt::ApplicationModal);
  progress.setMinimumDuration(0);
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  bool cancelled = false, timedOut = false;
  QObject::connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &loop, &QEventLoop::quit);
  QObject::connect(&process, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
  QObject::connect(&progress, &QProgressDialog::canceled, &loop, [&] { cancelled = true; loop.quit(); });
  QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { timedOut = true; loop.quit(); });
  process.start();
  timeout.start(timeoutMs);
  loop.exec();
  if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(3000); }
  // Closing a progress dialog emits canceled too; only the user can cancel the operation.
  QObject::disconnect(&progress, nullptr, &loop, nullptr);
  progress.close();
  if (cancelled) return Result<QString>::failure("DWG の取り込みを中止しました。モデルは変更していません。");
  if (timedOut) return Result<QString>::failure("DWG の変換が制限時間を超えました。モデルは変更していません。");
  if (process.error() == QProcess::FailedToStart || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    return Result<QString>::failure("ODA File Converter の変換に失敗しました。設定した実行ファイルと元図面を確認してください。");
  if (!QFileInfo(output).isFile() || QFileInfo(output).size() == 0)
    return Result<QString>::failure("ODA File Converter が DXF を出力しませんでした。");
  return output;
}

Status convertDxfToDwg(const QString& dxf, const QString& executable, const QString& output, QWidget* parent,
                       int timeoutMs) {
  if (executable.isEmpty() || !QFileInfo(executable).isExecutable())
    return Status::failure("DWG で書き出すには ODA File Converter の実行ファイルを設定してください（ファイル > DWG 変換ソフトの設定）。");
  QTemporaryDir staging, converted;
  if (!staging.isValid() || !converted.isValid() ||
      !QFile::copy(dxf, staging.filePath(QStringLiteral("drawing.dxf"))))
    return Status::failure("DWG 変換用の一時ファイルを作れません。");
  QProcess process;
  process.setProgram(executable);
  process.setArguments({staging.path(), converted.path(), QStringLiteral("ACAD2018"), QStringLiteral("DWG"),
                        QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("drawing.dxf")});
  process.setStandardOutputFile(QProcess::nullDevice());
  process.setStandardErrorFile(QProcess::nullDevice());
  QProgressDialog progress(QStringLiteral("DXF を DWG に変換しています…"), QStringLiteral("中止"), 0, 0, parent);
  progress.setWindowModality(Qt::ApplicationModal);
  progress.setMinimumDuration(0);
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  bool cancelled = false, timedOut = false;
  QObject::connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &loop, &QEventLoop::quit);
  QObject::connect(&process, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
  QObject::connect(&progress, &QProgressDialog::canceled, &loop, [&] { cancelled = true; loop.quit(); });
  QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { timedOut = true; loop.quit(); });
  process.start();
  timeout.start(timeoutMs);
  loop.exec();
  if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(3000); }
  QObject::disconnect(&progress, nullptr, &loop, nullptr);
  progress.close();
  if (cancelled) return Status::failure("DWG への変換を中止しました。");
  if (timedOut) return Status::failure("DWG への変換が制限時間を超えました。");
  if (process.error() == QProcess::FailedToStart || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    return Status::failure("ODA File Converter の変換に失敗しました。設定した実行ファイルを確認してください。");
  const QString made = QDir(converted.path()).filePath(QStringLiteral("drawing.dwg"));
  if (!QFileInfo(made).isFile() || QFileInfo(made).size() == 0)
    return Status::failure("ODA File Converter が DWG を出力しませんでした。");
  // Replace the chosen file only now that the DWG exists.
  const QString temp = output + QStringLiteral(".part");
  QFile::remove(temp);
  if (!QFile::copy(made, temp)) return Status::failure("DWG を保存できません: " + output.toStdString());
  QFile::remove(output);
  if (!QFile::rename(temp, output)) {
    QFile::remove(temp);
    return Status::failure("DWG を保存できません: " + output.toStdString());
  }
  return Status::success();
}
}  // namespace sbr::app
