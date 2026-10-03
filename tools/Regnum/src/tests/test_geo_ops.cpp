// Тесты geo: операции над провинциями на синтетическом берегу (большой остров, малые острова, подкова,
// материк за краем карты): создание с прилипанием к берегу, расширение, вырезание, заливка, нож, слияние,
// снятие назначения, ручки, скольжение по берегу, откат при ошибке, отмена.
#include "tests/test_geo_util.h"

using namespace rg;
using namespace rg::geo;
using geotest::sampleCoast;

namespace {

constexpr double W = 1000, H = 600;

Store initStore() {
  Store s;
  s.transact("init", [](Tx& tx) { initFromCoast(tx, sampleCoast(W, H)); });
  return s;
}

std::vector<Vec2> rect(double x0, double y0, double x1, double y1) { return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}; }

double provArea(const World& w, Id p) {
  auto fs = buildFaces(w);
  auto* s = fs->shape(p);
  return s ? s->area : 0;
}

double terrainArea(const World& w, Terrain t, Id prov = Id(-1)) {
  auto fs = buildFaces(w);
  double a = 0;
  for (auto& f : fs->faces)
    if (f.terrain == t && (prov == Id(-1) || f.province == prov)) a += f.area;
  return a;
}

int countKind(const World& w, EdgeKind k) {
  int n = 0;
  w.edges.each([&](const Edge& e) { n += e.kind == k; });
  return n;
}

size_t coordCount(const World& w, EdgeKind k) {
  size_t n = 0;
  w.edges.each([&](const Edge& e) { if (e.kind == k) n += e.pts.size() + 1; });
  return n;
}

// Все грани провинции имеют заданный рельеф.
bool allTerrain(const World& w, Id p, Terrain t) {
  auto fs = buildFaces(w);
  auto* s = fs->shape(p);
  if (!s) return false;
  for (int f : s->faces)
    if (fs->faces[size_t(f)].terrain != t) return false;
  return true;
}

// Площадь острова, содержащего точку (грань суши).
double faceAreaAt(const World& w, Vec2 p) {
  auto fs = buildFaces(w);
  int f = fs->locate(p);
  return f < 0 ? 0 : fs->faces[size_t(f)].area;
}

}  // namespace

TEST(geo_init_from_coast) {
  Store s = initStore();
  const World& w = s.world();
  GEO_CHECK_WORLD(w, W, H);
  auto fs = buildFaces(w);
  CHECK_EQ(fs->faces.size(), size_t(7));  // море + 5 островов + материк
  CHECK(fs->terrainAt({480, 300}) == Terrain::Land);
  CHECK(fs->terrainAt({820, 140}) == Terrain::Land);
  CHECK(fs->terrainAt({820, 300}) == Terrain::Sea);   // залив подковы
  CHECK(fs->terrainAt({760, 300}) == Terrain::Land);  // западная часть подковы
  CHECK(fs->terrainAt({20, 300}) == Terrain::Land);   // материк у левого края
  CHECK(fs->terrainAt({950, 50}) == Terrain::Sea);
  for (auto& f : fs->faces) CHECK_EQ(f.province, Id(0));
  CHECK(countKind(w, EdgeKind::Border) == 0);
  CHECK(countKind(w, EdgeKind::Coast) >= 10);
  // повторная инициализация запрещена; самопересекающийся берег отклоняется
  CHECK_THROWS(s.transact("again", [](Tx& tx) { initFromCoast(tx, sampleCoast(W, H)); }));
  Store bad;
  Coast c;
  c.width = 100;
  c.height = 100;
  c.landRings.push_back({{10, 10}, {50, 50}, {50, 10}, {10, 40}});
  CHECK_THROWS(bad.transact("bad", [&](Tx& tx) { initFromCoast(tx, c); }));
  CHECK(bad.world().edges.empty());
  geotest::renderWorld(w, "geo_init.png", 1.0, Box2(0, 0, W, H));
}

