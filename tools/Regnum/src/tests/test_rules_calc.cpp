// Тесты rules: источники модификаторов и формулы расчёта провинций и фракций (RULES.md §1–3), кеш расчёта.
#include <atomic>
#include <cstdio>
#include <thread>

#include "tests/test_rules_util.h"

using namespace rg;
using namespace rg::rules;
using namespace rg::rulestest;

TEST(rules_fixture_geometry) {
  Fix f;
  auto is = geo::validate(f.w());
  CHECK(is.empty());
  auto fs = geo::faces(f.w());
  for (int i = 0; i < 8; i++) {
    CHECK_EQ(fs->provinceAt(center(i)), f.p[i]);
    CHECK(fs->terrainAt(center(i)) == Terrain::Land);
    CHECK_NEAR(fs->shape(f.p[i])->area, 40000, 1e-6);
  }
  CHECK(fs->terrainAt(kSeaNorth) == Terrain::Sea);
  CHECK_EQ(fs->provinceAt(kSeaWest), f.seaW);
  CHECK(f.w().province(f.seaW)->sea);
}

// ---------------------------------------------------------------- источники эффектов
TEST(rules_mods_province_sources) {
  Fix f;
  Id mP, mS, mT, mT2, mB1, mB2, mG, mGT, mGlobal;
  Id tech = 0, tech2 = 0, bld = 0, gtech = 0;
  f.tx([&](Tx& tx) {
    mP = makeMod(tx, {{Fx::TradePct, 5}});
    mS = makeMod(tx, {{Fx::TradePct, 7}, {Fx::IncomePct, 10}});  // глобальный эффект в локальные не входит
    mT = makeMod(tx, {{Fx::TradePct, 11}});
    mT2 = makeMod(tx, {{Fx::TradePct, 1000}});
    mB1 = makeMod(tx, {{Fx::TradePct, 13}});
    mB2 = makeMod(tx, {{Fx::TradePct, 17}});
    mG = makeMod(tx, {{Fx::TradePct, 19}});
    mGT = makeMod(tx, {{Fx::TradeFlat, 3}});
    mGlobal = makeMod(tx, {{Fx::ArmyUpkeepPct, -10}});
    Province& p = tx.province(f.p[0]);
    p.owner = f.A;
    p.modifiers = {mP, mGlobal};
    tx.faction(f.A).modifiers = {mS};
    tech = createTech(tx, f.A, "Изученная");
    tx.tech(tech).modifiers = {mT};
    tx.tech(tech).studied = true;
    tech2 = createTech(tx, f.A, "Не изученная");
    tx.tech(tech2).modifiers = {mT2};
    bld = createBuilding(tx, 0, "Рынок");
    Building& b = tx.building(bld);
    b.levels.resize(2);
    b.levels[0].modifiers = {mB1};
    b.levels[1].modifiers = {mB2};
    tx.province(f.p[0]).buildings.push_back(ProvBuilding{bld, 2, true, 3});  // строится 2-й уровень: действует 1-й
    tx.faction(f.G).modifiers = {mG};
    gtech = createTech(tx, f.G, "Склады");
    tx.tech(gtech).modifiers = {mGT};
    tx.tech(gtech).studied = true;
    tx.province(f.p[0]).hqs = {f.G};
  });
  Effects e = provinceEffects(f.w(), f.p[0]);
  CHECK_NEAR(e[Fx::TradePct], 5 + 7 + 11 + 13 + 19, 1e-9);
  CHECK_NEAR(e[Fx::TradeFlat], 3, 1e-9);
  CHECK_NEAR(e[Fx::IncomePct], 0, 1e-9);
  CHECK_NEAR(e[Fx::ArmyUpkeepPct], 0, 1e-9);
  // Источники по видам.
  auto hasSrc = [&](EffectSource::Kind k, Id id, Id m) {
    for (auto& s : e.sources)
      if (s.kind == k && s.id == id && s.modifier == m) return true;
    return false;
  };
  CHECK(hasSrc(EffectSource::Province, f.p[0], mP));
  CHECK(hasSrc(EffectSource::Faction, f.A, mS));
  CHECK(hasSrc(EffectSource::Tech, tech, mT));
  CHECK(!hasSrc(EffectSource::Tech, tech2, mT2));
  CHECK(hasSrc(EffectSource::Building, bld, mB1));
  CHECK(!hasSrc(EffectSource::Building, bld, mB2));
  CHECK(hasSrc(EffectSource::Guild, f.G, mG));
  CHECK(hasSrc(EffectSource::Tech, gtech, mGT));
  CHECK(!hasSrc(EffectSource::Province, f.p[0], mGlobal));  // нет локальных эффектов — не источник

  // Достроенный 2-й уровень заменяет бонусы 1-го.
  f.tx([&](Tx& tx) { tx.province(f.p[0]).buildings[0] = ProvBuilding{bld, 2, false, 0}; });
  e = provinceEffects(f.w(), f.p[0]);
  CHECK_NEAR(e[Fx::TradePct], 5 + 7 + 11 + 17 + 19, 1e-9);
  // Штаб гильдии в другой провинции — эффекты гильдии там, а не здесь.
  f.tx([&](Tx& tx) { tx.province(f.p[0]).hqs.clear(); });
  e = provinceEffects(f.w(), f.p[0]);
  CHECK_NEAR(e[Fx::TradePct], 5 + 7 + 11 + 17, 1e-9);
  // Провинция без владельца: модификаторы государства не действуют.
  f.tx([&](Tx& tx) { tx.province(f.p[0]).owner = 0; });
  e = provinceEffects(f.w(), f.p[0]);
  CHECK_NEAR(e[Fx::TradePct], 5 + 17, 1e-9);
}

