// Regnum — составные тайлы карты: белая суша → заливка провинций → море → реки и озёра → заливка сухопутных
// провинций на морской части → штриховка оккупации → границы провинций → границы государств (внутренняя обводка)
// → символы базовой карты.
#include <future>
#include <mutex>

#include "gfx/stroke.h"
#include "map/map_internal.h"

namespace rg::map::detail {

// ================================================================ индекс дуг
void GeoIndex::edgesIn(const Box2& b, std::vector<u32>& out) const {
  out.clear();
  if (cols <= 0 || rows <= 0 || b.empty()) return;
  const int c0 = clamp(int(std::floor(b.x0 / cell)), 0, cols - 1), c1 = clamp(int(std::floor(b.x1 / cell)), 0, cols - 1);
  const int r0 = clamp(int(std::floor(b.y0 / cell)), 0, rows - 1), r1 = clamp(int(std::floor(b.y1 / cell)), 0, rows - 1);
  for (int r = r0; r <= r1; r++)
    for (int c = c0; c <= c1; c++)
      for (u32 i : grid[size_t(r) * size_t(cols) + size_t(c)])
        if (edges[i].box.intersects(b)) out.push_back(i);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
}

namespace {

std::shared_ptr<GeoIndex> buildIndex(const World& w) {
  auto g = std::make_shared<GeoIndex>();
  g->faces = geo::faces(w);
  g->edges.reserve(w.edges.size());
  w.edges.each([&](const Edge& e) {
    EdgeGeo eg;
    eg.id = e.id;
    eg.kind = e.kind;
    eg.pl = e.pl;
    eg.pr = e.pr;
    eg.tl = e.tl;
    eg.tr = e.tr;
    eg.pts = geo::edgeCoords(w, e);
    for (Vec2 p : eg.pts) eg.box.add(p);
    if (eg.pts.size() < 2) return;
    const u32 idx = u32(g->edges.size());
    if (e.pl) g->provEdges[e.pl].push_back(idx);
    if (e.pr && e.pr != e.pl) g->provEdges[e.pr].push_back(idx);
    g->edges.push_back(std::move(eg));
  });
  Box2 b = g->faces ? g->faces->bounds : Box2();
  for (const EdgeGeo& e : g->edges) b.add(e.box);
  if (b.empty()) b = Box2(0, 0, schema::kMapWidth, schema::kMapHeight);
  g->cell = 256;
  g->cols = std::max(1, int(std::ceil(std::max(1.0, b.x1) / g->cell)));
  g->rows = std::max(1, int(std::ceil(std::max(1.0, b.y1) / g->cell)));
  g->grid.assign(size_t(g->cols) * size_t(g->rows), {});
  for (u32 i = 0; i < g->edges.size(); i++) {
    const EdgeGeo& e = g->edges[i];
    // Длинные дуги (берег острова) — по ячейкам отрезков, а не по всему габариту.
    for (size_t k = 0; k + 1 < e.pts.size(); k++) {
      Box2 sb;
      sb.add(e.pts[k]);
      sb.add(e.pts[k + 1]);
      const int c0 = clamp(int(std::floor(sb.x0 / g->cell)), 0, g->cols - 1), c1 = clamp(int(std::floor(sb.x1 / g->cell)), 0, g->cols - 1);
      const int r0 = clamp(int(std::floor(sb.y0 / g->cell)), 0, g->rows - 1), r1 = clamp(int(std::floor(sb.y1 / g->cell)), 0, g->rows - 1);
      for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) {
          auto& cellList = g->grid[size_t(r) * size_t(g->cols) + size_t(c)];
          if (cellList.empty() || cellList.back() != i) cellList.push_back(i);
        }
    }
  }
  return g;
}

// Запись кеша: индекс строится один раз, остальные потоки ждут его (а не строят те же грани параллельно).
struct IndexEntry {
  Table<Node> nodes;
  Table<Edge> edges;
  std::shared_future<std::shared_ptr<const GeoIndex>> idx;
};
std::mutex gIndexMu;
std::vector<IndexEntry> gIndex;

}  // namespace

