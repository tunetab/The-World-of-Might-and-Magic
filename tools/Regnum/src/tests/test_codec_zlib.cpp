// Тесты zlib: контрольные суммы, круговая упаковка, совместимость с эталонными потоками Python zlib/gzip,
// повреждённые данные, пределы, скорость.
#include <cstdio>

#include "base/fs.h"
#include "codec/zlib.h"
#include "tests/test.h"
#include "tests/test_codec_util.h"

using namespace rg;
using namespace rg::codec;

namespace {

std::vector<u8> bytesOf(std::string_view s) { return std::vector<u8>(s.begin(), s.end()); }

// Набор входов, общий с tests/data/gen_fixtures.py (там же воспроизводится побайтно).
std::vector<std::pair<std::string, std::vector<u8>>> sampleInputs() {
  std::vector<std::pair<std::string, std::vector<u8>>> r;
  r.push_back({"empty", {}});
  r.push_back({"one", {0x41}});
  r.push_back({"zeros", std::vector<u8>(100000, 0)});
  std::vector<u8> pat;
  for (int i = 0; i < 60000; i++) pat.push_back(u8("abcdefg"[i % ((i / 997) % 7 + 1)]));
  r.push_back({"pattern", pat});
  auto text = fs::readFile(test::dataPath("zlib/text.txt"));
  if (text) r.push_back({"text", bytesOf(*text)});
  auto bin = fs::readFile(test::dataPath("zlib/random.bin"));
  if (bin) r.push_back({"random", bytesOf(*bin)});
  return r;
}

}  // namespace

TEST(codec_zlib_checksums) {
  CHECK_EQ(crc32("123456789"), 0xCBF43926u);
  CHECK_EQ(crc32(""), 0u);
  CHECK_EQ(crc32("The quick brown fox jumps over the lazy dog"), 0x414FA339u);
  CHECK_EQ(adler32("Wikipedia"), 0x11E60398u);
  CHECK_EQ(adler32(""), 1u);
  // продолжение по частям совпадает с целым
  std::string s;
  Rng rng(7);
  for (int i = 0; i < 100003; i++) s.push_back(char(rng.next() & 255));
  u32 c = crc32(s), a = adler32(s);
  for (size_t cut : {size_t(0), size_t(1), size_t(7), size_t(5552), size_t(65536), s.size()}) {
    std::string_view x(s.data(), cut), y(s.data() + cut, s.size() - cut);
    CHECK_EQ(crc32(y, crc32(x)), c);
    CHECK_EQ(adler32(y, adler32(x)), a);
  }
  // большие значения: суммы Adler не переполняются на 0xFF
  std::string ff(1000000, '\xff');
  u32 ad = 1;
  {
    u64 A = 1, B = 0;
    for (size_t i = 0; i < ff.size(); i++) { A = (A + 255) % 65521; B = (B + A) % 65521; }
    ad = u32((B << 16) | A);
  }
  CHECK_EQ(adler32(ff), ad);
}

TEST(codec_zlib_roundtrip_all_levels) {
  for (auto& [name, data] : sampleInputs()) {
    for (int level = 0; level <= 9; level++) {
      for (ZFormat f : {ZFormat::Raw, ZFormat::Zlib, ZFormat::Gzip}) {
        auto z = deflate(data, level, f);
        std::string err;
        auto back = inflate(z, f, &err);
        CHECK_MSG(back.has_value(), name + " level " + std::to_string(level) + ": " + err);
        CHECK_MSG(*back == data, name + " level " + std::to_string(level));
        if (level == 0 && !data.empty()) CHECK(z.size() >= data.size());
      }
    }
  }
}

TEST(codec_zlib_roundtrip_random_structures) {
  // Случайные данные с повторами разной длины и расстояний, включая максимальное окно.
  Rng rng(12345);
  for (int iter = 0; iter < 60; iter++) {
    size_t n = size_t(rng.range(0, 200000));
    std::vector<u8> d;
    d.reserve(n);
    while (d.size() < n) {
      int kind = rng.range(0, 3);
      if (kind == 0 || d.size() < 10) {
        int k = rng.range(1, 40);
        for (int i = 0; i < k; i++) d.push_back(u8(rng.next() % (iter % 3 == 0 ? 4 : 256)));
      } else {
        size_t dist = size_t(rng.range(1, int(std::min<size_t>(d.size(), 32768))));
        int len = rng.range(3, 300);
        for (int i = 0; i < len; i++) d.push_back(d[d.size() - dist]);
      }
    }
    d.resize(n);
    int level = iter % 10;
    auto z = deflate(d, level, ZFormat::Zlib);
    std::string err;
    auto back = inflate(z, ZFormat::Zlib, &err);
    CHECK_MSG(back && *back == d, "iter " + std::to_string(iter) + " " + err);
  }
}

