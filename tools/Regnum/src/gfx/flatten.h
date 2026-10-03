// Regnum — аппроксимация контуров ломаными (внутренний заголовок gfx).
// Число отрезков кривой берётся из оценки второй производной, так что отклонение хорды от кривой не больше tol.
// Кривые целиком вне области видимости спрямляются, частично видимые длинные кривые делятся пополам.
#pragma once
#include "gfx/path.h"

namespace rg::gfx::detail {

constexpr int kMaxCurveSegments = 1024;

inline int segmentsFor(float dd, float k, float tol) {
  float n = std::sqrt(dd * k / tol);
  if (!(n >= 1)) return 1;
  if (n >= float(kMaxCurveSegments)) return kMaxCurveSegments;
  return int(std::ceil(n));
}

// Квадратичная: |B''| = 2|p0 − 2p1 + p2|, отклонение ≤ |B''|·h²/8.
inline int quadSegments(Pt p0, Pt p1, Pt p2, float tol) {
  float dx = p0.x - 2 * p1.x + p2.x, dy = p0.y - 2 * p1.y + p2.y;
  return segmentsFor(std::sqrt(dx * dx + dy * dy), 0.25f, tol);
}

// Кубическая: |B''| ≤ 6·max(|p0 − 2p1 + p2|, |p1 − 2p2 + p3|).
inline int cubicSegments(Pt p0, Pt p1, Pt p2, Pt p3, float tol) {
  float ax = p0.x - 2 * p1.x + p2.x, ay = p0.y - 2 * p1.y + p2.y;
  float bx = p1.x - 2 * p2.x + p3.x, by = p1.y - 2 * p2.y + p3.y;
  float m = std::sqrt(std::max(ax * ax + ay * ay, bx * bx + by * by));
  return segmentsFor(m, 0.75f, tol);
}

// Видимость рамки: 0 — снаружи, 1 — частично, 2 — целиком внутри.
struct CullBox {
  float x0 = -kHuge, y0 = -kHuge, x1 = kHuge, y1 = kHuge;
  static constexpr float kHuge = 3.0e38f;
  int classify(float ax, float ay, float bx, float by) const {
    if (!(ax <= bx) || !(ay <= by)) return 2;  // NaN: решает потребитель
    if (bx < x0 || ax > x1 || by < y0 || ay > y1) return 0;
    if (ax >= x0 && bx <= x1 && ay >= y0 && by <= y1) return 2;
    return 1;
  }
};

template <class Sink>
struct Flattener {
  Sink& sink;
  float tol;
  CullBox cull;

  // Мелкие кривые аппроксимируются точнее: относительная ошибка площади не больше ~0,3 %.
  float tolFor(float w, float h) const {
    float e = std::max(w, h);
    return std::isfinite(e) ? std::min(tol, std::max(e * (1.0f / 512), tol * 0.02f)) : tol;
  }

  void quad(Pt p0, Pt p1, Pt p2, int depth) {
    float ax = std::min({p0.x, p1.x, p2.x}), bx = std::max({p0.x, p1.x, p2.x});
    float ay = std::min({p0.y, p1.y, p2.y}), by = std::max({p0.y, p1.y, p2.y});
    int vis = cull.classify(ax, ay, bx, by);
    if (vis == 0) { sink.line(p2); return; }
    int n = quadSegments(p0, p1, p2, tolFor(bx - ax, by - ay));
    if (vis == 1 && n > 16 && depth < 16) {
      Pt a = (p0 + p1) * 0.5f, b = (p1 + p2) * 0.5f, m = (a + b) * 0.5f;
      quad(p0, a, m, depth + 1);
      quad(m, b, p2, depth + 1);
      return;
    }
    float h = 1.0f / float(n);
    for (int i = 1; i < n; i++) {
      float t = float(i) * h, u = 1 - t;
      float k0 = u * u, k1 = 2 * u * t, k2 = t * t;
      sink.line({k0 * p0.x + k1 * p1.x + k2 * p2.x, k0 * p0.y + k1 * p1.y + k2 * p2.y});
    }
    sink.line(p2);
  }

