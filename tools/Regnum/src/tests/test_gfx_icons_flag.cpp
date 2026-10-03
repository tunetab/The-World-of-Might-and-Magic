// Regnum — флаги: все узоры, чёткие края полос, размещение эмблемы, изображения (cover, повреждённые данные),
// скругление и рамка, повороты, скорость, наглядные листы.
#include "codec/png.h"
#include "core/world.h"
#include "gfx/emblems.h"
#include "gfx/flag.h"
#include "tests/test_gfx_icons_util.h"

using namespace rg;
using namespace rg::gfx;
using namespace rg::test::icons;

namespace {

const char* const kPatternNames[] = {"solid", "h2", "h3", "v2", "v3", "cross", "saltire", "quarters", "bend", "chevron",
                                     "border", "canton", "chief", "pale"};

Flag makeFlag(FlagPattern p, Color a, Color b, Color c, std::string emblem = {}, Color ec = Color::hex(0xf2e3b3)) {
  Flag f;
  f.pattern = p;
  f.colors = {a, b, c};
  f.emblem = std::move(emblem);
  f.emblemColor = ec;
  return f;
}

Color px(const Image& img, int x, int y) { return unpremul(img.at(x, y)); }
int dist(Color a, Color b) { return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b); }

std::string pngOf(const Image& img) {
  codec::RgbaImage r;
  r.w = img.w;
  r.h = img.h;
  r.rgba = img.toRgba();
  const auto bytes = codec::encodePng(r, 6);
  return std::string(bytes.begin(), bytes.end());
}

}  // namespace

TEST(gfx_icons_flag_all_patterns_fill) {
  CHECK_EQ(int(FlagPattern::Count), int(std::size(kPatternNames)));
  // Каждый узор полностью закрывает прямоугольник (без рамки и скругления) и использует свои цвета.
  const Color a = Color::hex(0xc0392b), b = Color::hex(0xf1c40f), c = Color::hex(0x2c3e50);
  for (int p = 0; p < int(FlagPattern::Count); p++) {
    Image img(90, 60, 0);
    Canvas cv(img);
    drawFlag(cv, makeFlag(FlagPattern(p), a, b, c), {0, 0, 90, 60}, 0, false);
    const Coverage cov = coverage(img);
    CHECK_MSG(cov.sum > 90 * 60 - 1, std::string(kPatternNames[p]) + ": не закрыт");
    // Цвета узора присутствуют (с учётом лёгкой светотени).
    auto has = [&](Color col) {
      for (u32 v : img.px)
        if (dist(unpremul(v), col) < 60) return true;
      return false;
    };
    CHECK_MSG(has(a), std::string(kPatternNames[p]) + ": нет c0");
    const bool usesB = FlagPattern(p) != FlagPattern::Solid;
    const bool usesC = FlagPattern(p) == FlagPattern::H3 || FlagPattern(p) == FlagPattern::V3 || FlagPattern(p) == FlagPattern::Cross ||
                       FlagPattern(p) == FlagPattern::Saltire || FlagPattern(p) == FlagPattern::Bend || FlagPattern(p) == FlagPattern::Chevron;
    if (usesB) CHECK_MSG(has(b), std::string(kPatternNames[p]) + ": нет c1");
    if (usesC) CHECK_MSG(has(c), std::string(kPatternNames[p]) + ": нет c2");
    if (!usesC) CHECK_MSG(!has(c), std::string(kPatternNames[p]) + ": лишний c2");
  }
}