TEST(codec_zlib_compression_ratio) {
  auto text = fs::readFile(test::dataPath("zlib/text.txt"));
  CHECK(text.has_value());
  auto z1 = deflate(*text, 1), z6 = deflate(*text, 6), z9 = deflate(*text, 9);
  CHECK(z6.size() <= z1.size());
  CHECK(z9.size() <= z6.size() + 16);
  // эталон: python zlib уровень 6 (размер записан генератором)
  auto ref = fs::readFile(test::dataPath("zlib/text.6.zz"));
  CHECK(ref.has_value());
  std::printf("       text %zu: deflate L1 %zu, L6 %zu, L9 %zu; python zlib L6 %zu\n", text->size(), z1.size(), z6.size(), z9.size(), ref->size());
  CHECK(double(z6.size()) <= double(ref->size()) * 1.06);
  std::vector<u8> zeros(1 << 20, 0);
  CHECK(deflate(zeros, 6).size() < 1200);
}

TEST(codec_zlib_python_streams) {
  // Потоки, созданные python zlib/gzip (gen_fixtures.py): уровни, стратегии, raw, gzip с полями, несколько участников.
  auto manifest = fs::readFile(test::dataPath("zlib/manifest.txt"));
  CHECK(manifest.has_value());
  int count = 0;
  for (auto& line : split(*manifest, '\n')) {
    auto parts = split(trim(line), ' ');
    if (parts.size() != 3) continue;
    const std::string& file = parts[0];
    ZFormat f = parts[1] == "raw" ? ZFormat::Raw : parts[1] == "gzip" ? ZFormat::Gzip : ZFormat::Zlib;
    auto packed = fs::readFile(test::dataPath("zlib/" + file));
    auto plain = fs::readFile(test::dataPath("zlib/" + parts[2]));
    CHECK_MSG(packed && plain, file);
    std::string err;
    auto out = inflate(*packed, f, &err);
    CHECK_MSG(out.has_value(), file + ": " + err);
    CHECK_MSG(std::string(out->begin(), out->end()) == *plain, file);
    count++;
  }
  CHECK(count >= 20);
}

TEST(codec_zlib_writes_python_artifacts) {
  // Для проверки внешним распаковщиком: tests/data/verify_outputs.py
  auto text = fs::readFile(test::dataPath("zlib/text.txt"));
  CHECK(text.has_value());
  std::string dir = test::outDir() + "/codec";
  CHECK(fs::makeDirs(dir));
  for (int level : {0, 1, 6, 9}) {
    auto z = deflate(*text, level, ZFormat::Zlib);
    CHECK(fs::writeFileAtomic(dir + "/text." + std::to_string(level) + ".zz", std::span<const u8>(z)));
    auto g = deflate(*text, level, ZFormat::Gzip);
    CHECK(fs::writeFileAtomic(dir + "/text." + std::to_string(level) + ".gz", std::span<const u8>(g)));
  }
  CHECK(fs::writeFileAtomic(dir + "/text.txt", *text));
}

