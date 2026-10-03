// Сценарии хода: подтверждение с предварительным итогом, снимок мира до хода в истории проекта, уведомление
// с отчётом, история ходов и просмотр прошлого хода только для чтения, отмена хода, снимки несохранённого мира.
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

int actionToast(app::App& a) {
  auto& t = a.toasts();
  for (size_t i = 0; i < t.size(); i++)
    if (!t[i].actionLabel.empty() && t[i].closeAt < 0) return int(i);
  return -1;
}

}  // namespace

TEST(app_turn_end_with_snapshot) {
  Harness h("turn_end");
  h.demo();
  std::string dir = fs::join(h.root, "Мир");
  CHECK(h->saveTo(dir));
  int t0 = h->store.world().turn();
  CHECK(h.clickUi("topbar.endturn"));
  h.settle();
  CHECK(h->hasDialog("turn.confirm"));
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("turn_confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK_EQ(h->store.world().turn(), t0 + 1);
  CHECK(h->lastTurnReport().has_value());
  CHECK_EQ(h->lastTurnReport()->turnTo, t0 + 1);
  // Конец хода t0 и начало хода t0 + 1 — в памяти, пока мир не сохранён (на диск — только при сохранении).
  auto snaps = h->snapshots();
  CHECK_EQ(snaps.size(), size_t(2));
  CHECK_EQ(snaps[0].turn, t0);
  CHECK_EQ(snaps[0].kind, std::string(io::kSnapEnd));
  CHECK(snaps[0].memory);
  CHECK_EQ(snaps[1].turn, t0 + 1);
  CHECK_EQ(snaps[1].kind, std::string(io::kSnapStart));
  CHECK_EQ(io::listSnapshots(dir).size(), size_t(1));   // на диске — только начало хода t0 (первое сохранение)
  // Уведомление с кнопкой «Отчёт».
  int ti = actionToast(h.a());
  CHECK(ti >= 0);
  h.step();
  CHECK(h.shot("turn_toast"));
  CHECK(h.clickUi("toast." + std::to_string(ti) + ".action"));
  h.settle();
  CHECK(h->hasDialog("turn.report"));
  CHECK(h.shot("turn_report"));
  h.key(Key::Escape);
  h.settle();
  CHECK(!h->hasDialog());
  // История ходов: открыть прошлый ход только для чтения.
  CHECK(h.clickUi("topbar.turn"));
  h.settle();
  CHECK(h->hasDialog("turn.history"));
  CHECK(h.shot("turn_history"));
  CHECK(h.clickUi("history.open." + std::to_string(t0)));
  h.settle();
  CHECK(h->readOnly());
  CHECK_EQ(h->world().turn(), t0);
  CHECK(h->uiRect("banner") != nullptr);
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("turn_view_past"));
  // Изменения недоступны, завершение хода тоже.
  Id pid = 0;
  h->world().provinces.each([&](const Province& p) {
    if (!pid) pid = p.id;
  });
  CHECK(!h->act("Правка", [&](Tx& tx) { tx.province(pid).name = "x"; }));
  h->endTurn();
  h.settle();
  CHECK(!h->hasDialog("turn.confirm"));
  CHECK(h.clickUi("banner.back"));
  h.settle();
  CHECK(!h->readOnly());
  CHECK_EQ(h->world().turn(), t0 + 1);
  // Сохранение записывает снимки ходов в history/.
  CHECK(h->save());
  snaps = h->snapshots();
  CHECK_EQ(snaps.size(), size_t(2));
  CHECK(!snaps[0].memory && !snaps[1].memory);
  CHECK(fs::isFile(fs::join(dir, snaps[0].key)));
  CHECK_EQ(io::listSnapshots(dir).size(), size_t(3));
  // Ход отменяется Ctrl+Z.
  h.key(Key::Z, ctrl());
  CHECK_EQ(h->store.world().turn(), t0);
}

TEST(app_turn_memory_snapshots) {
  Harness h("turn_memory");
  h.demo();
  int t0 = h->store.world().turn();
  CHECK(h->endTurnNow());
  CHECK(h->endTurnNow());
  auto snaps = h->snapshots();
  CHECK_EQ(snaps.size(), size_t(3));   // ходы t0, t0 + 1 и начало текущего t0 + 2
  CHECK(snaps[0].memory && snaps[1].memory && snaps[2].memory);
  CHECK(h->viewTurn(t0));
  CHECK(h->readOnly());
  CHECK_EQ(h->world().turn(), t0);
  h->backToCurrent();
  CHECK_EQ(h->world().turn(), t0 + 2);
  // При первом сохранении снимки уходят в history/: начало и конец каждого хода, начало текущего.
  std::string dir = fs::join(h.root, "Мир");
  CHECK(h->saveTo(dir));
  snaps = h->snapshots();
  CHECK_EQ(snaps.size(), size_t(3));
  CHECK(!snaps[0].memory && !snaps[1].memory && !snaps[2].memory);
  CHECK(fs::isFile(fs::join(dir, snaps[1].key)));
  CHECK_EQ(io::listSnapshots(dir).size(), size_t(5));
  // «Сохранить как» в новую папку переносит историю.
  std::string dir2 = fs::join(h.root, "Копия");
  CHECK(h->saveTo(dir2));
  CHECK_EQ(io::listSnapshots(dir2).size(), size_t(5));
}
