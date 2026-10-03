// Тесты rules: дипломатия, торговля и дань, строительство, технологии, гильдии, торговые маршруты.
#include "tests/test_rules_util.h"

using namespace rg;
using namespace rg::rules;
using namespace rg::rulestest;

namespace {

const ProvBuilding* pbOf(const World& w, Id province, Id building) {
  for (auto& b : w.province(province)->buildings)
    if (b.building == building) return &b;
  return nullptr;
}

const BuildOption* opt(const std::vector<BuildOption>& v, Id b) {
  for (auto& o : v)
    if (o.building == b) return &o;
  return nullptr;
}

bool hasReason(const BuildOption* o, const std::string& sub) {
  if (!o) return false;
  for (auto& r : o->reasons)
    if (has(r, sub)) return true;
  return false;
}

}  // namespace

// ---------------------------------------------------------------- дипломатия
TEST(rules_diplomacy_relations) {
  Fix f;
  f.tx([&](Tx& tx) { setRelation(tx, f.A, f.B, 40, RelStatus::Alliance); });
  CHECK(f.w().relation(f.A, f.B) == (Relation{40, RelStatus::Alliance}));
  CHECK(f.w().relation(f.B, f.A) == (Relation{40, RelStatus::Alliance}));  // симметрично
  CHECK(has(lastLog(f.w()), "Отношения «Арден» и «Бельмар»: в союзе"));
  int logs = int(f.w().log.size());
  f.tx([&](Tx& tx) { setRelation(tx, f.B, f.A, 150, RelStatus::Alliance); });
  CHECK_NEAR(f.w().relation(f.A, f.B).v, 100, 1e-9);
  CHECK_EQ(int(f.w().log.size()), logs);  // значение без смены состояния — не событие хроники
  f.tx([&](Tx& tx) { setRelation(tx, f.A, f.B, -60, RelStatus::War); });
  CHECK_EQ(logCount(f.w(), LogKind::War), 1);
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setRelation(tx, f.A, f.A, 0, RelStatus::Neutral); }); }), "самой собой"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setRelation(tx, f.A, 999, 0, RelStatus::Neutral); }); }), "не найдена"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setRelation(tx, f.A, f.B, std::nan(""), RelStatus::Neutral); }); }), "недопустимое число"));
  auto rows = relationsOf(f.w(), f.A);
  CHECK_EQ(rows.size(), size_t(4));  // все прочие фракции
  CHECK_EQ(rows[0].other, f.B);      // сначала государства по алфавиту, затем гильдии
  CHECK_EQ(rows[1].other, f.C);
  CHECK_EQ(rows[2].other, f.G);
  CHECK_EQ(rows[3].other, f.H);
  CHECK(rows[0].status == RelStatus::War);
  CHECK_NEAR(rows[0].value, -60, 1e-9);
  CHECK(rows[1].status == RelStatus::Unknown);  // нет записи — 0 и «незнакомы»
  CHECK_NEAR(rows[1].value, 0, 1e-9);
  CHECK(relationsOf(f.w(), 999).empty());
}

