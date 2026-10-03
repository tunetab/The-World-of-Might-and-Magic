// Regnum — геометрические примитивы: точные предикаты, кольца, polylabel, упрощение.
#include "geo/geom.h"

#include <queue>

#include "geo/grid.h"

namespace rg::geo {

// ================================================================ точная арифметика
namespace {

// x + y = a + b точно.
inline void twoSum(double a, double b, double& x, double& y) {
  x = a + b;
  double bv = x - a;
  double av = x - bv;
  double br = b - bv;
  double ar = a - av;
  y = ar + br;
}

// Разбиение Деккера: a = hi + lo, у каждой половины не более 26 значащих битов.
constexpr double kSplitter = 134217729.0;  // 2^27 + 1
inline void splitD(double a, double& hi, double& lo) {
  double c = kSplitter * a;
  double big = c - a;
  hi = c - big;
  lo = a - hi;
}

// x + y = a * b точно (без FMA; отдельные операторы — сжатие в FMA компилятором не меняет результата).
inline void twoProd(double a, double b, double& x, double& y) {
  x = a * b;
  double ahi, alo, bhi, blo;
  splitD(a, ahi, alo);
  splitD(b, bhi, blo);
  double e1 = x - ahi * bhi;
  double e2 = e1 - alo * bhi;
  double e3 = e2 - ahi * blo;
  y = alo * blo - e3;
}

// Добавить b к разложению e (неперекрывающиеся компоненты по возрастанию модуля), нули отбрасываются.
int growExpansion(const double* e, int n, double b, double* h) {
  double q = b;
  int k = 0;
  for (int i = 0; i < n; i++) {
    double s, err;
    twoSum(q, e[i], s, err);
    q = s;
    if (err != 0) h[k++] = err;
  }
  if (q != 0 || k == 0) h[k++] = q;
  return k;
}

int orientExact(Vec2 a, Vec2 b, Vec2 c) {
  // det = ax*by − ay*bx + ay*cx − ax*cy + bx*cy − by*cx
  double t[12];
  twoProd(a.x, b.y, t[0], t[1]);
  twoProd(-a.y, b.x, t[2], t[3]);
  twoProd(a.y, c.x, t[4], t[5]);
  twoProd(-a.x, c.y, t[6], t[7]);
  twoProd(b.x, c.y, t[8], t[9]);
  twoProd(-b.y, c.x, t[10], t[11]);
  double e[16], h[16];
  int n = 0;
  for (double v : t) {
    if (v == 0) continue;
    n = growExpansion(e, n, v, h);
    std::copy(h, h + n, e);
  }
  for (int i = n - 1; i >= 0; i--)
    if (e[i] != 0) return e[i] > 0 ? 1 : -1;
  return 0;
}

inline int sgn(double v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

}  // namespace

int orient(Vec2 a, Vec2 b, Vec2 c) {
  // Быстрая оценка Шевчука с гарантированной границей ошибки.
  constexpr double kErr = (3.0 + 16.0 * 1.1102230246251565e-16) * 1.1102230246251565e-16;
  double detleft = (a.x - c.x) * (b.y - c.y);
  double detright = (a.y - c.y) * (b.x - c.x);
  double det = detleft - detright;
  double detsum;
  if (detleft > 0) {
    if (detright <= 0) return sgn(det);
    detsum = detleft + detright;
  } else if (detleft < 0) {
    if (detright >= 0) return sgn(det);
    detsum = -detleft - detright;
  } else {
    return sgn(det);
  }
  double bound = kErr * detsum;
  if (det >= bound || -det >= bound) return sgn(det);
  if (!std::isfinite(det)) return 0;
  return orientExact(a, b, c);
}

// ================================================================ отрезки
namespace {
SegRel collinearRel(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
  bool p1 = a == b, p2 = c == d;
  if (p1 && p2) return a == c ? SegRel::Touch : SegRel::None;
  Vec2 dir = !p1 ? b - a : d - c;
  bool useX = std::fabs(dir.x) >= std::fabs(dir.y);
  auto k = [&](Vec2 p) { return useX ? p.x : p.y; };
  double lo1 = std::min(k(a), k(b)), hi1 = std::max(k(a), k(b));
  double lo2 = std::min(k(c), k(d)), hi2 = std::max(k(c), k(d));
  double lo = std::max(lo1, lo2), hi = std::min(hi1, hi2);
  if (lo > hi) return SegRel::None;
  if (lo == hi || p1 || p2) return SegRel::Touch;
  return SegRel::Overlap;
}
}  // namespace

SegRel segRelation(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
  if (std::max(a.x, b.x) < std::min(c.x, d.x) || std::max(c.x, d.x) < std::min(a.x, b.x) ||
      std::max(a.y, b.y) < std::min(c.y, d.y) || std::max(c.y, d.y) < std::min(a.y, b.y))
    return SegRel::None;
  int o1 = orient(a, b, c), o2 = orient(a, b, d), o3 = orient(c, d, a), o4 = orient(c, d, b);
  if (o1 == 0 && o2 == 0 && o3 == 0 && o4 == 0) return collinearRel(a, b, c, d);
  if ((o1 > 0 && o2 > 0) || (o1 < 0 && o2 < 0) || (o3 > 0 && o4 > 0) || (o3 < 0 && o4 < 0)) return SegRel::None;
  if (o1 != 0 && o2 != 0 && o3 != 0 && o4 != 0) return SegRel::Proper;
  return SegRel::Touch;
}

bool onSegment(Vec2 p, Vec2 a, Vec2 b) {
  if (p.x < std::min(a.x, b.x) || p.x > std::max(a.x, b.x) || p.y < std::min(a.y, b.y) || p.y > std::max(a.y, b.y))
    return false;
  return orient(a, b, p) == 0;
}

Vec2 crossPoint(Vec2 a, Vec2 b, Vec2 c, Vec2 d, double* t, double* u) {
  Vec2 r = b - a, s = d - c;
  double den = r.cross(s);
  if (den == 0 || !std::isfinite(den)) {
    if (t) *t = 0;
    if (u) *u = 0;
    return a;
  }
  Vec2 ca = c - a;
  double tt = clamp(ca.cross(s) / den, 0.0, 1.0);
  double uu = clamp(ca.cross(r) / den, 0.0, 1.0);
  if (t) *t = tt;
  if (u) *u = uu;
  return a + r * tt;
}

Proj project(Vec2 p, Vec2 a, Vec2 b) {
  Vec2 d = b - a;
  double l2 = d.len2();
  Proj r;
  r.t = l2 > 0 ? clamp((p - a).dot(d) / l2, 0.0, 1.0) : 0.0;
  r.p = r.t <= 0 ? a : (r.t >= 1 ? b : a + d * r.t);
  r.d2 = dist2(p, r.p);
  return r;
}

double distToSeg2(Vec2 p, Vec2 a, Vec2 b) { return project(p, a, b).d2; }

double segSegDist2(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
  if (segRelation(a, b, c, d) != SegRel::None) return 0;
  return std::min({distToSeg2(a, c, d), distToSeg2(b, c, d), distToSeg2(c, a, b), distToSeg2(d, a, b)});
}

// ================================================================ кольца
double signedArea(const std::vector<Vec2>& ring) {
  size_t n = ring.size();
  if (n < 3) return 0;
  Vec2 o = ring[0];
  double s = 0;
  for (size_t i = 1; i + 1 < n; i++) s += (ring[i] - o).cross(ring[i + 1] - o);
  return s * 0.5;
}

double polygonArea(const std::vector<std::vector<Vec2>>& rings) {
  if (rings.empty()) return 0;
  double a = std::fabs(signedArea(rings[0]));
  for (size_t i = 1; i < rings.size(); i++) a -= std::fabs(signedArea(rings[i]));
  return a;
}

Box2 bounds(const std::vector<Vec2>& pts) {
  Box2 b;
  for (auto& p : pts) b.add(p);
  return b;
}

Vec2 ringCentroid(const std::vector<Vec2>& ring) {
  size_t n = ring.size();
  if (n == 0) return {};
  Vec2 o = ring[0];
  double a = 0, cx = 0, cy = 0;
  for (size_t i = 1; i + 1 < n; i++) {
    Vec2 p = ring[i] - o, q = ring[i + 1] - o;
    double c = p.cross(q);
    a += c;
    cx += (p.x + q.x) * c;
    cy += (p.y + q.y) * c;
  }
  if (std::fabs(a) > 1e-300) return {o.x + cx / (3 * a), o.y + cy / (3 * a)};
  Vec2 s;
  for (auto& p : ring) s += p;
  return s / double(n);
}

bool pointInRing(Vec2 p, const std::vector<Vec2>& ring) {
  bool in = false;
  size_t n = ring.size();
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    Vec2 a = ring[j], b = ring[i];
    if ((a.y > p.y) != (b.y > p.y)) {
      double x = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
      if (x > p.x) in = !in;
    }
  }
  return in;
}

bool pointInPolygon(Vec2 p, const std::vector<std::vector<Vec2>>& rings) {
  bool in = false;
  for (auto& r : rings)
    if (pointInRing(p, r)) in = !in;
  return in;
}

double distToRings(Vec2 p, const std::vector<std::vector<Vec2>>& rings) {
  double best = kInf;
  for (auto& r : rings) {
    size_t n = r.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) best = std::min(best, distToSeg2(p, r[j], r[i]));
  }
  return std::sqrt(best);
}

