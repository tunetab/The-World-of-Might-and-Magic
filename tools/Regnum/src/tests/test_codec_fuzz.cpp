// Фаззинг кодеков: случайная порча корректных файлов (байты, обрезка, вставки, исправленные CRC) не должна
// приводить к падению, зависанию или чтению за границами. Итог — ошибка или изображение разумного размера.
#include <cstdio>

#include "base/fs.h"
#include "base/json.h"
#include "codec/base64.h"
#include "codec/jpeg.h"
#include "codec/png.h"
#include "codec/zip.h"
#include "codec/zlib.h"
#include "tests/test.h"
#include "tests/test_codec_util.h"

using namespace rg;
using namespace rg::codec;

namespace {

std::string mutate(const std::string& src, Rng& rng, size_t protectPrefix = 0) {
  std::string s = src;
  int kind = rng.range(0, 9);
  if (s.empty()) return s;
  auto pos = [&]() { return size_t(protectPrefix + rng.next() % std::max<size_t>(1, s.size() - std::min(s.size(), protectPrefix))); };
  if (kind <= 4) {
    int flips = rng.range(1, 1 + kind * 3);
    for (int i = 0; i < flips; i++) {
      size_t p = std::min(pos(), s.size() - 1);
      s[p] = char(rng.next() & 1 ? rng.next() : (u8(s[p]) ^ (1u << rng.range(0, 7))));
    }
  } else if (kind == 5) {
    s.resize(pos());
  } else if (kind == 6) {
    size_t p = std::min(pos(), s.size());
    s.insert(p, std::string(size_t(rng.range(1, 64)), char(rng.next())));
  } else if (kind == 7) {
    size_t p = std::min(pos(), s.size()), n = std::min<size_t>(s.size() - p, size_t(rng.range(1, 200)));
    s.erase(p, n);
  } else if (kind == 8) {
    // повтор куска из другого места
    size_t a = std::min(pos(), s.size() - 1), b = std::min(pos(), s.size() - 1);
    size_t n = std::min<size_t>({size_t(rng.range(1, 300)), s.size() - a, s.size() - b});
    s.replace(b, n, s.substr(a, n));
  } else {
    for (int i = 0; i < 4; i++) {
      size_t p = std::min(pos(), s.size() - 1);
      s[p] = char(0xFF);
    }
  }
  return s;
}

// Пересчитать CRC всех блоков PNG, чтобы порча доходила до распаковки и фильтров.
void fixPngCrcs(std::string& s) {
  size_t p = 8;
  while (p + 12 <= s.size()) {
    const u8* d = reinterpret_cast<const u8*>(s.data());
    u32 len = (u32(d[p]) << 24) | (u32(d[p + 1]) << 16) | (u32(d[p + 2]) << 8) | d[p + 3];
    if (len > s.size() || p + 12 + len > s.size()) return;
    u32 c = crc32(d + p + 4, len + 4);
    s[p + 8 + len] = char(c >> 24);
    s[p + 9 + len] = char(c >> 16);
    s[p + 10 + len] = char(c >> 8);
    s[p + 11 + len] = char(c);
    p += 12 + len;
  }
}

std::vector<std::string> filesIn(const std::string& dir, const char* ext) {
  std::vector<std::string> r;
  for (auto& e : fs::list(test::dataPath(dir)))
    if (!e.dir && endsWith(e.name, ext)) r.push_back(*fs::readFile(e.path));
  return r;
}

}  // namespace

TEST(codec_fuzz_png) {
  auto files = filesIn("png", ".png");
  CHECK(files.size() > 40);
  Rng rng(1);
  int decoded = 0, iters = 0;
  double t0 = nowSeconds();
  for (int i = 0; i < 6000; i++) {
    const std::string& src = files[size_t(rng.next() % files.size())];
    std::string m = mutate(src, rng, rng.range(0, 1) ? 33 : 8);
    if (i % 2) fixPngCrcs(m);
    PngDecodeOptions o;
    o.maxPixels = 1 << 20;
    auto img = decodePng(reinterpret_cast<const u8*>(m.data()), m.size(), o);
    if (img) {
      CHECK(img->w > 0 && img->h > 0 && img->rgba.size() == size_t(img->w) * size_t(img->h) * 4);
      decoded++;
    }
    iters++;
  }
  std::printf("       PNG: %d мутаций, декодировано %d, %.2f s\n", iters, decoded, nowSeconds() - t0);
}

