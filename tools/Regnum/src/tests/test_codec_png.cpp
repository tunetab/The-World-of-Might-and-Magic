// Тесты PNG: эталонные файлы всех типов цвета и глубин (Adam7, tRNS), круговое кодирование с сокращением формата,
// повреждённые файлы, PNG репозитория (контрольные суммы Pillow), скорость на исходной карте 8000 × 4500.
#include <cstdio>

#include "base/fs.h"
#include "base/jobs.h"
#include "codec/png.h"
#include "codec/zlib.h"
#include "tests/test.h"
#include "tests/test_codec_util.h"

using namespace rg;
using namespace rg::codec;

namespace {

RgbaImage makeImage(int w, int h, int kind, u64 seed) {
  RgbaImage img;
  img.w = w;
  img.h = h;
  img.rgba.resize(size_t(w) * size_t(h) * 4);
  Rng rng(seed);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      u8* p = img.rgba.data() + (size_t(y) * size_t(w) + size_t(x)) * 4;
      switch (kind) {
        case 0:  // шум RGBA
          for (int c = 0; c < 4; c++) p[c] = u8(rng.next());
          break;
        case 1:  // непрозрачный градиент RGB
          p[0] = u8(x * 255 / std::max(1, w - 1));
          p[1] = u8(y * 255 / std::max(1, h - 1));
          p[2] = u8((x + y) & 255);
          p[3] = 255;
          break;
        case 2:  // серый непрозрачный
          p[0] = p[1] = p[2] = u8(x * 7 + y * 3);
          p[3] = 255;
          break;
        case 3:  // серый с альфой
          p[0] = p[1] = p[2] = u8(x * 7 + y * 3);
          p[3] = u8(255 - ((x * y) & 255));
          if ((x + y) % 3 == 0) p[0] = p[1] = p[2] = u8(rng.next());
          break;
        case 4: {  // палитра: 40 цветов, часть полупрозрачные
          int i = int(((x / 4) * 7 + (y / 3) * 13) % 40);
          p[0] = u8(i * 6);
          p[1] = u8(255 - i * 5);
          p[2] = u8(i * 37);
          p[3] = i % 5 == 0 ? u8(i * 3) : 255;
          break;
        }
        case 5:  // две краски
          p[0] = p[1] = p[2] = ((x / 3 + y / 2) & 1) ? 255 : 0;
          p[3] = 255;
          break;
        case 6:  // карта: заливки с прозрачными областями и сглаженными границами
        default: {
          double d = std::sin(x * 0.05) * 20 + std::cos(y * 0.07) * 20 + (x - w / 2.0) * 0.1;
          bool sea = d < 0;
          p[0] = sea ? 0 : u8(180 + int(d) % 40);
          p[1] = sea ? 0 : u8(150 + int(d * 2) % 60);
          p[2] = sea ? 0 : u8(90);
          p[3] = sea ? 0 : u8(std::min(255.0, d * 40));
          break;
        }
      }
    }
  return img;
}

}  // namespace

TEST(codec_png_fixtures_all_formats) {
  int n = 0;
  for (auto& e : fs::list(test::dataPath("png"))) {
    if (e.dir || !endsWith(e.name, ".png")) continue;
    std::string base = e.name.substr(0, e.name.size() - 4);
    auto file = fs::readFile(e.path);
    auto expZ = fs::readFile(test::dataPath("png/" + base + ".rgba.zz"));
    CHECK_MSG(file && expZ, base);
    auto exp = inflate(*expZ);
    CHECK(exp.has_value());
    std::string err;
    auto img = decodePng(*file, &err);
    CHECK_MSG(img.has_value(), base + ": " + err);
    CHECK_MSG(img->rgba == *exp, base + ": пиксели не совпадают");
    auto info = pngInfo(reinterpret_cast<const u8*>(file->data()), file->size());
    CHECK(info && info->w == img->w && info->h == img->h);
    n++;
  }
  CHECK(n >= 45);
}

TEST(codec_png_roundtrip_reduction) {
  struct Case { int kind, w, h, ct; };
  // ожидаемый тип цвета после сокращения
  const Case cases[] = {{0, 67, 41, 6}, {1, 64, 64, 2}, {2, 33, 17, 0}, {3, 40, 30, 4}, {4, 71, 29, 3}, {5, 37, 23, 0}, {6, 300, 200, 6},
                        {0, 1, 1, 3}, {1, 1, 300, 2}, {1, 1, 200, 3}, {0, 513, 2, 6}};
  for (auto& c : cases) {
    RgbaImage img = makeImage(c.w, c.h, c.kind, u64(c.kind * 100 + c.w));
    for (int level : {0, 1, 6, 9}) {
      auto png = encodePng(img, level);
      std::string err;
      auto back = decodePng(png.data(), png.size(), &err);
      std::string tag = strf("kind %d %dx%d L%d", c.kind, c.w, c.h, level);
      CHECK_MSG(back.has_value(), tag + ": " + err);
      CHECK_MSG(back->w == img.w && back->h == img.h && back->rgba == img.rgba, tag);
      auto info = pngInfo(png.data(), png.size());
      CHECK_MSG(info && info->colorType == c.ct, tag + strf(": тип %d", info ? info->colorType : -1));
    }
    // без сокращения — всегда RGBA8
    PngEncodeOptions o;
    o.reduce = false;
    auto png = encodePng(img, o);
    auto info = pngInfo(png.data(), png.size());
    CHECK(info && info->colorType == 6 && info->bitDepth == 8);
    auto back = decodePng(png.data(), png.size());
    CHECK(back && back->rgba == img.rgba);
  }
  // две краски — 1 бит серого
  auto two = encodePng(makeImage(37, 23, 5, 1));
  auto ti = pngInfo(two.data(), two.size());
  CHECK(ti && ti->colorType == 0 && ti->bitDepth == 1);
  // пустое изображение не кодируется
  CHECK(encodePng(RgbaImage{}).empty());
}

