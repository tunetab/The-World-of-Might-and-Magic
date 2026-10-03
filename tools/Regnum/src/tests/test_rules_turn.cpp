// Тесты rules: завершение хода по шагам RULES.md §4 — казна и долг, добыча, сделки «каждый ход» и недостача,
// дань, строительство и исследования, население и довольство, дипломатия, восстания, итог, предпросмотр, отмена.
#include "tests/test_rules_util.h"

using namespace rg;
using namespace rg::rules;
using namespace rg::rulestest;

namespace {

TurnReport turn(Fix& f) {
  return f.s.transact("Завершить ход", [](Tx& tx) { return endTurn(tx); });
}

const TurnFactionLine* lineOf(const TurnReport& r, Id f) {
  for (auto& l : r.factions)
    if (l.faction == f) return &l;
  return nullptr;
}

// Сравнение миров по содержимому (без времени записей хроники).
bool sameContent(const World& a, const World& b) {
  if (a.turn() != b.turn() || *a.relations != *b.relations) return false;
  bool ok = true;
  a.factions.each([&](const Faction& x) {
    const Faction* y = b.faction(x.id);
    ok = ok && y && x.res == y->res;
  });
  a.provinces.each([&](const Province& x) {
    const Province* y = b.province(x.id);
    if (!y || x.contentment != y->contentment || x.races.size() != y->races.size()) { ok = false; return; }
    for (size_t i = 0; i < x.races.size(); i++) ok = ok && x.races[i].pop == y->races[i].pop;
    ok = ok && x.buildings.size() == y->buildings.size();
  });
  a.deals.each([&](const Deal& x) {
    const Deal* y = b.deal(x.id);
    ok = ok && y && x.status == y->status && x.items.size() == y->items.size();
    if (ok)
      for (size_t i = 0; i < x.items.size(); i++) ok = ok && x.items[i].left == y->items[i].left;
  });
  a.techs.each([&](const Tech& x) {
    const Tech* y = b.tech(x.id);
    ok = ok && y && x.studied == y->studied && x.progress == y->progress;
  });
  std::vector<std::string> la, lb;
  a.log.each([&](const LogEntry& e) { la.push_back(e.text); });
  b.log.each([&](const LogEntry& e) { lb.push_back(e.text); });
  return ok && la == lb;
}

}  // namespace

TEST(rules_turn_treasury_resources_debt) {
  Fix f;
  f.tx([&](Tx& tx) {
    Province& p = tx.province(f.p[0]);
    p.owner = f.A;
    p.baseTrade = 200;  // налог 10 % → 20
    p.resource = 5;
    p.resourceAmount = 20;
    tx.faction(f.A).res[kGold] = 100;
    addArmyRow(tx, f.A, UnitType::LightInf, "", 5, 2);  // содержание 10
    Id treasurer = createCharacter(tx, f.A, "Казначей");  // герой фракции — входит в «специалисты»
    tx.character(treasurer).upkeep = 3;
    tx.character(treasurer).hero = true;
    tx.faction(f.B).res[kGold] = 20;
    Id envoy = createCharacter(tx, f.B, "Посол");
    tx.character(envoy).upkeep = 50;
    tx.faction(f.B).ruler = envoy;
  });
  TurnReport r = turn(f);
  CHECK_EQ(r.turnFrom, 1);
  CHECK_EQ(r.turnTo, 2);
  CHECK_EQ(f.w().turn(), 2);
  CHECK_NEAR(f.w().faction(f.A)->treasury(), 107, 1e-9);
  CHECK_NEAR(f.w().faction(f.A)->stock(5), 20, 1e-9);
  const TurnFactionLine* a = lineOf(r, f.A);
  CHECK(a != nullptr);
  CHECK_NEAR(a->treasuryBefore, 100, 1e-9);
  CHECK_NEAR(a->treasuryAfter, 107, 1e-9);
  CHECK_NEAR(a->income, 20, 1e-9);
  CHECK_NEAR(a->expenses, 13, 1e-9);
  CHECK_NEAR(a->resources.at(5), 20, 1e-9);
  // Казна может уйти в минус — отметка в хронике.
  CHECK_NEAR(f.w().faction(f.B)->treasury(), -30, 1e-9);
  CHECK(has(lastLog(f.w(), "долг"), "Казна «Бельмар» ушла в долг: −30"));
  turn(f);
  CHECK_NEAR(f.w().faction(f.B)->treasury(), -80, 1e-9);
  CHECK(has(lastLog(f.w(), "Долг"), "Долг казны «Бельмар»: −80"));
  CHECK(has(lastLog(f.w(), "Завершён"), "Завершён ход 2"));
  // Записи шагов хода относятся к завершаемому ходу.
  const LogEntry* last = nullptr;
  f.w().log.each([&](const LogEntry& e) { last = &e; });
  CHECK_EQ(last->turn, 2);
  CHECK(last->kind == LogKind::Turn);
  CHECK(std::find(r.logIds.begin(), r.logIds.end(), Id(0)) == r.logIds.end());
}

