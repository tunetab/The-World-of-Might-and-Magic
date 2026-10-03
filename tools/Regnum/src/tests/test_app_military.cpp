// Сценарии войск и флота (ТЗ 1.c, гарнизоны 1.a.vi): таблицы фракции, инспектор войска, гарнизон, выдвижная
// панель, инструменты постановки, отряды из резерва, расформирование, разделение, роспуск союза.
// Перетаскивание, встречи и битвы — test_app_military_drag.cpp.
#include "tests/test_app_military_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

i64 reserveNow(Harness& h, Id faction, Id row, bool fleet = false) { return app::mil::reserveOf(h->world(), faction, row, fleet); }

}  // namespace

// Инструменты «Новое войско» и «Новый флот»: панель с фракцией, призрак, вода и суша, выделение нового объекта.
TEST(app_military_place_tools) {
  Harness h("military_place");
  RealArmyTools tools;
  h.demo();
  h.waitMap();
  Id hel = factionByName(h->world(), "Хельдвиг");
  CHECK(hel != 0);
  h->select(app::SelType::Faction, hel);
  h.settle();
  h->clearSelection();
  h.settle();
  h->select(app::SelType::Faction, hel);
  h.settle();
  // A — новое войско; фракция по умолчанию — выделенная.
  h.key(Key::A);
  CHECK(h->ui.tool == app::ToolId::NewArmy);
  h.settle();
  CHECK(h->uiRect("tool.options") != nullptr);
  CHECK_EQ(app::mil::lastPlaceFaction(), hel);
  // Вид объекта на панели инструмента: флот и обратно, фракция сохраняется.
  const RectF* kind = h->uiRect("place.kind");
  CHECK(kind != nullptr);
  h.click(kind->right() - kind->w * 0.25f, kind->cy());
  h.settle();
  CHECK(h->ui.tool == app::ToolId::NewFleet);
  CHECK_EQ(app::mil::lastPlaceFaction(), hel);
  kind = h->uiRect("place.kind");
  h.click(kind->x + kind->w * 0.25f, kind->cy());
  h.settle();
  CHECK(h->ui.tool == app::ToolId::NewArmy);
  CHECK_EQ(app::mil::lastPlaceFaction(), hel);
  h->clearSelection();
  h.settle();
  auto land = freeSpot(h, ArmyKind::Army);
  CHECK(land.has_value());
  auto sea = freeSpot(h, ArmyKind::Fleet);
  CHECK(sea.has_value());
  // Щелчок по морю инструментом войска — отказ.
  size_t n0 = h->world().armies.size();
  h.move(sea->second.x, sea->second.y);
  h.dropToasts();
  h.move(sea->second.x + 1, sea->second.y);
  CHECK(h.shot("military_tool_invalid"));
  h.click(sea->second.x, sea->second.y);
  CHECK_EQ(h->world().armies.size(), n0);
  CHECK(!h->toasts().empty());
  h.dropToasts();
  // Призрак на суше и панель инструмента.
  h.move(land->second.x, land->second.y);
  h.move(land->second.x + 1, land->second.y + 1);
  CHECK(h.shot("military_tool_army"));
  h.click(land->second.x + 1, land->second.y + 1);
  CHECK_EQ(h->world().armies.size(), n0 + 1);
  CHECK(h->ui.sel.type == app::SelType::Army);
  Id army = h->ui.sel.id;
  const Army* a = h->world().army(army);
  CHECK(a != nullptr);
  CHECK(!a->isFleet());
  CHECK_EQ(a->leader(), hel);
  CHECK(h->ui.tool == app::ToolId::Select);   // после постановки — к выбору
  // Отмена создания.
  h.key(Key::Z, ctrl());
  CHECK(h->world().army(army) == nullptr);
  h.key(Key::Y, ctrl());
  CHECK(h->world().army(army) != nullptr);

  // Shift+A — новый флот: только море.
  h.key(Key::A, platform::ModShift);
  CHECK(h->ui.tool == app::ToolId::NewFleet);
  h.settle();
  sea = freeSpot(h, ArmyKind::Fleet);
  CHECK(sea.has_value());
  land = freeSpot(h, ArmyKind::Army);
  CHECK(land.has_value());
  size_t n1 = h->world().armies.size();
  h.click(land->second.x, land->second.y);
  CHECK_EQ(h->world().armies.size(), n1);
  h.dropToasts();
  h.click(sea->second.x, sea->second.y);
  CHECK_EQ(h->world().armies.size(), n1 + 1);
  CHECK(h->ui.sel.type == app::SelType::Army);
  const Army* f = h->world().army(h->ui.sel.id);
  CHECK(f != nullptr && f->isFleet() && f->leader() == hel);
  // Esc закрывает инструмент.
  h.key(Key::A);
  CHECK(h->ui.tool == app::ToolId::NewArmy);
  h.key(Key::Escape);
  h.settle();
  CHECK(h->ui.tool == app::ToolId::Select);
}

