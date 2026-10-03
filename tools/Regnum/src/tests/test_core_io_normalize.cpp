// Тесты core/io: нормализация мира, исправленного вручную или агентом (неверные ссылки, пределы,
// повторы, циклы), и устойчивость чтения к произвольным изменениям JSON.
#include <unordered_set>

#include "base/json.h"
#include "codec/base64.h"
#include "tests/test_core_io_util.h"

using namespace rg;
using namespace rg::iotest;

namespace {

json::Value readJson(const std::string& dir, const char* rel) { return json::parse(fs::readFile(fs::join(dir, rel)).value()); }
void writeJson(const std::string& dir, const char* rel, const json::Value& v) {
  CHECK(fs::writeFileAtomic(fs::join(dir, rel), json::write(v, json::WriteOptions{2, true, true})));
}
json::Value& byId(json::Value& doc, const char* key, const char* id) {
  for (auto& e : doc[key].items())
    if (e.str("id") == id) return e;
  throw std::runtime_error(std::string("нет записи ") + id);
}
json::Value parse(const char* text) { return json::parse(text); }

// Проверка инвариантов, которые гарантирует нормализация. Пустая строка — всё верно.
std::string checkInvariants(const World& w) {
  std::string err;
  auto bad = [&](const std::string& m) { if (err.empty()) err = m; };
  auto res = [&](Id id) { return w.resource(id) != nullptr; };
  auto catHas = [](const std::vector<CatalogItem>& l, Id id) { return Catalogs::find(l, id) != nullptr; };
  const Catalogs& c = *w.catalogs;
  if (!w.resource(kGold) || !w.resource(kGold)->builtin) bad("нет золота");
  std::array<u32, kSeqCount> maxId{};
  auto upd = [&](Seq s, Id id) { maxId[size_t(s)] = std::max(maxId[size_t(s)], id); };
  auto uniq = [](const std::vector<Id>& v) { return std::unordered_set<Id>(v.begin(), v.end()).size() == v.size(); };
  for (int i = 0; i < 6; i++) {
    const std::vector<CatalogItem>* lists[6] = {&c.resources, &c.races, &c.cultures, &c.religions, &c.governments, &c.positions};
    std::unordered_set<Id> ids;
    for (auto& it : *lists[i]) {
      if (!it.id || !ids.insert(it.id).second) bad("ID справочника");
      upd(Seq(int(Seq::Resource) + i), it.id);
    }
  }
  w.nodes.each([&](const Node& n) {
    upd(Seq::Node, n.id);
    if (!(n.p.x >= 0 && n.p.x <= schema::kMapWidth && n.p.y >= 0 && n.p.y <= schema::kMapHeight)) bad("узел вне карты");
  });
  w.edges.each([&](const Edge& e) {
    upd(Seq::Edge, e.id);
    if (!w.nodes.has(e.a) || !w.nodes.has(e.b)) bad("дуга без узла");
    if ((e.pl && !w.province(e.pl)) || (e.pr && !w.province(e.pr))) bad("дуга: провинция");
  });
  std::unordered_set<Id> rows;
  w.factions.each([&](const Faction& f) {
    upd(Seq::Faction, f.id);
    for (auto& r : f.army) { upd(Seq::Row, r.id); if (!r.id || !rows.insert(r.id).second || r.total < 0) bad("строка армии"); }
    for (auto& r : f.fleet) { upd(Seq::Row, r.id); if (!r.id || !rows.insert(r.id).second || r.total < 0) bad("строка флота"); }
    for (auto& s : f.council) { upd(Seq::Council, s.id); if (s.character && !w.character(s.character)) bad("совет"); }
    if ((f.culture && !catHas(c.cultures, f.culture)) || (f.religion && !catHas(c.religions, f.religion)) ||
        (f.government && !catHas(c.governments, f.government)))
      bad("фракция: справочник");
    if ((f.ruler && !w.character(f.ruler)) || (f.capital && !w.province(f.capital))) bad("фракция: ссылки");
    for (auto& [r, v] : f.res) if (!res(r) || !std::isfinite(v)) bad("фракция: ресурс");
    if (!uniq(f.modifiers)) bad("фракция: повтор модификатора");
    for (Id m : f.modifiers) if (!w.modifier(m)) bad("фракция: модификатор");
    if (!(f.tax >= 0 && f.tax <= 100)) bad("налог");
    if (f.isState() && (f.homeState || f.stateGuild)) bad("государство с признаками гильдии");
    if (f.homeState && (!w.faction(f.homeState) || !w.faction(f.homeState)->isState())) bad("homeState");
    if (f.flag.image && f.flag.png.empty()) bad("флаг");
  });
  w.characters.each([&](const Character& x) {
    upd(Seq::Character, x.id);
    if (x.faction && !w.faction(x.faction)) bad("персонаж: фракция");
    if (!(x.upkeep >= 0)) bad("персонаж: содержание");
  });
  w.modifiers.each([&](const Modifier& m) {
    upd(Seq::Modifier, m.id);
    for (int i = 0; i < kFxCount; i++) {
      double v = m.fx[size_t(i)];
      if (!m.has(Fx(i)) && v != 0) bad("эффект без бита");
      if (m.has(Fx(i)) && !(v >= schema::kEffects[i].min && v <= schema::kEffects[i].max)) bad("эффект вне пределов");
    }
    if (m.fxMask >> kFxCount) bad("лишние биты эффектов");
    if (!uniq(m.targets)) bad("повтор цели");
    for (Id t : m.targets) if (!w.faction(t)) bad("цель модификатора");
  });
  w.buildings.each([&](const Building& b) {
    upd(Seq::Building, b.id);
    if (b.owner && !w.faction(b.owner)) bad("постройка: владелец");
    if (b.levels.empty()) bad("постройка без уровней");
    for (auto& l : b.levels) {
      if (l.turns < 1) bad("ходы уровня");
      for (auto& [r, v] : l.cost) if (!res(r) || !(v >= 0)) bad("цена");
      for (Id m : l.modifiers) if (!w.modifier(m)) bad("уровень: модификатор");
    }
    for (auto& r : b.requires_) {
      const Building* t = w.building(r.building);
      if (!t || r.building == b.id || r.level < 1 || r.level > int(t->levels.size())) bad("требование постройки");
    }
  });
  w.techs.each([&](const Tech& t) {
    upd(Seq::Tech, t.id);
    if (!w.faction(t.faction)) bad("технология без фракции");
    if (t.turns < 1 || t.progress < 0 || t.progress > t.turns || (t.studied && t.research)) bad("технология: ходы");
    if (!uniq(t.prereqs)) bad("повтор условия");
    for (Id p : t.prereqs) if (!w.tech(p) || w.tech(p)->faction != t.faction || p == t.id) bad("условие технологии");
  });
  w.provinces.each([&](const Province& p) {
    upd(Seq::Province, p.id);
    if (p.owner && (!w.faction(p.owner) || !w.faction(p.owner)->isState())) bad("владелец провинции");
    if (p.lord && !w.character(p.lord)) bad("лорд");
    if (p.resource && !res(p.resource)) bad("ресурс провинции");
    if (!(p.resourceAmount >= 0) || !(p.contentment >= -100 && p.contentment <= 100) || !(p.baseTrade >= 0) ||
        !(p.localTax >= -100 && p.localTax <= 100))
      bad("пределы провинции");
    const Faction* owner = w.faction(p.owner);
    std::unordered_set<Id> g;
    for (auto& e : p.garrison) if (!owner || !owner->armyRow(e.row) || e.count < 0 || !g.insert(e.row).second) bad("гарнизон");
    std::unordered_set<Id> r;
    for (auto& e : p.races) if (!catHas(c.races, e.race) || e.pop < 0 || !r.insert(e.race).second) bad("расы");
    double sum = 0;
    std::unordered_set<Id> gi;
    for (auto& e : p.influence) {
      sum += e.pct;
      if (!w.faction(e.guild) || !w.faction(e.guild)->isGuild() || e.pct < 0 || !gi.insert(e.guild).second) bad("влияние");
    }
    if (sum > 100 + 1e-6) bad("сумма влияния");
    if (p.hqs.size() > size_t(schema::kMaxHqPerProvince) || !uniq(p.hqs)) bad("штабы");
    for (Id h : p.hqs) if (!w.faction(h) || !w.faction(h)->isGuild()) bad("штаб не гильдии");
    std::unordered_set<Id> bs;
    for (auto& b : p.buildings) {
      const Building* d = w.building(b.building);
      if (!d || b.level < 1 || b.level > int(d->levels.size()) || b.left < 0 || !bs.insert(b.building).second) bad("постройка провинции");
    }
    for (Id m : p.modifiers) if (!w.modifier(m)) bad("модификатор провинции");
    if (p.occupied && (!w.faction(p.occupier) || !w.faction(p.occupier)->isState() || p.occupier == p.owner)) bad("оккупация");
    if (!p.occupied && p.occupier) bad("оккупант без оккупации");
  });
  std::unordered_set<Id> heroes;
  w.armies.each([&](const Army& a) {
    upd(Seq::Army, a.id);
    if (a.groups.empty()) bad("войско без групп");
    if (a.commander && !w.character(a.commander)) bad("полководец");
    std::unordered_set<Id> fs;
    for (auto& g : a.groups) {
      const Faction* f = w.faction(g.faction);
      if (!f || !fs.insert(g.faction).second) bad("группа войска");
      std::unordered_set<Id> us;
      for (auto& u : g.units)
        if (!f || (a.isFleet() ? !f->fleetRow(u.row) : !f->armyRow(u.row)) || u.count < 0 || !us.insert(u.row).second) bad("отряд");
      for (Id h : g.heroes) if (!w.character(h) || !heroes.insert(h).second) bad("герой");
    }
  });
  w.routes.each([&](const Route& r) {
    upd(Seq::Route, r.id);
    if (r.pts.size() < 2 || (r.guild && (!w.faction(r.guild) || !w.faction(r.guild)->isGuild()))) bad("маршрут");
  });
  w.deals.each([&](const Deal& d) {
    upd(Seq::Deal, d.id);
    if (!w.faction(d.a) || !w.faction(d.b) || d.a == d.b) bad("стороны сделки");
    for (auto& it : d.items) if (!res(it.res) || !(it.amount >= 0) || it.turns < 1 || it.left < 0 || it.left > it.turns) bad("позиция сделки");
  });
  w.log.each([&](const LogEntry& l) {
    upd(Seq::Log, l.id);
    if (l.turn < 1 || !uniq(l.factions)) bad("хроника");
  });
  for (auto& [k, r] : *w.relations) {
    Id a = Id(k >> 32), b = Id(k & 0xFFFFFFFFu);
    if (a == b || !w.faction(a) || !w.faction(b) || !(r.v >= -100 && r.v <= 100)) bad("отношение");
  }
  for (int i = 0; i < kSeqCount; i++)
    if (w.meta->seq[size_t(i)] < maxId[size_t(i)]) bad("счётчик " + std::to_string(i));
  if (w.meta->turn < 1) bad("ход");
  if (!(w.settings->fillOpacity >= 0 && w.settings->fillOpacity <= 1)) bad("прозрачность");
  return err;
}

}  // namespace

