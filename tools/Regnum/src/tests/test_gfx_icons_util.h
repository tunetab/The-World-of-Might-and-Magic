// Regnum — общие помощники тестов значков, эмблем, флагов и фигурок: запись PNG, подписи, увеличение, покрытие.
#pragma once
#include "codec/png.h"
#include "gfx/canvas.h"
#include "gfx/text.h"
#include "tests/test.h"

namespace rg::test::icons {

inline void savePng(const gfx::Image& img, const std::string& name) {
  codec::RgbaImage out;
  out.w = img.w;
  out.h = img.h;
  out.rgba = img.toRgba();
  const std::string path = test::outDir() + "/" + name + ".png";
  CHECK_MSG(codec::writePngFile(path, out, 6), path);
}

// Подпись мелким шрифтом (если системные шрифты недоступны — без подписи).
inline void label(gfx::Canvas& c, std::string_view text, float x, float y, Color col, float size = 10.5f) {
  if (!gfx::initFonts()) return;
  gfx::TextStyle st;
  st.size = size;
  gfx::drawText(c, text, st, x, y, gfx::Paint(col));
}

// Увеличение ближайшим соседом (для разглядывания пикселей).
inline gfx::Image zoomed(const gfx::Image& src, int k) {
  gfx::Image z(src.w * k, src.h * k);
  for (int y = 0; y < z.h; y++)
    for (int x = 0; x < z.w; x++) z.row(y)[x] = src.at(x / k, y / k);
  return z;
}

// Покрытие (сумма альфы / 255) и габариты непрозрачных пикселей прозрачного холста.
struct Coverage {
  double sum = 0;
  int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;   // включительно
  double cx = 0, cy = 0;                              // центр масс
  bool any() const { return x1 >= 0; }
};
inline Coverage coverage(const gfx::Image& img, u8 threshold = 24) {
  Coverage cv;
  double mx = 0, my = 0;
  for (int y = 0; y < img.h; y++)
    for (int x = 0; x < img.w; x++) {
      const u32 a = img.at(x, y) >> 24;
      if (!a) continue;
      cv.sum += a / 255.0;
      mx += (x + 0.5) * a / 255.0;
      my += (y + 0.5) * a / 255.0;
      if (a < threshold) continue;
      cv.x0 = std::min(cv.x0, x);
      cv.y0 = std::min(cv.y0, y);
      cv.x1 = std::max(cv.x1, x);
      cv.y1 = std::max(cv.y1, y);
    }
  if (cv.sum > 0) {
    cv.cx = mx / cv.sum;
    cv.cy = my / cv.sum;
  }
  return cv;
}

}  // namespace rg::test::icons
