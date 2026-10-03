// Regnum — выдвижная панель «Войска»: все войска и флоты на карте (цвет фракции, численность), поиск,
// отбор по виду и фракции; щелчок выделяет объект и показывает его на карте.
#include "app/panels/military.h"

namespace rg::app::mil {

namespace {

struct DrawerState {
  std::string query;
  int kind = 0;     // 0 — все, 1 — войска, 2 — флот
  Id faction = 0;   // 0 — все фракции
};

void drawDrawer(App& a) {
  const World& w = frameWorld(a);
  auto& st = ui::state<DrawerState>(ui::id("military-drawer"));
  if (st.faction && !w.faction(st.faction)) st.faction = 0;
  {
    ui::Disabled dis(a.readOnly());
    ui::Row r({ui::fr(1), ui::fr(1)}, 30, 8);
    if (ui::button("Войско", {.icon = "tool-army", .fill = true, .tooltip = "Поставить новое войско на карту (A)"}))
      startPlacing(a, ArmyKind::Army, st.faction ? st.faction : lastPlaceFaction());
    a.markUi("mil.drawer.newArmy");
    if (ui::button("Флот", {.icon = "tool-fleet", .fill = true, .tooltip = "Поставить новый флот на карту (Shift+A)"}))
      startPlacing(a, ArmyKind::Fleet, st.faction ? st.faction : lastPlaceFaction());
    a.markUi("mil.drawer.newFleet");
  }
  ui::searchField("q", st.query, "Название, фракция, провинция");
  a.markUi("mil.drawer.search");
  ui::segmented("kind", st.kind, {{nullptr, "Все", "Войска и флоты"}, {"army", "Войска", "Только войска"}, {"fleet", "Флот", "Только флоты"}},
                {.size = ui::Size::Small});
  a.markUi("mil.drawer.kind");
  w::factionPicker("faction", st.faction, w::FactionFilter::Any, "Все фракции");
  a.markUi("mil.drawer.faction");

  struct Item {
    const Army* army;
    std::string sub;
    std::string key;
  };
  std::vector<Item> items;
  i64 armies = 0, fleets = 0, units = 0;
  w.armies.each([&](const Army& ar) {
    if (st.kind == 1 && ar.isFleet()) return;
    if (st.kind == 2 && !ar.isFleet()) return;
    std::vector<Id> fs = factionsIn(ar);
    if (st.faction && std::find(fs.begin(), fs.end(), st.faction) == fs.end()) return;
    std::vector<std::string> names;
    for (Id f : fs) names.push_back(w.factionName(f));
    std::string prov = w.provinceName(provinceUnder(w, ar.pos));
    std::string sub = join(names, " + ") + " · " + prov;
    if (!st.query.empty() && !utf8::matches(ar.name + " " + sub, st.query)) return;
    (ar.isFleet() ? fleets : armies)++;
    units += unitCount(ar);
    items.push_back(Item{&ar, sub, w.factionName(ar.leader())});
  });
  std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) {
    if (int c = compareRu(x.key, y.key); c != 0) return c < 0;
    if (x.army->kind != y.army->kind) return x.army->kind < y.army->kind;
    return compareRu(x.army->name, y.army->name) < 0;
  });
  {
    ui::HStack hs(20, ui::Align::Left, 6);
    ui::caption(std::to_string(armies) + " " + plural(armies, "войско", "войска", "войск") + " · " + std::to_string(fleets) + " " +
                plural(fleets, "флот", "флота", "флотов"));
    ui::flex();
    ui::label(fmtShort(double(units)), {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "users", .tooltip = "Численность в найденных объектах"});
  }
  if (items.empty()) {
    if (w.armies.empty()) {
      ui::Disabled dis(a.readOnly());
      if (ui::emptyState("army", "Войск и флотов на карте пока нет.", "Новое войско", "tool-army"))
        startPlacing(a, ArmyKind::Army, st.faction ? st.faction : lastPlaceFaction());
    } else {
      if (ui::emptyState("search", "Ничего не найдено.", "Сбросить отбор", "close")) st = DrawerState{};
    }
    return;
  }
  for (const Item& it : items) {
    if (objectItem(w, *it.army, a.ui.sel == Selection{SelType::Army, it.army->id}, it.sub)) a.select(SelType::Army, it.army->id, true);
    a.markUi("mil.drawer.item." + std::to_string(it.army->id));
  }
}

DrawerReg drawer({"military", "army", "Войска и флот", 50, drawDrawer, "Ctrl+5"});

}  // namespace
}  // namespace rg::app::mil