TEST(rules_mods_faction_sources_and_diplomacy) {
  Fix f;
  Id bld = 0;
  f.tx([&](Tx& tx) {
    Id m1 = makeMod(tx, {{Fx::IncomePct, 10}, {Fx::TradePct, 50}, {Fx::DiplomacyPerTurn, 3}}, {f.B, f.C, f.B, f.A});
    Id m2 = makeMod(tx, {{Fx::DiplomacyPerTurn, -1}}, {f.B, 9999});
    Id m3 = makeMod(tx, {{Fx::ArmyUpkeepPct, -20}});
    Id m4 = makeMod(tx, {{Fx::FleetUpkeepPct, 5}});
    tx.faction(f.A).modifiers = {m1, m2};
    Id t = createTech(tx, f.A, "Т");
    tx.tech(t).modifiers = {m3};
    tx.tech(t).studied = true;
    bld = createBuilding(tx, 0, "Верфь");
    tx.building(bld).levels[0].modifiers = {m4};
    tx.province(f.p[0]).owner = f.A;
    tx.province(f.p[0]).buildings.push_back(ProvBuilding{bld, 1, false, 0});
    tx.province(f.p[1]).owner = f.B;
    tx.province(f.p[1]).buildings.push_back(ProvBuilding{bld, 1, false, 0});  // чужая провинция — эффект у B
    tx.province(f.p[2]).owner = f.A;
    tx.province(f.p[2]).buildings.push_back(ProvBuilding{bld, 1, true, 2});   // не достроено
  });
  Effects a = factionEffects(f.w(), f.A);
  CHECK_NEAR(a[Fx::IncomePct], 10, 1e-9);
  CHECK_NEAR(a[Fx::TradePct], 0, 1e-9);  // локальный эффект в глобальные не входит
  CHECK_NEAR(a[Fx::ArmyUpkeepPct], -20, 1e-9);
  CHECK_NEAR(a[Fx::FleetUpkeepPct], 5, 1e-9);
  CHECK_NEAR(a[Fx::DiplomacyPerTurn], 2, 1e-9);
  CHECK_EQ(a.diplomacy.size(), size_t(2));  // B и C; себя и несуществующих нет, повторы — один раз
  CHECK_NEAR(a.diplomacy.at(f.B), 3 - 1, 1e-9);
  CHECK_NEAR(a.diplomacy.at(f.C), 3, 1e-9);
  Effects b = factionEffects(f.w(), f.B);
  CHECK_NEAR(b[Fx::FleetUpkeepPct], 5, 1e-9);
  CHECK_NEAR(b[Fx::IncomePct], 0, 1e-9);
  CHECK(factionEffects(f.w(), 12345).sources.empty());
}

// ---------------------------------------------------------------- формулы провинции
TEST(rules_calc_slots) {
  Fix f;
  Id b1 = 0, b2 = 0;
  f.tx([&](Tx& tx) {
    Province& p = tx.province(f.p[0]);
    p.owner = f.A;
    p.size = ProvSize::Large;
    p.city = CityType::City;
    p.modifiers = {makeMod(tx, {{Fx::Slots, 2}})};
    tx.faction(f.A).modifiers = {makeMod(tx, {{Fx::Slots, -1}})};
    Id t = createTech(tx, f.A, "Т");
    tx.tech(t).modifiers = {makeMod(tx, {{Fx::Slots, 1}})};
    tx.tech(t).studied = true;
    Id t2 = createTech(tx, f.A, "Т2");
    tx.tech(t2).modifiers = {makeMod(tx, {{Fx::Slots, 5}})};
    Province& q = tx.province(f.p[1]);
    q.size = ProvSize::Small;
    q.city = CityType::Outpost;
    q.modifiers = {makeMod(tx, {{Fx::Slots, -5}})};
    b1 = createBuilding(tx, 0, "А");
    b2 = createBuilding(tx, 0, "Б");
    tx.province(f.p[2]).buildings = {ProvBuilding{b1, 1, false, 0}, ProvBuilding{b2, 1, true, 2}};
  });
  const ProvinceCalc& a = f.pc(f.p[0]);
  CHECK_EQ(a.slotsSize, 3);
  CHECK_EQ(a.slotsCity, 3);
  CHECK_EQ(a.slotsMods, 2);
  CHECK_EQ(a.slots, 8);
  const ProvinceCalc& b = f.pc(f.p[1]);
  CHECK_EQ(b.slotsSize, 1);
  CHECK_EQ(b.slotsCity, 0);
  CHECK_EQ(b.slotsMods, -5);
  CHECK_EQ(b.slots, 0);  // не меньше нуля
  const ProvinceCalc& c = f.pc(f.p[2]);
  CHECK_EQ(c.slots, 3);  // средняя + деревня по умолчанию
  CHECK_EQ(c.slotsUsed, 2);  // строящиеся тоже занимают слот
}

