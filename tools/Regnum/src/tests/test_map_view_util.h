// Regnum — общие средства тестов отрисовки карты (test_map_view*.cpp, test_map_demo*.cpp).
#pragma once
#include "base/fs.h"
#include "codec/png.h"
#include "gfx/canvas.h"
#include "map/basemap.h"
#include "map/demo_world.h"
#include "map/mapview.h"
#include "tests/test.h"

namespace rg::mvtest {

// Настоящая базовая карта репозитория (тесты запускаются из корня репозитория).
inline const map::Basemap& basemap() {
  static const map::Basemap bm = [] {
    map::Basemap b;
    std::string err;
    if (!b.load("tools/Regnum/assets/basemap", &err)) test::fail(__FILE__, __LINE__, "базовая карта: " + err);
    return b;
  }();
  return bm;
}

inline const World& demo() {
  static const World w = map::makeDemoWorld(basemap());
  return w;
}

// Пустой мир на берегу базовой карты (без провинций).
inline const World& emptyWorld() {
  static const World w = [] {
    World x = newWorld("Пустой");
    Tx tx(x);
    geo::initFromCoast(tx, basemap().coast());
    return std::move(tx).finish();
  }();
  return w;
}

// Кадр с дорисовкой: рендер, ожидание фоновых тайлов, повтор — пока всё не готово.
inline gfx::Image renderFull(map::MapView& mv, const map::RenderOptions& opt, int w, int h, float dpi) {
  mv.setViewport(RectF(0, 0, float(w), float(h)), dpi);
  gfx::Image img(int(std::lround(w * dpi)), int(std::lround(h * dpi)), 0xff000000u);
  for (int i = 0; i < 20; i++) {
    gfx::Canvas c(img);
    mv.render(c, opt);
    if (!mv.loading() && !mv.stats().fallback) break;
    mv.waitIdle(20);
  }
  return img;
}

inline void savePng(const gfx::Image& img, const std::string& name) {
  codec::RgbaImage out;
  out.w = img.w;
  out.h = img.h;
  out.rgba = img.toRgba();
  if (!codec::writePngFile(fs::join(test::outDir(), name), out, 4)) test::fail(__FILE__, __LINE__, "не записать " + name);
}

// Увеличенный фрагмент (каждый пиксель — k × k) для разглядывания швов, берега и подписей.
inline void saveCrop(const gfx::Image& img, gfx::RectI r, int k, const std::string& name) {
  r = r.intersect(gfx::RectI(0, 0, img.w, img.h));
  gfx::Image out(r.w * k, r.h * k, 0);
  for (int y = 0; y < out.h; y++)
    for (int x = 0; x < out.w; x++) out.row(y)[x] = img.at(r.x + x / k, r.y + y / k);
  savePng(out, name);
}

}  // namespace rg::mvtest
