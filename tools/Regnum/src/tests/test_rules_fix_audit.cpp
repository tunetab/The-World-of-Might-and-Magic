// Регрессионные тесты правил по итогам аудита ТЗ: налог ≤ 100 %, модификатор дохода и переводы, прирост малого
// населения, морские провинции, возврат ровно уплаченного, удаление уровня постройки, условия изученной технологии,
// провинции без области, союзные войска при войне, государственная гильдия, битва без отрядов у победителя.
#include "tests/test_rules_util.h"

using namespace rg;
using namespace rg::rules;
using namespace rg::rulestest;

namespace {

double stockSum(const World& w, Id faction) {
  const Faction* f = w.faction(faction);
  double s = 0;
  for (auto& [r, v] : f->res) s += v;
  return s;
}

// Постройка с ценой уровня (золото и ресурс 2) и провинция p0 у A с полными запасами.
struct BuildFix {
  Fix f;
  Id b = 0;
  BuildFix() {
    f.tx([&](Tx& tx) {
      b = createBuilding(tx, 0, "Мастерская");
      Building& m = tx.building(b);
      m.levels[0].turns = 2;
      m.levels[0].cost = {{kGold, 100}, {2, 30}};
      BuildingLevel l2;
      l2.turns = 2;
      l2.cost = {{kGold, 200}};
      m.levels.push_back(l2);
      tx.province(f.p[0]).owner = f.A;
      tx.province(f.p[0]).size = ProvSize::Large;
      tx.faction(f.A).res[kGold] = 1000;
      tx.faction(f.A).res[2] = 1000;
      tx.faction(f.B).res[kGold] = 0;
    });
  }
  Id costMod(double pct) {
    Id m = 0;
    f.tx([&](Tx& tx) {
      m = makeMod(tx, {{Fx::BuildCostPct, pct}});
      tx.province(f.p[0]).modifiers.push_back(m);
    });
    return m;
  }
  const ProvBuilding* pb() const {
    for (const ProvBuilding& x : f.w().province(f.p[0])->buildings)
      if (x.building == b) return &x;
    return nullptr;
  }
};

template <class V, class X> bool contains(const V& v, const X& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

i64 unitsOf(const World& w, Id army) {
  i64 n = 0;
  for (const ArmyGroup& g : w.army(army)->groups)
    for (const ArmyUnit& u : g.units) n += u.count;
  return n;
}

}  // namespace

// ---------------------------------------------------------------- 1. общий налог 1…100 %
TEST(rules_fix_total_tax_clamped) {
  Fix f;
  f.tx([&](Tx& tx) {
    tx.faction(f.A).tax = 100;
    Province& p = tx.province(f.p[0]);
    p.owner = f.A;
    p.baseTrade = 200;
    p.localTax = 30;
    p.hqs = {f.G};
    p.influence = {{f.G, 40}};
  });
  const ProvinceCalc& pc = f.pc(f.p[0]);
  CHECK_NEAR(pc.taxTotal, 100, 1e-9);
  CHECK(pc.provinceTax + pc.guildTax <= pc.tradeValue + 1e-9);
  CHECK(pc.guilds.size() == 1 && pc.guilds[0].net >= -1e-9);
  // Нижняя граница — 1 % (ТЗ 1.d.iv).
  f.tx([&](Tx& tx) {
    tx.faction(f.A).tax = 0;
    tx.province(f.p[0]).localTax = -50;
  });
  CHECK_NEAR(f.pc(f.p[0]).taxTotal, 1, 1e-9);
}

// ---------------------------------------------------------------- 2. модификатор дохода не множит переводы
TEST(rules_fix_income_pct_not_on_transfers) {
  Fix f;
  f.tx([&](Tx& tx) {
    tx.faction(f.A).modifiers = {makeMod(tx, {{Fx::IncomePct, 6}})};
    tx.faction(f.A).res[kGold] = 500;
    tx.faction(f.B).res[kGold] = 500;
  });
  const double recv0 = f.fc(f.A).net, pay0 = f.fc(f.B).net;
  f.tx([&](Tx& tx) { imposeTribute(tx, DealKind::Tribute, f.A, f.B, 100, 3); });
  CHECK_NEAR(f.fc(f.A).net - recv0, 100, 1e-9);
  CHECK_NEAR(f.fc(f.B).net - pay0, -100, 1e-9);
  const double t0a = f.w().faction(f.A)->treasury(), t0b = f.w().faction(f.B)->treasury();
  f.tx([&](Tx& tx) { endTurn(tx); });
  // Сколько отнято у одного, столько отдано другому.
  CHECK_NEAR((f.w().faction(f.A)->treasury() - t0a) + (f.w().faction(f.B)->treasury() - t0b), 0, 1e-9);
  // Собственный доход по-прежнему умножается.
  f.tx([&](Tx& tx) {
    tx.province(f.p[0]).owner = f.A;
    tx.province(f.p[0]).baseTrade = 1000;
    tx.faction(f.A).tax = 10;
  });
  CHECK_NEAR(f.fc(f.A).incTotal, 100 * 1.06 + 100, 1e-9);
}

// ---------------------------------------------------------------- 5. прирост малого населения
TEST(rules_fix_small_population_grows) {
  Fix f;
  Id race = 0;
  Id m = 0;
  f.tx([&](Tx& tx) {
    race = addCatalogItem(tx, CatalogList::Races, "Люди");
    m = makeMod(tx, {{Fx::PopGrowthPct, 1}});
    Province& p = tx.province(f.p[0]);
    p.owner = f.A;
    p.races = {RacePop{race, 40}};
    p.modifiers = {m};
    tx.province(f.p[1]).races = {RacePop{race, 1000}};
    tx.province(f.p[1]).modifiers = {m};
    tx.province(f.p[2]).races = {RacePop{race, 0}};
    tx.province(f.p[2]).modifiers = {m};
    tx.province(f.p[3]).races = {RacePop{race, 40}};  // без модификатора — без изменений
  });
  f.tx([&](Tx& tx) { endTurn(tx); });
  CHECK_EQ(f.w().province(f.p[0])->races[0].pop, 41);
  CHECK_EQ(f.w().province(f.p[1])->races[0].pop, 1010);
  CHECK_EQ(f.w().province(f.p[2])->races[0].pop, 0);
  CHECK_EQ(f.w().province(f.p[3])->races[0].pop, 40);
  for (int i = 0; i < 9; i++) f.tx([&](Tx& tx) { endTurn(tx); });
  CHECK_EQ(f.w().province(f.p[0])->races[0].pop, 50);
  // Убыль: −1 % от 40 — тоже не меньше одного за ход.
  f.tx([&](Tx& tx) {
    tx.modifier(m).fx[size_t(Fx::PopGrowthPct)] = -1;
    tx.province(f.p[0]).races = {RacePop{race, 40}};
  });
  f.tx([&](Tx& tx) { endTurn(tx); });
  CHECK_EQ(f.w().province(f.p[0])->races[0].pop, 39);
}

// ---------------------------------------------------------------- 6–7. морская провинция
TEST(rules_fix_sea_province_pauses_and_clears) {
  BuildFix s;
  Fix& f = s.f;
  Id row = 0;
  f.tx([&](Tx& tx) {
    row = addArmyRow(tx, f.A, UnitType::LightInf, "", 100, 1);
    setGarrison(tx, f.p[0], row, 40);
    setCapital(tx, f.A, f.p[0]);
    setOccupied(tx, f.p[0], f.B);
    startBuilding(tx, f.p[0], s.b);
  });
  f.tx([&](Tx& tx) { setProvinceSea(tx, f.p[0], true); });
  const Province* p = f.w().province(f.p[0]);
  CHECK(p->sea);
  CHECK(p->garrison.empty());
  CHECK(!p->occupied && !p->occupier);
  CHECK_EQ(f.w().faction(f.A)->capital, Id(0));
  CHECK_EQ(p->owner, f.A);  // данные суши сохраняются
  CHECK(has(lastLog(f.w()), "стала морской"));
  CHECK(has(lastLog(f.w()), "приостановлено"));
  CHECK_EQ(f.fc(f.A).armyField, 0);  // отряды гарнизона — в резерве
  // Ходы идут — стройка в море стоит.
  for (int i = 0; i < 5; i++) f.tx([&](Tx& tx) { endTurn(tx); });
  CHECK(s.pb() && s.pb()->constructing && s.pb()->left == 2);
  CHECK(lastLog(f.w(), "Достроено").empty());
  // Снова суша — стройка продолжается и завершается.
  f.tx([&](Tx& tx) { setProvinceSea(tx, f.p[0], false); });
  CHECK(!f.w().province(f.p[0])->sea);
  for (int i = 0; i < 2; i++) f.tx([&](Tx& tx) { endTurn(tx); });
  CHECK(s.pb() && !s.pb()->constructing);
  CHECK(s.pb()->paid.empty() && s.pb()->payer == 0);
  // Сырая отметка «море» (без правила) тоже не продвигает стройку.
  f.tx([&](Tx& tx) {
    startBuilding(tx, f.p[0], s.b);  // уровень 2
    tx.province(f.p[0]).sea = true;
  });
  for (int i = 0; i < 4; i++) f.tx([&](Tx& tx) { endTurn(tx); });
  CHECK(s.pb()->constructing && s.pb()->left == 2);
}

// ---------------------------------------------------------------- 8. возврат ровно уплаченного и плательщику
TEST(rules_fix_refund_exact_paid) {
  {
    BuildFix s;  // бесплатная стройка → после снятия скидки отмена ничего не возвращает
    Id m = s.costMod(-100);
    const double before = stockSum(s.f.w(), s.f.A);
    s.f.tx([&](Tx& tx) { startBuilding(tx, s.f.p[0], s.b); });
    CHECK_NEAR(stockSum(s.f.w(), s.f.A), before, 1e-9);
    CHECK(s.pb()->paid.empty());
    CHECK_EQ(s.pb()->payer, s.f.A);
    s.f.tx([&](Tx& tx) {
      auto& v = tx.province(s.f.p[0]).modifiers;
      v.erase(std::remove(v.begin(), v.end(), m), v.end());
    });
    s.f.tx([&](Tx& tx) { cancelBuilding(tx, s.f.p[0], s.b); });
    CHECK_NEAR(stockSum(s.f.w(), s.f.A), before, 1e-9);
  }
  {
    BuildFix s;  // полная цена → скидка −50 % после начала → возврат полный
    const double before = stockSum(s.f.w(), s.f.A);
    s.f.tx([&](Tx& tx) { startBuilding(tx, s.f.p[0], s.b); });
    CHECK_NEAR(before - stockSum(s.f.w(), s.f.A), 130, 1e-9);
    CHECK_NEAR(s.pb()->paid.at(kGold), 100, 1e-9);
    s.costMod(-50);
    s.f.tx([&](Tx& tx) { cancelBuilding(tx, s.f.p[0], s.b); });
    CHECK_NEAR(stockSum(s.f.w(), s.f.A), before, 1e-9);
  }
  {
    BuildFix s;  // цена уровня изменена в дереве во время стройки → возвращается уплаченное
    const double before = stockSum(s.f.w(), s.f.A);
    s.f.tx([&](Tx& tx) { startBuilding(tx, s.f.p[0], s.b); });
    s.f.tx([&](Tx& tx) { tx.building(s.b).levels[0].cost[kGold] = 10000; });
    s.f.tx([&](Tx& tx) { cancelBuilding(tx, s.f.p[0], s.b); });
    CHECK_NEAR(stockSum(s.f.w(), s.f.A), before, 1e-9);
  }
  {
    BuildFix s;  // провинция сменила владельца: стройка продолжается, возврат — прежнему владельцу (плательщику)
    const double a0 = stockSum(s.f.w(), s.f.A);
    s.f.tx([&](Tx& tx) { startBuilding(tx, s.f.p[0], s.b); });
    s.f.tx([&](Tx& tx) { setProvinceOwner(tx, s.f.p[0], s.f.B); });
    CHECK(s.pb() && s.pb()->constructing);
    CHECK_EQ(s.pb()->payer, s.f.A);
    const double b0 = stockSum(s.f.w(), s.f.B);
    s.f.tx([&](Tx& tx) { cancelBuilding(tx, s.f.p[0], s.b); });
    CHECK_NEAR(stockSum(s.f.w(), s.f.B), b0, 1e-9);
    CHECK_NEAR(stockSum(s.f.w(), s.f.A), a0, 1e-9);
    CHECK(has(lastLog(s.f.w()), "возвращена «Арден»"));
  }
  {
    BuildFix s;  // постройка удалена из дерева — плательщику уплаченное; плательщик упразднён — никому
    const double a0 = stockSum(s.f.w(), s.f.A);
    s.f.tx([&](Tx& tx) { startBuilding(tx, s.f.p[0], s.b); });
    s.f.tx([&](Tx& tx) { tx.building(s.b).levels[0].cost[kGold] = 5; });
    s.f.tx([&](Tx& tx) { removeBuilding(tx, s.b); });
    CHECK_NEAR(stockSum(s.f.w(), s.f.A), a0, 1e-9);
  }
  {
    BuildFix s;
    s.f.tx([&](Tx& tx) { startBuilding(tx, s.f.p[0], s.b); });
    s.f.tx([&](Tx& tx) { setProvinceOwner(tx, s.f.p[0], s.f.B); });
    s.f.tx([&](Tx& tx) { removeFaction(tx, s.f.A); });
    CHECK_EQ(s.pb()->payer, Id(0));
    const double b0 = stockSum(s.f.w(), s.f.B);
    s.f.tx([&](Tx& tx) { cancelBuilding(tx, s.f.p[0], s.b); });
    CHECK_NEAR(stockSum(s.f.w(), s.f.B), b0, 1e-9);
  }
  {
    BuildFix s;  // удаление провинции возвращает уплаченное за начатое строительство
    const double a0 = stockSum(s.f.w(), s.f.A);
    s.f.tx([&](Tx& tx) { startBuilding(tx, s.f.p[0], s.b); });
    s.f.tx([&](Tx& tx) { deleteProvince(tx, s.f.p[0]); });
    CHECK_NEAR(stockSum(s.f.w(), s.f.A), a0, 1e-9);
    CHECK(has(lastLog(s.f.w()), "возвращена"));
  }
}

// ---------------------------------------------------------------- 9. удаление последнего уровня постройки
TEST(rules_fix_remove_last_level_lowers_requirements) {
  Fix f;
  Id req = 0, dep = 0, other = 0;
  f.tx([&](Tx& tx) {
    req = createBuilding(tx, 0, "Стены");
    tx.building(req).levels.resize(3);
    dep = createBuilding(tx, 0, "Башня");
    tx.building(dep).requires_ = {BuildingReq{req, 3}};
    other = createBuilding(tx, 0, "Ров");
    tx.building(other).requires_ = {BuildingReq{req, 1}};
  });
  std::vector<Id> lowered;
  f.tx([&](Tx& tx) { lowered = removeLastBuildingLevel(tx, req); });
  CHECK_EQ(int(f.w().building(req)->levels.size()), 2);
  CHECK_EQ(f.w().building(dep)->requires_[0].level, 2);
  CHECK_EQ(f.w().building(other)->requires_[0].level, 1);
  CHECK(lowered == std::vector<Id>{dep});
  // Построенный уровень удалить нельзя; единственный — тоже.
  f.tx([&](Tx& tx) { tx.province(f.p[0]).buildings.push_back(ProvBuilding{req, 2, false, 0}); });
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { removeLastBuildingLevel(tx, req); }); }), "уже построен"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { removeLastBuildingLevel(tx, other); }); }), "хотя бы один уровень"));
}

