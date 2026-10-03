// Regnum — отображение мира в JSON и обратно: файлы проекта, перечисления, ссылки.
#include <charconv>
#include <cstdio>

#include "codec/base64.h"
#include "core/io_internal.h"

namespace rg::io::detail {

using json::Value;
using schema::EnumInfo;

const FileDef kFiles[F_COUNT] = {
  {"data/catalogs.json", "", TB_CATALOGS},
  {"data/geo.json", "", TB_GEO},
  {"data/provinces.json", "provinces", TB_PROVINCES},
  {"data/factions.json", "factions", TB_FACTIONS},
  {"data/characters.json", "characters", TB_CHARACTERS},
  {"data/relations.json", "relations", TB_RELATIONS},
  {"data/modifiers.json", "modifiers", TB_MODIFIERS},
  {"data/buildings.json", "buildings", TB_BUILDINGS},
  {"data/techs.json", "techs", TB_TECHS},
  {"data/armies.json", "armies", TB_ARMIES},
  {"data/routes.json", "routes", TB_ROUTES},
  {"data/deals.json", "deals", TB_DEALS},
  {"data/log.json", "log", TB_LOG},
  {"world.json", "", TB_META | TB_SETTINGS},
};

int fileIndex(std::string_view rel) {
  for (int i = 0; i < F_COUNT; i++)
    if (rel == kFiles[i].path) return i;
  return -1;
}

// ================================================================ перечисления
namespace {

struct Enum {
  const EnumInfo* list;
  int n;
};
template <size_t N> Enum en(const EnumInfo (&a)[N]) { return {a, int(N)}; }

// Перечисления, у которых нет таблиц в schema (только идентификаторы для JSON).
const EnumInfo kTerrainE[] = {{"none", "", ""}, {"land", "", ""}, {"sea", "", ""}};
const EnumInfo kEdgeKindE[] = {{"border", "", ""}, {"coast", "", ""}, {"frame", "", ""}};
const EnumInfo kArmyKindE[] = {{"army", "", ""}, {"fleet", "", ""}};
const EnumInfo kDealSideE[] = {{"a", "", ""}, {"b", "", ""}};
const EnumInfo kDealStatusE[] = {{"active", "", ""}, {"done", "", ""}, {"cancelled", "", ""}};

const char* const kSeqNames[kSeqCount] = {
  "province", "faction", "character", "modifier", "building", "tech", "army", "route", "deal", "log", "row",
  "council", "node", "edge", "resource", "race", "culture", "religion", "government", "position",
};

const char* const kCatalogKeys[6] = {"resources", "races", "cultures", "religions", "governments", "positions"};
const Seq kCatalogSeq[6] = {Seq::Resource, Seq::Race, Seq::Culture, Seq::Religion, Seq::Government, Seq::Position};

std::vector<CatalogItem>& catalogList(Catalogs& c, int i) {
  switch (i) {
    case 0: return c.resources;
    case 1: return c.races;
    case 2: return c.cultures;
    case 3: return c.religions;
    case 4: return c.governments;
    default: return c.positions;
  }
}
const std::vector<CatalogItem>& catalogList(const Catalogs& c, int i) { return catalogList(const_cast<Catalogs&>(c), i); }

std::string enumChoices(Enum e) {
  std::string s;
  for (int i = 0; i < e.n; i++) {
    if (i) s += ", ";
    s += e.list[i].id;
  }
  return s;
}

}  // namespace

// ================================================================ ссылки
std::string refStr(Seq s, Id id) {
  if (!id) return {};
  std::string r = schema::idPrefix(s);
  char buf[16];
  auto res = std::to_chars(buf, buf + sizeof buf, id);
  r.append(buf, size_t(res.ptr - buf));
  return r;
}

std::optional<Id> parseRef(const Value& v, Seq s) {
  if (v.isNull()) return Id(0);
  if (v.isNum()) {
    double d = v.asNum();
    if (d >= 0 && d <= 4294967295.0 && d == std::floor(d)) return Id(d);
    return std::nullopt;
  }
  if (!v.isStr()) return std::nullopt;
  std::string_view t = v.asStr();
  if (t.empty()) return Id(0);
  std::string_view p = schema::idPrefix(s);
  if (!p.empty() && t.size() > p.size() && t.substr(0, p.size()) == p && t[p.size()] >= '0' && t[p.size()] <= '9')
    t.remove_prefix(p.size());
  if (t.empty() || t[0] < '0' || t[0] > '9') return std::nullopt;
  u64 n = 0;
  auto r = std::from_chars(t.data(), t.data() + t.size(), n);
  if (r.ec != std::errc() || r.ptr != t.data() + t.size() || n > 0xFFFFFFFFull) return std::nullopt;
  return Id(n);
}

// ================================================================ запись
namespace {

double fin(double v) { return std::isfinite(v) ? v : 0.0; }
// Координаты карты и схем — с точностью 0,01.
double r2(double v) { return std::isfinite(v) ? std::round(v * 100.0) / 100.0 : 0.0; }
// float -> кратчайшее десятичное (0.3f пишется как 0.3).
double floatV(float f) {
  if (!std::isfinite(f)) return 0;
  char buf[32];
  auto r = std::to_chars(buf, buf + sizeof buf, f);
  double d = 0;
  std::from_chars(buf, r.ptr, d);
  return d;
}

Value ref(Seq s, Id id) { return id ? Value(refStr(s, id)) : Value(); }
Value refs(Seq s, const std::vector<Id>& ids) {
  json::Array a;
  a.reserve(ids.size());
  for (Id id : ids) a.emplace_back(ref(s, id));
  return Value(std::move(a));
}
Value colorV(Color c) {
  char buf[12];
  if (c.a == 255) std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
  else std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
  return Value(buf);
}
Value posV(Vec2 p) {
  json::Array a;
  a.reserve(2);
  a.emplace_back(r2(p.x));
  a.emplace_back(r2(p.y));
  return Value(std::move(a));
}
Value ptsV(const std::vector<Vec2>& v) {
  json::Array a;
  a.reserve(v.size() * 2);
  for (const Vec2& p : v) {
    a.emplace_back(r2(p.x));
    a.emplace_back(r2(p.y));
  }
  return Value(std::move(a));
}
Value resV(const std::map<Id, double>& m) {
  Value o = Value::object();
  for (auto& [id, v] : m) o.set(refStr(Seq::Resource, id), fin(v));
  return o;
}
Value enumV(Enum e, int v) { return Value(e.list[(v >= 0 && v < e.n) ? v : 0].id); }
Value bytesV(const std::string& b) { return Value(codec::base64::encode(b)); }

Value encMeta(const Meta& m) {
  Value o = Value::object();
  o.set("name", m.name);
  o.set("createdAt", m.createdAt);
  o.set("updatedAt", m.updatedAt);
  o.set("turn", m.turn);
  Value seq = Value::object();
  for (int i = 0; i < kSeqCount; i++) seq.set(kSeqNames[i], m.seq[size_t(i)]);
  o.set("seq", std::move(seq));
  o.set("basemap", m.basemap);
  o.set("notes", m.notes);
  return o;
}

Value encSettings(const Settings& s) {
  Value o = Value::object();
  o.set("fillOpacity", floatV(s.fillOpacity));
  o.set("labelStates", s.labelStates);
  o.set("labelProvinces", s.labelProvinces);
  o.set("labelArmies", s.labelArmies);
  o.set("occupiedIncome", enumV(en(schema::kOccupiedIncome), int(s.occupiedIncome)));
  o.set("rebellionRoll", s.rebellionRoll);
  o.set("autosaveFolder", s.autosaveFolder);
  o.set("autosaveSec", s.autosaveSec);
  return o;
}

Value encCatalogs(const Catalogs& c) {
  Value o = Value::object();
  for (int i = 0; i < 6; i++) {
    json::Array a;
    for (const CatalogItem& it : catalogList(c, i)) {
      Value e = Value::object();
      e.set("id", ref(kCatalogSeq[i], it.id));
      e.set("name", it.name);
      e.set("color", colorV(it.color));
      e.set("icon", it.icon);
      e.set("builtin", it.builtin);
      a.push_back(std::move(e));
    }
    o.set(kCatalogKeys[i], Value(std::move(a)));
  }
  return o;
}

Value encGeo(const World& w) {
  json::Array nodes, edges;
  nodes.reserve(w.nodes.size());
  edges.reserve(w.edges.size());
  w.nodes.each([&](const Node& n) {
    json::Array a;
    a.reserve(3);
    a.emplace_back(n.id);
    a.emplace_back(r2(n.p.x));
    a.emplace_back(r2(n.p.y));
    nodes.emplace_back(std::move(a));
  });
  w.edges.each([&](const Edge& e) {
    Value o = Value::object();
    o.set("id", e.id);
    o.set("a", e.a);
    o.set("b", e.b);
    o.set("kind", enumV(en(kEdgeKindE), int(e.kind)));
    o.set("pl", ref(Seq::Province, e.pl));
    o.set("pr", ref(Seq::Province, e.pr));
    o.set("tl", enumV(en(kTerrainE), int(e.tl)));
    o.set("tr", enumV(en(kTerrainE), int(e.tr)));
    o.set("pts", ptsV(e.pts));
    edges.push_back(std::move(o));
  });
  Value o = Value::object();
  o.set("nodes", Value(std::move(nodes)));
  o.set("edges", Value(std::move(edges)));
  return o;
}

Value encProvince(const Province& p) {
  Value o = Value::object();
  o.set("id", ref(Seq::Province, p.id));
  o.set("name", p.name);
  o.set("owner", ref(Seq::Faction, p.owner));
  o.set("lord", ref(Seq::Character, p.lord));
  o.set("sea", p.sea);
  o.set("capital", p.capital);
  o.set("size", enumV(en(schema::kProvSizes), int(p.size)));
  o.set("city", enumV(en(schema::kCityTypes), int(p.city)));
  o.set("resource", ref(Seq::Resource, p.resource));
  o.set("resourceAmount", fin(p.resourceAmount));
  json::Array g;
  for (auto& e : p.garrison) {
    Value x = Value::object();
    x.set("row", ref(Seq::Row, e.row));
    x.set("count", e.count);
    g.push_back(std::move(x));
  }
  o.set("garrison", Value(std::move(g)));
  o.set("contentment", fin(p.contentment));
  o.set("culture", ref(Seq::Culture, p.culture));
  o.set("religion", ref(Seq::Religion, p.religion));
  json::Array races;
  for (auto& e : p.races) {
    Value x = Value::object();
    x.set("race", ref(Seq::Race, e.race));
    x.set("pop", e.pop);
    races.push_back(std::move(x));
  }
  o.set("races", Value(std::move(races)));
  json::Array inf;
  for (auto& e : p.influence) {
    Value x = Value::object();
    x.set("guild", ref(Seq::Faction, e.guild));
    x.set("pct", fin(e.pct));
    inf.push_back(std::move(x));
  }
  o.set("influence", Value(std::move(inf)));
  o.set("hqs", refs(Seq::Faction, p.hqs));
  o.set("baseTrade", fin(p.baseTrade));
  o.set("localTax", fin(p.localTax));
  json::Array b;
  for (auto& e : p.buildings) {
    Value x = Value::object();
    x.set("building", ref(Seq::Building, e.building));
    x.set("level", e.level);
    x.set("constructing", e.constructing);
    x.set("left", e.left);
    if (e.constructing) {  // уплаченное за строящийся уровень и плательщик (возврат при отмене)
      x.set("paid", resV(e.paid));
      x.set("payer", ref(Seq::Faction, e.payer));
    }
    b.push_back(std::move(x));
  }
  o.set("buildings", Value(std::move(b)));
  o.set("modifiers", refs(Seq::Modifier, p.modifiers));
  o.set("occupied", p.occupied);
  o.set("occupier", ref(Seq::Faction, p.occupier));
  o.set("notes", p.notes);
  o.set("entity", p.entity);
  return o;
}

Value encFaction(const Faction& f) {
  Value o = Value::object();
  o.set("id", ref(Seq::Faction, f.id));
  o.set("kind", enumV(en(schema::kFactionKinds), int(f.kind)));
  o.set("name", f.name);
  o.set("color", colorV(f.color));
  Value flag = Value::object();
  flag.set("image", f.flag.image);
  flag.set("pattern", enumV(en(schema::kFlagPatterns), int(f.flag.pattern)));
  json::Array cols;
  for (Color c : f.flag.colors) cols.push_back(colorV(c));
  flag.set("colors", Value(std::move(cols)));
  flag.set("emblem", f.flag.emblem);
  flag.set("emblemColor", colorV(f.flag.emblemColor));
  flag.set("png", bytesV(f.flag.png));
  o.set("flag", std::move(flag));
  o.set("culture", ref(Seq::Culture, f.culture));
  o.set("government", ref(Seq::Government, f.government));
  o.set("religion", ref(Seq::Religion, f.religion));
  o.set("ruler", ref(Seq::Character, f.ruler));
  o.set("rulerTitle", f.rulerTitle);
  json::Array council;
  for (auto& s : f.council) {
    Value x = Value::object();
    x.set("id", ref(Seq::Council, s.id));
    x.set("position", s.position);
    x.set("character", ref(Seq::Character, s.character));
    council.push_back(std::move(x));
  }
  o.set("council", Value(std::move(council)));
  o.set("capital", ref(Seq::Province, f.capital));
  json::Array army;
  for (auto& r : f.army) {
    Value x = Value::object();
    x.set("id", ref(Seq::Row, r.id));
    x.set("name", r.name);
    x.set("type", enumV(en(schema::kUnitTypes), int(r.type)));
    x.set("total", r.total);
    x.set("upkeep", fin(r.upkeep));
    army.push_back(std::move(x));
  }
  o.set("army", Value(std::move(army)));
  json::Array fleet;
  for (auto& r : f.fleet) {
    Value x = Value::object();
    x.set("id", ref(Seq::Row, r.id));
    x.set("name", r.name);
    x.set("type", enumV(en(schema::kShipTypes), int(r.type)));
    x.set("total", r.total);
    x.set("upkeep", fin(r.upkeep));
    fleet.push_back(std::move(x));
  }
  o.set("fleet", Value(std::move(fleet)));
  o.set("res", resV(f.res));
  o.set("modifiers", refs(Seq::Modifier, f.modifiers));
  o.set("tax", fin(f.tax));
  o.set("homeState", ref(Seq::Faction, f.homeState));
  o.set("stateGuild", f.stateGuild);
  o.set("notes", f.notes);
  o.set("entity", f.entity);
  return o;
}

Value encCharacter(const Character& c) {
  Value o = Value::object();
  o.set("id", ref(Seq::Character, c.id));
  o.set("name", c.name);
  o.set("title", c.title);
  o.set("faction", ref(Seq::Faction, c.faction));
  o.set("hero", c.hero);
  o.set("upkeep", fin(c.upkeep));
  o.set("portrait", bytesV(c.portrait));
  o.set("notes", c.notes);
  o.set("entity", c.entity);
  return o;
}

Value encModifier(const Modifier& m) {
  Value o = Value::object();
  o.set("id", ref(Seq::Modifier, m.id));
  o.set("name", m.name);
  o.set("icon", m.icon);
  o.set("color", colorV(m.color));
  o.set("desc", m.desc);
  Value fx = Value::object();
  for (int i = 0; i < kFxCount; i++)
    if (m.has(Fx(i))) fx.set(schema::kEffects[i].id, fin(m.fx[size_t(i)]));
  o.set("fx", std::move(fx));
  o.set("targets", refs(Seq::Faction, m.targets));
  return o;
}

Value encBuilding(const Building& b) {
  Value o = Value::object();
  o.set("id", ref(Seq::Building, b.id));
  o.set("owner", ref(Seq::Faction, b.owner));
  o.set("name", b.name);
  o.set("icon", b.icon);
  o.set("cat", enumV(en(schema::kBuildingCats), int(b.cat)));
  o.set("desc", b.desc);
  json::Array req;
  for (auto& r : b.requires_) {
    Value x = Value::object();
    x.set("building", ref(Seq::Building, r.building));
    x.set("level", r.level);
    req.push_back(std::move(x));
  }
  o.set("requires", Value(std::move(req)));
  json::Array levels;
  for (auto& l : b.levels) {
    Value x = Value::object();
    x.set("turns", l.turns);
    x.set("cost", resV(l.cost));
    x.set("modifiers", refs(Seq::Modifier, l.modifiers));
    x.set("desc", l.desc);
    levels.push_back(std::move(x));
  }
  o.set("levels", Value(std::move(levels)));
  o.set("pos", posV(b.pos));
  return o;
}

Value encTech(const Tech& t) {
  Value o = Value::object();
  o.set("id", ref(Seq::Tech, t.id));
  o.set("faction", ref(Seq::Faction, t.faction));
  o.set("name", t.name);
  o.set("desc", t.desc);
  o.set("turns", t.turns);
  o.set("prereqs", refs(Seq::Tech, t.prereqs));
  o.set("modifiers", refs(Seq::Modifier, t.modifiers));
  o.set("studied", t.studied);
  o.set("research", t.research);
  o.set("progress", t.progress);
  o.set("pos", posV(t.pos));
  return o;
}

Value encArmy(const Army& a) {
  Value o = Value::object();
  o.set("id", ref(Seq::Army, a.id));
  o.set("kind", enumV(en(kArmyKindE), int(a.kind)));
  o.set("name", a.name);
  o.set("pos", posV(a.pos));
  json::Array groups;
  for (auto& g : a.groups) {
    Value x = Value::object();
    x.set("faction", ref(Seq::Faction, g.faction));
    json::Array units;
    for (auto& u : g.units) {
      Value y = Value::object();
      y.set("row", ref(Seq::Row, u.row));
      y.set("count", u.count);
      units.push_back(std::move(y));
    }
    x.set("units", Value(std::move(units)));
    x.set("heroes", refs(Seq::Character, g.heroes));
    groups.push_back(std::move(x));
  }
  o.set("groups", Value(std::move(groups)));
  o.set("commander", ref(Seq::Character, a.commander));
  return o;
}

Value encRoute(const Route& r) {
  Value o = Value::object();
  o.set("id", ref(Seq::Route, r.id));
  o.set("name", r.name);
  o.set("guild", ref(Seq::Faction, r.guild));
  o.set("pts", ptsV(r.pts));
  o.set("color", r.color ? colorV(*r.color) : Value());
  return o;
}

Value encDeal(const Deal& d) {
  Value o = Value::object();
  o.set("id", ref(Seq::Deal, d.id));
  o.set("kind", enumV(en(schema::kDealKinds), int(d.kind)));
  o.set("a", ref(Seq::Faction, d.a));
  o.set("b", ref(Seq::Faction, d.b));
  json::Array items;
  for (auto& it : d.items) {
    Value x = Value::object();
    x.set("from", enumV(en(kDealSideE), int(it.from)));
    x.set("res", ref(Seq::Resource, it.res));
    x.set("amount", fin(it.amount));
    x.set("mode", enumV(en(schema::kDealModes), int(it.mode)));
    x.set("turns", it.turns);
    x.set("left", it.left);
    items.push_back(std::move(x));
  }
  o.set("items", Value(std::move(items)));
  o.set("turn", d.turn);
  o.set("status", enumV(en(kDealStatusE), int(d.status)));
  o.set("note", d.note);
  return o;
}

Value encLog(const LogEntry& l) {
  Value o = Value::object();
  o.set("id", ref(Seq::Log, l.id));
  o.set("turn", l.turn);
  o.set("kind", enumV(en(schema::kLogKinds), int(l.kind)));
  o.set("text", l.text);
  o.set("province", ref(Seq::Province, l.province));
  o.set("army", ref(Seq::Army, l.army));
  o.set("factions", refs(Seq::Faction, l.factions));
  o.set("at", l.at);
  return o;
}

template <class T, class F>
Value encTable(const Table<T>& t, const char* key, F&& enc) {
  json::Array a;
  a.reserve(t.size());
  t.each([&](const T& e) { a.push_back(enc(e)); });
  Value o = Value::object();
  o.set(key, Value(std::move(a)));
  return o;
}

}  // namespace

Value encode(const World& w, FileId f) {
  switch (f) {
    case F_WORLD: {
      Value o = Value::object();
      o.set("format", kFormat);
      o.set("version", kVersion);
      o.set("meta", encMeta(*w.meta));
      o.set("settings", encSettings(*w.settings));
      return o;
    }
    case F_CATALOGS: return encCatalogs(*w.catalogs);
    case F_GEO: return encGeo(w);
    case F_PROVINCES: return encTable(w.provinces, "provinces", encProvince);
    case F_FACTIONS: return encTable(w.factions, "factions", encFaction);
    case F_CHARACTERS: return encTable(w.characters, "characters", encCharacter);
    case F_RELATIONS: {
      Value rel = Value::object();
      for (auto& [key, r] : *w.relations) {
        Id a = Id(key >> 32), b = Id(key & 0xFFFFFFFFu);
        Value x = Value::object();
        x.set("v", fin(r.v));
        x.set("s", enumV(en(schema::kRelStatus), int(r.s)));
        rel.set(refStr(Seq::Faction, a) + "|" + refStr(Seq::Faction, b), std::move(x));
      }
      Value o = Value::object();
      o.set("relations", std::move(rel));
      return o;
    }
    case F_MODIFIERS: return encTable(w.modifiers, "modifiers", encModifier);
    case F_BUILDINGS: return encTable(w.buildings, "buildings", encBuilding);
    case F_TECHS: return encTable(w.techs, "techs", encTech);
    case F_ARMIES: return encTable(w.armies, "armies", encArmy);
    case F_ROUTES: return encTable(w.routes, "routes", encRoute);
    case F_DEALS: return encTable(w.deals, "deals", encDeal);
    case F_LOG: return encTable(w.log, "log", encLog);
    default: return Value::object();
  }
}

// Запись «по строке на элемент»: верхний объект с отсортированными ключами, внутри — массивы и объекты,
// каждый элемент которых компактно в своей строке (порядок элементов и членов сохраняется).
static std::string formatLines(const Value& v) {
  const json::WriteOptions compact{0, true, true};
  std::vector<const json::Member*> order;
  for (auto& m : v.members()) order.push_back(&m);
  std::sort(order.begin(), order.end(), [](const json::Member* a, const json::Member* b) { return a->first < b->first; });
  std::string out = "{";
  for (size_t i = 0; i < order.size(); i++) {
    out += i ? ",\n  " : "\n  ";
    json::appendString(out, order[i]->first);
    out += ": ";
    const Value& x = order[i]->second;
    if (x.isArr() && !x.empty()) {
      out += "[";
      const auto& items = x.items();
      for (size_t k = 0; k < items.size(); k++) {
        out += k ? ",\n    " : "\n    ";
        json::write(out, items[k], compact);
      }
      out += "\n  ]";
    } else if (x.isObj() && !x.empty()) {
      out += "{";
      const auto& ms = x.members();
      for (size_t k = 0; k < ms.size(); k++) {
        out += k ? ",\n    " : "\n    ";
        json::appendString(out, ms[k].first);
        out += ": ";
        json::write(out, ms[k].second, compact);
      }
      out += "\n  }";
    } else {
      json::write(out, x, compact);
    }
  }
  out += order.empty() ? "}\n" : "\n}\n";
  return out;
}

std::string format(const Value& v, FileId f) {
  if (f == F_GEO || f == F_RELATIONS) return formatLines(v);
  std::string out;
  json::write(out, v, json::WriteOptions{2, true, true});
  out.push_back('\n');
  return out;
}

// ================================================================ чтение
namespace {

std::string show(const Value& v) {
  std::string s = json::write(v, json::WriteOptions{0, false, true});
  if (utf8::count(s) > 40) {
    size_t i = 0;
    for (int k = 0; k < 40 && i < s.size(); k++) i = utf8::next(s, i);
    s = s.substr(0, i) + "…";
  }
  return s;
}

struct Ctx {
  const char* file;
  Warnings& out;
  void warn(std::string where, std::string msg) { out.push_back(Warning{file, std::move(where), std::move(msg)}); }
};

// Объект записи JSON: чтение полей с проверкой типов и учётом неизвестных полей.
class Rec {
 public:
  Rec(const Value& v, Ctx& c, int index = -1) : v_(v), c_(c), index_(index) {}
  Rec(const Value& v, Ctx& c, const Rec* parent, std::string_view key, int index)
      : v_(v), c_(c), parent_(parent), key_(key), index_(index) {}
  Rec(const Rec&) = delete;
  Rec& operator=(const Rec&) = delete;

