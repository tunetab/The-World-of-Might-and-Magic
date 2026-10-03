// Сценарии geo: провинция из нескольких островов, нож через дыры, морские провинции вокруг островов,
// атолл с лагуной и островком, слияние несмежных провинций, удаление провинции и отмена, варианты с EditOptions,
// скольжение стыка на кольце с одним стыком, понятные сообщения об ошибках.
#include "tests/test_geo_util.h"

using namespace rg;
using namespace rg::geo;
using geotest::sampleCoast;

namespace {

constexpr double W = 1000, H = 600;

Store initStore() {
  Store s;
  s.transact("init", [](Tx& tx) { initFromCoast(tx, sampleCoast(W, H)); });
  s.clearHistory();
  return s;
}

std::vector<Vec2> rect(double x0, double y0, double x1, double y1) { return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}; }

double provArea(const World& w, Id p) {
  auto fs = buildFaces(w);
  auto* s = fs->shape(p);
  return s ? s->area : 0;
}

size_t provFaces(const World& w, Id p) {
  auto fs = buildFaces(w);
  auto* s = fs->shape(p);
  return s ? s->faces.size() : 0;
}

double faceAreaAt(const World& w, Vec2 p) {
  auto fs = buildFaces(w);
  int f = fs->locate(p);
  return f < 0 ? 0 : fs->faces[size_t(f)].area;
}

double landArea(const World& w) {
  auto fs = buildFaces(w);  // держать FaceSet, пока идёт обход граней
  double a = 0;
  for (auto& f : fs->faces) a += f.terrain == Terrain::Land ? f.area : 0;
  return a;
}

const Face* faceAt(const FaceSet& fs, Vec2 p) {
  int f = fs.locate(p);
  return f < 0 ? nullptr : &fs.faces[size_t(f)];
}

int countKind(const World& w, EdgeKind k) {
  int n = 0;
  w.edges.each([&](const Edge& e) { n += e.kind == k; });
  return n;
}

bool refersTo(const World& w, Id p) {
  bool r = false;
  w.edges.each([&](const Edge& e) { r = r || e.pl == p || e.pr == p; });
  return r;
}

bool hasPair(const std::vector<std::pair<Id, Id>>& nb, Id a, Id b) {
  return std::find(nb.begin(), nb.end(), std::make_pair(std::min(a, b), std::max(a, b))) != nb.end();
}

bool hasCode(const std::vector<Issue>& is, const std::string& code) {
  for (auto& i : is)
    if (i.code == code) return true;
  return false;
}

bool contains(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

// Сообщение отказа операции (пусто — операция прошла). Отказ не меняет мир.
template <class F> std::string errorOf(Store& s, F&& fn) {
  World before = s.world();
  try {
    s.transact("probe", fn);
  } catch (const UserError& e) {
    CHECK_EQ(World::diff(before, s.world()), 0u);
    return e.what();
  }
  return {};
}

// ---------------------------------------------------------------- ручная сборка графа (атолл)
struct Lbl { Id p = 0; Terrain t = Terrain::Land; };

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

// Кольцо против часовой (математически) из двух дуг: слева — inside, справа — outside.
void addRing(Tx& tx, const std::vector<Vec2>& ring, EdgeKind k, Lbl inside, Lbl outside) {
  size_t h = ring.size() / 2;
  Id n0 = tx.add(Node{0, ring[0]}).id, n1 = tx.add(Node{0, ring[h]}).id;
  std::vector<Vec2> p1(ring.begin() + 1, ring.begin() + long(h)), p2(ring.begin() + long(h) + 1, ring.end());
  addEdge(tx, n0, n1, p1, k, inside, outside);
  addEdge(tx, n1, n0, p2, k, inside, outside);
}

std::vector<Vec2> circle(Vec2 c, double r, int n) {
  std::vector<Vec2> v;
  for (int i = 0; i < n; i++) v.push_back({c.x + r * std::cos(2 * kPi * i / n), c.y + r * std::sin(2 * kPi * i / n)});
  return v;
}

// Атолл 1000 × 1000: кольцо суши (R = 300), лагуна (r = 150), в лагуне островок 80 × 80.
Store atollStore() {
  Store s;
  s.transact("atoll", [](Tx& tx) {
    Id a = tx.add(Node{0, {0, 0}}).id, b = tx.add(Node{0, {1000, 0}}).id, c = tx.add(Node{0, {1000, 1000}}).id,
       d = tx.add(Node{0, {0, 1000}}).id;
    Lbl sea{0, Terrain::Sea}, out{0, Terrain::None}, land{0, Terrain::Land};
    addEdge(tx, a, b, {}, EdgeKind::Frame, sea, out);
    addEdge(tx, b, c, {}, EdgeKind::Frame, sea, out);
    addEdge(tx, c, d, {}, EdgeKind::Frame, sea, out);
    addEdge(tx, d, a, {}, EdgeKind::Frame, sea, out);
    addRing(tx, circle({500, 500}, 300, 64), EdgeKind::Coast, land, sea);
    addRing(tx, circle({500, 500}, 150, 48), EdgeKind::Coast, sea, land);
    addRing(tx, {{460, 460}, {540, 460}, {540, 540}, {460, 540}}, EdgeKind::Coast, land, sea);
  });
  s.clearHistory();
  return s;
}

}  // namespace

