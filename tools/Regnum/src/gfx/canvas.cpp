// Regnum — холст: состояние (преобразование, отсечение, прозрачность), заливка, обводка, изображения, тени.
//
// Конвейер: контур -> Rasterizer (покрытие строк) -> Blitter (маска отсечения, источник цвета, смешивание).
// Источник цвета — сплошной цвет, градиент (таблица 257 точек 8.8 + упорядоченный дизеринг) или изображение
// (ближайший, билинейный или площадной фильтр при сильном уменьшении). Временные буферы — на поток.
#include "gfx/canvas.h"

#include <mutex>

#include "gfx/blend.h"
#include "gfx/raster.h"
#include "gfx/stroke.h"

namespace rg::gfx {

struct Canvas::State {
  Affine m;
  RectI clip;                         // в пикселях устройства, всегда внутри изображения
  std::shared_ptr<const Mask> mask;   // сглаженное отсечение (nullptr — только прямоугольник)
  RectI maskRect;                     // область устройства, которую покрывает маска (содержит clip)
  float opacity = 1;
  Blend blend = Blend::Normal;
};

namespace {

constexpr float kStrokeTolerance = 0.1f;  // пиксели устройства

// ---------------------------------------------------------------- аффинное преобразование в double
struct DAffine {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  static DAffine from(const Affine& m) { return {m.a, m.b, m.c, m.d, m.e, m.f}; }
  bool inverse(DAffine& out) const {
    double k = a * d - b * c;
    if (!std::isfinite(k) || std::fabs(k) < 1e-18) return false;
    double ia = d / k, ib = -b / k, ic = -c / k, id = a / k;
    out = {ia, ib, ic, id, -(ia * e + ic * f), -(ib * e + id * f)};
    return std::isfinite(out.a) && std::isfinite(out.b) && std::isfinite(out.c) && std::isfinite(out.d) &&
           std::isfinite(out.e) && std::isfinite(out.f);
  }
};

// ---------------------------------------------------------------- временные буферы потока
struct Scratch {
  Rasterizer ras;
  Path shape, outline;
  std::vector<u32> colors;
  std::vector<u8> cov;
  std::vector<i32> colX0, colX1;
  std::vector<u32> colFx;
  std::vector<i32> tapStart, tapCount, tapIdx;
  std::vector<float> tapW;
  std::vector<i32> rowIdx;
  std::vector<float> rowW, acc;
  std::vector<u32> rowTmp;
  void ensure(int w) {
    if (colors.size() < size_t(w)) colors.resize(size_t(w));
    if (cov.size() < size_t(w)) cov.resize(size_t(w));
  }
};
Scratch& scratch() {
  thread_local Scratch s;
  return s;
}

u8 toU8(float a) { return u8(std::clamp(a, 0.f, 1.f) * 255.f + 0.5f); }

// ---------------------------------------------------------------- источники цвета
struct Shader {
  virtual ~Shader() = default;
  // Цвета (premultiplied) для пикселей [x, x + n) строки y.
  virtual void shade(int x, int y, int n, u32* out) = 0;
};

const u8 kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// Градиент: таблица из kN premultiplied-цветов 8.8 (4 канала в одном u64: a | r | g | b) и упорядоченный
// дизеринг 4×4: к каждому каналу прибавляется один и тот же порог, поэтому каналы не превышают альфу.
class GradientShader final : public Shader {
 public:
  static constexpr int kN = 512;

  // false — градиент вырожден: рисуется сплошным цветом solid.
  bool init(const Gradient& g, const Affine& ctm, Color& solid) {
    std::vector<std::pair<float, Color>> stops;
    stops.reserve(g.stops.size());
    for (auto& s : g.stops) stops.push_back({std::isfinite(s.first) ? std::clamp(s.first, 0.f, 1.f) : 0.f, s.second});
    std::stable_sort(stops.begin(), stops.end(), [](auto& a, auto& b) { return a.first < b.first; });
    if (stops.empty()) return false;
    solid = stops.back().second;
    if (stops.size() == 1) return false;
    DAffine inv;
    if (!DAffine::from(ctm).inverse(inv)) return false;
    radial_ = g.kind == Gradient::Radial;
    if (radial_) {
      if (!(g.r1 > 0) || !std::isfinite(g.r1)) return false;
      inv_ = inv;
      cx_ = g.p0.x;
      cy_ = g.p0.y;
      invR_ = 1.0 / g.r1;
    } else {
      double vx = double(g.p1.x) - g.p0.x, vy = double(g.p1.y) - g.p0.y, l2 = vx * vx + vy * vy;
      if (!(l2 > 1e-12) || !std::isfinite(l2)) return false;
      ax_ = (inv.a * vx + inv.b * vy) / l2;
      ay_ = (inv.c * vx + inv.d * vy) / l2;
      ac_ = ((inv.e - g.p0.x) * vx + (inv.f - g.p0.y) * vy) / l2;
    }
    // Интерполяция в premultiplied-пространстве (без тёмных ореолов у прозрачных точек).
    auto q88 = [](float v) { return u64(std::clamp(v * 256.f + 0.5f, 0.f, 65280.f)); };
    size_t j = 0;
    for (int i = 0; i < kN; i++) {
      const float t = float(i) / float(kN - 1);
      while (j + 1 < stops.size() && !(t < stops[j + 1].first)) j++;
      Color c0 = stops[j].second, c1 = c0;
      float k = 0;
      if (j + 1 < stops.size() && t > stops[j].first) {
        c1 = stops[j + 1].second;
        const float span = stops[j + 1].first - stops[j].first;
        k = span > 0 ? (t - stops[j].first) / span : 0.f;
      }
      const float a0 = c0.a / 255.f, a1 = c1.a / 255.f;
      const float pa = a0 + (a1 - a0) * k;
      const float pr = c0.r * a0 + (c1.r * a1 - c0.r * a0) * k;
      const float pg = c0.g * a0 + (c1.g * a1 - c0.g * a0) * k;
      const float pb = c0.b * a0 + (c1.b * a1 - c0.b * a0) * k;
      lut_[i] = (q88(pa * 255.f) << 48) | (q88(pr) << 32) | (q88(pg) << 16) | q88(pb);
    }
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++) dither_[r][c] = u64(kBayer[r][c] * 16 + 8) * 0x0001000100010001ull;
    return true;
  }

  void shade(int x, int y, int n, u32* out) override {
    const u64* d = dither_[y & 3];
    const double px = x + 0.5, py = y + 0.5;
    if (!radial_) {
      double t = ax_ * px + ay_ * py + ac_;
      if (ax_ == 0) {
        // Вертикальный градиент: цвет постоянен в строке, меняется только порог дизеринга.
        const u64 v = lut_[idx(t)];
        const u32 col[4] = {pack(v + d[0]), pack(v + d[1]), pack(v + d[2]), pack(v + d[3])};
        for (int i = 0; i < n; i++) out[i] = col[(x + i) & 3];
        return;
      }
      for (int i = 0; i < n; i++, t += ax_) out[i] = pack(lut_[idx(t)] + d[(x + i) & 3]);
    } else {
      double ux = inv_.a * px + inv_.c * py + inv_.e - cx_, uy = inv_.b * px + inv_.d * py + inv_.f - cy_;
      for (int i = 0; i < n; i++, ux += inv_.a, uy += inv_.b) out[i] = pack(lut_[idx(std::sqrt(ux * ux + uy * uy) * invR_)] + d[(x + i) & 3]);
    }
  }

 private:
  static int idx(double t) { return t > 0 ? (t < 1 ? int(t * (kN - 1) + 0.5) : kN - 1) : 0; }
  // Четыре поля 8.8 -> байты пикселя 0xAARRGGBB.
  static u32 pack(u64 v) {
    v = (v >> 8) & 0x00FF00FF00FF00FFull;
    v |= v >> 8;
    return u32(v & 0xFFFF) | (u32(v >> 16) & 0xFFFF0000u);
  }