// ---------------------------------------------------------------- торговля
TEST(rules_trade_validate) {
  Fix f;
  f.tx([&](Tx& tx) { tx.faction(f.A).res[5] = 12; tx.faction(f.A).res[3] = 15; });
  auto problems = [&](const Deal& d) { return join(validateDeal(f.w(), d).problems, " | "); };
  Deal d;
  CHECK(!validateDeal(f.w(), d).ok);
  CHECK(has(problems(d), "Не выбрана сторона сделки"));
  CHECK(has(problems(d), "нет ни одной позиции"));
  d.a = f.A;
  d.b = f.A;
  CHECK(has(problems(d), "Стороны сделки совпадают"));
  d.b = f.B;
  d.items = {DealItem{DealSide::A, 5, 40, DealMode::Once, 1, 0}};
  CHECK(has(problems(d), "Недостаточно ресурса «Железо» у «Арден»: нужно 40, есть 12"));
  d.items = {DealItem{DealSide::A, 3, 10, DealMode::Once, 1, 0}, DealItem{DealSide::A, 3, 10, DealMode::Once, 1, 0}};
  CHECK(has(problems(d), "нужно 20, есть 15"));  // разовые позиции суммируются
  d.items = {DealItem{DealSide::A, 999, 1, DealMode::Once, 1, 0}};
  CHECK(has(problems(d), "Ресурс позиции не найден"));
  d.items = {DealItem{DealSide::B, kGold, 0, DealMode::Once, 1, 0}};
  CHECK(has(problems(d), "больше нуля"));
  d.items = {DealItem{DealSide::B, kGold, 5, DealMode::PerTurn, 0, 0}};
  CHECK(has(problems(d), "не меньше одного хода"));
  d.items = {DealItem{DealSide::A, 5, 100, DealMode::PerTurn, 3, 0}};  // «каждый ход» не требует запаса сейчас
  CHECK(validateDeal(f.w(), d).ok);
  d.kind = DealKind::Tribute;
  CHECK(has(problems(d), "золотом каждый ход"));
  d.items = {DealItem{DealSide::B, kGold, 10, DealMode::PerTurn, 3, 0}};
  CHECK(validateDeal(f.w(), d).ok);
}

TEST(rules_trade_conclude_once_and_gift) {
  Fix f;
  f.tx([&](Tx& tx) {
    tx.faction(f.A).res[5] = 100;
    tx.faction(f.B).res[kGold] = 50;
  });
  Deal d;
  d.id = 12345;  // ID назначается заново
  d.a = f.A;
  d.b = f.B;
  d.items = {DealItem{DealSide::A, 5, 40, DealMode::Once, 1, 0}, DealItem{DealSide::B, kGold, 30, DealMode::Once, 1, 0}};
  Id id = 0;
  f.tx([&](Tx& tx) { id = concludeDeal(tx, d); });
  const World& w = f.w();
  CHECK(id != 12345);
  CHECK_NEAR(w.faction(f.A)->stock(5), 60, 1e-9);
  CHECK_NEAR(w.faction(f.A)->treasury(), 30, 1e-9);
  CHECK_NEAR(w.faction(f.B)->stock(5), 40, 1e-9);
  CHECK_NEAR(w.faction(f.B)->treasury(), 20, 1e-9);
  CHECK(w.deal(id)->status == DealStatus::Done);
  CHECK_EQ(w.deal(id)->turn, 1);
  CHECK(has(lastLog(w), "Сделка «Арден» и «Бельмар»"));
  Deal g;
  g.a = f.A;
  g.b = f.C;
  g.items = {DealItem{DealSide::A, 5, 10, DealMode::Once, 1, 0}};
  f.tx([&](Tx& tx) { concludeDeal(tx, g); });
  CHECK(has(lastLog(f.w()), "«Арден» дарит «Церис»: Железо 10"));
  CHECK_NEAR(f.w().faction(f.C)->stock(5), 10, 1e-9);
  // Нехватка — сделка не оформляется, мир не меняется.
  World before = f.w();
  g.items[0].amount = 51;
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { concludeDeal(tx, g); }); }), "Недостаточно ресурса"));
  CHECK_EQ(World::diff(before, f.w()), 0u);
}

