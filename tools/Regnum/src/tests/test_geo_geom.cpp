// Тесты geo: точные предикаты, отрезки, кольца, polylabel, упрощение, сетка.
#include "geo/geom.h"
#include "geo/grid.h"
#include "tests/test.h"

using namespace rg;
using namespace rg::geo;

TEST(geo_orient_basic) {
  CHECK_EQ(orient({0, 0}, {1, 0}, {0, 1}), 1);
  CHECK_EQ(orient({0, 0}, {1, 0}, {0, -1}), -1);
  CHECK_EQ(orient({0, 0}, {1, 1}, {2, 2}), 0);
  CHECK_EQ(orient({0, 0}, {0, 0}, {5, 7}), 0);
  CHECK_EQ(orient({3, 3}, {3, 3}, {3, 3}), 0);
}

// Почти коллинеарные точки: наивная формула ошибается, точный предикат согласован при перестановках.
TEST(geo_orient_exact_near_degenerate) {
  // Классический пример: точки около прямой y = x с возмущением в младший бит.
  int mismatches = 0;
  for (int i = 0; i < 64; i++)
    for (int j = 0; j < 64; j++) {
      Vec2 p{0.5 + i * std::ldexp(1.0, -53), 0.5 + j * std::ldexp(1.0, -53)};
      Vec2 q{12, 12}, r{24, 24};
      int s = orient(p, q, r);
      // циклические перестановки сохраняют знак, транспозиция меняет
      CHECK_EQ(orient(q, r, p), s);
      CHECK_EQ(orient(r, p, q), s);
      CHECK_EQ(orient(q, p, r), -s);
      // знак совпадает с точным: p выше прямой y = x ⇔ j > i
      int expect = j > i ? 1 : (j < i ? -1 : 0);
      if (s != expect) mismatches++;
    }
  CHECK_EQ(mismatches, 0);
  // Большие координаты и крошечное смещение.
  Vec2 a{8000.123456789, 4500.987654321}, b{-7999.5, -4499.25};
  Vec2 m = (a + b) * 0.5;
  int s0 = orient(a, b, m);
  CHECK_EQ(orient(b, a, m), -s0);
  Vec2 up = m + Vec2(0, 1e-9), dn = m - Vec2(0, 1e-9);
  CHECK(orient(a, b, up) != orient(a, b, dn));
}

TEST(geo_seg_relation) {
  CHECK(segRelation({0, 0}, {2, 2}, {0, 2}, {2, 0}) == SegRel::Proper);
  CHECK(segRelation({0, 0}, {1, 1}, {2, 2}, {3, 3}) == SegRel::None);
  CHECK(segRelation({0, 0}, {2, 2}, {2, 2}, {3, 0}) == SegRel::Touch);       // общий конец
  CHECK(segRelation({0, 0}, {2, 0}, {1, 0}, {1, 5}) == SegRel::Touch);       // Т-образное касание
  CHECK(segRelation({0, 0}, {2, 0}, {1, 0}, {3, 0}) == SegRel::Overlap);
  CHECK(segRelation({0, 0}, {2, 0}, {2, 0}, {3, 0}) == SegRel::Touch);       // коллинеарны, касание концом
  CHECK(segRelation({0, 0}, {2, 0}, {2.5, 0}, {3, 0}) == SegRel::None);
  CHECK(segRelation({0, 0}, {0, 2}, {0, 1}, {0, 1}) == SegRel::Touch);       // точка на отрезке
  CHECK(segRelation({0, 0}, {0, 2}, {1, 1}, {1, 1}) == SegRel::None);
  CHECK(segRelation({0, 0}, {0, 4}, {0, 1}, {0, 3}) == SegRel::Overlap);     // вертикальное наложение
  CHECK(onSegment({1, 1}, {0, 0}, {2, 2}));
  CHECK(!onSegment({1, 1.0000001}, {0, 0}, {2, 2}));
  CHECK(!onSegment({3, 3}, {0, 0}, {2, 2}));
  double t = 0, u = 0;
  Vec2 x = crossPoint({0, 0}, {4, 0}, {1, -1}, {1, 3}, &t, &u);
  CHECK_NEAR(x.x, 1, 1e-12);
  CHECK_NEAR(x.y, 0, 1e-12);
  CHECK_NEAR(t, 0.25, 1e-12);
  CHECK_NEAR(u, 0.25, 1e-12);
  CHECK_NEAR(segSegDist2({0, 0}, {1, 0}, {0, 2}, {1, 2}), 4, 1e-12);
  CHECK(segmentsNear({0, 0}, {1, 0}, {0.5, 0.05}, {0.5, 1}, 0.1));
  CHECK(!segmentsNear({0, 0}, {1, 0}, {0.5, 0.2}, {0.5, 1}, 0.1));
}

