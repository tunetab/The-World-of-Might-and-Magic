// Аудит (tz-build-mods-turn): сохранение между файлами — «Сохранить как» поверх другого мира, архив .regnum
// (внешние изменения, восстановление после сбоя), резервные копии при автосохранении в папку мира,
// ручная правка JSON со ссылками, которые нормализация не исправляет.
// Ожидаемое поведение проверяется строго только при REGNUM_AUDIT=1; иначе расхождение печатается.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "app/editors/buildings.h"
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;

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

Id landProvince(const World& w) {
  Id pid = 0;
  w.provinces.each([&](const Province& p) {
    if (!pid && !p.sea && p.owner) pid = p.id;
  });
  return pid;
}

void endTurns(Harness& h, int n) {
  for (int i = 0; i < n; i++) {
    CHECK(h->endTurnNow());
    h->toasts().clear();
  }
}

void loadSecondWorld(Harness& h, const std::string& name) {
  World w = map::makeDemoWorld(*h->basemap());
  Tx tx(w);
  tx.meta().name = name;
  h->loadWorld(std::move(tx).finish(), "Второй мир");
  h.settle();
}

}  // namespace

// «Сохранить как» → папка с другим миром → «Заменить мир?» → «Заменить». Файлы мира заменяются, а history/
// прежнего мира оставалась. Исправлено (app/project.cpp saveImpl → io::replaceHistory): история места
// заменяется историей этого мира — регрессионная проверка.
TEST(audit_tbmt_saveas_replace_keeps_foreign_history) {
  Harness h("audit_tbmt_saveas_hist");
  h.demo();
  std::string other = fs::join(h.root, "Чужой мир");
  CHECK(h->saveTo(other));
  endTurns(h, 3);
  CHECK(h->save());
  CHECK_EQ(h->snapshots().size(), size_t(4));   // ходы 1–3 и начало текущего хода 4
  // Другой мир, ещё без файла, — «Сохранить как» в ту же папку с заменой.
  loadSecondWorld(h, "Совсем другой мир");
  CHECK_EQ(h->snapshots().size(), size_t(1));   // только начало его хода, в памяти
  CHECK(h->snapshots().front().memory);
  hl::queueDialogResult(other);
  h->saveAs();
  h.settle();
  CHECK(h->hasDialog("world.saveas"));
  CHECK(h.clickUi("saveas.choose"));
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK_EQ(h->projectPath(), fs::absolute(other));
  auto snaps = io::listSnapshots(other);
  CHECK(!snaps.empty());
  std::string foreign;
  for (auto& s : snaps) {
    io::LoadResult r = io::loadSnapshotFile(other, s.file);
    if (r.world.meta->name != h->store.world().meta->name) foreign += " " + std::to_string(s.turn) + " («" + r.world.meta->name + "»)";
  }
  CHECK_MSG(foreign.empty(), "в истории нового мира остались снимки заменённого мира, ходы:" + foreign);
  for (auto& s : h->snapshots()) CHECK(!s.memory);
}

// То же для архива: несохранённый мир записывался поверх существующего .regnum вместе с историей ПРЕЖНЕГО
// архива. Исправлено (io::saveBundleWith: история только прежнего пути этого мира) — регрессионная проверка.
TEST(audit_tbmt_save_bundle_over_other_keeps_foreign_history) {
  Harness h("audit_tbmt_bundle_hist");
  h.demo();
  std::string bundle = fs::join(h.root, "Чужой.regnum");
  CHECK(h->saveTo(bundle));
  endTurns(h, 2);
  CHECK(h->save());
  CHECK(io::listSnapshots(bundle).size() >= size_t(4));   // начало и конец ходов 1–2, начало хода 3
  loadSecondWorld(h, "Новый мир поверх архива");
  CHECK(h->saveTo(bundle));
  auto snaps = io::listSnapshots(bundle);
  CHECK_EQ(snaps.size(), size_t(1));   // только начало текущего хода нового мира
  for (auto& s : snaps) CHECK_EQ(io::loadSnapshotFile(bundle, s.file).world.meta->name, std::string("Новый мир поверх архива"));
}

