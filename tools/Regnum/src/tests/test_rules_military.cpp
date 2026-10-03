// Тесты rules: войска и флот — размещение, отряды и резерв, гарнизоны, герои, встречи всех видов,
// объединение, союзные войска, разделение, битвы с потерями и отступлением.
#include <cstdio>

#include "tests/test_rules_util.h"

using namespace rg;
using namespace rg::rules;
using namespace rg::rulestest;

namespace {

constexpr double R = schema::kObjectRadius;

i64 unitsIn(const World& w, Id army, Id faction, Id row) {
  const Army* a = w.army(army);
  if (!a) return -1;
  i64 n = 0;
  for (auto& g : a->groups)
    if (g.faction == faction)
      for (auto& u : g.units)
        if (u.row == row) n += u.count;
  return n;
}

// Все объекты стоят на допустимых местах и не перекрываются.
void checkPlacement(const World& w) {
  w.armies.each([&](const Army& a) {
    std::string why;
    CHECK_MSG(validPosition(w, a.kind, a.pos, a.id, &why), why);
  });
}

}  // namespace

TEST(rules_military_valid_position) {
  Fix f;
  std::string why;
  CHECK(validPosition(f.w(), ArmyKind::Army, center(0)));
  CHECK(!validPosition(f.w(), ArmyKind::Army, kSeaNorth, 0, &why));
  CHECK(has(why, "только на суше"));
  CHECK(validPosition(f.w(), ArmyKind::Fleet, kSeaNorth));
  CHECK(validPosition(f.w(), ArmyKind::Fleet, kSeaWest));  // морская провинция — море
  CHECK(!validPosition(f.w(), ArmyKind::Fleet, center(0), 0, &why));
  CHECK(has(why, "только на море"));
  CHECK(!validPosition(f.w(), ArmyKind::Army, {-10, 300}, 0, &why));
  CHECK(has(why, "вне карты"));
  Id a1 = 0;
  f.tx([&](Tx& tx) { a1 = createArmy(tx, ArmyKind::Army, f.A, {200, 200}); });
  CHECK(!validPosition(f.w(), ArmyKind::Army, {200 + 2 * R - 0.5, 200}, 0, &why));
  CHECK(has(why, "Место занято: «Войско №1»"));
  CHECK(validPosition(f.w(), ArmyKind::Army, {200 + 2 * R, 200}));      // ровно 2R — касание без наложения
  CHECK(validPosition(f.w(), ArmyKind::Army, {210, 200}, a1));          // себя не учитываем
  // Войско и флот тоже не накладываются.
  f.tx([&](Tx& tx) { createArmy(tx, ArmyKind::Army, f.A, {700, 130}); });
  CHECK(validPosition(f.w(), ArmyKind::Fleet, {700, 60}));
  CHECK(!validPosition(f.w(), ArmyKind::Fleet, {700, 80}, 0, &why));
  CHECK(has(why, "Место занято: «Войско №2»"));
  // armyAt: точка под фигуркой.
  CHECK_EQ(armyAt(f.w(), {200 + R - 1, 200}), a1);
  CHECK_EQ(armyAt(f.w(), {200 + R + 1, 200}), Id(0));
  CHECK_EQ(armyAt(f.w(), {200, 200}, a1), Id(0));
  // Мир без карты.
  World empty = newWorld("x");
  CHECK(!validPosition(empty, ArmyKind::Army, {10, 10}, 0, &why));
  CHECK(has(why, "Карта ещё не создана"));
  CHECK(!findFreeSpot(empty, ArmyKind::Army, {10, 10}));
}

