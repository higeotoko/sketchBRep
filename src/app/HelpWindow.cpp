#include "HelpWindow.h"
#include "sbr/ConstructionGuide.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QSysInfo>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

#include "CrashLog.h"

namespace sbr::app {

namespace {
struct TopicFile {
  const char* id;
  const char* file;
};
const TopicFile kTopics[] = {{"start", "01_start.md"},       {"keys", "02_keys.md"},
                             {"drawing", "03_drawing.md"},   {"modeling", "04_modeling.md"},
                             {"architecture", "05_architecture.md"}, {"drawings", "06_drawings.md"},
                             {"exchange", "07_exchange.md"}, {"trouble", "08_trouble.md"},
                             {"construction_timber", nullptr}};
}  // namespace

HelpWindow::HelpWindow(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("helpWindow"));
  setWindowTitle(tr("SketchBRep ヘルプ"));
  setWindowFlag(Qt::WindowMaximizeButtonHint);
  for (const TopicFile& t : kTopics) {
    QString markdown;
    if (t.file) {
      QFile f(QStringLiteral(":/help/") + QString::fromLatin1(t.file));
      if (!f.open(QIODevice::ReadOnly)) continue;
      markdown = QString::fromUtf8(f.readAll());
    } else {
      markdown = QString::fromStdString(construction::guideMarkdown("timber"));
    }
    Topic topic{QString::fromLatin1(t.id), {}, markdown};
    // The title is the first heading.
    const QString first = topic.markdown.section(QLatin1Char('\n'), 0, 0);
    topic.title = first.startsWith(QLatin1String("# ")) ? first.mid(2).trimmed() : topic.id;
    topics_.push_back(std::move(topic));
  }

  auto* layout = new QVBoxLayout(this);
  search_ = new QLineEdit(this);
  search_->setObjectName(QStringLiteral("helpSearch"));
  search_->setPlaceholderText(tr("ヘルプ内を検索（例: フィレット、JWW、Ctrl）"));
  search_->setClearButtonEnabled(true);
  layout->addWidget(search_);
  auto* splitter = new QSplitter(this);
  list_ = new QListWidget(splitter);
  list_->setObjectName(QStringLiteral("helpTopics"));
  for (const Topic& t : topics_) {
    auto* item = new QListWidgetItem(t.title, list_);
    item->setData(Qt::UserRole, t.id);
  }
  text_ = new QTextBrowser(splitter);
  text_->setObjectName(QStringLiteral("helpText"));
  text_->setOpenExternalLinks(true);
  splitter->addWidget(list_);
  splitter->addWidget(text_);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({220, 640});
  layout->addWidget(splitter, 1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();
    for (const Topic& t : topics_) {
      if (t.id == id) text_->setMarkdown(t.markdown);
    }
    if (!search_->text().trimmed().isEmpty()) text_->find(search_->text().trimmed());
  });
  connect(search_, &QLineEdit::textChanged, this, &HelpWindow::search);
  resize(900, 640);
  if (list_->count() > 0) list_->setCurrentRow(0);
}

void HelpWindow::setConstructionOverrides(const std::string& patch) {
  if (patch == constructionPatch_) return;
  const QString markdown = QString::fromStdString(construction::guideMarkdown("timber", patch));
  constructionPatch_ = patch;
  for (Topic& t : topics_) {
    if (t.id != QLatin1String("construction_timber")) continue;
    t.markdown = markdown;
    if (currentTopic() == t.id) text_->setMarkdown(t.markdown);
  }
  search(search_->text());
}

QStringList HelpWindow::topicIds() const {
  QStringList out;
  for (const Topic& t : topics_) out << t.id;
  return out;
}

bool HelpWindow::showTopic(const QString& id) {
  for (int i = 0; i < list_->count(); ++i) {
    QListWidgetItem* item = list_->item(i);
    if (item->data(Qt::UserRole).toString() != id) continue;
    item->setHidden(false);
    list_->setCurrentItem(item);
    return true;
  }
  return false;
}

QString HelpWindow::currentTopic() const {
  return list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString() : QString();
}

void HelpWindow::search(const QString& text) {
  const QString needle = text.trimmed();
  QListWidgetItem* firstShown = nullptr;
  for (int i = 0; i < list_->count(); ++i) {
    QListWidgetItem* item = list_->item(i);
    const QString id = item->data(Qt::UserRole).toString();
    bool match = needle.isEmpty();
    for (const Topic& t : topics_) {
      if (t.id == id && !match) match = t.markdown.contains(needle, Qt::CaseInsensitive);
    }
    item->setHidden(!match);
    if (match && !firstShown) firstShown = item;
  }
  if (list_->currentItem() && !list_->currentItem()->isHidden()) {
    if (!needle.isEmpty()) {
      text_->moveCursor(QTextCursor::Start);
      text_->find(needle);
    }
  } else if (firstShown) {
    list_->setCurrentItem(firstShown);
  }
}

int HelpWindow::visibleTopicCount() const {
  int n = 0;
  for (int i = 0; i < list_->count(); ++i) n += list_->item(i)->isHidden() ? 0 : 1;
  return n;
}

QString bugReportText() {
  QStringList lines;
  lines << QStringLiteral("SketchBRep %1").arg(QApplication::applicationVersion())
        << QStringLiteral("System: %1 (%2)").arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture())
        << QString() << QStringLiteral("Last actions (oldest first):");
  for (const QString& a : recentActivity()) lines << QStringLiteral("  ") + a;
  return lines.join(QLatin1Char('\n'));
}

BugReportDialog::BugReportDialog(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("bugReportDialog"));
  setWindowTitle(tr("不具合を報告"));
  auto* layout = new QVBoxLayout(this);
  auto* intro = new QLabel(
      tr("販売ページの連絡先へ、次を送ってください。<br>"
         "1. 何をしたら、どうなったか（できれば手順）<br>"
         "2. 下の情報（「報告用の情報をコピー」で写せます）<br>"
         "3. 落ちたときは、記録のフォルダの <code>crash-…txt</code><br>"
         "4. 可能なら、そのときの <code>.sbr</code> ファイル<br><br>"
         "この情報は自動ではどこにも送りません。モデルの形やファイルの中身は含みません（ファイル名は含みます）。"),
      this);
  intro->setWordWrap(true);
  layout->addWidget(intro);
  auto* info = new QPlainTextEdit(bugReportText(), this);
  info->setObjectName(QStringLiteral("bugReportInfo"));
  info->setReadOnly(true);
  info->setLineWrapMode(QPlainTextEdit::NoWrap);
  layout->addWidget(info, 1);
  auto* buttons = new QDialogButtonBox(this);
  QPushButton* copy = buttons->addButton(tr("報告用の情報をコピー"), QDialogButtonBox::ActionRole);
  copy->setObjectName(QStringLiteral("copyBugReport"));
  QPushButton* folder = buttons->addButton(tr("記録のフォルダを開く"), QDialogButtonBox::ActionRole);
  folder->setEnabled(!crashLogDirectory().isEmpty());
  buttons->addButton(QDialogButtonBox::Close);
  connect(copy, &QPushButton::clicked, this, [info] { QApplication::clipboard()->setText(info->toPlainText()); });
  connect(folder, &QPushButton::clicked, this,
          [] { QDesktopServices::openUrl(QUrl::fromLocalFile(crashLogDirectory())); });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
  resize(640, 520);
}

}  // namespace sbr::app
