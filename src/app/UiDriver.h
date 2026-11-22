#pragma once
// docs/242: the app's windows read and worked by the AI on this PC:
// list a window's widgets and menu items, set a field, press a button or a menu item, as a person
// would. Used through AiBridge's ui_* commands (the AI on this PC; the owner can turn them off).

#include <string>

#include <nlohmann/json.hpp>

class QWidget;
class QMainWindow;
class QAction;

namespace sbr::app::ui {

// The window meant by `which`: "main", "modal" (the window waiting for an answer), "top" (that one,
// else the main window), or a visible window's title or object name. Null when none.
QWidget* window(QMainWindow* main, const std::string& which);

// The visible windows (title, object name, kind, modal).
nlohmann::json windows(QMainWindow* main);

// The window's widgets a person reads or works (buttons, fields, lists, tables, labels...), each
// with a `ref` to name it by (its object name, else "#n" in this order); `filter` keeps those whose
// ref, kind or text holds it. With `actions`, the main window's menu items too.
nlohmann::json list(QWidget* window, const std::string& filter, bool actions, int limit);

// The widget named by `ref` (object name, "#n" as list gives, or a button's text). Null when none.
QWidget* find(QWidget* window, const std::string& ref);

// Sets a field: text (line edit, text edit), a number (spin box), true/false (check box, radio
// button, checkable button), an item's text or data (combo box, list), or a table cell (`row`,
// `column`: the cell's text, or the combo box / spin box / check box in it). Throws when it cannot.
void set(QWidget* widget, const nlohmann::json& value, int row, int column);

// The menu item named by object name or by its text (with or without "&" and "..."), or by its
// menus' path "ファイル/図面一式を書き出す...". Null when none.
QAction* action(QMainWindow* main, const std::string& name);

}  // namespace sbr::app::ui