  u64 lut_[kN];        // заполняется в init()
  u64 dither_[4][4];
  bool radial_ = false;
  double ax_ = 0, ay_ = 0, ac_ = 0;
  DAffine inv_;
  double cx_ = 0, cy_ = 0, invR_ = 0;
};

// Индекс по оси при повторе изображения (узор): v — целое значение в double, результат в [0, n).
inline i32 wrapIdx(double v, i32 n) {
  double m = std::fmod(v, double(n));
  if (m < 0) m += n;
  const i32 i = (m >= 0 && m < n) ? i32(m) : 0;
  return i >= n ? n - 1 : i;
}

// Изображение: inv — из устройства в пиксели изображения. За пределами bounds — крайние пиксели
// либо (wrap) повтор всего изображения.
class ImageShader final : public Shader {
 public:
  ImageShader(const Image& img, const DAffine& inv, RectI bounds, bool bilinear, bool wrap, int dx0, int dx1)
      : img_(img), inv_(inv), bilinear_(bilinear), wrap_(wrap), S_(scratch()) {
    if (wrap) bounds = {0, 0, img.w, img.h};
    bounds = bounds.intersect({0, 0, img.w, img.h});
    valid_ = !bounds.empty() && dx1 > dx0;
    if (!valid_) return;
    bx0_ = bounds.x; by0_ = bounds.y; bx1_ = bounds.right() - 1; by1_ = bounds.bottom() - 1;
    axis_ = std::fabs(inv.b) < 1e-12 && std::fabs(inv.c) < 1e-12;
    dx0_ = dx0;
    dx1_ = dx1;
    if (!axis_) return;
    area_ = bilinear && (std::fabs(inv.a) > 2.0 || std::fabs(inv.d) > 2.0);
    const int n = dx1 - dx0;
    if (area_) {
      S_.tapStart.resize(size_t(n));
      S_.tapCount.resize(size_t(n));
      S_.tapIdx.clear();
      S_.tapW.clear();
      for (int i = 0; i < n; i++) {
        double x = dx0 + i;
        S_.tapStart[size_t(i)] = i32(S_.tapIdx.size());
        taps(inv.a * x + inv.e, inv.a * (x + 1) + inv.e, bx0_, bx1_, wrap_, S_.tapIdx, S_.tapW);
        S_.tapCount[size_t(i)] = i32(S_.tapIdx.size()) - S_.tapStart[size_t(i)];
      }
      accY_ = std::numeric_limits<int>::min();
    } else {
      S_.colX0.resize(size_t(n));
      S_.colX1.resize(size_t(n));
      S_.colFx.resize(size_t(n));
      sx0_ = std::numeric_limits<i32>::max();
      sx1_ = std::numeric_limits<i32>::min();
      for (int i = 0; i < n; i++) {
        double u = inv.a * (dx0 + i + 0.5) + inv.e;
        i32 x0, x1;
        u32 fx;
        coord(u, bx0_, bx1_, x0, x1, fx);
        S_.colX0[size_t(i)] = x0;
        S_.colX1[size_t(i)] = x1;
        S_.colFx[size_t(i)] = fx;
        sx0_ = std::min({sx0_, x0, x1});
        sx1_ = std::max({sx1_, x0, x1});
      }
      rowY_ = std::numeric_limits<int>::min();
    }
  }

  bool valid() const { return valid_; }

  void shade(int x, int y, int n, u32* out) override {
    if (axis_) {
      if (area_) shadeArea(x, y, n, out);
      else shadeAxis(x, y, n, out);
    } else {
      shadeAffine(x, y, n, out);
    }
  }

 private:
  // Координата выборки по оси: индексы соседей и доля (0..256).
  void coord(double u, i32 lo, i32 hi, i32& i0, i32& i1, u32& f) const {
    if (!std::isfinite(u)) u = 0;
    if (wrap_) {
      const i32 n = hi - lo + 1;
      if (!bilinear_) {
        i0 = i1 = lo + wrapIdx(std::floor(u), n);
        f = 0;
        return;
      }
      const double s = u - 0.5;
      double fl = std::floor(s);
      f = u32((s - fl) * 256.0 + 0.5);
      if (f >= 256) { fl += 1; f = 0; }
      i0 = wrapIdx(fl, n);
      i1 = lo + (i0 + 1 == n ? 0 : i0 + 1);
      i0 += lo;
      return;
    }
    if (!bilinear_) {
      double fl = std::floor(u);
      i0 = fl < lo ? lo : fl > hi ? hi : i32(fl);
      i1 = i0;
      f = 0;
      return;
    }
    double s = u - 0.5;
    double fl = std::floor(s);
    if (fl < lo) { i0 = i1 = lo; f = 0; return; }
    if (fl >= hi) { i0 = i1 = hi; f = 0; return; }
    i0 = i32(fl);
    f = u32((s - fl) * 256.0 + 0.5);
    if (f >= 256) { i0++; f = 0; }
    i1 = std::min(i0 + 1, hi);
    if (i1 == i0) f = 0;
  }

  // Площадной фильтр (уменьшение > 2×) или билинейные веса для отрезка [a, b) исходной оси.
  static void taps(double a, double b, i32 lo, i32 hi, bool wrap, std::vector<i32>& idx, std::vector<float>& w) {
    if (!std::isfinite(a) || !std::isfinite(b)) { idx.push_back(lo); w.push_back(1); return; }
    if (a > b) std::swap(a, b);
    if (wrap) {
      const i32 n = hi - lo + 1;
      if (b - a <= 2.0) {
        const double s = (a + b) * 0.5 - 0.5, fl = std::floor(s);
        const float f = float(s - fl);
        const i32 i0 = wrapIdx(fl, n);
        idx.push_back(lo + i0); w.push_back(1 - f);
        idx.push_back(lo + (i0 + 1 == n ? 0 : i0 + 1)); w.push_back(f);
        return;
      }
      // Сдвиг на целое число периодов: счётчик цикла мал при любых координатах.
      const double shift = std::floor(a / n) * n;
      const double sa = a - shift, sb = b - shift;
      if (!(b - a < 4.0 * n) || !(sa >= 0 && sa <= n && sb > sa)) {
        // След пикселя покрывает много периодов узора (или точность исчерпана) — среднее по периоду.
        for (i32 k = 0; k < n; k++) { idx.push_back(lo + k); w.push_back(1.0f / float(n)); }
        return;
      }
      const double inv = 1.0 / (sb - sa);
      for (i32 k = i32(std::floor(sa)), ke = i32(std::ceil(sb)); k < ke; k++) {
        const double o = std::min(sb, double(k) + 1) - std::max(sa, double(k));
        if (o > 0) { idx.push_back(lo + wrapIdx(k, n)); w.push_back(float(o * inv)); }
      }
      return;
    }
    if (b - a <= 2.0) {
      double s = (a + b) * 0.5 - 0.5, fl = std::floor(s);
      if (fl < lo) { idx.push_back(lo); w.push_back(1); return; }
      if (fl >= hi) { idx.push_back(hi); w.push_back(1); return; }
      float f = float(s - fl);
      idx.push_back(i32(fl)); w.push_back(1 - f);
      idx.push_back(i32(fl) + 1); w.push_back(f);
      return;
    }
    double ca = std::max(a, double(lo)), cb = std::min(b, double(hi) + 1);
    if (cb <= ca) { idx.push_back(b <= lo ? lo : hi); w.push_back(1); return; }
    const double inv = 1.0 / (cb - ca);
    for (i32 k = i32(std::floor(ca)); k < i32(std::ceil(cb)); k++) {
      double o = std::min(cb, double(k) + 1) - std::max(ca, double(k));
      if (o > 0) { idx.push_back(k); w.push_back(float(o * inv)); }
    }
  }