// Отряды из резерва в инспекторе войска: не больше резерва, только строки своей фракции, отмена.
TEST(app_military_assign_units) {
  Harness h("military_units");
  RealArmyTools tools;
  h.demo();
  const World& w0 = h->world();
  Id hel = factionByName(w0, "Хельдвиг");
  const Faction* hf = w0.faction(hel);
  CHECK(hf && hf->army.size() >= 2);
  Id rowInf = hf->army[0].id, rowRng = hf->army[2].id;
  // Новое войско фракции рядом с её войском.
  Id src = armyOf(w0, hel, ArmyKind::Army);
  CHECK(src != 0);
  auto spot = rules::findFreeSpot(w0, ArmyKind::Army, w0.army(src)->pos + Vec2(200, 0));
  CHECK(spot.has_value());
  Id id = 0;
  CHECK(h->act("Новое войско", [&](Tx& tx) { id = rules::createArmy(tx, ArmyKind::Army, hel, *spot); }));
  h->select(app::SelType::Army, id);
  h.settle();
  i64 res0 = reserveNow(h, hel, rowInf);
  CHECK(res0 > 0);
  // «Отряд из резерва»: строка и численность (по умолчанию — весь резерв, больше нельзя).
  CHECK(h.clickUi("army.addunit." + std::to_string(hel)));
  CHECK(h.clickUi("army.addunit.count"));
  h.retype("999999");
  h.key(Key::Enter);
  h.settle();
  CHECK(h.shot("military_add_units_popup"));
  CHECK(h.clickUi("army.addunit.ok"));
  h.settle();
  const Army* a = h->world().army(id);
  CHECK(a && a->groups.size() == 1 && !a->groups[0].units.empty());
  Id row = a->groups[0].units[0].row;
  i64 got = a->groups[0].units[0].count;
  CHECK_EQ(got, reserveNow(h, hel, row) + got);   // резерв строки исчерпан
  CHECK_EQ(reserveNow(h, hel, row), 0);
  // Численность в таблице: больше резерва не задать.
  std::string field = "army.unit." + std::to_string(hel) + "." + std::to_string(row);
  CHECK(h.clickUi(field));
  h.retype(std::to_string(got + 5000));
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(h->world().army(id)->groups[0].units[0].count, got);
  CHECK(h.clickUi(field));
  h.retype("10");
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(h->world().army(id)->groups[0].units[0].count, 10);
  CHECK_EQ(reserveNow(h, hel, row), got - 10);
  // Правило: только строки своей фракции (чужая строка — отказ правил).
  Id other = factionByName(h->world(), "Альмарин");
  Id foreignRow = h->world().faction(other)->army[0].id;
  CHECK(!h->act("Чужие отряды", [&](Tx& tx) { rules::setUnits(tx, id, hel, foreignRow, 1); }));
  h.dropToasts();
  // Отмена возвращает прежнюю численность.
  h.key(Key::Z, ctrl());
  CHECK_EQ(h->world().army(id)->groups[0].units[0].count, got);
  // Герой: только свои, первый — главный полководец.
  Id hero = 0;
  h->world().characters.each([&](const Character& c) {
    if (!hero && c.faction == hel && !app::mil::heroLocation(h->world(), c.id)) hero = c.id;
  });
  CHECK(hero != 0);
  CHECK(h.clickUi("army.addhero." + std::to_string(hel)));
  h.type(h->world().characterName(hero));
  h.key(Key::Enter);
  h.settle();
  const Army* a2 = h->world().army(id);
  CHECK(a2 && !a2->groups[0].heroes.empty());
  CHECK_EQ(a2->commander, a2->groups[0].heroes[0]);
  (void)rowRng;
}

