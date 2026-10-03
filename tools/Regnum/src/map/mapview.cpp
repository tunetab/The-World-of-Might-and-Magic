// Regnum — отрисовка карты: камера, мир и инвалидация, композиция кадра, попадание мышью.
#include "map/mapview.h"

#include <unordered_set>

#include "base/fs.h"
#include "base/jobs.h"
#include "codec/png.h"
#include "map/map_internal.h"

namespace rg::map {

using namespace detail;

namespace {

constexpr double kAnimSec = 0.18;
constexpr size_t kRasterBudget = size_t(384) << 20;
constexpr size_t kTileBudget = size_t(256) << 20;

double easeOut(double t) {
  t = clamp(t, 0.0, 1.0);
  return 1 - (1 - t) * (1 - t) * (1 - t);
}

std::shared_ptr<const gfx::Image> loadPng(const std::string& path) {
  if (path.empty()) return nullptr;
  auto bytes = fs::readFile(path);
  if (!bytes) return nullptr;
  auto img = codec::decodePng(*bytes);
  if (!img) return nullptr;
  return std::make_shared<gfx::Image>(gfx::Image::fromRgba(img->rgba.data(), img->w, img->h));
}

Box2 edgeBox(const World& w, const Edge& e) {
  Box2 b;
  if (const Node* a = w.nodes.get(e.a)) b.add(a->p);
  if (const Node* c = w.nodes.get(e.b)) b.add(c->p);
  for (Vec2 p : e.pts) b.add(p);
  return b;
}

}  // namespace

// ================================================================ состояние
struct MapView::Impl {
  const Basemap* bm = nullptr;
  World world;
  u64 gen = 0;
  View v;

  // анимация камеры
  bool anim = false, pending = false;
  double t0 = 0, now = 0;
  double fx = 0, fy = 0, fz = 1, tx = 0, ty = 0, tz = 1;
  bool anchored = false;
  float ax = 0, ay = 0;
  Vec2 amap;

  // тайлы
  TileStore store;
  TileStyle style;
  bool styleSet = false;
  std::shared_ptr<const Looks> looks;
  std::shared_ptr<const TileScene> scene;
  u64 frame = 0;
  std::function<void()> wake;

  // базовая карта целиком (запасной слой)
  std::shared_ptr<const gfx::Image> thumb;
  std::mutex previewMu;
  std::shared_ptr<const gfx::Image> preview;
  bool previewRequested = false;
  std::atomic<bool> previewArrived{false};

  LabelCache labels;
  bool labelsPending = false;
  RenderStats stats;

  // свободная часть области просмотра между панелями (пустая — вся область)
  RectF safe;

  // отметки войск и флота: раскладка пересчитывается при смене мира, масштаба, выделения и скрытых объектов
  Id lastSel = 0;
  std::vector<Id> lastHidden;   // по возрастанию
  mutable MarkLayout marks;
  mutable bool marksValid = false;
  mutable u64 marksGen = 0;
  mutable double marksZoom = 0;
  mutable Id marksSel = 0;
  mutable std::vector<Id> marksHidden;

  // мини-карта
  gfx::Image miniTint;
  u64 miniGen = ~u64(0);
  std::shared_ptr<const Looks> miniLooks;
  Table<Node> miniNodes;
  Table<Edge> miniEdges;

  explicit Impl(const Basemap* b) : bm(b && b->loaded() ? b : nullptr), store(bm, kRasterBudget, kTileBudget) {
    if (bm) thumb = loadPng(bm->thumbPath());
  }

  double mapW() const { return bm ? bm->width() : schema::kMapWidth; }
  double mapH() const { return bm ? bm->height() : schema::kMapHeight; }

  // ---------------------------------------------------------------- камера
  // Свободная часть области просмотра (между панелями); слишком маленькая или не заданная — вся область.
  RectF fitArea() const {
    if (safe.empty()) return v.viewport;
    const RectF r = safe.intersect(v.viewport);
    return r.w < 40 || r.h < 40 ? v.viewport : r;
  }
  static double fitZoomIn(const RectF& area, Box2 b, float margin) {
    const double vw = std::max(1.0, double(area.w) - 2 * margin), vh = std::max(1.0, double(area.h) - 2 * margin);
    return std::min(vw / std::max(1.0, b.w()), vh / std::max(1.0, b.h()));
  }
  // Весь мир помещается в свободную часть (а значит, и во всю область просмотра).
  double minZoom() const { return std::max(1e-4, fitZoomIn(fitArea(), Box2(0, 0, mapW(), mapH()), 16)); }
  double maxZoom() const { return std::max(minZoom(), 3.0); }
  // Центр камеры, при котором точка карты p видна в центре свободной части при масштабе z.
  Vec2 centerFor(Vec2 p, double z) const {
    const RectF a = fitArea();
    return {p.x - (double(a.cx()) - double(v.viewport.cx())) / z, p.y - (double(a.cy()) - double(v.viewport.cy())) / z};
  }

