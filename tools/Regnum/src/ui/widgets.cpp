// Regnum — основные виджеты: текст, кнопки, переключатели, ползунок, вкладки, бейджи, фишки, полосы,
// диаграммы, плитки показателей, аватары, пустые состояния.
#include "gfx/flag.h"
#include "ui/ui_internal.h"

namespace rg::ui {

using namespace in;

namespace in {

void drawChevron(RectF r, Color c, float angleDeg) {
  float cx = r.cx(), cy = r.cy();
  float s = std::min(r.w, r.h) / 14.f;
  float a = angleDeg * float(kPi) / 180.f, cs = std::cos(a), sn = std::sin(a);
  auto P = [&](float x, float y) { return gfx::Pt{cx + (x * cs - y * sn) * s, cy + (x * sn + y * cs) * s}; };
  gfx::Path p;
  gfx::Pt a0 = P(-3.75f, -1.9f), a1 = P(0, 1.9f), a2 = P(3.75f, -1.9f);
  p.moveTo(a0.x, a0.y);
  p.lineTo(a1.x, a1.y);
  p.lineTo(a2.x, a2.y);
  draw::pathStroke(p, c, 1.75f * s);
}

void drawCheckGlyph(RectF b, Color c, float t) {
  if (t <= 0.01f) return;
  float s = b.w / 18.f;
  gfx::Path p;
  p.moveTo(b.x + 4.6f * s, b.y + 9.4f * s);
  p.lineTo(b.x + 7.6f * s, b.y + 12.3f * s);
  p.lineTo(b.x + 13.4f * s, b.y + 6.2f * s);
  draw::pathStroke(p, c.alpha(clamp(t * 1.5f, 0.f, 1.f)), 2.f * s);
}

void drawFieldFrame(RectF r, const Interaction& it, bool focused, bool disabled, bool invalid, float invalidT) {
  Ctx& c = C();
  const Theme& t = c.th;
  float rad = t.radiusField;
  Color bg = t.dark ? t.surface3 : t.surface2;
  if (!t.dark && disabled) bg = t.surface3;
  cmdRect(r, bg, rad);
  Color bc = t.dark ? t.border.lighten(0.04f) : t.border;
  if (it.hovered && !disabled) bc = t.borderStrong;
  if (focused) bc = t.accent;
  if (invalid) bc = mixc(bc, t.danger, invalidT);
  if (focused || (invalid && invalidT > 0.01f)) {
    Color glow = invalid ? t.danger : t.accent;
    cmdStroke(r.expand(3), glow.alpha(invalid ? 0.25f * invalidT : 0.22f), rad + 3, 3);
  }
  cmdStroke(r, bc, rad, 1);
}

}  // namespace in

namespace {

float ctlH(Size s) { return s == Size::Small ? C().th.controlHSmall : C().th.controlH; }

// Свой тон приглушённого фона: бейджи, теги, иконки в кружках.
Color tint(Color c, float k) { return c.alpha(k); }

std::string initialsOf(std::string_view name) {
  std::string out;
  std::string_view s = name;
  bool take = true;
  size_t i = 0;
  int n = 0;
  while (i < s.size() && n < 2) {
    u32 cp = utf8::decode(s, i);
    bool word = utf8::isWordChar(cp);
    if (word && take) {
      utf8::append(out, utf8::upperCp(cp));
      n++;
      take = false;
    } else if (!word) {
      take = true;
    }
  }
  return out;
}

}  // namespace

// ================================================================ текст
void label(std::string_view txt, const LabelOpt& o) {
  Ctx& c = C();
  const auto& st = styleOf(o.font);
  std::string_view s = displayText(txt);
  Color col = o.color.a > 0 ? o.color : inkColor(o.ink);
  Color iconCol = o.ink == Ink::Normal && o.color.a == 0 ? c.th.textDim : col;
  float lh = lineHeight(o.font);
  float is = o.font >= Font::Title ? 22.f : 16.f;
  float iconW = o.icon ? is + 6 : 0;
  RectF r;
  if (o.wrap) {
    RectF a = c.oneShot ? *c.oneShot : avail();
    float w = std::max(1.f, a.w - iconW);
    TextEntry* te = cachedText(s, st, w, o.maxLines, o.maxLines > 0, o.align, true);
    r = place(0, std::max(lh, te->layout.height));
    if (o.icon) cmdIcon(o.icon, RectF{r.x, r.y + (lh - is) * 0.5f, is, is}, iconCol);
    cmdText(te, r.x + iconW, r.y, col);
  } else {
    float tw = textWidth(s, st);
    r = place(o.align == Align::Left ? std::ceil(tw + iconW) : 0, lh);
    float total = std::min(r.w, tw + iconW);
    float x = r.x;
    if (o.align == Align::Center) x = r.x + (r.w - total) * 0.5f;
    else if (o.align == Align::Right) x = r.right() - total;
    if (o.icon) {
      cmdIcon(o.icon, RectF{x, r.cy() - is * 0.5f, is, is}, iconCol);
      x += iconW;
    }
    textIn(s, RectF{x, r.y, std::max(0.f, r.right() - x), r.h}, st, col);
  }
  if (!o.tooltip.empty()) {
    // ID — текст вместе с подсказкой: одинаковые числа рядом («10 %» налога государства и общего) с разными
    // подсказками не делят один ID.
    WidgetId wid = hashMix(id(txt), hash64(o.tooltip, 0x1abe1ull));
    Interaction it = interact(wid, r);
    setLast(wid, r, it);
    tooltip(o.tooltip);
  } else {
    c.last = Item{};
    c.last.rect = r;
  }
}

void text(std::string_view s, Font f, Ink ink) {
  LabelOpt o;
  o.font = f;
  o.ink = ink;
  o.wrap = true;
  label(s, o);
}

void caption(std::string_view s) {
  Ctx& c = C();
  gfx::TextStyle st = styleWith(Font::Caption, gfx::FontWeight::Semibold);
  st.letterSpacing = 0.7f;
  RectF r = place(0, 18);
  textIn(utf8::upper(displayText(s)), r, st, c.th.textMuted);
  c.last = Item{};
  c.last.rect = r;
}

void prop(std::string_view lbl, const char* icon, float frac) {
  Ctx& c = C();
  const Theme& t = c.th;
  RectF r = place(0, t.controlH);
  float lw = std::round(r.w * clamp(frac, 0.1f, 0.9f));
  float x = r.x;
  if (icon) {
    cmdIcon(icon, RectF{x, r.cy() - 8, 16, 16}, t.textMuted);
    x += 22;
  }
  textIn(displayText(lbl), RectF{x, r.y, r.x + lw - 8 - x, r.h}, styleOf(Font::Body), t.textDim);
  c.oneShot = RectF{r.x + lw, r.y, r.w - lw, r.h};
  c.oneShotAlign = Align::Left;
}

bool link(std::string_view txt, const char* icon) {
  Ctx& c = C();
  const auto& st = styleOf(Font::Body);
  std::string_view s = displayText(txt);
  float tw = textWidth(s, st) + (icon ? 20 : 0);
  RectF r = place(tw, lineHeight(Font::Body));
  WidgetId wid = id(txt);
  Interaction it = interact(wid, r, IfFocusable);
  Color col = it.hovered ? c.th.accentHover : c.th.accent;
  float x = r.x;
  if (icon) {
    cmdIcon(icon, RectF{x, r.cy() - 8, 16, 16}, col);
    x += 20;
  }
  TextEntry* te = cachedText(s, st);
  float ty = r.y + (r.h - te->layout.height) * 0.5f;
  cmdText(te, x, ty, col);
  if (it.hovered) {
    c.cursor = platform::Cursor::Hand;
    float base = ty + (te->layout.lines.empty() ? te->layout.height : te->layout.lines[0].baseline) + 2;
    draw::line(x, base, x + te->layout.width, base, col.alpha(0.7f), 1);
  }
  setLast(wid, r, it);
  focusRing(r.expand(2), 4);
  return it.clicked;
}

void kbd(std::string_view keys) {
  Ctx& c = C();
  const Theme& t = c.th;
  std::vector<std::string> parts;
  std::string_view s = keys;
  // «Ctrl+Shift+S» → три клавиши; «+» как клавиша — «Ctrl++».
  size_t i = 0;
  std::string cur;
  while (i < s.size()) {
    if (s[i] == '+' && !cur.empty()) {
      parts.push_back(cur);
      cur.clear();
      i++;
      continue;
    }
    cur.push_back(s[i]);
    i++;
  }
  if (!cur.empty()) parts.push_back(cur);
  if (parts.empty()) return;
  const gfx::TextStyle st = styleWith(Font::Small, gfx::FontWeight::Semibold);
  float total = 0;
  std::vector<float> ws;
  for (auto& p : parts) {
    float w = std::max(20.f, textWidth(p, st) + 12);
    ws.push_back(w);
    total += w;
  }
  total += 3 * float(parts.size() - 1);
  RectF r = place(total, 22);
  float x = r.x;
  for (size_t k = 0; k < parts.size(); k++) {
    RectF cap{x, r.y, ws[k], 21};
    cmdRect(RectF{cap.x, cap.y + 1, cap.w, cap.h}, t.dark ? Color(0, 0, 0, 90) : t.borderStrong, 5);
    cmdRect(RectF{cap.x, cap.y, cap.w, cap.h - 1}, t.dark ? t.surface3 : t.surface2, 5);
    cmdStroke(RectF{cap.x, cap.y, cap.w, cap.h - 1}, t.dark ? t.borderStrong : t.border, 5, 1);
    textIn(parts[k], RectF{cap.x, cap.y, cap.w, cap.h - 1}, st, t.textDim, Align::Center);
    x += ws[k] + 3;
  }
  c.last = Item{};
  c.last.rect = r;
}

void kbd(Shortcut s) { kbd(shortcutText(s)); }

void separator() {
  Ctx& c = C();
  RectF r = place(0, 9);
  draw::line(r.x, r.cy(), r.right(), r.cy(), c.th.border, 1);
}

void separatorV() {
  Ctx& c = C();
  Frame& f = frame();
  float h = f.flow == Flow::HStack ? f.stackH : c.th.controlH;
  RectF r = place(9, h);
  draw::line(r.cx(), r.y + h * 0.2f, r.cx(), r.bottom() - h * 0.2f, c.th.border, 1);
}

void icon(const char* name, Ink ink, float size, std::string_view tip) { iconColored(name, inkColor(ink), size, tip); }

void iconColored(const char* name, Color col, float size, std::string_view tip) {
  Ctx& c = C();
  float s = size > 0 ? size : c.th.iconSize;
  RectF r = place(s, s);
  cmdIcon(name ? name : "", r, col);
  if (!tip.empty()) {
    WidgetId wid = id(std::string_view(name ? name : "")) ^ hash64(tip);
    Interaction it = interact(wid, r);
    setLast(wid, r, it);
    tooltip(tip);
  } else {
    c.last = Item{};
    c.last.rect = r;
  }
}

void image(const gfx::Image& img, float w, float h, float radius) {
  RectF r = place(w, h);
  draw::image(img, r, radius);
  C().last = Item{};
  C().last.rect = r;
}

// ================================================================ кнопки
bool button(std::string_view lbl, const ButtonOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  DisableGuard dg(o.disabled);
  bool disabled = isDisabled();
  std::string_view s = displayText(lbl);
  bool small = o.size == Size::Small;
  const gfx::TextStyle st = small ? styleWith(Font::Small, gfx::FontWeight::Semibold) : styleOf(Font::Strong);
  float is = small ? 14 : 16;
  float pad = small ? 10 : 14;
  float tw = s.empty() ? 0 : textWidth(s, st);
  float w = pad * 2 + tw + (o.icon ? is + (s.empty() ? 0 : 7) : 0) + (o.iconRight ? is + 6 : 0);
  if (o.icon && !s.empty()) w -= 2;
  float h = ctlH(o.size);
  RectF r = place(o.fill ? 0 : std::ceil(w), h);
  if (r.h > h) {
    r.y += std::round((r.h - h) * 0.5f);
    r.h = h;
  }
  WidgetId wid = id(lbl.empty() ? std::string_view(o.icon ? o.icon : "button") : lbl);
  Interaction it = interact(wid, r, IfFocusable);
  bool clicked = it.clicked;
  if (o.isDefault && !c.modalStack.empty()) c.modalStack.back().defaultBtn = wid;
  if (c.pendingDefault == wid) {
    c.pendingDefault = 0;
    if (!disabled) clicked = true;
  }
  if (o.shortcut && !disabled && shortcut(o.shortcut)) clicked = true;
  float hv = animate(wid ^ 0xb7701ull, it.hovered ? 1.f : 0.f);
  bool down = it.held && it.hovered;
  float rad = t.radiusField;
  Color bg{0, 0, 0, 0}, fg = t.text, border{0, 0, 0, 0};
  switch (o.variant) {
    case Variant::Primary:
      bg = mixc(t.accent, t.accentHover, hv);
      if (down) bg = t.accent.darken(0.08f);
      fg = t.onAccent;
      break;
    case Variant::Danger:
      bg = mixc(t.danger, t.danger.lighten(0.1f), hv);
      if (down) bg = t.danger.darken(0.08f);
      fg = Color(255, 255, 255);
      break;
    case Variant::Secondary:
      bg = t.dark ? mixc(t.surface3, t.surface3.lighten(0.05f), hv) : mixc(t.surface2, t.surface3, hv * 0.6f);
      if (down) bg = t.dark ? t.surface3.darken(0.06f) : t.surface3;
      border = t.dark ? mixc(t.borderStrong.alpha(0.7f), t.borderStrong, hv) : mixc(t.border, t.borderStrong, hv);
      break;
    case Variant::Ghost:
      bg = t.hover.alpha(hv);
      if (down) bg = t.pressed;
      break;
    case Variant::Subtle:
      bg = t.hover.alpha(hv * 0.8f);
      if (down) bg = t.pressed;
      fg = mixc(t.textDim, t.text, hv);
      break;
  }
  if (o.variant == Variant::Primary || o.variant == Variant::Danger) {
    cmdShadow(r, rad, 3, Color(0, 0, 0, t.dark ? 70 : 30), 1);
    draw::gradient(r, bg.lighten(0.07f), bg.darken(0.03f), rad);
    cmdStroke(r, bg.darken(0.12f).alpha(0.6f), rad, 1);
  } else {
    cmdRect(r, bg, rad);
    if (border.a) cmdStroke(r, border, rad, 1);
  }
  float cw = tw + (o.icon ? is + (s.empty() ? 0 : 5) : 0) + (o.iconRight ? is + 6 : 0);
  float x = r.x + std::round((r.w - cw) * 0.5f);
  if (o.icon) {
    cmdIcon(o.icon, RectF{x, r.cy() - is * 0.5f, is, is}, fg);
    x += is + (s.empty() ? 0 : 5);
  }
  if (!s.empty()) {
    textIn(s, RectF{x, r.y, std::min(tw + 1, r.right() - pad * 0.5f - x), r.h}, st, fg);
    x += tw + 6;
  }
  if (o.iconRight) cmdIcon(o.iconRight, RectF{x, r.cy() - is * 0.5f, is, is}, o.variant == Variant::Secondary ? t.textDim : fg);
  setLast(wid, r, it);
  c.last.clicked = clicked;
  focusRing(r, rad);
  if (!o.tooltip.empty() || o.shortcut) tooltip(o.tooltip.empty() ? s : o.tooltip, o.shortcut);
  return clicked;
}

bool iconButton(const char* icon, std::string_view tip, const IconButtonOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  DisableGuard dg(o.disabled);
  bool disabled = isDisabled();
  if (tip.empty()) {
    static bool warned = false;
    if (!warned) logWarn("ui: iconButton без подсказки (%s)", icon ? icon : "?");
    warned = true;
  }
  float h = ctlH(o.size);
  RectF r = place(h, h);
  if (r.w > h || r.h > h) r = RectF{r.x + std::round((r.w - h) * 0.5f), r.y + std::round((r.h - h) * 0.5f), h, h};
  std::string key = std::string(icon ? icon : "") + "\x1f" + std::string(tip);
  WidgetId wid = id(key);
  Interaction it = interact(wid, r, IfFocusable);
  bool clicked = it.clicked;
  if (o.shortcut && !disabled && shortcut(o.shortcut)) clicked = true;
  float hv = animate(wid ^ 0x1c0b1ull, it.hovered ? 1.f : 0.f);
  float on = animate(wid ^ 0x1c0b2ull, o.toggled ? 1.f : 0.f);
  bool down = it.held && it.hovered;
  float rad = t.radiusField;
  Color fg = o.tone != Tone::Neutral ? toneColor(o.tone) : mixc(t.textDim, t.text, hv);
  switch (o.variant) {
    case Variant::Primary:
      draw::gradient(r, mixc(t.accent, t.accentHover, hv).lighten(0.06f), mixc(t.accent, t.accentHover, hv).darken(0.03f), rad);
      fg = t.onAccent;
      break;
    case Variant::Danger:
      cmdRect(r, mixc(t.danger, t.danger.lighten(0.1f), hv), rad);
      fg = Color(255, 255, 255);
      break;
    case Variant::Secondary:
      cmdRect(r, t.dark ? mixc(t.surface3, t.surface3.lighten(0.05f), hv) : mixc(t.surface2, t.surface3, hv * 0.6f), rad);
      cmdStroke(r, t.dark ? t.borderStrong.alpha(0.7f + 0.3f * hv) : mixc(t.border, t.borderStrong, hv), rad, 1);
      break;
    case Variant::Ghost:
    case Variant::Subtle:
      if (hv > 0.01f) cmdRect(r, down ? t.pressed : t.hover.alpha(hv), rad);
      break;
  }
  if (on > 0.01f) {
    cmdRect(r, t.accent.alpha((t.dark ? 0.16f : 0.14f) * on), rad);
    cmdStroke(r, t.accent.alpha(0.38f * on), rad, 1);
    fg = mixc(fg, t.accent, on);
  }
  float is = o.size == Size::Small ? t.iconSizeSmall : t.iconSize;
  RectF ir{r.x + std::round((r.w - is) * 0.5f), r.y + std::round((r.h - is) * 0.5f), is, is};
  if (down) ir.y += 0.5f;
  cmdIcon(icon ? icon : "", ir, fg);
  if (o.badge) {
    Color bc = o.badgeColor.a ? o.badgeColor : t.danger;
    float bx = r.right() - 7, by = r.y + 7;
    draw::circle(bx, by, 4.5f, t.surface1);
    draw::circle(bx, by, 3.2f, bc);
  }
  setLast(wid, r, it);
  c.last.clicked = clicked;
  focusRing(r, rad);
  tooltip(tip, o.shortcut);
  return clicked;
}

bool iconToggle(const char* icon, std::string_view tip, bool& on, const IconButtonOpt& o) {
  IconButtonOpt k = o;
  k.toggled = on;
  if (iconButton(icon, tip, k)) {
    on = !on;
    C().last.changed = true;
    return true;
  }
  return false;
}

// ================================================================ сегменты
static bool segmentedImpl(std::string_view name, int& index, std::span<const Segment> items, const SegmentedOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  DisableGuard dg(o.disabled);
  int n = int(items.size());
  if (n == 0) return false;
  float h = ctlH(o.size);
  const gfx::TextStyle& st = styleOf(o.size == Size::Small ? Font::Small : Font::Body);
  const gfx::TextStyle stOn = styleWith(o.size == Size::Small ? Font::Small : Font::Body, gfx::FontWeight::Semibold);
  float is = o.size == Size::Small ? 14 : 16;
  std::vector<float> nat(size_t(n), 0);
  float natTotal = 4;
  for (int i = 0; i < n; i++) {
    const Segment& sg = items[size_t(i)];
    float w = (sg.icon ? is : 0) + (sg.label.empty() ? 0 : textWidth(displayText(sg.label), stOn) + (sg.icon ? 6 : 0));
    nat[size_t(i)] = w + (sg.label.empty() ? 14 : 24);
    natTotal += nat[size_t(i)];
  }
  RectF r = place(o.fill ? 0 : natTotal, h);
  if (r.h > h) {
    r.y += std::round((r.h - h) * 0.5f);
    r.h = h;
  }
  WidgetId wid = id(name);
  registerId(wid);
  Color track = t.dark ? mixc(t.bg, t.surface1, 0.5f) : t.surface3;
  cmdRect(r, track, t.radiusField);
  cmdStroke(r, t.dark ? t.border.alpha(0.7f) : t.border.alpha(0.6f), t.radiusField, 1);
  RectF inner = r.inset(2);
  std::vector<float> xs(size_t(n) + 1, inner.x);
  for (int i = 0; i < n; i++) xs[size_t(i) + 1] = xs[size_t(i)] + (o.fill ? (inner.w / float(n)) : nat[size_t(i)]);
  int cur = clamp(index, -1, n - 1);
  bool changed = false;
  // Клавиатура: фокус на всём переключателе, стрелки меняют выбор.
  if (!isDisabled()) registerFocusable(wid);
  bool focused = c.focus == wid;
  if (focused && !isDisabled()) {
    if (takeKey(Key::Left) && cur > 0) { cur--; changed = true; }
    if (takeKey(Key::Right) && cur < n - 1) { cur++; changed = true; }
  }
  int hoveredI = -1;
  for (int i = 0; i < n; i++) {
    RectF cell{xs[size_t(i)], inner.y, xs[size_t(i) + 1] - xs[size_t(i)], inner.h};
    WidgetId cid = hashMix(wid, u64(i) + 1);
    Interaction it = interact(cid, cell);
    if (it.pressed) setFocus(wid, false);
    if (it.hovered) hoveredI = i;
    if (it.clicked && cur != i) { cur = i; changed = true; }
    if (it.hovered && !items[size_t(i)].tooltip.empty()) {
      setLast(cid, cell, it);
      tooltip(items[size_t(i)].tooltip);
    }
  }
  if (cur >= 0) {
    float tx = animate(wid ^ 0x5e1ull, xs[size_t(cur)], 0.16f);
    float tw = animate(wid ^ 0x5e2ull, xs[size_t(cur) + 1] - xs[size_t(cur)], 0.16f);
    RectF th{tx, inner.y, tw, inner.h};
    float rr = t.radiusField - 2;
    cmdShadow(th, rr, 3, Color(0, 0, 0, t.dark ? 90 : 28), 1);
    cmdRect(th, t.dark ? t.surface3.lighten(0.06f) : t.surface2, rr);
    if (t.dark) cmdStroke(th, Color(255, 255, 255, 14), rr, 1);
  }
  for (int i = 0; i < n; i++) {
    const Segment& sg = items[size_t(i)];
    RectF cell{xs[size_t(i)], inner.y, xs[size_t(i) + 1] - xs[size_t(i)], inner.h};
    bool on = i == cur;
    Color fg = on ? t.text : (i == hoveredI ? t.text : t.textDim);
    std::string_view lb = displayText(sg.label);
    float cw = (sg.icon ? is : 0) + (lb.empty() ? 0 : textWidth(lb, on ? stOn : st) + (sg.icon ? 6 : 0));
    float x = cell.x + std::round((cell.w - cw) * 0.5f);
    if (sg.icon) {
      cmdIcon(sg.icon, RectF{x, cell.cy() - is * 0.5f, is, is}, on ? t.accent : fg);
      x += is + 6;
    }
    if (!lb.empty()) textIn(lb, RectF{x, cell.y, cell.right() - x, cell.h}, on ? stOn : st, fg);
  }
  Interaction whole{};
  whole.hovered = hoveredI >= 0;
  setLast(wid, r, whole);
  c.last.focused = focused;
  focusRing(r, t.radiusField);
  if (changed) {
    index = cur;
    c.last.changed = true;
  }
  return changed;
}

bool segmented(std::string_view name, int& index, std::initializer_list<Segment> items, const SegmentedOpt& o) {
  return segmentedImpl(name, index, std::span<const Segment>(items.begin(), items.size()), o);
}
bool segmented(std::string_view name, int& index, std::span<const Segment> items, const SegmentedOpt& o) { return segmentedImpl(name, index, items, o); }

// ================================================================ переключатели
bool toggle(std::string_view lbl, bool& on, bool disabled) {
  Ctx& c = C();
  const Theme& t = c.th;
  DisableGuard dg(disabled);
  std::string_view s = displayText(lbl);
  const auto& st = styleOf(Font::Body);
  float tw = s.empty() ? 0 : textWidth(s, st);
  Frame& f = frame();
  bool natural = f.flow == Flow::HStack;
  RectF r = place(natural ? tw + (s.empty() ? 0 : 12) + 36 : 0, t.controlH);
  WidgetId wid = id(lbl);
  Interaction it = interact(wid, r, IfFocusable);
  bool changed = false;
  if (it.clicked) {
    on = !on;
    changed = true;
  }
  float k = animate(wid ^ 0x7091ull, on ? 1.f : 0.f, 0.16f);
  float hv = animate(wid ^ 0x7092ull, it.hovered ? 1.f : 0.f);
  RectF sw{r.right() - 36, r.cy() - 10, 36, 20};
  if (!s.empty()) textIn(s, RectF{r.x, r.y, sw.x - 12 - r.x, r.h}, st, mixc(t.textDim, t.text, std::max(hv, 0.6f)));
  Color offc = t.dark ? t.borderStrong : Color::hex(0xd3c9b8);
  Color trackc = mixc(offc, t.accent, k);
  if (hv > 0.01f && !on) trackc = mixc(trackc, t.textMuted, 0.25f * hv);
  cmdRect(sw, trackc, 10);
  float kx = sw.x + 2 + 16 * k;
  RectF knob{kx, sw.y + 2, 16, 16};
  cmdShadow(knob, 8, 3, Color(0, 0, 0, 70), 1);
  draw::circle(knob.cx(), knob.cy(), 8, t.dark ? Color::hex(0xf4f1ea) : Color(255, 255, 255));
  setLast(wid, r, it);
  c.last.changed = changed;
  RectF fr = sw;
  focusRing(fr, 10);
  return changed;
}

static bool checkImpl(std::string_view lbl, Check& state, bool disabled, bool radioStyle) {
  Ctx& c = C();
  const Theme& t = c.th;
  DisableGuard dg(disabled);
  std::string_view s = displayText(lbl);
  const auto& st = styleOf(Font::Body);
  float tw = s.empty() ? 0 : textWidth(s, st);
  RectF r = place(18 + (s.empty() ? 0 : 8 + tw), std::max(22.f, lineHeight(Font::Body)));
  WidgetId wid = id(lbl);
  Interaction it = interact(wid, r, IfFocusable);
  bool changed = false;
  if (it.clicked) {
    state = radioStyle ? Check::On : (state == Check::On ? Check::Off : Check::On);
    changed = true;
  }
  float k = animate(wid ^ 0xc4e1ull, state != Check::Off ? 1.f : 0.f, 0.12f);
  float hv = animate(wid ^ 0xc4e2ull, it.hovered ? 1.f : 0.f);
  RectF box{r.x, r.cy() - 9, 18, 18};
  if (radioStyle) {
    Color ring = mixc(t.dark ? t.borderStrong : t.borderStrong, t.textMuted, hv * 0.6f);
    draw::circle(box.cx(), box.cy(), 9, t.dark ? t.surface3 : t.surface2);
    draw::ring(box.cx(), box.cy(), 8.5f, 1.25f, mixc(ring, t.accent, k));
    if (k > 0.01f) {
      draw::circle(box.cx(), box.cy(), 9 * k, t.accent);
      draw::circle(box.cx(), box.cy(), 3.6f * k, t.onAccent);
    }
  } else {
    float rad = 5;
    cmdRect(box, mixc(t.dark ? t.surface3 : t.surface2, t.accent, k), rad);
    if (k < 0.99f) cmdStroke(box, mixc(t.borderStrong, t.textMuted, hv * 0.6f).alpha(1 - k), rad, 1.25f);
    if (state == Check::Mixed) draw::line(box.x + 5, box.cy(), box.right() - 5, box.cy(), t.onAccent.alpha(k), 2);
    else drawCheckGlyph(box, t.onAccent, k);
  }
  if (!s.empty()) textIn(s, RectF{box.right() + 8, r.y, r.right() - box.right() - 8, r.h}, st, t.text);
  setLast(wid, r, it);
  c.last.changed = changed;
  focusRing(box, radioStyle ? 9 : 5);
  return changed;
}

bool checkbox(std::string_view lbl, bool& on, bool disabled) {
  Check s = on ? Check::On : Check::Off;
  bool ch = checkImpl(lbl, s, disabled, false);
  if (ch) on = s == Check::On;
  return ch;
}

bool checkbox(std::string_view lbl, Check& st, bool disabled) {
  if (st == Check::Mixed) {
    Check s = st;
    bool ch = checkImpl(lbl, s, disabled, false);
    if (ch) st = Check::On;
    return ch;
  }
  return checkImpl(lbl, st, disabled, false);
}

bool radio(std::string_view lbl, int& value, int option, bool disabled) {
  Check s = value == option ? Check::On : Check::Off;
  bool ch = checkImpl(lbl, s, disabled, true);
  if (ch && value != option) {
    value = option;
    return true;
  }
  C().last.changed = false;
  return false;
}

// ================================================================ ползунок
bool slider(std::string_view name, double& v, double mn, double mx, const SliderOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  DisableGuard dg(o.disabled);
  if (!(mx > mn)) mx = mn + 1;
  RectF r = place(0, t.controlH);
  WidgetId wid = id(name);
  const auto& vst = styleOf(Font::Body);
  auto fmt = [&](double x) {
    std::string s = fmtNum(x, o.digits);
    if (o.unit) {
      std::string u = unitFor(o.unit, x, o.digits);
      s += u == "%" ? " " : " ";
      s += u;
    }
    return s;
  };
  float valW = o.showValue ? std::max(44.f, textWidth(fmt(mx), vst) + 6) : 0;
  RectF tr{r.x + 8, r.cy() - 2, r.w - 16 - (o.showValue ? valW + 8 : 0), 4};
  RectF hit{r.x, r.y, tr.right() + 8 - r.x, r.h};
  Interaction it = interact(wid, hit, IfFocusable);
  double nv = clamp(v, mn, mx);
  auto snapv = [&](double x) {
    if (o.step > 0) x = mn + std::round((x - mn) / o.step) * o.step;
    if (o.digits >= 0 && o.step <= 0) {
      double p = std::pow(10.0, o.digits);
      x = std::round(x * p) / p;
    }
    return clamp(x, mn, mx);
  };
  if (c.active == wid && (it.held || it.pressed)) {
    double k = clamp(double((c.m.x - tr.x) / std::max(1.f, tr.w)), 0.0, 1.0);
    nv = snapv(mn + (mx - mn) * k);
  }
  if (it.focused && !isDisabled()) {
    double st = o.step > 0 ? o.step : (mx - mn) / 100;
    if (takeKey(Key::Right) || takeKey(Key::Up)) nv = snapv(nv + st);
    if (takeKey(Key::Left) || takeKey(Key::Down)) nv = snapv(nv - st);
    if (takeKey(Key::Right, platform::ModShift) || takeKey(Key::Up, platform::ModShift)) nv = snapv(nv + st * 10);
    if (takeKey(Key::Left, platform::ModShift) || takeKey(Key::Down, platform::ModShift)) nv = snapv(nv - st * 10);
    if (takeKey(Key::Home)) nv = mn;
    if (takeKey(Key::End)) nv = mx;
  }
  bool changed = nv != v;
  if (changed) v = nv;
  float k = float((v - mn) / (mx - mn));
  float hv = animate(wid ^ 0x511ull, (it.hovered || c.active == wid) ? 1.f : 0.f);
  Color tone = toneColor(o.tone);
  cmdRect(tr, t.track, 2);
  cmdRect(RectF{tr.x, tr.y, tr.w * k, tr.h}, tone, 2);
  float kx = tr.x + tr.w * k, ky = tr.cy();
  if (hv > 0.01f) draw::circle(kx, ky, 8 + 5 * hv, tone.alpha(0.18f * hv));
  cmdShadow(RectF{kx - 8, ky - 8, 16, 16}, 8, 4, Color(0, 0, 0, t.dark ? 110 : 45), 1);
  draw::circle(kx, ky, 8, t.dark ? Color::hex(0xf4f1ea) : Color(255, 255, 255));
  draw::ring(kx, ky, 7.25f, 1.5f, tone);
  if (o.showValue) textIn(fmt(v), RectF{tr.right() + 8, r.y, valW + 8, r.h}, vst, t.textDim, Align::Right);
  if (o.bubble && c.active == wid) {
    std::string s = fmt(v);
    const auto bst = styleWith(Font::Small, gfx::FontWeight::Semibold);
    float bw = textWidth(s, bst) + 14;
    RectF b{kx - bw * 0.5f, r.y - 26, bw, 22};
    cmdShadow(b, 6, 8, t.shadow, 2);
    cmdRect(b, t.dark ? Color::hex(0x2a323e) : Color::hex(0x2a2620), 6);
    textIn(s, b, bst, Color::hex(0xf4f1ea), Align::Center);
  }
  setLast(wid, r, it);
  c.last.changed = changed;
  c.last.active = c.active == wid;
  focusRing(RectF{kx - 8, ky - 8, 16, 16}, 8);
  if (it.hovered || c.active == wid) c.cursor = platform::Cursor::Hand;
  return changed;
}

// ================================================================ вкладки
static bool tabsImpl(std::string_view name, int& active, std::span<const Tab> items, const TabsOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  int n = int(items.size());
  if (n == 0) return false;
  bool pill = o.style == TabStyle::Pill;
  bool small = o.size == Size::Small;
  float h = pill ? (small ? 28.f : 32.f) : (small ? 34.f : 40.f);
  const gfx::TextStyle st = styleWith(small ? Font::Small : Font::Body, gfx::FontWeight::Semibold);
  float is = small ? 16 : 18;
  std::vector<float> nat(static_cast<size_t>(n));
  float total = pill ? 4 : 0;
  for (int i = 0; i < n; i++) {
    const Tab& tb = items[size_t(i)];
    std::string_view lb = displayText(tb.label);
    float w = (tb.icon ? is : 0) + (lb.empty() ? 0 : textWidth(lb, st) + (tb.icon ? 7 : 0));
    if (tb.badge > 0) w += textWidth(std::to_string(tb.badge), styleWith(Font::Caption, gfx::FontWeight::Semibold)) + 16;
    nat[size_t(i)] = w + (lb.empty() ? 20 : 24);
    total += nat[size_t(i)];
  }
  RectF r = place(o.fill ? 0 : total, h);
  WidgetId wid = id(name);
  registerId(wid);
  RectF inner = pill ? r.inset(2) : r;
  if (pill) {
    cmdRect(r, t.dark ? mixc(t.bg, t.surface1, 0.5f) : t.surface3, t.radiusField + 2);
    cmdStroke(r, t.dark ? t.border.alpha(0.7f) : t.border.alpha(0.6f), t.radiusField + 2, 1);
  } else {
    draw::line(r.x, r.bottom() - 0.5f, r.right(), r.bottom() - 0.5f, t.border, 1);
  }
  std::vector<float> xs(size_t(n) + 1, inner.x);
  for (int i = 0; i < n; i++) xs[size_t(i) + 1] = xs[size_t(i)] + (o.fill ? inner.w / float(n) : nat[size_t(i)]);
  int cur = clamp(active, 0, n - 1);
  bool changed = false;
  registerFocusable(wid);
  bool focused = c.focus == wid;
  if (focused) {
    if (takeKey(Key::Left) && cur > 0) { cur--; changed = true; }
    if (takeKey(Key::Right) && cur < n - 1) { cur++; changed = true; }
  }
  std::vector<float> hov(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) {
    RectF cell{xs[size_t(i)], inner.y, xs[size_t(i) + 1] - xs[size_t(i)], inner.h};
    WidgetId cid = hashMix(wid, u64(i) + 7);
    Interaction it = interact(cid, cell);
    if (it.pressed) {
      setFocus(wid, false);
      if (cur != i) { cur = i; changed = true; }
    }
    hov[size_t(i)] = animate(cid ^ 0x7ab1ull, it.hovered ? 1.f : 0.f);
    const Tab& tb = items[size_t(i)];
    if (it.hovered && (!tb.tooltip.empty() || !tb.label.empty())) {
      setLast(cid, cell, it);
      if (!tb.tooltip.empty()) tooltip(tb.tooltip);
      else if (displayText(tb.label).empty()) tooltip(tb.label);
    }
  }
  float ax = animate(wid ^ 0x7ab2ull, xs[size_t(cur)], 0.18f);
  float aw = animate(wid ^ 0x7ab3ull, xs[size_t(cur) + 1] - xs[size_t(cur)], 0.18f);
  if (pill) {
    RectF th{ax, inner.y, aw, inner.h};
    cmdShadow(th, t.radiusField, 4, Color(0, 0, 0, t.dark ? 90 : 26), 1);
    cmdRect(th, t.dark ? t.surface3.lighten(0.06f) : t.surface2, t.radiusField);
    if (t.dark) cmdStroke(th, Color(255, 255, 255, 14), t.radiusField, 1);
  }
  for (int i = 0; i < n; i++) {
    const Tab& tb = items[size_t(i)];
    RectF cell{xs[size_t(i)], inner.y, xs[size_t(i) + 1] - xs[size_t(i)], inner.h};
    bool on = i == cur;
    float hv = hov[size_t(i)];
    if (!pill && hv > 0.01f && !on) cmdRect(RectF{cell.x + 2, cell.y + 4, cell.w - 4, cell.h - 9}, t.hover.alpha(hv), 6);
    Color fg = on ? t.text : mixc(t.textMuted, t.text, hv * 0.8f);
    std::string_view lb = displayText(tb.label);
    std::string badgeTxt = tb.badge > 0 ? std::to_string(tb.badge) : std::string();
    const auto bst = styleWith(Font::Caption, gfx::FontWeight::Semibold);
    float bw = tb.badge > 0 ? textWidth(badgeTxt, bst) + 10 : 0;
    // Подпись укорачивается с многоточием, если вкладка узкая.
    float fixedW = (tb.icon ? is + (lb.empty() ? 0 : 7) : 0) + (bw > 0 ? bw + 6 : 0);
    float lw = lb.empty() ? 0 : std::max(0.f, std::min(textWidth(lb, st), cell.w - 16 - fixedW));
    float cw = fixedW + lw;
    float x = cell.x + std::round((cell.w - cw) * 0.5f);
    float cy = pill ? cell.cy() : cell.cy() - 1;
    if (tb.icon) {
      cmdIcon(tb.icon, RectF{x, cy - is * 0.5f, is, is}, on ? t.accent : fg);
      if (tb.badge < 0) {
        draw::circle(x + is + 1, cy - is * 0.5f + 1, 4, pill ? t.surface3 : t.surface1);
        draw::circle(x + is + 1, cy - is * 0.5f + 1, 2.8f, toneColor(tb.badgeTone));
      }
      x += is + (lb.empty() ? 0 : 7);
    }
    if (!lb.empty()) {
      textIn(lb, RectF{x, cell.y, lw + 1, cell.h}, st, fg);
      x += lw;
    }
    if (bw > 0) {
      x += 6;
      RectF b{x, cy - 8, bw, 16};
      Color tc = toneColor(tb.badgeTone);
      cmdRect(b, on ? tc : tc.alpha(0.18f), 8);
      textIn(badgeTxt, b, bst, on ? (tb.badgeTone == Tone::Accent ? t.onAccent : Color(255, 255, 255)) : tc, Align::Center);
    }
  }
  if (!pill) cmdRect(RectF{ax + 10, r.bottom() - 2, std::max(0.f, aw - 20), 2}, t.accent, 1);
  Interaction whole{};
  setLast(wid, r, whole);
  c.last.focused = focused;
  focusRing(r, 6);
  if (changed) {
    active = cur;
    c.last.changed = true;
  }
  return changed;
}

bool tabs(std::string_view name, int& active, std::initializer_list<Tab> items, const TabsOpt& o) {
  return tabsImpl(name, active, std::span<const Tab>(items.begin(), items.size()), o);
}
bool tabs(std::string_view name, int& active, std::span<const Tab> items, const TabsOpt& o) { return tabsImpl(name, active, items, o); }

// ================================================================ отображение
void badge(std::string_view txt, Tone tone, bool solid) {
  Ctx& c = C();
  const Theme& t = c.th;
  const auto st = styleWith(Font::Caption, gfx::FontWeight::Semibold);
  std::string_view s = displayText(txt);
  float w = std::max(18.f, textWidth(s, st) + 12);
  RectF r = place(w, 18);
  Color tc = tone == Tone::Neutral ? t.textDim : toneColor(tone);
  if (solid) {
    cmdRect(r, tc, 9);
    textIn(s, r, st, tone == Tone::Accent ? t.onAccent : Color(255, 255, 255), Align::Center);
  } else {
    cmdRect(r, tone == Tone::Neutral ? t.surface3 : tint(tc, t.dark ? 0.18f : 0.14f), 9);
    textIn(s, r, st, tc, Align::Center);
  }
  c.last = Item{};
  c.last.rect = r;
}

void tag(std::string_view txt, Tone tone, const char* icon) {
  Ctx& c = C();
  const Theme& t = c.th;
  const auto st = styleWith(Font::Small, gfx::FontWeight::Semibold);
  std::string_view s = displayText(txt);
  float w = textWidth(s, st) + 16 + (icon ? 18 : 0);
  RectF r = place(w, 22);
  Color tc = tone == Tone::Neutral ? t.textDim : toneColor(tone);
  if (tone == Tone::Neutral) {
    cmdRect(r, t.surface3, 6);
    cmdStroke(r, t.border, 6, 1);
  } else {
    cmdRect(r, tint(tc, t.dark ? 0.14f : 0.11f), 6);
    cmdStroke(r, tint(tc, 0.32f), 6, 1);
  }
  float x = r.x + 8;
  if (icon) {
    cmdIcon(icon, RectF{x, r.cy() - 7, 14, 14}, tc);
    x += 18;
  }
  textIn(s, RectF{x, r.y, r.right() - x - 6, r.h}, st, tone == Tone::Neutral ? t.text : tc);
  c.last = Item{};
  c.last.rect = r;
}

ChipAction chip(std::string_view lbl, const ChipOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  const auto& st = styleOf(Font::Small);
  std::string_view s = displayText(lbl);
  bool lead = o.icon || o.color.a > 0;
  float w = textWidth(s, st) + 20 + (lead ? 16 : 0) + (o.removable ? 18 : 0);
  RectF r = place(w, 26);
  if (r.h > 26) r = RectF{r.x, r.y + std::round((r.h - 26) * 0.5f), r.w, 26};
  WidgetId wid = id(lbl);
  bool interactive = o.clickable || o.removable || !o.tooltip.empty();
  // Крестик получает нажатие раньше фишки.
  RectF xr{r.right() - 22, r.cy() - 8, 16, 16};
  WidgetId xid = wid ^ 0xc42ull;
  Interaction xi{};
  if (o.removable) xi = interact(xid, xr);
  Interaction it{};
  if (interactive) it = interact(wid, r, o.clickable ? IfFocusable : IfNone);
  float hv = interactive ? animate(wid ^ 0xc41ull, it.hovered ? 1.f : 0.f) : 0.f;
  Color tc = o.tone == Tone::Neutral ? t.text : toneColor(o.tone);
  Color bg = o.tone == Tone::Neutral ? t.surface3 : tint(toneColor(o.tone), 0.14f);
  Color bc = o.tone == Tone::Neutral ? t.border : tint(toneColor(o.tone), 0.32f);
  if (o.selected) {
    bg = tint(t.accent, 0.14f);
    bc = t.accent.alpha(0.6f);
  }
  if (o.clickable) bc = mixc(bc, t.borderStrong, hv);
  cmdRect(r, bg, 13);
  cmdStroke(r, bc, 13, 1);
  float x = r.x + 10;
  if (o.color.a > 0) {
    draw::circle(x + 4, r.cy(), 4.5f, o.color);
    draw::ring(x + 4, r.cy(), 4.5f, 1, Color(0, 0, 0, t.dark ? 70 : 30));
    x += 14;
  } else if (o.icon) {
    cmdIcon(o.icon, RectF{x - 1, r.cy() - 7, 14, 14}, o.tone == Tone::Neutral ? t.textDim : tc);
    x += 16;
  }
  float right = r.right() - 10 - (o.removable ? 16 : 0);
  textIn(s, RectF{x, r.y, right - x, r.h}, st, tc);
  ChipAction act = ChipAction::None;
  if (o.removable) {
    float xh = animate(xid ^ 1, xi.hovered ? 1.f : 0.f);
    if (xh > 0.01f) draw::circle(xr.cx(), xr.cy(), 8, t.danger.alpha(0.18f * xh));
    cmdIcon("close", xr.inset(2.5f), mixc(t.textMuted, t.danger, xh));
    if (xi.clicked) act = ChipAction::Remove;
  }
  if (act == ChipAction::None && o.clickable && it.clicked) act = ChipAction::Click;
  if (o.clickable && it.hovered) c.cursor = platform::Cursor::Hand;
  setLast(wid, r, it);
  if (o.clickable) focusRing(r, 13);
  if (!o.tooltip.empty()) tooltip(o.tooltip);
  return act;
}

void progress(double v, const ProgressOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  double k = std::isfinite(v) ? clamp(v, 0.0, 1.0) : 0.0;
  std::string lbl = !o.text.empty() ? std::string(o.text) : (o.label ? fmtPct(k * 100) : std::string());
  const auto& st = styleOf(Font::Small);
  float lw = lbl.empty() ? 0 : std::max(36.f, textWidth(lbl, st) + 4);
  float h = std::max(o.height, lbl.empty() ? o.height : lineHeight(Font::Small));
  RectF r = place(o.width, h);
  RectF tr{r.x, r.cy() - o.height * 0.5f, r.w - (lw > 0 ? lw + 8 : 0), o.height};
  Color col = o.color.a ? o.color : toneColor(o.tone);
  cmdRect(tr, t.track, o.height * 0.5f);
  if (k > 0) {
    RectF fr{tr.x, tr.y, std::max(o.height, tr.w * float(k)), tr.h};
    draw::gradient(fr, col.lighten(0.08f), col, o.height * 0.5f, true);
  }
  if (lw > 0) textIn(lbl, RectF{tr.right() + 8, r.y, lw, r.h}, st, t.textDim, Align::Right);
  c.last = Item{};
  c.last.rect = r;
}

void meter(double value, const MeterOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  double mn = o.min, mx = o.max > o.min ? o.max : o.min + 1;
  double v = std::isfinite(value) ? clamp(value, mn, mx) : 0;
  std::string lbl = o.label ? fmtSigned(v) : std::string();
  const auto st = styleWith(Font::Small, gfx::FontWeight::Semibold);
  float lw = lbl.empty() ? 0 : std::max(34.f, textWidth(fmtSigned(mx), st) + 6);
  RectF r = place(o.width, std::max(o.height + 6, 18.f));
  RectF tr{r.x, r.cy() - o.height * 0.5f, r.w - (lw > 0 ? lw + 8 : 0), o.height};
  cmdRect(tr, t.track, o.height * 0.5f);
  float zero = clamp(float((0 - mn) / (mx - mn)), 0.f, 1.f);
  float zx = tr.x + tr.w * zero;
  float vx = tr.x + tr.w * float((v - mn) / (mx - mn));
  Color col = v >= 0 ? t.success : t.danger;
  if (std::fabs(vx - zx) > 0.5f) {
    RectF fr{std::min(zx, vx), tr.y, std::fabs(vx - zx), tr.h};
    bool pos = vx > zx;
    float rr = o.height * 0.5f;
    cmdRect4(fr, col, pos ? 0 : rr, pos ? rr : 0, pos ? rr : 0, pos ? 0 : rr);
  }
  cmdRect(RectF{zx - 1, r.cy() - o.height * 0.5f - 3, 2, o.height + 6}, t.textMuted, 1);
  if (lw > 0) textIn(lbl, RectF{tr.right() + 8, r.y, lw, r.h}, st, std::fabs(v) < 1e-9 ? t.textDim : col, Align::Right);
  c.last = Item{};
  c.last.rect = r;
}

static int pieImpl(std::span<const Slice> slices, const PieOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  float sz = std::max(16.f, o.size);
  float legendW = 0;
  const auto& lst = styleOf(Font::Small);
  double sum = 0;
  for (const Slice& s : slices) sum += std::max(0.0, s.value);
  if (o.legend) {
    for (const Slice& s : slices) legendW = std::max(legendW, textWidth(displayText(s.label), lst) + 60);
    legendW = std::min(legendW + 16, 240.f);
  }
  float lh = 22;
  float legendH = o.legend ? lh * float(slices.size()) : 0;
  RectF r = place(o.legend ? 0 : sz, std::max(sz, legendH));
  WidgetId wid = autoId("pie");
  float cx = r.x + sz * 0.5f, cy = r.y + r.h * 0.5f;
  float R = sz * 0.5f - 3;
  float inner = o.thickness > 0 ? std::max(0.f, R - o.thickness) : 0;
  // Наведённый сектор
  int hovered = -1;
  float dxm = c.m.x - cx, dym = c.m.y - cy;
  float dm = std::sqrt(dxm * dxm + dym * dym);
  bool over = c.win == c.hoveredWin && dm <= R + 3 && dm >= inner - 2 && currentClip().contains(c.m.x, c.m.y) && c.active == 0;
  double ang = std::atan2(dym, dxm) + kPi * 0.5;
  if (ang < 0) ang += 2 * kPi;
  if (sum <= 0) {
    draw::ring(cx, cy, (R + inner) * 0.5f, std::max(2.f, R - inner), t.track);
  } else {
    double a0 = 0;
    for (size_t i = 0; i < slices.size(); i++) {
      double frac = std::max(0.0, slices[i].value) / sum;
      double a1 = a0 + frac * 2 * kPi;
      if (over && ang >= a0 && ang < a1) hovered = int(i);
      a0 = a1;
    }
    a0 = 0;
    float gapPx = slices.size() > 1 ? 1.2f : 0;
    for (size_t i = 0; i < slices.size(); i++) {
      double frac = std::max(0.0, slices[i].value) / sum;
      double a1 = a0 + frac * 2 * kPi;
      if (frac <= 0) {
        a0 = a1;
        continue;
      }
      float grow = animate(hashMix(wid, i + 1), int(i) == hovered ? 1.f : 0.f, 0.12f);
      float Ro = R + 2.5f * grow, Ri = inner > 0 ? inner : 0;
      double ga = Ro > 0 ? gapPx / Ro : 0, gi = Ri > 0 ? gapPx / Ri : 0;
      double s0 = a0 + ga * 0.5, s1 = a1 - ga * 0.5;
      if (s1 <= s0) {
        s0 = a0;
        s1 = a1;
      }
      gfx::Path p;
      int segs = std::max(2, int((s1 - s0) / (kPi / 60)) + 1);
      for (int k = 0; k <= segs; k++) {
        double a = s0 + (s1 - s0) * k / segs - kPi * 0.5;
        float x = cx + Ro * float(std::cos(a)), y = cy + Ro * float(std::sin(a));
        if (k == 0) p.moveTo(x, y);
        else p.lineTo(x, y);
      }
      if (Ri > 0) {
        double i0 = a0 + gi * 0.5, i1 = a1 - gi * 0.5;
        if (i1 <= i0) {
          i0 = a0;
          i1 = a1;
        }
        for (int k = segs; k >= 0; k--) {
          double a = i0 + (i1 - i0) * k / segs - kPi * 0.5;
          p.lineTo(cx + Ri * float(std::cos(a)), cy + Ri * float(std::sin(a)));
        }
      } else {
        p.lineTo(cx, cy);
      }
      p.close();
      Color col = slices[i].color;
      if (hovered >= 0 && int(i) != hovered) col = col.alpha(0.55f);
      draw::path(p, col);
      a0 = a1;
    }
  }
  if (inner > 12 && (!o.centerValue.empty() || !o.centerLabel.empty())) {
    Font vf = inner > 34 ? Font::Title : Font::Strong;
    float vh = lineHeight(vf), ch = o.centerLabel.empty() ? 0 : lineHeight(Font::Caption);
    float y0 = cy - (vh + ch) * 0.5f;
    std::string hv;
    std::string_view cv = o.centerValue, cl = o.centerLabel;
    if (hovered >= 0 && sum > 0) {
      hv = fmtPct(std::max(0.0, slices[size_t(hovered)].value) / sum * 100);
      cv = hv;
      cl = displayText(slices[size_t(hovered)].label);
    }
    textIn(cv, RectF{cx - inner, y0, inner * 2, vh}, styleOf(vf), t.text, Align::Center);
    if (ch > 0 || !cl.empty()) textIn(cl, RectF{cx - inner + 6, y0 + vh, inner * 2 - 12, lineHeight(Font::Caption)}, styleOf(Font::Caption), t.textMuted, Align::Center);
  }
  if (o.legend && !slices.empty()) {
    float lx = r.x + sz + 16;
    float ly = r.y + (r.h - legendH) * 0.5f;
    float lwid = r.right() - lx;
    for (size_t i = 0; i < slices.size(); i++) {
      RectF row{lx, ly + lh * float(i), lwid, lh};
      bool hi = int(i) == hovered;
      if (hi) cmdRect(row.inset(-4, 0), t.hover, 5);
      draw::circle(row.x + 5, row.cy(), 4.5f, slices[i].color);
      std::string pct = sum > 0 ? fmtPct(std::max(0.0, slices[i].value) / sum * 100) : std::string("—");
      const auto pst = styleWith(Font::Small, gfx::FontWeight::Semibold);
      float pw = textWidth(pct, pst);
      textIn(displayText(slices[i].label), RectF{row.x + 16, row.y, row.w - 24 - pw, row.h}, lst, hi ? t.text : t.textDim);
      textIn(pct, RectF{row.right() - pw, row.y, pw, row.h}, pst, t.text, Align::Right);
    }
  }
  c.last = Item{};
  c.last.rect = r;
  if (hovered >= 0) {
    c.last.hovered = true;
    c.last.id = hashMix(wid, u64(hovered) + 100);
    if (o.tooltips && !o.legend) {
      std::string tip = std::string(displayText(slices[size_t(hovered)].label)) + " — " + fmtPct(std::max(0.0, slices[size_t(hovered)].value) / sum * 100);
      tooltip(tip);
    }
  }
  return hovered;
}

int pie(std::span<const Slice> slices, const PieOpt& o) { return pieImpl(slices, o); }
int pie(std::initializer_list<Slice> slices, const PieOpt& o) { return pieImpl(std::span<const Slice>(slices.begin(), slices.size()), o); }

void sparkline(std::span<const float> values, const SparkOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  RectF r = place(o.width, o.height);
  c.last = Item{};
  c.last.rect = r;
  if (values.size() < 2) {
    draw::line(r.x, r.cy(), r.right(), r.cy(), t.border, 1);
    return;
  }
  float mn = values[0], mx = values[0];
  for (float v : values) {
    if (!std::isfinite(v)) continue;
    mn = std::min(mn, v);
    mx = std::max(mx, v);
  }
  if (mx - mn < 1e-6f) {
    mn -= 1;
    mx += 1;
  }
  Color col = o.color.a ? o.color : t.accent;
  RectF a = r.inset(3, 3);
  auto P = [&](size_t i) {
    float v = std::isfinite(values[i]) ? values[i] : mn;
    return gfx::Pt{a.x + a.w * float(i) / float(values.size() - 1), a.bottom() - a.h * (v - mn) / (mx - mn)};
  };
  gfx::Path line;
  for (size_t i = 0; i < values.size(); i++) {
    gfx::Pt p = P(i);
    if (i == 0) line.moveTo(p.x, p.y);
    else line.lineTo(p.x, p.y);
  }
  if (o.fill) {
    gfx::Path area = line;
    area.lineTo(a.right(), r.bottom());
    area.lineTo(a.x, r.bottom());
    area.close();
    draw::path(area, col.alpha(0.14f));
  }
  draw::pathStroke(line, col, 1.6f);
  if (o.dot) {
    gfx::Pt p = P(values.size() - 1);
    draw::circle(p.x, p.y, 4.5f, col.alpha(0.25f));
    draw::circle(p.x, p.y, 2.5f, col);
  }
}

void stat(std::string_view value, std::string_view lbl, const StatOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  RectF r = place(0, 64);
  WidgetId wid = id(lbl) ^ 0x57a7ull;
  Interaction it{};
  if (!o.tooltip.empty()) it = interact(wid, r);
  float hv = o.tooltip.empty() ? 0.f : animate(wid ^ 1, it.hovered ? 1.f : 0.f);
  Color bg = t.dark ? Color(255, 255, 255, u8(7 + 5 * hv)) : mixc(t.surface3.alpha(0.55f), t.surface3, hv);
  Color bc = t.dark ? Color(255, 255, 255, 11) : t.border.alpha(0.8f);
  cmdRect(r, bg, t.radiusCard);
  cmdStroke(r, bc, t.radiusCard, 1);
  float x = r.x + 12;
  Color tc = toneColor(o.tone);
  if (o.icon) {
    float d = 36;
    RectF ic{x, r.cy() - d * 0.5f, d, d};
    cmdRect(ic, tc.alpha(t.dark ? 0.15f : 0.12f), 10);
    cmdIcon(o.icon, ic.inset(9), tc);
    x += d + 12;
  }
  const auto& vst = styleOf(Font::Number);
  // Мини-график — только если число и подпись помещаются целиком.
  float need = std::max(textWidth(value, vst), textWidth(displayText(lbl), styleOf(Font::Small)));
  float sparkW = o.spark.size() >= 2 ? std::min(72.f, r.right() - 12 - x - need - 12) : 0;
  if (sparkW < 40) sparkW = 0;
  float right = r.right() - 12 - (sparkW > 0 ? sparkW + 8 : 0);
  float vh = lineHeight(Font::Number), lh = lineHeight(Font::Small);
  float y0 = r.cy() - (vh + lh - 2) * 0.5f;
  float vw = std::min(textWidth(value, vst), right - x);
  textIn(value, RectF{x, y0, vw + 1, vh}, vst, t.text);
  bool hasDelta = o.delta != 0 || !o.deltaText.empty();
  if (hasDelta) {
    bool up = o.delta > 0 || (o.delta == 0 && !o.deltaText.empty() && o.deltaText[0] != '-' && !startsWith(o.deltaText, "−"));
    bool good = o.invertDelta ? !up : up;
    Color dc = o.delta == 0 && o.deltaText.empty() ? t.textMuted : (good ? t.success : t.danger);
    std::string dt = !o.deltaText.empty() ? std::string(o.deltaText) : fmtSigned(o.delta);
    const auto dst = styleWith(Font::Caption, gfx::FontWeight::Semibold);
    float dw = textWidth(dt, dst) + 22;
    float dx = x + vw + 8;
    if (dx + dw <= right + 4) {
      RectF dr{dx, y0 + (vh - 18) * 0.5f + 1, dw, 18};
      cmdRect(dr, dc.alpha(0.14f), 9);
      cmdIcon(up ? "trend-up" : "trend-down", RectF{dr.x + 5, dr.cy() - 6, 12, 12}, dc);
      textIn(dt, RectF{dr.x + 18, dr.y, dw - 20, dr.h}, dst, dc);
    }
  }
  textIn(displayText(lbl), RectF{x, y0 + vh - 2, right - x, lh}, styleOf(Font::Small), t.textDim);
  if (sparkW > 0) {
    RectF sr{r.right() - 12 - sparkW, r.cy() - 14, sparkW, 28};
    at(sr);
    SparkOpt so;
    so.height = 28;
    so.color = tc;
    sparkline(o.spark, so);
  }
  setLast(wid, r, it);
  if (!o.tooltip.empty()) tooltip(o.tooltip);
}

void avatar(std::string_view name, const AvatarOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  float s = std::max(12.f, o.size);
  float extra = o.ring ? 6 : 0;
  RectF r = place(s + extra, s + extra);
  RectF a{r.x + extra * 0.5f, r.y + std::round((r.h - s) * 0.5f), s, s};
  if (o.ring) draw::ring(a.cx(), a.cy(), s * 0.5f + 2.5f, 1.75f, t.accent);
  if (o.image && !o.image->empty()) {
    draw::image(*o.image, a, s * 0.5f);
  } else {
    Color bg = o.color.a ? o.color : Color::hsl(double(hash64(name) % 360), 0.32, t.dark ? 0.36 : 0.58);
    draw::circle(a.cx(), a.cy(), s * 0.5f, bg);
    std::string ini = initialsOf(name);
    gfx::TextStyle st = styleWith(Font::Body, gfx::FontWeight::Semibold);
    st.size = std::round(s * 0.4f);
    textIn(ini, a, st, bg.textOn(), Align::Center);
  }
  WidgetId wid = id(name) ^ 0xa7a7ull;
  Interaction it{};
  if (!o.tooltip.empty()) it = interact(wid, r);
  setLast(wid, r, it);
  if (!o.tooltip.empty()) tooltip(o.tooltip);
}

void flag(const Flag& f, float w, float h, float radius, std::string_view tip) {
  float fh = h > 0 ? h : std::round(w * 2.f / 3.f);
  RectF r = place(w, fh);
  if (r.w > w) r = RectF{r.x, r.y, w, r.h};
  if (r.h > fh) r = RectF{r.x, r.y + std::round((r.h - fh) * 0.5f), r.w, fh};
  const Flag* fp = &f;
  float rad = radius;
  float k = alphaMul();
  custom(r, [fp, rad, k](gfx::Canvas& cv, RectF d, float s) {
    cv.save();
    if (k < 1) cv.setOpacity(k);
    gfx::drawFlag(cv, *fp, d, rad * s, true);
    cv.restore();
  });
  WidgetId wid = autoId("flag");
  Interaction it{};
  if (!tip.empty()) it = interact(wid, r);
  setLast(wid, r, it);
  if (!tip.empty()) tooltip(tip);
}

bool emptyState(const char* icon, std::string_view txt, std::string_view action, const char* actionIcon) {
  Ctx& c = C();
  const Theme& t = c.th;
  RectF a = c.oneShot ? *c.oneShot : avail();
  float tw = std::min(a.w - 24, 300.f);
  TextEntry* te = cachedText(displayText(txt), styleOf(Font::Body), tw, 3, true, Align::Center, true);
  float h = 52 + 12 + te->layout.height + (action.empty() ? 0 : 12 + 30) + 16;
  RectF r = place(0, h);
  float cx = r.cx();
  float y = r.y + 8;
  draw::circle(cx, y + 26, 26, t.surface3);
  draw::ring(cx, y + 26, 25.5f, 1, t.border);
  cmdIcon(icon ? icon : "info", RectF{cx - 12, y + 14, 24, 24}, t.textMuted);
  y += 52 + 12;
  cmdText(te, r.x + (r.w - tw) * 0.5f, y, t.textDim);
  y += te->layout.height + 12;
  bool clicked = false;
  if (!action.empty()) {
    float bw = textWidth(displayText(action), styleOf(Font::Strong)) + 28 + (actionIcon ? 23 : 0);
    at(RectF{cx - bw * 0.5f, y, bw, 30});
    ButtonOpt bo;
    bo.icon = actionIcon;
    // Своя область ID: кнопка пустого состояния часто повторяет подпись кнопки над списком («Новая сделка»).
    IdScope es("##emptyState");
    clicked = button(action, bo);
  }
  c.last.rect = r;
  return clicked;
}

}  // namespace rg::ui

