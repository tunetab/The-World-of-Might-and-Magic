// Regnum — тесты контуров и растеризатора: точная площадь, правила заливки, отсечение, вырожденные данные.
#include "gfx/raster.h"
#include "gfx/stroke.h"
#include "tests/test.h"

using namespace rg;
using namespace rg::gfx;

namespace {

double maskArea(const Path& p, int w, int h, FillRule rule = FillRule::NonZero, const Affine& xf = {}) {
  Mask m(w, h);
  rasterizeToMask(p, xf, m, 0, 0, rule);
  return coverageSum(m);
}

u8 at(const Mask& m, int x, int y) { return m.a[size_t(y) * size_t(m.w) + size_t(x)]; }

}  // namespace

// ---------------------------------------------------------------- Path

TEST(gfx_raster_path_shapes) {
  Path p;
  p.addRect({10, 20, 30, 40});
  CHECK_EQ(p.verbs.size(), size_t(5));
  RectF b = p.bounds();
  CHECK_NEAR(b.x, 10, 1e-6); CHECK_NEAR(b.y, 20, 1e-6); CHECK_NEAR(b.w, 30, 1e-6); CHECK_NEAR(b.h, 40, 1e-6);

  // Отрицательный размер нормализуется, нулевой — пропускается.
  Path q;
  q.addRect({10, 10, -5, 5});
  CHECK_NEAR(q.bounds().x, 5, 1e-6);
  Path z;
  z.addRect({0, 0, 0, 5});
  z.addCircle(0, 0, 0);
  z.addRoundRect({0, 0, 10, 0}, 3);
  CHECK(z.empty());

  // Точные границы окружности (не по контрольным точкам).
  Path c;
  c.addCircle(50, 60, 25);
  RectF cb = c.bounds();
  CHECK_NEAR(cb.x, 25, 1e-3); CHECK_NEAR(cb.y, 35, 1e-3); CHECK_NEAR(cb.w, 50, 1e-3); CHECK_NEAR(cb.h, 50, 1e-3);

  // Кривые: границы по экстремумам.
  Path k;
  k.moveTo(0, 0);
  k.cubicTo(0, 100, 100, 100, 100, 0);
  CHECK_NEAR(k.bounds().h, 75, 1e-3);
  Path qd;
  qd.moveTo(0, 0);
  qd.quadTo(50, 100, 100, 0);
  CHECK_NEAR(qd.bounds().h, 50, 1e-3);
}

TEST(gfx_raster_path_round_rect_radii) {
  // Радиусы, не помещающиеся на стороне, уменьшаются пропорционально (как в CSS): получается «таблетка».
  Path p;
  p.addRoundRect({0, 0, 100, 40}, 100);
  RectF b = p.bounds();
  CHECK_NEAR(b.w, 100, 1e-3);
  CHECK_NEAR(b.h, 40, 1e-3);
  double area = maskArea(p, 110, 50);
  double expect = 100.0 * 40 - (4 - kPi) * 20 * 20;
  CHECK_NEAR(area, expect, expect * 0.004);

  // Разные радиусы углов: площадь = прямоугольник минус уголки.
  Path q;
  q.addRoundRect({5, 5, 80, 60}, 0, 10, 20, 5);
  double a2 = maskArea(q, 100, 80);
  double e2 = 80.0 * 60 - (1 - kPi / 4) * (10 * 10 + 20 * 20 + 5 * 5);
  CHECK_NEAR(a2, e2, e2 * 0.002);

  // Отрицательные и нечисловые радиусы — прямые углы.
  Path r;
  r.addRoundRect({0, 0, 10, 10}, -5, NAN, 0, -1);
  CHECK_NEAR(maskArea(r, 12, 12), 100, 0.05);
}

TEST(gfx_raster_path_add_path_transform) {
  Path a;
  a.addRect({0, 0, 10, 10});
  Path b;
  b.addPath(a, Affine::translate(5, 7) * Affine::scale(2));
  RectF r = b.bounds();
  CHECK_NEAR(r.x, 5, 1e-5); CHECK_NEAR(r.y, 7, 1e-5); CHECK_NEAR(r.w, 20, 1e-5); CHECK_NEAR(r.h, 20, 1e-5);
  b.addPath(b);  // самодобавление безопасно
  CHECK_EQ(b.verbs.size(), size_t(10));
}