TEST(codec_png_artifacts_for_external_check) {
  // Файлы для проверки Pillow: tests/data/verify_outputs.py (PNG + исходные пиксели .rgba)
  std::string dir = test::outDir() + "/codec/png";
  CHECK(fs::makeDirs(dir));
  for (int kind = 0; kind <= 6; kind++) {
    RgbaImage img = makeImage(kind == 6 ? 256 : 61, kind == 6 ? 160 : 47, kind, u64(kind));
    auto png = encodePng(img, 6);
    CHECK(fs::writeFileAtomic(dir + strf("/kind%d.png", kind), std::span<const u8>(png)));
    CHECK(fs::writeFileAtomic(dir + strf("/kind%d.rgba", kind), std::span<const u8>(img.rgba)));
  }
  // наглядный образец для просмотра
  RgbaImage pat = makeImage(256, 160, 6, 3);
  for (int y = 0; y < 160; y++)
    for (int x = 0; x < 64; x++) {
      u8* p = pat.rgba.data() + (size_t(y) * 256 + size_t(x)) * 4;
      p[0] = u8(x * 4);
      p[1] = u8(y * 255 / 159);
      p[2] = 200;
      p[3] = 255;
    }
  CHECK(writePngFile(test::outDir() + "/codec/png_pattern.png", pat));
  auto back = fs::readFile(test::outDir() + "/codec/png_pattern.png");
  CHECK(back.has_value());
  auto dec = decodePng(*back);
  CHECK(dec && dec->rgba == pat.rgba);
}

TEST(codec_png_corrupt_inputs) {
  RgbaImage img = makeImage(50, 40, 1, 5);
  auto good = encodePng(img, 6);
  std::string err;
  CHECK(decodePng(good.data(), good.size(), &err).has_value());
  // подпись
  auto bad = good;
  bad[1] = 'Q';
  CHECK(!decodePng(bad.data(), bad.size(), &err));
  CHECK(err.find("подпись") != std::string::npos);
  // CRC заголовка
  bad = good;
  bad[20] ^= 1;
  CHECK(!decodePng(bad.data(), bad.size(), &err));
  // CRC данных IDAT
  bad = good;
  bad[45] ^= 0x40;
  CHECK(!decodePng(bad.data(), bad.size(), &err));
  // обрезки на всех длинах — без падений, ошибка до полного файла
  for (size_t cut = 0; cut + 12 < good.size(); cut += 7) CHECK(!decodePng(good.data(), cut));
  // без IEND — допустимо, если данные полны
  CHECK(decodePng(good.data(), good.size() - 12).has_value());
  // огромный размер в заголовке
  bad = good;
  bad[16] = 0x7F;
  u32 crc = crc32(bad.data() + 12, 17);
  bad[29] = u8(crc >> 24); bad[30] = u8(crc >> 16); bad[31] = u8(crc >> 8); bad[32] = u8(crc);
  CHECK(!decodePng(bad.data(), bad.size(), &err));
  // неверный фильтр строки: пересобрать IDAT
  {
    std::vector<u8> raw(size_t(img.h) * (size_t(img.w) * 3 + 1), 0);
    raw[0] = 7;
    auto z = deflate(raw, 6);
    std::vector<u8> f(good.begin(), good.begin() + 33);
    auto put32 = [&](u32 v) { f.push_back(u8(v >> 24)); f.push_back(u8(v >> 16)); f.push_back(u8(v >> 8)); f.push_back(u8(v)); };
    put32(u32(z.size()));
    size_t st = f.size();
    f.insert(f.end(), {'I', 'D', 'A', 'T'});
    f.insert(f.end(), z.begin(), z.end());
    put32(crc32(f.data() + st, f.size() - st));
    CHECK(!decodePng(f.data(), f.size(), &err));
    CHECK(err.find("фильтр") != std::string::npos);
  }
  // палитровое изображение без PLTE
  RgbaImage pal = makeImage(30, 20, 4, 1);
  auto pp = encodePng(pal, 6);
  size_t pl = 0;
  for (size_t i = 8; i + 8 < pp.size(); i++)
    if (std::memcmp(pp.data() + i, "PLTE", 4) == 0) { pl = i - 4; break; }
  CHECK(pl > 0);
  u32 plen = (u32(pp[pl]) << 24) | (u32(pp[pl + 1]) << 16) | (u32(pp[pl + 2]) << 8) | pp[pl + 3];
  std::vector<u8> noPlte(pp.begin(), pp.begin() + long(pl));
  noPlte.insert(noPlte.end(), pp.begin() + long(pl + 12 + plen), pp.end());
  CHECK(!decodePng(noPlte.data(), noPlte.size(), &err));
  // предел пикселей
  PngDecodeOptions lim;
  lim.maxPixels = 100;
  CHECK(!decodePng(good.data(), good.size(), lim, &err));
}