TEST(rules_turn_occupied_income_recipient) {
  Fix f;
  f.tx([&](Tx& tx) {
    Province& p = tx.province(f.p[0]);
    p.owner = f.A;
    p.baseTrade = 100;
    p.resource = 2;
    p.resourceAmount = 10;
    setOccupied(tx, f.p[0], f.B);
    tx.settings().occupiedIncome = OccupiedIncome::Occupier;
  });
  turn(f);
  CHECK_NEAR(f.w().faction(f.A)->treasury(), 0, 1e-9);
  CHECK_NEAR(f.w().faction(f.B)->treasury(), 10, 1e-9);
  CHECK_NEAR(f.w().faction(f.B)->stock(2), 10, 1e-9);  // добыча — тому же получателю
  f.tx([&](Tx& tx) { tx.settings().occupiedIncome = OccupiedIncome::None; });
  turn(f);
  CHECK_NEAR(f.w().faction(f.A)->treasury() + f.w().faction(f.B)->treasury(), 10, 1e-9);
  CHECK_NEAR(f.w().faction(f.A)->stock(2) + f.w().faction(f.B)->stock(2), 10, 1e-9);
}

TEST(rules_turn_per_turn_deal_lifecycle_and_shortfall) {
  Fix f;
  Id d = 0;
  f.tx([&](Tx& tx) {
    tx.faction(f.A).res[5] = 15;
    Deal x;
    x.a = f.A;
    x.b = f.B;
    x.items = {DealItem{DealSide::A, 5, 10, DealMode::PerTurn, 2, 0}, DealItem{DealSide::B, kGold, 5, DealMode::PerTurn, 2, 0}};
    d = concludeDeal(tx, x);
  });
  turn(f);
  CHECK_NEAR(f.w().faction(f.A)->stock(5), 5, 1e-9);
  CHECK_NEAR(f.w().faction(f.B)->stock(5), 10, 1e-9);
  CHECK_NEAR(f.w().faction(f.A)->treasury(), 5, 1e-9);   // золото — через чистый доход шага 2
  CHECK_NEAR(f.w().faction(f.B)->treasury(), -5, 1e-9);
  CHECK_EQ(f.w().deal(d)->items[0].left, 1);
  CHECK(f.w().deal(d)->status == DealStatus::Active);
  turn(f);
  CHECK_NEAR(f.w().faction(f.A)->stock(5), 0, 1e-9);     // передано доступное: 5 из 10
  CHECK_NEAR(f.w().faction(f.B)->stock(5), 15, 1e-9);
  CHECK(has(lastLog(f.w(), "Недостача"), "«Арден» → «Бельмар», Железо 5 из 10"));
  CHECK(f.w().deal(d)->status == DealStatus::Done);
  CHECK_EQ(f.w().deal(d)->items[0].left, 0);
  CHECK(has(lastLog(f.w(), "Выполнена"), "Выполнена сделка «Арден» и «Бельмар»"));
  CHECK_NEAR(f.w().faction(f.A)->treasury(), 10, 1e-9);
  turn(f);  // завершённая сделка больше не действует
  CHECK_NEAR(f.w().faction(f.A)->treasury(), 10, 1e-9);
  CHECK_NEAR(f.w().faction(f.B)->stock(5), 15, 1e-9);
}