TEST(io_normalize_hand_edited) {
  std::string dir = tempDir("hand");
  io::save(dir, richWorld(), TB_ALL);

  json::Value world = readJson(dir, "world.json");
  world["meta"]["turn"] = -3;
  world["meta"]["seq"]["province"] = 1;
  world["settings"]["fillOpacity"] = 5;
  world["settings"]["comment"] = "лишнее поле";
  writeJson(dir, "world.json", world);

  json::Value cat = readJson(dir, "data/catalogs.json");
  auto& resItems = cat["resources"].items();
  resItems.erase(resItems.begin());  // нет золота
  cat["races"].push(json::Value::object({{"id", "rc1"}, {"name", "Дубль"}, {"color", "#123456"}}));
  cat["races"].push(json::Value::object({{"name", "Без ID"}, {"color", "не цвет"}}));
  writeJson(dir, "data/catalogs.json", cat);

  json::Value fac = readJson(dir, "data/factions.json");
  for (int i = 4; i <= 9; i++) {
    json::Value g = json::Value::object({{"id", "f" + std::to_string(i)}, {"kind", "guild"}, {"name", "Гильдия " + std::to_string(i)}});
    fac["factions"].push(g);
  }
  json::Value& f3 = byId(fac, "factions", "f3");
  f3["army"].push(json::Value::object({{"id", "u1"}, {"name", "Чужой ID"}, {"type", "dragons"}, {"total", -50}}));
  f3["tax"] = 140;
  f3["homeState"] = "f1";
  json::Value& f1 = byId(fac, "factions", "f1");
  f1["modifiers"] = parse("[\"m1\", \"m1\", \"m77\"]");
  f1["res"]["rs99"] = 10;
  f1["capital"] = 3;  // ссылка числом — допустима
  writeJson(dir, "data/factions.json", fac);

  json::Value chr = readJson(dir, "data/characters.json");
  byId(chr, "characters", "c1")["faction"] = "f42";
  byId(chr, "characters", "c2")["portrait"] = "не base64!!";
  writeJson(dir, "data/characters.json", chr);

  json::Value mod = readJson(dir, "data/modifiers.json");
  json::Value& m1 = byId(mod, "modifiers", "m1");
  m1["fx"]["tradePct"] = 1000;
  m1["fx"]["magicPct"] = 5;
  m1["targets"] = parse("[\"f1\", \"f1\", \"f99\", \"p3\"]");
  writeJson(dir, "data/modifiers.json", mod);

  json::Value bld = readJson(dir, "data/buildings.json");
  byId(bld, "buildings", "b1")["requires"] = parse("[{\"building\": \"b2\", \"level\": 5}]");
  byId(bld, "buildings", "b2")["levels"] = parse("[]");
  writeJson(dir, "data/buildings.json", bld);

  json::Value tch = readJson(dir, "data/techs.json");
  byId(tch, "techs", "t1")["prereqs"] = parse("[\"t2\"]");
  tch["techs"].push(parse("{\"id\": \"t3\", \"faction\": \"f99\", \"name\": \"Сирота\"}"));
  writeJson(dir, "data/techs.json", tch);

  json::Value prv = readJson(dir, "data/provinces.json");
  json::Value& p1 = byId(prv, "provinces", "p1");
  p1["owner"] = "f9";  // гильдия, а не государство
  p1["contentment"] = 250;
  p1["size"] = "huge";
  p1["comment"] = "заметка агента";
  p1["influence"] = parse("[{\"guild\": \"f2\", \"pct\": 80}, {\"guild\": \"f4\", \"pct\": 70}, {\"guild\": \"f1\", \"pct\": 10}]");
  p1["hqs"] = parse("[\"f2\", \"f4\", \"f5\", \"f6\", \"f7\", \"f8\", \"f2\", \"f1\"]");
  p1["buildings"] = parse("[{\"building\": \"b1\", \"level\": 9}, {\"building\": \"b404\"}, {\"building\": \"b1\"}]");
  json::Value& p2 = byId(prv, "provinces", "p2");
  p2["owner"] = "f3";
  p2["garrison"] = parse("[{\"row\": \"u1\", \"count\": 7}, {\"row\": \"u5\", \"count\": 3}, {\"row\": \"u5\", \"count\": 4}]");
  json::Value& p3 = byId(prv, "provinces", "p3");
  p3["occupier"] = "f1";  // совпадает с владельцем
  prv["provinces"].push(parse("{\"id\": \"p1\", \"name\": \"Дубликат\"}"));
  prv["provinces"].push(parse("{\"name\": \"Без ID\"}"));
  prv["provinces"].push(json::Value(42));
  writeJson(dir, "data/provinces.json", prv);

  json::Value arm = readJson(dir, "data/armies.json");
  arm["armies"].push(parse("{\"id\": \"a3\", \"kind\": \"army\", \"pos\": [100, 100], \"groups\": [{\"faction\": \"f3\", \"units\": [{\"row\": \"u1\", \"count\": 9}]}]}"));
  arm["armies"].push(parse("{\"id\": \"a4\", \"pos\": [9000, -5], \"groups\": [{\"faction\": \"f77\", \"units\": []}]}"));
  arm["armies"].push(parse("{\"id\": \"a5\", \"pos\": [5, 5], \"groups\": [{\"faction\": \"f1\", \"heroes\": [\"c1\"]}]}"));
  writeJson(dir, "data/armies.json", arm);

  json::Value rts = readJson(dir, "data/routes.json");
  rts["routes"].push(parse("{\"id\": \"r3\", \"pts\": [1, 2]}"));
  rts["routes"].push(parse("{\"id\": \"r4\", \"guild\": \"f1\", \"pts\": [[1, 2], [3, 4], [5]]}"));
  writeJson(dir, "data/routes.json", rts);

  json::Value dls = readJson(dir, "data/deals.json");
  dls["deals"].push(parse("{\"id\": \"d3\", \"a\": \"f1\", \"b\": \"f55\"}"));
  writeJson(dir, "data/deals.json", dls);

  json::Value rel = readJson(dir, "data/relations.json");
  rel["relations"]["f1|f99"] = parse("{\"v\": 10, \"s\": \"war\"}");
  rel["relations"]["f2|f2"] = parse("{\"v\": 10, \"s\": \"war\"}");
  rel["relations"]["f1|f3"] = parse("{\"v\": 500, \"s\": \"truce\"}");
  rel["relations"]["oops"] = parse("{}");
  writeJson(dir, "data/relations.json", rel);

  json::Value lg = readJson(dir, "data/log.json");
  lg["log"][0]["factions"] = parse("[\"f1\", \"f1\", \"f3\"]");
  writeJson(dir, "data/log.json", lg);

  io::LoadResult r = io::load(dir);
  const World& w = r.world;
  const io::Warnings& ws = r.warnings;
  std::string inv = checkInvariants(w);
  CHECK_MSG(inv.empty(), inv);
  CHECK(ws.size() >= 40);
  CHECK(r.fixedTables & TB_PROVINCES);
  CHECK(r.fixedTables & TB_META);

  // world.json
  CHECK_EQ(w.turn(), 1);
  CHECK(hasWarning(ws, "world.json", "meta.turn"));
  CHECK_EQ(w.settings->fillOpacity, 1.0f);
  CHECK(hasWarning(ws, "world.json", "settings.comment", "неизвестное поле"));
  CHECK(hasWarning(ws, "world.json", "meta.seq.province", "поднят"));
  CHECK(w.meta->seq[int(Seq::Province)] >= 5);
  // Справочники: золото восстановлено, повтор ID и запись без ID получили новые ID.
  CHECK(w.resource(kGold) && w.resource(kGold)->builtin);
  CHECK(hasWarning(ws, "data/catalogs.json", "resources", "Золото"));
  CHECK_EQ(w.catalogs->races.size(), size_t(4));
  CHECK_EQ(w.catalogs->races[2].id, Id(3));
  CHECK_EQ(w.catalogs->races[3].id, Id(4));
  CHECK(hasWarning(ws, "data/catalogs.json", "races[3].color", "неверный цвет"));
  // Фракции.
  const Faction* nf3 = w.faction(3);
  CHECK_EQ(nf3->army.size(), size_t(2));
  Id newRow = nf3->army[1].id;
  CHECK(newRow > 5);
  CHECK(nf3->army[1].type == UnitType::LightInf);
  CHECK_EQ(nf3->army[1].total, i64(0));
  CHECK_EQ(nf3->tax, 100.0);
  CHECK_EQ(nf3->homeState, Id(0));
  CHECK(hasWarning(ws, "data/factions.json", "f3.army[1].type", "dragons"));
  CHECK((w.faction(1)->modifiers == std::vector<Id>{1}));
  CHECK(!w.faction(1)->res.count(99));
  CHECK_EQ(w.faction(1)->capital, Id(3));
  // Персонажи.
  CHECK_EQ(w.character(1)->faction, Id(0));
  CHECK(w.character(2)->portrait.empty());
  CHECK(hasWarning(ws, "data/characters.json", "c2.portrait", "base64"));
  // Модификаторы.
  CHECK_EQ(w.modifier(1)->get(Fx::TradePct), 100.0);
  CHECK(hasWarning(ws, "data/modifiers.json", "m1.fx.magicPct", "неизвестное поле"));
  CHECK((w.modifier(1)->targets == std::vector<Id>{1}));
  CHECK(hasWarning(ws, "data/modifiers.json", "m1.targets[3]", "неверная ссылка"));
  // Постройки: пустые уровни, цикл требований.
  CHECK_EQ(w.building(2)->levels.size(), size_t(1));
  CHECK(w.building(1)->requires_.empty() || w.building(2)->requires_.empty());
  CHECK(hasWarning(ws, "data/buildings.json", "b2.levels", "нет уровней"));
  // Технологии: сирота удалена, цикл разорван.
  CHECK(!w.tech(3));
  CHECK(w.tech(1)->prereqs.empty() || w.tech(2)->prereqs.empty());
  CHECK(hasWarning(ws, "data/techs.json", "t3", "удалена"));
  // Провинции.
  const Province* np1 = w.province(1);
  CHECK_EQ(np1->name, std::string("Арден"));
  CHECK_EQ(np1->owner, Id(0));
  CHECK(np1->garrison.empty());  // без владельца гарнизона нет
  CHECK_EQ(np1->contentment, 100.0);
  CHECK(np1->size == ProvSize::Medium);
  CHECK(hasWarning(ws, "data/provinces.json", "p1.comment", "неизвестное поле"));
  CHECK(hasWarning(ws, "data/provinces.json", "p1.owner", "гильдия"));
  CHECK_EQ(np1->influence.size(), size_t(2));
  CHECK_NEAR(np1->influence[0].pct + np1->influence[1].pct, 100.0, 1e-9);
  CHECK_NEAR(np1->influence[0].pct, 80.0 * 100 / 150, 1e-9);
  CHECK((np1->hqs == std::vector<Id>{2, 4, 5, 6, 7}));
  CHECK_EQ(np1->buildings.size(), size_t(1));
  CHECK_EQ(np1->buildings[0].level, 2);
  const Province* np2 = w.province(2);
  CHECK_EQ(np2->garrison.size(), size_t(2));
  CHECK_EQ(np2->garrison[0].row, newRow);  // ссылка на перенумерованную строку обновлена
  CHECK_EQ(np2->garrison[1].count, i64(7));  // повтор строки — численности сложены
  CHECK(!w.province(3)->occupied && w.province(3)->occupier == 0);
  CHECK_EQ(w.provinces.size(), u32(5));
  CHECK_EQ(w.province(4)->name, std::string("Дубликат"));
  CHECK_EQ(w.province(5)->name, std::string("Без ID"));
  CHECK(hasWarning(ws, "data/provinces.json", "p1", "повторный ID"));
  CHECK(hasWarning(ws, "data/provinces.json", "[4]", "нет поля id"));
  CHECK(hasWarning(ws, "data/provinces.json", "[5]", "объектом"));
  // Войска.
  CHECK(w.army(3) != nullptr);
  CHECK_EQ(w.army(3)->groups[0].units[0].row, newRow);
  CHECK(!w.army(4));
  CHECK(hasWarning(ws, "data/armies.json", "a4", "удалено"));
  CHECK(w.army(5)->groups[0].heroes.empty());  // c1 уже герой войска a1
  // Маршруты, сделки, отношения, хроника.
  CHECK(!w.route(3));
  CHECK(w.route(4) && w.route(4)->guild == 0 && w.route(4)->pts.size() == 2);
  CHECK(!w.deal(3));
  CHECK_EQ(w.relation(1, 3).v, 100.0);
  CHECK(w.relation(1, 3).s == RelStatus::Unknown);
  CHECK(!w.relations->count(relKey(1, 99)));
  CHECK(hasWarning(ws, "data/relations.json", "oops", "ключ"));
  CHECK((w.log.get(1)->factions == std::vector<Id>{1, 3}));

  // Нормализация идемпотентна, а сохранение и чтение исправленного мира — без предупреждений.
  World again = w;
  io::Warnings w2;
  CHECK_EQ(io::normalize(again, w2), 0u);
  CHECK_MSG(w2.empty(), warningsText(w2));
  std::string dir2 = tempDir("hand2");
  io::save(dir2, w, TB_ALL);
  auto r2 = io::load(dir2);
  CHECK_MSG(r2.warnings.empty(), warningsText(r2.warnings));
  CHECK(diffWorld(w, r2.world).empty());
}

