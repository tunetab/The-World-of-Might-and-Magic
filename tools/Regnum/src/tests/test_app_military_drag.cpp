// Сценарии перетаскивания войск и флотов (ТЗ 1.c.iv): перемещение и отмена, вода и флот — возврат с причиной,
// объединение (да/нет), союзное войско, объявление войны и битва с потерями (проигравший смещается),
// отступление (нападавший на исходной позиции).
#include "tests/test_app_military_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

// Перетаскивание по шагам (snapshot — снимок, пока кнопка ещё зажата).
void dragTo(Harness& h, gfx::Pt from, gfx::Pt to, const std::string& snapshot = {}) {
  hl::mouseMove(from.x, from.y);
  h.step();
  hl::mouseDown(from.x, from.y, platform::MouseLeft);
  h.step();
  for (int i = 1; i <= 10; i++) {
    hl::mouseMove(from.x + (to.x - from.x) * float(i) / 10, from.y + (to.y - from.y) * float(i) / 10);
    h.step();
  }
  if (!snapshot.empty()) {
    h.dropToasts();
    h.settle();
    CHECK(h.shot(snapshot));
  }
  hl::mouseUp(to.x, to.y, platform::MouseLeft);
  h.step();
  hl::advance(0.6);
}

// Новое войско фракции рядом с точкой near (с отрядами первой строки).
Id spawnArmy(Harness& h, Id faction, Vec2 near, i64 units) {
  const World& w = h->world();
  auto spot = rules::findFreeSpot(w, ArmyKind::Army, near);
  if (!spot) return 0;
  Id id = 0;
  Id row = w.faction(faction)->army[0].id;
  h->act("Войско для теста", [&](Tx& tx) {
    id = rules::createArmy(tx, ArmyKind::Army, faction, *spot);
    rules::setUnits(tx, id, faction, row, units);
  });
  return id;
}

// Подготовка: выделить объект (инспектор открыт) и показать точку крупно.
void focusOn(Harness& h, Id army, Vec2 at, double zoom = 0.6) {
  h->select(app::SelType::Army, army);
  h.settle();
  showAt(h, at, zoom);
  h.dropToasts();
}

}  // namespace

// Перемещение, отмена, вода (возврат с причиной), войско не взаимодействует с флотом.
TEST(app_military_drag_move) {
  Harness h("military_drag_move");
  RealArmyTools tools;
  h.demo();
  Id hel = factionByName(h->world(), "Хельдвиг");
  Id x = armyOf(h->world(), hel, ArmyKind::Army);
  CHECK(x != 0);
  Vec2 orig = h->world().army(x)->pos;
  focusOn(h, x, orig);
  // Свободная суша рядом, без встречи.
  std::optional<Vec2> to;
  for (Vec2 d : {Vec2{180, 0}, Vec2{-180, 0}, Vec2{0, 180}, Vec2{0, -180}, Vec2{140, 140}, Vec2{-140, 140}}) {
    auto s = rules::findFreeSpot(h->world(), ArmyKind::Army, orig + d, x);
    if (!s || dist(*s, orig) < 90) continue;
    gfx::Pt sp = screenOf(h, *s);
    if (!freeOnScreen(h, sp.x, sp.y)) continue;
    if (rules::encounter(h->world(), x, *s).type != rules::EncounterType::None) continue;
    to = *s;
    break;
  }
  CHECK(to.has_value());
  dragTo(h, screenOf(h, orig), screenOf(h, *to), "military_drag_ghost");
  h.settle();
  const Army* a = h->world().army(x);
  CHECK(dist(a->pos, *to) < 4);
  CHECK(h->ui.sel == (app::Selection{app::SelType::Army, x}));
  h.key(Key::Z, ctrl());
  CHECK(h->world().army(x)->pos == orig);
  // На воду — нельзя: возврат и причина.
  h.dropToasts();
  std::optional<Vec2> water;
  for (float r = 120; r < 900 && !water; r += 40)
    for (int k = 0; k < 16 && !water; k++) {
      Vec2 p = orig + Vec2{r * std::cos(k * kPi / 8), r * std::sin(k * kPi / 8)};
      gfx::Pt sp = screenOf(h, p);
      if (!freeOnScreen(h, sp.x, sp.y)) continue;
      if (geo::faces(h->world())->terrainAt(p) != Terrain::Sea) continue;
      if (rules::armyAt(h->world(), p)) continue;
      if (rules::validPosition(h->world(), ArmyKind::Fleet, p)) water = p;
    }
  CHECK(water.has_value());
  if (water) {
    dragTo(h, screenOf(h, orig), screenOf(h, *water), "military_drag_blocked");
    h.settle();
    CHECK(h->world().army(x)->pos == orig);
    bool reason = false;
    for (auto& t : h->toasts()) reason = reason || t.text.find("суше") != std::string::npos;
    CHECK(reason);
  }
  // Войско на флот — нельзя: объекты не взаимодействуют и не наслаиваются (ТЗ 1.c.iv).
  h.dropToasts();
  Id fl = armyOf(h->world(), hel, ArmyKind::Fleet);
  CHECK(fl != 0);
  Vec2 fpos = h->world().army(fl)->pos;
  focusOn(h, x, (orig + fpos) * 0.5, std::min(0.5, 0.6 * h->mapArea().h / std::max(1.0, dist(orig, fpos) + 160)));
  gfx::Pt fs = screenOf(h, fpos), xs = screenOf(h, orig);
  CHECK(freeOnScreen(h, fs.x, fs.y) && freeOnScreen(h, xs.x, xs.y));
  dragTo(h, xs, fs);
  h.settle();
  CHECK(h->world().army(x)->pos == orig);
  CHECK(h->world().army(fl) != nullptr);
  CHECK(h->world().army(fl)->pos == fpos);
  bool reason = false;
  for (auto& t : h->toasts()) reason = reason || t.text.find("флот") != std::string::npos;
  CHECK(reason);
}

