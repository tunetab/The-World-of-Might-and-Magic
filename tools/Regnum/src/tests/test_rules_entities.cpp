// Тесты rules: создание и удаление сущностей с очисткой всех ссылок, справочники, владение и оккупация,
// удаление, объединение и разделение провинций, строки войск и флота.
#include "tests/test_rules_util.h"

using namespace rg;
using namespace rg::rules;
using namespace rg::rulestest;

namespace {

double provArea(const World& w, Id p) {
  auto fs = geo::buildFaces(w);
  auto* s = fs->shape(p);
  return s ? s->area : 0;
}

}  // namespace

TEST(rules_entities_create_defaults) {
  Fix f;
  Id s1 = 0, s2 = 0, g1 = 0;
  f.tx([&](Tx& tx) {
    s1 = createFaction(tx, FactionKind::State);
    s2 = createFaction(tx, FactionKind::State);
    g1 = createFaction(tx, FactionKind::Guild, "  Ганза  ");
  });
  CHECK_EQ(f.w().faction(s1)->name, std::string("Новое государство"));
  CHECK_EQ(f.w().faction(s2)->name, std::string("Новое государство 2"));
  CHECK_EQ(f.w().faction(g1)->name, std::string("Ганза"));
  CHECK(f.w().faction(g1)->isGuild());
  CHECK(f.w().faction(s1)->res.count(kGold) == 1);
  CHECK(f.w().faction(s1)->color != f.w().faction(s2)->color);
  Id c = 0, m = 0, b = 0, b2 = 0, t = 0, t2 = 0;
  f.tx([&](Tx& tx) {
    c = createCharacter(tx, f.A);
    m = createModifier(tx);
    b = createBuilding(tx, 0);
    b2 = createBuilding(tx, 0, "Мельница");
    t = createTech(tx, f.A);
    t2 = createTech(tx, f.A);
  });
  CHECK_EQ(f.w().character(c)->faction, f.A);
  CHECK_EQ(f.w().modifier(m)->name, std::string("Новый модификатор"));
  CHECK_EQ(f.w().building(b)->levels.size(), size_t(1));
  CHECK(f.w().building(b2)->pos.x > f.w().building(b)->pos.x);  // новые узлы схемы не накладываются
  CHECK(f.w().tech(t2)->pos.x > f.w().tech(t)->pos.x);
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { createBuilding(tx, f.G); }); }), "только у государств"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { createTech(tx, 999); }); }), "не найдена"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { createCharacter(tx, 999); }); }), "не найдена"));
}