// ---------------------------------------------------------------- 10. условие изученной технологии
TEST(rules_fix_studied_tech_rejects_unstudied_prereq) {
  Fix f;
  Id a = 0, b = 0;
  f.tx([&](Tx& tx) {
    a = createTech(tx, f.A, "Обработка железа");
    b = createTech(tx, f.A, "Мореходство");
    setStudied(tx, a, true);
  });
  World before = f.w();
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setPrereq(tx, a, b, true); }); }), "не может зависеть от неизученной"));
  CHECK(World::diff(before, f.w()) == 0);
  // Изученное условие — можно; к неизученной технологии — тоже.
  f.tx([&](Tx& tx) { setPrereq(tx, b, a, true); });
  CHECK(contains(f.w().tech(b)->prereqs, a));
}

// ---------------------------------------------------------------- 11. провинции без области удаляются
TEST(rules_fix_area_edits_remove_emptied_provinces) {
  Fix f;
  f.tx([&](Tx& tx) {
    for (int i = 0; i < 8; i++) tx.province(f.p[i]).owner = f.A;
    tx.province(f.p[0]).baseTrade = 500;
    setCapital(tx, f.A, f.p[0]);
  });
  // Вырезать всю провинцию p0 — запись удаляется, столица снимается, доход не считается.
  AreaEdit r;
  f.tx([&](Tx& tx) { r = removeArea(tx, f.p[0], rect(40, 40, 310, 310)); });
  CHECK(!f.w().province(f.p[0]));
  CHECK_EQ(r.province, Id(0));
  CHECK(r.removed == std::vector<std::string>{"П0"});
  CHECK_EQ(f.w().faction(f.A)->capital, Id(0));
  CHECK(!contains(f.fc(f.A).provinces, f.p[0]));
  CHECK(has(lastLog(f.w()), "не осталось её области"));
  // Новая провинция целиком поверх p1.
  f.tx([&](Tx& tx) { r = createProvince(tx, rect(290, 40, 510, 310), Terrain::Land); });
  CHECK(r.province != 0 && f.w().province(r.province));
  CHECK(!f.w().province(f.p[1]));
  CHECK(r.removed == std::vector<std::string>{"П1"});
  // Расширение p2 на всю p3.
  f.tx([&](Tx& tx) { r = addArea(tx, f.p[2], rect(690, 40, 960, 310)); });
  CHECK(!f.w().province(f.p[3]));
  CHECK_EQ(r.province, f.p[2]);
  CHECK(r.removed == std::vector<std::string>{"П3"});
  // Заливка: область p5 отдана p4.
  f.tx([&](Tx& tx) { r = fillAt(tx, center(5), f.p[4]); });
  CHECK(!f.w().province(f.p[5]));
  CHECK(r.removed == std::vector<std::string>{"П5"});
  // Частичная правка никого не удаляет.
  f.tx([&](Tx& tx) { r = removeArea(tx, f.p[6], rect(560, 360, 640, 440)); });
  CHECK(f.w().province(f.p[6]) && r.removed.empty());
}