  // Ось-ориентированный случай: сначала смешивание двух строк источника (один раз на строку, непрерывный
  // векторизуемый цикл по нужному диапазону столбцов), затем по x для каждого пикселя.
  void shadeAxis(int x, int y, int n, u32* out) {
    if (rowY_ != y) {
      rowY_ = y;
      i32 y0, y1;
      u32 fy;
      coord(inv_.d * (y + 0.5) + inv_.f, by0_, by1_, y0, y1, fy);
      const u32* r0 = img_.row(y0);
      if (fy == 0) {
        rowPtr_ = r0;
        rowOff_ = 0;
      } else {
        const u32* r1 = img_.row(y1);
        const size_t w = size_t(sx1_ - sx0_ + 1);
        if (S_.rowTmp.size() < w) S_.rowTmp.resize(w);
        u32* t = S_.rowTmp.data();
        for (size_t i = 0; i < w; i++) t[i] = px::lerp(r0[sx0_ + i32(i)], r1[sx0_ + i32(i)], fy);
        rowPtr_ = t;
        rowOff_ = sx0_;
      }
    }
    const u32* rp = rowPtr_;
    const i32 off = rowOff_;
    const i32* cx0 = S_.colX0.data() + (x - dx0_);
    const i32* cx1 = S_.colX1.data() + (x - dx0_);
    const u32* cf = S_.colFx.data() + (x - dx0_);
    for (int i = 0; i < n; i++) {
      const u32 fx = cf[i];
      out[i] = fx ? px::lerp(rp[cx0[i] - off], rp[cx1[i] - off], fx) : rp[cx0[i] - off];
    }
  }

  void shadeArea(int x, int y, int n, u32* out) {
    if (accY_ != y) {
      // Вертикальная свёртка нужных строк в буфер acc для всего диапазона столбцов фигуры.
      accY_ = y;
      S_.rowIdx.clear();
      S_.rowW.clear();
      taps(inv_.d * y + inv_.f, inv_.d * (y + 1) + inv_.f, by0_, by1_, wrap_, S_.rowIdx, S_.rowW);
      const int cnt = dx1_ - dx0_;
      i32 lo = bx0_, hi = bx1_;
      if (!wrap_) {
        // Отсчёты монотонны по столбцам: диапазон определяется крайними столбцами.
        lo = std::numeric_limits<i32>::max();
        hi = std::numeric_limits<i32>::min();
        for (int i : {0, cnt - 1}) {
          i32 s = S_.tapStart[size_t(i)], c = S_.tapCount[size_t(i)];
          lo = std::min({lo, S_.tapIdx[size_t(s)], S_.tapIdx[size_t(s + c - 1)]});
          hi = std::max({hi, S_.tapIdx[size_t(s)], S_.tapIdx[size_t(s + c - 1)]});
        }
      }
      accX0_ = lo;
      const size_t w = size_t(hi - lo + 1);
      S_.acc.assign(w * 4, 0.f);
      float* acc = S_.acc.data();
      for (size_t r = 0; r < S_.rowIdx.size(); r++) {
        const float wy = S_.rowW[r];
        const u32* row = img_.row(S_.rowIdx[r]) + lo;
        for (size_t k = 0; k < w; k++) {
          u32 p = row[k];
          acc[k * 4 + 0] += wy * float(p >> 24);
          acc[k * 4 + 1] += wy * float((p >> 16) & 255);
          acc[k * 4 + 2] += wy * float((p >> 8) & 255);
          acc[k * 4 + 3] += wy * float(p & 255);
        }
      }
    }
    const float* acc = S_.acc.data();
    for (int i = 0; i < n; i++) {
      const size_t c = size_t(x + i - dx0_);
      const i32 s = S_.tapStart[c], cnt = S_.tapCount[c];
      float a = 0, r = 0, g = 0, b = 0;
      for (i32 k = 0; k < cnt; k++) {
        const float w = S_.tapW[size_t(s + k)];
        const float* v = acc + size_t(S_.tapIdx[size_t(s + k)] - accX0_) * 4;
        a += w * v[0]; r += w * v[1]; g += w * v[2]; b += w * v[3];
      }
      u32 ca = std::min<u32>(u32(a + 0.5f), 255);
      out[i] = (ca << 24) | (std::min(u32(r + 0.5f), ca) << 16) | (std::min(u32(g + 0.5f), ca) << 8) | std::min(u32(b + 0.5f), ca);
    }
  }

  void shadeAffine(int x, int y, int n, u32* out) {
    const double h = bilinear_ ? 0.5 : 0.0, px = x + 0.5, py = y + 0.5;
    auto clampD = [](double v) { return std::isfinite(v) ? std::clamp(v, -1e9, 1e9) : 0.0; };
    const double u = clampD(inv_.a * px + inv_.c * py + inv_.e - h), v = clampD(inv_.b * px + inv_.d * py + inv_.f - h);
    i64 U = i64(std::floor(u * 65536.0)), V = i64(std::floor(v * 65536.0));
    const i64 dU = i64(std::clamp(inv_.a, -1e6, 1e6) * 65536.0), dV = i64(std::clamp(inv_.b, -1e6, 1e6) * 65536.0);
    if (wrap_) {
      const i64 W = bx1_ + 1, H = by1_ + 1;
      for (int i = 0; i < n; i++, U += dU, V += dV) {
        const i32 x0 = i32((((U >> 16) % W) + W) % W), y0 = i32((((V >> 16) % H) + H) % H);
        const i32 x1 = x0 + 1 == W ? 0 : x0 + 1, y1 = y0 + 1 == H ? 0 : y0 + 1;
        const u32 fx = bilinear_ ? u32((U >> 8) & 255) : 0, fy = bilinear_ ? u32((V >> 8) & 255) : 0;
        const u32* r0 = img_.row(y0);
        const u32* r1 = img_.row(y1);
        out[i] = px::bilerp(r0[x0], r0[x1], r1[x0], r1[x1], fx, fy);
      }
      return;
    }
    for (int i = 0; i < n; i++, U += dU, V += dV) {
      i32 x0 = i32(U >> 16), y0 = i32(V >> 16);
      u32 fx = bilinear_ ? u32((U >> 8) & 255) : 0, fy = bilinear_ ? u32((V >> 8) & 255) : 0;
      if (x0 < bx0_) { x0 = bx0_; fx = 0; } else if (x0 >= bx1_) { x0 = bx1_; fx = 0; }
      if (y0 < by0_) { y0 = by0_; fy = 0; } else if (y0 >= by1_) { y0 = by1_; fy = 0; }
      const u32* r0 = img_.row(y0);
      if (fy == 0) {
        out[i] = fx ? px::lerp(r0[x0], r0[x0 + 1], fx) : r0[x0];
      } else {
        const u32* r1 = img_.row(y0 + 1);
        out[i] = px::bilerp(r0[x0], r0[x0 + (fx ? 1 : 0)], r1[x0], r1[x0 + (fx ? 1 : 0)], fx, fy);
      }
    }
  }

  const Image& img_;
  DAffine inv_;
  bool bilinear_, wrap_;
  Scratch& S_;
  bool valid_ = false, axis_ = false, area_ = false;
  i32 bx0_ = 0, by0_ = 0, bx1_ = 0, by1_ = 0;
  int dx0_ = 0, dx1_ = 0;
  int accY_ = 0, rowY_ = 0;
  i32 sx0_ = 0, sx1_ = 0, rowOff_ = 0;  // диапазон столбцов источника (ось-ориентированный случай)
  const u32* rowPtr_ = nullptr;    // строка источника после смешивания по y (индексация по x источника)
  i32 accX0_ = 0;
};

// ---------------------------------------------------------------- смешивание строк
struct Blitter final : SpanSink {
  Image& img;
  const Mask* mask = nullptr;
  int mx = 0, my = 0;
  Blend blend = Blend::Normal;
  u32 solid = 0;
  Shader* shader = nullptr;
  u32 alpha = 255;
  u8* tcov = nullptr;
  u32* tcol = nullptr;

  explicit Blitter(Image& i) : img(i) {}

  void row(int y, const Span* s, int n) override {
    u32* d = img.row(y);
    for (int i = 0; i < n; i++) span(d, y, s[i].x, s[i].len, s[i].cov, s[i].value);
  }