TEST(rules_turn_tribute_lifecycle) {
  Fix f;
  Id d = 0;
  f.tx([&](Tx& tx) {
    tx.faction(f.B).res[kGold] = 100;
    d = imposeTribute(tx, DealKind::Tribute, f.A, f.B, 30, 2);
  });
  turn(f);
  CHECK_NEAR(f.w().faction(f.B)->treasury(), 70, 1e-9);
  CHECK_NEAR(f.w().faction(f.A)->treasury(), 30, 1e-9);
  turn(f);
  CHECK_NEAR(f.w().faction(f.B)->treasury(), 40, 1e-9);
  CHECK_NEAR(f.w().faction(f.A)->treasury(), 60, 1e-9);
  CHECK(f.w().deal(d)->status == DealStatus::Done);
  CHECK(has(lastLog(f.w(), "Завершена"), "Завершена выплата дани: «Бельмар» → «Арден»"));
  turn(f);
  CHECK_NEAR(f.w().faction(f.B)->treasury(), 40, 1e-9);
  CHECK_NEAR(f.w().faction(f.A)->treasury(), 60, 1e-9);
}

TEST(rules_turn_construction_effects_next_turn) {
  Fix f;
  Id b = 0;
  f.tx([&](Tx& tx) {
    tx.province(f.p[0]).owner = f.A;
    tx.province(f.p[0]).baseTrade = 100;
    b = createBuilding(tx, 0, "Рынок");
    tx.building(b).levels[0].turns = 1;
    tx.building(b).levels[0].modifiers = {makeMod(tx, {{Fx::IncomePct, 50}})};
    startBuilding(tx, f.p[0], b);
  });
  TurnReport r = turn(f);
  CHECK(!f.w().province(f.p[0])->buildings[0].constructing);
  CHECK(has(lastLog(f.w(), "Достроено"), "Достроено: «Рынок» в провинции «П0»"));
  CHECK_NEAR(f.w().faction(f.A)->treasury(), 10, 1e-9);  // бонус ещё не действовал в этом ходу
  CHECK_NEAR(lineOf(r, f.A)->income, 10, 1e-9);
  turn(f);
  CHECK_NEAR(f.w().faction(f.A)->treasury(), 25, 1e-9);  // со следующего расчёта: 10 × 1,5
  CHECK(has(lastLog(f.w(), "Завершён ход 1"), "Построек достроено: 1"));
}

TEST(rules_turn_research_lifecycle) {
  Fix f;
  Id t = 0, t1 = 0, t2 = 0;
  f.tx([&](Tx& tx) {
    t = createTech(tx, f.A, "Астрономия");
    tx.tech(t).turns = 3;
    startResearch(tx, t);
    t1 = createTech(tx, f.A, "Основа");
    t2 = createTech(tx, f.A, "Надстройка");
    setPrereq(tx, t2, t1, true);
    tx.tech(t2).research = true;  // повреждённое состояние: исследование без изученных условий
  });
  turn(f);
  turn(f);
  CHECK_EQ(f.w().tech(t)->progress, 2);
  CHECK(!f.w().tech(t)->studied);
  CHECK(!f.w().tech(t2)->research);
  CHECK(has(lastLog(f.w(), "остановлено"), "«Надстройка» остановлено"));
  turn(f);
  CHECK(f.w().tech(t)->studied);
  CHECK(!f.w().tech(t)->research);
  CHECK_EQ(f.w().tech(t)->progress, 3);
  CHECK(has(lastLog(f.w(), "изучена"), "«Арден»: изучена технология «Астрономия»"));
}

