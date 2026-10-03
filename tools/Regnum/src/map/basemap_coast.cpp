// Regnum — береговая линия базовой карты: marching squares, упрощение с сохранением топологии, проверка, маска.
//
// Поле берега задано в центрах пикселей (x + 0,5; y + 0,5); за краем карты — море (255). Суша — значения ≤ 127,
// изолиния 127,5 не проходит через узлы сетки. Седловая клетка: морские вершины соединены, если сумма четырёх
// значений ≥ 510 (то же правило использует сегментация при поиске замкнутых участков моря).
// Точка пересечения на ребре отстоит от узлов не меньше чем на 0,05 пикселя: после округления до 0,01 разные
// участки контуров не сходятся. На ребре между краем и пикселем карты точка лежит на границе карты; точки рёбер
// крайних рядов и столбцов (ровно 0,5 пикселя от рамки) тоже кладутся на рамку — так же их «прилепило» бы ядро
// геометрии (geo::initFromCoast прилипает к рамке ближе 0,5 пикселя), и береговая линия совпадает с графом точно.
#include <cstring>

#include "base/jobs.h"
#include "geo/topo.h"
#include "map/basemap_build.h"

namespace rg::map::bake {

namespace {

// ---------------------------------------------------------------- таблица marching squares
// Рёбра клетки: 0 — верх, 1 — право, 2 — низ, 3 — лево. Углы: 0 — tl, 1 — tr, 2 — br, 3 — bl.
// next[case][join][вход] = выход; суша слева по ходу (на экране, ось Y вниз). join = 1 — седло соединяет море.
struct Table {
  i8 next[16][2][4];
  Table() {
    std::memset(next, -1, sizeof(next));
    const double mid[4][2] = {{0.5, 0}, {1, 0.5}, {0.5, 1}, {0, 0.5}};
    const double corner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const int edgeCorners[4][2] = {{0, 1}, {1, 2}, {3, 2}, {0, 3}};
    const int cornerEdges[4][2] = {{0, 3}, {0, 1}, {1, 2}, {2, 3}};
    for (int c = 0; c < 16; c++) {
      auto land = [&](int k) { return ((c >> k) & 1) != 0; };
      for (int join = 0; join < 2; join++) {
        struct S { int e1, e2, ref; };
        std::vector<S> segs;
        if (c == 5 || c == 10) {
          for (int k = 0; k < 4; k++) {
            // join = 1: отрезаются углы суши; join = 0: углы моря.
            if (land(k) == (join == 1)) segs.push_back({cornerEdges[k][0], cornerEdges[k][1], k});
          }
        } else {
          int e[2], n = 0;
          for (int k = 0; k < 4; k++)
            if (land(edgeCorners[k][0]) != land(edgeCorners[k][1])) e[n++] = k;
          if (n != 2) continue;
          int ref = 0;
          while (!land(ref)) ref++;
          segs.push_back({e[0], e[1], ref});
        }
        for (auto s : segs) {
          const double dx = mid[s.e2][0] - mid[s.e1][0], dy = mid[s.e2][1] - mid[s.e1][1];
          const double lx = corner[s.ref][0] - mid[s.e1][0], ly = corner[s.ref][1] - mid[s.e1][1];
          const double cr = dx * ly - dy * lx;  // < 0 — угол слева на экране
          const bool leftOk = land(s.ref) ? cr < 0 : cr > 0;
          if (leftOk) next[c][join][s.e1] = i8(s.e2);
          else next[c][join][s.e2] = i8(s.e1);
        }
      }
    }
  }
};
const Table& table() {
  static const Table t;
  return t;
}

// ---------------------------------------------------------------- точная геометрия на целых координатах
inline i64 cross(IPt a, IPt b, IPt c) { return i64(b.x - a.x) * (c.y - a.y) - i64(b.y - a.y) * (c.x - a.x); }
inline int orient(IPt a, IPt b, IPt c) {
  const i64 v = cross(a, b, c);
  return (v > 0) - (v < 0);
}
inline bool inBox(IPt p, IPt a, IPt b) {
  return p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) && p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y);
}