  // Центр ограничен так, что край карты может зайти не дальше 40 % области просмотра или 40 % свободной части
  // (место под панели; берётся более свободное из двух ограничений).
  void clampCenter(double& cx, double& cy, double z) const {
    auto range = [](double size, double vc, double r0, double r1, double zz, double& lo, double& hi) {
      const double m = 0.4 * (r1 - r0);
      lo = (vc - r0 - m) / zz;          // ближний край карты — не дальше r0 + m от начала области
      hi = size + (vc - r1 + m) / zz;   // дальний край — не ближе r1 - m
      if (lo > hi) std::swap(lo, hi);
    };
    auto axis = [&](double c, double size, double vc, double v0, double v1, double s0, double s1) {
      double lo, hi, lo2, hi2;
      range(size, vc, v0, v1, z, lo, hi);
      range(size, vc, s0, s1, z, lo2, hi2);
      return clamp(c, std::min(lo, lo2), std::max(hi, hi2));
    };
    const RectF& vp = v.viewport;
    const RectF a = fitArea();
    cx = axis(cx, mapW(), vp.cx(), vp.x, vp.right(), a.x, a.right());
    cy = axis(cy, mapH(), vp.cy(), vp.y, vp.bottom(), a.y, a.bottom());
  }

  const MarkLayout& markLayout() const {
    if (!marksValid || marksGen != gen || marksZoom != v.zoom || marksSel != lastSel || marksHidden != lastHidden) {
      std::vector<const Army*> list;
      list.reserve(world.armies.size());
      world.armies.each([&](const Army& a) {
        if (!std::binary_search(lastHidden.begin(), lastHidden.end(), a.id)) list.push_back(&a);
      });
      marks = layoutMarks(list, v.zoom, lastSel);
      marksValid = true;
      marksGen = gen;
      marksZoom = v.zoom;
      marksSel = lastSel;
      marksHidden = lastHidden;
    }
    return marks;
  }
  ArmyMark publicMark(const MarkLayout::Mark& m) const {
    ArmyMark r;
    r.top = m.members.front();
    r.members = m.members;
    r.pos = m.pos;
    r.units = m.units;
    const gfx::Pt s = v.toScreen(m.pos);
    r.bounds = RectF(s.x + m.rel.x, s.y + m.rel.y, m.rel.w, m.rel.h);
    return r;
  }
  void startAnim(double cx, double cy, double z) {
    fx = v.cx;
    fy = v.cy;
    fz = v.zoom;
    tx = cx;
    ty = cy;
    tz = z;
    anim = true;
    pending = true;
  }
  void apply(double cx, double cy, double z) {
    v.cx = cx;
    v.cy = cy;
    v.zoom = z;
  }
  void target(double& cx, double& cy, double& z) const {
    if (anim) { cx = tx; cy = ty; z = tz; }
    else { cx = v.cx; cy = v.cy; z = v.zoom; }
  }
  void step(double t) {
    now = t;
    if (!anim) return;
    if (pending) {
      t0 = t;
      pending = false;
      return;
    }
    const double k = (t - t0) / kAnimSec;
    if (k >= 1) {
      anim = false;
      apply(tx, ty, tz);
      return;
    }
    const double e = easeOut(k);
    const double z = std::exp(std::log(fz) + (std::log(tz) - std::log(fz)) * e);
    if (anchored) {
      apply(amap.x - (double(ax) - double(v.viewport.cx())) / z, amap.y - (double(ay) - double(v.viewport.cy())) / z, z);
    } else {
      // при смене масштаба центр движется согласованно с 1/z (без «дуги»), иначе — по кривой замедления
      const double den = 1 / tz - 1 / fz;
      const double kk = std::fabs(den) > 1e-12 ? clamp((1 / z - 1 / fz) / den, 0.0, 1.0) : e;
      apply(fx + (tx - fx) * kk, fy + (ty - fy) * kk, z);
    }
  }

  // ---------------------------------------------------------------- мир и стиль
  void rebuildScene() {
    auto sc = std::make_shared<TileScene>();
    sc->world = world;
    sc->style = style;
    sc->styleKey = style.key();
    sc->looks = looks;
    sc->gen = gen;
    scene = sc;
    store.setScene(sc);
  }

  void setStyle(const TileStyle& s) {
    if (styleSet && s == style) return;
    style = s;
    styleSet = true;
    looks = computeLooks(world, style);
    rebuildScene();
  }

  void applyWorld(const World& after) {
    const World before = world;
    const u32 what = World::diff(before, after);
    world = after;
    if (!what && gen) return;
    gen++;
    if (!styleSet) {
      style.fillOpacity = world.settings->fillOpacity;
      styleSet = true;
    }
    // Прозрачность заливки — часть стиля тайлов.
    if (world.settings->fillOpacity != style.fillOpacity) style.fillOpacity = world.settings->fillOpacity;
    auto oldLooks = looks;
    looks = computeLooks(world, style);

    std::vector<Box2> dirty;
    bool all = !oldLooks || gen == 1;
    if (!all && (what & TB_GEO)) {
      std::unordered_set<Id> nodes;
      before.nodes.diff(after.nodes, [&](Id id) { nodes.insert(id); });
      before.edges.diff(after.edges, [&](Id id) {
        if (const Edge* e = before.edges.get(id)) dirty.push_back(edgeBox(before, *e));
        if (const Edge* e = after.edges.get(id)) dirty.push_back(edgeBox(after, *e));
      });
      if (!nodes.empty()) {
        auto touch = [&](const World& w) {
          w.edges.each([&](const Edge& e) {
            if (nodes.count(e.a) || nodes.count(e.b)) dirty.push_back(edgeBox(w, e));
          });
        };
        touch(before);
        touch(after);
      }
    }
    if (!all) {
      if (oldLooks->fillAlpha != looks->fillAlpha) all = true;
      auto fsA = geo::faces(after);
      auto fsB = (what & TB_GEO) ? geo::faces(before) : fsA;
      auto addProv = [&](Id pid) {
        if (const geo::ProvinceShape* s = fsA->shape(pid)) dirty.push_back(s->box);
        if (fsB != fsA)
          if (const geo::ProvinceShape* s = fsB->shape(pid)) dirty.push_back(s->box);
      };
      std::unordered_set<Id> lineChanged;
      for (const auto& [sid, col] : looks->stateLine) {
        auto it = oldLooks->stateLine.find(sid);
        if (it == oldLooks->stateLine.end() || !(it->second == col)) lineChanged.insert(sid);
      }
      for (const auto& [sid, col] : oldLooks->stateLine)
        if (!looks->stateLine.count(sid)) lineChanged.insert(sid);
      for (const auto& [pid, look] : looks->prov) {
        auto it = oldLooks->prov.find(pid);
        if (it == oldLooks->prov.end() || !(it->second == look) || lineChanged.count(look.state)) addProv(pid);
      }
      for (const auto& [pid, look] : oldLooks->prov)
        if (!looks->prov.count(pid) || lineChanged.count(look.state)) addProv(pid);
      if (dirty.size() > 4000) all = true;
    }
    store.invalidate(dirty, gen, style.key(), all);
    rebuildScene();
  }