TEST(rules_turn_population_and_contentment) {
  Fix f;
  Id human = 0, elf = 0;
  f.tx([&](Tx& tx) {
    human = addCatalogItem(tx, CatalogList::Races, "Люди");
    elf = addCatalogItem(tx, CatalogList::Races, "Эльфы");
    Province& p = tx.province(f.p[0]);
    p.races = {RacePop{human, 1000}, RacePop{elf, 333}};
    p.contentment = 95;
    p.modifiers = {makeMod(tx, {{Fx::PopGrowthPct, 10}, {Fx::ContentmentPerTurn, 25}})};
    Province& q = tx.province(f.p[1]);
    q.races = {RacePop{human, 3}};
    q.contentment = -90;
    q.modifiers = {makeMod(tx, {{Fx::PopGrowthPct, -50}, {Fx::ContentmentPerTurn, -25}})};
    Province& s = tx.province(f.seaW);
    s.races = {RacePop{human, 100}};
    s.modifiers = q.modifiers;
    tx.province(f.p[2]).races = {RacePop{elf, 500}};  // без модификаторов — без изменений
  });
  turn(f);
  const World& w = f.w();
  CHECK_EQ(w.province(f.p[0])->races[0].pop, 1100);
  CHECK_EQ(w.province(f.p[0])->races[1].pop, 366);  // 366,3 → 366
  CHECK_NEAR(w.province(f.p[0])->contentment, 100, 1e-9);  // в пределах −100…100
  CHECK_EQ(w.province(f.p[1])->races[0].pop, 2);    // 1,5 → 2
  CHECK_NEAR(w.province(f.p[1])->contentment, -100, 1e-9);
  CHECK_EQ(w.province(f.seaW)->races[0].pop, 100);  // морская провинция не участвует
  CHECK_EQ(w.province(f.p[2])->races[0].pop, 500);
  turn(f);
  CHECK_EQ(f.w().province(f.p[0])->races[0].pop, 1210);
  CHECK_EQ(f.w().province(f.p[1])->races[0].pop, 1);
}

TEST(rules_turn_diplomacy_per_turn) {
  Fix f;
  f.tx([&](Tx& tx) {
    tx.faction(f.A).modifiers = {makeMod(tx, {{Fx::DiplomacyPerTurn, 10}}, {f.B})};
    tx.faction(f.B).modifiers = {makeMod(tx, {{Fx::DiplomacyPerTurn, 5}}, {f.A})};
    tx.faction(f.C).modifiers = {makeMod(tx, {{Fx::DiplomacyPerTurn, -25}}, {f.A})};
    setRelation(tx, f.A, f.C, -80, RelStatus::Neutral);
  });
  turn(f);
  CHECK_NEAR(f.w().relation(f.A, f.B).v, 15, 1e-9);
  CHECK(f.w().relation(f.A, f.B).s == RelStatus::Unknown);  // состояние не меняется
  CHECK_NEAR(f.w().relation(f.A, f.C).v, -100, 1e-9);       // в пределах −100…100
  CHECK(f.w().relation(f.A, f.C).s == RelStatus::Neutral);
  f.tx([&](Tx& tx) { setRelation(tx, f.A, f.B, 95, RelStatus::Alliance); });
  turn(f);
  CHECK_NEAR(f.w().relation(f.A, f.B).v, 100, 1e-9);
}

