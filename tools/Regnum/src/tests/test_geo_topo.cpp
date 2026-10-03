// Тесты geo: построение граней (полурёбра, дыры, параллельные дуги, петли), поиск, проверка целостности, кеш.
#include <atomic>
#include <thread>

#include "tests/test_geo_util.h"

using namespace rg;
using namespace rg::geo;

namespace {

struct Lbl { Id p = 0; Terrain t = Terrain::Land; };

World build(const std::function<void(Tx&)>& fn) {
  Store s;
  s.transact("build", [&](Tx& tx) { fn(tx); });
  return s.world();
}

Id addNode(Tx& tx, Vec2 p) { return tx.add(Node{0, p}).id; }

Id addEdge(Tx& tx, Id a, Id b, std::vector<Vec2> pts, EdgeKind k, Lbl l, Lbl r) {
  Edge e;
  e.a = a;
  e.b = b;
  e.pts = std::move(pts);
  e.kind = k;
  e.pl = l.p;
  e.tl = l.t;
  e.pr = r.p;
  e.tr = r.t;
  return tx.add(e).id;
}

// Рамка против часовой (математически): внутренность слева.
void addFrame(Tx& tx, double W, double H, Lbl inner) {
  Id a = addNode(tx, {0, 0}), b = addNode(tx, {W, 0}), c = addNode(tx, {W, H}), d = addNode(tx, {0, H});
  Lbl out{0, Terrain::None};
  addEdge(tx, a, b, {}, EdgeKind::Frame, inner, out);
  addEdge(tx, b, c, {}, EdgeKind::Frame, inner, out);
  addEdge(tx, c, d, {}, EdgeKind::Frame, inner, out);
  addEdge(tx, d, a, {}, EdgeKind::Frame, inner, out);
}

// Кольцо (против часовой) из двух дуг: слева — inside, справа — outside.
void addRing(Tx& tx, const std::vector<Vec2>& ring, EdgeKind k, Lbl inside, Lbl outside) {
  size_t h = ring.size() / 2;
  Id n0 = addNode(tx, ring[0]), n1 = addNode(tx, ring[h]);
  std::vector<Vec2> p1(ring.begin() + 1, ring.begin() + long(h)), p2(ring.begin() + long(h) + 1, ring.end());
  addEdge(tx, n0, n1, p1, k, inside, outside);
  addEdge(tx, n1, n0, p2, k, inside, outside);
}

std::vector<Vec2> square(Vec2 c, double r) { return {{c.x - r, c.y - r}, {c.x + r, c.y - r}, {c.x + r, c.y + r}, {c.x - r, c.y + r}}; }
std::vector<Vec2> circle(Vec2 c, double r, int n) {
  std::vector<Vec2> v;
  for (int i = 0; i < n; i++) v.push_back({c.x + r * std::cos(2 * kPi * i / n), c.y + r * std::sin(2 * kPi * i / n)});
  return v;
}

bool hasCode(const std::vector<Issue>& is, const std::string& code) {
  for (auto& i : is)
    if (i.code == code) return true;
  return false;
}

}  // namespace

TEST(geo_faces_frame_only) {
  World w = build([](Tx& tx) { addFrame(tx, 100, 50, {0, Terrain::Sea}); });
  auto fs = buildFaces(w);
  CHECK_EQ(fs->faces.size(), size_t(1));
  CHECK_NEAR(fs->faces[0].area, 5000, 1e-9);
  CHECK(fs->faces[0].terrain == Terrain::Sea);
  CHECK_EQ(fs->locate({10, 10}), 0);
  CHECK_EQ(fs->locate({-1, 10}), -1);
  CHECK_EQ(fs->locate({50, 51}), -1);
  CHECK(fs->terrainAt({200, 10}) == Terrain::None);
  CHECK_NEAR(fs->faces[0].label.x, 50, 1);
  CHECK_NEAR(fs->faces[0].label.y, 25, 1);
  CHECK(validate(w).empty());
}