// Провинция из двух островов: грани, площадь, подпись в крупнейшей грани; нож между островами — отказ;
// нож через один остров отрезает часть только этого острова; слияние возвращает всё.
TEST(geo_scenario_multi_island_province) {
  Store s = initStore();
  double isl = faceAreaAt(s.world(), {820, 140}), shoe = faceAreaAt(s.world(), {760, 300});
  CHECK(shoe > isl);
  Id p = s.transact("p", [](Tx& tx) { return createProvince(tx, rect(730, 70, 905, 385), Terrain::Land); });
  GEO_CHECK_WORLD(s.world(), W, H);
  {
    auto fs = buildFaces(s.world());
    const ProvinceShape* sh = fs->shape(p);
    CHECK(sh != nullptr);
    CHECK_EQ(sh->faces.size(), size_t(2));
    CHECK_NEAR(sh->area, isl + shoe, 1e-6);
    CHECK_EQ(fs->provinceAt({820, 140}), p);
    CHECK_EQ(fs->provinceAt({760, 300}), p);
    CHECK_EQ(fs->provinceAt({820, 300}), Id(0));  // залив подковы — море
    CHECK_EQ(fs->locate(sh->label), fs->locate({760, 300}));  // подпись — в крупнейшей грани
    CHECK(sh->box.contains({820, 140}) && sh->box.contains({760, 300}));
    CHECK(fs->neighbors().empty());
  }
  CHECK_EQ(countKind(s.world(), EdgeKind::Border), 0);
  // нож проходит между островами, не задевая их
  std::string err = errorOf(s, [&](Tx& tx) { split(tx, p, {{700, 210}, {950, 210}}); });
  CHECK_MSG(contains(err, "от края до края"), err);
  // нож через западную часть подковы
  Id q = s.transact("cut", [&](Tx& tx) { return split(tx, p, {{770, 200}, {770, 400}}); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), p) + provArea(s.world(), q), isl + shoe, 1e-6);
  CHECK(provArea(s.world(), q) < provArea(s.world(), p));
  CHECK_EQ(provFaces(s.world(), p), size_t(2));  // остров + остаток подковы
  CHECK_EQ(provFaces(s.world(), q), size_t(1));
  CHECK(!s.world().province(q)->sea);
  {
    auto fs = buildFaces(s.world());
    CHECK_EQ(fs->provinceAt({760, 300}), q);
    CHECK_EQ(fs->provinceAt({820, 140}), p);
    CHECK(hasPair(fs->neighbors(), p, q));
  }
  geotest::renderWorld(s.world(), "geo_scenario_islands.png", 2.0, Box2(700, 60, 920, 400));
  s.transact("merge", [&](Tx& tx) { merge(tx, p, q); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), p), isl + shoe, 1e-6);
  CHECK_EQ(countKind(s.world(), EdgeKind::Border), 0);
  // морская провинция тем же контуром: одна грань с двумя дырами-островами (залив соединён с морем)
  Id sea = s.transact("sea", [](Tx& tx) { return createProvince(tx, rect(730, 70, 905, 385), Terrain::Sea); });
  GEO_CHECK_WORLD(s.world(), W, H);
  auto fs = buildFaces(s.world());
  CHECK_EQ(fs->shape(sea)->faces.size(), size_t(1));
  CHECK_EQ(fs->faces[size_t(fs->shape(sea)->faces[0])].rings.size(), size_t(3));
  CHECK_NEAR(provArea(s.world(), sea), 175.0 * 315.0 - isl - shoe, 1e-6);
  CHECK(hasPair(fs->neighbors(), p, sea));
}

// Нож через дыру: провинция-кольцо (внутри — другая провинция) разрезается на две части,
// внутренняя провинция не меняется; нож, заходящий в дыру с одной стороны, — отказ.
TEST(geo_scenario_knife_through_hole) {
  Store s = initStore();
  Id a = s.transact("a", [](Tx& tx) { return createProvince(tx, rect(400, 220, 560, 380), Terrain::Land); });
  Id b = s.transact("b", [](Tx& tx) { return createProvince(tx, rect(450, 270, 510, 330), Terrain::Land); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), a), 160.0 * 160 - 60.0 * 60, 1e-6);
  CHECK_NEAR(provArea(s.world(), b), 3600, 1e-6);
  {
    auto fs = buildFaces(s.world());
    CHECK_EQ(fs->faces[size_t(fs->shape(a)->faces[0])].rings.size(), size_t(2));  // дыра — провинция b
  }
  std::string err = errorOf(s, [&](Tx& tx) { split(tx, a, {{380, 300}, {480, 300}}); });
  CHECK_MSG(contains(err, "от края до края"), err);
  Id c = s.transact("knife", [&](Tx& tx) { return split(tx, a, {{380, 300}, {580, 300}}); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), a), 11000, 1e-6);
  CHECK_NEAR(provArea(s.world(), c), 11000, 1e-6);
  CHECK_NEAR(provArea(s.world(), b), 3600, 1e-6);
  CHECK_EQ(provFaces(s.world(), a), size_t(1));
  CHECK_EQ(provFaces(s.world(), c), size_t(1));
  {
    auto fs = buildFaces(s.world());
    auto nb = fs->neighbors();
    CHECK(hasPair(nb, a, b) && hasPair(nb, c, b) && hasPair(nb, a, c));
    // вертикаль через середину: верхняя часть, дыра, нижняя часть
    auto on = fs->provincesOnPolyline({{480, 230}, {480, 370}});
    CHECK_EQ(on.size(), size_t(3));
    if (on.size() == 3) {
      CHECK_EQ(on[1], b);
      CHECK(on[0] != on[2] && (on[0] == a || on[0] == c) && (on[2] == a || on[2] == c));
    }
  }
  // нож только через внутреннюю провинцию: части кольца не меняются
  Id d = s.transact("inner", [&](Tx& tx) { return split(tx, b, {{480, 250}, {480, 350}}); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), b) + provArea(s.world(), d), 3600, 1e-6);
  CHECK_NEAR(provArea(s.world(), a), 11000, 1e-6);
  CHECK_NEAR(provArea(s.world(), c), 11000, 1e-6);
  geotest::renderWorld(s.world(), "geo_scenario_hole.png", 3.0, Box2(370, 200, 590, 400));
}