TEST(io_normalize_in_memory) {
  World base = richWorld();
  Tx tx(base);
  tx.province(1).contentment = std::nan("");
  tx.province(1).size = ProvSize(9);
  tx.province(1).localTax = -1e9;
  tx.province(2).buildings = {{1, 0, true, -4}};
  tx.add(Node{9, {std::nan(""), 5}});
  Edge e;
  e.id = 9;
  e.a = 9;
  e.b = 1;
  tx.add(e);
  Edge loop;
  loop.id = 10;
  loop.a = 1;
  loop.b = 1;
  tx.add(loop);
  tx.node(2).p = {-50, 9000};
  tx.faction(1).army.push_back(ArmyRow{0, "Без ID", UnitType::Ranged, 10, 1});
  tx.faction(1).council.push_back(CouncilSeat{1, "Дубль", 0});
  tx.faction(1).flag.png = "not a png";
  tx.modifier(2).fxMask |= 1u << 30;
  tx.modifier(2).fx[int(Fx::IncomePct)] = 7;  // значение без бита
  tx.tech(2).progress = 50;
  tx.tech(1).research = true;
  tx.meta().seq[int(Seq::Log)] = 0;
  tx.catalogs().resources[0].builtin = false;
  tx.army(1).groups.push_back(ArmyGroup{1, {{1, 5}}, {}});  // вторая группа той же фракции
  World w = std::move(tx).finish();

  io::Warnings warns;
  u32 mask = io::normalize(w, warns);
  std::string inv = checkInvariants(w);
  CHECK_MSG(inv.empty(), inv);
  CHECK(mask & TB_PROVINCES);
  CHECK(mask & TB_NODES);
  CHECK(mask & TB_EDGES);
  CHECK(mask & TB_META);
  CHECK_EQ(w.province(1)->contentment, 0.0);
  CHECK(w.province(1)->size == ProvSize::Medium);
  CHECK_EQ(w.province(1)->localTax, -100.0);
  CHECK_EQ(w.province(2)->buildings[0].level, 1);
  CHECK_EQ(w.province(2)->buildings[0].left, 0);
  CHECK(!w.nodes.has(9));
  CHECK(!w.edges.has(9));
  CHECK(!w.edges.has(10));
  CHECK((w.nodes.get(2)->p == Vec2{0, schema::kMapHeight}));
  CHECK(w.faction(1)->army.back().id > 5);
  CHECK(w.faction(1)->council.back().id > 2);
  CHECK(w.faction(1)->flag.png.empty() && !w.faction(1)->flag.image);
  CHECK_EQ(w.modifier(2)->fxMask, 1u << int(Fx::PopGrowthPct));
  CHECK_EQ(w.modifier(2)->fx[int(Fx::IncomePct)], 0.0);
  CHECK_EQ(w.tech(2)->progress, 5);
  CHECK(!w.tech(1)->research);
  CHECK_EQ(w.meta->seq[int(Seq::Log)], 5u);
  CHECK(w.resource(kGold)->builtin);
  CHECK_EQ(w.army(1)->groups.size(), size_t(1));
  CHECK_EQ(w.army(1)->groups[0].units[0].count, i64(505));
  io::Warnings again;
  CHECK_EQ(io::normalize(w, again), 0u);
  CHECK_MSG(again.empty(), warningsText(again));
}

