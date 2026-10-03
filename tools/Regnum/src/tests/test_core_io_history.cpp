// Тесты core/io — история ходов с ветвями (снимки с номерами: уникальные файлы, время создания сохраняется,
// head — ветвь сохранённого мира), замена истории при записи в чужое место, отпечаток архива .regnum
// (внешние изменения), время последнего сохранения, отдельная очередь резервных копий автосохранения.
#include <chrono>
#include <thread>

#include "base/json.h"
#include "tests/test_core_io_util.h"

using namespace rg;
using namespace rg::iotest;

namespace {

World turnWorld(const World& base, int turn, double contentment) {
  Tx tx(base);
  tx.meta().turn = turn;
  tx.province(1).contentment = contentment;
  return std::move(tx).finish();
}

io::NewSnapshot snap(const World& w, u64 seq, const char* kind, u64 parent, const std::string& at) {
  io::NewSnapshot n;
  n.info.turn = w.turn();
  n.info.seq = seq;
  n.info.kind = kind;
  n.info.parent = parent;
  n.info.at = at;
  n.info.label = std::string(kind) + " " + std::to_string(seq);
  n.info.name = w.meta->name;
  n.gz = io::pack(w, 6);
  return n;
}

}  // namespace

TEST(io_history_numbered_snapshots_folder_and_bundle) {
  std::string dir = tempDir("history_numbered");
  World w = richWorld();
  std::string folder = fs::join(dir, "Мир");
  io::save(folder, w, TB_ALL);
  World s1 = turnWorld(w, 1, 10), e1 = turnWorld(w, 1, 11), s2 = turnWorld(w, 2, 20), s2b = turnWorld(w, 2, 25);
  // Два снимка начала хода 2 (разные ветви) не заменяют друг друга; время создания — из сведений.
  io::addSnapshots(folder, {snap(s1, 1, io::kSnapStart, 0, "2026-01-01T10:00:00Z"), snap(e1, 2, io::kSnapEnd, 1, "2026-01-01T10:05:00Z"),
                            snap(s2, 3, io::kSnapStart, 2, "2026-01-01T10:06:00Z")},
                   3);
  io::addSnapshots(folder, {snap(s2b, 4, io::kSnapStart, 2, "2026-01-01T11:00:00Z")}, 4);
  io::History h = io::listHistory(folder);
  CHECK_EQ(h.head, u64(4));
  CHECK_EQ(h.list.size(), size_t(4));
  CHECK_EQ(h.list[0].file, std::string("history/turn-0001-000001.json.gz"));
  CHECK_EQ(h.list[1].kind, std::string(io::kSnapEnd));
  CHECK_EQ(h.list[1].parent, u64(1));
  CHECK_EQ(h.list[1].at, std::string("2026-01-01T10:05:00Z"));
  CHECK_EQ(h.list[2].seq, u64(3));
  CHECK_EQ(h.list[3].seq, u64(4));
  CHECK_NEAR(io::loadSnapshotFile(folder, h.list[2].file).world.province(1)->contentment, 20, 1e-9);
  CHECK_NEAR(io::loadSnapshotFile(folder, h.list[3].file).world.province(1)->contentment, 25, 1e-9);
  CHECK_NEAR(io::loadSnapshot(folder, 2).world.province(1)->contentment, 25, 1e-9);   // последний снимок хода
  CHECK_THROWS(io::loadSnapshotFile(folder, "history/turn-0009-000009.json.gz"));
  // Старый снимок без номера читается рядом с новыми.
  io::writeSnapshot(folder, turnWorld(w, 3, 30), "Старый");
  h = io::listHistory(folder);
  CHECK_EQ(h.list.size(), size_t(5));
  CHECK_EQ(h.head, u64(4));   // запись старым способом ветвь не теряет
  CHECK_EQ(h.list.back().seq, u64(0));
  // Архив из папки: история как есть (номера, время, ветвь), плюс новый снимок.
  std::string bundle = fs::join(dir, "Мир.regnum");
  io::saveBundleWith(bundle, w, folder, {snap(turnWorld(w, 3, 31), 5, io::kSnapEnd, 4, "2026-01-01T12:00:00Z")}, 5);
  io::History hb = io::listHistory(bundle);
  CHECK_EQ(hb.list.size(), size_t(6));
  CHECK_EQ(hb.head, u64(5));
  for (size_t i = 0; i < h.list.size(); i++) {
    auto it = std::find_if(hb.list.begin(), hb.list.end(), [&](const io::SnapshotInfo& s) { return s.file == h.list[i].file; });
    CHECK(it != hb.list.end());
    if (it != hb.list.end()) CHECK_EQ(it->at, h.list[i].at);
  }
  io::addSnapshots(bundle, {snap(turnWorld(w, 4, 40), 6, io::kSnapStart, 5, "2026-01-01T12:01:00Z")}, 6);
  CHECK_EQ(io::listHistory(bundle).list.size(), size_t(7));
  CHECK_EQ(io::listHistory(bundle).head, u64(6));
  CHECK_NEAR(io::loadSnapshotFile(bundle, "history/turn-0004-000006.json.gz").world.province(1)->contentment, 40, 1e-9);
  // Архив поверх существующего без historyFrom — без чужой истории.
  io::saveBundleWith(bundle, w, {}, {}, 0);
  CHECK(io::listHistory(bundle).list.empty());
}