// ---------------------------------------------------------------- 12a–b. союзное войско и война
TEST(rules_fix_war_dissolves_allied_army) {
  Fix f;
  Id rA = 0, rB = 0, al = 0, b2 = 0;
  f.tx([&](Tx& tx) {
    setRelation(tx, f.A, f.B, 60, RelStatus::Alliance);
    rA = addArmyRow(tx, f.A, UnitType::LightInf, "", 100, 1);
    rB = addArmyRow(tx, f.B, UnitType::LightInf, "", 100, 1);
    al = createArmy(tx, ArmyKind::Army, f.A, center(1));
    setUnits(tx, al, f.A, rA, 50);
    Id x = createArmy(tx, ArmyKind::Army, f.B, center(1) + Vec2(80, 0));
    setUnits(tx, x, f.B, rB, 40);
    formAllied(tx, al, x);
    b2 = createArmy(tx, ArmyKind::Army, f.B, center(6));
    setUnits(tx, b2, f.B, rB, 30);
  });
  CHECK(f.w().army(al)->allied());
  // Войско B на союзном {A, B}: отряды складываются в группу B (не битва и не новый союз).
  Encounter e = encounter(f.w(), b2, f.w().army(al)->pos);
  CHECK(e.type == EncounterType::Merge);
  f.tx([&](Tx& tx) { mergeArmies(tx, al, b2); });
  CHECK(!f.w().army(b2));
  CHECK_EQ(unitsOf(f.w(), al), 120);
  // Война между участниками распускает союзное войско на отдельные объекты.
  const u32 armies0 = f.w().armies.size();
  f.tx([&](Tx& tx) { setRelation(tx, f.A, f.B, -60, RelStatus::War); });
  CHECK(!f.w().army(al)->allied());
  CHECK_EQ(f.w().armies.size(), armies0 + 1);
  CHECK(has(lastLog(f.w()), "распущено"));
  // То же при объявлении войны (declareWar) после нового союза.
  f.tx([&](Tx& tx) { setRelation(tx, f.A, f.B, 50, RelStatus::Alliance); });
  Id other = 0;
  f.w().armies.each([&](const Army& a) {
    if (a.id != al && a.leader() == f.B) other = a.id;
  });
  CHECK(other != 0);
  f.tx([&](Tx& tx) {
    tx.army(other).pos = f.w().army(al)->pos + Vec2(80, 0);
    formAllied(tx, al, other);
  });
  CHECK(f.w().army(al)->allied());
  f.tx([&](Tx& tx) { declareWar(tx, f.B, f.A); });
  CHECK(!f.w().army(al)->allied());
}