  // ---------------------------------------------------------------- превью (асинхронно)
  std::shared_ptr<const gfx::Image> backdrop() {
    if (!bm) return nullptr;
    if (!previewRequested) {
      previewRequested = true;
      const std::string path = bm->previewPath();
      auto self = this;
      // Impl живёт дольше задачи: деструктор ждёт её (см. ~Impl).
      previewJob = jobs::submit([self, path] {
        auto img = loadPng(path);
        {
          std::lock_guard<std::mutex> lk(self->previewMu);
          self->preview = img;
        }
        self->previewArrived = true;
        std::function<void()> w;
        {
          std::lock_guard<std::mutex> lk(self->previewMu);
          w = self->wake;
        }
        if (w) w();
      });
    }
    std::lock_guard<std::mutex> lk(previewMu);
    return preview ? preview : thumb;
  }
  std::future<void> previewJob;

  ~Impl() {
    if (previewJob.valid()) {
      try {
        previewJob.wait();
      } catch (...) {
      }
    }
  }

  // ---------------------------------------------------------------- кадр
  void drawTiles(gfx::Image& target, const gfx::RectI& clip, double X0, double Y0, double ds, bool idle);
};

// ================================================================ составные тайлы в кадре
void MapView::Impl::drawTiles(gfx::Image& target, const gfx::RectI& clipIn, double X0, double Y0, double ds, bool idle) {
  const int T = kTile;
  const u64 sk = style.key();
  // Область карты на устройстве.
  const gfx::RectI mapDev(int(X0), int(Y0), int(std::lround(X0 + mapW() * ds)) - int(X0), int(std::lround(Y0 + mapH() * ds)) - int(Y0));
  const gfx::RectI clip = clipIn.intersect(mapDev);
  if (clip.empty()) return;

  struct Op {
    std::shared_ptr<const gfx::Image> img;
    double x0, y0, x1, y1;
    gfx::RectI clip;
    bool copy;   // точный масштаб: копирование
  };
  std::vector<Op> ops;
  std::vector<TileRequest> reqs;
  std::vector<TileKey> used;

  // 1. Тайлы текущего масштаба (в покое).
  const int cols = int(std::ceil(mapW() * ds / T)), rows = int(std::ceil(mapH() * ds / T));
  const int tx0 = std::max(0, int(std::floor((clip.x - X0) / T))), tx1 = std::min(cols - 1, int(std::floor((clip.right() - 1 - X0) / T)));
  const int ty0 = std::max(0, int(std::floor((clip.y - Y0) / T))), ty1 = std::min(rows - 1, int(std::floor((clip.bottom() - 1 - Y0) / T)));
  std::vector<gfx::RectI> missing;
  if (idle) {
    const double cxs = (clip.x + clip.w * 0.5 - X0) / T, cys = (clip.y + clip.h * 0.5 - Y0) / T;
    for (int ty = ty0; ty <= ty1; ty++)
      for (int tx = tx0; tx <= tx1; tx++) {
        const TileKey k{sk, ds, tx, ty};
        const gfx::RectI r = gfx::RectI(int(X0) + tx * T, int(Y0) + ty * T, T, T).intersect(clip);
        reqs.push_back({k, std::hypot(tx + 0.5 - cxs, ty + 0.5 - cys)});
        used.push_back(k);
        TileView tv;
        if (store.get(k, tv)) ops.push_back({tv.img, X0 + tx * T, Y0 + ty * T, X0 + (tx + 1) * T, Y0 + (ty + 1) * T, r, true});
        else missing.push_back(r);
      }
    // Кольцо вокруг видимой области — заранее (для панорамы).
    for (int ty = ty0 - 1; ty <= ty1 + 1; ty++)
      for (int tx = tx0 - 1; tx <= tx1 + 1; tx++) {
        if (tx < 0 || ty < 0 || tx >= cols || ty >= rows) continue;
        if (tx >= tx0 && tx <= tx1 && ty >= ty0 && ty <= ty1) continue;
        reqs.push_back({TileKey{sk, ds, tx, ty}, 100 + std::hypot(tx + 0.5 - cxs, ty + 0.5 - cys)});
      }
  } else {
    missing.push_back(clip);
    // Во время анимации — тайлы конечного масштаба для конечной области.
    if (anim) {
      const double tds = tz * double(v.dpi);
      const double tX0 = std::round(double(v.viewport.cx()) * v.dpi - tx * tds), tY0 = std::round(double(v.viewport.cy()) * v.dpi - ty * tds);
      const int tc = int(std::ceil(mapW() * tds / T)), tr = int(std::ceil(mapH() * tds / T));
      const double vx0 = v.viewport.x * v.dpi, vy0 = v.viewport.y * v.dpi, vx1 = v.viewport.right() * v.dpi, vy1 = v.viewport.bottom() * v.dpi;
      const int a0 = std::max(0, int(std::floor((vx0 - tX0) / T))), a1 = std::min(tc - 1, int(std::floor((vx1 - 1 - tX0) / T)));
      const int b0 = std::max(0, int(std::floor((vy0 - tY0) / T))), b1 = std::min(tr - 1, int(std::floor((vy1 - 1 - tY0) / T)));
      const double cxs = ((vx0 + vx1) * 0.5 - tX0) / T, cys = ((vy0 + vy1) * 0.5 - tY0) / T;
      for (int y = b0; y <= b1; y++)
        for (int x = a0; x <= a1; x++) reqs.push_back({TileKey{sk, tds, x, y}, std::hypot(x + 0.5 - cxs, y + 0.5 - cys)});
    }
  }
  // Опорный уровень (вся карта) — всегда, после видимого.
  const int bc = int(std::ceil(mapW() * kBaseScale / T)), br = int(std::ceil(mapH() * kBaseScale / T));
  for (int y = 0; y < br; y++)
    for (int x = 0; x < bc; x++) {
      reqs.push_back({TileKey{sk, kBaseScale, x, y}, double(1000 + y * bc + x)});
      used.push_back(TileKey{sk, kBaseScale, x, y});
    }
  store.request(reqs, frame);

  // 2. Запасные слои для недостающих тайлов.
  stats.fallback = !missing.empty();
  std::vector<Op> back;
  if (!missing.empty()) {
    gfx::RectI need;
    for (const gfx::RectI& r : missing) need = need.unite(r);
    const Box2 nb((need.x - X0) / ds, (need.y - Y0) / ds, (need.right() - X0) / ds, (need.bottom() - Y0) / ds);
    std::vector<TileView> cand;
    store.available(0, nb, cand);
    // Группы по (стиль, масштаб); ищем лучшую, покрывающую всю нужную область.
    struct Group { u64 style; double ds; std::vector<TileView> tiles; double score; bool covers; };
    std::vector<Group> groups;
    for (TileView& t : cand) {
      if (idle && t.key.style == sk && t.key.ds == ds) continue;
      auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.style == t.key.style && g.ds == t.key.ds; });
      if (it == groups.end()) { groups.push_back({t.key.style, t.key.ds, {}, 0, false}); it = groups.end() - 1; }
      it->tiles.push_back(std::move(t));
    }
    for (Group& g : groups) {
      const double lr = std::log2(g.ds / ds);
      g.score = (g.style == sk ? 0 : 10) + (lr >= 0 ? lr * 0.6 : -lr) ;   // чем меньше, тем лучше; мельче — предпочтительнее
      // покрытие: все тайлы группы, пересекающие nb, на месте
      const int gc = int(std::ceil(mapW() * g.ds / T)), gr = int(std::ceil(mapH() * g.ds / T));
      const int a0 = std::max(0, int(std::floor(nb.x0 * g.ds / T))), a1 = std::min(gc - 1, int(std::floor(nb.x1 * g.ds / T - 1e-9)));
      const int b0 = std::max(0, int(std::floor(nb.y0 * g.ds / T))), b1 = std::min(gr - 1, int(std::floor(nb.y1 * g.ds / T - 1e-9)));
      int have = 0;
      for (const TileView& t : g.tiles)
        if (t.key.tx >= a0 && t.key.tx <= a1 && t.key.ty >= b0 && t.key.ty <= b1) have++;
      g.covers = have >= (a1 - a0 + 1) * (b1 - b0 + 1);
    }
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.score > b.score; });  // худшие первыми
    size_t firstCover = groups.size();
    for (size_t i = groups.size(); i-- > 0;)
      if (groups[i].covers) { firstCover = i; break; }
    // Лучшее полное покрытие и всё, что лучше него, поверх.
    const size_t from = firstCover < groups.size() ? firstCover : 0;
    if (firstCover >= groups.size()) {
      if (auto bd = backdrop())
        for (const gfx::RectI& r : missing) back.push_back({bd, X0, Y0, X0 + mapW() * ds, Y0 + mapH() * ds, r, false});
    }
    for (size_t i = from; i < groups.size(); i++)
      for (const TileView& t : groups[i].tiles) {
        const double k = ds / t.key.ds;
        const double x0 = X0 + t.key.tx * T * k, y0 = Y0 + t.key.ty * T * k;
        for (const gfx::RectI& r : missing) {
          const gfx::RectI rr = r.intersect(gfx::RectI(int(std::floor(x0)), int(std::floor(y0)), int(std::ceil(T * k)) + 2, int(std::ceil(T * k)) + 2));
          if (!rr.empty()) back.push_back({t.img, x0, y0, x0 + T * k, y0 + T * k, rr, false});
        }
      }
    if (back.empty())
      for (const gfx::RectI& r : missing) {
        // Ничего нет: суша цветом базовой карты.
        gfx::Image one(1, 1, gfx::premul(bm ? bm->landColor() : Color(255, 255, 255)));
        back.push_back({std::make_shared<gfx::Image>(one), double(r.x), double(r.y), double(r.right()), double(r.bottom()), r, false});
      }
  }
  store.touch(used, frame);

  // 3. Исполнение: запасные (масштабирование) — полосами в пуле, точные — копированием.
  std::vector<Op> all;
  all.reserve(back.size() + ops.size());
  for (Op& o : back) all.push_back(std::move(o));
  for (Op& o : ops) all.push_back(std::move(o));
  stats.tilesDrawn = int(ops.size());
  stats.fallbackOps = int(back.size());
  if (all.empty()) return;
  const bool heavy = !back.empty();
  const int bands = heavy ? std::max(1, std::min(16, clip.h / 48)) : std::max(1, std::min(8, clip.h / 128));
  auto run = [&](size_t bi) {
    const int y0 = clip.y + int(clip.h * bi / size_t(bands)), y1 = clip.y + int(clip.h * (bi + 1) / size_t(bands));
    const gfx::RectI band(clip.x, y0, clip.w, y1 - y0);
    for (const Op& o : all) {
      const gfx::RectI rc = o.clip.intersect(band);
      if (rc.empty()) continue;
      if (o.copy) copyImage(target, *o.img, int(o.x0), int(o.y0), rc);
      else scaleImage(target, *o.img, o.x0, o.y0, o.x1, o.y1, rc, true);
    }
  };
  if (bands == 1) run(0);
  else jobs::parallelFor(size_t(bands), run, 1);
}

