// Тесты файловой системы: кириллица и пробелы в путях, атомарная запись, списки, переименование, копирование,
// удаление, длинные пути Windows, системные папки, работа со строками путей, одновременная запись.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "base/fs.h"
#include "base/jobs.h"
#include "tests/test.h"

using namespace rg;

namespace {

std::string sandbox(const char* name) {
  std::string d = test::outDir() + "/fs тест/" + name;
  fs::removeAll(d);
  CHECK(fs::makeDirs(d));
  return d;
}

i64 nowMs() {
  return i64(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

}  // namespace

TEST(base_fs_cyrillic_write_read) {
  std::string d = sandbox("Кириллица и пробелы");
  std::string f = d + "/Папка с пробелами/файл ё «1».txt";
  std::string text = "Арден, Вингард — ёлка\n\x00\xFF двоичные байты";
  text.push_back('\0');
  CHECK(!fs::exists(f));
  std::string err;
  CHECK_MSG(fs::writeFileAtomic(f, text, &err), err);
  CHECK(fs::exists(f));
  CHECK(fs::isFile(f));
  CHECK(!fs::isDir(f));
  CHECK(fs::isDir(d + "/Папка с пробелами"));
  auto back = fs::readFile(f, &err);
  CHECK_MSG(back.has_value(), err);
  CHECK(*back == text);
  CHECK_EQ(fs::fileSize(f).value_or(0), u64(text.size()));
  i64 mt = *fs::mtime(f);
  CHECK_MSG(std::llabs(mt - nowMs()) < 120000, strf("mtime %lld now %lld", (long long)mt, (long long)nowMs()));
  // перезапись — без временных файлов в папке
  std::vector<u8> bytes = {1, 2, 3};
  CHECK(fs::writeFileAtomic(f, std::span<const u8>(bytes)));
  CHECK(*fs::readFile(f) == std::string("\x01\x02\x03"));
  auto entries = fs::list(d + "/Папка с пробелами");
  CHECK_EQ(entries.size(), size_t(1));
  CHECK_EQ(entries[0].name, std::string("файл ё «1».txt"));
  CHECK_EQ(entries[0].size, u64(3));
  CHECK(!entries[0].dir);
  CHECK(entries[0].mtime > 0);
  // пустой файл
  CHECK(fs::writeFileAtomic(d + "/пусто", std::string_view()));
  CHECK(fs::readFile(d + "/пусто")->empty());
  // отсутствующий файл
  CHECK(!fs::readFile(d + "/нет.txt", &err));
  CHECK(err.find("нет.txt") != std::string::npos);
  CHECK(!fs::fileSize(d + "/нет.txt"));
  CHECK(!fs::mtime(d + "/нет.txt"));
  CHECK(!fs::exists(""));
}

TEST(base_fs_list_rename_copy_remove) {
  std::string d = sandbox("операции");
  CHECK(fs::writeFileAtomic(d + "/б.txt", std::string("b")));
  CHECK(fs::writeFileAtomic(d + "/а.txt", std::string("a")));
  CHECK(fs::writeFileAtomic(d + "/Z.txt", std::string("z")));
  CHECK(fs::makeDirs(d + "/папка/вложенная"));
  CHECK(fs::writeFileAtomic(d + "/папка/вложенная/глубоко.json", std::string("{}")));
  CHECK(fs::writeFileAtomic(d + "/папка/x.bin", std::string("x")));
  auto l = fs::list(d);
  std::vector<std::string> names;
  for (auto& e : l) names.push_back(e.name);
  CHECK((names == std::vector<std::string>{"Z.txt", "а.txt", "б.txt", "папка"}));
  CHECK(l[3].dir);
  CHECK_EQ(l[1].path, d + "/а.txt");
  auto tree = fs::listTree(d);
  CHECK((tree == std::vector<std::string>{"Z.txt", "а.txt", "б.txt", "папка/x.bin", "папка/вложенная/глубоко.json"}));
  CHECK(fs::list(d + "/нет").empty());
  // переименование с заменой существующего
  std::string err;
  CHECK_MSG(fs::rename(d + "/а.txt", d + "/б.txt", &err), err);
  CHECK(!fs::exists(d + "/а.txt"));
  CHECK(*fs::readFile(d + "/б.txt") == "a");
  CHECK(fs::rename(d + "/б.txt", d + "/новая папка/в.txt"));
  CHECK(*fs::readFile(d + "/новая папка/в.txt") == "a");
  CHECK(fs::rename(d + "/папка", d + "/папка2"));
  CHECK(fs::isDir(d + "/папка2/вложенная"));
  CHECK(!fs::rename(d + "/нет", d + "/нет2", &err));
  // копирование с перезаписью
  CHECK(fs::copyFile(d + "/Z.txt", d + "/копии/Z.txt"));
  CHECK(fs::writeFileAtomic(d + "/Z.txt", std::string("zz")));
  CHECK(fs::copyFile(d + "/Z.txt", d + "/копии/Z.txt"));
  CHECK(*fs::readFile(d + "/копии/Z.txt") == "zz");
  CHECK(!fs::copyFile(d + "/нет", d + "/x", &err));
  // удаление
  CHECK(fs::remove(d + "/Z.txt"));
  CHECK(!fs::exists(d + "/Z.txt"));
  CHECK(fs::remove(d + "/Z.txt"));  // отсутствие — успех
  CHECK(!fs::remove(d + "/папка2", &err));  // непустая папка
  CHECK(fs::removeAll(d + "/папка2"));
  CHECK(!fs::exists(d + "/папка2"));
  CHECK(fs::removeAll(d + "/папка2"));
}

TEST(base_fs_path_strings) {
  CHECK_EQ(fs::join("a", "b"), std::string("a/b"));
  CHECK_EQ(fs::join("a/", "b"), std::string("a/b"));
  CHECK_EQ(fs::join("", "b"), std::string("b"));
  CHECK_EQ(fs::join("a", ""), std::string("a"));
  CHECK_EQ(fs::join("мир", "провинции.json"), std::string("мир/провинции.json"));
  CHECK_EQ(fs::join("a", "/abs"), std::string("/abs"));
  CHECK_EQ(fs::parent("a/b/c.txt"), std::string("a/b"));
  CHECK_EQ(fs::parent("c.txt"), std::string(""));
  CHECK_EQ(fs::filename("a/b/c.txt"), std::string("c.txt"));
  CHECK_EQ(fs::stem("a/b/c.txt"), std::string("c"));
  CHECK_EQ(fs::ext("a/b/c.txt"), std::string(".txt"));
  CHECK_EQ(fs::ext("a/b"), std::string(""));
  CHECK_EQ(fs::stem("архив.tar.gz"), std::string("архив.tar"));
  CHECK_EQ(fs::ext("Архив.REGNUM"), std::string(".REGNUM"));
  CHECK_EQ(fs::filename("папка/Карта мира.png"), std::string("Карта мира.png"));
  std::string abs = fs::absolute("x/../y");
  CHECK(fs::isAbsolute(abs));
  CHECK(endsWith(abs, "/y"));
  CHECK(abs.find('\\') == std::string::npos);
  CHECK_EQ(fs::fromPath(fs::path("Кириллица/файл")), std::string("Кириллица/файл"));
  CHECK(!fs::isAbsolute("отн/путь"));
  // некорректный UTF-8 не приводит к исключениям
  std::string bad = "папка/\xFF\xFE\xC0.txt";
  CHECK(!fs::exists(bad));
  CHECK(!fs::readFile(bad).has_value());
  CHECK(!fs::fromPath(fs::path(bad)).empty());
  CHECK(fs::list(bad).empty());
}

TEST(base_fs_long_paths) {
  // Путь длиннее 260 символов (MAX_PATH Windows) с кириллицей.
  std::string d = sandbox("длинные");
  std::string p = d;
  for (int i = 0; i < 8; i++) p += "/длинная папка номер " + std::to_string(i) + " с пробелами";
  std::string f = p + "/файл с очень длинным именем для проверки предела путей.json";
  CHECK(fs::absolute(f).size() > 300);
  std::string err;
  CHECK_MSG(fs::makeDirs(p, &err), err);
  CHECK(fs::isDir(p));
  CHECK_MSG(fs::writeFileAtomic(f, std::string("{\"длинный\": true}"), &err), err);
  CHECK(fs::exists(f));
  auto r = fs::readFile(f, &err);
  CHECK_MSG(r && *r == "{\"длинный\": true}", err);
  CHECK(fs::fileSize(f).has_value());
  CHECK(fs::mtime(f).has_value());
  auto l = fs::list(p);
  CHECK(l.size() == 1 && l[0].name == "файл с очень длинным именем для проверки предела путей.json");
  CHECK_EQ(fs::listTree(d).size(), size_t(1));
  CHECK(fs::copyFile(f, f + ".копия"));
  CHECK(fs::rename(f + ".копия", f + ".2"));
  CHECK(fs::exists(f + ".2"));
  CHECK(fs::remove(f + ".2"));
  CHECK(fs::removeAll(d));
  CHECK(!fs::exists(d));
}

TEST(base_fs_system_dirs) {
  std::string exe = fs::exeDir(), home = fs::homeDir(), docs = fs::documentsDir(), data = fs::userDataDir(), tmp = fs::tempDir();
  CHECK_MSG(fs::isDir(exe), exe);
  CHECK_MSG(fs::isDir(home), home);
  CHECK_MSG(fs::isDir(docs), docs);
  CHECK_MSG(fs::isDir(data), data);
  CHECK_MSG(fs::isDir(tmp), tmp);
  CHECK(endsWith(data, "Regnum") || endsWith(data, "regnum"));
  for (auto* s : {&exe, &home, &docs, &data, &tmp}) CHECK(s->find('\\') == std::string::npos);
  // в папке исполняемого файла есть хотя бы он сам
  bool anyFile = false;
  for (auto& e : fs::list(exe))
    if (!e.dir && e.size > 0) anyFile = true;
  CHECK(anyFile);
}

TEST(base_fs_concurrent_atomic_writes) {
  // Одновременная запись одного файла из многих потоков: итог — одно из целых содержимых, без мусора.
  std::string d = sandbox("одновременно");
  std::string f = d + "/общий.json";
  std::atomic<int> failures{0};
  jobs::parallelFor(64, [&](size_t i) {
    std::string body(size_t(1000 + i * 37), char('a' + i % 26));
    if (!fs::writeFileAtomic(f, body)) failures++;
  }, 1);
  CHECK_EQ(failures.load(), 0);
  auto r = fs::readFile(f);
  CHECK(r.has_value() && !r->empty());
  CHECK(std::all_of(r->begin(), r->end(), [&](char c) { return c == (*r)[0]; }));
  size_t i = size_t((*r)[0] - 'a');
  CHECK((r->size() - 1000) % 37 == 0 && (r->size() - 1000) / 37 % 26 == i);
  CHECK_EQ(fs::list(d).size(), size_t(1));  // временные файлы не остались
}