  Ctx& ctx() { return c_; }
  void setBase(std::string b) { base_ = std::move(b); }

  std::string where() const {
    if (!base_.empty()) return base_;
    std::string w;
    if (parent_) w = parent_->where(key_);
    if (index_ >= 0) w += "[" + std::to_string(index_) + "]";
    return w;
  }
  std::string where(std::string_view key) const {
    std::string w = where();
    if (!w.empty()) w += '.';
    w += key;
    return w;
  }
  void warn(std::string_view key, std::string msg) { c_.warn(where(key), std::move(msg)); }
  void warnHere(std::string msg) { c_.warn(where(), std::move(msg)); }

  // Поле записи (null — нет поля).
  const Value* get(std::string_view key) {
    const Value* x = v_.find(key);
    if (std::find(known_.begin(), known_.end(), key) == known_.end()) {
      known_.push_back(key);
      if (x) found_++;
    }
    return x;
  }
  bool has(std::string_view key) const { return v_.has(key); }

  std::string str(std::string_view key, std::string def = {}) {
    const Value* x = get(key);
    if (!x || x->isNull()) return def;
    if (x->isStr()) return x->asStr();
    if (x->isNum() || x->isBool()) {
      warn(key, "ожидалась строка, получено " + show(*x) + " — записано как строка");
      return show(*x);
    }
    warn(key, "ожидалась строка, получено " + show(*x) + " — поле пропущено");
    return def;
  }

