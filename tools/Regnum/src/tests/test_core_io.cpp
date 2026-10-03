// Тесты core/io: папка проекта, архив, снимки ходов, резервные копии, внешние изменения,
// автосохранение, недавние проекты, ошибки чтения.
#include "base/json.h"
#include "codec/zip.h"
#include "codec/zlib.h"
#include "tests/test_core_io_util.h"

using namespace rg;
using namespace rg::iotest;

namespace {

std::string read(const std::string& path) { return fs::readFile(path).value_or(std::string()); }
void write(const std::string& path, const std::string& text) { CHECK(fs::writeFileAtomic(path, text)); }

std::string errorOf(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const UserError& e) {
    return e.what();
  }
  return {};
}

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

}  // namespace

TEST(io_rich_world_is_normal) {
  World w = richWorld();
  World n = w;
  io::Warnings warns;
  u32 mask = io::normalize(n, warns);
  CHECK_MSG(warns.empty(), warningsText(warns));
  CHECK_EQ(mask, 0u);
  CHECK_EQ(World::diff(w, n), 0u);
}

TEST(io_roundtrip_folder) {
  World w = richWorld();
  std::string dir = tempDir("roundtrip");
  io::FileState st;
  auto res = io::save(dir, w, TB_ALL, &st);
  CHECK_EQ(res.written.size(), io::projectFiles().size());
  CHECK(res.backups.empty());
  CHECK_EQ(st.folder, dir);
  CHECK_EQ(st.files.size(), io::projectFiles().size());

  io::LoadResult r = io::load(dir);
  CHECK_MSG(r.warnings.empty(), warningsText(r.warnings));
  CHECK_EQ(r.fixedTables, 0u);
  CHECK(!r.bundle);
  std::string d = diffWorld(w, r.world);
  CHECK_MSG(d.empty(), d);
  CHECK_EQ(io::toJson(w), io::toJson(r.world));
  // Двоичные данные — побайтно.
  CHECK(r.world.character(1)->portrait == jpegLikeBytes());
  CHECK_EQ(r.world.faction(1)->flag.png.substr(0, 8), std::string("\x89PNG\r\n\x1a\n"));
  CHECK_EQ(r.world.meta->name, std::string("Мир Ардена 🐉 «тест»"));

  // Повторное сохранение того же мира ничего не меняет на диске.
  auto again = io::save(dir, r.world, TB_ALL, &r.files);
  CHECK(again.written.empty());
  CHECK_EQ(again.unchanged.size(), io::projectFiles().size());
  CHECK(!fs::exists(fs::join(dir, io::kBackupDir)));

  // world.json по пути файла тоже открывается.
  io::LoadResult r2 = io::load(fs::join(dir, "world.json"));
  CHECK(diffWorld(w, r2.world).empty());
}

