// Regnum — разборщик путей SVG: команды, числа, флаги дуг, отражение контрольных точек, ошибки, запись.
#include "gfx/svgpath.h"
#include "tests/test.h"

using namespace rg;
using namespace rg::gfx;

namespace {

bool near(Pt a, Pt b, float eps = 1e-4f) { return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps; }

std::string verbs(const Path& p) {
  std::string s;
  for (Path::Verb v : p.verbs) s += "MLQCZ"[int(v)];
  return s;
}

}  // namespace

TEST(gfx_icons_svgpath_basic_commands) {
  Path p;
  CHECK(parseSvgPath("M10 20 L30 40 H50 V60 Z", p));
  CHECK_EQ(verbs(p), std::string("MLLLZ"));
  CHECK(near(p.pts[0], {10, 20}));
  CHECK(near(p.pts[1], {30, 40}));
  CHECK(near(p.pts[2], {50, 40}));
  CHECK(near(p.pts[3], {50, 60}));
}

TEST(gfx_icons_svgpath_relative_and_implicit) {
  Path p;
  CHECK(parseSvgPath("m10 10 5 0 0 5h-5v-5z m20 0 l1 1 1 1", p));
  // m с повтором — l; после z новый m относительно начала прежнего подпути.
  CHECK_EQ(verbs(p), std::string("MLLLLZMLL"));
  CHECK(near(p.pts[1], {15, 10}));
  CHECK(near(p.pts[2], {15, 15}));
  CHECK(near(p.pts[3], {10, 15}));
  CHECK(near(p.pts[4], {10, 10}));
  CHECK(near(p.pts[5], {30, 10}));
  CHECK(near(p.pts[7], {32, 12}));
  Path q;
  CHECK(parseSvgPath("M0 0L1 1 2 2 3 3", q));
  CHECK_EQ(verbs(q), std::string("MLLL"));
  CHECK(near(q.pts[3], {3, 3}));
}

TEST(gfx_icons_svgpath_number_forms) {
  Path p;
  CHECK(parseSvgPath("M10-5L.5.5L-.25-1e1L1E2+3L1.5e-1,2.5E+1", p));
  CHECK_EQ(p.pts.size(), size_t(5));
  CHECK(near(p.pts[0], {10, -5}));
  CHECK(near(p.pts[1], {0.5f, 0.5f}));
  CHECK(near(p.pts[2], {-0.25f, -10}));
  CHECK(near(p.pts[3], {100, 3}));
  CHECK(near(p.pts[4], {0.15f, 25}));
  Path q;
  CHECK(parseSvgPath("  M 1 , 2\n\tL\r3 4  ", q));
  CHECK(near(q.pts[1], {3, 4}));
  Path w;
  CHECK(parseSvgPath("M0.000000000000000000000001 123456789012345678901234567890e-29", w));
  CHECK(near(w.pts[0], {0, 1.23456789f}, 1e-5f));
}

TEST(gfx_icons_svgpath_curves_and_reflection) {
  Path p;
  CHECK(parseSvgPath("M0 0C0 10 10 10 10 0S20-10 20 0", p));
  CHECK_EQ(verbs(p), std::string("MCC"));
  // Первая контрольная точка S — отражение (10,10) относительно (10,0).
  CHECK(near(p.pts[4], {10, -10}));
  CHECK(near(p.pts[5], {20, -10}));
  CHECK(near(p.pts[6], {20, 0}));
  Path q;
  CHECK(parseSvgPath("M0 0Q5 10 10 0T20 0t10 0", q));
  CHECK_EQ(verbs(q), std::string("MQQQ"));
  CHECK(near(q.pts[3], {15, -10}));
  CHECK(near(q.pts[5], {25, 10}));
  CHECK(near(q.pts[6], {30, 0}));
  // S без предыдущей C: первая контрольная точка — текущая.
  Path r;
  CHECK(parseSvgPath("M5 5S10 10 15 5", r));
  CHECK(near(r.pts[1], {5, 5}));
  // Относительные кривые.
  Path s;
  CHECK(parseSvgPath("M10 10c0 10 10 10 10 0q5-5 10 0", s));
  CHECK(near(s.pts[3], {20, 10}));
  CHECK(near(s.pts[4], {25, 5}));
  CHECK(near(s.pts[5], {30, 10}));
}

TEST(gfx_icons_svgpath_arcs) {
  Path p;
  CHECK(parseSvgPath("M0 0a5 5 0 0 1 10 0", p));
  CHECK(!p.verbs.empty());
  CHECK(near(p.pts.back(), {10, 0}, 1e-3f));
  const RectF b = p.bounds();
  // Дуга по часовой (sweep=1) из (0,0) в (10,0) проходит сверху (ось Y вниз): y ≈ -5.
  CHECK(b.y < -4.5f && b.y > -5.6f);
  // Слитые флаги: «a1 1 0 01 2 2» = rx 1, ry 1, rot 0, large 0, sweep 1, x 2, y 2.
  Path q;
  CHECK(parseSvgPath("M0 0a1 1 0 01 2 2", q));
  CHECK(near(q.pts.back(), {2, 2}, 1e-3f));
  Path r;
  CHECK(parseSvgPath("M0 0A1,1,0,1,0,2,0", r));
  CHECK(near(r.pts.back(), {2, 0}, 1e-3f));
  // Нулевой радиус — отрезок.
  Path z;
  CHECK(parseSvgPath("M0 0A0 0 0 0 0 5 5", z));
  CHECK(near(z.pts.back(), {5, 5}));
  // Неверный флаг.
  Path bad;
  SvgPathError e;
  CHECK(!parseSvgPath("M0 0A5 5 0 2 1 10 0", bad, &e));
  CHECK(e.pos > 0);
}