// Внешние изменения: для мира-архива .regnum проверка была отключена, чужая правка архива молча затиралась
// следующим Ctrl+S. Исправлено (app/project.cpp freshChanges: отпечаток архива — размер, время, CRC) —
// регрессионная проверка: при возврате фокуса — вопрос, Ctrl+S — выбор, «Отмена» не трогает файл.
TEST(audit_tbmt_bundle_external_change_ignored) {
  Harness h("audit_tbmt_bundle_ext");
  h.demo();
  std::string bundle = fs::join(h.root, "Мир.regnum");
  CHECK(h->saveTo(bundle));
  Id pid = landProvince(h->world());
  {
    World w = h->store.world();
    Tx tx(w);
    tx.province(pid).name = "Правка агента";
    hl::advance(2);
    io::saveBundle(bundle, std::move(tx).finish(), bundle);
  }
  hl::setFocus(false);
  hl::setFocus(true);
  h.settle();
  CHECK_MSG(h->hasDialog("external"), "архив .regnum изменён другой программой — редактор не спросил, что оставить");
  h->closeDialogs();
  // Пользователь правит другое и сохраняет — редактор спрашивает, правка агента не пропадает.
  CHECK(h->act("Правка", [&](Tx& tx) { tx.meta().notes = "заметка"; }));
  CHECK(!h->save());
  h.settle();
  CHECK(h->hasDialog("external"));
  CHECK(h.clickUi("external.cancel"));
  h.settle();
  CHECK(!h->hasDialog("external"));
  CHECK(h->dirty());
  io::LoadResult r = io::load(bundle);
  CHECK_MSG(r.world.province(pid)->name == "Правка агента", "Ctrl+S затёр внешнюю правку архива без вопроса");
  // «Сохранить с заменой» — осознанная запись поверх; после неё архив снова отслеживается без вопросов.
  h.key(Key::S, ctrl());
  h.settle();
  CHECK(h->hasDialog("external"));
  CHECK(h.clickUi("external.overwrite"));
  h.settle();
  CHECK(!h->dirty());
  r = io::load(bundle);
  CHECK(r.world.province(pid)->name != "Правка агента");
  CHECK_EQ(r.world.meta->notes, std::string("заметка"));
  hl::setFocus(true);
  h.settle();
  CHECK(!h->hasDialog("external"));
}

// Восстановление после сбоя для архива: автосохранение предлагалось, только если оно новее файла (mtime), а
// завершение хода переписывало архив (снимок внутрь). Исправлено: снимки ходов пишутся только при сохранении,
// а «новее» сравнивается со временем последнего сохранения мира (meta.updatedAt) — регрессионная проверка.
TEST(audit_tbmt_bundle_recovery_hidden_after_end_turn) {
  Harness h("audit_tbmt_bundle_recovery");
  h.demo();
  CHECK(h->act("Без автосохранения в файл", [](Tx& tx) { tx.settings().autosaveFolder = false; }));
  std::string bundle = fs::join(h.root, "Мир.regnum");
  CHECK(h->saveTo(bundle));
  std::this_thread::sleep_for(std::chrono::milliseconds(2600));   // автосохранение заметно новее файла
  Id pid = landProvince(h->world());
  CHECK(h->act("Несохранённая работа", [&](Tx& tx) { tx.province(pid).name = "Несохранённое имя"; }));
  h->autosaveNow();
  h->waitBackground();
  h.frames(3);
  auto inRecovery = [&]() {
    h->impl().recoveryDirty = true;
    app::detail::refreshStartData(h.a());
    for (auto& info : h->impl().recovery)
      if (!info.project.empty() && fs::absolute(info.project) == fs::absolute(bundle)) return true;
    return false;
  };
  CHECK(inRecovery());   // до хода — предлагается
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(h->endTurnNow());
  h->toasts().clear();
  // «Сбой» до следующего автосохранения: проверяем то, что увидит пользователь при следующем запуске.
  CHECK_MSG(inRecovery(), "после завершения хода автосохранение с несохранённой работой больше не предлагается к восстановлению");
  World unsaved = h->store.world();
  CHECK(h->loadProject(bundle));
  h.settle();
  CHECK_MSG(h->hasDialog("choice"), "повторное открытие архива не предложило восстановить несохранённую работу");
  CHECK(h->world().province(pid)->name != "Несохранённое имя");
  // «Восстановить» возвращает несохранённую работу.
  CHECK(h.clickUi("dialog.button.1"));
  h.settle();
  CHECK_EQ(h->world().province(pid)->name, std::string("Несохранённое имя"));
  CHECK(h->dirty());
}