TEST(io_file_format) {
  World w = richWorld();
  std::string dir = tempDir("format");
  io::save(dir, w, TB_ALL);
  for (auto& f : io::projectFiles()) {
    std::string text = read(fs::join(dir, f.path));
    CHECK_MSG(!text.empty() && text.back() == '\n', f.path);
    CHECK_MSG(text.find('\r') == std::string::npos, f.path);
    CHECK_MSG(json::tryParse(text).has_value(), f.path);
    CHECK_EQ(text, io::fileText(w, f.path));
  }
  std::string world = read(fs::join(dir, "world.json"));
  CHECK(world.find("\"format\": \"regnum-world\"") != std::string::npos);
  CHECK(world.find("\"version\": 1") != std::string::npos);
  CHECK(world.find("\"fillOpacity\": 0.3,") != std::string::npos);
  CHECK(world.find("\"occupiedIncome\": \"occupier\"") != std::string::npos);
  // Ключи отсортированы: format < meta < settings < version.
  CHECK(world.find("\"format\"") < world.find("\"meta\""));
  CHECK(world.find("\"meta\"") < world.find("\"settings\""));
  CHECK(world.find("\"settings\"") < world.find("\"version\""));

  std::string geo = read(fs::join(dir, "data/geo.json"));
  CHECK(geo.find("\n    [1,100.25,200.5],\n") != std::string::npos);
  CHECK(geo.find("\"pts\":[150.5,190.25,250,210]") != std::string::npos);
  CHECK(geo.find("\"kind\":\"coast\"") != std::string::npos);
  CHECK(geo.find("\"pl\":\"p1\",\"pr\":\"p2\"") != std::string::npos);

  std::string rel = read(fs::join(dir, "data/relations.json"));
  CHECK(rel.find("\"f1|f2\": {\"s\":\"alliance\",\"v\":50.5}") != std::string::npos);
  CHECK(rel.find("\"f1|f3\": {\"s\":\"war\",\"v\":-25}") != std::string::npos);
  CHECK(rel.find("f1|f2") < rel.find("f1|f3"));

  std::string prov = read(fs::join(dir, "data/provinces.json"));
  CHECK(prov.find("\"owner\": \"f1\"") != std::string::npos);
  CHECK(prov.find("\"lord\": null") != std::string::npos);
  CHECK(prov.find("\"modifiers\": [\"m1\", \"m2\"]") != std::string::npos);
  CHECK(prov.find("\"size\": \"large\"") != std::string::npos);
  CHECK(prov.find("\"row\": \"u1\"") != std::string::npos);
  CHECK(prov.find("p1") < prov.find("p2"));

  std::string fac = read(fs::join(dir, "data/factions.json"));
  CHECK(fac.find("\"res\": {\n") != std::string::npos);
  CHECK(fac.find("\"rs1\": 15000.5") != std::string::npos);
  CHECK(fac.find("\"pattern\": \"saltire\"") != std::string::npos);
  CHECK(fac.find("\"type\": \"heavy_inf\"") != std::string::npos);

  std::string mods = read(fs::join(dir, "data/modifiers.json"));
  CHECK(mods.find("\"tradePct\": 10") != std::string::npos);
  CHECK(mods.find("\"incomePct\"") == std::string::npos);  // незаданные эффекты не пишутся

  std::string cat = read(fs::join(dir, "data/catalogs.json"));
  CHECK(cat.find("\"color\": \"#0a141e80\"") != std::string::npos);  // цвет с прозрачностью
  CHECK(cat.find("\"id\": \"rs7\"") != std::string::npos);
}

TEST(io_coordinates_rounded) {
  World base = richWorld();
  Tx tx(base);
  tx.node(1).p = {100.123456, 200.987654};
  tx.army(1).pos = {1.004, 2.006};
  World w = std::move(tx).finish();
  std::string dir = tempDir("round");
  io::save(dir, w, TB_ALL);
  auto r = io::load(dir);
  CHECK_EQ(r.world.nodes.get(1)->p.x, 100.12);
  CHECK_EQ(r.world.nodes.get(1)->p.y, 200.99);
  CHECK_EQ(r.world.army(1)->pos.x, 1.0);
  CHECK_EQ(r.world.army(1)->pos.y, 2.01);
}

TEST(io_roundtrip_bundle) {
  World w = richWorld();
  std::string dir = tempDir("bundle");
  std::string path = fs::join(dir, "Арден.regnum");
  io::saveBundle(path, w);
  CHECK(io::isProject(path));
  CHECK(io::isBundlePath(path));
  auto r = io::loadBundle(path);
  CHECK(r.bundle);
  CHECK_MSG(r.warnings.empty(), warningsText(r.warnings));
  std::string d = diffWorld(w, r.world);
  CHECK_MSG(d.empty(), d);
  auto r2 = io::load(path);
  CHECK(diffWorld(w, r2.world).empty());

  // Архив — обычный zip той же раскладки, содержимое файлов совпадает с папкой.
  codec::ZipReader zr;
  CHECK(zr.openFile(path));
  for (auto& f : io::projectFiles()) {
    auto text = zr.read(f.path);
    CHECK_MSG(text.has_value(), f.path);
    CHECK_EQ(*text, io::fileText(w, f.path));
  }
  CHECK_EQ(zr.entries()[0].name, std::string("world.json"));
}