TEST(codec_zlib_errors) {
  std::string data = "Сжатие и распаковка: проверка ошибок. " + std::string(5000, 'x');
  auto z = deflate(data, 6, ZFormat::Zlib);
  std::string err;
  // испорченная контрольная сумма
  auto bad = z;
  bad.back() ^= 1;
  CHECK(!inflate(bad, ZFormat::Zlib, &err));
  CHECK(err.find("контрольная") != std::string::npos);
  // без проверки — проходит
  InflateOptions noCheck;
  noCheck.verifyChecksum = false;
  CHECK(inflate(bad, ZFormat::Zlib, nullptr, noCheck).has_value());
  // неверный заголовок
  bad = z;
  bad[0] = 0x79;
  CHECK(!inflate(bad, ZFormat::Zlib, &err));
  // обрыв на каждой длине
  for (size_t cut = 0; cut < z.size(); cut += 1 + cut / 16) {
    std::vector<u8> t(z.begin(), z.begin() + long(cut));
    CHECK(!inflate(t, ZFormat::Zlib));
  }
  // gzip: размер и CRC
  auto g = deflate(data, 6, ZFormat::Gzip);
  bad = g;
  bad[bad.size() - 1] ^= 1;
  CHECK(!inflate(bad, ZFormat::Gzip, &err));
  bad = g;
  bad[bad.size() - 5] ^= 1;
  CHECK(!inflate(bad, ZFormat::Gzip, &err));
  // лишний мусор после gzip
  bad = g;
  bad.push_back(0x42);
  CHECK(!inflate(bad, ZFormat::Gzip));
  bad = g;
  bad.push_back(0);
  bad.push_back(0);
  CHECK(inflate(bad, ZFormat::Gzip).has_value());  // нули после потока допустимы
  // два участника подряд
  auto g2 = deflate(std::string("второй"), 9, ZFormat::Gzip);
  bad = g;
  bad.insert(bad.end(), g2.begin(), g2.end());
  auto two = inflate(bad, ZFormat::Gzip);
  CHECK(two && std::string(two->begin(), two->end()) == data + "второй");
  // тип блока 3
  std::vector<u8> bt = {0x07, 0x00};
  CHECK(!inflate(bt, ZFormat::Raw, &err));
  // stored с неверным NLEN
  std::vector<u8> st = {0x01, 0x05, 0x00, 0x00, 0x00, 'a', 'b', 'c', 'd', 'e'};
  CHECK(!inflate(st, ZFormat::Raw));
  st = {0x01, 0x05, 0x00, 0xFA, 0xFF, 'a', 'b', 'c', 'd', 'e'};
  auto ok = inflate(st, ZFormat::Raw);
  CHECK(ok && std::string(ok->begin(), ok->end()) == "abcde");
  // ссылка за пределы окна: фиксированный блок, длина 3, расстояние 1 в самом начале
  std::vector<u8> far = {0x03, 0x02, 0x00};  // 1 + 01 + код 257 (0000001) + расст. 0 (00000)
  CHECK(!inflate(far, ZFormat::Raw, &err));
}

TEST(codec_zlib_limits) {
  std::vector<u8> zeros(1 << 22, 0);
  auto z = deflate(zeros, 9);
  InflateOptions o;
  o.maxOutput = 1 << 20;
  std::string err;
  CHECK(!inflate(z, ZFormat::Zlib, &err, o));
  CHECK(err.find("больше") != std::string::npos);
  o.stopAtMax = true;
  auto part = inflate(z, ZFormat::Zlib, &err, o);
  CHECK(part && part->size() == size_t(1) << 20);
  // фиксированный буфер
  std::vector<u8> buf(zeros.size());
  auto n = inflateInto(z, ZFormat::Zlib, buf, &err);
  CHECK(n && *n == zeros.size() && buf == zeros);
  std::vector<u8> small(1000);
  CHECK(!inflateInto(z, ZFormat::Zlib, small));
  auto n2 = inflateInto(z, ZFormat::Zlib, small, nullptr, true);
  CHECK(n2 && *n2 == 1000);
  // подсказка размера
  o = InflateOptions{};
  o.sizeHint = zeros.size();
  auto full = inflate(z, ZFormat::Zlib, nullptr, o);
  CHECK(full && *full == zeros);
}

TEST(codec_zlib_speed) {
  // Типичные данные изображения: строки карты (повторы на расстоянии строки + шум).
  Rng rng(99);
  const size_t W = 2048 * 4, H = 512;
  std::vector<u8> img(W * H);
  for (size_t y = 0; y < H; y++)
    for (size_t x = 0; x < W; x++) {
      u8 base = u8((x / 64 * 37 + y / 48 * 91) & 255);
      img[y * W + x] = (rng.next() % 8 == 0) ? u8(base + rng.next() % 9) : base;
    }
  double t0 = nowSeconds();
  auto z = deflate(img, 6);
  double t1 = nowSeconds();
  auto back = inflate(z);
  double t2 = nowSeconds();
  CHECK(back && *back == img);
  std::printf("       deflate L6 %.1f MB: %.1f ms (%.0f MB/s), %.1f%%; inflate %.1f ms (%.0f MB/s)\n", img.size() / 1e6, (t1 - t0) * 1e3,
              img.size() / 1e6 / (t1 - t0), 100.0 * z.size() / img.size(), (t2 - t1) * 1e3, img.size() / 1e6 / (t2 - t1));
}