TEST(rules_trade_per_turn_and_cancel) {
  Fix f;
  Deal d;
  d.a = f.A;
  d.b = f.B;
  d.items = {DealItem{DealSide::A, 5, 5, DealMode::PerTurn, 3, 0}, DealItem{DealSide::B, 3, 2, DealMode::Once, 1, 0}};
  f.tx([&](Tx& tx) { tx.faction(f.B).res[3] = 2; });
  Id id = 0;
  f.tx([&](Tx& tx) { id = concludeDeal(tx, d); });
  const Deal& x = *f.w().deal(id);
  CHECK(x.status == DealStatus::Active);
  CHECK_EQ(x.items[0].left, 3);
  CHECK_EQ(x.items[1].left, 0);
  CHECK_NEAR(f.w().faction(f.A)->stock(5), 0, 1e-9);  // «каждый ход» исполняется при завершении хода
  CHECK_NEAR(f.w().faction(f.A)->stock(3), 2, 1e-9);  // разовая — сразу
  CHECK(has(lastLog(f.w()), "Железо 5 за ход, 3 хода"));
  f.tx([&](Tx& tx) { cancelDeal(tx, id); });
  CHECK(f.w().deal(id)->status == DealStatus::Cancelled);
  CHECK(has(lastLog(f.w()), "Расторгнута сделка"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { cancelDeal(tx, id); }); }), "уже расторгнута"));
  Deal once;
  once.a = f.A;
  once.b = f.B;
  once.items = {DealItem{DealSide::A, kGold, 0.5, DealMode::Once, 1, 0}};
  f.tx([&](Tx& tx) { tx.faction(f.A).res[kGold] = 1; });
  f.tx([&](Tx& tx) { id = concludeDeal(tx, once); });
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { cancelDeal(tx, id); }); }), "уже выполнена"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { cancelDeal(tx, 9999); }); }), "не найдена"));
}

TEST(rules_trade_tribute_and_reparations) {
  Fix f;
  Id t = 0, r = 0;
  f.tx([&](Tx& tx) {
    t = imposeTribute(tx, DealKind::Tribute, f.A, f.B, 50, 10);
    r = imposeTribute(tx, DealKind::Reparations, f.C, f.B, 7.5, 2);
  });
  const Deal& d = *f.w().deal(t);
  CHECK(d.kind == DealKind::Tribute);
  CHECK_EQ(d.a, f.A);
  CHECK_EQ(d.b, f.B);
  CHECK_EQ(d.items.size(), size_t(1));
  CHECK(d.items[0].from == DealSide::B);
  CHECK_EQ(d.items[0].res, kGold);
  CHECK_EQ(d.items[0].left, 10);
  CHECK(d.status == DealStatus::Active);
  CHECK(f.w().deal(r)->kind == DealKind::Reparations);
  CHECK(has(lastLog(f.w()), "«Бельмар» выплачивает «Церис» репарации: 7,5 золота за ход, 2 хода"));
  CHECK_NEAR(f.fc(f.A).incTribute, 50, 1e-9);
  CHECK_NEAR(f.fc(f.C).incTribute, 7.5, 1e-9);
  CHECK_NEAR(f.fc(f.B).expTribute, 57.5, 1e-9);
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { imposeTribute(tx, DealKind::Trade, f.A, f.B, 1, 1); }); }), "дань или репарации"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { imposeTribute(tx, DealKind::Tribute, f.A, f.B, 0, 1); }); }), "больше нуля"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { imposeTribute(tx, DealKind::Tribute, f.A, f.B, 1, 0); }); }), "одного хода"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { imposeTribute(tx, DealKind::Tribute, f.A, f.A, 1, 1); }); }), "самой себе"));
  f.tx([&](Tx& tx) { cancelDeal(tx, t); });
  CHECK(has(lastLog(f.w()), "Отменена дань"));
  CHECK_NEAR(f.fc(f.B).expTribute, 7.5, 1e-9);
}

