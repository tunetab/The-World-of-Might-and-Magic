// Regnum — редактирование текста: каретка, выделение мышью и клавиатурой, слова, буфер обмена, отмена;
// однострочное и многострочное поля, поле поиска, числовое поле (перетаскивание, шаги, проверка ввода).
#include "ui/ui_internal.h"

namespace rg::ui {

using namespace in;

namespace in {

namespace {

constexpr size_t kUndoMax = 200;

bool isWordAt(std::string_view s, size_t i) {
  if (i >= s.size()) return false;
  size_t j = i;
  return utf8::isWordChar(utf8::decode(s, j));
}

size_t prevWord(std::string_view s, size_t i) {
  while (i > 0) {
    size_t p = utf8::prev(s, i);
    if (isWordAt(s, p)) break;
    i = p;
  }
  while (i > 0) {
    size_t p = utf8::prev(s, i);
    if (!isWordAt(s, p)) break;
    i = p;
  }
  return i;
}

size_t nextWord(std::string_view s, size_t i) {
  while (i < s.size() && !isWordAt(s, i)) i = utf8::next(s, i);
  while (i < s.size() && isWordAt(s, i)) i = utf8::next(s, i);
  return i;
}

void wordAt(std::string_view s, size_t i, size_t& a, size_t& b) {
  if (s.empty()) {
    a = b = 0;
    return;
  }
  if (i >= s.size()) i = utf8::prev(s, s.size());
  bool w = isWordAt(s, i);
  if (!w && i > 0 && isWordAt(s, utf8::prev(s, i))) {
    i = utf8::prev(s, i);
    w = true;
  }
  a = i;
  b = utf8::next(s, i);
  if (!w) return;
  while (a > 0 && isWordAt(s, utf8::prev(s, a))) a = utf8::prev(s, a);
  while (b < s.size() && isWordAt(s, b)) b = utf8::next(s, b);
}

const gfx::TextLayout& editLayout(const TextEdit& te, const EditParams& p) {
  if (p.multiline) return cachedText(te.buf, *p.style, std::max(1.f, p.textRect.w), 0, false, Align::Left, true)->layout;
  return cachedText(te.buf, *p.style)->layout;
}

float singleLineY(const EditParams& p) {
  float lh = gfx::metrics(*p.style).lineHeight;
  return p.textRect.y + std::round((p.textRect.h - lh) * 0.5f);
}

Vec2 originOf(const TextEdit& te, const EditParams& p) {
  if (p.multiline) return {p.textRect.x, p.textRect.y - te.scrollY};
  return {p.textRect.x - te.scrollX, singleLineY(p)};
}

size_t selMin(const TextEdit& te) { return std::min(te.caret, te.anchor); }
size_t selMax(const TextEdit& te) { return std::max(te.caret, te.anchor); }

void pushUndo(TextEdit& te, bool typing) {
  Ctx& c = C();
  if (typing && te.typingGroup && c.time - te.lastTypeTime < 1.0) {
    te.lastTypeTime = c.time;
    return;
  }
  te.undo.push_back({te.buf, te.caret, te.anchor});
  if (te.undo.size() > kUndoMax) te.undo.erase(te.undo.begin());
  te.redo.clear();
  te.typingGroup = typing;
  te.lastTypeTime = c.time;
}

void eraseSel(TextEdit& te) {
  size_t a = selMin(te), b = selMax(te);
  te.buf.erase(a, b - a);
  te.caret = te.anchor = a;
}

// Вставка с фильтром символов и ограничением длины.
bool insertText(TextEdit& te, const EditParams& p, std::string_view s, bool typing) {
  std::string clean;
  size_t i = 0;
  while (i < s.size()) {
    u32 cp = utf8::decode(s, i);
    if (cp == '\r') continue;
    if (cp == '\n' || cp == '\t') {
      if (!p.multiline) cp = ' ';
      else if (cp == '\t') cp = ' ';
    } else if (cp < 0x20 || cp == 0x7f) {
      continue;
    }
    if (p.filter && !p.filter(cp)) continue;
    utf8::append(clean, cp);
  }
  if (clean.empty()) return false;
  if (p.maxLength > 0) {
    size_t have = utf8::count(te.buf) - utf8::count(std::string_view(te.buf).substr(selMin(te), selMax(te) - selMin(te)));
    size_t room = have >= size_t(p.maxLength) ? 0 : size_t(p.maxLength) - have;
    size_t n = 0, j = 0;
    while (j < clean.size() && n < room) {
      j = utf8::next(clean, j);
      n++;
    }
    clean.resize(j);
    if (clean.empty()) return false;
  }
  pushUndo(te, typing);
  eraseSel(te);
  te.buf.insert(te.caret, clean);
  te.caret += clean.size();
  te.anchor = te.caret;
  return true;
}

void moveCaret(TextEdit& te, size_t pos, bool shift) {
  te.caret = std::min(pos, te.buf.size());
  if (!shift) te.anchor = te.caret;
  te.typingGroup = false;
}

}  // namespace

void editBegin(TextEdit& te, const std::string& value, bool selectAll) {
  te.buf = value;
  te.original = value;
  te.caret = te.buf.size();
  te.anchor = selectAll ? 0 : te.caret;
  te.editing = true;
  te.undo.clear();
  te.redo.clear();
  te.typingGroup = false;
  te.desiredX = -1;
  te.mouseSelecting = false;
  te.scrollX = 0;
  te.prevCaret = te.prevSize = size_t(-1);
  markInput();
}

EditResult editProcess(TextEdit& te, const EditParams& p, const Interaction& it, WidgetId wid) {
  Ctx& c = C();
  EditResult res;
  te.caret = std::min(te.caret, te.buf.size());
  te.anchor = std::min(te.anchor, te.buf.size());
  // ---- мышь
  {
    const gfx::TextLayout& L = editLayout(te, p);
    Vec2 o = originOf(te, p);
    if (it.pressed && it.button == 0) {
      size_t pos = gfx::hitTest(L, c.m.x - float(o.x), c.m.y - float(o.y));
      bool shift = (c.m.mods & platform::ModShift) != 0;
      if (c.clicks >= 3) {
        te.anchor = 0;
        te.caret = te.buf.size();
        te.selMode = 2;
      } else if (c.clicks == 2) {
        wordAt(te.buf, pos, te.wordA, te.wordB);
        te.anchor = te.wordA;
        te.caret = te.wordB;
        te.selMode = 1;
      } else {
        te.caret = pos;
        if (!shift) te.anchor = pos;
        te.selMode = 0;
      }
      te.mouseSelecting = true;
      te.desiredX = -1;
      te.typingGroup = false;
      markInput();
    } else if (te.mouseSelecting) {
      if (c.active == wid && it.held) {
        size_t pos = gfx::hitTest(L, c.m.x - float(o.x), c.m.y - float(o.y));
        if (te.selMode == 0) te.caret = pos;
        else if (te.selMode == 1) {
          size_t a, b;
          wordAt(te.buf, pos, a, b);
          if (pos < te.wordA) {
            te.anchor = te.wordB;
            te.caret = a;
          } else {
            te.anchor = te.wordA;
            te.caret = std::max(b, te.wordB);
          }
        }
      } else {
        te.mouseSelecting = false;
      }
    }
  }
  // ---- клавиатура
  if (c.focus == wid) {
    const u32 primary = platform::primaryMod();
    for (KeyEv& k : c.kbd) {
      if (k.consumed) continue;
      const platform::Event& e = k.e;
      if (e.type == platform::EventType::Text) {
        if (!p.readOnly && insertText(te, p, e.text, true)) res.edited = true;
        k.consumed = true;
        markInput();
        continue;
      }
      if (e.type != platform::EventType::KeyDown) continue;
      u32 m = e.mods & (platform::ModShift | platform::ModCtrl | platform::ModAlt | platform::ModSuper);
      bool shift = (m & platform::ModShift) != 0;
      bool word = (m & (platform::ModCtrl | platform::ModAlt)) != 0;
      bool prim = (m & primary) != 0 && !(m & platform::ModAlt);
      // ⌘←/⌘→ (macOS) — к началу/концу строки, ⌘↑/⌘↓ — к началу/концу текста.
      platform::Key key = e.key;
      if ((m & platform::ModSuper) && !(m & platform::ModCtrl)) {
        if (key == Key::Left) key = Key::Home;
        else if (key == Key::Right) key = Key::End;
        else if (key == Key::Up || key == Key::Down) {
          moveCaret(te, key == Key::Up ? 0 : te.buf.size(), shift);
          k.consumed = true;
          continue;
        }
      }
      bool handled = true;
      const gfx::TextLayout& L = editLayout(te, p);
      switch (key) {
        case Key::Left:
          if (te.caret != te.anchor && !shift) moveCaret(te, selMin(te), false);
          else moveCaret(te, word ? prevWord(te.buf, te.caret) : utf8::prev(te.buf, te.caret), shift);
          te.desiredX = -1;
          break;
        case Key::Right:
          if (te.caret != te.anchor && !shift) moveCaret(te, selMax(te), false);
          else moveCaret(te, word ? nextWord(te.buf, te.caret) : utf8::next(te.buf, te.caret), shift);
          te.desiredX = -1;
          break;
        case Key::Up:
        case Key::Down:
        case Key::PageUp:
        case Key::PageDown: {
          if (!p.multiline) {
            handled = false;
            break;
          }
          gfx::Pt cp = gfx::caretPos(L, te.caret);
          if (te.desiredX < 0) te.desiredX = cp.x;
          float lh = std::max(1.f, L.lineHeight);
          float dy = key == Key::Up ? -lh : key == Key::Down ? lh : (key == Key::PageUp ? -1.f : 1.f) * std::max(lh, p.textRect.h - lh);
          size_t pos = gfx::hitTest(L, te.desiredX, cp.y + dy + lh * 0.5f);
          if (key == Key::Up && cp.y < lh * 0.5f) pos = 0;
          if (key == Key::Down && cp.y + lh >= L.height - 0.5f) pos = te.buf.size();
          float keep = te.desiredX;
          moveCaret(te, pos, shift);
          te.desiredX = keep;
          break;
        }
        case Key::Home:
        case Key::End: {
          size_t pos;
          if (word || !p.multiline) pos = key == Key::Home ? 0 : te.buf.size();
          else {
            size_t li = gfx::lineOf(L, te.caret);
            if (li < L.lines.size()) pos = key == Key::Home ? L.lines[li].begin : L.lines[li].end;
            else pos = key == Key::Home ? 0 : te.buf.size();
          }
          moveCaret(te, pos, shift);
          te.desiredX = -1;
          break;
        }
        case Key::Backspace:
          if (p.readOnly) break;
          if (te.caret != te.anchor) {
            pushUndo(te, false);
            eraseSel(te);
            res.edited = true;
          } else if (te.caret > 0) {
            size_t a = word ? prevWord(te.buf, te.caret) : utf8::prev(te.buf, te.caret);
            pushUndo(te, !word);
            te.buf.erase(a, te.caret - a);
            te.caret = te.anchor = a;
            res.edited = true;
          }
          break;
        case Key::Delete:
          if (p.readOnly) break;
          if (te.caret != te.anchor) {
            pushUndo(te, false);
            eraseSel(te);
            res.edited = true;
          } else if (te.caret < te.buf.size()) {
            size_t b = word ? nextWord(te.buf, te.caret) : utf8::next(te.buf, te.caret);
            pushUndo(te, false);
            te.buf.erase(te.caret, b - te.caret);
            te.anchor = te.caret;
            res.edited = true;
          }
          break;
        case Key::Enter:
        case Key::NumEnter:
          if (p.multiline && !prim) {
            if (!p.readOnly && insertText(te, p, "\n", false)) res.edited = true;
          } else {
            res.enter = true;
            handled = false;   // модальному окну — кнопка по умолчанию
          }
          break;
        case Key::Escape:
          res.escape = true;
          break;
        case Key::A:
          if (prim && !shift) {
            te.anchor = 0;
            te.caret = te.buf.size();
          } else handled = false;
          break;
        case Key::C:
        case Key::X:
          if (prim && !shift) {
            if (te.caret != te.anchor) {
              clipboardSet(te.buf.substr(selMin(te), selMax(te) - selMin(te)));
              if (key == Key::X && !p.readOnly) {
                pushUndo(te, false);
                eraseSel(te);
                res.edited = true;
              }
            }
          } else handled = false;
          break;
        case Key::V:
          if (prim && !shift) {
            if (!p.readOnly && insertText(te, p, clipboardGet(), false)) res.edited = true;
            te.typingGroup = false;
          } else handled = false;
          break;
        case Key::Z:
        case Key::Y: {
          bool undo = key == Key::Z && prim && !shift;
          bool redo = (key == Key::Y && prim && !shift) || (key == Key::Z && prim && shift);
          if (!undo && !redo) {
            handled = false;
            break;
          }
          auto& from = undo ? te.undo : te.redo;
          auto& to = undo ? te.redo : te.undo;
          if (!from.empty() && !p.readOnly) {
            to.push_back({te.buf, te.caret, te.anchor});
            TextEdit::Snap s = from.back();
            from.pop_back();
            te.buf = s.text;
            te.caret = s.caret;
            te.anchor = s.anchor;
            te.typingGroup = false;
            res.edited = true;
          }
          break;
        }
        default:
          // Буквенные клавиши без модификаторов приходят и текстом — поглощаем, чтобы не сработали сочетания.
          handled = !(m & (platform::ModCtrl | platform::ModAlt | platform::ModSuper)) && key != Key::Tab && key != Key::Unknown &&
                    !(key >= Key::F1 && key <= Key::F12);
          break;
      }
      if (handled) {
        k.consumed = true;
        markInput();
      }
    }
  }
  te.caret = std::min(te.caret, te.buf.size());
  te.anchor = std::min(te.anchor, te.buf.size());
  // ---- каретка в видимой области
  const gfx::TextLayout& L = editLayout(te, p);
  if (te.caret != te.prevCaret || te.buf.size() != te.prevSize || te.mouseSelecting) {
    gfx::Pt cp = gfx::caretPos(L, te.caret);
    if (p.multiline) {
      float lh = std::max(1.f, L.lineHeight);
      if (cp.y - te.scrollY + lh > p.textRect.h) te.scrollY = cp.y + lh - p.textRect.h;
      if (cp.y < te.scrollY) te.scrollY = cp.y;
    } else {
      float vw = p.textRect.w;
      if (cp.x - te.scrollX > vw - 2) te.scrollX = cp.x - vw + 2;
      if (cp.x - te.scrollX < 0) te.scrollX = cp.x;
    }
    te.prevCaret = te.caret;
    te.prevSize = te.buf.size();
  }
  if (p.multiline) te.scrollY = clamp(te.scrollY, 0.f, std::max(0.f, L.height - p.textRect.h));
  else te.scrollX = clamp(te.scrollX, 0.f, std::max(0.f, L.width - p.textRect.w + 2));
  if (c.focus == wid) {
    Vec2 o = originOf(te, p);
    gfx::Pt cp = gfx::caretPos(L, te.caret);
    c.textInput = RectF{float(o.x) + cp.x, float(o.y) + cp.y, 1, std::max(1.f, L.lineHeight)};
    c.caretBlink = c.time - c.lastInputTime < 10;
  }
  return res;
}

void editDraw(TextEdit& te, const EditParams& p, bool focused, Color textColor) {
  Ctx& c = C();
  const Theme& t = c.th;
  const std::string& s = te.buf;
  TextEntry* e = p.multiline ? cachedText(s, *p.style, std::max(1.f, p.textRect.w), 0, false, Align::Left, true) : cachedText(s, *p.style);
  const gfx::TextLayout& L = e->layout;
  Vec2 o = originOf(te, p);
  draw::pushClip(RectF{p.textRect.x - 2, p.textRect.y, p.textRect.w + 4, p.textRect.h}.intersect(currentClip()));
  if (te.caret != te.anchor) {
    for (const RectF& sr : gfx::selectionRects(L, selMin(te), selMax(te))) {
      RectF rr{float(o.x) + sr.x, float(o.y) + sr.y, std::max(sr.w, 3.f), sr.h};
      cmdRect(rr, focused ? t.selection : t.textMuted.alpha(0.22f), 2);
    }
  }
  cmdText(e, float(o.x), float(o.y), textColor);
  if (focused) {
    double since = c.time - c.lastInputTime;
    bool on = since < 0.5 || since >= 10 || std::fmod(since - 0.5, 1.06) < 0.53;
    if (on) {
      gfx::Pt cp = gfx::caretPos(L, te.caret);
      float lh = std::max(1.f, L.lineHeight);
      float x = float(o.x) + cp.x;
      cmdRect(RectF{std::round(x * c.ds) / c.ds - 0.5f, float(o.y) + cp.y + 1, 1.5f, lh - 2}, t.accent, 0.5f);
    }
  }
  draw::popClip();
}

}  // namespace in

// ================================================================ поля
namespace {

struct FieldGeom {
  RectF r, textRect, clear, iconR;
};

}  // namespace

bool textField(std::string_view name, std::string& value, const TextOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  DisableGuard dg(o.disabled);
  bool disabled = isDisabled();
  WidgetId wid = id(name);
  TextEdit& te = state<TextEdit>(wid);
  float h = t.controlH;
  RectF r = place(0, h);
  if (r.h > h) r = RectF{r.x, r.y + std::round((r.h - h) * 0.5f), r.w, h};
  const gfx::TextStyle& st = styleOf(Font::Body);
  float x0 = r.x + 10 + (o.icon ? 24 : 0);
  const std::string& shown = te.editing ? te.buf : value;
  bool showClear = o.clearButton && !o.readOnly && !disabled && !shown.empty();
  RectF xr{r.right() - 26, r.cy() - 10, 20, 20};
  float x1 = r.right() - 10 - (showClear ? 20 : 0);
  // Кнопка очистки получает нажатие раньше поля.
  bool cleared = false;
  Interaction xi{};
  if (showClear) {
    xi = interact(wid ^ 0xc1ea7ull, xr);
    cleared = xi.clicked;
  }
  Interaction it = interact(wid, r, disabled ? IfNone : (IfFocusable | IfTextInput));
  if (o.autofocus && !te.autoDone && !disabled) {
    te.autoDone = true;
    setFocus(wid, true);
    c.focusSeen = true;
  }
  bool focused = c.focus == wid && !disabled;
  bool changed = false;
  if (focused && !te.editing) editBegin(te, value, o.selectAllOnFocus || c.focusVisible);
  if (!focused && te.editing) {
    if (!o.live && te.buf != value) {
      value = te.buf;
      changed = true;
    }
    te.editing = false;
  }
  if (te.editing && o.live && te.buf != value && !it.pressed) {
    // Значение изменили снаружи (например, сброс фильтра) — принять.
    te.buf = value;
    te.caret = te.anchor = te.buf.size();
  }
  EditParams p;
  p.readOnly = o.readOnly;
  p.maxLength = o.maxLength;
  p.filter = o.filter;
  p.textRect = RectF{x0, r.y, std::max(1.f, x1 - x0), r.h};
  p.style = &st;
  if (cleared) {
    if (te.editing) {
      te.undo.push_back({te.buf, te.caret, te.anchor});
      te.buf.clear();
      te.caret = te.anchor = 0;
    }
    if (!value.empty()) {
      value.clear();
      changed = true;
    }
  }
  if (focused) {
    EditResult res = editProcess(te, p, it, wid);
    if (res.edited && o.live && te.buf != value) {
      value = te.buf;
      changed = true;
    }
    if (res.enter) {
      if (te.buf != value) {
        value = te.buf;
        changed = true;
      }
      if (!o.live) {
        te.editing = false;
        clearFocus();
        focused = false;
      }
    }
    if (res.escape) {
      if (o.live && value != te.original) {
        value = te.original;
        changed = true;
      }
      te.editing = false;
      clearFocus();
      focused = false;
    }
  }
  // Отрисовка
  drawFieldFrame(r, it, focused, disabled);
  if (o.icon) cmdIcon(o.icon, RectF{r.x + 10, r.cy() - 8, 16, 16}, focused ? t.accent : t.textMuted);
  if (te.editing && focused) {
    if (te.buf.empty() && !o.placeholder.empty()) textIn(o.placeholder, p.textRect, st, t.textMuted);
    editDraw(te, p, true, o.readOnly ? t.textDim : t.text);
  } else if (value.empty()) {
    if (!o.placeholder.empty()) textIn(o.placeholder, p.textRect, st, t.textMuted);
  } else {
    textIn(value, p.textRect, st, o.readOnly ? t.textDim : t.text);
  }
  if (showClear) {
    float xh = animate(wid ^ 0xc1ea8ull, xi.hovered ? 1.f : 0.f);
    if (xh > 0.01f) draw::circle(xr.cx(), xr.cy(), 10, t.hover.alpha(xh * 2));
    cmdIcon("close", xr.inset(4), mixc(t.textMuted, t.text, xh));
  }
  if (it.hovered && !xi.hovered && !disabled) c.cursor = platform::Cursor::IBeam;
  setLast(wid, r, it);
  c.last.changed = changed;
  c.last.active = focused;
  c.last.focused = focused;
  if (!o.tooltip.empty()) tooltip(o.tooltip);
  return changed;
}

bool textArea(std::string_view name, std::string& value, float height, const TextOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  DisableGuard dg(o.disabled);
  bool disabled = isDisabled();
  WidgetId wid = id(name);
  TextEdit& te = state<TextEdit>(wid);
  RectF r = place(0, std::max(height, t.controlH));
  const gfx::TextStyle& st = styleOf(Font::Body);
  Interaction it = interact(wid, r, disabled ? IfNone : (IfFocusable | IfTextInput));
  if (o.autofocus && !te.autoDone && !disabled) {
    te.autoDone = true;
    setFocus(wid, true);
    c.focusSeen = true;
  }
  bool focused = c.focus == wid && !disabled;
  bool changed = false;
  if (focused && !te.editing) {
    float keep = te.scrollY;
    editBegin(te, value, o.selectAllOnFocus);
    te.scrollY = keep;
  }
  if (!focused && te.editing) {
    if (!o.live && te.buf != value) {
      value = te.buf;
      changed = true;
    }
    te.editing = false;
  }
  if (!te.editing) te.buf = value;
  EditParams p;
  p.multiline = true;
  p.readOnly = o.readOnly;
  p.maxLength = o.maxLength;
  p.filter = o.filter;
  p.textRect = RectF{r.x + 10, r.y + 7, std::max(1.f, r.w - 24), std::max(1.f, r.h - 14)};
  p.style = &st;
  const gfx::TextLayout& L0 = cachedText(te.buf, st, p.textRect.w, 0, false, Align::Left, true)->layout;
  // Колесо прокручивает текст
  float maxS = std::max(0.f, L0.height - p.textRect.h);
  if (it.hovered && c.m.wheelY != 0 && maxS > 0) {
    float ns = clamp(te.scrollY - c.m.wheelY * 0.5f, 0.f, maxS);
    if (ns != te.scrollY) {
      te.scrollY = ns;
      c.m.wheelY = 0;
    }
  }
  if (focused) {
    EditResult res = editProcess(te, p, it, wid);
    if (res.edited && o.live && te.buf != value) {
      value = te.buf;
      changed = true;
    }
    if (res.enter) {
      if (te.buf != value) {
        value = te.buf;
        changed = true;
      }
      te.editing = false;
      clearFocus();
      focused = false;
    }
    if (res.escape) {
      if (o.live && value != te.original) {
        value = te.original;
        changed = true;
      }
      te.editing = false;
      clearFocus();
      focused = false;
    }
  }
  drawFieldFrame(r, it, focused, disabled);
  if (te.buf.empty() && !o.placeholder.empty()) textIn(o.placeholder, RectF{p.textRect.x, p.textRect.y, p.textRect.w, lineHeight(Font::Body)}, st, t.textMuted);
  editDraw(te, p, focused, o.readOnly ? t.textDim : t.text);
  const gfx::TextLayout& L = cachedText(te.buf, st, p.textRect.w, 0, false, Align::Left, true)->layout;
  if (L.height > p.textRect.h + 0.5f) {
    float th = std::max(20.f, p.textRect.h * p.textRect.h / L.height);
    float ty = p.textRect.y + (p.textRect.h - th) * (te.scrollY / std::max(1.f, L.height - p.textRect.h));
    cmdRect(RectF{r.right() - 7, ty, 3, th}, t.textMuted.alpha(0.5f), 1.5f);
  }
  if (it.hovered && !disabled) c.cursor = platform::Cursor::IBeam;
  setLast(wid, r, it);
  c.last.changed = changed;
  c.last.active = focused;
  c.last.focused = focused;
  if (!o.tooltip.empty()) tooltip(o.tooltip);
  return changed;
}

bool searchField(std::string_view name, std::string& query, std::string_view placeholder) {
  TextOpt o;
  o.placeholder = placeholder;
  o.icon = "search";
  o.clearButton = true;
  o.live = true;
  return textField(name, query, o);
}

// ================================================================ число
namespace {

struct NumState {
  TextEdit te;
  bool editing = false;
  bool scrubbing = false;
  double scrubStart = 0;
  double shakeAt = -10;
  double holdStart = -1, lastRepeat = 0;
  int holdDir = 0;
  std::string shown;   // текст, с которого начата правка: без изменений поле ничего не записывает
};

bool numChar(u32 cp) {
  return (cp >= '0' && cp <= '9') || cp == ',' || cp == '.' || cp == '-' || cp == '+' || cp == 0x2212 || cp == ' ' || cp == 0x202f || cp == 0xa0;
}

double roundTo(double v, int digits) {
  if (digits < 0) return v;
  double p = std::pow(10.0, std::min(digits, 12));
  double r = std::round(v * p) / p;
  return std::isfinite(r) ? r : v;
}

std::string editText(double v, int digits) {
  std::string s = strf("%.*f", std::max(0, std::min(digits, 12)), v);
  for (char& ch : s)
    if (ch == '.') ch = ',';
  if (s == "-0") s = "0";
  return s;
}

}  // namespace

bool numberField(std::string_view name, double& v, const NumberOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  DisableGuard dg(o.disabled);
  bool disabled = isDisabled();
  bool readOnly = o.readOnly || disabled;
  WidgetId wid = id(name);
  NumState& ns = state<NumState>(wid);
  float h = t.controlH;
  Frame& pf = frame();
  RectF r = place(pf.flow == Flow::HStack ? 120 : 0, h);
  if (r.h > h) r = RectF{r.x, r.y + std::round((r.h - h) * 0.5f), r.w, h};
  double mn = std::min(o.min, o.max), mx = std::max(o.min, o.max);
  double step = o.step > 0 ? o.step : 1;
  auto clampv = [&](double x) { return clamp(roundTo(x, o.digits), mn, mx); };
  bool changed = false;
  double before = v;
  // Кнопки − и + (раньше поля — им достаётся нажатие)
  RectF minusR{}, plusR{};
  int stepDir = 0;
  float mh = 0, ph = 0;   // наведение на − и +
  if (o.steppers && !readOnly) {
    plusR = RectF{r.right() - 26, r.cy() - 11, 22, 22};
    minusR = RectF{plusR.x - 24, plusR.y, 22, 22};
    Interaction mi = interact(wid ^ 0x5e1ull, minusR);
    Interaction pi = interact(wid ^ 0x5e2ull, plusR);
    mh = animate(wid ^ 0x5e3ull, mi.hovered ? 1.f : 0.f);
    ph = animate(wid ^ 0x5e4ull, pi.hovered ? 1.f : 0.f);
    auto held = [&](const Interaction& x, int dir) {
      if (x.pressed) {
        stepDir = dir;
        ns.holdStart = c.time;
        ns.lastRepeat = c.time;
        ns.holdDir = dir;
      } else if (x.held && ns.holdDir == dir && c.time - ns.holdStart > 0.4) {
        requestRedraw();
        if (c.time - ns.lastRepeat >= 0.06) {
          stepDir = dir;
          ns.lastRepeat = c.time;
        }
      }
      if (x.released && ns.holdDir == dir) ns.holdDir = 0;
    };
    held(mi, -1);
    held(pi, +1);
  }
  Interaction it = interact(wid, r, readOnly ? IfNone : (IfFocusable | IfTextInput));
  bool focused = c.focus == wid && !readOnly;
  // Перетаскивание (scrub)
  if (it.pressed && !ns.editing) {
    ns.scrubStart = v;
    ns.scrubbing = false;
  }
  if (c.active == wid && !ns.editing && !readOnly && c.activeDragging) {
    ns.scrubbing = true;
    double k = 1;
    if (c.m.mods & platform::ModShift) k = 0.1;
    if (c.m.mods & platform::ModCtrl) k = 10;
    double units = std::trunc(it.dx / 4.0);
    double nv = clampv(ns.scrubStart + units * step * k);
    if (nv != v) {
      v = nv;
      changed = true;
    }
  }
  // Начало правки: текст поля — значение, округлённое до digits; запоминаем его, чтобы уход из поля без ввода
  // (Tab, щелчок мимо, Enter) не записал округлённое значение и не добавил шаг отмены.
  auto beginEdit = [&]() {
    ns.shown = editText(v, o.digits);
    editBegin(ns.te, ns.shown, true);
  };
  if (it.released && !ns.scrubbing && !ns.editing && it.clicked && !readOnly) {
    ns.editing = true;
    beginEdit();
  }
  if (it.released) ns.scrubbing = false;
  if (focused && c.focusVisible && !ns.editing && c.active != wid) {
    ns.editing = true;
    beginEdit();
  }
  auto commit = [&]() {
    if (ns.te.buf == ns.shown) {   // текст не правили — значение остаётся точным
      ns.editing = false;
      return;
    }
    std::optional<double> x = parseNum(trim(ns.te.buf));
    if (!x || !std::isfinite(*x)) {
      ns.shakeAt = c.time;
    } else {
      double nv = clampv(*x);
      if (nv != v) {
        v = nv;
        changed = true;
      }
    }
    ns.editing = false;
  };
  if (!focused && ns.editing) commit();
  // Геометрия содержимого
  const gfx::TextStyle& st = styleOf(Font::Body);
  float lx = r.x + 10;
  float labelW = 0;
  if (o.icon) labelW = 22;
  else if (!o.label.empty()) labelW = textWidth(displayText(o.label), styleOf(Font::Small)) + 8;
  float vx = lx + labelW;
  float right = r.right() - 10 - (o.steppers && !readOnly ? 50 : 0);
  std::string unitS = unitFor(o.unit, v, o.digits);
  float unitW = unitS.empty() ? 0 : textWidth(unitS, st) + 3;
  EditParams p;
  p.filter = numChar;
  p.maxLength = 32;
  p.style = &st;
  p.textRect = RectF{vx, r.y, std::max(1.f, right - vx), r.h};
  if (ns.editing && focused) {
    EditResult res = editProcess(ns.te, p, it, wid);
    if (res.enter) {
      commit();
      clearFocus();
      focused = false;
    } else if (res.escape) {
      ns.editing = false;
      clearFocus();
      focused = false;
    }
  }
  // Стрелки и колесо
  if (focused) {
    int dir = 0;
    double mul = 1;
    if (takeKey(Key::Up)) dir = 1;
    else if (takeKey(Key::Down)) dir = -1;
    else if (takeKey(Key::Up, platform::ModShift)) { dir = 1; mul = 10; }
    else if (takeKey(Key::Down, platform::ModShift)) { dir = -1; mul = 10; }
    if (dir != 0) {
      double base = v;
      if (ns.editing) {
        if (auto x = parseNum(trim(ns.te.buf)); x && std::isfinite(*x)) base = *x;
      }
      double nv = clampv(base + dir * step * mul);
      if (nv != v) {
        v = nv;
        changed = true;
      }
      if (ns.editing) beginEdit();
    }
  }
  if (it.hovered && focused && c.m.wheelY != 0) {
    double nv = clampv(v + (c.m.wheelY > 0 ? step : -step));
    c.m.wheelY = 0;
    if (nv != v) {
      v = nv;
      changed = true;
      if (ns.editing) beginEdit();
    }
  }
  if (stepDir != 0) {
    double nv = clampv(v + stepDir * step);
    if (nv != v) {
      v = nv;
      changed = true;
    }
    if (ns.editing) beginEdit();
  }
  // Отрисовка
  float shakeT = float(c.time - ns.shakeAt);
  bool shaking = shakeT >= 0 && shakeT < 0.45f;
  if (shaking) c.animating = true;
  float sdx = shaking ? std::sin(shakeT * 46.f) * 4.f * (1 - shakeT / 0.45f) : 0.f;
  drawFieldFrame(RectF{r.x + sdx, r.y, r.w, r.h}, it, focused && ns.editing, disabled, shaking, shaking ? 1 - shakeT / 0.45f : 0.f);
  bool scrubHot = (it.hovered || ns.scrubbing) && !ns.editing && !readOnly;
  Color labelC = scrubHot ? t.textDim : t.textMuted;
  if (o.icon) cmdIcon(o.icon, RectF{lx + sdx, r.cy() - 8, 16, 16}, ns.scrubbing ? t.accent : labelC);
  else if (!o.label.empty()) textIn(displayText(o.label), RectF{lx + sdx, r.y, labelW, r.h}, styleOf(Font::Small), ns.scrubbing ? t.accent : labelC);
  if (ns.editing && focused) {
    EditParams pd = p;
    pd.textRect.x += sdx;
    editDraw(ns.te, pd, true, t.text);
  } else {
    std::string s = o.sign ? fmtSigned(v, o.digits) : fmtNum(v, o.digits);
    float sw = textWidth(s, st);
    RectF tr{vx + sdx, r.y, std::min(sw + 1, right - vx - unitW), r.h};
    textIn(s, tr, st, readOnly && !disabled ? t.textDim : t.text);
    unitS = unitFor(o.unit, v, o.digits);
    if (!unitS.empty()) textIn(unitS, RectF{tr.right() + 3, r.y, textWidth(unitS, st) + 1, r.h}, st, t.textMuted);
  }
  if (o.steppers && !readOnly) {
    if (mh > 0.01f) cmdRect(minusR, t.hover.alpha(mh * 2), 5);
    if (ph > 0.01f) cmdRect(plusR, t.hover.alpha(ph * 2), 5);
    cmdIcon("minus", minusR.inset(4), v <= mn ? t.textMuted.alpha(0.5f) : t.textDim);
    cmdIcon("plus", plusR.inset(4), v >= mx ? t.textMuted.alpha(0.5f) : t.textDim);
  }
  if (it.hovered && !readOnly) {
    bool overSteppers = o.steppers && (minusR.contains(c.m.x, c.m.y) || plusR.contains(c.m.x, c.m.y));
    if (!overSteppers) c.cursor = ns.editing ? platform::Cursor::IBeam : platform::Cursor::ResizeH;
  }
  if (ns.scrubbing) c.cursor = platform::Cursor::ResizeH;
  setLast(wid, r, it);
  c.last.changed = changed || v != before;
  c.last.active = ns.scrubbing || (ns.editing && focused) || ns.holdDir != 0;
  c.last.focused = focused;
  if (!o.tooltip.empty()) tooltip(o.tooltip);
  return c.last.changed;
}

}  // namespace rg::ui