TEST(rules_calc_trade_value_routes_and_modifiers) {
  Fix f;
  f.tx([&](Tx& tx) {
    for (int i : {0, 1, 2}) {
      tx.province(f.p[i]).owner = f.A;
      tx.province(f.p[i]).baseTrade = 100;
    }
    tx.province(f.p[0]).modifiers = {makeMod(tx, {{Fx::TradePct, 20}, {Fx::TradeFlat, 15}})};
    tx.faction(f.A).modifiers = {makeMod(tx, {{Fx::TradePct, 10}})};
    tx.province(f.p[3]).baseTrade = 10;
    tx.province(f.p[3]).modifiers = {makeMod(tx, {{Fx::TradeFlat, -5000}})};
  });
  CHECK_NEAR(f.pc(f.p[0]).tradeValue, 100 * 1.3 + 15, 1e-9);
  CHECK_NEAR(f.pc(f.p[1]).tradeValue, 110, 1e-9);
  CHECK_NEAR(f.pc(f.p[3]).tradeValue, 0, 1e-9);  // не меньше нуля
  CHECK_NEAR(f.pc(f.p[3]).tradeBase, 10, 1e-9);

  // Маршрут через p0 и p1.
  f.tx([&](Tx& tx) { createRoute(tx, {center(0), center(1)}); });
  CHECK_EQ(f.pc(f.p[0]).routes, 1);
  CHECK_NEAR(f.pc(f.p[0]).tradeValue, 100 * 1.4 + 15, 1e-9);
  CHECK_NEAR(f.pc(f.p[1]).tradeValue, 120, 1e-9);
  CHECK_EQ(f.pc(f.p[2]).routes, 0);
  // Второй маршрут возвращается в p0: провинция считается один раз на маршрут.
  f.tx([&](Tx& tx) { createRoute(tx, {center(0), center(1), {210, 250}}); });
  CHECK_EQ(f.pc(f.p[0]).routes, 2);
  CHECK_EQ(f.pc(f.p[1]).routes, 2);
  CHECK_NEAR(f.pc(f.p[0]).tradeValue, 100 * 1.5 + 15, 1e-9);
  // Маршрут целиком внутри p2 (начало и конец в провинции).
  f.tx([&](Tx& tx) { createRoute(tx, {{560, 160}, {640, 240}}); });
  CHECK_EQ(f.pc(f.p[2]).routes, 1);
  CHECK_NEAR(f.pc(f.p[2]).tradeValue, 120, 1e-9);
  auto c = calc(f.w());
  CHECK_EQ(c->routeCounts.at(f.p[0]), 2);
  CHECK(c->routeCounts.count(f.p[5]) == 0);
}

TEST(rules_calc_production) {
  Fix f;
  f.tx([&](Tx& tx) {
    Province& p = tx.province(f.p[0]);
    p.owner = f.A;
    p.resource = 5;
    p.resourceAmount = 50;
    p.modifiers = {makeMod(tx, {{Fx::ResourcePct, 50}, {Fx::ResourceFlat, 5}})};
    tx.province(f.p[1]).resourceAmount = 50;  // без ресурса
    tx.province(f.p[2]).resource = 5;
    tx.province(f.p[2]).resourceAmount = 10;
    tx.province(f.p[2]).modifiers = {makeMod(tx, {{Fx::ResourceFlat, -100}})};
    tx.province(f.p[3]).resource = 999;  // нет в справочнике
    tx.province(f.p[3]).resourceAmount = 10;
  });
  CHECK_NEAR(f.pc(f.p[0]).production, 80, 1e-9);
  CHECK_NEAR(f.pc(f.p[1]).production, 0, 1e-9);
  CHECK_NEAR(f.pc(f.p[2]).production, 0, 1e-9);
  CHECK_NEAR(f.pc(f.p[3]).production, 0, 1e-9);
  CHECK_NEAR(f.fc(f.A).resources.at(5).production, 80, 1e-9);
  CHECK_NEAR(f.fc(f.A).resources.at(5).net, 80, 1e-9);
}

TEST(rules_calc_rebellion_clamp) {
  Fix f;
  f.tx([&](Tx& tx) {
    tx.province(f.p[0]).contentment = -40;
    tx.province(f.p[1]).contentment = -40;
    tx.province(f.p[1]).modifiers = {makeMod(tx, {{Fx::RebellionPct, 10}})};
    tx.province(f.p[2]).contentment = 60;
    Province& p = tx.province(f.p[3]);
    p.contentment = -100;
    p.owner = f.A;
    p.modifiers = {makeMod(tx, {{Fx::RebellionPct, 30}})};
    tx.faction(f.A).modifiers = {makeMod(tx, {{Fx::RebellionPct, 50}})};
    tx.province(f.p[4]).contentment = 20;
    tx.province(f.p[4]).modifiers = {makeMod(tx, {{Fx::RebellionPct, 25}})};
  });
  CHECK_NEAR(f.pc(f.p[0]).rebellion, 20, 1e-9);   // 2 довольства : 1 %
  CHECK_NEAR(f.pc(f.p[1]).rebellion, 30, 1e-9);
  CHECK_NEAR(f.pc(f.p[2]).rebellion, 0, 1e-9);    // не меньше 0
  CHECK_NEAR(f.pc(f.p[3]).rebellion, 100, 1e-9);  // не больше 100
  CHECK_NEAR(f.pc(f.p[4]).rebellion, 15, 1e-9);
  CHECK_NEAR(f.pc(f.p[5]).rebellion, 0, 1e-9);
}