TEST(rules_military_create_units_reserve) {
  Fix f;
  Id r1 = 0, rB = 0, s1 = 0, a1 = 0, a2 = 0, fl = 0;
  f.tx([&](Tx& tx) {
    r1 = addArmyRow(tx, f.A, UnitType::MediumInf, "Пехота", 100, 1);
    rB = addArmyRow(tx, f.B, UnitType::MediumInf, "Пехота Б", 100, 1);
    s1 = addFleetRow(tx, f.A, ShipType::Frigate, "", 5, 1);
    tx.province(f.p[0]).owner = f.A;
    setGarrison(tx, f.p[0], r1, 30);
    a1 = createArmy(tx, ArmyKind::Army, f.A, center(1));
    a2 = createArmy(tx, ArmyKind::Army, f.A, center(2));
    fl = createArmy(tx, ArmyKind::Fleet, f.A, kSeaNorth);
    setUnits(tx, a1, f.A, r1, 50);
  });
  CHECK_EQ(f.w().army(a1)->name, std::string("Войско №1"));
  CHECK_EQ(f.w().army(a2)->name, std::string("Войско №2"));
  CHECK_EQ(f.w().army(fl)->name, std::string("Флот №1"));
  CHECK(has(lastLog(f.w(), "собран флот"), "Флот №1"));
  CHECK_EQ(f.fc(f.A).army[0].reserve, 20);
  std::string e = errorOf([&] { f.tx([&](Tx& tx) { setUnits(tx, a2, f.A, r1, 25); }); });
  CHECK(has(e, "нужно 25"));
  CHECK(has(e, "в резерве 20"));
  f.tx([&](Tx& tx) { setUnits(tx, a2, f.A, r1, 20); });
  CHECK_EQ(f.fc(f.A).army[0].reserve, 0);
  f.tx([&](Tx& tx) { setUnits(tx, a1, f.A, r1, 10); });  // уменьшение возвращает в резерв
  CHECK_EQ(f.fc(f.A).army[0].reserve, 40);
  CHECK_EQ(unitsIn(f.w(), a1, f.A, r1), 10);
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setUnits(tx, a1, f.A, s1, 1); }); }), "нет в таблице войск"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setUnits(tx, fl, f.A, r1, 1); }); }), "нет в таблице флота"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setUnits(tx, a1, f.B, rB, 1); }); }), "нет отрядов"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setUnits(tx, a1, f.A, r1, -1); }); }), "отрицательной"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setUnits(tx, fl, f.A, s1, 6); }); }), "в резерве 5"));
  f.tx([&](Tx& tx) { setUnits(tx, fl, f.A, s1, 5); });
  CHECK_EQ(f.fc(f.A).fleet[0].reserve, 0);
  f.tx([&](Tx& tx) { setUnits(tx, a1, f.A, r1, 0); });
  CHECK(f.w().army(a1)->groups[0].units.empty());
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { createArmy(tx, ArmyKind::Army, f.A, kSeaNorth); }); }), "только на суше"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { createArmy(tx, ArmyKind::Army, f.A, center(1)); }); }), "Место занято"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { createArmy(tx, ArmyKind::Army, 999, center(5)); }); }), "не найдена"));
}

TEST(rules_military_garrison) {
  Fix f;
  Id r1 = 0, rB = 0;
  f.tx([&](Tx& tx) {
    r1 = addArmyRow(tx, f.A, UnitType::LightInf, "", 40, 1);
    rB = addArmyRow(tx, f.B, UnitType::LightInf, "", 40, 1);
    tx.province(f.p[0]).owner = f.A;
  });
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setGarrison(tx, f.p[1], r1, 5); }); }), "нет владельца"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setGarrison(tx, f.seaW, r1, 5); }); }), "морской"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setGarrison(tx, f.p[0], rB, 5); }); }), "нет в таблице войск"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setGarrison(tx, f.p[0], r1, 41); }); }), "в резерве 40"));
  f.tx([&](Tx& tx) { setGarrison(tx, f.p[0], r1, 25); });
  CHECK_EQ(f.fc(f.A).army[0].garrison, 25);
  CHECK_EQ(f.fc(f.A).army[0].field, 25);
  CHECK_EQ(f.fc(f.A).army[0].reserve, 15);
  f.tx([&](Tx& tx) { setGarrison(tx, f.p[0], r1, 40); });  // 15 из резерва сверх текущих 25
  CHECK_EQ(f.fc(f.A).army[0].reserve, 0);
  CHECK_EQ(f.w().province(f.p[0])->garrison.size(), size_t(1));
  f.tx([&](Tx& tx) { setGarrison(tx, f.p[0], r1, 0); });
  CHECK(f.w().province(f.p[0])->garrison.empty());
  CHECK_EQ(f.fc(f.A).army[0].reserve, 40);
}