enum class Rel : u8 { None, Touch, Proper, Overlap };
Rel relation(IPt a, IPt b, IPt c, IPt d) {
  const int o1 = orient(a, b, c), o2 = orient(a, b, d), o3 = orient(c, d, a), o4 = orient(c, d, b);
  if (o1 == 0 && o2 == 0) {
    const bool useX = std::abs(i64(b.x) - a.x) >= std::abs(i64(b.y) - a.y);
    i64 a0 = useX ? a.x : a.y, a1 = useX ? b.x : b.y, c0 = useX ? c.x : c.y, c1 = useX ? d.x : d.y;
    if (a0 > a1) std::swap(a0, a1);
    if (c0 > c1) std::swap(c0, c1);
    const i64 lo = std::max(a0, c0), hi = std::min(a1, c1);
    if (lo < hi) return Rel::Overlap;
    return lo == hi ? Rel::Touch : Rel::None;
  }
  if (o1 * o2 < 0 && o3 * o4 < 0) return Rel::Proper;
  if ((o1 == 0 && inBox(c, a, b)) || (o2 == 0 && inBox(d, a, b)) || (o3 == 0 && inBox(a, c, d)) || (o4 == 0 && inBox(b, c, d)))
    return Rel::Touch;
  return Rel::None;
}

double segDist2(IPt a, IPt b, IPt c, IPt d) {
  auto pd = [](IPt p, IPt s, IPt e) {
    const double vx = e.x - s.x, vy = e.y - s.y, wx = p.x - s.x, wy = p.y - s.y;
    const double l2 = vx * vx + vy * vy;
    double t = l2 > 0 ? (vx * wx + vy * wy) / l2 : 0;
    t = clamp(t, 0.0, 1.0);
    const double dx = wx - t * vx, dy = wy - t * vy;
    return dx * dx + dy * dy;
  };
  return std::min(std::min(pd(a, c, d), pd(b, c, d)), std::min(pd(c, a, b), pd(d, a, b)));
}

// Расстояние² точки до отрезка (единицы координат).
inline double pointSeg2(IPt p, IPt s, IPt e) {
  const double vx = double(e.x) - s.x, vy = double(e.y) - s.y, wx = double(p.x) - s.x, wy = double(p.y) - s.y;
  const double l2 = vx * vx + vy * vy;
  double t = l2 > 0 ? (vx * wx + vy * wy) / l2 : 0;
  t = clamp(t, 0.0, 1.0);
  const double dx = wx - t * vx, dy = wy - t * vy;
  return dx * dx + dy * dy;
}

// Чёт-нечет, точно; точки на границе дают произвольный, но согласованный ответ.
bool pointInRing(IPt p, const IRing& r) {
  bool in = false;
  const size_t n = r.size();
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    const IPt a = r[j], b = r[i];
    if ((a.y > p.y) == (b.y > p.y)) continue;
    const i64 den = i64(b.y) - a.y, num = (i64(p.y) - a.y) * (i64(b.x) - a.x), lhs = (i64(p.x) - a.x) * den;
    if (den > 0 ? lhs < num : lhs > num) in = !in;
  }
  return in;
}

struct Box {
  i32 x0, y0, x1, y1;
  bool contains(IPt p) const { return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1; }
};
Box boxOf(const IRing& r) {
  Box b{r[0].x, r[0].y, r[0].x, r[0].y};
  for (auto p : r) {
    b.x0 = std::min(b.x0, p.x); b.y0 = std::min(b.y0, p.y);
    b.x1 = std::max(b.x1, p.x); b.y1 = std::max(b.y1, p.y);
  }
  return b;
}

// ---------------------------------------------------------------- сетка отрезков
struct Seg {
  IPt a, b;
  int ring, k, m;  // кольцо, номер отрезка, число отрезков кольца
};

class SegGrid {
 public:
  SegGrid(const std::vector<Seg>& segs, i32 inflate) : segs_(segs), inflate_(inflate) {
    i32 x0 = 0, y0 = 0, x1 = 1, y1 = 1;
    for (auto& s : segs) {
      x0 = std::min({x0, s.a.x, s.b.x}); y0 = std::min({y0, s.a.y, s.b.y});
      x1 = std::max({x1, s.a.x, s.b.x}); y1 = std::max({y1, s.a.y, s.b.y});
    }
    ox_ = x0 - inflate - 1;
    oy_ = y0 - inflate - 1;
    cols_ = int((i64(x1) + inflate + 1 - ox_) / kCell) + 1;
    rows_ = int((i64(y1) + inflate + 1 - oy_) / kCell) + 1;
    start_.assign(size_t(cols_) * size_t(rows_) + 1, 0);
    for (size_t i = 0; i < segs.size(); i++) forCells(i, [&](size_t c) { start_[c + 1]++; });
    for (size_t c = 1; c < start_.size(); c++) start_[c] += start_[c - 1];
    items_.resize(start_.back());
    std::vector<u32> pos(start_.begin(), start_.end() - 1);
    for (size_t i = 0; i < segs.size(); i++) forCells(i, [&](size_t c) { items_[pos[c]++] = u32(i); });
  }