// Вложенность: суша → «лагуна» (море внутри суши) → островок. Дыры — в наименьшей содержащей грани.
TEST(geo_faces_nested_holes) {
  World w = build([](Tx& tx) {
    addFrame(tx, 1000, 1000, {0, Terrain::Sea});
    addRing(tx, circle({500, 500}, 400, 64), EdgeKind::Coast, {0, Terrain::Land}, {0, Terrain::Sea});
    addRing(tx, circle({500, 500}, 200, 48), EdgeKind::Coast, {0, Terrain::Sea}, {0, Terrain::Land});
    addRing(tx, square({500, 500}, 50), EdgeKind::Coast, {0, Terrain::Land}, {0, Terrain::Sea});
    addRing(tx, square({100, 100}, 30), EdgeKind::Coast, {0, Terrain::Land}, {0, Terrain::Sea});
  });
  auto is = validate(w);
  CHECK_MSG(is.empty(), geotest::issuesText(is));
  auto fs = buildFaces(w);
  CHECK_EQ(fs->faces.size(), size_t(5));
  CHECK_NEAR(geotest::faceAreaSum(*fs), 1e6, 1e-6);
  int sea = fs->locate({900, 900}), ring = fs->locate({500, 150}), lagoon = fs->locate({500, 380}), islet = fs->locate({500, 500}),
      small = fs->locate({100, 100});
  CHECK(sea >= 0 && ring >= 0 && lagoon >= 0 && islet >= 0 && small >= 0);
  CHECK(fs->faces[size_t(sea)].terrain == Terrain::Sea);
  CHECK(fs->faces[size_t(ring)].terrain == Terrain::Land);
  CHECK(fs->faces[size_t(lagoon)].terrain == Terrain::Sea);
  CHECK(fs->faces[size_t(islet)].terrain == Terrain::Land);
  CHECK_EQ(fs->faces[size_t(sea)].rings.size(), size_t(3));     // рамка + большое кольцо + малый остров
  CHECK_EQ(fs->faces[size_t(ring)].rings.size(), size_t(2));
  CHECK_EQ(fs->faces[size_t(lagoon)].rings.size(), size_t(2));
  CHECK_EQ(fs->faces[size_t(islet)].rings.size(), size_t(1));
  CHECK_NEAR(fs->faces[size_t(islet)].area, 10000, 1e-6);
  // метки внутри своих граней
  for (size_t f = 0; f < fs->faces.size(); f++) CHECK_EQ(fs->locate(fs->faces[f].label), int(f));
  // ringEdges воспроизводят координаты колец
  for (auto& f : fs->faces)
    for (size_t r = 0; r < f.rings.size(); r++) {
      std::vector<Vec2> pts;
      for (auto& he : f.ringEdges[r]) {
        auto c = edgeCoords(w, *w.edges.get(he.edge));
        if (!he.forward) std::reverse(c.begin(), c.end());
        pts.insert(pts.end(), c.begin(), c.end() - 1);
      }
      CHECK(pts == f.rings[r]);
    }
}

// Две дуги между одними узлами (линза), петля, висящая дуга внутри грани.
TEST(geo_faces_parallel_arcs_and_loops) {
  World w = build([](Tx& tx) {
    addFrame(tx, 400, 300, {0, Terrain::Land});
    Id a = addNode(tx, {100, 150}), b = addNode(tx, {200, 150});
    // «слева» — математически: cross(b − a, p − a) > 0 (на экране с осью Y вниз это правая сторона)
    addEdge(tx, a, b, {{150, 100}}, EdgeKind::Border, {1, Terrain::Land}, {0, Terrain::Land});   // дуга через y = 100
    addEdge(tx, a, b, {{150, 200}}, EdgeKind::Border, {0, Terrain::Land}, {1, Terrain::Land});   // дуга через y = 200
    Id c = addNode(tx, {300, 100});
    addEdge(tx, c, c, {{340, 100}, {340, 140}, {300, 140}}, EdgeKind::Border, {2, Terrain::Land}, {0, Terrain::Land});
    tx.add(Province{});
    tx.add(Province{});
  });
  auto is = validate(w);
  CHECK_MSG(is.empty(), geotest::issuesText(is));
  auto fs = buildFaces(w);
  CHECK_EQ(fs->faces.size(), size_t(3));
  CHECK_EQ(fs->provinceAt({150, 150}), Id(1));
  CHECK_EQ(fs->provinceAt({320, 120}), Id(2));
  CHECK_EQ(fs->provinceAt({50, 50}), Id(0));
  CHECK_NEAR(fs->shape(1)->area, 5000, 1e-9);
  CHECK_NEAR(fs->shape(2)->area, 1600, 1e-9);
  CHECK_NEAR(geotest::faceAreaSum(*fs), 120000, 1e-6);
  auto nb = fs->neighbors();
  CHECK(nb.empty());  // 1 и 2 граничат только с «не назначено»
  auto on = fs->provincesOnPolyline({{50, 150}, {350, 150}, {320, 120}});
  CHECK_EQ(on.size(), size_t(2));
  CHECK_EQ(on[0], Id(1));
  CHECK_EQ(on[1], Id(2));
}