TEST(io_normalize_empty_world) {
  World w;
  io::Warnings warns;
  io::normalize(w, warns);
  CHECK(w.resource(kGold) != nullptr);
  CHECK(checkInvariants(w).empty());
  // Пустой проект: только world.json.
  std::string dir = tempDir("minimal");
  CHECK(fs::writeFileAtomic(fs::join(dir, "world.json"), std::string_view("{\"format\": \"regnum-world\", \"version\": 1}")));
  auto r = io::load(dir);
  CHECK(r.world.provinces.empty());
  CHECK(r.world.resource(kGold) != nullptr);
  CHECK_EQ(r.world.meta->name, std::string("Новый мир"));
  CHECK(hasWarning(r.warnings, "world.json", "meta", "нет раздела"));
  CHECK(hasWarning(r.warnings, "data/provinces.json", "", "не найден"));
}

// Случайные изменения значений во всех файлах: чтение не падает, результат удовлетворяет
// инвариантам, нормализация идемпотентна, повторное сохранение и чтение — без предупреждений.
TEST(io_fuzz_values) {
  const json::Value doc = json::parse(io::toJson(richWorld()));
  const json::Value kinds[] = {
    json::Value(), json::Value(true), json::Value(false), json::Value(0), json::Value(-1), json::Value(0.5), json::Value(3),
    json::Value(-1e300), json::Value(1e15), json::Value(4294967296.0), json::Value(""), json::Value("p1"), json::Value("f2"),
    json::Value("u3"), json::Value("rs1"), json::Value("c1"), json::Value("#zzz"), json::Value("war"), json::Value("guild"),
    json::Value("fleet"), json::Value("[1,2]"), json::Value::array(), json::Value::object(), json::parse("[1, 2, 3, 4, 5]"),
    json::parse("[\"f1\", \"f2\", \"f3\"]"), json::parse("[[1, 2], [3, 4]]"), json::parse("{\"id\": \"p1\"}"),
    json::parse("[{\"row\": \"u1\", \"count\": 5}]"), json::parse("{\"rs1\": -5, \"rs2\": \"x\"}"),
  };
  Rng rng(20261001);
  int ok = 0, rejected = 0;
  for (int iter = 0; iter < 400; iter++) {
    json::Value d = doc;
    int edits = 1 + rng.range(0, 7);
    for (int k = 0; k < edits; k++) {
      // Случайный путь от корня «files».
      json::Value* v = d.find("files");
      int depth = rng.range(1, 6);
      for (int s = 0; s < depth; s++) {
        if (v->isObj() && !v->empty()) {
          auto& ms = v->members();
          std::string key = ms[size_t(rng.range(0, int(ms.size()) - 1))].first;
          v = v->find(key);
        } else if (v->isArr() && !v->empty()) {
          v = &v->items()[size_t(rng.range(0, int(v->size()) - 1))];
        } else {
          break;
        }
      }
      if (v == d.find("files")) continue;
      *v = kinds[size_t(rng.range(0, int(std::size(kinds)) - 1))];
    }
    std::string text = json::write(d, json::WriteOptions{0, false, true});
    io::LoadResult r;
    try {
      r = io::fromJson(text);
    } catch (const UserError& e) {
      // Допустим только отказ из-за формата или версии world.json.
      std::string m = e.what();
      CHECK_MSG(m.find("world.json") != std::string::npos || m.find("версией") != std::string::npos, m);
      rejected++;
      continue;
    }
    std::string inv = checkInvariants(r.world);
    CHECK_MSG(inv.empty(), "итерация " + std::to_string(iter) + ": " + inv);
    World again = r.world;
    io::Warnings w2;
    io::normalize(again, w2);
    CHECK_MSG(w2.empty(), "итерация " + std::to_string(iter) + ":" + warningsText(w2));
    auto r2 = io::fromJson(io::toJson(r.world));
    CHECK_MSG(r2.warnings.empty(), "итерация " + std::to_string(iter) + ":" + warningsText(r2.warnings));
    std::string diff = diffWorld(r.world, r2.world);
    CHECK_MSG(diff.empty(), "итерация " + std::to_string(iter) + ": " + diff);
    ok++;
  }
  CHECK(ok > 300);
  (void)rejected;
}

