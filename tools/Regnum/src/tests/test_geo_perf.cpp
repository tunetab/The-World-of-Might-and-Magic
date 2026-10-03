// Производительность geo на картах масштаба 8000 × 4500:
//  а) берег ~58 тыс. точек (материк 20 тыс. + ~1500 островов) — initFromCoast, buildFaces, validate, операции;
//  б) разбиение 46 × 46 провинций с извилистыми границами (~2100 граней, ~4300 дуг, ~41 тыс. точек) — buildFaces.
#include "tests/test_geo_util.h"

using namespace rg;
using namespace rg::geo;

namespace {

constexpr double W = 8000, H = 4500;

Coast bigCoast(size_t* points = nullptr) {
  Coast c;
  c.width = W;
  c.height = H;
  // материк: звёздная кривая с высокочастотной изрезанностью
  std::vector<Vec2> m;
  const int n = 20000;
  const Vec2 cc{4000, 2250};
  for (int i = 0; i < n; i++) {
    double t = 2 * kPi * i / n;
    double r = 1500 * (1 + 0.15 * std::sin(3 * t) + 0.08 * std::sin(7 * t + 1) + 0.03 * std::sin(31 * t) + 0.01 * std::sin(97 * t) +
                       0.004 * std::sin(331 * t));
    m.push_back({cc.x + r * std::cos(t) * 1.25, cc.y + r * std::sin(t)});
  }
  c.landRings.push_back(std::move(m));
  // острова по сетке 110 × 110 вне материка
  Rng rng(5);
  for (double y = 55; y + 55 <= H; y += 110)
    for (double x = 55; x + 55 <= W; x += 110) {
      Vec2 d{(x - cc.x) / 1.25, y - cc.y};
      if (d.len() < 1500 * 1.29 + 80) continue;
      c.landRings.push_back(geotest::starRing({x, y}, 28 + 8 * rng.uniform(), 24, rng, 0.3));
    }
  if (points) {
    *points = 0;
    for (auto& r : c.landRings) *points += r.size();
  }
  return c;
}

template <class F> double bestOf(int n, F&& f) {
  double best = kInf;
  for (int i = 0; i < n; i++) {
    double t0 = nowSeconds();
    f();
    best = std::min(best, nowSeconds() - t0);
  }
  return best;
}

}  // namespace

