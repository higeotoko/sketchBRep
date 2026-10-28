#include <QtTest/QtTest>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVariantAnimation>
#include "ToolPalette.h"
#include "Cursors.h"
#include "Branding.h"

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
void titleMouse(QWidget* title, QEvent::Type type, QPoint global,
                Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  QMouseEvent event(type, QPointF(title->mapFromGlobal(global)), QPointF(global),
                    type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                    type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
  QApplication::sendEvent(title, &event);
  events();
}
struct DockRig : QMainWindow {
  PanelDock* dock;
  DockRig() {
    setDockOptions(dockOptions() & ~QMainWindow::AnimatedDocks);
    setCentralWidget(new QWidget);
    resize(800, 600); move(100, 100);
    dock = new PanelDock(QStringLiteral("属性"), this);
    dock->setWidget(new QWidget);
    addDockWidget(Qt::RightDockWidgetArea, dock); show(); events();
    dock->setFloating(true); dock->resize(180, 160);
    dock->move(mapToGlobal(QPoint(250, 180))); dock->show(); events();
  }
  void press() { titleMouse(dock->titleBarWidget(), QEvent::MouseButtonPress, dock->titleBarWidget()->mapToGlobal(QPoint(10, 12))); }
  void go(QPoint point, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    titleMouse(dock->titleBarWidget(), QEvent::MouseMove, mapToGlobal(point), modifiers);
  }
  void release(QPoint point) { titleMouse(dock->titleBarWidget(), QEvent::MouseButtonRelease, mapToGlobal(point)); }
  QWidget* guide() { return findChild<QWidget*>(QStringLiteral("panelDockPreview")); }
  QList<QWidget*> guides() {
    QList<QWidget*> result;
    for (auto* w : findChildren<QWidget*>()) if (w->objectName().startsWith(QStringLiteral("panelDockPreview"))) result.append(w);
    return result;
  }
};
QVariantAnimation* previewMotion(QWidget* guide) {
  return guide ? guide->findChild<QVariantAnimation*>(QStringLiteral("dockPreviewMotion")) : nullptr;
}
void finishPreviews(DockRig& rig) {
  for (auto* guide : rig.guides()) if (auto* motion = previewMotion(guide)) motion->setCurrentTime(motion->duration());
  events();
}
qint64 alphaSum(QWidget* guide) {
  const auto image = guide->grab().toImage();
  qint64 sum = 0;
  for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) sum += image.pixelColor(x, y).alpha();
  return sum;
}
}

class ToolPaletteTests : public QObject {
  Q_OBJECT
 private slots:
  void previewAppearsSettlesAndStopsUpdating() {
    DockRig rig;
    rig.press(); rig.go(QPoint(400, 12));
    auto* guide = rig.guide();
    QVERIFY(guide && guide->isVisible());
    auto* motion = previewMotion(guide); QVERIFY(motion);
    QCOMPARE(motion->duration(), 140);
    QCOMPARE(motion->easingCurve().type(), QEasingCurve::OutCubic);
    motion->setCurrentTime(0); events();
    QCOMPARE(alphaSum(guide), qint64(0));
    motion->setCurrentTime(50); events();
    const auto middle = alphaSum(guide);
    motion->setCurrentTime(140); events();
    const auto held = alphaSum(guide);
    QVERIFY(middle > 0 && middle < held);
    QCOMPARE(motion->state(), QAbstractAnimation::Stopped);
    struct PaintCounter : QObject {
      int count = 0;
      bool eventFilter(QObject*, QEvent* e) override { if (e->type() == QEvent::Paint) ++count; return false; }
    } paints;
    guide->installEventFilter(&paints); events(); paints.count = 0;
    QTest::qWait(50);
    QCOMPARE(paints.count, 0); // no permanent pulse or repaint timer
    rig.release(QPoint(400, 12));
    QVERIFY(!rig.dock->isFloating()); // placement never waits for the animation
    QVERIFY(guide->isVisible());
    QCOMPARE(motion->duration(), 160);
    QCOMPARE(motion->state(), QAbstractAnimation::Running);
    motion->setCurrentTime(80); events();
    QVERIFY(alphaSum(guide) < held / 2);
    motion->setCurrentTime(160); events();
    QVERIFY(!guide->isVisible());
    QCOMPARE(motion->state(), QAbstractAnimation::Stopped);
  }