  void span(u32* d, int y, int x, int len, const u8* cov, u32 val) {
    if (len <= 0) return;
    if (mask) {
      const u8* m = mask->row(y - my) + (x - mx);
      if (cov) for (int i = 0; i < len; i++) tcov[i] = u8(px::mul8(cov[i], m[i]));
      else for (int i = 0; i < len; i++) tcov[i] = u8(px::mul8(val, m[i]));
      cov = tcov;
    }
    if (shader) {
      if (alpha != 255) {
        if (cov) {
          for (int i = 0; i < len; i++) tcov[i] = u8(px::mul8(cov[i], alpha));
          cov = tcov;
        } else {
          val = px::mul8(val, alpha);
        }
      }
      shader->shade(x, y, len, tcol);
      px::colors(blend, d + x, len, tcol, cov, val);
    } else if (cov) {
      px::solidCov(blend, d + x, len, solid, cov);
    } else {
      px::solidConst(blend, d + x, len, val == 255 ? solid : px::mul(solid, val));
    }
  }
};

// Подготовка источника цвета. false — рисовать нечего.
struct PaintSetup {
  GradientShader grad;
  std::optional<ImageShader> image;
  Shader* shader = nullptr;
  u32 solid = 0;
  u32 alpha = 255;
};

}  // namespace

// ================================================================ реализация Canvas
struct CanvasImpl {
  static Canvas::State& st(Canvas& c) { return *c.st_; }

  static Blend blendOf(const Canvas::State& s, const Paint& p) { return p.blend != Blend::Normal ? p.blend : s.blend; }

  static bool setupPaint(Canvas::State& s, const Paint& paint, const RectI& area, PaintSetup& ps) {
    float op = paint.opacity * s.opacity;
    if (!(op > 0)) return false;
    op = std::min(op, 1.f);
    if (paint.image && !paint.image->empty()) {
      DAffine inv;
      if (!DAffine::from(s.m * paint.imageXf).inverse(inv)) return false;
      ps.image.emplace(*paint.image, inv, RectI{0, 0, paint.image->w, paint.image->h}, paint.bilinear, paint.tile, area.x, area.right());
      if (!ps.image->valid()) return false;
      ps.shader = &*ps.image;
      ps.alpha = toU8(op);
      return ps.alpha > 0;
    }
    if (paint.gradient) {
      Color solid;
      if (ps.grad.init(*paint.gradient, s.m, solid)) {
        ps.shader = &ps.grad;
        ps.alpha = toU8(op);
        return ps.alpha > 0;
      }
      if (paint.gradient->stops.empty()) solid = paint.color;
      ps.solid = premul(solid.alpha(op));
      return ps.solid != 0;
    }
    ps.solid = premul(paint.color.alpha(op));
    return ps.solid != 0;
  }

  static void initBlitter(Blitter& b, Canvas::State& s, Scratch& S, Blend blend) {
    S.ensure(b.img.w);
    b.tcov = S.cov.data();
    b.tcol = S.colors.data();
    b.blend = blend;
    if (s.mask) {
      b.mask = s.mask.get();
      b.mx = s.maskRect.x;
      b.my = s.maskRect.y;
    }
  }

  // Заливка контура в координатах xf.
  static void fill(Canvas& c, const Path& path, const Affine& xf, const Paint& paint, FillRule rule) {
    Canvas::State& s = st(c);
    if (s.clip.empty() || path.empty()) return;
    Scratch& S = scratch();
    S.ras.reset(s.clip);
    S.ras.addPath(path, xf, kFillTolerance);
    if (S.ras.invalid() || S.ras.empty()) return;
    const RectI area = S.ras.bounds();
    if (area.empty()) return;
    PaintSetup ps;
    if (!setupPaint(s, paint, area, ps)) return;
    Blitter b(*c.img_);
    initBlitter(b, s, S, blendOf(s, paint));
    b.solid = ps.solid;
    b.shader = ps.shader;
    b.alpha = ps.alpha;
    S.ras.sweep(rule, b);
  }

  // Видимая область в координатах пользователя (для спрямления далёких кривых обводки).
  static bool userClip(const Canvas::State& s, RectF& out) {
    DAffine inv;
    if (!DAffine::from(s.m).inverse(inv)) return false;
    double x0 = kInf, y0 = kInf, x1 = -kInf, y1 = -kInf;
    const double xs[2] = {double(s.clip.x), double(s.clip.right())}, ys[2] = {double(s.clip.y), double(s.clip.bottom())};
    for (double X : xs)
      for (double Y : ys) {
        double ux = inv.a * X + inv.c * Y + inv.e, uy = inv.b * X + inv.d * Y + inv.f;
        x0 = std::min(x0, ux); x1 = std::max(x1, ux);
        y0 = std::min(y0, uy); y1 = std::max(y1, uy);
      }
    if (!(std::fabs(x0) < 1e30 && std::fabs(x1) < 1e30 && std::fabs(y0) < 1e30 && std::fabs(y1) < 1e30)) return false;
    out = {float(x0), float(y0), float(x1 - x0), float(y1 - y0)};
    return true;
  }

  // Обводка контура (path) или ломаной (pts).
  static void stroke(Canvas& c, const Path* path, const Pt* pts, size_t n, bool closed, const Stroke& sk, const Paint& paint) {
    Canvas::State& s = st(c);
    if (s.clip.empty()) return;
    const float sc = s.m.scaleFactor();
    if (!(sc > 0) || !std::isfinite(sc)) return;
    if (!(sk.width > 0) || !std::isfinite(sk.width)) return;
    const float dev = sk.width * sc;
    const Stroke* use = &sk;
    Stroke hair;
    Paint p2 = paint;
    if (dev < 1) {
      // Волосяная линия: ширина 1 пиксель, покрытие пропорционально толщине.
      hair = sk;
      hair.width = 1 / sc;
      use = &hair;
      p2.opacity *= dev;
    }
    const float tol = kStrokeTolerance / sc;
    Scratch& S = scratch();
    S.outline.clear();
    if (path) {
      RectF cull;
      strokeToPath(*path, *use, tol, S.outline, userClip(s, cull) ? &cull : nullptr);
    } else {
      RectF cull;
      strokePolyline(pts, n, closed, *use, tol, S.outline, userClip(s, cull) ? &cull : nullptr);
    }
    fill(c, S.outline, s.m, p2, FillRule::NonZero);
  }

  // Изображение: quad — область изображения (в его пикселях), bounds — допустимая область выборки.
  static void image(Canvas& c, const Image& img, const RectF& quad, RectI bounds, const Affine& imageToUser, float opacity,
                    bool bilinear) {
    Canvas::State& s = st(c);
    if (s.clip.empty() || img.empty()) return;
    float op = opacity * s.opacity;
    if (!(op > 0)) return;
    const u32 a8 = toU8(op);
    if (a8 == 0) return;
    const Affine total = s.m * imageToUser;
    const Blend blend = s.blend;
    bounds = bounds.intersect({0, 0, img.w, img.h});
    if (bounds.empty()) return;
    // Копия 1:1 со сдвигом на целое число пикселей.
    if (!s.mask && total.b == 0 && total.c == 0 && std::fabs(total.a - 1) < 1e-6f && std::fabs(total.d - 1) < 1e-6f) {
      const double ex = total.e, ey = total.f, rx = std::floor(ex + 0.5), ry = std::floor(ey + 0.5);
      auto isInt = [](float v) { return std::fabs(v - std::floor(v + 0.5f)) < 1e-4f; };
      if (std::fabs(ex - rx) < 1e-3 && std::fabs(ey - ry) < 1e-3 && isInt(quad.x) && isInt(quad.y) && isInt(quad.w) &&
          isInt(quad.h) && std::fabs(rx) < 1e8 && std::fabs(ry) < 1e8) {
        const int ox = int(rx), oy = int(ry);
        RectI q{int(std::floor(quad.x + 0.5f)), int(std::floor(quad.y + 0.5f)), int(std::floor(quad.w + 0.5f)), int(std::floor(quad.h + 0.5f))};
        q = q.intersect(bounds);
        RectI dev = RectI{q.x + ox, q.y + oy, q.w, q.h}.intersect(s.clip);
        for (int y = dev.y; y < dev.bottom(); y++)
          px::colors(blend, c.img_->row(y) + dev.x, dev.w, img.row(y - oy) + (dev.x - ox), nullptr, a8);
        return;
      }
    }
    Scratch& S = scratch();
    S.shape.clear();
    S.shape.addRect(quad);
    S.ras.reset(s.clip);
    S.ras.addPath(S.shape, total, kFillTolerance);
    if (S.ras.invalid() || S.ras.empty()) return;
    const RectI area = S.ras.bounds();
    if (area.empty()) return;
    DAffine inv;
    if (!DAffine::from(total).inverse(inv)) return;
    ImageShader sh(img, inv, bounds, bilinear, false, area.x, area.right());
    if (!sh.valid()) return;
    Blitter b(*c.img_);
    initBlitter(b, s, S, blend);
    b.shader = &sh;
    b.alpha = a8;
    S.ras.sweep(FillRule::NonZero, b);
  }
};

