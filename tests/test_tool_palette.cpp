#include <QtTest/QtTest>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QMainWindow>
#include <QPainter>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include "ToolPalette.h"
#include "Cursors.h"

using namespace sbr::app;

namespace {
QList<QAction*> populate(ToolPalette& tools, int count = 25) {
  QList<QAction*> actions;
  const QStringList names{QStringLiteral("選択"), QStringLiteral("線"), QStringLiteral("プッシュ/プル"),
                          QStringLiteral("フォローミー"), QStringLiteral("回転矩形")};
  for (int i = 0; i < count; ++i) {
    auto* action = new QAction(toolIcon(static_cast<CursorKind>(i % 24)), names[i % names.size()], &tools);
    action->setCheckable(true); action->setToolTip(action->text() + QStringLiteral(" (M)"));
    tools.addTool(action); actions.push_back(action);
  }
  return actions;
}
void events() { QCoreApplication::processEvents(); }
}

class ToolPaletteTests : public QObject {
  Q_OBJECT
 private slots:
  void narrowAndWideKeepEveryButtonAccessible() {
    ToolPalette tools;
    const auto actions = populate(tools);
    tools.show();
    for (bool labels : {false, true, false}) {
      tools.setShowLabels(labels); events();
      const auto fixed = tools.cellSize();
      if (!labels) QCOMPARE(fixed, QSize(36, 36));
      for (int width : {1, 76, 128, 220, 480, 1100}) {
        tools.resize(width, 160); events();
        QVERIFY(!tools.horizontalScrollBar()->isVisible());
        const auto buttons = tools.findChildren<QToolButton*>();
        QCOMPARE(buttons.size(), 25);
        for (int i = 0; i < buttons.size(); ++i) {
          auto* button = buttons[i];
          QCOMPARE(button->size(), fixed);
          QVERIFY(button->geometry().left() >= 0);
          QVERIFY2(button->geometry().right() < tools.viewport()->width(),
                   qPrintable(QStringLiteral("width=%1 button=%2 viewport=%3 right=%4")
                              .arg(width).arg(i).arg(tools.viewport()->width()).arg(button->geometry().right())));
          tools.ensureButtonVisible(button); events();
          QVERIFY(tools.viewport()->rect().contains(button->geometry()));
          QSignalSpy clicks(actions[i], &QAction::triggered);
          QTest::mouseClick(button, Qt::LeftButton);
          QCOMPARE(clicks.count(), 1);
          QVERIFY(!button->menu());
        }
      }
    }
  }

  void rowCountAndScrollbarRecoverAfterResizing() {
    ToolPalette tools;
    populate(tools); tools.show();
    tools.resize(130, 90); events();
    QVERIFY(tools.verticalScrollBar()->maximum() > 0);
    tools.resize(130, 450); events();
    QCOMPARE(tools.verticalScrollBar()->maximum(), 0);
    auto buttons = tools.findChildren<QToolButton*>();
    QCOMPARE(buttons[2]->y(), buttons[0]->y());
    tools.resize(700, 90); events();
    const auto size = buttons.front()->size();
    tools.resize(1100, 600); events();
    QCOMPARE(buttons.front()->size(), size);
    QCOMPARE(buttons.back()->y(), buttons.front()->y());
  }

  void sectionFloatingWindowFitsAndRedocks() {
    QMainWindow host;
    host.resize(800, 600);
    auto* dock = new ToolPaletteDock(QStringLiteral("断面"), &host, 4, true);
    populate(*dock->tools(), 4);
    host.addDockWidget(Qt::TopDockWidgetArea, dock);
    host.show(); events();
    dock->setFloating(true); dock->show(); dock->resetSize(); events();
    const auto compact = dock->size();
    dock->resize(900, 700); events();
    QCOMPARE(dock->size(), compact);
    const auto buttons = dock->findChildren<QToolButton*>();
    QCOMPARE(buttons.size(), 4);
    for (auto* button : buttons) {
      QCOMPARE(button->size(), QSize(36, 36));
      QVERIFY(dock->tools()->viewport()->rect().contains(button->geometry()));
    }
    dock->setFloating(false); events();
    QVERIFY(!dock->isFloating());
    QVERIFY(dock->tools()->maximumWidth() > 1000);
    for (auto* button : buttons) QCOMPARE(button->size(), QSize(36, 36));
    dock->setFloating(true); events();
    dock->resize(20, 20); events();
    QCOMPARE(dock->size(), compact);
  }