// Флот: перемещение по морю, суша — возврат с причиной, битва флотов (потери кораблей из таблицы флота).
TEST(app_military_drag_fleet) {
  Harness h("military_drag_fleet");
  RealArmyTools tools;
  h.demo();
  Id hel = factionByName(h->world(), "Хельдвиг");
  Id vk = factionByName(h->world(), "Валь-Кетра");
  CHECK(h->world().relation(hel, vk).s == RelStatus::War);
  Id f1 = armyOf(h->world(), hel, ArmyKind::Fleet);
  Id f2 = armyOf(h->world(), vk, ArmyKind::Fleet);
  CHECK(f1 && f2);
  Vec2 p1 = h->world().army(f1)->pos, p2 = h->world().army(f2)->pos;
  focusOn(h, f1, (p1 + p2) * 0.5, std::min(0.6, 0.45 * h->mapArea().w / std::max(1.0, dist(p1, p2) + 200)));
  CHECK(h.shot("military_fleet_inspector"));
  // Свободное море рядом.
  std::optional<Vec2> to;
  for (float r = 120; r < 500 && !to; r += 30)
    for (int k = 0; k < 12 && !to; k++) {
      Vec2 p = p1 + Vec2{r * std::cos(k * kPi / 6), r * std::sin(k * kPi / 6)};
      gfx::Pt sp = screenOf(h, p);
      if (!freeOnScreen(h, sp.x, sp.y)) continue;
      if (rules::encounter(h->world(), f1, p).type != rules::EncounterType::None) continue;
      bool margin = true;
      for (Vec2 d : {Vec2{25, 0}, Vec2{-25, 0}, Vec2{0, 25}, Vec2{0, -25}}) margin = margin && rules::validPosition(h->world(), ArmyKind::Fleet, p + d, f1);
      if (margin) to = p;
    }
  CHECK(to.has_value());
  if (to) {
    dragTo(h, screenOf(h, p1), screenOf(h, *to), "military_drag_fleet_ghost");
    h.settle();
    CHECK(dist(h->world().army(f1)->pos, *to) < 4);
    h.key(Key::Z, ctrl());
    CHECK(h->world().army(f1)->pos == p1);
  }
  // На сушу — нельзя.
  h.dropToasts();
  std::optional<Vec2> land;
  for (float r = 120; r < 1200 && !land; r += 30)
    for (int k = 0; k < 16 && !land; k++) {
      Vec2 p = p1 + Vec2{r * std::cos(k * kPi / 8), r * std::sin(k * kPi / 8)};
      gfx::Pt sp = screenOf(h, p);
      if (!freeOnScreen(h, sp.x, sp.y) || rules::armyAt(h->world(), p)) continue;
      if (rules::validPosition(h->world(), ArmyKind::Army, p)) land = p;
    }
  CHECK(land.has_value());
  if (land) {
    dragTo(h, screenOf(h, p1), screenOf(h, *land));
    h.settle();
    CHECK(h->world().army(f1)->pos == p1);
    bool reason = false;
    for (auto& t : h->toasts()) reason = reason || t.text.find("море") != std::string::npos;
    CHECK(reason);
  }
  // Битва флотов: Хельдвиг нападает, потери — линкоры защитника целиком, фрегаты — 2.
  h.dropToasts();
  const Army& d = *h->world().army(f2);
  CHECK(d.groups[0].units.size() >= 2);
  Id lineRow = d.groups[0].units[0].row, frigRow = d.groups[0].units[1].row;
  i64 lines = d.groups[0].units[0].count, frigs = d.groups[0].units[1].count;
  i64 vkLineTotal = h->world().faction(vk)->fleetRow(lineRow)->total;
  i64 vkFrigTotal = h->world().faction(vk)->fleetRow(frigRow)->total;
  dragTo(h, screenOf(h, p1), screenOf(h, p2), "military_drag_fleet_battle_ghost");
  h.settle();
  CHECK(h->hasDialog("battle"));
  CHECK(h.clickUi("battle.loss." + std::to_string(f2) + "." + std::to_string(lineRow)));
  h.retype(std::to_string(lines));
  h.key(Key::Enter);
  CHECK(h.clickUi("battle.loss." + std::to_string(f2) + "." + std::to_string(frigRow)));
  h.retype("2");
  h.key(Key::Enter);
  h.settle();
  CHECK(h.shot("military_battle_fleet"));
  CHECK(h.clickUi("battle.apply"));
  h.settle();
  CHECK(!h->hasDialog());
  const Army* w1 = h->world().army(f1);
  const Army* l2 = h->world().army(f2);
  CHECK(w1 && l2);
  CHECK(dist(w1->pos, p2) < 1e-6);                                 // победитель — на месте боя
  CHECK(dist(l2->pos, p2) >= 2 * schema::kObjectRadius - 1e-6);    // проигравший смещён
  CHECK_EQ(app::mil::unitCount(*l2), frigs - 2);                     // линкоры потоплены — строка ушла из состава
  CHECK_EQ(h->world().faction(vk)->fleetRow(lineRow)->total, vkLineTotal - lines);
  CHECK_EQ(h->world().faction(vk)->fleetRow(frigRow)->total, vkFrigTotal - 2);
  CHECK(h->ui.sel == (app::Selection{app::SelType::Army, f1}));
  h.key(Key::Z, ctrl());
  CHECK(h->world().army(f1)->pos == p1);
  CHECK(h->world().army(f2)->pos == p2);
  CHECK_EQ(h->world().faction(vk)->fleetRow(lineRow)->total, vkLineTotal);
}

