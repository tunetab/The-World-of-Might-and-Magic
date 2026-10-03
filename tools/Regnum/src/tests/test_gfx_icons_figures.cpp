// Regnum — фигурки карты: габариты, кеш и совпадение с векторной отрисовкой, поворот, скорость,
// наглядный лист на светлой карте с морем.
#include "gfx/figures.h"
#include "tests/test_gfx_icons_util.h"

using namespace rg;
using namespace rg::gfx;
using namespace rg::test::icons;

namespace {

const Color kFactions[] = {Color::hex(0xc0392b), Color::hex(0x2e86de), Color::hex(0x27ae60), Color::hex(0x8e44ad),
                           Color::hex(0xf1c40f), Color::hex(0x1d2333), Color::hex(0xf5f5f5), Color::hex(0xe67e22)};

// Фон «карта»: светлая суша и голубое море с береговой линией.
void mapBackground(Canvas& c, int w, int h) {
  c.clear(Color::hex(0xf4f1ea));
  Path sea;
  sea.moveTo(0, float(h) * 0.55f);
  for (int x = 0; x <= w; x += 8)
    sea.lineTo(float(x), float(h) * 0.55f + 18 * std::sin(float(x) * 0.013f) + 9 * std::sin(float(x) * 0.041f + 1));
  sea.lineTo(float(w), float(h));
  sea.lineTo(0, float(h));
  sea.close();
  c.fillPath(sea, Color::hex(0xa9cbe8));
  Stroke coast;
  coast.width = 1.5f;
  c.strokePath(sea, coast, Color::hex(0x7aa6cf));
  // Границы провинций.
  for (int i = 1; i < 6; i++) c.line(float(i) * float(w) / 6, 0, float(i) * float(w) / 6 + 30, float(h) * 0.5f, 1, Color(120, 110, 95, 90));
}

}  // namespace

TEST(gfx_icons_figures_bounds) {
  // Всё нарисованное помещается в figureBounds / markerBounds.
  const float size = 60;
  for (int kind = 0; kind < 5; kind++)
    for (int sel = 0; sel < 2; sel++) {
      Image img(200, 200, 0);
      Canvas c(img);
      const Pt ctr{100.3f, 99.6f};
      RectF b;
      switch (kind) {
        case 0: drawArmyFigure(c, ctr, size, kFactions[0], sel, sel, kFactions[1]); b = figureBounds(ctr, size); break;
        case 1: drawFleetFigure(c, ctr, size, kFactions[1], sel, sel, kFactions[2]); b = figureBounds(ctr, size); break;
        case 2: drawCapitalMarker(c, ctr, size, kFactions[2]); b = markerBounds(ctr, size); break;
        case 3: drawHqMarker(c, ctr, size, kFactions[3]); b = markerBounds(ctr, size); break;
        default: drawBattleMarker(c, ctr, size); b = markerBounds(ctr, size); break;
      }
      const Coverage cv = coverage(img, 8);
      CHECK(cv.any());
      CHECK_MSG(cv.x0 >= int(std::floor(b.x)) - 1 && cv.y0 >= int(std::floor(b.y)) - 1 && cv.x1 <= int(std::ceil(b.right())) &&
                    cv.y1 <= int(std::ceil(b.bottom())),
                "вид " + std::to_string(kind) + ": выход за габариты");
      // Центр постамента — в точке карты (по горизонтали симметрично).
      CHECK(std::fabs((cv.x0 + cv.x1 + 1) * 0.5 - ctr.x) < size * 0.2);
    }
}

TEST(gfx_icons_figures_cache_matches_vector) {
  clearFigureCache();
  CHECK_EQ(figureCacheSize(), size_t(0));
  // Через кеш (без поворота) и напрямую (поворот на ничтожный угол — векторный путь) — почти одно и то же.
  Image a(120, 120, 0), b(120, 120, 0);
  {
    Canvas c(a);
    drawArmyFigure(c, {60, 64}, 64, kFactions[0], true, true, kFactions[1]);
  }
  CHECK(figureCacheSize() == 1);
  {
    Canvas c(b);
    c.translate(60, 64);
    c.concat(Affine::rotate(1e-6f));
    drawArmyFigure(c, {0, 0}, 64, kFactions[0], true, true, kFactions[1]);
  }
  double diff = 0;
  for (size_t i = 0; i < a.px.size(); i++) {
    const Color p = unpremul(a.px[i]), q = unpremul(b.px[i]);
    diff += std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b) + std::abs(p.a - q.a);
  }
  CHECK_MSG(diff / double(a.px.size()) < 3.0, "среднее расхождение " + fmtNum(diff / double(a.px.size()), 2));
  // Повторная отрисовка — из кеша, тот же результат; другой цвет — новая запись.
  Image a2(120, 120, 0);
  Canvas c2(a2);
  drawArmyFigure(c2, {60, 64}, 64, kFactions[0], true, true, kFactions[1]);
  CHECK(a2.px == a.px);
  CHECK(figureCacheSize() == 1);
  drawArmyFigure(c2, {60, 64}, 64, kFactions[2]);
  CHECK(figureCacheSize() == 2);
  // Масштаб холста учитывается: размер на устройстве удваивается.
  Image s(140, 140, 0);
  Canvas cs(s);
  cs.scale(2, 2);
  drawFleetFigure(cs, {35, 35}, 40, kFactions[1]);
  const Coverage cv = coverage(s);
  CHECK(cv.x1 - cv.x0 > 80);
  clearFigureCache();
  CHECK_EQ(figureCacheSize(), size_t(0));
}