double signedDist(Vec2 p, const std::vector<std::vector<Vec2>>& rings) {
  double d = distToRings(p, rings);
  return pointInPolygon(p, rings) ? d : -d;
}

bool isSimple(const std::vector<Vec2>& pts, bool closed) {
  size_t n = pts.size();
  if (n < 2) return n == 1 && !closed;
  for (auto& p : pts)
    if (!finite(p)) return false;
  size_t ns = closed ? n : n - 1;
  if (closed && n < 3) return false;
  std::vector<Vec2> a(ns), b(ns);
  for (size_t i = 0; i < ns; i++) {
    a[i] = pts[i];
    b[i] = pts[(i + 1) % n];
    if (a[i] == b[i]) return false;
  }
  auto adjacent = [&](size_t i, size_t j) {  // i < j
    return j == i + 1 || (closed && i == 0 && j == ns - 1);
  };
  auto check = [&](size_t i, size_t j) {
    SegRel r = segRelation(a[i], b[i], a[j], b[j]);
    if (r == SegRel::None) return true;
    return adjacent(i, j) && r == SegRel::Touch;
  };
  if (ns <= 48) {
    for (size_t i = 0; i < ns; i++)
      for (size_t j = i + 1; j < ns; j++)
        if (!check(i, j)) return false;
    return true;
  }
  SegGrid g;
  g.build(a, b);
  std::vector<u32> stamp(ns, 0);
  for (size_t i = 0; i < ns; i++) {
    Box2 q;
    q.add(a[i]);
    q.add(b[i]);
    bool ok = true;
    g.queryUnique(q, stamp, u32(i + 1), [&](int j) {
      if (size_t(j) <= i || !ok) return;
      if (!check(i, size_t(j))) ok = false;
    });
    if (!ok) return false;
  }
  return true;
}

