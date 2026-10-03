// Аудит (tz-build-mods-turn), продолжение: снимки экранов строительства, модификаторов, истории ходов и
// сценарии, найденные после прерывания первого прохода. Строгие проверки — только при REGNUM_AUDIT=1.
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "app/dialogs/turn_ui.h"
#include "app/editors/buildings.h"
#include "tests/test_app_trees_util.h"

using namespace rg;
using namespace rg::apptest;
using namespace rg::apptest::trees;

namespace {

[[maybe_unused]] bool auditStrict() {
  const char* v = std::getenv("REGNUM_AUDIT");
  return v && *v == '1';
}

#define AUDIT_EXPECT(cond, msg)                                                                         \
  do {                                                                                                  \
    if (!(cond)) {                                                                                      \
      if (auditStrict()) ::rg::test::fail(__FILE__, __LINE__, std::string("AUDIT: ") + (msg));          \
      else std::fprintf(stderr, "AUDIT DEFECT (%s:%d): %s\n", __FILE__, __LINE__, std::string(msg).c_str()); \
    }                                                                                                   \
  } while (0)

struct Pick {
  Id pid = 0, owner = 0, building = 0;
};
// Провинция государства со свободным слотом и общая постройка без требований с ненулевой ценой.
Pick pickFree(const World& w) {
  Pick r;
  auto c = rules::calc(w);
  w.provinces.each([&](const Province& p) {
    if (r.pid || p.sea || !p.owner) return;
    const Faction* f = w.faction(p.owner);
    if (!f || !f->isState()) return;
    const rules::ProvinceCalc* pc = c->province(p.id);
    if (!pc || pc->slots <= int(p.buildings.size())) return;
    w.buildings.each([&](const Building& b) {
      if (r.pid || b.owner || !b.requires_.empty() || b.levels.empty() || b.levels[0].cost.empty()) return;
      for (const ProvBuilding& pb : p.buildings)
        if (pb.building == b.id) return;
      r = Pick{p.id, p.owner, b.id};
    });
  });
  return r;
}

void setStock(app::App& a, Id faction, double v) {
  CHECK(a.act("Запасы", [&](Tx& tx) {
    Faction& f = tx.faction(faction);
    f.res[kGold] = v;
    for (const CatalogItem& c : tx.w().catalogs->resources) f.res[c.id] = v;
  }));
}

double stockSum(const World& w, Id faction) {
  const Faction* f = w.faction(faction);
  double s = f->treasury();
  for (const CatalogItem& c : w.catalogs->resources)
    if (c.id != kGold) s += f->stock(c.id);
  return s;
}

Id otherState(const World& w, Id not1) {
  Id r = 0;
  w.factions.each([&](const Faction& f) {
    if (!r && f.isState() && f.id != not1) r = f.id;
  });
  return r;
}

}  // namespace

// Снимки экранов зоны аудита — для визуальной проверки (сами по себе не проверяют ничего).
TEST(audit_tbmt_more_shots) {
  HideTestRegs hide;
  Harness h("audit_tbmt_more_shots", 1440, 900);
  h.demo();
  Pick pk = pickFree(h->world());
  CHECK(pk.pid != 0);
  setStock(h.a(), pk.owner, 5);   // почти пусто: часть построек недоступна
  h->ui.tabOf[app::SelType::Province] = "province.buildings";
  h->select(app::SelType::Province, pk.pid);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("audit_tbmt_more_buildings_tab"));
  CHECK(h.clickUi("prov.build"));
  h.settle();
  CHECK(h->hasDialog("build.picker"));
  h.dropToasts();
  h.settle();
  CHECK(h.shot("audit_tbmt_more_picker_poor"));
  h->closeDialogs();
  h.settle();
  // Модификаторы: окно с выбранным модификатором дипломатии (если есть).
  Id dip = 0, any = 0;
  h->world().modifiers.each([&](const Modifier& m) {
    if (!any) any = m.id;
    if (!dip && m.has(Fx::DiplomacyPerTurn)) dip = m.id;
  });
  h->openEditor("modifiers", dip ? dip : any);
  h.dropToasts();
  h.settle();
  h.settle();
  CHECK(h.shot("audit_tbmt_more_modifiers"));
  h->closeEditor();
  h.settle();
  // Общее дерево построек.
  app::openBuildingTree(h.a(), 0);
  h.dropToasts();
  h.settle();
  h.settle();
  CHECK(h.shot("audit_tbmt_more_common_tree"));
  h->closeEditor();
  h.settle();
  // Ход: подтверждение, отчёт, история.
  h->endTurn();
  h.dropToasts();
  h.settle();
  CHECK(h->hasDialog("turn.confirm"));
  CHECK(h.shot("audit_tbmt_more_turn_confirm"));
  h->closeDialogs();
  h.settle();
  CHECK(h->endTurnNow());
  CHECK(h->endTurnNow());
  h->toasts().clear();
  h->openDialog("turn.report");
  h.dropToasts();
  h.settle();
  CHECK(h.shot("audit_tbmt_more_turn_report"));
  h->closeDialogs();
  h->openDialog("turn.history");
  h.dropToasts();
  h.settle();
  h.settle();
  CHECK(h.shot("audit_tbmt_more_history"));
  h->closeDialogs();
  h.settle();
  CHECK(h->viewTurn(1));
  h.dropToasts();
  h.settle();
  h.settle();
  CHECK(h.shot("audit_tbmt_more_view_past"));
}

