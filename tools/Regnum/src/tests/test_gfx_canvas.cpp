// Regnum — тесты холста: смешивание, прозрачность, преобразования, отсечение, обводка, пунктир, изображения,
// градиенты, маски, тени, размытие, устойчивость к выходу за границы.
#include "gfx/canvas.h"
#include "gfx/raster.h"
#include "gfx/stroke.h"
#include "tests/test.h"

using namespace rg;
using namespace rg::gfx;

namespace {

u32 raw(const Image& img, int x, int y) { return img.at(x, y); }
int A(u32 p) { return int(p >> 24); }
int R(u32 p) { return int((p >> 16) & 255); }
int G(u32 p) { return int((p >> 8) & 255); }
int B(u32 p) { return int(p & 255); }

// Площадь покрытия: рисуем белым по прозрачному и суммируем альфу.
double alphaArea(const Image& img) {
  u64 s = 0;
  for (u32 p : img.px) s += p >> 24;
  return double(s) / 255.0;
}

bool nearPx(u32 p, int a, int r, int g, int b, int tol = 1) {
  return std::abs(A(p) - a) <= tol && std::abs(R(p) - r) <= tol && std::abs(G(p) - g) <= tol && std::abs(B(p) - b) <= tol;
}

std::string pxs(u32 p) { return strf("(a%d r%d g%d b%d)", A(p), R(p), G(p), B(p)); }

}  // namespace

TEST(gfx_canvas_fill_solid) {
  Image img(64, 64);
  Canvas c(img);
  c.clear(Color(255, 255, 255));
  CHECK_EQ(raw(img, 0, 0), 0xFFFFFFFFu);
  c.fillRect({10, 10, 20, 20}, Color(255, 0, 0));
  CHECK_EQ(raw(img, 15, 15), 0xFFFF0000u);
  CHECK_EQ(raw(img, 10, 10), 0xFFFF0000u);
  CHECK_EQ(raw(img, 29, 29), 0xFFFF0000u);
  CHECK_EQ(raw(img, 30, 15), 0xFFFFFFFFu);
  CHECK_EQ(raw(img, 9, 15), 0xFFFFFFFFu);
  // Полупиксельный край: 50 % красного на белом.
  c.fillRect({40.5f, 10, 10, 10}, Color(255, 0, 0));
  CHECK_MSG(nearPx(raw(img, 40, 15), 255, 255, 127, 127), pxs(raw(img, 40, 15)));
  CHECK_EQ(c.width(), 64);
  CHECK_EQ(c.height(), 64);
}

TEST(gfx_canvas_blend_math) {
  Image img(4, 1);
  Canvas c(img);
  // Normal: 50 % красного поверх синего.
  img.px[0] = 0xFF0000FFu;
  c.fillRect({0, 0, 1, 1}, Color(255, 0, 0, 128));
  CHECK_MSG(nearPx(raw(img, 0, 0), 255, 128, 0, 127, 0), pxs(raw(img, 0, 0)));
  // Multiply / Screen / Add по непрозрачным цветам.
  const u32 d = premul(Color(200, 100, 50));
  const Color s(128, 255, 0);
  img.px = {d, d, d, d};
  Paint pm(s);
  pm.blend = Blend::Multiply;
  c.fillRect({0, 0, 1, 1}, pm);
  CHECK_MSG(nearPx(raw(img, 0, 0), 255, 100, 100, 0, 0), pxs(raw(img, 0, 0)));
  Paint ps(s);
  ps.blend = Blend::Screen;
  c.fillRect({1, 0, 1, 1}, ps);
  CHECK_MSG(nearPx(raw(img, 1, 0), 255, 228, 255, 50, 0), pxs(raw(img, 1, 0)));
  Paint pa(s);
  pa.blend = Blend::Add;
  c.fillRect({2, 0, 1, 1}, pa);
  CHECK_MSG(nearPx(raw(img, 2, 0), 255, 255, 255, 50, 0), pxs(raw(img, 2, 0)));
  // Режим состояния применяется, если у краски Normal.
  c.save();
  c.setBlend(Blend::Multiply);
  c.fillRect({3, 0, 1, 1}, s);
  c.restore();
  CHECK_MSG(nearPx(raw(img, 3, 0), 255, 100, 100, 0, 0), pxs(raw(img, 3, 0)));

  // Формулы premultiplied на полупрозрачных цветах (сверка с вычислением в double).
  Rng rng(3);
  for (int k = 0; k < 200; k++) {
    Color sc(u8(rng.range(0, 255)), u8(rng.range(0, 255)), u8(rng.range(0, 255)), u8(rng.range(0, 255)));
    Color dc(u8(rng.range(0, 255)), u8(rng.range(0, 255)), u8(rng.range(0, 255)), u8(rng.range(0, 255)));
    for (Blend bm : {Blend::Normal, Blend::Multiply, Blend::Screen, Blend::Add}) {
      Image one(1, 1, premul(dc));
      Canvas cc(one);
      Paint p(sc);
      p.blend = bm;
      cc.fillRect({0, 0, 1, 1}, p);
      const u32 sp = premul(sc), dp = premul(dc);
      auto ch = [](u32 v, int sh) { return double((v >> sh) & 255) / 255; };
      double sa = ch(sp, 24), da = ch(dp, 24);
      u32 got = one.px[0];
      for (int sh : {24, 16, 8, 0}) {
        double S = ch(sp, sh), D = ch(dp, sh), e = 0;
        switch (bm) {
          case Blend::Normal: e = S + D * (1 - sa); break;
          case Blend::Multiply: e = sh == 24 ? sa + da - sa * da : S * D + S * (1 - da) + D * (1 - sa); break;
          case Blend::Screen: e = S + D - S * D; break;
          case Blend::Add: e = std::min(1.0, S + D); break;
        }
        CHECK_MSG(std::fabs(ch(got, sh) * 255 - e * 255) <= 1.01, strf("blend %d ch %d got %d expect %.2f", int(bm), sh, int((got >> sh) & 255), e * 255));
      }
      // Результат — корректный premultiplied-пиксель.
      CHECK(R(got) <= A(got) && G(got) <= A(got) && B(got) <= A(got));
    }
  }
}

TEST(gfx_canvas_opacity) {
  Image img(10, 10);
  Canvas c(img);
  c.save();
  c.setOpacity(0.5f);
  c.setOpacity(0.5f);  // умножается
  CHECK_NEAR(c.opacity(), 0.25, 1e-6);
  Paint p(Color(255, 255, 255));
  c.fillRect({0, 0, 10, 10}, p);
  c.restore();
  CHECK_NEAR(A(raw(img, 5, 5)), 64, 1);
  CHECK_NEAR(c.opacity(), 1, 1e-6);
  p.opacity = 0.5f;
  img.clear();
  c.fillRect({0, 0, 10, 10}, p);
  CHECK_NEAR(A(raw(img, 5, 5)), 128, 1);
  // Нулевая и нечисловая прозрачность — ничего не рисуется.
  img.clear();
  c.save();
  c.setOpacity(NAN);
  c.fillRect({0, 0, 10, 10}, Color(255, 0, 0));
  c.restore();
  CHECK_EQ(raw(img, 5, 5), 0u);
}