TEST(rules_military_heroes_commander) {
  Fix f;
  Id a1 = 0, a2 = 0, h1 = 0, h2 = 0, hb = 0, free = 0;
  f.tx([&](Tx& tx) {
    a1 = createArmy(tx, ArmyKind::Army, f.A, center(0));
    a2 = createArmy(tx, ArmyKind::Army, f.A, center(1));
    h1 = createCharacter(tx, f.A, "Аларик");
    h2 = createCharacter(tx, f.A, "Бранд");
    hb = createCharacter(tx, f.B, "Чужой");
    free = createCharacter(tx, 0, "Странник");
    setHero(tx, a1, h1, true);
  });
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setHero(tx, a2, h1, true); }); }), "уже сопровождает «Войско №1»"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setHero(tx, a1, hb, true); }); }), "нет её отрядов"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setHero(tx, a1, free, true); }); }), "не состоит"));
  f.tx([&](Tx& tx) { setCommander(tx, a1, h2); });  // полководец становится героем войска
  CHECK_EQ(f.w().army(a1)->commander, h2);
  CHECK(f.w().army(a1)->groups[0].heroes == std::vector<Id>({h1, h2}));
  f.tx([&](Tx& tx) { setHero(tx, a1, h2, false); });
  CHECK_EQ(f.w().army(a1)->commander, Id(0));
  f.tx([&](Tx& tx) { setCommander(tx, a1, h1); });
  f.tx([&](Tx& tx) { setCommander(tx, a1, 0); });
  CHECK_EQ(f.w().army(a1)->commander, Id(0));
  CHECK(f.w().army(a1)->groups[0].heroes == std::vector<Id>({h1}));
}

TEST(rules_military_disband_and_move) {
  Fix f;
  Id r1 = 0, a1 = 0, h = 0;
  f.tx([&](Tx& tx) {
    r1 = addArmyRow(tx, f.A, UnitType::LightInf, "", 60, 1);
    a1 = createArmy(tx, ArmyKind::Army, f.A, center(0));
    setUnits(tx, a1, f.A, r1, 60);
    h = createCharacter(tx, f.A, "Герой");
    setCommander(tx, a1, h);
    createArmy(tx, ArmyKind::Army, f.B, center(2));
  });
  CHECK_EQ(f.fc(f.A).army[0].reserve, 0);
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { moveArmy(tx, a1, kSeaNorth); }); }), "только на суше"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { moveArmy(tx, a1, center(2) + Vec2{10, 0}); }); }), "Место занято"));
  f.tx([&](Tx& tx) { moveArmy(tx, a1, center(5)); });
  CHECK(f.w().army(a1)->pos == center(5));
  f.tx([&](Tx& tx) { disband(tx, a1); });
  CHECK(!f.w().army(a1));
  CHECK_EQ(f.fc(f.A).army[0].reserve, 60);  // отряды вернулись в резерв
  CHECK(f.w().character(h) != nullptr);     // герой остаётся у фракции
  CHECK(has(lastLog(f.w()), "расформировано"));
}

