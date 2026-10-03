// Тесты core/io — общие помощники: мир со всеми сущностями и полями, поэлементное сравнение миров,
// временные папки.
#pragma once
#include "base/fs.h"
#include "codec/png.h"
#include "core/io.h"
#include "core/schema.h"
#include "tests/test.h"

namespace rg::iotest {

// Временная папка теста (очищается при создании).
inline std::string tempDir(const std::string& name) {
  std::string d = fs::absolute(fs::join(fs::join(test::outDir(), "io"), name));
  fs::removeAll(d);
  fs::makeDirs(d);
  return d;
}

inline std::string pngBytes(int w, int h, u8 seed) {
  codec::RgbaImage img;
  img.w = w;
  img.h = h;
  img.rgba.resize(size_t(w) * size_t(h) * 4);
  for (size_t i = 0; i < img.rgba.size(); i++) img.rgba[i] = u8(i * 31 + seed);
  auto b = codec::encodePng(img, 6);
  return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

// Байты «JPEG»: сигнатура и все 256 значений байта (включая 0) — проверка двоичной точности.
inline std::string jpegLikeBytes() {
  std::string s = "\xFF\xD8\xFF\xE0";
  for (int k = 0; k < 3; k++)
    for (int i = 0; i < 256; i++) s.push_back(char(i));
  s += "\xFF\xD9";
  return s;
}

// Мир, в котором заполнено каждое поле каждой сущности (значения уже нормализованы).
inline World richWorld() {
  World base = newWorld("Мир Ардена 🐉 «тест»");
  Tx tx(base);
  Meta& m = tx.meta();
  m.createdAt = "2026-09-01T10:00:00Z";
  m.updatedAt = "2026-10-01T11:22:33Z";
  m.turn = 7;
  m.basemap = "wmm-expanded-v1";
  m.notes = "Заметки мира\nвторая строка\t«кавычки» \"двойные\" \\ обратная черта 🏰";

  Settings& s = tx.settings();
  s.fillOpacity = 0.3f;
  s.labelStates = false;
  s.labelProvinces = false;
  s.labelArmies = false;
  s.occupiedIncome = OccupiedIncome::Occupier;
  s.rebellionRoll = true;
  s.autosaveFolder = false;
  s.autosaveSec = 120;

  Catalogs& c = tx.catalogs();
  auto item = [](Id id, const char* n, u32 color, const char* icon) {
    CatalogItem it;
    it.id = id;
    it.name = n;
    it.color = Color::hex(color);
    it.icon = icon;
    return it;
  };
  c.resources.push_back(item(7, "Мифрил ✨", 0x9fd3ff, "gem"));
  c.races = {item(1, "Люди", 0xd9b38c, "person"), item(2, "Эльфы 🧝", 0x7fbf7f, "leaf")};
  c.cultures = {item(1, "Северная", 0x6688aa, "snow"), item(3, "Южная", 0xaa8866, "sun")};
  c.religions = {item(2, "Культ Солнца", 0xffcc33, "sun")};
  c.governments[1].color = Color(10, 20, 30, 128);  // полупрозрачный цвет
  m.seq[int(Seq::Resource)] = 7;
  m.seq[int(Seq::Race)] = 2;
  m.seq[int(Seq::Culture)] = 3;
  m.seq[int(Seq::Religion)] = 2;

  // Геометрия: квадрат из четырёх узлов, одна дуга с промежуточными точками.
  tx.add(Node{1, {100.25, 200.5}});
  tx.add(Node{2, {300, 200.5}});
  tx.add(Node{3, {300, 400.75}});
  tx.add(Node{4, {100.25, 400.75}});

  Faction f1;
  f1.id = 1;
  f1.kind = FactionKind::State;
  f1.name = "Королевство Арден 👑";
  f1.color = Color::hex(0x3366cc);
  f1.flag.image = true;
  f1.flag.pattern = FlagPattern::Saltire;
  f1.flag.colors = {Color::hex(0x112233), Color::hex(0x445566), Color::hex(0x778899)};
  f1.flag.emblem = "lion";
  f1.flag.emblemColor = Color::hex(0xffd700);
  f1.flag.png = pngBytes(6, 4, 3);
  f1.culture = 1;
  f1.government = 2;
  f1.religion = 2;
  f1.ruler = 1;
  f1.rulerTitle = "Король";
  f1.council = {{1, "Канцлер", 2}, {2, "Маршал", 0}};
  f1.capital = 1;
  f1.army = {{1, "Гвардия", UnitType::HeavyInf, 1200, 1.5}, {2, "Конные лучники", UnitType::LightCav, 300, 2.25}};
  f1.fleet = {{3, "Северная эскадра", ShipType::ShipOfLine, 12, 40}};
  f1.res = {{kGold, 15000.5}, {2, 320}, {7, 3}};
  f1.modifiers = {1};
  f1.tax = 15;
  f1.notes = "Заметки фракции";
  f1.entity = "FAC-0001";
  tx.add(f1);

  Faction f2;
  f2.id = 2;
  f2.kind = FactionKind::Guild;
  f2.name = "Гильдия Серебряного пути";
  f2.color = Color::hex(0xc0c0c0);
  f2.flag.pattern = FlagPattern::Chevron;
  f2.homeState = 1;
  f2.stateGuild = true;
  f2.res = {{kGold, 900}};
  f2.tax = 0;
  tx.add(f2);

  Faction f3;
  f3.id = 3;
  f3.name = "Империя Зар";
  f3.color = Color::hex(0x992222);
  f3.fleet = {{4, "Пиратский флот", ShipType::Galleon, 5, 10}};
  f3.army = {{5, "Орда", UnitType::Monsters, 50, 7}};
  tx.add(f3);
  m.seq[int(Seq::Row)] = 5;
  m.seq[int(Seq::Council)] = 2;

  Character c1;
  c1.id = 1;
  c1.name = "Эдмунд Арденский";
  c1.title = "Король";
  c1.faction = 1;
  c1.hero = true;
  c1.upkeep = 12.5;
  c1.portrait = jpegLikeBytes();
  c1.notes = "Заметки";
  c1.entity = "CHR-0001";
  tx.add(c1);
  Character c2;
  c2.id = 2;
  c2.name = "Сир Бертран";
  c2.faction = 1;
  c2.portrait = pngBytes(3, 3, 9);
  tx.add(c2);
  Character c3;
  c3.id = 3;
  c3.name = "Адмирал Кейл";
  c3.faction = 3;
  tx.add(c3);

  Modifier m1;
  m1.id = 1;
  m1.name = "Торговый бум";
  m1.icon = "trade";
  m1.color = Color::hex(0x33aa55);
  m1.desc = "Описание";
  m1.fx[int(Fx::TradePct)] = 10;
  m1.fx[int(Fx::DiplomacyPerTurn)] = -5;
  m1.fx[int(Fx::Slots)] = 2;
  m1.fxMask = (1u << int(Fx::TradePct)) | (1u << int(Fx::DiplomacyPerTurn)) | (1u << int(Fx::Slots));
  m1.targets = {3};
  tx.add(m1);
  Modifier m2;
  m2.id = 2;
  m2.name = "Чума";
  m2.fx[int(Fx::PopGrowthPct)] = -12.5;
  m2.fxMask = 1u << int(Fx::PopGrowthPct);
  tx.add(m2);

  Building b1;
  b1.id = 1;
  b1.name = "Рынок";
  b1.icon = "market";
  b1.cat = BuildingCat::Economic;
  b1.desc = "Торговля";
  BuildingLevel l1;
  l1.turns = 2;
  l1.cost = {{kGold, 100}, {2, 5}};
  l1.modifiers = {1};
  l1.desc = "Уровень 1";
  BuildingLevel l2;
  l2.turns = 4;
  l2.cost = {{kGold, 250.5}};
  l2.desc = "Уровень 2";
  b1.levels = {l1, l2};
  b1.pos = {12.5, 40};
  tx.add(b1);
  Building b2;
  b2.id = 2;
  b2.owner = 1;
  b2.name = "Королевская кузница";
  b2.cat = BuildingCat::Military;
  b2.requires_ = {{1, 2}};
  b2.pos = {200, 40.25};
  tx.add(b2);

  Tech t1;
  t1.id = 1;
  t1.faction = 1;
  t1.name = "Мореходство";
  t1.desc = "Корабли";
  t1.turns = 3;
  t1.modifiers = {2};
  t1.studied = true;
  t1.progress = 3;
  t1.pos = {0, 0};
  tx.add(t1);
  Tech t2;
  t2.id = 2;
  t2.faction = 1;
  t2.name = "Навигация";
  t2.turns = 5;
  t2.prereqs = {1};
  t2.research = true;
  t2.progress = 2;
  t2.pos = {150.5, 80};
  tx.add(t2);

  Province p1;
  p1.id = 1;
  p1.name = "Арден";
  p1.owner = 1;
  p1.lord = 1;
  p1.capital = "Арденбург";
  p1.size = ProvSize::Large;
  p1.city = CityType::City;
  p1.resource = 2;
  p1.resourceAmount = 40.5;
  p1.garrison = {{1, 200}, {2, 50}};
  p1.contentment = -12.5;
  p1.culture = 1;
  p1.religion = 2;
  p1.races = {{1, 120000}, {2, 3500}};
  p1.influence = {{2, 35.5}};
  p1.hqs = {2};
  p1.baseTrade = 250;
  p1.localTax = -2.5;
  p1.buildings = {{1, 2, true, 3, {{kGold, 120.5}, {2, 40}}, 1}, {2, 1, false, 0}};  // строится: уплачено государством f1
  p1.modifiers = {1, 2};
  p1.notes = "Провинция\r\nс CRLF";
  p1.entity = "PRV-0001";
  tx.add(p1);
  Province p2;
  p2.id = 2;
  p2.name = "Море Бурь";
  p2.sea = true;
  p2.size = ProvSize::Small;
  p2.city = CityType::Outpost;
  tx.add(p2);
  Province p3;
  p3.id = 3;
  p3.name = "Зарград";
  p3.owner = 1;
  p3.occupied = true;
  p3.occupier = 3;
  p3.city = CityType::Town;
  tx.add(p3);

  Edge e1;
  e1.id = 1;
  e1.a = 1;
  e1.b = 2;
  e1.pts = {{150.5, 190.25}, {250, 210}};
  e1.kind = EdgeKind::Coast;
  e1.pl = 1;
  e1.pr = 2;
  e1.tl = Terrain::Land;
  e1.tr = Terrain::Sea;
  tx.add(e1);
  Edge e2;
  e2.id = 2;
  e2.a = 2;
  e2.b = 3;
  e2.kind = EdgeKind::Frame;
  e2.pl = 3;
  e2.tl = Terrain::None;
  tx.add(e2);

  Army a1;
  a1.id = 1;
  a1.kind = ArmyKind::Army;
  a1.name = "Первая армия";
  a1.pos = {210.5, 300.25};
  a1.groups = {ArmyGroup{1, {{1, 500}, {2, 100}}, {1}}};
  a1.commander = 1;
  tx.add(a1);
  Army a2;
  a2.id = 2;
  a2.kind = ArmyKind::Fleet;
  a2.name = "Союзный флот";
  a2.pos = {500, 1000};
  a2.groups = {ArmyGroup{1, {{3, 6}}, {2}}, ArmyGroup{3, {{4, 2}}, {3}}};
  a2.commander = 3;
  tx.add(a2);

  Route r1;
  r1.id = 1;
  r1.name = "Серебряный путь";
  r1.guild = 2;
  r1.pts = {{100, 100}, {200.5, 150.25}, {400, 300}};
  r1.color = Color::hex(0xaabbcc);
  tx.add(r1);
  Route r2;
  r2.id = 2;
  r2.pts = {{10, 10}, {20, 20}};
  tx.add(r2);

  Deal d1;
  d1.id = 1;
  d1.kind = DealKind::Trade;
  d1.a = 1;
  d1.b = 3;
  d1.items = {DealItem{DealSide::A, kGold, 500, DealMode::Once, 1, 0}, DealItem{DealSide::B, 7, 2.5, DealMode::PerTurn, 5, 3}};
  d1.turn = 4;
  d1.status = DealStatus::Done;
  d1.note = "Сделка";
  tx.add(d1);
  Deal d2;
  d2.id = 2;
  d2.kind = DealKind::Tribute;
  d2.a = 3;
  d2.b = 1;
  d2.items = {DealItem{DealSide::B, kGold, 100, DealMode::PerTurn, 10, 10}};
  d2.status = DealStatus::Cancelled;
  tx.add(d2);

  LogEntry g1;
  g1.id = 1;
  g1.turn = 6;
  g1.kind = LogKind::Battle;
  g1.text = "Битва при Зарграде ⚔";
  g1.province = 3;
  g1.army = 1;
  g1.factions = {1, 3};
  g1.at = "2026-09-30T20:00:00Z";
  tx.add(g1);
  LogEntry g2;
  g2.id = 5;
  g2.turn = 7;
  g2.kind = LogKind::Note;
  g2.text = "Запись";
  g2.army = 99;  // войско давно расформировано: в хронике это допустимо
  tx.add(g2);

  tx.setRelation(1, 3, Relation{-25, RelStatus::War});
  tx.setRelation(1, 2, Relation{50.5, RelStatus::Alliance});
  tx.setRelation(2, 3, Relation{0, RelStatus::Neutral});
  return std::move(tx).finish();
}

// ---------------------------------------------------------------- поэлементное сравнение
#define RG_D(cond, what) \
  do {                   \
    if (!(cond)) return std::string(what); \
  } while (0)

inline std::string diffIds(const std::vector<Id>& a, const std::vector<Id>& b, const std::string& w) { return a == b ? "" : w; }
inline bool sameCat(const std::vector<CatalogItem>& a, const std::vector<CatalogItem>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (a[i].id != b[i].id || a[i].name != b[i].name || !(a[i].color == b[i].color) || a[i].icon != b[i].icon || a[i].builtin != b[i].builtin)
      return false;
  return true;
}

template <class T, class F>
std::string diffTable(const Table<T>& a, const Table<T>& b, const char* name, F&& eq) {
  RG_D(a.size() == b.size(), std::string(name) + ": число записей " + std::to_string(a.size()) + " != " + std::to_string(b.size()));
  std::string out;
  a.each([&](const T& x) {
    if (!out.empty()) return;
    const T* y = b.get(x.id);
    if (!y) out = std::string(name) + " " + std::to_string(x.id) + ": нет во втором мире";
    else if (std::string d = eq(x, *y); !d.empty()) out = std::string(name) + " " + std::to_string(x.id) + ": " + d;
  });
  return out;
}

// Пустая строка — миры равны во всех полях.
inline std::string diffWorld(const World& a, const World& b) {
  const Meta &ma = *a.meta, &mb = *b.meta;
  RG_D(ma.name == mb.name && ma.createdAt == mb.createdAt && ma.updatedAt == mb.updatedAt && ma.turn == mb.turn && ma.seq == mb.seq &&
           ma.basemap == mb.basemap && ma.notes == mb.notes, "meta");
  const Settings &sa = *a.settings, &sb = *b.settings;
  RG_D(sa.fillOpacity == sb.fillOpacity && sa.labelStates == sb.labelStates && sa.labelProvinces == sb.labelProvinces &&
           sa.labelArmies == sb.labelArmies && sa.occupiedIncome == sb.occupiedIncome && sa.rebellionRoll == sb.rebellionRoll &&
           sa.autosaveFolder == sb.autosaveFolder && sa.autosaveSec == sb.autosaveSec, "settings");
  const Catalogs &ca = *a.catalogs, &cb = *b.catalogs;
  RG_D(sameCat(ca.resources, cb.resources) && sameCat(ca.races, cb.races) && sameCat(ca.cultures, cb.cultures) &&
           sameCat(ca.religions, cb.religions) && sameCat(ca.governments, cb.governments) && sameCat(ca.positions, cb.positions), "catalogs");
  RG_D(*a.relations == *b.relations, "relations");
  std::string d;
  d = diffTable(a.nodes, b.nodes, "node", [](const Node& x, const Node& y) { return x.p == y.p ? "" : "p"; });
  if (!d.empty()) return d;
  d = diffTable(a.edges, b.edges, "edge", [](const Edge& x, const Edge& y) {
    return x.a == y.a && x.b == y.b && x.pts == y.pts && x.kind == y.kind && x.pl == y.pl && x.pr == y.pr && x.tl == y.tl && x.tr == y.tr ? "" : "поля";
  });
  if (!d.empty()) return d;
  d = diffTable(a.provinces, b.provinces, "province", [](const Province& x, const Province& y) -> std::string {
    RG_D(x.name == y.name && x.owner == y.owner && x.lord == y.lord && x.sea == y.sea && x.capital == y.capital && x.size == y.size &&
             x.city == y.city && x.resource == y.resource && x.resourceAmount == y.resourceAmount, "основные поля");
    RG_D(x.garrison.size() == y.garrison.size(), "garrison");
    for (size_t i = 0; i < x.garrison.size(); i++) RG_D(x.garrison[i].row == y.garrison[i].row && x.garrison[i].count == y.garrison[i].count, "garrison");
    RG_D(x.contentment == y.contentment && x.culture == y.culture && x.religion == y.religion, "contentment/culture/religion");
    RG_D(x.races.size() == y.races.size(), "races");
    for (size_t i = 0; i < x.races.size(); i++) RG_D(x.races[i].race == y.races[i].race && x.races[i].pop == y.races[i].pop, "races");
    RG_D(x.influence.size() == y.influence.size(), "influence");
    for (size_t i = 0; i < x.influence.size(); i++) RG_D(x.influence[i].guild == y.influence[i].guild && x.influence[i].pct == y.influence[i].pct, "influence");
    RG_D(x.hqs == y.hqs && x.baseTrade == y.baseTrade && x.localTax == y.localTax, "hqs/trade/tax");
    RG_D(x.buildings.size() == y.buildings.size(), "buildings");
    for (size_t i = 0; i < x.buildings.size(); i++) {
      auto &p = x.buildings[i], &q = y.buildings[i];
      RG_D(p.building == q.building && p.level == q.level && p.constructing == q.constructing && p.left == q.left && p.paid == q.paid &&
               p.payer == q.payer,
           "buildings");
    }
    RG_D(x.modifiers == y.modifiers && x.occupied == y.occupied && x.occupier == y.occupier && x.notes == y.notes && x.entity == y.entity, "прочие поля");
    return "";
  });
  if (!d.empty()) return d;
  d = diffTable(a.factions, b.factions, "faction", [](const Faction& x, const Faction& y) -> std::string {
    RG_D(x.kind == y.kind && x.name == y.name && x.color == y.color, "kind/name/color");
    RG_D(x.flag.image == y.flag.image && x.flag.pattern == y.flag.pattern && x.flag.colors == y.flag.colors && x.flag.emblem == y.flag.emblem &&
             x.flag.emblemColor == y.flag.emblemColor && x.flag.png == y.flag.png, "flag");
    RG_D(x.culture == y.culture && x.government == y.government && x.religion == y.religion && x.ruler == y.ruler && x.rulerTitle == y.rulerTitle, "ruler");
    RG_D(x.council.size() == y.council.size(), "council");
    for (size_t i = 0; i < x.council.size(); i++)
      RG_D(x.council[i].id == y.council[i].id && x.council[i].position == y.council[i].position && x.council[i].character == y.council[i].character, "council");
    RG_D(x.capital == y.capital, "capital");
    RG_D(x.army.size() == y.army.size() && x.fleet.size() == y.fleet.size(), "rows");
    for (size_t i = 0; i < x.army.size(); i++) {
      auto &p = x.army[i], &q = y.army[i];
      RG_D(p.id == q.id && p.name == q.name && p.type == q.type && p.total == q.total && p.upkeep == q.upkeep, "army");
    }
    for (size_t i = 0; i < x.fleet.size(); i++) {
      auto &p = x.fleet[i], &q = y.fleet[i];
      RG_D(p.id == q.id && p.name == q.name && p.type == q.type && p.total == q.total && p.upkeep == q.upkeep, "fleet");
    }
    RG_D(x.res == y.res && x.modifiers == y.modifiers && x.tax == y.tax && x.homeState == y.homeState && x.stateGuild == y.stateGuild &&
             x.notes == y.notes && x.entity == y.entity, "прочие поля");
    return "";
  });
  if (!d.empty()) return d;
  d = diffTable(a.characters, b.characters, "character", [](const Character& x, const Character& y) {
    return x.name == y.name && x.title == y.title && x.faction == y.faction && x.hero == y.hero && x.upkeep == y.upkeep &&
                   x.portrait == y.portrait && x.notes == y.notes && x.entity == y.entity ? "" : "поля";
  });
  if (!d.empty()) return d;
  d = diffTable(a.modifiers, b.modifiers, "modifier", [](const Modifier& x, const Modifier& y) {
    return x.name == y.name && x.icon == y.icon && x.color == y.color && x.desc == y.desc && x.fx == y.fx && x.fxMask == y.fxMask &&
                   x.targets == y.targets ? "" : "поля";
  });
  if (!d.empty()) return d;
  d = diffTable(a.buildings, b.buildings, "building", [](const Building& x, const Building& y) -> std::string {
    RG_D(x.owner == y.owner && x.name == y.name && x.icon == y.icon && x.cat == y.cat && x.desc == y.desc && x.pos == y.pos, "поля");
    RG_D(x.requires_.size() == y.requires_.size(), "requires");
    for (size_t i = 0; i < x.requires_.size(); i++)
      RG_D(x.requires_[i].building == y.requires_[i].building && x.requires_[i].level == y.requires_[i].level, "requires");
    RG_D(x.levels.size() == y.levels.size(), "levels");
    for (size_t i = 0; i < x.levels.size(); i++) {
      auto &p = x.levels[i], &q = y.levels[i];
      RG_D(p.turns == q.turns && p.cost == q.cost && p.modifiers == q.modifiers && p.desc == q.desc, "levels");
    }
    return "";
  });
  if (!d.empty()) return d;
  d = diffTable(a.techs, b.techs, "tech", [](const Tech& x, const Tech& y) {
    return x.faction == y.faction && x.name == y.name && x.desc == y.desc && x.turns == y.turns && x.prereqs == y.prereqs &&
                   x.modifiers == y.modifiers && x.studied == y.studied && x.research == y.research && x.progress == y.progress && x.pos == y.pos
               ? "" : "поля";
  });
  if (!d.empty()) return d;
  d = diffTable(a.armies, b.armies, "army", [](const Army& x, const Army& y) -> std::string {
    RG_D(x.kind == y.kind && x.name == y.name && x.pos == y.pos && x.commander == y.commander && x.groups.size() == y.groups.size(), "поля");
    for (size_t i = 0; i < x.groups.size(); i++) {
      auto &p = x.groups[i], &q = y.groups[i];
      RG_D(p.faction == q.faction && p.heroes == q.heroes && p.units.size() == q.units.size(), "groups");
      for (size_t k = 0; k < p.units.size(); k++) RG_D(p.units[k].row == q.units[k].row && p.units[k].count == q.units[k].count, "units");
    }
    return "";
  });
  if (!d.empty()) return d;
  d = diffTable(a.routes, b.routes, "route", [](const Route& x, const Route& y) {
    return x.name == y.name && x.guild == y.guild && x.pts == y.pts && x.color == y.color ? "" : "поля";
  });
  if (!d.empty()) return d;
  d = diffTable(a.deals, b.deals, "deal", [](const Deal& x, const Deal& y) -> std::string {
    RG_D(x.kind == y.kind && x.a == y.a && x.b == y.b && x.turn == y.turn && x.status == y.status && x.note == y.note && x.items.size() == y.items.size(), "поля");
    for (size_t i = 0; i < x.items.size(); i++) {
      auto &p = x.items[i], &q = y.items[i];
      RG_D(p.from == q.from && p.res == q.res && p.amount == q.amount && p.mode == q.mode && p.turns == q.turns && p.left == q.left, "items");
    }
    return "";
  });
  if (!d.empty()) return d;
  d = diffTable(a.log, b.log, "log", [](const LogEntry& x, const LogEntry& y) {
    return x.turn == y.turn && x.kind == y.kind && x.text == y.text && x.province == y.province && x.army == y.army && x.factions == y.factions &&
                   x.at == y.at ? "" : "поля";
  });
  return d;
}
#undef RG_D

inline std::string warningsText(const io::Warnings& w) {
  std::string s;
  for (auto& x : w) s += "\n    " + x.text();
  return s;
}

inline bool hasWarning(const io::Warnings& w, std::string_view file, std::string_view where, std::string_view msgPart = {}) {
  for (auto& x : w)
    if (x.file == file && x.where == where && x.msg.find(msgPart) != std::string::npos) return true;
  return false;
}

}  // namespace rg::iotest