// ---------------------------------------------------------------- строительство
namespace {

struct BuildSetup {
  Fix f;
  Id farm = 0, smith = 0, exp = 0, uniqA = 0, uniqB = 0, mFarm1 = 0, mFarm2 = 0;
  BuildSetup() {
    f.tx([&](Tx& tx) {
      Province& p = tx.province(f.p[0]);
      p.owner = f.A;
      p.modifiers = {makeMod(tx, {{Fx::BuildCostPct, 50}})};
      Faction& a = tx.faction(f.A);
      a.res[kGold] = 100;
      a.res[5] = 30;
      a.res[3] = 50;
      mFarm1 = makeMod(tx, {{Fx::TradeFlat, 10}});
      mFarm2 = makeMod(tx, {{Fx::TradeFlat, 25}});
      farm = createBuilding(tx, 0, "Ферма");
      Building& fb = tx.building(farm);
      fb.cat = BuildingCat::Economic;
      fb.levels = {BuildingLevel{2, {{3, 20}}, {mFarm1}, ""}, BuildingLevel{3, {{3, 40}}, {mFarm2}, ""}};
      smith = createBuilding(tx, 0, "Кузница");
      tx.building(smith).cat = BuildingCat::Industrial;
      tx.building(smith).requires_ = {BuildingReq{farm, 2}};
      tx.building(smith).levels[0].cost = {{5, 10}};
      exp = createBuilding(tx, 0, "Арсенал");
      tx.building(exp).cat = BuildingCat::Military;
      tx.building(exp).levels[0].cost = {{5, 40}};
      uniqA = createBuilding(tx, f.A, "Храм Ардена");
      uniqB = createBuilding(tx, f.B, "Храм Бельмара");
    });
  }
};

}  // namespace

TEST(rules_build_options) {
  BuildSetup s;
  Fix& f = s.f;
  auto opts = buildOptions(f.w(), f.p[0]);
  CHECK_EQ(opts.size(), size_t(4));  // уникальная постройка другого государства не предлагается
  CHECK(!opt(opts, s.uniqB));
  CHECK_EQ(opts[0].building, s.exp);  // по категориям: военные первыми
  const BuildOption* farm = opt(opts, s.farm);
  CHECK(farm->can);
  CHECK_EQ(farm->level, 1);
  CHECK(!farm->upgrade);
  CHECK_EQ(farm->turns, 2);
  CHECK_NEAR(farm->cost.at(3), 30, 1e-9);  // 20 × 1,5
  CHECK(hasReason(opt(opts, s.smith), "Нужна постройка «Ферма» уровня 2"));
  CHECK(hasReason(opt(opts, s.exp), "Недостаточно ресурса «Железо»: нужно 60, есть 30"));
  CHECK(opt(opts, s.uniqA)->can);
  // Без владельца и в море.
  auto none = buildOptions(f.w(), f.p[1]);
  CHECK(hasReason(opt(none, s.farm), "нет владельца"));
  CHECK(!opt(none, s.uniqA));
  auto sea = buildOptions(f.w(), f.seaW);
  CHECK(hasReason(opt(sea, s.farm), "морской"));
  // Слоты: средняя провинция + деревня = 3.
  f.tx([&](Tx& tx) {
    for (int i = 0; i < 3; i++) {
      Id b = createBuilding(tx, 0, "Заполнитель " + std::to_string(i));
      tx.province(f.p[0]).buildings.push_back(ProvBuilding{b, 1, false, 0});
    }
  });
  opts = buildOptions(f.w(), f.p[0]);
  CHECK(hasReason(opt(opts, s.farm), "Нет свободных слотов: занято 3 из 3"));
  CHECK(buildOptions(f.w(), 9999).empty());
}