TEST(rules_fix_no_battle_against_own_group) {
  Fix f;
  Id rA = 0, rB = 0, mixed = 0, x = 0;
  f.tx([&](Tx& tx) {
    rA = addArmyRow(tx, f.A, UnitType::LightInf, "", 100, 1);
    rB = addArmyRow(tx, f.B, UnitType::LightInf, "", 100, 1);
    setRelation(tx, f.A, f.B, -60, RelStatus::War);
    setRelation(tx, f.A, f.C, 60, RelStatus::Alliance);
    // Повреждённые данные: в одном объекте группы воюющих A и B.
    Army a;
    a.name = "Смешанное";
    a.pos = center(1);
    a.groups = {ArmyGroup{f.A, {ArmyUnit{rA, 30}}, {}}, ArmyGroup{f.B, {ArmyUnit{rB, 30}}, {}}};
    mixed = tx.add(std::move(a)).id;
    x = createArmy(tx, ArmyKind::Army, f.B, center(6));
    setUnits(tx, x, f.B, rB, 20);
  });
  // B на объект с группой B — не битва.
  Encounter e = encounter(f.w(), x, f.w().army(mixed)->pos);
  CHECK(e.type != EncounterType::Battle);
  // Частичное совпадение составов при войне — отказ с причиной.
  Id y = 0;
  f.tx([&](Tx& tx) {
    Id rC = addArmyRow(tx, f.C, UnitType::LightInf, "", 100, 1);
    Army a;
    a.name = "Союз A и C";
    a.pos = center(7);
    a.groups = {ArmyGroup{f.A, {ArmyUnit{rA, 10}}, {}}, ArmyGroup{f.C, {ArmyUnit{rC, 10}}, {}}};
    y = tx.add(std::move(a)).id;
  });
  e = encounter(f.w(), y, f.w().army(mixed)->pos);
  CHECK(e.type == EncounterType::Blocked);
  CHECK(has(e.reason, "в обоих объектах") || has(e.reason, "В обоих объектах"));
  // Битва, где фракция есть в обоих объектах, правилами не принимается.
  BattleResult r;
  r.attacker = x;
  r.defender = mixed;
  r.attackerWins = true;
  r.attackerOrigin = f.w().army(x)->pos;
  r.losses[mixed][{f.B, rB}] = 10;
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { resolveBattle(tx, r); }); }), "сама с собой"));
}

// ---------------------------------------------------------------- 12c. государственная гильдия в союзе со своим государством
TEST(rules_fix_state_guild_allied_with_state) {
  Fix f;
  Id g = 0, ga = 0, sa = 0;
  f.tx([&](Tx& tx) { g = createStateGuild(tx, f.A, ""); });
  Relation rel = f.w().relation(f.A, g);
  CHECK(rel.s == RelStatus::Alliance);
  CHECK_NEAR(rel.v, 100, 1e-9);
  f.tx([&](Tx& tx) {
    Id rg = addArmyRow(tx, g, UnitType::Ranged, "", 100, 1);
    Id ra = addArmyRow(tx, f.A, UnitType::LightInf, "", 100, 1);
    ga = createArmy(tx, ArmyKind::Army, g, center(5));
    setUnits(tx, ga, g, rg, 50);
    sa = createArmy(tx, ArmyKind::Army, f.A, center(6));
    setUnits(tx, sa, f.A, ra, 50);
  });
  Encounter e = encounter(f.w(), ga, f.w().army(sa)->pos);
  CHECK(e.type == EncounterType::Alliance);
}
