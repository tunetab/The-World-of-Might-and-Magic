// Сценарии хода, истории и хроники: подтверждение с пробным ходом (вкладки казны, событий, предупреждений), отчёт о
// ходе с записями хроники, два хода подряд со снимками, история (лента, сравнение), просмотр прошлого хода только
// для чтения, возврат к снимку и его отмена, хроника (поиск, фильтр вида, запись ведущего, переход к объекту).
#include "app/dialogs/turn_ui.h"
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

int toastWith(app::App& a, std::string_view label) {
  auto& t = a.toasts();
  for (size_t i = 0; i < t.size(); i++)
    if (t[i].actionLabel == label && t[i].closeAt < 0) return int(i);
  return -1;
}

// Щёлкнуть по i-й из n равных частей элемента (вкладки с fill).
bool clickPart(Harness& h, std::string_view name, int i, int n) {
  const RectF* r = h->uiRect(name);
  if (!r) return false;
  RectF c = *r;
  h.click(c.x + c.w * (float(i) + 0.5f) / float(n), c.cy());
  return true;
}

void shotClean(Harness& h, const std::string& name) {
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot(name));
}

}  // namespace

TEST(app_turn2_flow_history_rollback) {
  Harness h("turn2_flow");
  h.demo();
  std::string dir = fs::join(h.root, "Мир");
  CHECK(h->saveTo(dir));
  int t0 = h->store.world().turn();

  // Подтверждение: пробный ход не меняет мир.
  World before = h->store.world();
  CHECK(h.clickUi("topbar.endturn"));
  h.settle();
  CHECK(h->hasDialog("turn.confirm"));
  CHECK(World::diff(before, h->store.world()) == 0);
  shotClean(h, "turn2_confirm");
  // Вкладки: ресурсы, события, внимание.
  CHECK(clickPart(h, "turn.confirm.tabs", 1, 4));
  shotClean(h, "turn2_confirm_resources");
  CHECK(clickPart(h, "turn.confirm.tabs", 2, 4));
  shotClean(h, "turn2_confirm_events");
  CHECK(clickPart(h, "turn.confirm.tabs", 3, 4));
  shotClean(h, "turn2_confirm_warnings");
  CHECK(h->hasDialog("turn.confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK_EQ(h->store.world().turn(), t0 + 1);
  CHECK(!h->snapshots().empty() && h->snapshots().front().turn == t0 && h->snapshots().front().kind == io::kSnapEnd);

  // Отчёт: записи хроники хода, карточки фракций.
  int ti = toastWith(h.a(), "Отчёт");
  CHECK(ti >= 0);
  CHECK(h.clickUi("toast." + std::to_string(ti) + ".action"));
  h.settle();
  CHECK(h->hasDialog("turn.report"));
  CHECK(h->uiRect("turn.report.body") != nullptr);
  CHECK(!h->lastTurnReport()->logIds.empty());
  shotClean(h, "turn2_report");
  // «Хроника хода» — панель с фильтром по ходу.
  CHECK(h.clickUi("turn.report.chronicle"));
  h.settle();
  CHECK(!h->hasDialog());
  CHECK_EQ(h->ui.drawer, std::string("chronicle"));
  for (Id lid : h->lastTurnReport()->logIds) CHECK(h->uiRect("chronicle.entry." + std::to_string(lid)) != nullptr);
  shotClean(h, "turn2_chronicle_turn");

  // Второй ход.
  CHECK(h->endTurnNow());
  h.settle();
  auto snaps = h->snapshots();
  CHECK_EQ(snaps.size(), size_t(3));   // ходы t0, t0 + 1 и начало текущего
  CHECK_EQ(snaps[1].turn, t0 + 1);
  CHECK(snaps[1].memory);   // до сохранения мира
  CHECK_EQ(h->store.world().turn(), t0 + 2);

  // История: лента и сравнение (снимки читаются в фоне).
  h->openDialog("turn.history");
  h.dropToasts();
  h.settle();
  h.settle();
  CHECK(h->hasDialog("turn.history"));
  CHECK(h->uiRect("history.open." + std::to_string(t0)) != nullptr);
  CHECK(h->uiRect("history.rollback." + std::to_string(t0 + 1)) != nullptr);
  shotClean(h, "turn2_history");
  CHECK(clickPart(h, "history.tabs", 1, 2));
  h.settle();
  shotClean(h, "turn2_history_compare");
  CHECK(clickPart(h, "history.tabs", 0, 2));
  h.settle();

  // Просмотр прошлого хода: только чтение, хроника показывает мир того хода.
  CHECK(h.clickUi("history.open." + std::to_string(t0 + 1)));
  h.settle();
  CHECK(h->readOnly());
  CHECK_EQ(h->world().turn(), t0 + 1);
  CHECK(h->uiRect("banner") != nullptr);
  CHECK(!h->act("Правка", [&](Tx& tx) { tx.meta().name = "x"; }));
  shotClean(h, "turn2_view_past");

  // Возврат к ходу t0: подтверждение, текущее состояние остаётся в истории.
  World cur = h->store.world();
  h->openDialog("turn.history");
  h.settle();
  CHECK(h.clickUi("history.rollback." + std::to_string(t0)));
  h.settle();
  CHECK(h->hasDialog("confirm"));
  shotClean(h, "turn2_rollback_confirm");
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK(!h->readOnly());
  CHECK(!h->hasDialog());
  CHECK_EQ(h->store.world().turn(), t0);
  CHECK(!h->dirty());   // конец хода t0 — ровно сохранённый мир (снимок в памяти, а не перечитанный с диска)
  snaps = h->snapshots();
  CHECK_EQ(snaps.size(), size_t(3));
  CHECK_EQ(snaps.back().turn, t0 + 2);
  CHECK_EQ(snaps.back().kind, std::string(io::kSnapBranch));
  CHECK_EQ(snaps.front().kind, std::string(io::kSnapStart));   // текущий ход t0 — его начало
  // Отмена возврата из уведомления.
  ti = toastWith(h.a(), "Отменить");
  CHECK(ti >= 0);
  CHECK(h.clickUi("toast." + std::to_string(ti) + ".action"));
  h.settle();
  CHECK_EQ(h->store.world().turn(), t0 + 2);
  CHECK(World::diff(cur, h->store.world()) == 0);
  // Вперёд по истории — к сохранённому состоянию хода t0 + 2 тем же способом.
  CHECK(app::turnui::rollbackToTurn(h.a(), t0));
  h.settle();
  CHECK_EQ(h->store.world().turn(), t0);
  CHECK(app::turnui::rollbackToTurn(h.a(), t0 + 2));
  h.settle();
  CHECK_EQ(h->store.world().turn(), t0 + 2);
  CHECK_EQ(h->store.world().log.size(), cur.log.size());
}

TEST(app_turn2_memory_rollback_and_stale_report) {
  Harness h("turn2_memory");
  h.demo();
  int t0 = h->store.world().turn();
  CHECK(h->endTurnNow());
  CHECK(h->endTurnNow());
  CHECK_EQ(h->snapshots().size(), size_t(3));   // ходы t0, t0 + 1 и начало текущего
  // Отчёт отменённого хода не открывается.
  h.key(Key::Z, ctrl());
  CHECK_EQ(h->store.world().turn(), t0 + 1);
  h->openDialog("turn.report");
  h.settle();
  CHECK(!h->hasDialog("turn.report"));
  h.key(Key::Y, ctrl());
  CHECK_EQ(h->store.world().turn(), t0 + 2);
  h->openDialog("turn.report");
  h.settle();
  CHECK(h->hasDialog("turn.report"));
  h.key(Key::Escape);
  h.settle();
  // Возврат в несохранённом мире: снимки в памяти.
  CHECK(app::turnui::rollbackToTurn(h.a(), t0 + 1));
  h.settle();
  CHECK_EQ(h->store.world().turn(), t0 + 1);
  auto snaps = h->snapshots();
  CHECK_EQ(snaps.size(), size_t(3));
  CHECK(snaps.back().memory);
  CHECK(h->viewTurn(t0 + 2));
  CHECK(h->readOnly());
  CHECK_EQ(h->world().turn(), t0 + 2);
  h->backToCurrent();
  CHECK_EQ(h->world().turn(), t0 + 1);
  // Снимка нет — мир не меняется.
  World w = h->store.world();
  CHECK(!app::turnui::rollbackToTurn(h.a(), t0 + 40));
  CHECK(World::diff(w, h->store.world()) == 0);
}

TEST(app_turn2_chronicle_filter_note) {
  Harness h("turn2_chronicle");
  h.demo();
  // Демо-экономика сбалансирована: государство с отрицательным чистым доходом и почти пустой казной уйдёт в долг.
  Id debtor = 0;
  auto c0 = rules::calc(h->store.world());
  h->store.world().factions.each([&](const Faction& f) {
    if (!debtor && f.isState() && c0->faction(f.id)->net < 0) debtor = f.id;
  });
  CHECK(debtor != 0);
  CHECK(h->act("Казна", [&](Tx& tx) { tx.faction(debtor).res[kGold] = 1; }));
  int t0 = h->store.world().turn();
  CHECK(h->endTurnNow());
  h.settle();
  const World& w0 = h->store.world();
  CHECK(h.clickUi("drawer.chronicle"));
  h.settle();
  CHECK_EQ(h->ui.drawer, std::string("chronicle"));
  shotClean(h, "turn2_chronicle");
  // Все записи хода видны (новые сверху, группы по ходам).
  Id turnEntry = 0, buildEntry = 0, debtEntry = 0;
  w0.log.each([&](const LogEntry& e) {
    if (e.turn != t0) return;
    if (e.kind == LogKind::Turn) turnEntry = e.id;
    if (e.kind == LogKind::Build && !buildEntry) buildEntry = e.id;
    if (e.kind == LogKind::Economy && !debtEntry) debtEntry = e.id;
  });
  CHECK(turnEntry && debtEntry);
  CHECK(h->uiRect("chronicle.entry." + std::to_string(turnEntry)) != nullptr);
  // Поиск.
  CHECK(h.clickUi("chronicle.search"));
  h.type("долг");
  h.settle();
  CHECK(h->uiRect("chronicle.entry." + std::to_string(debtEntry)) != nullptr);
  CHECK(h->uiRect("chronicle.entry." + std::to_string(turnEntry)) == nullptr);
  h.key(Key::A, ctrl());
  h.key(Key::Backspace);
  h.key(Key::Escape);
  h.settle();
  CHECK(h->uiRect("chronicle.entry." + std::to_string(turnEntry)) != nullptr);
  // Фильтр вида: только «Ход».
  CHECK(h.clickUi("chronicle.filter"));
  h.settle();
  CHECK(h.clickUi("chronicle.kind." + std::to_string(int(LogKind::Turn))));
  h.settle();
  shotClean(h, "turn2_chronicle_filter");
  h.key(Key::Escape);
  h.settle();
  CHECK(h->uiRect("chronicle.entry." + std::to_string(turnEntry)) != nullptr);
  CHECK(h->uiRect("chronicle.entry." + std::to_string(debtEntry)) == nullptr);
  CHECK(h.clickUi("chronicle.filter"));
  h.settle();
  CHECK(h.clickUi("chronicle.kind." + std::to_string(int(LogKind::Turn))));
  h.key(Key::Escape);
  h.settle();
  CHECK(h->uiRect("chronicle.entry." + std::to_string(debtEntry)) != nullptr);

  // Запись ведущего с провинцией.
  Id pid = 0;
  std::string pname;
  w0.provinces.each([&](const Province& p) {
    if (!pid && !p.sea && p.owner && !p.name.empty()) {
      pid = p.id;
      pname = p.name;
    }
  });
  u32 logs = h->store.world().log.size();
  CHECK(h.clickUi("chronicle.note"));
  h.settle();
  CHECK(h->hasDialog("chronicle.note"));
  h.type("Пир в столице по случаю урожая");
  CHECK(h.clickUi("note.province"));
  h.type(pname);
  h.key(Key::Enter);
  h.settle();
  shotClean(h, "turn2_chronicle_note");
  CHECK(h.clickUi("note.ok"));
  h.settle();
  CHECK(!h->hasDialog());
  const World& w1 = h->store.world();
  CHECK_EQ(w1.log.size(), logs + 1);
  Id note = 0;
  w1.log.each([&](const LogEntry& e) {
    if (e.kind == LogKind::Note) note = e.id;
  });
  CHECK(note != 0);
  const LogEntry* ne = w1.log.get(note);
  CHECK(ne && ne->text == "Пир в столице по случаю урожая");
  CHECK(ne && ne->turn == w1.turn() && ne->province == pid);
  // Щелчок по записи — провинция выделена и показана.
  CHECK(h.clickUi("chronicle.entry." + std::to_string(note)));
  h.settle();
  CHECK(h->ui.sel == (app::Selection{app::SelType::Province, pid}));
  shotClean(h, "turn2_chronicle_focus");
  // Запись отменяется Ctrl+Z.
  h.key(Key::Escape);
  h->undo();
  CHECK_EQ(h->store.world().log.size(), logs);
}

TEST(app_turn2_light_theme) {
  Harness h("turn2_light", 1440, 900, 1, false);
  h.demo();
  CHECK(h->endTurnNow());
  h.settle();
  h->openDialog("turn.report");
  h.settle();
  CHECK(h->hasDialog("turn.report"));
  shotClean(h, "turn2_report_light");
  h.key(Key::Escape);
  h.settle();
  h->openDialog("turn.history");
  h.dropToasts();
  h.settle();
  h.settle();
  shotClean(h, "turn2_history_light");
}