TEST(gfx_icons_flag_crisp_stripes) {
  // Полосы ложатся на пиксели: на границе нет строк/столбцов со смешанным цветом.
  const Color a = Color::hex(0xffffff), b = Color::hex(0x000000), c = Color::hex(0xff0000);
  for (int hgt : {16, 24, 25, 40, 61}) {
    const int wid = hgt * 3 / 2;
    for (FlagPattern p : {FlagPattern::H2, FlagPattern::H3, FlagPattern::V2, FlagPattern::V3, FlagPattern::Pale, FlagPattern::Chief}) {
      Image img(wid + 4, hgt + 4, 0);
      Canvas cv(img);
      drawFlag(cv, makeFlag(p, a, b, c), {2.3f, 1.8f, float(wid), float(hgt)}, 0, false);
      const bool vertical = p == FlagPattern::V2 || p == FlagPattern::V3 || p == FlagPattern::Pale;
      const int n = vertical ? wid : hgt;
      for (int i = 0; i < n; i++) {
        const Color q = vertical ? px(img, 2 + i, 2 + hgt / 2) : px(img, 2 + wid / 2, 2 + i);
        const int best = std::min({dist(q, a), dist(q, b), dist(q, c)});
        CHECK_MSG(best < 90, "узор " + std::to_string(int(p)) + ", высота " + std::to_string(hgt) + ", позиция " + std::to_string(i) +
                                 ": смешанный цвет " + q.toHex());
      }
    }
  }
  // Края флага — по пикселям (без полупрозрачной каймы при дробных координатах).
  Image img(40, 30, 0);
  Canvas cv(img);
  drawFlag(cv, makeFlag(FlagPattern::Solid, Color::hex(0x336699), {}, {}), {5.4f, 4.6f, 30, 20}, 0, false);
  for (u32 v : img.px) {
    const u32 al = v >> 24;
    CHECK(al == 0 || al == 255);
  }
}

TEST(gfx_icons_flag_emblem_and_contrast) {
  // Эмблема видна по центру; при цвете эмблемы как у поля — обводка контрастным цветом узора.
  const Color field = Color::hex(0x1f4e79);
  Image plain(150, 100, 0), with(150, 100, 0), same(150, 100, 0);
  {
    Canvas c(plain);
    drawFlag(c, makeFlag(FlagPattern::Solid, field, field, field), {0, 0, 150, 100}, 0, false);
  }
  {
    Canvas c(with);
    drawFlag(c, makeFlag(FlagPattern::Solid, field, field, field, "star", Color::hex(0xf2e3b3)), {0, 0, 150, 100}, 0, false);
  }
  const Color center = px(with, 75, 52);
  CHECK(dist(center, Color::hex(0xf2e3b3)) < 60);
  CHECK(dist(px(with, 5, 5), px(plain, 5, 5)) < 4);
  {
    // Кремовая эмблема на кремовом кресте — с тёмной обводкой.
    Canvas c(same);
    drawFlag(c, makeFlag(FlagPattern::Pale, Color::hex(0x2c3e50), Color::hex(0xf2e3b3), Color::hex(0x2c3e50), "star",
                         Color::hex(0xf2e3b3)),
             {0, 0, 150, 100}, 0, false);
  }
  int dark = 0;
  for (int y = 20; y < 80; y++)
    for (int x = 55; x < 95; x++)
      if (dist(px(same, x, y), Color::hex(0x2c3e50)) < 80) dark++;
  CHECK(dark > 40);
  // Неизвестная эмблема — поле без изменений.
  Image unk(150, 100, 0);
  Canvas cu(unk);
  drawFlag(cu, makeFlag(FlagPattern::Solid, field, field, field, "no-such"), {0, 0, 150, 100}, 0, false);
  CHECK(unk.px == plain.px);
}

