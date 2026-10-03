// Regnum — значки: отрисовка векторных глифов (маска из слоёв, подгонка к пикселям, кеш) и реестр значков.
#include "gfx/icons.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "gfx/figures.h"
#include "gfx/icons_shapes.h"
#include "gfx/raster.h"
#include "gfx/stroke.h"
#include "gfx/svgpath.h"

namespace rg::gfx {

// ================================================================ отрисовка глифов
namespace {

constexpr float kHintStrength = 0.6f;   // доля приближения толщины штриха к целому числу пикселей
constexpr float kAxisEps = 0.02f;       // допуск «вертикальности» отрезка (единицы сетки)
constexpr int kCacheMaxDim = 192;       // маски крупнее не кешируются
constexpr size_t kCacheGeneration = 1500;

// Сведение покрытия слоя в маску: объединение (максимум) или вычитание.
struct CombineSink final : SpanSink {
  Mask& m;
  int ox, oy;
  bool erase;
  u32 k;  // множитель покрытия, 256 = 1
  CombineSink(Mask& m_, int x, int y, bool e, u32 k_) : m(m_), ox(x), oy(y), erase(e), k(k_) {}
  inline void put(u8& d, u32 c) const {
    const u32 v = (c * k) >> 8;
    if (erase) d = u8((u32(d) * (255 - v) + 127) / 255);
    else if (v > d) d = u8(v);
  }
  void row(int y, const Span* s, int n) override {
    u8* d = m.row(y - oy);
    for (int i = 0; i < n; i++) {
      u8* p = d + (s[i].x - ox);
      const int len = s[i].len;
      if (s[i].cov) {
        for (int j = 0; j < len; j++) put(p[j], s[i].cov[j]);
      } else {
        for (int j = 0; j < len; j++) put(p[j], s[i].value);
      }
    }
  }
};

// Опорная координата подгонки: исходное значение v (пиксели устройства) и цель t.
struct Anchor {
  float v, t;
  bool strong;
};

// Монотонное кусочно-линейное отображение одной оси по опорным точкам.
class AxisFit {
 public:
  void clear() { a_.clear(); }
  void add(float v, float t, bool strong) { a_.push_back({v, t, strong}); }
  void build() {
    std::sort(a_.begin(), a_.end(), [](const Anchor& x, const Anchor& y) {
      return x.v != y.v ? x.v < y.v : (x.strong && !y.strong);
    });
    std::vector<Anchor> out;
    out.reserve(a_.size());
    for (const Anchor& x : a_) {
      if (!out.empty() && x.v - out.back().v < 0.02f) {
        if (x.strong && !out.back().strong) out.back() = x;
        continue;
      }
      bool keep = true;
      while (!out.empty() && x.t < out.back().t) {
        if (x.strong && !out.back().strong) out.pop_back();
        else { keep = false; break; }
      }
      if (keep) out.push_back(x);
    }
    a_.swap(out);
  }
  float map(float x) const {
    if (a_.empty()) return x;
    if (x <= a_.front().v) return x + (a_.front().t - a_.front().v);
    if (x >= a_.back().v) return x + (a_.back().t - a_.back().v);
    auto it = std::upper_bound(a_.begin(), a_.end(), x, [](float q, const Anchor& a) { return q < a.v; });
    const Anchor& b = *it;
    const Anchor& a = *(it - 1);
    const float u = (x - a.v) / (b.v - a.v);
    return a.t + (b.t - a.t) * u;
  }
  bool empty() const { return a_.empty(); }

 private:
  std::vector<Anchor> a_;
};

// Центр штриха нечётной толщины — в центр пикселя, чётной — на границу. Равноудалённость — к центру глифа.
float snapStroke(float X, int n, float center) {
  if (n % 2 == 1) {
    const float f = std::floor(X), frac = X - f;
    if (frac < 0.02f || frac > 0.98f) {
      const float e = std::floor(X + 0.5f);
      return X > center ? e - 0.5f : e + 0.5f;
    }
    return f + 0.5f;
  }
  const float frac = X - std::floor(X);
  if (std::fabs(frac - 0.5f) < 0.02f) return X > center ? std::floor(X) : std::ceil(X);
  return std::floor(X + 0.5f);
}

// Край заливки — на границу пикселя; равноудалённость — от центра глифа (фигура не худеет).
float snapEdge(float X, float center) {
  const float frac = X - std::floor(X);
  if (std::fabs(frac - 0.5f) < 0.02f) return X > center ? std::ceil(X) : std::floor(X);
  return std::floor(X + 0.5f);
}

struct HintFrame {
  float k, ox, oy, cx, cy;
};

// Опорные точки контура: осевые отрезки (сильные) и осевые касательные на концах кривых (слабые).
void collectAnchors(const Path& p, bool stroke, int n, const HintFrame& h, AxisFit& fx, AxisFit& fy) {
  auto addX = [&](float x, bool strong) {
    const float X = h.ox + h.k * x;
    fx.add(X, stroke ? snapStroke(X, n, h.cx) : snapEdge(X, h.cx), strong);
  };
  auto addY = [&](float y, bool strong) {
    const float Y = h.oy + h.k * y;
    fy.add(Y, stroke ? snapStroke(Y, n, h.cy) : snapEdge(Y, h.cy), strong);
  };
  auto seg = [&](Pt a, Pt b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    if (std::fabs(dx) <= kAxisEps && std::fabs(dy) >= 1.0f) addX((a.x + b.x) * 0.5f, true);
    else if (std::fabs(dy) <= kAxisEps && std::fabs(dx) >= 1.0f) addY((a.y + b.y) * 0.5f, true);
  };
  auto tangent = [&](Pt at, Pt d) {
    const float l = std::sqrt(d.x * d.x + d.y * d.y);
    if (!(l > 1e-4f)) return;
    if (std::fabs(d.x) <= kAxisEps * l) addX(at.x, false);
    else if (std::fabs(d.y) <= kAxisEps * l) addY(at.y, false);
  };
  size_t pi = 0;
  const size_t np = p.pts.size();
  Pt cur{0, 0}, start{0, 0};
  for (Path::Verb v : p.verbs) {
    switch (v) {
      case Path::Move:
        if (pi + 1 > np) return;
        cur = start = p.pts[pi++];
        break;
      case Path::Line:
        if (pi + 1 > np) return;
        seg(cur, p.pts[pi]);
        cur = p.pts[pi++];
        break;
      case Path::Quad: {
        if (pi + 2 > np) return;
        const Pt c = p.pts[pi], e = p.pts[pi + 1];
        tangent(cur, c - cur);
        tangent(e, e - c);
        cur = e;
        pi += 2;
        break;
      }
      case Path::Cubic: {
        if (pi + 3 > np) return;
        const Pt c1 = p.pts[pi], c2 = p.pts[pi + 1], e = p.pts[pi + 2];
        Pt d0 = c1 - cur;
        if (d0.x * d0.x + d0.y * d0.y < 1e-8f) d0 = c2 - cur;
        Pt d1 = e - c2;
        if (d1.x * d1.x + d1.y * d1.y < 1e-8f) d1 = e - c1;
        tangent(cur, d0);
        tangent(e, d1);
        cur = e;
        pi += 3;
        break;
      }
      case Path::Close:
        seg(cur, start);
        cur = start;
        break;
    }
  }
}

inline bool isStrokeKind(VecLayer::Kind k) { return k == VecLayer::Stroke || k == VecLayer::EraseStroke; }
inline bool isEraseKind(VecLayer::Kind k) { return k == VecLayer::Erase || k == VecLayer::EraseStroke; }

int hintedPixels(float w) { return std::max(1, int(std::lround(w))); }

float maxStrokeWidth(const VecGlyph& g, const GlyphStyle& st) {
  float m = 0;
  for (const VecLayer& L : g.layers)
    if (isStrokeKind(L.kind)) m = std::max(m, (L.width > 0 ? L.width : st.strokeWidth) * st.strokeScale);
  return m;
}

// ---------------------------------------------------------------- кеш масок
struct MaskKey {
  u64 id;
  u32 size, scale, width;
  u8 flags;
  bool operator==(const MaskKey&) const = default;
};
struct MaskKeyHash {
  size_t operator()(const MaskKey& k) const {
    u64 h = hashMix(k.id, (u64(k.size) << 32) | k.scale);
    h = hashMix(h, (u64(k.width) << 8) | k.flags);
    return size_t(h);
  }
};

struct CachedMask {
  Mask mask;
  int margin = 0;
};

// Кеш из двух поколений: промах в текущем ищется в прошлом; переполнение текущего делает его прошлым.
class MaskCache {
 public:
  std::shared_ptr<const CachedMask> get(const MaskKey& k) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = cur_.find(k);
    if (it != cur_.end()) return it->second;
    auto jt = old_.find(k);
    if (jt == old_.end()) return nullptr;
    auto v = jt->second;
    insertLocked(k, v);
    return v;
  }
  void put(const MaskKey& k, std::shared_ptr<const CachedMask> v) {
    std::lock_guard<std::mutex> lock(mu_);
    insertLocked(k, std::move(v));
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mu_);
    cur_.clear();
    old_.clear();
  }
  size_t size() {
    std::lock_guard<std::mutex> lock(mu_);
    return cur_.size() + old_.size();
  }

 private:
  void insertLocked(const MaskKey& k, std::shared_ptr<const CachedMask> v) {
    if (cur_.size() >= kCacheGeneration) {
      old_.swap(cur_);
      cur_.clear();
    }
    cur_[k] = std::move(v);
  }
  std::mutex mu_;
  std::unordered_map<MaskKey, std::shared_ptr<const CachedMask>, MaskKeyHash> cur_, old_;
};

MaskCache& maskCache() {
  static MaskCache c;
  return c;
}

u32 fbits(float f) {
  u32 u;
  std::memcpy(&u, &f, 4);
  return u;
}

// Залить маску в пикселях устройства (преобразование холста временно сбрасывается).
void blitMask(Canvas& c, const Mask& m, int x, int y, Color color) {
  c.save();
  c.setTransform(Affine{});
  c.fillMask(m, float(x), float(y), Paint(color));
  c.restore();
}

}  // namespace

void renderGlyphMask(const VecGlyph& g, const Affine& T, Mask& out, int ox, int oy, const GlyphStyle& st) {
  std::fill(out.a.begin(), out.a.end(), u8(0));
  if (out.empty() || g.layers.empty()) return;
  const float sc = T.scaleFactor();
  if (!(sc > 0) || !std::isfinite(sc)) return;
  const float tolA = 1e-4f * std::max(std::fabs(T.a), std::fabs(T.b));
  const bool conformal = std::fabs(T.a - T.d) <= tolA && std::fabs(T.b + T.c) <= tolA;
  const bool axis = conformal && T.b == 0 && T.c == 0 && T.a > 0;
  const float k = T.a;
  const bool hint = st.hint && axis && k * g.grid <= st.hintMaxPx + 0.01f;

  thread_local AxisFit fx, fy;
  thread_local Rasterizer ras;
  thread_local Path dev, outline;
  fx.clear();
  fy.clear();
  if (hint) {
    const HintFrame hf{k, T.e, T.f, T.e + k * g.grid * 0.5f, T.f + k * g.grid * 0.5f};
    for (const VecLayer& L : g.layers) {
      if (isEraseKind(L.kind)) continue;
      const bool stroke = L.kind == VecLayer::Stroke;
      const float w = (L.width > 0 ? L.width : st.strokeWidth) * st.strokeScale * k;
      collectAnchors(L.path, stroke, hintedPixels(w), hf, fx, fy);
    }
    fx.build();
    fy.build();
  }

  const RectI clip{ox, oy, out.w, out.h};
  for (const VecLayer& L : g.layers) {
    if (L.path.empty()) continue;
    const bool stroke = isStrokeKind(L.kind);
    const float wGrid = (L.width > 0 ? L.width : st.strokeWidth) * st.strokeScale;
    u32 kcov = 256;
    ras.reset(clip);
    if (conformal) {
      // Контур в пикселях устройства (с подгонкой), обводка — там же: толщина и скругления точные.
      dev = L.path;
      for (Pt& p : dev.pts) {
        p = T.apply(p);
        if (hint) p = {fx.map(p.x), fy.map(p.y)};
      }
      if (stroke) {
        float w = wGrid * sc;
        if (hint) w += (float(hintedPixels(w)) - w) * kHintStrength;
        if (w < 1) { kcov = u32(std::max(0.f, w) * 256 + 0.5f); w = 1; }
        Stroke s;
        s.width = w;
        s.join = st.join;
        s.cap = st.cap;
        strokeToPath(dev, s, 0.05f, outline);
        ras.addPath(outline);
      } else {
        ras.addPath(dev);
      }
    } else {
      // Наклон или неравномерный масштаб: обводка в единицах сетки, затем преобразование.
      if (stroke) {
        float w = wGrid;
        if (w * sc < 1) { kcov = u32(std::max(0.f, w * sc) * 256 + 0.5f); w = 1 / sc; }
        Stroke s;
        s.width = w;
        s.join = st.join;
        s.cap = st.cap;
        strokeToPath(L.path, s, 0.05f / sc, outline);
        ras.addPath(outline, T);
      } else {
        ras.addPath(L.path, T);
      }
    }
    if (ras.invalid() || ras.empty() || kcov == 0) continue;
    CombineSink sink(out, ox, oy, isEraseKind(L.kind), kcov);
    ras.sweep(L.kind == VecLayer::FillEvenOdd ? FillRule::EvenOdd : FillRule::NonZero, sink);
  }
}