  void previewTransitionsLeaveNoControlOrEscapeResidue() {
    DockRig rig;
    rig.press(); rig.go(QPoint(400, 12)); finishPreviews(rig);
    QPointer<QWidget> old = rig.guide();
    rig.go(QPoint(790, 300));
    QVERIFY(rig.guide() != old);
    QVERIFY(old->isVisible() && rig.guide()->isVisible());
    QCOMPARE(previewMotion(old)->duration(), 90);
    for (auto* guide : rig.guides()) QVERIFY(std::min(guide->width(), guide->height()) <= 36);
    finishPreviews(rig);
    QVERIFY(!old->isVisible() && rig.guide()->isVisible());
    rig.go(QPoint(400, 150)); // normal leave fades out
    QVERIFY(rig.guide()->isVisible());
    rig.go(QPoint(790, 300)); // rapid reentry replaces, rather than queues, animations
    finishPreviews(rig);
    QVERIFY(rig.guides().size() <= 2);
    QTest::keyPress(rig.dock->titleBarWidget(), Qt::Key_Control); events();
    for (auto* guide : rig.guides()) { QVERIFY(!guide->isVisible()); QCOMPARE(previewMotion(guide)->state(), QAbstractAnimation::Stopped); }
    QTest::keyRelease(rig.dock->titleBarWidget(), Qt::Key_Control);
    rig.go(QPoint(12, 300));
    QVERIFY(rig.guide()->isVisible());
    // Mouse modifiers alone also suppress the guide without a fading ghost.
    rig.go(QPoint(12, 300), Qt::ControlModifier);
    for (auto* guide : rig.guides()) QVERIFY(!guide->isVisible());
    rig.go(QPoint(12, 300));
    QTest::keyClick(rig.dock->titleBarWidget(), Qt::Key_Escape); events();
    for (auto* guide : rig.guides()) { QVERIFY(!guide->isVisible()); QCOMPARE(previewMotion(guide)->state(), QAbstractAnimation::Stopped); }
  }

  void earlyDropDoesNotFlashAtFullOpacity() {
    DockRig rig;
    rig.press(); rig.go(QPoint(400, 12));
    auto* guide = rig.guide(); QVERIFY(guide);
    auto* motion = previewMotion(guide); QVERIFY(motion);
    motion->setCurrentTime(20); events();
    const auto early = alphaSum(guide);
    QVERIFY(early > 0);
    rig.release(QPoint(400, 12));
    QVERIFY(!rig.dock->isFloating());
    // A rapid drop fades from the partly revealed guide, rather than
    // introducing a fully opaque confirmation frame.
    QVERIFY(alphaSum(guide) <= early * 1.3);
    motion->setCurrentTime(motion->duration()); events();
    QVERIFY(!guide->isVisible());
  }

  void rapidCrossingConsecutivePanelsAndCloseCleanUp() {
    DockRig rig;
    rig.press(); rig.go(QPoint(400, 12));
    rig.go(QPoint(400, 150)); rig.release(QPoint(400, 150));
    QTest::qWait(190);
    for (auto* guide : rig.guides()) QVERIFY(!guide->isVisible());
    rig.press(); rig.go(QPoint(400, 12)); finishPreviews(rig);
    rig.release(QPoint(400, 12));
    QVERIFY(rig.guide()->isVisible()); // settling from the first panel
    auto* other = new PanelDock(QStringLiteral("素材"), &rig);
    other->setWidget(new QWidget); rig.addDockWidget(Qt::RightDockWidgetArea, other);
    other->setFloating(true); other->show(); events();
    titleMouse(other->titleBarWidget(), QEvent::MouseButtonPress, other->titleBarWidget()->mapToGlobal(QPoint(10, 12)));
    for (auto* guide : rig.guides()) QVERIFY(!guide->isVisible());
    titleMouse(other->titleBarWidget(), QEvent::MouseMove, rig.mapToGlobal(QPoint(790, 300)));
    other->hide(); events();
    for (auto* guide : rig.guides()) { QVERIFY(!guide->isVisible()); QCOMPARE(previewMotion(guide)->state(), QAbstractAnimation::Stopped); }
    delete other;
    QTest::qWait(190);
    for (auto* guide : rig.guides()) QVERIFY(!guide->isVisible());
  }