TEST(gfx_canvas_transform_and_clip) {
  Image img(100, 100);
  Canvas c(img);
  c.save();
  c.translate(10, 20);
  c.scale(2, 2);
  c.fillRect({0, 0, 5, 5}, Color(255, 0, 0));
  CHECK_EQ(raw(img, 10, 20), 0xFFFF0000u);
  CHECK_EQ(raw(img, 19, 29), 0xFFFF0000u);
  CHECK_EQ(raw(img, 20, 29), 0u);
  CHECK_EQ(raw(img, 9, 20), 0u);
  // Отсечение прямоугольником в текущем преобразовании.
  c.clipRect({10, 10, 10, 10});  // устройство: (30, 40) – (50, 60)
  RectI cb = c.clipBounds();
  CHECK_EQ(cb, RectI(30, 40, 20, 20));
  CHECK(c.quickReject({0, 0, 5, 5}));
  CHECK(!c.quickReject({12, 12, 1, 1}));
  c.fillRect({-100, -100, 1000, 1000}, Color(0, 255, 0));
  CHECK_EQ(raw(img, 30, 40), 0xFF00FF00u);
  CHECK_EQ(raw(img, 49, 59), 0xFF00FF00u);
  CHECK_EQ(raw(img, 50, 59), 0u);
  CHECK_EQ(raw(img, 29, 45), 0u);
  c.restore();
  CHECK_EQ(c.clipBounds(), RectI(0, 0, 100, 100));
  CHECK(c.transform().isIdentity());
  // Вложенные отсечения пересекаются; пустое пересечение — ничего не рисуется.
  c.save();
  c.clipRect({0, 0, 50, 50});
  c.clipRect({60, 60, 10, 10});
  CHECK(c.clipBounds().empty());
  CHECK(c.quickReject({0, 0, 100, 100}));
  img.clear();
  c.fillRect({0, 0, 100, 100}, Color(255, 255, 255));
  c.restore();
  CHECK_EQ(alphaArea(img), 0.0);
  // restore без save безопасен.
  c.restore();
  c.restore();
  // concat и setTransform.
  c.setTransform(Affine::translate(5, 5));
  c.concat(Affine::scale(2));
  Pt p = c.transform().apply({1, 1});
  CHECK_NEAR(p.x, 7, 1e-6);
  CHECK_NEAR(p.y, 7, 1e-6);
}

TEST(gfx_canvas_clip_mask) {
  Image img(100, 100);
  Canvas c(img);
  // Круглое отсечение: площадь заливки = площадь круга.
  c.save();
  Path circle;
  circle.addCircle(50, 50, 30);
  c.clipPath(circle);
  CHECK_EQ(c.clipBounds(), RectI(20, 20, 60, 60));
  c.fillRect({0, 0, 100, 100}, Color(255, 255, 255));
  c.restore();
  CHECK_NEAR(alphaArea(img), kPi * 900, kPi * 900 * 0.005);
  // Пересечение двух масок: круг ∩ полуплоскость.
  img.clear();
  c.save();
  c.clipPath(circle);
  Path half;
  half.addRect({0, 0, 50, 100});
  c.clipPath(half);
  c.fillRect({0, 0, 100, 100}, Color(255, 255, 255));
  c.restore();
  CHECK_NEAR(alphaArea(img), kPi * 450, kPi * 450 * 0.006);
  // Маска + прямоугольник.
  img.clear();
  c.save();
  c.clipRoundRect({10, 10, 80, 80}, 20);
  c.clipRect({0, 0, 50, 100});
  c.fillRect({0, 0, 100, 100}, Color(255, 255, 255));
  c.restore();
  double rr = 80.0 * 80 - (4 - kPi) * 400;
  CHECK_NEAR(alphaArea(img), rr / 2, rr / 2 * 0.006);
  // Повёрнутый прямоугольник отсечения — сглаженная маска той же площади.
  img.clear();
  c.save();
  c.translate(50, 50);
  c.concat(Affine::rotate(0.5f));
  c.clipRect({-20, -20, 40, 40});
  c.setTransform({});
  c.fillRect({0, 0, 100, 100}, Color(255, 255, 255));
  c.restore();
  CHECK_NEAR(alphaArea(img), 1600, 1600 * 0.003);
  // Пустой контур отсечения скрывает всё.
  c.save();
  c.clipPath(Path{});
  CHECK(c.clipBounds().empty());
  c.restore();
}

TEST(gfx_canvas_stroke_width) {
  Image img(100, 100);
  Canvas c(img);
  c.line(10, 50, 90, 50, 3, Color(255, 255, 255), Cap::Butt);
  double col = 0;
  for (int y = 0; y < 100; y++) col += A(raw(img, 50, y)) / 255.0;
  CHECK_NEAR(col, 3.0, 0.01);
  CHECK_NEAR(alphaArea(img), 80 * 3, 0.4);  // покрытие 0,5 -> 128/255
  img.clear();
  c.line(50, 10, 50, 90, 5, Color(255, 255, 255), Cap::Butt);
  double row = 0;
  for (int x = 0; x < 100; x++) row += A(raw(img, x, 50)) / 255.0;
  CHECK_NEAR(row, 5.0, 0.01);
  // Наклонная линия: площадь = длина × ширина.
  img.clear();
  c.line(10, 20, 80, 70, 4, Color(255, 255, 255), Cap::Butt);
  double len = std::sqrt(70.0 * 70 + 50 * 50);
  CHECK_NEAR(alphaArea(img), len * 4, len * 4 * 0.003);
  // Толщина масштабируется преобразованием.
  img.clear();
  c.save();
  c.scale(2, 2);
  c.line(5, 25, 45, 25, 1.5f, Color(255, 255, 255), Cap::Butt);
  c.restore();
  CHECK_NEAR(alphaArea(img), 80 * 3, 0.4);  // покрытие 0,5 -> 128/255
}

TEST(gfx_canvas_stroke_caps) {
  Image img(100, 100);
  Canvas c(img);
  const Color w(255, 255, 255);
  c.line(20, 50, 60, 50, 10, w, Cap::Butt);
  CHECK_NEAR(alphaArea(img), 400, 0.3);
  img.clear();
  c.line(20, 50, 60, 50, 10, w, Cap::Square);
  CHECK_NEAR(alphaArea(img), 500, 0.3);
  img.clear();
  c.line(20, 50, 60, 50, 10, w, Cap::Round);
  CHECK_NEAR(alphaArea(img), 400 + kPi * 25, 0.6);
  // Отрезок нулевой длины: точка с круглым/квадратным концом, пусто с плоским.
  img.clear();
  c.line(50, 50, 50, 50, 10, w, Cap::Round);
  CHECK_NEAR(alphaArea(img), kPi * 25, 0.4);
  img.clear();
  c.line(50, 50, 50, 50, 10, w, Cap::Square);
  CHECK_NEAR(alphaArea(img), 100, 0.3);
  img.clear();
  c.line(50, 50, 50, 50, 10, w, Cap::Butt);
  CHECK_EQ(alphaArea(img), 0.0);
}

TEST(gfx_canvas_stroke_joins) {
  // Ломаная под прямым углом шириной 10: площади для miter/bevel/round.
  const Pt pts[3] = {{20, 20}, {70, 20}, {70, 70}};
  struct Case { Join j; double area; } cases[] = {{Join::Miter, 1000}, {Join::Bevel, 987.5}, {Join::Round, 975 + kPi * 25 / 4}};
  for (auto& k : cases) {
    Image img(100, 100);
    Canvas c(img);
    Stroke s;
    s.width = 10;
    s.join = k.j;
    c.polyline(pts, 3, false, s, Color(255, 255, 255));
    CHECK_MSG(std::fabs(alphaArea(img) - k.area) < 0.8, strf("join %d area %.3f expect %.3f", int(k.j), alphaArea(img), k.area));
  }
  // Предел miter: острый угол переходит в bevel.
  {
    const Pt sharp[3] = {{10, 50}, {90, 45}, {10, 40}};
    Image a(100, 100), b(100, 100);
    Canvas ca(a), cb(b);
    Stroke s;
    s.width = 4;
    s.miterLimit = 4;
    ca.polyline(sharp, 3, false, s, Color(255, 255, 255));
    s.join = Join::Bevel;
    cb.polyline(sharp, 3, false, s, Color(255, 255, 255));
    CHECK_NEAR(alphaArea(a), alphaArea(b), 0.01);
    s.join = Join::Miter;
    s.miterLimit = 100;
    a.clear();
    ca.polyline(sharp, 3, false, s, Color(255, 255, 255));
    CHECK(alphaArea(a) > alphaArea(b) + 5);
  }
  // Внутренняя сторона излома и самопересечения не смешиваются дважды.
  {
    Image img(100, 100, 0xFFFFFFFFu);
    Canvas c(img);
    const Pt zig[5] = {{10, 10}, {90, 50}, {10, 50}, {90, 10}, {50, 90}};
    for (Join j : {Join::Miter, Join::Round, Join::Bevel}) {
      img.clear(0xFFFFFFFFu);
      Stroke s;
      s.width = 12;
      s.join = j;
      c.polyline(zig, 5, false, s, Color(0, 0, 0, 128));
      int bad = 0;
      for (u32 p : img.px) if (R(p) < 126) bad++;  // двойное смешивание дало бы 64
      CHECK_MSG(bad == 0, strf("join %d: %d pixels double-blended", int(j), bad));
    }
  }
  // Замкнутый контур: полоса между внешним и внутренним краем, центр пуст.
  {
    Image img(100, 100);
    Canvas c(img);
    Stroke s;
    s.width = 10;
    const Pt sq[4] = {{20, 20}, {80, 20}, {80, 80}, {20, 80}};
    c.polyline(sq, 4, true, s, Color(255, 255, 255));
    CHECK_NEAR(alphaArea(img), 70.0 * 70 - 50.0 * 50, 0.5);
    CHECK_EQ(raw(img, 50, 50), 0u);
    CHECK_EQ(A(raw(img, 16, 16)), 255);  // угол miter
  }
}

