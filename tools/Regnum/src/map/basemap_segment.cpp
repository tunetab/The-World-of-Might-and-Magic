// Regnum — разложение исходной карты на слои ocean / inland / symbols (см. basemap_build.h).
#include <cstring>

#include "base/jobs.h"
#include "map/basemap_build.h"

namespace rg::map::bake {

namespace {

using codec::RgbaImage;

constexpr int kInkCore = 40;    // краска 255 − B «тела» знака (не сглаженный край)
constexpr int kInkLink = 16;    // краска, по которой пиксели связываются в область знака (светлый контур снега)
constexpr int kLight = 250;     // «белый» фон: все каналы не темнее
constexpr int kDark = 60;       // линии замков и башен темнее, контур снега и тело горы — светлее
constexpr int kSnowArea = 400;  // снежная шапка — замкнутая белая область не больше этой площади
constexpr int kSnowSide = 40;
constexpr int kMountainBody = 8;  // пикселей ровного серого тела, чтобы область краски считалась горой
constexpr int kHaloGrow = 16;   // ореол берега (0 < t ≤ 0,5) присоединяется к морю не дальше (пиксели)

void rows(int h, const std::function<void(size_t)>& fn) { jobs::parallelFor(size_t(h), fn, 8); }

// Расстояние по строке (не больше 255) до ближайшего пикселя, где (m != 0) == target. За краем таких нет.
void rowDistance(const std::vector<u8>& m, bool target, int w, int h, std::vector<u8>& out) {
  out.resize(m.size());
  rows(h, [&](size_t y) {
    const u8* s = m.data() + y * size_t(w);
    u8* o = out.data() + y * size_t(w);
    int run = 255;
    for (int x = 0; x < w; x++) {
      if ((s[x] != 0) == target) run = 0;
      else if (run < 255) run++;
      o[x] = u8(run);
    }
    run = 255;
    for (int x = w - 1; x >= 0; x--) {
      if ((s[x] != 0) == target) run = 0;
      else if (run < 255) run++;
      if (run < o[x]) o[x] = u8(run);
    }
  });
}

// Точный евклидов круг радиуса R по строчным расстояниям hd:
// d²(x, y) = min по |dy| ≤ R от hd(x, y + dy)² + dy². outside = true: out = d² > R² (эрозия), иначе d² ≤ R².
void diskTest(const std::vector<u8>& hd, int w, int h, int R, bool outside, std::vector<u8>& out) {
  out.resize(hd.size());
  const int R2 = R * R;
  rows(h, [&](size_t yy) {
    const int y = int(yy);
    std::vector<int> best(size_t(w), std::numeric_limits<int>::max());
    for (int dy = -R; dy <= R; dy++) {
      int sy = y + dy;
      if (sy < 0 || sy >= h) continue;
      const u8* s = hd.data() + size_t(sy) * size_t(w);
      const int dd = dy * dy;
      for (int x = 0; x < w; x++) {
        int v = int(s[x]) * int(s[x]) + dd;
        if (v < best[size_t(x)]) best[size_t(x)] = v;
      }
    }
    u8* o = out.data() + size_t(y) * size_t(w);
    for (int x = 0; x < w; x++) o[x] = outside ? u8(best[size_t(x)] > R2) : u8(best[size_t(x)] <= R2);
  });
}

// Заливка интервалами по маске из начальных точек (reached дополняется).
// conn8 — диагональные соседи; field (при conn8 = false) — седловые диагонали по правилу marching squares:
// две морские вершины клетки по диагонали связаны, если сумма четырёх значений клетки ≥ 510.
void fill(const u8* mask, int w, int h, bool conn8, const u8* field, std::vector<u64>& stack, std::vector<u8>& reached) {
  auto open = [&](size_t i) { return mask[i] && !reached[i]; };
  auto push = [&](int x, int y) { stack.push_back((u64(u32(y)) << 32) | u32(x)); };
  while (!stack.empty()) {
    u64 v = stack.back();
    stack.pop_back();
    const int x = int(u32(v)), y = int(v >> 32);
    const size_t row = size_t(y) * size_t(w);
    if (!open(row + size_t(x))) continue;
    int xl = x, xr = x;
    while (xl > 0 && open(row + size_t(xl - 1))) xl--;
    while (xr < w - 1 && open(row + size_t(xr + 1))) xr++;
    std::memset(&reached[row + size_t(xl)], 1, size_t(xr - xl + 1));
    for (int ny = y - 1; ny <= y + 1; ny += 2) {
      if (ny < 0 || ny >= h) continue;
      const size_t nrow = size_t(ny) * size_t(w);
      const int a = conn8 ? std::max(0, xl - 1) : xl, b = conn8 ? std::min(w - 1, xr + 1) : xr;
      bool run = false;
      for (int xx = a; xx <= b; xx++) {
        if (open(nrow + size_t(xx))) {
          if (!run) push(xx, ny);
          run = true;
        } else {
          run = false;
        }
      }
      if (field && !conn8) {
        auto saddle = [&](int sx, int ex) {  // sx — вершина интервала, ex — диагональный сосед
          const size_t d = nrow + size_t(ex);
          if (!open(d) || mask[row + size_t(ex)] || mask[nrow + size_t(sx)]) return;
          int sum = field[row + size_t(sx)] + field[row + size_t(ex)] + field[nrow + size_t(sx)] + field[d];
          if (sum >= 510) push(ex, ny);
        };
        if (xl > 0) saddle(xl, xl - 1);
        if (xr < w - 1) saddle(xr, xr + 1);
      }
    }
  }
}

std::vector<u8> fillFromBorder(const u8* mask, int w, int h, bool conn8, const u8* field) {
  std::vector<u8> reached(size_t(w) * size_t(h), 0);
  std::vector<u64> stack;
  auto seed = [&](int x, int y) {
    if (mask[size_t(y) * size_t(w) + size_t(x)]) stack.push_back((u64(u32(y)) << 32) | u32(x));
  };
  for (int x = 0; x < w; x++) {
    seed(x, 0);
    if (h > 1) seed(x, h - 1);
  }
  for (int y = 1; y < h - 1; y++) {
    seed(0, y);
    if (w > 1) seed(w - 1, y);
  }
  fill(mask, w, h, conn8, field, stack, reached);
  return reached;
}

// Выпуклая оболочка (монотонная цепь), против часовой в математической системе; точки — целые углы пикселей.
std::vector<std::array<int, 2>> convexHull(std::vector<std::array<int, 2>> p) {
  std::sort(p.begin(), p.end());
  p.erase(std::unique(p.begin(), p.end()), p.end());
  if (p.size() < 3) return p;
  auto cross = [](const std::array<int, 2>& o, const std::array<int, 2>& a, const std::array<int, 2>& b) {
    return i64(a[0] - o[0]) * (b[1] - o[1]) - i64(a[1] - o[1]) * (b[0] - o[0]);
  };
  std::vector<std::array<int, 2>> hull(p.size() * 2);
  size_t k = 0;
  for (size_t i = 0; i < p.size(); i++) {
    while (k >= 2 && cross(hull[k - 2], hull[k - 1], p[i]) <= 0) k--;
    hull[k++] = p[i];
  }
  for (size_t i = p.size() - 1, t = k + 1; i-- > 0;) {
    while (k >= t && cross(hull[k - 2], hull[k - 1], p[i]) <= 0) k--;
    hull[k++] = p[i];
  }
  hull.resize(k - 1);
  return hull;
}

// Нейтральный серый пикселя без воды (только при t < 0,5).
inline int neutralGray(int R, int B) {
  int tw = B - R;
  return std::min(255, (R * 255 + (255 - tw) / 2) / (255 - tw));
}


struct Span { int y, x0, x1; };

// Обход малых связных областей маски: не касаются края карты, площадь ≤ maxArea, габарит < maxSide.
// onSmall(spans, area) получает интервалы области. Детерминировано (порядок — построчно).
template <class F>
void forSmallComponents(const std::vector<u8>& mask, int w, int h, bool conn8, i64 maxArea, int maxSide, F&& onSmall) {
  std::vector<u8> seen(size_t(w) * size_t(h), 0);
  std::vector<u64> stack;
  std::vector<Span> spans;
  auto open = [&](size_t i) { return mask[i] && !seen[i]; };
  for (int sy = 0; sy < h; sy++) {
    const size_t srow = size_t(sy) * size_t(w);
    for (int sx = 0; sx < w; sx++) {
      if (!open(srow + size_t(sx))) continue;
      spans.clear();
      i64 area = 0;
      int x0 = sx, x1 = sx, y0 = sy, y1 = sy;
      bool small = true;
      stack.push_back((u64(u32(sy)) << 32) | u32(sx));
      while (!stack.empty()) {
        const u64 v = stack.back();
        stack.pop_back();
        const int x = int(u32(v)), y = int(v >> 32);
        const size_t row = size_t(y) * size_t(w);
        if (!open(row + size_t(x))) continue;
        int xl = x, xr = x;
        while (xl > 0 && open(row + size_t(xl - 1))) xl--;
        while (xr < w - 1 && open(row + size_t(xr + 1))) xr++;
        std::memset(&seen[row + size_t(xl)], 1, size_t(xr - xl + 1));
        area += xr - xl + 1;
        x0 = std::min(x0, xl); x1 = std::max(x1, xr);
        y0 = std::min(y0, y); y1 = std::max(y1, y);
        if (small) {
          if (xl == 0 || xr == w - 1 || y == 0 || y == h - 1 || area > maxArea || x1 - x0 >= maxSide || y1 - y0 >= maxSide) {
            small = false;
            spans.clear();
          } else {
            spans.push_back({y, xl, xr});
          }
        }
        for (int ny = y - 1; ny <= y + 1; ny += 2) {
          if (ny < 0 || ny >= h) continue;
          const size_t nrow = size_t(ny) * size_t(w);
          bool run = false;
          const int a = conn8 ? std::max(0, xl - 1) : xl, b = conn8 ? std::min(w - 1, xr + 1) : xr;
          for (int xx = a; xx <= b; xx++) {
            if (open(nrow + size_t(xx))) {
              if (!run) stack.push_back((u64(u32(ny)) << 32) | u32(xx));
              run = true;
            } else {
              run = false;
            }
          }
        }
      }
      if (small) onSmall(spans, area);
    }
  }
}

// Островки: компоненты суши (8-связность), не касающиеся края карты, площадью ≤ maxArea и с габаритом < maxSide.
// При поиске открытого моря они считаются водой: цепочки островков не перекрывают проливы при эрозии.
void fillIslets(std::vector<u8>& water, int w, int h, int maxArea, int maxSide, int* count, i64* pixels) {
  std::vector<u8> land(water.size());
  for (size_t i = 0; i < water.size(); i++) land[i] = !water[i];
  forSmallComponents(land, w, h, true, maxArea, maxSide, [&](const std::vector<Span>& spans, i64 area) {
    for (const Span& s : spans) std::memset(&water[size_t(s.y) * size_t(w) + size_t(s.x0)], 1, size_t(s.x1 - s.x0 + 1));
    if (count) ++*count;
    if (pixels) *pixels += area;
  });
}

// Продолжение значений каналов внутрь неизвестных пикселей (под непрозрачными знаками): обход слоями от известных
// пикселей, каждый получает среднее уже известных соседей (прямые с весом 2, диагональные — 1). Детерминировано.
// Возвращает число заполненных пикселей; недостижимые (нет известных пикселей вовсе) остаются как есть.
i64 inpaint(const std::vector<u8>& unknown, int w, int h, const std::vector<std::vector<u8>*>& chans) {
  const size_t N = size_t(w) * size_t(h);
  std::vector<u8> state(N, 0);  // 0 — известно, 1 — неизвестно, 2 — в текущем слое
  std::vector<u32> ring, next;
  for (size_t i = 0; i < N; i++) state[i] = unknown[i] ? 1 : 0;
  auto forNeighbors = [&](u32 i, auto&& f) {
    const int x = int(i % u32(w)), y = int(i / u32(w));
    for (int dy = -1; dy <= 1; dy++) {
      const int ny = y + dy;
      if (ny < 0 || ny >= h) continue;
      for (int dx = -1; dx <= 1; dx++) {
        const int nx = x + dx;
        if ((dx | dy) == 0 || nx < 0 || nx >= w) continue;
        f(u32(size_t(ny) * size_t(w) + size_t(nx)), (dx & dy) ? 1 : 2);
      }
    }
  };
  for (size_t i = 0; i < N; i++) {
    if (state[i] != 1) continue;
    bool edge = false;
    forNeighbors(u32(i), [&](u32 j, int) { edge = edge || state[j] == 0; });
    if (edge) {
      state[i] = 2;
      ring.push_back(u32(i));
    }
  }
  const size_t C = chans.size();
  std::vector<u8> vals;
  i64 filled = 0;
  while (!ring.empty()) {
    vals.assign(ring.size() * C, 0);
    for (size_t k = 0; k < ring.size(); k++) {
      u32 sum[4] = {0, 0, 0, 0}, wsum = 0;
      forNeighbors(ring[k], [&](u32 j, int wt) {
        if (state[j] != 0) return;
        wsum += u32(wt);
        for (size_t c = 0; c < C && c < 4; c++) sum[c] += u32((*chans[c])[j]) * u32(wt);
      });
      for (size_t c = 0; c < C && c < 4; c++) vals[k * C + c] = u8((sum[c] + wsum / 2) / std::max<u32>(1, wsum));
    }
    next.clear();
    for (size_t k = 0; k < ring.size(); k++) {
      const u32 i = ring[k];
      for (size_t c = 0; c < C && c < 4; c++) (*chans[c])[i] = vals[k * C + c];
      state[i] = 0;
    }
    filled += i64(ring.size());
    for (u32 i : ring)
      forNeighbors(i, [&](u32 j, int) {
        if (state[j] == 1) {
          state[j] = 2;
          next.push_back(j);
        }
      });
    ring.swap(next);
  }
  return filled;
}

// Число связных (8-связность) областей маски.
int countComponents(const std::vector<u8>& mask, int w, int h) {
  std::vector<u8> seen(mask.size(), 0);
  std::vector<u64> stack;
  int n = 0;
  for (size_t i = 0; i < mask.size(); i++) {
    if (!mask[i] || seen[i]) continue;
    n++;
    stack.push_back((u64(u32(i / size_t(w))) << 32) | u32(i % size_t(w)));
    fill(mask.data(), w, h, true, nullptr, stack, seen);
  }
  return n;
}

}  // namespace

void flattenOnWhite(RgbaImage& img) {
  const int w = img.w;
  rows(img.h, [&](size_t y) {
    u8* p = img.rgba.data() + y * size_t(w) * 4;
    for (int x = 0; x < w; x++, p += 4) {
      const u32 a = p[3];
      if (a == 255) continue;
      for (int c = 0; c < 3; c++) p[c] = u8((u32(p[c]) * a + 255u * (255u - a) + 127u) / 255u);
      p[3] = 255;
    }
  });
}

Layers segment(const RgbaImage& flat, const Options& opt, SegmentStats* stats) {
  const double t0 = nowSeconds();
  const int w = flat.w, h = flat.h;
  if (w <= 0 || h <= 0 || flat.rgba.size() < size_t(w) * size_t(h) * 4) fail("Пустое изображение карты.");
  if (i64(w) * h >= (i64(1) << 31)) fail("Изображение карты слишком велико.");
  const size_t N = size_t(w) * size_t(h);
  const u8* px = flat.rgba.data();
  const int r = clamp(opt.erodeRadius, 1, 120);
  SegmentStats st;

  Layers L;
  L.w = w;
  L.h = h;

  // 1. Знаки: связные области краски. Горы (серые) — непрозрачны внутри выпуклой оболочки, кроме фона между
  // слившимися горами; снежные шапки — малые замкнутые белые области — непрозрачно белые. Остальное — краска с альфой.
  // ink: 0..254 — цвет краски для разложения, 255 — непрозрачно (цвет = исходник).
  std::vector<u8> ink(N, 0);
  L.kind.assign(N, 0);
  {
    // Фон: белые пиксели, кроме снежных шапок (малых замкнутых белых областей без тёмных линий вокруг).
    std::vector<u8> bg(N);
    rows(h, [&](size_t y) {
      const u8* p = px + y * size_t(w) * 4;
      u8* o = bg.data() + y * size_t(w);
      for (int x = 0; x < w; x++, p += 4) o[x] = u8(std::min({p[0], p[1], p[2]}) >= kLight);
    });
    std::vector<u8> light = bg;
    forSmallComponents(light, w, h, false, kSnowArea, kSnowSide, [&](const std::vector<Span>& spans, i64) {
      for (const Span& sp : spans)
        for (int x = sp.x0; x <= sp.x1; x++)
          for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
              const u8* q = px + (size_t(sp.y + dy) * size_t(w) + size_t(x + dx)) * 4;
              if (std::min({q[0], q[1], q[2]}) < kDark) return;  // внутренность замка или башни
            }
      for (const Span& sp : spans) std::memset(&bg[size_t(sp.y) * size_t(w) + size_t(sp.x0)], 0, size_t(sp.x1 - sp.x0 + 1));
      st.snowCaps++;
    });
    light.clear();
    light.shrink_to_fit();
    std::vector<u8> seen(N, 0);
    std::vector<u32> comp, stack;
    std::vector<int> grays;
    std::vector<std::array<int, 2>> corners;
    auto inkAt = [&](size_t i) { return 255 - int(px[i * 4 + 2]) >= kInkLink; };
    // Серый тела горы (нейтральный, без воды) или −1.
    auto bodyGray = [&](int x, int y) {
      const u8* q = px + (size_t(y) * size_t(w) + size_t(x)) * 4;
      if (int(q[2]) - int(q[0]) >= 32) return -1;
      const int g = neutralGray(q[0], q[2]);
      return g >= 72 && g <= 120 ? g : -1;
    };
    for (size_t start = 0; start < N; start++) {
      if (seen[start] || !inkAt(start)) continue;
      comp.clear();
      stack.push_back(u32(start));
      seen[start] = 1;
      while (!stack.empty()) {
        u32 i = stack.back();
        stack.pop_back();
        comp.push_back(i);
        const int x = int(i % u32(w)), y = int(i / u32(w));
        for (int dy = -1; dy <= 1; dy++) {
          const int ny = y + dy;
          if (ny < 0 || ny >= h) continue;
          for (int dx = -1; dx <= 1; dx++) {
            const int nx = x + dx;
            if (nx < 0 || nx >= w) continue;
            const size_t j = size_t(ny) * size_t(w) + size_t(nx);
            if (!seen[j] && inkAt(j)) {
              seen[j] = 1;
              stack.push_back(u32(j));
            }
          }
        }
      }
      st.components++;

      // Признак горы: «тело» — пиксели ровного среднего серого, у которых и четыре соседа такие же
      // (у чёрных знаков и их сглаженных краёв такой внутренности нет). Гора может слиться со стеной или замком.
      int x0 = w, y0 = h, x1 = -1, y1 = -1, body = 0;
      grays.clear();
      for (u32 i : comp) {
        const int x = int(i % u32(w)), y = int(i / u32(w));
        x0 = std::min(x0, x); x1 = std::max(x1, x);
        y0 = std::min(y0, y); y1 = std::max(y1, y);
        if (x == 0 || y == 0 || x == w - 1 || y == h - 1) continue;
        const int g = bodyGray(x, y);
        if (g < 0 || bodyGray(x - 1, y) < 0 || bodyGray(x + 1, y) < 0 || bodyGray(x, y - 1) < 0 || bodyGray(x, y + 1) < 0) continue;
        body++;
        grays.push_back(g);
      }
      const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
      const bool mountain = body >= kMountainBody && bw >= 6 && bh >= 5;
      if (!mountain) continue;
      st.mountains++;
      std::nth_element(grays.begin(), grays.begin() + grays.size() / 2, grays.end());
      const u8 gray = u8(grays[grays.size() / 2]);

      // Оболочка по углам крайних пикселей каждой строки.
      std::vector<int> rowMin(size_t(bh), w), rowMax(size_t(bh), -1);
      for (u32 i : comp) {
        const int x = int(i % u32(w)), y = int(i / u32(w)) - y0;
        rowMin[size_t(y)] = std::min(rowMin[size_t(y)], x);
        rowMax[size_t(y)] = std::max(rowMax[size_t(y)], x);
      }
      corners.clear();
      for (int y = 0; y < bh; y++) {
        if (rowMax[size_t(y)] < 0) continue;
        const int yy = y + y0;
        corners.push_back({rowMin[size_t(y)], yy});
        corners.push_back({rowMin[size_t(y)], yy + 1});
        corners.push_back({rowMax[size_t(y)] + 1, yy});
        corners.push_back({rowMax[size_t(y)] + 1, yy + 1});
      }
      const auto hull = convexHull(corners);
      if (hull.size() < 3) continue;
      struct EdgeLine { double nx, ny, c; };  // расстояние внутрь: nx·x + ny·y + c
      std::vector<EdgeLine> lines;
      for (size_t k = 0; k < hull.size(); k++) {
        const auto& a = hull[k];
        const auto& b = hull[(k + 1) % hull.size()];
        double ex = b[0] - a[0], ey = b[1] - a[1], len = std::sqrt(ex * ex + ey * ey);
        // Против часовой (математически): внутренность слева, нормаль (−ey, ex).
        double nx = -ey / len, ny = ex / len;
        lines.push_back({nx, ny, -(nx * a[0] + ny * a[1])});
      }
      for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
          const double cx = x + 0.5, cy = y + 0.5;
          double dmin = kInf;
          for (const auto& e : lines) dmin = std::min(dmin, e.nx * cx + e.ny * cy + e.c);
          if (dmin < 0) continue;
          const size_t i = size_t(y) * size_t(w) + size_t(x);
          const int R = px[i * 4], B = px[i * 4 + 2];
          L.kind[i] |= KindMountain;
          bool edge = B - R > 64 || bg[i];  // над водой и на фоне — обычное разложение
          if (!edge && 255 - B < kInkCore) {
            // Сглаженный край, касающийся фона стороной (склоны, промежутки между слившимися горами), —
            // тоже обычное разложение; светлый контур снега внутри горы остаётся непрозрачным.
            edge = (x > 0 && bg[i - 1]) || (x + 1 < w && bg[i + 1]) || (y > 0 && bg[i - size_t(w)]) || (y + 1 < h && bg[i + size_t(w)]);
          }
          ink[i] = edge ? gray : u8(255);
        }
      }
    }
  }

  // 2. Разложение пикселя: альфа и серый знака, доля воды под ним (море или внутренние воды — ниже).
  std::vector<u8> water(N, 0);
  L.symA.assign(N, 0);
  L.symV.assign(N, 0);
  rows(h, [&](size_t y) {
    const u8* p = px + y * size_t(w) * 4;
    const size_t row = y * size_t(w);
    for (int x = 0; x < w; x++, p += 4) {
      const size_t i = row + size_t(x);
      const int B = p[2], R = std::min<int>(p[0], B);
      const int tw = B - R, d = 255 - B, aMax = 255 - tw;
      const int c = ink[i];
      int a8 = 0, s8 = 0, wa = tw;
      if ((d > 0 || c == 255) && aMax > 0) {
        const bool opaque = c == 255 || d * 255 >= (255 - c) * aMax;
        if (opaque) {
          a8 = aMax;
          s8 = std::min(255, (R * 255 + a8 / 2) / a8);
          wa = a8 < 255 ? 255 : 0;
        } else {
          a8 = std::max(1, (d * 255 + (255 - c) / 2) / (255 - c));
          const double A = a8 / 255.0;
          s8 = clamp(int(std::lround(255.0 - d / A)), 0, 255);
          if (a8 >= 255) {
            wa = 0;
          } else {
            double a = 1.0 - (R - A * s8) / ((1.0 - A) * 255.0);
            wa = clamp(int(std::lround(a * 255.0)), 0, 255);
          }
        }
      }
      L.symA[i] = u8(a8);
      L.symV[i] = a8 ? u8(s8) : 0;
      water[i] = u8(wa);
    }
  });
  ink.clear();
  ink.shrink_to_fit();

  // 3. Открытое море: вода (t > 0,5) с островками, эрозия кругом радиуса r (за краем карты — вода),
  // часть эрозии, связанная с краем, и дилатация на r + 2 — возвращает берега и сглаживание.
  std::vector<u8> sea(N), tmp, hd;
  rows(h, [&](size_t y) {
    const u8* p = px + y * size_t(w) * 4;
    u8* o = sea.data() + y * size_t(w);
    for (int x = 0; x < w; x++, p += 4) o[x] = u8(int(p[2]) - int(p[0]) > 127);
  });
  if (opt.isletFillArea > 0) fillIslets(sea, w, h, opt.isletFillArea, std::max(1, opt.isletFillSide), &st.isletsFilled, &st.isletFillPx);
  rowDistance(sea, false, w, h, hd);
  diskTest(hd, w, h, r, true, tmp);
  {
    std::vector<u8> core = fillFromBorder(tmp.data(), w, h, true, nullptr);
    rowDistance(core, true, w, h, hd);
  }
  diskTest(hd, w, h, r + 2, false, tmp);

  // 4. Классы воды и поле берега (доля воды под чёрной краской (B − R) / B). Под непрозрачными знаками
  // значения продолжаются от соседей: на уровне 0 их не видно, а уменьшение и берег не получают «дыр».
  L.field.assign(N, 0);
  L.ocean.assign(N, 0);
  L.inland.assign(N, 0);
  rows(h, [&](size_t y) {
    const u8* p = px + y * size_t(w) * 4;
    const size_t row = y * size_t(w);
    for (int x = 0; x < w; x++, p += 4) {
      const size_t i = row + size_t(x);
      const int R = p[0], B = p[2];
      sea[i] = L.symA[i] == 255;  // sea больше не нужен: метка неизвестных пикселей
      if (sea[i]) continue;
      if (tmp[i] && B > R) {
        L.field[i] = u8((255 * (B - R) + B / 2) / B);
        L.ocean[i] = water[i];
      } else {
        L.inland[i] = water[i];
      }
    }
  });
  // Мягкий край берега шире дилатации: море продолжается по пикселям ореола (0 < t ≤ 0,5) не дальше kHaloGrow,
  // ядра рек (t > 0,5) остаются внутренними водами.
  {
    auto halo = [&](size_t i) {
      const int t = int(px[i * 4 + 2]) - int(px[i * 4]);
      return !sea[i] && !L.field[i] && t > 0 && t <= 127;
    };
    auto grow = [&](u32 i, std::vector<u32>& out) {
      const int x = int(i % u32(w)), y = int(i / u32(w));
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          const int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          const size_t j = size_t(ny) * size_t(w) + size_t(nx);
          if (halo(j) && !tmp[j]) {
            tmp[j] = 2;  // в очереди
            out.push_back(u32(j));
          }
        }
    };
    std::vector<u32> ring, next;
    for (size_t i = 0; i < N; i++) {
      if (!halo(i)) continue;
      const int x = int(i % size_t(w)), y = int(i / size_t(w));
      bool near = false;
      for (int dy = -1; dy <= 1 && !near; dy++)
        for (int dx = -1; dx <= 1 && !near; dx++) {
          const int nx = x + dx, ny = y + dy;
          near = nx >= 0 && ny >= 0 && nx < w && ny < h && L.field[size_t(ny) * size_t(w) + size_t(nx)] > 0;
        }
      if (near) {
        tmp[i] = 2;
        ring.push_back(u32(i));
      }
    }
    for (int depth = 0; depth < kHaloGrow && !ring.empty(); depth++) {
      next.clear();
      for (u32 i : ring) {
        const int R = px[size_t(i) * 4], B = px[size_t(i) * 4 + 2];
        L.field[i] = u8((255 * (B - R) + B / 2) / B);
        L.ocean[i] = L.inland[i];
        L.inland[i] = 0;
        st.haloPx++;
      }
      if (depth + 1 < kHaloGrow)
        for (u32 i : ring) grow(i, next);
      ring.swap(next);
    }
  }
  water.clear();
  water.shrink_to_fit();
  st.inpaintedPx = inpaint(sea, w, h, {&L.field, &L.ocean, &L.inland});

  // 5. Замкнутые участки моря (не связаны с краем по правилам marching squares) — во внутренние воды.
  {
    std::vector<u8>& core = tmp;  // tmp больше не нужен
    rows(h, [&](size_t y) {
      const size_t row = y * size_t(w);
      for (int x = 0; x < w; x++) core[row + size_t(x)] = u8(L.field[row + size_t(x)] >= 128);
    });
    std::vector<u8> reached = fillFromBorder(core.data(), w, h, false, L.field.data());
    rows(h, [&](size_t y) {
      const size_t row = y * size_t(w);
      for (int x = 0; x < w; x++) core[row + size_t(x)] = u8(core[row + size_t(x)] && !reached[row + size_t(x)]);
    });
    st.pockets = countComponents(core, w, h);
    for (size_t i = 0; i < N; i++) st.pocketPx += core[i];
    // Мягкий край (поле < 128) остаётся морем, только если связан с достижимым морем через такие же пиксели.
    rows(h, [&](size_t y) {
      const size_t row = y * size_t(w);
      for (int x = 0; x < w; x++) {
        const size_t i = row + size_t(x);
        core[i] = u8(reached[i] || (L.field[i] < 128 && (L.field[i] || L.ocean[i])));
      }
    });
    const std::vector<u8> keep = fillFromBorder(core.data(), w, h, true, nullptr);
    rows(h, [&](size_t y) {
      const size_t row = y * size_t(w);
      for (int x = 0; x < w; x++) {
        const size_t i = row + size_t(x);
        if ((!L.field[i] && !L.ocean[i]) || keep[i]) continue;
        const int o = L.ocean[i], n = L.inland[i];
        L.inland[i] = u8(255 - ((255 - o) * (255 - n) + 127) / 255);
        L.ocean[i] = 0;
        L.field[i] = 0;
      }
    });
  }
  hd.clear();
  hd.shrink_to_fit();

  // 6. Классы пикселей и счётчики.
  rows(h, [&](size_t y) {
    const size_t row = y * size_t(w);
    for (int x = 0; x < w; x++) {
      const size_t i = row + size_t(x);
      u8 k = L.kind[i];
      if (L.ocean[i]) k |= KindOcean;
      if (L.inland[i]) k |= KindInland;
      if (L.symA[i]) k |= KindInk;
      L.kind[i] = k;
    }
  });
  for (size_t i = 0; i < N; i++) {
    const u8 k = L.kind[i];
    st.oceanPx += (k & KindOcean) != 0;
    st.inlandPx += (k & KindInland) != 0;
    st.inkPx += (k & KindInk) != 0;
    st.opaquePx += L.symA[i] == 255;
  }
  st.seconds = nowSeconds() - t0;
  if (stats) *stats = st;
  return L;
}

