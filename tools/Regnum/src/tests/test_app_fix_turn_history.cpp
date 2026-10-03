// Регрессионные сценарии истории ходов (исправления аудита tz-build-mods-turn): снимки уникальны и не заменяют
// друг друга, «Начать ход заново» — ровно начало хода текущей ветви (и после Ctrl+Z / Ctrl+Y, и после повторного
// открытия мира), снимки несохранённых ходов отбрасываются вместе с миром, история не переходит к другому миру.
#include <set>

#include "app/dialogs/turn_ui.h"
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

Id landProvince(const World& w) {
  Id pid = 0;
  w.provinces.each([&](const Province& p) {
    if (!pid && !p.sea && p.owner) pid = p.id;
  });
  return pid;
}

void rename(Harness& h, Id pid, const std::string& name) {
  CHECK(h->act("Имя", [&](Tx& tx) { tx.province(pid).name = name; }));
}

std::string nameOf(Harness& h, Id pid) { return h->store.world().province(pid)->name; }

void endTurn(Harness& h) {
  CHECK(h->endTurnNow());
  h->toasts().clear();
}

// «Начать ход заново» через историю ходов (как пользователь).
void restartViaHistory(Harness& h) {
  int t = h->store.world().turn();
  h->openDialog("turn.history");
  h.dropToasts();
  h.settle();
  CHECK(h.clickUi("history.rollback." + std::to_string(t)));
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK_EQ(h->store.world().turn(), t);
}

}  // namespace

// Снимок начала хода не заменяется: повторное прохождение хода создаёт новые файлы, прежние байты на диске целы.
TEST(app_fix_turn_history_start_never_overwritten) {
  Harness h("fix_turn_start_kept");
  h.demo();
  Id pid = landProvince(h->world());
  const std::string orig = nameOf(h, pid);
  std::string dir = fs::join(h.root, "Мир");
  CHECK(h->saveTo(dir));
  auto first = io::listSnapshots(dir);
  CHECK_EQ(first.size(), size_t(1));
  CHECK_EQ(first[0].kind, std::string(io::kSnapStart));
  const std::string startBytes = fs::readFile(fs::join(dir, first[0].file)).value_or("");
  CHECK(!startBytes.empty());
  rename(h, pid, "Ветвь 1");
  endTurn(h);
  endTurn(h);
  CHECK(app::turnui::rollbackToTurn(h.a(), 1));
  h.settle();
  rename(h, pid, "Ветвь 2");
  endTurn(h);
  endTurn(h);
  CHECK(h->save());
  auto all = io::listSnapshots(dir);
  std::set<std::string> files;
  std::set<u64> seqs;
  int starts2 = 0;
  for (auto& s : all) {
    files.insert(s.file);
    seqs.insert(s.seq);
    if (s.turn == 2 && s.kind == io::kSnapStart) starts2++;
  }
  CHECK_EQ(files.size(), all.size());
  CHECK_EQ(seqs.size(), all.size());
  CHECK_EQ(starts2, 2);   // начало хода 2 в каждой ветви
  CHECK_EQ(fs::readFile(fs::join(dir, first[0].file)).value_or(""), startBytes);
  // Начало хода 3 — ветви 2; начало хода 1 — исходный мир.
  rename(h, pid, "Правка в ходе 3");
  restartViaHistory(h);
  CHECK_EQ(nameOf(h, pid), std::string("Ветвь 2"));
  CHECK(app::turnui::rollbackToTurn(h.a(), 1));
  h.settle();
  restartViaHistory(h);
  CHECK_EQ(nameOf(h, pid), orig);
}

// Ctrl+Z и Ctrl+Y завершения хода и возврата возвращают и ветвь: «Начать ход заново» — начало хода той ветви,
// в которой мир сейчас (а не самый новый снимок начала этого хода).
TEST(app_fix_turn_history_undo_redo_keeps_branch) {
  Harness h("fix_turn_undo_branch");
  h.demo();
  Id pid = landProvince(h->world());
  rename(h, pid, "А");
  endTurn(h);   // ход 2 начинается с «А»
  rename(h, pid, "Б");
  endTurn(h);   // ход 3 (ветвь 1) начинается с «Б»
  CHECK_EQ(h->store.world().turn(), 3);
  rename(h, pid, "Б2");
  CHECK(app::turnui::rollbackToTurn(h.a(), 2));   // конец хода 2: «Б»
  h.settle();
  CHECK_EQ(nameOf(h, pid), std::string("Б"));
  rename(h, pid, "В");
  endTurn(h);   // ход 3 (ветвь 2) начинается с «В» — самое новое начало хода 3
  // Отмена завершения хода: снова ход 2 ветви 2; его начало — «А».
  h.key(Key::Z, ctrl());
  CHECK_EQ(h->store.world().turn(), 2);
  CHECK_EQ(nameOf(h, pid), std::string("В"));
  h.key(Key::Z, ctrl());   // правка «В»
  h.key(Key::Z, ctrl());   // возврат к ходу 2 — снова ход 3 ветви 1
  CHECK_EQ(h->store.world().turn(), 3);
  CHECK_EQ(nameOf(h, pid), std::string("Б2"));
  restartViaHistory(h);
  CHECK_EQ(nameOf(h, pid), std::string("Б"));
  // Отмена и повтор самого «Начать заново».
  h.key(Key::Z, ctrl());
  CHECK_EQ(nameOf(h, pid), std::string("Б2"));
  h.key(Key::Y, ctrl());
  CHECK_EQ(nameOf(h, pid), std::string("Б"));
  // Ход 2 ветви 1 начинается с «А».
  CHECK(app::turnui::rollbackToTurn(h.a(), 2));
  h.settle();
  rename(h, pid, "Г");
  restartViaHistory(h);
  CHECK_EQ(nameOf(h, pid), std::string("А"));
}