// ТЗ 1.f.ii: ресурсы «вычитаются из списка ресурсов государства при её возведении». Провинция перешла к другому
// государству во время стройки: стройка продолжается для нового владельца, а «Отменить» возвращает стоимость
// НОВОМУ владельцу (rules/build.cpp cancelBuilding: refund → p.owner на момент отмены), хотя платил прежний.
TEST(audit_tbmt_more_refund_to_new_owner) {
  Harness h("audit_tbmt_more_refund_owner");
  h.demo();
  Pick pk = pickFree(h->world());
  CHECK(pk.pid != 0);
  Id other = otherState(h->world(), pk.owner);
  CHECK(other != 0);
  setStock(h.a(), pk.owner, 10000);
  setStock(h.a(), other, 0);
  const double a0 = stockSum(h->world(), pk.owner);
  CHECK(h->act("Построить", [&](Tx& tx) { rules::startBuilding(tx, pk.pid, pk.building); }));
  const double paid = a0 - stockSum(h->world(), pk.owner);
  CHECK(paid > 0);
  CHECK(h->act("Смена владельца", [&](Tx& tx) { rules::setProvinceOwner(tx, pk.pid, other); }));
  const double b0 = stockSum(h->world(), other);
  CHECK(h->act("Отменить", [&](Tx& tx) { rules::cancelBuilding(tx, pk.pid, pk.building); }));
  const double gotB = stockSum(h->world(), other) - b0;
  CHECK_NEAR(stockSum(h->world(), pk.owner), a0, 1e-6);  // плательщику (прежнему владельцу) — ровно уплаченное
  CHECK_MSG(gotB < 1e-6, "новый владелец получил возврат " + fmtNum(gotB, 2) + " за стройку, которую оплатил прежний (" +
                                fmtNum(paid, 2) + "); прежний владелец ничего не получил");
}

// Возврат считается по ТЕКУЩЕЙ цене уровня в дереве построек, а не по уплаченной: цену уровня поправили в дереве
// во время стройки (обычная работа ведущего) — «Отменить» возвращает новую цену (rules/build.cpp cancelBuilding,
// rules/entities.cpp dropBuildingEverywhere). Из 150 золота получается сколько угодно.
TEST(audit_tbmt_more_refund_uses_edited_cost) {
  Harness h("audit_tbmt_more_refund_cost");
  h.demo();
  Pick pk = pickFree(h->world());
  CHECK(pk.pid != 0);
  setStock(h.a(), pk.owner, 10000);
  const double a0 = stockSum(h->world(), pk.owner);
  CHECK(h->act("Построить", [&](Tx& tx) { rules::startBuilding(tx, pk.pid, pk.building); }));
  const double paid = a0 - stockSum(h->world(), pk.owner);
  CHECK(paid > 0);
  CHECK(h->act("Цена уровня", [&](Tx& tx) {
    for (auto& [res, v] : tx.building(pk.building).levels[0].cost) v *= 100;
  }));
  CHECK(h->act("Отменить", [&](Tx& tx) { rules::cancelBuilding(tx, pk.pid, pk.building); }));
  const double net = stockSum(h->world(), pk.owner) - a0;
  CHECK_MSG(std::fabs(net) < 1e-6, "уплачено " + fmtNum(paid, 2) + ", после отмены государство в плюсе на " + fmtNum(net, 2) +
                                         " — возврат взят из изменённой цены уровня");
}