TEST(gfx_icons_flag_image_cover_and_fallback) {
  clearFlagCache();
  // Изображение 200 × 100: левая половина красная, правая синяя, центр — белая полоса.
  Image src(200, 100);
  for (int y = 0; y < 100; y++)
    for (int x = 0; x < 200; x++) src.row(y)[x] = premul(x < 100 ? Color(220, 20, 20) : Color(20, 40, 220));
  for (int y = 0; y < 100; y++)
    for (int x = 98; x < 102; x++) src.row(y)[x] = premul(Color(255, 255, 255));
  Flag f;
  f.image = true;
  f.png = pngOf(src);
  // Квадрат 60 × 60: «cover» обрезает края, видна середина (красное | синее).
  Image img(60, 60, 0);
  Canvas c(img);
  drawFlag(c, f, {0, 0, 60, 60}, 0, false);
  CHECK(dist(px(img, 10, 30), Color(220, 20, 20)) < 50);
  CHECK(dist(px(img, 50, 30), Color(20, 40, 220)) < 50);
  CHECK(px(img, 30, 30).r > 150 && px(img, 30, 30).b > 150);
  CHECK(flagCacheSize() >= 1);
  // Повторная отрисовка из кеша — тот же результат.
  Image again(60, 60, 0);
  Canvas c2(again);
  drawFlag(c2, f, {0, 0, 60, 60}, 0, false);
  CHECK(again.px == img.px);
  // С поворотом холста — без кеша по размеру, но рисуется.
  Image rot(80, 80, 0);
  Canvas cr(rot);
  cr.translate(40, 40);
  cr.concat(Affine::rotate(0.3f));
  drawFlag(cr, f, {-25, -25, 50, 50}, 4, true);
  CHECK(coverage(rot).sum > 50 * 50 * 0.9);
  // Повреждённые данные: узор вместо изображения.
  Flag bad = makeFlag(FlagPattern::Solid, Color::hex(0x2f5d3a), {}, {});
  bad.image = true;
  bad.png = "\x89PNG\r\n\x1a\nbroken";
  Image fb(30, 20, 0);
  Canvas cb(fb);
  drawFlag(cb, bad, {0, 0, 30, 20}, 0, false);
  CHECK(dist(px(fb, 15, 10), Color::hex(0x2f5d3a)) < 40);
  // Пустые данные изображения — тоже узор.
  bad.png.clear();
  drawFlag(cb, bad, {0, 0, 30, 20}, 0, false);
  CHECK(dist(px(fb, 15, 10), Color::hex(0x2f5d3a)) < 40);
  clearFlagCache();
  CHECK_EQ(flagCacheSize(), size_t(0));
}

TEST(gfx_icons_flag_round_frame_degenerate) {
  Image img(60, 40, 0);
  Canvas c(img);
  drawFlag(c, makeFlag(FlagPattern::Solid, Color::hex(0xc0392b), {}, {}), {0, 0, 60, 40}, 8, true);
  CHECK((img.at(0, 0) >> 24) == 0);                       // скруглённый угол
  CHECK((img.at(30, 20) >> 24) == 255);
  const Color edge = px(img, 30, 0), inner = px(img, 30, 20);
  CHECK(edge.r < inner.r);                                // тёмная кромка рамки
  // Вырожденные размеры и нечисловые значения — без падений и рисования.
  Image e(20, 20, 0);
  Canvas ce(e);
  const Flag f = makeFlag(FlagPattern::Saltire, Color::hex(0xffffff), Color::hex(0x000000), Color::hex(0xff0000), "lion");
  drawFlag(ce, f, {0, 0, 0, 10}, 2, true);
  drawFlag(ce, f, {0, 0, 10, -1}, 2, true);
  drawFlag(ce, f, {std::nanf(""), 0, 10, 10}, 2, true);
  drawFlag(ce, f, {0, 0, 10, 10}, std::nanf(""), true);
  CHECK(coverage(e).sum > 50);
  drawFlag(ce, f, {0, 0, 1e30f, 1e30f}, 1e30f, true);
}

TEST(gfx_icons_flag_speed) {
  Image img(1200, 800);
  Canvas c(img);
  c.clear(Color::hex(0x151a22));
  const auto& em = emblemNames();
  double t0 = nowSeconds();
  int n = 0;
  for (int i = 0; i < 1000; i++) {
    const Flag f = makeFlag(FlagPattern(i % int(FlagPattern::Count)), Color::palette(i), Color::palette(i + 7), Color::palette(i + 3),
                            em[size_t(i) % em.size()]);
    drawFlag(c, f, {float(i % 30) * 40.f, float(i / 30) * 24.f, 36, 24}, 3, true);
    n++;
  }
  const double ms = (nowSeconds() - t0) * 1000;
  std::printf("  флаги: %d шт. 36×24 с эмблемой за %.1f мс (%.1f мкс/шт.)\n", n, ms, ms * 1000 / n);
  CHECK(ms < 1500);
}

