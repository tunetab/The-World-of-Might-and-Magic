// Regnum — выдвижные панели «Государства» (ТЗ 1.b.ii) и «Гильдии» (ТЗ 1.d.i): списки с флагом, названием,
// правителем или государством расположения и казной; поиск, сортировка, создание (приятный различимый цвет и флаг),
// контекстное меню (открыть, показать на карте, флаг, удалить с подтверждением). Щелчок открывает инспектор.
#include "app/panels/faction_common.h"

namespace rg::app {
namespace {

using namespace fac;

struct ListState {
  std::string q;
  int sort = 0;      // 0 — название, 1 — казна, 2 — провинции/штабы, 3 — население
  Id menu = 0;       // фракция контекстного меню
};

void drawList(App& a, FactionKind kind) {
  const World& w = frameWorld(a);
  const bool ro = a.readOnly();
  const bool states = kind == FactionKind::State;
  const std::string pre = states ? "states" : "guilds";
  auto calc = rules::calc(w);
  ListState& st = ui::state<ListState>(ui::id("##list"));

  // Поиск, сортировка, создание.
  {
    ui::Row r({ui::fr(1), ui::px(30), ui::px(30)}, 30, 6);
    ui::searchField("q", st.q, states ? "Найти государство" : "Найти гильдию");
    a.markUi(pre + ".search");
    if (ui::iconButton("sort", "Сортировка")) ui::openPopup("sort");
    a.markUi(pre + ".sort");
    if (ui::beginMenu("sort")) {
      ui::menuHeader("Сортировка");
      if (ui::menuItem("По названию", {.icon = "sort-asc", .checked = st.sort == 0})) st.sort = 0;
      if (ui::menuItem("По казне", {.icon = "coins", .checked = st.sort == 1})) st.sort = 1;
      if (ui::menuItem(states ? "По числу провинций" : "По числу штабов", {.icon = states ? "province" : "hq", .checked = st.sort == 2})) st.sort = 2;
      if (states && ui::menuItem("По населению", {.icon = "population", .checked = st.sort == 3})) st.sort = 3;
      ui::endMenu();
    }
    if (ui::iconButton("plus", states ? "Новое государство" : "Новая гильдия", {.variant = ui::Variant::Secondary, .disabled = ro}))
      createFactionUi(a, kind);
    a.markUi(pre + ".add");
  }

  std::vector<const Faction*> all, list;
  w.factions.each([&](const Faction& f) {
    if (f.kind == kind) all.push_back(&f);
  });
  for (const Faction* f : all) {
    std::string sub = factionSubtitle(w, *f);
    if (!st.q.empty() && !utf8::matches(f->name + " " + sub, st.q)) continue;
    list.push_back(f);
  }
  auto num = [&](const Faction* f) -> double {
    const rules::FactionCalc* fc = calc->faction(f->id);
    switch (st.sort) {
      case 1: return f->treasury();
      case 2: return fc ? double(fc->provinces.size()) : 0;
      case 3: return fc ? double(fc->population) : 0;
      default: return 0;
    }
  };
  std::stable_sort(list.begin(), list.end(), [&](const Faction* x, const Faction* y) {
    if (st.sort != 0) {
      double u = num(x), v = num(y);
      if (u != v) return u > v;
    }
    return compareRu(displayName(*x), displayName(*y)) < 0;
  });

  ui::spacer(2);
  if (all.empty()) {
    if (ui::emptyState(states ? "crown" : "guild", states ? "Государств пока нет." : "Гильдий пока нет.", ro ? "" : (states ? "Новое государство" : "Новая гильдия"),
                       "plus"))
      createFactionUi(a, kind);
    return;
  }
  {
    std::string cap = st.q.empty() ? std::to_string(all.size()) + " " + (states ? plural(i64(all.size()), "государство", "государства", "государств")
                                                                            : plural(i64(all.size()), "гильдия", "гильдии", "гильдий"))
                                   : "Найдено: " + std::to_string(list.size());
    ui::caption(cap);
  }
  if (list.empty()) {
    ui::emptyState("search", "Ничего не найдено.");
    return;
  }
  ui::gap(2);
  bool openCtx = false;
  for (const Faction* f : list) {
    ui::IdScope sc{i64(f->id)};
    const rules::FactionCalc* fc = calc->faction(f->id);
    std::string hint;
    const char* hintIcon = "coins";
    if (st.sort == 2) {
      hint = std::to_string(fc ? fc->provinces.size() : 0);
      hintIcon = states ? "province" : "hq";
    } else if (st.sort == 3) {
      hint = fmtShort(double(fc ? fc->population : 0));
      hintIcon = "population";
    } else {
      hint = fmtShort(f->treasury());
    }
    bool sel = a.ui.sel == Selection{SelType::Faction, f->id};
    RowEvents ev = factionRow(*f, factionSubtitle(w, *f), hint, hintIcon, sel);
    a.markUi(pre + ".row." + std::to_string(f->id), ev.rect);
    if (f->stateGuild) {
      const ui::Theme& t = ui::theme();
      RectF lk{ev.rect.x + 12 + 36 - 8, ev.rect.cy() + 3, 13, 13};
      ui::draw::circle(lk.cx(), lk.cy(), 7.5f, t.surface1);
      ui::draw::icon("lock", lk.inset(1), t.accent);
    }
    if (ev.doubleClicked) a.select(SelType::Faction, f->id, true);
    else if (ev.clicked) a.select(SelType::Faction, f->id);
    if (ev.rightClicked) {
      st.menu = f->id;
      openCtx = true;
    }
  }
  if (openCtx) ui::openContextMenu("ctx");
  if (ui::beginMenu("ctx")) {
    const Faction* f = w.faction(st.menu);
    if (f) {
      Id fid = f->id;
      ui::menuHeader(displayName(*f));
      if (ui::menuItem("Открыть", {.icon = "info"})) a.select(SelType::Faction, fid);
      if (ui::menuItem("Показать на карте", {.icon = "target"})) a.select(SelType::Faction, fid, true);
      if (ui::menuItem("Изменить флаг…", {.icon = "flag", .disabled = ro})) openFlagEditor(a, fid);
      if (f->isState() && ui::menuItem("Государственная гильдия", {.icon = "guild", .disabled = ro})) createStateGuildUi(a, fid);
      ui::menuSeparator();
      if (ui::menuItem(states ? "Удалить государство" : "Удалить гильдию", {.icon = "trash", .danger = true, .disabled = ro})) confirmDelete(a, fid);
      a.markUi(pre + ".ctx.delete");
    }
    ui::endMenu();
  }
}

void drawStates(App& a) { drawList(a, FactionKind::State); }
void drawGuildList(App& a) { drawList(a, FactionKind::Guild); }

DrawerReg statesDrawer({"states", "crown", "Государства", 20, drawStates, "Ctrl+2"});
DrawerReg guildsDrawer({"guilds", "guild", "Торговые гильдии", 30, drawGuildList, "Ctrl+3"});

}  // namespace
}  // namespace rg::app
