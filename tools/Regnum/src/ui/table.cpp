// Regnum — таблица (ширины столбцов, сортировка по шапке, закреплённая шапка, чередование строк, выделение,
// ячейки с любыми виджетами, итоговая строка, пустое состояние, виртуализация) и дерево.
#include <numeric>

#include "ui/ui_internal.h"

namespace rg::ui {

using namespace in;

struct Table::Impl {
  WidgetId id = 0;
  std::vector<Column> cols;
  int rows = 0;
  TableOpt o;
  std::vector<float> colX, colW;
  RectF slot;
  bool slotOneShot = false;
  float headerH = 0, rowH = 34, footerH = 36;
  float bodyTop = 0;
  float headerY = 0;
  bool stuck = false;
  bool fixed = false;
  std::unique_ptr<Scroll> scroll;
  bool pushedFrame = false;
  int k0 = 0, k1 = 0;
  std::vector<int> order;
  int curK = -1, curRow = -1, col = -1;
  bool rowOpen = false, footerOpen = false;
  RectF rowR;
  int clicked = -1, dbl = -1, right = -1, hovered = -1;
  int selLocal = -1;
  int sortCol = -1;
  bool desc = false;
  bool hadFooter = false;
  float footerY = 0;
  size_t headerMark = size_t(-1);
};

namespace {

struct TableState {
  int sel = -1;
  bool hadFooter = false;
};

}  // namespace

static void tableBegin(Table::Impl& d, std::string_view name, std::span<const Column> cols, int rows, const TableOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  d.id = id(name);
  registerId(d.id);
  d.cols.assign(cols.begin(), cols.end());
  d.rows = std::max(0, rows);
  d.o = o;
  d.rowH = o.compact ? 28 : std::max(20.f, o.rowHeight);
  d.headerH = o.header ? (o.compact ? 28 : 32) : 0;
  d.footerH = o.compact ? 30 : 36;
  TableState& ts = state<TableState>(d.id);
  d.hadFooter = ts.hadFooter;
  d.selLocal = ts.sel;
  auto sk = c.sticky.find(d.id ^ 0x5047ull);
  if (sk != c.sticky.end()) {
    int v = int(sk->second);
    d.sortCol = std::abs(v) - 1;
    d.desc = v < 0;
  }
  d.fixed = o.height > 0;
  float natH = d.headerH + (d.rows > 0 ? d.rowH * float(d.rows) : 120.f) + (d.hadFooter ? d.footerH : 0);
  RectF r0 = slotBegin(0, d.fixed ? o.height : natH, &d.slotOneShot);
  r0.h = d.fixed ? o.height : natH;
  d.slot = r0;
  // Столбцы
  float fixedW = 0, frs = 0;
  for (const Column& col : d.cols) {
    fixedW += col.width.px;
    frs += col.width.fr;
  }
  float avail = r0.w - (d.fixed ? 10 : 0);
  float rest = std::max(0.f, avail - fixedW);
  float x = r0.x;
  for (const Column& col : d.cols) {
    float w = std::max(col.width.px + (frs > 0 ? rest * col.width.fr / frs : 0), col.width.min);
    d.colX.push_back(std::round(x));
    d.colW.push_back(std::round(x + w) - std::round(x));
    x += w;
  }
  // Шапка: закрепляется у верхнего края видимой области родителя.
  RectF clip = currentClip();
  d.headerY = r0.y;
  if (!d.fixed && d.headerH > 0) {
    float maxY = r0.y + natH - d.headerH - (d.hadFooter ? d.footerH : 0) - d.rowH;
    if (clip.y > r0.y && maxY > r0.y) {
      d.headerY = std::min(clip.y, maxY);
      d.stuck = true;
    }
  }
  // Сортировка по щелчку на шапке
  if (d.headerH > 0) {
    for (size_t i = 0; i < d.cols.size(); i++) {
      const Column& col = d.cols[i];
      RectF hr{d.colX[i], d.headerY, d.colW[i], d.headerH};
      WidgetId hid = hashMix(d.id, 0x4ead00ull + i);
      Interaction hi = interact(hid, hr);
      if (col.sortable && hi.clicked) {
        if (d.sortCol != int(i)) {
          d.sortCol = int(i);
          d.desc = false;
        } else if (!d.desc) {
          d.desc = true;
        } else {
          d.sortCol = -1;
          d.desc = false;
        }
        c.sticky[d.id ^ 0x5047ull] = d.sortCol < 0 ? 0.f : float(d.desc ? -(d.sortCol + 1) : d.sortCol + 1);
      }
      if (col.sortable && hi.hovered) c.cursor = platform::Cursor::Hand;
      if (hi.hovered && !col.tooltip.empty()) {
        setLast(hid, hr, hi);
        tooltip(col.tooltip);
      }
      animate(hid ^ 1, hi.hovered ? 1.f : 0.f);
    }
  }
  // Тело
  float bodyH = r0.h - d.headerH - (d.hadFooter ? d.footerH : 0);
  if (d.fixed) {
    at(RectF{r0.x, r0.y + d.headerH, r0.w, std::max(20.f, bodyH)});
    d.scroll = std::make_unique<Scroll>("##tbody", std::max(20.f, bodyH));
    Frame& f = frame();
    d.bodyTop = f.y;
    RectF vc = f.clip;
    d.k0 = clamp(int(std::floor((vc.y - d.bodyTop) / d.rowH)), 0, d.rows);
    d.k1 = clamp(int(std::ceil((vc.bottom() - d.bodyTop) / d.rowH)), d.k0, d.rows);
    // строки — по всей ширине области прокрутки
    float sx = f.box.x;
    for (float& cx : d.colX) cx = cx - r0.x + sx;
  } else {
    d.bodyTop = r0.y + d.headerH;
    Frame f;
    f.win = c.win;
    f.box = RectF{r0.x, d.bodyTop, r0.w, 1e7f};
    f.x = r0.x;
    f.y = d.bodyTop;
    f.bottom = d.bodyTop;
    f.gap = t.gap;
    f.clip = clip;
    pushFrame(f);
    d.pushedFrame = true;
    float top = d.stuck ? d.headerY + d.headerH : clip.y;
    if (d.stuck) pushClipFrame(RectF{clip.x, top, clip.w, std::max(0.f, clip.bottom() - top)});
    d.k0 = clamp(int(std::floor((top - d.bodyTop) / d.rowH)), 0, d.rows);
    d.k1 = clamp(int(std::ceil((clip.bottom() - d.bodyTop) / d.rowH)), d.k0, d.rows);
  }
  // Клавиатура: фокус таблицы, стрелки меняют выделенную строку.
  if (o.selectable && d.rows > 0) registerFocusable(d.id);
}

Table::Table(std::string_view name, std::span<const Column> cols, int rows, const TableOpt& o) : d_(std::make_unique<Impl>()) {
  tableBegin(*d_, name, cols, rows, o);
}

Table::Table(std::string_view name, std::initializer_list<Column> cols, int rows, const TableOpt& o) : d_(std::make_unique<Impl>()) {
  tableBegin(*d_, name, std::span<const Column>(cols.begin(), cols.size()), rows, o);
}

int Table::sortColumn() const { return d_->sortCol; }
bool Table::sortDescending() const { return d_->desc; }

void Table::sort(const std::function<int(int, int, int)>& cmp) {
  Impl& d = *d_;
  d.order.resize(size_t(d.rows));
  std::iota(d.order.begin(), d.order.end(), 0);
  if (d.sortCol < 0 || !cmp) return;
  int col = d.sortCol;
  bool desc = d.desc;
  std::stable_sort(d.order.begin(), d.order.end(), [&](int a, int b) { return desc ? cmp(b, a, col) < 0 : cmp(a, b, col) < 0; });
}

static int selectedOf(const Table::Impl& d) { return d.o.selected ? *d.o.selected : d.selLocal; }
static void setSelected(Table::Impl& d, int row) {
  if (d.o.selected) *d.o.selected = row;
  d.selLocal = row;
}

static void closeRow(Table::Impl& d) {
  if (d.rowOpen) {
    popId();
    d.rowOpen = false;
    C().oneShot.reset();
  }
}

static void enterRow(Table::Impl& d, int k) {
  Ctx& c = C();
  const Theme& t = c.th;
  d.curK = k;
  d.curRow = d.order.empty() ? k : d.order[size_t(k)];
  float x0 = d.colX.empty() ? d.slot.x : d.colX.front();
  float x1 = d.colX.empty() ? d.slot.right() : d.colX.back() + d.colW.back();
  d.rowR = RectF{x0, d.bodyTop + d.rowH * float(k), x1 - x0, d.rowH};
  d.col = -1;
  C().idStack.push_back(hashMix(d.id, u64(i64(d.curRow)) * 0x9e3779b97f4a7c15ull + 1));   // область строки — внутри таблицы
  d.rowOpen = true;
  WidgetId rid = id("##row");
  Interaction ri = interact(rid, d.rowR, IfAllowOverlap | IfRightButton);
  bool sel = selectedOf(d) == d.curRow;
  if (ri.hovered) d.hovered = d.curRow;
  if (ri.clicked) {
    d.clicked = d.curRow;
    if (d.o.selectable) {
      setSelected(d, d.curRow);
      setFocus(d.id, false);
    }
  }
  if (ri.pressed && ri.button == 0 && d.o.selectable) setFocus(d.id, false);
  if (ri.doubleClicked) d.dbl = d.curRow;
  if (ri.rightClicked) {
    d.right = d.curRow;
    if (d.o.selectable) setSelected(d, d.curRow);
  }
  float hv = animate(rid ^ 0x40c1ull, ri.hovered ? 1.f : 0.f, 0.1f);
  if (d.o.striped && (k & 1)) cmdRect(d.rowR, t.stripe, 0);
  if (sel) {
    cmdRect(d.rowR, t.accent.alpha(t.dark ? 0.13f : 0.11f), 6);
    cmdRect4(RectF{d.rowR.x, d.rowR.y + 6, 3, d.rowR.h - 12}, t.accent, 0, 2, 2, 0);
  } else if (hv > 0.01f) {
    cmdRect(d.rowR, t.hover.alpha(hv * 1.4f), 6);
  }
  if (!d.o.striped && k > 0) draw::line(d.rowR.x + 4, d.rowR.y, d.rowR.right() - 4, d.rowR.y, t.border.alpha(0.6f), 1);
  setLast(rid, d.rowR, ri);
}

int Table::It::operator*() const { return t->d_->curRow; }

Table::It& Table::It::operator++() {
  Impl& d = *t->d_;
  closeRow(d);
  k++;
  if (k < d.k1) enterRow(d, k);
  return *this;
}

Table::It Table::begin() {
  Impl& d = *d_;

  if (d.k0 < d.k1) enterRow(d, d.k0);
  return It{this, d.k0};
}

Table::It Table::end() { return It{this, d_->k1}; }

RectF Table::cell() {
  Ctx& c = C();
  Impl& d = *d_;
  if (!d.rowOpen && !d.footerOpen) return {};
  int n = int(d.cols.size());
  if (n == 0) return {};
  d.col = std::min(d.col + 1, n - 1);
  float rh = d.footerOpen ? d.footerH : d.rowH;
  float y = d.footerOpen ? d.footerY : d.rowR.y;
  RectF cr{d.colX[size_t(d.col)] + 10, y + 2, std::max(0.f, d.colW[size_t(d.col)] - 20), rh - 4};
  c.oneShot = cr;
  c.oneShotAlign = d.cols[size_t(d.col)].align;
  return cr;
}

void Table::text(std::string_view s, Ink ink, Font f) {
  Ctx& c = C();
  RectF cr = cell();
  c.oneShot.reset();
  if (cr.empty()) return;
  Impl& d = *d_;
  Font ff = d.footerOpen && f == Font::Body ? Font::Strong : f;
  textIn(displayText(s), cr, styleOf(ff), inkColor(ink), d.cols[size_t(d.col)].align);
}

bool Table::footer() {
  Ctx& c = C();
  const Theme& t = c.th;
  Impl& d = *d_;
  closeRow(d);
  if (d.footerOpen) return true;
  d.footerOpen = true;
  d.col = -1;
  if (d.fixed) {
    d.footerY = d.slot.bottom() - d.footerH;
    // Итоговая строка вне прокрутки
    if (d.scroll) {
      Frame& f = frame();
      f.bottom = std::max(f.bottom, d.bodyTop + d.rowH * float(d.rows));
      f.placed = true;
      d.scroll.reset();
    }
  } else {
    d.footerY = d.bodyTop + d.rowH * float(d.rows);
  }
  float x0 = d.slot.x, x1 = d.slot.right();
  RectF fr{x0, d.footerY, x1 - x0, d.footerH};
  cmdRect(fr, t.dark ? Color(255, 255, 255, 6) : t.surface3.alpha(0.6f), 0);
  draw::line(x0, d.footerY, x1, d.footerY, t.borderStrong, 1);
  C().idStack.push_back(hashMix(d.id, 0xf007e7ull));
  return true;
}

RectF Table::rowRect() const { return d_->footerOpen ? RectF{d_->slot.x, d_->footerY, d_->slot.w, d_->footerH} : d_->rowR; }
int Table::selected() const { return selectedOf(*d_); }
void Table::select(int row) { setSelected(*d_, row); }
int Table::clicked() const { return d_->clicked; }
int Table::doubleClicked() const { return d_->dbl; }
int Table::rightClicked() const { return d_->right; }
int Table::hovered() const { return d_->hovered; }

Table::~Table() {
  Ctx& c = C();
  const Theme& t = c.th;
  Impl& d = *d_;
  closeRow(d);
  bool hadFooter = d.footerOpen;
  if (d.footerOpen) {
    popId();
    c.oneShot.reset();
  }
  // Клавиатура
  if (c.focus == d.id && d.rows > 0 && d.o.selectable) {
    int dir = takeKey(Key::Down) ? 1 : takeKey(Key::Up) ? -1 : 0;
    if (dir != 0) {
      int sel = selectedOf(d);
      int k = -1;
      for (int i = 0; i < d.rows; i++)
        if ((d.order.empty() ? i : d.order[size_t(i)]) == sel) k = i;
      k = k < 0 ? (dir > 0 ? 0 : d.rows - 1) : clamp(k + dir, 0, d.rows - 1);
      setSelected(d, d.order.empty() ? k : d.order[size_t(k)]);
      Item saved = c.last;
      c.last.rect = RectF{d.slot.x, d.bodyTop + d.rowH * float(k), d.slot.w, d.rowH};
      scrollToItem();
      c.last = saved;
    }
  }
  // Пустое состояние
  if (d.rows == 0) {
    float h = 120;
    at(RectF{d.slot.x, d.bodyTop, d.slot.w, h});
    emptyState(d.o.emptyIcon, d.o.emptyText);
  }
  if (d.fixed && d.scroll) {
    Frame& f = frame();
    f.bottom = std::max(f.bottom, d.bodyTop + d.rowH * float(d.rows));
    f.placed = true;
    d.scroll.reset();
  }
  if (d.pushedFrame) popFrame();
  // Шапка рисуется последней — поверх прокрученных строк.
  if (d.headerH > 0) {
    RectF hr{d.slot.x, d.headerY, d.slot.w, d.headerH};
    if (d.stuck) {
      cmdShadow(hr, 0, 8, Color(0, 0, 0, t.dark ? 60 : 18), 3);
      cmdRect(hr, t.surface2, 0);
    }
    draw::line(hr.x, hr.bottom() - 0.5f, hr.right(), hr.bottom() - 0.5f, t.border, 1);
    const auto hst = styleWith(Font::Small, gfx::FontWeight::Semibold);
    for (size_t i = 0; i < d.cols.size(); i++) {
      const Column& col = d.cols[i];
      float x0 = (d.fixed ? d.slot.x + (d.colX[i] - d.colX[0]) : d.colX[i]) + 10;
      RectF cr{x0, hr.y, d.colW[i] - 20, hr.h};
      bool active = d.sortCol == int(i);
      WidgetId hid = hashMix(d.id, 0x4ead00ull + i);
      float hv = animate(hid ^ 1, (c.hot == hid) ? 1.f : 0.f);
      std::string_view title = displayText(col.title);
      float tw = title.empty() ? 0 : textWidth(title, hst);
      float iw = col.icon ? 16 + (title.empty() ? 0 : 5) : 0;
      float aw = col.sortable ? 14 : 0;
      float total = std::min(cr.w, tw + iw + aw);
      float x = cr.x;
      if (col.align == Align::Right) x = cr.right() - total;
      else if (col.align == Align::Center) x = cr.x + (cr.w - total) * 0.5f;
      Color fg = active ? t.text : mixc(t.textMuted, t.textDim, hv);
      if (col.icon) {
        cmdIcon(col.icon, RectF{x, hr.cy() - 8, 16, 16}, active ? t.accent : fg);
        x += iw;
      }
      if (!title.empty()) {
        float avail = std::max(0.f, cr.right() - x - aw);
        textIn(title, RectF{x, hr.y, std::min(tw + 1, avail), hr.h}, hst, fg);
        x += std::min(tw, avail) + 2;
      }
      if (col.sortable && (active || hv > 0.01f)) {
        Color ac = active ? t.accent : t.textMuted.alpha(hv);
        drawChevron(RectF{x, hr.cy() - 6, 12, 12}, ac, active && !d.desc ? 180.f : 0.f);
      }
    }
  }
  TableState& ts = state<TableState>(d.id);
  ts.hadFooter = hadFooter;
  ts.sel = d.selLocal;
  float h = d.fixed ? d.o.height : d.headerH + (d.rows > 0 ? d.rowH * float(d.rows) : 120.f) + (hadFooter ? d.footerH : 0);
  RectF r{d.slot.x, d.slot.y, d.slot.w, h};
  if (hadFooter != d.hadFooter) requestRedraw();
  if (!d.slotOneShot) slotEnd(r);
  c.last = Item{};
  c.last.id = d.id;
  c.last.rect = r;
  c.last.focused = c.focus == d.id;
}

// ================================================================ дерево
TreeNode::TreeNode(std::string_view lbl, const TreeOpt& o) {
  Ctx& c = C();
  const Theme& t = c.th;
  WidgetId tid = id(lbl);
  auto st = c.sticky.find(tid);
  bool open = !o.leaf && (st == c.sticky.end() ? o.defaultOpen : st->second > 0.5f);
  RectF r = place(0, 28);
  Interaction it = interact(tid, r, IfFocusable);
  RectF chev{r.x + 2, r.cy() - 7, 14, 14};
  bool onChevron = !o.leaf && c.m.x < chev.right() + 4;
  if (it.clicked && !it.keyActivated) {
    if (onChevron) open = !open;
    else clicked_ = true;
  }
  if (it.keyActivated) clicked_ = true;
  if (it.doubleClicked && !o.leaf) {
    open = !open;
    dbl_ = true;
  }
  if (it.focused && !o.leaf) {
    if (takeKey(Key::Right)) open = true;
    if (takeKey(Key::Left)) open = false;
  }
  if (!o.leaf) c.sticky[tid] = open ? 1.f : 0.f;
  float hv = animate(tid ^ 0x7ee1ull, it.hovered ? 1.f : 0.f);
  if (o.selected) cmdRect(r, t.accent.alpha(t.dark ? 0.14f : 0.12f), 6);
  else if (hv > 0.01f) cmdRect(r, t.hover.alpha(hv * 1.4f), 6);
  float rot = animate(tid ^ 0x7ee2ull, open ? 0.f : -90.f, 0.14f);
  if (!o.leaf) drawChevron(chev, t.textMuted, rot);
  float x = r.x + 22;
  if (o.dot.a > 0) {
    draw::circle(x + 5, r.cy(), 4.5f, o.dot);
    x += 16;
  } else if (o.icon) {
    cmdIcon(o.icon, RectF{x, r.cy() - 8, 16, 16}, o.selected ? t.accent : t.textDim);
    x += 24;
  }
  float right = r.right() - 8;
  if (!o.badge.empty()) {
    const auto bs = styleWith(Font::Caption, gfx::FontWeight::Semibold);
    float bw = textWidth(o.badge, bs) + 12;
    RectF br{right - bw, r.cy() - 9, bw, 18};
    cmdRect(br, t.surface3, 9);
    textIn(o.badge, br, bs, t.textDim, Align::Center);
    right = br.x - 6;
  }
  textIn(displayText(lbl), RectF{x, r.y, right - x, r.h}, o.selected ? styleWith(Font::Body, gfx::FontWeight::Semibold) : styleOf(Font::Body), o.selected ? t.text : t.text);
  setLast(tid, r, it);
  focusRing(r, 6);
  open_ = open;
  gx_ = chev.cx();
  gy_ = r.bottom();
  if (open_) frame().indent += 16;
}

TreeNode::~TreeNode() {
  if (!open_) return;
  Frame& f = frame();
  f.indent = std::max(0.f, f.indent - 16);
  float y1 = f.y - f.gap;
  if (y1 > gy_ + 2) draw::line(gx_, gy_ + 2, gx_, y1 - 2, C().th.border, 1);
}

}  // namespace rg::ui
