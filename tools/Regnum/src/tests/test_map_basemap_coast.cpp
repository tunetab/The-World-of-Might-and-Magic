// Тесты береговой линии базовой карты: marching squares, упрощение с сохранением топологии, проверка колец,
// маска моря и проверка настоящим ядром геометрии (geo::initFromCoast + geo::validate).
#include "tests/test_map_basemap_util.h"

using namespace rg;
using namespace rg::map::bake;

namespace {

// Поле берега: 255 — море, 0 — суша.
struct Field {
  int w, h;
  std::vector<u8> v;
  Field(int w_, int h_, u8 fillv = 255) : w(w_), h(h_), v(size_t(w_) * size_t(h_), fillv) {}
  void rect(int x0, int y0, int x1, int y1, u8 val) {
    for (int y = y0; y < y1; y++)
      for (int x = x0; x < x1; x++) v[size_t(y) * size_t(w) + size_t(x)] = val;
  }
  u8& at(int x, int y) { return v[size_t(y) * size_t(w) + size_t(x)]; }
};

double dist2Seg(Vec2 p, Vec2 a, Vec2 b) {
  const Vec2 ab = b - a;
  const double l2 = ab.len2();
  const double t = l2 > 0 ? clamp((p - a).dot(ab) / l2, 0.0, 1.0) : 0.0;
  return dist2(p, a + ab * t);
}

Vec2 toV(IPt p) { return Vec2(double(p.x) / kCoordScale, double(p.y) / kCoordScale); }

// Наибольшее отклонение точек исходного кольца от упрощённого (пиксели).
double maxDeviation(const IRing& raw, const IRing& simp) {
  double worst = 0;
  for (const IPt& p : raw) {
    double best = kInf;
    for (size_t k = 0; k < simp.size(); k++) best = std::min(best, dist2Seg(toV(p), toV(simp[k]), toV(simp[(k + 1) % simp.size()])));
    worst = std::max(worst, std::sqrt(best));
  }
  return worst;
}

bool inRing(Vec2 p, const IRing& r) {
  bool in = false;
  for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) {
    const Vec2 a = toV(r[j]), b = toV(r[i]);
    if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y)) in = !in;
  }
  return in;
}

// Случайное «пятнистое» поле: сумма гауссовых пятен, с мягким краем; детерминировано по зерну.
Field blobs(u64 seed, int w, int h, int count) {
  Rng rng(seed);
  struct B { double x, y, r, s; };
  std::vector<B> bs;
  for (int i = 0; i < count; i++) bs.push_back({rng.uniform() * w, rng.uniform() * h, 2 + rng.uniform() * 14, rng.uniform() < 0.25 ? -1.0 : 1.0});
  Field f(w, h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      double s = 0;
      for (const B& b : bs) {
        const double d2 = (x + 0.5 - b.x) * (x + 0.5 - b.x) + (y + 0.5 - b.y) * (y + 0.5 - b.y);
        s += b.s * std::exp(-d2 / (b.r * b.r));
      }
      // s > 0,5 — суша; плавный переход через изолинию.
      f.at(x, y) = u8(clamp(int(std::lround(127.5 - (s - 0.5) * 300)), 0, 255));
    }
  return f;
}

}  // namespace

TEST(map_basemap_coast_square) {
  Field f(40, 30);
  f.rect(10, 8, 20, 18, 0);  // 10 × 10 суши
  CoastStats st;
  const auto rings = traceCoast(f.v.data(), f.w, f.h, 0, &st);
  CHECK_EQ(rings.size(), size_t(1));
  // Изолиния посередине между центрами пикселей: квадрат 10 × 10 со срезанными углами (−4 · 1/8).
  CHECK_NEAR(ringArea(rings[0]), 99.5, 1e-9);
  CHECK_EQ(st.holes, 0);
  const RingCheck c = checkRings(rings, f.w, f.h);
  CHECK(c.ok());
  CHECK(inRing({15, 13}, rings[0]));
  CHECK(!inRing({5, 5}, rings[0]));
}

TEST(map_basemap_coast_subpixel_interpolation) {
  Field f(20, 10);
  f.rect(5, 0, 20, 10, 0);
  for (int y = 0; y < 10; y++) f.at(4, y) = 64;  // промежуточное значение: изолиния смещается к морю
  const auto rings = traceCoast(f.v.data(), f.w, f.h, 0);
  CHECK_EQ(rings.size(), size_t(1));
  // Между центрами 3,5 (255) и 4,5 (64): t = (127,5 − 255) / (64 − 255) ≈ 0,6675 → x ≈ 4,17.
  double minX = kInf;
  for (const IPt& p : rings[0])
    if (p.y > 100 && p.y < 900) minX = std::min(minX, p.x / 100.0);
  CHECK_NEAR(minX, 3.5 + (127.5 - 255.0) / (64.0 - 255.0), 0.006);
}

