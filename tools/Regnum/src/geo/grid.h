// Regnum — пространственный хеш (равномерная сетка) для отрезков, точек и граней.
//
// Элемент — отрезок (a, b); точка — отрезок нулевой длины. Отрезок регистрируется во всех ячейках,
// которые он реально проходит (по строкам сетки), поэтому длинные наклонные отрезки не раздувают сетку.
// Хранение CSR: после build() структура только читается и потокобезопасна.
#pragma once
#include "base/base.h"

namespace rg::geo {

class SegGrid {
 public:
  SegGrid() = default;

  // Построить по отрезкам. bounds расширяется до габаритов всех элементов.
  // cellSize ≤ 0 — подобрать автоматически (≈ perCell элементов на ячейку, не более 4096 ячеек по стороне).
  void build(std::vector<Vec2> a, std::vector<Vec2> b, Box2 bounds = {}, double cellSize = 0, double perCell = 2.0);
  // Сетка по прямоугольникам (элемент i занимает все ячейки boxes[i]). a/b элементов — углы прямоугольника.
  void buildBoxes(const std::vector<Box2>& boxes, Box2 bounds = {}, double cellSize = 0, double perCell = 2.0);

  int size() const { return int(a_.size()); }
  bool empty() const { return nx_ == 0; }
  Vec2 a(int i) const { return a_[size_t(i)]; }
  Vec2 b(int i) const { return b_[size_t(i)]; }
  const Box2& box() const { return box_; }
  double cell() const { return cs_; }
  int cols() const { return nx_; }
  int rows() const { return ny_; }

  int cellX(double x) const { return clampCell((x - box_.x0) * inv_, nx_); }
  int cellY(double y) const { return clampCell((y - box_.y0) * inv_, ny_); }

  // Элементы ячеек, пересекающих прямоугольник q. Элемент может встретиться несколько раз.
  template <class F> void query(const Box2& q, F&& f) const {
    if (nx_ == 0 || q.empty() || !q.intersects(box_)) return;
    int x0 = cellX(q.x0), x1 = cellX(q.x1), y0 = cellY(q.y0), y1 = cellY(q.y1);
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        size_t c = size_t(y) * size_t(nx_) + size_t(x);
        for (u32 k = start_[c], e = start_[c + 1]; k < e; k++) f(int(items_[k]));
      }
  }

  // То же без повторов. stamp — рабочий массив вызывающего (не короче size()), tag — новое значение обхода.
  template <class F> void queryUnique(const Box2& q, std::vector<u32>& stamp, u32 tag, F&& f) const {
    if (stamp.size() < a_.size()) stamp.resize(a_.size(), 0);
    query(q, [&](int i) {
      if (stamp[size_t(i)] == tag) return;
      stamp[size_t(i)] = tag;
      f(i);
    });
  }

  // Луч вправо от p по строке сетки: visit(i) для элементов ячеек от столбца p.x вправо;
  // после каждой ячейки вызывается stop(xПравойГраницыЯчейки) — true прекращает обход.
  template <class Visit, class Stop> void rayRight(Vec2 p, Visit&& visit, Stop&& stop) const {
    if (nx_ == 0 || !(p.y >= box_.y0 && p.y <= box_.y1) || p.x > box_.x1) return;
    int y = cellY(p.y);
    int x0 = p.x < box_.x0 ? 0 : cellX(p.x);
    for (int x = x0; x < nx_; x++) {
      size_t c = size_t(y) * size_t(nx_) + size_t(x);
      for (u32 k = start_[c], e = start_[c + 1]; k < e; k++) visit(int(items_[k]));
      if (stop(box_.x0 + double(x + 1) * cs_)) break;
    }
  }

  // Поиск ближайшего: обход ячеек кольцами вокруг p. visit(i) может уменьшать best;
  // обход прекращается, когда ни одна непросмотренная ячейка не может быть ближе best.
  template <class Visit> void around(Vec2 p, const double& best, Visit&& visit) const {
    if (nx_ == 0) return;
    int cx = cellX(p.x), cy = cellY(p.y);
    int maxR = std::max({cx, nx_ - 1 - cx, cy, ny_ - 1 - cy});
    auto cellAt = [&](int x, int y) {
      if (cellDist2(p, x, y) > best * best) return;
      size_t c = size_t(y) * size_t(nx_) + size_t(x);
      for (u32 k = start_[c], e = start_[c + 1]; k < e; k++) visit(int(items_[k]));
    };
    for (int r = 0; r <= maxR; r++) {
      if (r >= 1 && double(r - 1) * cs_ > best) break;
      if (r == 0) {
        cellAt(cx, cy);
        continue;
      }
      // только периметр кольца r вокруг (cx, cy), в пределах сетки
      int xa = std::max(cx - r, 0), xb = std::min(cx + r, nx_ - 1);
      if (cy - r >= 0)
        for (int x = xa; x <= xb; x++) cellAt(x, cy - r);
      if (cy + r < ny_)
        for (int x = xa; x <= xb; x++) cellAt(x, cy + r);
      int ya = std::max(cy - r + 1, 0), yb = std::min(cy + r - 1, ny_ - 1);
      if (cx - r >= 0)
        for (int y = ya; y <= yb; y++) cellAt(cx - r, y);
      if (cx + r < nx_)
        for (int y = ya; y <= yb; y++) cellAt(cx + r, y);
    }
  }

  // Ближайший элемент на расстоянии ≤ maxDist (расстояние до отрезка). -1 — нет. accept(i) — фильтр.
  template <class Accept> int nearest(Vec2 p, double maxDist, double* outDist, Accept&& accept) const;
  int nearest(Vec2 p, double maxDist, double* outDist = nullptr) const {
    return nearest(p, maxDist, outDist, [](int) { return true; });
  }

 private:
  static bool finite2(Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }
  static int clampCell(double f, int n) {
    double fl = std::floor(f);
    if (!(fl >= 0)) return 0;  // включая NaN
    if (fl >= double(n)) return n - 1;
    return int(fl);
  }
  double cellDist2(Vec2 p, int x, int y) const {
    double x0 = box_.x0 + x * cs_, y0 = box_.y0 + y * cs_;
    double dx = p.x < x0 ? x0 - p.x : (p.x > x0 + cs_ ? p.x - x0 - cs_ : 0);
    double dy = p.y < y0 ? y0 - p.y : (p.y > y0 + cs_ ? p.y - y0 - cs_ : 0);
    return dx * dx + dy * dy;
  }
  void setup(Box2 bounds, double cellSize, double perCell);
  template <class F> void cellsOfSeg(int i, F&& f) const;
  template <class F> void fill(F&& cellsOf);

  std::vector<Vec2> a_, b_;
  Box2 box_;
  double cs_ = 1, inv_ = 1;
  int nx_ = 0, ny_ = 0;
  std::vector<u32> start_, items_;
};

double distToSeg2Grid(Vec2 p, Vec2 a, Vec2 b);  // то же, что geo::distToSeg2 (без зависимости заголовков)

template <class Accept> int SegGrid::nearest(Vec2 p, double maxDist, double* outDist, Accept&& accept) const {
  double best = maxDist;
  int bi = -1;
  around(p, best, [&](int i) {
    double d = std::sqrt(distToSeg2Grid(p, a_[size_t(i)], b_[size_t(i)]));
    bool better = bi < 0 ? d <= best : (d < best || (d == best && i < bi));
    if (better && accept(i)) { best = d; bi = i; }
  });
  if (outDist) *outDist = bi >= 0 ? best : kInf;
  return bi;
}

}  // namespace rg::geo
