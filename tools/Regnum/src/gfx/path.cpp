// Regnum — векторные контуры: фигуры, дуги SVG, границы, аппроксимация ломаными.
#include "gfx/path.h"

#include "gfx/flatten.h"

namespace rg::gfx {

namespace {

// Коэффициент кубической аппроксимации четверти окружности.
constexpr float kArcK = 0.5522847498f;

bool finiteRect(const RectF& r) { return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h); }

RectF normalized(RectF r) {
  if (r.w < 0) { r.x += r.w; r.w = -r.w; }
  if (r.h < 0) { r.y += r.h; r.h = -r.h; }
  return r;
}

float safeRadius(float r) { return std::isfinite(r) && r > 0 ? r : 0.f; }

// Текущая точка контура: после Close — начало замкнутого подпути.
std::optional<Pt> currentPoint(const Path& p) {
  if (p.verbs.empty() || p.pts.empty()) return std::nullopt;
  if (p.verbs.back() != Path::Close) return p.pts.back();
  size_t pi = 0;
  std::optional<Pt> start;
  for (Path::Verb v : p.verbs) {
    switch (v) {
      case Path::Move: if (pi < p.pts.size()) start = p.pts[pi]; pi += 1; break;
      case Path::Line: pi += 1; break;
      case Path::Quad: pi += 2; break;
      case Path::Cubic: pi += 3; break;
      case Path::Close: break;
    }
  }
  return start ? start : std::optional<Pt>(p.pts.back());
}

// Экстремумы кривых по одной оси: корни производной в (0, 1).
void quadExtrema(float a, float b, float c, float& lo, float& hi) {
  float den = a - 2 * b + c;
  if (std::fabs(den) > 1e-12f) {
    float t = (a - b) / den;
    if (t > 0 && t < 1) {
      float u = 1 - t, v = u * u * a + 2 * u * t * b + t * t * c;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  }
}

void cubicExtrema(float a, float b, float c, float d, float& lo, float& hi) {
  // B'(t)/3 = (b−a)(1−t)² + 2(c−b)(1−t)t + (d−c)t² = A t² + B t + C
  double A = double(d) - 3.0 * c + 3.0 * b - a, B = 2.0 * (double(c) - 2.0 * b + a), C = double(b) - a;
  double ts[2];
  int n = 0;
  if (std::fabs(A) < 1e-12) {
    if (std::fabs(B) > 1e-12) ts[n++] = -C / B;
  } else {
    double disc = B * B - 4 * A * C;
    if (disc >= 0) {
      double s = std::sqrt(disc);
      ts[n++] = (-B + s) / (2 * A);
      ts[n++] = (-B - s) / (2 * A);
    }
  }
  for (int i = 0; i < n; i++) {
    double t = ts[i];
    if (!(t > 0 && t < 1)) continue;
    double u = 1 - t;
    float v = float(u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d);
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
}

}  // namespace

void Path::addRect(const RectF& r0) {
  if (!finiteRect(r0)) return;
  RectF r = normalized(r0);
  if (r.w <= 0 || r.h <= 0) return;
  moveTo(r.x, r.y);
  lineTo(r.x + r.w, r.y);
  lineTo(r.x + r.w, r.y + r.h);
  lineTo(r.x, r.y + r.h);
  close();
}

void Path::addRoundRect(const RectF& r, float radius) { addRoundRect(r, radius, radius, radius, radius); }

void Path::addRoundRect(const RectF& r0, float tl, float tr, float br, float bl) {
  if (!finiteRect(r0)) return;
  RectF r = normalized(r0);
  if (r.w <= 0 || r.h <= 0) return;
  tl = safeRadius(tl); tr = safeRadius(tr); br = safeRadius(br); bl = safeRadius(bl);
  // Как в CSS: если соседние радиусы не помещаются на стороне — все уменьшаются пропорционально.
  float f = 1;
  auto fit = [&](float a, float b, float len) { if (a + b > len) f = std::min(f, len / (a + b)); };
  fit(tl, tr, r.w); fit(bl, br, r.w); fit(tl, bl, r.h); fit(tr, br, r.h);
  tl *= f; tr *= f; br *= f; bl *= f;
  if (tl == 0 && tr == 0 && br == 0 && bl == 0) { addRect(r); return; }
  const float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h, k = 1 - kArcK;
  moveTo(x0 + tl, y0);
  if (x1 - tr > x0 + tl) lineTo(x1 - tr, y0);
  if (tr > 0) cubicTo(x1 - tr * k, y0, x1, y0 + tr * k, x1, y0 + tr);
  if (y1 - br > y0 + tr) lineTo(x1, y1 - br);
  if (br > 0) cubicTo(x1, y1 - br * k, x1 - br * k, y1, x1 - br, y1);
  if (x0 + bl < x1 - br) lineTo(x0 + bl, y1);
  if (bl > 0) cubicTo(x0 + bl * k, y1, x0, y1 - bl * k, x0, y1 - bl);
  if (y0 + tl < y1 - bl) lineTo(x0, y0 + tl);
  if (tl > 0) cubicTo(x0, y0 + tl * k, x0 + tl * k, y0, x0 + tl, y0);
  close();
}

void Path::addCircle(float cx, float cy, float r) { addEllipse(cx, cy, r, r); }

void Path::addEllipse(float cx, float cy, float rx, float ry) {
  if (!std::isfinite(cx) || !std::isfinite(cy)) return;
  rx = safeRadius(std::fabs(rx));
  ry = safeRadius(std::fabs(ry));
  if (rx <= 0 || ry <= 0) return;
  const float kx = rx * kArcK, ky = ry * kArcK;
  moveTo(cx + rx, cy);
  cubicTo(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
  cubicTo(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
  cubicTo(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
  cubicTo(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
  close();
}

void Path::addPolygon(const Pt* p, size_t n, bool closed) {
  if (!p || n == 0) return;
  moveTo(p[0].x, p[0].y);
  for (size_t i = 1; i < n; i++) lineTo(p[i].x, p[i].y);
  if (closed) close();
}

void Path::addPath(const Path& o, const Affine& m) {
  if (&o == this) {
    Path copy = o;
    addPath(copy, m);
    return;
  }
  verbs.insert(verbs.end(), o.verbs.begin(), o.verbs.end());
  size_t base = pts.size();
  pts.insert(pts.end(), o.pts.begin(), o.pts.end());
  if (!m.isIdentity())
    for (size_t i = base; i < pts.size(); i++) pts[i] = m.apply(pts[i]);
}

void Path::arcTo(float rx0, float ry0, float rotDeg, bool largeArc, bool sweep, float x, float y) {
  auto cp = currentPoint(*this);
  if (!cp) { moveTo(x, y); return; }
  const double x0 = cp->x, y0 = cp->y;
  if (x0 == x && y0 == y) return;
  double rx = std::fabs(double(rx0)), ry = std::fabs(double(ry0));
  if (!(rx > 0) || !(ry > 0) || !std::isfinite(rx) || !std::isfinite(ry)) { lineTo(x, y); return; }
  const double phi = std::isfinite(rotDeg) ? double(rotDeg) * kPi / 180.0 : 0.0;
  const double cs = std::cos(phi), sn = std::sin(phi);
  // SVG F.6.5: из конечных точек в центр.
  const double dx2 = (x0 - x) * 0.5, dy2 = (y0 - y) * 0.5;
  const double x1p = cs * dx2 + sn * dy2, y1p = -sn * dx2 + cs * dy2;
  // F.6.6: увеличение радиусов, если дуга не помещается.
  double lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
  if (lam > 1) { double s = std::sqrt(lam); rx *= s; ry *= s; }
  const double rx2 = rx * rx, ry2 = ry * ry;
  const double num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
  const double den = rx2 * y1p * y1p + ry2 * x1p * x1p;
  double coef = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0.0;
  if (largeArc == sweep) coef = -coef;
  const double cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
  const double cx = cs * cxp - sn * cyp + (x0 + x) * 0.5, cy = sn * cxp + cs * cyp + (y0 + y) * 0.5;
  auto angle = [](double ux, double uy, double vx, double vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
  const double ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry, vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
  const double th1 = angle(1, 0, ux, uy);
  double dth = angle(ux, uy, vx, vy);
  if (!sweep && dth > 0) dth -= 2 * kPi;
  else if (sweep && dth < 0) dth += 2 * kPi;
  // Не более 90° на кубический сегмент. Нечисловые промежуточные значения (переполнение) — отрезок.
  if (!std::isfinite(dth) || !std::isfinite(th1) || !std::isfinite(cx) || !std::isfinite(cy)) { lineTo(x, y); return; }
  int segs = std::max(1, int(std::ceil(std::fabs(dth) / (kPi * 0.5) - 1e-9)));
  const double step = dth / segs, k = 4.0 / 3.0 * std::tan(step / 4);
  auto map = [&](double px, double py) {
    return Pt{float(cx + cs * rx * px - sn * ry * py), float(cy + sn * rx * px + cs * ry * py)};
  };
  double a = th1;
  for (int i = 0; i < segs; i++) {
    double b = a + step;
    double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
    Pt c1 = map(ca - k * sa, sa + k * ca), c2 = map(cb + k * sb, sb - k * cb);
    Pt e = i + 1 == segs ? Pt{x, y} : map(cb, sb);
    cubicTo(c1.x, c1.y, c2.x, c2.y, e.x, e.y);
    a = b;
  }
}

RectF Path::bounds() const {
  float x0 = std::numeric_limits<float>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
  auto addPt = [&](Pt p) {
    x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
    x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
  };
  size_t pi = 0;
  const size_t np = pts.size();
  Pt cur{0, 0}, start{0, 0};
  bool any = false;
  for (Verb v : verbs) {
    switch (v) {
      case Move:
        if (pi + 1 > np) break;
        start = cur = pts[pi++];
        addPt(cur);
        any = true;
        break;
      case Line:
        if (pi + 1 > np) break;
        if (!any) { addPt(cur); any = true; }
        cur = pts[pi++];
        addPt(cur);
        break;
      case Quad: {
        if (pi + 2 > np) break;
        if (!any) { addPt(cur); any = true; }
        Pt c = pts[pi], p = pts[pi + 1];
        pi += 2;
        addPt(p);
        quadExtrema(cur.x, c.x, p.x, x0, x1);
        quadExtrema(cur.y, c.y, p.y, y0, y1);
        cur = p;
        break;
      }
      case Cubic: {
        if (pi + 3 > np) break;
        if (!any) { addPt(cur); any = true; }
        Pt c1 = pts[pi], c2 = pts[pi + 1], p = pts[pi + 2];
        pi += 3;
        addPt(p);
        cubicExtrema(cur.x, c1.x, c2.x, p.x, x0, x1);
        cubicExtrema(cur.y, c1.y, c2.y, p.y, y0, y1);
        cur = p;
        break;
      }
      case Close: cur = start; break;
    }
  }
  if (!any || !(x0 <= x1) || !(y0 <= y1)) return {};
  return {x0, y0, x1 - x0, y1 - y0};
}

void Path::flatten(float tol, const std::function<void(const std::vector<Pt>&, bool)>& cb) const {
  struct Sink {
    const std::function<void(const std::vector<Pt>&, bool)>& cb;
    std::vector<Pt> pts;
    void move(Pt p) { pts.clear(); pts.push_back(p); }
    void line(Pt p) { pts.push_back(p); }
    void end(bool closed) {
      if (pts.size() >= 2 || (closed && !pts.empty())) cb(pts, closed);
      pts.clear();
    }
  } sink{cb, {}};
  detail::flattenPath(*this, Affine{}, tol, sink);
}

}  // namespace rg::gfx