TEST(map_basemap_coast_border_and_orientation) {
  Field f(30, 20);
  f.rect(0, 0, 12, 20, 0);  // суша у левого края во всю высоту
  const auto rings = traceCoast(f.v.data(), f.w, f.h, 0);
  CHECK_EQ(rings.size(), size_t(1));
  CHECK(ringArea(rings[0]) > 0);  // ориентация как у geo::signedArea
  bool onLeft = false, onTop = false;
  for (const IPt& p : rings[0]) {
    onLeft = onLeft || p.x == 0;
    onTop = onTop || p.y == 0;
    CHECK(p.x >= 0 && p.y >= 0 && p.x <= 3000 && p.y <= 2000);
  }
  CHECK(onLeft && onTop);
  CHECK(checkRings(rings, f.w, f.h).ok());
}

TEST(map_basemap_coast_holes_and_islets) {
  Field f(60, 40);
  f.rect(5, 5, 35, 35, 0);   // остров с замкнутой лагуной
  f.rect(15, 15, 25, 25, 255);
  f.rect(45, 10, 47, 12, 0);  // островок 2 × 2: площадь 3,5 < 12
  f.rect(45, 20, 50, 25, 0);  // островок 5 × 5: площадь 24,5
  CoastStats st;
  const auto rings = traceCoast(f.v.data(), f.w, f.h, 12, &st);
  CHECK_EQ(rings.size(), size_t(2));
  CHECK_EQ(st.holes, 1);
  CHECK_EQ(st.islets, 1);
  CHECK_EQ(st.rawRings, 4);
  CHECK(checkRings(rings, f.w, f.h).ok());
}

TEST(map_basemap_coast_saddles) {
  // Шахматные диагонали: суша касается углами. Седло решается суммой значений клетки; кольца не касаются.
  for (int seaV : {255, 140}) {
    Field f(16, 16);
    for (int y = 4; y < 12; y++)
      for (int x = 4; x < 12; x++)
        if ((x + y) % 2 == 0) f.at(x, y) = 0;
        else f.at(x, y) = u8(seaV);
    const auto rings = traceCoast(f.v.data(), f.w, f.h, 0);
    CHECK(!rings.empty());
    const RingCheck c = checkRings(rings, f.w, f.h);
    CHECK_MSG(c.ok(), strf("море %d: self %lld ring %lld nested %lld deg %lld", seaV, (long long)c.selfHits, (long long)c.ringHits,
                           (long long)c.nested, (long long)c.degenerate));
    const GeoCheck g = geoCheck(toCoast(rings, f.w, f.h));
    CHECK_MSG(g.ok(), g.built ? (g.issues.empty() ? "площадь" : g.issues[0]) : g.error);
  }
}

TEST(map_basemap_coast_simplify_bounds_and_topology) {
  // Круг из многих точек: упрощение в пределах допуска; два близких острова после упрощения не касаются.
  Field f(200, 120);
  for (int y = 0; y < f.h; y++)
    for (int x = 0; x < f.w; x++) {
      const double d1 = std::hypot(x + 0.5 - 60, y + 0.5 - 60) - 45, d2 = std::hypot(x + 0.5 - 140.6, y + 0.5 - 60) - 34.5;
      const double d = std::min(d1, d2);
      f.at(x, y) = u8(clamp(int(std::lround(127.5 + d * 60)), 0, 255));
    }
  const auto raw = traceCoast(f.v.data(), f.w, f.h, 0);
  CHECK_EQ(raw.size(), size_t(2));
  for (double tol : {0.25, 0.75, 2.0, 6.0}) {
    CoastStats st;
    const auto simp = simplifyCoast(raw, tol, f.w, f.h, &st);
    CHECK_EQ(simp.size(), raw.size());
    for (size_t r = 0; r < raw.size(); r++) {
      CHECK(simp[r].size() >= 4);
      CHECK(simp[r].size() < raw[r].size());
      const double dev = maxDeviation(raw[r], simp[r]);
      // Отклонение — допуск, кроме точек, возвращённых ради топологии (они уменьшают отклонение).
      CHECK_MSG(dev <= tol + 0.011, strf("tol %.2f: отклонение %.3f", tol, dev));
    }
    const RingCheck c = checkRings(simp, f.w, f.h);
    CHECK_MSG(c.ok(), strf("tol %.2f: пересечений %lld", tol, (long long)c.ringHits));
    CHECK(c.minGap > 0);
  }
}