TEST(rules_entities_remove_faction_cleanup) {
  Fix f;
  Id r1 = 0, armyA = 0, allied = 0, uniq = 0, common = 0, techA = 0, mod = 0, route = 0, deal = 0, sg = 0, ch = 0;
  f.tx([&](Tx& tx) {
    r1 = addArmyRow(tx, f.A, UnitType::LightInf, "", 100, 1);
    Province& p = tx.province(f.p[0]);
    p.owner = f.A;
    tx.faction(f.A).capital = f.p[0];
    setGarrison(tx, f.p[0], r1, 10);
    tx.province(f.p[1]).owner = f.B;
    setOccupied(tx, f.p[1], f.A);
    tx.province(f.p[2]).influence = {Influence{f.G, 10}};
    sg = createStateGuild(tx, f.A);
    tx.province(f.p[2]).influence.push_back(Influence{sg, 20});
    tx.province(f.p[2]).hqs = {sg, f.G};
    ch = createCharacter(tx, f.A, "Советник");
    mod = makeMod(tx, {{Fx::DiplomacyPerTurn, 2}}, {f.A, f.B});
    uniq = createBuilding(tx, f.A, "Уникальная");
    common = createBuilding(tx, 0, "Общая");
    tx.building(common).requires_ = {BuildingReq{uniq, 1}};
    tx.province(f.p[1]).buildings = {ProvBuilding{uniq, 1, false, 0}, ProvBuilding{common, 1, false, 0}};
    techA = createTech(tx, f.A, "Т");
    armyA = createArmy(tx, ArmyKind::Army, f.A, center(4));
    setUnits(tx, armyA, f.A, r1, 20);
    setRelation(tx, f.A, f.B, 50, RelStatus::Alliance);
    setRelation(tx, f.A, f.C, 10, RelStatus::Neutral);
    Id armyB = createArmy(tx, ArmyKind::Army, f.B, center(5));
    allied = createArmy(tx, ArmyKind::Army, f.A, center(6));
    formAllied(tx, armyB, allied);
    allied = armyB;
    tx.army(allied).commander = 0;
    route = createRoute(tx, {center(0), center(1)}, sg);
    deal = imposeTribute(tx, DealKind::Tribute, f.B, f.A, 5, 3);
  });
  f.tx([&](Tx& tx) { removeFaction(tx, f.A); });
  const World& w = f.w();
  CHECK(!w.faction(f.A));
  CHECK_EQ(w.province(f.p[0])->owner, Id(0));
  CHECK(w.province(f.p[0])->garrison.empty());
  CHECK(!w.province(f.p[1])->occupied);
  CHECK_EQ(w.province(f.p[1])->occupier, Id(0));
  CHECK_EQ(w.province(f.p[1])->owner, f.B);
  // Государственная гильдия осталась, но без государства.
  CHECK(w.faction(sg));
  CHECK_EQ(w.faction(sg)->homeState, Id(0));
  CHECK(!w.faction(sg)->stateGuild);
  CHECK_EQ(w.character(ch)->faction, Id(0));
  CHECK(w.modifier(mod)->targets == std::vector<Id>({f.B}));
  CHECK(!w.building(uniq));
  CHECK(w.building(common)->requires_.empty());
  CHECK_EQ(w.province(f.p[1])->buildings.size(), size_t(1));
  CHECK(!w.tech(techA));
  CHECK(!w.army(armyA));
  CHECK(w.army(allied));
  CHECK_EQ(w.army(allied)->groups.size(), size_t(1));
  CHECK_EQ(w.army(allied)->groups[0].faction, f.B);
  CHECK_EQ(w.route(route)->guild, sg);  // маршрут гильдии, не государства
  CHECK(!w.deal(deal));
  CHECK(w.relation(f.A, f.B) == Relation{});
  for (auto& [k, r] : *w.relations) {
    CHECK(Id(k >> 32) != f.A);
    CHECK(Id(k & 0xFFFFFFFFu) != f.A);
  }
  CHECK(has(lastLog(w), "Арден"));
  CHECK(geo::validate(w).empty());

  // Удаление гильдии: штабы, влияние, маршруты.
  f.tx([&](Tx& tx) { removeFaction(tx, sg); });
  CHECK(f.w().province(f.p[2])->hqs == std::vector<Id>({f.G}));
  CHECK_EQ(f.w().province(f.p[2])->influence.size(), size_t(1));
  CHECK_EQ(f.w().route(route)->guild, Id(0));
}

TEST(rules_entities_remove_character_cleanup) {
  Fix f;
  Id c = 0, other = 0, army = 0;
  f.tx([&](Tx& tx) {
    c = createCharacter(tx, f.A, "Герой");
    other = createCharacter(tx, f.A, "Другой");
    tx.province(f.p[0]).lord = c;
    tx.faction(f.A).ruler = c;
    tx.faction(f.A).council = {CouncilSeat{tx.nextId(Seq::Council), "Маршал", c}, CouncilSeat{tx.nextId(Seq::Council), "Казначей", other}};
    army = createArmy(tx, ArmyKind::Army, f.A, center(1));
    setHero(tx, army, other, true);
    setCommander(tx, army, c);
  });
  CHECK_EQ(f.w().army(army)->commander, c);
  CHECK_EQ(f.w().army(army)->groups[0].heroes.size(), size_t(2));
  f.tx([&](Tx& tx) { removeCharacter(tx, c); });
  const World& w = f.w();
  CHECK(!w.character(c));
  CHECK_EQ(w.province(f.p[0])->lord, Id(0));
  CHECK_EQ(w.faction(f.A)->ruler, Id(0));
  CHECK_EQ(w.faction(f.A)->council.size(), size_t(2));
  CHECK_EQ(w.faction(f.A)->council[0].character, Id(0));
  CHECK_EQ(w.faction(f.A)->council[0].position, std::string("Маршал"));
  CHECK_EQ(w.faction(f.A)->council[1].character, other);
  CHECK_EQ(w.army(army)->commander, Id(0));
  CHECK(w.army(army)->groups[0].heroes == std::vector<Id>({other}));
}

