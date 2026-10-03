// Regnum — всплывающие слои: подсказки, всплывающие окна и меню (подменю, клавиатура), модальные окна
// (затемнение, ловушка фокуса, Esc/Enter, исчезание), уведомления, перетаскивание.
#include "ui/ui_internal.h"

namespace rg::ui {

using namespace in;

namespace in {

namespace {

constexpr double kTipDelay = 0.35;
constexpr double kTipWarm = 0.5;

Color tipBg() { return C().th.dark ? Color::hex(0x272f3b) : Color::hex(0x2a2620); }
Color tipFg() { return Color::hex(0xf1ede4); }

Window* findWindow(WidgetId id) {
  auto it = C().windows.find(id);
  return it == C().windows.end() ? nullptr : it->second.get();
}

void pushOverlayFrame(Window* w, RectF box) {
  Frame f;
  f.win = w;
  f.kind = Frame::PopupK;
  f.surface = C().th.dark ? C().th.surface2.lighten(0.02f) : C().th.surface2;
  f.box = box;
  f.x = box.x;
  f.y = box.y;
  f.bottom = box.y;
  f.right = box.x;
  f.gap = C().th.gap;
  f.clip = w->rect;
  f.bodyTop = box.y;
  pushFrame(f);
}

// Положение всплывающего окна размера (w, h) у якоря с переворотом при нехватке места.
RectF placeAt(RectF anchor, float w, float h, Side side) {
  Ctx& c = C();
  RectF v = c.view.inset(8);
  RectF r{anchor.x, anchor.bottom() + 4, w, h};
  switch (side) {
    case Side::Below:
      if (r.bottom() > v.bottom() && anchor.y - 4 - h >= v.y) r.y = anchor.y - 4 - h;
      break;
    case Side::Above:
      r.y = anchor.y - 4 - h;
      if (r.y < v.y && anchor.bottom() + 4 + h <= v.bottom()) r.y = anchor.bottom() + 4;
      break;
    case Side::Right:
      r.x = anchor.right() + 2;
      r.y = anchor.y - 5;
      if (r.right() > v.right() && anchor.x - 2 - w >= v.x) r.x = anchor.x - 2 - w;
      break;
    case Side::Left:
      r.x = anchor.x - 2 - w;
      r.y = anchor.y - 5;
      if (r.x < v.x && anchor.right() + 2 + w <= v.right()) r.x = anchor.right() + 2;
      break;
  }
  r.x = clamp(r.x, v.x, std::max(v.x, v.right() - w));
  r.y = clamp(r.y, v.y, std::max(v.y, v.bottom() - h));
  r.x = std::round(r.x);
  r.y = std::round(r.y);
  return r;
}

void makeGhost(Window* w) {
  Ctx& c = C();
  if (!w) return;
  Ghost g;
  g.list = w->list;
  // Изображения вызывающего могли уже исчезнуть — в копии их нет.
  for (Cmd& k : g.list.cmds)
    if (k.op == Op::Image) k.op = Op::None;
  g.rect = w->rect;
  g.rank = w->rank;
  g.start = c.time;
  g.backdrop = true;
  c.ghosts.push_back(std::move(g));
}

}  // namespace

int currentPopupLevel() {
  Ctx& c = C();
  if (!c.win || c.win->kind != WinKind::Popup) return 0;
  for (size_t i = 0; i < c.popups.size(); i++)
    if (c.popups[i].id == c.win->id) return int(i) + 1;
  return int(c.popups.size());
}

int popupIndex(WidgetId id) {
  Ctx& c = C();
  for (size_t i = 0; i < c.popups.size(); i++)
    if (c.popups[i].id == id) return int(i);
  return -1;
}

bool popupOpen(WidgetId id) { return popupIndex(id) >= 0; }

void closePopupsFrom(int level) {
  Ctx& c = C();
  if (level < 0) level = 0;
  if (size_t(level) < c.popups.size()) c.popups.resize(size_t(level));
}

void openPopupAt(WidgetId id, RectF anchor, WidgetId opener, bool menu) {
  Ctx& c = C();
  if (id == c.suppressPopup) {   // щелчок по открывателю только что закрыл это окно
    c.suppressPopup = 0;
    return;
  }
  int level = currentPopupLevel();
  if (size_t(level) < c.popups.size() && c.popups[size_t(level)].id == id) {
    c.popups[size_t(level)].anchor = anchor;
    return;
  }
  closePopupsFrom(level);
  PopupEntry e;
  e.id = id;
  e.opener = opener;
  e.anchor = anchor;
  e.seq = ++c.popupSeq;
  e.parentRank = c.win ? c.win->rank : 0;
  e.menu = menu;
  c.popups.push_back(e);
  requestRedraw();
}

bool beginPopupId(WidgetId pid, const PopupOpt& o, bool menu) {
  Ctx& c = C();
  const Theme& t = c.th;
  int idx = popupIndex(pid);
  if (idx < 0) return false;
  PopupEntry& e = c.popups[size_t(idx)];
  e.seen = true;
  Window* prev = findWindow(pid);
  bool measured = prev && prev->measured && prev->lastFrame + 1 >= c.frame;
  Vec2 cs = measured ? prev->contentSize : Vec2{};
  float pad = o.pad;
  float w = o.width > 0 ? o.width : float(cs.x) + 2 * pad;
  float maxH = o.maxHeight > 0 ? o.maxHeight : c.view.h - 16;
  float h = std::min(float(cs.y) + 2 * pad, maxH);
  w = std::min(w, c.view.w - 16);
  RectF r = placeAt(e.anchor, std::max(w, 8.f), std::max(h, 8.f), o.side);
  int rank = kRankPopup;
  if (e.parentRank >= kRankModal && e.parentRank < kRankToast) rank = (e.parentRank % 10 == 5) ? e.parentRank : e.parentRank + 5;
  Window* win = beginWindow(pid, WinKind::Popup, rank, e.seq, r);
  c.idStack.push_back(pid);
  win->hidden = !measured;
  if (win->hidden) {
    win->measured = false;
    requestRedraw();
  } else if (e.shownAt < 0) {
    e.shownAt = c.time;
  }
  float tt = e.shownAt < 0 ? 0.f : clamp(float((c.time - e.shownAt) / 0.12), 0.f, 1.f);
  if (tt < 1) c.animating = true;
  win->alpha = easeOut(tt);
  win->dy = (o.side == Side::Above ? 4.f : -4.f) * (1 - easeOut(tt));
  if (o.side == Side::Right || o.side == Side::Left) win->dy = 0;
  float rad = 10;
  cmdShadow(r, rad, 28, t.shadow, 10);
  cmdShadow(r, rad, 3, t.shadow.alpha(0.6f), 1);
  cmdRect(r, t.dark ? t.surface2.lighten(0.02f) : t.surface2, rad);
  cmdStroke(r, t.dark ? t.borderStrong : t.border, rad, 1);
  float bw = o.width > 0 ? o.width - 2 * pad : (measured ? float(cs.x) : 2000.f);
  pushOverlayFrame(win, RectF{r.x + pad, r.y + pad, bw, 1e7f});
  frame().gap = menu ? 0 : t.gap;
  pushClipFrame(r.inset(1));
  // Меню: стрелки и Enter, пока окно верхнее.
  if (menu && idx == int(c.popups.size()) - 1 && !win->hidden) {
    e.run = 0;
    e.activate = false;
    if (e.count > 0) {
      if (takeKey(Key::Down)) e.highlight = (e.highlight + 1 + e.count) % e.count;
      if (takeKey(Key::Up)) e.highlight = e.highlight < 0 ? e.count - 1 : (e.highlight - 1 + e.count) % e.count;
      if (e.highlight >= 0 && (takeKey(Key::Enter) || takeKey(Key::Space))) e.activate = true;
    }
  } else {
    e.run = 0;
    e.activate = false;
  }
  return true;
}

// ---------------------------------------------------------------- этапы кадра
void overlaysBeginFrame() {
  Ctx& c = C();
  for (auto& p : c.popups) p.seen = false;
  for (auto& m : c.modals) m.seen = false;
  c.tipRequested = false;
  c.tipPending = false;
  c.tipTouched = false;
  c.tipRich = false;
  // Нажатие вне всплывающих окон закрывает их (кроме тех, внутри которых нажали).
  bool press = c.pressedEv[0] || c.pressedEv[1] || c.pressedEv[2];
  if (press) c.suppressPopup = 0;
  if (press && !c.popups.empty()) {
    int keep = -1;
    for (int i = int(c.popups.size()) - 1; i >= 0; i--) {
      Window* w = findWindow(c.popups[size_t(i)].id);
      if (w && !w->hidden && w->rect.contains(c.m.x, c.m.y)) {
        keep = i;
        break;
      }
    }
    // Нажатие в окне выше всплывающих (уведомление) их не трогает.
    bool aboveAll = c.hoveredWin && c.hoveredWin->rank >= kRankToast;
    if (!aboveAll && keep + 1 < int(c.popups.size())) {
      for (size_t i = size_t(keep + 1); i < c.popups.size(); i++)
        if (c.popups[i].anchor.contains(c.m.x, c.m.y)) {
          c.closedOpener = c.popups[i].opener;
          c.suppressPopup = c.popups[i].id;
        }
      closePopupsFrom(keep + 1);
    }
  }
  // Щелчок по фону модального окна
  if (c.pressedEv[0] && !c.modals.empty()) {
    ModalEntry& top = c.modals.back();
    Window* w = findWindow(top.id);
    bool inPopup = false;
    for (auto& p : c.popups) {
      Window* pw = findWindow(p.id);
      if (pw && pw->rect.contains(c.m.x, c.m.y)) inPopup = true;
    }
    bool inToast = c.hoveredWin && c.hoveredWin->rank >= kRankToast;
    if (top.dismiss && w && !w->hidden && !w->rect.contains(c.m.x, c.m.y) && !inPopup && !inToast) top.closeReq = true;
  }
}

static void buildTooltip();
static void buildToasts();
static void buildDrag();

void overlaysEndFrame() {
  Ctx& c = C();
  // Всплывающие окна, которые не рисовались в этом кадре, закрываются (с ними и вложенные).
  for (size_t i = 0; i < c.popups.size(); i++)
    if (!c.popups[i].seen) {
      closePopupsFrom(int(i));
      break;
    }
  // Модальные окна, которые перестали рисовать, исчезают плавно.
  for (size_t i = 0; i < c.modals.size();) {
    if (!c.modals[i].seen) {
      Window* w = findWindow(c.modals[i].id);
      if (w && w->lastFrame + 1 == c.frame && !w->hidden) makeGhost(w);
      c.modals.erase(c.modals.begin() + long(i));
    } else {
      i++;
    }
  }
  c.winStack.resize(1);
  c.win = c.root;
  buildToasts();
  buildTooltip();
  buildDrag();
  if (!c.tipTouched) c.tipItem = 0;
  c.win = c.root;
}

}  // namespace in

// ================================================================ подсказки
static bool tipReady(Ctx& c) {
  const Item& L = c.last;
  if (!L.hovered || L.id == 0) return false;
  if (c.drag.active || (c.active && c.active != L.id)) return false;
  c.tipTouched = true;
  if (c.tipItem != L.id) {
    c.tipItem = L.id;
    bool warm = c.time - c.tipLastShown < kTipWarm;
    c.tipStart = warm ? c.time - kTipDelay : c.time;
  }
  if (c.m.down[0] || c.m.down[1] || c.pressedEv[0] || c.releasedEv[0]) {
    c.tipStart = 1e300;   // после щелчка — до следующего наведения
    return false;
  }
  if (c.time - c.tipStart >= kTipDelay) return true;
  c.tipPending = true;
  return false;
}

void tooltip(std::string_view text, Shortcut s) {
  Ctx& c = C();
  if (text.empty() && !s) return;
  if (!tipReady(c)) return;
  c.tipRequested = true;
  c.tipRich = false;
  c.tipText.assign(text.data(), text.size());
  c.tipShortcut = s;
  c.tipAnchor = c.last.rect;
}

static WidgetId kTipWin = hash64("##tooltip");

bool beginTooltip(float width) {
  Ctx& c = C();
  if (!tipReady(c)) return false;
  c.tipRequested = true;
  c.tipRich = true;
  c.tipAnchor = c.last.rect;
  Window* prev = nullptr;
  if (auto it = c.windows.find(kTipWin); it != c.windows.end()) prev = it->second.get();
  bool measured = prev && prev->measured && prev->lastFrame + 1 >= c.frame && c.tipShownItem == c.tipItem;
  float h = measured ? float(prev->contentSize.y) + 20 : 40;
  RectF anchor = c.tipAnchor;
  RectF v = c.view.inset(8);
  RectF r{anchor.cx() - width * 0.5f, anchor.bottom() + 6, width, h};
  if (r.bottom() > v.bottom()) r.y = anchor.y - 6 - h;
  r.x = std::round(clamp(r.x, v.x, std::max(v.x, v.right() - width)));
  r.y = std::round(clamp(r.y, v.y, std::max(v.y, v.bottom() - h)));
  Window* w = beginWindow(kTipWin, WinKind::Tooltip, kRankTooltip, 0, r);
  c.idStack.push_back(kTipWin);
  w->hidden = !measured;
  if (!measured) requestRedraw();
  if (c.tipShownItem != c.tipItem) {
    c.tipShownItem = c.tipItem;
    c.tipShownAt = c.time;
  }
  float a = clamp(float((c.time - c.tipShownAt) / 0.1), 0.f, 1.f);
  if (a < 1) c.animating = true;
  w->alpha = a;
  const Theme& t = c.th;
  cmdShadow(r, 8, 16, t.shadow, 6);
  cmdRect(r, t.dark ? t.surface2.lighten(0.03f) : t.surface2, 8);
  cmdStroke(r, t.dark ? t.borderStrong : t.border, 8, 1);
  pushOverlayFrame(w, RectF{r.x + 10, r.y + 10, width - 20, 1e7f});
  frame().gap = 4;
  return true;
}

void endTooltip() {
  Ctx& c = C();
  Frame f = frame();
  popFrame();
  if (c.win && c.win->kind == WinKind::Tooltip) {
    c.win->contentSize = Vec2{f.box.w, f.placed ? f.bottom - f.box.y : 0};
    if (c.win->measured && std::fabs(float(c.win->prevContentSize.y) - float(c.win->contentSize.y)) > 0.5f) requestRedraw();
    c.win->measured = true;
  }
  popId();
  endWindow();
  c.tipLastShown = c.time;
}

namespace in {

static void buildTooltip() {
  Ctx& c = C();
  if (!c.tipRequested || c.tipRich) return;
  if (c.tipShownItem != c.tipItem) {
    c.tipShownItem = c.tipItem;
    c.tipShownAt = c.time;
  }
  c.tipLastShown = c.time;
  const auto& st = styleOf(Font::Small);
  std::string sc = shortcutText(c.tipShortcut);
  const auto scst = styleWith(Font::Caption, gfx::FontWeight::Semibold);
  float scW = sc.empty() ? 0 : textWidth(sc, scst) + 12;
  TextEntry* te = cachedText(c.tipText, st, 260, 6, true, Align::Left, true);
  float tw = te->layout.width;
  float w = std::ceil(tw + 20 + (scW > 0 ? scW + (tw > 0 ? 8 : 0) : 0));
  float h = std::ceil(std::max(te->layout.height, 18.f) + 12);
  RectF anchor = c.tipAnchor;
  RectF v = c.view.inset(6);
  RectF r{anchor.cx() - w * 0.5f, anchor.bottom() + 6, w, h};
  bool above = false;
  if (r.bottom() > v.bottom()) {
    r.y = anchor.y - 6 - h;
    above = true;
  }
  r.x = std::round(clamp(r.x, v.x, std::max(v.x, v.right() - w)));
  r.y = std::round(clamp(r.y, v.y, std::max(v.y, v.bottom() - h)));
  Window* win = beginWindow(kTipWin, WinKind::Tooltip, kRankTooltip, 0, r);
  float a = clamp(float((c.time - c.tipShownAt) / 0.1), 0.f, 1.f);
  if (a < 1) c.animating = true;
  win->alpha = a;
  win->dy = (above ? 3.f : -3.f) * (1 - a);
  cmdShadow(r, 6, 14, Color(0, 0, 0, 90), 4);
  cmdRect(r, tipBg(), 6);
  cmdStroke(r, Color(255, 255, 255, 18), 6, 1);
  float x = r.x + 10;
  if (tw > 0) cmdText(te, x, r.y + (r.h - te->layout.height) * 0.5f, tipFg());
  if (scW > 0) {
    RectF k{r.right() - 6 - scW, r.cy() - 9, scW, 18};
    cmdRect(k, Color(255, 255, 255, 22), 4);
    textIn(sc, k, scst, Color::hex(0xc9c3b6), Align::Center);
  }
  endWindow();
}

static void buildDrag() {
  Ctx& c = C();
  if (!c.drag.active) return;
  const Theme& t = c.th;
  const auto& st = styleOf(Font::Strong);
  float tw = c.drag.label.empty() ? 0 : textWidth(c.drag.label, st);
  float w = 20 + tw + (c.drag.icon.empty() ? 0 : 22);
  RectF r{std::round(c.m.x + 14), std::round(c.m.y + 10), std::ceil(w), 30};
  if (c.drag.label.empty() && c.drag.icon.empty()) return;
  beginWindow(hash64("##drag"), WinKind::Drag, kRankDrag, 0, r);
  cmdShadow(r, 8, 18, t.shadow, 6);
  cmdRect(r, t.surface2, 8);
  cmdStroke(r, t.accent, 8, 1.5f);
  float x = r.x + 10;
  if (!c.drag.icon.empty()) {
    cmdIcon(c.drag.icon, RectF{x, r.cy() - 8, 16, 16}, t.accent);
    x += 22;
  }
  textIn(c.drag.label, RectF{x, r.y, tw + 1, r.h}, st, t.text);
  endWindow();
}

static const char* toastIcon(Tone tone) {
  switch (tone) {
    case Tone::Success: return "check-circle";
    case Tone::Warning: return "warning";
    case Tone::Danger: return "error";
    default: return "info";
  }
}

static void buildToasts() {
  Ctx& c = C();
  if (c.toasts.empty()) return;
  const Theme& t = c.th;
  const float W = 340, margin = 16;
  // Не больше пяти одновременно: старшие уходят.
  int visible = 0;
  for (size_t i = c.toasts.size(); i-- > 0;) {
    if (c.toasts[i].closeAt >= 0) continue;
    if (++visible > 5) c.toasts[i].closeAt = c.time;
  }
  float y = c.view.bottom() - margin;
  const auto& st = styleOf(Font::Body);
  for (size_t i = c.toasts.size(); i-- > 0;) {
    ToastEntry& e = c.toasts[i];
    TextEntry* te = cachedText(e.text, st, W - 16 - 34 - 12 - 32, 4, true, Align::Left, true);
    float h = std::max(48.f, std::ceil(te->layout.height) + 28);
    e.height = h;
    float tin = clamp(float((c.time - e.start) / 0.22), 0.f, 1.f);
    float tout = e.closeAt >= 0 ? clamp(float((c.time - e.closeAt) / 0.18), 0.f, 1.f) : 0.f;
    float ty = animate(hashMix(e.id, 0x7057ull), y - h, 0.18f);
    RectF r{std::round(c.view.right() - margin - W + 28 * (1 - easeOut(tin)) + 28 * easeOut(tout)), std::round(ty), W, h};
    WidgetId wid = hashMix(hash64("##toast"), e.id);
    Window* w = beginWindow(wid, WinKind::Toast, kRankToast, u32(e.id), r);
    w->alpha = easeOut(tin) * (1 - tout);
    Frame f;
    f.win = w;
    f.box = r;
    f.clip = r;
    pushFrame(f);
    Interaction it = interact(wid ^ 1, r, IfAllowOverlap);
    e.hovered = it.hovered;
    if (e.hovered && e.closeAt < 0) e.paused += c.dt;
    Color tc = toneColor(e.tone);
    cmdShadow(r, 10, 26, t.shadow, 10);
    cmdRect(r, t.dark ? t.surface2.lighten(0.03f) : t.surface2, 10);
    cmdStroke(r, t.dark ? t.borderStrong : t.border, 10, 1);
    RectF ic{r.x + 14, r.y + 14, 22, 22};
    cmdRect(ic.expand(3), tc.alpha(0.16f), 8);
    cmdIcon(e.icon.empty() ? toastIcon(e.tone) : e.icon.c_str(), ic.inset(1), tc);
    cmdText(te, r.x + 50, r.y + (h - te->layout.height) * 0.5f, t.text);
    RectF xr{r.right() - 30, r.y + 12, 22, 22};
    Interaction xi = interact(wid ^ 2, xr);
    float xh = animate(wid ^ 3, xi.hovered ? 1.f : 0.f);
    if (xh > 0.01f) cmdRect(xr, t.hover.alpha(xh * 1.5f), 6);
    cmdIcon("close", xr.inset(4), mixc(t.textMuted, t.text, xh));
    if (xi.clicked && e.closeAt < 0) e.closeAt = c.time;
    double left = e.duration - (c.time - e.start - e.paused);
    if (e.closeAt < 0) {
      float k = clamp(float(left / std::max(0.1, e.duration)), 0.f, 1.f);
      cmdRect4(RectF{r.x + 1, r.bottom() - 3, (r.w - 2) * k, 2}, tc.alpha(0.55f), 0, 0, k > 0.98f ? 10 : 0, 10);
      if (left <= 0) e.closeAt = c.time;
    }
    popFrame();
    endWindow();
    y -= (h + 8) * (1 - tout);
  }
  for (auto it = c.toasts.begin(); it != c.toasts.end();) {
    if (it->closeAt >= 0 && c.time - it->closeAt > 0.18) it = c.toasts.erase(it);
    else ++it;
  }
}

}  // namespace in

void toast(std::string text, Tone tone, const char* icon, double seconds) {
  Ctx& c = C();
  ToastEntry e;
  e.id = ++c.toastSeq;
  e.text = std::move(text);
  e.tone = tone;
  e.icon = icon ? icon : "";
  e.start = c.time;
  e.duration = std::max(0.5, seconds);
  c.toasts.push_back(std::move(e));
  c.redraw = true;
}

int toastCount() {
  int n = 0;
  for (auto& t : C().toasts)
    if (t.closeAt < 0) n++;
  return n;
}

// ================================================================ всплывающие окна
void openPopup(std::string_view name) {
  Ctx& c = C();
  openPopupAt(id(name), c.last.rect, c.last.id, false);
}

void closePopup() {
  Ctx& c = C();
  if (!c.win || c.win->kind != WinKind::Popup) return;
  int idx = popupIndex(c.win->id);
  if (idx >= 0) closePopupsFrom(idx);
}

bool isPopupOpen(std::string_view name) { return popupOpen(id(name)); }

bool beginPopup(std::string_view name, const PopupOpt& o) { return beginPopupId(id(name), o, false); }

void endPopup() {
  Ctx& c = C();
  if (!c.win || c.win->kind != WinKind::Popup) {
    logWarn("ui: endPopup вне всплывающего окна");
    return;
  }
  Frame f = frame();
  popFrame();
  Window* w = c.win;
  Vec2 cs{f.placed ? f.right - f.box.x : 0, f.placed ? f.bottom - f.box.y : 0};
  if (!w->measured || std::fabs(float(cs.x - w->contentSize.x)) > 0.5f || std::fabs(float(cs.y - w->contentSize.y)) > 0.5f) requestRedraw();
  w->contentSize = cs;
  w->measured = true;
  int idx = popupIndex(w->id);
  if (idx >= 0) {
    PopupEntry& e = c.popups[size_t(idx)];
    if (e.menu) e.count = e.run;
    if (idx == int(c.popups.size()) - 1 && takeKey(Key::Escape)) closePopupsFrom(idx);
    else if (e.menu && idx > 0 && idx == int(c.popups.size()) - 1 && takeKey(Key::Left)) closePopupsFrom(idx);
  }
  popId();
  endWindow();
}

// ================================================================ меню
bool beginContextMenu(std::string_view name) {
  Ctx& c = C();
  if (c.last.rightClicked) openPopupAt(id(name), RectF{c.m.x, c.m.y, 0, 0}, c.last.id, true);
  return beginMenu(name);
}

bool beginMenu(std::string_view name) {
  PopupOpt o;
  o.pad = 5;
  WidgetId pid = id(name);
  int idx = popupIndex(pid);
  if (idx >= 0) C().popups[size_t(idx)].menu = true;
  return beginPopupId(pid, o, true);
}

void endMenu() { endPopup(); }

namespace {

PopupEntry* currentMenu() {
  Ctx& c = C();
  if (!c.win || c.win->kind != WinKind::Popup) return nullptr;
  int idx = popupIndex(c.win->id);
  return idx >= 0 ? &c.popups[size_t(idx)] : nullptr;
}

struct MenuRow {
  RectF r;
  Interaction it;
  bool hot = false;
  int index = -1;
};

MenuRow menuRow(WidgetId wid, float natW, bool disabled) {
  Ctx& c = C();
  MenuRow m;
  RectF r = place(natW, 30);
  r.w = std::max(r.w, frame().box.w);
  m.r = r;
  m.it = interact(wid, r);
  PopupEntry* e = currentMenu();
  if (e) {
    m.index = e->run++;
    if (m.it.hovered && !c.win->hidden && (c.mouseMoved || e->highlight < 0)) e->highlight = m.index;
    m.hot = e->highlight == m.index && !disabled;
  }
  if (!e) m.hot = m.it.hovered && !disabled;
  return m;
}

}  // namespace

bool menuItem(std::string_view lbl, const MenuItemOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  bool disabled = o.disabled || isDisabled();
  std::string_view s = displayText(lbl);
  const auto& st = styleOf(Font::Body);
  std::string sc = shortcutText(o.shortcut);
  const auto& scs = styleOf(Font::Small);
  float scW = sc.empty() ? 0 : textWidth(sc, scs) + 28;
  float natW = 10 + 16 + 10 + textWidth(s, st) + scW + 14;
  WidgetId wid = id(lbl);
  MenuRow m = menuRow(wid, natW, disabled);
  int level = currentPopupLevel();
  // Наведение на обычный пункт закрывает открытое подменю этого меню.
  if (m.it.hovered && int(c.popups.size()) > level) closePopupsFrom(level);
  Color fg = disabled ? t.textMuted : (o.danger ? t.danger : t.text);
  if (m.hot) cmdRect(m.r, o.danger ? t.danger.alpha(0.14f) : (t.dark ? t.surface3.lighten(0.04f) : t.surface3), 6);
  float x = m.r.x + 10;
  if (o.checked) cmdIcon("check", RectF{x, m.r.cy() - 8, 16, 16}, disabled ? t.textMuted : t.accent);
  else if (o.icon) cmdIcon(o.icon, RectF{x, m.r.cy() - 8, 16, 16}, disabled ? t.textMuted : (o.danger ? t.danger : (m.hot ? t.text : t.textDim)));
  x += 26;
  textIn(s, RectF{x, m.r.y, m.r.right() - x - scW - 8, m.r.h}, st, fg);
  if (!sc.empty()) textIn(sc, RectF{m.r.right() - scW, m.r.y, scW - 12, m.r.h}, scs, t.textMuted, Align::Right);
  PopupEntry* e = currentMenu();
  bool clicked = !disabled && (m.it.clicked || (e && e->activate && e->highlight == m.index));
  setLast(wid, m.r, m.it);
  if (clicked) {
    closePopupsFrom(0);
    c.last.clicked = true;
  }
  return clicked;
}

void menuSeparator() {
  Ctx& c = C();
  RectF r = place(1, 9);
  r.w = std::max(r.w, frame().box.w);
  draw::line(r.x + 6, r.cy(), r.right() - 6, r.cy(), c.th.border, 1);
}

void menuHeader(std::string_view text) {
  Ctx& c = C();
  gfx::TextStyle st = styleWith(Font::Caption, gfx::FontWeight::Semibold);
  st.letterSpacing = 0.6f;
  std::string up = utf8::upper(displayText(text));
  RectF r = place(textWidth(up, st) + 20, 26);
  r.w = std::max(r.w, frame().box.w);
  textIn(up, RectF{r.x + 10, r.y + 4, r.w - 20, r.h - 4}, st, c.th.textMuted);
}

bool beginSubmenu(std::string_view lbl, const char* icon) {
  Ctx& c = C();
  const Theme& t = c.th;
  std::string_view s = displayText(lbl);
  const auto& st = styleOf(Font::Body);
  float natW = 10 + 16 + 10 + textWidth(s, st) + 40;
  WidgetId wid = id(lbl);
  WidgetId sub = hashMix(wid, 0x5ab3e2ull);
  bool open = popupOpen(sub);
  MenuRow m = menuRow(wid, natW, false);
  double& hoverStart = state<double>(wid ^ 0x5ab1ull);
  if (!m.it.hovered) hoverStart = -1;
  else if (hoverStart < 0) hoverStart = c.time;
  bool want = m.it.clicked || (m.it.hovered && c.time - hoverStart >= 0.12);
  PopupEntry* e = currentMenu();
  if (e && e->highlight == m.index && (e->activate || takeKey(Key::Right))) want = true;
  if (m.it.hovered && !open && c.time - hoverStart < 0.12) c.tipPending = true;
  if (want && !open) {
    openPopupAt(sub, m.r, wid, true);
    open = true;
  }
  if (m.hot || open) cmdRect(m.r, t.dark ? t.surface3.lighten(0.04f) : t.surface3, 6);
  float x = m.r.x + 10;
  if (icon) cmdIcon(icon, RectF{x, m.r.cy() - 8, 16, 16}, m.hot || open ? t.text : t.textDim);
  x += 26;
  textIn(s, RectF{x, m.r.y, m.r.right() - x - 30, m.r.h}, st, t.text);
  drawChevron(RectF{m.r.right() - 24, m.r.cy() - 7, 14, 14}, t.textMuted, -90);
  setLast(wid, m.r, m.it);
  if (!open) return false;
  PopupOpt o;
  o.side = Side::Right;
  o.pad = 5;
  if (int idx = popupIndex(sub); idx >= 0) c.popups[size_t(idx)].menu = true;
  return beginPopupId(sub, o, true);
}

void endSubmenu() { endPopup(); }

// ================================================================ модальные окна
void openModal(std::string_view name) {
  Ctx& c = C();
  WidgetId mid = id(name);
  for (auto& m : c.modals)
    if (m.id == mid) return;
  ModalEntry e;
  e.id = mid;
  e.seq = ++c.modalSeq;
  c.modals.push_back(e);
  closePopupsFrom(0);
  requestRedraw();
}

static int modalIndex(WidgetId mid) {
  Ctx& c = C();
  for (size_t i = 0; i < c.modals.size(); i++)
    if (c.modals[i].id == mid) return int(i);
  return -1;
}

bool beginModal(std::string_view name, const ModalOpt& o, bool* open) {
  Ctx& c = C();
  const Theme& t = c.th;
  WidgetId mid = id(name);
  int idx = modalIndex(mid);
  if (open) {
    if (*open && idx < 0) {
      openModal(name);
      idx = modalIndex(mid);
    } else if (!*open && idx >= 0) {
      Window* w = findWindow(mid);
      if (w && !w->hidden && w->lastFrame + 1 == c.frame) makeGhost(w);
      c.modals.erase(c.modals.begin() + idx);
      idx = -1;
    }
  }
  if (idx < 0) return false;
  ModalEntry& e = c.modals[size_t(idx)];
  if (e.closeReq) {
    Window* w = findWindow(mid);
    if (w && !w->hidden && w->lastFrame + 1 == c.frame) makeGhost(w);
    c.modals.erase(c.modals.begin() + idx);
    if (open) *open = false;
    return false;
  }
  e.seen = true;
  e.dismiss = o.dismissOnBackdrop;
  Window* prev = findWindow(mid);
  bool measured = prev && prev->measured && prev->lastFrame + 1 >= c.frame;
  float W = std::min(o.width, c.view.w - 32);
  float H = measured ? std::min(float(prev->contentSize.y), c.view.h - 32) : 200;
  RectF r{std::round((c.view.w - W) * 0.5f), std::round(std::max(16.f, (c.view.h - H) * 0.42f)), W, H};
  Window* w = beginWindow(mid, WinKind::Modal, kRankModal + 10 * idx, e.seq, r);
  c.idStack.push_back(mid);
  w->hidden = !measured;
  w->backdrop = true;
  if (!measured) {
    w->measured = false;
    requestRedraw();
  } else if (e.shownAt < 0) {
    e.shownAt = c.time;
  }
  float tt = e.shownAt < 0 ? 0.f : clamp(float((c.time - e.shownAt) / 0.18), 0.f, 1.f);
  if (tt < 1) c.animating = true;
  w->alpha = easeOut(tt);
  w->dy = 10 * (1 - easeOut(tt));
  cmdShadow(r, t.radiusPanel, 60, Color(0, 0, 0, t.dark ? 150 : 60), 24);
  cmdShadow(r, t.radiusPanel, 4, Color(0, 0, 0, t.dark ? 80 : 20), 1);
  cmdRect(r, t.surface1, t.radiusPanel);
  cmdStroke(r, t.dark ? t.borderStrong.alpha(0.8f) : t.border, t.radiusPanel, 1);
  if (t.dark) cmdStroke(r.inset(1), Color(255, 255, 255, 6), t.radiusPanel - 1, 1);
  c.modalStack.push_back({mid, 0, open, o.dismissOnBackdrop, false});
  // Заголовок
  float pad = 24;
  float top = r.y + pad;
  Frame f;
  f.win = w;
  f.kind = Frame::PopupK;
  f.clip = r;
  f.gap = t.gap;
  pushFrame(f);
  if (!o.title.empty() || o.icon) {
    float x = r.x + pad;
    if (o.icon) {
      Color tc = toneColor(o.tone);
      RectF ic{x, top - 2, 36, 36};
      cmdRect(ic, tc.alpha(t.dark ? 0.15f : 0.12f), 10);
      cmdIcon(o.icon, ic.inset(8), tc);
      x += 48;
    }
    textIn(displayText(o.title), RectF{x, top - 2, r.right() - pad - 36 - x, 36}, styleOf(Font::Title), t.text);
    top += 36 + 14;
  }
  if (o.closeButton) {
    // Крестик — только мышью (с клавиатуры окно закрывает Esc), в обход Tab.
    RectF xr{r.right() - 14 - 30, r.y + 14, 30, 30};
    WidgetId xid = hashMix(mid, 0xc105eull);
    Interaction xi = interact(xid, xr);
    float xh = animate(xid ^ 1, xi.hovered ? 1.f : 0.f);
    if (xh > 0.01f) cmdRect(xr, t.hover.alpha(std::min(1.f, xh * 1.5f)), t.radiusField);
    cmdIcon("close", RectF{xr.x + 6, xr.y + 6, 18, 18}, mixc(t.textDim, t.text, xh));
    setLast(xid, xr, xi);
    tooltip("Закрыть", {Key::Escape, 0});
    if (xi.clicked) c.modalStack.back().closeReq = true;
  }
  popFrame();
  Frame body;
  body.win = w;
  body.kind = Frame::PopupK;
  body.surface = t.surface1;
  body.box = RectF{r.x + pad, top, W - 2 * pad, 1e7f};
  body.x = body.box.x;
  body.y = top;
  body.bottom = top;
  body.gap = t.gap;
  body.clip = r;
  body.bodyTop = r.y;
  body.pad = pad;
  pushFrame(body);
  return true;
}

void endModal() {
  Ctx& c = C();
  if (c.modalStack.empty() || !c.win || c.win->kind != WinKind::Modal) {
    logWarn("ui: endModal без beginModal");
    return;
  }
  Frame f = frame();
  popFrame();
  Window* w = c.win;
  Ctx::ModalCtx mc = c.modalStack.back();
  c.modalStack.pop_back();
  float H = (f.placed ? f.bottom : f.y - f.gap) - w->rect.y + f.pad;
  Vec2 cs{w->rect.w, std::ceil(H)};
  if (!w->measured || std::fabs(float(cs.y - w->contentSize.y)) > 0.5f) requestRedraw();
  w->contentSize = cs;
  w->measured = true;
  int idx = modalIndex(mc.id);
  bool top = idx == int(c.modals.size()) - 1;
  bool popupAbove = false;
  for (auto& p : c.popups)
    if (p.parentRank >= w->rank) popupAbove = true;
  bool close = mc.closeReq;
  if (top && !popupAbove && !w->hidden) {
    if (takeKey(Key::Escape)) close = true;
    if (mc.defaultBtn && takeKey(Key::Enter)) {
      c.pendingDefault = mc.defaultBtn;
      requestRedraw();
    }
  }
  if (close && idx >= 0) {
    if (!w->hidden) makeGhost(w);
    w->hidden = true;
    c.modals.erase(c.modals.begin() + idx);
    if (mc.open) *mc.open = false;
    // Фокус внутри закрытого окна теряется.
    clearFocus();
  }
  popId();
  endWindow();
}

void closeModal() {
  Ctx& c = C();
  if (!c.modalStack.empty()) c.modalStack.back().closeReq = true;
}

bool isModalOpen(std::string_view name) { return modalIndex(id(name)) >= 0; }
bool anyModalOpen() { return !C().modals.empty(); }

ModalFooter::ModalFooter() {
  Ctx& c = C();
  spacer(8);
  Frame& f = frame();
  float x0 = f.box.x - f.pad, x1 = f.box.right() + f.pad;
  if (f.pad <= 0) {
    x0 = f.box.x;
    x1 = f.box.right();
  }
  float y = f.y;
  draw::line(x0, y, x1, y, c.th.border, 1);
  spacer(16);
  hstackBegin(c.th.controlH, Align::Right, 8);
}

ModalFooter::~ModalFooter() { hstackEnd(); }

// ================================================================ перетаскивание
bool dragSource(std::string_view type, u64 payload, std::string_view label, const char* icon) {
  Ctx& c = C();
  const Item& L = c.last;
  if (L.id && c.active == L.id && c.activeDragging && !c.drag.active) {
    c.drag.active = true;
    c.drag.type.assign(type.data(), type.size());
    c.drag.payload = payload;
    c.drag.label.assign(label.data(), label.size());
    c.drag.icon = icon ? icon : "";
    c.drag.source = L.id;
  }
  return c.drag.active && c.drag.source == L.id && L.id != 0;
}

std::optional<u64> dropTarget(std::string_view type) {
  Ctx& c = C();
  if (!c.drag.active || c.drag.type != type) return std::nullopt;
  const Item& L = c.last;
  if (L.rect.empty() || L.id == c.drag.source) return std::nullopt;
  bool over = c.win == c.hoveredWin && L.rect.contains(c.m.x, c.m.y) && currentClip().contains(c.m.x, c.m.y);
  if (!over) return std::nullopt;
  const Theme& t = c.th;
  cmdRect(L.rect.expand(2), t.accent.alpha(0.10f), 8);
  cmdStroke(L.rect.expand(2), t.accent, 8, 1.5f);
  if (c.releasedEv[0]) {
    u64 p = c.drag.payload;
    c.drag = Ctx::Drag{};
    return p;
  }
  return std::nullopt;
}

bool dragging(std::string_view type) {
  Ctx& c = C();
  return c.drag.active && (type.empty() || c.drag.type == type);
}

}  // namespace rg::ui

namespace rg::ui {
void openContextMenu(std::string_view name) {
  in::Ctx& c = in::C();
  in::openPopupAt(id(name), RectF{c.m.x, c.m.y, 0, 0}, c.last.id, true);
}
}  // namespace rg::ui