TEST(map_basemap_coast_fuzz_valid_rings) {
  for (u64 seed = 1; seed <= 40; seed++) {
    const Field f = blobs(seed, 160, 110, 26);
    CoastStats st;
    const auto raw = traceCoast(f.v.data(), f.w, f.h, 4, &st);
    const RingCheck rc = checkRings(raw, f.w, f.h);
    CHECK_MSG(rc.ok(), strf("seed %llu: исходные кольца некорректны", (unsigned long long)seed));
    for (double tol : {0.75, 3.0}) {
      const auto simp = simplifyCoast(raw, tol, f.w, f.h, &st);
      const RingCheck c = checkRings(simp, f.w, f.h);
      CHECK_MSG(c.ok(), strf("seed %llu tol %.2f: self %lld ring %lld nested %lld deg %lld", (unsigned long long)seed, tol, (long long)c.selfHits,
                             (long long)c.ringHits, (long long)c.nested, (long long)c.degenerate));
      {
        const GeoCheck g = geoCheck(toCoast(simp, f.w, f.h));
        CHECK_MSG(g.ok(), strf("seed %llu: %s; граней суши %d из %zu, площадь %.4f против %.4f", (unsigned long long)seed,
                               g.built ? (g.issues.empty() ? "площадь" : g.issues[0].c_str()) : g.error.c_str(), g.landFaces, simp.size(),
                               g.landArea, g.ringArea));
        CHECK_EQ(g.landFaces, int(simp.size()));
      }
    }
  }
}

TEST(map_basemap_coast_frame_and_corners) {
  // Суша у рамки и в углах: точки в 0,5 пикселя от рамки лежат на рамке, углы не выпадают при упрощении —
  // ядро геометрии ничего не «прилепляет», и площадь суши по граням совпадает с кольцами точно.
  Field f(50, 40);
  f.at(0, 0) = 0;                // одиночный пиксель в углу
  f.rect(45, 0, 50, 6, 0);       // угол (w, 0)
  f.rect(0, 30, 12, 40, 0);      // угол (0, h)
  f.rect(20, 37, 30, 40, 0);     // у нижнего края
  f.rect(49, 15, 50, 25, 0);     // столбец у правого края
  for (int x = 30; x < 44; x += 3) f.at(x, 0) = 0;  // пунктир по верхнему краю
  f.at(25, 20) = 0;              // одиночный пиксель внутри
  CoastStats st;
  const auto raw = traceCoast(f.v.data(), f.w, f.h, 0, &st);
  CHECK(checkRings(raw, f.w, f.h).ok());
  for (const IRing& r : raw)
    for (const IPt& p : r) {
      const double x = p.x / 100.0, y = p.y / 100.0;
      const double d = std::min(std::min(x, f.w - x), std::min(y, f.h - y));
      CHECK_MSG(d == 0 || d > 0.5, strf("точка (%.2f, %.2f) в %.2f пикселя от рамки", x, y, d));
    }
  int corners = 0;
  for (const IRing& r : raw)
    for (const IPt& p : r) corners += (p.x == 0 || p.x == f.w * 100) && (p.y == 0 || p.y == f.h * 100);
  CHECK_EQ(corners, 3);
  for (double tol : {0.0, 0.75, 3.0}) {
    const auto rings = tol > 0 ? simplifyCoast(raw, tol, f.w, f.h) : raw;
    int kept = 0;
    for (const IRing& r : rings)
      for (const IPt& p : r) kept += (p.x == 0 || p.x == f.w * 100) && (p.y == 0 || p.y == f.h * 100);
    CHECK_EQ(kept, 3);
    CHECK(checkRings(rings, f.w, f.h).ok());
    const GeoCheck g = geoCheck(toCoast(rings, f.w, f.h));
    CHECK_MSG(g.ok(), strf("tol %.2f: %s, площадь %.4f против %.4f", tol, g.built ? (g.issues.empty() ? "площадь" : g.issues[0].c_str()) : g.error.c_str(),
                           g.landArea, g.ringArea));
    CHECK_EQ(g.landFaces, int(rings.size()));
  }
}