  void cubic(Pt p0, Pt p1, Pt p2, Pt p3, int depth) {
    float ax = std::min({p0.x, p1.x, p2.x, p3.x}), bx = std::max({p0.x, p1.x, p2.x, p3.x});
    float ay = std::min({p0.y, p1.y, p2.y, p3.y}), by = std::max({p0.y, p1.y, p2.y, p3.y});
    int vis = cull.classify(ax, ay, bx, by);
    if (vis == 0) { sink.line(p3); return; }
    int n = cubicSegments(p0, p1, p2, p3, tolFor(bx - ax, by - ay));
    if (vis == 1 && n > 16 && depth < 16) {
      Pt a = (p0 + p1) * 0.5f, b = (p1 + p2) * 0.5f, c = (p2 + p3) * 0.5f;
      Pt ab = (a + b) * 0.5f, bc = (b + c) * 0.5f, m = (ab + bc) * 0.5f;
      cubic(p0, a, ab, m, depth + 1);
      cubic(m, bc, c, p3, depth + 1);
      return;
    }
    float h = 1.0f / float(n);
    for (int i = 1; i < n; i++) {
      float t = float(i) * h, u = 1 - t;
      float k0 = u * u * u, k1 = 3 * u * u * t, k2 = 3 * u * t * t, k3 = t * t * t;
      sink.line({k0 * p0.x + k1 * p1.x + k2 * p2.x + k3 * p3.x, k0 * p0.y + k1 * p1.y + k2 * p2.y + k3 * p3.y});
    }
    sink.line(p3);
  }
};

// Обход контура: sink.move(Pt), sink.line(Pt), sink.end(bool closed). Точки предварительно преобразуются m.
// Отрезок после Close без Move начинает новый подпуть из начала предыдущего (как в SVG).
template <class Sink>
void flattenPath(const Path& path, const Affine& m, float tol, Sink& sink, const CullBox& cull = {}) {
  if (!(tol > 0)) tol = 0.1f;
  tol = std::max(tol, 1e-6f);
  Flattener<Sink> f{sink, tol, cull};
  const bool ident = m.isIdentity();
  auto X = [&](Pt p) { return ident ? p : m.apply(p); };
  const size_t np = path.pts.size();
  size_t pi = 0;
  bool open = false;
  Pt start = X({0, 0}), cur = start;
  auto ensureOpen = [&] {
    if (!open) { sink.move(cur); start = cur; open = true; }
  };
  for (Path::Verb v : path.verbs) {
    switch (v) {
      case Path::Move:
        if (pi + 1 > np) { if (open) sink.end(false); return; }
        if (open) sink.end(false);
        start = cur = X(path.pts[pi++]);
        sink.move(cur);
        open = true;
        break;
      case Path::Line: {
        if (pi + 1 > np) { if (open) sink.end(false); return; }
        ensureOpen();
        Pt p = X(path.pts[pi++]);
        sink.line(p);
        cur = p;
        break;
      }
      case Path::Quad: {
        if (pi + 2 > np) { if (open) sink.end(false); return; }
        ensureOpen();
        Pt c = X(path.pts[pi]), p = X(path.pts[pi + 1]);
        pi += 2;
        f.quad(cur, c, p, 0);
        cur = p;
        break;
      }
      case Path::Cubic: {
        if (pi + 3 > np) { if (open) sink.end(false); return; }
        ensureOpen();
        Pt c1 = X(path.pts[pi]), c2 = X(path.pts[pi + 1]), p = X(path.pts[pi + 2]);
        pi += 3;
        f.cubic(cur, c1, c2, p, 0);
        cur = p;
        break;
      }
      case Path::Close:
        if (open) {
          sink.end(true);
          open = false;
          cur = start;
        }
        break;
    }
  }
  if (open) sink.end(false);
}

}  // namespace rg::gfx::detail