// ================================================================ MapView
MapView::MapView(const Basemap* basemap) : d_(std::make_unique<Impl>(basemap)) {
  d_->v.cx = d_->mapW() / 2;
  d_->v.cy = d_->mapH() / 2;
}

MapView::~MapView() = default;

void MapView::setWorld(const World& w) { d_->applyWorld(w); }
void MapView::worldChanged(const World&, const World& after, u32) { d_->applyWorld(after); }

void MapView::setWakeCallback(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lk(d_->previewMu);
    d_->wake = fn;
  }
  d_->store.setWake(std::move(fn));
}

void MapView::setViewport(RectF logical, float dpi) {
  const bool first = d_->v.viewport.empty();
  const bool resized = first || logical.w != d_->v.viewport.w || logical.h != d_->v.viewport.h;
  d_->v.viewport = logical;
  d_->v.dpi = dpi > 0 ? dpi : 1;
  if (logical.empty()) return;
  if (first) {
    d_->v.zoom = d_->minZoom();
    d_->v.cx = d_->mapW() / 2;
    d_->v.cy = d_->mapH() / 2;
  }
  // Пределы камеры проверяются при смене размера окна; смена свободной части (открылась или закрылась панель)
  // камеру не дёргает.
  if (!resized) return;
  double z = clamp(d_->v.zoom, d_->minZoom(), d_->maxZoom()), cx = d_->v.cx, cy = d_->v.cy;
  d_->clampCenter(cx, cy, z);
  d_->apply(cx, cy, z);
  if (d_->anim) {
    d_->tz = clamp(d_->tz, d_->minZoom(), d_->maxZoom());
    d_->clampCenter(d_->tx, d_->ty, d_->tz);
  }
}