TEST(gfx_canvas_stroke_curves) {
  // Обводка окружности: площадь кольца.
  Image img(200, 200);
  Canvas c(img);
  c.strokeCircle(100, 100, 60, 8, Color(255, 255, 255));
  double expect = kPi * (64.0 * 64 - 56.0 * 56);
  CHECK_NEAR(alphaArea(img), expect, expect * 0.004);
  // Скруглённый прямоугольник.
  img.clear();
  c.strokeRoundRect({40, 40, 120, 80}, 12, 2, Color(255, 255, 255));
  double outer = 122.0 * 82 - (4 - kPi) * 13 * 13, inner = 118.0 * 78 - (4 - kPi) * 11 * 11;
  CHECK_NEAR(alphaArea(img), outer - inner, (outer - inner) * 0.01);
  // strokeToPath даёт тот же результат, что strokePath.
  Path p;
  p.moveTo(20, 150);
  p.cubicTo(60, 60, 140, 240, 180, 150);
  Stroke s;
  s.width = 6;
  s.cap = Cap::Round;
  img.clear();
  c.strokePath(p, s, Color(255, 255, 255));
  double a1 = alphaArea(img);
  Path o = strokeToPath(p, s, 0.05f);
  img.clear();
  c.fillPath(o, Color(255, 255, 255));
  CHECK_NEAR(alphaArea(img), a1, a1 * 0.002);
}

TEST(gfx_canvas_dash) {
  Image img(120, 20);
  Canvas c(img);
  Stroke s;
  s.width = 1;
  s.dash = {10, 5};
  const Pt l[2] = {{0, 10.5f}, {120, 10.5f}};
  c.polyline(l, 2, false, s, Color(255, 255, 255));
  for (int x = 0; x < 120; x++) {
    int ph = x % 15;
    int expect = ph < 10 ? 255 : 0;
    CHECK_MSG(A(raw(img, x, 10)) == expect, strf("x=%d alpha=%d", x, A(raw(img, x, 10))));
  }
  // Смещение шаблона.
  img.clear();
  s.dashOffset = 3;
  c.polyline(l, 2, false, s, Color(255, 255, 255));
  for (int x = 0; x < 120; x++) {
    int ph = (x + 3) % 15;
    CHECK_EQ(A(raw(img, x, 10)), ph < 10 ? 255 : 0);
  }
  // Отрицательное смещение и нечётный шаблон [4] -> [4, 4].
  img.clear();
  s.dash = {4};
  s.dashOffset = -2;
  c.polyline(l, 2, false, s, Color(255, 255, 255));
  for (int x = 0; x < 120; x++) {
    int ph = ((x - 2) % 8 + 8) % 8;
    CHECK_EQ(A(raw(img, x, 10)), ph < 4 ? 255 : 0);
  }
  // Недопустимый шаблон — сплошная линия.
  img.clear();
  s.dash = {5, -1};
  s.dashOffset = 0;
  c.polyline(l, 2, false, s, Color(255, 255, 255));
  CHECK_NEAR(alphaArea(img), 120, 0.01);
  // Длины штрихов по ломаной через изломы.
  std::vector<double> lens;
  const Pt poly[3] = {{0, 0}, {7, 0}, {7, 13}};
  dashPolyline(poly, 3, false, {6, 2}, 0, [&](const Pt* p, size_t n) { lens.push_back(polylineLength(p, n, false)); });
  CHECK_EQ(lens.size(), size_t(3));
  CHECK_NEAR(lens[0], 6, 1e-5);
  CHECK_NEAR(lens[1], 6, 1e-5);
  CHECK_NEAR(lens[2], 4, 1e-5);
  // Замкнутый контур: последний штрих сливается с первым.
  lens.clear();
  const Pt sq[4] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
  dashPolyline(sq, 4, true, {7, 3}, 0, [&](const Pt* p, size_t n) { lens.push_back(polylineLength(p, n, false)); });
  CHECK_EQ(lens.size(), size_t(4));
  double total = 0;
  for (double v : lens) total += v;
  CHECK_NEAR(total, 28, 1e-4);
  // Слишком частый шаблон — отказ (рисуется сплошной).
  CHECK(!dashPolyline(l, 2, false, {1e-6f, 1e-6f}, 0, [](const Pt*, size_t) {}));
  // Отрезки вне кадра пропускаются без построения, но фаза шаблона сохраняется: результат совпадает
  // с обводкой без отсечения.
  {
    const Pt far[5] = {{10, 10.5f}, {90, 10.5f}, {90, 50000}, {50, 50000}, {50, 20}};
    Stroke fs;
    fs.width = 2;
    fs.dash = {7, 3};
    fs.cap = Cap::Round;
    Image a(100, 100), b(100, 100);
    Canvas ca(a), cb(b);
    ca.polyline(far, 5, false, fs, Color(255, 255, 255));
    Path pp;
    pp.addPolygon(far, 5, false);
    cb.fillPath(strokeToPath(pp, fs, 0.1f), Color(255, 255, 255));
    int worst = 0;
    for (size_t i = 0; i < a.px.size(); i++) worst = std::max(worst, std::abs(A(a.px[i]) - A(b.px[i])));
    CHECK_MSG(worst <= 2, strf("worst %d", worst));
    CHECK(alphaArea(a) > 100);
    // Очень длинная невидимая часть с мелким шаблоном не превышает лимит штрихов и считается быстро.
    const Pt longp[4] = {{10, 30.5f}, {90, 30.5f}, {90, 3e7f}, {10, 3e7f}};
    fs.dash = {2, 2};
    double t0 = nowSeconds();
    ca.polyline(longp, 4, false, fs, Color(255, 255, 255));
    CHECK((nowSeconds() - t0) * 1000 < 20);
    for (int x = 12; x < 88; x++) CHECK(A(raw(a, x, 30)) == 255 || A(raw(a, x, 30)) < 255);  // нет падения
  }
  // Пунктирная окружность: доля закрашенной длины.
  Image ci(100, 100);
  Canvas cc(ci);
  Stroke cs;
  cs.width = 2;
  cs.dash = {6, 6};
  cc.strokeCircle(50, 50, 40, 2, Color(255, 255, 255));
  double solid = alphaArea(ci);
  ci.clear();
  Path circ;
  circ.addCircle(50, 50, 40);
  cc.strokePath(circ, cs, Color(255, 255, 255));
  CHECK_NEAR(alphaArea(ci), solid / 2, solid * 0.03);
}

TEST(gfx_canvas_hairline) {
  Image img(100, 20);
  Canvas c(img);
  c.line(0, 10.5f, 100, 10.5f, 0.25f, Color(255, 255, 255), Cap::Butt);
  for (int x = 0; x < 100; x++) CHECK_NEAR(A(raw(img, x, 10)), 64, 1);
  CHECK_EQ(A(raw(img, 50, 9)), 0);
  img.clear();
  c.line(0, 10.5f, 100, 10.5f, 0, Color(255, 255, 255));
  CHECK_EQ(alphaArea(img), 0.0);
  // Волосяная линия при уменьшении масштаба остаётся 1 пикселем устройства.
  img.clear();
  c.save();
  c.scale(0.1f, 0.1f);
  c.line(0, 105, 1000, 105, 2, Color(255, 255, 255), Cap::Butt);  // устройство: ширина 0,2
  c.restore();
  double col = 0;
  for (int y = 0; y < 20; y++) col += A(raw(img, 50, y)) / 255.0;
  CHECK_NEAR(col, 0.2, 0.01);
}