// ---------------------------------------------------------------- размытие
namespace {

// Размеры трёх ящиков, приближающих гауссиан с сигмой sigma (радиусы ящиков).
void boxRadii(double sigma, int r[3]) {
  sigma = std::isfinite(sigma) ? std::clamp(sigma, 0.0, 1.0e5) : 0.0;  // защита от переполнения int
  const double s2 = sigma * sigma;
  const double wIdeal = std::sqrt(12.0 * s2 / 3.0 + 1.0);
  int wl = int(std::floor(wIdeal));
  if (wl % 2 == 0) wl--;
  wl = std::max(wl, 1);
  const int wu = wl + 2;
  const double mIdeal = (12.0 * s2 - 3.0 * wl * wl - 12.0 * wl - 9.0) / (-4.0 * wl - 4.0);
  const int m = std::clamp(int(std::lround(mIdeal)), 0, 3);
  for (int i = 0; i < 3; i++) r[i] = ((i < m ? wl : wu) - 1) / 2;
}

// Скользящее среднее окна 2r+1; за краем — нули.
void boxZero(const i32* src, i32* dst, int n, int r) {
  if (r <= 0) { std::copy(src, src + n, dst); return; }
  const i64 w = 2 * r + 1;
  i64 sum = 0;
  for (int j = 0; j <= std::min(r, n - 1); j++) sum += src[j];
  for (int i = 0; i < n; i++) {
    dst[i] = i32((sum + w / 2) / w);
    if (i + r + 1 < n) sum += src[i + r + 1];
    if (i - r >= 0) sum -= src[i - r];
  }
}

// Скользящее среднее окна 2r+1 по строке из n пикселей с 4 чередующимися каналами; за краем — крайнее значение.
// Деление заменено умножением на обратную величину (32.32).
void boxRow4(const u32* src, u32* dst, int n, int r) {
  if (r <= 0) { std::copy(src, src + size_t(n) * 4, dst); return; }
  const u64 w = u64(2 * r + 1), inv = ((u64(1) << 32) + w - 1) / w, half = u64(1) << 31;
  u64 s[4];
  for (int c = 0; c < 4; c++) s[c] = u64(r + 1) * src[c];
  for (int k = 1; k <= r; k++) {
    const u32* p = src + size_t(std::min(k, n - 1)) * 4;
    for (int c = 0; c < 4; c++) s[c] += p[c];
  }
  for (int i = 0; i < n; i++) {
    u32* o = dst + size_t(i) * 4;
    const u32* ad = src + size_t(std::min(i + r + 1, n - 1)) * 4;
    const u32* sb = src + size_t(std::max(i - r, 0)) * 4;
    for (int c = 0; c < 4; c++) {
      o[c] = u32((s[c] * inv + half) >> 32);
      s[c] = s[c] + ad[c] - sb[c];
    }
  }
}

// Вертикальный проход: скользящие суммы сразу для всей строки (последовательный доступ к памяти).
void boxCols(const u32* src, u32* dst, size_t rowLen, int h, int r, std::vector<u64>& acc) {
  if (r <= 0) { std::copy(src, src + rowLen * size_t(h), dst); return; }
  const u64 w = u64(2 * r + 1), inv = ((u64(1) << 32) + w - 1) / w, half = u64(1) << 31;
  acc.assign(rowLen, 0);
  auto rowp = [&](int y) { return src + size_t(std::clamp(y, 0, h - 1)) * rowLen; };
  for (int k = -r; k <= r; k++) {
    const u32* p = rowp(k);
    for (size_t i = 0; i < rowLen; i++) acc[i] += p[i];
  }
  for (int y = 0; y < h; y++) {
    u32* o = dst + size_t(y) * rowLen;
    for (size_t i = 0; i < rowLen; i++) o[i] = u32((acc[i] * inv + half) >> 32);
    const u32* ad = rowp(y + r + 1);
    const u32* sb = rowp(y - r);
    for (size_t i = 0; i < rowLen; i++) acc[i] = acc[i] + ad[i] - sb[i];
  }
}

// Размытие плоскости w×h (значения ×256) тремя ящиками по строкам и столбцам; за краем — нули.
void blurPlaneZero(std::vector<i32>& p, int w, int h, const int r[3]) {
  std::vector<i32> a(size_t(std::max(w, h))), b(a.size());
  for (int y = 0; y < h; y++) {
    i32* row = p.data() + size_t(y) * size_t(w);
    boxZero(row, a.data(), w, r[0]);
    boxZero(a.data(), b.data(), w, r[1]);
    boxZero(b.data(), row, w, r[2]);
  }
  for (int x = 0; x < w; x++) {
    for (int y = 0; y < h; y++) b[size_t(y)] = p[size_t(y) * size_t(w) + size_t(x)];
    boxZero(b.data(), a.data(), h, r[0]);
    boxZero(a.data(), b.data(), h, r[1]);
    boxZero(b.data(), a.data(), h, r[2]);
    for (int y = 0; y < h; y++) p[size_t(y) * size_t(w) + size_t(x)] = a[size_t(y)];
  }
}

// ---------------------------------------------------------------- кеш теней
// Маска размытого скруглённого прямоугольника. При растяжении по оси (sx/sy) хранится каноническая
// плитка: углы и один средний столбец/строка, которые повторяются для любого размера.
struct ShadowTile {
  Mask m;
  int pad = 0, k = 0;       // поле размытия; зона угла внутри прямоугольника
  int w = 0, h = 0;         // размер прямоугольника внутри маски
  bool sx = false, sy = false;
};

struct ShadowKey {
  i32 w, h, r4, s16;
  bool operator==(const ShadowKey&) const = default;
};

class ShadowCache {
 public:
  std::shared_ptr<const ShadowTile> find(const ShadowKey& k) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& e : entries_)
      if (e.key == k) { e.used = ++tick_; return e.tile; }
    return nullptr;
  }
  void put(const ShadowKey& k, std::shared_ptr<const ShadowTile> t) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& e : entries_)
      if (e.key == k) return;
    bytes_ += t->m.a.size();
    entries_.push_back({k, std::move(t), ++tick_});
    while ((bytes_ > kMaxBytes || entries_.size() > kMaxEntries) && entries_.size() > 1) {
      size_t old = 0;
      for (size_t i = 1; i < entries_.size(); i++)
        if (entries_[i].used < entries_[old].used) old = i;
      bytes_ -= entries_[old].tile->m.a.size();
      entries_.erase(entries_.begin() + long(old));
    }
  }

 private:
  static constexpr size_t kMaxBytes = size_t(32) << 20;
  static constexpr size_t kMaxEntries = 256;
  struct Entry { ShadowKey key; std::shared_ptr<const ShadowTile> tile; u64 used; };
  std::mutex mu_;
  std::vector<Entry> entries_;
  size_t bytes_ = 0;
  u64 tick_ = 0;
};

ShadowCache& shadowCache() {
  static ShadowCache c;
  return c;
}

// Размытая маска скруглённого прямоугольника r (радиус R) в сетке mw×mh; xf — из координат r в сетку.
// Размытие тремя ящиками r3, за краем сетки — нули.
Mask blurredRoundRect(int mw, int mh, const RectF& r, float R, const Affine& xf, const int r3[3]) {
  Mask m(mw, mh);
  Path p;
  p.addRoundRect(r, R);
  rasterizeToMask(p, xf, m);
  std::vector<i32> plane(m.a.size());
  for (size_t i = 0; i < plane.size(); i++) plane[i] = i32(m.a[i]) << 8;
  blurPlaneZero(plane, mw, mh, r3);
  for (size_t i = 0; i < plane.size(); i++) m.a[i] = u8(std::clamp((plane[i] + 128) >> 8, 0, 255));
  return m;
}

