// Regnum — войска и флот: общие помощники панелей, диалогов и инструментов (ТЗ 1.c, гарнизоны — 1.a.vi).
//
// Панели: вкладки фракции «Войска» и «Флот» и полноэкранные таблицы «Войска и флот» (military_faction.cpp),
// вкладка провинции «Гарнизон» (military_garrison.cpp), выдвижная панель «Войска и флот» (military_drawer.cpp),
// инспектор войска и флота (army_inspector.cpp). Диалоги: битва (dialogs/battle.cpp), встреча войск
// (dialogs/encounter.cpp), разделение (dialogs/split.cpp). Инструменты: новое войско и флот (tools_army.cpp),
// перетаскивание в инструменте выбора (tools.cpp).
#pragma once
#include "app/app.h"
#include "app/widgets.h"

namespace rg::app::mil {

// ---------------------------------------------------------------- данные
// Мир для рисования кадра: копия a.world(), живущая до следующего кадра. Указатели на сущности и флаги из неё
// не повиснут, если панель в том же кадре вызовет act() (правки со слиянием отмены освобождают прежний мир),
// а флаг дорисуется в endFrame.
const World& frameWorld(App& a);

// Строка таблицы войск или флота фракции в общем виде.
struct UnitRow {
  Id id = 0;
  std::string name;
  const char* icon = "army";      // значок типа
  const char* typeName = "";      // подпись типа
  int type = 0;                   // UnitType или ShipType
  i64 total = 0;
  double upkeep = 0;              // содержание одного
};
std::vector<UnitRow> unitRows(const World& w, Id faction, bool fleet);
std::optional<UnitRow> unitRow(const World& w, Id faction, Id row, bool fleet);
// Расчёт строки (в поле, резерв, содержание с модификатором). nullptr — нет такой строки.
const rules::RowCalc* rowCalc(const rules::Calc& c, Id faction, Id row, bool fleet);
i64 reserveOf(const World& w, Id faction, Id row, bool fleet);

i64 groupCount(const ArmyGroup& g);
i64 unitCount(const Army& a);
i64 rowCount(const ArmyGroup& g, Id row);
std::vector<Id> factionsIn(const Army& a);
std::string objectCaption(const Army& a);          // «Войско», «Союзный флот»…
const char* objectIcon(const Army& a);             // army / fleet
std::string objectName(const Army& a);             // название или «Без названия»
Id provinceUnder(const World& w, Vec2 p);          // провинция под точкой карты (0 — нет)
Id heroLocation(const World& w, Id character, Id skip = 0);   // объект, который сопровождает персонаж
std::string fmtCount(i64 n);                       // численность: «12 400»
std::string fmtMoney(double v);                    // содержание: «1 840», «12,5»
// Цвета фигурки объекта: лидер и второй союзник.
Color leaderColor(const World& w, const Army& a);
Color allyColor(const World& w, const Army& a);

// ---------------------------------------------------------------- виджеты
// Фигурка как на карте (жетон с силуэтом) в прямоугольнике r (точки интерфейса).
void figureIn(RectF r, ArmyKind kind, Color c1, bool allied = false, Color c2 = Color(0, 0, 0, 0), bool selected = false);
void figure(const World& w, const Army& a, float size, bool selected = false);   // в потоке: слот size × size
// Флаг фракции в потоке (с подсказкой-названием).
void factionFlag(const World& w, Id faction, float width, float height = 0);
// Флаг и название фракции (щелчок — инспектор фракции).
void factionLabel(const World& w, Id faction, float flagW = 22);
// Плитка значка типа (отряд или судно).
void typeTile(RectF r, const char* icon, Color tint);
// Строка списка объекта: фигурка, название, подзаголовок, численность. true — щелчок.
bool objectItem(const World& w, const Army& a, bool selected, std::string_view subtitle);
// Две строки текста в ячейке таблицы (основная и приглушённая подпись).
void cellLines(RectF r, std::string_view top, std::string_view bottom, ui::Align align, ui::Ink topInk = ui::Ink::Normal);
// Название в ячейке: в одну строку с подписью или, если не помещается, в две строки по пробелу.
void nameCell(RectF r, std::string_view name, std::string_view caption = {});
// Ячейка «отряд»: плитка типа (подсказка — тип) и название.
void unitCell(RectF r, const UnitRow& row, bool accent = false, std::string_view caption = {});

// ---------------------------------------------------------------- действия и диалоги
// Битва (ТЗ 1.c.iv): attacker пришёл из origin на позицию defender. done(applied) — после выбора.
void openBattle(App& a, Id attacker, Id defender, Vec2 origin, std::function<void(App&, bool applied)> done = {});
// Встреча при перетаскивании: объединение, союз, объявление войны. done(accepted) — после выбора.
void openEncounter(App& a, Id moving, Id target, const rules::Encounter& e, Vec2 origin, std::function<void(App&, bool accepted)> done);
// Текст диалога союза: какие отряды сложатся с плитками своей фракции в цели, какие займут новые плитки.
std::string allianceText(const World& w, const Army& moving, const Army& target);
void openSplit(App& a, Id army);
void askDisband(App& a, Id army);                  // подтверждение, затем расформирование
void dissolveAllied(App& a, Id army);              // распустить союзное войско (флот)
void askRemoveRow(App& a, Id faction, Id row, bool fleet);   // удалить строку таблицы с подтверждением

// Инструменты карты «Новое войско» и «Новый флот» (повторная регистрация в тестах).
std::unique_ptr<MapTool> makePlaceTool(ArmyKind kind);
// Фракция для новых объектов: последняя выбранная в инструменте (0 — ещё не выбирали).
Id& lastPlaceFaction();
// Выбрать инструмент постановки объекта для фракции.
void startPlacing(App& a, ArmyKind kind, Id faction);

}  // namespace rg::app::mil