TEST(rules_build_lifecycle) {
  BuildSetup s;
  Fix& f = s.f;
  f.tx([&](Tx& tx) { startBuilding(tx, f.p[0], s.farm); });
  CHECK_NEAR(f.w().faction(f.A)->stock(3), 20, 1e-9);
  const ProvBuilding* pb = pbOf(f.w(), f.p[0], s.farm);
  CHECK(pb && pb->constructing && pb->level == 1 && pb->left == 2);
  CHECK(has(lastLog(f.w()), "Начато строительство «Ферма» в провинции «П0»: 2 хода"));
  CHECK_EQ(f.pc(f.p[0]).slotsUsed, 1);
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { startBuilding(tx, f.p[0], s.farm); }); }), "Уже строится"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { demolish(tx, f.p[0], s.farm); }); }), "сначала отмените"));
  // Отмена — полный возврат.
  f.tx([&](Tx& tx) { cancelBuilding(tx, f.p[0], s.farm); });
  CHECK_NEAR(f.w().faction(f.A)->stock(3), 50, 1e-9);
  CHECK(!pbOf(f.w(), f.p[0], s.farm));
  CHECK(has(lastLog(f.w()), "стоимость возвращена"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { cancelBuilding(tx, f.p[0], s.farm); }); }), "нет в провинции"));
  // Строительство завершается через указанное число ходов; бонус — только после завершения.
  f.tx([&](Tx& tx) { startBuilding(tx, f.p[0], s.farm); });
  CHECK_NEAR(provinceEffects(f.w(), f.p[0])[Fx::TradeFlat], 0, 1e-9);
  f.tx([&](Tx& tx) { endTurn(tx); });
  CHECK_EQ(pbOf(f.w(), f.p[0], s.farm)->left, 1);
  f.tx([&](Tx& tx) { endTurn(tx); });
  pb = pbOf(f.w(), f.p[0], s.farm);
  CHECK(!pb->constructing);
  CHECK_EQ(pb->builtLevel(), 1);
  CHECK_NEAR(provinceEffects(f.w(), f.p[0])[Fx::TradeFlat], 10, 1e-9);
  // Улучшение: во время строительства действуют бонусы прежнего уровня.
  f.tx([&](Tx& tx) { tx.faction(f.A).res[3] = 100; });
  auto up = *opt(buildOptions(f.w(), f.p[0]), s.farm);
  CHECK(up.upgrade);
  CHECK_EQ(up.level, 2);
  CHECK_NEAR(up.cost.at(3), 60, 1e-9);
  f.tx([&](Tx& tx) { startBuilding(tx, f.p[0], s.farm); });
  pb = pbOf(f.w(), f.p[0], s.farm);
  CHECK(pb->constructing && pb->level == 2 && pb->left == 3 && pb->builtLevel() == 1);
  CHECK_NEAR(f.w().faction(f.A)->stock(3), 40, 1e-9);
  CHECK_NEAR(provinceEffects(f.w(), f.p[0])[Fx::TradeFlat], 10, 1e-9);
  CHECK(has(lastLog(f.w()), "(уровень 2)"));
  f.tx([&](Tx& tx) { cancelBuilding(tx, f.p[0], s.farm); });  // отмена улучшения: остаётся 1-й уровень
  pb = pbOf(f.w(), f.p[0], s.farm);
  CHECK(!pb->constructing && pb->level == 1);
  CHECK_NEAR(f.w().faction(f.A)->stock(3), 100, 1e-9);
  f.tx([&](Tx& tx) {
    startBuilding(tx, f.p[0], s.farm);
    for (int i = 0; i < 3; i++) endTurn(tx);
  });
  CHECK_EQ(pbOf(f.w(), f.p[0], s.farm)->builtLevel(), 2);
  CHECK_NEAR(provinceEffects(f.w(), f.p[0])[Fx::TradeFlat], 25, 1e-9);  // уровень заменяет бонусы
  CHECK(hasReason(opt(buildOptions(f.w(), f.p[0]), s.farm), "наибольший уровень"));
  // Требование выполнено — кузница доступна.
  CHECK(opt(buildOptions(f.w(), f.p[0]), s.smith)->can);
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { startBuilding(tx, f.p[0], s.exp); }); }), "Недостаточно ресурса «Железо»"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { startBuilding(tx, f.p[0], s.uniqB); }); }), "уникальная постройка другого"));
  f.tx([&](Tx& tx) { demolish(tx, f.p[0], s.farm); });
  CHECK(!pbOf(f.w(), f.p[0], s.farm));
  CHECK(has(lastLog(f.w()), "Снесена постройка «Ферма»"));
}