void MapView::setSafeArea(RectF logical) { d_->safe = logical; }
RectF MapView::safeArea() const { return d_->safe; }

const View& MapView::view() const { return d_->v; }

void MapView::zoomAt(float sx, float sy, double factor, bool animate) {
  if (!(factor > 0) || d_->v.viewport.empty()) return;
  double cx, cy, z;
  d_->target(cx, cy, z);
  // Если текущий масштаб меньше minZoom (свободная часть выросла), отдаление его не увеличивает.
  const double nz = clamp(z * factor, std::min(d_->minZoom(), z), d_->maxZoom());
  // Точка под курсором — по видимому состоянию (при серии щелчков колеса цель накапливается).
  const bool sameAnchor = d_->anim && d_->anchored && d_->ax == sx && d_->ay == sy;
  const Vec2 m = sameAnchor ? d_->amap : d_->v.toMap(sx, sy);
  double ncx = m.x - (double(sx) - double(d_->v.viewport.cx())) / nz, ncy = m.y - (double(sy) - double(d_->v.viewport.cy())) / nz;
  const double rx = ncx, ry = ncy;
  d_->clampCenter(ncx, ncy, nz);
  if (!animate) {
    d_->anim = false;
    d_->apply(ncx, ncy, nz);
    return;
  }
  if (sameAnchor) {
    // продолжить текущую анимацию к новой цели
    d_->fx = d_->v.cx; d_->fy = d_->v.cy; d_->fz = d_->v.zoom;
    d_->tx = ncx; d_->ty = ncy; d_->tz = nz;
    d_->pending = true;
  } else {
    d_->startAnim(ncx, ncy, nz);
  }
  d_->anchored = std::fabs(rx - ncx) < 1e-9 && std::fabs(ry - ncy) < 1e-9;
  d_->ax = sx;
  d_->ay = sy;
  d_->amap = m;
}

void MapView::panBy(float dx, float dy) {
  if (d_->v.viewport.empty()) return;
  double cx = d_->v.cx - dx / d_->v.zoom, cy = d_->v.cy - dy / d_->v.zoom;
  d_->clampCenter(cx, cy, d_->v.zoom);
  const double mx = cx - d_->v.cx, my = cy - d_->v.cy;
  d_->apply(cx, cy, d_->v.zoom);
  if (d_->anim) {  // панорама во время анимации сдвигает и цель
    d_->fx += mx; d_->fy += my;
    d_->tx += mx; d_->ty += my;
    d_->amap = d_->amap + Vec2(mx, my);
    d_->clampCenter(d_->tx, d_->ty, d_->tz);
  }
}