// «Начать ход заново» (снимок текущего хода есть после Ctrl+Z завершения хода): диалог обещает «мир станет таким,
// каким был в начале хода», а загружался снимок КОНЦА хода. Исправлено: отдельный снимок начала хода (при открытии
// мира и после расчёта) — регрессионная проверка.
TEST(audit_tbmt_more_restart_is_not_turn_start) {
  Harness h("audit_tbmt_more_restart_start");
  h.demo();
  Pick pk = pickFree(h->world());
  CHECK(pk.pid != 0);
  const std::string orig = h->world().province(pk.pid)->name;
  const int t0 = h->store.world().turn();
  CHECK(h->act("Имя", [&](Tx& tx) { tx.province(pk.pid).name = "Правка в ходе"; }));
  CHECK(h->endTurnNow());
  h->toasts().clear();
  undo(h);   // завершение хода отменено — снова ход t0, снимок хода t0 остаётся
  CHECK_EQ(h->store.world().turn(), t0);
  h->openDialog("turn.history");
  h.dropToasts();
  h.settle();
  CHECK(h.clickUi("history.rollback." + std::to_string(t0)));
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  const std::string name = h->store.world().province(pk.pid)->name;
  CHECK_MSG(name == orig, "после «Начать ход " + std::to_string(t0) + " заново» провинция называется «" + name + "», в начале хода было «" + orig + "»");
  CHECK_EQ(h->store.world().turn(), t0);
}

// Сохранённый мир: завершение хода сразу писало снимок в history/ на диске, хотя сам мир не сохранён; после
// «Не сохранять» история возвращала отброшенные правки. Исправлено: снимки ходов после последнего сохранения
// живут в памяти и пишутся только при сохранении — регрессионная проверка.
TEST(audit_tbmt_more_discarded_turn_stays_in_history) {
  Harness h("audit_tbmt_more_discard");
  h.demo();
  std::string dir = fs::join(h.root, "Мир");
  CHECK(h->saveTo(dir));
  Pick pk = pickFree(h->world());
  CHECK(pk.pid != 0);
  CHECK(h->act("Имя", [&](Tx& tx) { tx.province(pk.pid).name = "Отброшенная правка"; }));
  CHECK(h->endTurnNow());
  h->toasts().clear();
  CHECK(h->act("Без автосохранения", [](Tx& tx) { tx.settings().autosaveFolder = false; }));
  h->closeWorld();
  h.settle();
  CHECK(h->hasDialog("choice"));
  CHECK(h.clickUi("dialog.button.0"));   // «Не сохранять»
  h.settle();
  CHECK(h->loadProject(dir));
  h.settle();
  h->closeDialogs();
  CHECK_EQ(h->store.world().turn(), 1);
  CHECK(h->world().province(pk.pid)->name != "Отброшенная правка");
  // В истории — только начало текущего хода 1 (записано при первом сохранении), снимков отброшенного хода нет.
  std::string turns;
  for (auto& s : h->snapshots()) turns += " " + std::to_string(s.turn) + "/" + s.kind;
  CHECK_MSG(h->snapshots().size() == 1 && h->snapshots().front().turn == 1 && h->snapshots().front().kind == io::kSnapStart,
            "после «Не сохранять» в истории мира остались снимки отброшенных ходов:" + turns);
  for (auto& s : io::listSnapshots(dir)) CHECK_MSG(s.kind == io::kSnapStart && s.turn == 1, "на диске снимок отброшенного хода: " + s.file);
  CHECK(app::turnui::rollbackToTurn(h.a(), 1));
  h.settle();
  CHECK_MSG(h->world().province(pk.pid)->name != "Отброшенная правка",
            "«Начать ход 1 заново» вернул правку, от которой пользователь отказался («Не сохранять»)");
}