TEST(rules_calc_tax_minimum_one_percent) {
  Fix f;
  f.tx([&](Tx& tx) {
    tx.faction(f.A).tax = 10;
    for (int i : {0, 1}) {
      tx.province(f.p[i]).owner = f.A;
      tx.province(f.p[i]).baseTrade = 200;
    }
    tx.province(f.p[0]).localTax = -15;
    tx.province(f.p[1]).localTax = 5;
  });
  CHECK_NEAR(f.pc(f.p[0]).taxState, 10, 1e-9);
  CHECK_NEAR(f.pc(f.p[0]).taxLocal, -15, 1e-9);
  CHECK_NEAR(f.pc(f.p[0]).taxTotal, 1, 1e-9);
  CHECK_NEAR(f.pc(f.p[0]).provinceTax, 2, 1e-9);
  CHECK_NEAR(f.pc(f.p[1]).taxTotal, 15, 1e-9);
  CHECK_NEAR(f.pc(f.p[1]).provinceTax, 30, 1e-9);
  CHECK_NEAR(f.fc(f.A).incProvinces, 32, 1e-9);
  // Налог государства не бывает отрицательным: повреждённое значение считается нулём.
  f.tx([&](Tx& tx) { tx.faction(f.A).tax = -5; tx.province(f.p[1]).localTax = 0; });
  CHECK_NEAR(f.pc(f.p[1]).taxState, 0, 1e-9);
  CHECK_NEAR(f.pc(f.p[1]).taxTotal, 1, 1e-9);
}

TEST(rules_calc_guild_income_split) {
  Fix f;
  f.tx([&](Tx& tx) {
    tx.faction(f.A).tax = 10;
    Province& p = tx.province(f.p[0]);
    p.owner = f.A;
    p.baseTrade = 200;
    p.influence = {Influence{f.H, 20}, Influence{f.G, 30}};
    p.hqs = {f.G};
    tx.province(f.p[1]).hqs = {f.H};  // штаб без влияния
    Province& q = tx.province(f.p[2]);  // повреждённые данные: сумма влияния 150 %
    q.owner = f.A;
    q.baseTrade = 100;
    q.influence = {Influence{f.G, 80}, Influence{f.H, 70}};
    q.hqs = {f.G, f.H};
  });
  const ProvinceCalc& pc = f.pc(f.p[0]);
  CHECK_EQ(pc.guilds.size(), size_t(2));
  CHECK_EQ(pc.guilds[0].guild, f.G);  // по убыванию влияния
  CHECK(pc.guilds[0].hq);
  CHECK_NEAR(pc.guilds[0].gross, 60, 1e-9);
  CHECK_NEAR(pc.guilds[0].tax, 6, 1e-9);
  CHECK_NEAR(pc.guilds[0].net, 54, 1e-9);
  CHECK_EQ(pc.guilds[1].guild, f.H);
  CHECK(!pc.guilds[1].hq);
  CHECK_NEAR(pc.guilds[1].gross, 0, 1e-9);  // без штаба дохода нет
  CHECK_NEAR(pc.provinceTax, (200 - 60) * 0.1, 1e-9);
  CHECK_NEAR(pc.guildTax, 6, 1e-9);
  const ProvinceCalc& q = f.pc(f.p[2]);
  CHECK_NEAR(q.guilds[0].pct + q.guilds[1].pct, 100, 1e-9);
  CHECK_NEAR(q.provinceTax, 0, 1e-9);
  CHECK_NEAR(q.guilds[0].gross + q.guilds[1].gross, 100, 1e-9);
  const ProvinceCalc& h = f.pc(f.p[1]);
  CHECK_EQ(h.guilds.size(), size_t(1));
  CHECK(h.guilds[0].hq);
  CHECK_NEAR(h.guilds[0].pct, 0, 1e-9);

  const FactionCalc& a = f.fc(f.A);
  CHECK_NEAR(a.incProvinces, 14 + 0, 1e-9);
  CHECK_NEAR(a.incGuildTax, 6 + 10, 1e-9);
  const FactionCalc& g = f.fc(f.G);
  CHECK_NEAR(g.incGuilds, 54 + q.guilds[0].net, 1e-9);
  CHECK(g.provinces == std::vector<Id>({f.p[0], f.p[2]}));
  const FactionCalc& hh = f.fc(f.H);
  CHECK(hh.provinces == std::vector<Id>({f.p[1], f.p[2]}));
  CHECK_NEAR(hh.incGuilds, q.guilds[1].net, 1e-9);
}