// Таблицы войск и флота фракции: добавить строку, изменить, не меньше «в поле», удалить с подтверждением.
TEST(app_military_faction_tables) {
  Harness h("military_tables");
  RealArmyTools tools;
  h.demo();
  h.waitMap();
  const World& w0 = h->world();
  Id hel = factionByName(w0, "Хельдвиг");
  CHECK(hel != 0);
  h->ui.tabOf[app::SelType::Faction] = "faction.army";
  h->select(app::SelType::Faction, hel);
  h.settle();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("military_faction_army"));
  if (const RectF* r = h->uiRect("inspector")) CHECK(cropShot("military_zoom_faction", *r));
  // Добавить отряд: «+» в заголовке раздела → тип.
  size_t rows0 = h->world().faction(hel)->army.size();
  const RectF* add = h->uiRect("mil.army.add");
  CHECK(add != nullptr);
  h.click(add->right() - 13, add->cy());
  h.settle();
  CHECK(h.shot("military_add_row_menu"));
  CHECK(h.clickUi("mil.addrow." + std::to_string(int(UnitType::Monsters))));
  h.settle();
  const Faction* f = h->world().faction(hel);
  CHECK_EQ(f->army.size(), rows0 + 1);
  CHECK(f->army.back().type == UnitType::Monsters);
  Id nrow = f->army.back().id;
  // Правка выбранной строки: численность и содержание.
  CHECK(h->uiRect("mil.detail.total") != nullptr);
  CHECK(h.clickUi("mil.detail.total"));
  h.retype("250");
  h.key(Key::Enter);
  h.settle();
  CHECK(h.clickUi("mil.detail.upkeep"));
  h.retype("7,5");
  h.key(Key::Enter);
  h.settle();
  f = h->world().faction(hel);
  CHECK_EQ(f->armyRow(nrow)->total, 250);
  CHECK_NEAR(f->armyRow(nrow)->upkeep, 7.5, 1e-9);
  CHECK(h.clickUi("mil.detail.name"));
  h.retype("Ледяные тролли");
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(h->world().faction(hel)->armyRow(nrow)->name, std::string("Ледяные тролли"));
  h.dropToasts();
  h.settle();
  CHECK(h.shot("military_faction_row_edit"));
  // Общая численность не меньше «в поле»: строка пехоты (есть войска и гарнизон).
  Id inf = h->world().faction(hel)->army[0].id;
  auto c = rules::calc(h->world());
  i64 field = app::mil::rowCalc(*c, hel, inf, false)->field;
  CHECK(field > 0);
  CHECK(h.clickUi("mil.row.0"));
  CHECK(h.clickUi("mil.detail.total"));
  h.retype("1");
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(h->world().faction(hel)->armyRow(inf)->total, field);
  // Удаление строки — с подтверждением (опасное действие).
  CHECK(h.clickUi("mil.row." + std::to_string(rows0)));
  CHECK(h.clickUi("mil.detail.delete"));
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK(h->world().faction(hel)->armyRow(nrow) == nullptr);
  h.key(Key::Z, ctrl());
  CHECK(h->world().faction(hel)->armyRow(nrow) != nullptr);
  // Флот.
  h->ui.tabOf[app::SelType::Faction] = "faction.fleet";
  h.settle();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("military_faction_fleet"));
  // Таблицы во весь экран.
  h->ui.tabOf[app::SelType::Faction] = "faction.army";
  h.settle();
  const RectF* insp = h->uiRect("inspector");
  CHECK(insp != nullptr);
  h.wheel(insp->cx(), insp->cy() + 120, -30);   // кнопка — внизу вкладки
  h.settle();
  CHECK(h.clickUi("mil.fullscreen"));
  h.settle();
  CHECK_EQ(h->ui.editor, std::string("military"));
  h.dropToasts();
  h.settle();
  CHECK(h.shot("military_editor"));
  h.key(Key::Escape);
  h.settle();
  CHECK(h->ui.editor.empty());
}

