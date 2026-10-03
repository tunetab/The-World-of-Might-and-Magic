// Regnum — растровые изображения: преобразование RGBA, высококачественное масштабирование, вырезание.
//
// Масштабирование раздельное (сначала строки, затем столбцы) в premultiplied-пространстве:
// уменьшение — точное усреднение по площади, увеличение — бикубический фильтр Катмулла — Рома.
// Промежуточные строки хранятся в кольцевом буфере, поэтому память — O(ширина × число отсчётов).
#include "gfx/image.h"

namespace rg::gfx {

namespace {

struct Taps {
  std::vector<i32> start, count, idx;
  std::vector<float> w;
  int maxCount = 0;
};

float catmullRom(float x) {
  x = std::fabs(x);
  if (x < 1) return 1.5f * x * x * x - 2.5f * x * x + 1;
  if (x < 2) return -0.5f * x * x * x + 2.5f * x * x - 4 * x + 2;
  return 0;
}

// Отсчёты для оси src -> dst.
Taps makeTaps(int src, int dst) {
  Taps t;
  t.start.resize(size_t(dst));
  t.count.resize(size_t(dst));
  const double s = double(src) / double(dst);
  for (int i = 0; i < dst; i++) {
    t.start[size_t(i)] = i32(t.idx.size());
    if (src == dst) {
      t.idx.push_back(i);
      t.w.push_back(1);
    } else if (dst < src) {
      // Усреднение по площади: выходной пиксель покрывает [i·s, (i+1)·s).
      const double a = i * s, b = std::min(double(src), (i + 1) * s);
      for (int k = int(std::floor(a)); k < int(std::ceil(b)); k++) {
        double o = std::min(b, double(k) + 1) - std::max(a, double(k));
        if (o > 1e-12) { t.idx.push_back(std::min(k, src - 1)); t.w.push_back(float(o / s)); }
      }
    } else {
      const double c = (i + 0.5) * s - 0.5;
      const int f = int(std::floor(c));
      float sum = 0;
      const size_t base = t.idx.size();
      for (int k = f - 1; k <= f + 2; k++) {
        const float wk = catmullRom(float(c - k));
        if (wk == 0) continue;
        const i32 kk = std::clamp(k, 0, src - 1);
        bool merged = false;
        for (size_t j = base; j < t.idx.size(); j++)
          if (t.idx[j] == kk) { t.w[j] += wk; merged = true; break; }
        if (!merged) { t.idx.push_back(kk); t.w.push_back(wk); }
        sum += wk;
      }
      if (sum != 0)
        for (size_t j = base; j < t.idx.size(); j++) t.w[j] /= sum;
    }
    t.count[size_t(i)] = i32(t.idx.size()) - t.start[size_t(i)];
    t.maxCount = std::max(t.maxCount, int(t.count[size_t(i)]));
  }
  return t;
}

}  // namespace

Image Image::fromRgba(const u8* rgba, int w, int h) {
  if (!rgba || w <= 0 || h <= 0) return {};
  Image img(w, h);
  const size_t n = size_t(w) * size_t(h);
  for (size_t i = 0; i < n; i++) {
    const u8* p = rgba + i * 4;
    img.px[i] = premul(Color(p[0], p[1], p[2], p[3]));
  }
  return img;
}

std::vector<u8> Image::toRgba() const {
  std::vector<u8> out;
  if (empty()) return out;
  const size_t n = size_t(w) * size_t(h);
  out.resize(n * 4);
  for (size_t i = 0; i < n; i++) {
    Color c = unpremul(px[i]);
    u8* o = out.data() + i * 4;
    o[0] = c.r; o[1] = c.g; o[2] = c.b; o[3] = c.a;
  }
  return out;
}

Image Image::scaled(int nw, int nh) const {
  if (empty() || nw <= 0 || nh <= 0) return {};
  if (nw == w && nh == h) return *this;
  const Taps tx = makeTaps(w, nw), ty = makeTaps(h, nh);
  Image out(nw, nh);
  // Кольцо строк, уже масштабированных по горизонтали (4 канала float).
  const int ring = ty.maxCount + 1;
  std::vector<float> rows(size_t(ring) * size_t(nw) * 4);
  std::vector<i32> tag(size_t(ring), -1);
  auto hrow = [&](int sy) -> const float* {
    const size_t slot = size_t(sy % ring);
    float* r = rows.data() + slot * size_t(nw) * 4;
    if (tag[slot] == sy) return r;
    tag[slot] = sy;
    const u32* src = row(sy);
    for (int x = 0; x < nw; x++) {
      float a = 0, cr = 0, cg = 0, cb = 0;
      const i32 s0 = tx.start[size_t(x)], c = tx.count[size_t(x)];
      for (i32 k = 0; k < c; k++) {
        const u32 p = src[tx.idx[size_t(s0 + k)]];
        const float wk = tx.w[size_t(s0 + k)];
        a += wk * float(p >> 24);
        cr += wk * float((p >> 16) & 255);
        cg += wk * float((p >> 8) & 255);
        cb += wk * float(p & 255);
      }
      float* o = r + size_t(x) * 4;
      o[0] = a; o[1] = cr; o[2] = cg; o[3] = cb;
    }
    return r;
  };
  std::vector<const float*> src(size_t(ty.maxCount));
  for (int y = 0; y < nh; y++) {
    const i32 s0 = ty.start[size_t(y)], c = ty.count[size_t(y)];
    for (i32 k = 0; k < c; k++) src[size_t(k)] = hrow(ty.idx[size_t(s0 + k)]);
    u32* d = out.row(y);
    for (int x = 0; x < nw; x++) {
      float a = 0, cr = 0, cg = 0, cb = 0;
      for (i32 k = 0; k < c; k++) {
        const float wk = ty.w[size_t(s0 + k)];
        const float* v = src[size_t(k)] + size_t(x) * 4;
        a += wk * v[0]; cr += wk * v[1]; cg += wk * v[2]; cb += wk * v[3];
      }
      auto q = [](float v) { return u32(std::clamp(v + 0.5f, 0.f, 255.f)); };
      const u32 qa = q(a);
      d[x] = (qa << 24) | (std::min(q(cr), qa) << 16) | (std::min(q(cg), qa) << 8) | std::min(q(cb), qa);
    }
  }
  return out;
}

Image Image::cropped(RectI r) const {
  r = r.intersect({0, 0, w, h});
  if (r.empty()) return {};
  Image out(r.w, r.h);
  for (int y = 0; y < r.h; y++) std::memcpy(out.row(y), row(r.y + y) + r.x, size_t(r.w) * sizeof(u32));
  return out;
}

}  // namespace rg::gfx