// Контур, захватывающий малый остров с запасом: провинция = остров целиком, границы только береговые.
TEST(geo_create_province_sticks_to_coast) {
  Store s = initStore();
  double island = faceAreaAt(s.world(), {820, 140});
  Id p = s.transact("create", [](Tx& tx) { return createProvince(tx, rect(750, 70, 890, 210), Terrain::Land); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), p), island, 1e-6);
  CHECK_EQ(countKind(s.world(), EdgeKind::Border), 0);
  CHECK(!s.world().province(p)->sea);
  // морская провинция тем же контуром: квадрат минус остров
  Id q = s.transact("sea", [](Tx& tx) { return createProvince(tx, rect(750, 70, 890, 210), Terrain::Sea); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK(s.world().province(q)->sea);
  CHECK_NEAR(provArea(s.world(), q), 140.0 * 140.0 - island, 1e-6);
  CHECK(allTerrain(s.world(), q, Terrain::Sea));
  CHECK_NEAR(provArea(s.world(), p), island, 1e-6);  // суша не задета
  auto nb = buildFaces(s.world())->neighbors();
  CHECK_EQ(nb.size(), size_t(1));
  CHECK(nb[0] == std::make_pair(std::min(p, q), std::max(p, q)));
  // terrain = None: по первой вершине (в море)
  Id r = s.transact("auto", [](Tx& tx) { return createProvince(tx, rect(900, 20, 980, 100)); });
  CHECK(s.world().province(r)->sea);
  GEO_CHECK_WORLD(s.world(), W, H);
  geotest::renderWorld(s.world(), "geo_sticks.png", 1.0, Box2(0, 0, W, H));
}

// Частичный охват большого острова: граница идёт по берегу и по суше, в море дуг не остаётся.
TEST(geo_create_partial_and_overlap) {
  Store s = initStore();
  double land0 = terrainArea(s.world(), Terrain::Land);
  Id a = s.transact("a", [](Tx& tx) { return createProvince(tx, rect(550, 150, 720, 450), Terrain::Land); });
  GEO_CHECK_WORLD(s.world(), W, H);
  double aa = provArea(s.world(), a);
  CHECK(aa > 1000 && aa < 170.0 * 300.0);
  CHECK(allTerrain(s.world(), a, Terrain::Land));
  s.world().edges.each([&](const Edge& e) {
    if (e.kind == EdgeKind::Border) CHECK(e.tl == Terrain::Land && e.tr == Terrain::Land);
  });
  // вторая провинция частично поверх первой: первая уменьшается, суша сохраняется
  Id b = s.transact("b", [](Tx& tx) {
    return createProvince(tx, {{430, 120}, {600, 140}, {640, 300}, {560, 470}, {420, 480}}, Terrain::Land);
  });
  GEO_CHECK_WORLD(s.world(), W, H);
  double aa2 = provArea(s.world(), a), bb = provArea(s.world(), b);
  CHECK(aa2 < aa);
  CHECK(bb > 0);
  CHECK_NEAR(terrainArea(s.world(), Terrain::Land), land0, 1e-6 * land0);
  CHECK_NEAR(terrainArea(s.world(), Terrain::Land, 0) + aa2 + bb, land0, 1e-6 * land0);
  auto nb = buildFaces(s.world())->neighbors();
  CHECK(std::find(nb.begin(), nb.end(), std::make_pair(std::min(a, b), std::max(a, b))) != nb.end());
  // провинции по линии
  auto on = buildFaces(s.world())->provincesOnPolyline({{300, 300}, {700, 300}});
  CHECK_EQ(on.size(), size_t(2));
  CHECK_EQ(on[0], b);
  CHECK_EQ(on[1], a);
  geotest::renderWorld(s.world(), "geo_partial.png", 1.0, Box2(0, 0, W, H));
}

TEST(geo_add_remove_area) {
  Store s = initStore();
  Id a = s.transact("a", [](Tx& tx) { return createProvince(tx, rect(400, 200, 520, 320), Terrain::Land); });
  CHECK_NEAR(provArea(s.world(), a), 120.0 * 120.0, 1e-6);  // целиком внутри острова
  Id b = s.transact("b", [](Tx& tx) { return createProvince(tx, rect(520, 200, 600, 320), Terrain::Land); });
  CHECK_NEAR(provArea(s.world(), b), 80.0 * 120.0, 1e-6);
  GEO_CHECK_WORLD(s.world(), W, H);
  // расширение a за счёт b (общая граница переносится)
  s.transact("add", [&](Tx& tx) { addArea(tx, a, rect(510, 220, 560, 300)); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), a), 120.0 * 120.0 + 40.0 * 80.0, 1e-6);
  CHECK_NEAR(provArea(s.world(), b), 80.0 * 120.0 - 40.0 * 80.0, 1e-6);
  // вырезание дыры внутри a
  s.transact("remove", [&](Tx& tx) { removeArea(tx, a, rect(420, 230, 460, 270)); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), a), 120.0 * 120.0 + 40.0 * 80.0 - 1600, 1e-6);
  // вырезание, не задевающее провинцию, и расширение без новой территории — ошибки, мир не меняется
  World before = s.world();
  CHECK_THROWS(s.transact("x", [&](Tx& tx) { removeArea(tx, a, rect(10, 10, 30, 30)); }));
  CHECK_THROWS(s.transact("y", [&](Tx& tx) { addArea(tx, a, rect(430, 210, 450, 225)); }));
  CHECK_EQ(World::diff(before, s.world()), 0u);
  // расширение морем не захватывает сушу (прилипание к берегу)
  double landA = terrainArea(s.world(), Terrain::Land, a);
  Id sea = s.transact("sea", [](Tx& tx) { return createProvince(tx, rect(905, 300, 990, 400), Terrain::Sea); });
  s.transact("seaAdd", [&](Tx& tx) { addArea(tx, sea, rect(600, 5, 990, 60)); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK(allTerrain(s.world(), sea, Terrain::Sea));
  CHECK_NEAR(terrainArea(s.world(), Terrain::Land, a), landA, 1e-6);
  geotest::renderWorld(s.world(), "geo_add_remove.png", 1.0, Box2(0, 0, W, H));
}

TEST(geo_fill_split_merge_unassign) {
  Store s = initStore();
  size_t coast0 = coordCount(s.world(), EdgeKind::Coast);
  int nodes0 = int(s.world().nodes.size());
  // заливка острова одним щелчком
  double isl = faceAreaAt(s.world(), {860, 470});
  Id f = s.transact("fill", [](Tx& tx) { return fillAt(tx, {860, 470}, 0); });
  CHECK_NEAR(provArea(s.world(), f), isl, 1e-6);
  CHECK(!s.world().province(f)->sea);
  CHECK_THROWS(s.transact("fillSea", [&](Tx& tx) { fillAt(tx, {950, 580}, f); }));  // суша → море нельзя
  CHECK_THROWS(s.transact("fillOut", [&](Tx& tx) { fillAt(tx, {-5, 5}, 0); }));
  Id g = s.transact("fill2", [](Tx& tx) { return fillAt(tx, {720, 520}, 0); });
  GEO_CHECK_WORLD(s.world(), W, H);
  // провинция из двух островов (fillAt существующей)
  s.transact("fill3", [&](Tx& tx) { fillAt(tx, {720, 520}, f); });
  CHECK_NEAR(provArea(s.world(), f), isl + faceAreaAt(s.world(), {720, 520}), 1e-6);
  CHECK(provArea(s.world(), g) == 0);
  // нож через большой остров
  Id big = s.transact("big", [](Tx& tx) { return fillAt(tx, {480, 300}, 0); });
  double bigA = provArea(s.world(), big);
  Id cut = s.transact("split", [&](Tx& tx) { return split(tx, big, {{250, 380}, {500, 330}, {720, 260}}); });
  GEO_CHECK_WORLD(s.world(), W, H);
  double a1 = provArea(s.world(), big), a2 = provArea(s.world(), cut);
  CHECK(a2 > 0 && a2 <= a1);
  CHECK_NEAR(a1 + a2, bigA, 1e-6 * bigA);
  // нож, не пересекающий провинцию насквозь, — ошибка
  World before = s.world();
  CHECK_THROWS(s.transact("bad", [&](Tx& tx) { split(tx, big, {{480, 200}, {490, 250}}); }));
  CHECK_THROWS(s.transact("bad2", [&](Tx& tx) { split(tx, big, {{100, 50}, {200, 60}}); }));
  CHECK_EQ(World::diff(before, s.world()), 0u);
  // зигзаг: три части, меньшая сторона — новая провинция
  Id z = s.transact("zig", [&](Tx& tx) {
    return split(tx, big, {{300, 100}, {380, 500}, {420, 500}, {460, 100}}, [](Tx& t, Id from) {
      Province p = *t.w().province(from);
      p.id = 0;
      p.name = "Отрезанная";
      return t.add(p).id;
    });
  });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_EQ(s.world().province(z)->name, std::string("Отрезанная"));
  CHECK_NEAR(provArea(s.world(), big) + provArea(s.world(), z) + provArea(s.world(), cut), bigA, 1e-6 * bigA);
  geotest::renderWorld(s.world(), "geo_split.png", 1.0, Box2(0, 0, W, H));
  // слияние: общая граница исчезает
  int borders = countKind(s.world(), EdgeKind::Border);
  s.transact("merge", [&](Tx& tx) { merge(tx, big, cut); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK(countKind(s.world(), EdgeKind::Border) < borders);
  CHECK_EQ(provArea(s.world(), cut), 0.0);
  CHECK(s.world().province(cut) != nullptr);  // запись не удаляется
  CHECK_THROWS(s.transact("self", [&](Tx& tx) { merge(tx, big, big); }));
  // снятие назначения: граф возвращается к исходному берегу (без лишних точек и узлов)
  for (Id p : {big, z, f}) s.transact("un", [&](Tx& tx) { unassign(tx, p); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_EQ(countKind(s.world(), EdgeKind::Border), 0);
  CHECK_EQ(coordCount(s.world(), EdgeKind::Coast), coast0);
  CHECK_EQ(int(s.world().nodes.size()), nodes0);
}

TEST(geo_undo_restores_exactly) {
  Store s = initStore();
  std::vector<World> snaps{s.world()};
  Id a = s.transact("a", [](Tx& tx) { return createProvince(tx, rect(550, 150, 720, 450), Terrain::Land); });
  snaps.push_back(s.world());
  s.transact("b", [&](Tx& tx) { addArea(tx, a, rect(450, 200, 600, 260)); });
  snaps.push_back(s.world());
  Id c = s.transact("c", [&](Tx& tx) { return split(tx, a, {{600, 100}, {640, 500}}); });
  snaps.push_back(s.world());
  s.transact("d", [&](Tx& tx) { merge(tx, a, c); });
  snaps.push_back(s.world());
  for (size_t i = snaps.size() - 1; i > 0; i--) {
    CHECK(s.undo());
    CHECK_EQ(World::diff(s.world(), snaps[i - 1]), 0u);
    GEO_CHECK_WORLD(s.world(), W, H);
  }
  for (size_t i = 1; i < snaps.size(); i++) {
    CHECK(s.redo());
    CHECK_EQ(World::diff(s.world(), snaps[i]), 0u);
  }
}

TEST(geo_invalid_input_rolls_back) {
  Store s = initStore();
  World before = s.world();
  CHECK_THROWS(s.transact("bow", [](Tx& tx) { createProvince(tx, {{400, 200}, {500, 300}, {500, 200}, {400, 300}}, Terrain::Land); }));
  CHECK_THROWS(s.transact("two", [](Tx& tx) { createProvince(tx, {{400, 200}, {500, 300}}, Terrain::Land); }));
  CHECK_THROWS(s.transact("nan", [](Tx& tx) { createProvince(tx, {{400, 200}, {std::nan(""), 300}, {500, 200}}, Terrain::Land); }));
  CHECK_THROWS(s.transact("sea-none", [](Tx& tx) { createProvince(tx, rect(905, 10, 990, 60), Terrain::Land); }));  // в море нет суши
  CHECK_THROWS(s.transact("nop", [](Tx& tx) { addArea(tx, 12345, rect(400, 200, 500, 300)); }));
  CHECK_EQ(World::diff(before, s.world()), 0u);
  // контур целиком за пределами карты не захватывает ничего
  CHECK_THROWS(s.transact("out", [](Tx& tx) { createProvince(tx, rect(1100, 100, 1200, 200), Terrain::Land); }));
  // контур, накрывающий всю карту: вся суша — одна провинция
  Id all = s.transact("all", [](Tx& tx) { return createProvince(tx, rect(-100, -100, 1100, 700), Terrain::Land); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), all), terrainArea(before, Terrain::Land), 1e-6 * W * H);
}

// Прилипание: вершины ввода у существующих узлов не создают щелевых граней.
TEST(geo_snap_no_slivers) {
  Store s = initStore();
  Id a = s.transact("a", [](Tx& tx) { return createProvince(tx, rect(400, 200, 500, 300), Terrain::Land); });
  size_t faces1 = buildFaces(s.world())->faces.size();
  // вторая провинция примыкает к первой с ошибкой ввода 0,3 (меньше допуска 1)
  Id b = s.transact("b", [](Tx& tx) { return createProvince(tx, {{500.3, 200.2}, {560, 200}, {560, 300}, {499.8, 299.7}}, Terrain::Land); });
  GEO_CHECK_WORLD(s.world(), W, H);
  auto fs = buildFaces(s.world());
  CHECK_EQ(fs->faces.size(), faces1 + 1);
  CHECK_NEAR(provArea(s.world(), a), 10000, 1e-6);
  CHECK_NEAR(provArea(s.world(), b), 6000, 1e-6);
  // крупный допуск из инструмента
  EditOptions opt;
  opt.snap = 5;
  Id c = s.transact("c", [&](Tx& tx) { return createProvince(tx, {{557, 203}, {600, 200}, {600, 300}, {562, 297}}, Terrain::Land, opt); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), c), 4000, 1e-6);
  CHECK_NEAR(provArea(s.world(), b), 6000, 1e-6);
}

TEST(geo_handles_move_insert_delete) {
  Store s = initStore();
  Id a = s.transact("a", [](Tx& tx) { return createProvince(tx, rect(400, 200, 500, 300), Terrain::Land); });
  Id b = s.transact("b", [](Tx& tx) { return createProvince(tx, rect(500, 200, 600, 300), Terrain::Land); });
  const World& w = s.world();
  // узел (500, 200) — стык трёх областей
  Handle h = hitHandle(w, {500.5, 200.4}, 2, 0);
  CHECK(h.kind == Handle::Node);
  CHECK(handlePos(w, h) == Vec2(500, 200));
  CHECK(!handleLocked(w, h));
  CHECK(!isCoastJunction(w, h.node));
  CHECK(canMove(w, h, {510, 190}));
  CHECK(!canMove(w, h, {610, 250}));   // перехлёст через соседнюю границу
  CHECK(!canMove(w, h, {400, 300}));   // в чужой узел
  double total = provArea(w, a) + provArea(w, b);
  s.transact("move", [&](Tx& tx) { moveHandle(tx, h, {510, 190}); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), a) + provArea(s.world(), b), total + 0.5 * 10 * 100 + 0.5 * 10 * 100, 1e-6);
  World before = s.world();
  CHECK_THROWS(s.transact("bad", [&](Tx& tx) { moveHandle(tx, h, {700, 250}); }));
  CHECK_EQ(World::diff(before, s.world()), 0u);
  // вставка точки в общую границу, перемещение, удаление
  auto eh = hitEdge(s.world(), {505.1, 245}, 1, a);  // общая граница идёт от (510, 190) к (500, 300)
  CHECK(eh.has_value());
  Handle p = s.transact("ins", [&](Tx& tx) { return insertPoint(tx, eh->edge, eh->segment, {505.1, 245}); });
  CHECK(p.kind == Handle::Point);
  CHECK_NEAR(handlePos(s.world(), p).x, 505, 0.2);
  double aA = provArea(s.world(), a);
  s.transact("mv", [&](Tx& tx) { moveHandle(tx, p, {530, 250}); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK(provArea(s.world(), a) > aA);  // общая граница сдвинута в сторону b
  CHECK(!canMove(s.world(), p, {700, 250}));
  s.transact("del", [&](Tx& tx) { deletePoint(tx, p); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), a), aA, 1e-6);
  // ручки фильтруются по провинции; точки берега заблокированы
  Handle hb = hitHandle(s.world(), {600, 300}, 2, b);
  CHECK(hb);
  CHECK(handlePos(s.world(), hb) == Vec2(600, 300));
  CHECK(!hitHandle(s.world(), {600, 300}, 2, a));
  Handle coastPt;
  s.world().edges.each([&](const Edge& e) {
    if (!coastPt && e.kind == EdgeKind::Coast && !e.pts.empty()) { coastPt.kind = Handle::Point; coastPt.edge = e.id; coastPt.index = 0; }
  });
  CHECK(handleLocked(s.world(), coastPt));
  CHECK(!canMove(s.world(), coastPt, handlePos(s.world(), coastPt) + Vec2(1, 1)));
  CHECK_THROWS(s.transact("coast", [&](Tx& tx) { moveHandle(tx, coastPt, handlePos(tx.w(), coastPt) + Vec2(1, 1)); }));
  // удаление узла степени 2: появляется при вставке узла? — создаём через нож и слияние нельзя; проверяем отказ для стыка трёх
  CHECK_THROWS(s.transact("delnode", [&](Tx& tx) { deletePoint(tx, h); }));
}

TEST(geo_delete_degree2_node) {
  Store s = initStore();
  Id a = s.transact("a", [](Tx& tx) { return createProvince(tx, rect(400, 200, 500, 300), Terrain::Land); });
  // кольцо провинции из двух дуг: удаление любого из двух узлов дало бы петлю — отказ
  std::vector<Id> ring;
  s.world().nodes.each([&](const Node& n) {
    if (n.p == Vec2(400, 200) || n.p == Vec2(500, 300)) ring.push_back(n.id);
  });
  CHECK_EQ(ring.size(), size_t(2));
  CHECK_THROWS(s.transact("loop", [&](Tx& tx) { deletePoint(tx, Handle{Handle::Node, ring[0], 0, -1}); }));
  // узел степени 2 в углу (500, 200), как в загруженных данных: дуга разрезана вручную
  Id mid = s.transact("cut", [&](Tx& tx) {
    Id eid = 0;
    tx.w().edges.each([&](const Edge& e) {
      if (e.kind == EdgeKind::Border && std::find(e.pts.begin(), e.pts.end(), Vec2(500, 200)) != e.pts.end()) eid = e.id;
    });
    Edge e = *tx.w().edges.get(eid);
    auto it = std::find(e.pts.begin(), e.pts.end(), Vec2(500, 200));
    Id n = tx.add(Node{0, {500, 200}}).id;
    Edge e2 = e;
    e2.id = 0;
    e2.a = n;
    e2.pts.assign(it + 1, e.pts.end());
    Edge& e1 = tx.edge(eid);
    e1.b = n;
    e1.pts.assign(e.pts.begin(), it);
    tx.add(e2);
    return n;
  });
  GEO_CHECK_WORLD(s.world(), W, H);
  Handle h{Handle::Node, mid, 0, -1};
  CHECK(!handleLocked(s.world(), h));
  s.transact("del", [&](Tx& tx) { deletePoint(tx, h); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK(!s.world().nodes.has(mid));
  CHECK_NEAR(provArea(s.world(), a), 5000, 1e-6);  // угол срезан диагональю
}

TEST(geo_slide_junction_along_coast) {
  Store s = initStore();
  // провинция на восточной части большого острова: стыки с берегом сверху и снизу
  Id a = s.transact("a", [](Tx& tx) { return createProvince(tx, {{560, 60}, {735, 60}, {735, 480}, {560, 480}}, Terrain::Land); });
  GEO_CHECK_WORLD(s.world(), W, H);
  std::vector<Id> junctions;
  s.world().nodes.each([&](const Node& n) { if (isCoastJunction(s.world(), n.id)) junctions.push_back(n.id); });
  CHECK(junctions.size() >= 2);
  // стык на западной стороне контура (x = 560), северный
  std::sort(junctions.begin(), junctions.end(), [&](Id x, Id y) {
    return dist(s.world().nodes.get(x)->p, {560, 0}) < dist(s.world().nodes.get(y)->p, {560, 0});
  });
  double land = terrainArea(s.world(), Terrain::Land);
  Id j = junctions[0];
  CHECK_NEAR(s.world().nodes.get(j)->p.x, 560, 1.0);  // линия могла прилипнуть к вершине берега
  Vec2 p0 = s.world().nodes.get(j)->p;
  // к востоку от стыка вдоль берега (провинция уменьшается или растёт, суша неизменна)
  Vec2 want = p0 + Vec2(25, 0);
  auto t = slideTarget(s.world(), j, want);
  CHECK(t.has_value());
  double aA = provArea(s.world(), a);
  s.transact("slide", [&](Tx& tx) { slideJunction(tx, j, want); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK(s.world().nodes.get(j)->p == *t);
  CHECK(dist(*t, p0) > 1);
  CHECK(std::fabs(provArea(s.world(), a) - aA) > 1);
  CHECK_NEAR(terrainArea(s.world(), Terrain::Land), land, 1e-6 * land);
  CHECK(isCoastJunction(s.world(), j));
  // свободное перемещение стыка запрещено
  CHECK(!canMove(s.world(), Handle{Handle::Node, j, 0, -1}, *t + Vec2(5, 5)));
  // обратно — площадь возвращается
  s.transact("back", [&](Tx& tx) { slideJunction(tx, j, p0); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), a), aA, 1e-6);
  // за соседний стык нельзя: цель у любого другого стыка недостижима
  for (size_t k = 1; k < junctions.size(); k++) {
    Vec2 other = s.world().nodes.get(junctions[k])->p;
    auto t2 = slideTarget(s.world(), j, other);
    CHECK(!t2.has_value() || dist(*t2, other) > kMinLen);
  }
  geotest::renderWorld(s.world(), "geo_slide.png", 1.0, Box2(0, 0, W, H));
}

// Суша, лежащая на рамке, выходящая за неё и занимающая угол карты.
TEST(geo_init_land_on_frame) {
  Store s;
  Coast c;
  c.width = W;
  c.height = H;
  c.landRings.push_back({{0, 100}, {200, 100}, {200, 300}, {0, 300}});              // сторона на x = 0
  c.landRings.push_back({{800, 0}, {1000, 0}, {1000, 150}, {800, 150}});            // угол (1000, 0)
  c.landRings.push_back({{-50, 400}, {150, 400}, {150, 550}, {-50, 550}});          // за краем карты
  c.landRings.push_back({{400, 590}, {500, 590}, {500, 650}, {400, 650}});          // за нижним краем
  s.transact("init", [&](Tx& tx) { initFromCoast(tx, c); });
  GEO_CHECK_WORLD(s.world(), W, H);
  double land = 200.0 * 200 + 200.0 * 150 + 150.0 * 150 + 100.0 * 10;
  CHECK_NEAR(terrainArea(s.world(), Terrain::Land), land, 1e-6);
  auto fs = buildFaces(s.world());
  CHECK_EQ(fs->faces.size(), size_t(5));
  CHECK(fs->terrainAt({990, 10}) == Terrain::Land);
  CHECK(fs->terrainAt({5, 200}) == Terrain::Land);
  CHECK(fs->terrainAt({450, 595}) == Terrain::Land);
  CHECK(fs->terrainAt({450, 585}) == Terrain::Sea);
  // береговых дуг на рамке нет: участки вдоль края остались рамкой
  s.world().edges.each([&](const Edge& e) {
    if (e.kind != EdgeKind::Coast) return;
    for (auto& p : edgeCoords(s.world(), e)) CHECK(p.x >= 0 && p.x <= W && p.y >= 0 && p.y <= H);
  });
  // провинция на суше у края: граница по рамке и по берегу
  Id p = s.transact("p", [](Tx& tx) { return createProvince(tx, rect(-10, 90, 120, 320), Terrain::Land); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), p), 120.0 * 200, 1e-6);
  // ещё раз — обратное снятие возвращает исходные дуги рамки
  s.transact("u", [&](Tx& tx) { unassign(tx, p); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_EQ(countKind(s.world(), EdgeKind::Border), 0);
}

// Одинаковая последовательность операций даёт побайтно одинаковую геометрию (детерминизм).
TEST(geo_determinism) {
  auto runOnce = [] {
    Store s = initStore();
    Id a = s.transact("a", [](Tx& tx) { return createProvince(tx, {{430, 120}, {600, 140}, {640, 300}, {560, 470}, {420, 480}}, Terrain::Land); });
    Id b = s.transact("b", [&](Tx& tx) { return split(tx, a, {{400, 300}, {700, 280}}); });
    s.transact("c", [&](Tx& tx) { addArea(tx, b, rect(450, 250, 650, 420)); });
    s.transact("d", [](Tx& tx) { createProvince(tx, rect(700, 50, 990, 250), Terrain::Sea); });
    std::vector<std::string> dump;
    s.world().nodes.each([&](const Node& n) { dump.push_back(strf("n%u %.17g %.17g", n.id, n.p.x, n.p.y)); });
    s.world().edges.each([&](const Edge& e) {
      std::string x = strf("e%u %u %u %d %u %u %d %d", e.id, e.a, e.b, int(e.kind), e.pl, e.pr, int(e.tl), int(e.tr));
      for (auto& p : e.pts) x += strf(" %.17g %.17g", p.x, p.y);
      dump.push_back(x);
    });
    return dump;
  };
  auto d1 = runOnce(), d2 = runOnce();
  CHECK_EQ(d1.size(), d2.size());
  CHECK(d1 == d2);
}

TEST(geo_hit_priorities) {
  Store s = initStore();
  Id a = s.transact("a", [](Tx& tx) { return createProvince(tx, rect(400, 200, 500, 300), Terrain::Land); });
  s.transact("b", [](Tx& tx) { return createProvince(tx, rect(500, 200, 600, 300), Terrain::Land); });
  const World& w = s.world();
  // узел (500, 200) предпочтительнее более близкой промежуточной точки
  Handle n = hitHandle(w, {500, 200}, 3);
  CHECK(n.kind == Handle::Node);
  // ближе к углу (400, 300) — точка, а не узел (узла там нет)
  Handle p = hitHandle(w, {401, 299}, 3, a);
  CHECK(p);
  CHECK(handlePos(w, p) == Vec2(400, 300));
  // далеко — ничего
  CHECK(!hitHandle(w, {450, 250}, 3));
  // ребро: проекция и расстояние
  auto eh = hitEdge(w, {450, 202}, 3, a);
  CHECK(eh.has_value());
  CHECK_NEAR(eh->dist, 2, 1e-9);
  CHECK_NEAR(eh->p.y, 200, 1e-9);
  CHECK(!hitEdge(w, {450, 250}, 3).has_value());
  // ручки берега заблокированы и уступают подвижным
  Handle coast = hitHandle(w, {760, 300}, 40);  // у подковы только береговые точки
  CHECK(coast);
  CHECK(handleLocked(w, coast));
}