TEST(rules_calc_occupied_income_recipient) {
  Fix f;
  f.tx([&](Tx& tx) {
    Province& p = tx.province(f.p[0]);
    p.owner = f.A;
    p.baseTrade = 100;
    p.resource = 2;
    p.resourceAmount = 20;
    p.occupied = true;
    p.occupier = f.B;
  });
  CHECK_EQ(f.pc(f.p[0]).recipient, f.A);  // по умолчанию — владельцу
  CHECK_NEAR(f.fc(f.A).incProvinces, 10, 1e-9);
  CHECK_NEAR(f.fc(f.A).resources.at(2).production, 20, 1e-9);
  CHECK_NEAR(f.fc(f.B).incProvinces, 0, 1e-9);

  f.tx([&](Tx& tx) { tx.settings().occupiedIncome = OccupiedIncome::Occupier; });
  CHECK_EQ(f.pc(f.p[0]).recipient, f.B);
  CHECK_NEAR(f.fc(f.A).incProvinces, 0, 1e-9);
  CHECK_NEAR(f.fc(f.B).incProvinces, 10, 1e-9);
  CHECK_NEAR(f.fc(f.B).resources.at(2).production, 20, 1e-9);
  CHECK_NEAR(f.fc(f.A).resources.at(2).production, 0, 1e-9);
  CHECK(f.fc(f.A).provinces == std::vector<Id>({f.p[0]}));  // владение остаётся у A

  f.tx([&](Tx& tx) { tx.settings().occupiedIncome = OccupiedIncome::None; });
  CHECK_EQ(f.pc(f.p[0]).recipient, Id(0));
  CHECK_NEAR(f.fc(f.A).incProvinces + f.fc(f.B).incProvinces, 0, 1e-9);
}

TEST(rules_calc_upkeep_with_modifiers) {
  Fix f;
  Id r1 = 0, r2 = 0, s1 = 0, bld = 0;
  f.tx([&](Tx& tx) {
    r1 = addArmyRow(tx, f.A, UnitType::LightInf, "Копейщики", 100, 2);
    r2 = addArmyRow(tx, f.A, UnitType::HeavyCav, "Рыцари", 50, 4);
    s1 = addFleetRow(tx, f.A, ShipType::Frigate, "", 10, 10);
  });
  CHECK_NEAR(f.fc(f.A).expArmy, 400, 1e-9);
  CHECK_NEAR(f.fc(f.A).expFleet, 100, 1e-9);
  f.tx([&](Tx& tx) {
    tx.faction(f.A).modifiers = {makeMod(tx, {{Fx::ArmyUpkeepPct, -25}})};
    Id t = createTech(tx, f.A, "Т");
    tx.tech(t).modifiers = {makeMod(tx, {{Fx::FleetUpkeepPct, 50}})};
    tx.tech(t).studied = true;
  });
  CHECK_NEAR(f.fc(f.A).expArmy, 300, 1e-9);
  CHECK_NEAR(f.fc(f.A).expFleet, 150, 1e-9);
  f.tx([&](Tx& tx) {
    bld = createBuilding(tx, 0, "Казармы");
    tx.building(bld).levels[0].modifiers = {makeMod(tx, {{Fx::ArmyUpkeepPct, -25}})};
    tx.province(f.p[0]).owner = f.A;
    tx.province(f.p[0]).buildings.push_back(ProvBuilding{bld, 1, false, 0});
    tx.province(f.p[1]).owner = f.B;
    tx.province(f.p[1]).buildings.push_back(ProvBuilding{bld, 1, false, 0});
  });
  const FactionCalc& a = f.fc(f.A);
  CHECK_NEAR(a.expArmy, 200, 1e-9);
  CHECK_EQ(a.army.size(), size_t(2));
  CHECK_EQ(a.army[0].row, r1);
  CHECK_NEAR(a.army[0].upkeepEach, 2, 1e-9);
  CHECK_NEAR(a.army[0].upkeepTotal, 100, 1e-9);
  CHECK_EQ(a.army[1].row, r2);
  CHECK_NEAR(a.army[1].upkeepTotal, 100, 1e-9);
  CHECK_EQ(a.fleet[0].row, s1);
  CHECK_EQ(a.armyTotal, 150);
  CHECK_EQ(a.fleetTotal, 10);
  CHECK_NEAR(a.expTotal, 200 + 150, 1e-9);
  // Множитель содержания не уходит ниже нуля.
  f.tx([&](Tx& tx) { tx.faction(f.A).modifiers.push_back(makeMod(tx, {{Fx::ArmyUpkeepPct, -75}})); });
  CHECK_NEAR(f.fc(f.A).expArmy, 0, 1e-9);
}

