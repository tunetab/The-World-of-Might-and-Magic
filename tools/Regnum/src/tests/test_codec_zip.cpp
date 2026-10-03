// Тесты ZIP: архивы python zipfile (stored/deflate, дескрипторы данных, UTF-8 и CP437 имена, папки, комментарий),
// неподдерживаемые варианты (ZIP64, шифрование, bzip2), запись и обратное чтение, безопасные имена.
#include <cstdio>

#include "base/fs.h"
#include "codec/zip.h"
#include "codec/zlib.h"
#include "tests/test.h"
#include "tests/test_codec_util.h"

using namespace rg;
using namespace rg::codec;

namespace {
std::string repeat(const std::string& s, int n) {
  std::string r;
  for (int i = 0; i < n; i++) r += s;
  return r;
}
}  // namespace

TEST(codec_zip_read_python_basic) {
  ZipReader z;
  std::string err;
  CHECK_MSG(z.openFile(test::dataPath("zip/basic.zip"), &err), err);
  CHECK_EQ(z.comment(), std::string("Комментарий архива"));
  CHECK_EQ(z.entries().size(), size_t(6));
  auto readme = z.read("readme.txt", &err);
  CHECK_MSG(readme.has_value(), err);
  CHECK(*readme == repeat("Regnum — архив проекта.\n", 200));
  auto nums = z.read("data/числа.json", &err);
  CHECK(nums && *nums == "{\"a\": [1, 2, 3]}");
  auto empty = z.read("empty.txt");
  CHECK(empty && empty->empty());
  auto big = z.read("big.bin", &err);
  CHECK(big && big->size() == 12000 && crc32(*big) == 0x01EB0BBCu);
  auto cafe = z.read("café.txt", &err);  // имя в CP437 без флага UTF-8
  CHECK_MSG(cafe && *cafe == "cp437", err);
  int d = z.find("dir/");
  CHECK(d >= 0 && z.entries()[size_t(d)].dir);
  const ZipEntry& e = z.entries()[size_t(z.find("readme.txt"))];
  CHECK_EQ(e.method, u16(8));
  CHECK_EQ(e.mtime, i64(1790857844));
  CHECK(z.entries()[size_t(z.find("data/числа.json"))].method == 0);
  CHECK_EQ(z.find("нет такого"), -1);
  CHECK(!z.read("нет такого", &err));
}

TEST(codec_zip_read_descriptors_and_unsupported) {
  ZipReader z;
  std::string err;
  CHECK_MSG(z.openFile(test::dataPath("zip/descriptor.zip"), &err), err);
  CHECK_EQ(z.entries().size(), size_t(4));
  for (size_t i = 0; i < z.entries().size(); i++) CHECK_MSG(z.read(i, &err).has_value(), err);
  CHECK(*z.read("readme.txt") == repeat("Regnum — архив проекта.\n", 200));

  ZipReader b;
  CHECK(b.openFile(test::dataPath("zip/bzip2.zip"), &err));
  CHECK(!b.read("bz.txt", &err));
  CHECK(err.find("метод сжатия 12") != std::string::npos);
  auto plain = b.read("plain.txt", &err);
  CHECK(plain && *plain == "ok");

  ZipReader z64;
  CHECK(!z64.openFile(test::dataPath("zip/zip64.zip"), &err));
  CHECK(err.find("ZIP64") != std::string::npos);

  ZipReader enc;
  CHECK(enc.openFile(test::dataPath("zip/encrypted.zip"), &err));
  CHECK(!enc.read(0, &err));
  CHECK(err.find("зашифрован") != std::string::npos);

  ZipReader none;
  CHECK(!none.open("это не архив, а просто текст достаточной длины", &err));
  CHECK(!none.openFile(test::outDir() + "/нет/такого.zip", &err));
}

