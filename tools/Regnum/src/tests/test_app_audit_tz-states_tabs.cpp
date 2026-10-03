// Аудит ТЗ 1.b / 1.e, аудитор tz-states (продолжение): снимки вкладок государства и редакторов (совет, провинции,
// герои, экономика, торговля, дерево технологий) и воспроизведения расхождений, найденных по ним.
// Найденные расхождения исправлены; их проверки — обычные CHECK (регрессионные тесты).
#include <cstdio>

#include "app/editors/techtree.h"
#include "app/editors/trade.h"
#include "tests/test_app_faction_util.h"

using namespace rg;
using namespace rg::apptest;
using namespace rg::factest;

namespace {

void shot(Harness& h, const std::string& name) {
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot(name));
}

Id techByName(const World& w, Id faction, std::string_view name) {
  Id r = 0;
  w.techs.each([&](const Tech& t) {
    if (t.faction == faction && t.name == name) r = t.id;
  });
  return r;
}

}  // namespace

// ---------------------------------------------------------------- снимки вкладок Альмарина (ТЗ 1.b.iv)
TEST(app_audit_tz_states_tab_shots) {
  HideTestRegs regs;
  Harness h("audit_tzstates_tabs");
  h.demo();
  h.waitMap();
  Id alm = findFaction(h->world(), "Королевство Альмарин");
  Id hel = findFaction(h->world(), "Северный союз Хельдвиг");
  CHECK(alm && hel);
  openTab(h, alm, "faction.council");
  shot(h, "audit_tzstates_tab_council");
  openTab(h, alm, "faction.provinces");
  shot(h, "audit_tzstates_tab_provinces");
  scrollInspector(h, -6);
  h.frames(24);
  shot(h, "audit_tzstates_tab_provinces2");
  openTab(h, alm, "faction.heroes");
  shot(h, "audit_tzstates_tab_heroes");
  openTab(h, alm, "faction.economy");
  shot(h, "audit_tzstates_tab_economy");
  openTab(h, alm, "faction.trade");
  shot(h, "audit_tzstates_tab_trade");

  // Редактор торговли: подарок золотом и поставка ресурса «каждый ход».
  app::trade::startDraft(alm, hel);
  {
    app::trade::DraftItem& g = app::trade::addItem(DealSide::A);
    g.res = kGold;
    g.amount = 250;
  }
  {
    Id res = 0;
    for (auto& [r, v] : h->world().faction(hel)->res)
      if (r != kGold && v > 0 && !res) res = r;
    app::trade::DraftItem& p = app::trade::addItem(DealSide::B);
    p.res = res ? res : kGold;
    p.amount = 15;
    p.mode = DealMode::PerTurn;
    p.turns = 6;
  }
  h->openEditor("trade", alm);
  h.settle();
  shot(h, "audit_tzstates_trade_editor");

  // Дерево технологий Альмарина.
  Id iron = techByName(h->world(), alm, "Обработка железа");
  app::openTechTree(h.a(), alm, iron);
  h.settle();
  shot(h, "audit_tzstates_techtree");
}

// ---------------------------------------------------------------- 1.b.vi: щелчок по полю отношений меняет отношения
// Модификатор «Отношения за ход» задаётся с одним знаком после запятой (+2,5). После хода отношения дробные (+2,5),
// поле отношений показывает целое. Щелчок в поле и Tab без ввода записывают округлённое значение — у обеих сторон
// (отношения симметричны), появляется шаг отмены «Отношения».
TEST(app_audit_tz_states_relation_click_rounds) {
  HideTestRegs regs;
  Harness h("audit_tzstates_relround");
  h.demo();
  h.waitMap();
  Id alm = findFaction(h->world(), "Королевство Альмарин");
  Id hel = findFaction(h->world(), "Северный союз Хельдвиг");
  CHECK(alm && hel);
  CHECK(h->act("Модификатор дипломатии", [&](Tx& tx) {
    Id m = rules::createModifier(tx, "Посольство");
    Modifier& md = tx.modifier(m);
    md.fx[size_t(Fx::DiplomacyPerTurn)] = 2.5;
    md.fxMask |= 1u << int(Fx::DiplomacyPerTurn);
    md.targets.push_back(hel);
    tx.faction(alm).modifiers.push_back(m);
  }));
  const double r0 = h->world().relation(alm, hel).v;
  CHECK(h->endTurnNow());
  h.settle();
  h.dropToasts();
  const double r1 = h->world().relation(alm, hel).v;
  CHECK_NEAR(r1, r0 + 2.5, 1e-9);
  const std::string undo0 = h->store.canUndo() ? h->store.undoLabel() : std::string();
  openTab(h, alm, "faction.diplomacy");
  CHECK(clickIn(h, "dip.value." + std::to_string(hel)));
  h.key(Key::Tab);
  h.settle();
  const double r2 = h->world().relation(alm, hel).v;
  const std::string undo1 = h->store.canUndo() ? h->store.undoLabel() : std::string();
  CHECK_MSG(r2 == r1, "щелчок по полю отношений и Tab без ввода изменили отношения Альмарина и Хельдвига: " + fmtNum(r1, 2) + " → " + fmtNum(r2, 2) +
                      " (шаг отмены «" + undo1 + "»)");
  CHECK_MSG(undo1 == undo0, "после щелчка по полю отношений появился шаг отмены «" + undo1 + "»");
  shot(h, "audit_tzstates_relation_round");
}