// Специалисты (RULES.md §3): различные персонажи с ролью — правитель, места совета (персонаж любой фракции),
// герои самой фракции. Персонаж фракции без роли не оплачивается; несколько ролей — одно содержание.
TEST(rules_calc_specialists) {
  Fix f;
  Id chancellor = 0, mage = 0, idle = 0, tramp = 0, admiral = 0, keeper = 0;
  f.tx([&](Tx& tx) {
    chancellor = createCharacter(tx, f.A, "Канцлер");
    tx.character(chancellor).upkeep = 15;
    mage = createCharacter(tx, f.A, "Маг");
    tx.character(mage).upkeep = 5;
    tx.character(mage).hero = true;
    idle = createCharacter(tx, f.A, "Придворный");     // без роли
    tx.character(idle).upkeep = 40;
    tramp = createCharacter(tx, 0, "Бродяга");
    tx.character(tramp).upkeep = 100;
    admiral = createCharacter(tx, f.B, "Адмирал");
    tx.character(admiral).upkeep = 7;
    tx.character(admiral).hero = true;
    keeper = createCharacter(tx, f.G, "Казначей");
    tx.character(keeper).upkeep = 3;
    tx.faction(f.A).ruler = chancellor;
    tx.faction(f.A).council.push_back(CouncilSeat{tx.nextId(Seq::Council), "Канцлер", chancellor});  // правитель и советник — один раз
    tx.faction(f.G).council.push_back(CouncilSeat{tx.nextId(Seq::Council), "Казначей", keeper});
  });
  CHECK_NEAR(f.fc(f.A).expSpecialists, 20, 1e-9);
  CHECK_NEAR(f.fc(f.B).expSpecialists, 7, 1e-9);
  CHECK_NEAR(f.fc(f.G).expSpecialists, 3, 1e-9);
  CHECK_NEAR(f.fc(f.A).net, -20, 1e-9);
  // Советник другой фракции оплачивается тем, в чьём совете сидит (и своей фракцией — если он там герой).
  f.tx([&](Tx& tx) { tx.faction(f.A).council.push_back(CouncilSeat{tx.nextId(Seq::Council), "Адмирал", admiral}); });
  CHECK_NEAR(f.fc(f.A).expSpecialists, 27, 1e-9);
  CHECK_NEAR(f.fc(f.B).expSpecialists, 7, 1e-9);
  // Снятие с ролей уменьшает расход: место совета, герой, правитель.
  f.tx([&](Tx& tx) { tx.faction(f.A).council.clear(); });
  CHECK_NEAR(f.fc(f.A).expSpecialists, 20, 1e-9);
  f.tx([&](Tx& tx) { tx.character(mage).hero = false; });
  CHECK_NEAR(f.fc(f.A).expSpecialists, 15, 1e-9);
  f.tx([&](Tx& tx) { tx.faction(f.A).ruler = 0; });
  CHECK_NEAR(f.fc(f.A).expSpecialists, 0, 1e-9);
  // Персонаж без роли становится советником — расход растёт.
  f.tx([&](Tx& tx) { tx.faction(f.A).council.push_back(CouncilSeat{tx.nextId(Seq::Council), "Придворный", idle}); });
  CHECK_NEAR(f.fc(f.A).expSpecialists, 40, 1e-9);
}

TEST(rules_calc_income_trade_tribute_and_net) {
  Fix f;
  f.tx([&](Tx& tx) {
    tx.faction(f.A).tax = 10;
    tx.province(f.p[0]).owner = f.A;
    tx.province(f.p[0]).baseTrade = 100;
    tx.province(f.p[1]).owner = f.A;
    tx.province(f.p[1]).resource = kGold;  // добыча золота — доход провинции
    tx.province(f.p[1]).resourceAmount = 7;
    tx.faction(f.A).modifiers = {makeMod(tx, {{Fx::IncomePct, 20}})};
    tx.faction(f.A).res[5] = 100;
    Deal d;
    d.a = f.A;
    d.b = f.B;
    d.items.push_back(DealItem{DealSide::B, kGold, 30, DealMode::PerTurn, 4, 0});
    d.items.push_back(DealItem{DealSide::A, 5, 5, DealMode::PerTurn, 4, 0});
    concludeDeal(tx, d);
    imposeTribute(tx, DealKind::Tribute, f.A, f.C, 20, 3);
  });
  const FactionCalc& a = f.fc(f.A);
  CHECK_NEAR(a.incProvinces, 10 + 7, 1e-9);
  CHECK_NEAR(a.incTrade, 30, 1e-9);
  CHECK_NEAR(a.incTribute, 20, 1e-9);
  CHECK_NEAR(a.incGross, 67, 1e-9);
  CHECK_NEAR(a.incomePct, 20, 1e-9);
  // Модификатор дохода — только к собственному доходу (налоги, добыча золота); сделки и дань — без изменений.
  CHECK_NEAR(a.incTotal, 17 * 1.2 + 30 + 20, 1e-9);
  CHECK_NEAR(a.net, 17 * 1.2 + 30 + 20, 1e-9);
  CHECK_NEAR(a.resources.at(kGold).tradeIn, 50, 1e-9);
  CHECK_NEAR(a.resources.at(kGold).production, 7, 1e-9);
  CHECK_NEAR(a.resources.at(kGold).net, a.net, 1e-9);
  CHECK_NEAR(a.resources.at(5).tradeOut, 5, 1e-9);
  CHECK_NEAR(a.resources.at(5).net, -5, 1e-9);
  CHECK_NEAR(a.resources.at(5).stock, 100, 1e-9);
  const FactionCalc& b = f.fc(f.B);
  CHECK_NEAR(b.expTrade, 30, 1e-9);
  CHECK_NEAR(b.resources.at(5).tradeIn, 5, 1e-9);
  CHECK_NEAR(b.net, -30, 1e-9);
  CHECK_NEAR(f.fc(f.C).expTribute, 20, 1e-9);
  // Все позиции справочника ресурсов присутствуют.
  CHECK_EQ(b.resources.size(), f.w().catalogs->resources.size());
}