// Гарнизон провинции: из резерва владельца, не больше резерва, пустые состояния.
TEST(app_military_garrison) {
  Harness h("military_garrison");
  RealArmyTools tools;
  h.demo();
  const World& w0 = h->world();
  Id pid = 0;
  w0.provinces.each([&](const Province& p) {
    if (!pid && !p.sea && p.owner && !p.garrison.empty()) pid = p.id;
  });
  CHECK(pid != 0);
  Id owner = w0.province(pid)->owner;
  Id row = w0.province(pid)->garrison[0].row;
  i64 g0 = w0.province(pid)->garrison[0].count;
  h->ui.tabOf[app::SelType::Province] = "province.garrison";
  h->select(app::SelType::Province, pid);
  h.settle();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("military_garrison"));
  i64 res = reserveNow(h, owner, row);
  std::string field = "garrison.row." + std::to_string(row);
  CHECK(h.clickUi(field));
  h.retype(std::to_string(g0 + res + 1000));
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(h->world().province(pid)->garrison[0].count, g0 + res);   // ограничено резервом
  CHECK_EQ(reserveNow(h, owner, row), 0);
  h.key(Key::Z, ctrl());
  CHECK_EQ(h->world().province(pid)->garrison[0].count, g0);
  // Назначить другую строку из резерва.
  CHECK(h.clickUi("garrison.add"));
  CHECK(h.clickUi("garrison.add.ok"));
  h.settle();
  CHECK(h->world().province(pid)->garrison.size() >= 2);
  // Морская провинция — вкладки нет (ТЗ 1.a.iii: никаких сведений).
  const app::TabDef* tab = nullptr;
  for (const app::TabDef& t : app::tabs())
    if (std::string_view(t.id) == "province.garrison") tab = &t;
  CHECK(tab != nullptr && tab->visible != nullptr);
  Id sea = 0, unowned = 0;
  h->world().provinces.each([&](const Province& p) {
    if (!sea && p.sea) sea = p.id;
    if (!unowned && !p.sea && !p.owner) unowned = p.id;
  });
  CHECK(sea != 0);
  CHECK(!tab->visible(*h.app, sea));
  CHECK(tab->visible(*h.app, pid));
  h->select(app::SelType::Province, sea);
  h.settle();
  CHECK(h->uiRect("garrison.add") == nullptr);
  // Провинция без владельца — пустое состояние без назначения.
  if (unowned) {
    CHECK(tab->visible(*h.app, unowned));
    h->ui.tabOf[app::SelType::Province] = "province.garrison";
    h->select(app::SelType::Province, unowned);
    h.settle();
    CHECK(h->uiRect("garrison.add") == nullptr);
    CHECK(!h->act("Гарнизон без владельца", [&](Tx& tx) { rules::setGarrison(tx, unowned, row, 1); }));
    h.dropToasts();
  }
}