// Морские провинции: дыра-остров, нож через остров, отказы при смешении суши и моря.
TEST(geo_scenario_sea_provinces) {
  Store s = initStore();
  double isl = faceAreaAt(s.world(), {820, 140});
  Id sea = s.transact("sea", [](Tx& tx) { return createProvince(tx, rect(740, 60, 900, 220), Terrain::Sea); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK(s.world().province(sea)->sea);
  CHECK_NEAR(provArea(s.world(), sea), 160.0 * 160 - isl, 1e-6);
  {
    auto fs = buildFaces(s.world());
    const Face* f = faceAt(*fs, {750, 70});
    CHECK(f && f->province == sea && f->terrain == Terrain::Sea && f->rings.size() == 2);
    CHECK_EQ(fs->provinceAt({820, 140}), Id(0));
  }
  // нож с записью другого типа — отказ
  std::string err = errorOf(s, [&](Tx& tx) {
    split(tx, sea, {{730, 140}, {910, 140}}, [](Tx& t, Id) { return t.add(Province{}).id; });  // запись суши
  });
  CHECK_MSG(contains(err, "того же типа"), err);
  // нож через остров: режется только море
  Id s2 = s.transact("knife", [&](Tx& tx) { return split(tx, sea, {{730, 140}, {910, 140}}); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK(s.world().province(s2)->sea);
  CHECK_NEAR(provArea(s.world(), sea) + provArea(s.world(), s2), 160.0 * 160 - isl, 1e-6);
  CHECK_EQ(buildFaces(s.world())->provinceAt({820, 140}), Id(0));
  // морской провинции нельзя назначить сушу
  err = errorOf(s, [&](Tx& tx) { fillAt(tx, {820, 140}, sea); });
  CHECK_MSG(contains(err, "только море"), err);
  Id land = s.transact("land", [](Tx& tx) { return fillAt(tx, {820, 140}, 0); });
  CHECK(!s.world().province(land)->sea);
  CHECK_NEAR(provArea(s.world(), land), isl, 1e-6);
  CHECK(hasPair(buildFaces(s.world())->neighbors(), land, sea));
  // расширение моря поверх острова: суша не задета, морская часть соседа переходит
  double before = provArea(s.world(), sea) + provArea(s.world(), s2);
  s.transact("add", [&](Tx& tx) { addArea(tx, sea, rect(780, 100, 880, 200)); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), land), isl, 1e-6);
  CHECK_NEAR(provArea(s.world(), sea) + provArea(s.world(), s2), before, 1e-6);
  // вырезание в «не назначено» (у северо-западного угла — та часть, что там оказалась после ножа)
  Id nw = buildFaces(s.world())->provinceAt({755, 75});
  CHECK(nw == sea || nw == s2);
  double a0 = provArea(s.world(), nw);
  s.transact("remove", [&](Tx& tx) { removeArea(tx, nw, rect(745, 65, 765, 85)); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), nw), a0 - 400, 1e-6);
  CHECK_EQ(buildFaces(s.world())->provinceAt({755, 75}), Id(0));
  CHECK(buildFaces(s.world())->terrainAt({755, 75}) == Terrain::Sea);
  // сушу с морем не объединить
  err = errorOf(s, [&](Tx& tx) { merge(tx, sea, land); });
  CHECK_MSG(contains(err, "сухопутную провинцию с морской"), err);
  s.transact("merge", [&](Tx& tx) { merge(tx, sea, s2); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), sea), 160.0 * 160 - isl - 400, 1e-6);
  CHECK_EQ(provArea(s.world(), s2), 0.0);
  geotest::renderWorld(s.world(), "geo_scenario_sea.png", 3.0, Box2(720, 50, 920, 230));
}

// Атолл: кольцо суши вокруг лагуны с островком. Провинция суши из двух граней (кольцо с дырой и островок),
// морская провинция лагуны (с дырой-островком), океан; нож через всю фигуру; слияние и снятие — исходный граф.
TEST(geo_scenario_lagoon_atoll) {
  Store s = atollStore();
  const World w0 = s.world();
  CHECK_MSG(validate(w0).empty(), geotest::issuesText(validate(w0)));
  auto fs0 = buildFaces(w0);
  CHECK_EQ(fs0->faces.size(), size_t(4));
  double ring = faceAt(*fs0, {500, 250})->area, lagoon = faceAt(*fs0, {500, 400})->area, islet = faceAt(*fs0, {500, 500})->area;
  CHECK_NEAR(islet, 6400, 1e-9);
  CHECK(faceAt(*fs0, {500, 400})->terrain == Terrain::Sea);
  const u32 nodes0 = w0.nodes.size();

  Id land = s.transact("land", [](Tx& tx) { return createProvince(tx, rect(150, 150, 850, 850), Terrain::Land); });
  GEO_CHECK_WORLD(s.world(), 1000, 1000);
  CHECK_EQ(provFaces(s.world(), land), size_t(2));
  CHECK_NEAR(provArea(s.world(), land), ring + islet, 1e-6);
  CHECK_EQ(countKind(s.world(), EdgeKind::Border), 0);
  Id lag = s.transact("lagoon", [](Tx& tx) { return createProvince(tx, rect(300, 300, 700, 700), Terrain::Sea); });
  GEO_CHECK_WORLD(s.world(), 1000, 1000);
  CHECK(s.world().province(lag)->sea);
  CHECK_NEAR(provArea(s.world(), lag), lagoon, 1e-6);
  Id ocean = s.transact("ocean", [](Tx& tx) { return fillAt(tx, {50, 50}, 0); });
  CHECK(s.world().province(ocean)->sea);
  CHECK_NEAR(provArea(s.world(), ocean), 1e6 - ring - lagoon - islet, 1e-6);
  {
    auto fs = buildFaces(s.world());
    CHECK_EQ(faceAt(*fs, {500, 400})->rings.size(), size_t(2));  // лагуна с дырой-островком
    CHECK_EQ(faceAt(*fs, {50, 50})->rings.size(), size_t(2));    // океан с дырой-атоллом
    auto nb = fs->neighbors();
    CHECK(hasPair(nb, land, lag) && hasPair(nb, land, ocean));
    CHECK(!hasPair(nb, lag, ocean));  // лагуна замкнута
    auto on = fs->provincesOnPolyline({{20, 500}, {500, 500}});
    CHECK_EQ(on.size(), size_t(3));
    if (on.size() == 3) CHECK(on[0] == ocean && on[1] == land && on[2] == lag);
  }
  // нож через океан, кольцо, лагуну, островок, лагуну, кольцо, океан
  Id south = s.transact("knife", [&](Tx& tx) { return split(tx, land, {{100, 500}, {900, 500}}); });
  GEO_CHECK_WORLD(s.world(), 1000, 1000);
  CHECK_NEAR(provArea(s.world(), land) + provArea(s.world(), south), ring + islet, 1e-6);
  CHECK_EQ(provFaces(s.world(), land), size_t(2));
  CHECK_EQ(provFaces(s.world(), south), size_t(2));
  CHECK_NEAR(provArea(s.world(), lag), lagoon, 1e-6);
  CHECK_NEAR(provArea(s.world(), ocean), 1e6 - ring - lagoon - islet, 1e-6);
  {
    auto fs = buildFaces(s.world());
    // половины по одну сторону ножа — одной провинции
    Id n1 = fs->provinceAt({500, 250}), n2 = fs->provinceAt({500, 480});
    Id s1 = fs->provinceAt({500, 750}), s3 = fs->provinceAt({500, 520});
    CHECK_EQ(n1, n2);
    CHECK_EQ(s1, s3);
    CHECK(n1 != s1);
    CHECK((n1 == land && s1 == south) || (n1 == south && s1 == land));
  }
  geotest::renderWorld(s.world(), "geo_scenario_atoll.png", 0.6, Box2(0, 0, 1000, 1000));
  s.transact("merge", [&](Tx& tx) { merge(tx, land, south); });
  GEO_CHECK_WORLD(s.world(), 1000, 1000);
  CHECK_EQ(provFaces(s.world(), land), size_t(2));
  CHECK_NEAR(provArea(s.world(), land), ring + islet, 1e-6);
  CHECK_EQ(countKind(s.world(), EdgeKind::Border), 0);
  CHECK_EQ(s.world().nodes.size(), nodes0);  // точки разреза на берегу убраны
  for (Id p : {land, lag, ocean}) s.transact("un", [&](Tx& tx) { unassign(tx, p); });
  GEO_CHECK_WORLD(s.world(), 1000, 1000);
  CHECK_EQ(s.world().nodes.size(), nodes0);
  CHECK_EQ(s.world().edges.size(), w0.edges.size());
  // отмена до исходного состояния — в точности
  while (s.undo()) {}
  CHECK_EQ(World::diff(s.world(), w0), 0u);
}

// Слияние несмежных провинций (острова); слияние с провинцией без территории.
TEST(geo_scenario_merge_non_adjacent) {
  Store s = initStore();
  Id p1 = s.transact("f1", [](Tx& tx) { return fillAt(tx, {860, 470}, 0); });
  Id p2 = s.transact("f2", [](Tx& tx) { return fillAt(tx, {720, 520}, 0); });
  double a1 = provArea(s.world(), p1), a2 = provArea(s.world(), p2);
  CHECK(buildFaces(s.world())->neighbors().empty());
  int edges0 = int(s.world().edges.size());
  s.transact("merge", [&](Tx& tx) { merge(tx, p1, p2); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), p1), a1 + a2, 1e-9);
  CHECK_EQ(provFaces(s.world(), p1), size_t(2));
  CHECK(!refersTo(s.world(), p2));
  CHECK(s.world().province(p2) != nullptr);  // запись остаётся (удаляет rules)
  CHECK_EQ(int(s.world().edges.size()), edges0);  // берег не меняется
  // пустая запись: слияние с ней ничего не меняет, слияние в неё переносит территорию
  Id empty = s.transact("rec", [](Tx& tx) { return tx.add(Province{}).id; });
  World before = s.world();
  s.transact("noop", [&](Tx& tx) { merge(tx, p1, empty); });
  CHECK_EQ(World::diff(before, s.world()), 0u);
  s.transact("into", [&](Tx& tx) { merge(tx, empty, p1); });
  CHECK_NEAR(provArea(s.world(), empty), a1 + a2, 1e-9);
  CHECK_EQ(provArea(s.world(), p1), 0.0);
  std::string err = errorOf(s, [&](Tx& tx) { merge(tx, empty, 9999); });
  CHECK_MSG(contains(err, "не найдена"), err);
}