TEST(rules_military_encounter_types) {
  Fix f;
  Id a1 = 0, a2 = 0, b1 = 0, c1 = 0, a4 = 0, fl = 0;
  f.tx([&](Tx& tx) {
    a1 = createArmy(tx, ArmyKind::Army, f.A, center(0));
    a2 = createArmy(tx, ArmyKind::Army, f.A, center(1));
    b1 = createArmy(tx, ArmyKind::Army, f.B, center(2));
    c1 = createArmy(tx, ArmyKind::Army, f.C, center(3));
    a4 = createArmy(tx, ArmyKind::Army, f.A, {500, 150});
    fl = createArmy(tx, ArmyKind::Fleet, f.A, {700, 50});
  });
  Encounter e = encounter(f.w(), a1, center(5));
  CHECK(e.type == EncounterType::None);
  CHECK_EQ(e.target, Id(0));
  e = encounter(f.w(), a1, center(1));
  CHECK(e.type == EncounterType::Merge);
  CHECK_EQ(e.target, a2);
  e = encounter(f.w(), a1, center(1) + Vec2{0, 40});  // фигурки перекрываются
  CHECK(e.type == EncounterType::Merge);
  e = encounter(f.w(), a1, kSeaNorth);
  CHECK(e.type == EncounterType::Blocked);
  CHECK(has(e.reason, "только на суше"));
  e = encounter(f.w(), fl, {500, 90});  // флот на войско у берега
  CHECK(e.type == EncounterType::Blocked);
  CHECK_EQ(e.target, a4);
  CHECK(has(e.reason, "не взаимодействует"));
  e = encounter(f.w(), 9999, center(5));
  CHECK(e.type == EncounterType::Blocked);
  // Незнакомы — предложение объявить войну.
  e = encounter(f.w(), a1, center(2));
  CHECK(e.type == EncounterType::DeclareWar);
  CHECK_EQ(e.us, f.A);
  CHECK_EQ(e.them, f.B);
  CHECK(has(e.reason, "Объявить войну «Бельмар»"));
  f.tx([&](Tx& tx) {
    setRelation(tx, f.A, f.B, 10, RelStatus::Neutral);
    setRelation(tx, f.A, f.C, 60, RelStatus::Alliance);
  });
  CHECK(encounter(f.w(), a1, center(2)).type == EncounterType::DeclareWar);  // статус-кво
  e = encounter(f.w(), a1, center(3));
  CHECK(e.type == EncounterType::Alliance);
  CHECK_EQ(e.target, c1);
  f.tx([&](Tx& tx) { declareWar(tx, f.A, f.B); });
  e = encounter(f.w(), a1, center(2));
  CHECK(e.type == EncounterType::Battle);
  CHECK_EQ(e.target, b1);
  CHECK_EQ(e.us, f.A);
  CHECK_EQ(e.them, f.B);
  // Союзное войско {A, C}: войско A складывается в свою группу (не новый союз), враг — битва.
  f.tx([&](Tx& tx) { formAllied(tx, c1, a2); });
  CHECK(encounter(f.w(), a1, center(3)).type == EncounterType::Merge);
  e = encounter(f.w(), b1, center(3));
  CHECK(e.type == EncounterType::Battle);
  CHECK_EQ(e.us, f.B);
  CHECK_EQ(e.them, f.A);
}

TEST(rules_military_declare_war) {
  Fix f;
  f.tx([&](Tx& tx) { setRelation(tx, f.A, f.B, 80, RelStatus::Alliance); });
  f.tx([&](Tx& tx) { declareWar(tx, f.B, f.A); });
  Relation r = f.w().relation(f.A, f.B);
  CHECK(r.s == RelStatus::War);
  CHECK_NEAR(r.v, -25, 1e-9);  // «равны 0 и ещё минус 25»
  CHECK(f.w().relation(f.B, f.A) == r);
  CHECK(has(lastLog(f.w()), "объявляет войну"));
  CHECK_EQ(logCount(f.w(), LogKind::War), 1);
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { declareWar(tx, f.A, f.B); }); }), "уже в войне"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { declareWar(tx, f.A, f.A); }); }), "самой себе"));
}

TEST(rules_military_merge_armies) {
  Fix f;
  Id r1 = 0, r2 = 0, a1 = 0, a2 = 0, b1 = 0, fl = 0, h1 = 0, h2 = 0;
  f.tx([&](Tx& tx) {
    r1 = addArmyRow(tx, f.A, UnitType::LightInf, "", 100, 1);
    r2 = addArmyRow(tx, f.A, UnitType::Ranged, "", 100, 1);
    a1 = createArmy(tx, ArmyKind::Army, f.A, center(0));
    a2 = createArmy(tx, ArmyKind::Army, f.A, center(1));
    setUnits(tx, a1, f.A, r1, 10);
    setUnits(tx, a2, f.A, r1, 15);
    setUnits(tx, a2, f.A, r2, 5);
    h1 = createCharacter(tx, f.A, "Один");
    h2 = createCharacter(tx, f.A, "Два");
    setHero(tx, a1, h1, true);
    setCommander(tx, a2, h2);
    b1 = createArmy(tx, ArmyKind::Army, f.B, center(2));
    fl = createArmy(tx, ArmyKind::Fleet, f.A, kSeaNorth);
  });
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { mergeArmies(tx, a1, b1); }); }), "одной фракции"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { mergeArmies(tx, a1, fl); }); }), "с флотом"));
  f.tx([&](Tx& tx) { mergeArmies(tx, a1, a2); });
  const World& w = f.w();
  CHECK(!w.army(a2));
  CHECK_EQ(unitsIn(w, a1, f.A, r1), 25);
  CHECK_EQ(unitsIn(w, a1, f.A, r2), 5);
  CHECK(w.army(a1)->groups[0].heroes == std::vector<Id>({h1, h2}));
  CHECK_EQ(w.army(a1)->commander, h2);  // у цели не было полководца
  CHECK(w.army(a1)->pos == center(0));
  CHECK_EQ(f.fc(f.A).army[0].field, 25);
  CHECK(has(lastLog(w), "Объединены"));
}