TEST(rules_entities_remove_modifier_tech_cleanup) {
  Fix f;
  Id m = 0, keep = 0, b = 0, t1 = 0, t2 = 0, t3 = 0;
  f.tx([&](Tx& tx) {
    m = makeMod(tx, {{Fx::TradePct, 5}});
    keep = makeMod(tx, {{Fx::TradePct, 1}});
    tx.province(f.p[0]).modifiers = {m, keep};
    tx.faction(f.A).modifiers = {m};
    b = createBuilding(tx, 0);
    tx.building(b).levels = {BuildingLevel{1, {}, {m}, ""}, BuildingLevel{1, {}, {keep, m}, ""}};
    t1 = createTech(tx, f.A);
    t2 = createTech(tx, f.A);
    t3 = createTech(tx, f.A);
    tx.tech(t1).modifiers = {m};
    setPrereq(tx, t2, t1, true);
    setPrereq(tx, t3, t1, true);
    setPrereq(tx, t3, t2, true);
  });
  f.tx([&](Tx& tx) { removeModifier(tx, m); });
  const World& w = f.w();
  CHECK(!w.modifier(m));
  CHECK(w.province(f.p[0])->modifiers == std::vector<Id>({keep}));
  CHECK(w.faction(f.A)->modifiers.empty());
  CHECK(w.building(b)->levels[0].modifiers.empty());
  CHECK(w.building(b)->levels[1].modifiers == std::vector<Id>({keep}));
  CHECK(w.tech(t1)->modifiers.empty());
  f.tx([&](Tx& tx) { removeTech(tx, t1); });
  CHECK(f.w().tech(t2)->prereqs.empty());
  CHECK(f.w().tech(t3)->prereqs == std::vector<Id>({t2}));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { removeModifier(tx, m); }); }), "не найден"));
}

TEST(rules_entities_remove_building_cleanup_refund) {
  Fix f;
  Id b = 0, req = 0;
  f.tx([&](Tx& tx) {
    tx.province(f.p[0]).owner = f.A;
    tx.faction(f.A).res[5] = 100;
    b = createBuilding(tx, 0, "Кузница");
    tx.building(b).levels[0].cost = {{5, 40}};
    tx.building(b).levels[0].turns = 3;
    req = createBuilding(tx, 0, "Оружейная");
    tx.building(req).requires_ = {BuildingReq{b, 1}};
    startBuilding(tx, f.p[0], b);
    tx.province(f.p[1]).buildings = {ProvBuilding{b, 1, false, 0}};
  });
  CHECK_NEAR(f.w().faction(f.A)->stock(5), 60, 1e-9);
  f.tx([&](Tx& tx) { removeBuilding(tx, b); });
  CHECK(!f.w().building(b));
  CHECK(f.w().province(f.p[0])->buildings.empty());
  CHECK(f.w().province(f.p[1])->buildings.empty());
  CHECK(f.w().building(req)->requires_.empty());
  CHECK_NEAR(f.w().faction(f.A)->stock(5), 100, 1e-9);  // незавершённое строительство возвращено
  CHECK(has(lastLog(f.w(), "прекращено"), "Кузница"));
}