TEST(gfx_canvas_no_out_of_bounds) {
  // Всё, что рисуется вне отсечения, не меняет пиксели; огромные и нечисловые координаты не роняют.
  Image img(64, 64, 0xFF102030u);
  Canvas c(img);
  c.save();
  c.clipRect({16, 16, 32, 32});
  const Color col(250, 10, 10);
  Stroke s;
  s.width = 1e6f;
  Path huge;
  huge.addCircle(1e20f, -1e20f, 1e25f);
  c.fillPath(huge, col);
  c.fillRect({NAN, 0, 10, 10}, col);
  c.fillRect({0, 0, INFINITY, 10}, col);
  c.fillCircle(-1e9f, 1e9f, 5, col);
  c.strokePath(huge, s, col);
  c.line(-1e30f, -1e30f, 1e30f, 1e30f, 3, col);
  c.line(NAN, 0, 10, 10, 3, col);
  Stroke ds;
  ds.width = 2;
  ds.dash = {1e-3f, 1e-3f};
  c.line(-1e7f, 30, 1e7f, 30, 2, col);
  Path dl;
  dl.moveTo(-1e7f, 30);
  dl.lineTo(1e7f, 31);
  c.strokePath(dl, ds, col);
  Image small(8, 8, 0xFFFFFFFFu);
  c.drawImage(small, {-1e9f, -1e9f, 2e9f, 2e9f});
  c.drawImageXf(small, Affine::rotate(0.7f) * Affine::scale(1e7f));
  c.drawImageXf(small, Affine{NAN, 0, 0, 1, 0, 0});
  c.drawImage(small, {-5, -5, 1e-6f, 1e-6f});
  c.boxShadow({-1e8f, -1e8f, 2e8f, 2e8f}, 30, 40, 5, Color(0, 0, 0, 200));
  c.boxShadow({NAN, 0, 5, 5}, 3, 4, 0, Color(0, 0, 0, 200));
  Mask m(30, 30);
  std::fill(m.a.begin(), m.a.end(), u8(255));
  c.fillMask(m, -20, -20, col);
  c.fillMask(m, 1e30f, 5, col);
  c.blurRegion({-100, -100, 1000, 1000}, 50);
  c.restore();
  int changedOutside = 0, changedInside = 0;
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      bool inside = x >= 16 && x < 48 && y >= 16 && y < 48;
      if (raw(img, x, y) != 0xFF102030u) (inside ? changedInside : changedOutside)++;
    }
  CHECK_EQ(changedOutside, 0);
  CHECK(changedInside > 0);
  // Без отсечения: фигуры на границах изображения.
  Image e(17, 13);
  Canvas ce(e);
  ce.fillCircle(0, 0, 30, col);
  ce.fillCircle(17, 13, 5, col);
  ce.fillRect({-3, -3, 100, 100}, col);
  ce.boxShadow({0, 0, 17, 13}, 4, 10, 0, Color(0, 0, 0));
  ce.blurRegion({0, 0, 17, 13}, 30);
  // Пустое изображение.
  Image z;
  Canvas cz(z);
  cz.fillRect({0, 0, 10, 10}, col);
  cz.clear(col);
  cz.drawImage(small, {0, 0, 10, 10});
  cz.blurRegion({0, 0, 10, 10}, 3);
  CHECK(cz.clipBounds().empty());
}

TEST(gfx_canvas_draw_image) {
  Image src(4, 4);
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++) src.px[size_t(y * 4 + x)] = premul(Color(u8(x * 60), u8(y * 60), 77));
  // Копия 1:1.
  Image img(20, 20);
  Canvas c(img);
  c.drawImage(src, {3, 5, 4, 4});
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++) CHECK_EQ(raw(img, 3 + x, 5 + y), src.at(x, y));
  CHECK_EQ(raw(img, 2, 5), 0u);
  CHECK_EQ(raw(img, 7, 5), 0u);
  // Половинная прозрачность.
  img.clear();
  c.drawImage(src, {3, 5, 4, 4}, 0.5f);
  CHECK_NEAR(A(raw(img, 4, 6)), 128, 1);
  // Увеличение 2× ближайшим соседом — блоки 2×2.
  img.clear();
  c.drawImage(src, {0, 0, 8, 8}, 1, false);
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) CHECK_EQ(raw(img, x, y), src.at(x / 2, y / 2));
  // Поворот на 90° (ближайший сосед): точное отображение пикселей.
  img.clear();
  c.drawImageXf(src, Affine::translate(10, 2) * Affine::rotate(float(kPi / 2)), 1, false);
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++) CHECK_EQ(raw(img, 10 - y - 1, 2 + x), src.at(x, y));
  // Билинейная выборка: линейный переход между двумя пикселями.
  Image ramp(2, 1);
  ramp.px = {0xFF000000u, 0xFFFFFFFFu};
  Image line(200, 1);
  Canvas cl(line);
  cl.drawImage(ramp, {0, 0, 200, 1});
  CHECK_EQ(raw(line, 0, 0), 0xFF000000u);
  CHECK_EQ(raw(line, 199, 0), 0xFFFFFFFFu);
  CHECK_NEAR(R(raw(line, 99, 0)), 126, 3);
  for (int x = 1; x < 200; x++) CHECK(R(raw(line, x, 0)) >= R(raw(line, x - 1, 0)));
  // Атлас: выборка не выходит за src (нет примеси соседней половины).
  Image atlas(20, 10);
  for (int y = 0; y < 10; y++)
    for (int x = 0; x < 20; x++) atlas.px[size_t(y * 20 + x)] = x < 10 ? 0xFFFF0000u : 0xFF0000FFu;
  Image big(100, 100);
  Canvas cb(big);
  cb.drawImage(atlas, {0, 0, 10, 10}, {0, 0, 100, 100});
  for (u32 p : big.px) CHECK_EQ(B(p), 0);
  CHECK_EQ(raw(big, 99, 99), 0xFFFF0000u);
  // Сильное уменьшение — усреднение по площади (шахматка 1 px -> ровный серый).
  Image checker(64, 64);
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) checker.px[size_t(y * 64 + x)] = (x + y) % 2 ? 0xFFFFFFFFu : 0xFF000000u;
  Image down(16, 16);
  Canvas cd(down);
  cd.drawImage(checker, {0, 0, 16, 16});
  for (u32 p : down.px) CHECK_MSG(std::abs(R(p) - 128) <= 2 && A(p) == 255, pxs(p));
  // Повёрнутое изображение: площадь сохраняется, края сглажены.
  Image white(30, 20, 0xFFFFFFFFu);
  Image rot(100, 100);
  Canvas cr(rot);
  cr.drawImageXf(white, Affine::translate(50, 50) * Affine::rotate(0.6f) * Affine::translate(-15, -10));
  CHECK_NEAR(alphaArea(rot), 600, 600 * 0.003);
  // src частично вне изображения — рисуется только существующая часть.
  Image part(20, 20);
  Canvas cp(part);
  cp.drawImage(src, {2, 0, 4, 4}, {0, 0, 8, 8}, 1, false);
  CHECK_EQ(raw(part, 0, 0), src.at(2, 0));
  CHECK_EQ(raw(part, 3, 7), src.at(3, 3));
  CHECK_EQ(raw(part, 4, 0), 0u);
}

