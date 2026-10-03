// Regnum — реализация модели мира, транзакций и хранилища.
#include "core/world.h"

namespace rg {

// ---------------------------------------------------------------- World
Relation World::relation(Id a, Id b) const {
  if (a == b) return Relation{100, RelStatus::Alliance};
  auto it = relations->find(relKey(a, b));
  return it == relations->end() ? Relation{} : it->second;
}

std::string World::factionName(Id id) const { const Faction* f = faction(id); return f ? (f->name.empty() ? "Без названия" : f->name) : "\xE2\x80\x94"; }
std::string World::provinceName(Id id) const { const Province* p = province(id); return p ? (p->name.empty() ? "Без названия" : p->name) : "\xE2\x80\x94"; }
std::string World::characterName(Id id) const { const Character* c = character(id); return c ? (c->name.empty() ? "Без имени" : c->name) : "\xE2\x80\x94"; }

u32 World::diff(const World& a, const World& b) {
  u32 m = 0;
  if (a.meta != b.meta) m |= TB_META;
  if (a.settings != b.settings) m |= TB_SETTINGS;
  if (a.catalogs != b.catalogs) m |= TB_CATALOGS;
  if (!a.nodes.same(b.nodes)) m |= TB_NODES;
  if (!a.edges.same(b.edges)) m |= TB_EDGES;
  if (!a.provinces.same(b.provinces)) m |= TB_PROVINCES;
  if (!a.factions.same(b.factions)) m |= TB_FACTIONS;
  if (!a.characters.same(b.characters)) m |= TB_CHARACTERS;
  if (a.relations != b.relations) m |= TB_RELATIONS;
  if (!a.modifiers.same(b.modifiers)) m |= TB_MODIFIERS;
  if (!a.buildings.same(b.buildings)) m |= TB_BUILDINGS;
  if (!a.techs.same(b.techs)) m |= TB_TECHS;
  if (!a.armies.same(b.armies)) m |= TB_ARMIES;
  if (!a.routes.same(b.routes)) m |= TB_ROUTES;
  if (!a.deals.same(b.deals)) m |= TB_DEALS;
  if (!a.log.same(b.log)) m |= TB_LOG;
  return m;
}

World newWorld(const std::string& name) {
  World w;
  auto meta = std::make_shared<Meta>();
  meta->name = name.empty() ? "Новый мир" : name;
  meta->createdAt = meta->updatedAt = nowIso();
  auto cat = std::make_shared<Catalogs>();
  auto item = [](Id id, const char* n, u32 color, const char* icon, bool builtin = false) {
    CatalogItem c; c.id = id; c.name = n; c.color = Color::hex(color); c.icon = icon; c.builtin = builtin; return c;
  };
  cat->resources = {
    item(kGold, "Золото", 0xe2b33c, "coins", true),
    item(2, "Зерно", 0xd9c36b, "grain"),
    item(3, "Древесина", 0x8a6a43, "wood"),
    item(4, "Камень", 0x9aa0a8, "stone"),
    item(5, "Железо", 0x6d7c8f, "iron"),
    item(6, "Лошади", 0xa0714f, "horse"),
  };
  meta->seq[int(Seq::Resource)] = 6;
  const char* govs[] = {"Монархия", "Империя", "Республика", "Теократия", "Олигархия", "Племенной союз"};
  for (int i = 0; i < 6; i++) cat->governments.push_back(item(Id(i + 1), govs[i], 0x888888, ""));
  meta->seq[int(Seq::Government)] = 6;
  const char* pos[] = {"Канцлер", "Казначей", "Маршал", "Адмирал", "Тайный советник", "Придворный маг"};
  for (int i = 0; i < 6; i++) cat->positions.push_back(item(Id(i + 1), pos[i], 0x888888, ""));
  meta->seq[int(Seq::Position)] = 6;
  w.meta = meta;
  w.catalogs = cat;
  return w;
}

// ---------------------------------------------------------------- Tx
Tx::Tx(const World& base)
    : w_(base),
      nodes_(&w_.nodes), edges_(&w_.edges), provinces_(&w_.provinces), factions_(&w_.factions),
      characters_(&w_.characters), modifiers_(&w_.modifiers), buildings_(&w_.buildings), techs_(&w_.techs),
      armies_(&w_.armies), routes_(&w_.routes), deals_(&w_.deals), log_(&w_.log) {}

Meta& Tx::meta() {
  if (!metaOwned_) { meta_ = std::make_shared<Meta>(*w_.meta); w_.meta = meta_; metaOwned_ = true; touched_ |= TB_META; }
  return *meta_;
}
Settings& Tx::settings() {
  if (!settingsOwned_) { settings_ = std::make_shared<Settings>(*w_.settings); w_.settings = settings_; settingsOwned_ = true; touched_ |= TB_SETTINGS; }
  return *settings_;
}
Catalogs& Tx::catalogs() {
  if (!catalogsOwned_) { catalogs_ = std::make_shared<Catalogs>(*w_.catalogs); w_.catalogs = catalogs_; catalogsOwned_ = true; touched_ |= TB_CATALOGS; }
  return *catalogs_;
}
RelMap& Tx::relations() {
  if (!relOwned_) { rel_ = std::make_shared<RelMap>(*w_.relations); w_.relations = rel_; relOwned_ = true; touched_ |= TB_RELATIONS; }
  return *rel_;
}

Id Tx::nextId(Seq s) { return ++meta().seq[int(s)]; }

void Tx::setRelation(Id a, Id b, Relation r) {
  if (a == 0 || b == 0 || a == b) return;
  r.v = clamp(r.v, -100.0, 100.0);
  relations()[relKey(a, b)] = r;
}

#define RG_TX_TABLE(Type, name, field, bit, SEQ, label)                                              \
  Type& Tx::name(Id id) {                                                                            \
    Type* p = field.mut(id);                                                                         \
    if (!p) fail(std::string(label) + " не найден(а): #" + std::to_string(id));                     \
    touched_ |= bit;                                                                                 \
    return *p;                                                                                       \
  }                                                                                                  \
  Type& Tx::add(Type v) {                                                                            \
    if (v.id == 0) v.id = nextId(SEQ);                                                               \
    else if (v.id > meta().seq[int(SEQ)]) meta().seq[int(SEQ)] = v.id;                               \
    touched_ |= bit;                                                                                 \
    return field.put(std::move(v));                                                                  \
  }

RG_TX_TABLE(Node, node, nodes_, TB_NODES, Seq::Node, "Узел")
RG_TX_TABLE(Edge, edge, edges_, TB_EDGES, Seq::Edge, "Граница")
RG_TX_TABLE(Province, province, provinces_, TB_PROVINCES, Seq::Province, "Провинция")
RG_TX_TABLE(Faction, faction, factions_, TB_FACTIONS, Seq::Faction, "Фракция")
RG_TX_TABLE(Character, character, characters_, TB_CHARACTERS, Seq::Character, "Персонаж")
RG_TX_TABLE(Modifier, modifier, modifiers_, TB_MODIFIERS, Seq::Modifier, "Модификатор")
RG_TX_TABLE(Building, building, buildings_, TB_BUILDINGS, Seq::Building, "Постройка")
RG_TX_TABLE(Tech, tech, techs_, TB_TECHS, Seq::Tech, "Технология")
RG_TX_TABLE(Army, army, armies_, TB_ARMIES, Seq::Army, "Войско")
RG_TX_TABLE(Route, route, routes_, TB_ROUTES, Seq::Route, "Маршрут")
RG_TX_TABLE(Deal, deal, deals_, TB_DEALS, Seq::Deal, "Сделка")
#undef RG_TX_TABLE

LogEntry& Tx::add(LogEntry v) {
  if (v.id == 0) v.id = nextId(Seq::Log);
  else if (v.id > meta().seq[int(Seq::Log)]) meta().seq[int(Seq::Log)] = v.id;
  touched_ |= TB_LOG;
  return log_.put(std::move(v));
}

#define RG_TX_ERASE(fn, field, bit) \
  void Tx::fn(Id id) { if (field.erase(id)) touched_ |= bit; }
RG_TX_ERASE(eraseNode, nodes_, TB_NODES)
RG_TX_ERASE(eraseEdge, edges_, TB_EDGES)
RG_TX_ERASE(eraseProvince, provinces_, TB_PROVINCES)
RG_TX_ERASE(eraseFaction, factions_, TB_FACTIONS)
RG_TX_ERASE(eraseCharacter, characters_, TB_CHARACTERS)
RG_TX_ERASE(eraseModifier, modifiers_, TB_MODIFIERS)
RG_TX_ERASE(eraseBuilding, buildings_, TB_BUILDINGS)
RG_TX_ERASE(eraseTech, techs_, TB_TECHS)
RG_TX_ERASE(eraseArmy, armies_, TB_ARMIES)
RG_TX_ERASE(eraseRoute, routes_, TB_ROUTES)
RG_TX_ERASE(eraseDeal, deals_, TB_DEALS)
#undef RG_TX_ERASE

void Tx::replaceWorld(const World& w) {
  w_ = w;
  metaOwned_ = settingsOwned_ = catalogsOwned_ = relOwned_ = false;
  meta_.reset();
  settings_.reset();
  catalogs_.reset();
  rel_.reset();
  nodes_ = TableEdit<Node>(&w_.nodes);
  edges_ = TableEdit<Edge>(&w_.edges);
  provinces_ = TableEdit<Province>(&w_.provinces);
  factions_ = TableEdit<Faction>(&w_.factions);
  characters_ = TableEdit<Character>(&w_.characters);
  modifiers_ = TableEdit<Modifier>(&w_.modifiers);
  buildings_ = TableEdit<Building>(&w_.buildings);
  techs_ = TableEdit<Tech>(&w_.techs);
  armies_ = TableEdit<Army>(&w_.armies);
  routes_ = TableEdit<Route>(&w_.routes);
  deals_ = TableEdit<Deal>(&w_.deals);
  log_ = TableEdit<LogEntry>(&w_.log);
  touched_ = TB_ALL;
}

World Tx::finish() && { return std::move(w_); }

// ---------------------------------------------------------------- Store
Store::Store() : cur_(newWorld("")), saved_(cur_) {}

void Store::commit(std::string_view label, Tx&& tx, const TxOptions& opt) {
  u32 touched = tx.touched();
  if (!touched) return;
  World next = std::move(tx).finish();
  u32 changed = World::diff(cur_, next);
  if (!changed) return;
  World before = cur_;
  cur_ = std::move(next);
  version_++;
  if (opt.history) {
    double now = nowSeconds();
    if (!opt.coalesce.empty() && !undo_.empty() && undo_.back().coalesce == opt.coalesce && now - undo_.back().time < opt.coalesceSec) {
      undo_.back().after = cur_;
      undo_.back().time = now;
    } else {
      undo_.push_back(Entry{std::string(label), before, cur_, opt.coalesce, now});
      if (undo_.size() > limit) undo_.erase(undo_.begin(), undo_.begin() + long(undo_.size() - limit));
    }
    redo_.clear();
  }
  Change c;
  c.kind = Change::Commit;
  c.label = std::string(label);
  c.tables = changed;
  c.before = &before;
  c.after = &cur_;
  notify(c);
}

bool Store::undo() {
  if (undo_.empty()) return false;
  Entry e = std::move(undo_.back());
  undo_.pop_back();
  World before = cur_;
  cur_ = e.before;
  version_++;
  Change c;
  c.kind = Change::Undo;
  c.label = e.label;
  c.tables = World::diff(before, cur_);
  c.before = &before;
  c.after = &cur_;
  redo_.push_back(std::move(e));
  notify(c);
  return true;
}

bool Store::redo() {
  if (redo_.empty()) return false;
  Entry e = std::move(redo_.back());
  redo_.pop_back();
  World before = cur_;
  cur_ = e.after;
  version_++;
  Change c;
  c.kind = Change::Redo;
  c.label = e.label;
  c.tables = World::diff(before, cur_);
  c.before = &before;
  c.after = &cur_;
  e.coalesce.clear();
  undo_.push_back(std::move(e));
  notify(c);
  return true;
}

void Store::endCoalesce() { if (!undo_.empty()) undo_.back().coalesce.clear(); }
void Store::clearHistory() { undo_.clear(); redo_.clear(); }

void Store::replace(World w, std::string_view label, bool mark) {
  World before = cur_;
  cur_ = std::move(w);
  version_++;
  undo_.clear();
  redo_.clear();
  if (mark) saved_ = cur_;
  Change c;
  c.kind = Change::Load;
  c.label = std::string(label);
  c.tables = TB_ALL;
  c.before = &before;
  c.after = &cur_;
  notify(c);
}

void Store::markSaved() { saved_ = cur_; }

int Store::subscribe(std::function<void(const Change&)> fn) {
  int t = nextSub_++;
  subs_.emplace_back(t, std::move(fn));
  return t;
}

void Store::unsubscribe(int token) {
  subs_.erase(std::remove_if(subs_.begin(), subs_.end(), [&](auto& p) { return p.first == token; }), subs_.end());
}

void Store::notify(const Change& c) {
  auto copy = subs_;
  for (auto& [t, fn] : copy) {
    try { fn(c); } catch (const std::exception& e) { logError("store subscriber: %s", e.what()); }
  }
}

}  // namespace rg
