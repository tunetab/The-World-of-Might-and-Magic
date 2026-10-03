// Regnum — грани плоского графа провинций, поиск по точке, кеш, проверка целостности.
#include "geo/topo.h"

#include <cstring>
#include <mutex>
#include <unordered_map>

#include "base/jobs.h"
#include "geo/graph.h"

namespace rg::geo {

struct FaceSet::Index {
  detail::Locator loc;
  std::vector<std::pair<Id, Id>> neighbors;
};

namespace {

// Вход построителя граней прямо из таблиц мира (без копирования точек дуг).
struct WorldInput {
  detail::FaceInput in;
  std::vector<const Edge*> edges;
};

WorldInput inputFrom(const World& w) {
  WorldInput wi;
  std::unordered_map<Id, int> idx;
  idx.reserve(size_t(w.nodes.size()) * 2 + 1);
  wi.in.pos.reserve(w.nodes.size());
  w.nodes.each([&](const Node& n) {
    idx[n.id] = int(wi.in.pos.size());
    wi.in.pos.push_back(n.p);
  });
  wi.in.edges.reserve(w.edges.size());
  wi.edges.reserve(w.edges.size());
  w.edges.each([&](const Edge& e) {
    detail::FaceInput::E x;
    x.pts = &e.pts;
    auto ia = idx.find(e.a), ib = idx.find(e.b);
    bool fin = true;
    for (auto& p : e.pts) fin = fin && finite(p);
    if (ia != idx.end() && ib != idx.end() && fin && finite(wi.in.pos[size_t(ia->second)]) &&
        finite(wi.in.pos[size_t(ib->second)])) {
      x.a = ia->second;
      x.b = ib->second;
    }
    wi.in.edges.push_back(x);
    wi.edges.push_back(&e);
  });
  return wi;
}

}  // namespace

std::vector<Vec2> edgeCoords(const World& w, const Edge& e) {
  std::vector<Vec2> c;
  c.reserve(e.pts.size() + 2);
  if (const Node* a = w.nodes.get(e.a)) c.push_back(a->p);
  c.insert(c.end(), e.pts.begin(), e.pts.end());
  if (const Node* b = w.nodes.get(e.b)) c.push_back(b->p);
  return c;
}

std::shared_ptr<const FaceSet> buildFaces(const World& w) {
  WorldInput wi = inputFrom(w);
  detail::FaceBuild fb = detail::buildFaceCore(wi.in);
  auto fs = std::make_shared<FaceSet>();
  const int nf = fb.nfaces();
  fs->faces.resize(size_t(nf));
  auto sideOfHalf = [&](int h) { return detail::sideOf(*wi.edges[size_t(h >> 1)], (h & 1) == 0); };
  for (int f = 0; f < nf; f++) {
    Face& F = fs->faces[size_t(f)];
    detail::Side s = detail::faceSide(fb, f, sideOfHalf);
    F.province = s.prov;
    F.terrain = s.ter;
    F.area = fb.faceArea[size_t(f)];
    F.box = fb.faceBox[size_t(f)];
    auto addRing = [&](int c) {
      F.rings.push_back(fb.cycleCoords(wi.in, c));
      std::vector<HalfEdge> he;
      for (int k = fb.cycOff[size_t(c)]; k < fb.cycOff[size_t(c) + 1]; k++) {
        int h = fb.cycHalf[size_t(k)];
        he.push_back(HalfEdge{wi.edges[size_t(h >> 1)]->id, (h & 1) == 0});
      }
      F.ringEdges.push_back(std::move(he));
    };
    addRing(fb.faceOuter[size_t(f)]);
    for (int c : fb.faceHoles[size_t(f)]) addRing(c);
  }
  // Точки подписей: полюс недоступности с точностью ~1 (мельче для маленьких граней). Грани независимы.
  jobs::parallelFor(size_t(nf), [&](size_t f) {
    Face& F = fs->faces[f];
    double minDim = std::min(F.box.w(), F.box.h());
    double prec = clamp(minDim / 50.0, 1e-3, 1.0);
    F.label = polylabel(F.rings, prec);
  }, 8);
  for (int f = 0; f < nf; f++) {
    const Face& F = fs->faces[size_t(f)];
    fs->bounds.add(F.box);
    if (F.province == 0) continue;
    ProvinceShape& ps = fs->provinces[F.province];
    ps.faces.push_back(f);
    ps.area += F.area;
    ps.box.add(F.box);
  }
  for (auto& [id, ps] : fs->provinces) {
    int big = ps.faces[0];
    for (int f : ps.faces)
      if (fs->faces[size_t(f)].area > fs->faces[size_t(big)].area) big = f;
    ps.label = fs->faces[size_t(big)].label;
  }
  auto idx = std::make_shared<FaceSet::Index>();
  for (int e = 0; e < fb.ne; e++) {
    if (fb.halfCycle[size_t(2 * e)] < 0) continue;
    int fl = fb.faceOfHalf(2 * e), fr = fb.faceOfHalf(2 * e + 1);
    if (fl < 0 || fr < 0) continue;
    Id pa = fs->faces[size_t(fl)].province, pb = fs->faces[size_t(fr)].province;
    if (pa == 0 || pb == 0 || pa == pb) continue;
    idx->neighbors.push_back({std::min(pa, pb), std::max(pa, pb)});
  }
  std::sort(idx->neighbors.begin(), idx->neighbors.end());
  idx->neighbors.erase(std::unique(idx->neighbors.begin(), idx->neighbors.end()), idx->neighbors.end());
  idx->loc = std::move(fb.loc);
  fs->index = std::move(idx);
  return fs;
}

// ---------------------------------------------------------------- FaceSet
int FaceSet::locate(Vec2 p) const { return index ? index->loc.locate(p) : -1; }

Id FaceSet::provinceAt(Vec2 p) const {
  int f = locate(p);
  return f < 0 ? 0 : faces[size_t(f)].province;
}

Terrain FaceSet::terrainAt(Vec2 p) const {
  int f = locate(p);
  return f < 0 ? Terrain::None : faces[size_t(f)].terrain;
}

const ProvinceShape* FaceSet::shape(Id province) const {
  auto it = provinces.find(province);
  return it == provinces.end() ? nullptr : &it->second;
}

std::vector<Id> FaceSet::provincesOnPolyline(const std::vector<Vec2>& line) const {
  std::vector<Id> out;
  auto add = [&](Id p) {
    if (p != 0 && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
  };
  if (line.empty() || !index) return out;
  add(provinceAt(line[0]));
  const SegGrid& g = index->loc.grid;
  std::vector<u32> stamp;
  u32 tag = 0;
  for (size_t i = 0; i + 1 < line.size(); i++) {
    Vec2 p = line[i], q = line[i + 1];
    if (!finite(p) || !finite(q)) continue;
    std::vector<double> ts{0.0, 1.0};
    Box2 bb;
    bb.add(p);
    bb.add(q);
    g.queryUnique(bb, stamp, ++tag, [&](int s) {
      Vec2 a = g.a(s), b = g.b(s);
      SegRel r = segRelation(p, q, a, b);
      if (r == SegRel::None) return;
      if (orient(p, q, a) == 0 && orient(p, q, b) == 0) {  // на одной прямой: границы общей части
        ts.push_back(project(a, p, q).t);
        ts.push_back(project(b, p, q).t);
        return;
      }
      double t = 0;
      crossPoint(p, q, a, b, &t);
      ts.push_back(t);
    });
    std::sort(ts.begin(), ts.end());
    for (size_t k = 0; k + 1 < ts.size(); k++) {
      if (ts[k + 1] - ts[k] <= 0) continue;
      add(provinceAt(p + (q - p) * ((ts[k] + ts[k + 1]) * 0.5)));
    }
    add(provinceAt(q));
  }
  return out;
}

std::vector<std::pair<Id, Id>> FaceSet::neighbors() const { return index ? index->neighbors : std::vector<std::pair<Id, Id>>{}; }

// ---------------------------------------------------------------- кеш
// Ключ — тождество блоков таблиц nodes/edges. Внутри транзакции блоки, уже скопированные ею, меняются на месте
// (тождество сохраняется), поэтому запись кеша дополнительно сверяется с числом узлов и дуг: обращение к faces(tx.w())
// посреди транзакции, после которого граф ещё менялся, не подменит грани зафиксированного мира, если изменилось
// число узлов или дуг (так меняют граф все операции, кроме перемещения ручек). В отладочной сборке сверяется
// и отпечаток содержимого.
namespace {
struct CacheEntry {
  Table<Node> nodes;   // копии таблиц удерживают блоки: адрес не может быть переиспользован
  Table<Edge> edges;
  u32 nn = 0, ne = 0;
  u64 print = 0;       // отпечаток содержимого (только RG_DEBUG)
  std::shared_ptr<const FaceSet> fs;
};
std::mutex gCacheMu;
std::vector<CacheEntry> gCache;  // последние построения, свежие — в начале
constexpr size_t kCacheSize = 4;

#ifdef RG_DEBUG
constexpr bool kCheckPrint = true;
#else
constexpr bool kCheckPrint = false;
#endif

u64 mixD(u64 h, double v) {
  u64 b;
  std::memcpy(&b, &v, sizeof b);
  return hashMix(h, b);
}

// Отпечаток геометрии и меток графа.
u64 fingerprint(const World& w) {
  u64 h = 0x5eed;
  w.nodes.each([&](const Node& n) { h = mixD(mixD(hashMix(h, n.id), n.p.x), n.p.y); });
  w.edges.each([&](const Edge& e) {
    h = hashMix(hashMix(hashMix(h, e.id), (u64(e.a) << 32) | e.b), (u64(e.pl) << 32) | e.pr);
    h = hashMix(h, u64(e.kind) | (u64(e.tl) << 8) | (u64(e.tr) << 16) | (u64(e.pts.size()) << 24));
    for (auto& p : e.pts) h = mixD(mixD(h, p.x), p.y);
  });
  return h;
}

bool matches(const CacheEntry& e, const World& w) {
  return e.nodes.same(w.nodes) && e.edges.same(w.edges) && e.nn == w.nodes.size() && e.ne == w.edges.size();
}
}  // namespace

std::shared_ptr<const FaceSet> faces(const World& w) {
  const u64 print = kCheckPrint ? fingerprint(w) : 0;
  {
    std::lock_guard<std::mutex> lk(gCacheMu);
    for (size_t i = 0; i < gCache.size(); i++) {
      if (!matches(gCache[i], w)) continue;
      if (kCheckPrint && gCache[i].print != print) {
        logWarn("geo::faces: граф изменён на месте после кеширования (faces(tx.w()) внутри транзакции) — грани перестроены");
        gCache.erase(gCache.begin() + long(i));
        break;
      }
      auto fs = gCache[i].fs;
      if (i > 0) std::rotate(gCache.begin(), gCache.begin() + long(i), gCache.begin() + long(i) + 1);
      return fs;
    }
  }
  auto fs = buildFaces(w);
  std::lock_guard<std::mutex> lk(gCacheMu);
  for (auto& e : gCache)  // другой поток успел построить то же самое
    if (matches(e, w) && e.print == print) return e.fs;
  // Устаревшие записи с тем же тождеством таблиц (граф менялся на месте) заменяются.
  gCache.erase(std::remove_if(gCache.begin(), gCache.end(),
                              [&](const CacheEntry& e) { return e.nodes.same(w.nodes) && e.edges.same(w.edges); }),
               gCache.end());
  gCache.insert(gCache.begin(), CacheEntry{w.nodes, w.edges, w.nodes.size(), w.edges.size(), print, fs});
  if (gCache.size() > kCacheSize) gCache.resize(kCacheSize);
  return fs;
}

std::vector<Issue> validate(const World& w) {
  detail::Graph g;
  g.load(w);
  return detail::checkGraph(g, [&](Id p) { return w.provinces.has(p); });
}

}  // namespace rg::geo