void drawGlyph(Canvas& c, const VecGlyph& g, RectF rect, Color color, const GlyphStyle& st) {
  if (color.a == 0 || g.layers.empty() || !(g.grid > 0)) return;
  const float s = std::min(rect.w, rect.h);
  if (!(s > 0) || !std::isfinite(s) || !std::isfinite(rect.x) || !std::isfinite(rect.y)) return;
  const float ux = rect.cx() - s * 0.5f, uy = rect.cy() - s * 0.5f;
  const float overflow = maxStrokeWidth(g, st) / g.grid * s;  // обводка может выступать за сетку
  if (c.quickReject({ux - overflow, uy - overflow, s + 2 * overflow, s + 2 * overflow})) return;
  const RectI clipB = c.clipBounds();
  if (clipB.empty()) return;

  const Affine& M = c.transform();
  const bool axis = M.b == 0 && M.c == 0 && M.a > 0 && std::fabs(M.a - M.d) <= 1e-4f * M.a;
  if (axis) {
    float S = s * M.a;
    if (!(S >= 0.5f) || S > 1.0e5f) return;
    const Pt cd = M.apply({rect.cx(), rect.cy()});
    const bool hint = st.hint && S <= st.hintMaxPx;
    S = hint ? std::max(1.f, std::round(S)) : std::round(S * 64) / 64;
    const float X0 = std::round(cd.x - S * 0.5f), Y0 = std::round(cd.y - S * 0.5f);
    if (!(std::fabs(X0) < 1e8f && std::fabs(Y0) < 1e8f)) return;
    const float k = S / g.grid;
    const int margin = int(std::ceil(maxStrokeWidth(g, st) * k * 0.5f + 1.5f));
    const int dim = int(std::ceil(S)) + 2 * margin;
    const int mx = int(X0) - margin, my = int(Y0) - margin;
    if (g.id != 0 && dim <= kCacheMaxDim) {
      const MaskKey key{g.id, fbits(S), fbits(st.strokeScale), fbits(st.strokeWidth),
                        u8((st.hint ? 1 : 0) | (u8(st.join) << 1) | (u8(st.cap) << 3))};
      auto cm = maskCache().get(key);
      if (!cm) {
        auto fresh = std::make_shared<CachedMask>();
        fresh->margin = margin;
        fresh->mask = Mask(dim, dim);
        renderGlyphMask(g, Affine{k, 0, 0, k, float(margin), float(margin)}, fresh->mask, 0, 0, st);
        cm = fresh;
        maskCache().put(key, cm);
      }
      blitMask(c, cm->mask, mx, my, color);
      return;
    }
    // Крупный или временный глиф: только видимая часть.
    const RectI area = RectI{mx, my, dim, dim}.intersect(clipB);
    if (area.empty()) return;
    Mask m(area.w, area.h);
    renderGlyphMask(g, Affine{k, 0, 0, k, X0, Y0}, m, area.x, area.y, st);
    blitMask(c, m, area.x, area.y, color);
    return;
  }
  // Поворот, наклон, отражение: маска по габаритам повёрнутого квадрата.
  const float kg = s / g.grid;
  const Affine T = M * Affine{kg, 0, 0, kg, ux, uy};
  const float ov = maxStrokeWidth(g, st) * 0.5f + 0.5f;
  const Pt corners[4] = {T.apply({-ov, -ov}), T.apply({g.grid + ov, -ov}), T.apply({g.grid + ov, g.grid + ov}),
                         T.apply({-ov, g.grid + ov})};
  float x0 = corners[0].x, x1 = x0, y0 = corners[0].y, y1 = y0;
  for (const Pt& p : corners) {
    x0 = std::min(x0, p.x); x1 = std::max(x1, p.x);
    y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
  }
  if (!(std::isfinite(x0) && std::isfinite(x1) && std::isfinite(y0) && std::isfinite(y1))) return;
  x0 = std::clamp(x0, -1e8f, 1e8f); x1 = std::clamp(x1, -1e8f, 1e8f);
  y0 = std::clamp(y0, -1e8f, 1e8f); y1 = std::clamp(y1, -1e8f, 1e8f);
  const int ix0 = int(std::floor(x0)) - 1, iy0 = int(std::floor(y0)) - 1;
  const RectI area = RectI{ix0, iy0, int(std::ceil(x1)) + 1 - ix0, int(std::ceil(y1)) + 1 - iy0}.intersect(clipB);
  if (area.empty()) return;
  Mask m(area.w, area.h);
  renderGlyphMask(g, T, m, area.x, area.y, st);
  blitMask(c, m, area.x, area.y, color);
}

void clearGlyphCache() { maskCache().clear(); }
size_t glyphCacheSize() { return maskCache().size(); }

// ================================================================ построитель реестров
namespace shapes {

std::string n(double v) { return svgNum(v, 3); }

std::string circ(double cx, double cy, double r) {
  return "M" + n(cx - r) + " " + n(cy) + "A" + n(r) + " " + n(r) + " 0 1 0 " + n(cx + r) + " " + n(cy) + "A" + n(r) + " " +
         n(r) + " 0 1 0 " + n(cx - r) + " " + n(cy) + "Z";
}

std::string ell(double cx, double cy, double rx, double ry) {
  return "M" + n(cx - rx) + " " + n(cy) + "A" + n(rx) + " " + n(ry) + " 0 1 0 " + n(cx + rx) + " " + n(cy) + "A" + n(rx) +
         " " + n(ry) + " 0 1 0 " + n(cx - rx) + " " + n(cy) + "Z";
}

std::string rrect(double x, double y, double w, double h, double r) {
  r = std::max(0.0, std::min({r, w / 2, h / 2}));
  if (r <= 0) return "M" + n(x) + " " + n(y) + "H" + n(x + w) + "V" + n(y + h) + "H" + n(x) + "Z";
  const std::string a = "A" + n(r) + " " + n(r) + " 0 0 1 ";
  return "M" + n(x + r) + " " + n(y) + "H" + n(x + w - r) + a + n(x + w) + " " + n(y + r) + "V" + n(y + h - r) + a +
         n(x + w - r) + " " + n(y + h) + "H" + n(x + r) + a + n(x) + " " + n(y + h - r) + "V" + n(y + r) + a + n(x + r) +
         " " + n(y) + "Z";
}

std::string poly(std::initializer_list<double> xy, bool closed) {
  std::string s;
  int i = 0;
  double x = 0;
  for (double v : xy) {
    if (i % 2 == 0) x = v;
    else s += (i == 1 ? "M" : "L") + n(x) + " " + n(v);
    i++;
  }
  if (closed && !s.empty()) s += "Z";
  return s;
}

std::string star(double cx, double cy, double R, double r, int points, double rotDeg) {
  std::string s;
  const int m = std::max(2, points) * 2;
  for (int i = 0; i < m; i++) {
    const double a = (rotDeg + 180.0 * i / (m / 2)) * kPi / 180.0;
    const double rad = i % 2 == 0 ? R : r;
    s += (i == 0 ? "M" : "L") + n(cx + rad * std::cos(a)) + " " + n(cy + rad * std::sin(a));
  }
  return s + "Z";
}

std::string gear(double cx, double cy, double ro, double ri, int teeth, double topHalfDeg, double baseHalfDeg) {
  std::string s;
  auto pt = [&](double r, double deg) {
    const double a = deg * kPi / 180.0;
    return n(cx + r * std::cos(a)) + " " + n(cy + r * std::sin(a));
  };
  const double step = 360.0 / teeth;
  for (int i = 0; i < teeth; i++) {
    const double a = -90 + i * step;
    s += (i == 0 ? "M" : "L") + pt(ri, a - baseHalfDeg);
    s += "L" + pt(ro, a - topHalfDeg);
    s += "A" + n(ro) + " " + n(ro) + " 0 0 1 " + pt(ro, a + topHalfDeg);
    s += "L" + pt(ri, a + baseHalfDeg);
    s += "A" + n(ri) + " " + n(ri) + " 0 0 1 " + pt(ri, a + step - baseHalfDeg);
  }
  return s + "Z";
}

std::string arc(double cx, double cy, double r, double fromDeg, double toDeg) {
  const double a0 = fromDeg * kPi / 180.0, a1 = toDeg * kPi / 180.0;
  const double sweepDeg = toDeg - fromDeg;
  const bool large = std::fabs(sweepDeg) > 180.0;
  const bool sweep = sweepDeg > 0;
  return "M" + n(cx + r * std::cos(a0)) + " " + n(cy + r * std::sin(a0)) + "A" + n(r) + " " + n(r) + " 0 " +
         (large ? "1 " : "0 ") + (sweep ? "1 " : "0 ") + n(cx + r * std::cos(a1)) + " " + n(cy + r * std::sin(a1));
}

std::string mirrorX(std::string_view d, double axis) {
  Path p = svgPath(d);
  p.transform(Affine{-1, 0, 0, 1, float(2 * axis), 0});
  return toSvgPath(p, 3);
}

std::string dashedCircle(double cx, double cy, double r, int dashes, double fill) {
  std::string s;
  const double step = 360.0 / dashes;
  for (int i = 0; i < dashes; i++) {
    const double a = -90 + i * step + step * (1 - fill) * 0.5;
    s += arc(cx, cy, r, a, a + step * fill);
  }
  return s;
}

}  // namespace shapes