  double num(std::string_view key, double def) {
    const Value* x = get(key);
    if (!x || x->isNull()) return def;
    if (x->isNum()) return x->asNum();
    if (x->isStr()) {
      if (auto d = parseNum(x->asStr()); d && std::isfinite(*d)) {
        warn(key, "число записано строкой " + show(*x));
        return *d;
      }
    }
    warn(key, "ожидалось число, получено " + show(*x) + " — поле пропущено");
    return def;
  }

  i64 integer(std::string_view key, i64 def) {
    const Value* x = get(key);
    if (!x || x->isNull()) return def;
    double d = 0;
    std::optional<double> parsed = x->isStr() ? parseNum(x->asStr()) : std::nullopt;
    if (x->isNum()) d = x->asNum();
    else if (parsed && std::isfinite(*parsed)) {
      d = *parsed;
      warn(key, "число записано строкой " + show(*x));
    } else {
      warn(key, "ожидалось целое число, получено " + show(*x) + " — поле пропущено");
      return def;
    }
    if (std::fabs(d) > 9.0e15) {
      warn(key, "слишком большое число " + show(*x) + " — поле пропущено");
      return def;
    }
    double r = std::nearbyint(d);
    if (r != d) warn(key, "ожидалось целое число, получено " + show(*x) + " — округлено");
    return i64(r);
  }