TEST(gfx_canvas_image_paint) {
  Image src(4, 4);
  for (int i = 0; i < 16; i++) src.px[size_t(i)] = premul(Color(u8(i * 15), 100, u8(255 - i * 15)));
  Image img(40, 40);
  Canvas c(img);
  Paint p;
  p.image = &src;
  p.imageXf = Affine::translate(10, 10) * Affine::scale(5);
  p.bilinear = false;
  c.fillRect({10, 10, 20, 20}, p);
  CHECK_EQ(raw(img, 12, 12), src.at(0, 0));
  CHECK_EQ(raw(img, 17, 12), src.at(1, 0));
  CHECK_EQ(raw(img, 29, 29), src.at(3, 3));
  CHECK_EQ(raw(img, 9, 12), 0u);
  // Вне изображения — повтор края.
  img.clear();
  c.fillRect({0, 0, 40, 40}, p);
  CHECK_EQ(raw(img, 0, 0), src.at(0, 0));
  CHECK_EQ(raw(img, 39, 0), src.at(3, 0));
  // Узор (tile): изображение повторяется в обе стороны, в том числе при отрицательных координатах.
  p.tile = true;
  img.clear();
  c.fillRect({0, 0, 40, 40}, p);
  for (int y = 0; y < 40; y++)
    for (int x = 0; x < 40; x++) {
      int ix = (((x - 10) / 5) % 4 + 4) % 4, iy = (((y - 10) / 5) % 4 + 4) % 4;
      if (x < 10) ix = ((((x - 10) - 4) / 5) % 4 + 4) % 4;
      if (y < 10) iy = ((((y - 10) - 4) / 5) % 4 + 4) % 4;
      CHECK_MSG(raw(img, x, y) == src.at(ix, iy), strf("x=%d y=%d", x, y));
    }
  // Узор с поворотом и билинейной выборкой: шов на границе периода отсутствует (значения из соседних плиток).
  Image two(2, 1);
  two.px = {0xFF000000u, 0xFFFFFFFFu};
  Paint tp;
  tp.image = &two;
  tp.tile = true;
  tp.imageXf = Affine::scale(10);
  Image row(40, 1);
  Canvas cr(row);
  cr.fillRect({0, 0, 40, 1}, tp);
  // период 20 px: тёмный центр в 5, светлый в 15, линейные переходы через границы периода.
  CHECK(R(raw(row, 5, 0)) < 20);
  CHECK(R(raw(row, 15, 0)) > 235);
  CHECK_NEAR(R(raw(row, 20, 0)), R(raw(row, 0, 0)), 1);
  CHECK_NEAR(R(raw(row, 0, 0)), 128, 16);
  // Сильное уменьшение узора — среднее значение.
  tp.imageXf = Affine::scale(0.05f);
  cr.fillRect({0, 0, 40, 1}, tp);
  for (int x = 0; x < 40; x++) CHECK_NEAR(R(raw(row, x, 0)), 128, 2);
  // Повёрнутый узор (общий путь).
  tp.imageXf = Affine::rotate(0.3f) * Affine::scale(3);
  Image big(64, 64);
  Canvas cb(big);
  cb.fillRect({0, 0, 64, 64}, tp);
  int dark = 0, light = 0;
  for (u32 q : big.px) { if (R(q) < 40) dark++; if (R(q) > 215) light++; }
  CHECK(dark > 600 && light > 600);
}

TEST(gfx_canvas_gradient) {
  Image img(100, 10);
  Canvas c(img);
  Gradient g;
  g.p0 = {0, 0};
  g.p1 = {100, 0};
  g.stops = {{0.f, Color(255, 0, 0)}, {1.f, Color(0, 0, 255)}};
  Paint p;
  p.gradient = &g;
  c.fillRect({0, 0, 100, 10}, p);
  CHECK_MSG(nearPx(raw(img, 0, 5), 255, 254, 0, 1, 2), pxs(raw(img, 0, 5)));
  CHECK_MSG(nearPx(raw(img, 99, 5), 255, 1, 0, 254, 2), pxs(raw(img, 99, 5)));
  CHECK_MSG(nearPx(raw(img, 50, 5), 255, 126, 0, 129, 2), pxs(raw(img, 50, 5)));
  for (int x = 1; x < 100; x++) CHECK(R(raw(img, x, 5)) <= R(raw(img, x - 1, 5)) + 1);
  // Переход от прозрачного: интерполяция в premultiplied — без тёмной каймы.
  g.stops = {{0.f, Color(0, 0, 0, 0)}, {1.f, Color(255, 0, 0)}};
  img.clear();
  c.fillRect({0, 0, 100, 10}, p);
  Color mid = unpremul(raw(img, 50, 5));
  CHECK_NEAR(mid.a, 129, 3);
  CHECK(mid.r >= 250);
  // Градиент в координатах текущего преобразования.
  img.clear();
  g.stops = {{0.f, Color(255, 0, 0)}, {1.f, Color(0, 0, 255)}};
  c.save();
  c.translate(50, 0);
  c.scale(0.5f, 1);
  c.fillRect({-100, 0, 200, 10}, p);
  c.restore();
  CHECK(R(raw(img, 49, 5)) > 250);
  CHECK(B(raw(img, 99, 5)) > 250);
  CHECK_NEAR(R(raw(img, 75, 5)), 126, 3);
  // Радиальный: центр -> край, дальше — цвет последней точки.
  Image ri(100, 100);
  Canvas rc(ri);
  Gradient rg;
  rg.kind = Gradient::Radial;
  rg.p0 = {50, 50};
  rg.r1 = 50;
  rg.stops = {{0.f, Color(255, 0, 0)}, {1.f, Color(0, 0, 255)}};
  Paint rp;
  rp.gradient = &rg;
  rc.fillRect({0, 0, 100, 100}, rp);
  CHECK(R(raw(ri, 50, 50)) > 250);
  CHECK(B(raw(ri, 0, 0)) > 252);
  CHECK_NEAR(B(raw(ri, 75, 50)), 128, 4);
  // Вырожденные градиенты: один цвет / нулевая длина / пустые точки.
  Gradient one;
  one.stops = {{0.5f, Color(0, 255, 0)}};
  Paint op;
  op.gradient = &one;
  img.clear();
  c.fillRect({0, 0, 10, 10}, op);
  CHECK_EQ(raw(img, 5, 5), 0xFF00FF00u);
  Gradient zero;
  zero.p0 = zero.p1 = {5, 5};
  zero.stops = {{0.f, Color(255, 0, 0)}, {1.f, Color(0, 0, 255)}};
  op.gradient = &zero;
  c.fillRect({0, 0, 10, 10}, op);
  CHECK_EQ(raw(img, 5, 5), 0xFF0000FFu);
  Gradient none;
  op.gradient = &none;
  op.color = Color(1, 2, 3);
  c.fillRect({0, 0, 10, 10}, op);
  CHECK_EQ(raw(img, 5, 5), premul(Color(1, 2, 3)));
}

TEST(gfx_canvas_fill_mask) {
  Mask m(4, 3);
  for (size_t i = 0; i < m.a.size(); i++) m.a[i] = u8(i * 20);
  Image img(20, 20);
  Canvas c(img);
  c.fillMask(m, 10, 10, Color(255, 0, 0));
  for (int y = 0; y < 3; y++)
    for (int x = 0; x < 4; x++) {
      u32 p = raw(img, 10 + x, 10 + y);
      int v = (y * 4 + x) * 20;
      CHECK_MSG(A(p) == v && R(p) == v && G(p) == 0, pxs(p));
    }
  // Перенос из преобразования, прозрачность, отсечение.
  img.clear();
  c.save();
  c.translate(2, 3);
  c.setOpacity(0.5f);
  c.clipRect({0, 0, 3, 100});  // устройство x < 5
  c.fillMask(m, 0, 0, Color(255, 255, 255));
  c.restore();
  CHECK_NEAR(A(raw(img, 2 + 3, 3 + 2)), 0, 0);   // за отсечением
  CHECK_NEAR(A(raw(img, 2 + 2, 3 + 2)), (10 * 20) / 2, 1);
  CHECK_EQ(A(raw(img, 1, 3)), 0);
  // Градиент через маску.
  Gradient g;
  g.p0 = {0, 0};
  g.p1 = {20, 0};
  g.stops = {{0.f, Color(0, 0, 0)}, {1.f, Color(255, 255, 255)}};
  Paint gp;
  gp.gradient = &g;
  Mask full(20, 1);
  std::fill(full.a.begin(), full.a.end(), u8(255));
  img.clear();
  c.fillMask(full, 0, 15, gp);
  CHECK(R(raw(img, 19, 15)) > R(raw(img, 0, 15)) + 200);
  // Маска отсечения (контур) применяется к маске покрытия.
  img.clear();
  c.save();
  Path half;
  half.addRect({0, 0, 10.5f, 20});
  c.clipPath(half);
  c.fillMask(full, 0, 15, Color(255, 255, 255));
  c.restore();
  CHECK_EQ(A(raw(img, 9, 15)), 255);
  CHECK_NEAR(A(raw(img, 10, 15)), 128, 1);
  CHECK_EQ(A(raw(img, 11, 15)), 0);
}