TEST(gfx_icons_flag_sheet) {
  struct Combo {
    Color a, b, c, e;
  };
  const Combo combos[] = {
      {Color::hex(0x7a4fd6), Color::hex(0xf2e3b3), Color::hex(0x1d2333), Color::hex(0xf2e3b3)},
      {Color::hex(0x8e1b25), Color::hex(0xe8c36a), Color::hex(0x14202e), Color::hex(0xe8c36a)},
      {Color::hex(0x1f4e79), Color::hex(0xffffff), Color::hex(0xc8102e), Color::hex(0xffffff)},
      {Color::hex(0x2f5d3a), Color::hex(0xd9c27a), Color::hex(0x6b2d14), Color::hex(0x10140f)},
      {Color::hex(0xf4f1e8), Color::hex(0x1b1b1b), Color::hex(0xb3202a), Color::hex(0x1b1b1b)},
  };
  const char* emblems[] = {"lion", "eagle", "crown", "tower", "dragon", "star", "rose", "wolf", "ship", "sun", "skull", "lily", "bull", "kraken"};
  const int cols = int(FlagPattern::Count), fw = 96, fh = 64, gap = 14;
  const int rows = int(std::size(combos));
  Image img(cols * (fw + gap) + gap, rows * (fh + gap) + gap + 22 + 120);
  Canvas c(img);
  c.clear(Color::hex(0x151a22));
  for (int p = 0; p < cols; p++) label(c, kPatternNames[p], float(gap + p * (fw + gap)), 4, Color(255, 255, 255, 150), 11);
  for (int r = 0; r < rows; r++)
    for (int p = 0; p < cols; p++) {
      const Combo& k = combos[r];
      const Flag f = makeFlag(FlagPattern(p), k.a, k.b, k.c, r == 4 && p % 3 == 0 ? "" : emblems[(p + r * 3) % std::size(emblems)], k.e);
      drawFlag(c, f, {float(gap + p * (fw + gap)), float(22 + gap + r * (fh + gap)), float(fw), float(fh)}, 5, true);
    }
  // Ряд размеров: от значка в списке до крупного.
  float x = float(gap);
  const float y = float(22 + gap + rows * (fh + gap));
  const Flag big = makeFlag(FlagPattern::Chief, Color::hex(0x8e1b25), Color::hex(0xe8c36a), {}, "lion", Color::hex(0xe8c36a));
  for (int h : {12, 16, 20, 24, 32, 48, 72, 100}) {
    drawFlag(c, big, {x, y, float(h) * 1.5f, float(h)}, h >= 24 ? 4.f : 2.f, true);
    x += float(h) * 1.5f + 12;
  }
  savePng(img, "gfx_flags");
  savePng(zoomed(img.cropped(RectI{gap - 2, int(y) - 2, 250, 52}), 4), "gfx_flags_zoom_small");
  savePng(zoomed(img.cropped(RectI{gap - 2, 22 + gap - 2, 330, fh + 4}), 3), "gfx_flags_zoom_patterns");
  // Все эмблемы на флагах 3:2.
  const auto& em = emblemNames();
  const int ec = 9;
  Image all(ec * 110 + 10, int((em.size() + ec - 1) / ec) * 80 + 10);
  Canvas ca(all);
  ca.clear(Color::hex(0xeef2f7));
  for (size_t i = 0; i < em.size(); i++) {
    const Combo& k = combos[i % std::size(combos)];
    const Flag f = makeFlag(FlagPattern(i % size_t(FlagPattern::Count)), k.a, k.b, k.c, em[i], k.e);
    drawFlag(ca, f, {10.f + float(i % ec) * 110, 10.f + float(i / ec) * 80, 96, 64}, 4, true);
  }
  savePng(all, "gfx_flags_emblems");
}