  int intv(std::string_view key, int def) {
    i64 v = integer(key, def);
    if (v < -2000000000 || v > 2000000000) {
      warn(key, "число вне допустимого диапазона — поле пропущено");
      return def;
    }
    return int(v);
  }

  bool flag(std::string_view key, bool def) {
    const Value* x = get(key);
    if (!x || x->isNull()) return def;
    if (x->isBool()) return x->asBool();
    if (x->isNum() && (x->asNum() == 0 || x->asNum() == 1)) {
      warn(key, "ожидалось true или false, получено " + show(*x));
      return x->asNum() == 1;
    }
    if (x->isStr() && (x->asStr() == "true" || x->asStr() == "false")) {
      warn(key, "ожидалось true или false, получено " + show(*x));
      return x->asStr() == "true";
    }
    warn(key, "ожидалось true или false, получено " + show(*x) + " — поле пропущено");
    return def;
  }

  Id refOf(const Value& x, std::string_view key, Seq s) {
    auto r = parseRef(x, s);
    if (!r) {
      warn(key, "неверная ссылка " + show(x) + " (ожидалось вида «" + std::string(schema::idPrefix(s)) + "12») — ссылка удалена");
      return 0;
    }
    return *r;
  }
  Id ref(std::string_view key, Seq s) {
    const Value* x = get(key);
    return x ? refOf(*x, key, s) : 0;
  }

  // ID записи: нет или неверный — 0 (новый ID назначается при сборке мира).
  Id id(Seq s) {
    const Value* x = get("id");
    if (!x || x->isNull()) {
      warnHere("нет поля id — будет назначен новый ID");
      return 0;
    }
    auto r = parseRef(*x, s);
    if (!r || *r == 0) {
      warn("id", "неверный ID " + show(*x) + " (ожидалось вида «" + std::string(schema::idPrefix(s)) + "12») — будет назначен новый");
      return 0;
    }
    return *r;
  }

  const Value* arr(std::string_view key) {
    const Value* x = get(key);
    if (!x || x->isNull()) return nullptr;
    if (!x->isArr()) {
      warn(key, "ожидался массив [...] — поле пропущено");
      return nullptr;
    }
    return x;
  }
  const Value* obj(std::string_view key) {
    const Value* x = get(key);
    if (!x || x->isNull()) return nullptr;
    if (!x->isObj()) {
      warn(key, "ожидался объект {...} — поле пропущено");
      return nullptr;
    }
    return x;
  }

  std::vector<Id> refs(std::string_view key, Seq s) {
    std::vector<Id> out;
    const Value* a = arr(key);
    if (!a) return out;
    out.reserve(a->size());
    const auto& items = a->items();
    for (size_t i = 0; i < items.size(); i++) {
      auto r = parseRef(items[i], s);
      if (!r) warn(std::string(key) + "[" + std::to_string(i) + "]",
                   "неверная ссылка " + show(items[i]) + " (ожидалось вида «" + std::string(schema::idPrefix(s)) + "12») — удалена");
      else if (*r) out.push_back(*r);
    }
    return out;
  }