TEST(rules_military_allied_form_and_dissolve) {
  Fix f;
  Id rA = 0, rC = 0, aA = 0, aC = 0, bB = 0, hA = 0, hC = 0;
  f.tx([&](Tx& tx) {
    rA = addArmyRow(tx, f.A, UnitType::LightInf, "", 50, 1);
    rC = addArmyRow(tx, f.C, UnitType::Beasts, "", 50, 1);
    aA = createArmy(tx, ArmyKind::Army, f.A, center(0));
    aC = createArmy(tx, ArmyKind::Army, f.C, center(1));
    bB = createArmy(tx, ArmyKind::Army, f.B, center(2));
    setUnits(tx, aA, f.A, rA, 20);
    setUnits(tx, aC, f.C, rC, 15);
    hA = createCharacter(tx, f.A, "Герой А");
    hC = createCharacter(tx, f.C, "Герой Ц");
    setHero(tx, aA, hA, true);
    setHero(tx, aC, hC, true);
  });
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { formAllied(tx, aA, aC); }); }), "не в союзе"));
  f.tx([&](Tx& tx) { setRelation(tx, f.A, f.C, 50, RelStatus::Alliance); });
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { formAllied(tx, aA, bB); }); }), "не в союзе"));
  f.tx([&](Tx& tx) {
    formAllied(tx, aA, aC);
    setCommander(tx, aA, hC);
  });
  const Army& al = *f.w().army(aA);
  CHECK(!f.w().army(aC));
  CHECK(al.allied());
  CHECK_EQ(al.name, std::string("Союзное войско"));
  CHECK_EQ(al.groups.size(), size_t(2));
  CHECK_EQ(al.groups[0].faction, f.A);
  CHECK_EQ(al.groups[1].faction, f.C);
  CHECK_EQ(unitsIn(f.w(), aA, f.C, rC), 15);  // группы хранятся раздельно
  CHECK_EQ(f.fc(f.A).army[0].field, 20);
  CHECK_EQ(f.fc(f.C).army[0].field, 15);
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { dissolveAllied(tx, bB); }); }), "не союзное"));
  std::vector<Id> parts;
  f.tx([&](Tx& tx) { parts = dissolveAllied(tx, aA); });
  CHECK_EQ(parts.size(), size_t(2));
  CHECK_EQ(parts[0], aA);
  const World& w = f.w();
  CHECK_EQ(w.army(aA)->groups.size(), size_t(1));
  CHECK_EQ(w.army(aA)->groups[0].faction, f.A);
  CHECK(w.army(aA)->name != "Союзное войско");
  CHECK_EQ(w.army(aA)->commander, Id(0));
  const Army& n = *w.army(parts[1]);
  CHECK_EQ(n.groups[0].faction, f.C);
  CHECK_EQ(unitsIn(w, parts[1], f.C, rC), 15);
  CHECK(n.groups[0].heroes == std::vector<Id>({hC}));
  CHECK_EQ(n.commander, hC);  // полководец ушёл со своей группой
  CHECK(dist(n.pos, w.army(aA)->pos) >= 2 * R);
  checkPlacement(w);
  (void)hA;
}

