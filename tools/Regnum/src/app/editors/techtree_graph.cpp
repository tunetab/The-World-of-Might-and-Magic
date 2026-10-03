// Regnum — общие части схем деревьев: камера, связи, сетка, мини-карта, масштаб, карточка сведений, фишки с переносом
// (см. techtree.h).
#include "app/editors/techtree.h"

#include "app/widgets.h"
#include "gfx/icons.h"

namespace rg::app::tree {

namespace {

constexpr double kAnimSec = 0.28;
constexpr float kFitPad = 56;

double easeOut(double t) {
  t = clamp(t, 0.0, 1.0);
  double u = 1 - t;
  return 1 - u * u * u;
}

// Толщина линии на экране: растёт с масштабом умеренно (чётко и вблизи, и издали).
float screenWidth(float w, double zoom) { return w * float(clamp(zoom, 0.7, 1.35)); }

}  // namespace

// ================================================================ камера
Vec2 toWorld(const Camera& c, RectF canvas, float sx, float sy) {
  return {c.x + double(sx - canvas.x) / c.z, c.y + double(sy - canvas.y) / c.z};
}

gfx::Pt toScreen(const Camera& c, RectF canvas, Vec2 p) {
  return {float(double(canvas.x) + (p.x - c.x) * c.z), float(double(canvas.y) + (p.y - c.y) * c.z)};
}

RectF toScreen(const Camera& c, RectF canvas, double x, double y, double w, double h) {
  gfx::Pt a = toScreen(c, canvas, {x, y});
  return {a.x, a.y, float(w * c.z), float(h * c.z)};
}

void zoomAt(Camera& c, RectF canvas, float sx, float sy, double factor) {
  Vec2 wp = toWorld(c, canvas, sx, sy);
  double nz = clamp(c.z * factor, kMinZoom, kMaxZoom);
  c.z = nz;
  c.x = wp.x - double(sx - canvas.x) / nz;
  c.y = wp.y - double(sy - canvas.y) / nz;
  c.t0 = -1;
}

void zoomCenter(Camera& c, RectF canvas, double factor) { zoomAt(c, canvas, canvas.cx(), canvas.cy(), factor); }

// Экранная точка «центра» холста: середина части, не закрытой левой колонкой (insetLeft).
static double anchorX(const Camera& c, RectF canvas) { return double(c.insetLeft) + (double(canvas.w) - c.insetLeft) * 0.5; }

static void moveTo(Camera& c, RectF canvas, Vec2 center, double z, bool animate) {
  z = clamp(z, kMinZoom, kMaxZoom);
  if (!animate || !c.ready) {
    c.z = z;
    c.x = center.x - anchorX(c, canvas) / z;
    c.y = center.y - double(canvas.h) * 0.5 / z;
    c.t0 = -1;
    return;
  }
  c.fx = c.x + anchorX(c, canvas) / c.z;
  c.fy = c.y + double(canvas.h) * 0.5 / c.z;
  c.fz = c.z;
  c.tx = center.x;
  c.ty = center.y;
  c.tz = z;
  c.t0 = ui::time();
}

void fit(Camera& c, RectF canvas, const Box2& bounds, bool animate, double maxZoom) {
  if (canvas.empty()) return;
  if (bounds.empty()) {
    moveTo(c, canvas, {0, 0}, 1, animate);
    c.ready = true;
    return;
  }
  double bw = std::max(1.0, bounds.w()), bh = std::max(1.0, bounds.h());
  double z = std::min((double(canvas.w) - c.insetLeft - 2 * kFitPad) / bw, (double(canvas.h) - 2 * kFitPad) / bh);
  if (!(z > 0)) z = kMinZoom;
  moveTo(c, canvas, bounds.center(), std::min(z, maxZoom), animate);
  c.ready = true;
}

void reveal(Camera& c, RectF canvas, const Box2& box, bool animate) {
  if (canvas.empty() || box.empty()) return;
  RectF s = toScreen(c, canvas, box.x0, box.y0, box.w(), box.h());
  RectF safe = canvas.inset(40);
  safe.x += c.insetLeft;
  safe.w = std::max(0.f, safe.w - c.insetLeft);
  if (s.x >= safe.x && s.y >= safe.y && s.right() <= safe.right() && s.bottom() <= safe.bottom()) return;
  moveTo(c, canvas, box.center(), std::max(c.z, 0.6), animate);
}

// Шаг анимации с размером холста (центр и масштаб интерполируются, масштаб — в логарифме).
static void stepAnim(Camera& c, RectF canvas) {
  if (c.t0 < 0) return;
  double t = (ui::time() - c.t0) / kAnimSec;
  double e = easeOut(t);
  double z = std::exp(std::log(c.fz) + (std::log(c.tz) - std::log(c.fz)) * e);
  double cx = c.fx + (c.tx - c.fx) * e, cy = c.fy + (c.ty - c.fy) * e;
  c.z = z;
  c.x = cx - anchorX(c, canvas) / z;
  c.y = cy - double(canvas.h) * 0.5 / z;
  if (t >= 1) c.t0 = -1;
  else ui::requestRedraw();
}

void panZoom(Camera& cam, RectF canvas, const ui::Interaction& bg, Pan& pan, bool wheel) {
  stepAnim(cam, canvas);
  if (bg.pressed && (bg.button == 0 || bg.button == 2)) {
    pan.on = true;
    pan.x = cam.x;
    pan.y = cam.y;
  }
  if (pan.on && bg.held) {
    if (bg.dragging || bg.button == 2) {
      cam.x = pan.x - double(bg.dx) / cam.z;
      cam.y = pan.y - double(bg.dy) / cam.z;
      cam.t0 = -1;
      ui::setCursor(platform::Cursor::Grabbing);
    }
  }
  if (!bg.held) pan.on = false;
  if (wheel) {
    const ui::Mouse& m = ui::mouse();
    if (m.wheelY != 0 && canvas.contains(m.x, m.y)) {
      double notches = clamp(double(m.wheelY) / 64.0, -6.0, 6.0);
      zoomAt(cam, canvas, m.x, m.y, std::pow(1.18, notches));
    }
  }
}

// ================================================================ связи
Curve curve(Vec2 from, Vec2 to) {
  double dx = to.x - from.x;
  double k = std::max(48.0, std::fabs(dx) * 0.5);
  if (dx < 0) k = std::max(k, 90.0);   // связь назад — петля наружу
  return Curve{from, {from.x + k, from.y}, {to.x - k, to.y}, to};
}

Vec2 curveAt(const Curve& k, double t) {
  double u = 1 - t;
  double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
  return {k.p0.x * a + k.c0.x * b + k.c1.x * c + k.p1.x * d, k.p0.y * a + k.c0.y * b + k.c1.y * c + k.p1.y * d};
}

double curveDistance(const Curve& k, Vec2 p) {
  double best = kInf;
  Vec2 prev = k.p0;
  for (int i = 1; i <= 32; i++) {
    Vec2 q = curveAt(k, double(i) / 32);
    Vec2 d = q - prev;
    double l2 = d.len2();
    double t = l2 > 0 ? clamp((p - prev).dot(d) / l2, 0.0, 1.0) : 0;
    best = std::min(best, dist(p, prev + d * t));
    prev = q;
  }
  return best;
}

// ================================================================ отрисовка
Ink ink() {
  const ui::Theme& t = ui::theme();
  Ink k;
  k.dark = t.dark;
  k.bg = t.dark ? Color::mix(t.bg, t.surface1, 0.35f) : Color::mix(t.bg, t.surface3, 0.25f);
  k.grid = t.dark ? t.text.alpha(0.09f) : t.text.alpha(0.12f);
  k.surface = t.surface2;
  k.surfaceHi = t.surface3;
  k.border = t.border;
  k.borderStrong = t.borderStrong;
  k.text = t.text;
  k.textDim = t.textDim;
  k.textMuted = t.textMuted;
  k.accent = t.accent;
  k.success = t.success;
  k.warning = t.warning;
  k.danger = t.danger;
  k.info = t.info;
  k.onAccent = t.onAccent;
  k.shadow = t.shadow;
  return k;
}

void applyCamera(gfx::Canvas& c, RectF dev, float scale, const Camera& cam) {
  float s = float(cam.z) * scale;
  c.translate(dev.x - float(cam.x) * s, dev.y - float(cam.y) * s);
  c.scale(s, s);
}

void drawGrid(gfx::Canvas& c, RectF dev, float scale, const Camera& cam, const Ink& k) {
  c.fillRect(dev, k.bg);
  double step = 32;
  while (step * cam.z * scale < 22 * scale) step *= 2;
  double s = cam.z * scale;
  double wx0 = cam.x, wy0 = cam.y;
  double wx1 = cam.x + dev.w / s, wy1 = cam.y + dev.h / s;
  double gx0 = std::floor(wx0 / step) * step, gy0 = std::floor(wy0 / step) * step;
  float dot = std::max(1.f, std::round(1.2f * scale));
  Color minor = k.grid, major = k.grid.alpha(1.9f);
  for (double gy = gy0; gy <= wy1; gy += step) {
    float py = float(dev.y + (gy - wy0) * s);
    bool my = std::fmod(std::fabs(gy), step * 4) < 1e-6;
    for (double gx = gx0; gx <= wx1; gx += step) {
      float px = float(dev.x + (gx - wx0) * s);
      bool mj = my && std::fmod(std::fabs(gx), step * 4) < 1e-6;
      float d = mj ? dot + 1 : dot;
      c.fillRect(RectF{std::round(px - d * 0.5f), std::round(py - d * 0.5f), d, d}, mj ? major : minor);
    }
  }
}

void drawCurve(gfx::Canvas& c, const Curve& k, Color col, float width, double zoom, bool arrow, bool dashed) {
  float w = screenWidth(width, zoom) / float(zoom);
  Vec2 end = k.p1;
  float ah = screenWidth(9, zoom) / float(zoom);
  if (arrow) end = {k.p1.x - ah * 0.8, k.p1.y};
  gfx::Path p;
  p.moveTo(float(k.p0.x), float(k.p0.y));
  p.cubicTo(float(k.c0.x), float(k.c0.y), float(k.c1.x), float(k.c1.y), float(end.x), float(end.y));
  gfx::Stroke st;
  st.width = w;
  st.cap = gfx::Cap::Round;
  st.join = gfx::Join::Round;
  if (dashed) st.dash = {w * 3.f, w * 2.4f};
  c.strokePath(p, st, gfx::Paint(col));
  if (arrow) {
    gfx::Path t;
    float x = float(k.p1.x), y = float(k.p1.y);
    t.moveTo(x, y);
    t.lineTo(x - ah, y - ah * 0.55f);
    t.lineTo(x - ah * 0.78f, y);
    t.lineTo(x - ah, y + ah * 0.55f);
    t.close();
    c.fillPath(t, gfx::Paint(col));
  }
}

void drawPort(gfx::Canvas& c, Vec2 p, Color ring, Color fill, bool filled, bool hot, double zoom) {
  float r = (hot ? 7.5f : 4.6f) * float(clamp(zoom, 0.8, 1.3)) / float(zoom);
  float w = 1.5f / float(zoom);
  c.fillCircle(float(p.x), float(p.y), r + w, fill);
  if (filled || hot) c.fillCircle(float(p.x), float(p.y), r, ring);
  else c.strokeCircle(float(p.x), float(p.y), r - w * 0.5f, w, ring);
  if (hot) {
    float a = r * 0.55f;
    c.line(float(p.x) - a, float(p.y), float(p.x) + a, float(p.y), w * 1.1f, gfx::Paint(fill));
    c.line(float(p.x), float(p.y) - a, float(p.x), float(p.y) + a, w * 1.1f, gfx::Paint(fill));
  }
}

gfx::TextStyle textStyle(float size, gfx::FontWeight weight, gfx::FontFamily family) {
  gfx::TextStyle st = ui::textStyle(ui::Font::Body);
  st.size = size;
  st.weight = weight;
  st.family = family;
  return st;
}

void text(gfx::Canvas& c, std::string_view s, const gfx::TextStyle& st, RectF box, Color col, gfx::Align align, int maxLines, gfx::VAlign valign) {
  if (s.empty() || box.w <= 1) return;
  gfx::drawTextBox(c, s, st, box, gfx::Paint(col), align, valign, maxLines != 1, maxLines, true);
}

void icon(gfx::Canvas& c, std::string_view name, RectF box, Color col) { gfx::drawIcon(c, name, box, col); }

// ================================================================ поверх холста
void zoomBar(App& a, Camera& cam, RectF canvas, const Box2& bounds, std::string_view mark) {
  const ui::Theme& th = ui::theme();
  RectF r{canvas.x + cam.insetLeft + 12, canvas.bottom() - 12 - 38, 186, 38};
  ui::draw::shadow(r, 10, 18, th.shadow.alpha(0.6f), 4);
  ui::draw::rect(r, th.surface1, 10);
  ui::draw::rectStroke(r, th.border, 10, 1);
  ui::Area ar(r.inset(4), 0);
  ui::HStack hs(30, ui::Align::Left, 2);
  ui::IdScope s(mark);
  if (ui::iconButton("zoom-out", "Отдалить", {.shortcut = {platform::Key::Minus, 0}})) zoomCenter(cam, canvas, 1 / 1.25);
  std::string pct = fmtNum(cam.z * 100) + "\xC2\xA0%";
  RectF pr = ui::next(58, 30);
  ui::at(pr);
  if (ui::button(pct + "##zoom100", {.variant = ui::Variant::Ghost, .size = ui::Size::Small, .tooltip = "Масштаб 100 %"})) {
    double f = 1.0 / cam.z;
    zoomCenter(cam, canvas, f);
  }
  if (ui::iconButton("zoom-in", "Приблизить", {.shortcut = {platform::Key::Equal, 0}})) zoomCenter(cam, canvas, 1.25);
  ui::separatorV();
  if (ui::iconButton("zoom-fit", "Показать всё дерево", {.shortcut = {platform::Key::F, 0}})) fit(cam, canvas, bounds, true);
  a.markUi(std::string(mark) + ".fit");
}

void minimap(Camera& cam, RectF canvas, const Box2& bounds, const std::vector<MiniItem>& items, std::string_view mark) {
  if (bounds.empty() || items.empty() || canvas.w < 520 || canvas.h < 320) return;
  const ui::Theme& th = ui::theme();
  Box2 b = bounds.inflated(80);
  // Видимая часть тоже на мини-карте (рамка не выходит за край). Всё дерево на виду — мини-карта не нужна.
  Box2 vis{cam.x + cam.insetLeft / cam.z, cam.y, cam.x + canvas.w / cam.z, cam.y + canvas.h / cam.z};
  if (vis.contains({bounds.x0, bounds.y0}) && vis.contains({bounds.x1, bounds.y1})) return;
  Box2 all = b;
  all.add(vis);
  const float W = 196, H = 126;
  RectF r{canvas.right() - 12 - W, canvas.bottom() - 12 - H, W, H};
  RectF in = r.inset(8);
  double k = std::min(double(in.w) / all.w(), double(in.h) / all.h());
  float ox = in.x + float((double(in.w) - all.w() * k) * 0.5), oy = in.y + float((double(in.h) - all.h() * k) * 0.5);
  auto map = [&](Vec2 p) { return gfx::Pt{ox + float((p.x - all.x0) * k), oy + float((p.y - all.y0) * k)}; };
  ui::draw::shadow(r, 10, 18, th.shadow.alpha(0.6f), 4);
  ui::draw::rect(r, th.surface1.alpha(0.96f), 10);
  ui::draw::rectStroke(r, th.border, 10, 1);
  ui::draw::pushClip(in);
  for (const MiniItem& it : items) {
    gfx::Pt a = map({it.box.x0, it.box.y0}), z = map({it.box.x1, it.box.y1});
    RectF q{a.x, a.y, std::max(2.f, z.x - a.x), std::max(2.f, z.y - a.y)};
    ui::draw::rect(q, it.color, 1.5f);
    if (it.selected) ui::draw::rectStroke(q.expand(1.5f), th.accent, 2, 1.2f);
  }
  gfx::Pt va = map({vis.x0, vis.y0}), vz = map({vis.x1, vis.y1});
  RectF vr{va.x, va.y, vz.x - va.x, vz.y - va.y};
  ui::draw::rect(vr, th.accent.alpha(0.08f), 3);
  ui::draw::rectStroke(vr, th.accent.alpha(0.85f), 3, 1.2f);
  ui::draw::popClip();
  ui::WidgetId wid = ui::id(std::string(mark) + "##minimap");
  ui::Interaction it = ui::interact(wid, r);
  if (it.hovered || it.held) ui::setCursor(platform::Cursor::Hand);
  if (it.held || it.clicked) {
    Vec2 w{all.x0 + double(it.mx - ox) / k, all.y0 + double(it.my - oy) / k};
    moveTo(cam, canvas, w, cam.z, !it.dragging);
  }
  if (it.hovered) ui::tooltip("Мини-схема: щелчок — перейти");
}

bool hoverDelay(u64 key) {
  struct S {
    u64 key = 0;
    double since = 0;
  };
  S& s = ui::state<S>(ui::id("##tree-tip-delay"));
  const ui::Mouse& m = ui::mouse();
  double now = ui::time();
  if (m.down[0] || m.down[1] || m.down[2]) {
    s.key = key;
    s.since = now + 1e9;
    return false;
  }
  if (s.key != key) {
    s.key = key;
    s.since = now;
  }
  if (key == 0) return false;
  if (now - s.since >= 0.45) return true;
  ui::requestRedraw();
  return false;
}

void tipCard(const Tip& t, RectF anchor, RectF area) {
  const ui::Theme& th = ui::theme();
  const float W = 300, pad = 14, iw = 18;
  float inner = W - 2 * pad;
  gfx::TextStyle small = ui::textStyle(ui::Font::Small);
  gfx::TextStyle body = ui::textStyle(ui::Font::Body);
  float lhS = ui::lineHeight(ui::Font::Small), lhB = ui::lineHeight(ui::Font::Body);
  struct Row {
    const TipLine* l;
    std::vector<std::string> parts;
    float h;
  };
  std::vector<Row> rows;
  float h = pad + ui::lineHeight(ui::Font::Subtitle) + (t.subtitle.empty() ? 0 : lhS + 2) + 8;
  for (const TipLine& l : t.lines) {
    Row r{&l, {}, 0};
    float tw = inner - (l.icon.empty() ? 0 : iw + 6);
    const gfx::TextStyle& st = l.strong ? body : small;
    float lh = l.strong ? lhB : lhS;
    if (l.wrap) {
      gfx::TextLayout L = gfx::layoutText(l.text, st, tw, 8, true);
      for (auto& ln : L.lines) r.parts.push_back(l.text.substr(ln.begin, ln.end - ln.begin));
      if (r.parts.empty()) r.parts.push_back({});
    } else {
      r.parts.push_back(l.text);
    }
    r.h = float(r.parts.size()) * lh + 3;
    h += r.h;
    rows.push_back(std::move(r));
  }
  h += pad - 3;
  // Справа от узла, иначе слева, иначе под ним.
  RectF r{anchor.right() + 12, anchor.y, W, h};
  if (r.right() > area.right() - 8) {
    r.x = anchor.x - 12 - W;
    if (r.x < area.x + 8) {
      r.x = clamp(anchor.cx() - W * 0.5f, area.x + 8, std::max(area.x + 8, area.right() - 8 - W));
      r.y = anchor.bottom() + 10;
      if (r.bottom() > area.bottom() - 8) r.y = anchor.y - 10 - h;
    }
  }
  r.y = clamp(r.y, area.y + 8, std::max(area.y + 8, area.bottom() - 8 - h));
  r.x = std::round(r.x);
  r.y = std::round(r.y);
  ui::draw::shadow(r, 12, 26, th.shadow, 8);
  ui::draw::rect(r, th.surface1, 12);
  ui::draw::rectStroke(r, th.borderStrong, 12, 1);
  ui::draw::rect(RectF{r.x, r.y + 12, 3, 22}, t.accent, 1.5f);
  float y = r.y + pad;
  float tx = r.x + pad;
  if (!t.icon.empty()) {
    ui::draw::icon(t.icon, RectF{tx, y + 1, 18, 18}, t.accent);
    tx += 24;
  }
  float lhT = ui::lineHeight(ui::Font::Subtitle);
  ui::draw::text(t.title, RectF{tx, y, r.right() - pad - tx, lhT}, ui::Font::Subtitle, th.text);
  y += lhT;
  if (!t.subtitle.empty()) {
    y += 2;
    ui::draw::text(t.subtitle, RectF{r.x + pad, y, inner, lhS}, ui::Font::Small, th.textMuted);
    y += lhS;
  }
  y += 8;
  ui::draw::line(r.x + pad, y - 4, r.right() - pad, y - 4, th.border, 1);
  for (const Row& row : rows) {
    const TipLine& l = *row.l;
    float x = r.x + pad;
    float lh = l.strong ? lhB : lhS;
    if (!l.icon.empty()) {
      ui::draw::icon(l.icon, RectF{x, y + (lh - 15) * 0.5f, 15, 15}, l.color.a ? l.color : th.textDim);
      x += iw + 6;
    }
    float yy = y;
    for (const std::string& p : row.parts) {
      ui::draw::text(p, RectF{x, yy, r.right() - pad - x, lh}, l.strong ? ui::Font::Strong : ui::Font::Small,
                     l.icon.empty() && l.color.a ? l.color : (l.strong ? th.text : th.textDim));
      yy += lh;
    }
    y += row.h;
  }
}

int canvasEmpty(RectF canvas, const char* icon, std::string_view text, std::string_view action, const char* actionIcon, bool disabled,
                std::string_view secondary, const char* secondaryIcon, std::string_view mark) {
  float w = std::min(360.f, canvas.w - 40);
  RectF r{std::round(canvas.cx() - w * 0.5f), std::round(canvas.cy() - 90), w, 220};
  ui::Area ar(r, 0);
  ui::Disabled d(disabled);
  int res = ui::emptyState(icon, text, action, actionIcon) ? 1 : 0;
  if (!mark.empty() && !action.empty()) app().markUi(std::string(mark) + ".add", emptyActionRect(action, actionIcon));
  if (!secondary.empty()) {
    ui::HStack hs(30, ui::Align::Center, 0);
    if (ui::button(secondary, {.variant = ui::Variant::Ghost, .icon = secondaryIcon, .size = ui::Size::Small})) res = 2;
    if (!mark.empty()) app().markUi(std::string(mark) + ".alt");
  }
  return res;
}

// ================================================================ фишки с переносом
namespace {
struct FlowState {
  RectF area;
  float x = 0, y = 0;
  bool any = false;
};
std::vector<FlowState>& flows() {
  static std::vector<FlowState> f;
  return f;
}
constexpr float kChipH = 26, kChipGap = 6;
}  // namespace

ChipFlow::ChipFlow() {
  FlowState f;
  f.area = ui::avail();
  f.x = f.area.x;
  f.y = f.area.y;
  flows().push_back(f);
}

ChipFlow::~ChipFlow() {
  if (flows().empty()) return;
  FlowState f = flows().back();
  flows().pop_back();
  if (f.any) ui::next(0, f.y + kChipH - f.area.y);
}

ui::ChipAction chip(std::string_view label, const ui::ChipOpt& o) {
  if (flows().empty()) return ui::chip(label, o);
  FlowState& f = flows().back();
  // Ширина — как у ui::chip: текст Small + поля, значок или точка, крестик.
  bool lead = o.icon || o.color.a > 0;
  float w = std::ceil(ui::measure(ui::displayText(label), ui::Font::Small)) + 20 + (lead ? 16 : 0) + (o.removable ? 18 : 0);
  w = std::min(w, std::max(40.f, f.area.w));
  if (f.any && f.x + w > f.area.right() + 0.5f) {
    f.x = f.area.x;
    f.y += kChipH + kChipGap;
  }
  ui::at(RectF{f.x, f.y, w, kChipH});
  f.x += w + kChipGap;
  f.any = true;
  return ui::chip(label, o);
}

bool effectChips(const World& w, const std::vector<Id>& modifiers) {
  bool any = false;
  for (Id mid : modifiers)
    if (const Modifier* m = w.modifier(mid))
      for (int f = 0; f < kFxCount; f++) any = any || (m->has(Fx(f)) && m->fx[size_t(f)] != 0);
  if (!any) return false;
  ChipFlow flow;
  for (Id mid : modifiers) {
    const Modifier* m = w.modifier(mid);
    if (!m) continue;
    for (int f = 0; f < kFxCount; f++) {
      if (!m->has(Fx(f)) || m->fx[size_t(f)] == 0) continue;
      ui::IdScope s(i64(mid) * 64 + f);
      double v = m->fx[size_t(f)];
      tree::chip(w::effectText(Fx(f), v), {.icon = schema::effect(Fx(f)).icon, .tone = w::effectGood(Fx(f), v) ? ui::Tone::Success : ui::Tone::Danger,
                                     .tooltip = m->name});
    }
  }
  return true;
}

bool modifierList(std::string_view id, std::vector<Id>& ids, bool disabled) {
  App& a = app();
  const World& w = a.world();
  ui::IdScope scope(id);
  disabled = disabled || a.readOnly();
  bool changed = false;
  bool any = false;
  for (Id mid : ids) any = any || w.modifier(mid) != nullptr;
  if (any) {
    ChipFlow flow;
    for (size_t i = 0; i < ids.size(); i++) {
      const Modifier* m = w.modifier(ids[i]);
      if (!m) continue;
      ui::IdScope s2{i64(ids[i])};
      std::string tip = m->desc;
      for (int f = 0; f < kFxCount; f++) {
        if (!m->has(Fx(f))) continue;
        if (!tip.empty()) tip += "\n";
        tip += w::effectText(Fx(f), m->fx[size_t(f)]);
      }
      ui::ChipOpt co;
      co.icon = m->icon.empty() || !gfx::hasIcon(m->icon) ? "sparkles" : m->icon.c_str();
      co.color = m->color;
      co.removable = !disabled;
      co.clickable = true;
      co.tooltip = tip;
      ui::ChipAction act = tree::chip(m->name.empty() ? std::string_view("Модификатор") : std::string_view(m->name), co);
      if (act == ui::ChipAction::Remove) {
        ids.erase(ids.begin() + long(i));
        changed = true;
        break;
      }
      if (act == ui::ChipAction::Click) a.openEditor("modifiers", m->id);
    }
  }
  if (disabled) return changed;
  std::vector<const Modifier*> cand;
  w.modifiers.each([&](const Modifier& m) {
    if (std::find(ids.begin(), ids.end(), m.id) == ids.end()) cand.push_back(&m);
  });
  if (cand.empty()) {
    if (w.modifiers.empty() && ui::link("Создать модификатор", "sparkles")) a.openEditor("modifiers", 0);
    return changed;
  }
  std::sort(cand.begin(), cand.end(), [](const Modifier* x, const Modifier* y) { return compareRu(x->name, y->name) < 0; });
  std::vector<ui::Option> opts;
  opts.reserve(cand.size());
  for (const Modifier* m : cand)
    opts.push_back(ui::Option{m->name.empty() ? std::string_view("Модификатор") : std::string_view(m->name),
                              m->icon.empty() || !gfx::hasIcon(m->icon) ? "sparkles" : m->icon.c_str(), m->color, {}, false});
  int idx = -1;
  if (ui::combo("add", idx, std::span<const ui::Option>(opts), {.placeholder = "Добавить модификатор", .icon = "plus"}) && idx >= 0 &&
      idx < int(cand.size())) {
    ids.push_back(cand[size_t(idx)]->id);
    changed = true;
  }
  return changed;
}

bool factionSwitch(std::string_view id, Id& value, bool states, std::string_view noneLabel) {
  const World& w = app().world();
  std::vector<const Faction*> list;
  w.factions.each([&](const Faction& f) {
    if (!states || f.isState()) list.push_back(&f);
  });
  std::sort(list.begin(), list.end(), [](const Faction* a, const Faction* b) {
    if (a->kind != b->kind) return a->kind < b->kind;
    return compareRu(a->name, b->name) < 0;
  });
  std::vector<std::string> names;
  names.reserve(list.size());
  for (const Faction* f : list) names.push_back(f->name.empty() ? std::string("Без названия") : f->name);
  std::vector<ui::Option> opts;
  int idx = -1;
  for (size_t i = 0; i < list.size(); i++) {
    opts.push_back(ui::Option{names[i], nullptr, list[i]->color, list[i]->isGuild() ? std::string_view("гильдия") : std::string_view(), false});
    if (list[i]->id == value) idx = int(i);
  }
  ui::ComboOpt co;
  co.noneLabel = noneLabel;
  co.placeholder = noneLabel.empty() ? std::string_view("—") : noneLabel;
  co.icon = states ? "crown" : "tech-tree";
  co.tooltip = "Другое дерево";
  if (!ui::combo(id, idx, std::span<const ui::Option>(opts), co)) return false;
  Id nv = idx >= 0 && idx < int(list.size()) ? list[size_t(idx)]->id : 0;
  if (nv == value) return false;
  value = nv;
  return true;
}

RectF emptyActionRect(std::string_view action, const char* actionIcon) {
  // Раскладка ui::emptyState: кнопка высотой 30 по центру, 16 точек от низа блока.
  RectF r = ui::lastItem().rect;
  float bw = ui::measure(ui::displayText(action), ui::Font::Strong) + 28 + (actionIcon ? 23 : 0);
  return RectF{r.cx() - bw * 0.5f, r.bottom() - 16 - 30, bw, 30};
}

std::string roman(int n) {
  if (n <= 0 || n >= 4000) return std::to_string(n);
  static const std::pair<int, const char*> tab[] = {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"},
                                                    {50, "L"},   {40, "XL"},  {10, "X"},  {9, "IX"},   {5, "V"},   {4, "IV"}, {1, "I"}};
  std::string s;
  for (auto& [v, r] : tab)
    while (n >= v) {
      s += r;
      n -= v;
    }
  return s;
}

}  // namespace rg::app::tree
