// Regnum — синтетическая карта для тестов базовой карты (общая для test_map_basemap_*.cpp).
// Море #0026FF, суша с мягким берегом, залив за цепочкой островков (проток уже 2r+1 без них не связан с морем),
// озеро и река к морю, две слившиеся горы со снежными шапками, замок (контур), башня, пунктир в море, мелкие островки.
#pragma once
#include <string>

#include "base/fs.h"
#include "codec/png.h"
#include "map/basemap_build.h"
#include "tests/test.h"

namespace rg::bmtest {

struct SynthMap {
  int w = 480, h = 320;
  int my = 150;  // основание гор (y центра)
  codec::RgbaImage img;
  // Опорные пиксели для проверок.
  Vec2 sea{12, 12}, bay{240, 80}, lake{330, 185}, river{330, 238}, land{200, 140};
  Vec2 snow{150, 145}, body{150, 153}, gap{156, 142};
  Vec2 castleInside{111, 207}, castleLine{100, 207}, tower{203, 204}, dot{30, 300};
};

namespace detail {

inline double sdEllipse(double x, double y, double cx, double cy, double a, double b) {
  const double dx = (x - cx) / a, dy = (y - cy) / b;
  return (std::sqrt(dx * dx + dy * dy) - 1.0) * std::min(a, b);
}
inline double sdCircle(double x, double y, double cx, double cy, double r) { return std::hypot(x - cx, y - cy) - r; }
inline double sdRect(double x, double y, double x0, double y0, double x1, double y1) {
  return std::max(std::max(x0 - x, x - x1), std::max(y0 - y, y - y1));
}
// Покрытие с мягким краем ширины ramp по знаковому расстоянию (внутри — отрицательное).
inline double soft(double d, double ramp) { return clamp(0.5 - d / ramp, 0.0, 1.0); }

struct Tri { double x0, y0, x1, y1, x2, y2; };
inline bool inTri(const Tri& t, double x, double y) {
  auto e = [](double ax, double ay, double bx, double by, double px, double py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); };
  const double a = e(t.x0, t.y0, t.x1, t.y1, x, y), b = e(t.x1, t.y1, t.x2, t.y2, x, y), c = e(t.x2, t.y2, t.x0, t.y0, x, y);
  return (a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0);
}

// Наложение краски цвета g с покрытием inside() (4 × 4 выборки на пиксель) в прямоугольнике [x0, x1) × [y0, y1).
template <class F>
void paint(codec::RgbaImage& im, int x0, int y0, int x1, int y1, int g, F&& inside) {
  for (int y = std::max(0, y0); y < std::min(im.h, y1); y++)
    for (int x = std::max(0, x0); x < std::min(im.w, x1); x++) {
      int n = 0;
      for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) n += inside(x + (i + 0.5) / 4.0, y + (j + 0.5) / 4.0) ? 1 : 0;
      if (!n) continue;
      const double a = n / 16.0;
      u8* p = im.rgba.data() + (size_t(y) * size_t(im.w) + size_t(x)) * 4;
      for (int c = 0; c < 3; c++) p[c] = u8(std::lround(p[c] * (1 - a) + g * a));
    }
}

}  // namespace detail

inline SynthMap makeSynth() {
  using namespace detail;
  SynthMap m;
  m.img.w = m.w;
  m.img.h = m.h;
  m.img.rgba.assign(size_t(m.w) * size_t(m.h) * 4, 255);
  const double islets[][3] = {{228, 52, 3}, {235, 52, 3}, {242, 52, 3}, {249, 52, 3},  // цепочка поперёк протока
                              {60, 20, 3},  {440, 300, 3},                             // островки в море
                              {30, 270, 1}, {20, 160, 1}};                             // крупинки (меньше порога)
  for (int y = 0; y < m.h; y++)
    for (int x = 0; x < m.w; x++) {
      const double px = x + 0.5, py = y + 0.5;
      double land = soft(sdEllipse(px, py, 240, 160, 190, 120), 3);
      land *= 1 - soft(sdCircle(px, py, 240, 75, 28), 3);             // залив
      land *= 1 - soft(sdRect(px, py, 222, -10, 258, 75), 3);         // проток к морю
      land *= 1 - soft(sdCircle(px, py, 330, 185, 20), 2);            // озеро
      land *= 1 - soft(sdRect(px, py, 327.5, 185, 332.5, 290), 1.5);  // река к морю
      for (const auto& is : islets) land = std::max(land, soft(sdCircle(px, py, is[0], is[1], is[2]), 1));
      const double t = 1 - land;
      u8* p = m.img.rgba.data() + (size_t(y) * size_t(m.w) + size_t(x)) * 4;
      p[0] = u8(std::lround(255 * (1 - t)));
      p[1] = u8(std::lround(255 - 217 * t));
      p[2] = 255;
      p[3] = 255;
    }
  // Две слившиеся горы: серое тело и белая шапка внутри него.
  for (double cx : {150.0, 163.0}) {
    const double cy = m.my;
    const Tri mount{cx, cy - 12, cx - 11, cy + 6, cx + 11, cy + 6};
    const Tri cap{cx, cy - 8, cx - 3.5, cy - 3, cx + 3.5, cy - 3};
    paint(m.img, int(cx) - 12, int(cy) - 13, int(cx) + 13, int(cy) + 7, 96, [&](double x, double y) { return inTri(mount, x, y); });
    paint(m.img, int(cx) - 5, int(cy) - 9, int(cx) + 6, int(cy) - 2, 255, [&](double x, double y) { return inTri(cap, x, y); });
  }
  // Замок: контур 1,5 пикселя, внутри белое.
  paint(m.img, 99, 199, 124, 216, 0, [](double x, double y) {
    const bool outer = x >= 100 && x <= 122 && y >= 200 && y <= 214;
    const bool inner = x > 101.5 && x < 120.5 && y > 201.5 && y < 212.5;
    return outer && !inner;
  });
  // Башня — сплошной прямоугольник.
  paint(m.img, 199, 199, 208, 210, 0, [](double x, double y) { return x >= 200 && x <= 206 && y >= 200 && y <= 208; });
  // Пунктир в море: квадраты 4 × 4.
  for (int k = 0; k < 10; k++) {
    const double x0 = 28 + 8 * k;
    paint(m.img, int(x0) - 1, 297, int(x0) + 5, 303, 0, [&](double x, double y) { return x >= x0 && x < x0 + 4 && y >= 298 && y < 302; });
  }
  return m;
}

inline size_t idx(const map::bake::Layers& L, Vec2 p) { return size_t(p.y) * size_t(L.w) + size_t(p.x); }

// Чистая папка для артефактов теста.
inline std::string freshDir(const std::string& name) {
  const std::string d = fs::join(test::outDir(), name);
  fs::removeAll(d);
  fs::makeDirs(d);
  return d;
}

}  // namespace rg::bmtest