// Подпути заливки ориентируются одинаково: NonZero даёт объединение фигур, а не дыры на пересечениях
// кругов и многоугольников, описанных в разных направлениях.
static Path unifyWinding(const Path& p) {
  struct Seg {
    Path::Verb v;
    Pt c1, c2, end;
  };
  Path out;
  std::vector<Seg> segs;
  Pt start{0, 0};
  bool closed = false, have = false;
  auto flush = [&]() {
    if (!have) return;
    Path sub;
    sub.moveTo(start.x, start.y);
    for (const Seg& s : segs) {
      if (s.v == Path::Line) sub.lineTo(s.end.x, s.end.y);
      else if (s.v == Path::Quad) sub.quadTo(s.c1.x, s.c1.y, s.end.x, s.end.y);
      else sub.cubicTo(s.c1.x, s.c1.y, s.c2.x, s.c2.y, s.end.x, s.end.y);
    }
    double area = 0;
    sub.flatten(0.05f, [&](const std::vector<Pt>& q, bool) {
      for (size_t i = 0, n = q.size(); i < n; i++) {
        const Pt a = q[i], b = q[(i + 1) % n];
        area += double(a.x) * b.y - double(b.x) * a.y;
      }
    });
    if (area >= 0) {
      out.addPath(sub);
    } else {
      const Pt last = segs.empty() ? start : segs.back().end;
      out.moveTo(last.x, last.y);
      for (size_t i = segs.size(); i-- > 0;) {
        const Pt prev = i > 0 ? segs[i - 1].end : start;
        const Seg& s = segs[i];
        if (s.v == Path::Line) out.lineTo(prev.x, prev.y);
        else if (s.v == Path::Quad) out.quadTo(s.c1.x, s.c1.y, prev.x, prev.y);
        else out.cubicTo(s.c2.x, s.c2.y, s.c1.x, s.c1.y, prev.x, prev.y);
      }
    }
    if (closed) out.close();
    segs.clear();
    closed = false;
    have = false;
  };
  size_t pi = 0;
  const size_t np = p.pts.size();
  Pt cur{0, 0};
  for (Path::Verb v : p.verbs) {
    if (v != Path::Move && v != Path::Close && !have) {
      // Рисование после Z без M — от начала прежнего подпути.
      have = true;
      cur = start;
    }
    switch (v) {
      case Path::Move:
        if (pi + 1 > np) return out;
        flush();
        start = cur = p.pts[pi++];
        have = true;
        break;
      case Path::Line:
        if (pi + 1 > np) return out;
        segs.push_back({Path::Line, {}, {}, p.pts[pi]});
        cur = p.pts[pi++];
        break;
      case Path::Quad:
        if (pi + 2 > np) return out;
        segs.push_back({Path::Quad, p.pts[pi], {}, p.pts[pi + 1]});
        cur = p.pts[pi + 1];
        pi += 2;
        break;
      case Path::Cubic:
        if (pi + 3 > np) return out;
        segs.push_back({Path::Cubic, p.pts[pi], p.pts[pi + 1], p.pts[pi + 2]});
        cur = p.pts[pi + 2];
        pi += 3;
        break;
      case Path::Close:
        if (have) {
          closed = true;
          flush();
        }
        cur = start;
        break;
    }
  }
  flush();
  return out;
}

GlyphBuilder::GlyphBuilder(float grid, u64 idBase) : grid_(grid), idBase_(idBase) {}

void GlyphBuilder::addList(const char* name, const std::vector<L>& layers) {
  VecGlyph g;
  g.grid = grid_;
  for (const L& l : layers) {
    if (l.op == L::Use) {
      const VecGlyph* src = find(l.d);
      if (!src) {
        issues_.push_back(std::string(name) + ": нет глифа «" + l.d + "» для вставки");
        continue;
      }
      for (const VecLayer& s : src->layers) {
        VecLayer c = s;
        if (!l.xf.isIdentity()) c.path.transform(l.xf);
        if (l.width > 0 && isStrokeKind(c.kind)) c.width = l.width;
        g.layers.push_back(std::move(c));
      }
      continue;
    }
    VecLayer v;
    v.kind = l.kind;
    v.width = l.width;
    SvgPathError err;
    if (!parseSvgPath(l.d, v.path, &err))
      issues_.push_back(std::string(name) + ": ошибка пути в позиции " + std::to_string(err.pos) + " (" + err.msg + ")");
    if (v.path.empty()) {
      issues_.push_back(std::string(name) + ": пустой слой");
      continue;
    }
    if (!l.xf.isIdentity()) v.path.transform(l.xf);
    if (v.kind == VecLayer::Fill || v.kind == VecLayer::Erase) v.path = unifyWinding(v.path);
    g.layers.push_back(std::move(v));
  }
  // Проверка: контуры внутри сетки (обводка может выступать не более чем на полтолщины).
  for (const VecLayer& v : g.layers) {
    const RectF b = v.path.bounds();
    const float w = isStrokeKind(v.kind) ? (v.width > 0 ? v.width : 1.75f) * 0.5f : 0.f;
    if (b.x - w < -0.05f || b.y - w < -0.05f || b.right() + w > grid_ + 0.05f || b.bottom() + w > grid_ + 0.05f)
      if (!isEraseKind(v.kind))
        issues_.push_back(std::string(name) + ": выход за сетку (" + svgNum(b.x) + ", " + svgNum(b.y) + ", " +
                          svgNum(b.right()) + ", " + svgNum(b.bottom()) + ")");
  }
  if (g.layers.empty()) issues_.push_back(std::string(name) + ": нет слоёв");
  if (index_.count(name)) issues_.push_back(std::string(name) + ": повторное имя");
  g.id = idBase_ + glyphs_.size() + 1;
  index_[name] = glyphs_.size();
  names_.push_back(name);
  glyphs_.push_back(std::move(g));
}

const VecGlyph* GlyphBuilder::find(std::string_view name) const {
  auto it = index_.find(name);
  return it == index_.end() ? nullptr : &glyphs_[it->second];
}