// ================================================================ polylabel
namespace {

// Знаковое расстояние до границы многоугольника с дырами; для крупных — через сетку.
class SignedDistFn {
 public:
  explicit SignedDistFn(const std::vector<std::vector<Vec2>>& rings) {
    for (auto& r : rings) {
      size_t n = r.size();
      if (n < 2) continue;
      for (size_t i = 0, j = n - 1; i < n; j = i++) {
        sa_.push_back(r[j]);
        sb_.push_back(r[i]);
      }
    }
    if (sa_.size() > 192) {
      grid_.build(sa_, sb_, {}, 0, 3.0);
      useGrid_ = true;
    }
  }

  double operator()(Vec2 p) const {
    if (!useGrid_) {
      bool in = false;
      double best = kInf;
      for (size_t i = 0, n = sa_.size(); i < n; i++) {
        Vec2 a = sa_[i], b = sb_[i];
        if ((a.y > p.y) != (b.y > p.y)) {
          double x = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
          if (x > p.x) in = !in;
        }
        double d2 = distToSeg2(p, a, b);
        if (d2 < best) best = d2;
      }
      double d = std::sqrt(best);
      return in ? d : -d;
    }
    // Чётность пересечений луча вправо: пересечение считается в той ячейке, где лежит его x.
    bool in = false;
    int col = p.x < grid_.box().x0 ? 0 : grid_.cellX(p.x);
    grid_.rayRight(
        p,
        [&](int i) {
          Vec2 a = sa_[size_t(i)], b = sb_[size_t(i)];
          if ((a.y > p.y) != (b.y > p.y)) {
            double x = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (x > p.x && grid_.cellX(x) == col) in = !in;
          }
        },
        [&](double) {
          col++;
          return false;
        });
    double dist = 0;
    grid_.nearest(p, kInf, &dist);
    return in ? dist : -dist;
  }

