// Regnum — входы в дерево построек: панель ленты «Дерево построек» (общее дерево, ТЗ 1.h.i), вкладка государства
// «Постройки» (уникальные постройки и стройки в провинциях), команда палитры.
#include "app/editors/buildings.h"
#include "app/editors/techtree.h"
#include "app/widgets.h"

namespace rg::app {

namespace {

struct Use {
  int built = 0, building = 0;
};
std::map<Id, Use> usage(const World& w, Id owner /*0 — все провинции*/) {
  std::map<Id, Use> u;
  w.provinces.each([&](const Province& p) {
    if (owner && p.owner != owner) return;
    for (const ProvBuilding& pb : p.buildings) {
      if (pb.builtLevel() > 0) u[pb.building].built++;
      else u[pb.building].building++;
    }
  });
  return u;
}

std::vector<const Building*> treeOf(const World& w, Id owner) {
  std::vector<const Building*> out;
  w.buildings.each([&](const Building& b) {
    if (b.owner == owner) out.push_back(&b);
  });
  std::stable_sort(out.begin(), out.end(), [](const Building* x, const Building* y) {
    if (x->cat != y->cat) return x->cat < y->cat;
    return compareRu(x->name, y->name) < 0;
  });
  return out;
}

std::string levelsText(const Building& b) {
  int n = int(b.levels.size());
  std::string s = std::to_string(n) + " " + plural(n, "уровень", "уровня", "уровней");
  if (n > 0) s += " · " + nTurns(bld::levelTurns(b, 1));
  return s;
}

Id createIn(App& a, Id owner, BuildingCat cat) {
  Id nid = 0;
  a.act("Новая постройка", [&](Tx& tx) {
    nid = rules::createBuilding(tx, owner, "Новая постройка");
    Building& b = tx.building(nid);
    b.cat = cat;
    b.icon = bld::catIcon(cat);
  });
  return nid;
}

// ---------------------------------------------------------------- панель ленты: общее дерево
void drawDrawer(App& a) {
  const World& w = a.world();
  const bool ro = a.readOnly();
  std::vector<const Building*> list = treeOf(w, 0);
  auto use = usage(w, 0);
  ui::text("Общее дерево одинаково для всех государств: любая их провинция может строить эти постройки.", ui::Font::Small, ui::Ink::Dim);
  if (ui::button("Открыть дерево построек", {.variant = ui::Variant::Primary, .icon = "building", .fill = true})) openBuildingTree(a, 0);
  a.markUi("drawer.buildings.open");
  for (int c = 0; c < int(BuildingCat::Count); c++) {
    BuildingCat cat = BuildingCat(c);
    int n = 0;
    for (const Building* b : list) n += b->cat == cat;
    ui::IdScope sc(c);
    ui::Section s(bld::catName(cat), bld::catIcon(cat), {.badge = std::to_string(n), .actionIcon = ro ? nullptr : "plus", .actionTooltip = "Новая постройка"});
    if (s.action() && !ro)
      if (Id nid = createIn(a, 0, cat)) openBuildingTree(a, 0, nid);
    if (!s) continue;
    if (n == 0) ui::label("Нет построек", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
    for (const Building* b : list) {
      if (b->cat != cat) continue;
      ui::IdScope bs{i64(b->id)};
      const Use& u = use[b->id];
      std::string hint = u.built + u.building > 0 ? std::to_string(u.built + u.building) : std::string();
      if (ui::listItem(b->name.empty() ? "Без названия" : b->name, {.icon = bld::iconOf(*b), .subtitle = levelsText(*b), .hint = hint,
                                                                    .tooltip = hint.empty() ? std::string_view() : std::string_view("Провинций с постройкой")}))
        openBuildingTree(a, 0, b->id);
      a.markUi("drawer.buildings.item." + std::to_string(b->id));
    }
  }
}

// ---------------------------------------------------------------- вкладка государства
void drawFaction(App& a, Id fid) {
  const World& w = a.world();
  const Faction* f = w.faction(fid);
  if (!f || !f->isState()) return;
  const bool ro = a.readOnly();
  std::vector<const Building*> uniq = treeOf(w, fid);
  auto use = usage(w, fid);
  int built = 0, building = 0;
  for (auto& [id, u] : use) {
    built += u.built;
    building += u.building;
  }
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 8);
    ui::stat(std::to_string(built), "Построено", {.icon = "building", .tone = ui::Tone::Success, .tooltip = "Построек в провинциях государства"});
    ui::stat(std::to_string(building), "Строится", {.icon = "hourglass", .tone = ui::Tone::Info});
  }
  {
    ui::Row r({ui::fr(1.2f), ui::fr(1)}, 30, 8);
    if (ui::button("Уникальные постройки##open", {.variant = ui::Variant::Primary, .icon = "crown", .fill = true})) openBuildingTree(a, fid);
    a.markUi("faction.buildings.unique");
    if (ui::button("Общее дерево", {.icon = "globe", .fill = true})) openBuildingTree(a, 0);
    a.markUi("faction.buildings.common");
  }
  {
    ui::Section s("Уникальные постройки", "crown", {.badge = std::to_string(uniq.size()), .actionIcon = ro ? nullptr : "plus", .actionTooltip = "Новая уникальная постройка"});
    if (s.action() && !ro)
      if (Id nid = createIn(a, fid, BuildingCat::Economic)) openBuildingTree(a, fid, nid);
    if (s) {
      if (uniq.empty()) {
        if (ui::emptyState("building", "Уникальных построек нет.", ro ? "" : "Новая постройка", "plus"))
          if (Id nid = createIn(a, fid, BuildingCat::Economic)) openBuildingTree(a, fid, nid);
        if (!ro) a.markUi("faction.buildings.new", tree::emptyActionRect("Новая постройка", "plus"));
      }
      for (const Building* b : uniq) {
        ui::IdScope bs{i64(b->id)};
        const Use& u = use[b->id];
        std::string sub = std::string(bld::catName(b->cat)) + " · " + levelsText(*b);
        if (ui::listItem(b->name.empty() ? "Без названия" : b->name,
                         {.icon = bld::iconOf(*b), .subtitle = sub, .hint = u.built + u.building ? std::to_string(u.built + u.building) : std::string()}))
          openBuildingTree(a, fid, b->id);
      }
    }
  }
  // Постройки в провинциях государства (общего дерева и уникальные): сколько и каких уровней.
  if (built > 0) {
    struct Agg {
      int count = 0;
      std::map<int, int> levels;   // действующий уровень -> провинций
    };
    std::map<Id, Agg> agg;
    w.provinces.each([&](const Province& p) {
      if (p.owner != fid) return;
      for (const ProvBuilding& pb : p.buildings) {
        int lv = pb.builtLevel();
        if (lv <= 0) continue;
        Agg& g = agg[pb.building];
        g.count++;
        g.levels[lv]++;
      }
    });
    std::vector<const Building*> list;
    for (auto& [id, g] : agg)
      if (const Building* b = w.building(id)) list.push_back(b);
    std::stable_sort(list.begin(), list.end(), [](const Building* x, const Building* y) {
      if (x->cat != y->cat) return x->cat < y->cat;
      return compareRu(x->name, y->name) < 0;
    });
    if (ui::Section s("В провинциях", "province", {.badge = std::to_string(built)}); s) {
      ui::IdScope ps("built");
      for (const Building* b : list) {
        const Agg& g = agg[b->id];
        ui::IdScope bs{i64(b->id)};
        std::vector<std::string> lv;
        for (auto& [l, n] : g.levels) lv.push_back(tree::roman(l) + (n > 1 ? " ×" + std::to_string(n) : std::string()));
        std::string sub = std::string(bld::catName(b->cat)) + " · ур. " + join(lv, ", ");
        if (ui::listItem(b->name.empty() ? "Без названия" : b->name,
                         {.icon = bld::iconOf(*b), .subtitle = sub, .hint = std::to_string(g.count), .tooltip = "Провинций с постройкой — открыть в дереве"}))
          openBuildingTree(a, b->owner, b->id);
        a.markUi("faction.buildings.built." + std::to_string(b->id));
      }
    }
  }
  // Стройки в провинциях государства
  if (building > 0) {
    if (ui::Section s("Стройки", "hourglass", {.badge = std::to_string(building)}); s) {
      w.provinces.each([&](const Province& p) {
        if (p.owner != fid) return;
        for (const ProvBuilding& pb : p.buildings) {
          if (!pb.constructing) continue;
          const Building* b = w.building(pb.building);
          if (!b) continue;
          ui::IdScope ps{i64(p.id) * 1000003 + i64(pb.building)};
          int total = bld::levelTurns(*b, pb.level);
          ui::Row row({ui::px(32), ui::fr(1)}, ui::kAuto, 10);
          bld::iconTile(*b, 32, true);
          ui::Group g(0, 3);
          {
            ui::HStack hs(24, ui::Align::Left, 6);
            ui::label(b->name + (pb.level > 1 ? " " + tree::roman(pb.level) : std::string()), {.font = ui::Font::Strong});
            w::provinceChip(p.id);
          }
          ui::progress(double(total - pb.left) / double(std::max(1, total)), {.tone = ui::Tone::Info, .height = 4, .text = "ещё " + nTurns(pb.left)});
        }
      });
    }
  }
  // Общее дерево (ТЗ 1.h.i: дерево государства = общее дерево + уникальные): здесь только просмотр, правка — в общем
  // дереве построек.
  {
    std::vector<const Building*> common = treeOf(w, 0);
    if (ui::Section s("Постройки общего дерева", "globe", {.badge = std::to_string(common.size())}); s) {
      ui::IdScope cs("common");
      if (common.empty()) ui::label("В общем дереве пока нет построек", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
      for (const Building* b : common) {
        ui::IdScope bs{i64(b->id)};
        const Use& u = use[b->id];
        std::string sub = std::string(bld::catName(b->cat)) + " · " + levelsText(*b);
        if (ui::listItem(b->name.empty() ? "Без названия" : b->name,
                         {.icon = bld::iconOf(*b), .subtitle = sub, .hint = u.built + u.building ? std::to_string(u.built + u.building) : std::string(),
                          .tooltip = "Постройка общего дерева — открыть в общем дереве"}))
          openBuildingTree(a, 0, b->id);
        a.markUi("faction.buildings.commonItem." + std::to_string(b->id));
      }
      if (ui::link("Изменить общее дерево", "globe")) openBuildingTree(a, 0);
    }
  }
}

bool isState(App& a, Id fid) {
  const Faction* f = a.world().faction(fid);
  return f && f->isState();
}

DrawerReg regDrawer({"buildings", "building", "Дерево построек", 60, drawDrawer});
TabReg regTab({"faction.buildings", "building", "Постройки", 62, SelType::Faction, isState, drawFaction});
CommandReg cmdBuildings({"trees.buildings", "Дерево построек", "building", nullptr, [](App& a) { openBuildingTree(a, 0); },
                         [](App& a) { return a.ui.screen == Screen::Editor; }, false, "Вид"});

}  // namespace
}  // namespace rg::app
