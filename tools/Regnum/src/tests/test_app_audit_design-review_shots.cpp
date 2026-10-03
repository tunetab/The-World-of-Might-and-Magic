// Regnum — аудит оформления (design-review): только снимки экранов для просмотра глазами.
// Каждый экран снимается в четырёх видах: тёмная тема 1440×900 (d), светлая 1440×900 (l), малое окно 1100×700 (s),
// масштаб интерфейса 125 % на 1440×900 (z). Снимки: .wmma/regnum-tests/app_audit_dr_<вид>_<экран>.png.
// Тесты ничего не проверяют, кроме того, что экраны открываются (сбоев нет); найденные недочёты описаны в отчёте
// аудита. Запуск: build.sh test-app --test app_audit_dr
#include "tests/test_app_faction_util.h"
#include "tests/test_app_military_util.h"

using namespace rg;
using namespace rg::apptest;
using namespace rg::factest;

namespace {

struct Cfg {
  const char* tag;
  int w, h;
  float scale;
  bool dark;
};
const Cfg kCfgs[] = {{"d", 1440, 900, 1.f, true}, {"l", 1440, 900, 1.f, false}, {"s", 1100, 700, 1.f, true}, {"z", 1440, 900, 1.25f, true}};

// Только выбранные виды (REGNUM_AUDIT_CFG=dl — тёмная и светлая), по умолчанию все.
bool wanted(const Cfg& c) {
  const char* v = std::getenv("REGNUM_AUDIT_CFG");
  if (!v || !*v) return true;
  return std::string_view(v).find(c.tag) != std::string_view::npos;
}

struct Rig {
  Harness h;
  const Cfg& c;
  Rig(const Cfg& cfg, const std::string& name) : h("audit_dr_" + std::string(cfg.tag) + "_" + name, cfg.w, cfg.h, 1, cfg.dark), c(cfg) {
    if (cfg.scale != 1.f) h->setUiScale(cfg.scale);
    h.settle();
  }
  void demo() {
    h.demo();
    h.waitMap();
    h.dropToasts();
    h.settle();
  }
  // Указатель — на край окна над морем: без подсказок и подсветки наведения на снимке.
  void park() {
    hl::mouseMove(3, float(c.h) * 0.5f);
    h.step();
  }
  void shot(const std::string& name) {
    std::fprintf(stderr, "[audit] %s_%s\n", c.tag, name.c_str());
    park();
    h.settle();
    h.waitMap();
    h.frames(2);
    h.shot("audit_dr_" + std::string(c.tag) + "_" + name);
  }
};

Id firstProvinceOf(const World& w, Id owner) {
  Id r = 0;
  double best = -1;
  auto fs = geo::faces(w);
  w.provinces.each([&](const Province& p) {
    if (p.sea || p.owner != owner) return;
    const geo::ProvinceShape* sh = fs->shape(p.id);
    if (sh && sh->area > best) {
      best = sh->area;
      r = p.id;
    }
  });
  return r;
}

Id seaProvince(const World& w) {
  Id r = 0;
  w.provinces.each([&](const Province& p) {
    if (!r && p.sea) r = p.id;
  });
  return r;
}

Id characterWithRoles(const World& w) {
  Id r = 0;
  w.characters.each([&](const Character& c) {
    if (!r) r = c.id;
  });
  return r;
}

Id firstRoute(const World& w) {
  Id r = 0;
  w.routes.each([&](const Route& x) {
    if (!r) r = x.id;
  });
  return r;
}

void openSel(Rig& r, app::SelType t, Id id, const char* tab) {
  r.h->ui.tabOf[t] = tab;
  r.h->select(t, id);
  r.h.settle();
  r.h.dropToasts();
  if (const RectF* ir = r.h->uiRect("inspector")) {
    r.h.wheel(ir->cx(), ir->bottom() - 120, 40);
    r.h.frames(24);
  }
}

void tabsOf(Rig& r, app::SelType t, Id id, const std::string& prefix, bool bottoms) {
  std::vector<std::string> ids;
  for (const app::TabDef& d : app::tabs()) {
    if (d.type != t || !d.draw) continue;
    if (d.visible && !d.visible(r.h.a(), id)) continue;
    ids.push_back(d.id);
  }
  for (auto& tab : ids) {
    openSel(r, t, id, tab.c_str());
    std::string n = tab;
    for (char& ch : n)
      if (ch == '.') ch = '_';
    r.shot(prefix + "_" + n);
    if (bottoms) {
      scrollInspector(r.h, -40);
      r.shot(prefix + "_" + n + "_end");
    }
  }
}

}  // namespace

// ---------------------------------------------------------------- экран запуска
TEST(app_audit_dr_start) {
  HideTestRegs regs;
  for (const Cfg& c : kCfgs) {
    if (!wanted(c)) continue;
    Rig r(c, "start");
    r.shot("start");
  }
}