// Маска тени: прямоугольник rw×rh (дробный) со скруглением R и полем pad вокруг.
Mask buildShadowMask(float rw, float rh, float R, const int r[3], int pad) {
  const int mw = int(std::ceil(rw)) + 2 * pad, mh = int(std::ceil(rh)) + 2 * pad;
  return blurredRoundRect(mw, mh, {float(pad), float(pad), rw, rh}, R, Affine{}, r);
}


}  // namespace

// ================================================================ Canvas
Canvas::Canvas(Image& target) : img_(&target), st_(std::make_shared<State>()) {
  st_->clip = target.empty() ? RectI{} : RectI{0, 0, target.w, target.h};
}

int Canvas::width() const { return img_->w; }
int Canvas::height() const { return img_->h; }

void Canvas::save() {
  stack_.push_back(st_);
  st_ = std::make_shared<State>(*st_);
}

void Canvas::restore() {
  if (stack_.empty()) return;
  st_ = stack_.back();
  stack_.pop_back();
}

void Canvas::translate(float x, float y) { st_->m = st_->m * Affine::translate(x, y); }
void Canvas::scale(float sx, float sy) { st_->m = st_->m * Affine::scale(sx, sy); }
void Canvas::concat(const Affine& m) { st_->m = st_->m * m; }
void Canvas::setTransform(const Affine& m) { st_->m = m; }
const Affine& Canvas::transform() const { return st_->m; }

void Canvas::clipRect(const RectF& r) {
  State& s = *st_;
  if (s.clip.empty()) return;
  const Affine& m = s.m;
  if (m.b == 0 && m.c == 0) {
    // Ось-ориентированный прямоугольник: отсечение по сетке пикселей (без сглаживания).
    double ax = double(m.a) * r.x + m.e, bx = double(m.a) * (double(r.x) + r.w) + m.e;
    double ay = double(m.d) * r.y + m.f, by = double(m.d) * (double(r.y) + r.h) + m.f;
    if (!(std::isfinite(ax) && std::isfinite(bx) && std::isfinite(ay) && std::isfinite(by))) { s.clip = {}; return; }
    if (ax > bx) std::swap(ax, bx);
    if (ay > by) std::swap(ay, by);
    auto snap = [](double v) { return std::clamp(std::floor(v + 0.5), -1e9, 1e9); };
    const double x0 = std::max(snap(ax), double(s.clip.x)), x1 = std::min(snap(bx), double(s.clip.right()));
    const double y0 = std::max(snap(ay), double(s.clip.y)), y1 = std::min(snap(by), double(s.clip.bottom()));
    if (x1 <= x0 || y1 <= y0) { s.clip = {}; return; }
    s.clip = {int(x0), int(y0), int(x1 - x0), int(y1 - y0)};
    return;
  }
  Path p;
  p.addRect(r);
  clipPath(p);
}

void Canvas::clipRoundRect(const RectF& r, float radius) {
  if (!(radius > 0)) { clipRect(r); return; }
  Path p;
  p.addRoundRect(r, radius);
  clipPath(p);
}

void Canvas::clipPath(const Path& p, FillRule rule) {
  State& s = *st_;
  if (s.clip.empty()) return;
  Scratch& S = scratch();
  S.ras.reset(s.clip);
  S.ras.addPath(p, s.m, kFillTolerance);
  const RectI b = S.ras.invalid() || S.ras.empty() ? RectI{} : S.ras.bounds();
  if (b.empty()) {
    s.clip = {};
    s.mask.reset();
    return;
  }
  auto mask = std::make_shared<Mask>(b.w, b.h);
  struct Sink final : SpanSink {
    Mask& m;
    RectI b;
    const Mask* old;
    RectI oldRect;
    Sink(Mask& m_, RectI b_, const Mask* o, RectI orc) : m(m_), b(b_), old(o), oldRect(orc) {}
    void row(int y, const Span* sp, int n) override {
      u8* d = m.row(y - b.y);
      const u8* o = old ? old->row(y - oldRect.y) : nullptr;
      for (int i = 0; i < n; i++) {
        u8* dd = d + (sp[i].x - b.x);
        if (sp[i].cov) std::memcpy(dd, sp[i].cov, size_t(sp[i].len));
        else std::memset(dd, sp[i].value, size_t(sp[i].len));
        if (o) {
          const u8* oo = o + (sp[i].x - oldRect.x);
          for (int k = 0; k < sp[i].len; k++) dd[k] = u8(px::mul8(dd[k], oo[k]));
        }
      }
    }
  } sink(*mask, b, s.mask.get(), s.maskRect);
  S.ras.sweep(rule, sink);
  s.mask = std::move(mask);
  s.maskRect = b;
  s.clip = b;
}

RectI Canvas::clipBounds() const { return st_->clip; }

bool Canvas::quickReject(const RectF& r) const {
  const State& s = *st_;
  if (s.clip.empty()) return true;
  const Affine& m = s.m;
  const Pt c[4] = {m.apply({r.x, r.y}), m.apply({r.x + r.w, r.y}), m.apply({r.x, r.y + r.h}), m.apply({r.x + r.w, r.y + r.h})};
  float x0 = c[0].x, x1 = c[0].x, y0 = c[0].y, y1 = c[0].y;
  for (const Pt& p : c) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return true;
    x0 = std::min(x0, p.x); x1 = std::max(x1, p.x);
    y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
  }
  return x1 <= float(s.clip.x) || x0 >= float(s.clip.right()) || y1 <= float(s.clip.y) || y0 >= float(s.clip.bottom());
}

void Canvas::setOpacity(float a) { st_->opacity *= std::isfinite(a) ? std::clamp(a, 0.f, 1.f) : 0.f; }
float Canvas::opacity() const { return st_->opacity; }
void Canvas::setBlend(Blend b) { st_->blend = b; }

void Canvas::clear(Color c) { img_->clear(premul(c)); }

void Canvas::fillPath(const Path& p, const Paint& paint, FillRule rule) { CanvasImpl::fill(*this, p, st_->m, paint, rule); }

void Canvas::strokePath(const Path& p, const Stroke& s, const Paint& paint) {
  CanvasImpl::stroke(*this, &p, nullptr, 0, false, s, paint);
}

void Canvas::fillRect(const RectF& r, const Paint& paint) {
  Path& p = scratch().shape;
  p.clear();
  p.addRect(r);
  fillPath(p, paint);
}

void Canvas::fillRoundRect(const RectF& r, float radius, const Paint& paint) {
  Path& p = scratch().shape;
  p.clear();
  p.addRoundRect(r, radius);
  fillPath(p, paint);
}

void Canvas::fillRoundRect(const RectF& r, float tl, float tr, float br, float bl, const Paint& paint) {
  Path& p = scratch().shape;
  p.clear();
  p.addRoundRect(r, tl, tr, br, bl);
  fillPath(p, paint);
}

void Canvas::strokeRoundRect(const RectF& r, float radius, float width, const Paint& paint) {
  Path& p = scratch().shape;
  p.clear();
  p.addRoundRect(r, radius);
  Stroke s;
  s.width = width;
  strokePath(p, s, paint);
}

void Canvas::fillCircle(float cx, float cy, float r, const Paint& paint) {
  Path& p = scratch().shape;
  p.clear();
  p.addCircle(cx, cy, r);
  fillPath(p, paint);
}

void Canvas::strokeCircle(float cx, float cy, float r, float width, const Paint& paint) {
  Path& p = scratch().shape;
  p.clear();
  p.addCircle(cx, cy, r);
  Stroke s;
  s.width = width;
  strokePath(p, s, paint);
}

void Canvas::line(float x0, float y0, float x1, float y1, float width, const Paint& paint, Cap cap) {
  const Pt pts[2] = {{x0, y0}, {x1, y1}};
  Stroke s;
  s.width = width;
  s.cap = cap;
  CanvasImpl::stroke(*this, nullptr, pts, 2, false, s, paint);
}