TEST(io_bundle_nested_folder) {
  // Архив, созданный упаковкой папки целиком: «Мир/world.json».
  World w = richWorld();
  codec::ZipWriter zw;
  for (auto& f : io::projectFiles()) CHECK(zw.add(std::string("Мой мир/") + f.path, io::fileText(w, f.path)));
  std::string dir = tempDir("nested");
  std::string path = fs::join(dir, "nested.regnum");
  write(path, zw.finish());
  auto r = io::load(path);
  CHECK(diffWorld(w, r.world).empty());
  CHECK(r.warnings.empty());

  // Не мир — понятная ошибка.
  codec::ZipWriter z2;
  CHECK(z2.add("readme.txt", std::string_view("x")));
  std::string bad = fs::join(dir, "bad.regnum");
  write(bad, z2.finish());
  std::string e = errorOf([&] { io::load(bad); });
  CHECK_MSG(e.find("нет world.json") != std::string::npos, e);
  write(bad, "не архив");
  e = errorOf([&] { io::load(bad); });
  CHECK_MSG(e.find("Не удалось открыть архив") != std::string::npos, e);
}

TEST(io_snapshot_document) {
  World w = richWorld();
  std::string js = io::toJson(w);
  auto r = io::fromJson(js);
  CHECK_MSG(r.warnings.empty(), warningsText(r.warnings));
  CHECK(diffWorld(w, r.world).empty());
  auto gz = io::pack(w);
  CHECK(gz.size() > 18 && gz[0] == 0x1f && gz[1] == 0x8b);
  auto r2 = io::unpack(gz);
  CHECK(diffWorld(w, r2.world).empty());
  std::string pretty = io::toJson(w, 2);
  CHECK(io::fromJson(pretty).warnings.empty());
  gz[gz.size() / 2] ^= 0x5a;
  std::string e = errorOf([&] { io::unpack(gz); });
  CHECK_MSG(e.find("повреждённые сжатые данные") != std::string::npos, e);
  e = errorOf([&] { io::fromJson("{\"format\": \"other\"}"); });
  CHECK_MSG(e.find("не снимок") != std::string::npos, e);
  e = errorOf([&] { io::fromJson("{\"format\": \"regnum-snapshot\", \"version\": 3, \"files\": {}}"); });
  CHECK_MSG(e.find("более новой версией") != std::string::npos, e);
}

TEST(io_dirty_save_writes_only_changed) {
  std::string dir = tempDir("dirty");
  Store store;
  store.replace(richWorld(), "load");
  io::FileState st;
  io::save(dir, store.world(), TB_ALL, &st);
  store.markSaved();
  std::string factionsBefore = read(fs::join(dir, "data/factions.json"));

  store.transact("rename", [](Tx& tx) { tx.province(1).name = "Новый Арден"; });
  CHECK_EQ(store.dirtyTables(), u32(TB_PROVINCES));
  auto res = io::save(dir, store.world(), store.dirtyTables(), &st);
  CHECK_EQ(res.written.size(), size_t(1));
  CHECK(contains(res.written, "data/provinces.json"));
  CHECK_EQ(res.backups.size(), size_t(1));
  CHECK_EQ(res.backups[0], std::string(".regnum-backup/data/provinces.000001.json"));
  CHECK_EQ(read(fs::join(dir, "data/factions.json")), factionsBefore);
  CHECK(read(fs::join(dir, "data/provinces.json")).find("Новый Арден") != std::string::npos);
  CHECK(fs::isFile(fs::join(dir, ".regnum-backup/.gitignore")));
  store.markSaved();

  // Новая сущность меняет и счётчики (world.json).
  store.transact("add", [](Tx& tx) { Character c; c.name = "Новый"; tx.add(c); });
  CHECK_EQ(store.dirtyTables(), u32(TB_CHARACTERS | TB_META));
  res = io::save(dir, store.world(), store.dirtyTables(), &st);
  CHECK_EQ(res.written.size(), size_t(2));
  CHECK_EQ(res.written.back(), std::string("world.json"));  // world.json — последним
  auto r = io::load(dir);
  CHECK(diffWorld(store.world(), r.world).empty());

  // Удалённый файл записывается при любом сохранении.
  CHECK(fs::remove(fs::join(dir, "data/deals.json")));
  res = io::save(dir, store.world(), 0, &st);
  CHECK_EQ(res.written.size(), size_t(1));
  CHECK(contains(res.written, "data/deals.json"));
}