TEST(gfx_raster_path_arc_svg) {
  // Полуокружность через дугу SVG: из (0, 50) в (100, 50), r = 50.
  Path p;
  p.moveTo(0, 50);
  p.arcTo(50, 50, 0, false, true, 100, 50);
  CHECK_NEAR(p.pts.back().x, 100, 1e-4);
  CHECK_NEAR(p.pts.back().y, 50, 1e-4);
  RectF b = p.bounds();
  CHECK_NEAR(b.w, 100, 0.01);
  CHECK_NEAR(b.h, 50, 0.05);
  CHECK_NEAR(b.y, 0, 0.05);  // sweep = 1 в системе с осью y вниз — дуга сверху

  // Обратное направление.
  Path q;
  q.moveTo(0, 50);
  q.arcTo(50, 50, 0, false, false, 100, 50);
  CHECK_NEAR(q.bounds().bottom(), 100, 0.05);

  // Слишком маленький радиус увеличивается до половины хорды.
  Path r;
  r.moveTo(0, 0);
  r.arcTo(1, 1, 0, false, true, 40, 0);
  CHECK_NEAR(r.bounds().h, 20, 0.05);

  // Большая дуга: 3/4 окружности радиуса 20 с центром (20, 20).
  Path s;
  s.moveTo(20, 0);
  s.arcTo(20, 20, 0, true, true, 0, 20);
  CHECK_NEAR(s.bounds().w, 40, 0.05);
  CHECK_NEAR(s.bounds().h, 40, 0.05);
  CHECK(s.verbs.size() >= 4);  // не более 90° на кубический сегмент

  // Повёрнутый эллипс: точки дуги лежат на эллипсе.
  Path e;
  e.moveTo(0, 0);
  e.arcTo(30, 10, 30, false, true, 40, 10);
  CHECK_NEAR(e.pts.back().x, 40, 1e-4);
  CHECK_NEAR(e.pts.back().y, 10, 1e-4);

  // Нулевой радиус — отрезок; совпадающие точки — ничего.
  Path l;
  l.moveTo(0, 0);
  l.arcTo(0, 5, 0, false, true, 10, 10);
  CHECK_EQ(l.verbs.back(), Path::Line);
  size_t n = l.verbs.size();
  l.arcTo(5, 5, 0, false, true, 10, 10);
  CHECK_EQ(l.verbs.size(), n);

  // Площадь круга из двух дуг.
  Path c;
  c.moveTo(10, 50);
  c.arcTo(40, 40, 0, false, true, 90, 50);
  c.arcTo(40, 40, 0, false, true, 10, 50);
  c.close();
  double a = maskArea(c, 100, 100);
  CHECK_NEAR(a, kPi * 1600, kPi * 1600 * 0.003);
}

TEST(gfx_raster_path_flatten_tolerance) {
  // Отклонение ломаной от окружности не больше допуска.
  for (float tol : {0.5f, 0.1f, 0.02f}) {
    Path c;
    c.addCircle(0, 0, 100);
    double worst = 0;
    size_t count = 0;
    bool closedSeen = false;
    c.flatten(tol, [&](const std::vector<Pt>& pts, bool closed) {
      closedSeen = closed;
      count += pts.size();
      for (size_t i = 0; i < pts.size(); i++) {
        Pt a = pts[i], b = pts[(i + 1) % pts.size()];
        Pt m = (a + b) * 0.5f;
        double r = std::sqrt(double(m.x) * m.x + double(m.y) * m.y);
        worst = std::max(worst, std::fabs(100 - r));
      }
    });
    CHECK(closedSeen);
    CHECK_MSG(worst <= tol * 1.05 + 0.03, strf("tol %.3f worst %.4f", tol, worst));  // + погрешность кубической дуги
    CHECK(count < size_t(4 * 1024));
  }
  // Подпуть без отрезков не выдаётся; открытый подпуть — closed = false.
  Path p;
  p.moveTo(1, 1);
  p.moveTo(2, 2);
  p.lineTo(3, 3);
  int calls = 0;
  p.flatten(0.1f, [&](const std::vector<Pt>& pts, bool closed) {
    calls++;
    CHECK_EQ(pts.size(), size_t(2));
    CHECK(!closed);
  });
  CHECK_EQ(calls, 1);
}

// ---------------------------------------------------------------- растеризатор

TEST(gfx_raster_circle_area) {
  // Площадь сглаженного круга совпадает с πr² (в пределах 0,5 %), при любом дробном центре.
  Rng rng(7);
  for (float r : {3.f, 7.5f, 20.f, 61.3f, 200.f}) {
    for (int k = 0; k < 4; k++) {
      float cx = 210 + float(rng.uniform()), cy = 210 + float(rng.uniform());
      Path c;
      c.addCircle(cx, cy, r);
      double area = maskArea(c, 420, 420);
      double expect = kPi * r * r;
      CHECK_MSG(std::fabs(area - expect) <= expect * 0.005, strf("r=%.1f area=%.3f expect=%.3f", r, area, expect));
    }
  }
}