TEST(codec_zip_write_and_read_back) {
  ZipWriter w(6);
  std::string err;
  std::string json = "{\"провинции\": [" + repeat("{\"id\": 1, \"имя\": \"Арден\"}, ", 300) + "{}]}";
  std::string noise;
  Rng rng(3);
  for (int i = 0; i < 5000; i++) noise.push_back(char(rng.next()));
  CHECK(w.addDir("мир"));
  CHECK(w.add("мир/провинции.json", json, 1790857844));
  CHECK(w.add("мир/шум.bin", noise, 1790857845));
  CHECK(w.add("пусто.txt", std::string_view()));
  CHECK(w.add("readme.md", std::string("Regnum"), 0));
  // недопустимые и повторные имена
  CHECK(!w.add("мир/провинции.json", std::string("x"), 0, &err));
  CHECK(err.find("повторное") != std::string::npos);
  for (const char* bad : {"../x", "a/../../b", "/abs", "C:/win", "a\\b", ""}) CHECK_MSG(!w.add(bad, std::string("x")), bad);
  CHECK_EQ(w.count(), size_t(5));
  std::string zip = w.finish("Архив Regnum");
  CHECK_EQ(w.count(), size_t(0));

  ZipReader r;
  CHECK_MSG(r.open(zip, &err), err);
  CHECK_EQ(r.comment(), std::string("Архив Regnum"));
  CHECK_EQ(r.entries().size(), size_t(5));
  CHECK(r.entries()[0].dir && r.entries()[0].name == "мир/");
  auto j = r.read("мир/провинции.json", &err);
  CHECK_MSG(j && *j == json, err);
  const ZipEntry& je = r.entries()[size_t(r.find("мир/провинции.json"))];
  CHECK(je.method == 8 && je.packedSize < json.size() / 10);
  CHECK_EQ(je.mtime, i64(1790857844));
  const ZipEntry& ne = r.entries()[size_t(r.find("мир/шум.bin"))];
  CHECK(ne.method == 0);              // шум не сжимается — хранится как есть
  CHECK_EQ(ne.mtime, i64(1790857844));  // точность DOS — 2 секунды
  CHECK(*r.read("мир/шум.bin") == noise);
  CHECK(r.read("пусто.txt")->empty());
  CHECK_EQ(r.entries()[size_t(r.find("readme.md"))].mtime, i64(315532800));  // 0 -> 1980-01-01

  // для внешней проверки (verify_outputs.py): архив и список «имя<TAB>crc32<TAB>размер»
  std::string dir = test::outDir() + "/codec";
  CHECK(fs::makeDirs(dir));
  CHECK(fs::writeFileAtomic(dir + "/архив тест.zip", zip));
  std::string man;
  for (auto& e : r.entries()) man += e.name + "\t" + strf("%08x", e.crc) + "\t" + std::to_string(e.size) + "\n";
  CHECK(fs::writeFileAtomic(dir + "/архив тест.zip.txt", man));
  ZipReader fromFile;
  CHECK_MSG(fromFile.openFile(dir + "/архив тест.zip", &err), err);
  CHECK(*fromFile.read("readme.md") == "Regnum");

  // данные перед архивом (самораспаковывающийся вариант)
  ZipReader sfx;
  CHECK_MSG(sfx.open(std::string(1000, 'X') + zip, &err), err);
  CHECK(*sfx.read("мир/провинции.json") == json);
}

TEST(codec_zip_safe_paths_and_corruption) {
  CHECK(safeZipPath("a/b/c.txt"));
  CHECK(safeZipPath("мир/провинции.json"));
  CHECK(safeZipPath("a..b/c"));
  CHECK(!safeZipPath("../a"));
  CHECK(!safeZipPath("a/../b"));
  CHECK(!safeZipPath("/etc/passwd"));
  CHECK(!safeZipPath("C:/Windows"));
  CHECK(!safeZipPath("a\\b"));
  CHECK(!safeZipPath(""));
  CHECK(!safeZipPath(std::string_view("a\0b", 3)));
  CHECK(!safeZipPath("\xFF\xFE"));

  ZipWriter w;
  for (int i = 0; i < 20; i++) CHECK(w.add(strf("f%02d.txt", i), repeat("строка " + std::to_string(i) + "\n", 50 + i)));
  std::string zip = w.finish();
  std::string err;
  // обрезки: открытие или чтение сообщает об ошибке, без падений
  for (size_t cut = 0; cut < zip.size(); cut += 13) {
    ZipReader r;
    if (!r.open(zip.substr(0, cut), &err)) continue;
    for (size_t i = 0; i < r.entries().size(); i++) (void)r.read(i);
  }
  // порча байтов данных записи — ошибка CRC или распаковки
  std::string bad = zip;
  bad[40] = char(bad[40] ^ 0x55);
  ZipReader r;
  CHECK(r.open(bad, &err));
  CHECK(!r.read(0, &err));
}