  // f(i, j) для каждой пары отрезков с пересекающимися (расширенными) рамками — ровно один раз.
  template <class F>
  void pairs(F&& f) const {
    const size_t cells = size_t(cols_) * size_t(rows_);
    for (size_t c = 0; c < cells; c++) {
      const u32 b = start_[c], e = start_[c + 1];
      if (e - b < 2) continue;
      const int cx = int(c % size_t(cols_)), cy = int(c / size_t(cols_));
      for (u32 p = b; p < e; p++) {
        const Seg& s = segs_[items_[p]];
        const Box bs = box(s);
        for (u32 q = p + 1; q < e; q++) {
          const Seg& t = segs_[items_[q]];
          const Box bt = box(t);
          if (bs.x0 > bt.x1 || bt.x0 > bs.x1 || bs.y0 > bt.y1 || bt.y0 > bs.y1) continue;
          // Пара обрабатывается в клетке угла пересечения рамок.
          if (cellX(std::max(bs.x0, bt.x0)) != cx || cellY(std::max(bs.y0, bt.y0)) != cy) continue;
          f(items_[p], items_[q]);
        }
      }
    }
  }

 private:
  static constexpr i64 kCell = 800;  // 8 пикселей
  Box box(const Seg& s) const {
    return {std::min(s.a.x, s.b.x) - inflate_, std::min(s.a.y, s.b.y) - inflate_, std::max(s.a.x, s.b.x) + inflate_,
            std::max(s.a.y, s.b.y) + inflate_};
  }
  int cellX(i64 x) const { return int((x - ox_) / kCell); }
  int cellY(i64 y) const { return int((y - oy_) / kCell); }
  template <class F>
  void forCells(size_t i, F&& f) const {
    const Box b = box(segs_[i]);
    for (int y = cellY(b.y0); y <= cellY(b.y1); y++)
      for (int x = cellX(b.x0); x <= cellX(b.x1); x++) f(size_t(y) * size_t(cols_) + size_t(x));
  }
  const std::vector<Seg>& segs_;
  i32 inflate_;
  i64 ox_ = 0, oy_ = 0;
  int cols_ = 1, rows_ = 1;
  std::vector<u32> start_, items_;
};

bool adjacent(const Seg& s, const Seg& t) {
  if (s.ring != t.ring) return false;
  return (s.k + 1) % s.m == t.k || (t.k + 1) % t.m == s.k;
}

// Плохая пара: пересечение/касание несмежных или наложение смежных отрезков.
bool badPair(const Seg& s, const Seg& t) {
  const Rel r = relation(s.a, s.b, t.a, t.b);
  if (adjacent(s, t)) return r == Rel::Overlap || (s.m == 2);
  return r != Rel::None;
}

i64 area2(const IRing& r) {  // удвоенная площадь в единицах²
  i64 s = 0;
  for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) s += i64(r[j].x) * r[i].y - i64(r[i].x) * r[j].y;
  return s;
}

}  // namespace

double ringArea(const IRing& r) {
  if (r.size() < 3) return 0;
  return double(area2(r)) * 0.5 / double(kCoordScale * kCoordScale);
}