TEST(rules_military_split_army) {
  Fix f;
  Id r1 = 0, r2 = 0, a = 0, h1 = 0, h2 = 0;
  f.tx([&](Tx& tx) {
    r1 = addArmyRow(tx, f.A, UnitType::LightInf, "", 100, 1);
    r2 = addArmyRow(tx, f.A, UnitType::HeavyInf, "", 100, 1);
    a = createArmy(tx, ArmyKind::Army, f.A, center(5));
    setUnits(tx, a, f.A, r1, 30);
    setUnits(tx, a, f.A, r2, 10);
    h1 = createCharacter(tx, f.A, "Первый");
    h2 = createCharacter(tx, f.A, "Второй");
    setCommander(tx, a, h1);
    setHero(tx, a, h2, true);
  });
  SplitSpec bad;
  bad.units[{f.A, r1}] = 31;
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { splitArmy(tx, a, bad); }); }), "больше, чем есть"));
  SplitSpec all;
  all.units[{f.A, r1}] = 30;
  all.units[{f.A, r2}] = 10;
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { splitArmy(tx, a, all); }); }), "весь состав"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { splitArmy(tx, a, SplitSpec{}); }); }), "Выберите"));
  SplitSpec s;
  s.units[{f.A, r1}] = 10;
  s.units[{f.A, r2}] = 10;
  s.heroes = {h1};
  Id n = 0;
  f.tx([&](Tx& tx) { n = splitArmy(tx, a, s); });
  const World& w = f.w();
  CHECK_EQ(unitsIn(w, a, f.A, r1), 20);
  CHECK_EQ(unitsIn(w, a, f.A, r2), 0);
  CHECK_EQ(w.army(a)->groups[0].units.size(), size_t(1));
  CHECK(w.army(a)->groups[0].heroes == std::vector<Id>({h2}));
  CHECK_EQ(w.army(a)->commander, Id(0));
  CHECK_EQ(unitsIn(w, n, f.A, r1), 10);
  CHECK_EQ(unitsIn(w, n, f.A, r2), 10);
  CHECK_EQ(w.army(n)->commander, h1);
  CHECK(dist(w.army(n)->pos, w.army(a)->pos) >= 2 * R);
  CHECK_EQ(f.fc(f.A).army[0].field, 30);  // разделение не меняет численность в поле
  checkPlacement(w);
  CHECK(has(lastLog(w), "разделено"));
}

namespace {

struct BattleSetup {
  Fix f;
  Id rA = 0, rB = 0, aA = 0, bB = 0, hB = 0;
  BattleSetup() {
    f.tx([&](Tx& tx) {
      rA = addArmyRow(tx, f.A, UnitType::LightInf, "", 100, 1);
      rB = addArmyRow(tx, f.B, UnitType::HeavyInf, "", 80, 1);
      aA = createArmy(tx, ArmyKind::Army, f.A, {200, 200});
      bB = createArmy(tx, ArmyKind::Army, f.B, {400, 200});
      setUnits(tx, aA, f.A, rA, 100);
      setUnits(tx, bB, f.B, rB, 80);
      hB = createCharacter(tx, f.B, "Воевода");
      setCommander(tx, bB, hB);
      declareWar(tx, f.A, f.B);
    });
  }
  BattleResult result(bool attackerWins, i64 lossA, i64 lossB) const {
    BattleResult r;
    r.attacker = aA;
    r.defender = bB;
    r.attackerWins = attackerWins;
    r.attackerOrigin = {200, 200};
    if (lossA) r.losses[aA][{f.A, rA}] = lossA;
    if (lossB) r.losses[bB][{f.B, rB}] = lossB;
    return r;
  }
};

}  // namespace

TEST(rules_military_battle_attacker_wins) {
  BattleSetup s;
  Fix& f = s.f;
  f.tx([&](Tx& tx) { resolveBattle(tx, s.result(true, 30, 50)); });
  const World& w = f.w();
  CHECK(w.army(s.aA)->pos == Vec2(400, 200));  // победитель на месте боя
  CHECK_EQ(unitsIn(w, s.aA, f.A, s.rA), 70);
  CHECK_EQ(w.faction(f.A)->armyRow(s.rA)->total, 70);  // потери вычтены из общей численности
  CHECK_EQ(unitsIn(w, s.bB, f.B, s.rB), 30);
  CHECK_EQ(w.faction(f.B)->armyRow(s.rB)->total, 30);
  Vec2 lp = w.army(s.bB)->pos;
  CHECK(lp.x > 400);  // отступил прочь от нападавшего
  CHECK(dist(lp, Vec2(400, 200)) >= 2 * R);
  CHECK(dist(lp, Vec2(400, 200)) < 4 * R);
  checkPlacement(w);
  std::string l = lastLog(w);
  CHECK(has(l, "Битва"));
  CHECK(has(l, "Победа: «Войско №1»"));
  CHECK(has(l, "Потери: 30 и 50"));
  CHECK_EQ(logCount(w, LogKind::Battle), 1);
}

