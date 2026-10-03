// Regnum — растры карты: кеш декодированных тайлов базовой карты, наложение и масштабирование изображений.
#include "map/map_internal.h"

namespace rg::map::detail {

namespace {

constexpr u32 kRB = 0x00FF00FFu;

inline u32 mulPx(u32 p, u32 a) {
  u32 rb = (p & kRB) * a + 0x00800080u;
  u32 ag = ((p >> 8) & kRB) * a + 0x00800080u;
  rb = ((rb + ((rb >> 8) & kRB)) >> 8) & kRB;
  ag = (ag + ((ag >> 8) & kRB)) & ~kRB;
  return rb | ag;
}
inline u32 over(u32 s, u32 d) { return s + mulPx(d, 255 - (s >> 24)); }

u64 rkey(int layer, int z, int x, int y) {
  return (u64(u32(layer)) << 56) | (u64(u32(z) & 0xff) << 48) | (u64(u32(x) & 0xffffff) << 24) | u64(u32(y) & 0xffffff);
}

// Отводы фильтра по одной оси: выход k покрывает исходный отрезок [u0 + k·r, u0 + (k + 1)·r).
struct Taps {
  std::vector<i32> first;
  std::vector<u8> count;
  std::vector<u16> w;   // по maxTaps на выход, сумма 256
  int maxTaps = 0;
  int lo = 0, hi = 0;   // диапазон используемых исходных индексов [lo, hi]
};

Taps makeTaps(int outN, double u0, double r, int srcN) {
  Taps t;
  t.maxTaps = r <= 1.0 ? 2 : int(std::ceil(r)) + 1;
  t.first.resize(size_t(outN));
  t.count.resize(size_t(outN));
  t.w.assign(size_t(outN) * size_t(t.maxTaps), 0);
  t.lo = srcN;
  t.hi = -1;
  std::vector<double> wt(size_t(t.maxTaps));
  std::vector<int> idx(size_t(t.maxTaps));
  for (int k = 0; k < outN; k++) {
    int n = 0;
    if (r <= 1.0) {
      const double c = u0 + (k + 0.5) * r - 0.5;
      const double fl = std::floor(c);
      const double f = c - fl;
      const int i0 = clamp(int(fl), 0, srcN - 1), i1 = clamp(int(fl) + 1, 0, srcN - 1);
      if (i0 == i1 || f <= 0) { idx[0] = i0; wt[0] = 1; n = 1; }
      else { idx[0] = i0; wt[0] = 1 - f; idx[1] = i1; wt[1] = f; n = 2; }
    } else {
      const double a = u0 + k * r, b = a + r;
      for (int i = int(std::floor(a)); i < int(std::ceil(b)) && n < t.maxTaps; i++) {
        const double ov = std::min(b, double(i + 1)) - std::max(a, double(i));
        if (ov <= 0) continue;
        const int ci = clamp(i, 0, srcN - 1);
        if (n > 0 && idx[size_t(n - 1)] == ci) { wt[size_t(n - 1)] += ov / r; continue; }
        idx[size_t(n)] = ci;
        wt[size_t(n)] = ov / r;
        n++;
      }
      if (n == 0) { idx[0] = clamp(int(std::floor(a)), 0, srcN - 1); wt[0] = 1; n = 1; }
    }
    // индексы идут подряд: первый + номер отвода
    const int first = idx[0];
    int sum = 0, big = 0;
    u16* w = &t.w[size_t(k) * size_t(t.maxTaps)];
    for (int j = 0; j < n; j++) {
      const int pos = idx[size_t(j)] - first;
      w[pos] = u16(std::lround(wt[size_t(j)] * 256));
      sum += w[pos];
    }
    const int span = idx[size_t(n - 1)] - first + 1;
    for (int j = 1; j < span; j++) if (w[j] > w[big]) big = j;
    w[big] = u16(int(w[big]) + 256 - sum);
    t.first[size_t(k)] = first;
    t.count[size_t(k)] = u8(span);
    t.lo = std::min(t.lo, first);
    t.hi = std::max(t.hi, first + span - 1);
  }
  return t;
}

inline u32 combine(const u32* const* rows, const u16* w, int n, int x) {
  u32 rb = 0, ag = 0;
  for (int j = 0; j < n; j++) {
    const u32 p = rows[j][x], k = w[j];
    rb += (p & kRB) * k;
    ag += ((p >> 8) & kRB) * k;
  }
  rb = ((rb + 0x00800080u) >> 8) & kRB;
  ag = (ag + 0x00800080u) & ~kRB;
  return rb | ag;
}

inline u32 combineRow(const u32* row, int first, const u16* w, int n) {
  u32 rb = 0, ag = 0;
  for (int j = 0; j < n; j++) {
    const u32 p = row[first + j], k = w[j];
    rb += (p & kRB) * k;
    ag += ((p >> 8) & kRB) * k;
  }
  rb = ((rb + 0x00800080u) >> 8) & kRB;
  ag = (ag + 0x00800080u) & ~kRB;
  return rb | ag;
}

// Пересэмплирование src в прямоугольник выхода [dx0, dx1) × [dy0, dy1) пикселей dst: пиксель X выхода покрывает
// исходный отрезок [ux + (X − dx0)·rx, ...). mode: 0 — копия, 1 — наложение.
void resample(gfx::Image& dst, int dx0, int dy0, int dx1, int dy1, const gfx::Image& src, double ux, double uy, double rx, double ry,
              bool blend) {
  const int ow = dx1 - dx0, oh = dy1 - dy0;
  if (ow <= 0 || oh <= 0 || src.empty()) return;
  const Taps tx = makeTaps(ow, ux, rx, src.w), ty = makeTaps(oh, uy, ry, src.h);
  // Горизонтальный проход для нужных строк источника.
  const int rows = ty.hi - ty.lo + 1;
  std::vector<u32> tmp(size_t(rows) * size_t(ow));
  for (int y = 0; y < rows; y++) {
    const u32* s = src.row(ty.lo + y);
    u32* o = &tmp[size_t(y) * size_t(ow)];
    for (int k = 0; k < ow; k++)
      o[k] = combineRow(s, tx.first[size_t(k)], &tx.w[size_t(k) * size_t(tx.maxTaps)], tx.count[size_t(k)]);
  }
  std::vector<const u32*> rp(size_t(ty.maxTaps));
  for (int j = 0; j < oh; j++) {
    const int n = ty.count[size_t(j)];
    for (int q = 0; q < n; q++) rp[size_t(q)] = &tmp[size_t(ty.first[size_t(j)] - ty.lo + q) * size_t(ow)];
    const u16* w = &ty.w[size_t(j) * size_t(ty.maxTaps)];
    u32* d = dst.row(dy0 + j) + dx0;
    if (n == 1 && w[0] == 256) {
      const u32* s = rp[0];
      if (blend) for (int x = 0; x < ow; x++) d[x] = over(s[x], d[x]);
      else std::memcpy(d, s, size_t(ow) * 4);
      continue;
    }
    if (blend) for (int x = 0; x < ow; x++) d[x] = over(combine(rp.data(), w, n, x), d[x]);
    else for (int x = 0; x < ow; x++) d[x] = combine(rp.data(), w, n, x);
  }
}

}  // namespace

// ================================================================ кеш тайлов базовой карты
RasterCache::RasterCache(const Basemap* bm, size_t budgetBytes) : bm_(bm), budget_(budgetBytes) {}

std::shared_ptr<const gfx::Image> RasterCache::peek(int layer, int z, int x, int y) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = map_.find(rkey(layer, z, x, y));
  return it == map_.end() ? nullptr : it->second.img;
}

