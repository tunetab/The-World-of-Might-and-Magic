// Фаззинг geo: тысячи случайных операций (контуры, нож, заливка, слияние, ручки со случайными сдвигами,
// скольжение по берегу, отмена/повтор) с фиксированными зёрнами. После каждой успешной операции граф корректен,
// грани покрывают карту, площадь суши неизменна, рельеф граней согласован с флагом sea провинции, а площади
// провинций меняются только так, как обещает операция (сохранение площади); отклонённая операция не меняет мир;
// отмена и повтор возвращают в точности прежние версии мира.
//
// Переменные окружения для расследований: GEO_FUZZ_SEEDS / GEO_FUZZ_STEPS / GEO_FUZZ_SEED0 — длина серии geo_fuzz_stress;
// GEO_FUZZ_REASONS — печать причин отказов (для отказов построения — и контура ввода);
// GEO_FUZZ_STOP=N — снимки geo_fuzz_before/after.png вокруг шага N и увеличенный снимок отказа построения.
#include <cstdlib>
#include <map>

#include "tests/test_geo_util.h"

using namespace rg;
using namespace rg::geo;

namespace {

constexpr double W = 1000, H = 600;
constexpr double kAreaEps = 1e-6;  // допуск площади «не изменилась» (единицы карты²)

enum Op { OpCreate, OpAdd, OpRemove, OpFill, OpSplit, OpMerge, OpUnassign, OpMove, OpInsert, OpDelete, OpSlide, OpUndo, OpCount };
const char* kOpNames[OpCount] = {"create", "add", "remove", "fill", "split", "merge", "unassign", "move", "insert", "delete", "slide", "undo"};

using Areas = std::map<Id, double>;

Areas areasOf(const World& w) {
  auto fs = buildFaces(w);
  Areas a;
  for (auto& [id, sh] : fs->provinces) a[id] = sh.area;
  return a;
}
double areaIn(const Areas& a, Id p) {
  auto it = a.find(p);
  return it == a.end() ? 0.0 : it->second;
}
// Провинции, площадь которых изменилась больше допуска.
std::vector<Id> changedProvinces(const Areas& a, const Areas& b) {
  std::vector<Id> out;
  std::vector<Id> ids;
  for (auto& [id, v] : a) ids.push_back(id);
  for (auto& [id, v] : b) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  for (Id id : ids)
    if (std::fabs(areaIn(a, id) - areaIn(b, id)) > kAreaEps) out.push_back(id);
  return out;
}

struct Fuzz {
  Store s;
  Rng rng;
  double land0 = 0;
  int ok[OpCount] = {}, rejected[OpCount] = {};
  std::map<std::string, int> reasons;  // причины отказов
  int steps = 0;
  std::vector<World> hist, fut;        // версии мира для проверки отмены/повтора

  explicit Fuzz(u64 seed) : rng(seed) {
    s.transact("init", [](Tx& tx) { initFromCoast(tx, geotest::sampleCoast(W, H)); });
    s.clearHistory();
    land0 = landArea(s.world());
    hist.push_back(s.world());
  }

  static double landArea(const World& w) {
    auto fs = buildFaces(w);
    double a = 0;
    for (auto& f : fs->faces)
      if (f.terrain == Terrain::Land) a += f.area;
    return a;
  }

  double u(double a, double b) { return a + (b - a) * rng.uniform(); }
  Vec2 pt() { return {u(-20, W + 20), u(-20, H + 20)}; }

  std::vector<Id> liveProvinces() {
    auto fs = buildFaces(s.world());
    std::vector<Id> r;
    for (auto& [id, sh] : fs->provinces) r.push_back(id);
    std::sort(r.begin(), r.end());
    return r;
  }

  std::vector<Vec2> vertices() {
    std::vector<Vec2> v;
    s.world().nodes.each([&](const Node& n) { v.push_back(n.p); });
    s.world().edges.each([&](const Edge& e) {
      if (e.kind == EdgeKind::Border) v.insert(v.end(), e.pts.begin(), e.pts.end());
    });
    return v;
  }

