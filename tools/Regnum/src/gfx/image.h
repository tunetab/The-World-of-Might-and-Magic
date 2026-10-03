// Regnum — растровые изображения.
// Image: premultiplied BGRA в u32 (0xAARRGGBB на little-endian: байты B, G, R, A).
// Mask: 8-битное покрытие (глифы, маски отсечения).
#pragma once
#include "base/base.h"

namespace rg::gfx {

struct RectI {
  int x = 0, y = 0, w = 0, h = 0;
  constexpr RectI() = default;
  constexpr RectI(int x_, int y_, int w_, int h_) : x(x_), y(y_), w(w_), h(h_) {}
  constexpr int right() const { return x + w; }
  constexpr int bottom() const { return y + h; }
  constexpr bool empty() const { return w <= 0 || h <= 0; }
  RectI intersect(const RectI& o) const {
    int ax = std::max(x, o.x), ay = std::max(y, o.y), bx = std::min(right(), o.right()), by = std::min(bottom(), o.bottom());
    return {ax, ay, std::max(0, bx - ax), std::max(0, by - ay)};
  }
  RectI unite(const RectI& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    int ax = std::min(x, o.x), ay = std::min(y, o.y), bx = std::max(right(), o.right()), by = std::max(bottom(), o.bottom());
    return {ax, ay, bx - ax, by - ay};
  }
  constexpr bool operator==(const RectI&) const = default;
};

// Упаковка premultiplied-пикселя.
constexpr u32 packPremul(u8 r, u8 g, u8 b, u8 a) { return (u32(a) << 24) | (u32(r) << 16) | (u32(g) << 8) | u32(b); }
inline u32 premul(Color c) {
  u32 a = c.a;
  auto m = [a](u8 v) { return u8((u32(v) * a + 127) / 255); };
  return packPremul(m(c.r), m(c.g), m(c.b), c.a);
}
inline Color unpremul(u32 p) {
  u8 a = u8(p >> 24);
  if (a == 0) return Color(0, 0, 0, 0);
  auto d = [a](u32 v) { return u8(std::min<u32>(255, (v * 255 + a / 2) / a)); };
  return Color(d((p >> 16) & 255), d((p >> 8) & 255), d(p & 255), a);
}

struct Image {
  int w = 0, h = 0;
  std::vector<u32> px;

  Image() = default;
  Image(int w_, int h_, u32 fill = 0) : w(w_), h(h_), px(size_t(std::max(0, w_)) * size_t(std::max(0, h_)), fill) {}
  bool empty() const { return w <= 0 || h <= 0; }
  u32* row(int y) { return px.data() + size_t(y) * size_t(w); }
  const u32* row(int y) const { return px.data() + size_t(y) * size_t(w); }
  u32 at(int x, int y) const { return px[size_t(y) * size_t(w) + size_t(x)]; }
  void clear(u32 v = 0) { std::fill(px.begin(), px.end(), v); }
  void resize(int nw, int nh) { w = nw; h = nh; px.assign(size_t(std::max(0, nw)) * size_t(std::max(0, nh)), 0); }

  // Из обычного (не premultiplied) RGBA8 и обратно — для кодеков PNG/JPEG.
  static Image fromRgba(const u8* rgba, int w, int h);
  std::vector<u8> toRgba() const;
  // Масштабирование: высокое качество (область/бикубика) для уменьшения превью и пирамид.
  Image scaled(int nw, int nh) const;
  Image cropped(RectI r) const;
};

struct Mask {
  int w = 0, h = 0;
  std::vector<u8> a;
  Mask() = default;
  Mask(int w_, int h_) : w(w_), h(h_), a(size_t(std::max(0, w_)) * size_t(std::max(0, h_)), 0) {}
  bool empty() const { return w <= 0 || h <= 0; }
  u8* row(int y) { return a.data() + size_t(y) * size_t(w); }
  const u8* row(int y) const { return a.data() + size_t(y) * size_t(w); }
};

}  // namespace rg::gfx
