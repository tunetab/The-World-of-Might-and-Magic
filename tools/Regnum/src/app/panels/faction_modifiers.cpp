// Regnum — вкладка «Модификаторы» (ТЗ 1.b.iii, 1.g): список модификаторов фракции фишками с переносом строк
// (действуют во всех провинциях государства; у гильдии — в провинциях штабов; щелчок — редактор модификаторов,
// крестик — убрать) и сводка действующих эффектов: глобальные (доход, содержание
// войск и флота), дипломатия по целям, локальные во всех провинциях и источники (модификаторы, технологии, постройки).
#include "app/panels/faction_common.h"
#include "gfx/icons.h"

namespace rg::app {
namespace {

using namespace fac;

const char* fxIcon(Fx f) {
  const char* ic = schema::effect(f).icon;
  return ic && gfx::hasIcon(ic) ? ic : "sparkles";
}

// Ряды фишек с переносом по ширине (ряд ui::HStack не переносит).
struct ChipItem {
  std::string label;
  const char* icon = nullptr;
  Color color{0, 0, 0, 0};
  ui::Tone tone = ui::Tone::Neutral;
  std::string tip;
  bool removable = false, clickable = false;
};

// Возвращает индекс фишки и действие (щелчок или крестик); {-1, None} — ничего.
// mark — префикс имён прямоугольников фишек для App::uiRect (тесты); пусто — не запоминать.
std::pair<int, ui::ChipAction> chipFlow(std::string_view id, const std::vector<ChipItem>& items, std::string_view mark = {}) {
  ui::IdScope scope(id);
  std::pair<int, ui::ChipAction> res{-1, ui::ChipAction::None};
  const float W = ui::avail().w, gap = 6;
  size_t i = 0;
  int line = 0;
  while (i < items.size()) {
    ui::IdScope ls(line++);
    ui::HStack hs(26, ui::Align::Left, gap);
    float x = 0;
    bool first = true;
    while (i < items.size()) {
      const ChipItem& c = items[i];
      // Ширина фишки — как в ui::chip: текст + поля, значок или точка, крестик.
      float w = ui::measure(c.label, ui::Font::Small) + 20 + ((c.icon || c.color.a) ? 16 : 0) + (c.removable ? 18 : 0);
      if (!first && x + w > W) break;
      ui::IdScope cs{int(i)};
      ui::ChipAction act = ui::chip(c.label, {.icon = c.icon, .color = c.color, .tone = c.tone, .removable = c.removable, .clickable = c.clickable,
                                              .tooltip = c.tip});
      if (act != ui::ChipAction::None) res = {int(i), act};
      if (!mark.empty()) app().markUi(std::string(mark) + std::to_string(i));
      x += w + gap;
      first = false;
      i++;
    }
  }
  return res;
}

std::string modName(const Modifier& m) { return m.name.empty() ? std::string("Модификатор") : m.name; }

// Модификаторы фракции фишками с переносом (крестик — убрать, щелчок — редактор модификаторов) и выбор для
// добавления. true — список изменился.
bool modifierChips(App& a, const World& w, std::vector<Id>& ids, bool ro) {
  std::vector<ChipItem> chips;
  std::vector<Id> shown;
  for (Id mid : ids) {
    const Modifier* m = w.modifier(mid);
    if (!m) continue;
    ChipItem c;
    c.label = modName(*m);
    c.icon = !m->icon.empty() && gfx::hasIcon(m->icon) ? m->icon.c_str() : "sparkles";
    c.color = m->color;
    c.removable = !ro;
    c.clickable = true;
    for (int f = 0; f < kFxCount; f++)
      if (m->has(Fx(f))) c.tip += (c.tip.empty() ? "" : "\n") + w::effectText(Fx(f), m->fx[size_t(f)]);
    if (!m->desc.empty()) c.tip = m->desc + (c.tip.empty() ? "" : "\n" + c.tip);
    if (c.tip.empty()) c.tip = "Открыть в редакторе модификаторов";
    chips.push_back(std::move(c));
    shown.push_back(mid);
  }
  bool changed = false;
  if (chips.empty()) {
    if (ro) ui::label("Нет модификаторов.", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
  } else {
    auto [k, act] = chipFlow("chips", chips, "mods.chip.");
    if (k >= 0 && act == ui::ChipAction::Remove) {
      ids.erase(std::remove(ids.begin(), ids.end(), shown[size_t(k)]), ids.end());
      changed = true;
    } else if (k >= 0 && act == ui::ChipAction::Click) {
      a.openEditor("modifiers", shown[size_t(k)]);
    }
  }
  if (ro) return changed;
  std::vector<const Modifier*> free;
  w.modifiers.each([&](const Modifier& m) {
    if (std::find(ids.begin(), ids.end(), m.id) == ids.end()) free.push_back(&m);
  });
  std::sort(free.begin(), free.end(), [](const Modifier* x, const Modifier* y) { return compareRu(modName(*x), modName(*y)) < 0; });
  if (free.empty()) {
    if (w.modifiers.empty() && ui::link("Создать модификатор", "sparkles")) a.openEditor("modifiers", 0);
    return changed;
  }
  std::vector<std::string> labels;
  labels.reserve(free.size());
  for (const Modifier* m : free) labels.push_back(modName(*m));
  std::vector<ui::Option> opts(free.size());
  for (size_t i = 0; i < free.size(); i++) {
    opts[i].label = labels[i];
    opts[i].icon = !free[i]->icon.empty() && gfx::hasIcon(free[i]->icon) ? free[i]->icon.c_str() : "sparkles";
    opts[i].color = free[i]->color;
  }
  int idx = -1;
  if (ui::combo("add", idx, std::span<const ui::Option>(opts), {.placeholder = "Добавить модификатор", .icon = "plus"}) && idx >= 0 &&
      idx < int(free.size())) {
    ids.push_back(free[size_t(idx)]->id);
    changed = true;
  }
  return changed;
}

std::string sourceName(const World& w, const rules::EffectSource& s, Id self) {
  switch (s.kind) {
    case rules::EffectSource::Province: return "провинция " + w.provinceName(s.id);
    case rules::EffectSource::Faction: return s.id == self ? std::string("фракция") : w.factionName(s.id);
    case rules::EffectSource::Tech: {
      const Tech* t = w.tech(s.id);
      return "технология «" + (t ? t->name : std::string("?")) + "»";
    }
    case rules::EffectSource::Building: {
      const Building* b = w.building(s.id);
      return "постройка «" + (b ? b->name : std::string("?")) + "»";
    }
    case rules::EffectSource::Guild: return "гильдия " + w.factionName(s.id);
  }
  return {};
}

const char* sourceIcon(rules::EffectSource::Kind k) {
  switch (k) {
    case rules::EffectSource::Province: return "province";
    case rules::EffectSource::Faction: return "crown";
    case rules::EffectSource::Tech: return "tech";
    case rules::EffectSource::Building: return "building";
    case rules::EffectSource::Guild: return "guild";
  }
  return "sparkles";
}

void drawModifiers(App& a, Id id) {
  const World& w = frameWorld(a);
  const Faction* f = w.faction(id);
  if (!f) return;
  const bool ro = a.readOnly();
  const bool state = f->isState();

  if (ui::Section s("Модификаторы", "sparkles", {.badge = f->modifiers.empty() ? std::string() : std::to_string(f->modifiers.size())}); s) {
    ui::label(state ? "Действуют во всех провинциях государства" : "Локальные эффекты действуют в провинциях штабов",
              {.font = ui::Font::Small, .ink = ui::Ink::Muted, .wrap = true});
    std::vector<Id> ids = f->modifiers;
    if (modifierChips(a, w, ids, ro)) a.act("Модификаторы фракции", [&](Tx& tx) { tx.faction(id).modifiers = ids; });
    a.markUi("mods.add");
  }

  rules::Effects fx = rules::factionEffects(w, id);
  if (ui::Section s("Действующие эффекты", "chart-bar"); s) {
    bool any = false;
    // Глобальные эффекты фракции.
    std::vector<ChipItem> global;
    for (Fx g : {Fx::IncomePct, Fx::ArmyUpkeepPct, Fx::FleetUpkeepPct}) {
      double v = fx[g];
      if (std::fabs(v) < 1e-9) continue;
      global.push_back({w::effectText(g, v), fxIcon(g), Color(0, 0, 0, 0), w::effectGood(g, v) ? ui::Tone::Success : ui::Tone::Danger, schema::effect(g).name});
    }
    if (!global.empty()) {
      ui::caption("Государство");
      chipFlow("global", global);
      any = true;
    }
    // Дипломатия: цели и изменение отношений за ход.
    std::vector<ChipItem> dip;
    for (auto& [target, v] : fx.diplomacy) {
      if (std::fabs(v) < 1e-9 || !w.faction(target)) continue;
      dip.push_back({w.factionName(target) + " · " + fmtSigned(v, std::fabs(v - std::round(v)) > 1e-9 ? 1 : 0) + " за ход", nullptr,
                     w::factionColor(w, target), v > 0 ? ui::Tone::Success : ui::Tone::Danger, "Изменение отношений каждый ход"});
    }
    if (!dip.empty()) {
      ui::caption("Дипломатия");
      chipFlow("dip", dip);
      any = true;
    }
    // Локальные эффекты (ТЗ 1.b.iii, 1.g, 1.h): модификаторы фракции и её изученных технологий действуют во всех её
    // провинциях (у гильдии — в провинциях штабов), модификаторы построек — в провинциях, где постройка стоит.
    std::vector<rules::EffectSource> localSrc;
    std::map<std::pair<Id, Id>, int> buildingProvinces;   // (постройка, модификатор) -> число провинций
    auto hasLocal = [&](const Modifier& m) {
      for (int k = 0; k < kFxCount; k++)
        if (schema::kEffects[k].local && m.has(Fx(k)) && std::isfinite(m.fx[size_t(k)])) return true;
      return false;
    };
    std::array<double, kFxCount> local{};
    auto addLocal = [&](rules::EffectSource::Kind kind, Id src, Id mid) {
      const Modifier* m = w.modifier(mid);
      if (!m || !hasLocal(*m)) return;
      for (int k = 0; k < kFxCount; k++)
        if (schema::kEffects[k].local && m->has(Fx(k)) && std::isfinite(m->fx[size_t(k)])) local[size_t(k)] += m->fx[size_t(k)];
      localSrc.push_back({kind, src, mid});
    };
    for (Id mid : f->modifiers) addLocal(rules::EffectSource::Faction, id, mid);
    w.techs.each([&](const Tech& t) {
      if (t.faction != id || !t.studied) return;
      for (Id mid : t.modifiers) addLocal(rules::EffectSource::Tech, t.id, mid);
    });
    // Постройки: эффекты одного экземпляра (не сумма по провинциям) и число провинций, где они действуют.
    std::map<std::pair<Id, Id>, std::array<double, kFxCount>> perBuilding;
    if (state)
      w.provinces.each([&](const Province& p) {
        if (p.sea || p.owner != id) return;
        for (const ProvBuilding& pb : p.buildings) {
          const Building* b = w.building(pb.building);
          int lvl = pb.builtLevel();
          if (!b || lvl < 1 || lvl > int(b->levels.size())) continue;
          for (Id mid : b->levels[size_t(lvl - 1)].modifiers) {
            const Modifier* m = w.modifier(mid);
            if (!m || !hasLocal(*m)) continue;
            int& n = buildingProvinces[{b->id, mid}];
            if (n++ == 0) {
              localSrc.push_back({rules::EffectSource::Building, b->id, mid});
              auto& arr = perBuilding[{b->id, mid}];
              for (int k = 0; k < kFxCount; k++)
                if (schema::kEffects[k].local && m->has(Fx(k)) && std::isfinite(m->fx[size_t(k)])) arr[size_t(k)] = m->fx[size_t(k)];
            }
          }
        }
      });
    std::vector<ChipItem> loc;
    for (int k = 0; k < kFxCount; k++) {
      double v = local[size_t(k)];
      if (std::fabs(v) < 1e-9) continue;
      loc.push_back({w::effectText(Fx(k), v), fxIcon(Fx(k)), Color(0, 0, 0, 0), w::effectGood(Fx(k), v) ? ui::Tone::Success : ui::Tone::Danger, schema::kEffects[k].name});
    }
    if (!loc.empty()) {
      ui::caption(state ? "В каждой провинции" : "В провинциях штабов");
      chipFlow("local", loc);
      any = true;
    }
    std::vector<ChipItem> bld;
    for (auto& [key, arr] : perBuilding) {
      const Building* b = w.building(key.first);
      int n = buildingProvinces[key];
      for (int k = 0; k < kFxCount; k++) {
        double v = arr[size_t(k)];
        if (std::fabs(v) < 1e-9) continue;
        bld.push_back({w::effectText(Fx(k), v) + " · " + (b ? b->name : std::string("постройка")), fxIcon(Fx(k)), Color(0, 0, 0, 0),
                       w::effectGood(Fx(k), v) ? ui::Tone::Success : ui::Tone::Danger,
                       std::string(schema::kEffects[k].name) + " — в провинциях с постройкой: " + std::to_string(n)});
      }
    }
    if (!bld.empty()) {
      ui::caption("В провинциях с постройками");
      chipFlow("buildings", bld);
      any = true;
    }
    if (!any) ui::label("Действующих эффектов нет", {.ink = ui::Ink::Muted});
    // Источники: глобальные (расчёт правил) и локальные (собраны выше).
    std::vector<rules::EffectSource> sources = fx.sources;
    sources.insert(sources.end(), localSrc.begin(), localSrc.end());
    if (!sources.empty()) {
      ui::caption("Источники");
      std::vector<std::pair<Id, Id>> seen;   // (модификатор, источник) без повторов
      int k = 0;
      for (const rules::EffectSource& src : sources) {
        std::pair<Id, Id> key{src.modifier, src.id};
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        const Modifier* m = w.modifier(src.modifier);
        ui::IdScope sc(k++);
        ui::Row r({ui::px(18), ui::fr(1)}, 24, 8);
        ui::icon(sourceIcon(src.kind), ui::Ink::Muted, 15);
        std::string text = (m ? (m->name.empty() ? std::string("Модификатор") : m->name) : std::string("Модификатор")) + " — " + sourceName(w, src, id);
        if (src.kind == rules::EffectSource::Building)
          if (auto it = buildingProvinces.find({src.id, src.modifier}); it != buildingProvinces.end())
            text += " · провинций: " + std::to_string(it->second);
        ui::label(text, {.font = ui::Font::Small, .ink = ui::Ink::Dim});
        a.markUi("mods.source." + std::to_string(int(src.kind)) + "." + std::to_string(src.id));
      }
    }
  }
}

TabReg tab({kTabModifiers, "sparkles", "Модификаторы", 65, SelType::Faction, nullptr, drawModifiers});

}  // namespace
}  // namespace rg::app