TEST(geo_perf_big_coast) {
  size_t pts = 0;
  Coast coast = bigCoast(&pts);
  CHECK(pts > 50000);
  Store s;
  double t0 = nowSeconds();
  s.transact("init", [&](Tx& tx) { initFromCoast(tx, coast); });
  double tInit = nowSeconds() - t0;
  const World& w = s.world();
  std::shared_ptr<const FaceSet> fs;
  double tFaces = bestOf(5, [&] { fs = buildFaces(w); });
  // подписи граней (polylabel) последовательно — в buildFaces они считаются параллельно
  double tLabels = bestOf(1, [&] {
    for (auto& f : fs->faces) polylabel(f.rings, clamp(std::min(f.box.w(), f.box.h()) / 50.0, 1e-3, 1.0));
  });
  std::vector<Issue> is;
  double tValidate = bestOf(3, [&] { is = validate(w); });
  CHECK_MSG(is.empty(), geotest::issuesText(is));
  CHECK_NEAR(geotest::faceAreaSum(*fs), W * H, 1e-6 * W * H);
  size_t arcs = w.edges.size(), faces = fs->faces.size();
  CHECK_MSG(arcs > 3000, std::to_string(arcs));
  CHECK_MSG(faces > 1500, std::to_string(faces));
  // поиск по точке
  Rng rng(1);
  int found = 0;
  double tLocate = nowSeconds();
  for (int i = 0; i < 100000; i++) found += fs->locate({rng.uniform() * W, rng.uniform() * H}) >= 0;
  tLocate = (nowSeconds() - tLocate) / 100000;
  CHECK_EQ(found, 100000);
  // операции на большой карте
  t0 = nowSeconds();
  Id a = s.transact("a", [](Tx& tx) {
    return createProvince(tx, {{4800, 1500}, {6400, 1500}, {6400, 3000}, {4800, 3000}}, Terrain::Land);
  });
  double tCreate = nowSeconds() - t0;
  t0 = nowSeconds();
  Id b = s.transact("b", [&](Tx& tx) { return split(tx, a, {{4700, 2200}, {5600, 2300}, {6500, 2250}}); });
  double tSplit = nowSeconds() - t0;
  t0 = nowSeconds();
  s.transact("fill", [&](Tx& tx) { fillAt(tx, {55 + 110 * 3, 55 + 110 * 2}, 0); });
  double tFill = nowSeconds() - t0;
  Handle h;
  s.world().edges.each([&](const Edge& e) {
    if (!h && e.kind == EdgeKind::Border && !e.pts.empty()) { h.kind = Handle::Point; h.edge = e.id; h.index = 0; }
  });
  t0 = nowSeconds();
  bool can = false;
  for (int i = 0; i < 50; i++) can = canMove(s.world(), h, handlePos(s.world(), h) + Vec2(0.5, 0.5));
  double tCanMove = (nowSeconds() - t0) / 50;
  CHECK(can);
  // ручки под курсором: наведение по всей карте (самые частые вызовы в режиме правки)
  int hits = 0;
  t0 = nowSeconds();
  for (int i = 0; i < 50; i++) {
    Vec2 p{4800.0 + 32.0 * i, 1500.0 + 30.0 * i};
    hits += bool(hitHandle(s.world(), p, 6));
    hits += hitEdge(s.world(), p, 6).has_value();
  }
  double tHit = (nowSeconds() - t0) / 100;
  CHECK(hits > 0);
  t0 = nowSeconds();
  s.transact("move", [&](Tx& tx) { moveHandle(tx, h, handlePos(tx.w(), h) + Vec2(0.5, 0.5)); });
  double tMove = nowSeconds() - t0;
  // кеш граней: попадание и новая версия после правки ручки
  auto fsMoved = geo::faces(s.world());
  t0 = nowSeconds();
  for (int i = 0; i < 1000; i++) CHECK(geo::faces(s.world()) == fsMoved);
  double tCacheHit = (nowSeconds() - t0) / 1000;
  // торговый путь через всю карту
  std::vector<Vec2> route;
  for (int i = 0; i <= 40; i++) route.push_back({100.0 + 195.0 * i, 2250 + 1800 * std::sin(i * 0.4)});
  t0 = nowSeconds();
  auto on = fsMoved->provincesOnPolyline(route);
  double tRoute = nowSeconds() - t0;
  // лассо из 4000 вершин через изрезанный берег материка (суша прилипает к берегу)
  std::vector<Vec2> lasso;
  for (int i = 0; i < 4000; i++) {
    double t = 2 * kPi * i / 4000;
    lasso.push_back({2250 + 420 * std::cos(t) + 6 * std::sin(37 * t), 2250 + 380 * std::sin(t)});
  }
  t0 = nowSeconds();
  Id l = s.transact("lasso", [&](Tx& tx) { return createProvince(tx, lasso, Terrain::Land); });
  double tLasso = nowSeconds() - t0;
  CHECK(buildFaces(s.world())->shape(l) != nullptr);
  // отмена/повтор правки — переключение версии мира
  t0 = nowSeconds();
  CHECK(s.undo());
  CHECK(s.redo());
  double tUndo = (nowSeconds() - t0) / 2;
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK(buildFaces(s.world())->shape(b) != nullptr);
  std::printf("  big coast: %zu points, %zu arcs, %zu faces | init %.0f ms, buildFaces %.1f ms (labels in 1 thread %.1f ms), "
              "validate %.1f ms, locate %.2f us, create %.0f ms, split %.0f ms, fill %.0f ms, canMove %.2f ms\n"
              "             hitHandle+hitEdge %.2f ms, moveHandle %.1f ms, faces() hit %.2f us, route %zu provinces %.2f ms, "
              "lasso 4000 pts %.0f ms, undo %.3f ms\n",
              pts, arcs, faces, tInit * 1e3, tFaces * 1e3, tLabels * 1e3, tValidate * 1e3, tLocate * 1e6, tCreate * 1e3,
              tSplit * 1e3, tFill * 1e3, tCanMove * 1e3, tHit * 1e3, tMove * 1e3, tCacheHit * 1e6, on.size(), tRoute * 1e3,
              tLasso * 1e3, tUndo * 1e3);
#ifdef NDEBUG
  CHECK(tFaces < rg::test::perf(0.080));
  CHECK(tHit < rg::test::perf(0.020));
  CHECK(tMove < rg::test::perf(0.050));
#endif
  geotest::renderWorld(s.world(), "geo_perf_coast.png", 0.125, Box2(0, 0, W, H));
}