TEST(map_basemap_coast_check_detects_defects) {
  auto sq = [](i32 x, i32 y, i32 s) { return IRing{{x, y}, {x, y + s}, {x + s, y + s}, {x + s, y}}; };
  // Ориентация: площадь > 0 по формуле Гаусса (как у колец traceCoast).
  IRing a = sq(100, 100, 500);
  if (ringArea(a) < 0) std::reverse(a.begin(), a.end());
  CHECK(checkRings({a}, 10, 10).ok());
  IRing b = sq(400, 400, 500);
  if (ringArea(b) < 0) std::reverse(b.begin(), b.end());
  CHECK(checkRings({a, b}, 10, 10).ringHits > 0);  // пересекаются
  IRing c = sq(200, 200, 100);
  if (ringArea(c) < 0) std::reverse(c.begin(), c.end());
  CHECK(checkRings({a, c}, 10, 10).nested > 0);  // вложено
  const IRing bow{{0, 0}, {500, 500}, {500, 0}, {0, 500}};
  CHECK(checkRings({bow}, 10, 10).selfHits + checkRings({bow}, 10, 10).degenerate > 0);  // восьмёрка
  CHECK(checkRings({IRing{{0, 0}, {100, 0}}}, 10, 10).degenerate == 1);
  IRing out = sq(900, 900, 500);
  if (ringArea(out) < 0) std::reverse(out.begin(), out.end());
  CHECK(checkRings({out}, 10, 10).outside > 0);  // за картой
  IRing touch = sq(600, 100, 300);                // касается a по стороне x = 600
  if (ringArea(touch) < 0) std::reverse(touch.begin(), touch.end());
  CHECK(checkRings({a, touch}, 20, 20).ringHits > 0);
}

TEST(map_basemap_coast_ocean_mask) {
  Field f(40, 40);
  f.rect(8, 8, 32, 32, 0);
  const auto rings = traceCoast(f.v.data(), f.w, f.h, 0);
  const std::vector<u8> m = oceanMask(rings, 10, 10, 4);  // клетка 4 × 4
  CHECK_EQ(m.size(), size_t(100));
  int land = 0;
  for (u8 v : m) land += v == 0;
  CHECK_EQ(land, 36);  // клетки 2..7 по обеим осям
  CHECK_EQ(int(m[0]), 255);
  CHECK_EQ(int(m[5 * 10 + 5]), 0);
}

TEST(map_basemap_coast_synthetic_map) {
  bmtest::SynthMap m = bmtest::makeSynth();
  flattenOnWhite(m.img);
  const Options opt;
  const Layers L = segment(m.img, opt);
  CoastStats st;
  const auto raw = traceCoast(L.field.data(), L.w, L.h, opt.minIsletArea, &st);
  const auto rings = simplifyCoast(raw, opt.simplifyTol, L.w, L.h, &st);
  CHECK(checkRings(rings, L.w, L.h).ok());
  // Материк — одно кольцо; островки протока (4) и моря (2) — отдельные кольца; крупинки и пунктир отброшены.
  int big = 0, small = 0;
  for (const IRing& r : rings) {
    const double a = ringArea(r);
    if (a > 10000) big++;
    else if (a > 15 && a < 60) small++;
    else CHECK_MSG(false, strf("неожиданное кольцо площадью %.1f", a));
  }
  CHECK_EQ(big, 1);
  CHECK_EQ(small, 6);
  // Озеро и река — суша для берега; залив и открытое море — нет.
  auto inAny = [&](Vec2 p) {
    for (const IRing& r : rings)
      if (inRing(p, r)) return true;
    return false;
  };
  CHECK(inAny(m.lake + Vec2(0.5, 0.5)));
  CHECK(inAny(m.river + Vec2(0.5, 0.5)));
  CHECK(inAny(m.land + Vec2(0.5, 0.5)));
  CHECK(inAny(m.tower + Vec2(0.5, 0.5)));
  CHECK(!inAny(m.bay + Vec2(0.5, 0.5)));
  CHECK(!inAny(m.sea + Vec2(0.5, 0.5)));
  CHECK(!inAny(m.dot + Vec2(0.5, 0.5)));
  // Настоящее ядро геометрии.
  const GeoCheck g = geoCheck(toCoast(rings, L.w, L.h));
  CHECK_MSG(g.ok(), g.built ? (g.issues.empty() ? "площадь" : g.issues[0]) : g.error);
  CHECK_EQ(g.landFaces, 7);
  CHECK(g.coastEdges >= 7);
  CHECK_NEAR(g.landArea, g.ringArea, 1e-6 * g.ringArea);
}