  std::vector<Vec2> last;  // последний сгенерированный контур (для отладки)
  std::vector<Vec2> poly() {
    last = polyGen();
    return last;
  }
  std::vector<Vec2> polyGen() {
    int kind = rng.range(0, 9);
    if (kind <= 4) {  // звёздный
      Vec2 c = pt();
      double R = u(8, 160);
      int n = rng.range(3, 10);
      double a0 = u(0, 2 * kPi);
      std::vector<Vec2> p;
      for (int i = 0; i < n; i++) {
        double t = a0 + 2 * kPi * i / n;
        double r = R * u(0.4, 1.0);
        p.push_back(c + Vec2(std::cos(t), std::sin(t)) * r);
      }
      return p;
    }
    if (kind <= 6) {  // прямоугольник
      Vec2 a = pt(), b = a + Vec2(u(5, 250), u(5, 250));
      return {a, {b.x, a.y}, b, {a.x, b.y}};
    }
    // через существующие вершины (с дрожанием в пределах допуска) — нагрузка на прилипание
    auto v = vertices();
    Vec2 c = v.empty() ? pt() : v[size_t(rng.range(0, int(v.size()) - 1))];
    double R = u(10, 120);
    std::vector<std::pair<double, Vec2>> cand;
    for (auto& q : v)
      if (dist(q, c) < R && dist(q, c) > 1e-9) cand.push_back({std::atan2(q.y - c.y, q.x - c.x), q});
    std::sort(cand.begin(), cand.end(), [](auto& x, auto& y) { return x.first < y.first; });
    std::vector<Vec2> p;
    for (size_t i = 0; i < cand.size() && p.size() < 8; i += 1 + size_t(rng.range(0, 2)))
      p.push_back(cand[i].second + Vec2(u(-0.8, 0.8), u(-0.8, 0.8)));
    if (p.size() < 3) {
      for (int i = 0; i < 4; i++) p.push_back(c + Vec2(std::cos(i * kPi / 2 + 0.3), std::sin(i * kPi / 2 + 0.3)) * R);
    }
    return p;
  }

  // Контур у провинции (звёздный, центр — в её габаритах с запасом): чаще задевает провинцию.
  std::vector<Vec2> polyNear(Id prov) {
    auto fs = buildFaces(s.world());
    auto* sh = fs->shape(prov);
    if (!sh || rng.uniform() < 0.3) return poly();
    Box2 b = sh->box.inflated(20);
    Vec2 c{u(b.x0, b.x1), u(b.y0, b.y1)};
    double R = u(6, std::max(b.w(), b.h()) * 0.5 + 10);
    int n = rng.range(3, 9);
    double a0 = u(0, 2 * kPi);
    std::vector<Vec2> p;
    for (int i = 0; i < n; i++) {
      double t = a0 + 2 * kPi * i / n;
      p.push_back(c + Vec2(std::cos(t), std::sin(t)) * (R * u(0.4, 1.0)));
    }
    last = p;
    return p;
  }

  std::vector<Vec2> knife(Id prov) {
    auto fs = buildFaces(s.world());
    auto* sh = fs->shape(prov);
    Box2 b = sh ? sh->box.inflated(15) : Box2(0, 0, W, H);
    Vec2 a, c;
    if (rng.uniform() < 0.5) {
      a = {b.x0, u(b.y0, b.y1)};
      c = {b.x1, u(b.y0, b.y1)};
    } else {
      a = {u(b.x0, b.x1), b.y0};
      c = {u(b.x0, b.x1), b.y1};
    }
    std::vector<Vec2> k{a};
    int mid = rng.range(0, 2);
    for (int i = 1; i <= mid; i++) k.push_back(a + (c - a) * (double(i) / (mid + 1)) + Vec2(u(-40, 40), u(-40, 40)));
    k.push_back(c);
    return k;
  }

  Handle handle() {
    std::vector<Handle> hs;
    s.world().edges.each([&](const Edge& e) {
      if (e.kind != EdgeKind::Border) return;
      for (int i = 0; i < int(e.pts.size()); i++) hs.push_back(Handle{Handle::Point, 0, e.id, i});
      hs.push_back(Handle{Handle::Node, e.a, 0, -1});
    });
    if (hs.empty()) return {};
    return hs[size_t(rng.range(0, int(hs.size()) - 1))];
  }

  void check(const char* what) {
    const World& w = s.world();
    auto is = validate(w);
    CHECK_MSG(is.empty(), std::string(what) + " #" + std::to_string(steps) + ": " + geotest::issuesText(is));
    auto fs = buildFaces(w);
    CHECK_NEAR(geotest::faceAreaSum(*fs), W * H, 1e-6 * W * H);
    double land = 0;
    for (auto& f : fs->faces) {
      if (f.terrain == Terrain::Land) land += f.area;
      // рельеф грани согласован с типом провинции
      if (f.province == 0) continue;
      const Province* p = w.province(f.province);
      CHECK_MSG(p != nullptr, std::string(what) + " #" + std::to_string(steps) + ": грань ссылается на несуществующую провинцию");
      if (p) CHECK_MSG(p->sea == (f.terrain == Terrain::Sea), std::string(what) + " #" + std::to_string(steps) + ": рельеф грани не совпадает с sea");
    }
    CHECK_NEAR(land, land0, 1e-6 * land0);
  }

