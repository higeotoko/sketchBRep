#pragma once
// The help window (ヘルプ > 操作方法): topics written for users, embedded in
// the program (src/app/help/*.md), with a search over all of them; and the
// bug report dialog (ヘルプ > 不具合を報告...), which shows what to send and
// the crash log folder (docs/51).

#include <QDialog>
#include <QString>
#include <QStringList>
#include <string>
#include <vector>

class QListWidget;
class QLineEdit;
class QTextBrowser;

namespace sbr::app {

class HelpWindow : public QDialog {
  Q_OBJECT
 public:
  explicit HelpWindow(QWidget* parent = nullptr);
  // Topic ids: "start", "keys", "drawing", "modeling", "architecture",
  // "drawings", "exchange", "trouble", "construction_timber".
  QStringList topicIds() const;
  void setConstructionOverrides(const std::string& patch);
  bool showTopic(const QString& id);
  QString currentTopic() const;
  // Shows only the topics containing the text (all when empty).
  void search(const QString& text);
  int visibleTopicCount() const;

 private:
  struct Topic {
    QString id, title, markdown;
  };
  std::vector<Topic> topics_;
  std::string constructionPatch_ = "{}";
  QLineEdit* search_ = nullptr;
  QListWidget* list_ = nullptr;
  QTextBrowser* text_ = nullptr;
};

// What to put in a bug report: version, system and the recent actions.
QString bugReportText();

class BugReportDialog : public QDialog {
  Q_OBJECT
 public:
  explicit BugReportDialog(QWidget* parent = nullptr);
};

}  // namespace sbr::app