void Canvas::polyline(const Pt* pts, size_t n, bool closed, const Stroke& s, const Paint& paint) {
  if (!pts || n == 0) return;
  CanvasImpl::stroke(*this, nullptr, pts, n, closed, s, paint);
}

void Canvas::drawImage(const Image& img, const RectF& dst, float opacity, bool bilinear) {
  drawImage(img, RectF(0, 0, float(img.w), float(img.h)), dst, opacity, bilinear);
}

void Canvas::drawImage(const Image& img, const RectF& src, const RectF& dst, float opacity, bool bilinear) {
  if (img.empty() || !(src.w > 0) || !(src.h > 0) || dst.w == 0 || dst.h == 0) return;
  if (!std::isfinite(dst.x) || !std::isfinite(dst.y) || !std::isfinite(dst.w) || !std::isfinite(dst.h)) return;
  // Часть src вне изображения не рисуется; dst сокращается пропорционально.
  const float x0 = std::max(src.x, 0.f), y0 = std::max(src.y, 0.f);
  const float x1 = std::min(src.right(), float(img.w)), y1 = std::min(src.bottom(), float(img.h));
  if (!(x1 > x0) || !(y1 > y0)) return;
  const float kx = dst.w / src.w, ky = dst.h / src.h;
  const Affine imageToUser{kx, 0, 0, ky, dst.x - src.x * kx, dst.y - src.y * ky};
  const int ix0 = int(std::floor(x0)), iy0 = int(std::floor(y0));
  const RectI bounds{ix0, iy0, int(std::ceil(x1)) - ix0, int(std::ceil(y1)) - iy0};
  CanvasImpl::image(*this, img, RectF{x0, y0, x1 - x0, y1 - y0}, bounds, imageToUser, opacity, bilinear);
}

void Canvas::drawImageXf(const Image& img, const Affine& imageToUser, float opacity, bool bilinear) {
  if (img.empty()) return;
  CanvasImpl::image(*this, img, RectF{0, 0, float(img.w), float(img.h)}, RectI{0, 0, img.w, img.h}, imageToUser, opacity, bilinear);
}

void Canvas::fillMask(const Mask& m, float x, float y, const Paint& paint) {
  State& s = *st_;
  if (m.empty() || s.clip.empty()) return;
  const Pt o = s.m.apply({x, y});
  if (!std::isfinite(o.x) || !std::isfinite(o.y)) return;
  const int ox = int(std::floor(std::clamp(o.x, -1e8f, 1e8f) + 0.5f));
  const int oy = int(std::floor(std::clamp(o.y, -1e8f, 1e8f) + 0.5f));
  const RectI dst = RectI{ox, oy, m.w, m.h}.intersect(s.clip);
  if (dst.empty()) return;
  PaintSetup ps;
  if (!CanvasImpl::setupPaint(s, paint, dst, ps)) return;
  Scratch& S = scratch();
  Blitter b(*img_);
  CanvasImpl::initBlitter(b, s, S, CanvasImpl::blendOf(s, paint));
  b.solid = ps.solid;
  b.shader = ps.shader;
  b.alpha = ps.alpha;
  for (int yy = dst.y; yy < dst.bottom(); yy++) {
    const Span sp{dst.x, dst.w, m.row(yy - oy) + (dst.x - ox), 0};
    b.row(yy, &sp, 1);
  }
}

void Canvas::boxShadow(const RectF& r, float radius, float blur, float spread, Color c, Pt offset) {
  State& s = *st_;
  if (s.clip.empty() || c.a == 0 || !(s.opacity > 0)) return;
  if (!std::isfinite(spread)) spread = 0;
  if (!std::isfinite(blur) || blur < 0) blur = 0;
  if (!std::isfinite(radius) || radius < 0) radius = 0;
  const RectF rr{r.x + offset.x - spread, r.y + offset.y - spread, r.w + 2 * spread, r.h + 2 * spread};
  if (!(rr.w > 0) || !(rr.h > 0) || !std::isfinite(rr.x) || !std::isfinite(rr.y)) return;
  const float rad = radius > 0 ? std::max(0.f, radius + spread) : 0.f;
  const Affine& m = s.m;
  const float sc = m.scaleFactor();
  if (!(sc > 0) || !std::isfinite(sc)) return;
  const double sigmaDev = blur * sc * 0.5;
  if (sigmaDev < 0.3) {
    fillRoundRect(rr, rad, Paint(c));
    return;
  }
  int br[3];
  boxRadii(sigmaDev, br);
  const int pad = br[0] + br[1] + br[2];

  // Ось-ориентированный случай: маска в пикселях устройства, канонические плитки из кеша.
  if (m.isTranslateScale()) {
    double ax = double(m.a) * rr.x + m.e, bx = double(m.a) * (double(rr.x) + rr.w) + m.e;
    double ay = double(m.d) * rr.y + m.f, by = double(m.d) * (double(rr.y) + rr.h) + m.f;
    if (ax > bx) std::swap(ax, bx);
    if (ay > by) std::swap(ay, by);
    if (!(std::fabs(ax) < 1e8 && std::fabs(bx) < 1e8 && std::fabs(ay) < 1e8 && std::fabs(by) < 1e8)) return;
    const int X0 = int(std::floor(ax + 0.5)), X1 = int(std::floor(bx + 0.5));
    const int Y0 = int(std::floor(ay + 0.5)), Y1 = int(std::floor(by + 0.5));
    const int W = X1 - X0, H = Y1 - Y0;
    if (W <= 0 || H <= 0) return;
    // Видимость: тень целиком вне отсечения.
    if (RectI{X0 - pad, Y0 - pad, W + 2 * pad, H + 2 * pad}.intersect(s.clip).empty()) return;
    const float R = std::min({rad * sc, float(W) * 0.5f, float(H) * 0.5f});
    const int K = int(std::ceil(R)) + pad;
    const bool sx = W > 2 * K + 1, sy = H > 2 * K + 1;
    const int cw = sx ? 2 * K + 1 : W, ch = sy ? 2 * K + 1 : H;
    if (u64(cw + 2 * pad) * u64(ch + 2 * pad) <= (u64(16) << 20)) {
      const ShadowKey key{sx ? -1 : W, sy ? -1 : H, i32(std::lround(R * 4)), i32(std::lround(sigmaDev * 16))};
      auto tile = shadowCache().find(key);
      if (!tile) {
        auto t = std::make_shared<ShadowTile>();
        t->m = buildShadowMask(float(cw), float(ch), R, br, pad);
        t->pad = pad;
        t->k = K;
        t->w = cw;
        t->h = ch;
        t->sx = sx;
        t->sy = sy;
        tile = t;
        shadowCache().put(key, t);
      }
      const Mask& tm = tile->m;
      Scratch& S = scratch();
      Blitter b(*img_);
      CanvasImpl::initBlitter(b, s, S, s.blend);
      b.solid = premul(c.alpha(std::min(1.f, s.opacity)));
      if (b.solid == 0) return;
      const int ox = X0 - pad, oy = Y0 - pad, DW = W + 2 * pad, DH = H + 2 * pad;
      auto mapIdx = [](int j, bool stretch, int k, int pd, int full, int canon) {
        if (!stretch) return j;
        if (j < pd + k) return j;
        if (j < full - pd - k) return pd + k;
        return j - (full - canon);
      };
      const RectI vis = RectI{ox, oy, DW, DH}.intersect(s.clip);
      Span spans[3];
      for (int y = vis.y; y < vis.bottom(); y++) {
        const int jy = mapIdx(y - oy, sy, K, pad, DH, tm.h);
        const u8* mrow = tm.row(jy);
        int n = 0;
        // Отрезки: левая плитка, средний столбец (постоянное значение), правая плитка.
        auto addSpan = [&](int dx0, int dx1, const u8* cov, u8 val) {
          int a = std::max(dx0 + ox, vis.x), e = std::min(dx1 + ox, vis.right());
          if (e <= a) return;
          spans[n++] = Span{a, e - a, cov ? cov + (a - ox - dx0) : nullptr, val};
        };
        if (!sx) {
          addSpan(0, DW, mrow, 0);
        } else {
          const int left = pad + K, right = DW - pad - K;
          addSpan(0, left, mrow, 0);
          if (mrow[left]) addSpan(left, right, nullptr, mrow[left]);
          addSpan(right, DW, mrow + (right - (DW - tm.w)) , 0);
        }
        if (n) b.row(y, spans, n);
      }
      return;
    }
  }

  // Общий случай (поворот, наклон или огромная тень): маска только для видимой части с полем размытия,
  // в собственной сетке (масштаб устройства), рисуется как изображение. Поле за краем видимой области
  // поглощает краевой эффект нулевого фона, поэтому видимая часть совпадает с полной тенью.
  RectF vis;
  if (!CanvasImpl::userClip(s, vis)) return;
  const double padU = double(pad) / sc;
  const double x0 = std::max(double(rr.x), double(vis.x)) - padU, y0 = std::max(double(rr.y), double(vis.y)) - padU;
  const double x1 = std::min(double(rr.right()), double(vis.right())) + padU;
  const double y1 = std::min(double(rr.bottom()), double(vis.bottom())) + padU;
  if (!(x1 > x0) || !(y1 > y0)) return;
  // Сильно размытая тень не содержит мелких деталей: шаг сетки до σ/3 пикселя устройства (затем билинейно).
  double res = sigmaDev > 6 ? sc * 3.0 / sigmaDev : double(sc);
  const double area = (x1 - x0) * (y1 - y0) * res * res;
  if (area > double(4 << 20)) res *= std::sqrt(double(4 << 20) / area);
  int r2[3] = {br[0], br[1], br[2]};
  if (res != double(sc)) boxRadii(blur * res * 0.5, r2);
  const int mw = int(std::ceil((x1 - x0) * res)), mh = int(std::ceil((y1 - y0) * res));
  if (mw <= 0 || mh <= 0 || mw > (1 << 14) || mh > (1 << 14)) return;
  const float fr = float(res);
  const Mask mk = blurredRoundRect(mw, mh, rr, std::min({rad, rr.w * 0.5f, rr.h * 0.5f}),
                                   Affine{fr, 0, 0, fr, float(-x0 * res), float(-y0 * res)}, r2);
  Image im(mk.w, mk.h);
  const u32 pc = premul(c);
  for (size_t i = 0; i < mk.a.size(); i++) im.px[i] = px::mul(pc, mk.a[i]);
  const float inv = float(1.0 / res);
  drawImageXf(im, Affine{inv, 0, 0, inv, float(x0), float(y0)}, 1, true);
}