// ================================================================ реестр значков
namespace {

using namespace shapes;
using L = GlyphBuilder::L;


const Affine kFlipX{-1, 0, 0, 1, 24, 0};

// Значок со значком-бейджем в правом нижнем углу: основа, вырез вокруг бейджа, бейдж.
void badge(GlyphBuilder& b, const char* name, const char* base, std::initializer_list<L> mark, float cx = 17.5f,
           float cy = 17.5f, float r = 5.25f) {
  std::vector<L> layers;
  layers.push_back(GlyphBuilder::U(base));
  layers.push_back(GlyphBuilder::X(circ(cx, cy, r + 1.4f)));
  for (const L& l : mark) layers.push_back(l);
  b.addList(name, layers);
}

const std::string kHorse = "M8.75 20.75c-.35-2.1.1-3.95 1.25-5.5-1.5.15-2.95-.05-4.35-.65-.9-.4-1.55-1.05-1.75-1.95-.15-.65 0-1.3.4-1.85L9 5.75c.7-.95 1.65-1.5 2.75-1.6L12.6 2.5l1.35 1.95c3.45.9 5.55 4.1 5.55 8.4v7.9Z";
// Меч по диагонали (остриё справа вверху) и его зона для вырезов.
const std::string kSword = "M20.5 3.5 19.6 7.1 11 15.7 8.3 13 16.9 4.4ZM6.5 11.5l6 6M9.5 14.5l-4.75 4.75";

void defineGeneral(GlyphBuilder& b) {
  using G = GlyphBuilder;
  const std::string shield = "M12 2.75 19.75 5.6v5.65c0 4.75-3.15 8.45-7.75 10.25-4.6-1.8-7.75-5.5-7.75-10.25V5.6Z";
  const std::string crownSmall = "M8.25 15.25V9.6l2.05 1.85L12 8.4l1.7 3.05 2.05-1.85v5.65Z";
  b.add("logo", {G::S(shield), G::F(crownSmall), G::S(crownSmall, 1.0f)});
  b.add("menu", {G::S("M4 6h16M4 12h16M4 18h16")});
  b.add("close", {G::S("M6 6l12 12M18 6 6 18")});
  b.add("check", {G::S("M4.75 12.5l4.75 4.75L19.25 7")});
  b.add("check-circle", {G::S(circ(12, 12, 9) + "M8.25 12.25l2.6 2.6 4.9-5.2")});
  b.add("plus", {G::S("M12 5v14M5 12h14")});
  b.add("minus", {G::S("M5 12h14")});
  b.add("trash", {G::S("M4 6.5h16M9.25 6.5V4.75c0-.69.56-1.25 1.25-1.25h3c.69 0 1.25.56 1.25 1.25V6.5"
                       "M6 6.5l.85 12.4c.09 1.18 1.07 2.1 2.25 2.1h5.8c1.18 0 2.16-.92 2.25-2.1L18 6.5M10 10.5v6.5M14 10.5v6.5")});
  b.add("edit", {G::S("M4 20l.95-4.25L15.6 5.1a2.05 2.05 0 0 1 2.9 0l.4.4a2.05 2.05 0 0 1 0 2.9L8.25 19.05ZM13.75 7l3.25 3.25")});
  b.add("copy", {G::S(rrect(8.5, 8.5, 12, 12, 2.25) +
                      "M15.5 8.5V5.75A2.25 2.25 0 0 0 13.25 3.5h-7.5A2.25 2.25 0 0 0 3.5 5.75v7.5a2.25 2.25 0 0 0 2.25 2.25H8.5")});
  b.add("duplicate", {G::U("copy"), G::S("M14.5 11.75v5.5M11.75 14.5h5.5")});
  b.add("search", {G::S(circ(10.5, 10.5, 6.75) + "M15.5 15.5l5 5")});
  b.add("filter", {G::S("M4 4.5h16l-6.25 7.75v6.25l-3.5 2V12.25Z")});
  b.add("sort", {G::S("M7.5 4v16M4 7.5 7.5 4 11 7.5M16.5 20V4M13 16.5l3.5 3.5 3.5-3.5")});
  b.add("sort-asc", {G::S("M4 6h6M4 12h8M4 18h10M18 19.5v-15M15 7.5l3-3 3 3")});
  b.add("sort-desc", {G::S("M4 6h10M4 12h8M4 18h6M18 4.5v15M15 16.5l3 3 3-3")});
  b.add("settings", {G::S(gear(12, 12, 9.25, 7, 8, 9, 14) + circ(12, 12, 3))});
  b.add("sliders", {G::S("M4 6h9M17 6h3M4 12h2M10 12h10M4 18h10M18 18h2" + circ(15, 6, 2) + circ(8, 12, 2) + circ(16, 18, 2))});
  b.add("info", {G::S(circ(12, 12, 9) + "M12 11v5.5"), G::F(circ(12, 7.6, 1.15))});
  b.add("warning", {G::S("M10.27 4.03 2.9 17.1a2 2 0 0 0 1.73 3h14.74a2 2 0 0 0 1.73-3L13.73 4.03a2 2 0 0 0-3.46 0ZM12 9.5v4.25"),
                    G::F(circ(12, 16.9, 1.15))});
  b.add("error", {G::S("M8.4 3h7.2L21 8.4v7.2L15.6 21H8.4L3 15.6V8.4ZM12 7.75v5"), G::F(circ(12, 16.25, 1.15))});
  b.add("help", {G::S(circ(12, 12, 9) + "M9.4 9.4a2.7 2.7 0 0 1 5.2 1c0 1.8-2.6 2.3-2.6 3.85"), G::F(circ(12, 17.1, 1.15))});
  b.add("undo", {G::S("M9 14 4 9l5-5M4 9h10.5a5.5 5.5 0 0 1 0 11H11")});
  b.add("redo", {G::S("M15 14l5-5-5-5M20 9H9.5a5.5 5.5 0 0 0 0 11H13")});
  const std::string floppy =
      "M5.5 3.5h10.1a2 2 0 0 1 1.42.59l2.89 2.89a2 2 0 0 1 .59 1.42V18.5a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2v-13a2 2 0 0 1 2-2Z"
      "M7.5 3.5v4.25a1 1 0 0 0 1 1h6a1 1 0 0 0 1-1V3.5M7 20.5v-5.75a1 1 0 0 1 1-1h8a1 1 0 0 1 1 1v5.75";
  b.add("save", {G::S(floppy)});
  const std::string pencilSmall = "M13 21v-2.5l6.1-6.1a1.77 1.77 0 0 1 2.5 2.5L15.5 21Z";
  b.add("save-as", {G::U("save"), G::X(pencilSmall), G::XS(pencilSmall, 4.5f), G::S(pencilSmall)});
  b.add("folder", {G::S("M3 6a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.6.8l1.2 1.7H19a2 2 0 0 1 2 2V18a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z")});
  b.add("folder-open", {G::S("M3 17.5V6a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.6.8l1.2 1.7H17.5a2 2 0 0 1 2 2V10"
                             "M3 17.5l2.3-6.2A2 2 0 0 1 7.2 10h13.4a1 1 0 0 1 .95 1.3l-2.1 6.9A2.5 2.5 0 0 1 17.05 20H5.5A2.5 2.5 0 0 1 3 17.5Z")});
  b.add("file", {G::S("M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8ZM14 3v4a1 1 0 0 0 1 1h4")});
  b.add("file-new", {G::U("file"), G::S("M12 11v6M9 14h6")});
  b.add("archive", {G::S(rrect(3, 4, 18, 4.5, 1.25) + "M4.75 8.5V18a2 2 0 0 0 2 2h10.5a2 2 0 0 0 2-2V8.5M10 12.25h4")});
  b.add("download", {G::S("M12 3.5v11.25M7.25 10l4.75 4.75L16.75 10M4 15.5V18a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-2.5")});
  b.add("upload", {G::S("M12 14.75V3.5M7.25 8.25 12 3.5l4.75 4.75M4 15.5V18a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-2.5")});
  b.add("export", {G::S("M10.5 4H6a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h4.5M15.5 7.5 20 12l-4.5 4.5M20 12H9")});
  b.add("import", {G::S("M13.5 4H18a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2h-4.5M8.5 7.5 13 12l-4.5 4.5M13 12H3.5")});
  const std::string eye = "M2.5 12C4.6 7.8 8 5.5 12 5.5s7.4 2.3 9.5 6.5c-2.1 4.2-5.5 6.5-9.5 6.5S4.6 16.2 2.5 12Z" + circ(12, 12, 3);
  b.add("eye", {G::S(eye)});
  b.add("eye-off", {G::S(eye), G::XS("M4 4l16 16", 4.75f), G::S("M4 4l16 16")});
  b.add("lock", {G::S(rrect(4.5, 10.5, 15, 10, 2.5) + "M8 10.5V7.5a4 4 0 0 1 8 0v3M12 14.5v2")});
  b.add("unlock", {G::S(rrect(4.5, 10.5, 15, 10, 2.5) + "M8 10.5V7.5a4 4 0 0 1 7.75-1.4M12 14.5v2")});
  b.add("pin", {G::S("M9 3.5h6M10 3.5v5.1L7.1 11.5a3 3 0 0 0-.85 2.1v.9h11.5v-.9a3 3 0 0 0-.85-2.1L14 8.6V3.5M12 14.5v6.5")});
  b.add("link", {G::S("M10 13.5a4.25 4.25 0 0 0 6.2.45l2.6-2.6a4.25 4.25 0 0 0-6-6l-1.5 1.5"
                      "M14 10.5a4.25 4.25 0 0 0-6.2-.45l-2.6 2.6a4.25 4.25 0 0 0 6 6l1.5-1.5")});
  b.add("unlink", {G::S("M17.6 12.6l1.2-1.25a4.25 4.25 0 0 0-6-6L11.55 6.6M6.4 11.4 5.2 12.65a4.25 4.25 0 0 0 6 6l1.25-1.25"
                        "M8 2.5v2.75M2.5 8h2.75M16 21.5v-2.75M21.5 16h-2.75")});
  b.add("chevron-left", {G::S("M14.5 6l-6 6 6 6")});
  b.add("chevron-right", {G::S("M9.5 6l6 6-6 6")});
  b.add("chevron-up", {G::S("M6 14.5l6-6 6 6")});
  b.add("chevron-down", {G::S("M6 9.5l6 6 6-6")});
  b.add("arrow-left", {G::S("M19 12H5M11 6l-6 6 6 6")});
  b.add("arrow-right", {G::S("M5 12h14M13 6l6 6-6 6")});
  b.add("arrow-up", {G::S("M12 19V5M6 11l6-6 6 6")});
  b.add("arrow-down", {G::S("M12 5v14M6 13l6 6 6-6")});
  b.add("external", {G::S("M14 4h6v6M20 4l-8.5 8.5M18 13.5V18a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4.5")});
  b.add("more-h", {G::F(circ(5.25, 12, 1.85) + circ(12, 12, 1.85) + circ(18.75, 12, 1.85))});
  b.add("more-v", {G::F(circ(12, 5.25, 1.85) + circ(12, 12, 1.85) + circ(12, 18.75, 1.85))});
  b.add("grip", {G::F(circ(9, 6, 1.5) + circ(15, 6, 1.5) + circ(9, 12, 1.5) + circ(15, 12, 1.5) + circ(9, 18, 1.5) + circ(15, 18, 1.5))});
  b.add("expand", {G::S("M7 15l5 5 5-5M7 9l5-5 5 5")});
  b.add("collapse", {G::S("M7 19.5l5-5 5 5M7 4.5l5 5 5-5")});
  b.add("maximize", {G::S("M14.5 3.5h6v6M20.5 3.5 14 10M9.5 20.5h-6v-6M3.5 20.5 10 14")});
  b.add("minimize", {G::S("M14 4v6h6M14 10l6.5-6.5M10 20v-6H4M10 14l-6.5 6.5")});
  b.add("fullscreen", {G::S("M4 9V6.5A2.5 2.5 0 0 1 6.5 4H9M15 4h2.5A2.5 2.5 0 0 1 20 6.5V9M20 15v2.5a2.5 2.5 0 0 1-2.5 2.5H15"
                            "M9 20H6.5A2.5 2.5 0 0 1 4 17.5V15")});
  b.add("fullscreen-exit", {G::S("M9 4v2.5A2.5 2.5 0 0 1 6.5 9H4M20 9h-2.5A2.5 2.5 0 0 1 15 6.5V4M15 20v-2.5a2.5 2.5 0 0 1 2.5-2.5H20"
                                 "M4 15h2.5A2.5 2.5 0 0 1 9 17.5V20")});
  b.add("sun", {G::S(circ(12, 12, 4) + "M12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2M5.28 5.28l1.42 1.42M17.3 17.3l1.42 1.42"
                                       "M5.28 18.72l1.42-1.42M17.3 6.7l1.42-1.42")});
  b.add("moon", {G::S("M20.25 14.5A8.5 8.5 0 1 1 9.5 3.75a6.75 6.75 0 0 0 10.75 10.75Z")});
  b.add("keyboard", {G::S(rrect(2.5, 6, 19, 12, 2.5) + "M8 14.5h8"),
                     G::F(circ(6.5, 10, 1.05) + circ(9.5, 10, 1.05) + circ(12.5, 10, 1.05) + circ(15.5, 10, 1.05) +
                          circ(17.75, 14.5, 1.05) + circ(6.25, 14.5, 1.05))});
  b.add("palette", {G::S("M12 3a9 9 0 0 0 0 18c1.1 0 1.75-.75 1.75-1.65 0-.43-.17-.82-.43-1.12-.26-.3-.42-.68-.42-1.1 "
                         "0-.9.73-1.63 1.65-1.63h1.95A4.5 4.5 0 0 0 21 11c0-4.42-4.03-8-9-8Z"),
                    G::F(circ(7.5, 11.5, 1.25) + circ(9.5, 7.25, 1.25) + circ(14.25, 7.25, 1.25) + circ(17, 11, 1.25))});
  b.add("image", {G::S(rrect(3, 3.5, 18, 17, 2.5) + circ(8.75, 9.25, 1.75) + "M20.5 15.5l-4.2-4.2a1 1 0 0 0-1.4 0L6 20.25")});
  b.add("zoom-in", {G::S(circ(10.5, 10.5, 6.75) + "M15.5 15.5l5 5M10.5 7.75v5.5M7.75 10.5h5.5")});
  b.add("zoom-out", {G::S(circ(10.5, 10.5, 6.75) + "M15.5 15.5l5 5M7.75 10.5h5.5")});
  b.add("zoom-fit", {G::S("M3.5 8V5.5a2 2 0 0 1 2-2H8M16 3.5h2.5a2 2 0 0 1 2 2V8M20.5 16v2.5a2 2 0 0 1-2 2H16M8 20.5H5.5a2 2 0 0 1-2-2V16" +
                          rrect(8.5, 8.5, 7, 7, 1.5))});
  b.add("target", {G::S(circ(12, 12, 9) + circ(12, 12, 5)), G::F(circ(12, 12, 1.6))});
  b.add("crosshair", {G::S(circ(12, 12, 7.5) + "M12 2v4.5M12 17.5V22M2 12h4.5M17.5 12H22")});
  b.add("measure", {G::S("M3.6 15.2 15.2 3.6a1.5 1.5 0 0 1 2.1 0l3.1 3.1a1.5 1.5 0 0 1 0 2.1L8.8 20.4a1.5 1.5 0 0 1-2.1 0l-3.1-3.1a1.5 1.5 0 0 1 0-2.1Z"
                         "M7.1 11.7l2 2M9.9 8.9l1.4 1.4M12.7 6.1l2 2")});
  b.add("layers", {G::S("M12 3.5 20.5 8 12 12.5 3.5 8ZM3.5 12 12 16.5 20.5 12M3.5 16 12 20.5 20.5 16")});
  b.add("grid", {G::S(rrect(3.5, 3.5, 17, 17, 2.5) + "M3.5 9.25h17M3.5 14.75h17M9.25 3.5v17M14.75 3.5v17")});
  b.add("magnet", {G::S("M5 4.5h4V12a3 3 0 0 0 6 0V4.5h4V12a7 7 0 0 1-14 0ZM5 8.5h4M15 8.5h4")});
  b.add("history", {G::S("M3.5 12a8.5 8.5 0 1 0 2.6-6.1L3.5 8.5M3.5 4v4.5H8M12 7.5V12l3 2")});
  b.add("clock", {G::S(circ(12, 12, 9) + "M12 7v5l3.25 2")});
  b.add("calendar", {G::S(rrect(3.5, 5, 17, 15.5, 2.5) + "M3.5 10h17M8 3v4M16 3v4")});
  b.add("hourglass", {G::S("M6.5 3.5h11M6.5 20.5h11M8 3.5v2.4a4 4 0 0 0 1.6 3.2L12 11l2.4-1.9a4 4 0 0 0 1.6-3.2V3.5"
                           "M8 20.5v-2.4a4 4 0 0 1 1.6-3.2L12 13l2.4 1.9a4 4 0 0 1 1.6 3.2v2.4")});
  b.add("play", {G::S("M7 5.1v13.8a1.1 1.1 0 0 0 1.68.93l10.9-6.9a1.1 1.1 0 0 0 0-1.86L8.68 4.17A1.1 1.1 0 0 0 7 5.1Z")});
  b.add("next-turn", {G::S("M5 5.6v12.8a1.1 1.1 0 0 0 1.7.92l9.3-6.4a1.1 1.1 0 0 0 0-1.84L6.7 4.68A1.1 1.1 0 0 0 5 5.6ZM19.5 5v14")});
  b.add("refresh", {G::S("M4.5 10a7.75 7.75 0 0 1 13.4-3.3L20 9M20 4.5V9h-4.5M19.5 14a7.75 7.75 0 0 1-13.4 3.3L4 15M4 19.5V15h4.5")});
  const std::string star5 = star(12, 12.9, 9.6, 4.3, 5, -90);
  b.add("star", {G::S(star5)});
  b.add("star-filled", {G::F(star5), G::S(star5)});
  b.add("heart", {G::S("M12 20.25C10 19 3.5 14.9 3.5 9.25A4.75 4.75 0 0 1 12 6.4a4.75 4.75 0 0 1 8.5 2.85c0 5.65-6.5 9.75-8.5 11Z")});
  b.add("user", {G::S(circ(12, 8, 4) + "M4.5 20.5a7.5 7.5 0 0 1 15 0")});
  b.add("users", {G::S(circ(9, 8, 3.5) + "M2.5 20a6.5 6.5 0 0 1 13 0M15.25 4.6a3.5 3.5 0 0 1 0 6.8M17.75 13.9A6.5 6.5 0 0 1 21.5 20")});
  b.add("user-plus", {G::S(circ(10, 8, 3.75) + "M3 20.5a7 7 0 0 1 12.5-4.35M19 14v6M16 17h6")});
  b.add("bolt", {G::S("M13.5 2.5 4.5 13.5H12l-1.5 8 9-11H12Z")});
  b.add("repeat", {G::S("M17 2.5l3.5 3.5L17 9.5M3.5 11.5V10a4 4 0 0 1 4-4h13M7 21.5 3.5 18 7 14.5M20.5 12.5V14a4 4 0 0 1-4 4h-13")});
  b.add("note", {G::S("M14.5 20.5H6A2.5 2.5 0 0 1 3.5 18V6A2.5 2.5 0 0 1 6 3.5h12A2.5 2.5 0 0 1 20.5 6v8.5Z"
                      "M14.5 20.5v-4a2 2 0 0 1 2-2h4M7.5 8.5h9M7.5 12h5")});
  b.add("list", {G::S("M9 6h11M9 12h11M9 18h11"), G::F(circ(4.75, 6, 1.25) + circ(4.75, 12, 1.25) + circ(4.75, 18, 1.25))});
  b.add("table", {G::S(rrect(3.5, 4, 17, 16, 2.5) + "M3.5 9.5h17M3.5 14.75h17M10 9.5V20")});
  b.add("tag", {G::S("M3.5 5v6.17a2 2 0 0 0 .59 1.42l8.32 8.32a2 2 0 0 0 2.83 0l5.67-5.67a2 2 0 0 0 0-2.83L12.59 4.09A2 2 0 0 0 11.17 3.5H5A1.5 1.5 0 0 0 3.5 5Z"),
                 G::F(circ(8, 8, 1.5))});
  b.add("globe", {G::S(circ(12, 12, 9) + "M12 3c-2.5 2.5-3.75 5.5-3.75 9s1.25 6.5 3.75 9c2.5-2.5 3.75-5.5 3.75-9S14.5 5.5 12 3ZM3 12h18")});
  b.add("compass", {G::S(circ(12, 12, 9) + "M15.75 8.25l-1.9 5.6-5.6 1.9 1.9-5.6Z"), G::F("M15.75 8.25 10.15 10.15 13.85 13.85Z")});
  b.add("map", {G::S("M3.5 6.25 9 3.75l6 2.5 5.5-2.5v14l-5.5 2.5-6-2.5-5.5 2.5ZM9 3.75v14M15 6.25v14")});
  b.add("map-pin", {G::S("M12 21.25c-1.5-1.3-7-6.2-7-11.5a7 7 0 0 1 14 0c0 5.3-5.5 10.2-7 11.5Z" + circ(12, 9.75, 2.5))});
  b.add("home", {G::S("M3.5 10.5 12 3.5l8.5 7M5.5 9v9.5a2 2 0 0 0 2 2h9a2 2 0 0 0 2-2V9M10 20.5V15h4v5.5")});
  b.add("command", {G::S("M9 9V6.5A2.5 2.5 0 1 0 6.5 9H17.5A2.5 2.5 0 1 0 15 6.5V17.5A2.5 2.5 0 1 0 17.5 15H6.5A2.5 2.5 0 1 0 9 17.5Z")});
  b.add("dice", {G::S(rrect(3.5, 3.5, 17, 17, 3.5)),
                 G::F(circ(8.25, 8.25, 1.4) + circ(15.75, 8.25, 1.4) + circ(12, 12, 1.4) + circ(8.25, 15.75, 1.4) + circ(15.75, 15.75, 1.4))});
  b.add("bell", {G::S("M6 9.5a6 6 0 0 1 12 0c0 4.6 1.2 6.4 2.25 7.5H3.75C4.8 15.9 6 14.1 6 9.5ZM10 20.25a2.25 2.25 0 0 0 4 0")});
}

void defineDomain(GlyphBuilder& b) {
  using G = GlyphBuilder;
  b.add("crown", {G::S("M4 8.5l3.75 3.25L12 5.5l4.25 6.25L20 8.5l-1.5 9h-13ZM5.5 20.5h13")});
  b.add("flag", {G::S("M5 21.5V3.5M5 4.5c2.5-1.6 4.75-1.6 7 0s4.5 1.6 7 0v9c-2.5 1.6-4.75 1.6-7 0s-4.5-1.6-7 0")});
  b.add("banner", {G::S("M4.5 3.5h15M6.5 3.5V20l5.5-3.75L17.5 20V3.5")});
  b.add("castle", {G::S("M3.5 20.5V5h2.5v2h1.5V5h2.5v5.5h4V5h2.5v2h1.5V5h2.5v15.5ZM10 20.5V17a2 2 0 0 1 4 0v3.5")});
  b.add("tower", {G::S("M6 3.5h2.5v2h2v-2h3v2h2v-2H18V9H6ZM7 9 6 20.5h12L17 9M10.5 20.5v-3a1.5 1.5 0 0 1 3 0v3")});
  const std::string shield = "M12 2.75l7.75 2.9v5.6c0 4.8-3.2 8.6-7.75 10.5-4.55-1.9-7.75-5.7-7.75-10.5v-5.6Z";
  b.add("shield", {G::S(shield)});
  b.add("sword", {G::S(kSword), G::F(circ(4.1, 19.9, 1.3))});
  const std::string blade = "M3.75 3.75h2.75l9 9-2.75 2.75-9-9ZM11.75 18.25l6.5-6.5M16 16l3.25 3.25";
  b.add("swords", {G::S(blade), G::F(circ(20.1, 20.1, 1.3)), G::T(G::X(blade), kFlipX), G::T(G::XS(blade, 4.5f), kFlipX),
                   G::T(G::XS(circ(20.1, 20.1, 1.3), 3.0f), kFlipX), G::T(G::S(blade), kFlipX),
                   G::T(G::F(circ(20.1, 20.1, 1.3)), kFlipX)});
  const std::string axe = "M5 20 16 9M13.25 4.25c2.6-.9 5.4-.1 7.25 2.5-1.3.1-2.4.6-3.25 1.5-.9.85-1.4 1.95-1.5 3.25-2.6-1.85-3.4-4.65-2.5-7.25Z";
  b.add("war", {G::S(axe), G::T(G::XS(axe, 4.5f), kFlipX), G::T(G::S(axe), kFlipX)});
  b.add("battle", {G::S(blade), G::F(circ(20.1, 20.1, 1.3)), G::T(G::X(blade), kFlipX), G::T(G::XS(blade, 4.5f), kFlipX),
                   G::T(G::XS(circ(20.1, 20.1, 1.3), 3.0f), kFlipX), G::T(G::S(blade), kFlipX),
                   G::T(G::F(circ(20.1, 20.1, 1.3)), kFlipX), G::S("M12 2v2.25M8.75 2.6l.9 1.6M15.25 2.6l-.9 1.6", 1.5f)});
  b.add("skull", {G::S("M12 3a7.5 7.5 0 0 0-7.5 7.5c0 2.5 1.2 4.6 3 5.9V19a1.5 1.5 0 0 0 1.5 1.5h6a1.5 1.5 0 0 0 1.5-1.5v-2.6"
                       "c1.8-1.3 3-3.4 3-5.9A7.5 7.5 0 0 0 12 3ZM10.5 20.5V18M13.5 20.5V18"),
                  G::F(circ(9, 11.25, 1.75) + circ(15, 11.25, 1.75))});
  const std::string helm = "M10.25 20.75 6.7 19.6A2.4 2.4 0 0 1 5 17.3V12.5a7 7 0 0 1 14 0v4.8a2.4 2.4 0 0 1-1.7 2.3l-3.55 1.15"
                           "V15.25H16.5v-3.5h-9v3.5h2.75Z";
  b.add("army", {G::S(helm), G::F("M8.6 2.6h6.8l-1.6 3.4h-3.6Z")});
  b.add("fleet", {G::S("M3 15.5h18l-2.4 3.9a2 2 0 0 1-1.7.95H7.1a2 2 0 0 1-1.7-.95ZM12 15.5V3M12 4.5 18.5 13H12M12 6.5 6.5 13H12")});
  b.add("anchor", {G::S(circ(12, 5.25, 2) + "M12 7.25V21M8 10.5h8M4.5 13.5a7.5 7.5 0 0 0 15 0M3 15l1.5-1.5L6 15M18 15l1.5-1.5L21 15")});
  b.add("horse", {G::S(kHorse), G::F(circ(11.4, 8.4, 1))});
  b.add("bow", {G::S("M7.5 3c3.9 2.1 6.25 5.3 6.25 9s-2.35 6.9-6.25 9M7.5 3v18M3 12h17.5M17.75 9.25 20.5 12l-2.75 2.75"
                     "M3 12l-.5-2M3 12l-.5 2M5.25 12l-.5-2M5.25 12l-.5 2")});
  b.add("staff", {G::S("M4.5 20.5 13 12" + circ(16, 8, 3.5) + "M14.5 2.5v1M20.5 9v1")});
  b.add("wand", {G::S("M4 20 14.5 9.5M17.5 3v3M16 4.5h3M20 9.5v2M19 10.5h2M11.5 3.5v2M10.5 4.5h2")});
  b.add("sparkles", {G::S("M10 3.5c.55 3.7 2.3 5.45 6 6-3.7.55-5.45 2.3-6 6-.55-3.7-2.3-5.45-6-6 3.7-.55 5.45-2.3 6-6Z"
                          "M18.5 14.5v5M16 17h5M18 3v3M16.5 4.5h3")});
  b.add("book", {G::S("M12 6.75C10.25 5.2 7.6 4.5 3.5 4.5v13.75c4.1 0 6.75.7 8.5 2.25 1.75-1.55 4.4-2.25 8.5-2.25V4.5"
                      "c-4.1 0-6.75.7-8.5 2.25ZM12 6.75V20.5")});
  b.add("scroll", {G::S(rrect(3.5, 3.5, 17, 4, 2) + rrect(3.5, 16.5, 17, 4, 2) + "M6 7.5v9M18 7.5v9M9.25 10.5h5.5M9.25 13.5h5.5")});
  b.add("quill", {G::S("M20.5 3.5C14 3.5 8.75 7.5 7.25 14L5.5 20.5M20.5 3.5c0 6.5-4.5 11-11.5 11.5M13.5 9.5 8 15M3.5 20.5h8")});
  b.add("coins", {G::S(ell(8.75, 6.75, 5.75, 2.25) + "M3 6.75v3.5c0 1.24 2.57 2.25 5.75 2.25s5.75-1.01 5.75-2.25v-3.5"
                       "M3 10.25v3.5c0 1.24 2.57 2.25 5.75 2.25M3 13.75v3.5c0 1.24 2.57 2.25 5.75 2.25"),
                  G::X(circ(15.75, 15.25, 6.4)), G::S(circ(15.75, 15.25, 5) + circ(15.75, 15.25, 2.1))});
  b.add("treasury", {G::S("M3.5 11h17v7.5a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2ZM3.5 11V8.5a4 4 0 0 1 4-4h9a4 4 0 0 1 4 4V11"
                          "M10.25 11v2.5a1 1 0 0 0 1 1h1.5a1 1 0 0 0 1-1V11")});
  b.add("scales", {G::S("M12 4v16.5M8 20.5h8M5 6.5h14M5 6.5 2.5 13M5 6.5 7.5 13M19 6.5 16.5 13M19 6.5 21.5 13"
                        "M2.5 13a2.5 2.5 0 0 0 5 0ZM16.5 13a2.5 2.5 0 0 0 5 0Z")});
  b.add("trade", {G::S("M4 8h15M15.5 4.5 19 8l-3.5 3.5M20 16H5M8.5 12.5 5 16l3.5 3.5")});
  b.add("handshake", {G::S("M2.5 8.5h2.75l2.6-1.6a3 3 0 0 1 2.2-.35L12 7l2.6-.6a3 3 0 0 1 2 .25L19 7.75h2.5"
                           "M2.5 15.5h2.25l4.5 3.6a1.5 1.5 0 0 0 2.05-.15l.45-.5.5.35a1.5 1.5 0 0 0 2-.2l.35-.4.5.25a1.5 1.5 0 0 0 1.85-.4"
                           "l1.5-1.85L21.5 14.5M12 7l-3.25 3.1a1.5 1.5 0 0 0 .1 2.15c.65.55 1.6.5 2.2-.1L13.5 10l4.25 4.35M11 16l1.75 1.75M13 14.5l2 2")});
  const std::string ringA = circ(9, 12, 5.5), ringB = circ(15, 12, 5.5);
  const std::string arcB = arc(15, 12, 5.5, -150, -100), arcA = arc(9, 12, 5.5, 30, 80);
  b.add("alliance", {G::S(ringA), G::S(ringB), G::XS(arcB, 4.5f), G::S(arcB), G::XS(arcA, 4.5f), G::S(arcA)});
  b.add("status-quo", {G::S(circ(12, 12, 9) + "M8 10h8M8 14h8")});
  b.add("unknown", {G::S(dashedCircle(12, 12, 9, 10, 0.55) + "M9.4 9.4a2.7 2.7 0 0 1 5.2 1c0 1.8-2.6 2.3-2.6 3.85"),
                    G::F(circ(12, 17.1, 1.15))});
  b.add("diplomacy", {G::S("M3.5 7.5a2 2 0 0 1 2-2h13a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2ZM3.75 6.5 12 13l8.25-6.5")});
  b.add("tribute", {G::S(circ(14.5, 6.5, 3.25) + "M3 14.5h2.5M5.5 13v7.5M5.5 14h3.4c.6 0 1.2.2 1.7.6l1.2.9h2.7a1.5 1.5 0 0 1 0 3h-3.5"
                                                "M14.5 18l4-2.4a1.6 1.6 0 0 1 2 2.4l-4 3.2a3 3 0 0 1-1.9.8H5.5")});
  b.add("reparations", {G::S(ell(8.25, 14.25, 5.25, 2.1) + "M3 14.25v3.25c0 1.16 2.35 2.1 5.25 2.1s5.25-.94 5.25-2.1v-3.25"
                                        "M20.5 14V8.5A3.5 3.5 0 0 0 17 5H9.5M12.5 2 9.5 5l3 3")});
  b.add("guild", {G::S("M4.5 21V3M4.5 5h15.5M8 5v3.5M17 5v3.5" + rrect(6, 8.5, 13, 8.5, 1.75)),
                  G::F("M12.5 10.25l2.5 2.5-2.5 2.5-2.5-2.5Z")});
  b.add("hq", {G::S("M3.5 20.5h17M5.5 20.5V11l6.5-4.5 6.5 4.5v9.5M12 6.5V2.75l3.5 1.25L12 5.25M10 20.5v-4a2 2 0 0 1 4 0v4")});
  b.add("route", {G::S(circ(6, 18, 2.25) + circ(18, 6, 2.25) + "M8.25 18H15a3 3 0 0 0 0-6H9a3 3 0 0 1 0-6h6.75")});
  const std::string prov = "M4.5 7 9.5 3.75l5 2 5.25-1.25-.75 6 1.5 5.25-5.5 4.25-5-1.75-5.5 1.5.5-6.25Z";
  b.add("province", {G::S(prov)});
  b.add("island", {G::S("M3 19.25c1.5 0 1.5.75 3 .75s1.5-.75 3-.75 1.5.75 3 .75 1.5-.75 3-.75 1.5.75 3 .75 1.5-.75 3-.75"
                        "M5.5 16.25c1.6-2.2 4-3.5 6.5-3.5s4.9 1.3 6.5 3.5M12 12.75V7.5M12 7.5c-1-1.6-2.6-2.4-4.5-2.2"
                        "M12 7.5c1-1.6 2.6-2.4 4.5-2.2M12 7.5c-1.75-.25-3.25.4-4.25 1.9M12 7.5c1.75-.25 3.25.4 4.25 1.9")});
  b.add("sea", {G::S("M3 7.5q2.25-2 4.5 0t4.5 0 4.5 0 4.5 0M3 12q2.25-2 4.5 0t4.5 0 4.5 0 4.5 0M3 16.5q2.25-2 4.5 0t4.5 0 4.5 0 4.5 0")});
  b.add("land", {G::S("M2.5 19.5h19M4.5 19.5c1.2-4.4 4-7 7.25-7 1.9 0 3.6.8 4.9 2.3M13.6 13.4c1.2-.9 2.5-1.4 3.9-1.4 1.9 0 3.3 1 4 2.5" +
                      circ(17.5, 6, 2))});
  b.add("mountain", {G::S("M2.5 19.5 9.5 7.5l4 6.75 2.5-3.5 5.5 8.75ZM7.25 11.4l2.25 1.35 2.1-1.35")});
  b.add("tech-tree", {G::S(rrect(9, 3, 6, 5, 1.25) + rrect(3, 16, 6, 5, 1.25) + rrect(15, 16, 6, 5, 1.25) +
                           "M12 8v4M6 16v-2a2 2 0 0 1 2-2h8a2 2 0 0 1 2 2v2")});
  b.add("tech", {G::S("M9.5 17.5h5M10.25 20.5h3.5M12 3a6 6 0 0 0-3.75 10.7c.8.65 1.25 1.3 1.25 2.3v1.5h5V16c0-1 .45-1.65 1.25-2.3A6 6 0 0 0 12 3Z")});
  b.add("research", {G::S("M9 3.5h6M10 3.5v5.75L4.9 18a1.75 1.75 0 0 0 1.5 2.5h11.2a1.75 1.75 0 0 0 1.5-2.5L14 9.25V3.5M7.4 14.5h9.2")});
  b.add("building", {G::S("M4.5 20.5V5A1.5 1.5 0 0 1 6 3.5h7.5A1.5 1.5 0 0 1 15 5v15.5M15 9.5h3a1.5 1.5 0 0 1 1.5 1.5v9.5M3 20.5h18"
                          "M8 7.5h3.5M8 11h3.5M8 14.5h3.5")});
  b.add("build", {G::S("M3.5 20.5h17v-5h-17ZM6.5 15.5v-5h11v5M12 15.5v5M9.5 10.5V5.5h5v5")});
  b.add("hammer", {G::S("M3.75 20.25l9.3-9.3" + poly({9.7, 6.88, 12.88, 3.7, 20.3, 11.12, 17.12, 14.3}))});
  b.add("pickaxe", {G::S("M4 20 14 10M5.5 7.5C9 4.5 14 3.5 18.5 5.5 20.5 10 19.5 15 16.5 18.5M12 6l6 6")});
  b.add("factory", {G::S("M3.5 20.5V11l5 3v-3l5 3v-3l5 3V3.5h2v17ZM7 17.5h2M11.5 17.5h2")});
  b.add("house", {G::S("M3.5 11 12 4l8.5 7M5.5 9.5v11h13v-11M10 20.5v-5.5h4v5.5M16 7.6V4.5h2v4.75")});
  b.add("slots", {G::S(rrect(3.5, 3.5, 7.5, 7.5, 1.75) + rrect(13, 3.5, 7.5, 7.5, 1.75) + rrect(3.5, 13, 7.5, 7.5, 1.75) +
                       rrect(13, 13, 7.5, 7.5, 1.75)),
                  G::F(rrect(3.5, 3.5, 7.5, 7.5, 1.75))});
  badge(b, "build-cost", "house", {G::S(ell(17.5, 15.25, 4.25, 1.75) + "M13.25 15.25v3.25c0 .97 1.9 1.75 4.25 1.75s4.25-.78 4.25-1.75v-3.25")});
  b.add("population", {G::S(circ(12, 7.5, 3) + "M7 20.5a5 5 0 0 1 10 0" + circ(5.5, 10, 2.25) + "M2 19a3.75 3.75 0 0 1 5-3.5" +
                            circ(18.5, 10, 2.25) + "M22 19a3.75 3.75 0 0 0-5-3.5")});
  b.add("contentment", {G::S(circ(12, 12, 9) + "M8.5 14a4.25 4.25 0 0 0 7 0"), G::F(circ(9, 9.75, 1.2) + circ(15, 9.75, 1.2))});
  b.add("discontent", {G::S(circ(12, 12, 9) + "M8.5 16.5a4.25 4.25 0 0 1 7 0"), G::F(circ(9, 9.75, 1.2) + circ(15, 9.75, 1.2))});
  b.add("rebellion", {G::S("M12 21.5V10.5M7 3v4.5a5 5 0 0 0 10 0V3M12 3v7.5")});
  b.add("religion", {G::S("M12 2.5v4M10.25 4h3.5M6 20.5v-8l6-5 6 5v8M4 20.5h16M10 20.5v-3a2 2 0 0 1 4 0v3"), G::F(circ(12, 12.75, 1.25))});
  b.add("culture", {G::S("M5 4.5c4.6 1.3 9.4 1.3 14 0v6.75a7 7 0 0 1-14 0ZM8.75 15a4.25 4.25 0 0 0 6.5 0"),
                    G::F("M7.6 10.25c.6-1.2 2.6-1.2 3.2 0-.9.55-2.3.55-3.2 0ZM13.2 10.25c.6-1.2 2.6-1.2 3.2 0-.9.55-2.3.55-3.2 0Z")});
  b.add("race", {G::S(ell(12, 12.25, 5, 6.75) + "M7.15 10.25 2.75 5.5l4.5 8.25M16.85 10.25l4.4-4.75-4.5 8.25"),
                 G::F(circ(10, 12, 1) + circ(14, 12, 1))});
  b.add("trade-value", {G::S(circ(12, 12, 4.25) + "M19.8 9.5A8.25 8.25 0 0 0 5.5 6.3M4.2 14.5a8.25 8.25 0 0 0 14.3 3.2M5.5 3.5v2.8h2.8"
                                                  "M18.5 20.5v-2.8h-2.8")});
  b.add("resource", {G::S("M12 3l8 4.5v9L12 21l-8-4.5v-9ZM4.25 7.6 12 12l7.75-4.4M12 12v9")});
  b.add("grain", {G::S("M12 21.5V8M12 17c-2.5 0-4-1.5-4-4 2.5 0 4 1.5 4 4Zm0 0c2.5 0 4-1.5 4-4-2.5 0-4 1.5-4 4ZM12 12.5c-2.5 0-4-1.5-4-4 "
                       "2.5 0 4 1.5 4 4Zm0 0c2.5 0 4-1.5 4-4-2.5 0-4 1.5-4 4ZM12 8c-1.2-1.2-1.2-3.3 0-4.5 1.2 1.2 1.2 3.3 0 4.5Z")});
  b.add("wood", {G::S(circ(7.5, 16, 3.5) + circ(16.5, 16, 3.5) + circ(12, 8.5, 3.5)), G::F(circ(7.5, 16, 1) + circ(16.5, 16, 1) + circ(12, 8.5, 1))});
  b.add("stone", {G::S("M4 18.5 6.5 11l5-4.5 5 2 4 5-1.5 5ZM6.5 11l4.5 2.5 1 5M11 13.5l5.5-5")});
  b.add("iron", {G::S("M2.75 18.5 5.5 11h13l2.75 7.5ZM5.5 11l1.5-3.5h10L18.5 11")});
  b.add("gem", {G::S("M6.5 4h11l3.5 5-9 11L3 9ZM3 9h18M9.5 4 8 9l4 11 4-11-1.5-5")});
  b.add("income", {G::S(ell(8.75, 10.5, 5.75, 2.25) + "M3 10.5v3.5c0 1.24 2.57 2.25 5.75 2.25s5.75-1.01 5.75-2.25v-3.5M3 14v3.5c0 1.24 2.57 2.25 5.75 2.25s5.75-1.01 5.75-2.25V14" + "M19 20.5V4.5M16 7.5l3-3 3 3")});
  b.add("expense", {G::S(ell(8.75, 10.5, 5.75, 2.25) + "M3 10.5v3.5c0 1.24 2.57 2.25 5.75 2.25s5.75-1.01 5.75-2.25v-3.5M3 14v3.5c0 1.24 2.57 2.25 5.75 2.25s5.75-1.01 5.75-2.25V14" + "M19 4.5v16M16 17.5l3 3 3-3")});
  b.add("percent", {G::S("M19 5 5 19" + circ(7, 7, 2.5) + circ(17, 17, 2.5))});
  b.add("trend-up", {G::S("M3 17l6-6 4 4 8-8M15 7h6v6")});
  b.add("trend-down", {G::S("M3 7l6 6 4-4 8 8M15 17h6v-6")});
  badge(b, "army-upkeep", "army", {G::S(ell(17.5, 15.25, 4.25, 1.75) + "M13.25 15.25v3.25c0 .97 1.9 1.75 4.25 1.75s4.25-.78 4.25-1.75v-3.25")});
  badge(b, "fleet-upkeep", "fleet", {G::S(ell(17.5, 15.25, 4.25, 1.75) + "M13.25 15.25v3.25c0 .97 1.9 1.75 4.25 1.75s4.25-.78 4.25-1.75v-3.25")});
  b.add("mode-political", {G::S(rrect(3.5, 3.5, 17, 17, 3) + "M3.5 11.5c2.5.5 4.5-.5 6-2.5s1-4 .5-5.5M9.5 9c1.5 2 1.5 4 .5 6-1 2-.5 4 .5 5.5"
                                                              "M10 15c2.5-.5 5 0 7 1.5s3.5.5 3.5.5")});
  b.add("mode-guilds", {G::S(circ(12, 5.5, 2.5) + circ(5, 18, 2.5) + circ(19, 18, 2.5) + "M10.75 7.65 6.25 15.85M13.25 7.65l4.5 8.2M7.5 18h9"),
                        G::F(circ(12, 13.75, 1.75))});
  b.add("mode-terrain", {G::S("M2.5 18.5 8.5 9l4 6 2.5-3.5 6.5 7Z" + circ(17, 5.5, 2))});
  b.add("chart-pie", {G::S("M20.6 13.75A8.75 8.75 0 1 1 10.25 3.4M13.5 2.6a8.25 8.25 0 0 1 7.9 7.9H13.5Z")});
  b.add("chart-bar", {G::S("M3.5 20.5h17M7 17v-5M12 17V6M17 17V9.5")});
  b.add("occupied", {G::S(rrect(3.5, 3.5, 17, 17, 3) + "M3.5 12.5l9-9M3.5 18.5l15-15M8.5 20.5l12-12M14.5 20.5l6-6")});
  b.add("split", {G::S("M12 21v-8M12 13 5.5 6.5M12 13l6.5-6.5M5.5 11V6.5H10M14 6.5h4.5V11")});
  b.add("merge", {G::S("M5.5 3.5 12 10M18.5 3.5 12 10M12 10v10.5M8.5 17l3.5 3.5 3.5-3.5")});
  b.add("disband", {G::U("flag"), G::XS("M3.5 3.5l17 17", 4.75f), G::S("M3.5 3.5l17 17")});
  b.add("retreat", {G::S("M11 17l-5-5 5-5M18 17l-5-5 5-5")});
  b.add("dissolve", {G::S("M10.5 3.5H6A2.5 2.5 0 0 0 3.5 6v12A2.5 2.5 0 0 0 6 20.5h4.5V3.5"),
                     G::F(rrect(13, 4, 3, 3, .75) + rrect(17.5, 8, 3, 3, .75) + rrect(13.5, 10.5, 2.5, 2.5, .6) + rrect(18, 14, 2.5, 2.5, .6) +
                          rrect(13.5, 17, 2.5, 2.5, .6) + rrect(18.75, 19, 1.75, 1.75, .45))});
  b.add("commander", {G::S(star(12, 7.5, 4.75, 2.1, 5, -90) + "M5.5 13 12 16.5l6.5-3.5M5.5 17.5 12 21l6.5-3.5")});
  b.add("hero", {G::S("M8 2.5l2.4 6M16 2.5l-2.4 6" + circ(12, 14.5, 6)), G::F(star(12, 14.9, 3.4, 1.5, 5, -90))});
  b.add("lord", {G::S(circ(12, 11, 3.75) + "M4.5 21a7.5 7.5 0 0 1 15 0M8.25 7V3l2 1.5L12 2.5l1.75 2 2-1.5v4")});
  b.add("council", {G::S(circ(12, 6.25, 2.5) + circ(5.5, 8.75, 2) + circ(18.5, 8.75, 2) + "M7.5 15.5a4.5 4.5 0 0 1 9 0"
                            "M2.5 15.5a3 3 0 0 1 5.4-1.8M21.5 15.5a3 3 0 0 0-5.4-1.8M2.5 15.5h19M5 15.5v5M19 15.5v5")});
  b.add("ruler", {G::S("M7 21v-3.5M17 21v-3.5M5 17.5h14M7 17.5V6a1.5 1.5 0 0 1 .8-1.3L12 2.5l4.2 2.2A1.5 1.5 0 0 1 17 6v11.5"
                       "M5 12.5v5M19 12.5v5M9.5 13h5")});
  b.add("character", {G::S(rrect(2.5, 5, 19, 14, 2.5) + circ(8.5, 10.25, 2.25) + "M5 16.25a3.5 3.5 0 0 1 7 0M14.5 10h4M14.5 13.5h3")});
  b.add("portrait", {G::S(rrect(4, 3, 16, 18, 2.5) + circ(12, 10, 3) + "M7.5 21v-.5a4.5 4.5 0 0 1 9 0v.5")});
  b.add("capital", {G::S(circ(12, 12, 9)), G::F(star(12, 12.6, 5.5, 2.4, 5, -90))});
  b.add("journal", {G::S("M5 18.75V5.25A1.75 1.75 0 0 1 6.75 3.5h12.5V18H6.75A1.75 1.75 0 0 0 5 19.75 1.75 1.75 0 0 0 6.75 21.5h12.5V18"
                         "M10 3.5V10l1.75-1.4L13.5 10V3.5")});
  b.add("chronicle", {G::S("M6 3.5h10.5a2 2 0 0 1 2 2v15H8a2 2 0 0 1-2-2ZM9.5 8h5.5M9.5 11.5h5.5M9.5 15h3")});
  b.add("sea-province", {G::S(prov), G::S("M8 11q1-1 2 0t2 0 2 0M8 14q1-1 2 0t2 0 2 0", 1.5f)});
}

void defineScales(GlyphBuilder& b) {
  using G = GlyphBuilder;
  b.add("size-s", {G::S(circ(12, 12, 9)), G::F(circ(12, 12, 2.75))});
  b.add("size-m", {G::S(circ(12, 12, 9)), G::F(circ(12, 12, 4.75))});
  b.add("size-l", {G::S(circ(12, 12, 9)), G::F(circ(12, 12, 6.75))});
  b.add("c-outpost", {G::S("M3.5 20h17M12 6.5 4.5 20M12 6.5 19.5 20M12 6.5V2.5l3.5 1.25L12 5M10 20l2-4.5 2 4.5")});
  b.add("c-village", {G::S("M3.5 11.5 9 6.5l5.5 5M5 10.25V20h8V10.25M8 20v-3.5h2V20M14.5 20h6v-6.5l-3-2.75-3 2.75")});
  b.add("c-town", {G::S("M3 20.5h18M4.5 20.5v-7l3.5-3 3.5 3v7M11.5 20.5V9l3-3.5 3 3.5v11.5M14.5 5.5V2.5M17.5 13h2.5v7.5")});
  b.add("c-city", {G::S("M2.5 20.5h19M4 20.5V9h3v11.5M7 13h4V6.5l2-2 2 2v14M15 11h5v9.5M4 9V7M17.5 11V8.5")});
  b.add("b-military", {G::S("M12 2.75l7.75 2.9v5.6c0 4.8-3.2 8.6-7.75 10.5-4.55-1.9-7.75-5.7-7.75-10.5v-5.6Z"
                            "M8.25 9.5 12 12l3.75-2.5M8.25 13.5 12 16l3.75-2.5")});
  b.add("b-economic", {G::S("M3.5 9.5 5 4.5h14l1.5 5M3.5 9.5a2.13 2.13 0 0 0 4.25 0 2.13 2.13 0 0 0 4.25 0 2.13 2.13 0 0 0 4.25 0 "
                            "2.13 2.13 0 0 0 4.25 0M5 12.25v8.25h14v-8.25M10 20.5v-4.5h4v4.5")});
  b.add("b-industrial", {G::S("M3.5 7.5H15a4.5 4.5 0 0 1 4.5 4.5H16l-1.5 1.5v3h3v3h-11v-3h3v-3L8 11.5c-2.5 0-4.5-2-4.5-4Z")});
  b.add("b-residential", {G::S("M2.5 11.5 7.5 7l5 4.5M4 10.25v10.25h7V10.25M11.5 9.5 16.5 5l5 4.5M13 8.5v12h7v-12M2.5 20.5h19")});
}

void defineUnits(GlyphBuilder& b) {
  using G = GlyphBuilder;
  b.add("u-light-inf", {G::S("M4.5 13.5a7.5 7.5 0 0 1 15 0M2.75 13.75c2.75 1.2 5.85 1.75 9.25 1.75s6.5-.55 9.25-1.75"
                                 "M7.5 15.75c0 2.9 2 4.75 4.5 4.75s4.5-1.85 4.5-4.75"),
                        G::F(circ(12, 5.25, 1.25))});
  b.add("u-medium-inf", {G::S("M5.75 12.25C5.75 7.75 8.25 4.5 12 2.75c3.75 1.75 6.25 5 6.25 9.5M5 12.25h14M12 12.25v5.25"
                                  "M6.25 12.25v4.25c0 2.6 2.55 4.25 5.75 4.25s5.75-1.65 5.75-4.25v-4.25")});
  b.add("u-heavy-inf", {G::F("M6.5 4.75C8 3.9 9.9 3.5 12 3.5s4 .4 5.5 1.25l1 1.5V18.5a2 2 0 0 1-1.6 1.96L12 21.25l-4.9-.79A2 2 0 0 1 5.5 18.5V6.25Z"),
                        G::X(rrect(6.75, 9.4, 4.25, 2, 1) + rrect(13, 9.4, 4.25, 2, 1) + rrect(11.3, 13, 1.4, 5.5, .7) +
                             circ(8.6, 15, .85) + circ(15.4, 15, .85))});
  b.add("u-light-cav", {G::S(kHorse), G::F(circ(11.4, 8.4, 1))});
  const std::string lance = "M2.75 21.25 17 7M17.25 4.25l3.75-1.25-1.25 3.75-2.5 1Z";
  b.add("u-medium-cav", {G::S(lance), G::X(kHorse), G::XS(kHorse, 4.75f), G::S(kHorse), G::F(circ(11.4, 8.4, 1))});
  b.add("u-heavy-cav", {G::S(kHorse), G::F("M9 5.75c.7-.95 1.65-1.5 2.75-1.6l1.5 2.6-5.4 7.6-3-1.1c-.15-.65 0-1.3.4-1.85Z"),
                        G::X(circ(10.9, 8.6, 1.05)), G::S("M14 4.4c1.2-1.55 3-2.1 5-1.6", 1.5f)});
  b.add("u-flying", {G::S("M3 13c3-6 9-9 18-9-1 3-3 5.5-5.5 7 2 0 3.5-.5 4.5-1.5-1 3-3.5 5-6.5 5.5 1.5.5 3 .5 4-.3-2 2.5-5.5 4-9 4"
                          "-2.5 0-4.5-1-5.5-2.7")});
  b.add("u-casters", {G::S("M6.75 16.75 10.6 6.5c.6-1.55 1.95-2.6 3.6-2.85l2.3-.4-2.25 3 3.25 10.5" + ell(12, 17.75, 8.75, 2.75)),
                      G::F(star(12.4, 12, 2.6, .85, 4, -90))});
  b.add("u-ranged", {G::S("M3 10c2.6-2.8 5.6-4.25 9-4.25s6.4 1.45 9 4.25M3 10l9 5 9-5M12 2.75v18M10.25 4.5 12 2.75l1.75 1.75" +
                         rrect(10, 15, 4, 6, 1.5))});
  b.add("u-beasts", {G::F(ell(12, 16, 4.5, 3.75) + ell(5.5, 11, 1.9, 2.4) + ell(9.25, 6.5, 1.9, 2.4) + ell(14.75, 6.5, 1.9, 2.4) +
                          ell(18.5, 11, 1.9, 2.4))});
  b.add("u-monsters", {G::S("M5 4c0 3 1.5 4.5 3.5 5M19 4c0 3-1.5 4.5-3.5 5M6.5 11a5.5 5.5 0 0 1 11 0v4a5.5 5.5 0 0 1-11 0Z"
                            "M9.5 17.5l1.25-1 1.25 1 1.25-1 1.25 1"),
                       G::F(poly({8.75, 11.5, 11, 12.5, 8.75, 13.5}) + poly({15.25, 11.5, 13, 12.5, 15.25, 13.5}))});
  b.add("u-machines", {G::S("M3 18.5h15" + circ(6, 18.5, 2) + circ(15, 18.5, 2) + "M10 18.5l3-6 3 6M8 16 18 5" + circ(18.5, 4.5, 1.5))});
  // Корабли: заливка парусов, корпус — обводка.
  b.add("s-ship-line", {G::S("M2.5 14.5h19l-1.6 4.1a2.5 2.5 0 0 1-2.33 1.6H6.43a2.5 2.5 0 0 1-2.33-1.6Z"),
                        G::S("M6.5 14.5V4.5M12 14.5V2.5M17.5 14.5V4.5", 1.25f),
                        G::F(rrect(4.25, 9.25, 4.5, 3.75, .6) + rrect(4.75, 5.75, 3.5, 2.75, .5) + rrect(9.75, 8.75, 4.5, 4.25, .6) +
                             rrect(10.25, 4.25, 3.5, 3.75, .5) + rrect(15.25, 9.25, 4.5, 3.75, .6) + rrect(15.75, 5.75, 3.5, 2.75, .5))});
  b.add("s-frigate", {G::S("M3.5 15h16.5l-1.9 3.5a2.5 2.5 0 0 1-2.2 1.3H7.6a2.5 2.5 0 0 1-2.2-1.3ZM20 15l1.75-3.5"),
                      G::S("M8.5 15V4M14.5 15V5.5", 1.25f),
                      G::F(rrect(5.75, 9.5, 5.5, 4, .6) + rrect(6.25, 5, 4.5, 3.5, .5) + rrect(12, 10, 5, 3.5, .6) + rrect(12.5, 6.5, 4, 2.5, .5) +
                           poly({18.5, 12.5, 18.5, 7.5, 21.25, 12.5}))});
  b.add("s-galleon", {G::S("M2.5 11h4.75l.75 3.5h11.5l2-1.75-1.75 5.45a2.5 2.5 0 0 1-2.38 1.7H6.95a2.5 2.5 0 0 1-2.4-1.8Z"),
                      G::S("M13 14.5V2.5", 1.25f),
                      G::F("M8.25 5c3.15-.7 6.35-.7 9.5 0 .65 2.8.65 5.7 0 8.5-3.15-.7-6.35-.7-9.5 0-.65-2.8-.65-5.7 0-8.5Z"
                           "M13.6 2.25h4l-1.25 1.1 1.25 1.15h-4Z")});
}

void defineTools(GlyphBuilder& b) {
  using G = GlyphBuilder;
  b.add("tool-select", {G::S("M5 3.5v15.25l4.1-3.9 2.7 5.9 2.6-1.2-2.7-5.8 5.8-.2Z")});
  b.add("tool-pan", {G::S("M8 13V5.5a1.5 1.5 0 0 1 3 0V12M11 11V4a1.5 1.5 0 0 1 3 0v7M14 11V5.5a1.5 1.5 0 0 1 3 0V12"
                          "M17 9.5a1.5 1.5 0 0 1 3 0V15a6.5 6.5 0 0 1-6.5 6.5h-1.6c-2 0-3.6-.8-4.8-2.3L4 15a1.6 1.6 0 0 1 2.4-2.1L8 14.5")});
  b.add("tool-edit", {G::S("M7 18 9 7.5M10.75 6l7.5 2.5M19 10.5l-3 7M14.5 19H8.5"), G::S(rrect(3.5, 17.5, 3, 3, 0.5) + rrect(8, 3.5, 3, 3, 0.5) +
                                                                                      rrect(18, 7, 3, 3, 0.5) + rrect(14.5, 17.5, 3, 3, 0.5))});
  const std::string pent = "M12 3.5l8 6-3 10H7l-3-10Z";
  b.add("tool-polygon", {G::S(pent), G::F(circ(12, 3.5, 1.9) + circ(20, 9.5, 1.9) + circ(17, 19.5, 1.9) + circ(7, 19.5, 1.9) + circ(4, 9.5, 1.9))});
  badge(b, "tool-polygon-plus", "tool-polygon", {G::S("M17.5 14v7M14 17.5h7")});
  badge(b, "tool-polygon-minus", "tool-polygon", {G::S("M14 17.5h7")});
  b.add("tool-lasso", {G::S("M7.6 15.9C4.9 14.85 3 12.85 3 10.5 3 6.9 7 4 12 4s9 2.9 9 6.5-4 6.5-9 6.5c-.85 0-1.7-.08-2.5-.24", 1.75f),
                       G::XS("M7.5 4.6v2.8M13.75 3.5v2.8M19.4 7.4l-2.2 1.6M19 14.2l-2.2-1.4M12.75 15.8v2.4", 1.6f),
                       G::S(circ(8.25, 17.25, 1.75) + "M7.4 18.8c-.6 1.35-1.8 2.25-3.4 2.7")});
  b.add("tool-knife", {G::S(poly({5.25, 21.25, 10.25, 16.25, 7.75, 13.75, 2.75, 18.75}) +
                             "M11.25 12.75 18.9 5.1a1.9 1.9 0 0 1 2.7 2.7c-1.5 4-4.8 7.3-8.8 8.8Z")});
  b.add("tool-merge", {G::S("M3.5 5.5a2 2 0 0 1 2-2H12a2 2 0 0 1 2 2V10h4.5a2 2 0 0 1 2 2v6.5a2 2 0 0 1-2 2H12a2 2 0 0 1-2-2V14H5.5a2 2 0 0 1-2-2Z")});
  b.add("tool-fill", {G::S("M5 12.5 12 5.5l7 7-6.4 6.4a1.5 1.5 0 0 1-2.1 0L5 13.6a.8.8 0 0 1 0-1.1ZM12 5.5 9.5 3M5 12.5h14"),
                      G::F("M20 15.5s1.5 2 1.5 3a1.5 1.5 0 0 1-3 0c0-1 1.5-3 1.5-3Z")});
  badge(b, "tool-delete-province", "province", {G::S("M15.5 15.5l4 4M19.5 15.5l-4 4")});
  badge(b, "tool-army", "army", {G::S("M17.5 14v7M14 17.5h7")});
  badge(b, "tool-fleet", "fleet", {G::S("M17.5 14v7M14 17.5h7")});
  b.add("tool-route", {G::S(circ(6, 18, 2.25) + "M8.25 18H15a3 3 0 0 0 0-6H9a3 3 0 0 1 0-6h4M17 3v6M14 6h6")});
}

// Значок для неизвестного имени: пунктирный квадрат и «?».
VecGlyph makeMissing() {
  GlyphBuilder b(24, 0x6d697373ull << 32);
  using G = GlyphBuilder;
  std::string d;
  // Пунктир по скруглённому квадрату: отрезки сторон.
  d += "M7 3.5h3M14 3.5h3M20.5 7v3M20.5 14v3M17 20.5h-3M10 20.5H7M3.5 17v-3M3.5 10V7";
  d += "M3.5 5.5a2 2 0 0 1 2-2M18.5 3.5a2 2 0 0 1 2 2M20.5 18.5a2 2 0 0 1-2 2M5.5 20.5a2 2 0 0 1-2-2";
  b.add("?", {G::S(d), G::S("M9.6 9.6a2.5 2.5 0 0 1 4.8.9c0 1.7-2.4 2.1-2.4 3.6"), G::F(circ(12, 16.6, 1.15))});
  return *b.find("?");
}

struct IconRegistry {
  GlyphBuilder b{24, 0x69636f6eull << 32};
  std::vector<std::string> names;
  std::unordered_set<std::string> figures;
  VecGlyph missing;
};

const char* const kFigureIcons[] = {"fig-army", "fig-fleet", "fig-allied-army", "fig-allied-fleet"};

const IconRegistry& icons() {
  static const IconRegistry r = [] {
    IconRegistry x;
    defineGeneral(x.b);
    defineDomain(x.b);
    defineScales(x.b);
    defineUnits(x.b);
    defineTools(x.b);
    x.names = x.b.names();
    for (const char* f : kFigureIcons) {
      x.names.push_back(f);
      x.figures.insert(f);
    }
    x.missing = makeMissing();
    return x;
  }();
  return r;
}

std::mutex gUnknownMu;
std::unordered_set<std::string> gUnknown;

}  // namespace