void MapView::centerOn(Vec2 p, double zoom, bool animate) {
  const double z = zoom > 0 ? clamp(zoom, d_->minZoom(), d_->maxZoom()) : clamp(d_->anim ? d_->tz : d_->v.zoom, d_->minZoom(), d_->maxZoom());
  double cx = p.x, cy = p.y;
  d_->clampCenter(cx, cy, z);
  if (!animate) {
    d_->anim = false;
    d_->apply(cx, cy, z);
    return;
  }
  d_->startAnim(cx, cy, z);
  d_->anchored = false;
}

void MapView::fit(Box2 box, bool animate) {
  if (box.empty()) return;
  const double minSide = 60;
  if (box.w() < minSide || box.h() < minSide) {
    const Vec2 c = box.center();
    box = Box2(c.x - std::max(box.w(), minSide) / 2, c.y - std::max(box.h(), minSide) / 2, c.x + std::max(box.w(), minSide) / 2,
               c.y + std::max(box.h(), minSide) / 2);
  }
  const double z = clamp(Impl::fitZoomIn(d_->fitArea(), box, 48), d_->minZoom(), d_->maxZoom());
  centerOn(d_->centerFor(box.center(), z), z, animate);
}

void MapView::fitAll(bool animate) {
  const double z = d_->minZoom();
  centerOn(d_->centerFor(Vec2(d_->mapW() / 2, d_->mapH() / 2), z), z, animate);
}
double MapView::minZoom() const { return d_->minZoom(); }
double MapView::maxZoom() const { return d_->maxZoom(); }
void MapView::update(double t) { d_->step(t); }
bool MapView::animating() const { return d_->anim; }

bool MapView::needsRedraw() const {
  if (d_->previewArrived.load() || d_->labelsPending) return true;
  if (d_->store.arrived()) return true;
  bool wakeSet;
  {
    std::lock_guard<std::mutex> lk(d_->previewMu);
    wakeSet = bool(d_->wake);
  }
  return !wakeSet && d_->store.busy();
}

bool MapView::loading() const { return d_->labelsPending || d_->store.busy(); }
bool MapView::waitIdle(double timeoutSec) { return d_->store.wait(timeoutSec); }
const RenderStats& MapView::stats() const { return d_->stats; }

void MapView::render(gfx::Canvas& c, const RenderOptions& opt) {
  Impl& d = *d_;
  const double t0 = nowSeconds();
  d.frame++;
  d.previewArrived = false;
  d.store.takeArrived();
  const View& v = d.v;
  if (v.viewport.empty()) return;
  TileStyle st;
  st.mode = opt.mode;
  st.editBorders = opt.editBorders;
  st.fillOpacity = d.world.settings->fillOpacity;
  st.dpi = v.dpi;
  d.setStyle(st);

  const float dpi = v.dpi;
  const double ds = v.zoom * dpi;
  // Холст — в пикселях устройства; сдвиг его преобразования учитывается, прочее игнорируется.
  const gfx::Affine base = c.transform();
  const bool shift = base.isTranslateScale() && base.a == 1 && base.d == 1;
  const double bx = shift ? std::round(base.e) : 0, by = shift ? std::round(base.f) : 0;
  const gfx::RectI vp(int(std::lround(v.viewport.x * dpi + bx)), int(std::lround(v.viewport.y * dpi + by)),
                      int(std::lround(v.viewport.right() * dpi + bx)) - int(std::lround(v.viewport.x * dpi + bx)),
                      int(std::lround(v.viewport.bottom() * dpi + by)) - int(std::lround(v.viewport.y * dpi + by)));
  const gfx::RectI clip = vp.intersect(c.clipBounds()).intersect(gfx::RectI(0, 0, c.target().w, c.target().h));
  if (clip.empty()) return;
  // Начало карты на устройстве — по целому пикселю (тайлы копируются без пересэмплирования).
  const double X0 = std::round(double(v.viewport.cx()) * dpi + bx - v.cx * ds);
  const double Y0 = std::round(double(v.viewport.cy()) * dpi + by - v.cy * ds);

  c.save();
  c.setTransform(gfx::Affine());
  c.clipRect(RectF(float(clip.x), float(clip.y), float(clip.w), float(clip.h)));
  // Фон за пределами карты.
  const Color back = opt.darkUi ? Color::hex(0x0b0e13) : Color::hex(0xe4ded2);
  const gfx::RectI mapDev(int(X0), int(Y0), int(std::lround(X0 + d.mapW() * ds)) - int(X0), int(std::lround(Y0 + d.mapH() * ds)) - int(Y0));
  {
    const gfx::RectI in = clip.intersect(mapDev);
    const u32 bp = gfx::premul(back);
    gfx::Image& img = c.target();
    for (int y = clip.y; y < clip.bottom(); y++) {
      u32* row = img.row(y);
      if (in.empty() || y < in.y || y >= in.bottom()) {
        std::fill(row + clip.x, row + clip.right(), bp);
        continue;
      }
      std::fill(row + clip.x, row + in.x, bp);
      std::fill(row + in.right(), row + clip.right(), bp);
    }
  }
  d.drawTiles(c.target(), clip, X0, Y0, ds, !d.anim);
  // Тонкая рамка края карты.
  c.strokeRoundRect(RectF(float(mapDev.x) - 0.5f, float(mapDev.y) - 0.5f, float(mapDev.w) + 1, float(mapDev.h) + 1), 0, 1,
                    opt.darkUi ? Color(255, 255, 255, 28) : Color(0, 0, 0, 40));
  const double t1 = nowSeconds();

  // Наложения.
  auto fs = geo::faces(d.world);
  FrameCtx f;
  f.c = &c;
  f.w = &d.world;
  f.fs = fs.get();
  f.opt = &opt;
  f.X0 = X0;
  f.Y0 = Y0;
  f.ds = ds;
  f.dpi = dpi;
  f.zoom = v.zoom;
  f.clip = clip;
  f.visible = Box2((clip.x - X0) / ds, (clip.y - Y0) / ds, (clip.right() - X0) / ds, (clip.bottom() - Y0) / ds);
  d.lastSel = opt.selArmy;
  d.lastHidden = opt.hideArmies;
  std::sort(d.lastHidden.begin(), d.lastHidden.end());
  d.lastHidden.erase(std::unique(d.lastHidden.begin(), d.lastHidden.end()), d.lastHidden.end());
  f.marks = &d.markLayout();
  drawHover(f);
  const bool guilds = opt.mode == schema::MapMode::Guilds;
  const bool capitals = opt.mode != schema::MapMode::Terrain;
  Obstacles obs;
  collectMarkerObstacles(f, obs, capitals, guilds, guilds);
  collectArmyObstacles(f, obs);
  const double tl = nowSeconds();
  // Новые спрайты подписей — в пределах бюджета кадра (при анимации меньше), остальные — в следующих кадрах.
  d.labelsPending = opt.labels && !drawLabels(f, d.labels, obs, figureSize() * dpi, d.anim ? 2.5 : 12.0);
  d.stats.labelsMs = (nowSeconds() - tl) * 1000;
  if (guilds || opt.mode == schema::MapMode::Trade || opt.selRoute) drawRoutes(f);
  if (guilds) drawGuildPies(f, obs);
  drawMarkers(f, obs, capitals, guilds);
  drawArmies(f);
  drawSelection(f);
  c.restore();
  d.labels.trim(600);
  d.store.trim(d.frame, d.style.key());
  d.stats.composeMs = (t1 - t0) * 1000;
  d.stats.frameMs = (nowSeconds() - t0) * 1000;
  d.stats.pending = d.store.pending();
  d.stats.labelsDeferred = d.labelsPending;
}