  // Успешная операция, изменившая мир, становится новой версией истории.
  void committed(const World& before) {
    if (World::diff(before, s.world()) == 0) return;
    hist.push_back(s.world());
    fut.clear();
  }

  template <class F> bool run(Op op, F&& fn) {
    World before = s.world();
    try {
      s.transact(kOpNames[op], fn);
    } catch (const UserError& e) {
      reasons[e.what()]++;
      if (std::getenv("GEO_FUZZ_REASONS") && std::string(e.what()).find("построить") != std::string::npos) {
        std::printf("    step %d %s: %s\n     poly:", steps, kOpNames[op], e.what());
        for (auto& q : last) std::printf(" {%.17g, %.17g},", q.x, q.y);
        if (std::getenv("GEO_FUZZ_STOP") && steps == std::atoi(std::getenv("GEO_FUZZ_STOP"))) {
          Box2 v = bounds(last).inflated(15);
          geotest::renderWorld(s.world(), "geo_fuzz_zoom.png", 600.0 / std::max(v.w(), v.h()), v, last);
        }
        std::printf("\n");
      }
      rejected[op]++;
      CHECK_EQ(World::diff(before, s.world()), 0u);
      return false;
    }
    ok[op]++;
    committed(before);
    check(kOpNames[op]);
    return true;
  }

  // Площади провинций после операции: допустимые изменения.
  void expectOnly(const char* what, const Areas& a0, const Areas& a1, std::initializer_list<Id> allowed) {
    for (Id id : changedProvinces(a0, a1)) {
      bool ok2 = std::find(allowed.begin(), allowed.end(), id) != allowed.end();
      CHECK_MSG(ok2, std::string(what) + " #" + std::to_string(steps) + ": изменилась площадь посторонней провинции " + std::to_string(id));
    }
  }
  // Провинции, отличные от p: площадь не растёт; провинции другого рельефа не меняются.
  void expectShrinkOthers(const char* what, const Areas& a0, const Areas& a1, Id p, bool sea) {
    for (Id id : changedProvinces(a0, a1)) {
      if (id == p) continue;
      CHECK_MSG(areaIn(a1, id) <= areaIn(a0, id) + kAreaEps, std::string(what) + " #" + std::to_string(steps) + ": выросла провинция " + std::to_string(id));
      const Province* rec = s.world().province(id);
      if (rec) CHECK_MSG(rec->sea == sea, std::string(what) + " #" + std::to_string(steps) + ": задета провинция другого рельефа");
    }
  }