  void iconsAndCursorHotspotsAtEveryScale() {
    for (qreal scale : {1.0, 1.5, 2.0}) {
      for (int i = 0; i <= static_cast<int>(CursorKind::SectionFill); ++i) {
        const auto image = toolIcon(static_cast<CursorKind>(i)).pixmap(QSize(24, 24), scale);
        QCOMPARE(image.size(), QSize(qRound(24*scale), qRound(24*scale)));
        QCOMPARE(image.devicePixelRatioF(), scale);
        bool visible = false;
        const auto pixels = image.toImage();
        for (int y = 0; y < pixels.height() && !visible; ++y)
          for (int x = 0; x < pixels.width(); ++x)
            if (pixels.pixelColor(x, y).alpha() > 100) { visible = true; break; }
        QVERIFY(visible);
      }
    }
    const auto move = toolCursor(CursorKind::Move);
    QCOMPARE(move.hotSpot(), QPoint(16, 16));
    const auto image = move.pixmap().toImage();
    const qreal dpr = move.pixmap().devicePixelRatioF();
    QVERIFY(image.pixelColor(qRound(16*dpr), qRound(16*dpr)).alpha() > 128);
    QVERIFY(image.pixelColor(qRound(12*dpr), qRound(16*dpr)).alpha() < 64); // open center, not a crossing stroke
    for (auto kind : {CursorKind::Pencil, CursorKind::Rotate, CursorKind::Scale})
      QCOMPARE(toolCursor(kind).hotSpot(), QPoint(3, 2));
  }

  void exportVisualSamples() {
    const auto output = qEnvironmentVariable("SBR_PALETTE_SCREENSHOTS");
    if (output.isEmpty()) return;
    QDir().mkpath(output);
    const auto original = QApplication::palette();
    for (bool dark : {false, true}) {
      QPalette palette = original;
      palette.setColor(QPalette::Window, dark ? QColor("#25272b") : QColor("#f3f4f6"));
      palette.setColor(QPalette::Button, palette.window().color());
      palette.setColor(QPalette::ButtonText, dark ? QColor("#e2e5ea") : QColor("#22262e"));
      palette.setColor(QPalette::WindowText, palette.buttonText().color());
      palette.setColor(QPalette::Base, palette.window().color());
      palette.setColor(QPalette::AlternateBase, dark ? QColor("#34383f") : QColor("#e8ebf0"));
      palette.setColor(QPalette::Highlight, QColor("#2868ce"));
      palette.setColor(QPalette::HighlightedText, Qt::white);
      QApplication::setPalette(palette);
      ToolPalette tools;
      const auto actions = populate(tools); actions[10]->setChecked(true);
      tools.resize(130, 380); tools.show(); events();
      tools.grab().save(output + (dark ? "/tools-dark.png" : "/tools-light.png"));
      tools.setShowLabels(true); tools.resize(240, 450); events();
      tools.grab().save(output + (dark ? "/labels-dark.png" : "/labels-light.png"));
      QPixmap cursors(320, 80); cursors.fill(palette.window().color());
      QPainter painter(&cursors);
      int x = 20;
      for (auto kind : {CursorKind::Move, CursorKind::Rotate, CursorKind::Scale, CursorKind::Pencil, CursorKind::Rectangle, CursorKind::Paint}) {
        const auto cursor = toolCursor(kind);
        const QPoint target(x, 24);
        painter.setPen(QPen(QColor("#458ff0"), 1));
        painter.drawLine(target - QPoint(10, 0), target + QPoint(10, 0));
        painter.drawLine(target - QPoint(0, 10), target + QPoint(0, 10));
        painter.drawPixmap(target - cursor.hotSpot(), cursor.pixmap());
        x += 50;
      }
      painter.end(); cursors.save(output + (dark ? "/cursors-dark.png" : "/cursors-light.png"));
    }
    QApplication::setPalette(original);
  }
};

QTEST_MAIN(ToolPaletteTests)
#include "test_tool_palette.moc"
