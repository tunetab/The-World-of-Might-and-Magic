// Regnum — общие виджеты предметной области (см. widgets.h).
#include "app/widgets.h"

#include "gfx/icons.h"

#include <algorithm>
#include <map>

namespace rg::app {
namespace edkit {   // поток фишек с переносом строк (editors/modifiers.cpp)
void chipsBegin();
ui::ChipAction chip(std::string_view label, const ui::ChipOpt& o);
void chipsEnd();
}  // namespace edkit
}  // namespace rg::app

namespace rg::app::w {

namespace {

// Запись справочника, созданная через «Добавить…»: выбирается при следующем вызове того же виджета.
std::map<ui::WidgetId, Id>& pendingCreated() {
  static std::map<ui::WidgetId, Id> m;
  return m;
}

// Хранилище подписей на время вызова виджета (ui::Option ссылается на строки).
struct Options {
  std::vector<std::string> labels, hints;
  std::vector<ui::Option> opts;
  std::vector<Id> ids;
  void reserve(size_t n) { labels.reserve(n); hints.reserve(n); opts.reserve(n); ids.reserve(n); }
  void add(Id id, std::string label, const char* icon, Color color, std::string hint = {}, bool disabled = false) {
    ids.push_back(id);
    labels.push_back(std::move(label));
    hints.push_back(std::move(hint));
    ui::Option o;
    o.icon = icon;
    o.color = color;
    o.disabled = disabled;
    opts.push_back(o);
  }
  // Строки переезжают при росте векторов — ссылки проставляются в конце.
  std::span<const ui::Option> finish() {
    for (size_t i = 0; i < opts.size(); i++) { opts[i].label = labels[i]; opts[i].hint = hints[i]; }
    return opts;
  }
  int indexOf(Id id) const {
    if (id == 0) return -1;
    for (size_t i = 0; i < ids.size(); i++) if (ids[i] == id) return int(i);
    return -1;
  }
};

std::string orUnnamed(const std::string& s, const char* fallback) { return s.empty() ? std::string(fallback) : s; }

constexpr Id kCreate = 0xFFFFFFFFu;

}  // namespace

Color factionColor(const World& w, Id faction) {
  const Faction* f = w.faction(faction);
  return f ? f->color : Color::hex(0x8a8f99);
}

bool factionPicker(std::string_view id, Id& value, FactionFilter filter, std::string_view noneLabel, Id exclude, bool disabled) {
  const World& w = app().world();
  std::vector<const Faction*> list;
  w.factions.each([&](const Faction& f) {
    if (f.id == exclude) return;
    if (filter == FactionFilter::States && !f.isState()) return;
    if (filter == FactionFilter::Guilds && !f.isGuild()) return;
    list.push_back(&f);
  });
  std::sort(list.begin(), list.end(), [](const Faction* a, const Faction* b) {
    if (a->kind != b->kind) return a->kind < b->kind;
    return compareRu(a->name, b->name) < 0;
  });
  Options o;
  o.reserve(list.size());
  for (const Faction* f : list) o.add(f->id, orUnnamed(f->name, "Без названия"), nullptr, f->color, f->isGuild() ? "гильдия" : "");
  int idx = o.indexOf(value);
  ui::ComboOpt co;
  co.noneLabel = noneLabel;
  co.disabled = disabled || app().readOnly();
  co.icon = filter == FactionFilter::Guilds ? "guild" : "crown";
  co.placeholder = noneLabel.empty() ? std::string_view("—") : noneLabel;
  if (!ui::combo(id, idx, o.finish(), co)) return false;
  Id nv = idx >= 0 && idx < int(o.ids.size()) ? o.ids[size_t(idx)] : 0;
  if (nv == value) return false;
  value = nv;
  return true;
}

bool provincePicker(std::string_view id, Id& value, Id owner, std::string_view noneLabel, bool disabled) {
  const World& w = app().world();
  std::vector<const Province*> list;
  w.provinces.each([&](const Province& p) {
    if (p.sea) return;
    if (owner && p.owner != owner) return;
    list.push_back(&p);
  });
  std::sort(list.begin(), list.end(), [](const Province* a, const Province* b) { return compareRu(a->name, b->name) < 0; });
  Options o;
  o.reserve(list.size());
  for (const Province* p : list) o.add(p->id, orUnnamed(p->name, "Без названия"), "province", factionColor(w, p->owner), p->capital);
  int idx = o.indexOf(value);
  ui::ComboOpt co;
  co.noneLabel = noneLabel;
  co.disabled = disabled || app().readOnly();
  co.icon = "province";
  if (!ui::combo(id, idx, o.finish(), co)) return false;
  Id nv = idx >= 0 && idx < int(o.ids.size()) ? o.ids[size_t(idx)] : 0;
  if (nv == value) return false;
  value = nv;
  return true;
}

bool characterPicker(std::string_view id, Id& value, Id faction, std::string_view noneLabel, bool allowCreate, bool disabled) {
  App& a = app();
  const World& w = a.world();
  std::vector<const Character*> list;
  w.characters.each([&](const Character& c) { list.push_back(&c); });
  std::sort(list.begin(), list.end(), [faction](const Character* x, const Character* y) {
    bool fx = faction && x->faction == faction, fy = faction && y->faction == faction;
    if (fx != fy) return fx;
    return compareRu(x->name, y->name) < 0;
  });
  Options o;
  o.reserve(list.size() + 1);
  for (const Character* c : list) {
    std::string hint = c->title;
    if (c->faction && c->faction != faction) {
      if (!hint.empty()) hint += " · ";
      hint += w.factionName(c->faction);
    }
    o.add(c->id, orUnnamed(c->name, "Без имени"), c->hero ? "hero" : "character", c->faction ? factionColor(w, c->faction) : Color(0, 0, 0, 0), hint);
  }
  if (allowCreate) o.add(kCreate, "Новый персонаж", "user-plus", Color(0, 0, 0, 0));
  int idx = o.indexOf(value);
  ui::ComboOpt co;
  co.noneLabel = noneLabel;
  co.disabled = disabled || a.readOnly();
  co.icon = "character";
  if (!ui::combo(id, idx, o.finish(), co)) return false;
  Id nv = idx >= 0 && idx < int(o.ids.size()) ? o.ids[size_t(idx)] : 0;
  if (nv == kCreate) {
    Id created = 0;
    if (!a.act("Новый персонаж", [&](Tx& tx) { created = rules::createCharacter(tx, faction, "Новый персонаж"); })) return false;
    nv = created;
  }
  if (nv == value) return false;
  value = nv;
  return true;
}

bool catalogPicker(std::string_view id, rules::CatalogList list, Id& value, std::string_view noneLabel, bool allowCreate, bool disabled) {
  App& a = app();
  const World& w = a.world();
  const auto& items = rules::catalogList(*w.catalogs, list);
  Options o;
  o.reserve(items.size() + 1);
  const char* defIcon = nullptr;
  switch (list) {
    case rules::CatalogList::Resources: defIcon = "resource"; break;
    case rules::CatalogList::Races: defIcon = "race"; break;
    case rules::CatalogList::Cultures: defIcon = "culture"; break;
    case rules::CatalogList::Religions: defIcon = "religion"; break;
    case rules::CatalogList::Governments: defIcon = "crown"; break;
    case rules::CatalogList::Positions: defIcon = "council"; break;
  }
  bool colored = list != rules::CatalogList::Governments && list != rules::CatalogList::Positions;
  for (const auto& c : items) {
    const char* icon = !c.icon.empty() && gfx::hasIcon(c.icon) ? c.icon.c_str() : (colored ? nullptr : defIcon);
    o.add(c.id, orUnnamed(c.name, "Без названия"), icon, colored ? c.color : Color(0, 0, 0, 0));
  }
  if (allowCreate) o.add(kCreate, "Добавить…", "plus", Color(0, 0, 0, 0));
  const ui::WidgetId wid = ui::id(id);
  if (auto it = pendingCreated().find(wid); it != pendingCreated().end()) {
    Id created = it->second;
    pendingCreated().erase(it);
    if (created && created != value && Catalogs::find(items, created)) {
      value = created;
      return true;
    }
  }
  int idx = o.indexOf(value);
  ui::ComboOpt co;
  co.noneLabel = noneLabel;
  co.disabled = disabled || a.readOnly();
  co.icon = defIcon;
  if (!ui::combo(id, idx, o.finish(), co)) return false;
  Id nv = idx >= 0 && idx < int(o.ids.size()) ? o.ids[size_t(idx)] : 0;
  if (nv == kCreate) {
    // Создание — асинхронно (запрос названия); текущее значение не меняется в этом кадре.
    a.prompt("Новая запись справочника", "Название", "", [list, wid](App& app2, const std::string& name) {
      Id created = 0;
      if (app2.act("Добавить в справочник", [&](Tx& tx) { created = rules::addCatalogItem(tx, list, name); }))
        pendingCreated()[wid] = created;   // выбирается в следующем кадре вызовом того же виджета
    });
    return false;
  }
  if (nv == value) return false;
  value = nv;
  return true;
}

bool modifierInert(const Modifier& m, ModScope where) {
  if (where == ModScope::Any) return false;
  bool any = false, used = false;
  for (int f = 0; f < kFxCount; f++) {
    if (!m.has(Fx(f))) continue;
    any = true;
    if (schema::kEffects[f].local) used = true;
  }
  return any && !used;
}

bool modifierList(std::string_view id, std::vector<Id>& ids, bool disabled, ModScope where) {
  App& a = app();
  const World& w = a.world();
  const std::string mark(id);
  ui::IdScope scope(id);
  bool changed = false;
  disabled = disabled || a.readOnly();
  bool anyChip = false;
  for (Id mid : ids) if (w.modifier(mid)) anyChip = true;
  if (anyChip) {
    edkit::chipsBegin();
    for (size_t i = 0; i < ids.size(); i++) {
      const Modifier* m = w.modifier(ids[i]);
      if (!m) continue;
      ui::IdScope s2{i64(ids[i])};
      ui::ChipOpt co;
      co.icon = m->icon.empty() ? "sparkles" : m->icon.c_str();
      co.color = m->color;
      co.removable = !disabled;
      co.clickable = true;
      std::string tip;
      for (int f = 0; f < kFxCount; f++) {
        if (!m->has(Fx(f))) continue;
        if (!tip.empty()) tip += "\n";
        tip += effectText(Fx(f), m->fx[size_t(f)]);
      }
      if (!m->desc.empty()) tip = m->desc + (tip.empty() ? "" : "\n" + tip);
      // Модификатор только с глобальными эффектами в провинции ни на что не влияет — предупреждение на фишке.
      const bool inert = modifierInert(*m, where);
      if (inert) {
        co.icon = "warning";
        co.color = Color(0, 0, 0, 0);
        co.tone = ui::Tone::Warning;
        tip = "Не действует в провинции: у модификатора только глобальные эффекты, они применяются к государству. "
              "Добавьте его государству (вкладка «Модификаторы»).\n" + tip;
      }
      co.tooltip = tip;
      ui::ChipAction act = edkit::chip(orUnnamed(m->name, "Модификатор"), co);
      if (inert) a.markUi(mark + ".warn." + std::to_string(m->id));
      if (act == ui::ChipAction::Remove) {
        ids.erase(ids.begin() + long(i));
        changed = true;
        break;
      }
      if (act == ui::ChipAction::Click) a.openEditor("modifiers", m->id);
    }
    edkit::chipsEnd();
  }
  if (!disabled) {
    Options o;
    int hidden = 0;   // не действующие здесь (в провинции — только с глобальными эффектами) не предлагаются
    w.modifiers.each([&](const Modifier& m) {
      if (std::find(ids.begin(), ids.end(), m.id) != ids.end()) return;
      if (modifierInert(m, where)) {
        hidden++;
        return;
      }
      o.add(m.id, orUnnamed(m.name, "Модификатор"), m.icon.empty() ? "sparkles" : m.icon.c_str(), m.color);
    });
    const std::string hiddenTip = "Модификаторы только с глобальными эффектами не предлагаются: в провинции они не действуют (скрыто: " +
                                  std::to_string(hidden) + ")";
    if (!o.ids.empty()) {
      int idx = -1;
      ui::ComboOpt co;
      co.placeholder = "Добавить модификатор";
      co.icon = "plus";
      if (hidden > 0) co.tooltip = hiddenTip;
      if (ui::combo("add", idx, o.finish(), co) && idx >= 0 && idx < int(o.ids.size())) {
        ids.push_back(o.ids[size_t(idx)]);
        changed = true;
      }
    } else if (w.modifiers.empty()) {
      if (ui::link("Создать модификатор", "sparkles")) a.openEditor("modifiers", 0);
    }
  }
  return changed;
}

void factionChip(Id faction, bool showKind) {
  App& a = app();
  const World& w = a.world();
  const Faction* f = w.faction(faction);
  ui::IdScope s(i64(faction) + 0x10000000LL);
  if (!f) {
    ui::chip("—", {});
    return;
  }
  ui::ChipOpt co;
  co.color = f->color;
  co.clickable = true;
  co.icon = showKind ? (f->isGuild() ? "guild" : "crown") : nullptr;
  co.tooltip = f->isGuild() ? "Торговая гильдия — открыть" : "Государство — открыть";
  if (ui::chip(orUnnamed(f->name, "Без названия"), co) == ui::ChipAction::Click) a.select(SelType::Faction, faction);
}

void provinceChip(Id province) {
  App& a = app();
  const World& w = a.world();
  const Province* p = w.province(province);
  ui::IdScope s(i64(province) + 0x20000000LL);
  if (!p) {
    ui::chip("—", {});
    return;
  }
  ui::ChipOpt co;
  co.icon = p->sea ? "sea" : "province";
  co.color = p->owner ? factionColor(w, p->owner) : Color(0, 0, 0, 0);
  co.clickable = true;
  co.tooltip = "Провинция — открыть и показать на карте";
  if (ui::chip(orUnnamed(p->name, "Без названия"), co) == ui::ChipAction::Click) a.select(SelType::Province, province, true);
}

void characterChip(Id character) {
  App& a = app();
  const World& w = a.world();
  const Character* c = w.character(character);
  ui::IdScope s(i64(character) + 0x30000000LL);
  if (!c) {
    ui::chip("—", {});
    return;
  }
  ui::ChipOpt co;
  co.icon = c->hero ? "hero" : "character";
  co.color = c->faction ? factionColor(w, c->faction) : Color(0, 0, 0, 0);
  co.clickable = true;
  co.tooltip = c->title.empty() ? std::string_view("Персонаж — открыть") : std::string_view(c->title);
  if (ui::chip(orUnnamed(c->name, "Без имени"), co) == ui::ChipAction::Click) a.select(SelType::Character, character);
}

std::string effectText(Fx f, double v) {
  const auto& e = schema::effect(f);
  std::string num = fmtSigned(v, std::fabs(v - std::round(v)) > 1e-9 ? 1 : 0);
  if (e.unit[0] == '%') num += "\xC2\xA0%";
  switch (f) {
    case Fx::PopGrowthPct: return num + " прироста населения за ход";
    case Fx::TradePct: return num + " торговой ценности";
    case Fx::TradeFlat: return num + " к торговой ценности";
    case Fx::BuildCostPct: return num + " к стоимости строительства";
    case Fx::ContentmentPerTurn: return num + " довольства за ход";
    case Fx::RebellionPct: return num + " к вероятности восстания";
    case Fx::ResourcePct: return num + " добычи ресурса";
    case Fx::ResourceFlat: return num + " к добыче ресурса";
    case Fx::Slots: return num + " " + plural(i64(std::fabs(v)), "слот", "слота", "слотов") + " построек";
    case Fx::IncomePct: return num + " дохода в казну";
    case Fx::DiplomacyPerTurn: return num + " к отношениям за ход";
    case Fx::ArmyUpkeepPct: return num + " содержания войск";
    case Fx::FleetUpkeepPct: return num + " содержания флота";
    default: return num;
  }
}

bool effectGood(Fx f, double v) {
  bool bad = f == Fx::BuildCostPct || f == Fx::RebellionPct || f == Fx::ArmyUpkeepPct || f == Fx::FleetUpkeepPct;
  return bad ? v < 0 : v > 0;
}

void effectChips(const Modifier& m) {
  ui::IdScope s(i64(m.id) + 0x40000000LL);
  bool any = false;
  for (int f = 0; f < kFxCount; f++) if (m.has(Fx(f)) && m.fx[size_t(f)] != 0) any = true;
  if (!any) {
    ui::label("Без эффектов", {.ink = ui::Ink::Muted});
    return;
  }
  edkit::chipsBegin();
  for (int f = 0; f < kFxCount; f++) {
    if (!m.has(Fx(f))) continue;
    double v = m.fx[size_t(f)];
    if (v == 0) continue;
    ui::IdScope s2(f);
    ui::ChipOpt co;
    co.icon = schema::effect(Fx(f)).icon;
    co.tone = effectGood(Fx(f), v) ? ui::Tone::Success : ui::Tone::Danger;
    edkit::chip(effectText(Fx(f), v), co);
  }
  edkit::chipsEnd();
}

const char* resourceIcon(const World& w, Id res) {
  if (res == kGold) return "coins";
  const CatalogItem* c = w.resource(res);
  if (c && !c->icon.empty() && gfx::hasIcon(c->icon)) return c->icon.c_str();
  return "resource";
}

Color resourceColor(const World& w, Id res) {
  const CatalogItem* c = w.resource(res);
  return c ? c->color : Color::hex(0x9aa0a8);
}

void resourceAmount(Id res, double amount, ui::Ink ink) {
  const World& w = app().world();
  const CatalogItem* c = w.resource(res);
  ui::HStack row(0, ui::Align::Left, ui::sp::xs);
  ui::iconColored(resourceIcon(w, res), resourceColor(w, res), 16, c ? std::string_view(c->name) : std::string_view("Ресурс"));
  ui::label(fmtNum(amount, std::fabs(amount - std::round(amount)) > 1e-9 ? 1 : 0), {.ink = ink});
}

}  // namespace rg::app::w