TEST(rules_calc_population_and_races) {
  Fix f;
  Id human = 0, elf = 0;
  f.tx([&](Tx& tx) {
    human = addCatalogItem(tx, CatalogList::Races, "Люди");
    elf = addCatalogItem(tx, CatalogList::Races, "Эльфы");
    tx.province(f.p[0]).owner = f.A;
    tx.province(f.p[0]).races = {RacePop{human, 100}, RacePop{elf, 300}};
    tx.province(f.p[1]).owner = f.A;
    tx.province(f.p[1]).races = {RacePop{human, 250}};
    tx.province(f.p[2]).races = {RacePop{elf, 1000}};  // без владельца
  });
  const ProvinceCalc& p = f.pc(f.p[0]);
  CHECK_EQ(p.population, 400);
  CHECK_EQ(p.races.size(), size_t(2));
  CHECK_EQ(p.races[0].first, elf);
  CHECK_EQ(p.races[0].second, 300);
  const FactionCalc& a = f.fc(f.A);
  CHECK_EQ(a.population, 650);
  CHECK_EQ(a.races[0].first, human);
  CHECK_EQ(a.races[0].second, 350);
  CHECK_EQ(a.races[1].second, 300);
}

TEST(rules_calc_sea_province_excluded) {
  Fix f;
  f.tx([&](Tx& tx) {
    Province& p = tx.province(f.seaW);  // повреждённые данные морской провинции
    p.owner = f.A;
    p.baseTrade = 500;
    p.races = {RacePop{1, 100}};
    p.contentment = -100;
    p.modifiers = {makeMod(tx, {{Fx::TradePct, 50}})};
  });
  const ProvinceCalc& pc = f.pc(f.seaW);
  CHECK(pc.sea);
  CHECK_NEAR(pc.tradeValue, 0, 1e-9);
  CHECK_NEAR(pc.rebellion, 0, 1e-9);
  CHECK_EQ(pc.population, 0);
  CHECK_EQ(pc.slots, 0);
  CHECK(f.fc(f.A).provinces.empty());
  CHECK_NEAR(f.fc(f.A).incProvinces, 0, 1e-9);
  CHECK(provinceEffects(f.w(), f.seaW).sources.empty());
}

TEST(rules_calc_deployed_and_rows) {
  Fix f;
  Id r1 = 0, s1 = 0;
  f.tx([&](Tx& tx) {
    r1 = addArmyRow(tx, f.A, UnitType::LightInf, "", 100, 1);
    s1 = addFleetRow(tx, f.A, ShipType::Galleon, "", 8, 1);
    tx.province(f.p[0]).owner = f.A;
    Id a1 = createArmy(tx, ArmyKind::Army, f.A, center(1));
    setUnits(tx, a1, f.A, r1, 30);
    Id a2 = createArmy(tx, ArmyKind::Army, f.A, center(2));
    setUnits(tx, a2, f.A, r1, 20);
    setGarrison(tx, f.p[0], r1, 15);
    Id fl = createArmy(tx, ArmyKind::Fleet, f.A, kSeaNorth);
    setUnits(tx, fl, f.A, s1, 5);
  });
  Deployed d = deployed(f.w(), f.A);
  CHECK_EQ(d.army.at(r1), 50);
  CHECK_EQ(d.garrison.at(r1), 15);
  CHECK_EQ(d.fleet.at(s1), 5);
  const FactionCalc& a = f.fc(f.A);
  CHECK_EQ(a.army[0].field, 65);
  CHECK_EQ(a.army[0].garrison, 15);
  CHECK_EQ(a.army[0].reserve, 35);
  CHECK_EQ(a.fleet[0].field, 5);
  CHECK_EQ(a.fleet[0].reserve, 3);
  CHECK_EQ(a.armyField, 65);
  CHECK_EQ(a.fleetField, 5);
}

// ---------------------------------------------------------------- кеш
TEST(rules_calc_cache_identity_and_tx) {
  Fix f;
  auto c1 = calc(f.w());
  auto c2 = calc(f.w());
  CHECK(c1.get() == c2.get());
  // Запись хроники не меняет расчёт.
  f.tx([&](Tx& tx) { addLog(tx, LogKind::Note, "Заметка"); });
  // meta меняется при выдаче ID записи — расчёт пересчитывается, но совпадает по значениям
  auto c3 = calc(f.w());
  CHECK(c3->provinces.size() == c1->provinces.size());
  World before = f.w();
  f.tx([&](Tx& tx) { tx.province(f.p[0]).baseTrade = 50; });
  auto c4 = calc(f.w());
  CHECK(c4.get() != c3.get());
  CHECK_NEAR(c4->province(f.p[0])->tradeBase, 50, 1e-9);
  CHECK(calc(before).get() == c3.get());  // прежняя версия всё ещё в кеше

  // Изменение внутри транзакции после расчёта не отравляет кеш.
  f.tx([&](Tx& tx) {
    tx.province(f.p[0]).baseTrade = 100;
    CHECK_NEAR(calc(tx)->province(f.p[0])->tradeBase, 100, 1e-9);
    tx.province(f.p[0]).baseTrade = 200;
    CHECK_NEAR(calc(tx)->province(f.p[0])->tradeBase, 200, 1e-9);
  });
  CHECK_NEAR(calc(f.w())->province(f.p[0])->tradeBase, 200, 1e-9);
  // Неизменённая транзакция пользуется кешем.
  f.tx([&](Tx& tx) { CHECK(calc(tx).get() == calc(f.w()).get()); });
}

