// Regnum — флаги: геометрия узоров, размещение эмблемы, светотень ткани, рамка, кеш изображений флагов.
#include "gfx/flag.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "codec/jpeg.h"
#include "core/world.h"
#include "gfx/emblems.h"

namespace rg::gfx {

namespace {

// Привязка координат к пикселям устройства (преобразование без поворота и с равномерным масштабом).
struct Snap {
  bool on = false;
  Affine m;
  float x(float v) const { return on ? (std::round(m.a * v + m.e) - m.e) / m.a : v; }
  float y(float v) const { return on ? (std::round(m.d * v + m.f) - m.f) / m.d : v; }
};

Snap snapFor(const Canvas& c) {
  Snap s;
  s.m = c.transform();
  s.on = s.m.b == 0 && s.m.c == 0 && s.m.a > 0 && s.m.d > 0;
  return s;
}

void poly(Canvas& c, std::initializer_list<Pt> pts, Color col) {
  Path p;
  bool first = true;
  for (Pt q : pts) {
    if (first) p.moveTo(q.x, q.y);
    else p.lineTo(q.x, q.y);
    first = false;
  }
  p.close();
  c.fillPath(p, Paint(col));
}

void rect(Canvas& c, float x0, float y0, float x1, float y1, Color col) {
  if (x1 > x0 && y1 > y0) c.fillRect({x0, y0, x1 - x0, y1 - y0}, Paint(col));
}

// Полоса шириной t вдоль отрезка a→b (концы продлены за флаг, лишнее отсекается).
void band(Canvas& c, Pt a, Pt b, float t, Color col) {
  const float dx = b.x - a.x, dy = b.y - a.y;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (!(len > 0)) return;
  const Pt d{dx / len, dy / len}, n{-dy / len * t * 0.5f, dx / len * t * 0.5f};
  const Pt a2 = a - d * t, b2 = b + d * t;
  poly(c, {a2 + n, b2 + n, b2 - n, a2 - n}, col);
}

double contrast(Color a, Color b) {
  const double la = a.luminance(), lb = b.luminance();
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// Узор: рисует поле и возвращает место эмблемы (квадрат), цвета узора под ней и все цвета узора.
struct EmblemPlace {
  float cx = 0, cy = 0, size = 0;
  std::vector<Color> under, used;
};

EmblemPlace paintPattern(Canvas& c, const Flag& f, RectF r, const Snap& s) {
  const Color c0 = f.colors[0].withA(255), c1 = f.colors[1].withA(255), c2 = f.colors[2].withA(255);
  const float x = r.x, y = r.y, w = r.w, h = r.h, X1 = r.right(), Y1 = r.bottom();
  EmblemPlace e{r.cx(), r.cy(), std::min(h * 0.6f, w * 0.8f), {c0}, {c0}};
  const bool inner = !(c2 == c1);
  switch (f.pattern) {
    case FlagPattern::Solid:
    case FlagPattern::Count:
      rect(c, x, y, X1, Y1, c0);
      e.size = std::min(h * 0.64f, w * 0.8f);
      break;
    case FlagPattern::H2: {
      const float m = s.y(y + h * 0.5f);
      rect(c, x, y, X1, m, c0);
      rect(c, x, m, X1, Y1, c1);
      e.under = e.used = {c0, c1};
      break;
    }
    case FlagPattern::H3: {
      const float a = s.y(y + h / 3), b = s.y(y + h * 2 / 3);
      rect(c, x, y, X1, a, c0);
      rect(c, x, a, X1, b, c1);
      rect(c, x, b, X1, Y1, c2);
      e.size = std::min(h * 0.56f, w * 0.8f);
      e.under = e.used = {c0, c1, c2};
      break;
    }
    case FlagPattern::V2: {
      const float m = s.x(x + w * 0.5f);
      rect(c, x, y, m, Y1, c0);
      rect(c, m, y, X1, Y1, c1);
      e.under = e.used = {c0, c1};
      break;
    }
    case FlagPattern::V3: {
      const float a = s.x(x + w / 3), b = s.x(x + w * 2 / 3);
      rect(c, x, y, a, Y1, c0);
      rect(c, a, y, b, Y1, c1);
      rect(c, b, y, X1, Y1, c2);
      e.size = std::min(h * 0.56f, w * 0.36f);
      e.used = {c0, c1, c2};
      e.under = e.size <= b - a ? std::vector<Color>{c1} : e.used;
      break;
    }
    case FlagPattern::Cross: {
      // Скандинавский крест: вертикаль сдвинута к древку (как у датского флага).
      const float t = h * 0.2f, ti = h * 0.09f;
      const float vx = x + w * 0.37f, hy = y + h * 0.5f;
      rect(c, x, y, X1, Y1, c0);
      rect(c, s.x(vx - t / 2), y, s.x(vx + t / 2), Y1, c1);
      rect(c, x, s.y(hy - t / 2), X1, s.y(hy + t / 2), c1);
      if (inner) {
        rect(c, s.x(vx - ti / 2), y, s.x(vx + ti / 2), Y1, c2);
        rect(c, x, s.y(hy - ti / 2), X1, s.y(hy + ti / 2), c2);
      }
      // Эмблема — в верхнем поле у древка.
      const float bw = (vx - t / 2) - x, bh = (hy - t / 2) - y;
      e = {x + bw * 0.5f, y + bh * 0.5f, std::min(bw, bh) * 0.8f, {c0}, {c0, c1}};
      if (inner) e.used.push_back(c2);
      break;
    }
    case FlagPattern::Saltire: {
      rect(c, x, y, X1, Y1, c0);
      const float t = h * 0.2f, ti = h * 0.08f;
      band(c, {x, y}, {X1, Y1}, t, c1);
      band(c, {X1, y}, {x, Y1}, t, c1);
      if (inner) {
        band(c, {x, y}, {X1, Y1}, ti, c2);
        band(c, {X1, y}, {x, Y1}, ti, c2);
      }
      e.size = std::min(h * 0.5f, w * 0.8f);
      e.under = e.used = {c0, c1};
      if (inner) e.under.push_back(c2), e.used.push_back(c2);
      break;
    }
    case FlagPattern::Quarters: {
      const float mx = s.x(x + w * 0.5f), my = s.y(y + h * 0.5f);
      rect(c, x, y, X1, Y1, c0);
      rect(c, mx, y, X1, my, c1);
      rect(c, x, my, mx, Y1, c1);
      e.size = std::min(h * 0.56f, w * 0.8f);
      e.under = e.used = {c0, c1};
      break;
    }
    case FlagPattern::Bend: {
      rect(c, x, y, X1, Y1, c0);
      poly(c, {{x, y}, {X1, Y1}, {x, Y1}}, c2);
      band(c, {x, y}, {X1, Y1}, h * 0.26f, c1);
      e.size = std::min(h * 0.5f, w * 0.8f);
      e.under = e.used = {c0, c1, c2};
      break;
    }
    case FlagPattern::Chevron: {
      // Стропило: вершина вверху по центру, ветви уходят к нижним углам; под стропилом — c2.
      const float t = h * 0.22f;
      const Pt apex{x + w * 0.5f, y + h * 0.14f};
      rect(c, x, y, X1, Y1, c0);
      poly(c, {{x, Y1}, apex, {X1, Y1}}, c1);
      poly(c, {{x, Y1 + t}, {apex.x, apex.y + t}, {X1, Y1 + t}}, c2);
      // Эмблема — в треугольнике под стропилом, ближе к основанию (там он шире).
      const float top = apex.y + t;
      const float side = std::min((Y1 - top) * 0.6f, w * 0.3f);
      e = {apex.x, top + (Y1 - top) * 0.6f, side, {c2}, {c0, c1, c2}};
      break;
    }
    case FlagPattern::Border: {
      const float b = std::max(h * 0.12f, 1.f / std::max(1e-3f, s.on ? s.m.a : 1.f));
      rect(c, x, y, X1, Y1, c1);
      rect(c, s.x(x + b), s.y(y + b), s.x(X1 - b), s.y(Y1 - b), c0);
      e.size = std::min(h * 0.52f, w * 0.7f);
      e.used = {c0, c1};
      break;
    }
    case FlagPattern::Canton: {
      const float cx1 = s.x(x + w * 0.42f), cy1 = s.y(y + h * 0.5f);
      rect(c, x, y, X1, Y1, c0);
      rect(c, x, y, cx1, cy1, c1);
      e = {(x + cx1) * 0.5f, (y + cy1) * 0.5f, std::min(cx1 - x, cy1 - y) * 0.78f, {c1}, {c0, c1}};
      break;
    }
    case FlagPattern::Chief: {
      const float cy1 = s.y(y + h * 0.3f);
      rect(c, x, y, X1, Y1, c0);
      rect(c, x, y, X1, cy1, c1);
      e = {r.cx(), (cy1 + Y1) * 0.5f, std::min((Y1 - cy1) * 0.76f, w * 0.8f), {c0}, {c0, c1}};
      break;
    }
    case FlagPattern::Pale: {
      const float a = s.x(x + w / 3), b = s.x(x + w * 2 / 3);
      rect(c, x, y, X1, Y1, c0);
      rect(c, a, y, b, Y1, c1);
      e = {(a + b) * 0.5f, r.cy(), std::min(h * 0.56f, (b - a) * 0.86f), {c1}, {c0, c1}};
      break;
    }
  }
  return e;
}

// Эмблема с лёгкой тенью; при слабом контрасте с полем — обводка цветом узора.
void paintEmblem(Canvas& c, const Flag& f, const EmblemPlace& e) {
  if (f.emblem.empty() || !(e.size > 0) || !hasEmblem(f.emblem)) return;
  const Color col = f.emblemColor.withA(255);
  const RectF box{e.cx - e.size * 0.5f, e.cy - e.size * 0.5f, e.size, e.size};
  double minC = 100;
  for (const Color& k : e.under) minC = std::min(minC, contrast(col, k));
  if (minC < 1.6) {
    // Обводка: эмблема, сдвинутая по кругу, цветом узора с наибольшим контрастом (или тёмным/светлым).
    Color halo = col.luminance() > 0.4f ? Color::hex(0x141820) : Color::hex(0xf4efe4);
    double best = 2.2;
    for (const Color& k : e.used)
      if (contrast(k, col) > best) best = contrast(k, col), halo = k;
    const float d = std::max(e.size * 0.035f, 0.6f / std::max(1e-3f, c.transform().scaleFactor()));
    for (int i = 0; i < 8; i++) {
      const float a = float(i) * float(kPi) / 4;
      drawEmblem(c, f.emblem, {box.x + d * std::cos(a), box.y + d * std::sin(a), box.w, box.h}, halo);
    }
  } else {
    drawEmblem(c, f.emblem, {box.x, box.y + e.size * 0.025f, box.w, box.h}, Color(0, 0, 0, 56));
  }
  drawEmblem(c, f.emblem, box, col);
}

// Светотень ткани: мягкие диагональные складки и затенение к низу.
void paintCloth(Canvas& c, RectF r) {
  Gradient folds;
  folds.kind = Gradient::Linear;
  folds.p0 = {r.x, r.y};
  folds.p1 = {r.right(), r.y + r.h * 0.35f};
  folds.stops = {{0.f, Color(255, 255, 255, 30)}, {0.2f, Color(255, 255, 255, 0)}, {0.4f, Color(0, 0, 0, 20)},
                 {0.62f, Color(255, 255, 255, 18)}, {0.82f, Color(0, 0, 0, 16)}, {1.f, Color(255, 255, 255, 10)}};
  Paint pf;
  pf.gradient = &folds;
  c.fillRect(r, pf);
  Gradient shade;
  shade.kind = Gradient::Linear;
  shade.p0 = {r.x, r.y};
  shade.p1 = {r.x, r.bottom()};
  shade.stops = {{0.f, Color(255, 255, 255, 20)}, {0.45f, Color(255, 255, 255, 0)}, {1.f, Color(0, 0, 0, 26)}};
  Paint ps;
  ps.gradient = &shade;
  c.fillRect(r, ps);
}

// ---------------------------------------------------------------- изображения флагов
struct ScaledKey {
  u64 hash;
  i32 w, h;
  bool operator==(const ScaledKey&) const = default;
};
struct ScaledKeyHash {
  size_t operator()(const ScaledKey& k) const { return size_t(hashMix(k.hash, (u64(u32(k.w)) << 32) | u32(k.h))); }
};

constexpr size_t kMaxDecoded = 48;
constexpr size_t kMaxScaled = 256;

class ImageCache {
 public:
  // Декодированное изображение (nullptr — данные повреждены; ошибка пишется в журнал один раз).
  std::shared_ptr<const Image> decoded(u64 hash, const std::string& bytes) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = decoded_.find(hash);
      if (it != decoded_.end()) return it->second;
    }
    std::string err;
    std::shared_ptr<const Image> img;
    if (auto rgba = codec::decodeImage(bytes, &err); rgba && !rgba->empty())
      img = std::make_shared<const Image>(Image::fromRgba(rgba->rgba.data(), rgba->w, rgba->h));
    std::lock_guard<std::mutex> lock(mu_);
    if (!img && failed_.insert(hash).second) logWarn("Изображение флага не читается: %s", err.c_str());
    if (decoded_.size() >= kMaxDecoded) decoded_.clear();
    decoded_[hash] = img;
    return img;
  }
  // Кадрирование «cover» и масштаб под размер устройства.
  std::shared_ptr<const Image> scaled(u64 hash, const Image& src, int w, int h) {
    const ScaledKey key{hash, w, h};
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = scaled_.find(key);
      if (it != scaled_.end()) return it->second;
    }
    const double k = std::max(double(w) / src.w, double(h) / src.h);
    const int cw = std::clamp(int(std::lround(w / k)), 1, src.w), ch = std::clamp(int(std::lround(h / k)), 1, src.h);
    const Image crop = src.cropped(RectI{(src.w - cw) / 2, (src.h - ch) / 2, cw, ch});
    auto out = std::make_shared<const Image>(crop.scaled(w, h));
    std::lock_guard<std::mutex> lock(mu_);
    if (scaled_.size() >= kMaxScaled) scaled_.clear();
    scaled_[key] = out;
    return out;
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mu_);
    decoded_.clear();
    scaled_.clear();
  }
  size_t size() {
    std::lock_guard<std::mutex> lock(mu_);
    return decoded_.size() + scaled_.size();
  }

 private:
  std::mutex mu_;
  std::unordered_map<u64, std::shared_ptr<const Image>> decoded_;
  std::unordered_map<ScaledKey, std::shared_ptr<const Image>, ScaledKeyHash> scaled_;
  std::unordered_set<u64> failed_;
};

