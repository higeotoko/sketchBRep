#include "UiDriver.h"

#include <stdexcept>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTreeWidget>

namespace sbr::app::ui {

using nlohmann::json;

namespace {

std::string str(const QString& s) { return s.toStdString(); }

// What a person reads in the widget; empty for widgets not listed.
std::string kindOf(const QWidget* w) {
  if (qobject_cast<const QCheckBox*>(w)) return "check_box";
  if (qobject_cast<const QRadioButton*>(w)) return "radio_button";
  if (qobject_cast<const QAbstractButton*>(w)) return "button";
  if (qobject_cast<const QLineEdit*>(w)) {
    // A spin box's or combo box's own line edit is listed with it.
    const QWidget* p = w->parentWidget();
    if (qobject_cast<const QAbstractSpinBox*>(p) || qobject_cast<const QComboBox*>(p)) return {};
    return "line_edit";
  }
  if (qobject_cast<const QComboBox*>(w)) return "combo_box";
  if (qobject_cast<const QDoubleSpinBox*>(w) || qobject_cast<const QSpinBox*>(w)) return "spin_box";
  if (qobject_cast<const QPlainTextEdit*>(w) || qobject_cast<const QTextEdit*>(w)) return "text_edit";
  if (qobject_cast<const QTableWidget*>(w)) return "table";
  if (qobject_cast<const QListWidget*>(w)) return "list";
  if (qobject_cast<const QTreeWidget*>(w)) return "tree";
  if (qobject_cast<const QTabWidget*>(w)) return "tabs";
  if (qobject_cast<const QGroupBox*>(w)) return "group";
  if (qobject_cast<const QLabel*>(w)) return "label";
  if (qobject_cast<const QDockWidget*>(w)) return "panel";
  return {};
}

std::string cellText(const QTableWidget* t, int r, int c) {
  if (QWidget* cw = t->cellWidget(r, c)) {
    if (auto* cb = qobject_cast<QComboBox*>(cw)) return str(cb->currentText());
    if (auto* sb = qobject_cast<QSpinBox*>(cw)) return std::to_string(sb->value());
    if (auto* db = qobject_cast<QDoubleSpinBox*>(cw)) return str(db->text());
    if (auto* ck = qobject_cast<QCheckBox*>(cw)) return ck->isChecked() ? "true" : "false";
    if (auto* b = qobject_cast<QAbstractButton*>(cw)) return str(b->text());
    if (auto* le = qobject_cast<QLineEdit*>(cw)) return str(le->text());
    return {};
  }
  const QTableWidgetItem* item = t->item(r, c);
  if (!item) return {};
  std::string text = str(item->text());
  if (item->flags() & Qt::ItemIsUserCheckable) text += item->checkState() == Qt::Checked ? " [x]" : " [ ]";
  return text;
}

json describe(QWidget* w, const std::string& kind) {
  json d = {{"kind", kind}, {"enabled", w->isEnabled()}};
  if (!w->toolTip().isEmpty()) d["tooltip"] = str(w->toolTip());
  if (auto* b = qobject_cast<QAbstractButton*>(w)) {
    d["text"] = str(b->text());
    if (b->isCheckable()) d["checked"] = b->isChecked();
  } else if (auto* le = qobject_cast<QLineEdit*>(w)) {
    d["text"] = str(le->text());
    if (!le->placeholderText().isEmpty()) d["placeholder"] = str(le->placeholderText());
    d["read_only"] = le->isReadOnly();
  } else if (auto* cb = qobject_cast<QComboBox*>(w)) {
    d["text"] = str(cb->currentText());
    d["data"] = str(cb->currentData().toString());
    json items = json::array();
    for (int i = 0; i < cb->count() && i < 60; ++i) items.push_back(str(cb->itemText(i)));
    d["items"] = items;
  } else if (auto* sb = qobject_cast<QAbstractSpinBox*>(w)) {
    d["text"] = str(sb->text());
  } else if (auto* pe = qobject_cast<QPlainTextEdit*>(w)) {
    d["text"] = str(pe->toPlainText());
  } else if (auto* te = qobject_cast<QTextEdit*>(w)) {
    d["text"] = str(te->toPlainText()).substr(0, 4000);
  } else if (auto* t = qobject_cast<QTableWidget*>(w)) {
    json headers = json::array();
    for (int c = 0; c < t->columnCount(); ++c)
      headers.push_back(t->horizontalHeaderItem(c) ? str(t->horizontalHeaderItem(c)->text()) : std::string());
    json rows = json::array();
    for (int r = 0; r < t->rowCount() && r < 200; ++r) {
      json row = json::array();
      for (int c = 0; c < t->columnCount(); ++c) row.push_back(cellText(t, r, c));
      rows.push_back(row);
    }
    d["headers"] = headers;
    d["rows"] = rows;
    d["row_count"] = t->rowCount();
    d["current_row"] = t->currentRow();
  } else if (auto* l = qobject_cast<QListWidget*>(w)) {
    json items = json::array();
    for (int i = 0; i < l->count() && i < 200; ++i) items.push_back(str(l->item(i)->text()));
    d["items"] = items;
    d["current_row"] = l->currentRow();
  } else if (auto* tr = qobject_cast<QTreeWidget*>(w)) {
    json items = json::array();
    for (int i = 0; i < tr->topLevelItemCount() && i < 200; ++i) items.push_back(str(tr->topLevelItem(i)->text(0)));
    d["items"] = items;
  } else if (auto* tabs = qobject_cast<QTabWidget*>(w)) {
    json names = json::array();
    for (int i = 0; i < tabs->count(); ++i) names.push_back(str(tabs->tabText(i)));
    d["tabs"] = names;
    d["current"] = tabs->currentIndex();
  } else if (auto* g = qobject_cast<QGroupBox*>(w)) {
    d["text"] = str(g->title());
    if (g->isCheckable()) d["checked"] = g->isChecked();
  } else if (auto* lb = qobject_cast<QLabel*>(w)) {
    d["text"] = str(lb->text()).substr(0, 2000);
  } else if (auto* dock = qobject_cast<QDockWidget*>(w)) {
    d["text"] = str(dock->windowTitle());
  }
  return d;
}

// The widgets in the order list gives them (refs "#n" count in this order).
std::vector<std::pair<QWidget*, std::string>> listed(QWidget* window) {
  std::vector<std::pair<QWidget*, std::string>> out;
  for (QWidget* w : window->findChildren<QWidget*>()) {
    if (!w->isVisibleTo(window) || w->window() != window) continue;
    const std::string kind = kindOf(w);
    if (kind.empty()) continue;
    if (kind == "label" && qobject_cast<QLabel*>(w)->text().isEmpty()) continue;
    out.emplace_back(w, kind);
  }
  return out;
}

std::string bare(QString text) {
  text.remove(QLatin1Char('&'));
  text = text.trimmed();
  while (text.endsWith(QLatin1Char('.')) || text.endsWith(QChar(0x2026))) text.chop(1);
  return str(text.trimmed());
}

void collectActions(QMenu* menu, const std::string& path, std::vector<std::pair<QAction*, std::string>>& out) {
  for (QAction* a : menu->actions()) {
    if (a->isSeparator()) continue;
    const std::string here = path.empty() ? bare(a->text()) : path + "/" + bare(a->text());
    if (a->menu()) collectActions(a->menu(), here, out);
    else out.emplace_back(a, here);
  }
}

std::vector<std::pair<QAction*, std::string>> menuActions(QMainWindow* main) {
  std::vector<std::pair<QAction*, std::string>> out;
  for (QAction* top : main->menuBar()->actions())
    if (top->menu()) collectActions(top->menu(), bare(top->text()), out);
  return out;
}

}  // namespace

QWidget* window(QMainWindow* main, const std::string& which) {
  if (which == "main") return main;
  QWidget* modal = QApplication::activeModalWidget();
  if (which == "modal") return modal;
  if (which.empty() || which == "top") return modal ? modal : main;
  for (QWidget* w : QApplication::topLevelWidgets())
    if (w->isVisible() && (str(w->windowTitle()) == which || str(w->objectName()) == which)) return w;
  return nullptr;
}

json windows(QMainWindow* main) {
  json out = json::array();
  QWidget* modal = QApplication::activeModalWidget();
  for (QWidget* w : QApplication::topLevelWidgets()) {
    if (!w->isVisible() || qobject_cast<QMenu*>(w)) continue;
    out.push_back({{"title", str(w->windowTitle())}, {"object_name", str(w->objectName())}, {"class", w->metaObject()->className()},
                   {"main", w == main}, {"modal", w == modal}});
  }
  return out;
}

json list(QWidget* window, const std::string& filter, bool actions, int limit) {
  json widgets = json::array();
  int n = 0, shown = 0;
  for (const auto& [w, kind] : listed(window)) {
    ++n;
    const std::string ref = w->objectName().isEmpty() ? "#" + std::to_string(n) : str(w->objectName());
    json d = describe(w, kind);
    d["ref"] = ref;
    if (!filter.empty()) {
      const std::string hay = ref + " " + kind + " " + d.value("text", std::string()) + " " + d.value("tooltip", std::string());
      if (hay.find(filter) == std::string::npos) continue;
    }
    if (shown++ >= limit) break;
    widgets.push_back(d);
  }
  json out = {{"title", str(window->windowTitle())}, {"widgets", widgets}, {"widget_count", n}};
  if (actions)
    if (auto* main = qobject_cast<QMainWindow*>(window)) {
      json items = json::array();
      for (const auto& [a, path] : menuActions(main)) {
        if (!filter.empty() && path.find(filter) == std::string::npos && str(a->objectName()).find(filter) == std::string::npos) continue;
        json d = {{"path", path}, {"enabled", a->isEnabled()}};
        if (!a->objectName().isEmpty()) d["object_name"] = str(a->objectName());
        if (a->isCheckable()) d["checked"] = a->isChecked();
        items.push_back(d);
      }
      out["menu_items"] = items;
    }
  return out;
}

QWidget* find(QWidget* window, const std::string& ref) {
  if (ref.empty()) return nullptr;
  const auto all = listed(window);
  if (ref[0] == '#') {
    const int n = std::atoi(ref.c_str() + 1);
    return n >= 1 && n <= static_cast<int>(all.size()) ? all[static_cast<std::size_t>(n - 1)].first : nullptr;
  }
  for (const auto& [w, kind] : all)
    if (str(w->objectName()) == ref) return w;
  if (QWidget* named = window->findChild<QWidget*>(QString::fromStdString(ref))) return named;
  for (const auto& [w, kind] : all)
    if (auto* b = qobject_cast<QAbstractButton*>(w))
      if (bare(b->text()) == bare(QString::fromStdString(ref))) return w;
  return nullptr;
}

namespace {
void setOne(QWidget* w, const json& value) {
  const auto text = [&] { return QString::fromStdString(value.is_string() ? value.get<std::string>() : value.dump()); };
  if (auto* ck = qobject_cast<QAbstractButton*>(w)) {
    if (!ck->isCheckable()) throw std::runtime_error("the button is not one to set; use ui_click");
    if (!value.is_boolean()) throw std::runtime_error("'value': true or false");
    ck->setChecked(value.get<bool>());
  } else if (auto* le = qobject_cast<QLineEdit*>(w)) {
    le->setText(text());
    emit le->textEdited(le->text());
    emit le->editingFinished();
  } else if (auto* cb = qobject_cast<QComboBox*>(w)) {
    int i = cb->findText(text());
    if (i < 0) i = cb->findData(text());
    if (i < 0 && value.is_number_integer()) i = value.get<int>();
    if (i < 0 || i >= cb->count()) {
      if (!cb->isEditable()) throw std::runtime_error("no such item in the list: " + text().toStdString());
      cb->setEditText(text());
    } else {
      cb->setCurrentIndex(i);
    }
  } else if (auto* sb = qobject_cast<QSpinBox*>(w)) {
    sb->setValue(value.is_number() ? value.get<int>() : text().toInt());
  } else if (auto* db = qobject_cast<QDoubleSpinBox*>(w)) {
    db->setValue(value.is_number() ? value.get<double>() : text().toDouble());
  } else if (auto* pe = qobject_cast<QPlainTextEdit*>(w)) {
    pe->setPlainText(text());
  } else if (auto* te = qobject_cast<QTextEdit*>(w)) {
    te->setPlainText(text());
  } else if (auto* l = qobject_cast<QListWidget*>(w)) {
    const auto found = l->findItems(text(), Qt::MatchExactly);
    if (found.isEmpty()) throw std::runtime_error("no such item in the list: " + text().toStdString());
    l->setCurrentItem(found.front());
  } else if (auto* tabs = qobject_cast<QTabWidget*>(w)) {
    for (int i = 0; i < tabs->count(); ++i)
      if (tabs->tabText(i) == text()) return tabs->setCurrentIndex(i);
    throw std::runtime_error("no such tab: " + text().toStdString());
  } else if (auto* g = qobject_cast<QGroupBox*>(w)) {
    if (!g->isCheckable() || !value.is_boolean()) throw std::runtime_error("the group cannot be set");
    g->setChecked(value.get<bool>());
  } else {
    throw std::runtime_error(std::string("this kind of widget cannot be set: ") + w->metaObject()->className());
  }
}
}  // namespace

void set(QWidget* widget, const json& value, int row, int column) {
  if (!widget->isEnabled()) throw std::runtime_error("the widget is greyed out (disabled)");
  if (auto* t = qobject_cast<QTableWidget*>(widget); t && row >= 0) {
    if (row >= t->rowCount() || column < 0 || column >= t->columnCount()) throw std::runtime_error("no such cell");
    t->setCurrentCell(row, column);
    if (QWidget* cw = t->cellWidget(row, column)) return setOne(cw, value);
    QTableWidgetItem* item = t->item(row, column);
    if (!item) {
      item = new QTableWidgetItem;
      t->setItem(row, column, item);
    }
    if (value.is_boolean() && (item->flags() & Qt::ItemIsUserCheckable)) {
      item->setCheckState(value.get<bool>() ? Qt::Checked : Qt::Unchecked);
      return;
    }
    if (!(item->flags() & Qt::ItemIsEditable)) throw std::runtime_error("the cell cannot be edited");
    item->setText(QString::fromStdString(value.is_string() ? value.get<std::string>() : value.dump()));
    return;
  }
  if (auto* l = qobject_cast<QListWidget*>(widget); l && row >= 0) {
    if (row >= l->count()) throw std::runtime_error("no such row");
    l->setCurrentRow(row);
    return;
  }
  setOne(widget, value);
}

QAction* action(QMainWindow* main, const std::string& name) {
  if (QAction* a = main->findChild<QAction*>(QString::fromStdString(name))) return a;
  const std::string wanted = bare(QString::fromStdString(name));
  QAction* byText = nullptr;
  for (const auto& [a, path] : menuActions(main)) {
    if (path == wanted || bare(QString::fromStdString(path)) == wanted) return a;
    if (!byText && bare(a->text()) == wanted) byText = a;
  }
  if (byText) return byText;
  for (QAction* a : main->findChildren<QAction*>())
    if (bare(a->text()) == wanted) return a;
  return nullptr;
}

}  // namespace sbr::app::ui
