// Regnum — выбор: выпадающий список с поиском и клавиатурой (виртуализация длинных списков),
// множественный выбор фишками, выбор цвета (геральдическая палитра, HSV, оттенок, hex).
#include "ui/ui_internal.h"

namespace rg::ui {

using namespace in;

namespace {

struct ComboState {
  std::string query, lastQuery;
  std::vector<int> filtered;
  int lastCount = -1;
  u64 lastSig = 0;
  int highlight = -1;
  bool justOpened = false;
  bool scrollToHighlight = false;
};

constexpr float kRowH = 30;

void drawOptionRow(RectF row, const Option& op, bool highlighted, bool selected, bool none) {
  const Theme& t = C().th;
  if (highlighted) cmdRect(row, t.dark ? t.surface3.lighten(0.04f) : t.surface3, 6);
  float x = row.x + 10;
  Color fg = op.disabled ? t.textMuted : (none ? t.textDim : t.text);
  if (op.color.a > 0) {
    draw::circle(x + 5, row.cy(), 5, op.color);
    draw::ring(x + 5, row.cy(), 5, 1, Color(0, 0, 0, t.dark ? 80 : 35));
    x += 20;
  } else if (op.icon) {
    cmdIcon(op.icon, RectF{x, row.cy() - 8, 16, 16}, selected ? t.accent : t.textDim);
    x += 24;
  }
  float right = row.right() - 10 - (selected ? 22 : 0);
  if (!op.hint.empty()) {
    const auto& hs = styleOf(Font::Small);
    float hw = std::min(textWidth(op.hint, hs), (right - x) * 0.45f);
    textIn(op.hint, RectF{right - hw, row.y, hw, row.h}, hs, t.textMuted, Align::Right);
    right -= hw + 10;
  }
  textIn(displayText(op.label), RectF{x, row.y, right - x, row.h}, selected ? styleWith(Font::Body, gfx::FontWeight::Semibold) : styleOf(Font::Body), fg);
  if (selected) cmdIcon("check", RectF{row.right() - 28, row.cy() - 8, 16, 16}, t.accent);
}

}  // namespace

static bool comboImpl(std::string_view name, int& index, int count, const std::function<Option(int)>& get, const ComboOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  DisableGuard dg(o.disabled);
  bool disabled = isDisabled();
  count = std::max(0, count);
  WidgetId wid = id(name);
  WidgetId pid = hashMix(wid, 0xc0b0ull);
  float h = t.controlH;
  RectF r = place(frame().flow == Flow::HStack ? 200 : 0, h);
  if (r.h > h) r = RectF{r.x, r.y + std::round((r.h - h) * 0.5f), r.w, h};
  Interaction it = interact(wid, r, disabled ? IfNone : IfFocusable);
  bool open = popupOpen(pid);
  ComboState& cs = state<ComboState>(pid);
  bool hasNone = !o.noneLabel.empty();
  bool searchable = o.search == 1 || (o.search < 0 && count > 8);
  auto openNow = [&]() {
    openPopupAt(pid, r, wid, false);
    open = true;
    cs.justOpened = true;
    cs.highlight = -1;
    if (!searchable) cs.query.clear();
  };
  if (!disabled) {
    if (it.pressed && it.button == 0) {
      if (open) {
        closePopupsFrom(popupIndex(pid));
        open = false;
      } else if (c.closedOpener != wid) {
        cs.query.clear();
        openNow();
      }
    } else if (it.keyActivated && !open) {
      cs.query.clear();
      openNow();
    } else if (it.focused && !open) {
      if (takeKey(Key::Down)) {
        cs.query.clear();
        openNow();
      } else if (searchable) {
        for (KeyEv& k : c.kbd) {
          if (k.consumed || k.e.type != platform::EventType::Text) continue;
          if (!open) {
            cs.query.clear();
            openNow();
          }
          cs.query += k.e.text;
          k.consumed = true;
        }
      }
    }
  }
  // Поле
  int cur = index < 0 ? -1 : (index < count ? index : -1);
  Option sel = cur >= 0 ? get(cur) : Option{};
  drawFieldFrame(r, it, open, disabled);
  float x = r.x + 10;
  if (cur >= 0 && sel.color.a > 0) {
    draw::circle(x + 5, r.cy(), 5, sel.color);
    draw::ring(x + 5, r.cy(), 5, 1, Color(0, 0, 0, t.dark ? 80 : 35));
    x += 20;
  } else if ((cur >= 0 && sel.icon) || o.icon) {
    cmdIcon(cur >= 0 && sel.icon ? sel.icon : o.icon, RectF{x, r.cy() - 8, 16, 16}, t.textDim);
    x += 24;
  }
  float chevX = r.right() - 26;
  std::string_view shown = cur >= 0 ? displayText(sel.label) : (index < 0 && hasNone ? o.noneLabel : o.placeholder);
  textIn(shown, RectF{x, r.y, chevX - x - 4, r.h}, styleOf(Font::Body), cur >= 0 ? t.text : t.textMuted);
  float rot = animate(wid ^ 0xc0b1ull, open ? 180.f : 0.f, 0.16f);
  drawChevron(RectF{chevX + 2, r.cy() - 7, 14, 14}, open ? t.accent : t.textMuted, rot);
  setLast(wid, r, it);
  focusRing(r, t.radiusField);
  if (!o.tooltip.empty() && !open) tooltip(o.tooltip);
  Item fieldItem = c.last;
  bool changed = false;
  if (!open) return false;

  // Фильтр (пересчёт только при смене запроса или числа пунктов)
  std::string key = utf8::searchKey(cs.query);
  bool seekCurrent = cs.justOpened;
  u64 sig = hashMix(hash64(key), u64(count));
  if (sig != cs.lastSig || cs.lastCount != count) {
    cs.filtered.clear();
    for (int i = 0; i < count; i++) {
      if (key.empty()) cs.filtered.push_back(i);
      else {
        Option op = get(i);
        if (utf8::matches(op.label, cs.query) || (!op.hint.empty() && utf8::matches(op.hint, cs.query))) cs.filtered.push_back(i);
      }
    }
    cs.lastSig = sig;
    cs.lastCount = count;
    if (key.empty()) seekCurrent = true;
    else cs.highlight = cs.filtered.empty() ? -1 : 0;
    cs.scrollToHighlight = true;
  }
  bool noneRow = hasNone && key.empty();
  int rows = int(cs.filtered.size()) + (noneRow ? 1 : 0);
  auto rowIndex = [&](int k) { return noneRow ? (k == 0 ? -1 : cs.filtered[size_t(k - 1)]) : cs.filtered[size_t(k)]; };
  if (seekCurrent) {
    cs.highlight = -1;
    for (int k = 0; k < rows; k++)
      if (rowIndex(k) == index) cs.highlight = k;
    if (cs.highlight < 0 && rows > 0) cs.highlight = 0;
    cs.scrollToHighlight = true;
  }
  if (cs.highlight >= rows) cs.highlight = rows - 1;
  float width = std::max(r.w, o.popupWidth);
  int visible = std::max(1, std::min(rows, std::max(3, o.maxVisible)));
  PopupOpt po;
  po.side = Side::Below;
  po.width = width;
  po.pad = 5;
  if (!beginPopupId(pid, po, false)) return false;
  IdScope scope{i64(pid)};
  bool topmost = popupIndex(pid) == int(c.popups.size()) - 1;
  int pick = -2;
  if (topmost && rows > 0) {
    int hl = cs.highlight;
    if (takeKey(Key::Down)) hl = std::min(rows - 1, hl + 1);
    if (takeKey(Key::Up)) hl = std::max(0, hl - 1);
    if (takeKey(Key::PageDown)) hl = std::min(rows - 1, hl + visible);
    if (takeKey(Key::PageUp)) hl = std::max(0, hl - visible);
    if (hl != cs.highlight) {
      cs.highlight = hl;
      cs.scrollToHighlight = true;
    }
    if (cs.highlight >= 0 && cs.highlight < rows && takeKey(Key::Enter)) pick = rowIndex(cs.highlight);
  }
  if (topmost && takeKey(Key::Escape)) {
    closePopupsFrom(popupIndex(pid));
    setFocus(wid, c.focusVisible);
  }
  if (searchable) {
    WidgetId sfid = id("##search");
    if (cs.justOpened) setFocus(sfid, true);
    TextOpt to;
    to.placeholder = "Поиск";
    to.icon = "search";
    to.live = true;
    to.clearButton = true;
    textField("##search", cs.query, to);
    spacer(4);
  }
  cs.justOpened = false;
  if (rows == 0) {
    RectF er = place(0, 44);
    textIn("Ничего не найдено", er, styleOf(Font::Body), t.textMuted, Align::Center);
  } else {
    VirtualList vl("##list", rows, kRowH, kRowH * float(visible));
    for (int k : vl) {
      RectF row = next(kRowH);
      int idx = rowIndex(k);
      Option op = idx >= 0 ? get(idx) : Option{o.noneLabel, nullptr, {}, {}, false};
      Interaction ri = interact(id(i64(k)), row);
      if (ri.hovered && c.mouseMoved) {
        cs.highlight = k;
      }
      if (ri.clicked && !op.disabled) pick = idx;
      drawOptionRow(row, op, k == cs.highlight, idx == index, idx < 0);
    }
    if (cs.scrollToHighlight && cs.highlight >= 0) {
      vl.scrollToRow(cs.highlight);
      cs.scrollToHighlight = false;
    }
  }
  if (pick != -2) {
    bool ok = pick < 0 || !get(pick).disabled;
    if (ok) {
      if (pick != index) {
        index = pick;
        changed = true;
      }
      closePopupsFrom(popupIndex(pid));
      setFocus(wid, c.focusVisible);
    }
  }
  endPopup();
  c.last = fieldItem;
  c.last.changed = changed;
  return changed;
}

bool combo(std::string_view name, int& index, std::span<const Option> options, const ComboOpt& o) {
  return comboImpl(name, index, int(options.size()), [&](int i) { return options[size_t(i)]; }, o);
}

bool combo(std::string_view name, int& index, std::initializer_list<Option> options, const ComboOpt& o) {
  std::span<const Option> s(options.begin(), options.size());
  return comboImpl(name, index, int(s.size()), [&](int i) { return s[size_t(i)]; }, o);
}

bool combo(std::string_view name, int& index, int count, const std::function<Option(int)>& get, const ComboOpt& o) {
  return comboImpl(name, index, count, get, o);
}

// ================================================================ множественный выбор
namespace {

float chipWidth(const Option& op) {
  bool lead = op.icon || op.color.a > 0;
  return std::ceil(textWidth(displayText(op.label), styleOf(Font::Small)) + 20 + (lead ? 16 : 0) + 18);
}

}  // namespace

bool multiSelect(std::string_view name, std::vector<int>& selected, std::span<const Option> options, const MultiOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  DisableGuard dg(o.disabled);
  bool disabled = isDisabled();
  WidgetId wid = id(name);
  WidgetId pid = hashMix(wid, 0x3a17ull);
  int n = int(options.size());
  selected.erase(std::remove_if(selected.begin(), selected.end(), [&](int i) { return i < 0 || i >= n; }), selected.end());
  RectF a = c.oneShot ? *c.oneShot : avail();
  float W = a.w;
  // Раскладка фишек с переносом
  const float pad = 4, gapX = 6, gapY = 6, chipH = 26, addW = 26;
  std::vector<RectF> rects;
  float x = pad, y = pad;
  for (int i : selected) {
    float w = std::min(chipWidth(options[size_t(i)]), W - 2 * pad);
    if (x > pad && x + w > W - pad) {
      x = pad;
      y += chipH + gapY;
    }
    rects.push_back({x, y, w, chipH});
    x += w + gapX;
  }
  if (x > pad && x + addW > W - pad) {
    x = pad;
    y += chipH + gapY;
  }
  RectF addR{x, y, addW, chipH};
  float H = std::max(t.controlH, y + chipH + pad);
  RectF r = place(0, H);
  bool open = popupOpen(pid);
  Interaction it = interact(wid, r, IfAllowOverlap);   // фон: фишки и «+» поверх
  drawFieldFrame(r, it, open, disabled);
  bool changed = false;
  int removeAt = -1;
  for (size_t k = 0; k < selected.size(); k++) {
    const Option& op = options[size_t(selected[k])];
    IdScope sc{i64(selected[k])};
    at(RectF{r.x + rects[k].x, r.y + rects[k].y, rects[k].w, chipH});
    ChipOpt co;
    co.icon = op.icon;
    co.color = op.color;
    co.removable = !disabled;
    if (chip(op.label, co) == ChipAction::Remove) removeAt = int(k);
  }
  if (selected.empty()) textIn(o.placeholder, RectF{r.x + 10, r.y, r.w - 50, t.controlH}, styleOf(Font::Body), t.textMuted);
  at(RectF{r.x + addR.x, r.y + addR.y, addR.w, addR.h});
  IconButtonOpt ao;
  ao.size = Size::Small;
  ao.toggled = open;
  if (iconButton("plus", o.addTooltip, ao) && !disabled) {
    if (open) closePopupsFrom(popupIndex(pid));
    else if (c.closedOpener != c.last.id) openPopupAt(pid, r, c.last.id, false);
  }
  if (removeAt >= 0) {
    selected.erase(selected.begin() + removeAt);
    changed = true;
  }
  Item fieldItem;
  fieldItem.id = wid;
  fieldItem.rect = r;
  fieldItem.hovered = it.hovered;
  if (popupOpen(pid)) {
    ComboState& cs = state<ComboState>(pid);
    PopupOpt po;
    po.width = std::max(240.f, std::min(r.w, 360.f));
    po.pad = 5;
    if (beginPopupId(pid, po, false)) {
      IdScope scope{i64(pid)};
      if (n > 8) {
        TextOpt to;
        to.placeholder = "Поиск";
        to.icon = "search";
        to.live = true;
        to.clearButton = true;
        to.autofocus = true;
        textField("##search", cs.query, to);
        spacer(4);
      }
      std::vector<int> rows;
      for (int i = 0; i < n; i++)
        if (cs.query.empty() || utf8::matches(options[size_t(i)].label, cs.query)) rows.push_back(i);
      if (rows.empty()) {
        RectF er = place(0, 44);
        textIn("Ничего не найдено", er, styleOf(Font::Body), t.textMuted, Align::Center);
      } else {
        VirtualList vl("##list", int(rows.size()), kRowH, kRowH * float(std::min<size_t>(rows.size(), 9)));
        for (int k : vl) {
          int i = rows[size_t(k)];
          const Option& op = options[size_t(i)];
          RectF row = next(kRowH);
          Interaction ri = interact(id(i64(i)), row);
          bool on = std::find(selected.begin(), selected.end(), i) != selected.end();
          if (ri.hovered) cmdRect(row, t.dark ? t.surface3.lighten(0.04f) : t.surface3, 6);
          RectF box{row.x + 10, row.cy() - 8, 16, 16};
          cmdRect(box, on ? t.accent : (t.dark ? t.surface3 : t.surface2), 4);
          if (!on) cmdStroke(box, t.borderStrong, 4, 1.25f);
          else drawCheckGlyph(RectF{box.x - 1, box.y - 1, 18, 18}, t.onAccent, 1);
          float tx = box.right() + 10;
          if (op.color.a > 0) {
            draw::circle(tx + 5, row.cy(), 5, op.color);
            tx += 18;
          } else if (op.icon) {
            cmdIcon(op.icon, RectF{tx, row.cy() - 8, 16, 16}, t.textDim);
            tx += 24;
          }
          textIn(displayText(op.label), RectF{tx, row.y, row.right() - tx - 8, row.h}, styleOf(Font::Body), op.disabled ? t.textMuted : t.text);
          if (ri.clicked && !op.disabled) {
            if (on) selected.erase(std::find(selected.begin(), selected.end(), i));
            else selected.push_back(i);
            changed = true;
          }
        }
      }
      endPopup();
    }
  } else {
    state<ComboState>(pid).query.clear();
  }
  c.last = fieldItem;
  c.last.changed = changed;
  return changed;
}

// ================================================================ цвет
namespace {

struct Hsv {
  float h = 0, s = 0, v = 0;
};

Hsv toHsv(Color c) {
  float r = c.r / 255.f, g = c.g / 255.f, b = c.b / 255.f;
  float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
  Hsv o;
  o.v = mx;
  o.s = mx > 0 ? d / mx : 0;
  if (d > 1e-6f) {
    if (mx == r) o.h = 60 * std::fmod((g - b) / d, 6.f);
    else if (mx == g) o.h = 60 * ((b - r) / d + 2);
    else o.h = 60 * ((r - g) / d + 4);
    if (o.h < 0) o.h += 360;
  }
  return o;
}

Color fromHsv(Hsv x, u8 a) {
  float h = std::fmod(std::max(0.f, x.h), 360.f) / 60.f;
  float cc = x.v * x.s, xx = cc * (1 - std::fabs(std::fmod(h, 2.f) - 1)), m = x.v - cc;
  float r = 0, g = 0, b = 0;
  int i = int(h);
  switch (i) {
    case 0: r = cc; g = xx; break;
    case 1: r = xx; g = cc; break;
    case 2: g = cc; b = xx; break;
    case 3: g = xx; b = cc; break;
    case 4: r = xx; b = cc; break;
    default: r = cc; b = xx; break;
  }
  auto u = [](float f) { return u8(clamp(std::lround(f * 255.f), 0L, 255L)); };
  return Color(u(r + m), u(g + m), u(b + m), a);
}

struct ColorState {
  Hsv hsv;
  Color last{0, 0, 0, 0};
  bool init = false;
  std::string hex;
};

std::string hexOf(Color c, bool alpha) {
  std::string s = c.toHex();
  if (alpha && c.a != 255) s += strf("%02x", c.a);
  return s;
}

void checker(RectF r, float rad) {
  const Theme& t = C().th;
  cmdRect(r, t.dark ? Color::hex(0x9aa0a8) : Color::hex(0xd8d2c6), rad);
  float s = 5;
  draw::pushClip(r);
  for (float y = r.y; y < r.bottom(); y += s)
    for (float x = r.x + (int((y - r.y) / s) % 2) * s; x < r.right(); x += 2 * s) cmdRect(RectF{x, y, std::min(s, r.right() - x), std::min(s, r.bottom() - y)}, t.dark ? Color::hex(0x6f7886) : Color::hex(0xf4f1ea));
  draw::popClip();
}

}  // namespace

bool colorPicker(std::string_view name, Color& col, const ColorOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  WidgetId wid = id(name);
  IdScope scope{i64(wid)};
  ColorState& cs = state<ColorState>(wid);
  if (!cs.init || !(cs.last == col)) {
    Hsv h = toHsv(col);
    if (cs.init && (h.s < 1e-4f || h.v < 1e-4f)) h.h = cs.hsv.h;   // оттенок серого не теряется
    if (cs.init && h.v < 1e-4f) h.s = cs.hsv.s;
    cs.hsv = h;
    cs.last = col;
    cs.init = true;
    cs.hex = hexOf(col, o.alpha);
  }
  bool changed = false;
  auto setColor = [&](Color nc) {
    if (!o.alpha) nc.a = 255;
    if (!(nc == col)) {
      col = nc;
      changed = true;
    }
    cs.last = col;
    cs.hex = hexOf(col, o.alpha);
  };
  RectF a = c.oneShot ? *c.oneShot : avail();
  // Палитра
  std::span<const Color> pal = o.palette.empty() ? heraldicPalette() : o.palette;
  const float sw = 22, gp = 6;
  int cols = std::max(1, int((a.w + gp) / (sw + gp)));
  int prow = int((pal.size() + size_t(cols) - 1) / size_t(cols));
  RectF pr = place(0, prow * (sw + gp) - gp);
  float gx = std::floor((pr.w - (cols * (sw + gp) - gp)) * 0.5f);
  for (size_t i = 0; i < pal.size(); i++) {
    int rr = int(i) / cols, cc = int(i) % cols;
    RectF s{pr.x + gx + cc * (sw + gp), pr.y + rr * (sw + gp), sw, sw};
    Interaction si = interact(id(i64(i) + 1000), s);
    bool same = pal[i].r == col.r && pal[i].g == col.g && pal[i].b == col.b;
    float hv = animate(id(i64(i) + 2000), si.hovered ? 1.f : 0.f);
    RectF d = s.inset(-1.5f * hv);
    cmdRect(d, pal[i], 6);
    cmdStroke(d, Color(0, 0, 0, t.dark ? 70 : 40), 6, 1);
    if (same) cmdStroke(s.expand(3), t.accent, 8, 2);
    if (si.clicked) {
      Color nc = pal[i];
      nc.a = col.a;
      setColor(nc);
      cs.hsv = toHsv(col);
    }
    if (si.hovered) {
      setLast(id(i64(i) + 1000), s, si);
      tooltip(pal[i].toHex());
    }
  }
  spacer(4);
  // Насыщенность/яркость
  RectF sv = place(0, 136);
  Color hueC = fromHsv({cs.hsv.h, 1, 1}, 255);
  cmdRect(sv, hueC, 8);
  draw::gradient(sv, Color(255, 255, 255, 255), Color(255, 255, 255, 0), 8, true);
  draw::gradient(sv, Color(0, 0, 0, 0), Color(0, 0, 0, 255), 8, false);
  cmdStroke(sv, Color(0, 0, 0, 40), 8, 1);
  WidgetId svId = id("##sv");
  Interaction svi = interact(svId, sv);
  if (c.active == svId && (svi.held || svi.pressed)) {
    cs.hsv.s = clamp((c.m.x - sv.x) / std::max(1.f, sv.w), 0.f, 1.f);
    cs.hsv.v = 1 - clamp((c.m.y - sv.y) / std::max(1.f, sv.h), 0.f, 1.f);
    setColor(fromHsv(cs.hsv, col.a));
  }
  if (svi.hovered || c.active == svId) c.cursor = platform::Cursor::Crosshair;
  {
    float mx = sv.x + cs.hsv.s * sv.w, my = sv.y + (1 - cs.hsv.v) * sv.h;
    draw::circle(mx, my, 7.5f, Color(0, 0, 0, 60));
    draw::ring(mx, my, 6, 2.5f, Color(255, 255, 255));
    draw::circle(mx, my, 4.5f, fromHsv(cs.hsv, 255));
  }
  // Оттенок
  RectF hb = place(0, 12);
  static const std::pair<float, Color> hueStops[] = {{0.f, Color::hex(0xff0000)},   {1.f / 6, Color::hex(0xffff00)}, {2.f / 6, Color::hex(0x00ff00)},
                                                     {3.f / 6, Color::hex(0x00ffff)}, {4.f / 6, Color::hex(0x0000ff)}, {5.f / 6, Color::hex(0xff00ff)},
                                                     {1.f, Color::hex(0xff0000)}};
  cmdGradient(hb, 6, true, hueStops);
  WidgetId hId = id("##hue");
  Interaction hi = interact(hId, hb.expand(4));
  if (c.active == hId && (hi.held || hi.pressed)) {
    cs.hsv.h = clamp((c.m.x - hb.x) / std::max(1.f, hb.w), 0.f, 0.9999f) * 360.f;
    setColor(fromHsv(cs.hsv, col.a));
  }
  {
    float kx = hb.x + cs.hsv.h / 360.f * hb.w;
    draw::circle(kx, hb.cy(), 9, Color(0, 0, 0, 60));
    draw::circle(kx, hb.cy(), 8, Color(255, 255, 255));
    draw::circle(kx, hb.cy(), 5.5f, hueC);
  }
  if (o.alpha) {
    RectF ab = place(0, 12);
    checker(ab, 6);
    Color c0 = col, c1 = col;
    c0.a = 0;
    c1.a = 255;
    draw::gradient(ab, c0, c1, 6, true);
    WidgetId aId = id("##alpha");
    Interaction ai = interact(aId, ab.expand(4));
    if (c.active == aId && (ai.held || ai.pressed)) {
      Color nc = col;
      nc.a = u8(std::lround(clamp((c.m.x - ab.x) / std::max(1.f, ab.w), 0.f, 1.f) * 255));
      setColor(nc);
    }
    float kx = ab.x + col.a / 255.f * ab.w;
    draw::circle(kx, ab.cy(), 9, Color(0, 0, 0, 60));
    draw::circle(kx, ab.cy(), 8, Color(255, 255, 255));
  }
  spacer(2);
  // Образец и hex
  {
    Row row({px(30), fr(1)}, 30, 8);
    RectF prev = next(30);
    if (col.a < 255) checker(prev, 7);
    cmdRect(prev, col, 7);
    cmdStroke(prev, Color(0, 0, 0, t.dark ? 70 : 40), 7, 1);
    TextOpt to;
    to.maxLength = o.alpha ? 9 : 7;
    to.filter = [](u32 cp) { return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'f') || (cp >= 'A' && cp <= 'F') || cp == '#'; };
    std::string hex = cs.hex;
    if (textField("##hex", hex, to)) {
      std::string s = trim(hex);
      if (!s.empty() && s[0] != '#') s = "#" + s;
      if (auto pc = Color::parse(s)) {
        Color nc = *pc;
        if (!o.alpha) nc.a = 255;
        setColor(nc);
        cs.hsv = toHsv(col);
      } else {
        cs.hex = hexOf(col, o.alpha);
      }
    }
  }
  c.last = Item{};
  c.last.id = wid;
  c.last.rect = RectF{a.x, a.y, a.w, frame().y - a.y};
  c.last.changed = changed;
  c.last.active = c.active == svId || c.active == hId;
  return changed;
}