// ---------------------------------------------------------------- технологии
TEST(rules_tech_research_and_prereqs) {
  Fix f;
  Id t1 = 0, t2 = 0, t3 = 0, tb = 0;
  f.tx([&](Tx& tx) {
    t1 = createTech(tx, f.A, "Бронза");
    t2 = createTech(tx, f.A, "Железо");
    t3 = createTech(tx, f.A, "Сталь");
    tb = createTech(tx, f.B, "Чужая");
    tx.tech(t1).turns = 2;
    setPrereq(tx, t2, t1, true);
    setPrereq(tx, t3, t1, true);
    setPrereq(tx, t3, t2, true);
    setPrereq(tx, t3, t2, true);  // повтор не дублируется
  });
  CHECK_EQ(f.w().tech(t3)->prereqs.size(), size_t(2));
  ResearchCheck c = canResearch(f.w(), t3);
  CHECK(!c.ok);
  CHECK(c.missing == std::vector<Id>({t1, t2}));
  CHECK(canResearch(f.w(), t1).ok);
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { startResearch(tx, t2); }); }), "Сначала изучите: «Бронза»"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setStudied(tx, t2, true); }); }), "Сначала изучите"));
  CHECK(wouldCycle(f.w(), t1, t3));
  CHECK(wouldCycle(f.w(), t1, t1));
  CHECK(!wouldCycle(f.w(), t3, t1));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setPrereq(tx, t1, t3, true); }); }), "цикл"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setPrereq(tx, t1, t1, true); }); }), "самой себя"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setPrereq(tx, t1, tb, true); }); }), "одного дерева"));
  f.tx([&](Tx& tx) { startResearch(tx, t1); });
  CHECK(f.w().tech(t1)->research);
  CHECK(has(lastLog(f.w()), "начато исследование «Бронза», осталось 2 хода"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { startResearch(tx, t1); }); }), "уже исследуется"));
  f.tx([&](Tx& tx) { endTurn(tx); });
  CHECK_EQ(f.w().tech(t1)->progress, 1);
  f.tx([&](Tx& tx) { stopResearch(tx, t1); });
  f.tx([&](Tx& tx) { endTurn(tx); });
  CHECK_EQ(f.w().tech(t1)->progress, 1);  // без исследования прогресс стоит, но сохраняется
  f.tx([&](Tx& tx) { startResearch(tx, t1); });
  f.tx([&](Tx& tx) { endTurn(tx); });
  CHECK(f.w().tech(t1)->studied);
  CHECK(!f.w().tech(t1)->research);
  CHECK(has(lastLog(f.w(), "изучена"), "«Арден»: изучена технология «Бронза»"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { startResearch(tx, t1); }); }), "уже изучена"));
  f.tx([&](Tx& tx) { setStudied(tx, t2, true); });  // отметкой
  CHECK(f.w().tech(t2)->studied);
  CHECK_EQ(f.w().tech(t2)->progress, f.w().tech(t2)->turns);
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setStudied(tx, t1, false); }); }), "От неё зависят изученные технологии: «Железо»"));
  f.tx([&](Tx& tx) { setStudied(tx, t2, false); });
  CHECK(!f.w().tech(t2)->studied);
  CHECK_EQ(f.w().tech(t2)->progress, 0);
  f.tx([&](Tx& tx) { setPrereq(tx, t3, t2, false); });
  CHECK(f.w().tech(t3)->prereqs == std::vector<Id>({t1}));
  CHECK(canResearch(f.w(), t3).ok);
}