namespace rg::ui {

using namespace in;

bool listItem(std::string_view lbl, const ListItemOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  DisableGuard dg(o.disabled);
  bool disabled = isDisabled();
  float h = o.subtitle.empty() ? 32.f : 44.f;
  RectF r = place(0, h);
  WidgetId wid = id(lbl);
  Interaction it = interact(wid, r, disabled ? IfNone : IfFocusable);
  float hv = animate(wid ^ 0x115e1ull, it.hovered ? 1.f : 0.f, 0.1f);
  float rad = 7;
  if (o.selected) {
    cmdRect(r, t.accent.alpha(t.dark ? 0.14f : 0.12f), rad);
    cmdRect4(RectF{r.x, r.y + 7, 3, r.h - 14}, t.accent, 0, 2, 2, 0);
  } else if (hv > 0.01f) {
    cmdRect(r, (it.held ? t.pressed : t.hover).alpha(std::min(1.f, hv * 1.4f)), rad);
  }
  float x = r.x + 12;
  if (o.dot.a > 0) {
    draw::circle(x + 5, r.cy(), 5, o.dot);
    draw::ring(x + 5, r.cy(), 5, 1, Color(0, 0, 0, t.dark ? 80 : 35));
    x += 20;
  } else if (o.icon) {
    cmdIcon(o.icon, RectF{x, r.cy() - 8, 16, 16}, o.selected ? t.accent : mixc(t.textDim, t.text, hv));
    x += 26;
  }
  float right = r.right() - 10;
  if (!o.badge.empty()) {
    const auto bs = styleWith(Font::Caption, gfx::FontWeight::Semibold);
    float bw = std::max(18.f, textWidth(o.badge, bs) + 12);
    RectF br{right - bw, r.cy() - 9, bw, 18};
    cmdRect(br, o.selected ? t.accent.alpha(0.22f) : t.surface3, 9);
    textIn(o.badge, br, bs, o.selected ? t.accent : t.textDim, Align::Center);
    right = br.x - 8;
  }
  if (!o.hint.empty()) {
    const auto& hs = styleOf(Font::Small);
    float hw = std::min(textWidth(o.hint, hs), (right - x) * 0.45f);
    textIn(o.hint, RectF{right - hw, r.y, hw, r.h}, hs, t.textMuted, Align::Right);
    right -= hw + 10;
  }
  const gfx::TextStyle st = o.selected ? styleWith(Font::Body, gfx::FontWeight::Semibold) : styleOf(Font::Body);
  if (o.subtitle.empty()) {
    textIn(displayText(lbl), RectF{x, r.y, right - x, r.h}, st, disabled ? t.textMuted : t.text);
  } else {
    float lh = lineHeight(Font::Body), sh = lineHeight(Font::Small);
    float y0 = r.cy() - (lh + sh) * 0.5f;
    textIn(displayText(lbl), RectF{x, y0, right - x, lh}, st, disabled ? t.textMuted : t.text);
    textIn(o.subtitle, RectF{x, y0 + lh, right - x, sh}, styleOf(Font::Small), t.textMuted);
  }
  setLast(wid, r, it);
  focusRing(r, rad);
  if (!o.tooltip.empty()) tooltip(o.tooltip);
  return it.clicked && !disabled;
}

void spinner(float size, Tone tone) {
  Ctx& c = C();
  float s = std::max(8.f, size);
  RectF r = place(s, s);
  c.last = Item{};
  c.last.rect = r;
  float cx = r.cx(), cy = r.cy(), rad = s * 0.5f - 1.5f;
  float w = std::max(1.5f, s / 9.f);
  draw::ring(cx, cy, rad, w, c.th.track);
  double a0 = std::fmod(c.time * 2 * kPi * 0.9, 2 * kPi);
  double sweep = kPi * (0.55 + 0.35 * std::sin(c.time * 2.4));
  gfx::Path p;
  int n = 24;
  for (int i = 0; i <= n; i++) {
    double a = a0 + sweep * i / n;
    float x = cx + rad * float(std::cos(a)), y = cy + rad * float(std::sin(a));
    if (i == 0) p.moveTo(x, y);
    else p.lineTo(x, y);
  }
  draw::pathStroke(p, toneColor(tone), w);
  c.animating = true;
}

}  // namespace rg::ui