// Ветвь сохранённого мира записывается в историю (head): после повторного открытия «Начать ход заново» — начало
// хода сохранённой ветви, даже если более новый снимок начала этого хода принадлежит другой ветви.
TEST(app_fix_turn_history_branch_survives_reopen) {
  Harness h("fix_turn_branch_reopen");
  h.demo();
  Id pid = landProvince(h->world());
  std::string dir = fs::join(h.root, "Мир");
  CHECK(h->saveTo(dir));
  rename(h, pid, "Ветвь 1");
  endTurn(h);
  endTurn(h);   // ход 3 ветви 1
  CHECK(app::turnui::rollbackToTurn(h.a(), 2));
  h.settle();
  rename(h, pid, "Ветвь 2");
  endTurn(h);   // ход 3 ветви 2
  h.key(Key::Z, ctrl());
  h.key(Key::Z, ctrl());
  h.key(Key::Z, ctrl());   // снова ход 3 ветви 1
  CHECK_EQ(nameOf(h, pid), std::string("Ветвь 1"));
  CHECK_EQ(h->store.world().turn(), 3);
  rename(h, pid, "Правка хода 3");
  CHECK(h->save());
  CHECK(h->loadProject(dir));
  h.settle();
  h->closeDialogs();
  CHECK_EQ(nameOf(h, pid), std::string("Правка хода 3"));
  restartViaHistory(h);
  CHECK_EQ(nameOf(h, pid), std::string("Ветвь 1"));
  // Архив: та же ветвь после «Сохранить как» и открытия.
  std::string bundle = fs::join(h.root, "Мир.regnum");
  rename(h, pid, "Правка в архиве");
  CHECK(h->saveTo(bundle));
  CHECK(h->loadProject(bundle));
  h.settle();
  h->closeDialogs();
  restartViaHistory(h);
  CHECK_EQ(nameOf(h, pid), std::string("Ветвь 1"));
}

// Смена мира: снимки прежнего мира (и несохранённые, и с диска) не переходят к открытому, новому или к экрану
// запуска; «Не сохранять» отбрасывает снимки несохранённых ходов.
TEST(app_fix_turn_history_reset_on_world_change) {
  Harness h("fix_turn_reset");
  h.demo();
  Id pid = landProvince(h->world());
  std::string a = fs::join(h.root, "Мир А"), b = fs::join(h.root, "Мир Б");
  CHECK(h->saveTo(a));
  endTurn(h);
  endTurn(h);
  CHECK(h->save());
  CHECK_EQ(h->snapshots().size(), size_t(3));
  // Мир Б: другой мир без ходов.
  {
    World w = map::makeDemoWorld(*h->basemap());
    Tx tx(w);
    tx.meta().name = "Мир Б";
    h->loadWorld(std::move(tx).finish(), "Мир Б");
    h.settle();
  }
  CHECK_EQ(h->snapshots().size(), size_t(1));
  CHECK(h->saveTo(b));
  CHECK_EQ(io::listSnapshots(b).size(), size_t(1));
  // Открыть А — история А; несохранённый ход и «Не сохранять» при открытии Б.
  CHECK(h->loadProject(a));
  h.settle();
  h->closeDialogs();
  CHECK_EQ(h->snapshots().size(), size_t(3));
  rename(h, pid, "Отброшенная правка");
  endTurn(h);
  CHECK(h->act("Без автосохранения", [](Tx& tx) { tx.settings().autosaveFolder = false; }));
  CHECK_EQ(h->snapshots().size(), size_t(4));
  h->openPath(b);
  h.settle();
  CHECK(h->hasDialog("choice"));
  CHECK(h.clickUi("dialog.button.0"));   // «Не сохранять»
  h.settle();
  CHECK_EQ(h->projectPath(), fs::absolute(b));
  auto snaps = h->snapshots();
  CHECK_EQ(snaps.size(), size_t(1));
  for (auto& s : snaps) CHECK_EQ(io::loadSnapshotFile(b, s.key).world.meta->name, std::string("Мир Б"));
  CHECK(h->loadProject(a));
  h.settle();
  h->closeDialogs();
  CHECK_EQ(h->store.world().turn(), 3);
  CHECK_EQ(h->snapshots().size(), size_t(3));   // хода 3 → 4 нет ни на диске, ни в памяти
  for (auto& s : io::listSnapshots(a)) CHECK(s.turn <= 3);
  // Экран запуска — истории нет.
  h->closeWorld();
  h.settle();
  CHECK(h->ui.screen == app::Screen::Start);
  CHECK(h->snapshots().empty());
}