// Удаление провинции (снятие назначения + удаление записи) и отмена; запись без снятия — ошибка проверки
// и понятный отказ операций, unassign чинит граф.
TEST(geo_scenario_delete_province_undo) {
  Store s = initStore();
  Id a = s.transact("a", [](Tx& tx) { return createProvince(tx, rect(400, 200, 500, 300), Terrain::Land); });
  Id b = s.transact("b", [](Tx& tx) { return createProvince(tx, rect(500, 200, 600, 300), Terrain::Land); });
  const World w0 = s.world();
  auto fs0 = faces(w0);
  s.transact("delete", [&](Tx& tx) {
    unassign(tx, b);
    tx.eraseProvince(b);
  });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK(s.world().province(b) == nullptr);
  CHECK(!refersTo(s.world(), b));
  CHECK(faces(s.world())->shape(b) == nullptr);
  CHECK_NEAR(provArea(s.world(), a), 10000, 1e-6);
  CHECK_EQ(buildFaces(s.world())->provinceAt({550, 250}), Id(0));
  const World w1 = s.world();
  CHECK(s.undo());
  CHECK_EQ(World::diff(s.world(), w0), 0u);
  CHECK(faces(s.world()) == fs0);  // та же версия графа — тот же кеш
  CHECK_NEAR(faces(s.world())->shape(b)->area, 10000, 1e-6);
  CHECK(s.redo());
  CHECK_EQ(World::diff(s.world(), w1), 0u);
  CHECK(s.undo());
  // обратный порядок в одной транзакции
  s.transact("delete2", [&](Tx& tx) {
    tx.eraseProvince(b);
    unassign(tx, b);
  });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK(!refersTo(s.world(), b));
  CHECK(s.undo());
  // только запись: граф ссылается на несуществующую провинцию
  s.transact("erase", [&](Tx& tx) { tx.eraseProvince(b); });
  CHECK(hasCode(validate(s.world()), "province-missing"));
  std::string err = errorOf(s, [](Tx& tx) { createProvince(tx, rect(300, 150, 350, 250), Terrain::Land); });
  CHECK_MSG(contains(err, "несуществующую провинцию"), err);
  s.transact("repair", [&](Tx& tx) { unassign(tx, b); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), a), 10000, 1e-6);
  // после починки операции снова работают
  Id c = s.transact("c", [](Tx& tx) { return createProvince(tx, rect(500, 200, 600, 300), Terrain::Land); });
  CHECK_NEAR(provArea(s.world(), c), 10000, 1e-6);
  GEO_CHECK_WORLD(s.world(), W, H);
}

