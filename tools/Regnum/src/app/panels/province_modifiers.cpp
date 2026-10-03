// Regnum — вкладка «Модификаторы» инспектора провинции (ТЗ 1.g, 1.b.iii): список модификаторов провинции и сводка
// всех действующих локальных эффектов с источниками — провинция, государство, технологии, постройки, гильдии.
#include "app/widgets.h"

namespace rg::app::prov {
// province_common.cpp
struct ChipSpec {
  std::string label;
  const char* icon = nullptr;
  Color color{0, 0, 0, 0};
  ui::Tone tone = ui::Tone::Neutral;
  std::string tooltip;
  bool clickable = false;
};
int flowChips(std::string_view key, const std::vector<ChipSpec>& chips);
const char* sourceIcon(rules::EffectSource::Kind k);
const char* sourceKind(rules::EffectSource::Kind k);
std::string sourceName(const World& wd, const rules::EffectSource& s);
}  // namespace rg::app::prov

namespace rg::app {
namespace {

bool landOnly(App& a, Id pid) {
  const Province* p = a.world().province(pid);
  return p && !p->sea;
}

int modBadge(App& a, Id pid) {
  const Province* p = a.world().province(pid);
  return p && !p->sea ? int(p->modifiers.size()) : 0;
}

// Локальные эффекты модификатора — фишки (польза — зелёная, вред — красная).
std::vector<prov::ChipSpec> localChips(const Modifier& m) {
  std::vector<prov::ChipSpec> out;
  for (int f = 0; f < kFxCount; f++) {
    if (!m.has(Fx(f)) || !schema::kEffects[f].local) continue;
    double v = m.fx[size_t(f)];
    if (v == 0 || !std::isfinite(v)) continue;
    prov::ChipSpec c;
    c.label = w::effectText(Fx(f), v);
    c.icon = schema::kEffects[f].icon;
    c.tone = w::effectGood(Fx(f), v) ? ui::Tone::Success : ui::Tone::Danger;
    c.tooltip = schema::kEffects[f].name;
    out.push_back(std::move(c));
  }
  return out;
}

// Карточка источника: значок вида, название (щелчок — открыть), модификатор и его эффекты.
void sourceCard(App& a, const rules::EffectSource& s, int idx) {
  const World& wd = a.world();
  const Modifier* m = wd.modifier(s.modifier);
  if (!m) return;
  ui::IdScope sc{idx};
  ui::Card card({.pad = 10});
  {
    ui::Row r({ui::px(22), ui::fr(1), ui::fr(0.8f)}, 24, 6);
    ui::icon(prov::sourceIcon(s.kind), ui::Ink::Dim, 16, prov::sourceKind(s.kind));
    std::string name = prov::sourceName(wd, s);
    bool canOpen = s.kind == rules::EffectSource::Faction || s.kind == rules::EffectSource::Guild;
    if (canOpen) {
      if (ui::link(name)) a.select(SelType::Faction, s.id);
    } else {
      ui::label(name, {.font = ui::Font::Strong});
    }
    ui::label(prov::sourceKind(s.kind), {.font = ui::Font::Small, .ink = ui::Ink::Muted, .align = ui::Align::Right});
  }
  {
    std::vector<prov::ChipSpec> chips;
    prov::ChipSpec head;
    head.label = m->name.empty() ? std::string("Модификатор") : m->name;
    head.icon = m->icon.empty() ? "sparkles" : m->icon.c_str();
    head.clickable = true;
    head.tooltip = m->desc.empty() ? std::string("Открыть в редакторе модификаторов") : m->desc;
    chips.push_back(head);
    for (auto& c : localChips(*m)) chips.push_back(std::move(c));
    if (prov::flowChips("chips", chips) == 0) a.openEditor("modifiers", m->id);
  }
}

void drawModifiers(App& a, Id pid) {
  const World& wd = a.world();
  const Province* p = wd.province(pid);
  if (!p) return;
  ui::IdScope ps{i64(pid)};
  bool ro = a.readOnly();
  auto calc = rules::calc(wd);
  rules::ProvinceCalc none;
  const rules::ProvinceCalc* pcp = calc->province(pid);
  const rules::ProvinceCalc& pc = pcp ? *pcp : none;

  {
    std::string badge = p->modifiers.empty() ? std::string() : fmtNum(double(p->modifiers.size()));
    if (ui::Section s("Модификаторы провинции", "sparkles", {.badge = badge}); s) {
      std::vector<Id> ids = p->modifiers;
      if (ids.empty() && ro) ui::label("Нет модификаторов.", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
      if (w::modifierList("mods", ids, ro, w::ModScope::Local)) a.act("Модификаторы провинции", [&](Tx& tx) { tx.province(pid).modifiers = ids; });
      a.markUi("province.mods");
    }
  }

  // Итог всех локальных эффектов (провинция + государство + технологии + постройки + гильдии).
  if (ui::Section s("Действующие эффекты", "bolt"); s) {
    std::vector<prov::ChipSpec> chips;
    for (int f = 0; f < kFxCount; f++) {
      if (!schema::kEffects[f].local) continue;
      double v = pc.fx.v[size_t(f)];
      if (std::fabs(v) < 1e-9) continue;
      prov::ChipSpec c;
      c.label = w::effectText(Fx(f), v);
      c.icon = schema::kEffects[f].icon;
      c.tone = w::effectGood(Fx(f), v) ? ui::Tone::Success : ui::Tone::Danger;
      c.tooltip = schema::kEffects[f].name;
      chips.push_back(std::move(c));
    }
    if (chips.empty()) ui::label("Эффектов нет.", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
    else prov::flowChips("total", chips);
    a.markUi("province.effects");
  }

  if (!pc.fx.sources.empty()) {
    std::string badge = fmtNum(double(pc.fx.sources.size()));
    if (ui::Section s("Источники", "layers", {.badge = badge}); s) {
      // По видам источников: провинция, государство, технологии, постройки, гильдии.
      std::vector<int> order(pc.fx.sources.size());
      for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
      std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return pc.fx.sources[size_t(x)].kind < pc.fx.sources[size_t(y)].kind; });
      for (int i : order) sourceCard(a, pc.fx.sources[size_t(i)], i);
    }
  }
}

TabReg tabModifiers({"province.modifiers", "sparkles", "Модификаторы", 60, SelType::Province, landOnly, drawModifiers, modBadge});

}  // namespace
}  // namespace rg::app