// ---------------------------------------------------------------- композиция

void compositePixel(const Layers& L, size_t i, u8 rgb[3]) {
  const double ao = L.ocean[i] / 255.0, ai = L.inland[i] / 255.0, as = L.symA[i] / 255.0, s = L.symV[i];
  const double blue[3] = {double(kOcean.r), double(kOcean.g), double(kOcean.b)};
  for (int c = 0; c < 3; c++) {
    double v = 255.0 * (1 - ao) + blue[c] * ao;
    v = v * (1 - ai) + blue[c] * ai;
    v = v * (1 - as) + s * as;
    rgb[c] = u8(clamp(int(std::lround(v)), 0, 255));
  }
}

RgbaImage composite(const Layers& L, int x0, int y0, int w, int h, Color tint) {
  RgbaImage out;
  out.w = w;
  out.h = h;
  out.rgba.assign(size_t(w) * size_t(h) * 4, 255);
  const double ta = tint.a / 255.0;
  const double base[3] = {255.0 * (1 - ta) + tint.r * ta, 255.0 * (1 - ta) + tint.g * ta, 255.0 * (1 - ta) + tint.b * ta};
  const double blue[3] = {double(kOcean.r), double(kOcean.g), double(kOcean.b)};
  rows(h, [&](size_t yy) {
    const int y = int(yy) + y0;
    for (int xx = 0; xx < w; xx++) {
      const int x = xx + x0;
      u8* o = out.rgba.data() + (yy * size_t(w) + size_t(xx)) * 4;
      if (x < 0 || y < 0 || x >= L.w || y >= L.h) {
        o[0] = o[1] = o[2] = 0;
        continue;
      }
      const size_t i = size_t(y) * size_t(L.w) + size_t(x);
      const double ao = L.ocean[i] / 255.0, ai = L.inland[i] / 255.0, as = L.symA[i] / 255.0, s = L.symV[i];
      for (int c = 0; c < 3; c++) {
        double v = base[c] * (1 - ao) + blue[c] * ao;
        v = v * (1 - ai) + blue[c] * ai;
        v = v * (1 - as) + s * as;
        o[c] = u8(clamp(int(std::lround(v)), 0, 255));
      }
    }
  });
  return out;
}