TEST(gfx_icons_svgpath_close_and_restart) {
  Path p;
  // После Z линия начинается от начальной точки подпути.
  CHECK(parseSvgPath("M10 10H20V20ZL5 5", p));
  CHECK_EQ(verbs(p), std::string("MLLZML"));
  CHECK(near(p.pts[3], {10, 10}));
  CHECK(near(p.pts[4], {5, 5}));
  Path q;
  CHECK(parseSvgPath("M1 1zzz", q));
  CHECK_EQ(verbs(q), std::string("MZ"));
}

TEST(gfx_icons_svgpath_errors_keep_prefix) {
  {
    Path p;
    SvgPathError e;
    CHECK(!parseSvgPath("L10 10", p, &e));
    CHECK(p.empty());
    CHECK_EQ(e.pos, size_t(0));
  }
  {
    Path p;
    CHECK(!parseSvgPath("M10 10 L20", p));
    CHECK_EQ(verbs(p), std::string("M"));
  }
  {
    Path p;
    SvgPathError e;
    CHECK(!parseSvgPath("M1 2 L3 4 x 5 6", p, &e));
    CHECK_EQ(verbs(p), std::string("ML"));
    CHECK_EQ(e.pos, size_t(10));
  }
  {
    Path p;
    CHECK(!parseSvgPath("M1,,2", p));
    CHECK(!parseSvgPath("M1 2,", p));
    CHECK(!parseSvgPath("M1 2Z 3 4", p));
    CHECK(!parseSvgPath("M1e999 0", p));
    CHECK(!parseSvgPath("M- 1 2", p));
    CHECK(!parseSvgPath("M.e1 2", p));
  }
  {
    Path p;
    CHECK(parseSvgPath("", p));
    CHECK(p.empty());
    CHECK(parseSvgPath("   \n ", p));
    CHECK(p.empty());
  }
}

TEST(gfx_icons_svgpath_fuzz_no_crash) {
  const char alphabet[] = "MmLlHhVvCcSsQqTtAaZz0123456789.,-+eE \t";
  Rng rng(42);
  size_t parsed = 0;
  for (int i = 0; i < 20000; i++) {
    std::string s;
    const int n = rng.range(0, 60);
    for (int k = 0; k < n; k++) s += alphabet[rng.range(0, int(sizeof alphabet) - 2)];
    if (rng.range(0, 3) == 0) s.insert(0, "M0 0");
    Path p;
    SvgPathError e;
    if (parseSvgPath(s, p, &e)) parsed++;
    else CHECK(e.pos <= s.size());
    for (Pt q : p.pts) CHECK(std::isfinite(q.x) && std::isfinite(q.y));
    // Число точек согласовано с командами.
    size_t need = 0;
    for (Path::Verb v : p.verbs) need += v == Path::Move || v == Path::Line ? 1 : v == Path::Quad ? 2 : v == Path::Cubic ? 3 : 0;
    CHECK_EQ(need, p.pts.size());
  }
  CHECK(parsed > 0);
  // Двоичный мусор.
  for (int i = 0; i < 2000; i++) {
    std::string s;
    const int n = rng.range(0, 40);
    for (int k = 0; k < n; k++) s += char(rng.range(0, 255));
    Path p;
    parseSvgPath(s, p);
  }
}

TEST(gfx_icons_svgpath_write_roundtrip) {
  CHECK_EQ(svgNum(1.5), std::string("1.5"));
  CHECK_EQ(svgNum(-0.0001), std::string("0"));
  CHECK_EQ(svgNum(2.0), std::string("2"));
  CHECK_EQ(svgNum(0.125, 2), std::string("0.13"));
  CHECK_EQ(svgNum(-12.3456, 3), std::string("-12.346"));
  CHECK_EQ(svgNum(std::nan(""), 3), std::string("0"));
  const char* src = "M3 4L10 4Q12 6 10 8C8 10 4 10 3 8ZM20 20L21 22";
  Path p = svgPath(src);
  const std::string out = toSvgPath(p);
  Path q = svgPath(out);
  CHECK_EQ(verbs(p), verbs(q));
  CHECK_EQ(p.pts.size(), q.pts.size());
  for (size_t i = 0; i < p.pts.size(); i++) CHECK(near(p.pts[i], q.pts[i], 1e-3f));
}

TEST(gfx_icons_svgpath_perf) {
  // Длинная строка: 20 000 кривых.
  std::string d = "M0 0";
  for (int i = 0; i < 20000; i++) d += "c1.5 -2.25 3.125 2 4.5e0 0.5s1 1 2 -0.75";
  const double t0 = nowSeconds();
  Path p;
  CHECK(parseSvgPath(d, p));
  const double ms = (nowSeconds() - t0) * 1000;
  CHECK_EQ(p.verbs.size(), size_t(40001));
  std::printf("  svgpath: %.1f МБ/с (%zu байт за %.2f мс)\n", d.size() / 1e6 / std::max(1e-6, ms / 1000), d.size(), ms);
  CHECK(ms < 500);
}