TEST(codec_fuzz_jpeg) {
  auto files = filesIn("jpeg", ".jpg");
  CHECK(files.size() > 25);
  Rng rng(2);
  int decoded = 0, iters = 0;
  double t0 = nowSeconds();
  for (int i = 0; i < 4000; i++) {
    const std::string& src = files[size_t(rng.next() % files.size())];
    std::string m = mutate(src, rng, rng.range(0, 1) ? 2 : 100);
    JpegDecodeOptions o;
    o.maxPixels = 1 << 22;
    auto img = decodeJpeg(reinterpret_cast<const u8*>(m.data()), m.size(), o);
    if (img) {
      CHECK(img->w > 0 && img->h > 0 && img->rgba.size() == size_t(img->w) * size_t(img->h) * 4);
      decoded++;
    }
    (void)jpegInfo(reinterpret_cast<const u8*>(m.data()), m.size());
    iters++;
  }
  std::printf("       JPEG: %d мутаций, декодировано %d, %.2f s\n", iters, decoded, nowSeconds() - t0);
}

TEST(codec_fuzz_inflate) {
  std::vector<std::pair<std::string, ZFormat>> streams;
  for (auto& line : split(fs::readFile(test::dataPath("zlib/manifest.txt")).value_or(""), '\n')) {
    auto f = split(trim(line), ' ');
    if (f.size() != 3) continue;
    streams.push_back({*fs::readFile(test::dataPath("zlib/" + f[0])), f[1] == "raw" ? ZFormat::Raw : f[1] == "gzip" ? ZFormat::Gzip : ZFormat::Zlib});
  }
  CHECK(streams.size() > 20);
  Rng rng(3);
  int ok = 0;
  std::vector<u8> fixed(1 << 16);
  for (int i = 0; i < 8000; i++) {
    auto& [src, fmt] = streams[size_t(rng.next() % streams.size())];
    std::string m = mutate(src, rng, 0);
    InflateOptions o;
    o.maxOutput = 1 << 20;
    o.verifyChecksum = i % 3 != 0;
    auto r = inflate(m, fmt, nullptr, o);
    if (r) {
      CHECK(r->size() <= o.maxOutput);
      ok++;
    }
    auto n = inflateInto(std::span<const u8>(reinterpret_cast<const u8*>(m.data()), m.size()), fmt, fixed, nullptr, i % 2 == 0, false);
    if (n) CHECK(*n <= fixed.size());
  }
  // полностью случайные данные как raw deflate
  for (int i = 0; i < 3000; i++) {
    std::string m;
    int n = rng.range(0, 400);
    for (int k = 0; k < n; k++) m.push_back(char(rng.next()));
    InflateOptions o;
    o.maxOutput = 1 << 18;
    (void)inflate(m, ZFormat::Raw, nullptr, o);
  }
  std::printf("       inflate: успешно распаковано после порчи %d\n", ok);
}

TEST(codec_fuzz_zip_json_base64) {
  ZipWriter w;
  for (int i = 0; i < 12; i++) w.add(strf("папка/файл%d.txt", i), std::string(size_t(100 + i * 40), char('a' + i)));
  w.addDir("пустая");
  std::vector<std::string> zips = {w.finish("комментарий")};
  for (auto& z : filesIn("zip", ".zip")) zips.push_back(z);
  Rng rng(4);
  for (int i = 0; i < 3000; i++) {
    std::string m = mutate(zips[size_t(rng.next() % zips.size())], rng, 0);
    ZipReader r;
    r.maxEntrySize = 1 << 20;
    if (!r.open(m)) continue;
    for (size_t k = 0; k < r.entries().size(); k++) (void)r.read(k);
  }
  std::string jtext = R"({"мир": {"ход": 12, "провинции": [{"id": 1, "имя": "Арден", "x": [1.5, -2e3, 0.25]}, {"id": 2, "флаг": true, "нет": null}],
    "строка": "Аб😀\n\t\"кавычки\"", "пусто": {}, "массив": []}})";
  CHECK(json::tryParse(jtext).has_value());
  for (int i = 0; i < 20000; i++) {
    std::string m = mutate(jtext, rng, 0);
    std::string err;
    auto v = json::tryParse(m, &err);
    if (v) (void)json::write(*v);
    else CHECK(!err.empty());
  }
  for (int i = 0; i < 5000; i++) {
    std::string m;
    int n = rng.range(0, 40);
    for (int k = 0; k < n; k++) m.push_back("ABCxyz019+/= \n\x80"[rng.next() % 16]);
    auto d = base64::decode(m);
    if (d) {
      auto again = base64::encode(*d);
      CHECK(base64::decode(again) == d);
    }
  }
}
