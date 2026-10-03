// Regnum — знак приложения: золотой щит с короной (значок «logo») и плитка значка программы.
// Только заголовок (зависит от gfx): используется экраном запуска, верхней панелью и regnum-cli icon.
#pragma once
#include "gfx/canvas.h"
#include "gfx/icons.h"

namespace rg::app {

// Золотой знак в квадрате r (по центру). hinted — подгонка к пикселям (малые размеры, сплошной цвет).
inline void drawLogoMark(gfx::Canvas& c, RectF r, bool hinted = false, float opacity = 1) {
  float s = std::min(r.w, r.h);
  RectF q{r.x + (r.w - s) * 0.5f, r.y + (r.h - s) * 0.5f, s, s};
  const gfx::VecGlyph* g = gfx::iconGlyph("logo");
  if (hinted || !g) {
    gfx::drawIcon(c, "logo", q, Color::hex(0xe2ae4c).alpha(opacity), s <= 20 ? 1.15f : 1.0f);
    return;
  }
  // Маска знака в пикселях устройства; заливка — золотой градиент сверху вниз, под ним мягкая тень.
  const gfx::Affine& T = c.transform();
  gfx::Pt p0 = T.apply({q.x, q.y});
  float k = T.scaleFactor() * s / g->grid;
  int ox = int(std::floor(p0.x)) - 2, oy = int(std::floor(p0.y)) - 2;
  int n = int(std::ceil(k * g->grid)) + 6;
  gfx::Mask m(n, n);
  gfx::GlyphStyle st;
  st.hint = false;
  st.strokeScale = s >= 96 ? 0.92f : 1.0f;
  gfx::renderGlyphMask(*g, gfx::Affine{k, 0, 0, k, p0.x, p0.y}, m, ox, oy, st);
  gfx::Gradient gold;
  gold.kind = gfx::Gradient::Linear;
  gold.p0 = {0, p0.y + 2 * k};
  gold.p1 = {0, p0.y + 22 * k};
  gold.stops = {{0.f, Color::hex(0xf6d488)}, {0.48f, Color::hex(0xe0a943)}, {1.f, Color::hex(0xa8741c)}};
  c.save();
  c.setTransform(gfx::Affine{});
  gfx::Paint shadow(Color(0, 0, 0, u8(90 * opacity)));
  c.fillMask(m, float(ox), float(oy) + std::max(1.f, 0.45f * k), shadow);
  gfx::Paint p;
  p.gradient = &gold;
  p.opacity = opacity;
  c.fillMask(m, float(ox), float(oy), p);
  c.restore();
}

// Плитка значка программы: графитовый скруглённый квадрат с золотой каймой и знаком.
inline void drawLogoTile(gfx::Canvas& c, RectF r) {
  float s = std::min(r.w, r.h);
  RectF t{r.x + (r.w - s) * 0.5f, r.y + (r.h - s) * 0.5f, s, s};
  float rad = s * 0.22f;
  gfx::Gradient bg;
  bg.kind = gfx::Gradient::Linear;
  bg.p0 = {t.x, t.y};
  bg.p1 = {t.x, t.bottom()};
  bg.stops = {{0.f, Color::hex(0x2a3341)}, {1.f, Color::hex(0x0c0f14)}};
  gfx::Paint p;
  p.gradient = &bg;
  c.fillRoundRect(t, rad, p);
  if (s >= 32) {
    // Мягкий блик сверху.
    gfx::Gradient hl;
    hl.kind = gfx::Gradient::Radial;
    hl.p0 = {t.cx(), t.y + s * 0.18f};
    hl.r1 = s * 0.62f;
    hl.stops = {{0.f, Color(255, 214, 140, 46)}, {1.f, Color(255, 214, 140, 0)}};
    gfx::Paint hp;
    hp.gradient = &hl;
    c.fillRoundRect(t, rad, hp);
  }
  float rim = std::max(1.f, s * 0.024f);
  c.strokeRoundRect(t.inset(rim * 0.5f + (s >= 32 ? s * 0.02f : 0.f)), rad - rim, rim, Color(226, 174, 76, s >= 32 ? 150 : 110));
  float gs = s * (s <= 16 ? 0.86f : s <= 32 ? 0.78f : 0.64f);
  RectF gr{t.cx() - gs * 0.5f, t.cy() - gs * 0.5f + s * 0.01f, gs, gs};
  drawLogoMark(c, gr, s <= 32);
}

}  // namespace rg::app
