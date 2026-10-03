// Тесты JPEG: эталоны Pillow (baseline/progressive, субдискретизация, рестарты, EXIF, Adobe RGB/CMYK, 4:4:0, 4:1:1,
// раздельные сканы, 16-битные таблицы), портреты репозитория, повреждённые файлы, скорость.
#include <cstdio>
#include <cstdlib>

#include "base/fs.h"
#include "codec/jpeg.h"
#include "codec/zlib.h"
#include "tests/test.h"
#include "tests/test_codec_util.h"

using namespace rg;
using namespace rg::codec;

TEST(codec_jpeg_fixtures_vs_pillow) {
  auto manifest = fs::readFile(test::dataPath("jpeg/manifest.txt"));
  CHECK(manifest.has_value());
  int n = 0;
  double worst = 0;
  std::string worstName;
  for (auto& line : split(*manifest, '\n')) {
    auto f = split(trim(line), ' ');
    if (f.size() != 4) continue;
    const std::string& name = f[0];
    int w = std::stoi(f[1]), h = std::stoi(f[2]);
    double tol = std::stod(f[3]);
    auto jpg = fs::readFile(test::dataPath("jpeg/" + name + ".jpg"));
    auto refPng = fs::readFile(test::dataPath("jpeg/" + name + ".ref.png"));
    CHECK_MSG(jpg && refPng, name);
    auto ref = decodePng(*refPng);
    CHECK_MSG(ref.has_value(), name);
    std::string err;
    auto img = decodeJpeg(*jpg, &err);
    CHECK_MSG(img.has_value(), name + ": " + err);
    CHECK_MSG(img->w == w && img->h == h, name + strf(": размер %d x %d", img->w, img->h));
    double mae = test::meanAbsError(img->rgba.data(), ref->rgba.data(), size_t(w) * size_t(h), 4, 4, 3);
    int maxDiff = 0;
    for (size_t i = 0; i < img->rgba.size(); i++)
      if (i % 4 != 3) maxDiff = std::max(maxDiff, std::abs(int(img->rgba[i]) - int(ref->rgba[i])));
    std::printf("       %-9s %3d x %-3d  MAE %.3f  макс. %d\n", name.c_str(), w, h, mae, maxDiff);
    if (name == "p422_big" || name == "cmyk" || name == "exif6" || name == "c440_big")
      CHECK(writePngFile(test::outDir() + "/codec/jpeg_" + name + ".png", *img, 1));
    CHECK_MSG(mae <= tol, name + strf(": MAE %.3f > %.1f", mae, tol));
    if (mae > worst) { worst = mae; worstName = name; }
    n++;
  }
  CHECK(n >= 30);
  std::printf("       наибольшая MAE %.3f (%s)\n", worst, worstName.c_str());
}

TEST(codec_jpeg_info_and_orientation) {
  auto jpg = fs::readFile(test::dataPath("jpeg/exif6.jpg"));
  CHECK(jpg.has_value());
  auto info = jpegInfo(reinterpret_cast<const u8*>(jpg->data()), jpg->size());
  CHECK(info && info->orientation == 6 && info->w == 29 && info->h == 19 && info->components == 3 && !info->progressive);
  JpegDecodeOptions o;
  o.applyOrientation = false;
  auto raw = decodeJpeg(reinterpret_cast<const u8*>(jpg->data()), jpg->size(), o);
  CHECK(raw && raw->w == 29 && raw->h == 19);
  auto rot = decodeJpeg(*jpg);
  CHECK(rot && rot->w == 19 && rot->h == 29);
  // поворот на 90° по часовой: пиксель (x, y) исходного -> (h−1−y, x)
  for (int y = 0; y < raw->h; y++)
    for (int x = 0; x < raw->w; x++) {
      const u8* a = raw->rgba.data() + (size_t(y) * 29 + size_t(x)) * 4;
      const u8* b = rot->rgba.data() + (size_t(x) * 19 + size_t(raw->h - 1 - y)) * 4;
      CHECK(std::memcmp(a, b, 4) == 0);
    }
  auto p = fs::readFile(test::dataPath("jpeg/p420.jpg"));
  auto pi = jpegInfo(reinterpret_cast<const u8*>(p->data()), p->size());
  CHECK(pi && pi->progressive && pi->orientation == 1);
  // decodeImage различает форматы
  CHECK(decodeImage(*p).has_value());
  std::string err;
  CHECK(!decodeImage(std::string("GIF89a....")).has_value());
  CHECK(!decodeImage(std::string("GIF89a...."), &err) && !err.empty());
}