  void step() {
    steps++;
    int r = rng.range(0, 99);
    auto provs = liveProvinces();
    auto anyProv = [&]() -> Id { return provs.empty() ? Id(0) : provs[size_t(rng.range(0, int(provs.size()) - 1))]; };
    const Areas a0 = areasOf(s.world());
    if (r < 22) {
      auto p = poly();
      Terrain t = rng.uniform() < 0.3 ? Terrain::None : (rng.uniform() < 0.75 ? Terrain::Land : Terrain::Sea);
      EditOptions opt;
      opt.snap = rng.uniform() < 0.3 ? u(0.2, 6) : 1.0;
      Id pid = 0;
      if (!run(OpCreate, [&](Tx& tx) { pid = createProvince(tx, p, t, opt); })) return;
      Areas a1 = areasOf(s.world());
      const Province* rec = s.world().province(pid);
      CHECK(rec != nullptr);
      if (!rec) return;
      if (t != Terrain::None) CHECK_EQ(rec->sea, t == Terrain::Sea);
      CHECK(areaIn(a0, pid) == 0 && areaIn(a1, pid) > 0);
      expectShrinkOthers("create", a0, a1, pid, rec->sea);
      double lost = 0;
      for (auto& [id, v] : a0) lost += v - areaIn(a1, id);
      CHECK(lost <= areaIn(a1, pid) + kAreaEps);
    } else if (r < 32) {
      Id id = anyProv();
      auto p = polyNear(id);
      if (!run(OpAdd, [&](Tx& tx) { addArea(tx, id, p); })) return;
      Areas a1 = areasOf(s.world());
      CHECK(areaIn(a1, id) > areaIn(a0, id));
      expectShrinkOthers("add", a0, a1, id, s.world().province(id)->sea);
    } else if (r < 40) {
      Id id = anyProv();
      auto p = polyNear(id);
      if (!run(OpRemove, [&](Tx& tx) { removeArea(tx, id, p); })) return;
      Areas a1 = areasOf(s.world());
      CHECK(areaIn(a1, id) < areaIn(a0, id));
      expectOnly("remove", a0, a1, {id});
    } else if (r < 46) {
      Vec2 p = pt();
      Id id = rng.uniform() < 0.5 ? Id(0) : anyProv();
      Id pid = 0;
      Id prev = buildFaces(s.world())->provinceAt(p);
      if (!run(OpFill, [&](Tx& tx) { pid = fillAt(tx, p, id); })) return;
      if (id) CHECK_EQ(pid, id);
      Areas a1 = areasOf(s.world());
      expectOnly("fill", a0, a1, {pid, prev});
      CHECK_EQ(buildFaces(s.world())->provinceAt(p), pid);
    } else if (r < 56) {
      Id id = anyProv();
      if (!id) return;
      auto k = knife(id);
      Id nid = 0;
      if (!run(OpSplit, [&](Tx& tx) { nid = split(tx, id, k); })) return;
      Areas a1 = areasOf(s.world());
      CHECK(areaIn(a0, nid) == 0);
      CHECK_NEAR(areaIn(a1, id) + areaIn(a1, nid), areaIn(a0, id), kAreaEps + 1e-9 * areaIn(a0, id));
      CHECK(areaIn(a1, nid) > 0 && areaIn(a1, nid) <= areaIn(a1, id) + kAreaEps);
      CHECK_EQ(s.world().province(nid)->sea, s.world().province(id)->sea);
      expectOnly("split", a0, a1, {id, nid});
    } else if (r < 60) {
      Id a = anyProv(), b = anyProv();
      if (!a || !b) return;
      if (!run(OpMerge, [&](Tx& tx) { merge(tx, a, b); })) return;
      Areas a1 = areasOf(s.world());
      CHECK_NEAR(areaIn(a1, a), areaIn(a0, a) + areaIn(a0, b), kAreaEps + 1e-9 * areaIn(a1, a));
      CHECK_EQ(areaIn(a1, b), 0.0);
      expectOnly("merge", a0, a1, {a, b});
    } else if (r < 63) {
      Id a = anyProv();
      if (!run(OpUnassign, [&](Tx& tx) { unassign(tx, a); })) return;
      Areas a1 = areasOf(s.world());
      CHECK_EQ(areaIn(a1, a), 0.0);
      expectOnly("unassign", a0, a1, {a});
    } else if (r < 80) {
      Handle h = handle();
      if (!h) return;
      Vec2 p0 = handlePos(s.world(), h);
      double scale = rng.uniform() < 0.7 ? 4 : 80;
      Vec2 to = p0 + Vec2(u(-scale, scale), u(-scale, scale));
      bool can = canMove(s.world(), h, to);
      World before = s.world();
      try {
        s.transact("move", [&](Tx& tx) { moveHandle(tx, h, to); });
        CHECK(can);
        ok[OpMove]++;
        committed(before);
        CHECK(handlePos(s.world(), h) == to);
        check("move");
        // набор провинций с территорией не меняется (грани не исчезают и не появляются)
        Areas a1 = areasOf(s.world());
        CHECK_EQ(a1.size(), a0.size());
      } catch (const UserError&) {
        CHECK(!can);
        rejected[OpMove]++;
        CHECK_EQ(World::diff(before, s.world()), 0u);
      }
    } else if (r < 86) {
      auto hit = hitEdge(s.world(), pt(), 30);
      if (!hit) return;
      const Edge* e = s.world().edges.get(hit->edge);
      Vec2 p = hit->p;
      Handle nh;
      if (!run(OpInsert, [&](Tx& tx) { nh = insertPoint(tx, e->id, hit->segment, p); })) return;
      CHECK(nh.kind == Handle::Point);
      CHECK(dist(handlePos(s.world(), nh), p) < 1e-9);
      expectOnly("insert", a0, areasOf(s.world()), {});
    } else if (r < 91) {
      Handle h = handle();
      if (!h) return;
      if (!run(OpDelete, [&](Tx& tx) { deletePoint(tx, h); })) return;
      CHECK_EQ(areasOf(s.world()).size(), a0.size());
    } else if (r < 97) {
      std::vector<Id> js;
      s.world().nodes.each([&](const Node& n) { if (isCoastJunction(s.world(), n.id)) js.push_back(n.id); });
      if (js.empty()) return;
      Id j = js[size_t(rng.range(0, int(js.size()) - 1))];
      Vec2 to = s.world().nodes.get(j)->p + Vec2(u(-30, 30), u(-30, 30));
      auto t = slideTarget(s.world(), j, to);
      World before = s.world();
      try {
        s.transact("slide", [&](Tx& tx) { slideJunction(tx, j, to); });
        CHECK(t.has_value());
        ok[OpSlide]++;
        committed(before);
        CHECK(s.world().nodes.get(j)->p == *t);
        CHECK(isCoastJunction(s.world(), j));
        check("slide");
        CHECK_EQ(areasOf(s.world()).size(), a0.size());
      } catch (const UserError&) {
        CHECK(!t.has_value());
        rejected[OpSlide]++;
        CHECK_EQ(World::diff(before, s.world()), 0u);
      }
    } else {
      // отмена/повтор возвращают в точности сохранённые версии
      int n = rng.range(1, 4);
      for (int i = 0; i < n; i++) {
        if (!s.undo()) break;
        CHECK(hist.size() > 1);
        if (hist.size() <= 1) break;
        fut.push_back(hist.back());
        hist.pop_back();
        CHECK_EQ(World::diff(s.world(), hist.back()), 0u);
      }
      CHECK(!s.world().edges.empty());
      check("undo");
      if (rng.uniform() < 0.5) {
        for (int i = 0; i < n; i++) {
          if (!s.redo()) break;
          CHECK(!fut.empty());
          if (fut.empty()) break;
          hist.push_back(fut.back());
          fut.pop_back();
          CHECK_EQ(World::diff(s.world(), hist.back()), 0u);
        }
        check("redo");
      }
      CHECK_EQ(s.canRedo(), !fut.empty());
      ok[OpUndo]++;
    }
  }