TEST(io_backups_rotation) {
  std::string dir = tempDir("backups");
  World w = richWorld();
  io::save(dir, w, TB_ALL);
  for (int i = 1; i <= 8; i++) {
    Tx tx(w);
    tx.province(1).contentment = i;
    w = std::move(tx).finish();
    io::save(dir, w, TB_PROVINCES);
  }
  std::vector<std::string> names;
  for (auto& e : fs::list(fs::join(dir, ".regnum-backup/data"))) names.push_back(e.name);
  std::sort(names.begin(), names.end());
  CHECK_EQ(names.size(), size_t(io::kKeepBackups));
  CHECK_EQ(names.front(), std::string("provinces.000004.json"));
  CHECK_EQ(names.back(), std::string("provinces.000008.json"));
  // Последняя копия — предпоследняя версия файла (довольство 7).
  auto v = json::parse(read(fs::join(dir, ".regnum-backup/data/provinces.000008.json")));
  CHECK_EQ(v.arr("provinces")[0].num("contentment"), 7.0);
  // Без резервных копий.
  io::SaveOptions so;
  so.backups = false;
  Tx tx(w);
  tx.province(1).contentment = 50;
  io::save(dir, std::move(tx).finish(), TB_PROVINCES, nullptr, so);
  CHECK_EQ(fs::list(fs::join(dir, ".regnum-backup/data")).size(), size_t(io::kKeepBackups));
  // world.json хранится в корне папки копий.
  Tx t2(w);
  t2.meta().notes = "изменено";
  io::save(dir, std::move(t2).finish(), TB_META);
  CHECK(fs::isFile(fs::join(dir, ".regnum-backup/world.000001.json")));
}

TEST(io_load_errors) {
  std::string dir = tempDir("errors");
  std::string e = errorOf([&] { io::load(dir); });
  CHECK_MSG(e.find("нет файла world.json") != std::string::npos, e);
  e = errorOf([&] { io::load(fs::join(dir, "нет такой папки")); });
  CHECK_MSG(e.find("не найден") != std::string::npos, e);

  World w = richWorld();
  io::save(dir, w, TB_ALL);
  std::string prov = fs::join(dir, "data/provinces.json");
  std::string good = read(prov);
  // Повреждённый JSON: файл, строка и столбец.
  std::string broken = good;
  const std::string key = "\"name\": \"Арден\"";
  broken.insert(broken.find(key) + key.size(), ",,");
  write(prov, broken);
  e = errorOf([&] { io::load(dir); });
  CHECK_MSG(e.find("data/provinces.json") != std::string::npos && e.find("строка") != std::string::npos &&
                e.find("столбец") != std::string::npos, e);
  write(prov, good.substr(0, good.size() / 2));
  e = errorOf([&] { io::load(dir); });
  CHECK_MSG(e.find("data/provinces.json") != std::string::npos, e);
  write(prov, good);

  // Более новая версия формата — отказ с понятным сообщением.
  std::string worldPath = fs::join(dir, "world.json");
  std::string wj = read(worldPath);
  write(worldPath, replaceAll(wj, "\"version\": 1", "\"version\": 2"));
  e = errorOf([&] { io::load(dir); });
  CHECK_MSG(e.find("более новой версией Regnum") != std::string::npos && e.find("Обновите") != std::string::npos, e);
  // Младшая версия того же формата (1.4) читается.
  write(worldPath, replaceAll(wj, "\"version\": 1", "\"version\": 1.4"));
  CHECK(io::load(dir).warnings.empty());
  // Чужой формат.
  write(worldPath, replaceAll(wj, "regnum-world", "atlas-map"));
  e = errorOf([&] { io::load(dir); });
  CHECK_MSG(e.find("не мир Regnum") != std::string::npos, e);
  write(worldPath, "[1, 2]");
  e = errorOf([&] { io::load(dir); });
  CHECK_MSG(e.find("ожидался объект") != std::string::npos, e);
  // BOM в начале файла допустим.
  write(worldPath, "\xEF\xBB\xBF" + wj);
  CHECK(io::load(dir).warnings.empty());
  write(worldPath, wj);

  // Отсутствующий файл таблицы — пустая таблица с предупреждением.
  CHECK(fs::remove(fs::join(dir, "data/routes.json")));
  auto r = io::load(dir);
  CHECK(r.world.routes.empty());
  CHECK(hasWarning(r.warnings, "data/routes.json", "", "не найден"));
  CHECK(r.fixedTables & TB_ROUTES);
}