  void applicationIconUsesItsSmallCanvas() {
    const auto icon = applicationIcon();
    const auto available = icon.availableSizes();
    for (int size : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256}) {
      QVERIFY(available.contains(QSize(size, size)));
      const auto image = icon.pixmap(QSize(size, size)).toImage();
      QVERIFY(!image.isNull());
      QRect content;
      for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
          if (image.pixelColor(x, y).alpha() > 8) content = content.united(QRect(x, y, 1, 1));
      QVERIFY(content.width() >= size * .85);
      QVERIFY(content.height() >= size * .85);
      QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
    }
  }

  void edgeTargetsAreNarrowAndStable() {
    const QRect bounds(100, 100, 800, 600);
    QCOMPARE(panelDockTarget(bounds, {500, 117}, Qt::AllDockWidgetAreas), Qt::NoDockWidgetArea);
    QCOMPARE(panelDockTarget(bounds, {500, 116}, Qt::AllDockWidgetAreas), Qt::TopDockWidgetArea);
    QCOMPARE(panelDockTarget(bounds, {500, 128}, Qt::AllDockWidgetAreas, Qt::TopDockWidgetArea), Qt::TopDockWidgetArea);
    QCOMPARE(panelDockTarget(bounds, {500, 129}, Qt::AllDockWidgetAreas, Qt::TopDockWidgetArea), Qt::NoDockWidgetArea);
    QCOMPARE(panelDockTarget(bounds, {500, 110}, Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea), Qt::NoDockWidgetArea);
    QCOMPARE(panelDockTarget(bounds, {110, 400}, Qt::LeftDockWidgetArea), Qt::LeftDockWidgetArea);
    QCOMPARE(panelDockTarget(bounds, {890, 400}, Qt::RightDockWidgetArea), Qt::RightDockWidgetArea);
    QCOMPARE(panelDockTarget(bounds, {500, 690}, Qt::BottomDockWidgetArea), Qt::BottomDockWidgetArea);
    QCOMPARE(panelDockTarget(bounds, {500, 91}, Qt::AllDockWidgetAreas), Qt::NoDockWidgetArea);
  }

  void previewDoesNotResizeTheEditingArea() {
    QMainWindow host;
    host.setDockOptions(host.dockOptions() & ~QMainWindow::AnimatedDocks);
    host.setCentralWidget(new QWidget);
    host.resize(800, 600); host.move(100, 100);
    auto* dock = new PanelDock(QStringLiteral("属性"), &host);
    dock->setWidget(new QWidget);
    host.addDockWidget(Qt::RightDockWidgetArea, dock);
    host.show(); events();
    dock->setFloating(true); dock->resize(180, 160);
    dock->move(host.mapToGlobal(QPoint(250, 180))); dock->show(); events();
    auto* title = dock->titleBarWidget();
    const auto central = host.centralWidget()->geometry();
    const QPoint press = title->mapToGlobal(QPoint(10, 12));
    titleMouse(title, QEvent::MouseButtonPress, press);
    titleMouse(title, QEvent::MouseMove, host.mapToGlobal(QPoint(400, 100)));
    for (int i = 0; i < 120; ++i)
      titleMouse(title, QEvent::MouseMove, host.mapToGlobal(QPoint(300 + i, 12 + i % 10)));
    QCOMPARE(host.centralWidget()->geometry(), central);
    QVERIFY(dock->isFloating());
    auto* preview = host.findChild<QWidget*>(QStringLiteral("panelDockPreview"));
    QVERIFY(preview && preview->isVisible());
    QCOMPARE(preview->height(), 36);
    // Holding Ctrl suppresses the preview even before another mouse move.
    QTest::keyPress(title, Qt::Key_Control); events();
    QVERIFY(!preview->isVisible());
    QTest::keyRelease(title, Qt::Key_Control);
    titleMouse(title, QEvent::MouseMove, host.mapToGlobal(QPoint(400, 12)));
    QVERIFY(preview->isVisible());
    auto* motion = preview->findChild<QVariantAnimation*>(QStringLiteral("dockPreviewMotion"));
    QVERIFY(motion);
    motion->setCurrentTime(motion->duration()); events();
    titleMouse(title, QEvent::MouseButtonRelease, host.mapToGlobal(QPoint(400, 12)));
    QVERIFY(!dock->isFloating());
    QCOMPARE(host.dockWidgetArea(dock), Qt::TopDockWidgetArea);
    QVERIFY(preview->isVisible()); // native docking is done while only the guide settles
    QCOMPARE(motion->state(), QAbstractAnimation::Running);
    QCOMPARE(motion->duration(), 160);
    motion->setCurrentTime(motion->duration()); events();
    QVERIFY(!preview->isVisible());

    // Escape returns a detached panel to its former dock, without leaving
    // the drag active or handing Escape on to a modeling tool.
    titleMouse(title, QEvent::MouseButtonPress, title->mapToGlobal(QPoint(10, 12)));
    titleMouse(title, QEvent::MouseMove, host.mapToGlobal(QPoint(350, 250)));
    QVERIFY(dock->isFloating());
    QTest::keyClick(title, Qt::Key_Escape); events();
    QVERIFY(!dock->isFloating());
    QCOMPARE(host.dockWidgetArea(dock), Qt::TopDockWidgetArea);
  }

  void titleButtonsAndTabsRemainUsable() {
    QMainWindow host;
    auto* a = new PanelDock(QStringLiteral("素材"), &host);
    auto* b = new PanelDock(QStringLiteral("タグ"), &host);
    a->setWidget(new QWidget); b->setWidget(new QWidget);
    host.addDockWidget(Qt::RightDockWidgetArea, a);
    host.addDockWidget(Qt::RightDockWidgetArea, b);
    host.tabifyDockWidget(a, b); host.show(); a->raise(); events();
    auto* floating = a->findChild<QAbstractButton*>(QStringLiteral("panelFloatButton"));
    auto* close = a->findChild<QAbstractButton*>(QStringLiteral("panelCloseButton"));
    QVERIFY(floating && close);
    QTest::mouseClick(floating, Qt::LeftButton); events();
    QVERIFY(a->isFloating());
    QVERIFY(a->windowFlags().testFlag(Qt::FramelessWindowHint));
    QTest::mouseClick(floating, Qt::LeftButton); events();
    QVERIFY(!a->isFloating());
    QVERIFY(host.tabifiedDockWidgets(a).contains(b));
    QTest::mouseDClick(a->titleBarWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(10, 12)); events();
    QVERIFY(a->isFloating());
    QTest::mouseDClick(a->titleBarWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(10, 12)); events();
    QVERIFY(!a->isFloating());
    QVERIFY(host.tabifiedDockWidgets(a).contains(b));
    QTest::mouseClick(close, Qt::LeftButton); events();
    QVERIFY(!a->isVisible());
    a->toggleViewAction()->trigger(); events();
    QVERIFY(a->isVisible());
  }

  void titleUsesThePanelTheme() {
    for (bool dark : {false, true}) {
      QMainWindow host;
      auto* dock = new PanelDock(QStringLiteral("ツール"), &host);
      QPalette palette = dock->palette();
      const QColor base(dark ? "#25272b" : "#f3f4f6");
      palette.setColor(QPalette::Window, base);
      palette.setColor(QPalette::WindowText, dark ? QColor("#e2e5ea") : QColor("#22262e"));
      dock->setPalette(palette); dock->setWidget(new QWidget);
      host.addDockWidget(Qt::LeftDockWidgetArea, dock); host.show();
      dock->setFloating(true); dock->resize(240, 160); dock->show(); events();
      const auto image = dock->titleBarWidget()->grab().toImage();
      QCOMPARE(image.pixelColor(1, 1).rgb(), (dark ? base.lighter(112) : base.darker(104)).rgb());
      QVERIFY(dock->windowFlags().testFlag(Qt::FramelessWindowHint));
    }
  }

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
      QMainWindow host;
      auto* dock = new PanelDock(QStringLiteral("マテリアル（素材）"), &host);
      auto* body = new QWidget;
      body->setMinimumSize(220, 100);
      dock->setWidget(body);
      host.addDockWidget(Qt::RightDockWidgetArea, dock); host.show();
      dock->setFloating(true); dock->resize(260, 180); dock->show(); events();
      dock->grab().save(output + (dark ? "/panel-dark.png" : "/panel-light.png"));
    }
    QApplication::setPalette(original);
  }
};

QTEST_MAIN(ToolPaletteTests)
#include "test_tool_palette.moc"