// ================================================================ мини-карта
void MapView::renderMinimap(gfx::Canvas& c, RectF rect, float dpi) {
  Impl& d = *d_;
  if (rect.empty()) return;
  // Подкрашенная миниатюра: заливка политической карты поверх thumb (умножение — символы остаются тёмными).
  // Перерисовывается, только когда меняются геометрия или политические цвета провинций.
  std::shared_ptr<const Looks> looks;
  if (d.miniGen != d.gen || d.miniTint.empty()) {
    d.miniGen = d.gen;
    TileStyle ps;
    ps.mode = schema::MapMode::Political;
    looks = computeLooks(d.world, ps);
    const bool same = !d.miniTint.empty() && d.miniLooks && d.miniNodes.same(d.world.nodes) && d.miniEdges.same(d.world.edges) &&
                      d.miniLooks->prov == looks->prov;
    if (same) looks = nullptr;
  }
  if (looks) {
    d.miniLooks = looks;
    d.miniNodes = d.world.nodes;
    d.miniEdges = d.world.edges;
    auto fs = geo::faces(d.world);
    if (d.thumb) {
      d.miniTint = *d.thumb;
    } else {
      // Без базовой карты: белая суша и море по граням.
      d.miniTint = gfx::Image(480, int(std::lround(480 * d.mapH() / d.mapW())), gfx::premul(Color(255, 255, 255)));
      gfx::Canvas sc(d.miniTint);
      const double k = d.miniTint.w / d.mapW();
      gfx::Path sea;
      for (const geo::Face& f : fs->faces) {
        if (f.terrain != Terrain::Sea) continue;
        for (const auto& ring : f.rings) {
          for (size_t i = 0; i < ring.size(); i++) {
            if (i == 0) sea.moveTo(float(ring[i].x * k), float(ring[i].y * k));
            else sea.lineTo(float(ring[i].x * k), float(ring[i].y * k));
          }
          sea.close();
        }
      }
      if (fs->faces.empty()) sea.addRect(RectF(0, 0, float(d.miniTint.w), float(d.miniTint.h)));
      sc.fillPath(sea, Color(0, 38, 255), gfx::FillRule::EvenOdd);
    }
    gfx::Image layer(d.miniTint.w, d.miniTint.h, 0);
    gfx::Canvas lc(layer);
    const double k = d.miniTint.w / d.mapW();
    for (const auto& [pid, sh] : fs->provinces) {
      auto it = looks->prov.find(pid);
      if (!pid || it == looks->prov.end() || !it->second.fill.a) continue;
      gfx::Path p;
      for (int fi : sh.faces)
        for (const auto& ring : fs->faces[size_t(fi)].rings) {
          for (size_t i = 0; i < ring.size(); i++) {
            if (i == 0) p.moveTo(float(ring[i].x * k), float(ring[i].y * k));
            else p.lineTo(float(ring[i].x * k), float(ring[i].y * k));
          }
          p.close();
        }
      lc.fillPath(p, it->second.fill, gfx::FillRule::EvenOdd);
    }
    // умножение с прозрачностью 0,6: d = d · (1 − a + a · c)
    for (size_t i = 0; i < layer.px.size(); i++) {
      const u32 s = layer.px[i];
      const u32 a = ((s >> 24) * 150) / 255;
      if (!a) continue;
      const Color sc = gfx::unpremul(s);
      const u32 dpx = d.miniTint.px[i];
      auto ch = [&](u32 dv, u32 cv) { return (dv * (255 - a) + dv * cv / 255 * a) / 255; };
      const u32 r = ch((dpx >> 16) & 255, sc.r), g = ch((dpx >> 8) & 255, sc.g), b = ch(dpx & 255, sc.b);
      d.miniTint.px[i] = (dpx & 0xFF000000u) | (r << 16) | (g << 8) | b;
    }
  }
  c.save();
  c.scale(dpi, dpi);
  c.save();
  c.clipRoundRect(rect, 6);
  c.drawImage(d.miniTint, rect);
  // Видимая область.
  const Box2 vb = d.v.visibleBox();
  const double kx = rect.w / d.mapW(), ky = rect.h / d.mapH();
  RectF r(float(rect.x + vb.x0 * kx), float(rect.y + vb.y0 * ky), float(vb.w() * kx), float(vb.h() * ky));
  r = r.intersect(rect.inset(1));
  if (!r.empty()) {
    gfx::Path outside;
    outside.addRect(rect);
    outside.addRect(r);
    c.fillPath(outside, Color(8, 10, 14, 70), gfx::FillRule::EvenOdd);
    c.strokeRoundRect(r.inset(-0.5f), 2, 2.5f, Color(0, 0, 0, 90));
    c.strokeRoundRect(r, 2, 1.5f, Color::hex(0xe8b75c));
  }
  c.restore();
  c.strokeRoundRect(rect.inset(0.5f), 6, 1, Color(255, 255, 255, 40));
  c.restore();
}