TEST(io_external_changes) {
  std::string dir = tempDir("external");
  World w = richWorld();
  io::save(dir, w, TB_ALL);
  auto r = io::load(dir);
  CHECK(io::externalChanges(r.files).empty());

  // Перезапись тем же содержимым (новое время) — не изменение.
  std::string prov = fs::join(dir, "data/provinces.json");
  std::string text = read(prov);
  write(prov, text);
  CHECK(io::externalChanges(r.files).empty());

  // Изменение содержимого.
  write(prov, replaceAll(text, "Арденбург", "Арденбург-на-Море"));
  auto ch = io::externalChanges(r.files);
  CHECK_EQ(ch.size(), size_t(1));
  CHECK_EQ(ch[0].file, std::string("data/provinces.json"));
  CHECK(ch[0].kind == io::ChangeKind::Modified);
  CHECK_EQ(ch[0].tables, u32(TB_PROVINCES));
  CHECK_EQ(io::externalChanges(r.files).size(), size_t(1));  // остаётся, пока мир не перечитан

  // Сохранение поверх чужих изменений — отказ; с заменой — запись.
  Tx tx(r.world);
  tx.province(1).notes = "моё";
  World mine = std::move(tx).finish();
  std::string e = errorOf([&] { io::save(dir, mine, TB_PROVINCES, &r.files); });
  CHECK_MSG(e.find("изменены другой программой") != std::string::npos && e.find("data/provinces.json") != std::string::npos, e);
  CHECK(read(prov).find("Арденбург-на-Море") != std::string::npos);
  // Изменение файла другой таблицы не мешает сохранению этой.
  io::SaveOptions so;
  so.overwriteExternal = true;
  io::save(dir, mine, TB_PROVINCES, &r.files, so);
  CHECK(io::externalChanges(r.files).empty());
  CHECK(read(prov).find("Арденбург-на-Море") == std::string::npos);

  // Удаление и появление файлов.
  CHECK(fs::remove(fs::join(dir, "data/log.json")));
  ch = io::externalChanges(r.files);
  CHECK_EQ(ch.size(), size_t(1));
  CHECK(ch[0].kind == io::ChangeKind::Removed);
  auto r2 = io::load(dir);
  CHECK(!r2.files.files["data/log.json"].exists);
  write(fs::join(dir, "data/log.json"), "{\"log\": []}\n");
  ch = io::externalChanges(r2.files);
  CHECK_EQ(ch.size(), size_t(1));
  CHECK(ch[0].kind == io::ChangeKind::Added);
  CHECK_EQ(ch[0].tables, u32(TB_LOG));

  // Архив не отслеживается.
  std::string b = fs::join(dir, "x.regnum");
  io::saveBundle(b, w);
  auto rb = io::load(b);
  CHECK(rb.files.folder.empty());
  CHECK(io::externalChanges(rb.files).empty());
}