// Резервные копии: хранятся последние пять версий файла (io::kKeepBackups), а автосохранение в папку мира
// вытесняло копию версии, сохранённой вручную. Исправлено: версии, записанные автосохранением, копируются в
// свою очередь .regnum-backup/auto/ — регрессионная проверка.
TEST(audit_tbmt_backups_flushed_by_folder_autosave) {
  Harness h("audit_tbmt_backups");
  h.demo();
  CHECK(h->store.world().settings->autosaveFolder);   // значение по умолчанию
  std::string dir = fs::join(h.root, "Мир");
  CHECK(h->saveTo(dir));
  Id pid = landProvince(h->world());
  CHECK(h->act("Ручная версия", [&](Tx& tx) { tx.province(pid).name = "Версия, сохранённая вручную"; }));
  h.key(Key::S, ctrl());
  CHECK(!h->dirty());
  const std::string manual = fs::readFile(fs::join(dir, "data/provinces.json")).value_or("");
  CHECK(!manual.empty());
  for (int i = 0; i < 6; i++) {
    CHECK(h->act("Правка", [&](Tx& tx) { tx.province(pid).contentment = double(i * 7 - 20); }));
    hl::advance(61);
    h.frames(3);
    h->waitBackground();
    h.frames(2);
  }
  CHECK(!h->dirty());   // автосохранение записало папку мира
  bool kept = false;
  for (auto& e : fs::list(fs::join(dir, ".regnum-backup/data")))
    if (!e.dir && fs::readFile(e.path).value_or("") == manual) kept = true;
  CHECK_MSG(kept, "через 6 минут правки резервной копии версии, сохранённой вручную (Ctrl+S), уже нет — копии вытеснены автосохранениями");
  // Копии версий автосохранения — в своей очереди, не больше kKeepBackups.
  CHECK(!fs::list(fs::join(dir, ".regnum-backup/auto/data")).empty());
  CHECK(fs::list(fs::join(dir, ".regnum-backup/auto/data")).size() <= size_t(io::kKeepBackups));
}

// Ручная правка JSON: «уникальная постройка» с владельцем-гильдией нормализацией не исправляется (FORMAT.md 7.6
// и core/io_normalize.cpp buildings(): проверяется только существование фракции). Такая постройка не видна
// ни в общем дереве, ни в дереве гильдии («Уникальные постройки бывают только у государств»), её нельзя
// построить, изменить или удалить из интерфейса.
TEST(audit_tbmt_hand_edit_guild_owned_building) {
  Harness h("audit_tbmt_hand_guild_building");
  h.demo();
  std::string dir = fs::join(h.root, "Мир");
  CHECK(h->saveTo(dir));
  Id guild = 0, bid = 0;
  h->world().factions.each([&](const Faction& f) {
    if (!guild && f.isGuild()) guild = f.id;
  });
  h->world().buildings.each([&](const Building& b) {
    if (!bid && b.owner == 0) bid = b.id;
  });
  CHECK(guild && bid);
  {
    // «Агент» правит data/buildings.json: owner постройки — гильдия.
    World w = h->store.world();
    Tx tx(w);
    tx.building(bid).owner = guild;
    World w2 = std::move(tx).finish();
    CHECK(fs::writeFileAtomic(fs::join(dir, "data/buildings.json"), io::fileText(w2, "data/buildings.json")));
  }
  io::LoadResult r = io::load(dir);
  bool warned = false;
  for (auto& wn : r.warnings) warned = warned || wn.text().find("data/buildings.json") != std::string::npos;
  CHECK(h->loadProject(dir));
  h.settle();
  // Нормализация: уникальная постройка гильдии стала общей и снова доступна провинциям.
  CHECK_EQ(h->world().building(bid)->owner, Id(0));
  bool offered = false;
  h->world().provinces.each([&](const Province& p) {
    if (p.sea || !p.owner) return;
    for (auto& o : rules::buildOptions(h->world(), p.id)) offered = offered || o.building == bid;
  });
  CHECK(offered);
  app::openBuildingTree(h.a(), guild, bid);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("audit_tbmt_guild_building_tree"));
  CHECK_MSG(warned, "постройка с владельцем-гильдией загружена без предупреждения и исправления — она недоступна в интерфейсе");
}