// Луч поиска проходит точно через вершины: результат согласован с прямой проверкой «точка в грани».
TEST(geo_locate_vertices_on_ray) {
  World w = build([](Tx& tx) {
    addFrame(tx, 100, 100, {0, Terrain::Sea});
    // ромб и треугольник с вершинами на y = 50
    addRing(tx, {{60, 50}, {70, 40}, {80, 50}, {70, 60}}, EdgeKind::Coast, {0, Terrain::Land}, {0, Terrain::Sea});
    addRing(tx, {{20, 50}, {30, 30}, {40, 50}}, EdgeKind::Coast, {0, Terrain::Land}, {0, Terrain::Sea});
  });
  CHECK(validate(w).empty());
  auto fs = buildFaces(w);
  for (double x : {5.0, 10.0, 45.0, 50.0, 55.0, 85.0, 90.0}) CHECK(fs->terrainAt({x, 50}) == Terrain::Sea);
  for (double x : {65.0, 70.0, 75.0}) CHECK(fs->terrainAt({x, 50}) == Terrain::Land);
  for (double x : {25.0, 30.0, 35.0}) CHECK(fs->terrainAt({x, 49.9}) == Terrain::Land);
  CHECK(fs->terrainAt({30, 50.1}) == Terrain::Sea);
}

TEST(geo_locate_matches_point_in_polygon) {
  Store s;
  s.transact("init", [](Tx& tx) { initFromCoast(tx, geotest::sampleCoast()); });
  auto fs = buildFaces(s.world());
  Rng rng(99);
  int checked = 0;
  for (int i = 0; i < 20000; i++) {
    Vec2 p{rng.uniform() * 1000, rng.uniform() * 600};
    int f = fs->locate(p);
    CHECK(f >= 0);
    // пропустить точки у самой границы
    if (distToRings(p, fs->faces[size_t(f)].rings) < 1e-6) continue;
    CHECK(pointInPolygon(p, fs->faces[size_t(f)].rings));
    int cnt = 0;
    for (auto& F : fs->faces)
      if (F.box.contains(p) && pointInPolygon(p, F.rings)) cnt++;
    CHECK_EQ(cnt, 1);
    checked++;
  }
  CHECK(checked > 19000);
}

