// Regnum — вкладка «Гильдии» государства (ТЗ 1.d.ii, 1.d.vi): торговые гильдии, расположенные в государстве
// (государство расположения — это государство), в том числе государственные; учреждение государственной гильдии.
#include "app/panels/faction_common.h"

namespace rg::app {
namespace {

using namespace fac;

void drawGuilds(App& a, Id id) {
  const World& w = frameWorld(a);
  const Faction* f = w.faction(id);
  if (!f) return;
  const bool ro = a.readOnly();
  auto calc = rules::calc(w);
  std::vector<const Faction*> list;
  w.factions.each([&](const Faction& g) {
    if (g.isGuild() && g.homeState == id) list.push_back(&g);
  });
  std::sort(list.begin(), list.end(), [](const Faction* x, const Faction* y) {
    if (x->stateGuild != y->stateGuild) return x->stateGuild;
    return compareRu(x->name, y->name) < 0;
  });
  int stateGuilds = 0;
  double hqIncome = 0;
  for (const Faction* g : list) {
    if (g->stateGuild) stateGuilds++;
    if (const rules::FactionCalc* gc = calc->faction(g->id)) hqIncome += gc->incGuilds;
  }
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 10);
    ui::stat(fmtInt(i64(list.size())), plural(i64(list.size()), "гильдия", "гильдии", "гильдий"),
             {.icon = "guild", .tone = ui::Tone::Info, .tooltip = "Государственных: " + std::to_string(stateGuilds)});
    ui::stat(fmtNum(hqIncome), "Доход штабов", {.icon = "income", .tone = ui::Tone::Success, .tooltip = "Сумма чистого дохода штабов этих гильдий"});
  }
  ui::spacer(2);
  if (ui::Section s("Гильдии государства", "guild", {.badge = list.empty() ? std::string() : std::to_string(list.size())}); s) {
    if (list.empty()) ui::emptyState("guild", "В государстве пока нет гильдий.");
    for (const Faction* g : list) {
      ui::IdScope sc{i64(g->id)};
      const rules::FactionCalc* gc = calc->faction(g->id);
      std::string sub = g->stateGuild ? std::string("Государственная") : std::string("Торговая");
      if (gc) sub += " · " + std::to_string(gc->provinces.size()) + " " + plural(i64(gc->provinces.size()), "штаб", "штаба", "штабов");
      RowEvents ev = factionRow(*g, sub, fmtShort(g->treasury()), "coins", false, 50);
      a.markUi("guilds.row." + std::to_string(g->id), ev.rect);
      if (g->stateGuild) {
        const ui::Theme& t = ui::theme();
        RectF lk{ev.rect.x + 12 + 36 - 8, ev.rect.cy() + 3, 13, 13};
        ui::draw::circle(lk.cx(), lk.cy(), 7.5f, t.surface2);
        ui::draw::icon("lock", lk.inset(1), t.accent);
      }
      if (ev.clicked || ev.doubleClicked) a.select(SelType::Faction, g->id);
    }
  }
  if (!ro) {
    ui::spacer(2);
    if (ui::button("Государственная гильдия", {.icon = "plus", .fill = true, .tooltip = "Учредить гильдию, закреплённую за этим государством"}))
      createStateGuildUi(a, id);
    a.markUi("guilds.create");
  }
}

bool stateOnly(App& a, Id id) { return isState(a, id); }

TabReg tab({kTabGuilds, "guild", "Гильдии", 70, SelType::Faction, stateOnly, drawGuilds});

}  // namespace
}  // namespace rg::app