TEST(rules_calc_cache_thread_safe) {
  Fix f;
  f.tx([&](Tx& tx) { tx.province(f.p[0]).owner = f.A; tx.province(f.p[0]).baseTrade = 10; });
  World w1 = f.w();
  f.tx([&](Tx& tx) { tx.province(f.p[0]).baseTrade = 20; });
  World w2 = f.w();
  std::atomic<int> bad{0};
  std::vector<std::thread> th;
  for (int t = 0; t < 8; t++)
    th.emplace_back([&, t] {
      for (int i = 0; i < 200; i++) {
        const World& w = ((i + t) & 1) ? w1 : w2;
        double want = ((i + t) & 1) ? 10 : 20;
        auto c = calc(w);
        if (std::fabs(c->province(f.p[0])->tradeBase - want) > 1e-9) bad++;
      }
    });
  for (auto& x : th) x.join();
  CHECK_EQ(bad.load(), 0);
}

// ---------------------------------------------------------------- производительность
TEST(rules_calc_perf_large_world) {
  World w = newWorld("perf");
  Tx tx(w);
  Rng rng(7);
  std::vector<Id> states, guilds, mods;
  for (int i = 0; i < 60; i++) states.push_back(createFaction(tx, FactionKind::State));
  for (int i = 0; i < 20; i++) guilds.push_back(createFaction(tx, FactionKind::Guild));
  for (int i = 0; i < 120; i++) mods.push_back(makeMod(tx, {{Fx(i % kFxCount), double(i % 7) - 3}}, {states[size_t(i) % states.size()]}));
  Id race1 = addCatalogItem(tx, CatalogList::Races, "Люди"), race2 = addCatalogItem(tx, CatalogList::Races, "Гномы");
  std::vector<Id> blds;
  for (int i = 0; i < 40; i++) {
    Id b = createBuilding(tx, 0);
    tx.building(b).levels.resize(3);
    for (auto& l : tx.building(b).levels) l.modifiers = {mods[size_t(rng.range(0, 119))]};
    blds.push_back(b);
  }
  for (int i = 0; i < 3000; i++) {
    Province p;
    p.owner = states[size_t(i) % states.size()];
    p.baseTrade = 50 + i % 200;
    p.resource = Id(2 + i % 5);
    p.resourceAmount = 10 + i % 30;
    p.races = {RacePop{race1, 1000 + i}, RacePop{race2, 500 + i}};
    p.modifiers = {mods[size_t(i) % mods.size()]};
    p.influence = {Influence{guilds[size_t(i) % guilds.size()], 30}};
    p.hqs = {guilds[size_t(i) % guilds.size()]};
    p.buildings = {ProvBuilding{blds[size_t(i) % blds.size()], 2, false, 0}, ProvBuilding{blds[size_t(i + 1) % blds.size()], 1, true, 2}};
    tx.add(std::move(p));
  }
  for (Id s : states) {
    for (int k = 0; k < 25; k++) {
      Id t = createTech(tx, s);
      tx.tech(t).modifiers = {mods[size_t(rng.range(0, 119))]};
      tx.tech(t).studied = k % 2 == 0;
    }
    for (int k = 0; k < 6; k++) addArmyRow(tx, s, UnitType(k), "", 1000, 2);
    tx.character(createCharacter(tx, s)).upkeep = 10;
  }
  for (int i = 0; i < 400; i++) {
    Army a;
    Id s = states[size_t(i) % states.size()];
    a.pos = {double(i), double(i)};
    a.groups.push_back(ArmyGroup{s, {ArmyUnit{tx.w().faction(s)->army[0].id, 10}}, {}});
    tx.add(std::move(a));
  }
  for (int i = 0; i < 300; i++) {
    Deal d;
    d.a = states[size_t(i) % states.size()];
    d.b = states[size_t(i + 1) % states.size()];
    d.items = {DealItem{DealSide::A, kGold, 5, DealMode::PerTurn, 10, 10}, DealItem{DealSide::B, 3, 2, DealMode::PerTurn, 10, 10}};
    tx.add(std::move(d));
  }
  World big = std::move(tx).finish();
  double t0 = nowSeconds();
  auto c = calc(big);
  double t1 = nowSeconds();
  auto c2 = calc(big);
  double t2 = nowSeconds();
  CHECK(c.get() == c2.get());
  CHECK_EQ(c->provinces.size(), size_t(3000));
  Tx tt(big);
  double t3 = nowSeconds();
  TurnReport rep = endTurn(tt);
  double t4 = nowSeconds();
  CHECK_EQ(rep.factions.size(), size_t(80));
  std::printf("  perf: calc 3000 провинций/80 фракций — %.1f мс, из кеша %.3f мс, endTurn %.1f мс\n", (t1 - t0) * 1000, (t2 - t1) * 1000,
              (t4 - t3) * 1000);
  CHECK((t1 - t0) < 1.0);
}