// Объединение войск одной фракции: отказ возвращает, согласие складывает численность.
TEST(app_military_drag_merge) {
  Harness h("military_drag_merge");
  RealArmyTools tools;
  h.demo();
  Id hel = factionByName(h->world(), "Хельдвиг");
  Id x = armyOf(h->world(), hel, ArmyKind::Army);
  Vec2 xpos = h->world().army(x)->pos;
  Id y = spawnArmy(h, hel, xpos + Vec2(170, 40), 100);
  CHECK(y != 0);
  Vec2 ypos = h->world().army(y)->pos;
  i64 nx = app::mil::unitCount(*h->world().army(x));
  focusOn(h, y, (xpos + ypos) * 0.5);
  dragTo(h, screenOf(h, ypos), screenOf(h, xpos));
  h.settle();
  CHECK(h->hasDialog("army.encounter"));
  CHECK(h.shot("military_merge_dialog"));
  CHECK(h.clickUi("encounter.cancel"));
  h.settle();
  CHECK(!h->hasDialog());
  CHECK(h->world().army(y) != nullptr);
  CHECK(h->world().army(y)->pos == ypos);   // отказ — на исходную позицию
  dragTo(h, screenOf(h, ypos), screenOf(h, xpos));
  h.settle();
  CHECK(h->hasDialog("army.encounter"));
  CHECK(h.clickUi("encounter.ok"));
  h.settle();
  CHECK(h->world().army(y) == nullptr);
  CHECK_EQ(app::mil::unitCount(*h->world().army(x)), nx + 100);
  CHECK(h->ui.sel == (app::Selection{app::SelType::Army, x}));
  h.key(Key::Z, ctrl());
  CHECK(h->world().army(y) != nullptr);
  CHECK_EQ(app::mil::unitCount(*h->world().army(x)), nx);
}

