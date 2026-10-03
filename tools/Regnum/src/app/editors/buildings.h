// Regnum — дерево построек (ТЗ 1.h): общее для всех государств и уникальные постройки государства.
// Общие помощники отображения построек для редактора, вкладок и выбора строительства.
#pragma once
#include "app/app.h"

namespace rg::app {

// Открыть дерево построек: owner 0 — общее дерево, иначе уникальные постройки государства; building — выделить.
void openBuildingTree(App& a, Id owner, Id building = 0);
// Окно выбора строительства в провинции (ТЗ 1.f.ii); также по имени: app.openDialog("build.picker", province).
std::unique_ptr<Dialog> buildPickerDialog(Id province);

namespace bld {

Color catColor(BuildingCat c);
const char* catIcon(BuildingCat c);
const char* catName(BuildingCat c);
const char* iconOf(const Building& b);                 // значок постройки (неизвестный — по категории)
int levelTurns(const Building& b, int level);          // срок уровня (1…)
// Стоимость фишками «значок + количество»; payer != nullptr — нехватка красным.
void costChips(const std::map<Id, double>& cost, const Faction* payer, bool showEmpty = true);
// Хватает ли ресурса у плательщика.
bool affordable(const std::map<Id, double>& cost, const Faction* payer);
// Эффекты модификаторов уровня фишками (или «Без эффектов»).
void levelEffects(const World& w, const BuildingLevel& L, bool compact = false);
std::string costText(const World& w, const std::map<Id, double>& cost);   // «150 золота, 40 железа» (подсказки)
// Плитка значка постройки цвета категории (в потоке, сторона size).
void iconTile(const Building& b, float size, bool dim = false);

}  // namespace bld
}  // namespace rg::app