ImageCache& imageCache() {
  static ImageCache c;
  return c;
}

// Изображение флага: true — нарисовано.
bool paintImage(Canvas& c, const Flag& f, RectF r, const Snap& s) {
  if (f.png.empty()) return false;
  const u64 hash = hash64(f.png);
  auto img = imageCache().decoded(hash, f.png);
  if (!img || img->empty()) return false;
  if (s.on) {
    const int W = int(std::lround(r.w * s.m.a)), H = int(std::lround(r.h * s.m.d));
    if (W >= 1 && H >= 1 && W <= 4096 && H <= 4096) {
      auto sc = imageCache().scaled(hash, *img, W, H);
      c.drawImage(*sc, r, 1, true);
      return true;
    }
  }
  const double k = std::max(double(r.w) / img->w, double(r.h) / img->h);
  const float sw = float(r.w / k), sh = float(r.h / k);
  c.drawImage(*img, RectF{(img->w - sw) * 0.5f, (img->h - sh) * 0.5f, sw, sh}, r, 1, true);
  return true;
}

}  // namespace

void drawFlag(Canvas& c, const Flag& flag, RectF r, float radius, bool frame) {
  if (!(r.w > 0) || !(r.h > 0) || !std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.w) || !std::isfinite(r.h)) return;
  if (c.quickReject(r)) return;
  const Snap s = snapFor(c);
  if (s.on) {
    // Края флага — по границам пикселей.
    const float x0 = s.x(r.x), y0 = s.y(r.y), x1 = s.x(r.right()), y1 = s.y(r.bottom());
    if (!(x1 > x0) || !(y1 > y0)) return;
    r = {x0, y0, x1 - x0, y1 - y0};
  }
  if (!std::isfinite(radius) || radius < 0) radius = 0;
  radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
  c.save();
  if (radius > 0) c.clipRoundRect(r, radius);
  else c.clipRect(r);
  if (!(flag.image && paintImage(c, flag, r, s))) {
    const EmblemPlace e = paintPattern(c, flag, r, s);
    paintCloth(c, r);
    paintEmblem(c, flag, e);
  } else {
    paintCloth(c, r);
  }
  c.restore();
  if (frame) {
    // Тонкая рамка внутри флага: тёмная кромка и светлый блик (1 пиксель устройства).
    const float px = 1.f / std::max(1e-3f, c.transform().scaleFactor());
    if (r.w > 2 * px && r.h > 2 * px) {
      c.strokeRoundRect(r.inset(px * 0.5f), std::max(0.f, radius - px * 0.5f), px, Color(10, 12, 18, 90));
      if (r.w > 6 * px && r.h > 6 * px)
        c.strokeRoundRect(r.inset(px * 1.5f), std::max(0.f, radius - px * 1.5f), px, Color(255, 255, 255, 34));
    }
  }
}

void clearFlagCache() { imageCache().clear(); }
size_t flagCacheSize() { return imageCache().size(); }

}  // namespace rg::gfx