  int enumv(std::string_view key, Enum e, int def) {
    const Value* x = get(key);
    if (!x || x->isNull()) return def;
    if (x->isStr()) {
      int i = schema::findEnum(e.list, e.n, x->asStr());
      if (i >= 0) return i;
    }
    warn(key, "неизвестное значение " + show(*x) + " (допустимо: " + enumChoices(e) + ") — взято «" + e.list[def].id + "»");
    return def;
  }

  std::optional<Color> colorOf(const Value& x, std::string_view key) {
    if (x.isStr())
      if (auto c = Color::parse(x.asStr())) return c;
    warn(key, "неверный цвет " + show(x) + " (ожидалось «#rrggbb») — взят цвет по умолчанию");
    return std::nullopt;
  }
  Color color(std::string_view key, Color def) {
    const Value* x = get(key);
    if (!x || x->isNull()) return def;
    return colorOf(*x, key).value_or(def);
  }
  std::optional<Color> optColor(std::string_view key) {
    const Value* x = get(key);
    if (!x || x->isNull()) return std::nullopt;
    return colorOf(*x, key);
  }

  Vec2 pos(std::string_view key) {
    const Value* x = get(key);
    if (!x || x->isNull()) return {};
    if (x->isArr() && x->size() == 2 && (*x)[0].isNum() && (*x)[1].isNum()) return {(*x)[0].asNum(), (*x)[1].asNum()};
    if (x->isObj() && x->get("x").isNum() && x->get("y").isNum()) return {x->get("x").asNum(), x->get("y").asNum()};
    warn(key, "ожидались координаты [x, y], получено " + show(*x) + " — взято [0, 0]");
    return {};
  }

  // Точки: плоский массив [x1, y1, x2, y2, ...] или пары [[x1, y1], ...].
  std::vector<Vec2> pts(std::string_view key) {
    std::vector<Vec2> out;
    const Value* a = arr(key);
    if (!a) return out;
    const auto& items = a->items();
    if (!items.empty() && items[0].isArr()) {
      out.reserve(items.size());
      for (size_t i = 0; i < items.size(); i++) {
        const Value& p = items[i];
        if (p.isArr() && p.size() == 2 && p[0].isNum() && p[1].isNum()) out.push_back({p[0].asNum(), p[1].asNum()});
        else warn(std::string(key) + "[" + std::to_string(i) + "]", "ожидалась точка [x, y] — пропущена");
      }
      return out;
    }
    out.reserve(items.size() / 2);
    for (size_t i = 0; i + 1 < items.size(); i += 2) {
      if (items[i].isNum() && items[i + 1].isNum()) out.push_back({items[i].asNum(), items[i + 1].asNum()});
      else warn(std::string(key) + "[" + std::to_string(i) + "]", "ожидались числа x, y — точка пропущена");
    }
    if (items.size() % 2) warn(key, "нечётное число координат — последняя отброшена");
    return out;
  }

  std::map<Id, double> resMap(std::string_view key) {
    std::map<Id, double> out;
    const Value* o = obj(key);
    if (!o) return out;
    for (auto& [k, v] : o->members()) {
      auto r = parseRef(Value(k), Seq::Resource);
      if (!r || !*r) {
        warn(std::string(key) + "." + k, "неверный ключ ресурса (ожидалось вида «rs1») — пропущен");
        continue;
      }
      if (!v.isNum()) {
        warn(std::string(key) + "." + k, "ожидалось число, получено " + show(v) + " — пропущено");
        continue;
      }
      out[*r] = v.asNum();
    }
    return out;
  }

  std::string bytes(std::string_view key) {
    const Value* x = get(key);
    if (!x || x->isNull()) return {};
    if (x->isStr()) {
      std::string_view t = x->asStr();
      // Допускается и data URI: «data:image/png;base64,...».
      if (t.substr(0, 5) == "data:")
        if (size_t comma = t.find(','); comma != std::string_view::npos && t.substr(0, comma).find(";base64") != std::string_view::npos)
          t.remove_prefix(comma + 1);
      if (auto d = codec::base64::decodeString(t)) return std::move(*d);
    }
    warn(key, "ожидались данные в base64 — поле пропущено");
    return {};
  }

  // Массив объектов: fn(Rec&) для каждого элемента.
  template <class F> void list(std::string_view key, F&& fn) {
    const Value* a = arr(key);
    if (!a) return;
    const auto& items = a->items();
    for (size_t i = 0; i < items.size(); i++) {
      if (!items[i].isObj()) {
        warn(std::string(key) + "[" + std::to_string(i) + "]", "ожидался объект {...} — элемент пропущен");
        continue;
      }
      Rec r(items[i], c_, this, key, int(i));
      fn(r);
      r.done();
    }
  }
  // Вложенный объект: fn(Rec&).
  template <class F> void sub(std::string_view key, F&& fn) {
    const Value* o = obj(key);
    if (!o) return;
    Rec r(*o, c_, this, key, -1);
    fn(r);
    r.done();
  }

  // Предупредить о неизвестных полях.
  void done() {
    if (found_ >= v_.size()) return;
    for (auto& m : v_.members()) {
      if (std::find(known_.begin(), known_.end(), std::string_view(m.first)) == known_.end())
        warn(m.first, "неизвестное поле отброшено");
    }
  }

