// Regnum — равномерная сетка отрезков (CSR).
#include "geo/grid.h"

namespace rg::geo {

double distToSeg2Grid(Vec2 p, Vec2 a, Vec2 b) {
  Vec2 d = b - a;
  double l2 = d.len2();
  double t = l2 > 0 ? clamp((p - a).dot(d) / l2, 0.0, 1.0) : 0.0;
  return dist2(p, a + d * t);
}

void SegGrid::setup(Box2 bounds, double cellSize, double perCell) {
  box_ = bounds;
  for (size_t i = 0; i < a_.size(); i++) {
    if (finite2(a_[i])) box_.add(a_[i]);
    if (finite2(b_[i])) box_.add(b_[i]);
  }
  nx_ = ny_ = 0;
  start_.clear();
  items_.clear();
  if (a_.empty() || box_.empty()) return;
  double w = std::max(box_.w(), 1e-9), h = std::max(box_.h(), 1e-9);
  if (!(cellSize > 0)) {
    double target = std::max(1.0, double(a_.size()) / std::max(perCell, 0.01));
    cellSize = std::sqrt(w * h / target);
  }
  // Не более 4096 ячеек по стороне и не тоньше габаритов, делённых на 4096.
  cellSize = std::max({cellSize, w / 4096.0, h / 4096.0, 1e-9});
  // Общее число ячеек — не более 4 млн.
  if ((w / cellSize) * (h / cellSize) > 4e6) cellSize = std::sqrt(w * h / 4e6);
  cs_ = cellSize;
  inv_ = 1.0 / cs_;
  nx_ = std::max(1, int(std::ceil(w * inv_)));
  ny_ = std::max(1, int(std::ceil(h * inv_)));
  nx_ = std::min(nx_, 4096);
  ny_ = std::min(ny_, 4096);
}

// Ячейки, через которые проходит отрезок i: по строкам — x-диапазон части отрезка внутри полосы строки.
template <class F> void SegGrid::cellsOfSeg(int i, F&& f) const {
  Vec2 a = a_[size_t(i)], b = b_[size_t(i)];
  if (!finite2(a) || !finite2(b)) return;
  if (a == b) {
    f(cellY(a.y) * nx_ + cellX(a.x));
    return;
  }
  double ylo = std::min(a.y, b.y), yhi = std::max(a.y, b.y);
  int r0 = cellY(ylo), r1 = cellY(yhi);
  double margin = cs_ * 1e-9 + 1e-12;
  double dy = b.y - a.y;
  for (int r = r0; r <= r1; r++) {
    double xlo, xhi;
    if (r0 == r1 || dy == 0) {
      xlo = std::min(a.x, b.x);
      xhi = std::max(a.x, b.x);
    } else {
      double by0 = box_.y0 + r * cs_, by1 = by0 + cs_;
      double t0 = (by0 - a.y) / dy, t1 = (by1 - a.y) / dy;
      double tmin = clamp(std::min(t0, t1), 0.0, 1.0), tmax = clamp(std::max(t0, t1), 0.0, 1.0);
      double xa = a.x + (b.x - a.x) * tmin, xb = a.x + (b.x - a.x) * tmax;
      xlo = std::min(xa, xb);
      xhi = std::max(xa, xb);
    }
    int c0 = cellX(xlo - margin), c1 = cellX(xhi + margin);
    for (int c = c0; c <= c1; c++) f(r * nx_ + c);
  }
}

template <class F> void SegGrid::fill(F&& cellsOf) {
  size_t nc = size_t(nx_) * size_t(ny_);
  start_.assign(nc + 1, 0);
  for (int i = 0; i < int(a_.size()); i++) cellsOf(i, [&](int c) { start_[size_t(c) + 1]++; });
  for (size_t c = 0; c < nc; c++) start_[c + 1] += start_[c];
  items_.assign(start_[nc], 0);
  std::vector<u32> pos(start_.begin(), start_.end() - 1);
  for (int i = 0; i < int(a_.size()); i++) cellsOf(i, [&](int c) { items_[pos[size_t(c)]++] = u32(i); });
}

void SegGrid::build(std::vector<Vec2> a, std::vector<Vec2> b, Box2 bounds, double cellSize, double perCell) {
  a_ = std::move(a);
  b_ = std::move(b);
  if (b_.size() != a_.size()) b_.resize(a_.size());
  setup(bounds, cellSize, perCell);
  if (nx_ == 0) return;
  fill([this](int i, auto&& f) { cellsOfSeg(i, f); });
}

void SegGrid::buildBoxes(const std::vector<Box2>& boxes, Box2 bounds, double cellSize, double perCell) {
  a_.clear();
  b_.clear();
  a_.reserve(boxes.size());
  b_.reserve(boxes.size());
  for (auto& bx : boxes) {
    if (bx.empty()) {
      a_.push_back({kInf, kInf});
      b_.push_back({kInf, kInf});
    } else {
      a_.push_back({bx.x0, bx.y0});
      b_.push_back({bx.x1, bx.y1});
    }
  }
  setup(bounds, cellSize, perCell);
  if (nx_ == 0) return;
  fill([this](int i, auto&& f) {
    Vec2 lo = a_[size_t(i)], hi = b_[size_t(i)];
    if (!finite2(lo) || !finite2(hi)) return;
    int x0 = cellX(lo.x), x1 = cellX(hi.x), y0 = cellY(lo.y), y1 = cellY(hi.y);
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) f(y * nx_ + x);
  });
}

}  // namespace rg::geo