TEST(rules_tech_autolayout) {
  Fix f;
  Id r1 = 0, r2 = 0, x = 0, y = 0, e = 0, g = 0;
  f.tx([&](Tx& tx) {
    r1 = createTech(tx, f.A, "Корень 1");
    r2 = createTech(tx, f.A, "Корень 2");
    x = createTech(tx, f.A, "X");  // зависит от r2
    y = createTech(tx, f.A, "Y");  // зависит от r1: в порядке ID связи пересекаются
    e = createTech(tx, f.A, "E");
    g = createTech(tx, f.A, "G");
    setPrereq(tx, x, r2, true);
    setPrereq(tx, y, r1, true);
    setPrereq(tx, e, x, true);
    setPrereq(tx, e, y, true);
    setPrereq(tx, g, r1, true);  // длинная связь через два слоя
    setPrereq(tx, g, e, true);
    createTech(tx, f.B, "Чужая");
  });
  f.tx([&](Tx& tx) { autoLayout(tx, f.A); });
  const World& w = f.w();
  auto P = [&](Id id) { return w.tech(id)->pos; };
  CHECK_NEAR(P(r1).x, 0, 1e-9);
  CHECK_NEAR(P(r2).x, 0, 1e-9);
  CHECK_NEAR(P(x).x, kTreeColStep, 1e-9);
  CHECK_NEAR(P(y).x, kTreeColStep, 1e-9);
  CHECK_NEAR(P(e).x, 2 * kTreeColStep, 1e-9);
  CHECK_NEAR(P(g).x, 3 * kTreeColStep, 1e-9);  // слой — длиннейший путь от корня
  // Пересечение снято: порядок детей совпадает с порядком родителей.
  CHECK((P(r1).y < P(r2).y) == (P(y).y < P(x).y));
  CHECK_NEAR(std::fabs(P(r1).y - P(r2).y), kTreeRowStep, 1e-9);
  CHECK_NEAR(P(e).y, kTreeRowStep * 0.5, 1e-9);  // один узел в слое — по центру
  // Повторная раскладка ничего не меняет.
  World before = f.w();
  f.tx([&](Tx& tx) { autoLayout(tx, f.A); });
  CHECK_EQ(World::diff(before, f.w()), 0u);
}

// ---------------------------------------------------------------- гильдии
TEST(rules_guilds_hq_influence_home) {
  Fix f;
  f.tx([&](Tx& tx) {
    tx.province(f.p[0]).owner = f.A;
    buildHq(tx, f.G, f.p[0]);
  });
  CHECK(f.w().province(f.p[0])->hqs == std::vector<Id>({f.G}));
  CHECK(has(lastLog(f.w()), "открыт штаб в провинции «П0»"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { buildHq(tx, f.G, f.p[0]); }); }), "уже есть штаб"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { buildHq(tx, f.A, f.p[0]); }); }), "нужна торговая гильдия"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { buildHq(tx, f.G, f.seaW); }); }), "морской"));
  f.tx([&](Tx& tx) {
    buildHq(tx, f.H, f.p[0]);
    for (int i = 0; i < 3; i++) buildHq(tx, createFaction(tx, FactionKind::Guild), f.p[0]);
  });
  CHECK_EQ(f.w().province(f.p[0])->hqs.size(), size_t(5));
  CHECK(has(errorOf([&] {
          f.tx([&](Tx& tx) { buildHq(tx, createFaction(tx, FactionKind::Guild), f.p[0]); });
        }),
        "уже 5 штабов"));
  f.tx([&](Tx& tx) { removeHq(tx, f.H, f.p[0]); });
  CHECK_EQ(f.w().province(f.p[0])->hqs.size(), size_t(4));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { removeHq(tx, f.H, f.p[0]); }); }), "нет штаба"));
  // Влияние: сумма не больше 100 %.
  f.tx([&](Tx& tx) { setInfluence(tx, f.p[0], f.G, 60); });
  std::string e = errorOf([&] { f.tx([&](Tx& tx) { setInfluence(tx, f.p[0], f.H, 50); }); });
  CHECK(has(e, "превысит 100 %"));
  CHECK(has(e, "свободно 40 %"));
  f.tx([&](Tx& tx) { setInfluence(tx, f.p[0], f.H, 40); });
  f.tx([&](Tx& tx) { setInfluence(tx, f.p[0], f.G, 55); });  // своё значение не мешает изменению
  CHECK_EQ(f.w().province(f.p[0])->influence.size(), size_t(2));
  CHECK_NEAR(f.w().province(f.p[0])->influence[0].pct, 55, 1e-9);
  f.tx([&](Tx& tx) { setInfluence(tx, f.p[0], f.G, 0); });
  CHECK_EQ(f.w().province(f.p[0])->influence.size(), size_t(1));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setInfluence(tx, f.p[0], f.G, 101); }); }), "от 0 до 100"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setInfluence(tx, f.p[0], f.G, -1); }); }), "от 0 до 100"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setInfluence(tx, f.p[0], f.A, 5); }); }), "нужна торговая гильдия"));
  // Государство расположения.
  f.tx([&](Tx& tx) { setHomeState(tx, f.G, f.A); });
  CHECK_EQ(f.w().faction(f.G)->homeState, f.A);
  CHECK(has(lastLog(f.w()), "перенесла основное государство расположения в «Арден»"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setHomeState(tx, f.G, f.H); }); }), "нужно государство"));
  Id sg = 0;
  f.tx([&](Tx& tx) { sg = createStateGuild(tx, f.B); });
  const Faction& g = *f.w().faction(sg);
  CHECK(g.isGuild());
  CHECK(g.stateGuild);
  CHECK_EQ(g.homeState, f.B);
  CHECK_EQ(g.name, std::string("Гильдия «Бельмар»"));
  CHECK(has(lastLog(f.w()), "учреждена государственная гильдия"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setHomeState(tx, sg, f.A); }); }), "Государственная гильдия не может"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { createStateGuild(tx, f.G); }); }), "нужно государство"));
}

