// Regnum — ядро интерфейса: очередь событий (с разнесением по кадрам), жизненный цикл кадра, ID,
// наведение/нажатие/фокус, сочетания клавиш, устойчивое состояние и анимации.
#include "ui/ui_internal.h"

namespace rg::ui {

using namespace in;
using platform::EventType;

namespace in {

Ctx& C() {
  static Ctx c;
  return c;
}

u32 normMods(u32 mods) {
  u32 m = mods & ~ModPrimary;
  if (mods & ModPrimary) m |= platform::primaryMod();
  return m & (platform::ModShift | platform::ModCtrl | platform::ModAlt | platform::ModSuper);
}

static void setMouse(Ctx& c, float lx, float ly) {
  c.m.x = lx / c.uiScale;
  c.m.y = ly / c.uiScale;
  c.mouseInside = true;
  c.mouseMoved = true;
}

static void tabNavigate(Ctx& c, bool back);

// Разбор очереди: в одном кадре не больше одного перехода кнопки мыши; нажатие в новой точке
// ждёт кадр, чтобы наведение успело обновиться; клавиши после щелчка — в следующем кадре.
// Tab/Enter/Esc завершают кадр (фокус и фиксация должны примениться до следующих клавиш).
static void processEvents(Ctx& c) {
  bool button = false, key = false, moved = false;
  while (!c.queue.empty()) {
    const platform::Event& e = c.queue.front();
    bool pop = true, stop = false;
    switch (e.type) {
      case EventType::MouseMove:
        if (button) { pop = false; stop = true; break; }
        if (std::fabs(e.x / c.uiScale - c.m.x) > 0.01f || std::fabs(e.y / c.uiScale - c.m.y) > 0.01f || !c.mouseInside) moved = true;
        setMouse(c, e.x, e.y);
        c.m.mods = e.mods;
        break;
      case EventType::MouseDown:
      case EventType::MouseUp: {
        if (e.button < 0 || e.button > 2) break;
        // Нажатие после перемещения — в следующем кадре: элементы с перекрытием успевают увидеть наведение.
        if (button || key || moved) { pop = false; stop = true; break; }
        float x = e.x / c.uiScale, y = e.y / c.uiScale;
        c.m.mods = e.mods;
        if (std::fabs(x - c.m.x) > 0.01f || std::fabs(y - c.m.y) > 0.01f || !c.mouseInside) {
          setMouse(c, e.x, e.y);
          pop = false;
          stop = true;
          break;
        }
        int b = e.button;
        if (e.type == EventType::MouseDown) {
          c.pressedEv[b] = true;
          c.m.down[b] = true;
          c.clicks = std::max(1, e.clicks);
          if (b == 0 || c.active == 0) { c.pressX = c.m.x; c.pressY = c.m.y; }
        } else {
          c.releasedEv[b] = true;
          c.m.down[b] = false;
          c.clicks = std::max(1, e.clicks);
        }
        c.lastInputTime = c.time;
        button = true;
        break;
      }
      case EventType::MouseWheel: {
        float x = e.x / c.uiScale, y = e.y / c.uiScale;
        if (std::fabs(x - c.m.x) > 0.01f || std::fabs(y - c.m.y) > 0.01f) setMouse(c, e.x, e.y);
        c.m.mods = e.mods;
        if (e.precise) {
          c.m.wheelX += e.wheelX / c.uiScale;
          c.m.wheelY += e.wheelY / c.uiScale;
          c.wheelPrecise = true;
        } else {
          c.m.wheelX += e.wheelX * 64;
          c.m.wheelY += e.wheelY * 64;
        }
        break;
      }
      case EventType::MouseLeave:
        c.mouseInside = false;
        c.m.x = c.m.y = -1e6f;
        break;
      case EventType::KeyDown:
      case EventType::Text: {
        if (button) { pop = false; stop = true; break; }
        c.m.mods = e.type == EventType::KeyDown ? e.mods : c.m.mods;
        c.kbd.push_back({e, false});
        c.lastInputTime = c.time;
        key = true;
        if (e.type == EventType::KeyDown) {
          if (e.key == Key::Tab && !(e.mods & (platform::ModCtrl | platform::ModAlt | platform::ModSuper))) {
            c.kbd.back().consumed = true;
            tabNavigate(c, (e.mods & platform::ModShift) != 0);
            stop = true;
          } else if (e.key == Key::Enter || e.key == Key::NumEnter || e.key == Key::Escape) {
            stop = true;
          }
        }
        break;
      }
      case EventType::KeyUp:
        c.m.mods = e.mods;
        break;
      case EventType::FocusOut:
        for (bool& d : c.m.down) d = false;
        c.active = 0;
        c.activeDragging = false;
        c.drag = Ctx::Drag{};
        break;
      default:
        break;
    }
    if (!pop) break;
    c.queue.pop_front();
    if (stop) break;
  }
}

// ---------------------------------------------------------------- фокус
void setFocus(WidgetId id, bool visible) {
  Ctx& c = C();
  if (c.focus != id) c.lastInputTime = c.time;
  c.focus = id;
  c.focusVisible = visible;
  c.focusSeen = true;   // назначен в этом кадре — не снимать в его конце
}

void clearFocus() {
  C().focus = 0;
  C().focusVisible = false;
}

static void tabNavigate(Ctx& c, bool back) {
  // Список прошлого кадра (текущий ещё не собран).
  const auto& list = c.focusList;
  WidgetId scope = 0;
  bool found = false;
  for (auto& f : list)
    if (f.id == c.focus) { scope = f.scope; found = true; }
  if (!c.modals.empty()) {
    // Фокус вне модального окна — переносим внутрь.
    bool inModal = false;
    for (auto& m : c.modals)
      if (scope == m.id) inModal = true;
    for (auto& p : c.popups)
      if (scope == p.id) inModal = true;
    if (!found || !inModal) { scope = c.modals.back().id; found = false; }
  } else if (!found) {
    scope = c.popups.empty() ? 0 : c.popups.back().id;
  }
  std::vector<WidgetId> ids;
  for (auto& f : list)
    if (f.scope == scope) ids.push_back(f.id);
  if (ids.empty()) return;
  int cur = -1;
  for (size_t i = 0; i < ids.size(); i++)
    if (ids[i] == c.focus) cur = int(i);
  int n = int(ids.size());
  int next = cur < 0 ? (back ? n - 1 : 0) : (back ? (cur - 1 + n) % n : (cur + 1) % n);
  setFocus(ids[size_t(next)], true);
  c.focusJustMoved = true;
}

void registerFocusable(WidgetId id) {
  Ctx& c = C();
  if (c.focus == id) c.focusSeen = true;   // и в кадре замера (автофокус всплывающего окна)
  if (!c.win || c.win->hidden) return;
  c.focusList.push_back({id, c.win->scope});
}

// ---------------------------------------------------------------- клавиши
static bool keyMatch(const KeyEv& k, Key key, u32 mods) {
  if (k.consumed || k.e.type != EventType::KeyDown) return false;
  if (k.e.key != key && !(key == Key::Enter && k.e.key == Key::NumEnter)) return false;
  u32 em = k.e.mods & (platform::ModShift | platform::ModCtrl | platform::ModAlt | platform::ModSuper);
  return em == normMods(mods);
}

bool hasKey(Key k, u32 mods) {
  for (auto& e : C().kbd)
    if (keyMatch(e, k, mods)) return true;
  return false;
}

bool takeKey(Key k, u32 mods) {
  for (auto& e : C().kbd)
    if (keyMatch(e, k, mods)) {
      e.consumed = true;
      return true;
    }
  return false;
}

std::string clipboardGet() {
  Ctx& c = C();
  if (c.clipGet) return c.clipGet();
  return c.clipLocal;
}

void clipboardSet(const std::string& s) {
  Ctx& c = C();
  if (c.clipSet) c.clipSet(s);
  else c.clipLocal = s;
}

void markInput() { C().lastInputTime = C().time; }

// ---------------------------------------------------------------- окна
Window* beginWindow(WidgetId id, WinKind kind, int rank, u32 order, RectF r) {
  Ctx& c = C();
  auto& slot = c.windows[id];
  if (!slot) {
    slot = std::make_unique<Window>();
    slot->id = id;
  }
  Window* w = slot.get();
  if (w->lastFrame != c.frame) {
    w->list.clear();
    w->prevContentSize = w->contentSize;
    w->wasHidden = w->hidden;
    w->hidden = false;
    w->blur = false;
    w->backdrop = false;
    w->alpha = 1;
    w->dy = 0;
    w->lastFrame = c.frame;
    c.frameWins.push_back(w);
  } else if (kind != WinKind::Root) {
    logWarn("ui: окно %016llx начато дважды за кадр", (unsigned long long)id);
  }
  w->kind = kind;
  w->rank = rank;
  w->order = order;
  w->rect = r;
  w->scope = (kind == WinKind::Popup || kind == WinKind::Modal) ? id : (c.win ? c.win->scope : 0);
  if (kind == WinKind::Root || kind == WinKind::Panel) w->scope = 0;
  c.winStack.push_back(w);
  c.win = w;
  return w;
}

void endWindow() {
  Ctx& c = C();
  if (c.winStack.size() <= 1) return;
  c.winStack.pop_back();
  c.win = c.winStack.back();
}

bool isDisabled() { return C().disabled > 0; }

bool modalBlocks() {
  Ctx& c = C();
  if (c.modals.empty() || !c.win) return false;
  return c.win->rank < kRankModal + 10 * int(c.modals.size() - 1);
}

// ---------------------------------------------------------------- ID
bool registerId(WidgetId id) {
  Ctx& c = C();
  if (c.seenIds.insert(id).second) return true;
  if (c.warnedIds.insert(id).second) logWarn("ui: повторяющийся ID виджета %016llx рядом с элементом (%.0f, %.0f) — используйте ui::IdScope или «текст##ключ»", (unsigned long long)id, c.last.rect.x, c.last.rect.y);
  return false;
}

void setLast(WidgetId id, RectF r, const Interaction& it) {
  Ctx& c = C();
  c.last = Item{};
  c.last.id = id;
  c.last.rect = r;
  c.last.hovered = it.hovered;
  c.last.active = c.active == id;
  c.last.focused = c.focus == id && id != 0;
  c.last.clicked = it.clicked;
  c.last.rightClicked = it.rightClicked;
  c.last.doubleClicked = it.doubleClicked;
  c.last.deactivated = it.released;
  if (c.last.focused && c.focusJustMoved) {
    c.focusJustMoved = false;
    scrollToItem();
  }
}

void focusRing(RectF r, float radius) {
  Ctx& c = C();
  if (!c.focusVisible || !c.last.focused) return;
  cmdStroke(r.expand(3), c.th.accent, radius + 3, c.th.focusRing);
}

}  // namespace in

// ================================================================ публичное: кадр
void init() {
  Ctx& c = C();
  if (c.inited) return;
  std::string err;
  if (!gfx::initFonts(&err)) logError("ui: %s", err.c_str());
  c.th = darkTheme();
  c.idStack.assign(1, hash64("regnum-ui"));
  c.inited = true;
}

void shutdown() {
  C() = Ctx();
}

void onEvent(const platform::Event& e) {
  Ctx& c = C();
  switch (e.type) {
    case EventType::MouseMove:
    case EventType::MouseDown:
    case EventType::MouseUp:
    case EventType::MouseWheel:
    case EventType::MouseLeave:
    case EventType::KeyDown:
    case EventType::KeyUp:
    case EventType::Text:
    case EventType::FocusOut:
      // Подряд идущие перемещения сливаются (последнее положение).
      if (e.type == EventType::MouseMove && !c.queue.empty() && c.queue.back().type == EventType::MouseMove) c.queue.back() = e;
      else c.queue.push_back(e);
      break;
    default:
      break;
  }
}

void beginFrame(float logicalW, float logicalH, float dpiScale, double timeSec) {
  Ctx& c = C();
  if (!c.inited) init();
  if (c.inFrame) {
    logWarn("ui: beginFrame без endFrame");
    c.frames.clear();
    c.winStack.clear();
  }
  c.dpi = dpiScale > 0 && std::isfinite(dpiScale) ? dpiScale : 1;
  c.ds = c.dpi * c.uiScale;
  c.view = RectF{0, 0, std::max(1.f, logicalW) / c.uiScale, std::max(1.f, logicalH) / c.uiScale};
  c.dt = c.frame == 0 ? 1.f / 60 : float(clamp(timeSec - c.time, 0.0, 0.25));
  c.prevTime = c.time;
  c.time = timeSec;
  c.frame++;

  // Ввод этого кадра
  for (int i = 0; i < 3; i++) c.pressedEv[i] = c.releasedEv[i] = false;
  c.clicks = 0;
  c.m.wheelX = c.m.wheelY = 0;
  c.wheelPrecise = false;
  c.mouseMoved = false;
  c.kbd.clear();
  c.rightReleaseClaimed = false;
  c.pressClaimed = false;
  processEvents(c);

  // Наведение: последний элемент под указателем в прошлом кадре
  c.hotLastPrev = c.hotLast;
  c.hotLast = 0;
  c.hot = 0;
  if (!c.activeSeen && c.active) {
    c.active = 0;
    c.activeDragging = false;
  }
  c.activeSeen = false;
  if (c.active && !c.m.down[c.activeButton] && !c.releasedEv[c.activeButton]) {
    c.active = 0;
    c.activeDragging = false;
  }

  // Окно под указателем (по прямоугольникам прошлого кадра)
  int modalRank = -1;
  for (Window* w : c.frameWins)
    if (w->kind == WinKind::Modal) modalRank = std::max(modalRank, w->rank);
  std::vector<Window*> order = c.frameWins;
  std::stable_sort(order.begin(), order.end(), [](Window* a, Window* b) { return a->rank != b->rank ? a->rank > b->rank : a->order > b->order; });
  c.hoveredWin = nullptr;
  for (Window* w : order) {
    if (w->hidden || w->kind == WinKind::Tooltip || w->kind == WinKind::Drag) continue;
    if (modalRank >= 0 && w->rank < modalRank) break;
    if (w->rect.contains(c.m.x, c.m.y)) {
      c.hoveredWin = w;
      break;
    }
  }
  if (!c.hoveredWin && modalRank < 0) c.hoveredWin = c.root;
  for (Window* w : c.frameWins) w->prevRect = w->rect;

  c.prevFocusList.swap(c.focusList);
  c.focusList.clear();
  c.focusSeen = false;
  c.seenIds.clear();
  c.frameWins.clear();
  c.panelSeq = 0;
  c.animating = false;
  c.redraw = false;
  c.cursor = platform::Cursor::Arrow;
  c.textInput.reset();
  c.caretBlink = false;
  c.closedOpener = 0;
  c.disabled = 0;
  c.last = Item{};
  c.idStack.assign(1, hash64("regnum-ui"));

  overlaysBeginFrame();

  // Корневое окно и его поток
  c.winStack.clear();
  c.win = nullptr;
  c.root = beginWindow(hash64("##root"), WinKind::Root, kRankBase, 0, c.view);
  c.inFrame = true;
  layoutBeginFrame();
}

void endFrame(gfx::Canvas& canvas) {
  Ctx& c = C();
  if (!c.inFrame) {
    logWarn("ui: endFrame без beginFrame");
    return;
  }
  layoutEndFrame();
  overlaysEndFrame();
  if (c.focus && !c.focusSeen) clearFocus();
  // Щелчок мимо всех элементов снимает фокус (поле фиксирует значение).
  if ((c.pressedEv[0] || c.pressedEv[1]) && !c.pressClaimed && c.focus) clearFocus();
  if (c.drag.active) c.cursor = platform::Cursor::Grabbing;
  if (c.drag.active && !c.m.down[0]) c.drag = Ctx::Drag{};

  // Уборка устойчивого состояния
  if (c.frame % 30 == 0) {
    for (auto it = c.states.begin(); it != c.states.end();) {
      if (it->second.lastFrame + 300 < c.frame && it->second.lastTime + 5 < c.time) it = c.states.erase(it);
      else ++it;
    }
    for (auto it = c.anims.begin(); it != c.anims.end();) {
      if (it->second.lastFrame + 120 < c.frame) it = c.anims.erase(it);
      else ++it;
    }
    for (auto it = c.texts.begin(); it != c.texts.end();) {
      if (it->second->lastFrame + 180 < c.frame) it = c.texts.erase(it);
      else ++it;
    }
    for (auto it = c.windows.begin(); it != c.windows.end();) {
      if (it->second.get() != c.root && it->second->lastFrame + 600 < c.frame) it = c.windows.erase(it);
      else ++it;
    }
  }
  renderAll(canvas);
  c.inFrame = false;
}

bool wantsMouse() {
  Ctx& c = C();
  if (c.active || c.drag.active || !c.modals.empty() || !c.popups.empty()) return true;
  if (c.hoveredWin && c.hoveredWin != c.root) return true;
  return c.hotLast != 0;
}

bool wantsKeyboard() {
  Ctx& c = C();
  return c.textInput.has_value();
}

bool needsRedraw() {
  Ctx& c = C();
  if (!c.queue.empty() || c.animating || c.redraw || c.tipPending || c.caretBlink || c.drag.active) return true;
  if (!c.toasts.empty() || !c.ghosts.empty()) return true;
  for (auto& [id, w] : c.windows)
    if (w->lastFrame == c.frame && w->hidden) return true;
  return false;
}

platform::Cursor cursor() { return C().cursor; }

std::optional<RectF> textInputRect() {
  Ctx& c = C();
  if (!c.textInput) return std::nullopt;
  RectF r = *c.textInput;
  float s = c.uiScale;
  return RectF{r.x * s, r.y * s, r.w * s, r.h * s};
}

void setClipboard(std::function<std::string()> get, std::function<void(const std::string&)> set) {
  C().clipGet = std::move(get);
  C().clipSet = std::move(set);
}

double time() { return C().time; }
float dt() { return C().dt; }
u64 frameIndex() { return C().frame; }
RectF viewport() { return C().view; }
float deviceScale() { return C().ds; }
const Mouse& mouse() { return C().m; }
void setCursor(platform::Cursor cur) { C().cursor = cur; }
void requestRedraw() { C().redraw = true; }

bool shortcut(Shortcut s) {
  Ctx& c = C();
  if (!s) return false;
  if (modalBlocks()) return false;
  u32 m = normMods(s.mods);
  bool plain = (m & (platform::ModCtrl | platform::ModAlt | platform::ModSuper)) == 0;
  if (plain && c.textInput) return false;
  if (plain && c.focus && hasKey(s.key, s.mods) && (s.key == Key::Space || s.key == Key::Enter)) return false;
  return takeKey(s.key, s.mods);
}

bool keyPressed(Key k, u32 mods) { return hasKey(k, mods); }
void consumeKey(Key k) {
  for (auto& e : C().kbd)
    if (!e.consumed && e.e.type == EventType::KeyDown && e.e.key == k) e.consumed = true;
}

// ================================================================ ID
WidgetId id(std::string_view s) { return hash64(s, C().idStack.empty() ? 0 : C().idStack.back()); }
WidgetId id(i64 n) { return hashMix(C().idStack.empty() ? 0 : C().idStack.back(), u64(n) * 0x9e3779b97f4a7c15ull + 0x632be59bd9b4e019ull); }
void pushId(std::string_view s) { C().idStack.push_back(id(s)); }
void pushId(i64 n) { C().idStack.push_back(id(n)); }
void pushId(const void* p) { C().idStack.push_back(id(i64(reinterpret_cast<uintptr_t>(p)))); }
void popId() {
  Ctx& c = C();
  if (c.idStack.size() > 1) c.idStack.pop_back();
  else logWarn("ui: popId без pushId");
}

std::string_view displayText(std::string_view label) {
  size_t p = label.find("##");
  return p == std::string_view::npos ? label : label.substr(0, p);
}

const Item& lastItem() { return C().last; }

// ================================================================ взаимодействие
Interaction interact(WidgetId wid, RectF r, u32 flags) {
  Ctx& c = C();
  Interaction it;
  registerId(wid);
  it.mx = c.m.x;
  it.my = c.m.y;
  bool disabled = c.disabled > 0;
  RectF clip = currentClip();
  bool inside = c.win && !c.win->hidden && c.win == c.hoveredWin && r.contains(c.m.x, c.m.y) && clip.contains(c.m.x, c.m.y);
  if (inside) c.hotLast = wid;
  bool canHover = inside && !disabled && (c.active == 0 || c.active == wid) && (!c.drag.active || c.active == wid);
  if (canHover && (flags & IfAllowOverlap) && c.hotLastPrev != wid) canHover = false;
  if (canHover) {
    it.hovered = true;
    c.hot = wid;
  }
  // Нажатие
  if (it.hovered && !c.pressClaimed && c.active == 0) {
    for (int b = 0; b < 3; b++) {
      if (!c.pressedEv[b]) continue;
      bool allowed = b == 0 || (b == 1 && (flags & IfRightButton)) || (b == 2 && (flags & IfMiddleButton));
      if (!allowed) {
        if (b == 1) c.pressClaimed = true;   // правый щелчок по элементу не снимает фокус
        continue;
      }
      c.active = wid;
      c.activeButton = b;
      c.activeDragging = false;
      c.pressClaimed = true;
      c.pressX = c.m.x;
      c.pressY = c.m.y;
      it.pressed = true;
      it.button = b;
      if (b == 0 && c.clicks >= 2) it.doubleClicked = true;
      if (flags & IfFocusable) setFocus(wid, false);
      else if (b == 0 && c.focus && c.focus != wid) clearFocus();
      break;
    }
  }
  if (c.active == wid) {
    c.activeSeen = true;
    it.button = c.activeButton;
    it.held = c.m.down[c.activeButton];
    it.dx = c.m.x - c.pressX;
    it.dy = c.m.y - c.pressY;
    if (!c.activeDragging && (std::fabs(it.dx) > 3 || std::fabs(it.dy) > 3)) c.activeDragging = true;
    it.dragging = c.activeDragging && it.held;
    if (c.releasedEv[c.activeButton]) {
      it.released = true;
      bool over = r.contains(c.m.x, c.m.y) && clip.contains(c.m.x, c.m.y) && c.win == c.hoveredWin;
      if (over && !disabled) {
        if (c.activeButton == 0) it.clicked = true;
        else if (c.activeButton == 1) it.rightClicked = true;
        else it.middleClicked = true;
      }
      c.active = 0;
      c.activeDragging = false;
      it.held = false;
    }
  }
  // Правый щелчок без захвата (контекстное меню) — по отпусканию
  if (!(flags & IfRightButton) && it.hovered && c.releasedEv[1] && !c.rightReleaseClaimed) {
    it.rightClicked = true;
    c.rightReleaseClaimed = true;
  }
  if (flags & IfFocusable) {
    if (!disabled) registerFocusable(wid);
    it.focused = c.focus == wid;
    // Enter/Пробел — только если клавиатура не принадлежит всплывающему или модальному окну выше
    bool keysHere = !modalBlocks() && (c.popups.empty() || (c.win && c.win->kind == WinKind::Popup && c.win->id == c.popups.back().id));
    if (it.focused && !disabled && !(flags & IfTextInput) && keysHere) {
      if (takeKey(Key::Enter) || takeKey(Key::Space)) {
        it.keyActivated = true;
        it.clicked = true;
      }
    }
  }
  return it;
}

// ================================================================ состояние
namespace detail {
StateBox* stateLookup(WidgetId id, const void* tag, StateBox* (*make)()) {
  Ctx& c = C();
  StateEntry& e = c.states[id];
  if (!e.box || e.tag != tag) {
    if (e.box) logWarn("ui: ID %016llx хранит состояние другого типа", (unsigned long long)id);
    e.box.reset(make());
    e.tag = tag;
  }
  e.lastFrame = c.frame;
  e.lastTime = c.time;
  return e.box.get();
}
}  // namespace detail

float animate(WidgetId id, float target, float duration) {
  Ctx& c = C();
  auto [it, fresh] = c.anims.try_emplace(id);
  Anim& a = it->second;
  if (fresh) {
    a.value = a.from = a.to = target;
    a.start = c.time;
    a.dur = duration;
  } else if (a.to != target) {
    a.from = a.value;
    a.to = target;
    a.start = c.time;
    a.dur = duration;
  }
  a.lastFrame = c.frame;
  float t = a.dur > 0 ? float((c.time - a.start) / a.dur) : 1.f;
  if (t >= 1 || !std::isfinite(t)) a.value = a.to;
  else {
    a.value = a.from + (a.to - a.from) * easeOut(t);
    c.animating = true;
  }
  return a.value;
}

Color animateColor(WidgetId id, Color target, float duration) {
  struct CA {
    Color from, to, value;
    double start = 0;
    bool init = false;
  };
  Ctx& c = C();
  CA& a = state<CA>(id);
  if (!a.init) {
    a.from = a.to = a.value = target;
    a.init = true;
  } else if (!(a.to == target)) {
    a.from = a.value;
    a.to = target;
    a.start = c.time;
  }
  float t = duration > 0 ? float((c.time - a.start) / duration) : 1.f;
  if (t >= 1) a.value = a.to;
  else {
    a.value = Color::mix(a.from, a.to, easeOut(t));
    c.animating = true;
  }
  return a.value;
}

}  // namespace rg::ui

namespace rg::ui {
int pendingEvents() { return int(in::C().queue.size()); }
}  // namespace rg::ui

namespace rg::ui {
void setKeyboardFocus(WidgetId wid) {
  in::setFocus(wid, true);
  in::C().focusJustMoved = wid != 0;
}
WidgetId keyboardFocus() { return in::C().focus; }
}  // namespace rg::ui