TEST(geo_perf_grid_partition) {
  const int N = 46;
  const double cw = W / N, ch = H / N;
  Store s;
  s.transact("grid", [&](Tx& tx) {
    for (int k = 0; k < N * N; k++) tx.add(Province{});
    std::vector<Id> node(size_t((N + 1) * (N + 1)));
    for (int j = 0; j <= N; j++)
      for (int i = 0; i <= N; i++) node[size_t(j * (N + 1) + i)] = tx.add(Node{0, {i * cw, j * ch}}).id;
    auto cell = [&](int i, int j) -> Id { return (i < 0 || j < 0 || i >= N || j >= N) ? Id(0) : Id(j * N + i + 1); };
    auto side = [&](int i, int j) { Id p = cell(i, j); return std::pair<Id, Terrain>{p, p ? Terrain::Land : Terrain::None}; };
    auto wig = [](double t, double amp) { double e = std::sin(kPi * t); return amp * e * e * std::sin(5 * kPi * t); };
    for (int j = 0; j <= N; j++)
      for (int i = 0; i < N; i++) {  // горизонтальная: слева (+y) — ячейка (i, j), справа — (i, j − 1)
        Edge e;
        e.a = node[size_t(j * (N + 1) + i)];
        e.b = node[size_t(j * (N + 1) + i + 1)];
        bool frame = j == 0 || j == N;
        e.kind = frame ? EdgeKind::Frame : EdgeKind::Border;
        if (!frame)
          for (int k = 1; k <= 10; k++) {
            double t = k / 11.0;
            e.pts.push_back({(i + t) * cw, j * ch + wig(t, 0.08 * ch)});
          }
        auto L = side(i, j), R = side(i, j - 1);
        e.pl = L.first; e.tl = L.second; e.pr = R.first; e.tr = R.second;
        tx.add(e);
      }
    for (int j = 0; j < N; j++)
      for (int i = 0; i <= N; i++) {  // вертикальная: слева (−x) — ячейка (i − 1, j), справа — (i, j)
        Edge e;
        e.a = node[size_t(j * (N + 1) + i)];
        e.b = node[size_t((j + 1) * (N + 1) + i)];
        bool frame = i == 0 || i == N;
        e.kind = frame ? EdgeKind::Frame : EdgeKind::Border;
        if (!frame)
          for (int k = 1; k <= 10; k++) {
            double t = k / 11.0;
            e.pts.push_back({i * cw + wig(t, 0.08 * cw), (j + t) * ch});
          }
        auto L = side(i - 1, j), R = side(i, j);
        e.pl = L.first; e.tl = L.second; e.pr = R.first; e.tr = R.second;
        tx.add(e);
      }
  });
  const World& w = s.world();
  size_t pts = 0;
  w.edges.each([&](const Edge& e) { pts += e.pts.size(); });
  std::shared_ptr<const FaceSet> fs;
  double tFaces = bestOf(5, [&] { fs = buildFaces(w); });
  double tLabels = bestOf(1, [&] {
    for (auto& f : fs->faces) polylabel(f.rings, clamp(std::min(f.box.w(), f.box.h()) / 50.0, 1e-3, 1.0));
  });
  std::vector<Issue> is;
  double tValidate = bestOf(3, [&] { is = validate(w); });
  CHECK_MSG(is.empty(), geotest::issuesText(is));
  CHECK_EQ(fs->faces.size(), size_t(N * N));
  CHECK_EQ(fs->provinces.size(), size_t(N * N));
  CHECK_NEAR(geotest::faceAreaSum(*fs), W * H, 1e-6 * W * H);
  CHECK_EQ(fs->neighbors().size(), size_t(2 * N * (N - 1)));
  for (auto& f : fs->faces) CHECK_EQ(fs->locate(f.label), int(&f - fs->faces.data()));
  // торговый путь по диагонали карты: провинции по порядку входа
  double tRoute = nowSeconds();
  auto route = fs->provincesOnPolyline({{10, 10}, {W - 10, H - 10}});
  tRoute = nowSeconds() - tRoute;
  CHECK(route.size() >= size_t(N));
  CHECK_EQ(route.front(), fs->provinceAt({10, 10}));
  CHECK_EQ(route.back(), fs->provinceAt({W - 10, H - 10}));
  // операция на плотном разбиении: новая провинция поверх 9 ячеек
  double t0 = nowSeconds();
  s.transact("p", [&](Tx& tx) { createProvince(tx, {{1000, 1000}, {1500, 1050}, {1450, 1500}, {980, 1480}}, Terrain::Land); });
  double tCreate = nowSeconds() - t0;
  GEO_CHECK_WORLD(s.world(), W, H);
  std::printf("  grid: %zu points, %u arcs, %zu faces | buildFaces %.1f ms (labels in 1 thread %.1f ms), validate %.1f ms, "
              "create %.0f ms, route %zu provinces %.2f ms\n",
              pts, w.edges.size(), fs->faces.size(), tFaces * 1e3, tLabels * 1e3, tValidate * 1e3, tCreate * 1e3, route.size(),
              tRoute * 1e3);
#ifdef NDEBUG
  CHECK(tFaces < rg::test::perf(0.080));
#endif
  geotest::renderWorld(s.world(), "geo_perf_grid.png", 0.125, Box2(0, 0, W, H));
}