TEST(codec_png_repository_files) {
  // Исходная карта и выборка PNG репозитория: контрольная сумма RGBA совпадает с Pillow.
  auto list = fs::readFile(test::dataPath("png/repo_crc.txt"));
  CHECK(list.has_value());
  int checked = 0, missing = 0;
  for (auto& line : split(*list, '\n')) {
    auto f = split(line, '\t');
    if (f.size() != 4) continue;
    auto bytes = fs::readFile(f[0]);
    if (!bytes) { missing++; continue; }
    double t0 = nowSeconds();
    std::string err;
    auto img = decodePng(*bytes, &err);
    double ms = (nowSeconds() - t0) * 1000;
    CHECK_MSG(img.has_value(), f[0] + ": " + err);
    CHECK_EQ(img->w, std::stoi(f[1]));
    CHECK_EQ(img->h, std::stoi(f[2]));
    CHECK_EQ(strf("%08x", crc32(img->rgba.data(), img->rgba.size())), f[3]);
    if (checked == 0) std::printf("       %s: %d x %d, %.1f MB, декодирование %.0f ms\n", f[0].c_str(), img->w, img->h, bytes->size() / 1e6, ms);
    checked++;
  }
  std::printf("       PNG репозитория: проверено %d, нет файла %d\n", checked, missing);
  CHECK(checked + missing >= 9);
}

TEST(codec_png_speed_map_and_tiles) {
  auto bytes = fs::readFile("tools/Regnum/assets/source/Expanded Map.png");
  if (!bytes) {
    std::printf("       нет tools/Regnum/assets/source/Expanded Map.png — пропуск\n");
    return;
  }
  double best = 1e9;
  std::optional<RgbaImage> img;
  for (int i = 0; i < 2; i++) {
    double t0 = nowSeconds();
    img = decodePng(*bytes);
    best = std::min(best, nowSeconds() - t0);
  }
  CHECK(img.has_value() && img->w == 8000 && img->h == 4500);
  std::printf("       декодирование карты 8000 x 4500 (%.1f MB): %.0f ms\n", bytes->size() / 1e6, best * 1000);
#ifdef NDEBUG
  CHECK(best < rg::test::perf(1.2));
#endif
  // тайлы 512 × 512 из разных мест карты, уровень 6
  double encBest = 1e9, encSum = 0;
  size_t total = 0;
  int tiles = 0;
  for (int ty = 0; ty < 3; ty++)
    for (int tx = 0; tx < 4; tx++) {
      RgbaImage tile;
      tile.w = tile.h = 512;
      tile.rgba.resize(512 * 512 * 4);
      int x0 = 600 + tx * 1800, y0 = 400 + ty * 1400;
      for (int y = 0; y < 512; y++)
        std::memcpy(tile.rgba.data() + size_t(y) * 2048, img->rgba.data() + (size_t(y0 + y) * 8000 + size_t(x0)) * 4, 2048);
      double t0 = nowSeconds();
      auto png = encodePng(tile, 6);
      double t = nowSeconds() - t0;
      encBest = std::min(encBest, t);
      encSum += t;
      total += png.size();
      tiles++;
      auto back = decodePng(png.data(), png.size());
      CHECK(back && back->rgba == tile.rgba);
      if (tx == 1 && ty == 1) CHECK(writePngFile(test::outDir() + "/codec/map_tile.png", tile));
    }
  std::printf("       кодирование тайла 512 x 512 L6: среднее %.1f ms, лучшее %.1f ms, средний размер %.0f KB\n", encSum / tiles * 1000,
              encBest * 1000, total / 1024.0 / tiles);
#ifdef NDEBUG
  CHECK(encSum / tiles < rg::test::perf(0.025));
#endif
  // параллельное декодирование тайлов в пуле (как basemap)
  std::vector<std::vector<u8>> encoded;
  for (int i = 0; i < 16; i++) {
    RgbaImage tile;
    tile.w = tile.h = 256;
    tile.rgba.resize(256 * 256 * 4);
    for (int y = 0; y < 256; y++)
      std::memcpy(tile.rgba.data() + size_t(y) * 1024, img->rgba.data() + (size_t(1000 + y + i * 200) * 8000 + size_t(2000 + i * 300)) * 4, 1024);
    encoded.push_back(encodePng(tile, 1));
  }
  std::vector<int> ok(encoded.size(), 0);
  jobs::parallelFor(encoded.size(), [&](size_t i) { ok[i] = decodePng(encoded[i].data(), encoded[i].size()).has_value(); });
  for (int v : ok) CHECK(v == 1);
}