// Внешние изменения проверялись только при возврате фокуса окну; Ctrl+S при изменённых файлах отказывал одним
// уведомлением без действий. Исправлено: Ctrl+S открывает выбор «Перечитать» / «Сохранить с заменой» / «Отмена» —
// регрессионная проверка (все три пути).
TEST(audit_tbmt_more_conflict_on_save_dead_end) {
  Harness h("audit_tbmt_more_conflict");
  h.demo();
  std::string dir = fs::join(h.root, "Мир");
  CHECK(h->saveTo(dir));
  Pick pk = pickFree(h->world());
  CHECK(pk.pid != 0);
  {
    World w = h->store.world();
    Tx tx(w);
    tx.province(pk.pid).name = "Правка агента";
    World w2 = std::move(tx).finish();
    hl::advance(2);
    CHECK(fs::writeFileAtomic(fs::join(dir, "data/provinces.json"), io::fileText(w2, "data/provinces.json")));
  }
  CHECK(h->act("Правка пользователя", [&](Tx& tx) { tx.province(pk.pid).contentment = 42; }));
  h->toasts().clear();
  h.key(Key::S, ctrl());
  h.settle();
  CHECK(h->dirty());   // не сохранено
  bool action = false;
  std::string text;
  for (auto& t : h->toasts()) {
    text += t.text + " | ";
    action = action || !t.actionLabel.empty();
  }
  CHECK(h.shot("audit_tbmt_more_conflict"));
  CHECK_MSG(h->hasDialog("external"), "Ctrl+S при внешнем изменении: только уведомление без выбора («" + text +
                                          "») — ни «Перечитать», ни «Сохранить с заменой»");
  (void)action;
  CHECK(h->uiRect("external.reload") && h->uiRect("external.overwrite") && h->uiRect("external.cancel"));
  // «Отмена» — ничего не записано.
  CHECK(h.clickUi("external.cancel"));
  h.settle();
  CHECK(!h->hasDialog("external"));
  CHECK(h->dirty());
  CHECK_EQ(io::load(dir).world.province(pk.pid)->name, std::string("Правка агента"));
  // «Сохранить с заменой» — версия редактора записана поверх.
  h.key(Key::S, ctrl());
  h.settle();
  CHECK(h.clickUi("external.overwrite"));
  h.settle();
  CHECK(!h->dirty());
  io::LoadResult r = io::load(dir);
  CHECK(r.world.province(pk.pid)->name != "Правка агента");
  CHECK_NEAR(r.world.province(pk.pid)->contentment, 42, 1e-9);
  // «Перечитать» — открыта версия с диска.
  {
    World w = h->store.world();
    Tx tx(w);
    tx.province(pk.pid).name = "Вторая правка агента";
    World w2 = std::move(tx).finish();
    hl::advance(2);
    CHECK(fs::writeFileAtomic(fs::join(dir, "data/provinces.json"), io::fileText(w2, "data/provinces.json")));
  }
  CHECK(h->act("Ещё правка", [&](Tx& tx) { tx.province(pk.pid).contentment = 7; }));
  h.key(Key::S, ctrl());
  h.settle();
  CHECK(h->hasDialog("external"));
  CHECK(h.clickUi("external.reload"));
  h.settle();
  CHECK(!h->hasDialog("external"));
  CHECK(!h->dirty());
  CHECK_EQ(h->world().province(pk.pid)->name, std::string("Вторая правка агента"));
}

// Архив .regnum: каждое завершение хода и каждое автосохранение (по умолчанию раз в минуту) пересобирает архив
// целиком вместе со всей историей — в основном потоке. Замер времени Ctrl+S после 40 ходов против папки.
TEST(audit_tbmt_more_bundle_save_time) {
  Harness h("audit_tbmt_more_bundle_time");
  h.demo();
  std::string bundle = fs::join(h.root, "Мир.regnum");
  CHECK(h->saveTo(bundle));
  auto timeSave = [&](int i) {
    CHECK(h->act("Правка", [&](Tx& tx) { tx.meta().notes = "заметка " + std::to_string(i); }));
    auto t0 = std::chrono::steady_clock::now();
    CHECK(h->save());
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  };
  const double first = timeSave(0);
  double lastTurn = 0;
  for (int i = 0; i < 40; i++) {
    auto t0 = std::chrono::steady_clock::now();
    CHECK(h->endTurnNow());
    lastTurn = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    h->toasts().clear();
  }
  const double after = timeSave(1);
  std::fprintf(stderr, "AUDIT INFO: архив: Ctrl+S %.0f мс → %.0f мс после 40 ходов; завершение 40-го хода %.0f мс; размер %.1f МБ\n", first, after,
               lastTurn, double(fs::fileSize(bundle).value_or(0)) / 1e6);
  AUDIT_EXPECT(after < first * 3 + 100, "сохранение архива после 40 ходов занимает " + fmtNum(after, 0) + " мс против " + fmtNum(first, 0) +
                                           " мс в начале — растёт с длиной истории");
}
