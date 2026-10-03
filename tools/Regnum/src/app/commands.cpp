// Regnum — встроенные команды оболочки (палитра Ctrl+K, сочетания, справка F1).
#include "app/app_internal.h"
#include "base/fs.h"

namespace rg::app {

namespace {

bool inEditor(App& a) { return a.ui.screen == Screen::Editor; }
bool onMap(App& a) { return a.ui.screen == Screen::Editor && a.ui.editor.empty(); }
bool editable(App& a) { return a.ui.screen == Screen::Editor && !a.readOnly(); }

template <int N>
void setMode(App& a) {
  a.setMapMode(schema::MapMode(N));
}
template <int N>
const char* modeTitle() {
  static const std::string t = std::string("Режим карты: ") + schema::kMapModes[N].name;
  return t.c_str();
}

void zoomBy(App& a, double k) {
  map::MapView& m = a.map();
  RectF ar = a.mapArea();
  m.zoomAt(ar.cx(), ar.cy(), k, true);
  a.requestRedraw();
}

// ---- файл
CommandReg cNew({"file.new", "Новый мир…", "file-new", "Ctrl+N", [](App& a) { a.newWorldDialog(); }, nullptr, false, "Файл"});
CommandReg cOpen({"file.open", "Открыть папку мира…", "folder-open", "Ctrl+O", [](App& a) { a.openWorldDialog(); }, nullptr, false, "Файл"});
CommandReg cOpenBundle({"file.openBundle", "Открыть файл .regnum…", "archive", nullptr, [](App& a) { a.openBundleDialog(); }, nullptr, false, "Файл"});
CommandReg cSave({"file.save", "Сохранить", "save", "Ctrl+S", [](App& a) { a.save(); }, inEditor, false, "Файл"});
CommandReg cSaveAs({"file.saveAs", "Сохранить как…", "save-as", "Ctrl+Shift+S", [](App& a) { a.saveAs(); }, inEditor, false, "Файл"});
CommandReg cReveal({"file.reveal", "Показать папку мира", "folder", nullptr,
                    [](App& a) { platform::openPath(a.projectIsBundle() ? fs::parent(a.projectPath()) : a.projectPath()); },
                    [](App& a) { return inEditor(a) && !a.projectPath().empty(); }, false, "Файл"});
CommandReg cClose({"file.close", "Закрыть мир", "close", "Ctrl+W", [](App& a) { a.closeWorld(); }, inEditor, false, "Файл"});
CommandReg cQuit({"app.quit", "Выйти", "arrow-left", "Ctrl+Q", [](App& a) { a.requestQuit(); }, nullptr, false, "Файл"});

// ---- правка
CommandReg cUndo({"edit.undo", "Отменить", "undo", "Ctrl+Z", [](App& a) { a.undo(); }, [](App& a) { return editable(a) && a.store.canUndo(); }, false, "Правка"});
CommandReg cRedo({"edit.redo", "Повторить", "redo", "Ctrl+Y", [](App& a) { a.redo(); }, [](App& a) { return editable(a) && a.store.canRedo(); }, false, "Правка"});

// ---- ход
CommandReg cEndTurn({"turn.end", "Завершить ход", "next-turn", "Ctrl+Enter", [](App& a) { a.endTurn(); }, editable, false, "Ход"});
CommandReg cHistory({"turn.history", "История ходов", "history", nullptr, [](App& a) { a.openDialog("turn.history"); }, inEditor, false, "Ход"});
CommandReg cReport({"turn.report", "Отчёт о последнем ходе", "scroll", nullptr, [](App& a) { a.openDialog("turn.report"); },
                    [](App& a) { return inEditor(a) && a.lastTurnReport().has_value(); }, false, "Ход"});
CommandReg cBack({"turn.current", "Вернуться к текущему ходу", "arrow-right", nullptr, [](App& a) { a.backToCurrent(); },
                  [](App& a) { return a.readOnly(); }, false, "Ход"});

// ---- карта
CommandReg cBorders({"map.borders", "Правка границ", "unlock", "E", [](App& a) { a.setEditBorders(!a.ui.editBorders); },
                     [](App& a) { return onMap(a) && !a.readOnly(); }, false, "Карта"});
CommandReg cFit({"map.fit", "Показать всю карту", "zoom-fit", "Home", [](App& a) {
                   // Весь мир — в свободной части между панелями, а не во всём окне.
                   a.map().setSafeArea(a.mapArea());
                   a.map().fitAll(true);
                   a.requestRedraw();
                 },
                 onMap, false, "Карта"});
CommandReg cZoomIn({"map.zoomIn", "Приблизить", "zoom-in", "+", [](App& a) { zoomBy(a, 1.4); }, onMap, false, "Карта"});
CommandReg cZoomOut({"map.zoomOut", "Отдалить", "zoom-out", "-", [](App& a) { zoomBy(a, 1 / 1.4); }, onMap, false, "Карта"});
CommandReg cFocus({"map.focus", "Показать выделенное на карте", "target", "F", [](App& a) { a.focusSelection(); },
                   [](App& a) { return onMap(a) && bool(a.ui.sel); }, false, "Карта"});
CommandReg cMinimap({"map.minimap", "Мини-карта", "map", "M", [](App& a) {
                       a.ui.showMinimap = !a.ui.showMinimap;
                       a.impl().prefsDirty = true;
                     },
                     onMap, false, "Карта"});
CommandReg cLegend({"map.legend", "Легенда", "list", "L", [](App& a) {
                      a.ui.showLegend = !a.ui.showLegend;
                      a.impl().prefsDirty = true;
                    },
                    onMap, false, "Карта"});
CommandReg cMode1({"map.mode1", modeTitle<0>(), schema::kMapModes[0].icon, "1", setMode<0>, onMap, false, "Режимы карты"});
CommandReg cMode2({"map.mode2", modeTitle<1>(), schema::kMapModes[1].icon, "2", setMode<1>, onMap, false, "Режимы карты"});
CommandReg cMode3({"map.mode3", modeTitle<2>(), schema::kMapModes[2].icon, "3", setMode<2>, onMap, false, "Режимы карты"});
CommandReg cMode4({"map.mode4", modeTitle<3>(), schema::kMapModes[3].icon, "4", setMode<3>, onMap, false, "Режимы карты"});
CommandReg cMode5({"map.mode5", modeTitle<4>(), schema::kMapModes[4].icon, "5", setMode<4>, onMap, false, "Режимы карты"});
CommandReg cMode6({"map.mode6", modeTitle<5>(), schema::kMapModes[5].icon, "6", setMode<5>, onMap, false, "Режимы карты"});
CommandReg cMode7({"map.mode7", modeTitle<6>(), schema::kMapModes[6].icon, "7", setMode<6>, onMap, false, "Режимы карты"});
CommandReg cMode8({"map.mode8", modeTitle<7>(), schema::kMapModes[7].icon, "8", setMode<7>, onMap, false, "Режимы карты"});
CommandReg cMode9({"map.mode9", modeTitle<8>(), schema::kMapModes[8].icon, "9", setMode<8>, onMap, false, "Режимы карты"});

// ---- вид и справка
CommandReg cPalette({"app.palette", "Палитра команд и поиск", "command", "Ctrl+K", [](App& a) { a.showPalette(); }, nullptr, false, "Вид"});
CommandReg cSettings({"app.settings", "Настройки", "settings", "Ctrl+,", [](App& a) { a.showSettings(); }, nullptr, false, "Вид"});
CommandReg cTheme({"app.theme", "Сменить тему", "sun", nullptr, [](App& a) { a.setTheme(!a.ui.darkTheme); }, nullptr, false, "Вид"});
CommandReg cFull({"app.fullscreen", "Во весь экран", "fullscreen", "F11", [](App& a) { a.toggleFullscreen(); }, nullptr, true, "Вид"});
CommandReg cHelp({"app.help", "Сочетания клавиш", "keyboard", "F1", [](App& a) { a.showHelp(); }, nullptr, true, "Вид"});

}  // namespace
}  // namespace rg::app
