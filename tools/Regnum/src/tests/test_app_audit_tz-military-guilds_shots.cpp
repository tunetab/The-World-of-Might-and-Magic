// Аудит ТЗ 1.c (войска и флот) и 1.d (торговые гильдии): снимки ключевых экранов для визуальной проверки.
// Тест ничего не утверждает о дефектах — только снимает экраны (app_audit_tzmil_*.png) для разбора аудитором.
#include "tests/test_app_military_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

void tab(Harness& h, app::SelType t, Id id, const char* tabId) {
  h->ui.tabOf[t] = tabId;
  h->select(t, id);
  h.settle();
  h.dropToasts();
  h.settle();
}

}  // namespace

// Вкладки фракции: войска, флот, полноэкранные таблицы; гильдия: обзор, штабы, войска; вкладка «Гильдии» государства.
TEST(app_audit_tzmil_shots_faction) {
  Harness h("audit_tzmil_faction");
  RealArmyTools tools;
  h.demo();
  h.waitMap();
  const World& w = h->world();
  Id hel = factionByName(w, "Хельдвиг");
  Id alm = factionByName(w, "Альмарин");
  Id amber = factionByName(w, "Янтарная");
  Id comp = factionByName(w, "Альмарская");
  CHECK(hel && alm && amber && comp);
  tab(h, app::SelType::Faction, hel, "faction.army");
  CHECK(h.shot("audit_tzmil_faction_army"));
  tab(h, app::SelType::Faction, hel, "faction.fleet");
  CHECK(h.shot("audit_tzmil_faction_fleet"));
  h->openEditor("military", hel);
  h.settle();
  CHECK(h.shot("audit_tzmil_military_editor"));
  h->closeEditor();
  h.settle();
  tab(h, app::SelType::Faction, alm, "faction.guilds");
  CHECK(h.shot("audit_tzmil_state_guilds"));
  tab(h, app::SelType::Faction, comp, "faction.overview");
  CHECK(h.shot("audit_tzmil_stateguild_overview"));
  tab(h, app::SelType::Faction, amber, "faction.overview");
  CHECK(h.shot("audit_tzmil_guild_overview"));
  tab(h, app::SelType::Faction, amber, "faction.hqs");
  CHECK(h.shot("audit_tzmil_guild_hqs"));
  tab(h, app::SelType::Faction, amber, "faction.army");
  CHECK(h.shot("audit_tzmil_guild_army"));
  tab(h, app::SelType::Faction, amber, "faction.diplomacy");
  CHECK(h.shot("audit_tzmil_guild_diplomacy"));
  tab(h, app::SelType::Faction, amber, "faction.economy");
  CHECK(h.shot("audit_tzmil_guild_economy"));
  tab(h, app::SelType::Faction, amber, "faction.trade");
  CHECK(h.shot("audit_tzmil_guild_trade"));
}

// Инспектор войска (обычное и союзное), разделение, гильдейский режим карты.
TEST(app_audit_tzmil_shots_army) {
  Harness h("audit_tzmil_army");
  RealArmyTools tools;
  h.demo();
  h.waitMap();
  const World& w = h->world();
  Id hel = factionByName(w, "Хельдвиг");
  Id x = armyOf(w, hel, ArmyKind::Army);
  CHECK(x != 0);
  tab(h, app::SelType::Army, x, "army.units");
  showAt(h, w.army(x)->pos, 0.6);
  h.dropToasts();
  CHECK(h.shot("audit_tzmil_army_inspector"));
  // Союзное войско демонстрационного мира.
  Id allied = 0;
  h->world().armies.each([&](const Army& a) {
    if (!allied && a.allied() && !a.isFleet()) allied = a.id;
  });
  CHECK(allied != 0);
  tab(h, app::SelType::Army, allied, "army.units");
  showAt(h, h->world().army(allied)->pos, 0.6);
  h.dropToasts();
  CHECK(h.shot("audit_tzmil_allied_inspector"));
  // Разделение союзного войска.
  CHECK(h.clickUi("army.split"));
  h.settle();
  CHECK(h->hasDialog("army.split"));
  CHECK(h.shot("audit_tzmil_split_dialog"));
  h.key(Key::Escape);
  h.settle();
  // Режим гильдий: весь мир и крупно.
  h->clearSelection();
  h->setMapMode(schema::MapMode::Guilds);
  h.settle();
  h.waitMap();
  h.dropToasts();
  CHECK(h.shot("audit_tzmil_guild_mode_default"));
  Box2 all = geo::faces(h->world())->bounds;
  h->focusMap(all);
  h.settle();
  h.waitMap();
  h.dropToasts();
  CHECK(h.shot("audit_tzmil_guild_mode_world"));
  Id alm = factionByName(h->world(), "Альмарин");
  Id cap = h->world().faction(alm)->capital;
  if (const auto fs = geo::faces(h->world()); fs && fs->shape(cap)) {
    showAt(h, fs->shape(cap)->label, 0.5);
    h.dropToasts();
    CHECK(h.shot("audit_tzmil_guild_mode_zoom"));
  }
}

// Поиск повторяющихся ID виджетов: обход вкладок фракций и войска с отметками в выводе.
TEST(app_audit_tzmil_dup_ids) {
  Harness h("audit_tzmil_dupids");
  RealArmyTools tools;
  h.demo();
  const World& w = h->world();
  Id hel = factionByName(w, "Хельдвиг");
  Id amber = factionByName(w, "Янтарная");
  Id comp = factionByName(w, "Альмарская");
  for (Id f : {hel, amber, comp})
    for (const char* t : {"faction.overview", "faction.council", "faction.provinces", "faction.hqs", "faction.economy", "faction.diplomacy",
                          "faction.trade", "faction.army", "faction.fleet", "faction.heroes", "faction.tech", "faction.modifiers", "faction.guilds",
                          "faction.buildings"}) {
      std::printf("  [audit tzmil] вкладка %s фракции %lld\n", t, (long long)f);
      std::fflush(stdout);
      tab(h, app::SelType::Faction, f, t);
      std::fflush(stderr);
    }
  Id allied = 0;
  w.armies.each([&](const Army& a) {
    if (!allied && a.allied()) allied = a.id;
  });
  for (Id ar : {armyOf(w, hel, ArmyKind::Army), armyOf(w, hel, ArmyKind::Fleet), allied}) {
    std::printf("  [audit tzmil] войско %lld\n", (long long)ar);
    std::fflush(stdout);
    tab(h, app::SelType::Army, ar, "army.units");
    std::fflush(stderr);
  }
}