TEST(rules_entities_copy_tech_tree) {
  Fix f;
  Id t1 = 0, t2 = 0;
  f.tx([&](Tx& tx) {
    t1 = createTech(tx, f.A, "Письменность");
    t2 = createTech(tx, f.A, "Право");
    tx.tech(t2).turns = 4;
    setPrereq(tx, t2, t1, true);
    tx.tech(t1).studied = true;
    tx.tech(t2).research = true;
    tx.tech(t2).progress = 2;
    createTech(tx, f.B, "Своя");
  });
  f.tx([&](Tx& tx) { copyTechTree(tx, f.A, f.B); });
  std::vector<const Tech*> bt;
  f.w().techs.each([&](const Tech& t) { if (t.faction == f.B) bt.push_back(&t); });
  CHECK_EQ(bt.size(), size_t(3));
  const Tech* c1 = bt[1];
  const Tech* c2 = bt[2];
  CHECK_EQ(c1->name, std::string("Письменность"));
  CHECK(!c1->studied);
  CHECK(!c2->research);
  CHECK_EQ(c2->progress, 0);
  CHECK_EQ(c2->turns, 4);
  CHECK(c2->prereqs == std::vector<Id>({c1->id}));  // связи переназначены на копии
  CHECK(c1->pos.y > bt[0]->pos.y);                   // ниже существующего дерева
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { copyTechTree(tx, f.C, f.A); }); }), "нет технологий"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { copyTechTree(tx, f.A, f.A); }); }), "ту же"));
}

TEST(rules_entities_catalogs) {
  Fix f;
  Id salt = 0, race = 0, keepRace = 0, cult = 0, rel = 0, gov = 0, pos = 0, b = 0, deal = 0, mixed = 0;
  f.tx([&](Tx& tx) {
    salt = addCatalogItem(tx, CatalogList::Resources, "Соль");
    race = addCatalogItem(tx, CatalogList::Races, "Орки");
    keepRace = addCatalogItem(tx, CatalogList::Races, "Люди");
    cult = addCatalogItem(tx, CatalogList::Cultures, "");
    rel = addCatalogItem(tx, CatalogList::Religions, "Солнце");
    gov = addCatalogItem(tx, CatalogList::Governments, "Совет");
    pos = addCatalogItem(tx, CatalogList::Positions, "Летописец");
  });
  const Catalogs& c = *f.w().catalogs;
  CHECK_EQ(c.resources.back().id, salt);
  CHECK(salt > 6);  // последовательность продолжена после встроенных
  CHECK_EQ(Catalogs::find(c.cultures, cult)->name, std::string("Новая культура"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { addCatalogItem(tx, CatalogList::Resources, "соль"); }); }), "уже есть"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { removeCatalogItem(tx, CatalogList::Resources, kGold); }); }), "Золото"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { removeCatalogItem(tx, CatalogList::Races, 9999); }); }), "не найдена"));

  f.tx([&](Tx& tx) {
    Province& p = tx.province(f.p[0]);
    p.resource = salt;
    p.races = {RacePop{race, 10}, RacePop{keepRace, 5}};
    p.culture = cult;
    p.religion = rel;
    Faction& a = tx.faction(f.A);
    a.res[salt] = 30;
    a.res[5] = 40;
    a.culture = cult;
    a.religion = rel;
    a.government = gov;
    b = createBuilding(tx, 0);
    tx.building(b).levels[0].cost = {{salt, 5}, {5, 1}};
    Deal d;
    d.a = f.A;
    d.b = f.B;
    d.items = {DealItem{DealSide::A, salt, 1, DealMode::PerTurn, 3, 0}};
    deal = concludeDeal(tx, d);
    d.items.push_back(DealItem{DealSide::A, 5, 1, DealMode::PerTurn, 3, 0});
    mixed = concludeDeal(tx, d);
  });
  f.tx([&](Tx& tx) {
    removeCatalogItem(tx, CatalogList::Resources, salt);
    removeCatalogItem(tx, CatalogList::Races, race);
    removeCatalogItem(tx, CatalogList::Cultures, cult);
    removeCatalogItem(tx, CatalogList::Religions, rel);
    removeCatalogItem(tx, CatalogList::Governments, gov);
    removeCatalogItem(tx, CatalogList::Positions, pos);
  });
  const World& w = f.w();
  CHECK(!w.resource(salt));
  CHECK_EQ(w.province(f.p[0])->resource, Id(0));
  CHECK_EQ(w.province(f.p[0])->races.size(), size_t(1));
  CHECK_EQ(w.province(f.p[0])->culture, Id(0));
  CHECK_EQ(w.province(f.p[0])->religion, Id(0));
  CHECK(w.faction(f.A)->res.count(salt) == 0);
  CHECK_NEAR(w.faction(f.A)->stock(5), 40, 1e-9);
  CHECK_EQ(w.faction(f.A)->culture, Id(0));
  CHECK_EQ(w.faction(f.A)->religion, Id(0));
  CHECK_EQ(w.faction(f.A)->government, Id(0));
  CHECK(w.building(b)->levels[0].cost.count(salt) == 0);
  CHECK_EQ(w.building(b)->levels[0].cost.size(), size_t(1));
  CHECK(w.deal(deal)->items.empty());
  CHECK(w.deal(deal)->status == DealStatus::Cancelled);
  CHECK_EQ(w.deal(mixed)->items.size(), size_t(1));
  CHECK(w.deal(mixed)->status == DealStatus::Active);
  CHECK(!Catalogs::find(w.catalogs->positions, pos));
}