size_t RasterCache::bytes() const {
  std::lock_guard<std::mutex> lk(mu_);
  return bytes_;
}

void RasterCache::evictLocked() {
  while (bytes_ > budget_) {
    auto victim = map_.end();
    for (auto it = map_.begin(); it != map_.end(); ++it)
      if (!it->second.loading && it->second.img && (victim == map_.end() || it->second.used < victim->second.used)) victim = it;
    if (victim == map_.end()) return;
    bytes_ -= victim->second.img->px.size() * 4;
    map_.erase(victim);
  }
}

std::shared_ptr<const gfx::Image> RasterCache::get(int layer, int z, int x, int y) {
  if (!bm_ || !bm_->loaded() || layer < 0 || layer >= int(bm_->layers().size())) return nullptr;
  const std::string& name = bm_->layers()[size_t(layer)];
  if (!bm_->tileExists(name, z, x, y)) return nullptr;
  const u64 k = rkey(layer, z, x, y);
  {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
      auto it = map_.find(k);
      if (it == map_.end()) break;
      if (!it->second.loading) {
        it->second.used = ++clock_;
        return it->second.img;
      }
      cv_.wait(lk);
    }
    Entry& e = map_[k];
    e.loading = true;
    e.used = ++clock_;
  }
  std::shared_ptr<gfx::Image> img;
  std::string err;
  try {
    if (auto rgba = bm_->readTile(name, z, x, y, &err)) img = std::make_shared<gfx::Image>(gfx::Image::fromRgba(rgba->rgba.data(), rgba->w, rgba->h));
  } catch (const std::exception& ex) {
    err = ex.what();
  }
  if (!img) logWarn("Тайл базовой карты %s z%d (%d, %d) не прочитан: %s", name.c_str(), z, x, y, err.c_str());
  {
    std::lock_guard<std::mutex> lk(mu_);
    Entry& e = map_[k];
    e.loading = false;
    e.img = img;
    if (img) bytes_ += img->px.size() * 4;
    evictLocked();
  }
  cv_.notify_all();
  return img;
}