TEST(gfx_icons_figures_degenerate) {
  Image img(40, 40, 0);
  Canvas c(img);
  drawArmyFigure(c, {20, 20}, 0, kFactions[0]);
  drawArmyFigure(c, {20, 20}, -5, kFactions[0]);
  drawArmyFigure(c, {std::nanf(""), 20}, 30, kFactions[0]);
  drawFleetFigure(c, {20, 20}, std::nanf(""), kFactions[0]);
  drawHqMarker(c, {1e30f, 20}, 30, kFactions[0]);
  drawArmyFigure(c, {20, 20}, 30, Color(0, 0, 0, 0));
  CHECK(!coverage(img).any());
  drawCapitalMarker(c, {20, 20}, 1e6f, kFactions[0]);  // огромный — рисуется векторно, видимая часть
  CHECK(coverage(img).any());
}

TEST(gfx_icons_figures_speed) {
  clearFigureCache();
  Image img(1600, 900);
  Canvas c(img);
  c.clear(Color::hex(0xf4f1ea));
  double t0 = nowSeconds();
  for (int i = 0; i < 64; i++) drawArmyFigure(c, {float(40 + (i % 16) * 90), float(60 + (i / 16) * 90)}, 36, kFactions[i % 8]);
  const double cold = (nowSeconds() - t0) * 1000;
  t0 = nowSeconds();
  int n = 0;
  for (int i = 0; i < 2000; i++, n++) {
    const Pt p{float(20 + (i * 37) % 1560), float(20 + (i * 53) % 860)};
    if (i % 2) drawArmyFigure(c, p, 36, kFactions[i % 8]);
    else drawFleetFigure(c, p, 36, kFactions[i % 8]);
  }
  const double warm = (nowSeconds() - t0) * 1000;
  std::printf("  фигурки: первые 64 (растеризация) %.1f мс, %d из кеша %.1f мс (%.1f мкс/шт.), записей %zu\n", cold, n, warm,
              warm * 1000 / n, figureCacheSize());
  CHECK(warm < 600);
}

TEST(gfx_icons_figures_sheet) {
  const int W = 1500, H = 820;
  Image img(W, H);
  Canvas c(img);
  mapBackground(c, W, H);
  const float sizes[] = {22, 28, 36, 48, 64, 96};
  // Ряды: войско (суша), флот (море), выбранные и союзные, метки.
  float x = 40;
  for (float s : sizes) {
    for (int k = 0; k < 2; k++) drawArmyFigure(c, {x + float(k) * (s + 14), 70}, s, kFactions[k * 3]);
    drawArmyFigure(c, {x, 70 + s + 40}, s, kFactions[1], true);
    drawArmyFigure(c, {x + s + 14, 70 + s + 40}, s, kFactions[0], false, true, kFactions[1]);
    x += 2 * s + 60;
  }
  x = 40;
  for (float s : sizes) {
    for (int k = 0; k < 2; k++) drawFleetFigure(c, {x + float(k) * (s + 14), 520}, s, kFactions[k * 3 + 1]);
    drawFleetFigure(c, {x, 520 + s + 40}, s, kFactions[2], true);
    drawFleetFigure(c, {x + s + 14, 520 + s + 40}, s, kFactions[4], false, true, kFactions[5]);
    x += 2 * s + 60;
  }
  // Все цвета фракций на 32 px.
  for (int i = 0; i < 8; i++) {
    drawArmyFigure(c, {float(60 + i * 48), 330}, 32, kFactions[i]);
    drawFleetFigure(c, {float(60 + i * 48), 790}, 32, kFactions[i]);
  }
  // Метки: столица, штаб, битва.
  x = 520;
  for (float s : {16.f, 22.f, 30.f, 44.f, 64.f}) {
    drawCapitalMarker(c, {x, 330}, s, kFactions[0]);
    drawHqMarker(c, {x, 400}, s, kFactions[3]);
    drawBattleMarker(c, {x + s + 10, 365}, s);
    x += 2 * s + 40;
  }
  // Крупно (векторный путь, без кеша).
  drawArmyFigure(c, {1180, 200}, 200, kFactions[0]);
  drawFleetFigure(c, {1180, 610}, 200, kFactions[1], false, true, kFactions[4]);
  savePng(img, "gfx_figures");
  savePng(img.cropped(RectI{1040, 40, 300, 300}), "gfx_figures_big_army");
  savePng(zoomed(img.cropped(RectI{10, 30, 260, 150}), 4), "gfx_figures_zoom_small");
}