TEST(rules_military_battle_defender_wins_and_destroyed) {
  {
    BattleSetup s;
    s.f.tx([&](Tx& tx) { resolveBattle(tx, s.result(false, 40, 0)); });
    const World& w = s.f.w();
    CHECK(w.army(s.bB)->pos == Vec2(400, 200));  // обороняющийся стоит
    Vec2 lp = w.army(s.aA)->pos;
    CHECK(lp.x < 400);                             // нападавший отходит к исходной позиции
    CHECK(dist(lp, Vec2(400, 200)) >= 2 * R);
    CHECK(dist(lp, Vec2(400, 200)) < 4 * R);
    CHECK_EQ(unitsIn(w, s.aA, s.f.A, s.rA), 60);
    checkPlacement(w);
  }
  {
    BattleSetup s;  // обороняющийся уничтожен целиком
    s.f.tx([&](Tx& tx) { resolveBattle(tx, s.result(true, 0, 80)); });
    const World& w = s.f.w();
    CHECK(!w.army(s.bB));
    CHECK(w.character(s.hB) != nullptr);  // герои остаются у фракции
    CHECK_EQ(w.faction(s.f.B)->armyRow(s.rB)->total, 0);
    CHECK(w.army(s.aA)->pos == Vec2(400, 200));
    CHECK(has(lastLog(w), "Уничтожено войско"));
  }
  {
    BattleSetup s;  // победитель без отрядов после потерь — отказ с причиной, мир не меняется
    World before = s.f.w();
    CHECK(has(errorOf([&] { s.f.tx([&](Tx& tx) { resolveBattle(tx, s.result(true, 100, 10)); }); }), "не осталось отрядов"));
    CHECK(World::diff(before, s.f.w()) == 0);
  }
  {
    BattleSetup s;  // уничтожены обе стороны — победы нет, оба объекта исчезают
    s.f.tx([&](Tx& tx) { resolveBattle(tx, s.result(true, 100, 80)); });
    const World& w = s.f.w();
    CHECK(!w.army(s.aA));
    CHECK(!w.army(s.bB));
    CHECK(has(lastLog(w), "Обе стороны уничтожены"));
  }
}

TEST(rules_military_battle_validation) {
  BattleSetup s;
  Fix& f = s.f;
  World before = f.w();
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { resolveBattle(tx, s.result(true, 101, 0)); }); }), "больше численности"));
  BattleResult neg = s.result(true, 0, 0);
  neg.losses[s.aA][{f.A, s.rA}] = -1;
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { resolveBattle(tx, neg); }); }), "отрицательными"));
  BattleResult foreign = s.result(true, 0, 0);
  foreign.losses[s.aA][{f.B, s.rB}] = 1;
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { resolveBattle(tx, foreign); }); }), "нет в составе"));
  Id other = 0;
  f.tx([&](Tx& tx) { other = createArmy(tx, ArmyKind::Army, f.C, center(6)); });
  BattleResult third = s.result(true, 0, 0);
  third.losses[other][{f.C, 1}] = 1;
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { resolveBattle(tx, third); }); }), "не участвующего"));
  BattleResult peace = s.result(true, 0, 0);
  peace.defender = other;
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { resolveBattle(tx, peace); }); }), "не находятся в состоянии войны"));
  CHECK_EQ(World::diff(before, f.w()) & ~u32(TB_ARMIES | TB_META | TB_LOG), 0u);
}