// ---------------------------------------------------------------- marching squares
std::vector<IRing> traceCoast(const u8* field, int w, int h, double minArea, CoastStats* stats) {
  const double t0 = nowSeconds();
  const Table& T = table();
  auto val = [&](int i, int j) -> int { return (i < 0 || j < 0 || i >= w || j >= h) ? 255 : field[size_t(j) * size_t(w) + size_t(i)]; };
  const size_t hw = size_t(w) + 1;  // горизонтальные рёбра строки j: i = −1 .. w − 1
  std::vector<u64> visited((hw * size_t(h) + 63) / 64, 0);
  auto markH = [&](int i, int j) { size_t k = size_t(j) * hw + size_t(i + 1); visited[k >> 6] |= u64(1) << (k & 63); };
  auto seenH = [&](int i, int j) { size_t k = size_t(j) * hw + size_t(i + 1); return (visited[k >> 6] >> (k & 63)) & 1; };

  auto cellCase = [&](int ci, int cj, int& join) {
    const int a = val(ci, cj), b = val(ci + 1, cj), c = val(ci + 1, cj + 1), d = val(ci, cj + 1);
    const int cs = (a < 128 ? 1 : 0) | (b < 128 ? 2 : 0) | (c < 128 ? 4 : 0) | (d < 128 ? 8 : 0);
    join = (a + b + c + d >= 510) ? 1 : 0;
    return cs;
  };
  auto lerpT = [](int a, int b) { return clamp((127.5 - a) / double(b - a), 0.05, 0.95); };
  // Точка пересечения на выходном ребре e клетки (ci, cj).
  auto point = [&](int ci, int cj, int e) -> IPt {
    double x, y;
    if (e == 0 || e == 2) {  // горизонтальное ребро H(ci, j)
      const int j = e == 0 ? cj : cj + 1;
      y = j == 0 ? 0.0 : (j == h - 1 ? double(h) : j + 0.5);
      if (ci < 0) x = 0;
      else if (ci + 1 >= w) x = w;
      else x = ci + 0.5 + lerpT(val(ci, j), val(ci + 1, j));
    } else {  // вертикальное ребро V(i, cj)
      const int i = e == 3 ? ci : ci + 1;
      x = i == 0 ? 0.0 : (i == w - 1 ? double(w) : i + 0.5);
      if (cj < 0) y = 0;
      else if (cj + 1 >= h) y = h;
      else y = cj + 0.5 + lerpT(val(i, cj), val(i, cj + 1));
    }
    return {i32(std::llround(x * kCoordScale)), i32(std::llround(y * kCoordScale))};
  };

  CoastStats st;
  std::vector<IRing> out;
  IRing ring;
  const i64 guard = 4 * (i64(w) + 2) * (i64(h) + 2);
  for (int j = 0; j < h; j++) {
    const u8* row = field + size_t(j) * size_t(w);
    for (int i = -1; i < w; i++) {
      const bool la = i >= 0 && row[i] < 128, lb = i + 1 < w && row[i + 1] < 128;
      if (la == lb || seenH(i, j)) continue;
      // Ребро H(i, j) — верх клетки (i, j) и низ клетки (i, j − 1).
      int ci = i, cj = j, entry = 0, join;
      int cs = cellCase(ci, cj, join);
      if (T.next[cs][join][0] < 0) {
        cj = j - 1;
        entry = 2;
        cs = cellCase(ci, cj, join);
        if (T.next[cs][join][2] < 0) throw std::logic_error("traceCoast: нет входа в клетку");
      }
      const int si = ci, sj = cj, se = entry;
      ring.clear();
      i64 steps = 0;
      do {
        cs = cellCase(ci, cj, join);
        const int ex = T.next[cs][join][entry];
        if (ex < 0 || ++steps > guard) throw std::logic_error("traceCoast: разрыв контура");
        const IPt p = point(ci, cj, ex);
        if (ring.empty() || !(ring.back() == p)) ring.push_back(p);
        switch (ex) {
          case 0: markH(ci, cj); cj--; entry = 2; break;
          case 1: ci++; entry = 3; break;
          case 2: markH(ci, cj + 1); cj++; entry = 0; break;
          default: ci--; entry = 1; break;
        }
      } while (ci != si || cj != sj || entry != se);
      while (ring.size() > 1 && ring.front() == ring.back()) ring.pop_back();
      st.rawRings++;
      st.rawPoints += i64(ring.size());
      if (ring.size() < 3) {
        st.islets++;
        continue;
      }
      const double a = ringArea(ring);
      if (a > 0) {  // суша слева на экране даёт отрицательную площадь; положительная — дыра
        st.holes++;
        continue;
      }
      if (-a < minArea) {
        st.islets++;
        continue;
      }
      std::reverse(ring.begin(), ring.end());
      out.push_back(ring);
    }
  }
  st.rings = int(out.size());
  for (auto& r : out) st.points += i64(r.size());
  st.seconds = nowSeconds() - t0;
  if (stats) {
    stats->rawRings = st.rawRings;
    stats->rawPoints = st.rawPoints;
    stats->islets = st.islets;
    stats->holes = st.holes;
    stats->rings = st.rings;
    stats->points = st.points;
    stats->seconds += st.seconds;
  }
  return out;
}