TEST(gfx_raster_half_plane_edge) {
  // Край по середине пикселя — покрытие 50 %; целые края — резкие.
  Path p;
  p.addRect({10.5f, 0, 20, 10});
  Mask m(40, 10);
  rasterizeToMask(p, {}, m);
  for (int y = 0; y < 10; y++) {
    CHECK_NEAR(at(m, 10, y), 128, 1);
    CHECK_EQ(int(at(m, 9, y)), 0);
    CHECK_EQ(int(at(m, 11, y)), 255);
    CHECK_EQ(int(at(m, 29, y)), 255);
    CHECK_NEAR(at(m, 30, y), 128, 1);
    CHECK_EQ(int(at(m, 31, y)), 0);
  }
  // Четверть пикселя по обеим осям.
  Path q;
  q.addRect({2.75f, 3.75f, 5, 5});
  Mask n(12, 12);
  rasterizeToMask(q, {}, n);
  CHECK_NEAR(at(n, 2, 3), 255.0 / 16, 1);
  CHECK_NEAR(at(n, 2, 5), 255.0 / 4, 1);
  CHECK_EQ(int(at(n, 4, 5)), 255);
  // Наклонная прямая через центр пикселя: покрытие 50 %.
  Path t;
  t.moveTo(0, 0);
  t.lineTo(20, 20);
  t.lineTo(0, 20);
  t.close();
  Mask tm(20, 20);
  rasterizeToMask(t, {}, tm);
  for (int i = 0; i < 20; i++) CHECK_NEAR(at(tm, i, i), 128, 1);
  CHECK_NEAR(coverageSum(tm), 200, 0.5);
}

TEST(gfx_raster_exact_polygon_area) {
  // Произвольные треугольники: сумма покрытия = площадь (точность 1/256 пикселя).
  Rng rng(42);
  for (int k = 0; k < 50; k++) {
    Pt a{float(rng.uniform() * 60), float(rng.uniform() * 60)};
    Pt b{float(rng.uniform() * 60), float(rng.uniform() * 60)};
    Pt c{float(rng.uniform() * 60), float(rng.uniform() * 60)};
    double area = std::fabs(double(b.x - a.x) * (c.y - a.y) - double(c.x - a.x) * (b.y - a.y)) * 0.5;
    Path p;
    Pt t[3] = {a, b, c};
    p.addPolygon(t, 3);
    double got = maskArea(p, 64, 64);
    CHECK_MSG(std::fabs(got - area) <= 0.02 * std::sqrt(area) + 0.05, strf("area %.3f got %.3f", area, got));
  }
}

TEST(gfx_raster_fill_rules) {
  // Два круга одного направления: NonZero заполняет центр, EvenOdd даёт кольцо.
  Path p;
  p.addCircle(50, 50, 40);
  p.addCircle(50, 50, 20);
  double nz = maskArea(p, 100, 100, FillRule::NonZero);
  double eo = maskArea(p, 100, 100, FillRule::EvenOdd);
  CHECK_NEAR(nz, kPi * 1600, kPi * 1600 * 0.005);
  CHECK_NEAR(eo, kPi * (1600 - 400), kPi * 1200 * 0.005);
  Mask m(100, 100);
  rasterizeToMask(p, {}, m, 0, 0, FillRule::EvenOdd);
  CHECK_EQ(int(at(m, 50, 50)), 0);
  CHECK_EQ(int(at(m, 50, 20)), 255);
  rasterizeToMask(p, {}, m, 0, 0, FillRule::NonZero);
  CHECK_EQ(int(at(m, 50, 50)), 255);

  // Внутренний контур противоположного направления — отверстие и при NonZero.
  Path h;
  h.addRect({10, 10, 80, 80});
  h.moveTo(30, 30);
  h.lineTo(30, 70);
  h.lineTo(70, 70);
  h.lineTo(70, 30);
  h.close();
  CHECK_NEAR(maskArea(h, 100, 100, FillRule::NonZero), 6400 - 1600, 0.5);
  CHECK_NEAR(maskArea(h, 100, 100, FillRule::EvenOdd), 6400 - 1600, 0.5);

  // Пятиконечная звезда: EvenOdd оставляет центр пустым.
  Path star;
  Pt sp[5];
  for (int i = 0; i < 5; i++) {
    double a = -kPi / 2 + i * 4 * kPi / 5;
    sp[i] = {float(50 + 45 * std::cos(a)), float(50 + 45 * std::sin(a))};
  }
  star.addPolygon(sp, 5);
  Mask sm(100, 100);
  rasterizeToMask(star, {}, sm, 0, 0, FillRule::EvenOdd);
  CHECK_EQ(int(at(sm, 50, 52)), 0);
  rasterizeToMask(star, {}, sm, 0, 0, FillRule::NonZero);
  CHECK_EQ(int(at(sm, 50, 52)), 255);

  // Совпадающие рёбра соседних подпутей одного контура не дают шва.
  Path two;
  two.addRect({10.3f, 10, 20, 20});
  two.addRect({30.3f, 10, 20, 20});
  Mask tw(64, 40);
  rasterizeToMask(two, {}, tw);
  for (int y = 10; y < 30; y++) CHECK_EQ(int(at(tw, 30, y)), 255);
}