bool colorButton(std::string_view name, Color& col, const ColorOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  WidgetId wid = id(name);
  WidgetId pid = hashMix(wid, 0xc0101ull);
  float h = o.size == Size::Small ? t.controlHSmall : t.controlH;
  std::string hex = hexOf(col, o.alpha);
  const auto& ms = styleOf(Font::Mono);
  float w = o.hex ? 12 + 20 + 8 + textWidth(hex, ms) + 26 : h;
  RectF r = place(std::ceil(w), h);
  if (r.h > h) r = RectF{r.x, r.y + std::round((r.h - h) * 0.5f), r.w, h};
  Interaction it = interact(wid, r, IfFocusable);
  bool open = popupOpen(pid);
  if ((it.pressed && it.button == 0) || it.keyActivated) {
    if (open) {
      closePopupsFrom(popupIndex(pid));
      open = false;
    } else if (c.closedOpener != wid) {
      openPopupAt(pid, r, wid, false);
      open = true;
    }
  }
  RectF sw;
  if (o.hex) {
    drawFieldFrame(r, it, open, isDisabled());
    sw = RectF{r.x + 8, r.cy() - 10, 20, 20};
    textIn(hex, RectF{sw.right() + 8, r.y, r.right() - sw.right() - 30, r.h}, ms, t.textDim);
    drawChevron(RectF{r.right() - 22, r.cy() - 7, 14, 14}, t.textMuted, open ? 180.f : 0.f);
  } else {
    sw = r.inset(3);
    float hv = animate(wid ^ 1, it.hovered ? 1.f : 0.f);
    cmdRect(r, t.hover.alpha(hv), t.radiusField);
  }
  if (col.a < 255) checker(sw, 5);
  cmdRect(sw, col, 5);
  cmdStroke(sw, Color(0, 0, 0, t.dark ? 90 : 45), 5, 1);
  setLast(wid, r, it);
  focusRing(r, t.radiusField);
  if (!o.tooltip.empty() && !open) tooltip(o.tooltip);
  Item fieldItem = c.last;
  bool changed = false;
  bool active = false;
  if (open) {
    PopupOpt po;
    po.width = 252;
    po.pad = 12;
    if (beginPopupId(pid, po, false)) {
      changed = colorPicker("##picker", col, o);
      active = c.last.active;
      endPopup();
    }
  }
  c.last = fieldItem;
  c.last.changed = changed;
  c.last.active = active;
  return changed;
}

}  // namespace rg::ui
