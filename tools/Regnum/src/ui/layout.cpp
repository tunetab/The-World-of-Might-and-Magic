// Regnum — раскладка: поток контейнера, строки со столбцами, горизонтальные ряды, группы, панели, карточки,
// разделы, прокрутка, разделитель областей, виртуальный список.
#include "ui/ui_internal.h"

namespace rg::ui {

using namespace in;

namespace in {

namespace {

struct ScrollState {
  float y = 0, ty = 0, x = 0, tx = 0;
  float contentH = 0, contentW = 0;
  RectF view;
  double activity = -10;
  float dragOff = 0;
  bool init = false;
};

struct StackMeasure {
  float total = 0, rest = 0;
};

constexpr float kInfH = 1e7f;

bool finite(float h) { return h < kInfH * 0.5f; }

void newLine(Frame& f) {
  f.rowTop += (f.rowAuto ? f.rowMax : f.rowH) + C().th.gap;
  f.col = 0;
  f.rowMax = 0;
}

}  // namespace

Frame& frame() {
  Ctx& c = C();
  if (!c.frames.empty()) return c.frames.back();
  // Вызов виджета вне beginFrame/endFrame: работаем с пустым кадром (ничего не рисуется), один раз пишем в журнал.
  static bool warned = false;
  if (!warned) logWarn("ui: виджет вызван вне кадра (beginFrame/endFrame)");
  warned = true;
  static Frame dummy;
  dummy = Frame{};
  return dummy;
}

RectF currentClip() {
  Ctx& c = C();
  return c.frames.empty() ? c.view : c.frames.back().clip;
}

void pushFrame(const Frame& f) {
  Ctx& c = C();
  c.frames.push_back(f);
  if (c.frames.back().surface.a == 0) c.frames.back().surface = c.frames.size() > 1 ? c.frames[c.frames.size() - 2].surface : c.th.bg;
}

void popFrame() {
  Ctx& c = C();
  if (c.frames.size() <= 1) {
    logWarn("ui: лишнее закрытие контейнера");
    return;
  }
  Frame& f = c.frames.back();
  if (f.pushedClip && f.win) {
    f.win->list.cmds.emplace_back();
    f.win->list.cmds.back().op = Op::ClipPop;
  }
  c.frames.pop_back();
}

void pushClipFrame(RectF clip) {
  Frame& f = frame();
  f.clip = f.clip.intersect(clip);
  if (!f.win) return;
  Cmd k;
  k.op = Op::ClipPush;
  k.r = f.clip;
  f.win->list.cmds.push_back(k);
  f.pushedClip = true;
}

WidgetId autoId(const char* kind) {
  Ctx& c = C();
  return hashMix(c.idStack.back(), hash64(kind) + 0x9e37u * (++c.autoSeq));
}

RectF slotBegin(float w, float h, bool* oneShot) {
  Ctx& c = C();
  if (oneShot) *oneShot = false;
  if (c.oneShot) {
    RectF r = *c.oneShot;
    Align a = c.oneShotAlign;
    c.oneShot.reset();
    c.oneShotAlign = Align::Left;
    if (oneShot) *oneShot = true;
    if (w > 0 && w < r.w) {
      if (a == Align::Center) r.x += (r.w - w) * 0.5f;
      else if (a == Align::Right) r.x = r.right() - w;
      r.w = w;
    }
    if (h > 0 && h < r.h) {
      r.y += std::round((r.h - h) * 0.5f);
      r.h = h;
    }
    return r;
  }
  Frame& f = frame();
  f.placed = true;
  switch (f.flow) {
    case Flow::Vertical: {
      float x = f.box.x + f.indent, aw = std::max(0.f, f.box.w - f.indent);
      float rw = w > 0 ? std::min(w, aw) : aw;
      return {x, f.y, rw, h};
    }
    case Flow::Row: {
      if (f.ncols <= 0) return {f.box.x, f.rowTop, f.box.w, h};
      if (f.col >= f.ncols) newLine(f);
      float cx = f.colX[size_t(f.col)], cw = f.colW[size_t(f.col)];
      RectF r{cx, f.rowTop, cw, f.rowAuto ? h : f.rowH};
      if (w > 0 && w < cw) {
        if (f.slotAlign == Align::Center) r.x += (cw - w) * 0.5f;
        else if (f.slotAlign == Align::Right) r.x = r.right() - w;
        r.w = w;
      }
      if (!f.rowAuto && h > 0 && h < f.rowH) {
        r.y += std::round((f.rowH - h) * 0.5f);
        r.h = h;
      }
      return r;
    }
    case Flow::HStack: {
      float rw = w > 0 ? w : 160;
      RectF r{f.x, f.y, rw, f.stackH};
      if (h > 0 && h < f.stackH) {
        r.y += std::round((f.stackH - h) * 0.5f);
        r.h = h;
      }
      return r;
    }
  }
  return {};
}

void slotEnd(RectF used) {
  Frame& f = frame();
  switch (f.flow) {
    case Flow::Vertical:
      f.y = used.bottom() + f.gap;
      f.bottom = std::max(f.bottom, used.bottom());
      f.right = std::max(f.right, used.right());
      break;
    case Flow::Row:
      if (f.rowAuto) f.rowMax = std::max(f.rowMax, used.bottom() - f.rowTop);
      f.bottom = std::max(f.bottom, f.rowAuto ? used.bottom() : f.rowTop + f.rowH);
      f.right = std::max(f.right, used.right());
      f.col++;
      break;
    case Flow::HStack:
      f.x = used.right() + f.gap;
      f.right = std::max(f.right, used.right());
      f.bottom = std::max(f.bottom, used.bottom());
      break;
  }
}

RectF place(float w, float h) {
  bool os = false;
  RectF r = slotBegin(w, h, &os);
  if (!os) slotEnd(r);
  return r;
}

void layoutBeginFrame() {
  Ctx& c = C();
  c.frames.clear();
  c.oneShot.reset();
  c.autoSeq = 0;
  Frame f;
  f.win = c.root;
  f.box = c.view;
  f.x = c.view.x;
  f.y = c.view.y;
  f.bottom = c.view.y;
  f.gap = c.th.gap;
  f.clip = c.view;
  f.surface = c.th.bg;
  c.frames.push_back(f);
}

void layoutEndFrame() {
  Ctx& c = C();
  if (c.frames.size() > 1) logWarn("ui: незакрытых контейнеров в конце кадра: %d", int(c.frames.size() - 1));
  while (c.frames.size() > 1) popFrame();
  if (c.idStack.size() > 1) logWarn("ui: pushId без popId: %d", int(c.idStack.size() - 1));
  if (c.disabled > 0) c.disabled = 0;
  c.winStack.resize(std::min<size_t>(c.winStack.size(), 1));
  c.win = c.root;
}

}  // namespace in

// ================================================================ поток
RectF avail() {
  Frame& f = frame();
  switch (f.flow) {
    case Flow::Vertical: {
      float h = finite(f.box.h) ? std::max(0.f, f.box.bottom() - f.y) : kInfH;
      return {f.box.x + f.indent, f.y, std::max(0.f, f.box.w - f.indent), h};
    }
    case Flow::Row: {
      if (f.ncols <= 0) return {f.box.x, f.rowTop, f.box.w, f.rowH};
      int col = f.col >= f.ncols ? 0 : f.col;
      return {f.colX[size_t(col)], f.rowTop, f.colW[size_t(col)], f.rowAuto ? kInfH : f.rowH};
    }
    case Flow::HStack:
      return {f.x, f.y, std::max(0.f, f.box.right() - f.x), f.stackH};
  }
  return {};
}

RectF next(float h) { return place(0, h); }
RectF next(float w, float h) { return place(w, h); }
void at(RectF r) {
  C().oneShot = r;
  C().oneShotAlign = Align::Left;
}

void spacer(float h) {
  Frame& f = frame();
  if (f.flow == Flow::Vertical) {
    f.y += h;
    f.bottom = std::max(f.bottom, f.y - f.gap);
  } else if (f.flow == Flow::HStack) {
    f.x += h;
  } else {
    place(0, h);
  }
}

void gap(float g) { frame().gap = std::max(0.f, g); }

// ================================================================ Area
Area::Area(RectF r, float pad) {
  Ctx& c = C();
  Frame f;
  f.win = c.win;
  f.kind = Frame::AreaK;
  f.box = r.inset(pad);
  f.x = f.box.x;
  f.y = f.box.y;
  f.bottom = f.box.y;
  f.gap = c.th.gap;
  f.clip = currentClip();
  pushFrame(f);
}

Area::~Area() { popFrame(); }

// ================================================================ Panel
Panel::Panel(std::string_view name, RectF r, const PanelOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  WidgetId wid = id(name);
  Window* w = beginWindow(wid, WinKind::Panel, kRankBase, ++c.panelSeq, r);
  c.idStack.push_back(wid);   // панель — область ID
  float rad = o.radius < 0 ? t.radiusPanel : o.radius;
  if (o.shadow) {
    cmdShadow(r, rad, 36, t.shadow, 14);
    cmdShadow(r, rad, 3, t.shadow.alpha(0.5f), 1);
  }
  if (o.glass) {
    w->blur = true;
    w->blurRadius = 22;
    cmdRect(r, t.surface1.alpha(0.82f), rad);
  } else {
    cmdRect(r, t.surface1, rad);
  }
  if (o.border) {
    cmdStroke(r, t.dark ? t.border : t.border.alpha(0.9f), rad, 1);
    if (t.dark) cmdStroke(r.inset(1), Color(255, 255, 255, 6), rad - 1, 1);
  }
  Frame f;
  f.win = w;
  f.kind = Frame::PanelK;
  f.surface = t.surface1;
  f.box = r.inset(o.pad);
  f.x = f.box.x;
  f.y = f.box.y;
  f.bottom = f.box.y;
  f.gap = t.gap;
  f.clip = r;
  pushFrame(f);
  pushClipFrame(r);
}

Panel::~Panel() {
  popFrame();
  popId();
  endWindow();
}

// ================================================================ Card
namespace {

void cardBackground(Frame& f) {
  f.marks[0] = cmdMark();
  f.marks[1] = cmdMark();
  f.marks[2] = cmdMark();
}

void cardPatch(const Frame& f, RectF r, bool hovered, Tone tone) {
  const Theme& t = C().th;
  float rad = t.radiusCard;
  if (t.dark) cmdPatchShadow(f.marks[0], r, rad, 6, Color(0, 0, 0, 60), 2);
  else cmdPatchShadow(f.marks[0], r, rad, 8, t.shadow.alpha(0.7f), 2);
  cmdPatchRect(f.marks[1], r, t.surface2, rad);
  Color bc = hovered ? t.borderStrong : (t.dark ? t.border.alpha(0.85f) : t.border);
  cmdPatchStroke(f.marks[2], r, bc, rad, 1);
  if (tone != Tone::Neutral) cmdRect4(RectF{r.x, r.y, 3, r.h}, toneColor(tone), rad, 0, 0, rad);
}

}  // namespace

Card::Card(const CardOpt& o) {
  Ctx& c = C();
  bool os = false;
  RectF r0 = slotBegin(0, 0, &os);
  Frame f;
  f.win = c.win;
  f.kind = Frame::CardK;
  f.surface = c.th.surface2;
  f.slot = r0;
  f.slotOneShot = os;
  f.pad = o.pad;
  f.t = float(int(o.tone));
  f.card = o.hoverable;
  cardBackground(f);
  float top = r0.y + o.pad;
  if (!o.title.empty() || o.icon) {
    float x = r0.x + o.pad;
    if (o.icon) {
      cmdIcon(o.icon, RectF{x, top + 1, 16, 16}, c.th.textDim);
      x += 22;
    }
    textIn(o.title, RectF{x, top, r0.right() - o.pad - x, 18}, styleOf(Font::Strong), c.th.text);
    top += 18 + 10;
  }
  f.box = RectF{r0.x + o.pad, top, std::max(0.f, r0.w - 2 * o.pad), kInfH};
  f.x = f.box.x;
  f.y = top;
  f.bottom = top;
  f.gap = c.th.gap;
  f.clip = currentClip();
  f.bodyTop = top;
  pushFrame(f);
}

Card::~Card() {
  Frame f = frame();
  popFrame();
  float bottom = f.placed ? f.bottom : f.bodyTop;
  RectF r{f.slot.x, f.slot.y, f.slot.w, bottom + f.pad - f.slot.y};
  Ctx& c = C();
  bool hovered = c.win == c.hoveredWin && r.contains(c.m.x, c.m.y) && currentClip().contains(c.m.x, c.m.y) && c.active == 0;
  cardPatch(f, r, hovered && f.card, Tone(int(f.t)));
  if (!f.slotOneShot) slotEnd(r);
  // Карточка — последний элемент: к ней можно привязать подсказку или цель перетаскивания.
  c.last = Item{};
  c.last.rect = r;
  c.last.hovered = hovered;
  c.last.id = autoId("card");
}

// ================================================================ Section
Section::Section(std::string_view title, const char* icon, const SectionOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  WidgetId sid = id(title);
  auto st = c.sticky.find(sid);
  bool open = st == c.sticky.end() ? o.defaultOpen : st->second > 0.5f;
  bool os = false;
  RectF r0 = slotBegin(0, 0, &os);
  Frame f;
  f.win = c.win;
  f.kind = Frame::SectionK;
  if (o.card) f.surface = t.surface2;
  f.slot = r0;
  f.slotOneShot = os;
  f.cid = sid;
  f.card = o.card;
  f.pad = o.card ? t.padCard : 0;
  if (o.card) cardBackground(f);
  float hh = o.card ? 40 : 32;
  RectF header{r0.x, r0.y, r0.w, hh};
  // Кнопка заголовка получает нажатие раньше самого заголовка.
  RectF actR{header.right() - (o.card ? t.padCard - 4 : 0) - 26, header.cy() - 13, 26, 26};
  Interaction ai{};
  WidgetId aid = sid ^ 0x5ec7a01ull;
  if (o.actionIcon) {
    ai = interact(aid, actR, IfFocusable);
    action_ = ai.clicked;
  }
  Interaction it = interact(sid, header, IfFocusable);
  if (it.clicked) {
    open = !open;
    c.sticky[sid] = open ? 1.f : 0.f;
  } else if (st == c.sticky.end()) {
    c.sticky[sid] = open ? 1.f : 0.f;
  }
  setLast(sid, header, it);
  float tt = animate(sid ^ 0x5ec7105ull, open ? 1.f : 0.f, 0.18f);
  // Высота содержимого ещё не известна (раздел впервые раскрывается) — без анимации.
  if (open && tt < 1 && c.sticky[sid ^ 0x5ec7107ull] <= 0) {
    tt = 1;
    c.anims[sid ^ 0x5ec7105ull] = Anim{1, 1, 1, c.time, 0, c.frame};
  }
  float hov = animate(sid ^ 0x5ec7106ull, it.hovered ? 1.f : 0.f);
  if (hov > 0.01f) {
    float rad = o.card ? t.radiusCard : 6;
    RectF hr = o.card ? header : header.inset(-4, 0);
    Color hc = t.hover.alpha(hov);
    float br = o.card ? rad * (1 - tt) : rad;
    cmdRect4(hr, hc, rad, rad, br, br);
  }
  if (!o.card) draw::line(r0.x, header.bottom(), r0.right(), header.bottom(), t.border, 1);
  float x = r0.x + (o.card ? t.padCard : 0);
  drawChevron(RectF{x, header.cy() - 7, 14, 14}, t.textMuted, -90 + 90 * tt);
  x += 20;
  if (icon) {
    cmdIcon(icon, RectF{x, header.cy() - 8, 16, 16}, mixc(t.textDim, t.text, hov));
    x += 24;
  }
  float right = header.right() - (o.card ? t.padCard : 0);
  if (o.actionIcon) {
    float ah = animate(aid ^ 1, ai.hovered ? 1.f : 0.f);
    if (ah > 0.01f) cmdRect(actR, t.hover.alpha(std::min(1.f, ah * 1.6f)), 6);
    cmdIcon(o.actionIcon, RectF{actR.x + 5, actR.y + 5, 16, 16}, mixc(t.textDim, t.text, ah));
    Item keep = c.last;
    setLast(aid, actR, ai);
    focusRing(actR, 6);
    tooltip(o.actionTooltip);
    c.last = keep;
    right = actR.x - 6;
  }
  if (!o.badge.empty()) {
    const auto& bs = styleWith(Font::Caption, gfx::FontWeight::Semibold);
    float bw = textWidth(o.badge, bs) + 14;
    RectF br{right - bw, header.cy() - 9, bw, 18};
    cmdRect(br, t.surface3, 9);
    textIn(o.badge, br, bs, t.textDim, Align::Center);
    right = br.x - 8;
  }
  if (o.card) textIn(displayText(title), RectF{x, header.y, right - x, hh}, styleOf(Font::Strong), t.text);
  else textIn(utf8::upper(displayText(title)), RectF{x, header.y, right - x, hh}, styleWith(Font::Caption, gfx::FontWeight::Semibold), t.textDim);
  focusRing(header, o.card ? t.radiusCard : 6);
  f.headerH = hh;
  f.t = tt;
  open_ = open || tt > 0.001f;
  began_ = true;
  float bodyTop = header.bottom() + (o.card ? 0 : 4);
  f.box = RectF{r0.x + f.pad, bodyTop, std::max(0.f, r0.w - 2 * f.pad), kInfH};
  f.x = f.box.x;
  f.y = bodyTop;
  f.bottom = bodyTop;
  f.gap = t.gap;
  f.bodyTop = bodyTop;
  f.clip = currentClip();
  f.placed = false;
  pushFrame(f);
  if (open_ && tt < 0.999f) {
    float prevH = c.sticky[sid ^ 0x5ec7107ull];
    pushClipFrame(RectF{r0.x - 4, bodyTop, r0.w + 8, prevH * tt + 1});
  }
}

Section::~Section() {
  if (!began_) return;
  Frame f = frame();
  popFrame();
  float contentH = 0;
  if (open_) {
    contentH = (f.placed ? f.bottom : f.bodyTop) - f.bodyTop + (f.placed ? f.pad : 0);
    if (!f.card && f.placed) contentH += 4;
    C().sticky[f.cid ^ 0x5ec7107ull] = contentH;   // высота содержимого — для плавного раскрытия
  }
  float h = f.headerH + contentH * f.t;
  RectF r{f.slot.x, f.slot.y, f.slot.w, h};
  if (f.card) cardPatch(f, r, false, Tone::Neutral);
  if (!f.slotOneShot) slotEnd(r);
}

// ================================================================ Scroll
Scroll::Scroll(std::string_view name, float height, const ScrollOpt& o) : id_(id(name)) {
  Ctx& c = C();
  ScrollState& s = state<ScrollState>(id_);
  Frame& pf = frame();
  float h = height;
  if (h <= 0) {
    if (pf.flow == Flow::Vertical && finite(pf.box.h)) h = std::max(40.f, pf.box.bottom() - pf.y);
    else if (pf.flow == Flow::Row && !pf.rowAuto) h = pf.rowH;
    else h = 240;
  }
  bool os = false;
  RectF view = slotBegin(0, h, &os);
  view.h = h;
  float maxY = std::max(0.f, s.contentH - view.h);
  s.ty = clamp(s.ty, 0.f, maxY);
  s.y = clamp(s.y, 0.f, maxY);
  float maxX = std::max(0.f, s.contentW - view.w);
  s.tx = clamp(s.tx, 0.f, maxX);
  s.x = clamp(s.x, 0.f, maxX);
  bool overflow = maxY > 0.5f;
  float barW = overflow ? 10 : 0;
  Frame f;
  f.win = c.win;
  f.kind = Frame::ScrollK;
  f.slot = view;
  f.slotOneShot = os;
  f.cid = id_;
  f.pad = o.pad;
  f.horizontal = o.horizontal;
  float sy = std::round(s.y * c.ds) / c.ds;
  float sx = std::round(s.x * c.ds) / c.ds;
  float cw = o.horizontal ? std::max(o.contentWidth > 0 ? o.contentWidth : view.w - 2 * o.pad - barW, 0.f) : view.w - 2 * o.pad - barW;
  f.box = RectF{view.x + o.pad - (o.horizontal ? sx : 0), view.y + o.pad - sy, std::max(0.f, cw), kInfH};
  f.x = f.box.x;
  f.y = f.box.y;
  f.bottom = f.box.y;
  f.gap = c.th.gap;
  f.clip = currentClip();
  f.scrollId = id_;
  f.scrollY = s.y;
  f.bodyTop = f.box.y - o.pad;
  pushFrame(f);
  pushClipFrame(view);
}

float Scroll::offset() const { return state<ScrollState>(id_).y; }

void Scroll::scrollTo(float y, bool smooth) {
  ScrollState& s = state<ScrollState>(id_);
  s.ty = std::max(0.f, y);
  if (!smooth) s.y = s.ty;
  requestRedraw();
}

Scroll::~Scroll() {
  Ctx& c = C();
  const Theme& t = c.th;
  Frame f = frame();
  popFrame();
  ScrollState& s = state<ScrollState>(id_);
  RectF view = f.slot;
  float contentH = f.placed ? f.bottom - f.bodyTop + f.pad : 0;
  float contentW = f.placed ? f.right - (f.box.x - f.pad) + f.pad : 0;
  s.contentH = contentH;
  s.contentW = f.horizontal ? contentW : view.w;
  s.view = view;
  float maxY = std::max(0.f, contentH - view.h);
  float maxX = f.horizontal ? std::max(0.f, contentW - view.w) : 0;
  RectF clip = currentClip();
  bool overView = c.win && c.win == c.hoveredWin && view.contains(c.m.x, c.m.y) && clip.contains(c.m.x, c.m.y);
  // Колесо: внутренняя область поглощает, если может прокрутиться в эту сторону.
  float wy = c.m.wheelY, wx = c.m.wheelX;
  if (f.horizontal && (c.m.mods & platform::ModShift) && wx == 0) {
    wx = wy;
    wy = 0;
  }
  if (overView && !c.drag.active) {
    if (wy != 0 && ((wy > 0 && s.ty > 0) || (wy < 0 && s.ty < maxY))) {
      s.ty = clamp(s.ty - wy, 0.f, maxY);
      if (c.wheelPrecise) s.y = s.ty;
      c.m.wheelY = 0;
      s.activity = c.time;
    }
    if (f.horizontal && wx != 0 && ((wx > 0 && s.tx > 0) || (wx < 0 && s.tx < maxX))) {
      s.tx = clamp(s.tx - wx, 0.f, maxX);
      if (c.wheelPrecise) s.x = s.tx;
      c.m.wheelX = 0;
      if (wy == 0 && (c.m.mods & platform::ModShift)) c.m.wheelY = 0;
      s.activity = c.time;
    }
  }
  s.ty = clamp(s.ty, 0.f, maxY);
  s.tx = clamp(s.tx, 0.f, maxX);
  auto smooth = [&](float& v, float target) {
    if (v == target) return;
    float k = 1 - std::exp(-c.dt * 20);
    v += (target - v) * k;
    if (std::fabs(v - target) < 0.5f) v = target;
    requestRedraw();
  };
  smooth(s.y, s.ty);
  smooth(s.x, s.tx);
  // Затухание у краёв: видно, что содержимое продолжается.
  if (maxY > 0.5f) {
    Color sc = f.surface;
    float fh = std::min(18.f, view.h * 0.25f);
    if (s.y > 0.5f) draw::gradient(RectF{view.x, view.y, view.w, fh}, sc, sc.withA(0));
    if (s.y < maxY - 0.5f) draw::gradient(RectF{view.x, view.bottom() - fh, view.w, fh}, sc.withA(0), sc);
  }
  // Полоса прокрутки: тонкая, расширяется при наведении, скрывается без движения.
  if (maxY > 0.5f) {
    RectF track{view.right() - 10, view.y + 2, 10, view.h - 4};
    float thumbH = std::max(28.f, track.h * view.h / std::max(contentH, 1.f));
    float thumbY = track.y + (track.h - thumbH) * (s.y / maxY);
    RectF thumb{track.x, thumbY, track.w, thumbH};
    Interaction ti = interact(id_ ^ 0x7b1ull, thumb);
    if (ti.pressed) s.dragOff = c.m.y - thumbY;
    if (c.active == (id_ ^ 0x7b1ull)) {
      float ny = c.m.y - s.dragOff;
      float k = (ny - track.y) / std::max(1.f, track.h - thumbH);
      s.ty = s.y = clamp(k, 0.f, 1.f) * maxY;
      s.activity = c.time;
    }
    Interaction tr = interact(id_ ^ 0x7b2ull, track);
    if (tr.pressed && !thumb.contains(c.m.x, c.m.y)) {
      s.ty = clamp(s.ty + (c.m.y < thumbY ? -view.h * 0.9f : view.h * 0.9f), 0.f, maxY);
      s.activity = c.time;
    }
    bool hotBar = ti.hovered || tr.hovered || c.active == (id_ ^ 0x7b1ull);
    bool show = overView || hotBar || c.time - s.activity < 0.9;
    if (c.time - s.activity < 0.9) requestRedraw();
    float alpha = animate(id_ ^ 0x7b3ull, show ? 1.f : 0.f, 0.2f);
    float w = animate(id_ ^ 0x7b4ull, hotBar ? 8.f : 4.f, 0.12f);
    if (alpha > 0.01f) {
      RectF tv{track.right() - w - 2, thumbY, w, thumbH};
      Color col = hotBar ? t.textDim.alpha(0.65f) : t.textMuted.alpha(0.55f);
      cmdRect(tv, col.alpha(alpha), w * 0.5f);
    }
  }
  if (maxX > 0.5f) {
    RectF track{view.x + 2, view.bottom() - 10, view.w - 4 - (maxY > 0.5f ? 10 : 0), 10};
    float thumbW = std::max(28.f, track.w * view.w / std::max(contentW, 1.f));
    float thumbX = track.x + (track.w - thumbW) * (s.x / maxX);
    RectF thumb{thumbX, track.y, thumbW, track.h};
    Interaction ti = interact(id_ ^ 0x7c1ull, thumb);
    if (ti.pressed) s.dragOff = c.m.x - thumbX;
    if (c.active == (id_ ^ 0x7c1ull)) {
      float k = (c.m.x - s.dragOff - track.x) / std::max(1.f, track.w - thumbW);
      s.tx = s.x = clamp(k, 0.f, 1.f) * maxX;
      s.activity = c.time;
    }
    bool hotBar = ti.hovered || c.active == (id_ ^ 0x7c1ull);
    float alpha = animate(id_ ^ 0x7c3ull, (overView || hotBar || c.time - s.activity < 0.9) ? 1.f : 0.f, 0.2f);
    float h = animate(id_ ^ 0x7c4ull, hotBar ? 8.f : 4.f, 0.12f);
    if (alpha > 0.01f) cmdRect(RectF{thumbX, track.bottom() - h - 2, thumbW, h}, t.textMuted.alpha(0.55f * alpha), h * 0.5f);
  }
  if (!f.slotOneShot) slotEnd(view);
}

void scrollToItem() {
  Ctx& c = C();
  RectF item = c.last.rect;
  for (size_t i = c.frames.size(); i-- > 0;) {
    const Frame& f = c.frames[i];
    if (f.kind != Frame::ScrollK) continue;
    ScrollState& s = state<ScrollState>(f.cid);
    RectF view = f.slot;
    float margin = 8;
    float ty = s.ty;
    if (item.y < view.y + margin) ty = std::max(0.f, s.ty - (view.y + margin - item.y));
    else if (item.bottom() > view.bottom() - margin) ty = s.ty + item.bottom() - (view.bottom() - margin);
    // Элемент уже виден — ни прокрутки, ни перерисовки (иначе вызов каждый кадр не даёт окну простаивать).
    if (ty == s.ty) return;
    s.ty = ty;
    s.activity = c.time;
    requestRedraw();
    return;
  }
}

// ================================================================ Row
static void beginRow(std::span<const Len> cols, float height, float gap) {
  Ctx& c = C();
  bool os = false;
  RectF r0 = slotBegin(0, 0, &os);
  Frame f;
  f.win = c.win;
  f.kind = Frame::RowK;
  f.flow = Flow::Row;
  f.slot = r0;
  f.slotOneShot = os;
  f.box = RectF{r0.x, r0.y, r0.w, kInfH};
  f.gap = c.th.gap;
  f.clip = currentClip();
  f.ncols = int(cols.size());
  f.rowAuto = height == kAuto;
  f.rowH = height > 0 ? height : c.th.controlH;
  f.rowTop = r0.y;
  f.bottom = r0.y;
  float fixed = gap * float(std::max<size_t>(cols.size(), 1) - 1), frs = 0;
  for (const Len& l : cols) {
    fixed += l.px;
    frs += l.fr;
  }
  float rest = std::max(0.f, r0.w - fixed);
  float x = r0.x;
  for (const Len& l : cols) {
    float w = l.px + (frs > 0 ? rest * l.fr / frs : 0);
    w = std::max(w, l.min);
    f.colX.push_back(std::round(x));
    f.colW.push_back(std::max(0.f, std::round(x + w) - std::round(x)));
    x += w + gap;
  }
  pushFrame(f);
}

Row::Row(std::initializer_list<Len> cols, float height, float gap) { beginRow(std::span<const Len>(cols.begin(), cols.size()), height, gap); }
Row::Row(std::span<const Len> cols, float height, float gap) { beginRow(cols, height, gap); }

Row::~Row() {
  Frame f = frame();
  popFrame();
  float bottom = f.placed ? f.bottom : f.slot.y;
  RectF r{f.slot.x, f.slot.y, f.slot.w, std::max(0.f, bottom - f.slot.y)};
  if (!f.slotOneShot) slotEnd(r);
}

// ================================================================ HStack
void in::hstackBegin(float height, Align align, float gap) {
  Ctx& c = C();
  float h = height > 0 ? height : c.th.controlH;
  bool os = false;
  RectF r0 = slotBegin(0, h, &os);
  r0.h = h;
  Frame f;
  f.win = c.win;
  f.kind = Frame::StackK;
  f.flow = Flow::HStack;
  f.slot = r0;
  f.slotOneShot = os;
  f.box = r0;
  f.stackH = h;
  f.gap = gap;
  f.align = align;
  f.clip = currentClip();
  f.stackId = autoId("hstack");
  const StackMeasure& m = state<StackMeasure>(f.stackId);
  float start = r0.x;
  if (align == Align::Right) start = r0.right() - m.total;
  else if (align == Align::Center) start = r0.x + std::round((r0.w - m.total) * 0.5f);
  f.x = f.stackStart = std::max(r0.x, start);
  f.y = r0.y;
  f.bottom = r0.y;
  f.right = f.x;
  pushFrame(f);
}

void in::hstackEnd() {
  Frame f = frame();
  popFrame();
  StackMeasure& m = state<StackMeasure>(f.stackId);
  float total = f.placed ? f.right - f.stackStart : 0;
  float rest = f.flexAt >= 0 && f.placed ? f.right - f.flexAt : 0;
  if (std::fabs(total - m.total) > 0.25f || std::fabs(rest - m.rest) > 0.25f) {
    if (f.align != Align::Left || f.flexAt >= 0) requestRedraw();
  }
  m.total = total;
  m.rest = rest;
  RectF r{f.slot.x, f.slot.y, f.slot.w, f.stackH};
  if (!f.slotOneShot) slotEnd(r);
}

void flex() {
  Frame& f = frame();
  if (f.flow != Flow::HStack) return;
  const StackMeasure& m = state<StackMeasure>(f.stackId);
  float x = std::max(f.x, f.box.right() - m.rest);
  f.x = x;
  f.flexAt = x;
}

// ================================================================ Group, Indent, Disabled
Group::Group(float width, float g) {
  Ctx& c = C();
  bool os = false;
  RectF r0 = slotBegin(width, 0, &os);
  Frame f;
  f.win = c.win;
  f.kind = Frame::GroupK;
  f.slot = r0;
  f.slotOneShot = os;
  f.box = RectF{r0.x, r0.y, r0.w, kInfH};
  f.x = r0.x;
  f.y = r0.y;
  f.bottom = r0.y;
  f.gap = g < 0 ? c.th.gap : g;
  f.clip = currentClip();
  pushFrame(f);
}

Group::~Group() {
  Frame f = frame();
  popFrame();
  float bottom = f.placed ? f.bottom : f.slot.y;
  RectF r{f.slot.x, f.slot.y, f.slot.w, std::max(f.slot.h, bottom - f.slot.y)};
  if (!f.slotOneShot) slotEnd(r);
}

Indent::Indent(float amount) : a_(amount) { frame().indent += amount; }
Indent::~Indent() { frame().indent = std::max(0.f, frame().indent - a_); }

Disabled::Disabled(bool on) : on_(on) {
  if (on_) C().disabled++;
}
Disabled::~Disabled() {
  if (on_ && C().disabled > 0) C().disabled--;
}

// ================================================================ splitter
Split splitter(std::string_view name, RectF area, float& pos, Axis axis, float minA, float minB) {
  Ctx& c = C();
  const Theme& t = c.th;
  WidgetId sid = id(name);
  bool hor = axis == Axis::Horizontal;
  float total = hor ? area.w : area.h;
  float lo = std::min(minA, total), hi = std::max(lo, total - minB);
  float p = clamp(pos, lo, hi);
  Split out;
  RectF handle = hor ? RectF{area.x + p - 3, area.y, 6, area.h} : RectF{area.x, area.y + p - 3, area.w, 6};
  Interaction it = interact(sid, handle);
  float& start = state<float>(sid);
  if (it.pressed) start = p;
  if (c.active == sid) {
    float np = clamp(start + (hor ? it.dx : it.dy), lo, hi);
    if (np != p) {
      p = np;
      out.changed = true;
    }
  }
  if (it.hovered || c.active == sid) c.cursor = hor ? platform::Cursor::ResizeH : platform::Cursor::ResizeV;
  float hl = animate(sid ^ 0x51ull, (it.hovered || c.active == sid) ? 1.f : 0.f);
  Color lc = mixc(t.border, t.accent, hl);
  float lw = 1 + hl;
  if (hor) cmdRect(RectF{area.x + p - lw * 0.5f, area.y, lw, area.h}, lc);
  else cmdRect(RectF{area.x, area.y + p - lw * 0.5f, area.w, lw}, lc);
  setLast(sid, handle, it);
  if (pos != p) {
    pos = p;
    out.changed = true;
  }
  if (hor) {
    out.a = RectF{area.x, area.y, p, area.h};
    out.b = RectF{area.x + p + 1, area.y, std::max(0.f, area.w - p - 1), area.h};
  } else {
    out.a = RectF{area.x, area.y, area.w, p};
    out.b = RectF{area.x, area.y + p + 1, area.w, std::max(0.f, area.h - p - 1)};
  }
  C().last.changed = out.changed;
  return out;
}

// ================================================================ VirtualList
VirtualList::VirtualList(std::string_view name, int count, float rowHeight, float height)
    : scroll_(name, height), count_(std::max(0, count)), rowH_(std::max(1.f, rowHeight)) {
  Frame& f = frame();
  top_ = f.y;
  x_ = f.box.x;
  w_ = f.box.w;
  RectF clip = f.clip;
  first_ = clamp(int(std::floor((clip.y - top_) / rowH_)), 0, count_);
  last_ = clamp(int(std::ceil((clip.bottom() - top_) / rowH_)), first_, count_);
}

VirtualList::~VirtualList() {
  if (rowOpen_) popId();
  Frame& f = frame();
  f.placed = true;
  f.bottom = std::max(f.bottom, top_ + rowH_ * float(count_));
  f.y = f.bottom;
}

void VirtualList::enterRow(int i) {
  Frame& f = frame();
  f.y = top_ + rowH_ * float(i);
  f.placed = true;
  pushId(i64(i));
  rowOpen_ = true;
}

VirtualList::It& VirtualList::It::operator++() {
  if (l->rowOpen_) {
    popId();
    l->rowOpen_ = false;
  }
  i++;
  if (i < l->last_) l->enterRow(i);
  return *this;
}

VirtualList::It VirtualList::begin() {
  if (first_ < last_) enterRow(first_);
  return It{this, first_};
}

VirtualList::It VirtualList::end() { return It{this, last_}; }

RectF VirtualList::rowRect(int i) const { return RectF{x_, top_ + rowH_ * float(i), w_, rowH_}; }

void VirtualList::scrollToRow(int i) {
  RectF r = rowRect(clamp(i, 0, std::max(0, count_ - 1)));
  Item saved = C().last;
  C().last.rect = r;
  scrollToItem();
  C().last = saved;
}

}  // namespace rg::ui

namespace rg::ui {
HStack::HStack(float height, Align align, float gap) { in::hstackBegin(height, align, gap); }
HStack::~HStack() { in::hstackEnd(); }
}  // namespace rg::ui