// ---------------------------------------------------------------- редактор: режимы карты
TEST(app_audit_dr_modes) {
  HideTestRegs regs;
  for (const Cfg& c : kCfgs) {
    if (!wanted(c)) continue;
    Rig r(c, "modes");
    r.demo();
    for (int i = 0; i < int(schema::MapMode::Count); i++) {
      r.h->setMapMode(schema::MapMode(i));
      r.shot("mode" + std::to_string(i + 1));
      if (c.tag[0] != 'd') break;   // в остальных видах — только политическая
    }
    r.h->setMapMode(schema::MapMode::Political);
    // Крупный и мелкий масштаб: подписи и фигурки.
    Id alm = findFaction(r.h->world(), "Королевство Альмарин");
    Id pid = firstProvinceOf(r.h->world(), alm);
    if (auto fs = geo::faces(r.h->world()); fs && fs->shape(pid)) {
      showAt(r.h, fs->shape(pid)->label, 1.6);
      r.shot("zoom_in");
      showAt(r.h, fs->shape(pid)->label, 0.25);
      r.shot("zoom_out");
    }
  }
}

// ---------------------------------------------------------------- инспектор провинции
TEST(app_audit_dr_province) {
  HideTestRegs regs;
  for (const Cfg& c : kCfgs) {
    if (!wanted(c)) continue;
    Rig r(c, "province");
    r.demo();
    Id alm = findFaction(r.h->world(), "Королевство Альмарин");
    Id pid = firstProvinceOf(r.h->world(), alm);
    tabsOf(r, app::SelType::Province, pid, "prov", c.tag[0] == 'd' || c.tag[0] == 's');
    Id sea = seaProvince(r.h->world());
    if (sea) {
      r.h->select(app::SelType::Province, sea);
      r.shot("prov_sea");
    }
  }
}

// ---------------------------------------------------------------- инспектор государства и гильдии
TEST(app_audit_dr_faction) {
  HideTestRegs regs;
  for (const Cfg& c : kCfgs) {
    if (!wanted(c)) continue;
    Rig r(c, "faction");
    r.demo();
    Id st = findFaction(r.h->world(), "Королевство Альмарин");
    tabsOf(r, app::SelType::Faction, st, "state", c.tag[0] == 'd');
    Id g = findFaction(r.h->world(), "Альмарская торговая компания");
    tabsOf(r, app::SelType::Faction, g, "guild", false);
  }
}

// ---------------------------------------------------------------- войско, флот, маршрут, персонаж
TEST(app_audit_dr_objects) {
  HideTestRegs regs;
  for (const Cfg& c : kCfgs) {
    if (!wanted(c)) continue;
    Rig r(c, "objects");
    RealArmyTools tools;
    r.demo();
    const World& w = r.h->world();
    Id hel = factionByName(w, "Хельдвиг");
    Id army = armyOf(w, hel, ArmyKind::Army);
    Id fleet = 0;
    w.factions.each([&](const Faction& f) {
      if (!fleet) fleet = armyOf(w, f.id, ArmyKind::Fleet);
    });
    if (army) {
      tabsOf(r, app::SelType::Army, army, "army", c.tag[0] == 'd');
      r.h->focusSelection();
      r.shot("army_focus");
    }
    if (fleet) tabsOf(r, app::SelType::Army, fleet, "fleet", false);
    Id rt = firstRoute(r.h->world());
    if (rt) tabsOf(r, app::SelType::Route, rt, "route", false);
    Id ch = characterWithRoles(r.h->world());
    if (ch) tabsOf(r, app::SelType::Character, ch, "char", false);
    // Союзное войско: Альмарин + Хельдвиг.
    Id alm = factionByName(r.h->world(), "Альмарин");
    if (army && alm) {
      Vec2 pos = r.h->world().army(army)->pos;
      auto spot = rules::findFreeSpot(r.h->world(), ArmyKind::Army, pos + Vec2(-160, 20));
      if (spot) {
        Id nid = 0;
        Id row = r.h->world().faction(alm)->army[0].id;
        r.h->act("Союзное войско (аудит)", [&](Tx& tx) {
          nid = rules::createArmy(tx, ArmyKind::Army, alm, *spot);
          rules::setUnits(tx, nid, alm, row, 400);
          rules::formAllied(tx, army, nid);
        });
        tabsOf(r, app::SelType::Army, army, "allied", false);
      }
    }
  }
}