TEST(geo_validate_detects_problems) {
  auto base = [](Tx& tx) { addFrame(tx, 100, 100, {0, Terrain::Land}); };
  // корректный
  CHECK(validate(build(base)).empty());
  // пересечение
  auto w1 = build([&](Tx& tx) {
    base(tx);
    Id a = addNode(tx, {10, 10}), b = addNode(tx, {90, 90}), c = addNode(tx, {10, 90}), d = addNode(tx, {90, 10});
    addEdge(tx, a, b, {}, EdgeKind::Border, {}, {});
    addEdge(tx, b, a, {{80, 20}}, EdgeKind::Border, {}, {});
    addEdge(tx, c, d, {}, EdgeKind::Border, {}, {});
    addEdge(tx, d, c, {{20, 15}}, EdgeKind::Border, {}, {});
  });
  CHECK(hasCode(validate(w1), "crossing"));
  // висячий узел и вырожденная дуга
  auto w2 = build([&](Tx& tx) {
    base(tx);
    Id a = addNode(tx, {10, 10}), b = addNode(tx, {20, 20});
    addEdge(tx, a, b, {}, EdgeKind::Border, {}, {});
    Id c = addNode(tx, {50, 50});
    addEdge(tx, c, c, {}, EdgeKind::Border, {}, {});
  });
  auto is2 = validate(w2);
  CHECK(hasCode(is2, "dangling"));
  CHECK(hasCode(is2, "degenerate-edge"));
  // нулевой отрезок, дубль узла
  auto w3 = build([&](Tx& tx) {
    base(tx);
    Id a = addNode(tx, {10, 10}), b = addNode(tx, {30, 10});
    addEdge(tx, a, b, {{20, 10}, {20, 10}, {20, 30}}, EdgeKind::Border, {}, {});
    addEdge(tx, b, a, {{20, 0.5}}, EdgeKind::Border, {}, {});
    addNode(tx, {30, 10});
  });
  auto is3 = validate(w3);
  CHECK(hasCode(is3, "zero-length"));
  CHECK(hasCode(is3, "duplicate-node"));
  // наложение: две прямые дуги между одними узлами
  auto w4 = build([&](Tx& tx) {
    base(tx);
    Id a = addNode(tx, {10, 10}), b = addNode(tx, {30, 10});
    addEdge(tx, a, b, {}, EdgeKind::Border, {}, {});
    addEdge(tx, b, a, {}, EdgeKind::Border, {}, {});
  });
  CHECK(hasCode(validate(w4), "overlap"));
  // касание без узла (Т-образное): узел петли лежит на отрезке треугольника
  auto w5 = build([&](Tx& tx) {
    base(tx);
    Id c = addNode(tx, {40, 20});
    addEdge(tx, c, c, {{20, 20}, {20, 15}}, EdgeKind::Border, {}, {});
    Id d = addNode(tx, {30, 20});
    addEdge(tx, d, d, {{30, 25}, {35, 25}}, EdgeKind::Border, {}, {});
  });
  auto is5 = validate(w5);
  CHECK(hasCode(is5, "touch"));
  CHECK(!hasCode(is5, "crossing"));
  // несогласованные метки граней, отсутствующая провинция, разный рельеф по сторонам границы
  auto w6 = build([&](Tx& tx) {
    base(tx);
    addRing(tx, square({50, 50}, 10), EdgeKind::Border, {7, Terrain::Land}, {0, Terrain::Land});
    // вторая дуга кольца с другой меткой внутри
    tx.edge(tx.w().edges.ids().back()).pl = 0;
    Id a = addNode(tx, {10, 80}), b = addNode(tx, {20, 80});
    addEdge(tx, a, b, {{15, 85}}, EdgeKind::Border, {0, Terrain::Sea}, {0, Terrain::Land});
    addEdge(tx, b, a, {{15, 75}}, EdgeKind::Border, {0, Terrain::Sea}, {0, Terrain::Land});
  });
  auto is6 = validate(w6);
  CHECK(hasCode(is6, "label-mismatch"));
  CHECK(hasCode(is6, "province-missing"));
  CHECK(hasCode(is6, "terrain"));
  // разорванная рамка и дуга вне карты
  auto w7 = build([&](Tx& tx) {
    base(tx);
    tx.eraseEdge(tx.w().edges.ids().front());
  });
  auto is7 = validate(w7);
  CHECK(hasCode(is7, "dangling") || hasCode(is7, "frame"));
  auto w8 = build([&](Tx& tx) {
    base(tx);
    addRing(tx, square({200, 50}, 10), EdgeKind::Border, {0, Terrain::Land}, {0, Terrain::Land});
  });
  CHECK(hasCode(validate(w8), "outside"));
  // ссылка на несуществующий узел
  auto w9 = build([&](Tx& tx) {
    base(tx);
    Id a = addNode(tx, {10, 10});
    addEdge(tx, a, 999, {}, EdgeKind::Border, {}, {});
  });
  CHECK(hasCode(validate(w9), "node-missing"));
  // почти-касание: вершина в 1e-6 от чужого отрезка (без пересечения)
  auto w11 = build([&](Tx& tx) {
    base(tx);
    addRing(tx, square({50, 50}, 10), EdgeKind::Border, {0, Terrain::Land}, {0, Terrain::Land});
    addRing(tx, {{60 + 1e-6, 50}, {70, 45}, {70, 55}}, EdgeKind::Border, {0, Terrain::Land}, {0, Terrain::Land});
  });
  auto is11 = validate(w11);
  CHECK(hasCode(is11, "near"));
  CHECK(!hasCode(is11, "crossing") && !hasCode(is11, "touch"));
  // нечисловые координаты
  auto w10 = build([&](Tx& tx) {
    base(tx);
    addNode(tx, {std::nan(""), 5});
  });
  CHECK(hasCode(validate(w10), "nan"));
}