TEST(io_snapshots_folder) {
  std::string dir = tempDir("snapshots");
  World w = richWorld();
  io::save(dir, w, TB_ALL);
  CHECK(io::listSnapshots(dir).empty());
  std::vector<World> turns;
  for (int t = 1; t <= 3; t++) {
    Tx tx(w);
    tx.meta().turn = t;
    tx.province(1).contentment = t * 10;
    World x = std::move(tx).finish();
    turns.push_back(x);
    auto info = io::writeSnapshot(dir, x, "Конец хода " + std::to_string(t));
    CHECK_EQ(info.turn, t);
    CHECK(fs::isFile(fs::join(dir, info.file)));
  }
  CHECK(fs::isFile(fs::join(dir, "history/turn-0002.json.gz")));
  auto list = io::listSnapshots(dir);
  CHECK_EQ(list.size(), size_t(3));
  CHECK_EQ(list[0].turn, 1);
  CHECK_EQ(list[2].turn, 3);
  CHECK_EQ(list[1].label, std::string("Конец хода 2"));
  CHECK_EQ(list[1].name, w.meta->name);
  CHECK_EQ(list[1].file, std::string("history/turn-0002.json.gz"));
  CHECK(list[1].size > 0 && !list[1].at.empty());

  auto s2 = io::loadSnapshot(dir, 2);
  CHECK_MSG(s2.warnings.empty(), warningsText(s2.warnings));
  CHECK(diffWorld(turns[1], s2.world).empty());
  CHECK_EQ(s2.world.province(1)->contentment, 20.0);
  CHECK_THROWS(io::loadSnapshot(dir, 9));

  // Тот же ход — замена.
  Tx tx(turns[1]);
  tx.province(1).contentment = 99;
  io::writeSnapshot(dir, std::move(tx).finish(), "Повтор");
  list = io::listSnapshots(dir);
  CHECK_EQ(list.size(), size_t(3));
  CHECK_EQ(list[1].label, std::string("Повтор"));
  CHECK_EQ(io::loadSnapshot(dir, 2).world.province(1)->contentment, 99.0);

  CHECK(io::removeSnapshot(dir, 1));
  CHECK(!io::removeSnapshot(dir, 1));
  list = io::listSnapshots(dir);
  CHECK_EQ(list.size(), size_t(2));
  CHECK_EQ(list[0].turn, 2);
  CHECK(!fs::exists(fs::join(dir, "history/turn-0001.json.gz")));

  // Без index.json история восстанавливается по именам файлов.
  CHECK(fs::remove(fs::join(dir, "history/index.json")));
  list = io::listSnapshots(dir);
  CHECK_EQ(list.size(), size_t(2));
  CHECK_EQ(list[1].turn, 3);
  CHECK(list[1].label.empty());
  CHECK_EQ(io::loadSnapshot(dir, 3).world.province(1)->contentment, 30.0);

  // Повреждённый снимок — понятная ошибка.
  write(fs::join(dir, "history/turn-0003.json.gz"), "мусор");
  std::string e = errorOf([&] { io::loadSnapshot(dir, 3); });
  CHECK_MSG(e.find("turn-0003") != std::string::npos, e);
}

TEST(io_snapshots_bundle_and_convert) {
  std::string dir = tempDir("convert");
  std::string folder = fs::join(dir, "folder");
  World w = richWorld();
  io::save(folder, w, TB_ALL);
  io::writeSnapshot(folder, w, "Седьмой");
  Tx tx(w);
  tx.meta().turn = 8;
  World w8 = std::move(tx).finish();
  io::writeSnapshot(folder, w8, "Восьмой");

  // Папка -> архив: мир и история.
  std::string bundle = fs::join(dir, "мир.regnum");
  io::Warnings warns;
  io::convert(folder, bundle, &warns);
  CHECK(warns.empty());
  auto list = io::listSnapshots(bundle);
  CHECK_EQ(list.size(), size_t(2));
  CHECK_EQ(list[1].label, std::string("Восьмой"));
  CHECK(diffWorld(w8, io::loadSnapshot(bundle, 8).world).empty());
  CHECK(diffWorld(w, io::load(bundle).world).empty());
  CHECK_THROWS(io::convert(folder, bundle));  // уже существует

  // Снимок в архив: архив пересобирается, мир в нём не меняется.
  Tx t9(w);
  t9.meta().turn = 9;
  io::writeSnapshot(bundle, std::move(t9).finish(), "Девятый");
  CHECK_EQ(io::listSnapshots(bundle).size(), size_t(3));
  CHECK(diffWorld(w, io::load(bundle).world).empty());
  CHECK(io::removeSnapshot(bundle, 7));
  CHECK_EQ(io::listSnapshots(bundle).size(), size_t(2));

  // Сохранение архива сохраняет его историю.
  Tx tn(w);
  tn.province(1).name = "Переименовано";
  World wn = std::move(tn).finish();
  io::saveBundle(bundle, wn);
  CHECK_EQ(io::listSnapshots(bundle).size(), size_t(2));
  CHECK_EQ(io::load(bundle).world.province(1)->name, std::string("Переименовано"));

  // Архив -> папка.
  std::string back = fs::join(dir, "back");
  io::convert(bundle, back);
  CHECK(diffWorld(wn, io::load(back).world).empty());
  list = io::listSnapshots(back);
  CHECK_EQ(list.size(), size_t(2));
  CHECK_EQ(list[0].turn, 8);
  CHECK(diffWorld(w8, io::loadSnapshot(back, 8).world).empty());
  std::string e = errorOf([&] { io::convert(bundle, back); });
  CHECK_MSG(e.find("не пуста") != std::string::npos, e);
  io::convert(bundle, back, nullptr, true);
  e = errorOf([&] { io::convert(back, back); });
  CHECK_MSG(e.find("совпадают") != std::string::npos, e);

  // Архив из папки с другой историей.
  std::string b2 = fs::join(dir, "b2.regnum");
  io::saveBundle(b2, wn, folder);
  CHECK_EQ(io::listSnapshots(b2).size(), size_t(2));
}