TEST(gfx_canvas_box_shadow) {
  Image img(300, 300);
  Canvas c(img);
  c.boxShadow({100, 100, 100, 100}, 10, 20, 0, Color(0, 0, 0, 255));
  CHECK_EQ(A(raw(img, 150, 150)), 255);
  CHECK_NEAR(A(raw(img, 100, 150)), 128, 6);   // на краю — половина
  CHECK_NEAR(A(raw(img, 199, 150)), 128, 6);
  CHECK(A(raw(img, 60, 150)) <= 1);             // 4σ снаружи
  for (int k = 0; k < 40; k++) {
    CHECK_EQ(A(raw(img, 100 - k, 150)), A(raw(img, 199 + k, 150)));  // симметрия
    CHECK_EQ(A(raw(img, 150, 100 - k)), A(raw(img, 150, 199 + k)));
    if (k) CHECK(A(raw(img, 100 - k, 150)) <= A(raw(img, 100 - k + 1, 150)));  // монотонность
  }
  // Угол мягче края.
  CHECK(A(raw(img, 100, 100)) < A(raw(img, 100, 150)));
  // Каноническая плитка (растяжение) совпадает с точным расчётом в общем пути (крошечный поворот).
  for (int size : {40, 260}) {
    Image a(400, 400), b(400, 400);
    Canvas ca(a), cb(b);
    const RectF r{70, 80, float(size), float(size) * 0.8f};
    ca.boxShadow(r, 14, 36, 4, Color(0, 0, 0, 115), {0, 14});
    cb.concat(Affine::rotate(1e-6f));
    cb.boxShadow(r, 14, 36, 4, Color(0, 0, 0, 115), {0, 14});
    int worst = 0;
    for (size_t i = 0; i < a.px.size(); i++) worst = std::max(worst, std::abs(A(a.px[i]) - A(b.px[i])));
    CHECK_MSG(worst <= 3, strf("size %d worst %d", size, worst));
  }
  // Повтор из кеша даёт тот же результат.
  Image d(300, 300);
  Canvas cd(d);
  cd.boxShadow({100, 100, 100, 100}, 10, 20, 0, Color(0, 0, 0, 255));
  CHECK(d.px == img.px);
  // Нулевое размытие — резкий скруглённый прямоугольник; spread расширяет.
  Image e(100, 100);
  Canvas ce(e);
  ce.boxShadow({20, 20, 60, 60}, 0, 0, 5, Color(255, 255, 255));
  CHECK_NEAR(alphaArea(e), 70 * 70, 0.5);
  // Прозрачность холста учитывается.
  Image f(300, 300);
  Canvas cf(f);
  cf.setOpacity(0.5f);
  cf.boxShadow({100, 100, 100, 100}, 10, 20, 0, Color(0, 0, 0, 255));
  CHECK_NEAR(A(raw(f, 150, 150)), 128, 1);
}

TEST(gfx_canvas_blur_region) {
  // Постоянный цвет не меняется.
  Image img(60, 60, premul(Color(200, 100, 50, 180)));
  Canvas c(img);
  const u32 v = img.px[0];
  c.blurRegion({0, 0, 60, 60}, 8);
  for (u32 p : img.px) CHECK_EQ(p, v);
  // Пятно расплывается симметрично, сумма почти сохраняется, вне области — без изменений.
  Image s(80, 80, 0xFF000000u);
  for (int y = 38; y < 41; y++)
    for (int x = 38; x < 41; x++) s.px[size_t(y * 80 + x)] = 0xFFFFFFFFu;
  Canvas cs(s);
  cs.blurRegion({10, 10, 59, 59}, 6);
  for (int k = 1; k < 15; k++) {
    CHECK_EQ(R(raw(s, 39 - k, 39)), R(raw(s, 39 + k, 39)));
    CHECK_EQ(R(raw(s, 39, 39 - k)), R(raw(s, 39, 39 + k)));
  }
  double sum = 0;
  for (u32 p : s.px) sum += R(p);
  CHECK_NEAR(sum, 9 * 255, 9 * 255 * 0.05);
  CHECK(R(raw(s, 39, 39)) < 255);
  CHECK(R(raw(s, 45, 39)) > 0);
  CHECK_EQ(raw(s, 5, 5), 0xFF000000u);
  // Отсечение ограничивает область.
  Image t(40, 40, 0xFF000000u);
  t.px[size_t(20 * 40 + 20)] = 0xFFFFFFFFu;
  Canvas ct(t);
  ct.clipRect({0, 0, 20, 40});
  ct.blurRegion({0, 0, 40, 40}, 6);
  CHECK_EQ(raw(t, 20, 20), 0xFFFFFFFFu);
}

TEST(gfx_canvas_image_ops) {
  // fromRgba / toRgba.
  const u8 rgba[8] = {255, 0, 0, 255, 200, 100, 50, 0};
  Image a = Image::fromRgba(rgba, 2, 1);
  CHECK_EQ(a.px[0], 0xFFFF0000u);
  CHECK_EQ(a.px[1], 0u);
  auto back = a.toRgba();
  CHECK_EQ(int(back[0]), 255);
  CHECK_EQ(int(back[3]), 255);
  CHECK_EQ(int(back[7]), 0);
  const u8 half[4] = {200, 100, 50, 128};
  Image h = Image::fromRgba(half, 1, 1);
  CHECK_EQ(A(h.px[0]), 128);
  CHECK_NEAR(R(h.px[0]), 100, 1);
  auto hb = h.toRgba();
  CHECK_NEAR(hb[0], 200, 1);
  CHECK_NEAR(hb[1], 100, 1);
  CHECK(Image::fromRgba(nullptr, 3, 3).empty());
  // Уменьшение по площади: блоки 2×2 -> точные цвета блоков.
  Image q(4, 4);
  const u32 cols[4] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu};
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++) q.px[size_t(y * 4 + x)] = cols[(y / 2) * 2 + x / 2];
  Image q2 = q.scaled(2, 2);
  CHECK_EQ(q2.w, 2);
  for (int i = 0; i < 4; i++) CHECK_EQ(q2.px[size_t(i)], cols[i]);
  // Шахматка -> серый; дробный коэффициент сохраняет среднее.
  Image ch(9, 9);
  for (int y = 0; y < 9; y++)
    for (int x = 0; x < 9; x++) ch.px[size_t(y * 9 + x)] = (x + y) % 2 ? 0xFFFFFFFFu : 0xFF000000u;
  Image ch3 = ch.scaled(3, 3);
  for (u32 p : ch3.px) CHECK_NEAR(R(p), 128, 15);
  // Увеличение постоянного цвета не меняет его; прозрачность не темнит края.
  Image k(3, 3, premul(Color(10, 200, 30)));
  Image k2 = k.scaled(10, 7);
  for (u32 p : k2.px) CHECK_EQ(p, k.px[0]);
  Image tr(2, 1);
  tr.px = {0xFFFF0000u, 0};
  Image tr1 = tr.scaled(1, 1);
  Color u = unpremul(tr1.px[0]);
  CHECK_NEAR(u.a, 128, 1);
  CHECK(u.r >= 254);
  // Бикубика с превышением даёт корректный premultiplied.
  Image st(4, 1);
  st.px = {0xFF000000u, 0xFF000000u, 0xFFFFFFFFu, 0xFFFFFFFFu};
  Image st2 = st.scaled(16, 1);
  for (u32 p : st2.px) CHECK(R(p) <= A(p));
  CHECK(st.scaled(0, 5).empty());
  // Вырезание.
  Image cr = q.cropped({1, 1, 10, 10});
  CHECK_EQ(cr.w, 3);
  CHECK_EQ(cr.h, 3);
  CHECK_EQ(cr.px[0], cols[0]);
  CHECK_EQ(cr.at(2, 2), cols[3]);
  CHECK(q.cropped({5, 5, 2, 2}).empty());
}