// ---------------------------------------------------------------- упрощение
std::vector<IRing> simplifyCoast(const std::vector<IRing>& rings, double tol, int w, int h, CoastStats* stats) {
  const i32 W = i32(w) * kCoordScale, H = i32(h) * kCoordScale;
  // Углы рамки остаются вершинами: иначе хорда у угла может пройти ближе 0,5 пикселя от него, и ядро геометрии
  // вставит угол в кольцо.
  auto fixedPt = [&](IPt p) { return (p.x == 0 || (w > 0 && p.x == W)) && (p.y == 0 || (h > 0 && p.y == H)); };
  const double t0 = nowSeconds();
  const double tol2 = (tol * kCoordScale) * (tol * kCoordScale);
  const size_t R = rings.size();
  std::vector<std::vector<int>> keep(R);

  // Самая удалённая от хорды точка участка (a, b) кольца; −1, если внутренних точек нет.
  auto farthest = [&](const IRing& r, int a, int b, double* d2) {
    const int n = int(r.size());
    int best = -1;
    double bd = -1;
    for (int k = a + 1; k < b; k++) {
      const double d = pointSeg2(r[size_t(k % n)], r[size_t(a % n)], r[size_t(b % n)]);
      if (d > bd) { bd = d; best = k; }
    }
    if (d2) *d2 = bd;
    return best;
  };

  jobs::parallelFor(R, [&](size_t ri) {
    const IRing& r = rings[ri];
    const int n = int(r.size());
    std::vector<int>& kp = keep[ri];
    if (n <= 4) {
      for (int k = 0; k < n; k++) kp.push_back(k);
      return;
    }
    int b = 0;
    double bd = -1;
    for (int k = 1; k < n; k++) {
      const double dx = double(r[size_t(k)].x) - r[0].x, dy = double(r[size_t(k)].y) - r[0].y;
      if (dx * dx + dy * dy > bd) { bd = dx * dx + dy * dy; b = k; }
    }
    std::vector<char> on(size_t(n), 0);
    on[0] = on[size_t(b)] = 1;
    std::vector<int> anchors{0, b};
    for (int k = 1; k < n; k++)
      if (k != b && fixedPt(r[size_t(k)])) anchors.push_back(k);
    std::sort(anchors.begin(), anchors.end());
    const bool twoAnchors = anchors.size() == 2;
    std::vector<std::pair<int, int>> stack;
    for (size_t k = 0; k < anchors.size(); k++) {
      on[size_t(anchors[k])] = 1;
      stack.push_back({anchors[k], k + 1 < anchors.size() ? anchors[k + 1] : n});
    }
    while (!stack.empty()) {
      auto [a, e] = stack.back();
      stack.pop_back();
      double d2;
      const int k = farthest(r, a, e, &d2);
      // На двух исходных участках внутренняя точка остаётся всегда: кольцо не короче четырёх вершин.
      const bool base = twoAnchors && ((a == 0 && e == b) || (a == b && e == n));
      if (k < 0 || (d2 <= tol2 && !base)) continue;
      on[size_t(k % n)] = 1;
      stack.push_back({a, k});
      stack.push_back({k, e});
    }
    for (int k = 0; k < n; k++)
      if (on[size_t(k)]) kp.push_back(k);
  });

  // Исправление топологии: конфликтующие отрезки делятся в самой удалённой точке, пока конфликты есть.
  std::vector<Box> rbox(R);
  for (size_t i = 0; i < R; i++) rbox[i] = boxOf(rings[i]);
  int rounds = 0;
  i64 refined = 0;
  std::vector<Seg> segs;
  for (;; rounds++) {
    segs.clear();
    std::vector<size_t> first(R + 1, 0);
    for (size_t ri = 0; ri < R; ri++) {
      first[ri] = segs.size();
      const auto& kp = keep[ri];
      const int m = int(kp.size());
      for (int k = 0; k < m; k++)
        segs.push_back({rings[ri][size_t(kp[size_t(k)])], rings[ri][size_t(kp[size_t((k + 1) % m)])], int(ri), k, m});
    }
    first[R] = segs.size();
    std::vector<u8> bad(segs.size(), 0);
    SegGrid grid(segs, 0);
    grid.pairs([&](u32 i, u32 j) {
      if (badPair(segs[i], segs[j])) bad[i] = bad[j] = 1;
    });
    // Ориентация и площадь не должны выродиться.
    std::vector<IRing> cur(R);
    for (size_t ri = 0; ri < R; ri++) {
      for (int k : keep[ri]) cur[ri].push_back(rings[ri][size_t(k)]);
      if (cur[ri].size() < 3 || area2(cur[ri]) <= 0)
        for (size_t s = first[ri]; s < first[ri + 1]; s++) bad[s] = 1;
    }
    // Вложенность: вершина кольца не может оказаться внутри другого кольца.
    for (size_t a = 0; a < R; a++) {
      const IPt p = cur[a][0];
      for (size_t b = 0; b < R; b++) {
        if (a == b || !rbox[b].contains(p) || !pointInRing(p, cur[b])) continue;
        const auto& kp = keep[b];
        const int m = int(kp.size()), n = int(rings[b].size());
        for (int k = 0; k < m; k++) {
          const int s0 = kp[size_t(k)], s1 = k + 1 < m ? kp[size_t(k + 1)] : n;
          Box bb{rings[b][size_t(s0)].x, rings[b][size_t(s0)].y, rings[b][size_t(s0)].x, rings[b][size_t(s0)].y};
          for (int q = s0; q <= s1; q++) {
            const IPt t = rings[b][size_t(q % n)];
            bb.x0 = std::min(bb.x0, t.x); bb.y0 = std::min(bb.y0, t.y);
            bb.x1 = std::max(bb.x1, t.x); bb.y1 = std::max(bb.y1, t.y);
          }
          if (bb.contains(p)) bad[first[b] + size_t(k)] = 1;
        }
      }
    }
    // Деление отмеченных отрезков.
    bool any = false, progress = false;
    for (size_t ri = 0; ri < R; ri++) {
      auto& kp = keep[ri];
      const int m = int(kp.size()), n = int(rings[ri].size());
      std::vector<int> add;
      for (int k = 0; k < m; k++) {
        if (!bad[first[ri] + size_t(k)]) continue;
        any = true;
        const int s0 = kp[size_t(k)], s1 = k + 1 < m ? kp[size_t(k + 1)] : n;
        const int f = farthest(rings[ri], s0, s1, nullptr);
        if (f >= 0) add.push_back(f % n);
      }
      if (add.empty()) continue;
      progress = true;
      refined += i64(add.size());
      kp.insert(kp.end(), add.begin(), add.end());
      std::sort(kp.begin(), kp.end());
      kp.erase(std::unique(kp.begin(), kp.end()), kp.end());
    }
    if (!any) break;
    if (!progress) throw std::logic_error("simplifyCoast: исходные кольца пересекаются");
  }

  std::vector<IRing> out(R);
  i64 pts = 0;
  for (size_t ri = 0; ri < R; ri++) {
    for (int k : keep[ri]) out[ri].push_back(rings[ri][size_t(k)]);
    pts += i64(out[ri].size());
  }
  if (stats) {
    stats->rings = int(R);
    stats->points = pts;
    stats->rounds = rounds;
    stats->refined = refined;
    stats->seconds += nowSeconds() - t0;
  }
  return out;
}