// Удобства ручной правки: ссылки числами, точки парами, координаты объектом, data URI, запись без ID,
// лишние файлы в data/, группировка предупреждений по файлам.
TEST(io_hand_edit_conveniences) {
  std::string dir = tempDir("conveniences");
  World base = richWorld();
  io::save(dir, base, TB_ALL);

  json::Value chr = readJson(dir, "data/characters.json");
  std::string png = pngBytes(2, 2, 1);
  byId(chr, "characters", "c3")["portrait"] = "data:image/png;base64," + codec::base64::encode(png);
  byId(chr, "characters", "c3")["faction"] = 1;  // ссылка числом
  writeJson(dir, "data/characters.json", chr);

  json::Value rts = readJson(dir, "data/routes.json");
  byId(rts, "routes", "r2")["pts"] = parse("[[10, 20], [30.5, 40.25], [50, 60]]");
  writeJson(dir, "data/routes.json", rts);

  json::Value arm = readJson(dir, "data/armies.json");
  byId(arm, "armies", "a1")["pos"] = parse("{\"x\": 11.5, \"y\": 22}");
  writeJson(dir, "data/armies.json", arm);

  json::Value prv = readJson(dir, "data/provinces.json");
  prv["provinces"].push(parse("{\"name\": \"Новая без ID\", \"owner\": \"f1\"}"));
  prv["provinces"].push(parse("{\"id\": \"p40\", \"name\": \"Далёкий ID\"}"));
  writeJson(dir, "data/provinces.json", prv);
  CHECK(fs::writeFileAtomic(fs::join(dir, "data/province.json"), std::string_view("{\"provinces\": []}\n")));

  auto r = io::load(dir);
  const World& w = r.world;
  CHECK(w.character(3)->portrait == png);
  CHECK_EQ(w.character(3)->faction, Id(1));
  CHECK_EQ(w.route(2)->pts.size(), size_t(3));
  CHECK_EQ(w.route(2)->pts[1].y, 40.25);
  CHECK((w.army(1)->pos == Vec2{11.5, 22}));
  // Запись без ID получает ID после наибольшего (p41); счётчик (3) меньше ID в данных — поднимается с предупреждением.
  CHECK(w.province(41) && w.province(41)->name == "Новая без ID");
  CHECK(w.province(40) && w.province(40)->name == "Далёкий ID");
  CHECK_EQ(w.meta->seq[int(Seq::Province)], 41u);
  CHECK(hasWarning(r.warnings, "world.json", "meta.seq.province", "поднят"));
  CHECK(hasWarning(r.warnings, "data/provinces.json", "[3]", "нет поля id"));
  CHECK(hasWarning(r.warnings, "data/province.json", "", "неизвестный файл"));
  // Только эти предупреждения: числовые ссылки, пары точек, {x, y} и data URI допустимы.
  CHECK_MSG(r.warnings.size() == 3, warningsText(r.warnings));
  CHECK_EQ(r.warnings.front().file, std::string("world.json"));
  CHECK_EQ(r.warnings.back().file, std::string("data/province.json"));
  CHECK(r.fixedTables & TB_PROVINCES);
  CHECK(r.fixedTables & TB_META);
  CHECK(!(r.fixedTables & TB_CHARACTERS));  // формат записи другой, но исправлений нет
}