TEST(codec_jpeg_repository_portraits) {
  // Средние цвета клеток 64 × 64 совпадают с декодированием Pillow (EXIF учтён).
  auto list = fs::readFile(test::dataPath("jpeg/portraits.txt"));
  auto cells = fs::readFile(test::dataPath("jpeg/portraits.bin"));
  CHECK(list && cells);
  int checked = 0, missing = 0, progressive = 0;
  double worst = 0, totalMs = 0;
  for (auto& line : split(*list, '\n')) {
    auto f = split(line, '\t');
    if (f.size() != 4) continue;
    auto bytes = fs::readFile(f[0]);
    if (!bytes) { missing++; continue; }
    double t0 = nowSeconds();
    std::string err;
    auto img = decodeJpeg(*bytes, &err);
    totalMs += (nowSeconds() - t0) * 1000;
    CHECK_MSG(img.has_value(), f[0] + ": " + err);
    int w = std::stoi(f[1]), h = std::stoi(f[2]);
    size_t off = size_t(std::stoul(f[3]));
    CHECK_EQ(img->w, w);
    CHECK_EQ(img->h, h);
    auto info = jpegInfo(reinterpret_cast<const u8*>(bytes->data()), bytes->size());
    if (info && info->progressive) progressive++;
    int cw = (w + 63) / 64, ch = (h + 63) / 64;
    CHECK(off + size_t(cw * ch * 3) <= cells->size());
    double err2 = 0;
    for (int cy = 0; cy < ch; cy++)
      for (int cx = 0; cx < cw; cx++) {
        u64 s[3] = {}, cnt = 0;
        for (int y = cy * 64; y < std::min(h, cy * 64 + 64); y++)
          for (int x = cx * 64; x < std::min(w, cx * 64 + 64); x++) {
            const u8* p = img->rgba.data() + (size_t(y) * size_t(w) + size_t(x)) * 4;
            s[0] += p[0];
            s[1] += p[1];
            s[2] += p[2];
            cnt++;
          }
        for (int c = 0; c < 3; c++) {
          double mean = double(s[c]) / double(cnt);
          err2 += std::fabs(mean - u8(cells->at(off + size_t((cy * cw + cx) * 3 + c))));
        }
      }
    double mae = err2 / (cw * ch * 3);
    worst = std::max(worst, mae);
    CHECK_MSG(mae < 0.75, f[0] + strf(": MAE клеток %.3f", mae));
    checked++;
  }
  std::printf("       портреты: проверено %d (прогрессивных %d), нет файла %d, худшая MAE клеток %.3f, среднее декодирование %.1f ms\n",
              checked, progressive, missing, worst, checked ? totalMs / checked : 0.0);
  CHECK(checked + missing >= 20);
}

TEST(codec_jpeg_corrupt_and_truncated) {
  auto good = fs::readFile(test::dataPath("jpeg/b420_big.jpg"));
  auto prog = fs::readFile(test::dataPath("jpeg/p422_big.jpg"));
  CHECK(good && prog);
  std::string err;
  // обрезанный файл — декодируется до места обрыва
  for (const std::string* src : {&*good, &*prog}) {
    std::string half = src->substr(0, src->size() * 2 / 3);
    auto img = decodeJpeg(half, &err);
    CHECK_MSG(img.has_value(), err);
    CHECK(img->w == 113 && img->h == 81);
  }
  // без SOI
  CHECK(!decodeJpeg(good->substr(2), &err));
  // только заголовки, без скана
  size_t sos = good->find("\xFF\xDA");
  CHECK(sos != std::string::npos);
  CHECK(!decodeJpeg(good->substr(0, sos), &err));
  // арифметическое кодирование
  std::string ar = *good;
  size_t sof = ar.find("\xFF\xC0");
  CHECK(sof != std::string::npos);
  ar[sof + 1] = char(0xC9);
  CHECK(!decodeJpeg(ar, &err));
  CHECK(err.find("арифметическое") != std::string::npos);
  // 12 бит
  std::string p12 = *good;
  p12[sof + 4] = 12;
  CHECK(!decodeJpeg(p12, &err));
  // предел пикселей
  JpegDecodeOptions lim;
  lim.maxPixels = 1000;
  CHECK(!decodeJpeg(reinterpret_cast<const u8*>(good->data()), good->size(), lim, &err));
}

TEST(codec_jpeg_speed) {
  std::string path;
  for (auto& line : split(fs::readFile(test::dataPath("jpeg/portraits.txt")).value_or(""), '\n')) {
    auto f = split(line, '\t');
    if (f.size() == 4 && fs::exists(f[0])) { path = f[0]; break; }
  }
  if (path.empty()) return;
  auto bytes = fs::readFile(path);
  double best = 1e9;
  for (int i = 0; i < 3; i++) {
    double t0 = nowSeconds();
    auto img = decodeJpeg(*bytes);
    best = std::min(best, nowSeconds() - t0);
    CHECK(img.has_value());
    if (i == 0) CHECK(writePngFile(test::outDir() + "/codec/portrait.png", *img, 1));
  }
  std::printf("       портрет %.0f KB: декодирование %.1f ms\n", bytes->size() / 1024.0, best * 1000);
}
