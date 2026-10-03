// Regnum — сглаживающий растеризатор: отсечение рёбер, ячейки cover/area, разреженные строки покрытия.
#include "gfx/raster.h"

#include "gfx/flatten.h"

namespace rg::gfx {

namespace {

constexpr u64 kMaxBandCells = u64(1) << 19;  // ≈ 8 МБ ячеек на полосу
constexpr i32 kNoCell = std::numeric_limits<i32>::min();

inline i32 fix(double v) { return i32(std::floor(v * 256.0 + 0.5)); }

// Площадь в единицах 1/(2·256·256) пикселя -> покрытие 0..255.
inline u32 alphaOf(i64 v, bool evenOdd) {
  if (v < 0) v = -v;
  if (evenOdd) {
    v &= 0x3FFFF;
    if (v > 0x20000) v = 0x40000 - v;
  } else if (v > 0x20000) {
    v = 0x20000;
  }
  return u32((v * 255 + 0x10000) >> 17);
}

struct RasterSink {
  Rasterizer& r;
  void move(Pt p) { r.moveTo(p.x, p.y); }
  void line(Pt p) { r.lineTo(p.x, p.y); }
  void end(bool) { r.close(); }
};

}  // namespace

void Rasterizer::reset(const RectI& clip) {
  clip_ = clip.empty() ? RectI{clip.x, clip.y, 0, 0} : clip;
  cx0_ = clip_.x; cy0_ = clip_.y; cx1_ = clip_.right(); cy1_ = clip_.bottom();
  edges_.clear();
  hasContour_ = false;
  invalid_ = false;
  pend_ = false;
  minX_ = minY_ = std::numeric_limits<i32>::max();
  maxX_ = maxY_ = std::numeric_limits<i32>::min();
}

RectI Rasterizer::bounds() const {
  if (edges_.empty() && !pend_) return {};
  i32 x0 = minX_, x1 = maxX_, y0 = minY_, y1 = maxY_;
  if (pend_) {
    x0 = std::min(x0, i32(cx0_) * 256);
    x1 = std::max(x1, i32(cx0_) * 256);
    y0 = std::min({y0, pendY0_, pendY1_});
    y1 = std::max({y1, pendY0_, pendY1_});
  }
  if (x0 > x1 || y0 > y1) return {};
  RectI r{x0 >> 8, y0 >> 8, ((x1 + 255) >> 8) - (x0 >> 8), ((y1 + 255) >> 8) - (y0 >> 8)};
  return r.intersect(clip_);
}

void Rasterizer::moveTo(float x, float y) {
  if (hasContour_) closeContour();
  if (!std::isfinite(x) || !std::isfinite(y)) { invalid_ = true; return; }
  sx_ = px_ = x;
  sy_ = py_ = y;
  hasContour_ = true;
}

void Rasterizer::lineTo(float x, float y) {
  if (!std::isfinite(x) || !std::isfinite(y)) { invalid_ = true; return; }
  if (!hasContour_) { moveTo(x, y); return; }
  if (!invalid_) edge(px_, py_, x, y);
  px_ = x;
  py_ = y;
}

void Rasterizer::close() {
  if (hasContour_) closeContour();
}

void Rasterizer::closeContour() {
  if (!invalid_ && (px_ != sx_ || py_ != sy_)) edge(px_, py_, sx_, sy_);
  px_ = sx_;
  py_ = sy_;
  hasContour_ = false;
}

void Rasterizer::addPath(const Path& p, const Affine& m, float tol) {
  RasterSink sink{*this};
  detail::CullBox cull{float(cx0_ - 1), float(cy0_ - 1), float(cx1_ + 1), float(cy1_ + 1)};
  detail::flattenPath(p, m, tol, sink, cull);
  if (hasContour_) closeContour();
}

void Rasterizer::addPolygon(const Pt* p, size_t n) {
  if (!p || n < 2) return;
  moveTo(p[0].x, p[0].y);
  for (size_t i = 1; i < n; i++) lineTo(p[i].x, p[i].y);
  close();
}

void Rasterizer::pushEdge(i32 x1, i32 y1, i32 x2, i32 y2) {
  if (y1 == y2) return;
  edges_.push_back({x1, y1, x2, y2});
  minX_ = std::min({minX_, x1, x2});
  maxX_ = std::max({maxX_, x1, x2});
  minY_ = std::min({minY_, y1, y2});
  maxY_ = std::max({maxY_, y1, y2});
}

// Вертикали на левой границе склеиваются: важен только итоговый сдвиг по y.
void Rasterizer::pushLeft(i32 y1, i32 y2) {
  if (y1 == y2) return;
  if (pend_ && pendY1_ == y1) { pendY1_ = y2; return; }
  flushLeft();
  pend_ = true;
  pendY0_ = y1;
  pendY1_ = y2;
}

void Rasterizer::flushLeft() {
  if (pend_ && pendY0_ != pendY1_) {
    i32 x = i32(cx0_) * 256;
    pushEdge(x, pendY0_, x, pendY1_);
  }
  pend_ = false;
}

void Rasterizer::edge(double x1, double y1, double x2, double y2) {
  if (y1 == y2) return;
  const double top = cy0_, bot = cy1_;
  if ((y1 <= top && y2 <= top) || (y1 >= bot && y2 >= bot)) return;
  const double dx = x2 - x1, dy = y2 - y1;
  double ax = x1, ay = y1, bx = x2, by = y2;
  if (ay < top) { ax = x1 + (top - y1) * dx / dy; ay = top; }
  else if (ay > bot) { ax = x1 + (bot - y1) * dx / dy; ay = bot; }
  if (by < top) { bx = x1 + (top - y1) * dx / dy; by = top; }
  else if (by > bot) { bx = x1 + (bot - y1) * dx / dy; by = bot; }
  if (ay == by) return;
  const double L = cx0_, R = cx1_;
  if (ax <= L && bx <= L) { pushLeft(fix(ay), fix(by)); return; }
  if (ax >= R && bx >= R) { maxX_ = std::max(maxX_, i32(R) * 256); return; }
  if (ax >= L && ax <= R && bx >= L && bx <= R) { pushEdge(fix(ax), fix(ay), fix(bx), fix(by)); return; }
  // Ребро пересекает левую и/или правую границу: делим на части.
  const double ex = bx - ax, ey = by - ay;
  double ts[4] = {0, 0, 0, 0};
  int nt = 1;
  double tl = (L - ax) / ex, tr = (R - ax) / ex;
  if (tl > 0 && tl < 1) ts[nt++] = tl;
  if (tr > 0 && tr < 1) ts[nt++] = tr;
  if (nt == 3 && ts[1] > ts[2]) std::swap(ts[1], ts[2]);
  ts[nt++] = 1;
  for (int i = 0; i + 1 < nt; i++) {
    const double t0 = ts[i], t1 = ts[i + 1];
    const double xa = i == 0 ? ax : ax + ex * t0, ya = i == 0 ? ay : ay + ey * t0;
    const double xb = i + 2 == nt ? bx : ax + ex * t1, yb = i + 2 == nt ? by : ay + ey * t1;
    const double xm = ax + ex * (t0 + t1) * 0.5;
    if (xm < L) pushLeft(fix(ya), fix(yb));
    else if (xm > R) maxX_ = std::max(maxX_, i32(R) * 256);
    else pushEdge(fix(std::clamp(xa, L, R)), fix(ya), fix(std::clamp(xb, L, R)), fix(yb));
  }
}

namespace {

#if defined(__GNUC__) || defined(__clang__)
#define RG_RASTER_INLINE inline __attribute__((always_inline))
#else
#define RG_RASTER_INLINE inline
#endif

using Cell = Rasterizer::Cell;

// Запись ячеек: текущая ячейка накапливается в регистрах, готовые пишутся в заранее выделенный буфер.
struct CellOut {
  Cell* wp;
  i32 cx, cy, cover, area;
  RG_RASTER_INLINE void add(i32 ex, i32 ey, i32 c, i32 a) {
    if (ex != cx || ey != cy) {
      if (cover | area) *wp++ = {cx, cy, cover, area};
      cx = ex;
      cy = ey;
      cover = 0;
      area = 0;
    }
    cover += c;
    area += a;
  }
  RG_RASTER_INLINE void flush() {
    if (cover | area) *wp++ = {cx, cy, cover, area};
    cover = area = 0;
  }
};

// Отрезок внутри одной строки ey; y1, y2 ∈ [0, 256] — доли строки.
// Пересечения с границами ячеек считаются от начала отрезка (без накопления ошибки) и округляются.
RG_RASTER_INLINE void hline(CellOut& o, i32 ey, i32 x1, i32 y1, i32 x2, i32 y2) {
  if (y1 == y2) return;
  const i32 ex1 = x1 >> 8, ex2 = x2 >> 8;
  const i32 fx1 = x1 & 255, fx2 = x2 & 255;
  const i32 dyTot = y2 - y1;
  if (ex1 == ex2) {
    o.add(ex1, ey, dyTot, (fx1 + fx2) * dyTot);
    return;
  }
  const double k = double(dyTot) / double(x2 - x1);
  const i32 lo = std::min(y1, y2), hi = std::max(y1, y2);
  // |Δy| ≤ 256: сдвиг на 512 делает округление усечением положительного числа.
  auto yAt = [&](i32 xb) {
    const i32 v = y1 + (i32(double(xb - x1) * k + 512.5) - 512);
    return v < lo ? lo : v > hi ? hi : v;
  };
  if (x2 > x1) {
    i32 xb = (ex1 + 1) << 8;
    i32 yc = yAt(xb);
    o.add(ex1, ey, yc - y1, (fx1 + 256) * (yc - y1));
    for (i32 ex = ex1 + 1; ex < ex2; ex++) {
      xb += 256;
      const i32 yn = yAt(xb), d = yn - yc;
      o.add(ex, ey, d, 256 * d);
      yc = yn;
    }
    const i32 d = y2 - yc;
    o.add(ex2, ey, d, fx2 * d);
  } else {
    i32 xb = ex1 << 8;
    i32 yc = yAt(xb);
    o.add(ex1, ey, yc - y1, fx1 * (yc - y1));
    for (i32 ex = ex1 - 1; ex > ex2; ex--) {
      xb -= 256;
      const i32 yn = yAt(xb), d = yn - yc;
      o.add(ex, ey, d, 256 * d);
      yc = yn;
    }
    const i32 d = y2 - yc;
    o.add(ex2, ey, d, (fx2 + 256) * d);
  }
}

// Отрезок в фиксированной точке 24.8: деление по строкам, затем по ячейкам.
RG_RASTER_INLINE void walkLine(CellOut& o, i32 x1, i32 y1, i32 x2, i32 y2) {
  const i32 ey1 = y1 >> 8, ey2 = y2 >> 8, fy1 = y1 & 255, fy2 = y2 & 255;
  if (ey1 == ey2) { hline(o, ey1, x1, fy1, x2, fy2); return; }
  if (x1 == x2) {
    // Вертикаль: площадь пропорциональна положению внутри пикселя.
    const i32 ex = x1 >> 8, twoFx = (x1 & 255) * 2;
    const bool down = y2 > y1;
    const i32 first = down ? 256 : 0, incr = down ? 1 : -1;
    i32 ey = ey1;
    i32 delta = first - fy1;
    o.add(ex, ey, delta, twoFx * delta);
    ey += incr;
    delta = first + first - 256;
    const i32 a = twoFx * delta;
    while (ey != ey2) {
      o.add(ex, ey, delta, a);
      ey += incr;
    }
    delta = fy2 - 256 + first;
    o.add(ex, ey, delta, twoFx * delta);
    return;
  }
  const double k = double(x2 - x1) / double(y2 - y1);
  const i32 lo = std::min(x1, x2), hi = std::max(x1, x2);
  // |Δx| < 2^30: сдвиг делает округление усечением положительного числа.
  auto xAt = [&](i32 yb) {
    const i64 v = i64(x1) + (i64(double(yb - y1) * k + 1073741824.5) - 1073741824);
    return i32(v < lo ? lo : v > hi ? hi : v);
  };
  if (y2 > y1) {
    i32 yb = (ey1 + 1) << 8;
    i32 xc = xAt(yb);
    hline(o, ey1, x1, fy1, xc, 256);
    for (i32 ey = ey1 + 1; ey < ey2; ey++) {
      yb += 256;
      const i32 xn = xAt(yb);
      hline(o, ey, xc, 0, xn, 256);
      xc = xn;
    }
    hline(o, ey2, xc, 0, x2, fy2);
  } else {
    i32 yb = ey1 << 8;
    i32 xc = xAt(yb);
    hline(o, ey1, x1, fy1, xc, 0);
    for (i32 ey = ey1 - 1; ey > ey2; ey--) {
      yb -= 256;
      const i32 xn = xAt(yb);
      hline(o, ey, xc, 256, xn, 0);
      xc = xn;
    }
    hline(o, ey2, xc, 256, x2, fy2);
  }
}

// Верхняя оценка числа ячеек ребра: пересечения строк и столбцов плюс концы.
inline u64 cellBound(i32 x1, i32 y1, i32 x2, i32 y2) {
  return u64(std::abs(i64(x2) - x1) >> 8) + u64(std::abs(i64(y2) - y1) >> 8) + 3;
}

}  // namespace

// Сортировка длинной строки ячеек по x: поразрядная (младшие 8 бит, затем старшие) — устойчивая, O(n).
void Rasterizer::sortRowX(Cell* c, Cell* e) {
  const size_t n = size_t(e - c);
  i32 lo = c->x, hi = c->x;
  for (Cell* i = c; i < e; i++) { lo = std::min(lo, i->x); hi = std::max(hi, i->x); }
  const i64 range = i64(hi) - lo;
  if (range >= 65536) {
    std::stable_sort(c, e, [](const Cell& a, const Cell& b) { return a.x < b.x; });
    return;
  }
  if (radixTmp_.size() < n) radixTmp_.resize(n);
  Cell* tmp = radixTmp_.data();
  const int passes = range < 256 ? 1 : 2;
  Cell* src = c;
  Cell* dst = tmp;
  for (int pass = 0; pass < passes; pass++) {
    const int sh = pass * 8;
    u32 cnt[257] = {};
    for (size_t i = 0; i < n; i++) cnt[((u32(src[i].x - lo) >> sh) & 255) + 1]++;
    for (int k = 0; k < 256; k++) cnt[k + 1] += cnt[k];
    for (size_t i = 0; i < n; i++) dst[cnt[(u32(src[i].x - lo) >> sh) & 255]++] = src[i];
    std::swap(src, dst);
  }
  if (src != c) std::copy(src, src + n, c);
}

void Rasterizer::sweep(FillRule rule, SpanSink& sink) {
  if (hasContour_) closeContour();
  flushLeft();
  if (invalid_ || edges_.empty() || clip_.empty()) return;
  const int row0 = std::max(clip_.y, minY_ >> 8);
  const int row1 = std::min(clip_.bottom(), (maxY_ + 255) >> 8);
  if (row0 >= row1) return;
  u64 est = 0;
  for (const Edge& e : edges_) est += cellBound(e.x1, e.y1, e.x2, e.y2);
  const int rows = row1 - row0;
  const int bands = int(std::min<u64>(u64(rows), est / kMaxBandCells + 1));
  const int bandH = (rows + bands - 1) / bands;
  const size_t w = size_t(clip_.w);
  if (covs_.size() < w + 1) covs_.resize(w + 1);
  if (spans_.size() < 2 * w + 4) spans_.resize(2 * w + 4);
  for (int b = row0; b < row1; b += bandH) sweepBand(b, std::min(row1, b + bandH), rule == FillRule::EvenOdd, sink);
}

void Rasterizer::sweepBand(int row0, int row1, bool evenOdd, SpanSink& sink) {
  const i32 Y0 = row0 * 256, Y1 = row1 * 256;
  // Оценка числа ячеек полосы — буфер выделяется один раз, запись без проверок ёмкости.
  u64 est = 1;
  for (const Edge& e : edges_) {
    const i32 lo = std::min(e.y1, e.y2), hi = std::max(e.y1, e.y2);
    if (hi > Y0 && lo < Y1) est += cellBound(e.x1, e.y1, e.x2, e.y2);
  }
  if (cells_.size() < est) cells_.resize(size_t(est + est / 4));
  CellOut o{cells_.data(), kNoCell, kNoCell, 0, 0};
  for (const Edge& e : edges_) {
    const i32 lo = std::min(e.y1, e.y2), hi = std::max(e.y1, e.y2);
    if (hi <= Y0 || lo >= Y1) continue;
    if (lo >= Y0 && hi <= Y1) { walkLine(o, e.x1, e.y1, e.x2, e.y2); continue; }
    // Часть ребра в полосе; точка разреза одинакова для соседних полос.
    auto xAt = [&](i32 y) {
      return e.x1 + i32(std::floor(double(y - e.y1) * double(e.x2 - e.x1) / double(e.y2 - e.y1) + 0.5));
    };
    i32 xa = e.x1, ya = e.y1, xb = e.x2, yb = e.y2;
    if (ya < Y0) { xa = xAt(Y0); ya = Y0; } else if (ya > Y1) { xa = xAt(Y1); ya = Y1; }
    if (yb < Y0) { xb = xAt(Y0); yb = Y0; } else if (yb > Y1) { xb = xAt(Y1); yb = Y1; }
    if (ya != yb) walkLine(o, xa, ya, xb, yb);
  }
  o.flush();
  const size_t ncells = size_t(o.wp - cells_.data());
  if (ncells == 0) return;

  // Сортировка: подсчётом по строкам, внутри строки — по x.
  const int rows = row1 - row0;
  rowStart_.assign(size_t(rows) + 1, 0);
  const Cell* cells = cells_.data();
  for (size_t i = 0; i < ncells; i++) {
    const i32 y = cells[i].y;
    if (y >= row0 && y < row1) rowStart_[size_t(y - row0) + 1]++;
  }
  for (int r = 0; r < rows; r++) rowStart_[size_t(r) + 1] += rowStart_[size_t(r)];
  rowPos_.assign(rowStart_.begin(), rowStart_.end() - 1);
  if (sorted_.size() < size_t(rowStart_[size_t(rows)])) sorted_.resize(size_t(rowStart_[size_t(rows)]));
  {
    Cell* out = sorted_.data();
    i32* pos = rowPos_.data();
    for (size_t i = 0; i < ncells; i++) {
      const i32 y = cells[i].y;
      if (y >= row0 && y < row1) out[pos[y - row0]++] = cells[i];
    }
  }

  const i32 clipX0 = clip_.x, clipX1 = clip_.right();
  u8* const covs = covs_.data();
  Span* const spans = spans_.data();
  for (int r = 0; r < rows; r++) {
    Cell* c = sorted_.data() + rowStart_[size_t(r)];
    Cell* const e = sorted_.data() + rowStart_[size_t(r) + 1];
    if (c == e) continue;
    if (e - c <= 16) {
      for (Cell* i = c + 1; i < e; i++) {
        Cell v = *i;
        Cell* j = i;
        while (j > c && (j - 1)->x > v.x) { *j = *(j - 1); --j; }
        *j = v;
      }
    } else {
      sortRowX(c, e);
    }
    Span* sp = spans;
    u8* cp = covs;
    i32 cover = 0;
    while (c < e) {
      const i32 x = c->x;
      i64 area = c->area;  // сумма по пикселю: много рёбер через один пиксель не переполняет
      cover += c->cover;
      ++c;
      while (c < e && c->x == x) { area += c->area; cover += c->cover; ++c; }
      if (x >= clipX1) break;
      if (x >= clipX0) {
        const u32 a = alphaOf((i64(cover) << 9) - area, evenOdd);
        if (a) {
          if (sp != spans && sp[-1].cov && sp[-1].x + sp[-1].len == x) {
            *cp++ = u8(a);
            sp[-1].len++;
          } else {
            *sp++ = Span{x, 1, cp, 0};
            *cp++ = u8(a);
          }
        }
      }
      if (cover != 0) {
        const i32 nx = std::min(c < e ? c->x : clipX1, clipX1);
        const i32 sx = std::max(x + 1, clipX0);
        if (nx > sx) {
          const u32 a = alphaOf(i64(cover) << 9, evenOdd);
          if (a) *sp++ = Span{sx, nx - sx, nullptr, u8(a)};
        }
      }
    }
    if (sp != spans) sink.row(row0 + r, spans, int(sp - spans));
  }
}

void rasterizeToMask(const Path& p, const Affine& xf, Mask& m, int ox, int oy, FillRule rule, float tol) {
  std::fill(m.a.begin(), m.a.end(), u8(0));
  if (m.empty()) return;
  thread_local Rasterizer ras;
  ras.reset({ox, oy, m.w, m.h});
  ras.addPath(p, xf, tol);
  struct Sink final : SpanSink {
    Mask& m;
    int ox, oy;
    Sink(Mask& m_, int x, int y) : m(m_), ox(x), oy(y) {}
    void row(int y, const Span* s, int n) override {
      u8* d = m.row(y - oy);
      for (int i = 0; i < n; i++) {
        if (s[i].cov) std::memcpy(d + (s[i].x - ox), s[i].cov, size_t(s[i].len));
        else std::memset(d + (s[i].x - ox), s[i].value, size_t(s[i].len));
      }
    }
  } sink(m, ox, oy);
  ras.sweep(rule, sink);
}

double coverageSum(const Mask& m) {
  u64 s = 0;
  for (u8 v : m.a) s += v;
  return double(s) / 255.0;
}

}  // namespace rg::gfx