// Расформирование (подтверждение, отряды в резерв, Delete), разделение, роспуск союза.
TEST(app_military_disband_split_dissolve) {
  Harness h("military_disband");
  RealArmyTools tools;
  h.demo();
  const World& w0 = h->world();
  Id hel = factionByName(w0, "Хельдвиг");
  Id id = armyOf(w0, hel, ArmyKind::Army);
  CHECK(id != 0);
  // Разделение: поровну.
  h->select(app::SelType::Army, id, true);
  h.settle();
  i64 total = app::mil::unitCount(*h->world().army(id));
  size_t n0 = h->world().armies.size();
  CHECK(h.clickUi("army.split"));
  h.settle();
  CHECK(h->hasDialog("army.split"));
  CHECK(h.clickUi("split.half"));
  h.settle();
  CHECK(h.shot("military_split"));
  CHECK(h.clickUi("split.ok"));
  h.settle();
  CHECK_EQ(h->world().armies.size(), n0 + 1);
  Id part = h->ui.sel.id;
  CHECK(part != id);
  i64 moved = app::mil::unitCount(*h->world().army(part));
  CHECK(moved > 0);
  CHECK_EQ(app::mil::unitCount(*h->world().army(id)) + moved, total);
  CHECK(dist(h->world().army(part)->pos, h->world().army(id)->pos) < 400);
  // Расформирование выделенной части: Delete → подтверждение → отряды в резерв.
  const Army* pa = h->world().army(part);
  Id row = pa->groups[0].units[0].row;
  i64 cnt = pa->groups[0].units[0].count;
  i64 res0 = reserveNow(h, hel, row);
  h.key(Key::Delete);
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK(h->world().army(part) == nullptr);
  CHECK_EQ(reserveNow(h, hel, row), res0 + cnt);
  h.key(Key::Z, ctrl());
  CHECK(h->world().army(part) != nullptr);
  // Кнопка «Расформировать» и отказ.
  h->select(app::SelType::Army, part);
  h.settle();
  CHECK(h.clickUi("army.disband"));
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.cancel"));
  h.settle();
  CHECK(h->world().army(part) != nullptr);
  // Роспуск союзного войска: группы — отдельные объекты рядом.
  Id allied = 0;
  h->world().armies.each([&](const Army& a) {
    if (!allied && a.allied() && !a.isFleet()) allied = a.id;
  });
  CHECK(allied != 0);
  size_t groups = h->world().army(allied)->groups.size();
  size_t n1 = h->world().armies.size();
  h->select(app::SelType::Army, allied, true);
  h.settle();
  CHECK(h.clickUi("army.dissolve"));
  h.settle();
  CHECK_EQ(h->world().armies.size(), n1 + groups - 1);
  CHECK(!h->world().army(allied)->allied());
  h.key(Key::Z, ctrl());
  CHECK(h->world().army(allied)->allied());
}