TEST(io_warnings_grouped_by_file) {
  std::string dir = tempDir("grouped");
  io::save(dir, richWorld(), TB_ALL);
  json::Value prv = readJson(dir, "data/provinces.json");
  byId(prv, "provinces", "p1")["owner"] = "f77";
  writeJson(dir, "data/provinces.json", prv);
  json::Value world = readJson(dir, "world.json");
  world["meta"]["turn"] = 0;
  world["extra"] = 1;
  writeJson(dir, "world.json", world);
  json::Value fac = readJson(dir, "data/factions.json");
  byId(fac, "factions", "f1")["tax"] = -5;
  byId(fac, "factions", "f2")["unknownField"] = true;
  writeJson(dir, "data/factions.json", fac);
  auto r = io::load(dir);
  CHECK(r.warnings.size() >= 5);
  auto order = [](const std::string& f) {
    if (f == "world.json") return -1;
    auto& files = io::projectFiles();
    for (size_t i = 0; i < files.size(); i++)
      if (f == files[i].path) return int(i);
    return int(files.size());
  };
  for (size_t i = 1; i < r.warnings.size(); i++)
    CHECK_MSG(order(r.warnings[i - 1].file) <= order(r.warnings[i].file), warningsText(r.warnings));
  // Внутри файла — сначала разбор (неизвестное поле), затем нормализация (пределы).
  CHECK_EQ(r.warnings[0].where, std::string("extra"));
  CHECK_EQ(r.warnings[1].where, std::string("meta.turn"));
}