TEST(gfx_canvas_no_seams) {
  // Соседние прямоугольники по целой границе и квадраты одной сетки в одном контуре — без швов.
  Image img(100, 100);
  Canvas c(img);
  for (int i = 0; i < 10; i++) c.fillRect({float(i * 10), 0, 10, 100}, Color(255, 255, 255));
  for (u32 p : img.px) CHECK_EQ(p, 0xFFFFFFFFu);
  img.clear();
  Path grid;
  for (int y = 0; y < 7; y++)
    for (int x = 0; x < 7; x++) grid.addRect({10 + x * 11.37f, 10 + y * 11.37f, 11.37f, 11.37f});
  c.save();
  c.translate(50, 50);
  c.concat(Affine::rotate(0.37f));
  c.translate(-50, -50);
  c.fillPath(grid, Color(255, 255, 255));
  c.restore();
  // Внутренность повёрнутой сетки сплошная.
  for (int y = 40; y < 60; y++)
    for (int x = 40; x < 60; x++) CHECK_EQ(raw(img, x, y), 0xFFFFFFFFu);
}

TEST(gfx_canvas_extreme_zoom) {
  // Сильное увеличение (×5000): край огромной окружности проходит через кадр. Невидимые части кривых
  // спрямляются, видимые делятся до допуска. Эталон — плотная выборка тех же кубических кривых в double
  // (4 дуги окружности сами отличаются от окружности на 2,7·10⁻⁴ радиуса, поэтому сравнение с кривыми).
  Path circle;
  circle.addCircle(0, 0, 100);
  const Affine zoom = Affine::translate(200, 150) * Affine::scale(5000) * Affine::translate(-100.f * std::cos(0.3f), -100.f * std::sin(0.3f));
  Path dev = circle;
  dev.transform(zoom);
  auto reference = [&](double offset) {
    // Кривые в устройстве, выборка в double; offset — смещение по нормали (0 — сама кривая).
    std::vector<Pt> pts;
    size_t pi = 1;
    Pt cur = dev.pts[0];
    for (size_t v = 1; v < dev.verbs.size(); v++) {
      if (dev.verbs[v] != Path::Cubic) continue;
      const Pt p0 = cur, p1 = dev.pts[pi], p2 = dev.pts[pi + 1], p3 = dev.pts[pi + 2];
      pi += 3;
      cur = p3;
      const int n = 400000;
      for (int i = 0; i < n; i++) {
        const double t = double(i) / n, u = 1 - t;
        double x = u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x;
        double y = u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y;
        const double dx = 3 * u * u * (double(p1.x) - p0.x) + 6 * u * t * (double(p2.x) - p1.x) + 3 * t * t * (double(p3.x) - p2.x);
        const double dy = 3 * u * u * (double(p1.y) - p0.y) + 6 * u * t * (double(p2.y) - p1.y) + 3 * t * t * (double(p3.y) - p2.y);
        const double l = std::hypot(dx, dy);
        x += -dy / l * offset;
        y += dx / l * offset;
        const bool near = x > -50 && x < 450 && y > -50 && y < 350;
        if (near || i % 4096 == 0) pts.push_back({float(x), float(y)});
      }
    }
    return pts;
  };
  auto compare = [&](const Image& img, const Mask& ref, const char* what) {
    double worst = 0, sum = 0;
    int edge = 0;
    for (int y = 0; y < 300; y++)
      for (int x = 0; x < 400; x++) {
        const double d = std::fabs(A(raw(img, x, y)) - ref.a[size_t(y) * 400 + size_t(x)]);
        worst = std::max(worst, d);
        if (ref.a[size_t(y) * 400 + size_t(x)] % 255) { sum += d; edge++; }
      }
    // допуск аппроксимации 0,1 px: хорды лежат внутри кривой, среднее смещение ≤ 2/3 допуска (≈ 17 уровней)
    CHECK_MSG(worst <= 40 && sum / std::max(edge, 1) < 22, strf("%s: worst %.0f mean %.2f", what, worst, sum / std::max(edge, 1)));
  };
  // Заливка.
  {
    Image a(400, 300);
    Canvas ca(a);
    ca.setTransform(zoom);
    double t0 = nowSeconds();
    ca.fillPath(circle, Color(255, 255, 255));
    CHECK((nowSeconds() - t0) * 1000 < 30);
    std::vector<Pt> rp = reference(0);
    Path rpath;
    rpath.addPolygon(rp.data(), rp.size());
    Mask ref(400, 300);
    rasterizeToMask(rpath, {}, ref);
    compare(a, ref, "fill");
    CHECK(alphaArea(a) > 30000);
  }
  // Обводка шириной 10 px экрана: полоса между смещёнными кривыми.
  {
    Image a(400, 300);
    Canvas ca(a);
    ca.setTransform(zoom);
    Stroke s;
    s.width = 0.002f;
    double t0 = nowSeconds();
    ca.strokePath(circle, s, Color(255, 255, 255));
    CHECK((nowSeconds() - t0) * 1000 < 30);
    std::vector<Pt> outer = reference(5), inner = reference(-5);
    Path rpath;
    rpath.addPolygon(outer.data(), outer.size());
    std::reverse(inner.begin(), inner.end());
    rpath.addPolygon(inner.data(), inner.size());
    Mask ref(400, 300);
    rasterizeToMask(rpath, {}, ref);
    compare(a, ref, "stroke");
    CHECK(alphaArea(a) > 3000);
  }
  // Ещё сильнее: координаты устройства порядка 1e9. Точность ограничена float в Affine/Pt (шаг 64 px),
  // поэтому проверяются только отсутствие зависаний и выход за изображение (камера карты передаёт
  // координаты относительно себя).
  Image c(200, 200);
  Canvas cc(c);
  cc.setTransform(Affine::translate(100, 100) * Affine::scale(1e7f) * Affine::translate(-100, 0));
  double t0 = nowSeconds();
  cc.fillPath(circle, Color(255, 255, 255));
  Stroke s;
  s.width = 1e-6f;
  cc.strokePath(circle, s, Color(255, 0, 0));
  CHECK((nowSeconds() - t0) * 1000 < 30);
  double area = alphaArea(c);
  CHECK(area >= 0 && area <= 200.0 * 200);
}

TEST(gfx_canvas_shader_blend_modes) {
  // Градиент и изображение с режимами смешивания и маской отсечения.
  Image img(64, 8, premul(Color(200, 100, 50)));
  Canvas c(img);
  Gradient g;
  g.p0 = {0, 0};
  g.p1 = {64, 0};
  g.stops = {{0.f, Color(255, 255, 255)}, {1.f, Color(255, 255, 255)}};
  Paint p;
  p.gradient = &g;
  p.blend = Blend::Multiply;
  c.fillRect({0, 0, 64, 8}, p);  // умножение на белый ничего не меняет
  for (u32 q : img.px) CHECK_MSG(nearPx(q, 255, 200, 100, 50, 1), pxs(q));
  Image white(4, 4, 0xFFFFFFFFu);
  c.save();
  c.setBlend(Blend::Screen);
  c.drawImage(white, {0, 0, 32, 8});  // screen с белым — белый
  c.restore();
  CHECK_EQ(raw(img, 10, 4), 0xFFFFFFFFu);
  CHECK_MSG(nearPx(raw(img, 40, 4), 255, 200, 100, 50, 1), pxs(raw(img, 40, 4)));
  // Add с половинной прозрачностью через маску отсечения.
  c.save();
  Path half;
  half.addRect({32, 0, 32, 4});
  c.clipPath(half);
  Paint ap(Color(40, 40, 40));
  ap.blend = Blend::Add;
  c.fillRect({0, 0, 64, 8}, ap);
  c.restore();
  CHECK_MSG(nearPx(raw(img, 40, 2), 255, 240, 140, 90, 1), pxs(raw(img, 40, 2)));
  CHECK_MSG(nearPx(raw(img, 40, 6), 255, 200, 100, 50, 1), pxs(raw(img, 40, 6)));
}