// ---------------------------------------------------------------- проверка
RingCheck checkRings(const std::vector<IRing>& rings, int w, int h) {
  RingCheck c;
  const i32 W = i32(w) * kCoordScale, H = i32(h) * kCoordScale;
  std::vector<Seg> segs;
  std::vector<Box> boxes;
  std::vector<u8> valid(rings.size(), 0);
  for (size_t ri = 0; ri < rings.size(); ri++) {
    const IRing& r = rings[ri];
    boxes.push_back(r.empty() ? Box{0, 0, -1, -1} : boxOf(r));
    bool deg = r.size() < 3;
    for (size_t k = 0; k < r.size() && !deg; k++)
      if (r[k] == r[(k + 1) % r.size()]) deg = true;
    if (!deg && area2(r) <= 0) deg = true;
    for (auto p : r)
      if (p.x < 0 || p.y < 0 || p.x > W || p.y > H) c.outside++;
    if (deg) {
      c.degenerate++;
      continue;
    }
    valid[ri] = 1;
    const int m = int(r.size());
    for (int k = 0; k < m; k++) segs.push_back({r[size_t(k)], r[size_t((k + 1) % m)], int(ri), k, m});
  }
  // Касания и пересечения; наименьший зазор между разными кольцами (до 1 пикселя).
  double gap2 = double(kCoordScale) * kCoordScale;
  SegGrid grid(segs, kCoordScale / 2);
  grid.pairs([&](u32 i, u32 j) {
    const Seg& s = segs[i];
    const Seg& t = segs[j];
    if (badPair(s, t)) {
      if (s.ring == t.ring) c.selfHits++;
      else c.ringHits++;
      return;
    }
    if (s.ring != t.ring) gap2 = std::min(gap2, segDist2(s.a, s.b, t.a, t.b));
  });
  c.minGap = std::sqrt(gap2) / kCoordScale;
  for (size_t a = 0; a < rings.size(); a++) {
    if (!valid[a]) continue;
    const IPt p = rings[a][0];
    for (size_t b = 0; b < rings.size(); b++)
      if (a != b && valid[b] && boxes[b].contains(p) && pointInRing(p, rings[b])) c.nested++;
  }
  return c;
}