TEST(gfx_raster_clipping_and_huge) {
  // Огромные координаты: отсечение в double до перевода в фиксированную точку.
  Path p;
  p.addRect({-1e30f, -1e30f, 2e30f, 2e30f});
  Mask m(64, 48);
  rasterizeToMask(p, {}, m);
  for (u8 v : m.a) CHECK_EQ(int(v), 255);

  // Круг с центром далеко слева, видна только часть: площадь видимой части.
  Path c;
  c.addCircle(-100, 50, 130);
  double got = maskArea(c, 100, 100);
  // Аналитически: площадь круга в полосе x ∈ [0, 30], y ∈ [0, 100].
  double expect = 0;
  for (int i = 0; i < 300000; i++) {
    double x = (i + 0.5) / 300000 * 30;
    double hh = std::sqrt(std::max(0.0, 130.0 * 130 - (x + 100) * (x + 100)));
    double y0 = std::max(0.0, 50 - hh), y1 = std::min(100.0, 50 + hh);
    expect += std::max(0.0, y1 - y0) * (30.0 / 300000);
  }
  CHECK_NEAR(got, expect, expect * 0.003);

  // Фигура, частично уходящая вправо и вниз.
  Path r;
  r.addRect({90.5f, 90.5f, 1e9f, 1e9f});
  CHECK_NEAR(maskArea(r, 100, 100), 9.5 * 9.5, 0.05);

  // Целиком снаружи — пусто; гигантская окружность, проходящая через область, — без зависаний.
  Path o;
  o.addCircle(5000, 5000, 100);
  o.addRect({-50, -50, 10, 10});
  CHECK_EQ(maskArea(o, 100, 100), 0.0);
  Path g;
  g.addCircle(50, 1e7f, 1e7f - 50);  // край круга проходит по середине области
  double ga = maskArea(g, 100, 100);
  CHECK_NEAR(ga, 50 * 100, 60);

  // Растеризация в смещённую область (отрицательное начало).
  Path s;
  s.addRect({-10, -10, 5, 5});
  Mask sm(20, 20);
  rasterizeToMask(s, {}, sm, -20, -20);
  CHECK_NEAR(coverageSum(sm), 25, 0.01);
  CHECK_EQ(int(at(sm, 10, 10)), 255);
  CHECK_EQ(int(at(sm, 15, 15)), 0);
}

TEST(gfx_raster_degenerate_input) {
  Mask m(50, 50);
  // NaN и бесконечности — фигура не рисуется.
  Path n;
  n.moveTo(10, 10);
  n.lineTo(NAN, 20);
  n.lineTo(30, 40);
  n.close();
  rasterizeToMask(n, {}, m);
  CHECK_EQ(coverageSum(m), 0.0);
  Path inf;
  inf.moveTo(0, 0);
  inf.lineTo(INFINITY, 10);
  inf.lineTo(0, 30);
  rasterizeToMask(inf, {}, m);
  CHECK_EQ(coverageSum(m), 0.0);
  // NaN в преобразовании.
  Path r;
  r.addRect({0, 0, 10, 10});
  rasterizeToMask(r, Affine{NAN, 0, 0, 1, 0, 0}, m);
  CHECK_EQ(coverageSum(m), 0.0);
  // Нулевая площадь: точка, отрезок, туда-обратно.
  Path z;
  z.moveTo(5, 5);
  z.lineTo(5, 5);
  z.moveTo(1, 1);
  z.lineTo(40, 40);
  z.moveTo(3, 3);
  z.lineTo(30, 3);
  z.lineTo(3, 3);
  rasterizeToMask(z, {}, m);
  CHECK_EQ(coverageSum(m), 0.0);
  // Повреждённый контур (точек меньше, чем требуют команды) не читает за пределами.
  Path bad;
  bad.verbs = {Path::Move, Path::Cubic, Path::Line};
  bad.pts = {{1, 1}, {2, 2}};
  rasterizeToMask(bad, {}, m);
  CHECK_EQ(coverageSum(m), 0.0);
  // Пустая маска и пустая рамка.
  Mask e;
  rasterizeToMask(r, {}, e);
  Rasterizer ras;
  ras.reset({0, 0, 0, 0});
  ras.addPath(r);
  struct Count final : SpanSink { int rows = 0; void row(int, const Span*, int) override { rows++; } } cnt;
  ras.sweep(FillRule::NonZero, cnt);
  CHECK_EQ(cnt.rows, 0);
  // Отрезок после Close без Move начинает подпуть из начала предыдущего (оба треугольника по часовой).
  Path cl;
  cl.moveTo(0, 0);
  cl.lineTo(10, 0);
  cl.lineTo(10, 10);
  cl.close();
  cl.lineTo(10, 10);
  cl.lineTo(0, 10);
  cl.close();
  CHECK_NEAR(maskArea(cl, 20, 20), 100, 0.01);
}