// Союзники: союзное войско с отдельными плитками фракций.
TEST(app_military_drag_alliance) {
  Harness h("military_drag_alliance");
  RealArmyTools tools;
  h.demo();
  Id hel = factionByName(h->world(), "Хельдвиг");
  Id alm = factionByName(h->world(), "Альмарин");
  CHECK(h->world().relation(hel, alm).s == RelStatus::Alliance);
  Id x = armyOf(h->world(), hel, ArmyKind::Army);
  Vec2 xpos = h->world().army(x)->pos;
  Id z = spawnArmy(h, alm, xpos + Vec2(-170, 60), 300);
  CHECK(z != 0);
  Vec2 zpos = h->world().army(z)->pos;
  focusOn(h, z, (xpos + zpos) * 0.5);
  dragTo(h, screenOf(h, zpos), screenOf(h, xpos), "military_drag_alliance_ghost");
  h.settle();
  CHECK(h->hasDialog("army.encounter"));
  CHECK(h.shot("military_alliance_dialog"));
  CHECK(h.clickUi("encounter.ok"));
  h.settle();
  CHECK(h->world().army(z) == nullptr);
  const Army* a = h->world().army(x);
  CHECK(a->allied());
  CHECK_EQ(a->groups.size(), size_t(2));
  CHECK_EQ(app::mil::groupCount(a->groups[1]), 300);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("military_alliance_formed"));
}

// Статус-кво: объявление войны (−25, «в войне»), затем битва с потерями; проигравший смещается.
TEST(app_military_drag_war_and_battle) {
  Harness h("military_drag_war");
  RealArmyTools tools;
  h.demo();
  Id alm = factionByName(h->world(), "Альмарин");
  Id vk = factionByName(h->world(), "Валь-Кетра");
  CHECK(alm && vk);
  CHECK(h->world().relation(alm, vk).s == RelStatus::Neutral);
  Id v = armyOf(h->world(), vk, ArmyKind::Army);
  CHECK(v != 0);
  Vec2 vpos = h->world().army(v)->pos;
  Id z = spawnArmy(h, alm, vpos + Vec2(-190, 30), 500);
  CHECK(z != 0);
  Vec2 zpos = h->world().army(z)->pos;
  Id zrow = h->world().army(z)->groups[0].units[0].row;
  const Army& va = *h->world().army(v);
  Id vrow = va.groups[0].units[0].row;
  i64 vcount = va.groups[0].units[0].count;
  i64 almTotal = h->world().faction(alm)->armyRow(zrow)->total;
  i64 vkTotal = h->world().faction(vk)->armyRow(vrow)->total;
  focusOn(h, z, (vpos + zpos) * 0.5);
  dragTo(h, screenOf(h, zpos), screenOf(h, vpos));
  h.settle();
  CHECK(h->hasDialog("army.encounter"));
  CHECK(h.shot("military_declare_war"));
  CHECK(h.clickUi("encounter.ok"));
  h.settle();
  Relation rel = h->world().relation(alm, vk);
  CHECK(rel.s == RelStatus::War);
  CHECK_NEAR(rel.v, -25, 1e-9);
  CHECK(h->hasDialog("battle"));
  // Потери: 120 у нападающего, 300 у защитника; победа нападающего.
  CHECK(h.clickUi("battle.loss." + std::to_string(z) + "." + std::to_string(zrow)));
  h.retype("120");
  h.key(Key::Enter);
  CHECK(h->hasDialog("battle"));
  CHECK(h.clickUi("battle.loss." + std::to_string(v) + "." + std::to_string(vrow)));
  h.retype(std::to_string(vcount + 999));   // больше численности — ограничивается
  h.key(Key::Enter);
  CHECK(h.clickUi("battle.loss." + std::to_string(v) + "." + std::to_string(vrow)));
  h.retype("300");
  h.key(Key::Enter);
  CHECK(h.clickUi("battle.winner.defender"));
  CHECK(h.clickUi("battle.winner.attacker"));
  h.settle();
  CHECK(h.shot("military_battle"));
  CHECK(h.clickUi("battle.apply"));
  h.settle();
  CHECK(!h->hasDialog());
  const Army* za = h->world().army(z);
  const Army* vb = h->world().army(v);
  CHECK(za && vb);
  CHECK(dist(za->pos, vpos) < 1e-6);                       // победитель — на месте боя
  CHECK(dist(vb->pos, vpos) >= 2 * schema::kObjectRadius - 1e-6);   // проигравший смещён
  CHECK_EQ(app::mil::unitCount(*za), 500 - 120);
  CHECK_EQ(h->world().faction(alm)->armyRow(zrow)->total, almTotal - 120);
  CHECK_EQ(h->world().faction(vk)->armyRow(vrow)->total, vkTotal - 300);
  bool logged = false;
  h->world().log.each([&](const LogEntry& e) { logged = logged || (e.kind == LogKind::Battle && e.army == z); });
  CHECK(logged);
  h.dropToasts();
  h.waitMap();
  h.settle();
  CHECK(h.shot("military_after_battle"));
  // Отмена битвы — одним шагом; объявление войны — отдельным.
  h.key(Key::Z, ctrl());
  CHECK(h->world().army(z)->pos == zpos);
  CHECK(h->world().army(v)->pos == vpos);
  CHECK(h->world().relation(alm, vk).s == RelStatus::War);
}

