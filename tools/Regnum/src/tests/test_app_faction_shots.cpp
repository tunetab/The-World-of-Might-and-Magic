// Снимки панелей государств и гильдий на демонстрационном мире: шапка и каждая вкладка инспектора, списки
// «Государства» и «Гильдии», редактор флага (тёмная тема) и светлая тема.
#include "tests/test_app_faction_util.h"

using namespace rg;
using namespace rg::apptest;
using namespace rg::factest;

TEST(app_faction_shots_state) {
  HideTestRegs regs;
  Harness h("faction_shots_state");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id st = findFaction(h->world(), "Королевство Альмарин");
  CHECK(st != 0);
  const char* tabs[] = {"faction.overview", "faction.council", "faction.provinces", "faction.economy", "faction.diplomacy",
                        "faction.heroes", "faction.modifiers", "faction.guilds"};
  for (const char* t : tabs) {
    openTab(h, st, t);
    CHECK_EQ(h->ui.tabOf[app::SelType::Faction], std::string(t));
    CHECK(h->uiRect("faction.flag") != nullptr);
    h.waitMap();
    std::string name = std::string("faction_state_") + (t + 8);
    CHECK(h.shot(name));
    if (std::string_view(t) == "faction.economy" || std::string_view(t) == "faction.provinces" || std::string_view(t) == "faction.overview" ||
        std::string_view(t) == "faction.diplomacy") {
      scrollInspector(h, -12);
      CHECK(h.shot(name + "_bottom"));
    }
  }
}

TEST(app_faction_shots_guild) {
  HideTestRegs regs;
  Harness h("faction_shots_guild");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id g = findFaction(h->world(), "Альмарская торговая компания");
  CHECK(g != 0);
  for (const char* t : {"faction.overview", "faction.council", "faction.hqs", "faction.economy", "faction.diplomacy", "faction.heroes",
                        "faction.modifiers"}) {
    openTab(h, g, t);
    CHECK_EQ(h->ui.tabOf[app::SelType::Faction], std::string(t));
    h.waitMap();
    CHECK(h.shot(std::string("faction_guild_") + (t + 8)));
  }
  // Вкладки только для государств у гильдии не показываются, и наоборот.
  for (const app::TabDef& t : app::tabs()) {
    if (t.type != app::SelType::Faction || !t.visible) continue;
    if (std::string_view(t.id) == "faction.provinces" || std::string_view(t.id) == "faction.guilds") CHECK(!t.visible(h.a(), g));
    if (std::string_view(t.id) == "faction.hqs") CHECK(t.visible(h.a(), g));
  }
  Id st = findFaction(h->world(), "Королевство Альмарин");
  for (const app::TabDef& t : app::tabs())
    if (t.type == app::SelType::Faction && t.visible && std::string_view(t.id) == "faction.hqs") CHECK(!t.visible(h.a(), st));
}

TEST(app_faction_shots_drawers) {
  HideTestRegs regs;
  Harness h("faction_shots_drawers");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id st = findFaction(h->world(), "Королевство Альмарин");
  h->openDrawer("states");
  h->select(app::SelType::Faction, st);
  h.settle();
  h.waitMap();
  CHECK(h->uiRect("states.add") != nullptr);
  CHECK(h.shot("faction_drawer_states"));
  h->openDrawer("guilds");
  h->clearSelection();
  h.settle();
  h.waitMap();
  CHECK(h->uiRect("guilds.add") != nullptr);
  CHECK(h.shot("faction_drawer_guilds"));
}

TEST(app_faction_shots_flag_editor) {
  HideTestRegs regs;
  Harness h("faction_shots_flag");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id st = findFaction(h->world(), "Империя Валь-Кетра");
  openTab(h, st, "faction.overview");
  CHECK(h.clickUi("faction.flag"));
  h.settle();
  CHECK(h->hasDialog("flag"));
  h.settle();
  CHECK(h.shot("faction_flag_editor"));
}

TEST(app_faction_shots_light) {
  HideTestRegs regs;
  Harness h("faction_shots_light", 1440, 900, 1, false);
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id st = findFaction(h->world(), "Республика Корвен");
  openTab(h, st, "faction.economy");
  h->openDrawer("states");
  h.settle();
  h.waitMap();
  CHECK(h.shot("faction_light_economy"));
  openTab(h, st, "faction.diplomacy");
  h.waitMap();
  CHECK(h.shot("faction_light_diplomacy"));
  openTab(h, st, "faction.overview");
  CHECK(h.clickUi("faction.flag"));
  h.settle();
  CHECK(h.shot("faction_light_flag_editor"));
}

// Узкий инспектор и масштаб 125 %: ничего не налезает, строки переносятся.
TEST(app_faction_shots_scaled) {
  HideTestRegs regs;
  Harness h("faction_shots_scaled", 1600, 1000, 1.25f);
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id g = findFaction(h->world(), "Гильдия магов Аркана");
  openTab(h, g, "faction.hqs");
  h.waitMap();
  CHECK(h.shot("faction_scaled_hqs"));
  Id st = findFaction(h->world(), "Республика Корвен");
  openTab(h, st, "faction.council");
  h.waitMap();
  CHECK(h.shot("faction_scaled_council"));
}
