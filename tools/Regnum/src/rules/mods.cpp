// Regnum — источники модификаторов и общие помощники правил (тексты, проверки, запасы).
#include "rules/internal.h"

namespace rg::rules {

namespace detail {

// ---------------------------------------------------------------- тексты
std::string amount(double v) { return fmtNum(v, 2); }

std::string resName(const World& w, Id res) {
  const CatalogItem* c = w.resource(res);
  if (!c) return "ресурс #" + std::to_string(res);
  return c->name.empty() ? "Без названия" : c->name;
}
std::string facName(const World& w, Id f) { return q(w.factionName(f)); }
std::string provName(const World& w, Id p) { return q(w.provinceName(p)); }
std::string armyName(const World& w, Id a) {
  const Army* x = w.army(a);
  if (!x) return q("—");
  if (!x->name.empty()) return q(x->name);
  return q(x->isFleet() ? "Флот" : "Войско");
}
std::string buildingName(const World& w, Id b) {
  const Building* x = w.building(b);
  return q(x ? (x->name.empty() ? "Без названия" : x->name) : "—");
}
std::string techName(const World& w, Id t) {
  const Tech* x = w.tech(t);
  return q(x ? (x->name.empty() ? "Без названия" : x->name) : "—");
}

std::string uniqueName(const std::vector<std::string>& taken, const std::string& base) {
  auto used = [&](const std::string& n) {
    std::string k = utf8::searchKey(n);
    for (auto& t : taken)
      if (utf8::searchKey(t) == k) return true;
    return false;
  };
  if (!used(base)) return base;
  for (int i = 2;; i++) {
    std::string n = base + " " + std::to_string(i);
    if (!used(n)) return n;
  }
}

// ---------------------------------------------------------------- проверки
const Faction& needFaction(const World& w, Id id) {
  const Faction* f = w.faction(id);
  if (!f) fail(id ? "Фракция не найдена" : "Не выбрана фракция");
  return *f;
}
const Faction& needState(const World& w, Id id) {
  const Faction* f = w.faction(id);
  if (!f) fail(id ? "Государство не найдено" : "Не выбрано государство");
  if (!f->isState()) fail(facName(w, id) + " — торговая гильдия, а нужно государство");
  return *f;
}
const Faction& needGuild(const World& w, Id id) {
  const Faction* f = w.faction(id);
  if (!f) fail(id ? "Гильдия не найдена" : "Не выбрана гильдия");
  if (!f->isGuild()) fail(facName(w, id) + " — государство, а нужна торговая гильдия");
  return *f;
}
const Province& needProvince(const World& w, Id id) {
  const Province* p = w.province(id);
  if (!p) fail(id ? "Провинция не найдена" : "Не выбрана провинция");
  return *p;
}
const Character& needCharacter(const World& w, Id id) {
  const Character* c = w.character(id);
  if (!c) fail(id ? "Персонаж не найден" : "Не выбран персонаж");
  return *c;
}
const Army& needArmy(const World& w, Id id) {
  const Army* a = w.army(id);
  if (!a) fail(id ? "Войско не найдено" : "Не выбрано войско");
  return *a;
}
const Building& needBuilding(const World& w, Id id) {
  const Building* b = w.building(id);
  if (!b) fail(id ? "Постройка не найдена" : "Не выбрана постройка");
  return *b;
}
const Tech& needTech(const World& w, Id id) {
  const Tech* t = w.tech(id);
  if (!t) fail(id ? "Технология не найдена" : "Не выбрана технология");
  return *t;
}
void needFinite(double v, const char* what) {
  if (!std::isfinite(v)) fail(std::string(what) + ": недопустимое число");
}

// ---------------------------------------------------------------- запасы
void addStock(Faction& f, Id res, double delta) {
  double& v = f.res[res];
  v += delta;
  if (std::fabs(v) < 1e-9) v = 0;  // без «−0» и хвостов округления
}

std::shared_ptr<const geo::FaceSet> facesFor(const Tx& tx) {
  return (tx.touched() & TB_GEO) ? geo::buildFaces(tx.w()) : geo::faces(tx.w());
}

// ---------------------------------------------------------------- эффекты
SourceIndex::SourceIndex(const World& w) {
  w.techs.each([&](const Tech& t) {
    if (t.studied && t.faction && !t.modifiers.empty()) studied[t.faction].push_back(&t);
  });
}
const std::vector<const Tech*>* SourceIndex::of(Id faction) const {
  auto it = studied.find(faction);
  return it == studied.end() ? nullptr : &it->second;
}

namespace {

// Сумматор эффектов: local — только локальные эффекты (провинция), иначе только глобальные (фракция).
struct FxSum {
  const World& w;
  bool local;
  Id self;
  Effects e;