TEST(rules_entities_province_owner) {
  Fix f;
  Id r1 = 0;
  f.tx([&](Tx& tx) {
    r1 = addArmyRow(tx, f.A, UnitType::LightInf, "", 50, 1);
    setProvinceOwner(tx, f.p[0], f.A);
    setCapital(tx, f.A, f.p[0]);
    setGarrison(tx, f.p[0], r1, 20);
    setOccupied(tx, f.p[0], f.B);
  });
  CHECK_EQ(f.fc(f.A).army[0].reserve, 30);
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setProvinceOwner(tx, f.p[0], f.G); }); }), "гильдия"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setProvinceOwner(tx, f.seaW, f.A); }); }), "морской"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setCapital(tx, f.B, f.p[0]); }); }), "самого государства"));
  int logs = int(f.w().log.size());
  f.tx([&](Tx& tx) { setProvinceOwner(tx, f.p[0], f.B); });
  const World& w = f.w();
  CHECK_EQ(w.province(f.p[0])->owner, f.B);
  CHECK(w.province(f.p[0])->garrison.empty());   // гарнизон распущен в резерв прежнего владельца
  CHECK_EQ(f.fc(f.A).army[0].reserve, 50);
  CHECK_EQ(w.faction(f.A)->capital, Id(0));       // столица прежнего владельца снята
  CHECK(!w.province(f.p[0])->occupied);           // новый владелец — оккупант
  CHECK_EQ(w.province(f.p[0])->occupier, Id(0));
  CHECK_EQ(int(w.log.size()), logs + 1);
  CHECK(has(lastLog(w), "перешла к «Бельмар»"));
  // Тот же владелец — без изменений и без записи.
  f.tx([&](Tx& tx) { setProvinceOwner(tx, f.p[0], f.B); });
  CHECK_EQ(int(f.w().log.size()), logs + 1);
  f.tx([&](Tx& tx) { setProvinceOwner(tx, f.p[0], 0); });
  CHECK(has(lastLog(f.w()), "без владельца"));
}

TEST(rules_entities_occupation) {
  Fix f;
  f.tx([&](Tx& tx) { tx.province(f.p[0]).owner = f.A; });
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setOccupied(tx, f.p[0], f.A); }); }), "собственную"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setOccupied(tx, f.p[0], f.G); }); }), "гильдия"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setOccupied(tx, f.seaW, f.B); }); }), "Морскую"));
  f.tx([&](Tx& tx) { setOccupied(tx, f.p[0], f.B); });
  CHECK(f.w().province(f.p[0])->occupied);
  CHECK_EQ(f.w().province(f.p[0])->occupier, f.B);
  CHECK(has(lastLog(f.w()), "оккупирована"));
  f.tx([&](Tx& tx) { setOccupied(tx, f.p[0], 0); });
  CHECK(!f.w().province(f.p[0])->occupied);
  CHECK_EQ(f.w().province(f.p[0])->occupier, Id(0));
  CHECK(has(lastLog(f.w()), "снята"));
}

TEST(rules_entities_delete_province) {
  Fix f;
  f.tx([&](Tx& tx) {
    tx.province(f.p[0]).owner = f.A;
    tx.faction(f.A).capital = f.p[0];
  });
  f.tx([&](Tx& tx) { deleteProvince(tx, f.p[0]); });
  const World& w = f.w();
  CHECK(!w.province(f.p[0]));
  CHECK_EQ(w.faction(f.A)->capital, Id(0));
  auto fs = geo::buildFaces(w);
  CHECK_EQ(fs->provinceAt(center(0)), Id(0));
  CHECK(fs->terrainAt(center(0)) == Terrain::Land);
  w.edges.each([&](const Edge& e) {
    CHECK(e.pl != f.p[0]);
    CHECK(e.pr != f.p[0]);
  });
  CHECK(geo::validate(w).empty());
  CHECK(has(lastLog(w), "удалена"));
}