Vec2 MapView::minimapToMap(RectF rect, float sx, float sy) const {
  const double x = (double(sx) - rect.x) / std::max(1.f, rect.w) * d_->mapW();
  const double y = (double(sy) - rect.y) / std::max(1.f, rect.h) * d_->mapH();
  return {clamp(x, 0.0, d_->mapW()), clamp(y, 0.0, d_->mapH())};
}

std::vector<LegendItem> MapView::legend(const World& w, schema::MapMode mode) const { return legendFor(w, mode); }

// ================================================================ попадание
Id MapView::provinceAt(float sx, float sy) const {
  const Vec2 p = d_->v.toMap(sx, sy);
  if (p.x < 0 || p.y < 0 || p.x > d_->mapW() || p.y > d_->mapH()) return 0;
  return geo::faces(d_->world)->provinceAt(p);
}

std::optional<ArmyMark> MapView::markAt(float sx, float sy) const {
  const MarkLayout& L = d_->markLayout();
  // Верхняя — нарисованная последней.
  for (size_t i = L.marks.size(); i-- > 0;) {
    const MarkLayout::Mark& m = L.marks[i];
    const gfx::Pt s = d_->v.toScreen(m.pos);
    const float dx = sx - s.x, dy = sy - s.y;
    if (!m.rel.contains(dx, dy)) continue;   // быстрый отсев по габаритам
    if (markHit(m, L.figure, dx, dy)) return d_->publicMark(m);
  }
  return std::nullopt;
}

Id MapView::armyAt(float sx, float sy) const {
  const std::optional<ArmyMark> m = markAt(sx, sy);
  return m ? m->top : 0;
}

std::vector<ArmyMark> MapView::armyMarks() const {
  const MarkLayout& L = d_->markLayout();
  std::vector<ArmyMark> out;
  out.reserve(L.marks.size());
  for (const MarkLayout::Mark& m : L.marks) out.push_back(d_->publicMark(m));
  return out;
}

double MapView::separateZoom(const ArmyMark& m) const {
  std::vector<const Army*> list;
  for (Id id : m.members)
    if (const Army* a = d_->world.army(id)) list.push_back(a);
  if (list.size() < 2) return 0;
  const double cur = d_->anim ? d_->tz : d_->v.zoom, zmax = d_->maxZoom();
  if (cur >= zmax * (1 - 1e-9)) return 0;
  double z = cur;
  while (z < zmax) {
    z = std::min(z * 1.2, zmax);
    if (layoutMarks(list, z, d_->lastSel).marks.size() == list.size()) return z;
  }
  return zmax;
}

Id MapView::routeAt(float sx, float sy, float tolPx) const {
  Id best = 0;
  double bd = double(tolPx) * tolPx;
  d_->world.routes.each([&](const Route& r) {
    const std::vector<Vec2> pts = routeLine(r.pts);
    for (size_t i = 1; i < pts.size(); i++) {
      const gfx::Pt a = d_->v.toScreen(pts[i - 1]), b = d_->v.toScreen(pts[i]);
      const double abx = b.x - a.x, aby = b.y - a.y, l2 = abx * abx + aby * aby;
      const double t = l2 > 0 ? clamp(((sx - a.x) * abx + (sy - a.y) * aby) / l2, 0.0, 1.0) : 0;
      const double dx = a.x + abx * t - sx, dy = a.y + aby * t - sy;
      if (dx * dx + dy * dy <= bd) {
        bd = dx * dx + dy * dy;
        best = r.id;
      }
    }
  });
  return best;
}

float MapView::figureSize() const { return figureSizeAt(d_->v.zoom); }

}  // namespace rg::map