  void add(EffectSource::Kind kind, Id id, Id mod) {
    const Modifier* m = w.modifier(mod);
    if (!m) return;
    bool any = false;
    for (int i = 0; i < kFxCount; i++) {
      if (!m->has(Fx(i)) || schema::kEffects[i].local != local) continue;
      double v = m->fx[size_t(i)];
      if (!std::isfinite(v)) continue;
      e.v[size_t(i)] += v;
      any = true;
      if (Fx(i) == Fx::DiplomacyPerTurn) {
        std::vector<Id> seen;
        for (Id t : m->targets) {
          if (t == 0 || t == self || contains(seen, t) || !w.faction(t)) continue;
          seen.push_back(t);
          e.diplomacy[t] += v;
        }
      }
    }
    if (any) e.sources.push_back(EffectSource{kind, id, mod});
  }

  // Модификаторы фракции и её изученных технологий.
  void faction(const SourceIndex& si, const Faction& f, EffectSource::Kind modKind) {
    for (Id m : f.modifiers) add(modKind, f.id, m);
    if (auto* ts = si.of(f.id))
      for (const Tech* t : *ts)
        for (Id m : t->modifiers) add(EffectSource::Tech, t->id, m);
  }

  // Постройки провинции: набор модификаторов текущего достроенного уровня.
  void buildings(const Province& p) {
    for (const ProvBuilding& pb : p.buildings) {
      const Building* b = w.building(pb.building);
      int lvl = pb.builtLevel();
      if (!b || lvl < 1 || lvl > int(b->levels.size())) continue;
      for (Id m : b->levels[size_t(lvl - 1)].modifiers) add(EffectSource::Building, b->id, m);
    }
  }
};

}  // namespace

Effects provinceFx(const World& w, const SourceIndex& si, const Province& p) {
  FxSum s{w, true, 0, {}};
  if (p.sea) return s.e;  // морские провинции не участвуют в расчётах
  for (Id m : p.modifiers) s.add(EffectSource::Province, p.id, m);
  if (const Faction* o = w.faction(p.owner); o && o->isState()) s.faction(si, *o, EffectSource::Faction);
  s.buildings(p);
  for (Id g : p.hqs)
    if (const Faction* gf = w.faction(g); gf && gf->isGuild()) s.faction(si, *gf, EffectSource::Guild);
  return std::move(s.e);
}

Effects factionFx(const World& w, const SourceIndex& si, const Faction& f, const std::vector<const Province*>& owned) {
  FxSum s{w, false, f.id, {}};
  s.faction(si, f, EffectSource::Faction);
  if (f.isState())
    for (const Province* p : owned)
      if (!p->sea && p->owner == f.id) s.buildings(*p);
  return std::move(s.e);
}

int slotsOf(const Province& p, const Effects& fx) {
  if (p.sea) return 0;
  int size = int(p.size) >= 0 && int(p.size) < 3 ? schema::kProvSizes[int(p.size)].value : 0;
  int city = int(p.city) >= 0 && int(p.city) < 4 ? schema::kCityTypes[int(p.city)].value : 0;
  long mods = std::lround(fx[Fx::Slots]);
  return int(std::max<long>(0, long(size) + long(city) + mods));
}

double costFactorOf(const Effects& fx) { return std::max(0.0, 1.0 + fx[Fx::BuildCostPct] / 100.0); }

}  // namespace detail

// ================================================================ публичный интерфейс
Effects provinceEffects(const World& w, Id province) {
  const Province* p = w.province(province);
  if (!p) return {};
  detail::SourceIndex si(w);
  return detail::provinceFx(w, si, *p);
}

Effects factionEffects(const World& w, Id faction) {
  const Faction* f = w.faction(faction);
  if (!f) return {};
  detail::SourceIndex si(w);
  std::vector<const Province*> owned;
  if (f->isState()) w.provinces.each([&](const Province& p) { if (p.owner == faction && !p.sea) owned.push_back(&p); });
  return detail::factionFx(w, si, *f, owned);
}

}  // namespace rg::rules