TEST(rules_entities_merge_provinces) {
  Fix f;
  Id human = 0, elf = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, r1 = 0;
  Id g[6]{};
  f.tx([&](Tx& tx) {
    human = addCatalogItem(tx, CatalogList::Races, "Люди");
    elf = addCatalogItem(tx, CatalogList::Races, "Эльфы");
    for (int i = 0; i < 6; i++) g[i] = createFaction(tx, FactionKind::Guild);
    b1 = createBuilding(tx, 0, "Б1");
    b2 = createBuilding(tx, 0, "Б2");
    b3 = createBuilding(tx, 0, "Б3");
    b4 = createBuilding(tx, 0, "Б4");
    r1 = addArmyRow(tx, f.A, UnitType::LightInf, "", 100, 1);
    Province& t = tx.province(f.p[0]);
    t.name = "Цель";
    t.owner = f.A;
    t.size = ProvSize::Small;  // 1 + 1 = 2 слота
    t.baseTrade = 77;
    t.races = {RacePop{human, 100}};
    t.buildings = {ProvBuilding{b1, 1, false, 0}};
    t.hqs = {g[0], g[1], g[2], g[3]};
    t.influence = {Influence{g[0], 60}};
    Province& s = tx.province(f.p[1]);
    s.owner = f.A;
    s.baseTrade = 500;
    s.races = {RacePop{human, 50}, RacePop{elf, 20}};
    s.buildings = {ProvBuilding{b1, 1, false, 0}, ProvBuilding{b2, 1, true, 2}, ProvBuilding{b3, 1, false, 0}};
    s.hqs = {g[3], g[4], g[5]};
    s.influence = {Influence{g[4], 40}, Influence{g[5], 40}};
    tx.faction(f.A).capital = f.p[1];
    setGarrison(tx, f.p[0], r1, 10);
    setGarrison(tx, f.p[1], r1, 5);
  });
  double a0 = provArea(f.w(), f.p[0]), a1 = provArea(f.w(), f.p[1]);
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { mergeProvinces(tx, f.p[0], f.p[0]); }); }), "самой собой"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { mergeProvinces(tx, f.p[0], f.seaW); }); }), "морскую"));
  f.tx([&](Tx& tx) { mergeProvinces(tx, f.p[0], f.p[1]); });
  const World& w = f.w();
  CHECK(!w.province(f.p[1]));
  const Province& t = *w.province(f.p[0]);
  CHECK_EQ(t.name, std::string("Цель"));
  CHECK_NEAR(t.baseTrade, 77, 1e-9);  // поля цели сохраняются
  CHECK_EQ(t.races.size(), size_t(2));
  CHECK_EQ(t.races[0].pop, 150);       // численности рас складываются
  CHECK_EQ(t.races[1].race, elf);
  CHECK_EQ(t.races[1].pop, 20);
  // Слоты цели: 2 → Б1 был, Б2 переносится, Б3 не помещается; повтор Б1 сносится.
  CHECK_EQ(t.buildings.size(), size_t(2));
  CHECK_EQ(t.buildings[1].building, b2);
  CHECK(t.buildings[1].constructing);
  CHECK_EQ(t.buildings[1].left, 2);
  CHECK_EQ(t.hqs.size(), size_t(5));   // не больше пяти штабов
  CHECK(t.hqs[4] == g[4]);
  double inf = 0;
  for (auto& i : t.influence) inf += i.pct;
  CHECK_NEAR(inf, 100, 1e-9);          // новые гильдии заняли остаток до 100 %
  CHECK_NEAR(t.influence[0].pct, 60, 1e-9);
  CHECK_NEAR(t.influence[1].pct, 20, 1e-9);
  CHECK_EQ(t.garrison.size(), size_t(1));
  CHECK_EQ(t.garrison[0].count, 15);   // гарнизон того же владельца сложен
  CHECK_EQ(w.faction(f.A)->capital, f.p[0]);
  CHECK_NEAR(provArea(w, f.p[0]), a0 + a1, 1e-6);
  CHECK(geo::validate(w).empty());
  std::string l = lastLog(w);
  CHECK(has(l, "присоединена"));
  CHECK(has(l, "«Б3»"));
  CHECK(has(l, "нехватки слотов"));
  CHECK(has(l, "повторные: «Б1»"));
  CHECK(has(l, "штабы"));
  (void)b4;
}