TEST(io_autosave) {
  std::string dir = tempDir("autosave");
  CHECK(io::listAutosaves(dir).empty());
  std::string err;
  CHECK(!io::loadAutosave(dir, &err).has_value());
  CHECK(!err.empty());

  World a = richWorld();
  Tx tx(a);
  tx.meta().name = "Второй мир";
  World b = std::move(tx).finish();
  std::string projA = fs::join(dir, "projects/A");
  auto ia = io::writeAutosave(a, projA, dir);
  CHECK(fs::isFile(ia.file));
  CHECK_EQ(ia.project, fs::absolute(projA));
  CHECK_EQ(ia.turn, 7);
  auto ib = io::writeAutosave(b, "", dir);  // новый несохранённый мир
  CHECK(ib.project.empty());

  auto list = io::listAutosaves(dir);
  CHECK_EQ(list.size(), size_t(2));
  auto latest = io::loadAutosave(dir);
  CHECK(latest.has_value());
  auto fa = io::loadAutosaveFor(projA, dir);
  CHECK(fa.has_value());
  CHECK(diffWorld(a, fa->world).empty());
  CHECK_EQ(fa->info.name, a.meta->name);
  auto fb = io::loadAutosaveFor("", dir);
  CHECK(fb.has_value());
  CHECK_EQ(fb->world.meta->name, std::string("Второй мир"));

  // Перезапись слота того же проекта.
  io::writeAutosave(b, projA, dir);
  CHECK_EQ(io::listAutosaves(dir).size(), size_t(2));
  CHECK_EQ(io::loadAutosaveFor(projA, dir)->world.meta->name, std::string("Второй мир"));

  // Повреждённое автосохранение — nullopt с причиной.
  write(ia.file, "мусор");
  err.clear();
  CHECK(!io::loadAutosaveFor(projA, dir, &err).has_value());
  CHECK(!err.empty());

  io::clearAutosave(projA, dir);
  io::clearAutosave("", dir);
  CHECK(io::listAutosaves(dir).empty());
}

TEST(io_recent_projects) {
  std::string dir = tempDir("recent");
  CHECK(io::recentProjects(dir).empty());
  std::string p1 = fs::join(dir, "worlds/one");
  io::save(p1, richWorld(), TB_ALL);
  io::addRecent(p1, "Первый", dir);
  for (int i = 0; i < 15; i++) io::addRecent(fs::join(dir, "missing/" + std::to_string(i)), "Нет " + std::to_string(i), dir);
  auto list = io::recentProjects(dir);
  CHECK_EQ(list.size(), size_t(io::kMaxRecent));
  CHECK_EQ(list[0].name, std::string("Нет 14"));
  CHECK(!list[0].exists);
  io::addRecent(p1, "Первый снова", dir);
  list = io::recentProjects(dir);
  CHECK_EQ(list.size(), size_t(io::kMaxRecent));
  CHECK_EQ(list[0].name, std::string("Первый снова"));
  CHECK(list[0].exists);
  CHECK_EQ(list[0].path, fs::absolute(p1));
  io::removeRecent(p1, dir);
  list = io::recentProjects(dir);
  CHECK_EQ(list.size(), size_t(io::kMaxRecent - 1));
  CHECK(list[0].name != "Первый снова");
  // Повреждённый список — пустой, без ошибки.
  write(fs::join(dir, "recent.json"), "{не json");
  CHECK(io::recentProjects(dir).empty());
}