TEST(gfx_canvas_degenerate_transforms) {
  // Вырожденное (нулевое) преобразование и нулевой масштаб — ничего не рисуется и не падает.
  Image img(32, 32);
  Canvas c(img);
  c.save();
  c.setTransform(Affine::scale(0, 1));
  c.fillRect({0, 0, 10, 10}, Color(255, 0, 0));
  c.strokeCircle(5, 5, 3, 1, Color(255, 0, 0));
  Image src(4, 4, 0xFFFFFFFFu);
  c.drawImage(src, {0, 0, 10, 10});
  c.boxShadow({0, 0, 10, 10}, 2, 4, 0, Color(0, 0, 0));
  c.clipRect({0, 0, 10, 10});
  c.restore();
  CHECK_EQ(alphaArea(img), 0.0);
  // Отражение (отрицательный масштаб) работает для заливки, изображения и отсечения.
  c.save();
  c.translate(32, 0);
  c.scale(-1, 1);
  c.clipRect({0, 0, 8, 32});  // устройство x ∈ [24, 32)
  c.fillRect({0, 0, 32, 32}, Color(255, 255, 255));
  c.restore();
  CHECK_EQ(A(raw(img, 30, 5)), 255);
  CHECK_EQ(A(raw(img, 20, 5)), 0);
  Image m(2, 1);
  m.px = {0xFFFF0000u, 0xFF0000FFu};
  Image out(2, 1);
  Canvas co(out);
  co.drawImage(m, {2, 0, -2, 1}, 1, false);  // зеркально по x
  CHECK_EQ(raw(out, 0, 0), 0xFF0000FFu);
  CHECK_EQ(raw(out, 1, 0), 0xFFFF0000u);
}

TEST(gfx_canvas_fuzz) {
  // Случайные операции с огромными, вырожденными и нечисловыми данными: ничего не падает, пиксели вне
  // отсечения не меняются, все пиксели остаются корректными premultiplied.
  Rng rng(20261001);
  auto coord = [&]() -> float {
    switch (rng.range(0, 9)) {
      case 0: return float((rng.uniform() - 0.5) * 1e7);
      case 1: return rng.range(0, 30) == 0 ? NAN : float(rng.uniform() * 64);
      case 2: return float((rng.uniform() - 0.5) * 1e30);
      case 3: return float(rng.range(-2, 66));
      default: return float(rng.uniform() * 80 - 8);
    }
  };
  auto randomPath = [&] {
    Path p;
    const int n = rng.range(0, 12);
    for (int i = 0; i < n; i++) {
      switch (rng.range(0, 6)) {
        case 0: p.moveTo(coord(), coord()); break;
        case 1: case 2: p.lineTo(coord(), coord()); break;
        case 3: p.quadTo(coord(), coord(), coord(), coord()); break;
        case 4: p.cubicTo(coord(), coord(), coord(), coord(), coord(), coord()); break;
        case 5: p.arcTo(coord(), coord(), coord(), rng.range(0, 1), rng.range(0, 1), coord(), coord()); break;
        default: p.close(); break;
      }
    }
    if (rng.range(0, 3) == 0) p.addRoundRect({coord(), coord(), coord(), coord()}, coord(), coord(), coord(), coord());
    return p;
  };
  Image tex(7, 5);
  for (u32& q : tex.px) q = premul(Color(u8(rng.next()), u8(rng.next()), u8(rng.next()), u8(rng.next())));
  Gradient grad;
  grad.kind = Gradient::Radial;
  grad.p0 = {30, 30};
  grad.r1 = 25;
  grad.stops = {{0.f, Color(255, 0, 0, 200)}, {0.6f, Color(0, 255, 0, 40)}, {1.f, Color(0, 0, 255)}};
  for (int iter = 0; iter < 400; iter++) {
    Image img(64, 48, 0xFF336699u);
    Canvas c(img);
    const RectI clip{rng.range(0, 20), rng.range(0, 20), rng.range(0, 50), rng.range(0, 40)};
    c.clipRect({float(clip.x), float(clip.y), float(clip.w), float(clip.h)});
    for (int op = 0; op < 12; op++) {
      c.save();
      if (rng.range(0, 2) == 0) c.concat(Affine{float(rng.uniform() * 4 - 2), float(rng.uniform() - 0.5), float(rng.uniform() - 0.5), float(rng.uniform() * 4 - 2), coord(), coord()});
      if (rng.range(0, 4) == 0) c.clipPath(randomPath(), rng.range(0, 1) ? FillRule::EvenOdd : FillRule::NonZero);
      if (rng.range(0, 5) == 0) c.setOpacity(float(rng.uniform()));
      Paint p(Color(u8(rng.next()), u8(rng.next()), u8(rng.next()), u8(rng.next())));
      p.blend = Blend(rng.range(0, 3));
      if (rng.range(0, 4) == 0) p.gradient = &grad;
      if (rng.range(0, 5) == 0) {
        p.image = &tex;
        p.imageXf = Affine{float(rng.uniform() * 8 - 4), 0, float(rng.uniform() - 0.5), float(rng.uniform() * 8 - 4), coord(), coord()};
        p.tile = rng.range(0, 1);
        p.bilinear = rng.range(0, 1);
      }
      switch (rng.range(0, 9)) {
        case 0: c.fillPath(randomPath(), p, rng.range(0, 1) ? FillRule::EvenOdd : FillRule::NonZero); break;
        case 1: {
          Stroke s;
          s.width = rng.range(0, 6) == 0 ? coord() : float(rng.uniform() * 12);
          s.join = Join(rng.range(0, 2));
          s.cap = Cap(rng.range(0, 2));
          s.miterLimit = float(rng.uniform() * 10);
          if (rng.range(0, 2) == 0) s.dash = {float(rng.uniform() * 6), float(rng.uniform() * 6), coord()};
          s.dashOffset = coord();
          c.strokePath(randomPath(), s, p);
          break;
        }
        case 2: c.drawImage(tex, {coord(), coord(), coord(), coord()}, {coord(), coord(), coord(), coord()}, float(rng.uniform()), rng.range(0, 1)); break;
        case 3: c.drawImageXf(tex, Affine{float(rng.uniform() * 20 - 10), float(rng.uniform() - 0.5), float(rng.uniform() - 0.5), float(rng.uniform() * 20 - 10), coord(), coord()}); break;
        case 4: c.boxShadow({coord(), coord(), coord(), coord()}, coord(), rng.range(0, 5) ? float(rng.uniform() * 40) : coord(), coord(), Color(0, 0, 0, 150), {coord(), coord()}); break;
        case 5: c.blurRegion({rng.range(-10, 70), rng.range(-10, 50), rng.range(-5, 80), rng.range(-5, 60)}, rng.range(0, 5) ? float(rng.uniform() * 40) : coord()); break;
        case 6: {
          Mask m(rng.range(0, 20), rng.range(0, 20));
          for (u8& v : m.a) v = u8(rng.next());
          c.fillMask(m, coord(), coord(), p);
          break;
        }
        case 7: c.fillRoundRect({coord(), coord(), coord(), coord()}, coord(), p); break;
        case 8: c.line(coord(), coord(), coord(), coord(), float(rng.uniform() * 5), p, Cap(rng.range(0, 2))); break;
        default: {
          std::vector<Pt> pts(size_t(rng.range(0, 20)));
          for (Pt& q : pts) q = {coord(), coord()};
          Stroke s;
          s.width = float(rng.uniform() * 4);
          s.join = Join(rng.range(0, 2));
          c.polyline(pts.data(), pts.size(), rng.range(0, 1), s, p);
          break;
        }
      }
      c.restore();
    }
    for (int y = 0; y < img.h; y++)
      for (int x = 0; x < img.w; x++) {
        const u32 q = raw(img, x, y);
        const bool inside = x >= clip.x && x < clip.right() && y >= clip.y && y < clip.bottom();
        if (!inside) CHECK_MSG(q == 0xFF336699u, strf("iter %d: pixel %d,%d outside clip changed", iter, x, y));
        CHECK_MSG(R(q) <= A(q) && G(q) <= A(q) && B(q) <= A(q), strf("iter %d: invalid premul %s", iter, pxs(q).c_str()));
      }
  }
}