// ---------------------------------------------------------------- выдвижные панели
TEST(app_audit_dr_drawers) {
  HideTestRegs regs;
  for (const Cfg& c : kCfgs) {
    if (!wanted(c)) continue;
    Rig r(c, "drawers");
    r.demo();
    for (const app::DrawerDef& d : app::drawers()) {
      r.h->openDrawer(d.id);
      r.h.settle();
      r.shot(std::string("drawer_") + d.id);
      r.h->openDrawer(d.id);   // закрыть
      r.h.settle();
    }
    // Выдвижная панель и инспектор одновременно (самая тесная раскладка).
    Id st = findFaction(r.h->world(), "Королевство Альмарин");
    r.h->openDrawer("provinces");
    openSel(r, app::SelType::Faction, st, "faction.overview");
    r.shot("drawer_and_inspector");
  }
}

// ---------------------------------------------------------------- полноэкранные редакторы
TEST(app_audit_dr_editors) {
  HideTestRegs regs;
  for (const Cfg& c : kCfgs) {
    if (!wanted(c)) continue;
    Rig r(c, "editors");
    r.demo();
    Id st = findFaction(r.h->world(), "Королевство Альмарин");
    for (const char* e : {"techtree", "buildings", "military", "trade", "modifiers", "catalogs"}) {
      r.h->openEditor(e, st);
      r.h.settle();
      r.shot(std::string("editor_") + e);
      r.h->closeEditor();
      r.h.settle();
    }
  }
}

// ---------------------------------------------------------------- диалоги
TEST(app_audit_dr_dialogs) {
  HideTestRegs regs;
  for (const Cfg& c : kCfgs) {
    if (!wanted(c)) continue;
    Rig r(c, "dialogs");
    RealArmyTools tools;
    r.demo();
    auto& h = r.h;
    const World& w0 = h->world();
    Id alm = factionByName(w0, "Альмарин");
    Id hel = factionByName(w0, "Хельдвиг");
    Id vk = factionByName(w0, "Валь-Кетра");
    Id pid = firstProvinceOf(w0, alm);
    auto close = [&] {
      h->closeDialogs();
      h.settle();
      h.dropToasts();
    };
    // Палитра, справка, настройки (все вкладки).
    h->showPalette();
    r.shot("dlg_palette");
    h.type("пров");
    r.shot("dlg_palette_query");
    close();
    h->showHelp();
    r.shot("dlg_help");
    close();
    h->showSettings();
    r.shot("dlg_settings");
    if (const RectF* t = h->uiRect("settings.tabs")) {
      for (int i = 1; i < 4; i++) {
        h.click(t->x + t->w * (float(i) + 0.5f) / 4, t->cy());
        r.shot("dlg_settings_tab" + std::to_string(i));
      }
    }
    close();
    // Файлы.
    h->openWorldDialog();
    r.shot("dlg_browser");
    close();
    h->newWorldDialog();
    r.shot("dlg_newworld");
    close();
    h->saveAs();
    r.shot("dlg_saveas");
    close();
    // Подтверждение, запрос, сообщение.
    h->confirm("Удалить провинцию?", "Земли отойдут соседям. Действие можно отменить Ctrl+Z.", "Удалить", true, [](app::App&) {});
    r.shot("dlg_confirm");
    close();
    h->prompt("Новое государство", "Название", "Королевство", [](app::App&, const std::string&) {});
    r.shot("dlg_prompt");
    close();
    h->message("Мир прочитан с предупреждениями", "Некоторые записи пропущены:", {"Провинция 12: неверный владелец", "Маршрут 4: нет точек"},
               app::ToastKind::Warning);
    r.shot("dlg_message");
    close();
    // Флаг, дань, выбор постройки, запись хроники.
    h->openDialog("flag", alm);
    r.shot("dlg_flag");
    close();
    h->openDialog("tribute", alm);
    r.shot("dlg_tribute");
    close();
    h->openDialog("build.picker", pid);
    r.shot("dlg_build_picker");
    close();
    h->openDialog("chronicle.note");
    r.shot("dlg_note");
    close();
    // Войска: битва, встречи, разделение.
    const World& w = h->world();
    Id ha = armyOf(w, hel, ArmyKind::Army);
    Id va = armyOf(w, vk, ArmyKind::Army);
    if (ha && va) {
      app::mil::openBattle(h.a(), ha, va, w.army(ha)->pos);
      r.shot("dlg_battle");
      close();
    }
    Id aa = armyOf(h->world(), alm, ArmyKind::Army);
    if (aa && ha) {
      rules::Encounter e = rules::encounter(h->world(), aa, h->world().army(ha)->pos);
      if (e.type != rules::EncounterType::None) {
        app::mil::openEncounter(h.a(), aa, ha, e, h->world().army(aa)->pos, [](app::App&, bool) {});
        r.shot("dlg_encounter_ally");
        close();
      }
    }
    if (aa && va) {
      rules::Encounter e = rules::encounter(h->world(), aa, h->world().army(va)->pos);
      if (e.type != rules::EncounterType::None) {
        app::mil::openEncounter(h.a(), aa, va, e, h->world().army(aa)->pos, [](app::App&, bool) {});
        r.shot("dlg_encounter_war");
        close();
      }
    }
    if (aa) {
      // Второе войско той же фракции рядом — объединение.
      auto spot = rules::findFreeSpot(h->world(), ArmyKind::Army, h->world().army(aa)->pos + Vec2(150, 0));
      if (spot) {
        Id nid = 0;
        Id row = h->world().faction(alm)->army[0].id;
        h->act("Второе войско (аудит)", [&](Tx& tx) {
          nid = rules::createArmy(tx, ArmyKind::Army, alm, *spot);
          rules::setUnits(tx, nid, alm, row, 100);
        });
        rules::Encounter e = rules::encounter(h->world(), nid, h->world().army(aa)->pos);
        if (e.type != rules::EncounterType::None) {
          app::mil::openEncounter(h.a(), nid, aa, e, *spot, [](app::App&, bool) {});
          r.shot("dlg_encounter_merge");
          close();
        }
      }
      app::mil::openSplit(h.a(), aa);
      r.shot("dlg_split");
      close();
    }
    // Ход: подтверждение, отчёт, история, просмотр прошлого хода.
    h->endTurn();
    r.shot("dlg_turn_confirm");
    if (const RectF* t = h->uiRect("turn.confirm.tabs")) {
      for (int i = 1; i < 3; i++) {
        h.click(t->x + t->w * (float(i) + 0.5f) / 3, t->cy());
        r.shot("dlg_turn_confirm_tab" + std::to_string(i));
      }
    }
    close();
    h->endTurnNow();
    h.settle();
    r.shot("after_turn_toast");
    h->openDialog("turn.report");
    r.shot("dlg_turn_report");
    close();
    h->openDialog("turn.history");
    r.shot("dlg_history");
    close();
    if (h->viewTurn(1)) {
      h.settle();
      h.dropToasts();
      r.shot("readonly_view");
      h->select(app::SelType::Province, pid);
      r.shot("readonly_province");
      h->backToCurrent();
      h.settle();
    }
    // Несохранённые изменения при закрытии мира.
    h->closeWorld();
    r.shot("dlg_unsaved");
    close();
  }
}