  std::string report() const {
    std::string r;
    for (int i = 0; i < OpCount; i++) r += std::string(kOpNames[i]) + " " + std::to_string(ok[i]) + "/" + std::to_string(ok[i] + rejected[i]) + "  ";
    return r;
  }
};

void fuzzSeed(u64 seed, int n, const char* png) {
  Fuzz f(seed);
  const char* stop = std::getenv("GEO_FUZZ_STOP");
  int stopAt = stop ? std::atoi(stop) : -1;
  for (int i = 0; i < n; i++) {
    if (i + 1 == stopAt) {
      geotest::renderWorld(f.s.world(), "geo_fuzz_before.png", 1.0, Box2(0, 0, W, H));
      f.s.markSaved();
    }
    f.step();
    if (i + 1 == stopAt) {
      geotest::renderWorld(f.s.world(), "geo_fuzz_after.png", 1.0, Box2(0, 0, W, H));
      return;
    }
  }
  std::printf("  fuzz %llu: %s\n", (unsigned long long)seed, f.report().c_str());
  if (std::getenv("GEO_FUZZ_REASONS"))
    for (auto& [m, c] : f.reasons) std::printf("    %5d  %s\n", c, m.c_str());
  // каждая операция должна реально выполняться
  CHECK(f.ok[OpCreate] > n / 25);
  CHECK(f.ok[OpSplit] > 0);
  CHECK(f.ok[OpMove] > n / 40);
  CHECK(f.ok[OpAdd] + f.ok[OpRemove] > n / 40);
  geotest::renderWorld(f.s.world(), png, 1.0, Box2(0, 0, W, H));
}

}  // namespace

TEST(geo_fuzz_seed_1) { fuzzSeed(1, 700, "geo_fuzz_1.png"); }
TEST(geo_fuzz_seed_2) { fuzzSeed(2, 700, "geo_fuzz_2.png"); }
TEST(geo_fuzz_seed_3) { fuzzSeed(20261001, 700, "geo_fuzz_3.png"); }

// Длинная серия: GEO_FUZZ_SEEDS зёрен по GEO_FUZZ_STEPS операций (по умолчанию 2 × 400).
TEST(geo_fuzz_stress) {
  const char* ns = std::getenv("GEO_FUZZ_SEEDS");
  const char* st = std::getenv("GEO_FUZZ_STEPS");
  int seeds = ns ? std::max(1, std::atoi(ns)) : 2;
  int steps = st ? std::max(1, std::atoi(st)) : 400;
  const char* s0 = std::getenv("GEO_FUZZ_SEED0");
  int first = s0 ? std::atoi(s0) : 1000;
  for (int k = 0; k < seeds; k++) fuzzSeed(u64(first + k), steps, "geo_fuzz_stress.png");
}