TEST(rules_turn_rebellion_roll) {
  Fix f;
  f.tx([&](Tx& tx) {
    for (int i = 0; i < 8; i++) {
      tx.province(f.p[i]).owner = f.A;
      tx.province(f.p[i]).contentment = -100;  // 50 %
    }
    tx.province(f.p[0]).modifiers = {makeMod(tx, {{Fx::RebellionPct, 50}})};   // 100 %
    tx.province(f.p[1]).contentment = 100;                                      // 0 %
    tx.province(f.p[2]).owner = 0;                                              // без владельца — не бросаем
    tx.province(f.p[2]).modifiers = tx.province(f.p[0]).modifiers;
  });
  CHECK(previewTurn(f.w()).rebellions.empty());  // выключено в настройках
  f.tx([&](Tx& tx) { tx.settings().rebellionRoll = true; });
  TurnReport a = previewTurn(f.w());
  TurnReport b = previewTurn(f.w());
  CHECK(a.rebellions == b.rebellions);  // зерно «ход + провинция»
  CHECK(std::find(a.rebellions.begin(), a.rebellions.end(), f.p[0]) != a.rebellions.end());
  CHECK(std::find(a.rebellions.begin(), a.rebellions.end(), f.p[1]) == a.rebellions.end());
  CHECK(std::find(a.rebellions.begin(), a.rebellions.end(), f.p[2]) == a.rebellions.end());
  TurnReport real = turn(f);
  CHECK(real.rebellions == a.rebellions);
  CHECK(has(lastLog(f.w(), "Восстание"), "(вероятность "));
  // Частота при 50 % — около половины на многих ходах.
  int n = 0, hit = 0;
  for (int t = 1; t <= 300; t++) {
    Tx tx(f.w());
    tx.meta().turn = t;
    World w = std::move(tx).finish();
    TurnReport r = previewTurn(w);
    for (int i = 3; i < 8; i++) {
      n++;
      hit += std::find(r.rebellions.begin(), r.rebellions.end(), f.p[i]) != r.rebellions.end();
    }
  }
  double share = double(hit) / n;
  CHECK_MSG(share > 0.44 && share < 0.56, std::to_string(share));
}

TEST(rules_turn_preview_and_determinism) {
  Fix f;
  f.tx([&](Tx& tx) {
    tx.province(f.p[0]).owner = f.A;
    tx.province(f.p[0]).baseTrade = 120;
    tx.province(f.p[0]).resource = 4;
    tx.province(f.p[0]).resourceAmount = 9;
    tx.faction(f.A).res[4] = 3;
    Deal d;
    d.a = f.A;
    d.b = f.C;
    d.items = {DealItem{DealSide::A, 4, 5, DealMode::PerTurn, 3, 0}};
    concludeDeal(tx, d);
    imposeTribute(tx, DealKind::Reparations, f.B, f.A, 4, 5);
    tx.settings().rebellionRoll = true;
    tx.province(f.p[3]).owner = f.B;
    tx.province(f.p[3]).contentment = -60;
  });
  World start = f.w();
  u64 ver = f.s.version();
  TurnReport p = previewTurn(f.w());
  CHECK_EQ(World::diff(start, f.w()), 0u);  // предпросмотр не меняет мир
  CHECK_EQ(f.s.version(), ver);
  TurnReport r = turn(f);
  CHECK_EQ(p.factions.size(), r.factions.size());
  for (size_t i = 0; i < p.factions.size(); i++) {
    CHECK_EQ(p.factions[i].faction, r.factions[i].faction);
    CHECK_NEAR(p.factions[i].treasuryAfter, r.factions[i].treasuryAfter, 1e-12);
    CHECK(p.factions[i].resources == r.factions[i].resources);
  }
  CHECK(p.rebellions == r.rebellions);
  CHECK_EQ(p.logIds.size(), r.logIds.size());
  // Тот же мир — тот же итог.
  Store other;
  other.replace(start, "copy");
  other.transact("t", [](Tx& tx) { endTurn(tx); });
  CHECK(sameContent(f.w(), other.world()));
  // Отмена возвращает мир до завершения хода.
  CHECK(f.s.undo());
  CHECK_EQ(World::diff(start, f.w()), 0u);
  CHECK_EQ(f.w().turn(), 1);
}