 private:
  const Value& v_;
  Ctx& c_;
  const Rec* parent_ = nullptr;
  std::string_view key_;
  int index_ = -1;
  std::string base_;
  std::vector<std::string_view> known_;
  size_t found_ = 0;
};

i64 countOf(Rec& r, std::string_view key) { return r.integer(key, 0); }

void decMeta(Rec& r, Meta& m) {
  m.name = r.str("name", m.name);
  m.createdAt = r.str("createdAt");
  m.updatedAt = r.str("updatedAt");
  m.turn = r.intv("turn", 1);
  r.sub("seq", [&](Rec& s) {
    for (int i = 0; i < kSeqCount; i++) {
      i64 v = s.integer(kSeqNames[i], 0);
      if (v < 0 || v > i64(0xFFFFFFFFu)) {
        s.warn(kSeqNames[i], "счётчик вне диапазона — взят 0");
        v = 0;
      }
      m.seq[size_t(i)] = u32(v);
    }
  });
  m.basemap = r.str("basemap", m.basemap);
  m.notes = r.str("notes");
}

void decSettings(Rec& r, Settings& s) {
  s.fillOpacity = float(r.num("fillOpacity", s.fillOpacity));
  s.labelStates = r.flag("labelStates", s.labelStates);
  s.labelProvinces = r.flag("labelProvinces", s.labelProvinces);
  s.labelArmies = r.flag("labelArmies", s.labelArmies);
  s.occupiedIncome = OccupiedIncome(r.enumv("occupiedIncome", en(schema::kOccupiedIncome), int(s.occupiedIncome)));
  s.rebellionRoll = r.flag("rebellionRoll", s.rebellionRoll);
  s.autosaveFolder = r.flag("autosaveFolder", s.autosaveFolder);
  s.autosaveSec = r.intv("autosaveSec", s.autosaveSec);
}

void decCatalogs(Rec& r, Catalogs& c) {
  for (int i = 0; i < 6; i++) {
    auto& list = catalogList(c, i);
    list.clear();
    Seq s = kCatalogSeq[i];
    r.list(kCatalogKeys[i], [&](Rec& e) {
      CatalogItem it;
      it.id = e.id(s);
      if (it.id) e.setBase(std::string(kCatalogKeys[i]) + "." + refStr(s, it.id));
      it.name = e.str("name");
      it.color = e.color("color", it.color);
      it.icon = e.str("icon");
      it.builtin = e.flag("builtin", false);
      list.push_back(std::move(it));
    });
  }
}

void decGeo(Rec& r, Parts& p) {
  if (const Value* a = r.arr("nodes")) {
    const auto& items = a->items();
    p.nodes.reserve(items.size());
    for (size_t i = 0; i < items.size(); i++) {
      const Value& x = items[i];
      auto where = [i] { return "nodes[" + std::to_string(i) + "]"; };
      if (!(x.isArr() && x.size() == 3 && x[0].isNum() && x[1].isNum() && x[2].isNum())) {
        r.ctx().warn(where(), "ожидалось [id, x, y], получено " + show(x) + " — узел пропущен");
        continue;
      }
      auto id = parseRef(x[0], Seq::Node);
      if (!id || !*id) {
        r.ctx().warn(where(), "неверный ID узла " + show(x[0]) + " — узел пропущен");
        continue;
      }
      Node n;
      n.id = *id;
      n.p = {x[1].asNum(), x[2].asNum()};
      p.nodes.push_back(n);
    }
  }
  if (const Value* a = r.arr("edges")) {
    const auto& items = a->items();
    p.edges.reserve(items.size());
    for (size_t i = 0; i < items.size(); i++) {
      if (!items[i].isObj()) {
        r.ctx().warn("edges[" + std::to_string(i) + "]", "ожидался объект {...} — дуга пропущена");
        continue;
      }
      Rec e(items[i], r.ctx(), &r, "edges", int(i));
      Edge d;
      d.id = e.id(Seq::Edge);
      if (d.id) e.setBase("edges." + std::to_string(d.id));
      d.a = e.ref("a", Seq::Node);
      d.b = e.ref("b", Seq::Node);
      d.kind = EdgeKind(e.enumv("kind", en(kEdgeKindE), 0));
      d.pl = e.ref("pl", Seq::Province);
      d.pr = e.ref("pr", Seq::Province);
      d.tl = Terrain(e.enumv("tl", en(kTerrainE), int(Terrain::Land)));
      d.tr = Terrain(e.enumv("tr", en(kTerrainE), int(Terrain::Land)));
      d.pts = e.pts("pts");
      e.done();
      p.edges.push_back(std::move(d));
    }
  }
}

void decProvince(Rec& r, Province& p) {
  p.name = r.str("name");
  p.owner = r.ref("owner", Seq::Faction);
  p.lord = r.ref("lord", Seq::Character);
  p.sea = r.flag("sea", false);
  p.capital = r.str("capital");
  p.size = ProvSize(r.enumv("size", en(schema::kProvSizes), int(ProvSize::Medium)));
  p.city = CityType(r.enumv("city", en(schema::kCityTypes), int(CityType::Village)));
  p.resource = r.ref("resource", Seq::Resource);
  p.resourceAmount = r.num("resourceAmount", 0);
  r.list("garrison", [&](Rec& e) {
    GarrisonEntry g;
    g.row = e.ref("row", Seq::Row);
    g.count = countOf(e, "count");
    p.garrison.push_back(g);
  });
  p.contentment = r.num("contentment", 0);
  p.culture = r.ref("culture", Seq::Culture);
  p.religion = r.ref("religion", Seq::Religion);
  r.list("races", [&](Rec& e) {
    RacePop x;
    x.race = e.ref("race", Seq::Race);
    x.pop = countOf(e, "pop");
    p.races.push_back(x);
  });
  r.list("influence", [&](Rec& e) {
    Influence x;
    x.guild = e.ref("guild", Seq::Faction);
    x.pct = e.num("pct", 0);
    p.influence.push_back(x);
  });
  p.hqs = r.refs("hqs", Seq::Faction);
  p.baseTrade = r.num("baseTrade", 0);
  p.localTax = r.num("localTax", 0);
  r.list("buildings", [&](Rec& e) {
    ProvBuilding b;
    b.building = e.ref("building", Seq::Building);
    b.level = e.intv("level", 1);
    b.constructing = e.flag("constructing", false);
    b.left = e.intv("left", 0);
    if (e.has("paid") || e.has("payer")) {
      b.paid = e.resMap("paid");
      b.payer = e.ref("payer", Seq::Faction);
    } else if (b.constructing) {
      // Файл прежней версии: уплаченное не записано — плательщиком считается владелец, возврата при отмене нет.
      b.payer = p.owner;
      e.warnHere("уплаченная стоимость строительства не записана — при отмене стоимость не вернётся");
    }
    p.buildings.push_back(b);
  });
  p.modifiers = r.refs("modifiers", Seq::Modifier);
  p.occupied = r.flag("occupied", false);
  p.occupier = r.ref("occupier", Seq::Faction);
  p.notes = r.str("notes");
  p.entity = r.str("entity");
}

void decFaction(Rec& r, Faction& f) {
  f.kind = FactionKind(r.enumv("kind", en(schema::kFactionKinds), 0));
  f.name = r.str("name");
  f.color = r.color("color", f.color);
  r.sub("flag", [&](Rec& q) {
    Flag& fl = f.flag;
    fl.image = q.flag("image", false);
    fl.pattern = FlagPattern(q.enumv("pattern", en(schema::kFlagPatterns), 0));
    if (const Value* cols = q.arr("colors")) {
      const auto& items = cols->items();
      if (items.size() > 3) q.warn("colors", "больше трёх цветов — лишние отброшены");
      for (size_t i = 0; i < items.size() && i < 3; i++)
        if (auto c = q.colorOf(items[i], "colors[" + std::to_string(i) + "]")) fl.colors[i] = *c;
    }
    fl.emblem = q.str("emblem");
    fl.emblemColor = q.color("emblemColor", fl.emblemColor);
    fl.png = q.bytes("png");
  });
  f.culture = r.ref("culture", Seq::Culture);
  f.government = r.ref("government", Seq::Government);
  f.religion = r.ref("religion", Seq::Religion);
  f.ruler = r.ref("ruler", Seq::Character);
  f.rulerTitle = r.str("rulerTitle");
  r.list("council", [&](Rec& e) {
    CouncilSeat s;
    s.id = e.ref("id", Seq::Council);
    s.position = e.str("position");
    s.character = e.ref("character", Seq::Character);
    f.council.push_back(std::move(s));
  });
  f.capital = r.ref("capital", Seq::Province);
  r.list("army", [&](Rec& e) {
    ArmyRow a;
    a.id = e.ref("id", Seq::Row);
    a.name = e.str("name");
    a.type = UnitType(e.enumv("type", en(schema::kUnitTypes), 0));
    a.total = countOf(e, "total");
    a.upkeep = e.num("upkeep", 0);
    f.army.push_back(std::move(a));
  });
  r.list("fleet", [&](Rec& e) {
    FleetRow a;
    a.id = e.ref("id", Seq::Row);
    a.name = e.str("name");
    a.type = ShipType(e.enumv("type", en(schema::kShipTypes), int(ShipType::Frigate)));
    a.total = countOf(e, "total");
    a.upkeep = e.num("upkeep", 0);
    f.fleet.push_back(std::move(a));
  });
  f.res = r.resMap("res");
  f.modifiers = r.refs("modifiers", Seq::Modifier);
  f.tax = r.num("tax", 10);
  f.homeState = r.ref("homeState", Seq::Faction);
  f.stateGuild = r.flag("stateGuild", false);
  f.notes = r.str("notes");
  f.entity = r.str("entity");
}

void decCharacter(Rec& r, Character& c) {
  c.name = r.str("name");
  c.title = r.str("title");
  c.faction = r.ref("faction", Seq::Faction);
  c.hero = r.flag("hero", false);
  c.upkeep = r.num("upkeep", 0);
  c.portrait = r.bytes("portrait");
  c.notes = r.str("notes");
  c.entity = r.str("entity");
}

void decModifier(Rec& r, Modifier& m) {
  m.name = r.str("name");
  m.icon = r.str("icon", m.icon);
  m.color = r.color("color", m.color);
  m.desc = r.str("desc");
  r.sub("fx", [&](Rec& q) {
    for (int i = 0; i < kFxCount; i++) {
      const char* key = schema::kEffects[i].id;
      if (!q.has(key)) continue;
      double v = q.num(key, std::nan(""));
      if (std::isnan(v)) continue;
      m.fx[size_t(i)] = v;
      m.fxMask |= 1u << i;
    }
  });
  m.targets = r.refs("targets", Seq::Faction);
}

void decBuilding(Rec& r, Building& b) {
  b.owner = r.ref("owner", Seq::Faction);
  b.name = r.str("name");
  b.icon = r.str("icon", b.icon);
  b.cat = BuildingCat(r.enumv("cat", en(schema::kBuildingCats), int(BuildingCat::Economic)));
  b.desc = r.str("desc");
  r.list("requires", [&](Rec& e) {
    BuildingReq q;
    q.building = e.ref("building", Seq::Building);
    q.level = e.intv("level", 1);
    b.requires_.push_back(q);
  });
  if (r.has("levels")) {
    b.levels.clear();
    r.list("levels", [&](Rec& e) {
      BuildingLevel l;
      l.turns = e.intv("turns", 1);
      l.cost = e.resMap("cost");
      l.modifiers = e.refs("modifiers", Seq::Modifier);
      l.desc = e.str("desc");
      b.levels.push_back(std::move(l));
    });
  }
  b.pos = r.pos("pos");
}

void decTech(Rec& r, Tech& t) {
  t.faction = r.ref("faction", Seq::Faction);
  t.name = r.str("name");
  t.desc = r.str("desc");
  t.turns = r.intv("turns", 1);
  t.prereqs = r.refs("prereqs", Seq::Tech);
  t.modifiers = r.refs("modifiers", Seq::Modifier);
  t.studied = r.flag("studied", false);
  t.research = r.flag("research", false);
  t.progress = r.intv("progress", 0);
  t.pos = r.pos("pos");
}

void decArmy(Rec& r, Army& a) {
  a.kind = ArmyKind(r.enumv("kind", en(kArmyKindE), 0));
  a.name = r.str("name");
  a.pos = r.pos("pos");
  r.list("groups", [&](Rec& e) {
    ArmyGroup g;
    g.faction = e.ref("faction", Seq::Faction);
    e.list("units", [&](Rec& u) {
      ArmyUnit x;
      x.row = u.ref("row", Seq::Row);
      x.count = countOf(u, "count");
      g.units.push_back(x);
    });
    g.heroes = e.refs("heroes", Seq::Character);
    a.groups.push_back(std::move(g));
  });
  a.commander = r.ref("commander", Seq::Character);
}

void decRoute(Rec& r, Route& x) {
  x.name = r.str("name");
  x.guild = r.ref("guild", Seq::Faction);
  x.pts = r.pts("pts");
  x.color = r.optColor("color");
}

void decDeal(Rec& r, Deal& d) {
  d.kind = DealKind(r.enumv("kind", en(schema::kDealKinds), 0));
  d.a = r.ref("a", Seq::Faction);
  d.b = r.ref("b", Seq::Faction);
  r.list("items", [&](Rec& e) {
    DealItem it;
    it.from = DealSide(e.enumv("from", en(kDealSideE), 0));
    it.res = e.has("res") ? e.ref("res", Seq::Resource) : kGold;
    it.amount = e.num("amount", 0);
    it.mode = DealMode(e.enumv("mode", en(schema::kDealModes), 0));
    it.turns = e.intv("turns", 1);
    it.left = e.intv("left", 0);
    d.items.push_back(it);
  });
  d.turn = r.intv("turn", 1);
  d.status = DealStatus(r.enumv("status", en(kDealStatusE), 0));
  d.note = r.str("note");
}

void decLog(Rec& r, LogEntry& l) {
  l.turn = r.intv("turn", 1);
  l.kind = LogKind(r.enumv("kind", en(schema::kLogKinds), int(LogKind::Note)));
  l.text = r.str("text");
  l.province = r.ref("province", Seq::Province);
  l.army = r.ref("army", Seq::Army);
  l.factions = r.refs("factions", Seq::Faction);
  l.at = r.str("at");
}

// Массив записей таблицы: {"<key>": [...]} (или сразу массив — с предупреждением).
template <class T, class F>
void decTable(const Value& v, FileId f, Ctx& c, std::vector<T>& out, Seq s, F&& fn) {
  const char* key = kFiles[f].key;
  const Value* arr = nullptr;
  std::optional<Rec> top;
  if (v.isArr()) {
    c.warn("", std::string("ожидался объект {\"") + key + "\": [...]}, найден массив — прочитан как список записей");
    arr = &v;
  } else if (v.isObj()) {
    top.emplace(v, c);
    arr = top->arr(key);
    if (!arr && !v.has(key)) c.warn("", std::string("нет массива «") + key + "» — таблица пуста");
  } else {
    c.warn("", std::string("ожидался объект {\"") + key + "\": [...]} — таблица пуста");
  }
  if (arr) {
    const auto& items = arr->items();
    out.reserve(items.size());
    for (size_t i = 0; i < items.size(); i++) {
      if (!items[i].isObj()) {
        c.warn("[" + std::to_string(i) + "]", "запись должна быть объектом {...} — пропущена");
        continue;
      }
      Rec r(items[i], c, int(i));
      T e;
      e.id = r.id(s);
      if (e.id) r.setBase(refStr(s, e.id));
      fn(r, e);
      r.done();
      out.push_back(std::move(e));
    }
  }
  if (top) top->done();
}

}  // namespace

void checkVersion(const Value& v, const std::string& origin, Warnings& warns) {
  if (!v.isObj()) fail(origin + ": ожидался объект JSON с полями format, version, meta, settings");
  const Value& fmt = v.get("format");
  if (fmt.isNull()) warns.push_back({kFiles[F_WORLD].path, "format", "нет поля format — считается «regnum-world»"});
  else if (!fmt.isStr() || fmt.asStr() != kFormat)
    fail(origin + ": это не мир Regnum (поле format = " + show(fmt) + ", ожидалось «regnum-world»)");
  const Value& ver = v.get("version");
  if (ver.isNull()) {
    warns.push_back({kFiles[F_WORLD].path, "version", "нет поля version — считается 1"});
    return;
  }
  if (!ver.isNum() || !std::isfinite(ver.asNum()) || ver.asNum() < 1)
    fail(origin + ": неверная версия формата " + show(ver) + " (ожидалось целое число ≥ 1)");
  double major = std::floor(ver.asNum());
  if (major > kVersion)
    fail("Мир сохранён более новой версией Regnum (формат " + show(Value(major)) + ", эта версия читает формат " +
         std::to_string(kVersion) + "). Обновите редактор.");
}

void decode(const Value& v, FileId f, Parts& p, Warnings& warns) {
  Ctx c{kFiles[f].path, warns};
  p.present[f] = true;
  switch (f) {
    case F_WORLD: {
      if (!v.isObj()) {
        c.warn("", "ожидался объект — взяты значения по умолчанию");
        return;
      }
      Rec r(v, c);
      r.get("format");
      r.get("version");
      if (!r.has("meta")) c.warn("meta", "нет раздела meta — взяты значения по умолчанию");
      r.sub("meta", [&](Rec& m) { decMeta(m, p.meta); });
      r.sub("settings", [&](Rec& s) { decSettings(s, p.settings); });
      r.done();
      return;
    }
    case F_CATALOGS: {
      if (!v.isObj()) {
        c.warn("", "ожидался объект со списками справочников — справочники пусты");
        return;
      }
      Rec r(v, c);
      decCatalogs(r, p.catalogs);
      r.done();
      return;
    }
    case F_GEO: {
      if (!v.isObj()) {
        c.warn("", "ожидался объект {\"nodes\": [...], \"edges\": [...]} — геометрия пуста");
        return;
      }
      Rec r(v, c);
      decGeo(r, p);
      r.done();
      return;
    }
    case F_RELATIONS: {
      const Value* rel = nullptr;
      std::optional<Rec> top;
      if (v.isObj()) {
        top.emplace(v, c);
        rel = top->obj("relations");
      } else {
        c.warn("", "ожидался объект {\"relations\": {...}} — отношений нет");
      }
      if (rel) {
        for (auto& [k, x] : rel->members()) {
          size_t bar = k.find('|');
          std::optional<Id> a, b;
          if (bar != std::string::npos) {
            a = parseRef(Value(std::string_view(k).substr(0, bar)), Seq::Faction);
            b = parseRef(Value(std::string_view(k).substr(bar + 1)), Seq::Faction);
          }
          if (!a || !b || !*a || !*b) {
            c.warn(k, "неверный ключ пары (ожидалось вида «f1|f2») — отношение пропущено");
            continue;
          }
          if (*a == *b) {
            c.warn(k, "отношение фракции к самой себе — пропущено");
            continue;
          }
          if (!x.isObj()) {
            c.warn(k, "ожидался объект {\"v\": число, \"s\": состояние} — отношение пропущено");
            continue;
          }
          Rec r(x, c);
          r.setBase(k);
          Relation rr;
          rr.v = r.num("v", 0);
          rr.s = RelStatus(r.enumv("s", en(schema::kRelStatus), int(RelStatus::Unknown)));
          r.done();
          u64 key = relKey(*a, *b);
          if (p.relations.count(key)) c.warn(k, "пара указана повторно — взято последнее значение");
          p.relations[key] = rr;
        }
      }
      if (top) top->done();
      return;
    }
    case F_PROVINCES: decTable(v, f, c, p.provinces, Seq::Province, decProvince); return;
    case F_FACTIONS: decTable(v, f, c, p.factions, Seq::Faction, decFaction); return;
    case F_CHARACTERS: decTable(v, f, c, p.characters, Seq::Character, decCharacter); return;
    case F_MODIFIERS: decTable(v, f, c, p.modifiers, Seq::Modifier, decModifier); return;
    case F_BUILDINGS: decTable(v, f, c, p.buildings, Seq::Building, decBuilding); return;
    case F_TECHS: decTable(v, f, c, p.techs, Seq::Tech, decTech); return;
    case F_ARMIES: decTable(v, f, c, p.armies, Seq::Army, decArmy); return;
    case F_ROUTES: decTable(v, f, c, p.routes, Seq::Route, decRoute); return;
    case F_DEALS: decTable(v, f, c, p.deals, Seq::Deal, decDeal); return;
    case F_LOG: decTable(v, f, c, p.log, Seq::Log, decLog); return;
    default: return;
  }
}

json::Value parseFile(std::string_view text, const std::string& origin) {
  if (text.size() >= 3 && u8(text[0]) == 0xEF && u8(text[1]) == 0xBB && u8(text[2]) == 0xBF) text.remove_prefix(3);
  try {
    return json::parse(text);
  } catch (const json::ParseError& e) {
    fail(origin + ": " + e.what());
  }
}

// ================================================================ сборка мира
namespace {

template <class T>
void putAll(Tx& tx, std::vector<T>& items, const Table<T>& table, Seq s, FileId f, Meta& meta, Warnings& warns) {
  u32 dataMax = 0;
  for (const T& e : items) dataMax = std::max(dataMax, e.id);
  // Счётчик меньше ID в данных — его поднимет нормализация (с предупреждением); здесь он не трогается.
  const bool seqValid = meta.seq[size_t(s)] >= dataMax;
  u32 maxId = std::max(meta.seq[size_t(s)], dataMax);
  std::vector<size_t> pending;
  for (size_t i = 0; i < items.size(); i++) {
    T& e = items[i];
    if (e.id == 0 || table.has(e.id)) {
      pending.push_back(i);
      continue;
    }
    tx.add(std::move(e));
  }
  if (pending.empty()) return;
  for (size_t i : pending) {
    T& e = items[i];
    Id old = e.id;
    e.id = ++maxId;
    if (old) warns.push_back({kFiles[f].path, refStr(s, old), "повторный ID — запись получила новый ID " + refStr(s, e.id)});
    tx.add(std::move(e));
  }
  if (seqValid) meta.seq[size_t(s)] = maxId;
}

}  // namespace

World assemble(Parts&& p, Warnings& warns, u32* fixed) {
  World base;
  Tx tx(base);
  Meta meta = p.meta;
  putAll(tx, p.nodes, tx.w().nodes, Seq::Node, F_GEO, meta, warns);
  putAll(tx, p.edges, tx.w().edges, Seq::Edge, F_GEO, meta, warns);
  putAll(tx, p.provinces, tx.w().provinces, Seq::Province, F_PROVINCES, meta, warns);
  putAll(tx, p.factions, tx.w().factions, Seq::Faction, F_FACTIONS, meta, warns);
  putAll(tx, p.characters, tx.w().characters, Seq::Character, F_CHARACTERS, meta, warns);
  putAll(tx, p.modifiers, tx.w().modifiers, Seq::Modifier, F_MODIFIERS, meta, warns);
  putAll(tx, p.buildings, tx.w().buildings, Seq::Building, F_BUILDINGS, meta, warns);
  putAll(tx, p.techs, tx.w().techs, Seq::Tech, F_TECHS, meta, warns);
  putAll(tx, p.armies, tx.w().armies, Seq::Army, F_ARMIES, meta, warns);
  putAll(tx, p.routes, tx.w().routes, Seq::Route, F_ROUTES, meta, warns);
  putAll(tx, p.deals, tx.w().deals, Seq::Deal, F_DEALS, meta, warns);
  putAll(tx, p.log, tx.w().log, Seq::Log, F_LOG, meta, warns);
  tx.meta() = meta;
  tx.settings() = p.settings;
  tx.catalogs() = std::move(p.catalogs);
  tx.relations() = std::move(p.relations);
  World w = std::move(tx).finish();
  u32 mask = normalize(w, warns);
  for (const Warning& x : warns) mask |= tablesOfFile(x.file);
  if (fixed) *fixed = mask;
  // Предупреждения группируются по файлам (world.json, затем data/* в порядке записи); внутри файла —
  // в порядке появления: разбор, затем нормализация.
  auto order = [](const std::string& file) {
    if (file == kFiles[F_WORLD].path) return -1;
    int i = fileIndex(file);
    return i < 0 ? int(F_COUNT) : i;
  };
  std::stable_sort(warns.begin(), warns.end(), [&](const Warning& a, const Warning& b) { return order(a.file) < order(b.file); });
  return w;
}

}  // namespace rg::io::detail