CompositeError compareComposite(const RgbaImage& flat, const Layers& L) {
  CompositeError e;
  if (flat.w != L.w || flat.h != L.h) fail("Размеры слоёв и исходника не совпадают.");
  const int w = L.w, h = L.h;
  std::vector<double> sum(size_t(h), 0);
  std::vector<int> mx(size_t(h), 0);
  std::vector<i64> o1(size_t(h), 0), o2(size_t(h), 0);
  std::vector<u8> neutral(size_t(h), 1);
  rows(h, [&](size_t y) {
    double s = 0;
    int m = 0;
    i64 c1 = 0, c2 = 0;
    for (int x = 0; x < w; x++) {
      const size_t i = y * size_t(w) + size_t(x);
      u8 rgb[3];
      compositePixel(L, i, rgb);
      const u8* p = flat.rgba.data() + i * 4;
      int pm = 0;
      for (int c = 0; c < 3; c++) {
        int d = std::abs(int(rgb[c]) - int(p[c]));
        s += d;
        pm = std::max(pm, d);
      }
      m = std::max(m, pm);
      c1 += pm > 1;
      c2 += pm > 2;
    }
    sum[y] = s;
    mx[y] = m;
    o1[y] = c1;
    o2[y] = c2;
  });
  double total = 0;
  for (int y = 0; y < h; y++) {
    total += sum[size_t(y)];
    e.max = std::max(e.max, mx[size_t(y)]);
    e.over1 += o1[size_t(y)];
    e.over2 += o2[size_t(y)];
  }
  e.pixels = i64(w) * h;
  e.mean = total / (3.0 * double(e.pixels));
  return e;
}

}  // namespace rg::map::bake