 private:
  std::vector<Vec2> sa_, sb_;
  SegGrid grid_;
  bool useGrid_ = false;
};

struct PlCell {
  Vec2 c;
  double h = 0, d = 0, max = 0;
  bool operator<(const PlCell& o) const { return max < o.max; }
};

}  // namespace

Vec2 polylabel(const std::vector<std::vector<Vec2>>& rings, double precision, double* outDist) {
  if (outDist) *outDist = 0;
  if (rings.empty() || rings[0].empty()) return {};
  const auto& outer = rings[0];
  Box2 bb = bounds(outer);
  double w = bb.w(), h = bb.h();
  double cs = std::min(w, h);
  if (!(cs > 0) || outer.size() < 3) return outer[0];
  precision = std::max(precision, cs * 1e-7);
  SignedDistFn f(rings);
  auto mk = [&](Vec2 c, double hh) {
    PlCell x;
    x.c = c;
    x.h = hh;
    x.d = f(c);
    x.max = x.d + hh * 1.4142135623730951;
    return x;
  };
  std::priority_queue<PlCell> q;
  double hh = cs * 0.5;
  for (double x = bb.x0; x < bb.x1; x += cs)
    for (double y = bb.y0; y < bb.y1; y += cs) q.push(mk({x + hh, y + hh}, hh));
  PlCell best = mk(ringCentroid(outer), 0);
  PlCell bc = mk(bb.center(), 0);
  if (bc.d > best.d) best = bc;
  int probes = 0;
  while (!q.empty() && probes < 200000) {
    PlCell c = q.top();
    q.pop();
    if (c.d > best.d) best = c;
    if (c.max - best.d <= precision) continue;
    double h2 = c.h * 0.5;
    q.push(mk({c.c.x - h2, c.c.y - h2}, h2));
    q.push(mk({c.c.x + h2, c.c.y - h2}, h2));
    q.push(mk({c.c.x - h2, c.c.y + h2}, h2));
    q.push(mk({c.c.x + h2, c.c.y + h2}, h2));
    probes += 4;
  }
  if (best.d <= 0) {
    // Очень тонкий многоугольник: точка у середины самого длинного звена со стороны внутренности.
    size_t n = outer.size(), bi = 0;
    double bl = -1;
    for (size_t i = 0; i < n; i++) {
      double l = dist2(outer[i], outer[(i + 1) % n]);
      if (l > bl) { bl = l; bi = i; }
    }
    Vec2 a = outer[bi], b = outer[(bi + 1) % n], m = (a + b) * 0.5;
    Vec2 nrm = (b - a).perp().norm();
    for (double eps = std::sqrt(bl) * 1e-3; eps > 1e-9; eps *= 0.1) {
      for (double s : {1.0, -1.0}) {
        Vec2 p = m + nrm * (eps * s);
        double d = f(p);
        if (d > best.d) { best.c = p; best.d = d; }
      }
      if (best.d > 0) break;
    }
  }
  if (outDist) *outDist = best.d;
  return best.c;
}

