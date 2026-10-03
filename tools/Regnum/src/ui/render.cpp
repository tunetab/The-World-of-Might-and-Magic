// Regnum — запись команд отрисовки, кеш раскладок текста, примитивы draw::, сведение слоёв на холст
// (привязка к пикселям устройства, размытый фон модальных окон с кешем).
#include <bit>

#include "ui/ui_internal.h"

namespace rg::ui {

using namespace in;

namespace in {

// ---------------------------------------------------------------- текст
TextEntry* cachedText(std::string_view s, const gfx::TextStyle& st, float maxW, int maxLines, bool ellipsis, Align align, bool wrap) {
  Ctx& c = C();
  if (maxW < 0) maxW = 0;
  u64 k = hash64(s);
  k = hashMix(k, u64(st.family) | (u64(st.weight) << 8) | (u64(std::lround(st.size * 4)) << 16) |
                     (u64(std::lround(st.letterSpacing * 100) + 1000) << 36));
  k = hashMix(k, u64(std::lround(maxW * 4)) | (u64(u32(maxLines)) << 32) | (u64(ellipsis) << 48) | (u64(align) << 50) |
                     (u64(wrap) << 53) | (u64(std::lround(st.lineHeight * 64)) << 56));
  auto& slot = c.texts[k];
  if (slot && slot->text == s) {
    slot->lastFrame = c.frame;
    return slot.get();
  }
  if (!slot) slot = std::make_unique<TextEntry>();
  slot->text.assign(s.data(), s.size());
  slot->key = k;
  slot->lastFrame = c.frame;
  gfx::LayoutOptions o;
  o.maxWidth = maxW;
  o.maxLines = std::max(0, maxLines);
  o.wrap = wrap && maxW > 0;
  o.ellipsis = ellipsis && maxW > 0;
  o.align = align == Align::Center ? gfx::Align::Center : align == Align::Right ? gfx::Align::Right : gfx::Align::Left;
  gfx::layoutTextInto(slot->layout, slot->text, st, o);
  return slot.get();
}

float textWidth(std::string_view s, const gfx::TextStyle& st) {
  if (s.empty()) return 0;
  return cachedText(s, st)->layout.width;
}

float alphaMul() { return C().disabled > 0 ? 0.42f : 1.f; }

static inline Color am(Color c) {
  float k = alphaMul();
  return k < 1 ? c.alpha(k) : c;
}

static inline DrawList* list() {
  Window* w = C().win;
  return w ? &w->list : nullptr;
}

static inline Cmd& push(Op op) {
  DrawList* L = list();
  L->cmds.emplace_back();
  Cmd& k = L->cmds.back();
  k.op = op;
  return k;
}

void cmdRect(RectF r, Color c, float rad) {
  if (!list() || r.empty() || c.a == 0) return;
  Cmd& k = push(Op::Rect);
  k.r = r;
  k.c = am(c);
  k.rad = rad;
}

void cmdRect4(RectF r, Color c, float tl, float tr, float br, float bl) {
  if (!list() || r.empty() || c.a == 0) return;
  Cmd& k = push(Op::Rect);
  k.flags = 1;
  k.r = r;
  k.c = am(c);
  k.rad = tl;
  k.w = tr;
  k.dy = br;
  k.spread = bl;
}

void cmdStroke(RectF r, Color c, float rad, float w) {
  if (!list() || r.empty() || c.a == 0 || w <= 0) return;
  Cmd& k = push(Op::RectStroke);
  k.r = r;
  k.c = am(c);
  k.rad = rad;
  k.w = w;
}

void cmdShadow(RectF r, float rad, float blur, Color c, float dy, float spread) {
  if (!list() || r.empty() || c.a == 0) return;
  Cmd& k = push(Op::Shadow);
  k.r = r;
  k.rad = rad;
  k.w = blur;
  k.c = am(c);
  k.dy = dy;
  k.spread = spread;
}

void cmdText(TextEntry* t, float x, float y, Color c) {
  if (!list() || !t || t->layout.glyphs.empty() || c.a == 0) return;
  Cmd& k = push(Op::Text);
  k.text = t;
  k.r = RectF{x, y, t->layout.width, t->layout.height};
  k.c = am(c);
}

void cmdIcon(std::string_view name, RectF r, Color c) {
  DrawList* L = list();
  if (!L || name.empty() || r.empty() || c.a == 0) return;
  Cmd& k = push(Op::Icon);
  k.r = r;
  k.c = am(c);
  k.a = u32(L->arena.size());
  k.n = u32(name.size());
  L->arena.append(name.data(), name.size());
}

void cmdGradient(RectF r, float rad, bool horizontal, std::span<const std::pair<float, Color>> stops) {
  DrawList* L = list();
  if (!L || r.empty() || stops.empty()) return;
  Cmd& k = push(Op::Gradient);
  k.r = r;
  k.rad = rad;
  k.flags = horizontal ? 1 : 0;
  k.a = u32(L->stops.size());
  k.n = u32(stops.size());
  for (auto& s : stops) L->stops.push_back({s.first, am(s.second)});
}

size_t cmdMark() {
  DrawList* L = list();
  if (!L) return size_t(-1);
  push(Op::None);
  return L->cmds.size() - 1;
}

void cmdPatchRect(size_t at, RectF r, Color c, float rad) {
  DrawList* L = list();
  if (!L || at >= L->cmds.size()) return;
  Cmd& k = L->cmds[at];
  k = Cmd{};
  if (r.empty() || c.a == 0) return;
  k.op = Op::Rect;
  k.r = r;
  k.c = am(c);
  k.rad = rad;
}

void cmdPatchStroke(size_t at, RectF r, Color c, float rad, float w) {
  DrawList* L = list();
  if (!L || at >= L->cmds.size()) return;
  Cmd& k = L->cmds[at];
  k = Cmd{};
  if (r.empty() || c.a == 0) return;
  k.op = Op::RectStroke;
  k.r = r;
  k.c = am(c);
  k.rad = rad;
  k.w = w;
}

void cmdPatchShadow(size_t at, RectF r, float rad, float blur, Color c, float dy) {
  DrawList* L = list();
  if (!L || at >= L->cmds.size()) return;
  Cmd& k = L->cmds[at];
  k = Cmd{};
  if (r.empty() || c.a == 0) return;
  k.op = Op::Shadow;
  k.r = r;
  k.rad = rad;
  k.w = blur;
  k.c = am(c);
  k.dy = dy;
}

void textIn(std::string_view s, RectF r, const gfx::TextStyle& st, Color c, Align a) {
  if (s.empty() || r.w <= 0) return;
  TextEntry* t = cachedText(s, st, r.w + 0.5f, 1, true);
  const auto& L = t->layout;
  float x = r.x;
  if (a == Align::Center) x = r.x + (r.w - L.width) * 0.5f;
  else if (a == Align::Right) x = r.right() - L.width;
  cmdText(t, x, r.y + (r.h - L.height) * 0.5f, c);
}

// ---------------------------------------------------------------- сведение слоёв
namespace {

struct Layer {
  const DrawList* list = nullptr;
  int rank = 0;
  u32 order = 0;
  float alpha = 1, dy = 0;
  bool backdrop = false;
  float scrim = 1;
  bool blur = false;
  RectF rect;
  float radius = 0, blurRadius = 0;
};

inline RectF snapR(RectF r, float ds, float oy) {
  float x0 = std::round(r.x * ds), y0 = std::round((r.y + oy) * ds);
  float x1 = std::round((r.x + r.w) * ds), y1 = std::round((r.y + oy + r.h) * ds);
  return {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
}

inline float hair(float w, float ds) {
  float d = w * ds;
  return d <= 1.6f ? std::max(1.f, std::floor(d + 0.25f)) : std::round(d);
}

void exec(gfx::Canvas& cv, const DrawList& L, float ds, float oy) {
  int depth = 0;
  const gfx::Affine ident;
  const gfx::Affine scaleXf{ds, 0, 0, ds, 0, oy * ds};
  for (const Cmd& k : L.cmds) {
    switch (k.op) {
      case Op::None: break;
      case Op::Rect: {
        RectF d = snapR(k.r, ds, oy);
        if (d.empty()) break;
        if (k.flags & 1) cv.fillRoundRect(d, k.rad * ds, k.w * ds, k.dy * ds, k.spread * ds, k.c);
        else if (k.rad <= 0) cv.fillRect(d, k.c);
        else cv.fillRoundRect(d, std::min(k.rad * ds, std::min(d.w, d.h) * 0.5f), k.c);
        break;
      }
      case Op::RectStroke: {
        RectF d = snapR(k.r, ds, oy);
        if (d.empty()) break;
        float sw = hair(k.w, ds);
        RectF in = d.inset(sw * 0.5f);
        float rad = std::max(0.f, std::min(k.rad * ds, std::min(d.w, d.h) * 0.5f) - sw * 0.5f);
        cv.strokeRoundRect(in, rad, sw, k.c);
        break;
      }
      case Op::Gradient: {
        RectF d = snapR(k.r, ds, oy);
        if (d.empty()) break;
        gfx::Gradient g;
        g.p0 = {d.x, d.y};
        g.p1 = (k.flags & 1) ? gfx::Pt{d.right(), d.y} : gfx::Pt{d.x, d.bottom()};
        if (k.n > 0 && size_t(k.a) + k.n <= L.stops.size()) g.stops.assign(L.stops.begin() + k.a, L.stops.begin() + k.a + k.n);
        else g.stops = {{0.f, k.c}, {1.f, k.c2}};
        gfx::Paint p;
        p.gradient = &g;
        if (k.rad > 0) cv.fillRoundRect(d, std::min(k.rad * ds, std::min(d.w, d.h) * 0.5f), p);
        else cv.fillRect(d, p);
        break;
      }
      case Op::Shadow:
        cv.boxShadow(RectF{k.r.x * ds, (k.r.y + oy) * ds, k.r.w * ds, k.r.h * ds}, k.rad * ds, k.w * ds, k.spread * ds, k.c, gfx::Pt{0, k.dy * ds});
        break;
      case Op::Circle:
        cv.fillCircle(k.r.x * ds, (k.r.y + oy) * ds, k.r.w * ds, k.c);
        break;
      case Op::Ring:
        cv.strokeCircle(k.r.x * ds, (k.r.y + oy) * ds, k.r.w * ds, k.w * ds, k.c);
        break;
      case Op::Line: {
        float x0 = k.r.x * ds, y0 = (k.r.y + oy) * ds, x1 = k.r.w * ds, y1 = (k.r.h + oy) * ds;
        float sw = hair(k.w, ds);
        if (std::fabs(y0 - y1) < 0.01f) {
          float y = std::round(y0 - sw * 0.5f);
          float a = std::round(std::min(x0, x1)), b = std::round(std::max(x0, x1));
          cv.fillRect({a, y, b - a, sw}, k.c);
        } else if (std::fabs(x0 - x1) < 0.01f) {
          float x = std::round(x0 - sw * 0.5f);
          float a = std::round(std::min(y0, y1)), b = std::round(std::max(y0, y1));
          cv.fillRect({x, a, sw, b - a}, k.c);
        } else {
          cv.line(x0, y0, x1, y1, k.w * ds, k.c, gfx::Cap::Round);
        }
        break;
      }
      case Op::Path:
        if (k.a < L.paths.size()) {
          cv.setTransform(scaleXf);
          cv.fillPath(L.paths[k.a], k.c);
          cv.setTransform(ident);
        }
        break;
      case Op::PathStroke:
        if (k.a < L.paths.size()) {
          gfx::Stroke st;
          st.width = k.w;
          st.join = gfx::Join::Round;
          st.cap = gfx::Cap(k.flags);
          cv.setTransform(scaleXf);
          cv.strokePath(L.paths[k.a], st, k.c);
          cv.setTransform(ident);
        }
        break;
      case Op::Icon: {
        if (size_t(k.a) + k.n > L.arena.size()) break;
        float side = std::round(std::min(k.r.w, k.r.h) * ds);
        if (side < 1) break;
        float cx = (k.r.x + k.r.w * 0.5f) * ds, cy = (k.r.y + oy + k.r.h * 0.5f) * ds;
        RectF d{std::round(cx - side * 0.5f), std::round(cy - side * 0.5f), side, side};
        gfx::drawIcon(cv, std::string_view(L.arena).substr(k.a, k.n), d, k.c);
        break;
      }
      case Op::Text:
        if (k.text) {
          cv.setTransform(scaleXf);
          gfx::drawLayout(cv, k.text->layout, k.r.x, k.r.y, gfx::Paint(k.c));
          cv.setTransform(ident);
        }
        break;
      case Op::Image:
        if (k.img && !k.img->empty()) {
          RectF d = snapR(k.r, ds, oy);
          if (d.empty()) break;
          if (k.rad > 0) {
            cv.save();
            cv.clipRoundRect(d, std::min(k.rad * ds, std::min(d.w, d.h) * 0.5f));
            cv.drawImage(*k.img, d, k.w);
            cv.restore();
          } else {
            cv.drawImage(*k.img, d, k.w);
          }
        }
        break;
      case Op::Custom:
        if (k.a < L.customs.size() && L.customs[k.a]) {
          RectF d = snapR(k.r, ds, oy);
          if (d.empty()) break;
          cv.save();
          cv.clipRect(d);
          cv.setTransform(ident);
          L.customs[k.a](cv, d, ds);
          cv.restore();
        }
        break;
      case Op::ClipPush:
        cv.save();
        cv.clipRect(snapR(k.r, ds, oy));
        depth++;
        break;
      case Op::ClipPop:
        if (depth > 0) {
          cv.restore();
          depth--;
        }
        break;
    }
  }
  while (depth-- > 0) cv.restore();
}

inline u64 mixF(u64 h, float f) { return hashMix(h, u64(std::bit_cast<u32>(f))); }

u64 hashList(u64 h, const DrawList& L) {
  for (const Cmd& k : L.cmds) {
    h = hashMix(h, u64(k.op) | (u64(k.flags) << 8) | (u64(k.a) << 16) | (u64(k.n) << 40));
    h = mixF(mixF(mixF(mixF(h, k.r.x), k.r.y), k.r.w), k.r.h);
    h = mixF(mixF(mixF(mixF(h, k.w), k.rad), k.dy), k.spread);
    h = hashMix(h, (u64(k.c.r) | u64(k.c.g) << 8 | u64(k.c.b) << 16 | u64(k.c.a) << 24) | (u64(k.c2.r) | u64(k.c2.g) << 8 | u64(k.c2.b) << 16 | u64(k.c2.a) << 24) << 32);
    h = hashMix(h, u64(reinterpret_cast<uintptr_t>(k.text)) ^ u64(reinterpret_cast<uintptr_t>(k.img)));
  }
  for (const gfx::Path& p : L.paths) {
    h = hashMix(h, p.pts.size());
    for (const gfx::Pt& q : p.pts) h = mixF(mixF(h, q.x), q.y);
  }
  for (const auto& s : L.stops) h = hashMix(mixF(h, s.first), u64(s.second.r) | u64(s.second.g) << 8 | u64(s.second.b) << 16 | u64(s.second.a) << 24);
  h = hashMix(h, L.customs.size());
  return hash64(L.arena, h);
}

}  // namespace

void renderAll(gfx::Canvas& cv) {
  Ctx& c = C();
  std::vector<Layer> layers;
  layers.reserve(c.frameWins.size() + c.ghosts.size());
  for (Window* w : c.frameWins) {
    if (w->hidden) continue;
    Layer l;
    l.list = &w->list;
    l.rank = w->rank;
    l.order = w->order;
    l.alpha = w->alpha;
    l.dy = w->dy;
    l.backdrop = w->backdrop;
    l.blur = w->blur;
    l.rect = w->rect;
    l.blurRadius = w->blurRadius;
    l.radius = c.th.radiusPanel;
    layers.push_back(l);
  }
  for (Ghost& g : c.ghosts) {
    float t = float((c.time - g.start) / 0.14);
    Layer l;
    l.list = &g.list;
    l.rank = g.rank;
    l.order = 0xffffffffu;
    l.alpha = clamp(1 - t, 0.f, 1.f);
    l.dy = 6 * easeOut(t);
    l.backdrop = g.backdrop;
    l.scrim = l.alpha;
    layers.push_back(l);
  }
  std::stable_sort(layers.begin(), layers.end(), [](const Layer& a, const Layer& b) { return a.rank != b.rank ? a.rank < b.rank : a.order < b.order; });

  const float ds = c.ds;
  gfx::Image& target = cv.target();
  const RectF full{0, 0, float(cv.width()), float(cv.height())};
  cv.save();
  cv.setTransform(gfx::Affine{});

  size_t start = 0;
  int firstBackdrop = -1;
  for (size_t i = 0; i < layers.size(); i++)
    if (layers[i].backdrop && layers[i].scrim >= 0.999f) { firstBackdrop = int(i); break; }
  if (firstBackdrop >= 0) {
    // Всё ниже модального окна неподвижно (ввод перекрыт) — размытый фон берётся из кеша, пока команды те же.
    u64 h = hashMix(hashMix(u64(target.w) << 32 | u64(target.h), u64(std::bit_cast<u32>(ds))), c.th.dark ? 1 : 2);
    for (int i = 0; i < firstBackdrop; i++) {
      h = hashMix(h, u64(layers[size_t(i)].rank) << 32 | layers[size_t(i)].order);
      h = mixF(mixF(h, layers[size_t(i)].alpha), layers[size_t(i)].dy);
      h = hashList(h, *layers[size_t(i)].list);
    }
    if (c.backdropValid && c.backdropHash == h && c.backdrop.w == target.w && c.backdrop.h == target.h) {
      std::copy(c.backdrop.px.begin(), c.backdrop.px.end(), target.px.begin());
    } else {
      for (int i = 0; i < firstBackdrop; i++) {
        const Layer& l = layers[size_t(i)];
        cv.save();
        if (l.alpha < 1) cv.setOpacity(l.alpha);
        exec(cv, *l.list, ds, l.dy);
        cv.restore();
      }
      cv.blurRegion(gfx::RectI{0, 0, target.w, target.h}, 14 * ds);
      cv.fillRect(full, c.th.scrim);
      c.backdrop = target;
      c.backdropHash = h;
      c.backdropValid = true;
    }
    start = size_t(firstBackdrop);
    layers[start].backdrop = false;
  } else {
    c.backdropValid = false;
  }

  for (size_t i = start; i < layers.size(); i++) {
    const Layer& l = layers[i];
    if (l.backdrop) cv.fillRect(full, l.scrim < 1 ? c.th.scrim.alpha(l.scrim) : c.th.scrim);
    if (l.blur && l.blurRadius > 0) {
      RectF d = snapR(l.rect, ds, 0);
      cv.save();
      cv.clipRoundRect(d, l.radius * ds);
      cv.blurRegion(gfx::RectI{int(d.x), int(d.y), int(d.w), int(d.h)}, l.blurRadius * ds);
      cv.restore();
    }
    if (l.alpha <= 0.001f) continue;
    cv.save();
    if (l.alpha < 1) cv.setOpacity(l.alpha);
    exec(cv, *l.list, ds, l.dy);
    cv.restore();
  }
  cv.restore();

  // Исчезающие модальные окна живут 140 мс; их раскладки текста не должны уйти из кеша.
  for (auto it = c.ghosts.begin(); it != c.ghosts.end();) {
    if (c.time - it->start > 0.14) it = c.ghosts.erase(it);
    else {
      for (const Cmd& k : it->list.cmds)
        if (k.text) k.text->lastFrame = c.frame;
      ++it;
    }
  }
}

}  // namespace in

// ================================================================ публичные примитивы
void custom(RectF r, CustomDraw fn) {
  DrawList* L = in::list();
  if (!L || r.empty() || !fn) return;
  L->customs.push_back(std::move(fn));
  Cmd& k = in::push(Op::Custom);
  k.r = r;
  k.a = u32(L->customs.size() - 1);
}

namespace draw {

void rect(RectF r, Color c, float radius) { cmdRect(r, c, radius); }
void rectStroke(RectF r, Color c, float radius, float width) { cmdStroke(r, c, radius, width); }

void gradient(RectF r, Color top, Color bottom, float radius, bool horizontal) {
  if (!in::list() || r.empty()) return;
  Cmd& k = in::push(Op::Gradient);
  k.r = r;
  k.c = in::am(top);
  k.c2 = in::am(bottom);
  k.rad = radius;
  k.flags = horizontal ? 1 : 0;
}

void shadow(RectF r, float radius, float blur, Color c, float dy, float spread) { cmdShadow(r, radius, blur, c, dy, spread); }

void circle(float cx, float cy, float r, Color c) {
  if (!in::list() || r <= 0 || c.a == 0) return;
  Cmd& k = in::push(Op::Circle);
  k.r = RectF{cx, cy, r, r};
  k.c = in::am(c);
}

void ring(float cx, float cy, float r, float width, Color c) {
  if (!in::list() || r <= 0 || width <= 0 || c.a == 0) return;
  Cmd& k = in::push(Op::Ring);
  k.r = RectF{cx, cy, r, r};
  k.w = width;
  k.c = in::am(c);
}

void line(float x0, float y0, float x1, float y1, Color c, float width) {
  if (!in::list() || c.a == 0 || width <= 0) return;
  Cmd& k = in::push(Op::Line);
  k.r = RectF{x0, y0, x1, y1};
  k.w = width;
  k.c = in::am(c);
}

void path(const gfx::Path& p, Color c) {
  DrawList* L = in::list();
  if (!L || p.empty() || c.a == 0) return;
  L->paths.push_back(p);
  Cmd& k = in::push(Op::Path);
  k.a = u32(L->paths.size() - 1);
  k.c = in::am(c);
}

void pathStroke(const gfx::Path& p, Color c, float width, gfx::Cap cap) {
  DrawList* L = in::list();
  if (!L || p.empty() || c.a == 0 || width <= 0) return;
  L->paths.push_back(p);
  Cmd& k = in::push(Op::PathStroke);
  k.a = u32(L->paths.size() - 1);
  k.c = in::am(c);
  k.w = width;
  k.flags = u8(cap);
}

void icon(std::string_view name, RectF r, Color c) { cmdIcon(name, r, c); }

void text(std::string_view s, RectF r, Font f, Color c, Align a, bool ellipsis) {
  if (s.empty()) return;
  if (ellipsis) {
    textIn(s, r, styleOf(f), c, a);
    return;
  }
  TextEntry* t = cachedText(s, styleOf(f));
  float x = a == Align::Center ? r.x + (r.w - t->layout.width) * 0.5f : a == Align::Right ? r.right() - t->layout.width : r.x;
  cmdText(t, x, r.y + (r.h - t->layout.height) * 0.5f, c);
}

void image(const gfx::Image& img, RectF r, float radius, float opacity) {
  if (!in::list() || r.empty() || img.empty()) return;
  Cmd& k = in::push(Op::Image);
  k.r = r;
  k.img = &img;
  k.rad = radius;
  k.w = clamp(opacity * in::alphaMul(), 0.f, 1.f);
}

void pushClip(RectF r) {
  if (!in::list()) return;
  Cmd& k = in::push(Op::ClipPush);
  k.r = r;
}

void popClip() {
  if (!in::list()) return;
  in::push(Op::ClipPop);
}

}  // namespace draw
}  // namespace rg::ui
