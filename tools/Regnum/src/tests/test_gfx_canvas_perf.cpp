// Regnum — замеры скорости холста (один поток). Печатаются лучшие времена из нескольких прогонов;
// в сборке release проверяется запас 3× к целевым значениям (защита от регрессий; замеры шумят при
// параллельной нагрузке на машину, поэтому берётся лучшее из многих прогонов).
#include <cstdio>

#include "gfx/canvas.h"
#include "gfx/raster.h"
#include "tests/test.h"

using namespace rg;
using namespace rg::gfx;

namespace {

template <class F>
double bestMs(int runs, F&& f) {
  double best = 1e30;
  for (int i = 0; i < runs; i++) {
    double t0 = nowSeconds();
    f();
    best = std::min(best, (nowSeconds() - t0) * 1000.0);
  }
  return best;
}

void report(const char* what, double ms, double target) {
  std::printf("  [gfx perf] %-58s %7.2f ms (цель < %.0f)\n", what, ms, target);
#ifdef NDEBUG
  CHECK_MSG(ms < target * 3, strf("%s: %.2f ms", what, ms));
#endif
}

}  // namespace

TEST(gfx_canvas_perf_shapes) {
  Image img(1920, 1080);
  Canvas c(img);
  auto frame = [&] {
    Rng rng(1234);
    c.clear(Color::hex(0x1e2533));
    for (int i = 0; i < 400; i++) {
      float w = float(10 + rng.uniform() * 190), h = float(10 + rng.uniform() * 190);
      float x = float(rng.uniform() * (1920 - w)), y = float(rng.uniform() * (1080 - h));
      Color col(u8(rng.range(0, 255)), u8(rng.range(0, 255)), u8(rng.range(0, 255)), u8(rng.range(160, 255)));
      c.fillRoundRect({x, y, w, h}, float(rng.uniform() * 20), col);
    }
    for (int i = 0; i < 400; i++) {
      float r = float(4 + rng.uniform() * 60);
      Color col(u8(rng.range(0, 255)), u8(rng.range(0, 255)), u8(rng.range(0, 255)), u8(rng.range(160, 255)));
      c.fillCircle(float(rng.uniform() * 1920), float(rng.uniform() * 1080), r, col);
    }
  };
  report("1920×1080: 400 скруглённых прямоугольников + 400 кругов", bestMs(15, frame), 15);
}

TEST(gfx_canvas_perf_polyline) {
  Image img(1920, 1080);
  Canvas c(img);
  Stroke s;
  s.width = 2;
  s.join = Join::Miter;
  // График во всю ширину кадра: 10 000 отрезков.
  std::vector<Pt> chart;
  Rng rng(5);
  for (int i = 0; i <= 10000; i++)
    chart.push_back({i * 1920.f / 10000, float(540 + 380 * std::sin(i * 0.013) * std::cos(i * 0.0021) + (rng.uniform() - 0.5) * 6)});
  double ms = bestMs(15, [&] {
    img.clear();
    c.polyline(chart.data(), chart.size(), false, s, Color(255, 255, 255));
  });
  report("обводка ломаной 10 000 отрезков (график), ширина 2", ms, 6);
  // Случайное блуждание шагом 4 px (граница области на карте).
  std::vector<Pt> walk;
  double x = 960, y = 540, dir = 0;
  for (int i = 0; i <= 10000; i++) {
    dir += (rng.uniform() - 0.5) * 0.8;
    x = std::clamp(x + 4 * std::cos(dir), 20.0, 1900.0);
    y = std::clamp(y + 4 * std::sin(dir), 20.0, 1060.0);
    walk.push_back({float(x), float(y)});
  }
  ms = bestMs(15, [&] {
    img.clear();
    c.polyline(walk.data(), walk.size(), false, s, Color(255, 255, 255));
  });
  report("обводка ломаной 10 000 отрезков (блуждание 4 px), ширина 2", ms, 6);
  s.join = Join::Round;
  s.cap = Cap::Round;
  ms = bestMs(5, [&] {
    img.clear();
    c.polyline(walk.data(), walk.size(), false, s, Color(255, 255, 255));
  });
  std::printf("  [gfx perf] %-58s %7.2f ms\n", "  то же, круглые соединения и концы", ms);
  Stroke d = s;
  d.dash = {8, 4};
  ms = bestMs(5, [&] {
    img.clear();
    c.polyline(walk.data(), walk.size(), false, d, Color(255, 255, 255));
  });
  std::printf("  [gfx perf] %-58s %7.2f ms\n", "  то же, пунктир 8/4", ms);
}