// Варианты addArea/removeArea/split с EditOptions: прилипание к узлам и границам с заданным допуском.
TEST(geo_edit_options_overloads) {
  Id a = 0, b = 0;
  auto two = [&] {
    Store s = initStore();
    a = s.transact("a", [](Tx& tx) { return createProvince(tx, rect(400, 200, 500, 300), Terrain::Land); });
    b = s.transact("b", [](Tx& tx) { return createProvince(tx, rect(500, 200, 600, 300), Terrain::Land); });
    return s;
  };
  const std::vector<Vec2> addPoly{{503, 197}, {550, 200}, {550, 300}, {497, 303}};
  EditOptions opt;
  opt.snap = 5;
  Store s = two();
  size_t faces0 = buildFaces(s.world())->faces.size();
  s.transact("add", [&](Tx& tx) { addArea(tx, a, addPoly, opt); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), a), 15000, 1e-6);
  CHECK_NEAR(provArea(s.world(), b), 5000, 1e-6);
  CHECK_EQ(buildFaces(s.world())->faces.size(), faces0);  // без щелевых граней
  // тот же контур с допуском по умолчанию захватывает лишние полоски
  Store s1 = two();
  s1.transact("add", [&](Tx& tx) { addArea(tx, a, addPoly); });
  GEO_CHECK_WORLD(s1.world(), W, H);
  CHECK(std::fabs(provArea(s1.world(), a) - 15000) > 1);
  // вырезание: вершины прилипают к сторонам и углу провинции
  s.transact("remove", [&](Tx& tx) { removeArea(tx, a, {{398, 250}, {450, 250}, {450, 302}, {398, 302}}, opt); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), a), 12500, 1e-6);
  // нож, не доходящий до границы на 3: с допуском 5 — разрез, с допуском 1 — отказ
  std::string err = errorOf(s, [&](Tx& tx) { split(tx, a, {{403, 225}, {547, 225}}); });
  CHECK_MSG(contains(err, "от края до края"), err);
  Id c = s.transact("split", [&](Tx& tx) { return split(tx, a, {{403, 225}, {547, 225}}, {}, opt); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK_NEAR(provArea(s.world(), c), 150.0 * 25, 1e-6);
  CHECK_NEAR(provArea(s.world(), a), 12500 - 150.0 * 25, 1e-6);
  CHECK_NEAR(provArea(s.world(), b), 5000, 1e-6);
  // нечисловой и отрицательный допуск заменяются минимальным
  EditOptions bad;
  bad.snap = std::nan("");
  Id d = s.transact("nan-snap", [&](Tx& tx) { return createProvince(tx, rect(420, 320, 470, 370), Terrain::Land, bad); });
  CHECK_NEAR(provArea(s.world(), d), 2500, 1e-6);
  bad.snap = -3;
  s.transact("neg-snap", [&](Tx& tx) { removeArea(tx, d, rect(430, 330, 440, 340), bad); });
  CHECK_NEAR(provArea(s.world(), d), 2400, 1e-6);
  bad.snap = 1e300;  // огромный допуск ограничивается
  s.transact("huge-snap", [&](Tx& tx) { addArea(tx, d, rect(430, 330, 440, 340), bad); });
  CHECK_NEAR(provArea(s.world(), d), 2500, 1e-6);  // вершины слились бы — допуск уменьшается до точного
  GEO_CHECK_WORLD(s.world(), W, H);
}

// Кольцо берега с единственным стыком (граница касается острова в одной точке): скольжение вдоль кольца.
TEST(geo_scenario_slide_single_junction_ring) {
  Store s = initStore();
  // крайняя правая точка острова (860, 470)
  Vec2 P{-1e9, 0};
  s.world().edges.each([&](const Edge& e) {
    if (e.kind != EdgeKind::Coast) return;
    for (auto& q : edgeCoords(s.world(), e))
      if (dist(q, {860, 470}) < 60 && q.x > P.x) P = q;
  });
  CHECK(P.x > 880);
  Id p = s.transact("tri", [&](Tx& tx) {
    return createProvince(tx, {P, P + Vec2(-20, 10), P + Vec2(-20, -10)}, Terrain::Land, EditOptions{0.01});
  });
  GEO_CHECK_WORLD(s.world(), W, H);
  Id J = 0;
  s.world().nodes.each([&](const Node& n) { if (n.p == P) J = n.id; });
  CHECK(J != 0);
  CHECK(isCoastJunction(s.world(), J));
  Handle hj{Handle::Node, J, 0, -1};
  CHECK(!handleLocked(s.world(), hj));
  CHECK(!canMove(s.world(), hj, P + Vec2(-3, 0)));
  std::string err = errorOf(s, [&](Tx& tx) { moveHandle(tx, hj, P + Vec2(-3, 0)); });
  CHECK_MSG(contains(err, "вдоль берега"), err);
  double a0 = provArea(s.world(), p);
  double land0 = landArea(s.world());
  auto t = slideTarget(s.world(), J, P + Vec2(0, 6));
  CHECK(t.has_value());
  s.transact("slide", [&](Tx& tx) { slideJunction(tx, J, P + Vec2(0, 6)); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK(t && s.world().nodes.get(J)->p == *t);
  CHECK(isCoastJunction(s.world(), J));
  CHECK(std::fabs(provArea(s.world(), p) - a0) > 1e-3);
  CHECK_NEAR(landArea(s.world()), land0, 1e-6 * land0);
  // и обратно в исходную точку
  s.transact("back", [&](Tx& tx) { slideJunction(tx, J, P); });
  GEO_CHECK_WORLD(s.world(), W, H);
  CHECK(s.world().nodes.get(J)->p == P);
  CHECK_NEAR(provArea(s.world(), p), a0, 1e-6);
  // отказ: не стык
  err = errorOf(s, [&](Tx& tx) { slideJunction(tx, 999999, P); });
  CHECK_MSG(contains(err, "только стык"), err);
}

// Понятные сообщения об ошибках; отказ не меняет мир (errorOf).
TEST(geo_fail_messages) {
  Store empty;
  std::string err = errorOf(empty, [](Tx& tx) { createProvince(tx, rect(1, 1, 5, 5), Terrain::Land); });
  CHECK_MSG(contains(err, "Карта ещё не создана"), err);
  err = errorOf(empty, [](Tx& tx) { fillAt(tx, {1, 1}, 0); });
  CHECK_MSG(contains(err, "Карта ещё не создана"), err);
  err = errorOf(empty, [](Tx& tx) { initFromCoast(tx, Coast{0, 100, {}}); });
  CHECK_MSG(contains(err, "размер карты"), err);

  Store s = initStore();
  Id a = s.transact("a", [](Tx& tx) { return createProvince(tx, rect(400, 200, 500, 300), Terrain::Land); });
  struct Case { const char* want; std::function<void(Tx&)> fn; };
  const Case cases[] = {
      {"уже построена", [](Tx& tx) { initFromCoast(tx, sampleCoast(W, H)); }},
      {"пересекает сам себя", [](Tx& tx) { createProvince(tx, {{400, 200}, {500, 300}, {500, 200}, {400, 300}}, Terrain::Land); }},
      {"не менее трёх точек", [](Tx& tx) { createProvince(tx, {{400, 200}, {500, 300}}, Terrain::Land); }},
      {"нулевая площадь", [](Tx& tx) { createProvince(tx, {{400, 200}, {450, 250}, {500, 300}}, Terrain::Land); }},
      {"Недопустимые координаты", [](Tx& tx) { createProvince(tx, {{400, 200}, {kInf, 300}, {500, 200}}, Terrain::Land); }},
      {"вне карты", [](Tx& tx) { createProvince(tx, rect(1100, 100, 1200, 200)); }},
      {"не захватывает сушу", [](Tx& tx) { createProvince(tx, rect(905, 10, 990, 60), Terrain::Land); }},
      {"не захватывает море", [](Tx& tx) { createProvince(tx, rect(420, 220, 480, 280), Terrain::Sea); }},
      {"не найдена", [](Tx& tx) { addArea(tx, 777, rect(0, 0, 10, 10)); }},
      {"не добавляет", [&](Tx& tx) { addArea(tx, a, rect(420, 220, 480, 280)); }},
      {"не задевает", [&](Tx& tx) { removeArea(tx, a, rect(10, 10, 30, 30)); }},
      {"вне карты", [](Tx& tx) { fillAt(tx, {-5, -5}, 0); }},
      {"только сушу", [&](Tx& tx) { fillAt(tx, {950, 50}, a); }},
      {"не найдена", [](Tx& tx) { split(tx, 777, {{0, 0}, {10, 10}}); }},
      {"не менее двух точек", [&](Tx& tx) { split(tx, a, {{400, 250}}); }},
      {"пересекает сама себя", [&](Tx& tx) { split(tx, a, {{380, 220}, {520, 280}, {520, 220}, {380, 280}}); }},
      {"от края до края", [&](Tx& tx) { split(tx, a, {{420, 250}, {480, 250}}); }},
      {"с самой собой", [&](Tx& tx) { merge(tx, a, a); }},
      {"Не выбрана", [](Tx& tx) { moveHandle(tx, Handle{}, {1, 1}); }},
      {"Не выбрана", [](Tx& tx) { deletePoint(tx, Handle{}); }},
      {"Граница не найдена", [](Tx& tx) { insertPoint(tx, 999999, 0, {1, 1}); }},
  };
  for (auto& c : cases) {
    err = errorOf(s, c.fn);
    CHECK_MSG(contains(err, c.want), std::string(c.want) + " <- \"" + err + "\"");
  }
  // ручки: берег и рамка заблокированы, вынос за карту, вставка у вершины
  Id coastEdge = 0;
  s.world().edges.each([&](const Edge& e) { if (!coastEdge && e.kind == EdgeKind::Coast && e.pts.size() > 2) coastEdge = e.id; });
  Handle cp{Handle::Point, 0, coastEdge, 1};
  err = errorOf(s, [&](Tx& tx) { moveHandle(tx, cp, handlePos(tx.w(), cp) + Vec2(1, 0)); });
  CHECK_MSG(contains(err, "не редактируются"), err);
  err = errorOf(s, [&](Tx& tx) { insertPoint(tx, coastEdge, 0, handlePos(tx.w(), cp)); });
  CHECK_MSG(contains(err, "не редактируются"), err);
  err = errorOf(s, [&](Tx& tx) { deletePoint(tx, cp); });
  CHECK_MSG(contains(err, "не редактируются"), err);
  Id border = 0;
  s.world().edges.each([&](const Edge& e) { if (!border && e.kind == EdgeKind::Border && !e.pts.empty()) border = e.id; });
  CHECK(border != 0);
  Handle bp{Handle::Point, 0, border, 0};
  err = errorOf(s, [&](Tx& tx) { moveHandle(tx, bp, {-50, 250}); });
  CHECK_MSG(contains(err, "за пределы карты"), err);
  err = errorOf(s, [&](Tx& tx) { moveHandle(tx, bp, {std::nan(""), 250}); });
  CHECK_MSG(contains(err, "Недопустимые координаты"), err);
  Vec2 v = s.world().edges.get(border)->pts[0];
  err = errorOf(s, [&](Tx& tx) { insertPoint(tx, border, 0, v); });
  CHECK_MSG(contains(err, "слишком близко"), err);
  err = errorOf(s, [&](Tx& tx) { insertPoint(tx, border, 99, v); });
  CHECK_MSG(contains(err, "Неверный номер"), err);
  Id frameNode = 0;
  s.world().nodes.each([&](const Node& n) { if (!frameNode && n.p == Vec2(0, 0)) frameNode = n.id; });
  CHECK(handleLocked(s.world(), Handle{Handle::Node, frameNode, 0, -1}));
  err = errorOf(s, [&](Tx& tx) { deletePoint(tx, Handle{Handle::Node, frameNode, 0, -1}); });
  CHECK_MSG(contains(err, "удалить нельзя"), err);
}

// Дополнительные случаи проверки графа: дубль дуги с промежуточными точками, нет рамки, внутренняя грань «вне карты».
TEST(geo_validate_more) {
  auto build = [](const std::function<void(Tx&)>& fn) {
    Store s;
    s.transact("b", [&](Tx& tx) { fn(tx); });
    return s.world();
  };
  auto frame = [](Tx& tx, Terrain inner) {
    Id a = tx.add(Node{0, {0, 0}}).id, b = tx.add(Node{0, {100, 0}}).id, c = tx.add(Node{0, {100, 100}}).id,
       d = tx.add(Node{0, {0, 100}}).id;
    Lbl in{0, inner}, out{0, Terrain::None};
    addEdge(tx, a, b, {}, EdgeKind::Frame, in, out);
    addEdge(tx, b, c, {}, EdgeKind::Frame, in, out);
    addEdge(tx, c, d, {}, EdgeKind::Frame, in, out);
    addEdge(tx, d, a, {}, EdgeKind::Frame, in, out);
  };
  // две одинаковые дуги с промежуточными точками (встречные)
  auto w1 = build([&](Tx& tx) {
    frame(tx, Terrain::Land);
    Id a = tx.add(Node{0, {20, 20}}).id, b = tx.add(Node{0, {60, 20}}).id;
    addEdge(tx, a, b, {{40, 40}, {50, 30}}, EdgeKind::Border, {}, {});
    addEdge(tx, b, a, {{50, 30}, {40, 40}}, EdgeKind::Border, {}, {});
  });
  CHECK(hasCode(validate(w1), "overlap"));
  // граф без рамки
  auto w2 = build([&](Tx& tx) { addRing(tx, circle({50, 50}, 20, 12), EdgeKind::Coast, {0, Terrain::Land}, {0, Terrain::Sea}); });
  CHECK(hasCode(validate(w2), "frame"));
  // внутренняя грань помечена «вне карты»
  auto w3 = build([&](Tx& tx) { frame(tx, Terrain::None); });
  CHECK(hasCode(validate(w3), "terrain"));
  // береговая дуга между сушей и сушей, рамка с двумя «внешними» сторонами
  auto w4 = build([&](Tx& tx) {
    frame(tx, Terrain::Land);
    addRing(tx, circle({50, 50}, 20, 12), EdgeKind::Coast, {0, Terrain::Land}, {0, Terrain::Land});
  });
  CHECK(hasCode(validate(w4), "terrain"));
  // пустой мир корректен
  CHECK(validate(World{}).empty());
  auto fs = buildFaces(World{});
  CHECK(fs->faces.empty());
  CHECK_EQ(fs->locate({1, 1}), -1);
  CHECK(fs->provincesOnPolyline({{0, 0}, {10, 10}}).empty());
  CHECK(fs->neighbors().empty());
}

// Запросы без транзакции устойчивы к нечисловому вводу и неверным ручкам: без исключений и NaN в ответах.
TEST(geo_queries_robust) {
  Store s = initStore();
  Id a = s.transact("a", [](Tx& tx) { return createProvince(tx, rect(400, 200, 500, 300), Terrain::Land); });
  const World& w = s.world();
  const double nan = std::nan("");
  const Vec2 bad{nan, 5}, inf{kInf, 5};
  auto fs = faces(w);
  for (Vec2 p : {bad, inf}) {
    CHECK_EQ(fs->locate(p), -1);
    CHECK_EQ(fs->provinceAt(p), Id(0));
    CHECK(fs->terrainAt(p) == Terrain::None);
    CHECK(!hitHandle(w, p, 5));
    CHECK(!hitEdge(w, p, 5).has_value());
  }
  CHECK(!hitHandle(w, {450, 200}, -1));
  CHECK(!hitHandle(w, {450, 200}, nan));
  CHECK(!hitEdge(w, {450, 200}, nan).has_value());
  // ломаная с нечисловой вершиной: учитываются только конечные звенья
  auto on = fs->provincesOnPolyline({{300, 250}, bad, {450, 250}, {470, 250}});
  CHECK_EQ(on.size(), size_t(1));
  CHECK(fs->provincesOnPolyline({}).empty());
  CHECK_EQ(fs->provincesOnPolyline({{450, 250}}).size(), size_t(1));
  // неверные ручки
  Handle none, ghostNode{Handle::Node, 999999, 0, -1}, ghostPt{Handle::Point, 0, 999999, 0};
  for (const Handle& h : {none, ghostNode, ghostPt}) {
    CHECK(handleLocked(w, h));
    CHECK(!canMove(w, h, {450, 250}));
    Vec2 p = handlePos(w, h);
    CHECK(finite(p));
  }
  Handle hb = hitHandle(w, {500, 300}, 2, a);
  CHECK(hb);
  CHECK(!canMove(w, hb, bad));
  CHECK(!canMove(w, hb, inf));
  CHECK(!canMove(w, hb, {-10, 250}));  // за рамкой
  Handle outOfRange{Handle::Point, 0, hitEdge(w, {450, 200}, 1, a)->edge, 1000};
  CHECK(handleLocked(w, outOfRange));
  CHECK(!isCoastJunction(w, 0));
  CHECK(!isCoastJunction(w, 999999));
  CHECK(!slideTarget(w, 999999, {1, 1}).has_value());
  // полюс недоступности и площади граней конечны
  for (auto& f : fs->faces) {
    CHECK(finite(f.label) && std::isfinite(f.area) && f.area > 0);
    CHECK(f.rings.size() == f.ringEdges.size());
  }
  for (auto& [id, sh] : fs->provinces) CHECK(finite(sh.label) && sh.area > 0 && !sh.faces.empty());
  CHECK(fs->shape(0) == nullptr);
  CHECK(fs->shape(999999) == nullptr);
}