// Война: сразу панель битвы; «Отступить» — нападавший на исходной позиции, мир не меняется.
TEST(app_military_drag_battle_retreat) {
  Harness h("military_drag_retreat");
  RealArmyTools tools;
  h.demo();
  Id hel = factionByName(h->world(), "Хельдвиг");
  Id vk = factionByName(h->world(), "Валь-Кетра");
  CHECK(h->world().relation(hel, vk).s == RelStatus::War);
  Id x = armyOf(h->world(), hel, ArmyKind::Army);
  Id v = armyOf(h->world(), vk, ArmyKind::Army);
  Vec2 xpos = h->world().army(x)->pos, vpos = h->world().army(v)->pos;
  focusOn(h, x, (xpos + vpos) * 0.5, std::min(0.6, 0.5 * h->mapArea().w / std::max(1.0, dist(xpos, vpos) + 200)));
  World before = h->world();
  dragTo(h, screenOf(h, xpos), screenOf(h, vpos), "military_drag_battle_ghost");
  h.settle();
  CHECK(h->hasDialog("battle"));
  CHECK(h.clickUi("battle.retreat"));
  h.settle();
  CHECK(!h->hasDialog());
  CHECK(h->world().army(x)->pos == xpos);
  CHECK(h->world().army(v)->pos == vpos);
  CHECK_EQ(World::diff(before, h->world()) & ~TB_LOG, 0u);
  // Битва из реестра диалогов недоступна, пока противник не рядом.
  h.dropToasts();
  CHECK(h->openDialog("battle", x));
  h.settle();
  CHECK(!h->hasDialog("battle"));
}

// Прошлый ход открыт только для просмотра: перетаскивание и правка недоступны.
TEST(app_military_readonly_past_turn) {
  Harness h("military_readonly");
  RealArmyTools tools;
  h.demo();
  CHECK(h->endTurnNow());
  h.dropToasts();
  CHECK(h->viewTurn(1));
  CHECK(h->readOnly());
  h.settle();
  Id hel = factionByName(h->world(), "Хельдвиг");
  Id x = armyOf(h->world(), hel, ArmyKind::Army);
  Vec2 orig = h->world().army(x)->pos;
  focusOn(h, x, orig);
  auto s = rules::findFreeSpot(h->world(), ArmyKind::Army, orig + Vec2(180, 0), x);
  CHECK(s.has_value());
  dragTo(h, screenOf(h, orig), screenOf(h, *s));
  h.settle();
  CHECK(h->world().army(x)->pos == orig);
  CHECK(h->store.world().army(x)->pos == orig);
  // Delete — только сообщение о просмотре прошлого хода, без подтверждения.
  h.key(Key::Delete);
  CHECK(!h->hasDialog("confirm"));
  CHECK(h->store.world().army(x) != nullptr);
  CHECK(h.shot("military_readonly"));
  h->backToCurrent();
  CHECK(!h->readOnly());
}