TEST(io_history_replace_folder_history) {
  std::string dir = tempDir("history_replace");
  World w = richWorld();
  std::string a = fs::join(dir, "А"), b = fs::join(dir, "Б");
  io::save(a, w, TB_ALL);
  io::save(b, w, TB_ALL);
  io::addSnapshots(a, {snap(turnWorld(w, 1, 1), 1, io::kSnapStart, 0, "2026-02-01T00:00:00Z")}, 1);
  io::addSnapshots(b, {snap(turnWorld(w, 5, 5), 7, io::kSnapStart, 0, "2026-02-02T00:00:00Z"),
                       snap(turnWorld(w, 6, 6), 8, io::kSnapStart, 7, "2026-02-03T00:00:00Z")},
                   8);
  io::replaceHistory(b, a);
  io::History h = io::listHistory(b);
  CHECK_EQ(h.list.size(), size_t(1));
  CHECK_EQ(h.list[0].seq, u64(1));
  CHECK_EQ(h.list[0].at, std::string("2026-02-01T00:00:00Z"));
  CHECK_EQ(h.head, u64(1));
  CHECK(!fs::exists(fs::join(b, "history/turn-0005-000007.json.gz")));
  io::replaceHistory(b, {});
  CHECK(io::listHistory(b).list.empty());
  CHECK(!fs::exists(fs::join(b, "history")));
  io::replaceHistory(a, a);   // сам в себя — без изменений
  CHECK_EQ(io::listHistory(a).list.size(), size_t(1));
}

TEST(io_history_bundle_stamp_and_saved_time) {
  std::string dir = tempDir("history_stamp");
  World w = richWorld();
  {
    Tx tx(w);
    tx.meta().updatedAt = "2026-03-04T05:06:07Z";
    w = std::move(tx).finish();
  }
  std::string bundle = fs::join(dir, "Мир.regnum");
  io::saveBundle(bundle, w);
  CHECK_EQ(io::savedTime(bundle), std::string("2026-03-04T05:06:07Z"));
  std::string folder = fs::join(dir, "Папка");
  io::save(folder, w, TB_ALL);
  CHECK_EQ(io::savedTime(folder), std::string("2026-03-04T05:06:07Z"));
  CHECK(io::savedTime(fs::join(dir, "нет")).empty());
  io::FileStamp st = io::stampFile(bundle);
  CHECK(st.exists);
  CHECK(!io::fileChanged(bundle, st));
  // Та же запись (то же содержимое) — не изменение; другая — изменение, даже в ту же секунду.
  std::string bytes = fs::readFile(bundle).value_or("");
  CHECK(fs::writeFileAtomic(bundle, bytes));
  CHECK(!io::fileChanged(bundle, st));
  Tx tx(w);
  tx.province(1).name = "Правка другой программы";
  io::saveBundle(bundle, std::move(tx).finish());
  CHECK(io::fileChanged(bundle, st));
  io::FileStamp st2 = io::stampFile(bundle);
  CHECK(!io::fileChanged(bundle, st2));
  CHECK(fs::remove(bundle));
  CHECK(io::fileChanged(bundle, st2));
  CHECK(!io::stampFile(bundle).exists);
}

TEST(io_backups_auto_queue) {
  std::string dir = tempDir("backups_auto");
  World w = richWorld();
  io::save(dir, w, TB_ALL);
  auto edit = [&](double v) {
    Tx tx(w);
    tx.province(1).contentment = v;
    w = std::move(tx).finish();
  };
  edit(1);
  io::save(dir, w, TB_PROVINCES);   // ручное: копия исходной версии — в общей очереди
  edit(2);
  io::SaveOptions so;
  so.autoBackups = {};               // версия на диске записана вручную — её копия тоже в общей очереди
  io::save(dir, w, TB_PROVINCES, nullptr, so);
  so.autoBackups = {"data/provinces.json"};
  for (int i = 3; i <= 12; i++) {
    edit(i);
    io::save(dir, w, TB_PROVINCES, nullptr, so);   // версии автосохранения — в своей очереди
  }
  auto manual = fs::list(fs::join(dir, ".regnum-backup/data"));
  size_t files = 0;
  for (auto& e : manual) files += e.dir ? 0 : 1;
  CHECK_EQ(files, size_t(2));
  CHECK_EQ(fs::list(fs::join(dir, ".regnum-backup/auto/data")).size(), size_t(io::kKeepBackups));
  // Ручная версия (довольство 1) цела, хотя автосохранений было больше kKeepBackups.
  bool kept = false;
  for (auto& e : manual) {
    if (e.dir) continue;
    auto v = json::parse(fs::readFile(e.path).value_or("{}"));
    kept = kept || v.arr("provinces")[0].num("contentment") == 1.0;
  }
  CHECK(kept);
}