TEST(rules_entities_split_province) {
  Fix f;
  f.tx([&](Tx& tx) {
    Province& p = tx.province(f.p[0]);
    p.owner = f.A;
    p.culture = 3;
    p.religion = 4;
    p.contentment = 25;
  });
  double before = provArea(f.w(), f.p[0]);
  Id nid = 0;
  f.tx([&](Tx& tx) { nid = splitProvince(tx, f.p[0], {{150, 50}, {150, 550}}); });
  const World& w = f.w();
  const Province* n = w.province(nid);
  CHECK(n != nullptr);
  CHECK_EQ(n->owner, f.A);
  CHECK_EQ(n->culture, Id(3));
  CHECK_EQ(n->religion, Id(4));
  CHECK_NEAR(n->contentment, 25, 1e-9);
  CHECK_EQ(n->name, std::string("П0 (часть)"));
  CHECK_NEAR(provArea(w, f.p[0]) + provArea(w, nid), before, 1e-6);
  CHECK(provArea(w, nid) < provArea(w, f.p[0]));
  CHECK(geo::validate(w).empty());
  CHECK(has(lastLog(w), "разделена"));
}

TEST(rules_entities_rows) {
  Fix f;
  Id r1 = 0, r2 = 0, s1 = 0, army = 0, fleet = 0;
  f.tx([&](Tx& tx) {
    r1 = addArmyRow(tx, f.A, UnitType::Ranged, "", 100, 2);
    r2 = addArmyRow(tx, f.A, UnitType::Monsters, "Тролли", 10, 50);
    s1 = addFleetRow(tx, f.A, ShipType::ShipOfLine, "", 4, 30);
    tx.province(f.p[0]).owner = f.A;
    army = createArmy(tx, ArmyKind::Army, f.A, center(1));
    setUnits(tx, army, f.A, r1, 40);
    setUnits(tx, army, f.A, r2, 5);
    setGarrison(tx, f.p[0], r1, 30);
    fleet = createArmy(tx, ArmyKind::Fleet, f.A, kSeaNorth);
    setUnits(tx, fleet, f.A, s1, 3);
  });
  CHECK(r1 != r2 && r2 != s1);
  CHECK_EQ(f.w().faction(f.A)->armyRow(r1)->name, std::string("Стрелки"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setRowTotal(tx, f.A, r1, 69); }); }), "в поле 70"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { setRowTotal(tx, f.A, s1, 2); }); }), "в море 3"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { addArmyRow(tx, f.A, UnitType::Ranged, "", -1, 0); }); }), "Численность"));
  CHECK(has(errorOf([&] { f.tx([&](Tx& tx) { addFleetRow(tx, f.A, ShipType::Frigate, "", 1, -3); }); }), "отрицательным"));
  f.tx([&](Tx& tx) { setRowTotal(tx, f.A, r1, 70); });
  CHECK_EQ(f.w().faction(f.A)->armyRow(r1)->total, 70);
  f.tx([&](Tx& tx) {
    removeRow(tx, f.A, r1);
    removeRow(tx, f.A, s1);
  });
  const World& w = f.w();
  CHECK(!w.faction(f.A)->armyRow(r1));
  CHECK(w.province(f.p[0])->garrison.empty());
  CHECK_EQ(w.army(army)->groups[0].units.size(), size_t(1));
  CHECK_EQ(w.army(army)->groups[0].units[0].row, r2);
  CHECK(w.army(fleet)->groups[0].units.empty());
  CHECK(w.faction(f.A)->fleet.empty());
}