// Инспектор союзного войска (плитки фракций), светлая тема, выдвижная панель «Войска и флот».
TEST(app_military_inspector_and_drawer) {
  {
    Harness h("military_inspector");
    RealArmyTools tools;
    h.demo();
    Id allied = 0;
    h->world().armies.each([&](const Army& a) {
      if (!allied && a.allied() && !a.isFleet()) allied = a.id;
    });
    CHECK(allied != 0);
    h->select(app::SelType::Army, allied, true);
    h.settle();
    h.waitMap();
    h.dropToasts();
    h.settle();
    CHECK(h.shot("military_army_allied"));
    if (const RectF* r = h->uiRect("inspector")) CHECK(cropShot("military_zoom_army", *r));
    // Переименование: F2 → запрос названия.
    h.key(Key::F2);
    CHECK(h->hasDialog("prompt"));
    h.retype("Северная гвардия");
    h.key(Key::Enter);
    h.settle();
    CHECK_EQ(h->world().army(allied)->name, std::string("Северная гвардия"));
    // События объекта.
    h->ui.tabOf[app::SelType::Army] = "army.log";
    h.settle();
    CHECK(h.shot("military_army_log"));
    // Выдвижная панель: поиск и переход к объекту.
    h->ui.tabOf[app::SelType::Army] = "army.units";
    h.key(Key::D5, ctrl());
    h.settle();
    CHECK_EQ(h->ui.drawer, std::string("military"));
    Id fleet = 0;
    h->world().armies.each([&](const Army& a) {
      if (!fleet && a.isFleet() && !a.allied()) fleet = a.id;
    });
    CHECK(fleet != 0);
    CHECK(h.clickUi("mil.drawer.search"));
    h.type(h->world().army(fleet)->name);
    h.settle();
    h.dropToasts();
    h.settle();
    CHECK(h.shot("military_drawer"));
    CHECK(h.clickUi("mil.drawer.item." + std::to_string(fleet)));
    h.settle();
    CHECK(h->ui.sel == (app::Selection{app::SelType::Army, fleet}));
  }
  {
    Harness h("military_light", 1440, 900, 1, false);
    RealArmyTools tools;
    h.demo();
    Id allied = 0;
    h->world().armies.each([&](const Army& a) {
      if (!allied && a.allied() && !a.isFleet()) allied = a.id;
    });
    h->select(app::SelType::Army, allied, true);
    h.settle();
    h.waitMap();
    h.dropToasts();
    h.settle();
    CHECK(h.shot("military_army_light"));
  }
}

// Гильдия (ТЗ 1.d.iii): свои таблицы войск и флота, пустые состояния, своё войско на карте.
TEST(app_military_guild_forces) {
  Harness h("military_guild");
  RealArmyTools tools;
  h.demo();
  h.waitMap();
  Id guild = 0;
  h->world().factions.each([&](const Faction& f) {
    if (!guild && f.isGuild() && f.army.empty()) guild = f.id;
  });
  CHECK(guild != 0);
  h->ui.tabOf[app::SelType::Faction] = "faction.army";
  h->select(app::SelType::Faction, guild);
  h.settle();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("military_guild_empty"));
  // «Добавить отряд» в пустом состоянии → тип.
  const RectF* e = h->uiRect("mil.army.empty");
  CHECK(e != nullptr);
  h.click(e->cx(), e->bottom() - 31);
  h.settle();
  CHECK(h.clickUi("mil.addrow." + std::to_string(int(UnitType::Ranged))));
  h.settle();
  const Faction* g = h->world().faction(guild);
  CHECK_EQ(g->army.size(), size_t(1));
  CHECK(g->army[0].type == UnitType::Ranged);
  CHECK(h.clickUi("mil.detail.total"));
  h.retype("400");
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(h->world().faction(guild)->army[0].total, 400);
  // «Поставить войско» → инструмент с фракцией гильдии → щелчок по суше.
  const RectF* insp = h->uiRect("inspector");
  h.wheel(insp->cx(), insp->cy() + 120, -30);
  h.settle();
  const RectF* pe = h->uiRect("mil.army.placeEmpty");
  CHECK(pe != nullptr);
  h.click(pe->cx(), pe->bottom() - 31);
  h.settle();
  CHECK(h->ui.tool == app::ToolId::NewArmy);
  CHECK_EQ(app::mil::lastPlaceFaction(), guild);
  h->clearSelection();
  h.settle();
  auto land = freeSpot(h, ArmyKind::Army);
  CHECK(land.has_value());
  h.click(land->second.x, land->second.y);
  CHECK(h->ui.sel.type == app::SelType::Army);
  const Army* a = h->world().army(h->ui.sel.id);
  CHECK(a && a->leader() == guild);
  // Отряды гильдии из её резерва.
  CHECK(h.clickUi("army.addunit." + std::to_string(guild)));
  CHECK(h.clickUi("army.addunit.ok"));
  h.settle();
  CHECK_EQ(app::mil::unitCount(*h->world().army(h->ui.sel.id)), 400);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("military_guild_army"));
}