// ================================================================ упрощение
namespace {

// Дуглас — Пекер на отрезке [i, j] массива pts: отмечает сохраняемые точки.
void dpRange(const std::vector<Vec2>& pts, size_t i0, size_t j0, double tol2, std::vector<char>& keep) {
  std::vector<std::pair<size_t, size_t>> st{{i0, j0}};
  while (!st.empty()) {
    auto [i, j] = st.back();
    st.pop_back();
    if (j <= i + 1) continue;
    double best = -1;
    size_t bk = i;
    for (size_t k = i + 1; k < j; k++) {
      double d = distToSeg2(pts[k], pts[i], pts[j]);
      if (d > best) { best = d; bk = k; }
    }
    if (best > tol2) {
      keep[bk] = 1;
      st.push_back({i, bk});
      st.push_back({bk, j});
    }
  }
}

// Флаги сохранения точек по Дугласу — Пекеру (для кольца — без повторения первой точки).
std::vector<char> dpFlags(const std::vector<Vec2>& pts, double tol, bool closed) {
  size_t n = pts.size();
  std::vector<char> keep(n, 0);
  if (n == 0) return keep;
  double tol2 = tol * tol;
  if (!closed) {
    keep[0] = 1;
    keep[n - 1] = 1;
    if (n > 2) dpRange(pts, 0, n - 1, tol2, keep);
    return keep;
  }
  if (n <= 3) {
    std::fill(keep.begin(), keep.end(), 1);
    return keep;
  }
  size_t far = 0;
  double fd = -1;
  for (size_t i = 1; i < n; i++) {
    double d = dist2(pts[i], pts[0]);
    if (d > fd) { fd = d; far = i; }
  }
  std::vector<Vec2> ext(pts);
  ext.push_back(pts[0]);
  std::vector<char> k2(n + 1, 0);
  k2[0] = k2[far] = k2[n] = 1;
  dpRange(ext, 0, far, tol2, k2);
  dpRange(ext, far, n, tol2, k2);
  for (size_t i = 0; i < n; i++) keep[i] = k2[i];
  int cnt = 0;
  for (char c : keep) cnt += c;
  if (cnt < 3) {
    double best = -1;
    size_t bk = 0;
    for (size_t i = 0; i < n; i++) {
      if (keep[i]) continue;
      double d = distToSeg2(pts[i], pts[0], pts[far]);
      if (d > best) { best = d; bk = i; }
    }
    keep[bk] = 1;
  }
  return keep;
}

}  // namespace

std::vector<Vec2> simplifyDP(const std::vector<Vec2>& pts, double tol, bool closed) {
  if (pts.size() < 3) return pts;
  auto keep = dpFlags(pts, tol, closed);
  std::vector<Vec2> out;
  for (size_t i = 0; i < pts.size(); i++)
    if (keep[i]) out.push_back(pts[i]);
  return out;
}