std::shared_ptr<const GeoIndex> geoIndex(const World& w) {
  std::promise<std::shared_ptr<const GeoIndex>> made;
  {
    std::lock_guard<std::mutex> lk(gIndexMu);
    for (size_t i = 0; i < gIndex.size(); i++)
      if (gIndex[i].nodes.same(w.nodes) && gIndex[i].edges.same(w.edges)) {
        auto f = gIndex[i].idx;
        if (i > 0) std::rotate(gIndex.begin(), gIndex.begin() + long(i), gIndex.begin() + long(i) + 1);
        return f.get();
      }
    gIndex.insert(gIndex.begin(), IndexEntry{w.nodes, w.edges, made.get_future().share()});
    if (gIndex.size() > 4) gIndex.resize(4);
  }
  try {
    std::shared_ptr<const GeoIndex> idx = buildIndex(w);
    made.set_value(idx);
    return idx;
  } catch (...) {
    // Ожидающие получат то же исключение; запись убирается, следующий вызов построит заново.
    made.set_exception(std::current_exception());
    std::lock_guard<std::mutex> lk(gIndexMu);
    gIndex.erase(std::remove_if(gIndex.begin(), gIndex.end(), [&](const IndexEntry& e) { return e.nodes.same(w.nodes) && e.edges.same(w.edges); }),
                 gIndex.end());
    throw;
  }
}

// ================================================================ тайл
namespace {

struct TileXf {
  double ds, ox, oy;
  gfx::Pt operator()(Vec2 m) const { return {float(m.x * ds - ox), float(m.y * ds - oy)}; }
};

void addRings(const geo::FaceSet& fs, const geo::ProvinceShape& sh, const TileXf& P, gfx::Path& path) {
  for (int fi : sh.faces) {
    const geo::Face& f = fs.faces[size_t(fi)];
    for (const auto& ring : f.rings) {
      if (ring.size() < 3) continue;
      path.moveTo(P(ring[0]).x, P(ring[0]).y);
      for (size_t i = 1; i < ring.size(); i++) {
        const gfx::Pt q = P(ring[i]);
        path.lineTo(q.x, q.y);
      }
      path.close();
    }
  }
}

void addPolyline(const Vec2* p, size_t n, const TileXf& P, gfx::Path& path) {
  if (n < 2) return;
  const gfx::Pt a = P(p[0]);
  path.moveTo(a.x, a.y);
  for (size_t i = 1; i < n; i++) {
    const gfx::Pt q = P(p[i]);
    path.lineTo(q.x, q.y);
  }
}

// Открытые ломаные пути — для обводки (strokePath замыкает только close()).
void addEdge(const EdgeGeo& e, const Box2& box, const TileXf& P, gfx::Path& path) {
  clipPolyline(e.pts, box, [&](const Vec2* p, size_t n) { addPolyline(p, n, P, path); });
}

gfx::Stroke strokeOf(float w) {
  gfx::Stroke s;
  s.width = w;
  s.join = gfx::Join::Round;
  s.cap = gfx::Cap::Round;
  return s;
}

const ProvLook* lookOf(const Looks& L, Id p) {
  auto it = L.prov.find(p);
  return it == L.prov.end() ? nullptr : &it->second;
}
Id stateOf(const Looks& L, Id p) {
  const ProvLook* l = p ? lookOf(L, p) : nullptr;
  return l && l->land ? l->state : 0;
}

}  // namespace