// ================================================================ базовая карта в изображение
void composeBasemap(gfx::Image& out, const Basemap* bm, RasterCache* rc, double ds, double ox, double oy, int fromLayer, int toLayer) {
  if (!bm || !bm->loaded() || !rc || out.empty() || !(ds > 0)) return;
  const int z = bm->levelFor(ds);
  const BasemapLevel& L = bm->level(z);
  const double f = double(1 << z);
  const double r = 1.0 / (ds * f);     // пикселей уровня на пиксель выхода
  const double u0 = ox * r, v0 = oy * r;
  // Видимая часть выхода (пиксели, попадающие на карту).
  const int ox0 = clamp(int(std::floor(-u0 / r)), 0, out.w), ox1 = clamp(int(std::ceil((L.w - u0) / r)), 0, out.w);
  const int oy0 = clamp(int(std::floor(-v0 / r)), 0, out.h), oy1 = clamp(int(std::ceil((L.h - v0) / r)), 0, out.h);
  if (ox0 >= ox1 || oy0 >= oy1) return;
  // Окно источника с запасом на фильтр.
  const int sx0 = clamp(int(std::floor(u0 + ox0 * r)) - 2, 0, L.w), sx1 = clamp(int(std::ceil(u0 + ox1 * r)) + 2, 0, L.w);
  const int sy0 = clamp(int(std::floor(v0 + oy0 * r)) - 2, 0, L.h), sy1 = clamp(int(std::ceil(v0 + oy1 * r)) + 2, 0, L.h);
  if (sx0 >= sx1 || sy0 >= sy1) return;
  const int T = bm->tileSize();
  const bool exact = std::fabs(r - 1.0) < 1e-12 && std::fabs(u0 - std::round(u0)) < 1e-9 && std::fabs(v0 - std::round(v0)) < 1e-9;
  gfx::Image win;
  for (int layer = fromLayer; layer < toLayer; layer++) {
    bool any = false;
    if (!exact) win = gfx::Image(sx1 - sx0, sy1 - sy0, 0);
    for (int ty = sy0 / T; ty <= (sy1 - 1) / T; ty++)
      for (int tx = sx0 / T; tx <= (sx1 - 1) / T; tx++) {
        auto img = rc->get(layer, z, tx, ty);
        if (!img) continue;
        any = true;
        const int bx = tx * T, by = ty * T;
        if (exact) {
          // Прямое наложение тайла уровня на выход: пиксель уровня = пиксель выхода.
          const int shiftX = int(std::lround(u0)), shiftY = int(std::lround(v0));
          blendImage(out, *img, bx - shiftX, by - shiftY, gfx::RectI(ox0, oy0, ox1 - ox0, oy1 - oy0));
          continue;
        }
        const int x0 = std::max(bx, sx0), x1 = std::min(bx + img->w, sx1);
        const int y0 = std::max(by, sy0), y1 = std::min(by + img->h, sy1);
        for (int y = y0; y < y1; y++) std::memcpy(win.row(y - sy0) + (x0 - sx0), img->row(y - by) + (x0 - bx), size_t(x1 - x0) * 4);
      }
    if (!any || exact) continue;
    resample(out, ox0, oy0, ox1, oy1, win, u0 + ox0 * r - sx0, v0 + oy0 * r - sy0, r, r, true);
  }
}