TEST(rules_military_battle_allied) {
  Fix f;
  Id rA = 0, rC = 0, rB = 0, al = 0, ac = 0, bB = 0;
  f.tx([&](Tx& tx) {
    rA = addArmyRow(tx, f.A, UnitType::LightInf, "", 50, 1);
    rC = addArmyRow(tx, f.C, UnitType::Flying, "", 20, 1);
    rB = addArmyRow(tx, f.B, UnitType::Monsters, "", 30, 1);
    setRelation(tx, f.A, f.C, 70, RelStatus::Alliance);
    al = createArmy(tx, ArmyKind::Army, f.A, center(5));
    ac = createArmy(tx, ArmyKind::Army, f.C, center(6));
    setUnits(tx, al, f.A, rA, 50);
    setUnits(tx, ac, f.C, rC, 20);
    formAllied(tx, al, ac);
    bB = createArmy(tx, ArmyKind::Army, f.B, center(1));
    setUnits(tx, bB, f.B, rB, 30);
    declareWar(tx, f.B, f.C);  // война с одним из союзников — уже битва
  });
  CHECK(encounter(f.w(), bB, center(5)).type == EncounterType::Battle);
  BattleResult r;
  r.attacker = bB;
  r.defender = al;
  r.attackerWins = false;
  r.attackerOrigin = center(1);
  r.losses[al][{f.C, rC}] = 20;
  r.losses[al][{f.A, rA}] = 5;
  r.losses[bB][{f.B, rB}] = 12;
  f.tx([&](Tx& tx) { resolveBattle(tx, r); });
  const World& w = f.w();
  CHECK_EQ(w.army(al)->groups.size(), size_t(1));  // группа без отрядов ушла с поля
  CHECK_EQ(w.army(al)->groups[0].faction, f.A);
  CHECK_EQ(unitsIn(w, al, f.A, rA), 45);
  CHECK_EQ(w.faction(f.C)->armyRow(rC)->total, 0);
  CHECK_EQ(w.faction(f.A)->armyRow(rA)->total, 45);
  CHECK_EQ(w.faction(f.B)->armyRow(rB)->total, 18);
  CHECK(w.army(al)->pos == center(5));
  Vec2 bp = w.army(bB)->pos;
  CHECK(dist(bp, center(5)) >= 2 * R);
  CHECK(bp.y < center(5).y);  // отступил в сторону, откуда пришёл (север)
  checkPlacement(w);
}

TEST(rules_military_find_free_spot) {
  Fix f;
  f.tx([&](Tx& tx) {
    createArmy(tx, ArmyKind::Army, f.A, center(1));
    createArmy(tx, ArmyKind::Army, f.A, center(1) + Vec2{2 * R, 0});
    createArmy(tx, ArmyKind::Army, f.A, center(1) - Vec2{2 * R, 0});
  });
  auto s1 = findFreeSpot(f.w(), ArmyKind::Army, center(1));
  auto s2 = findFreeSpot(f.w(), ArmyKind::Army, center(1));
  CHECK(s1.has_value());
  CHECK(s1 == s2);  // детерминированно
  CHECK(validPosition(f.w(), ArmyKind::Army, *s1));
  CHECK(dist(*s1, center(1)) <= 2 * R + 17);
  auto land = findFreeSpot(f.w(), ArmyKind::Army, kSeaNorth);
  CHECK(land.has_value());
  CHECK(land->y >= 100);  // ближайшая суша — к югу
  CHECK(land->y < 100 + 17);
  auto sea = findFreeSpot(f.w(), ArmyKind::Fleet, center(0));
  CHECK(sea.has_value());
  CHECK(geo::faces(f.w())->terrainAt(*sea) == Terrain::Sea);
  // Свободная позиция — сама точка.
  CHECK(findFreeSpot(f.w(), ArmyKind::Army, center(6)) == std::optional<Vec2>(center(6)));
}

TEST(rules_military_free_spot_worst_case) {
  // Карта без суши: войску негде встать — спираль проходит целиком и возвращает «нет места».
  Store s;
  s.transact("init", [](Tx& tx) {
    geo::Coast c;
    c.width = W;
    c.height = H;
    geo::initFromCoast(tx, c);
    Id a = createFaction(tx, FactionKind::State, "А");
    for (int i = 0; i < 60; i++) createArmy(tx, ArmyKind::Fleet, a, {70.0 + 95.0 * (i % 10), 70.0 + 90.0 * (i / 10)});
  });
  double t0 = nowSeconds();
  auto spot = findFreeSpot(s.world(), ArmyKind::Army, {500, 300});
  double t1 = nowSeconds();
  CHECK(!spot.has_value());
  auto sea = findFreeSpot(s.world(), ArmyKind::Fleet, {70, 70});
  double t2 = nowSeconds();
  CHECK(sea.has_value());
  std::printf("  perf: findFreeSpot без места — %.1f мс, с занятыми соседями — %.3f мс\n", (t1 - t0) * 1000, (t2 - t1) * 1000);
  CHECK((t1 - t0) < 0.5);
}