TEST(gfx_canvas_perf_draw_image) {
  Image src(2048, 2048);
  Rng rng(2);
  for (int y = 0; y < 2048; y++)
    for (int x = 0; x < 2048; x++)
      src.px[size_t(y) * 2048 + size_t(x)] = premul(Color(u8(x ^ y), u8((x * 3) >> 4), u8(rng.next() & 255)));
  Image img(1920, 1080);
  Canvas c(img);
  double ms = bestMs(15, [&] { c.drawImage(src, {0, 0, 1920, 1080}); });
  report("drawImage 2048×2048 -> 1920×1080, билинейно", ms, 12);
  ms = bestMs(5, [&] { c.drawImage(src, {0, 0, 1920, 1080}, 0.6f); });
  std::printf("  [gfx perf] %-58s %7.2f ms\n", "  то же, прозрачность 0,6", ms);
  ms = bestMs(5, [&] { c.drawImage(src, {0, 0, 2048, 2048}, {0, 0, 1920, 1080}, 1, false); });
  std::printf("  [gfx perf] %-58s %7.2f ms\n", "  то же, ближайший сосед", ms);
  Image tile = src.cropped({0, 0, 1920, 1080});
  ms = bestMs(5, [&] { c.drawImage(tile, {0, 0, 1920, 1080}); });
  std::printf("  [gfx perf] %-58s %7.2f ms\n", "  копия 1:1 1920×1080", ms);
  ms = bestMs(5, [&] {
    c.save();
    c.translate(960, 540);
    c.concat(Affine::rotate(0.3f));
    c.drawImage(src, {-1100, -700, 2200, 1400});
    c.restore();
  });
  std::printf("  [gfx perf] %-58s %7.2f ms\n", "  поворот 0,3 рад, на весь кадр", ms);
  ms = bestMs(3, [&] { c.drawImage(src, {0, 0, 480, 270}); });
  std::printf("  [gfx perf] %-58s %7.2f ms\n", "  уменьшение до 480×270 (площадной фильтр)", ms);
}

TEST(gfx_canvas_perf_fill_mask) {
  // Маски, похожие на глифы 10×14.
  std::vector<Mask> glyphs;
  Rng rng(8);
  for (int g = 0; g < 32; g++) {
    Mask m(10, 14);
    Path p;
    p.addRoundRect({1.f + g % 3, 2, 7, 10}, 2);
    p.addCircle(5, 7, 2.f + float(g % 2));
    rasterizeToMask(p, {}, m, 0, 0, FillRule::EvenOdd);
    glyphs.push_back(std::move(m));
  }
  std::vector<Pt> pos;
  for (int i = 0; i < 3000; i++) pos.push_back({float(rng.uniform() * 1900), float(rng.uniform() * 1060)});
  Image img(1920, 1080);
  Canvas c(img);
  const Paint ink(Color(230, 235, 245));
  double ms = bestMs(25, [&] {
    for (int i = 0; i < 3000; i++) c.fillMask(glyphs[size_t(i) % glyphs.size()], pos[size_t(i)].x, pos[size_t(i)].y, ink);
  });
  report("3000 × fillMask 10×14 (глифы)", ms, 2);
}

TEST(gfx_canvas_perf_effects) {
  Image img(1920, 1080);
  Canvas c(img);
  Gradient g;
  g.p0 = {0, 0};
  g.p1 = {1920, 1080};
  g.stops = {{0.f, Color::hex(0x1e2533)}, {1.f, Color::hex(0x5b84ff)}};
  Paint gp;
  gp.gradient = &g;
  double ms = bestMs(5, [&] { c.fillRect({0, 0, 1920, 1080}, gp); });
  std::printf("  [gfx perf] %-58s %7.2f ms\n", "линейный градиент на весь кадр", ms);
  ms = bestMs(5, [&] {
    for (int i = 0; i < 40; i++) c.boxShadow({float(40 + (i % 8) * 230), float(40 + (i / 8) * 200), 200, 160}, 14, 36, 0, Color(0, 0, 0, 115), {0, 14});
  });
  std::printf("  [gfx perf] %-58s %7.2f ms\n", "40 теней карточек 200×160 (из кеша)", ms);
  ms = bestMs(5, [&] { c.boxShadow({100, 100, 1500, 800}, 14, 36, 0, Color(0, 0, 0, 115), {0, 14}); });
  std::printf("  [gfx perf] %-58s %7.2f ms\n", "тень панели 1500×800", ms);
  ms = bestMs(5, [&] { c.blurRegion({200, 200, 400, 600}, 24); });
  std::printf("  [gfx perf] %-58s %7.2f ms\n", "blurRegion 400×600, радиус 24", ms);
  ms = bestMs(5, [&] {
    for (int i = 0; i < 2000; i++) c.fillRect({float((i * 37) % 1900), float((i * 53) % 1060), 18, 12}, Color(200, 210, 220, 200));
  });
  std::printf("  [gfx perf] %-58s %7.2f ms\n", "2000 × fillRect 18×12", ms);
}