// ---------------------------------------------------------------- инструменты карты
TEST(app_audit_dr_tools) {
  HideTestRegs regs;
  for (const Cfg& c : kCfgs) {
    if (!wanted(c)) continue;
    Rig r(c, "tools");
    RealArmyTools tools;
    r.demo();
    auto& h = r.h;
    Id alm = findFaction(h->world(), "Королевство Альмарин");
    Id pid = firstProvinceOf(h->world(), alm);
    auto fs = geo::faces(h->world());
    Vec2 lab = fs->shape(pid)->label;
    h->select(app::SelType::Province, pid);
    showAt(h, lab, 0.9);
    h->setEditBorders(true);
    h.settle();
    r.shot("tool_borders_on");
    h->setTool(app::ToolId::EditBorders);
    r.shot("tool_edit_borders");
    gfx::Pt s = screenOf(h, lab);
    h->setTool(app::ToolId::NewProvince);
    h.click(s.x - 60, s.y - 40);
    h.click(s.x + 40, s.y - 50);
    h.click(s.x + 30, s.y + 40);
    r.shot("tool_new_province");
    h.key(Key::Escape);
    h->setTool(app::ToolId::Knife);
    h.click(s.x - 80, s.y);
    h.move(s.x + 80, s.y + 10);
    r.shot("tool_knife");
    h.key(Key::Escape);
    h->setTool(app::ToolId::Merge);
    h.move(s.x + 140, s.y);
    r.shot("tool_merge");
    h->setTool(app::ToolId::Fill);
    r.shot("tool_fill");
    h->setTool(app::ToolId::AddArea);
    r.shot("tool_add_area");
    h->setEditBorders(false);
    h.settle();
    h->setTool(app::ToolId::Route);
    h.click(s.x, s.y);
    h.move(s.x + 120, s.y + 60);
    r.shot("tool_route");
    h.key(Key::Escape);
    h->setTool(app::ToolId::NewArmy);
    h.move(s.x + 30, s.y + 30);
    r.shot("tool_new_army");
    h->setTool(app::ToolId::NewFleet);
    r.shot("tool_new_fleet");
    h->setTool(app::ToolId::Select);
    // Контекстное меню на карте и уведомление.
    h.click(s.x, s.y, platform::MouseRight);
    r.shot("map_context_menu");
    h.key(Key::Escape);
    h->toast("Мир сохранён", app::ToastKind::Success);
    h->toast("Не удалось открыть архив: файл повреждён", app::ToastKind::Danger, "", "Подробнее", [](app::App&) {});
    h.frames(20);
    h.shot(std::string("audit_dr_") + c.tag + "_toasts");
  }
}