// ================================================================ наложение изображений
void blendImage(gfx::Image& dst, const gfx::Image& src, int dx, int dy, const gfx::RectI& clip, float opacity) {
  const gfx::RectI r = gfx::RectI(dx, dy, src.w, src.h).intersect(clip).intersect(gfx::RectI(0, 0, dst.w, dst.h));
  if (r.empty() || opacity <= 0) return;
  const u32 k = u32(std::lround(clamp(opacity, 0.f, 1.f) * 255));
  for (int y = r.y; y < r.bottom(); y++) {
    const u32* s = src.row(y - dy) + (r.x - dx);
    u32* d = dst.row(y) + r.x;
    if (k >= 255) {
      for (int x = 0; x < r.w; x++) {
        const u32 p = s[x];
        if (p >= 0xFF000000u) d[x] = p;
        else if (p) d[x] = over(p, d[x]);
      }
    } else {
      for (int x = 0; x < r.w; x++)
        if (const u32 p = s[x]) d[x] = over(mulPx(p, k), d[x]);
    }
  }
}

void copyImage(gfx::Image& dst, const gfx::Image& src, int dx, int dy, const gfx::RectI& clip) {
  const gfx::RectI r = gfx::RectI(dx, dy, src.w, src.h).intersect(clip).intersect(gfx::RectI(0, 0, dst.w, dst.h));
  if (r.empty()) return;
  for (int y = r.y; y < r.bottom(); y++) std::memcpy(dst.row(y) + r.x, src.row(y - dy) + (r.x - dx), size_t(r.w) * 4);
}

void scaleImage(gfx::Image& dst, const gfx::Image& src, double x0, double y0, double x1, double y1, const gfx::RectI& clip, bool opaque) {
  if (src.empty() || !(x1 > x0) || !(y1 > y0)) return;
  const int X0 = int(std::lround(x0)), X1 = int(std::lround(x1)), Y0 = int(std::lround(y0)), Y1 = int(std::lround(y1));
  const gfx::RectI r = gfx::RectI(X0, Y0, X1 - X0, Y1 - Y0).intersect(clip).intersect(gfx::RectI(0, 0, dst.w, dst.h));
  if (r.empty()) return;
  const double rx = src.w / (x1 - x0), ry = src.h / (y1 - y0);
  if (std::fabs(rx - 1) < 1e-9 && std::fabs(ry - 1) < 1e-9 && X0 == x0 && Y0 == y0) {
    if (opaque) copyImage(dst, src, X0, Y0, r);
    else blendImage(dst, src, X0, Y0, r);
    return;
  }
  resample(dst, r.x, r.y, r.right(), r.bottom(), src, (r.x - x0) * rx, (r.y - y0) * ry, rx, ry, !opaque);
}

}  // namespace rg::map::detail