// ---------------------------------------------------------------- маршруты
TEST(rules_routes) {
  Fix f;
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { createRoute(tx, {center(0)}); }); }), "не менее двух"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { createRoute(tx, {center(0), center(0)}); }); }), "не менее двух"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { createRoute(tx, {center(0), {1200, 100}}); }); }), "вне карты"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { createRoute(tx, {center(0), center(1)}, f.A); }); }), "нужна торговая гильдия"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { createRoute(tx, {center(0), {std::nan(""), 1}}); }); }), "Недопустимые координаты"));
  Id r = 0;
  f.tx([&](Tx& tx) { r = createRoute(tx, {center(0), center(0), center(1), center(2)}, f.G); });
  CHECK_EQ(f.w().route(r)->pts.size(), size_t(3));  // повторная точка убрана
  CHECK_EQ(f.w().route(r)->name, std::string("Торговый путь 1"));
  CHECK_EQ(f.w().route(r)->guild, f.G);
  CHECK(has(lastLog(f.w()), "Проложен торговый маршрут «Торговый путь 1» гильдии «Гильдия весов»"));
  auto c = calc(f.w());
  CHECK_EQ(c->routeCounts.at(f.p[0]), 1);
  CHECK_EQ(c->routeCounts.at(f.p[2]), 1);
  CHECK(c->routeCounts.count(f.p[3]) == 0);
  f.tx([&](Tx& tx) { setRoutePoints(tx, r, {center(3), center(7)}); });
  c = calc(f.w());
  CHECK(c->routeCounts.count(f.p[0]) == 0);
  CHECK_EQ(c->routeCounts.at(f.p[3]), 1);
  CHECK_EQ(c->routeCounts.at(f.p[7]), 1);
  f.tx([&](Tx& tx) { removeRoute(tx, r); });
  CHECK(!f.w().route(r));
  CHECK(calc(f.w())->routeCounts.empty());
  CHECK(has(lastLog(f.w()), "Упразднён торговый маршрут"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { removeRoute(tx, r); }); }), "не найден"));
}

TEST(rules_log_add) {
  Fix f;
  Id id = 0;
  f.tx([&](Tx& tx) { id = addLog(tx, LogKind::Note, "  Заметка  ", LogRefs{f.p[0], 0, {f.A, f.A, 0, f.B}}); });
  const LogEntry& e = *f.w().log.get(id);
  CHECK_EQ(e.text, std::string("Заметка"));
  CHECK_EQ(e.turn, 1);
  CHECK_EQ(e.province, f.p[0]);
  CHECK(e.factions == std::vector<Id>({f.A, f.B}));
  CHECK(!e.at.empty());
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { addLog(tx, LogKind::Note, "   "); }); }), "пустой"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { addLog(tx, LogKind::Count, "x"); }); }), "Неизвестный"));
}