TEST(geo_project_and_distance) {
  Proj p = project({1, 1}, {0, 0}, {2, 0});
  CHECK_NEAR(p.t, 0.5, 1e-12);
  CHECK_NEAR(p.d2, 1, 1e-12);
  p = project({-3, 4}, {0, 0}, {2, 0});
  CHECK_EQ(p.t, 0.0);
  CHECK_NEAR(p.d2, 25, 1e-12);
  p = project({1, 1}, {5, 5}, {5, 5});
  CHECK_EQ(p.t, 0.0);
  CHECK_NEAR(distToSeg({0, 3}, {-1, 0}, {1, 0}), 3, 1e-12);
}

TEST(geo_ring_area_pip) {
  std::vector<Vec2> sq{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
  CHECK_NEAR(signedArea(sq), 100, 1e-12);
  std::vector<Vec2> rsq(sq.rbegin(), sq.rend());
  CHECK_NEAR(signedArea(rsq), -100, 1e-12);
  std::vector<Vec2> hole{{3, 3}, {7, 3}, {7, 7}, {3, 7}};
  CHECK_NEAR(polygonArea({sq, hole}), 84, 1e-12);
  CHECK(pointInRing({5, 5}, sq));
  CHECK(!pointInRing({15, 5}, sq));
  CHECK(pointInPolygon({1, 1}, {sq, hole}));
  CHECK(!pointInPolygon({5, 5}, {sq, hole}));
  CHECK_NEAR(signedDist({1, 5}, {sq, hole}), 1, 1e-12);
  CHECK_NEAR(signedDist({5, 5}, {sq, hole}), -2, 1e-12);
  Vec2 c = ringCentroid(sq);
  CHECK_NEAR(c.x, 5, 1e-12);
  CHECK_NEAR(c.y, 5, 1e-12);
  Box2 b = bounds(sq);
  CHECK_EQ(b.x1, 10.0);
  // половинное правило: вершина на луче считается один раз
  std::vector<Vec2> diamond{{0, 5}, {5, 0}, {10, 5}, {5, 10}};
  CHECK(pointInRing({5, 5}, diamond));
  CHECK(!pointInRing({-1, 5}, diamond));
  CHECK(!pointInRing({11, 5}, diamond));
}

TEST(geo_is_simple) {
  CHECK(isSimple({{0, 0}, {10, 0}, {10, 10}, {0, 10}}, true));
  CHECK(!isSimple({{0, 0}, {10, 10}, {10, 0}, {0, 10}}, true));        // бабочка
  CHECK(!isSimple({{0, 0}, {10, 0}, {5, 0}, {5, 5}}, false));          // наложение соседних звеньев
  CHECK(!isSimple({{0, 0}, {10, 0}, {10, 10}, {5, 0}, {0, 10}}, true)); // касание вершиной
  CHECK(isSimple({{0, 0}, {1, 1}, {2, 0}}, false));
  CHECK(!isSimple({{0, 0}, {1, 1}, {1, 1}, {2, 0}}, false));           // повтор точки
  // большой простой (через сетку) и с одним самопересечением
  std::vector<Vec2> big;
  for (int i = 0; i < 2000; i++) {
    double t = 2 * kPi * i / 2000;
    big.push_back({std::cos(t) * (100 + 10 * std::sin(13 * t)), std::sin(t) * (100 + 10 * std::sin(13 * t))});
  }
  CHECK(isSimple(big, true));
  std::swap(big[100], big[900]);
  CHECK(!isSimple(big, true));
}

TEST(geo_polylabel) {
  std::vector<Vec2> sq{{0, 0}, {100, 0}, {100, 100}, {0, 100}};
  double d = 0;
  Vec2 p = polylabel({sq}, 0.5, &d);
  CHECK_NEAR(p.x, 50, 1);
  CHECK_NEAR(p.y, 50, 1);
  CHECK_NEAR(d, 50, 1);
  // L-образный: метка внутри и не ближе 15 к границе
  std::vector<Vec2> L{{0, 0}, {100, 0}, {100, 30}, {30, 30}, {30, 100}, {0, 100}};
  p = polylabel({L}, 0.5, &d);
  CHECK(pointInRing(p, L));
  CHECK(d >= 14.5);
  // кольцо с дырой в центре: метка в «бублике»
  std::vector<Vec2> hole{{20, 20}, {80, 20}, {80, 80}, {20, 80}};
  p = polylabel({sq, hole}, 0.5, &d);
  CHECK(pointInPolygon(p, {sq, hole}));
  CHECK(d >= 9.0);
  // крупный многоугольник (ветка с сеткой) совпадает с прямым перебором по качеству
  std::vector<Vec2> big;
  for (int i = 0; i < 3000; i++) {
    double t = 2 * kPi * i / 3000;
    double r = 500 + 60 * std::sin(5 * t) + 15 * std::sin(41 * t);
    big.push_back({1000 + r * std::cos(t), 1000 + r * std::sin(t)});
  }
  p = polylabel({big}, 1.0, &d);
  CHECK(pointInRing(p, big));
  CHECK_NEAR(d, distToRings(p, {big}), 1e-9);
  CHECK(d > 400);
  // очень тонкий
  std::vector<Vec2> thin{{0, 0}, {1000, 0}, {1000, 0.01}, {0, 0.01}};
  p = polylabel({thin}, 1, &d);
  CHECK(pointInRing(p, thin));
  CHECK(d > 0);
}

TEST(geo_simplify_dp) {
  std::vector<Vec2> line;
  for (int i = 0; i <= 100; i++) line.push_back({double(i), std::sin(i * 0.1) * 0.01});
  auto s = simplifyDP(line, 0.1);
  CHECK_EQ(s.size(), size_t(2));
  CHECK(s.front() == line.front());
  CHECK(s.back() == line.back());
  std::vector<Vec2> zig;
  for (int i = 0; i <= 10; i++) zig.push_back({double(i), (i % 2) ? 5.0 : 0.0});
  CHECK_EQ(simplifyDP(zig, 1).size(), zig.size());
  std::vector<Vec2> circle;
  for (int i = 0; i < 400; i++) circle.push_back({100 * std::cos(2 * kPi * i / 400), 100 * std::sin(2 * kPi * i / 400)});
  auto c = simplifyDP(circle, 0.5, true);
  CHECK(c.size() >= 3 && c.size() < 100);
  for (auto& p : circle) CHECK(distToRings(p, {c}) <= 0.5 + 1e-9);
}

// Две извилистые линии рядом и остров между ними: упрощение не создаёт пересечений и не «перепрыгивает» остров.
TEST(geo_simplify_topo) {
  std::vector<Vec2> a, b, island;
  for (int i = 0; i <= 400; i++) {
    double x = i * 0.5;
    a.push_back({x, 10 + 3 * std::sin(x * 0.7)});
    b.push_back({x, 14 + 3 * std::sin(x * 0.7 + 0.4)});
  }
  // остров у впадины линии a (x ≈ 105,47, a = 7, b ≈ 11,2): спрямление a «накрыло» бы его
  const double ix = (2 * kPi * 12 - kPi / 2) / 0.7;
  for (int i = 0; i < 12; i++) island.push_back({ix + 0.3 * std::cos(2 * kPi * i / 12), 8.5 + 0.3 * std::sin(2 * kPi * i / 12)});
  // проверка корректности исходного набора
  auto relOk = [](const std::vector<std::vector<Vec2>>& ls, const std::vector<bool>& cl) {
    for (size_t i = 0; i < ls.size(); i++) {
      if (!isSimple(ls[i], cl[i])) return false;
      for (size_t j = i + 1; j < ls.size(); j++) {
        size_t ni = cl[i] ? ls[i].size() : ls[i].size() - 1, nj = cl[j] ? ls[j].size() : ls[j].size() - 1;
        for (size_t s = 0; s < ni; s++)
          for (size_t t = 0; t < nj; t++)
            if (segRelation(ls[i][s], ls[i][(s + 1) % ls[i].size()], ls[j][t], ls[j][(t + 1) % ls[j].size()]) != SegRel::None)
              return false;
      }
    }
    return true;
  };
  std::vector<std::vector<Vec2>> lines{a, b, island};
  std::vector<bool> cl{false, false, true};
  CHECK(relOk(lines, cl));
  auto yAt = [](const std::vector<Vec2>& l, double x) {
    for (size_t i = 0; i + 1 < l.size(); i++)
      if (l[i].x <= x && l[i + 1].x >= x) return l[i].y + (l[i + 1].y - l[i].y) * (x - l[i].x) / (l[i + 1].x - l[i].x);
    return 0.0;
  };
  // остров между линиями: каждая его точка выше a и ниже b (по вертикали)
  auto between = [&](const std::vector<std::vector<Vec2>>& ls) {
    for (auto& p : ls[2])
      if (!(yAt(ls[0], p.x) < p.y && yAt(ls[1], p.x) > p.y)) return false;
    return true;
  };
  CHECK(between(lines));
  // наивное упрощение с большим допуском «перепрыгивает» остров
  std::vector<std::vector<Vec2>> naive{simplifyDP(a, 8), simplifyDP(b, 8), simplifyDP(island, 8, true)};
  CHECK(!between(naive));
  auto s = simplifyTopo(lines, cl, 8);
  CHECK(relOk(s, cl));
  CHECK(between(s));
  CHECK(s[0].size() < a.size() / 4);
  CHECK(s[1].size() < b.size() / 4);
  CHECK(s[0].front() == a.front() && s[0].back() == a.back());
  CHECK(s[2].size() >= 3);
}

TEST(geo_grid_queries_match_bruteforce) {
  Rng rng(7);
  std::vector<Vec2> a, b;
  for (int i = 0; i < 3000; i++) {
    Vec2 p{rng.uniform() * 1000, rng.uniform() * 600};
    double len = rng.uniform() < 0.1 ? 300 : 8;
    Vec2 q = p + Vec2(rng.uniform() - 0.5, rng.uniform() - 0.5) * len;
    a.push_back(p);
    b.push_back(q);
  }
  SegGrid g;
  g.build(a, b);
  std::vector<u32> stamp;
  for (int k = 0; k < 200; k++) {
    Box2 q;
    q.add({rng.uniform() * 1000, rng.uniform() * 600});
    q.add(q.center() + Vec2(rng.uniform() * 50, rng.uniform() * 50));
    std::vector<int> got, want;
    g.queryUnique(q, stamp, u32(k + 1), [&](int i) {
      Box2 sb;
      sb.add(a[size_t(i)]);
      sb.add(b[size_t(i)]);
      if (sb.intersects(q)) got.push_back(i);
    });
    for (int i = 0; i < 3000; i++) {
      // отрезок, реально пересекающий прямоугольник, обязан найтись
      Box2 sb;
      sb.add(a[size_t(i)]);
      sb.add(b[size_t(i)]);
      if (!sb.intersects(q)) continue;
      Vec2 c[4] = {{q.x0, q.y0}, {q.x1, q.y0}, {q.x1, q.y1}, {q.x0, q.y1}};
      bool hit = q.contains(a[size_t(i)]) || q.contains(b[size_t(i)]);
      for (int e = 0; e < 4 && !hit; e++) hit = segRelation(a[size_t(i)], b[size_t(i)], c[e], c[(e + 1) % 4]) != SegRel::None;
      if (hit) want.push_back(i);
    }
    std::sort(got.begin(), got.end());
    for (int i : want) CHECK(std::binary_search(got.begin(), got.end(), i));
    // ближайший
    Vec2 p{rng.uniform() * 1000, rng.uniform() * 600};
    double d = 0;
    int ni = g.nearest(p, kInf, &d);
    double best = kInf;
    for (int i = 0; i < 3000; i++) best = std::min(best, distToSeg(p, a[size_t(i)], b[size_t(i)]));
    CHECK(ni >= 0);
    CHECK_NEAR(d, best, 1e-9);
  }
}

TEST(geo_grid_boxes) {
  Rng rng(11);
  std::vector<Box2> boxes;
  for (int i = 0; i < 500; i++) {
    Vec2 a{rng.uniform() * 1000, rng.uniform() * 1000};
    boxes.push_back(Box2(a.x, a.y, a.x + rng.uniform() * 80, a.y + rng.uniform() * 80));
  }
  boxes.push_back(Box2());  // пустой не регистрируется
  SegGrid g;
  g.buildBoxes(boxes);
  std::vector<u32> stamp;
  for (int k = 0; k < 300; k++) {
    Vec2 p{rng.uniform() * 1000, rng.uniform() * 1000};
    Box2 q(p.x, p.y, p.x, p.y);
    std::vector<int> got;
    g.queryUnique(q, stamp, u32(k + 1), [&](int i) { if (boxes[size_t(i)].contains(p)) got.push_back(i); });
    std::sort(got.begin(), got.end());
    std::vector<int> want;
    for (int i = 0; i < int(boxes.size()); i++)
      if (boxes[size_t(i)].contains(p)) want.push_back(i);
    CHECK(got == want);
  }
}