std::vector<std::vector<Vec2>> simplifyTopo(const std::vector<std::vector<Vec2>>& lines, const std::vector<bool>& closed,
                                            double tol) {
  size_t L = lines.size();
  auto isClosed = [&](size_t i) { return i < closed.size() && closed[i]; };
  std::vector<std::vector<char>> keep(L);
  for (size_t i = 0; i < L; i++) keep[i] = dpFlags(lines[i], tol, isClosed(i));

  struct S { u32 line, i0, i1; };  // i1 может быть = n (замыкание кольца на точку 0)
  for (int iter = 0; iter < 10000; iter++) {
    std::vector<S> segs;
    std::vector<Vec2> sa, sb;
    std::vector<Vec2> verts;           // оставшиеся вершины
    std::vector<std::pair<u32, u32>> vref;
    for (size_t l = 0; l < L; l++) {
      const auto& pts = lines[l];
      size_t n = pts.size();
      if (n < 2) continue;
      std::vector<u32> ks;
      for (size_t k = 0; k < n; k++)
        if (keep[l][k]) ks.push_back(u32(k));
      for (u32 k : ks) {
        verts.push_back(pts[k]);
        vref.push_back({u32(l), k});
      }
      for (size_t s = 0; s + 1 < ks.size(); s++) segs.push_back({u32(l), ks[s], ks[s + 1]});
      if (isClosed(l) && ks.size() >= 2) segs.push_back({u32(l), ks.back(), u32(n)});
    }
    auto P = [&](u32 l, u32 k) { const auto& p = lines[l]; return p[k % p.size()]; };
    for (auto& s : segs) {
      sa.push_back(P(s.line, s.i0));
      sb.push_back(P(s.line, s.i1));
    }
    std::vector<char> bad(segs.size(), 0);
    // 1) пересечения и касания упрощённых звеньев
    SegGrid g;
    g.build(sa, sb);
    std::vector<u32> stamp(segs.size(), 0);
    auto lineEnd = [&](u32 l, u32 k) {
      return !isClosed(l) && (k == 0 || k + 1 == lines[l].size());
    };
    for (size_t i = 0; i < segs.size(); i++) {
      Box2 q;
      q.add(sa[i]);
      q.add(sb[i]);
      g.queryUnique(q, stamp, u32(i + 1), [&](int jj) {
        size_t j = size_t(jj);
        if (j <= i) return;
        SegRel r = segRelation(sa[i], sb[i], sa[j], sb[j]);
        if (r == SegRel::None) return;
        const S &x = segs[i], &y = segs[j];
        if (r == SegRel::Touch) {
          // соседние звенья одной линии
          if (x.line == y.line) {
            size_t n = lines[x.line].size();
            bool adj = x.i1 % n == y.i0 % n || y.i1 % n == x.i0 % n;
            if (adj) {
              // общая только вершина стыка (два звена кольца из двух точек — не допускаются)
              bool both = (x.i1 % n == y.i0 % n) && (y.i1 % n == x.i0 % n);
              if (!both) return;
            }
          } else {
            // концы разных линий в одной точке (дуги, сходящиеся в узле)
            Vec2 pts4[4] = {sa[i], sb[i], sa[j], sb[j]};
            u32 ks[4] = {x.i0, x.i1, y.i0, y.i1};
            u32 ls[4] = {x.line, x.line, y.line, y.line};
            bool ok = false;
            for (int u = 0; u < 2; u++)
              for (int v = 2; v < 4; v++)
                if (pts4[u] == pts4[v] && lineEnd(ls[u], ks[u]) && lineEnd(ls[v], ks[v])) ok = true;
            if (ok) return;
          }
        }
        bad[i] = 1;
        bad[j] = 1;
      });
    }
    // 2) вершины не должны попадать между исходным участком и упрощённым звеном
    SegGrid vg;
    vg.build(verts, verts);
    std::vector<u32> vstamp(verts.size(), 0);
    u32 tag = 0;
    for (size_t i = 0; i < segs.size(); i++) {
      if (bad[i]) continue;
      const S& s = segs[i];
      if (s.i1 <= s.i0 + 1) continue;
      std::vector<Vec2> poly;
      for (u32 k = s.i0; k <= s.i1; k++) poly.push_back(P(s.line, k));
      Box2 pb = bounds(poly);
      size_t n = lines[s.line].size();
      tag++;
      vg.queryUnique(pb, vstamp, tag, [&](int vi) {
        if (bad[i]) return;
        auto [vl, vk] = vref[size_t(vi)];
        if (vl == s.line && (vk == s.i0 % n || vk == s.i1 % n)) return;
        Vec2 v = verts[size_t(vi)];
        if (!pb.contains(v)) return;
        if (v == poly.front() || v == poly.back()) return;  // общий узел линий
        if (pointInRing(v, poly)) bad[i] = 1;
      });
    }
    // 3) уточнение: добавить самую дальнюю точку участка
    bool changed = false;
    for (size_t i = 0; i < segs.size(); i++) {
      if (!bad[i]) continue;
      const S& s = segs[i];
      if (s.i1 <= s.i0 + 1) continue;
      Vec2 a = P(s.line, s.i0), b = P(s.line, s.i1);
      double best = -1;
      u32 bk = s.i0 + 1;
      for (u32 k = s.i0 + 1; k < s.i1; k++) {
        double d = distToSeg2(P(s.line, k), a, b);
        if (d > best) { best = d; bk = k; }
      }
      size_t n = lines[s.line].size();
      if (!keep[s.line][bk % n]) {
        keep[s.line][bk % n] = 1;
        changed = true;
      }
    }
    if (!changed) break;
  }
  std::vector<std::vector<Vec2>> out(L);
  for (size_t l = 0; l < L; l++)
    for (size_t k = 0; k < lines[l].size(); k++)
      if (keep[l][k]) out[l].push_back(lines[l][k]);
  return out;
}

}  // namespace rg::geo