// «Сохранить как» в новую папку и первое сохранение мира со снимками в памяти перезаписывали время снимков.
// Исправлено: снимок хранит время создания, сохранение и «Сохранить как» (папка и архив) его переносят —
// регрессионная проверка.
TEST(audit_tbmt_saveas_resets_snapshot_times) {
  Harness h("audit_tbmt_snap_times");
  h.demo();
  endTurns(h, 1);   // снимки в памяти (мир ещё без файла)
  auto mem = h->snapshots();
  CHECK(!mem.empty());
  std::string at1 = mem.front().at;
  CHECK(!at1.empty());
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  std::string dir = fs::join(h.root, "Мир");
  CHECK(h->saveTo(dir));
  auto row1 = h->snapshots();
  CHECK(!row1.empty() && !row1.front().memory);
  CHECK_MSG(!row1.empty() && row1.front().at == at1, "время снимка хода после первого сохранения: " + (row1.empty() ? "" : row1.front().at) + " вместо " + at1);
  auto s1 = io::listSnapshots(dir);
  CHECK(!s1.empty());
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  for (std::string target : {fs::join(h.root, "Копия"), fs::join(h.root, "Копия.regnum")}) {
    CHECK(h->saveTo(target));
    auto s2 = io::listSnapshots(target);
    CHECK_EQ(s2.size(), s1.size());
    for (size_t i = 0; i < s1.size() && i < s2.size(); i++) {
      CHECK_EQ(s2[i].seq, s1[i].seq);
      CHECK_MSG(s2[i].at == s1[i].at, "«Сохранить как» изменило время снимка хода: " + s2[i].at + " вместо " + s1[i].at);
    }
  }
}

// Ручная правка data/techs.json: «studied: true» у технологии, чьи предшествующие не изучены. Нормализация
// (core/io_normalize.cpp techs()) проверяет существование и циклы, но не согласованность изученности с ТЗ 1.b.v
// («для изучения одной потребуется изучение нескольких других»): мир открывается без предупреждения.
TEST(audit_tbmt_hand_edit_studied_without_prereqs) {
  Harness h("audit_tbmt_hand_tech");
  h.demo();
  std::string dir = fs::join(h.root, "Мир");
  CHECK(h->saveTo(dir));
  Id tid = 0;
  h->world().techs.each([&](const Tech& t) {
    if (tid || t.prereqs.empty()) return;
    for (Id p : t.prereqs)
      if (const Tech* pt = h->world().tech(p); pt && !pt->studied) tid = t.id;
  });
  CHECK(tid != 0);
  {
    World w = h->store.world();
    Tx tx(w);
    tx.tech(tid).studied = true;
    tx.tech(tid).research = false;
    World w2 = std::move(tx).finish();
    CHECK(fs::writeFileAtomic(fs::join(dir, "data/techs.json"), io::fileText(w2, "data/techs.json")));
  }
  io::LoadResult r = io::load(dir);
  const Tech* t = r.world.tech(tid);
  CHECK(t != nullptr);
  bool consistent = true;
  for (Id p : t->prereqs)
    if (const Tech* pt = r.world.tech(p); t->studied && pt && !pt->studied) consistent = false;
  CHECK_MSG(consistent, "технология «" + t->name + "» загружена изученной при неизученных предшествующих");
  CHECK_MSG(!r.warnings.empty(), "технология «" + t->name + "» исправлена без предупреждения");
}

// Архив .regnum: каждое завершение хода пересобирает ВЕСЬ архив (мир + все снимки истории, core/io.cpp
// writeSnapshot), и каждое автосохранение в файл (по умолчанию раз в минуту) — тоже (saveBundle читает и
// переписывает всю историю). Время и объём записи растут линейно с числом ходов. Сценарий меряет рост.
TEST(audit_tbmt_bundle_rewrite_grows_with_history) {
  Harness h("audit_tbmt_bundle_growth");
  h.demo();
  std::string bundle = fs::join(h.root, "Мир.regnum");
  CHECK(h->saveTo(bundle));
  std::vector<double> ms;
  std::vector<i64> size;
  for (int i = 0; i < 12; i++) {
    auto t0 = std::chrono::steady_clock::now();
    CHECK(h->endTurnNow());
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    h->toasts().clear();
    size.push_back(i64(fs::fileSize(bundle).value_or(0)));
  }
  std::fprintf(stderr, "AUDIT INFO: размер архива после ходов: %lld → %lld байт; ход 1: %.0f мс, ход 12: %.0f мс\n", (long long)size.front(),
               (long long)size.back(), ms.front(), ms.back());
  i64 per = (size.back() - size.front()) / 11;
  std::fprintf(stderr, "AUDIT INFO: прирост на ход %lld байт — 200 ходов ≈ %.1f МБ перезаписи на каждый ход и автосохранение\n", (long long)per,
               double(per) * 200 / 1e6);
}