TEST(geo_faces_cache_identity_and_threads) {
  Store s;
  s.transact("init", [](Tx& tx) { initFromCoast(tx, geotest::sampleCoast()); });
  World w1 = s.world();
  auto a = faces(w1), b = faces(w1);
  CHECK(a == b);
  Id p = s.transact("p", [](Tx& tx) { return createProvince(tx, {{400, 200}, {560, 200}, {560, 380}, {400, 380}}, Terrain::Land); });
  World w2 = s.world();
  auto c = faces(w2);
  CHECK(c != a);
  CHECK(c->shape(p) != nullptr);
  CHECK(faces(w1) == a);  // старая версия ещё в кеше
  // параллельные обращения к разным версиям дают согласованные результаты
  std::vector<std::thread> th;
  std::atomic<int> bad{0};
  for (int t = 0; t < 8; t++)
    th.emplace_back([&, t] {
      for (int i = 0; i < 20; i++) {
        const World& w = ((t + i) % 2) ? w1 : w2;
        auto fs = faces(w);
        bool hasP = fs->shape(p) != nullptr;
        if (hasP != (&w == &w2)) bad++;
      }
    });
  for (auto& t : th) t.join();
  CHECK_EQ(bad.load(), 0);
}

// faces(tx.w()) посреди транзакции не подменяет грани зафиксированного мира, если граф затем ещё менялся.
TEST(geo_faces_cache_mid_transaction) {
  Store s;
  s.transact("init", [](Tx& tx) { initFromCoast(tx, geotest::sampleCoast()); });
  Id a = 0, b = 0;
  s.transact("two", [&](Tx& tx) {
    a = createProvince(tx, {{400, 200}, {500, 200}, {500, 300}, {400, 300}}, Terrain::Land);
    auto mid = faces(tx.w());
    CHECK(mid->shape(a) != nullptr);
    b = createProvince(tx, {{500, 200}, {560, 200}, {560, 300}, {500, 300}}, Terrain::Land);
  });
  auto fs = faces(s.world());
  auto ref = buildFaces(s.world());
  CHECK(fs->shape(b) != nullptr);
  CHECK_EQ(fs->faces.size(), ref->faces.size());
  CHECK_NEAR(fs->shape(a)->area, ref->shape(a)->area, 1e-9);
  CHECK(faces(s.world()) == fs);
#ifdef RG_DEBUG
  // отладочная сборка сверяет и отпечаток: перемещение узла на месте тоже замечается
  s.transact("move", [&](Tx& tx) {
    Id c = createProvince(tx, {{420, 320}, {470, 320}, {470, 370}, {420, 370}}, Terrain::Land);
    (void)faces(tx.w());
    Handle h = hitHandle(tx.w(), {470, 320}, 0.5, c);
    CHECK(h && !handleLocked(tx.w(), h));
    CHECK(canMove(tx.w(), h, {475, 315}));
    moveHandle(tx, h, {475, 315});
  });
  auto ref2 = buildFaces(s.world());
  auto fs2 = faces(s.world());
  CHECK_EQ(fs2->faces.size(), ref2->faces.size());
  for (auto& [id, sh] : ref2->provinces) CHECK_NEAR(fs2->shape(id)->area, sh.area, 1e-9);
#endif
}