TEST(io_project_files) {
  auto& files = io::projectFiles();
  CHECK_EQ(files.size(), size_t(14));
  CHECK_EQ(std::string(files.back().path), std::string("world.json"));
  u32 all = 0;
  for (auto& f : files) {
    CHECK_MSG((all & f.tables) == 0, f.path);
    all |= f.tables;
  }
  CHECK_EQ(all, u32(TB_ALL));
  CHECK_EQ(io::tablesOfFile("data/geo.json"), u32(TB_GEO));
  CHECK_EQ(io::tablesOfFile("world.json"), u32(TB_META | TB_SETTINGS));
  CHECK_EQ(io::tablesOfFile("data/unknown.json"), 0u);
  CHECK(io::isBundlePath("a/b/Мир.REGNUM"));
  CHECK(!io::isBundlePath("a/b/world.json"));
  io::Warning w{"data/provinces.json", "p1.owner", "нет фракции f9"};
  CHECK_EQ(w.text(), std::string("data/provinces.json: p1.owner: нет фракции f9"));
}

// Изменение другой программой сразу после чтения: тот же размер и то же время файла (грубая точность
// времени файловой системы) — изменение всё равно обнаруживается по содержимому.
TEST(io_external_change_same_size_and_time) {
  std::string dir = tempDir("racy");
  io::save(dir, richWorld(), TB_ALL);
  auto r = io::load(dir);
  std::string prov = fs::join(dir, "data/provinces.json");
  std::error_code ec;
  auto before = std::filesystem::last_write_time(fs::path(prov), ec);
  CHECK(!ec);
  std::string text = read(prov);
  std::string edited = replaceAll(text, "Арденбург", "Арбенбург");  // та же длина в байтах
  CHECK_EQ(edited.size(), text.size());
  CHECK(edited != text);
  write(prov, edited);
  std::filesystem::last_write_time(fs::path(prov), before, ec);
  CHECK(!ec);
  CHECK_EQ(fs::mtime(prov).value_or(0), r.files.files["data/provinces.json"].mtime);
  auto ch = io::externalChanges(r.files);
  CHECK_EQ(ch.size(), size_t(1));
  CHECK(!ch.empty() && ch[0].file == "data/provinces.json" && ch[0].kind == io::ChangeKind::Modified);
  // Содержимое возвращено — изменений нет.
  write(prov, text);
  std::filesystem::last_write_time(fs::path(prov), before, ec);
  CHECK(io::externalChanges(r.files).empty());
}

TEST(io_save_errors) {
  std::string dir = tempDir("save_errors");
  std::string file = fs::join(dir, "файл.txt");
  write(file, "x");
  World w = richWorld();
  std::string e = errorOf([&] { io::save(file, w, TB_ALL); });
  CHECK_MSG(e.find("файл, а не папка") != std::string::npos, e);
  e = errorOf([&] { io::save("", w, TB_ALL); });
  CHECK_MSG(e.find("Не указана папка") != std::string::npos, e);
  e = errorOf([&] { io::saveBundle(dir, w); });
  CHECK_MSG(e.find("папка, а не файл") != std::string::npos, e);
  // Мир в папке, путь к которой задан с другим написанием, — то же состояние файлов.
  io::FileState st;
  io::save(fs::join(dir, "world"), w, TB_ALL, &st);
  size_t stamps = st.files.size();
  auto res = io::save(fs::join(dir, "x/../world"), w, TB_PROVINCES, &st);
  CHECK_EQ(st.files.size(), stamps);
  CHECK(res.written.empty());
  CHECK(io::externalChanges(st).empty());
}