TEST(gfx_raster_spans_sparse) {
  // Внутренность выдаётся сплошными пролётами, края — отдельными пикселями.
  Rasterizer ras;
  ras.reset({0, 0, 1000, 10});
  Path p;
  p.addRect({0.5f, 0, 998, 10});
  ras.addPath(p);
  struct S final : SpanSink {
    int rows = 0, spans = 0, solidLen = 0;
    void row(int, const Span* s, int n) override {
      rows++;
      spans += n;
      for (int i = 0; i < n; i++) if (!s[i].cov && s[i].value == 255) solidLen += s[i].len;
      for (int i = 1; i < n; i++) CHECK(s[i].x >= s[i - 1].x + s[i - 1].len);
    }
  } sink;
  ras.sweep(FillRule::NonZero, sink);
  CHECK_EQ(sink.rows, 10);
  CHECK_EQ(sink.spans, 30);
  CHECK_EQ(sink.solidLen, 997 * 10);
  RectI b = ras.bounds();
  CHECK_EQ(b.x, 0);
  CHECK_EQ(b.right(), 999);
}

TEST(gfx_raster_bands_consistent) {
  // Очень много ячеек: обработка полосами даёт ту же площадь.
  Path p;
  const int n = 400;
  double expect = 0;
  auto q = [](float v) { return std::floor(double(v) * 256 + 0.5) / 256; };  // вершины в сетке 1/256
  for (int i = 0; i < n; i++) {
    float y = 2 + i * 4.9f;
    Pt t[3] = {{1, y}, {1999, y + 1.3f}, {3, y + 4.2f}};
    p.addPolygon(t, 3);
    expect += std::fabs((q(t[1].x) - q(t[0].x)) * (q(t[2].y) - q(t[0].y)) - (q(t[2].x) - q(t[0].x)) * (q(t[1].y) - q(t[0].y))) * 0.5;
  }
  double got = maskArea(p, 2000, 2000);
  CHECK_NEAR(got, expect, expect * 3e-4);  // смещение 8-битного округления покрытия ≤ 0,5/255 на пиксель
  // То же в двух половинах по высоте — совпадение покрытия на стыке.
  Mask full(2000, 2000), top(2000, 1000), bottom(2000, 1000);
  rasterizeToMask(p, {}, full);
  rasterizeToMask(p, {}, top, 0, 0);
  rasterizeToMask(p, {}, bottom, 0, 1000);
  int diff = 0;
  for (int y = 0; y < 2000; y++)
    for (int x = 0; x < 2000; x++) {
      u8 a = full.a[size_t(y) * 2000 + size_t(x)];
      u8 b = y < 1000 ? top.a[size_t(y) * 2000 + size_t(x)] : bottom.a[size_t(y - 1000) * 2000 + size_t(x)];
      diff = std::max(diff, std::abs(int(a) - int(b)));
    }
  CHECK(diff <= 1);
}

TEST(gfx_raster_transform) {
  // Масштаб и поворот через преобразование: площадь умножается на определитель.
  Path p;
  p.addCircle(0, 0, 10);
  Affine m = Affine::translate(50, 50) * Affine::rotate(0.3f) * Affine::scale(3, 1.5f);
  double a = maskArea(p, 100, 100, FillRule::NonZero, m);
  CHECK_NEAR(a, kPi * 100 * 4.5, kPi * 450 * 0.005);
}