gfx::Image renderTile(const TileScene& sc, const GeoIndex& g, const Basemap* bm, RasterCache* rc, double ds, int tx, int ty) {
  const int T = kTile;
  const Looks& L = *sc.looks;
  const float dpi = sc.style.dpi;
  const TileXf P{ds, double(tx) * T, double(ty) * T};
  const Box2 tb(P.ox / ds, P.oy / ds, (P.ox + T) / ds, (P.oy + T) / ds);
  const Color land = bm && bm->loaded() ? bm->landColor() : Color(255, 255, 255);
  gfx::Image img(T, T, gfx::premul(land));
  const geo::FaceSet* fs = g.faces.get();
  const double coastGrow = std::max(1.5, 0.75 / ds);   // заливка под мягкий край моря, единицы карты
  const Box2 qb = tb.inflated(coastGrow + 12.0 / ds);
  std::vector<u32> edgeIdx;
  g.edgesIn(qb, edgeIdx);

  // Провинции тайла по возрастанию ID (детерминированный порядок).
  std::vector<Id> provs;
  if (fs)
    for (const auto& [pid, sh] : fs->provinces)
      if (pid && sh.box.intersects(qb)) provs.push_back(pid);
  std::sort(provs.begin(), provs.end());

  // 1. Заливка: непрозрачно в отдельный слой, затем наложение с прозрачностью режима.
  bool anyFill = false;
  for (Id pid : provs)
    if (const ProvLook* l = lookOf(L, pid); l && l->fill.a) anyFill = true;
  if (anyFill && L.fillAlpha > 0) {
    gfx::Image fill(T, T, 0);
    gfx::Canvas fc(fill);
    gfx::Path path, coastPath, borderPath;
    for (Id pid : provs) {
      const ProvLook* l = lookOf(L, pid);
      if (!l || !l->fill.a) continue;
      const geo::ProvinceShape& sh = fs->provinces.at(pid);
      path.clear();
      addRings(*fs, sh, P, path);
      fc.fillPath(path, l->fill, gfx::FillRule::EvenOdd);
      // Расширение заливки: под мягкий край моря (берег) и на полпикселя за общие границы (без светлых швов).
      coastPath.clear();
      borderPath.clear();
      auto it = g.provEdges.find(pid);
      if (it != g.provEdges.end())
        for (u32 ei : it->second) {
          const EdgeGeo& e = g.edges[ei];
          if (!e.box.intersects(qb)) continue;
          addEdge(e, qb, P, e.kind == EdgeKind::Border ? borderPath : coastPath);
        }
      if (!coastPath.empty()) fc.strokePath(coastPath, strokeOf(float(2 * coastGrow * ds)), l->fill);
      if (!borderPath.empty()) fc.strokePath(borderPath, strokeOf(1.0f), l->fill);
    }
    blendImage(img, fill, 0, 0, gfx::RectI(0, 0, T, T), L.fillAlpha);
  }

  // 2. Море, реки и озёра.
  const int iOcean = bm ? bm->layerIndex("ocean") : -1, iInland = bm ? bm->layerIndex("inland") : -1;
  const int iSymbols = bm ? bm->layerIndex("symbols") : -1;
  if (iOcean >= 0) composeBasemap(img, bm, rc, ds, P.ox, P.oy, iOcean, iOcean + 1);
  if (iInland >= 0) composeBasemap(img, bm, rc, ds, P.ox, P.oy, iInland, iInland + 1);

  gfx::Canvas c(img);
  // Без базовой карты море — грани с рельефом «море» цветом моря.
  if (iOcean < 0 && fs) {
    gfx::Path sea;
    for (const geo::Face& f : fs->faces) {
      if (f.terrain != Terrain::Sea || !f.box.intersects(qb)) continue;
      for (const auto& ring : f.rings) {
        if (ring.size() < 3) continue;
        addPolyline(ring.data(), ring.size(), P, sea);
        sea.close();
      }
    }
    if (!sea.empty()) c.fillPath(sea, bm ? bm->oceanColor() : Color(0, 38, 255), gfx::FillRule::EvenOdd);
  }
  // 2а. Сухопутная провинция (без галочки «Морская») на морской части карты: заливка её морских граней поверх
  //     моря с прозрачностью режима — иначе море закрывает заливку и выбранный цвет не виден (ТЗ 1.a.vii).
  if (anyFill && L.fillAlpha > 0 && fs) {
    gfx::Image over(T, T, 0);
    gfx::Canvas oc(over);
    bool any = false;
    gfx::Path path;
    for (Id pid : provs) {
      const ProvLook* l = lookOf(L, pid);
      if (!l || !l->land || !l->fill.a) continue;
      path.clear();
      for (int fi : fs->provinces.at(pid).faces) {
        const geo::Face& face = fs->faces[size_t(fi)];
        if (face.terrain != Terrain::Sea || !face.box.intersects(qb)) continue;
        for (const auto& ring : face.rings) {
          if (ring.size() < 3) continue;
          addPolyline(ring.data(), ring.size(), P, path);
          path.close();
        }
      }
      if (path.empty()) continue;
      oc.fillPath(path, l->fill, gfx::FillRule::EvenOdd);
      any = true;
    }
    if (any) blendImage(img, over, 0, 0, gfx::RectI(0, 0, T, T), L.fillAlpha);
  }
  // 3. Штриховка оккупации: диагональные линии цвета оккупанта, узор привязан к глобальной сетке устройства.
  if (L.hatch && fs) {
    const float step = 9.0f * dpi, lw = 2.4f * dpi;
    for (Id pid : provs) {
      const ProvLook* l = lookOf(L, pid);
      if (!l || !l->hatch.a || !l->land) continue;
      gfx::Path area;
      addRings(*fs, fs->provinces.at(pid), P, area);
      gfx::Path lines;
      // линии x + y = k·step в глобальных координатах устройства
      const double g0 = P.ox + P.oy;
      const long k0 = long(std::floor((g0 - lw) / step)), k1 = long(std::ceil((g0 + 2.0 * T + lw) / step));
      for (long k = k0; k <= k1; k++) {
        const float s = float(k * double(step) - g0);  // x + y = s в координатах тайла
        lines.moveTo(s + 2, -2);
        lines.lineTo(-2, s + 2);
      }
      c.save();
      c.clipPath(area, gfx::FillRule::EvenOdd);
      gfx::Stroke st = strokeOf(lw);
      st.cap = gfx::Cap::Butt;
      c.strokePath(lines, st, l->hatch.withA(215));
      c.restore();
    }
  }

  // 4. Границы провинций: суша — тёмная тонкая линия, море — светлая.
  {
    gfx::Path landPath, seaPath;
    for (u32 ei : edgeIdx) {
      const EdgeGeo& e = g.edges[ei];
      if (e.kind != EdgeKind::Border || e.pl == e.pr) continue;
      const bool sea = e.tl == Terrain::Sea && e.tr == Terrain::Sea;
      addEdge(e, qb, P, sea ? seaPath : landPath);
    }
    const float wl = (sc.style.editBorders ? 1.15f : 0.8f) * dpi;
    if (!landPath.empty() && L.provLineAlpha > 0)
      c.strokePath(landPath, strokeOf(std::max(0.75f, wl)), Color(38, 32, 26, u8(255 * L.provLineAlpha)));
    if (!seaPath.empty() && L.seaLineAlpha > 0)
      c.strokePath(seaPath, strokeOf(std::max(0.75f, (sc.style.editBorders ? 1.15f : 0.9f) * dpi)), Color(235, 242, 255, u8(255 * L.seaLineAlpha)));
  }

  // 5. Границы государств: обводка двойной ширины, отсечённая областью государства (видна только внутренняя
  //    половина — у соседей двухцветная граница, у берега линия остаётся на суше).
  if (fs) {
    std::vector<Id> states;
    for (Id pid : provs)
      if (Id s = stateOf(L, pid)) states.push_back(s);
    std::sort(states.begin(), states.end());
    states.erase(std::unique(states.begin(), states.end()), states.end());
    gfx::Path seams;
    const float sw = L.stateWidth * dpi;
    for (Id s : states) {
      gfx::Path area, line;
      for (Id pid : provs)
        if (stateOf(L, pid) == s) addRings(*fs, fs->provinces.at(pid), P, area);
      for (u32 ei : edgeIdx) {
        const EdgeGeo& e = g.edges[ei];
        const Id a = stateOf(L, e.pl), b = stateOf(L, e.pr);
        if (a == b || (a != s && b != s)) continue;
        addEdge(e, qb, P, line);
      }
      if (line.empty() || area.empty()) continue;
      auto it = L.stateLine.find(s);
      const Color col = it != L.stateLine.end() ? it->second : Color(60, 52, 44, 220);
      c.save();
      c.clipPath(area, gfx::FillRule::EvenOdd);
      c.strokePath(line, strokeOf(2 * sw), col);
      c.restore();
    }
    // Тонкая тёмная линия по стыку двух государств.
    for (u32 ei : edgeIdx) {
      const EdgeGeo& e = g.edges[ei];
      const Id a = stateOf(L, e.pl), b = stateOf(L, e.pr);
      if (a && b && a != b) addEdge(e, qb, P, seams);
    }
    if (!seams.empty()) c.strokePath(seams, strokeOf(std::max(0.75f, 0.7f * dpi)), Color(24, 20, 16, u8(255 * L.seamAlpha)));
  }

  // 6. Символы базовой карты (горы, замки, башни) — поверх всего, без подкраски.
  if (iSymbols >= 0) composeBasemap(img, bm, rc, ds, P.ox, P.oy, iSymbols, iSymbols + 1);
  return img;
}

}  // namespace rg::map::detail