// ---------------------------------------------------------------- маска моря
std::vector<u8> oceanMask(const std::vector<IRing>& rings, int mw, int mh, double scale) {
  std::vector<u8> mask(size_t(mw) * size_t(mh), 255);
  const double S = scale * kCoordScale;  // единиц на клетку маски
  std::vector<std::vector<double>> xs(static_cast<size_t>(mh));
  for (const IRing& r : rings) {
    const size_t n = r.size();
    for (size_t i = 0; i < n; i++) {
      const IPt a = r[i], b = r[(i + 1) % n];
      if (a.y == b.y) continue;
      const double y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
      int r0 = std::max(0, int(std::ceil(y0 / S - 0.5)) - 1), r1 = std::min(mh - 1, int(std::ceil(y1 / S - 0.5)) + 1);
      for (int row = r0; row <= r1; row++) {
        const double Y = (row + 0.5) * S;
        if (Y < y0 || Y >= y1) continue;
        xs[size_t(row)].push_back(a.x + (Y - a.y) * double(b.x - a.x) / double(b.y - a.y));
      }
    }
  }
  jobs::parallelFor(size_t(mh), [&](size_t row) {
    auto& v = xs[row];
    std::sort(v.begin(), v.end());
    for (size_t k = 0; k + 1 < v.size(); k += 2) {
      const int c0 = std::max(0, int(std::ceil(v[k] / S - 0.5))), c1 = std::min(mw, int(std::ceil(v[k + 1] / S - 0.5)));
      for (int c = c0; c < c1; c++) mask[row * size_t(mw) + size_t(c)] = 0;
    }
  });
  return mask;
}

// ---------------------------------------------------------------- ядро геометрии
geo::Coast toCoast(const std::vector<IRing>& rings, int w, int h) {
  geo::Coast c;
  c.width = w;
  c.height = h;
  c.landRings.reserve(rings.size());
  for (const IRing& r : rings) {
    std::vector<Vec2> ring;
    ring.reserve(r.size());
    for (const IPt& p : r) ring.emplace_back(double(p.x) / kCoordScale, double(p.y) / kCoordScale);
    c.landRings.push_back(std::move(ring));
  }
  return c;
}

GeoCheck geoCheck(const geo::Coast& coast) {
  const double t0 = nowSeconds();
  GeoCheck g;
  for (const auto& r : coast.landRings) {
    double a = 0;
    for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) a += r[j].x * r[i].y - r[i].x * r[j].y;
    g.ringArea += std::fabs(a) * 0.5;
  }
  Store store;
  try {
    store.transact("Береговая линия", [&](Tx& tx) { geo::initFromCoast(tx, coast); });
    g.built = true;
  } catch (const std::exception& e) {
    g.error = e.what();
  }
  if (g.built) {
    const World& w = store.world();
    for (const geo::Issue& is : geo::validate(w)) g.issues.push_back(strf("%s: %s (%.2f, %.2f)", is.code.c_str(), is.msg.c_str(), is.at.x, is.at.y));
    g.nodes = int(w.nodes.size());
    g.edges = int(w.edges.size());
    w.edges.each([&](const Edge& e) { g.coastEdges += e.kind == EdgeKind::Coast; });
    const auto fs = geo::faces(w);
    for (const geo::Face& f : fs->faces) {
      if (f.terrain == Terrain::Land) {
        g.landFaces++;
        g.landArea += f.area;
      } else {
        g.seaFaces++;
      }
    }
  }
  g.seconds = nowSeconds() - t0;
  return g;
}

}  // namespace rg::map::bake