bool hasIcon(std::string_view name) {
  const IconRegistry& r = icons();
  return r.b.find(name) != nullptr || r.figures.count(std::string(name)) != 0;
}

const std::vector<std::string>& iconNames() { return icons().names; }

const VecGlyph* iconGlyph(std::string_view name) { return icons().b.find(name); }

std::vector<std::string> iconRegistryIssues() { return icons().b.issues(); }

void drawIcon(Canvas& c, std::string_view name, RectF rect, Color color, float strokeScale) {
  const IconRegistry& r = icons();
  GlyphStyle st;
  st.strokeScale = std::isfinite(strokeScale) && strokeScale > 0 ? strokeScale : 1.f;
  if (const VecGlyph* g = r.b.find(name)) {
    drawGlyph(c, *g, rect, color, st);
    return;
  }
  if (r.figures.count(std::string(name))) {
    const float s = std::min(rect.w, rect.h);
    if (!(s > 0) || !std::isfinite(s)) return;
    // Габариты фигурки (с наконечником и тенью) вписываются в квадрат значка.
    const RectF fb = figureBounds({0, 0}, 1);
    const float size = s / std::max(fb.w, fb.h);
    const Pt ctr{rect.cx() - (fb.x + fb.w * 0.5f) * size, rect.cy() - (fb.y + fb.h * 0.5f) * size};
    const bool allied = name == "fig-allied-army" || name == "fig-allied-fleet";
    const Color ally = color.lighten(0.45f);
    if (name == "fig-army" || name == "fig-allied-army") drawArmyFigure(c, ctr, size, color, false, allied, ally);
    else drawFleetFigure(c, ctr, size, color, false, allied, ally);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(gUnknownMu);
    if (gUnknown.insert(std::string(name)).second) logWarn("Значок «%.*s» не найден", int(name.size()), name.data());
  }
  drawGlyph(c, r.missing, rect, color, st);
}

}  // namespace rg::gfx