void Canvas::blurRegion(const RectI& region, float radius) {
  const RectI r = region.intersect(st_->clip);
  if (r.empty() || !(radius > 0) || !std::isfinite(radius)) return;
  // Сильное размытие считается в уменьшенной сетке (как фоновое размытие в браузерах): без видимой разницы, в 4–16 раз быстрее.
  // Сигма больше размера области даёт почти ровное среднее — ограничение бережёт время.
  const double sigma = std::min(double(radius) * 0.5, double(std::max(r.w, r.h)));
  const int f = sigma >= 12 ? 4 : sigma >= 5 ? 2 : 1;
  int br[3];
  boxRadii(sigma / f, br);
  if (f == 1 && br[0] + br[1] + br[2] == 0) return;
  const int w = r.w, h = r.h, sw = (w + f - 1) / f, sh = (h + f - 1) / f;
  const size_t rowLen = size_t(sw) * 4;
  std::vector<u32> a(rowLen * size_t(sh)), b(a.size());
  // Уменьшение: среднее блока f×f (каналы ×256).
  for (int sy = 0; sy < sh; sy++) {
    u32* dst = a.data() + size_t(sy) * rowLen;
    const int y0 = sy * f, y1 = std::min(h, y0 + f);
    for (int y = y0; y < y1; y++) {
      const u32* src = img_->row(r.y + y) + r.x;
      for (int x = 0; x < w; x++) {
        const u32 p = src[x];
        u32* o = dst + size_t(x / f) * 4;
        o[0] += p >> 24;
        o[1] += (p >> 16) & 255;
        o[2] += (p >> 8) & 255;
        o[3] += p & 255;
      }
    }
    for (int sx = 0; sx < sw; sx++) {
      const u32 cnt = u32((std::min(w, sx * f + f) - sx * f) * (y1 - y0));
      for (int c = 0; c < 4; c++) dst[size_t(sx) * 4 + size_t(c)] = (dst[size_t(sx) * 4 + size_t(c)] * 256 + cnt / 2) / cnt;
    }
  }
  // Три ящика по строкам, затем по столбцам.
  std::vector<u32> l1(rowLen), l2(rowLen);
  for (int y = 0; y < sh; y++) {
    u32* row = a.data() + size_t(y) * rowLen;
    boxRow4(row, l1.data(), sw, br[0]);
    boxRow4(l1.data(), l2.data(), sw, br[1]);
    boxRow4(l2.data(), row, sw, br[2]);
  }
  std::vector<u64> acc;
  boxCols(a.data(), b.data(), rowLen, sh, br[0], acc);
  boxCols(b.data(), a.data(), rowLen, sh, br[1], acc);
  boxCols(a.data(), b.data(), rowLen, sh, br[2], acc);
  const u32* res = b.data();
  auto pack = [](u32 a8, u32 r8, u32 g8, u32 b8) {
    a8 = std::min<u32>(a8, 255);
    return (a8 << 24) | (std::min(r8, a8) << 16) | (std::min(g8, a8) << 8) | std::min(b8, a8);
  };
  if (f == 1) {
    for (int y = 0; y < h; y++) {
      u32* d = img_->row(r.y + y) + r.x;
      const u32* s = res + size_t(y) * rowLen;
      for (int x = 0; x < w; x++, s += 4) d[x] = pack((s[0] + 128) >> 8, (s[1] + 128) >> 8, (s[2] + 128) >> 8, (s[3] + 128) >> 8);
    }
    return;
  }
  // Увеличение билинейно (таблицы столбцов; веса 0..256, значения 8.8).
  const size_t wn = size_t(w);
  std::vector<i32> cx0(wn), cx1(wn);
  std::vector<u32> cfx(wn);
  auto axis = [f](int i, int n, i32& i0, i32& i1, u32& fr) {
    const double s = (i + 0.5) / f - 0.5;
    const double fl = std::floor(s);
    if (fl < 0) { i0 = i1 = 0; fr = 0; return; }
    if (fl >= n - 1) { i0 = i1 = n - 1; fr = 0; return; }
    i0 = i32(fl);
    i1 = i0 + 1;
    fr = u32((s - fl) * 256 + 0.5);
  };
  for (int x = 0; x < w; x++) axis(x, sw, cx0[size_t(x)], cx1[size_t(x)], cfx[size_t(x)]);
  for (int y = 0; y < h; y++) {
    i32 y0, y1;
    u32 fy;
    axis(y, sh, y0, y1, fy);
    const u32* r0 = res + size_t(y0) * rowLen;
    const u32* r1 = res + size_t(y1) * rowLen;
    u32* d = img_->row(r.y + y) + r.x;
    for (int x = 0; x < w; x++) {
      const u32* p00 = r0 + size_t(cx0[size_t(x)]) * 4;
      const u32* p10 = r0 + size_t(cx1[size_t(x)]) * 4;
      const u32* p01 = r1 + size_t(cx0[size_t(x)]) * 4;
      const u32* p11 = r1 + size_t(cx1[size_t(x)]) * 4;
      const u32 fx = cfx[size_t(x)], ifx = 256 - fx, ify = 256 - fy;
      u32 v[4];
      for (int c = 0; c < 4; c++) {
        const u32 top = (p00[c] * ifx + p10[c] * fx) >> 8, bot = (p01[c] * ifx + p11[c] * fx) >> 8;
        v[c] = (((top * ify + bot * fy) >> 8) + 128) >> 8;
      }
      d[x] = pack(v[0], v[1], v[2], v[3]);
    }
  }
}

}  // namespace rg::gfx
