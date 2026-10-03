// Regnum — векторные контуры и аффинные преобразования (общий тип для растеризатора, шрифтов и значков).
#pragma once
#include "base/base.h"

namespace rg::gfx {

struct Pt {
  float x = 0, y = 0;
  constexpr Pt() = default;
  constexpr Pt(float x_, float y_) : x(x_), y(y_) {}
  constexpr Pt operator+(Pt o) const { return {x + o.x, y + o.y}; }
  constexpr Pt operator-(Pt o) const { return {x - o.x, y - o.y}; }
  constexpr Pt operator*(float k) const { return {x * k, y * k}; }
  constexpr bool operator==(const Pt&) const = default;
};

// Аффинное преобразование: x' = a*x + c*y + e; y' = b*x + d*y + f.
struct Affine {
  float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  static Affine translate(float tx, float ty) { return {1, 0, 0, 1, tx, ty}; }
  static Affine scale(float sx, float sy) { return {sx, 0, 0, sy, 0, 0}; }
  static Affine scale(float s) { return {s, 0, 0, s, 0, 0}; }
  static Affine rotate(float rad) { float cs = std::cos(rad), sn = std::sin(rad); return {cs, sn, -sn, cs, 0, 0}; }
  // Композиция: сначала o, затем this.
  Affine operator*(const Affine& o) const {
    return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d, a * o.e + c * o.f + e, b * o.e + d * o.f + f};
  }
  Pt apply(Pt p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
  Pt applyVec(Pt p) const { return {a * p.x + c * p.y, b * p.x + d * p.y}; }
  float det() const { return a * d - b * c; }
  Affine inverse() const {
    float k = det();
    if (std::fabs(k) < 1e-12f) return {};
    float ia = d / k, ib = -b / k, ic = -c / k, id = a / k;
    return {ia, ib, ic, id, -(ia * e + ic * f), -(ib * e + id * f)};
  }
  bool isIdentity() const { return a == 1 && b == 0 && c == 0 && d == 1 && e == 0 && f == 0; }
  bool isTranslateScale() const { return b == 0 && c == 0; }
  float scaleFactor() const { return std::sqrt(std::fabs(det())); }
};

// Контур из подпутей. Кривые хранятся как есть; растеризатор сам их аппроксимирует.
struct Path {
  enum Verb : u8 { Move, Line, Quad, Cubic, Close };
  std::vector<Verb> verbs;
  std::vector<Pt> pts;

  void clear() { verbs.clear(); pts.clear(); }
  bool empty() const { return verbs.empty(); }
  void moveTo(float x, float y) { verbs.push_back(Move); pts.push_back({x, y}); }
  void lineTo(float x, float y) { verbs.push_back(Line); pts.push_back({x, y}); }
  void quadTo(float cx, float cy, float x, float y) { verbs.push_back(Quad); pts.push_back({cx, cy}); pts.push_back({x, y}); }
  void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) { verbs.push_back(Cubic); pts.push_back({c1x, c1y}); pts.push_back({c2x, c2y}); pts.push_back({x, y}); }
  void close() { verbs.push_back(Close); }

  void addRect(const RectF& r);
  void addRoundRect(const RectF& r, float radius);
  void addRoundRect(const RectF& r, float tl, float tr, float br, float bl);  // радиусы углов
  void addCircle(float cx, float cy, float r);
  void addEllipse(float cx, float cy, float rx, float ry);
  void addPolygon(const Pt* p, size_t n, bool closed = true);
  void addPath(const Path& o, const Affine& m = {});
  // Дуга эллипса в стиле SVG (из текущей точки в (x, y)).
  void arcTo(float rx, float ry, float rotDeg, bool largeArc, bool sweep, float x, float y);

  void transform(const Affine& m) { for (auto& p : pts) p = m.apply(p); }
  RectF bounds() const;
  // Аппроксимация ломаными: cb(точки, замкнут). tol — допуск в единицах контура.
  void flatten(float tol, const std::function<void(const std::vector<Pt>&, bool)>& cb) const;
};

}  // namespace rg::gfx
