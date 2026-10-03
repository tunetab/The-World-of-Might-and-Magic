// Regnum — общие помощники инструментов правки карты: наложения, прилипание, ввод контура, панель параметров.
#include "app/tools_edit.h"

#include <set>

#include "gfx/icons.h"

namespace rg::app::tools {

// ================================================================ выделение и названия
Id selectedProvince(const App& a) { return a.ui.sel.type == SelType::Province ? a.ui.sel.id : 0; }

Id provinceUnder(App& a, const PointerEvent& e) { return a.map().provinceAt(e.sx, e.sy); }

std::string provinceTitle(const World& w, Id province) {
  const Province* p = w.province(province);
  if (!p) return "—";
  if (!p->name.empty()) return p->name;
  return p->sea ? "Морская провинция" : "Без названия";
}

std::string newProvinceName(const World& w) {
  std::set<std::string> taken;
  w.provinces.each([&](const Province& p) { taken.insert(p.name); });
  const std::string base = "Новая провинция";
  if (!taken.count(base)) return base;
  for (int i = 2;; i++) {
    std::string n = base + " " + std::to_string(i);
    if (!taken.count(n)) return n;
  }
}

// ================================================================ цвета
Palette palette() {
  Palette p;
  p.accent = ui::toneColor(ui::Tone::Accent);
  p.danger = ui::toneColor(ui::Tone::Danger);
  p.success = ui::toneColor(ui::Tone::Success);
  p.info = ui::toneColor(ui::Tone::Info);
  p.light = Color(255, 252, 244);
  p.ink = Color(18, 14, 8);
  return p;
}

// ================================================================ рисование
Pts toScreen(const map::View& v, const std::vector<Vec2>& pts) {
  Pts out;
  out.reserve(pts.size());
  for (Vec2 p : pts) out.push_back(v.toScreen(p));
  return out;
}

gfx::Path polyPath(const Pts& p, bool closed) {
  gfx::Path path;
  if (!p.empty()) path.addPolygon(p.data(), p.size(), closed);
  return path;
}

void strokeLine(gfx::Canvas& c, const Pts& p, bool closed, Color col, float width, std::vector<float> dash) {
  if (p.size() < 2 || col.a == 0) return;
  gfx::Stroke s;
  s.width = width;
  s.join = gfx::Join::Round;
  s.cap = gfx::Cap::Round;
  s.dash = std::move(dash);
  c.strokePath(polyPath(p, closed), s, col);
}

void glowLine(gfx::Canvas& c, const Pts& p, bool closed, Color col, float width, bool dashed) {
  if (p.size() < 2) return;
  const Palette P = palette();
  strokeLine(c, p, closed, col.alpha(0.22f), width + 7);
  strokeLine(c, p, closed, P.ink.alpha(0.55f), width + 2.4f);
  if (dashed) {
    strokeLine(c, p, closed, P.light, width);
    strokeLine(c, p, closed, col, width, {6.5f, 5});
  } else {
    strokeLine(c, p, closed, col, width);
  }
}

void fillPoly(gfx::Canvas& c, const Pts& p, Color col) {
  if (p.size() < 3 || col.a == 0) return;
  c.fillPath(polyPath(p, true), col, gfx::FillRule::EvenOdd);
}

namespace {
gfx::Path markPath(gfx::Pt p, Mark m, float r) {
  gfx::Path path;
  switch (m) {
    case Mark::Dot: path.addCircle(p.x, p.y, r); break;
    case Mark::Square: path.addRoundRect(RectF{p.x - r, p.y - r, 2 * r, 2 * r}, std::max(1.f, r * 0.35f)); break;
    case Mark::Diamond: {
      const float d = r * 1.32f;
      path.moveTo(p.x, p.y - d);
      path.lineTo(p.x + d, p.y);
      path.lineTo(p.x, p.y + d);
      path.lineTo(p.x - d, p.y);
      path.close();
      break;
    }
  }
  return path;
}
}  // namespace

void handleMark(gfx::Canvas& c, gfx::Pt p, Mark m, float r, Color fill, Color ring, float glow) {
  if (glow > 0) c.fillCircle(p.x, p.y, r + glow + 2, fill.alpha(0.26f));
  // Мягкая тень, обводка, заливка.
  c.fillPath(markPath(gfx::Pt{p.x, p.y + 1}, m, r + 1.8f), palette().ink.alpha(0.28f));
  c.fillPath(markPath(p, m, r + 1.5f), ring);
  c.fillPath(markPath(p, m, r), fill);
}

void insertGhost(gfx::Canvas& c, gfx::Pt p) {
  const Palette P = palette();
  handleMark(c, p, Mark::Dot, 6, P.accent, P.ink, 2);
  c.line(p.x - 3.2f, p.y, p.x + 3.2f, p.y, 1.6f, P.ink);
  c.line(p.x, p.y - 3.2f, p.x, p.y + 3.2f, 1.6f, P.ink);
}

void snapRing(gfx::Canvas& c, gfx::Pt p, Color col, bool edge) {
  const Palette P = palette();
  c.strokeCircle(p.x, p.y, 8.5f, 3.4f, P.ink.alpha(0.45f));
  c.strokeCircle(p.x, p.y, 8.5f, 1.8f, col);
  if (edge) {
    // К границе — перекрестие внутри кольца.
    c.line(p.x - 4, p.y, p.x + 4, p.y, 1.4f, col);
    c.line(p.x, p.y - 4, p.x, p.y + 4, 1.4f, col);
  } else {
    c.fillCircle(p.x, p.y, 3.2f, P.light);
    c.fillCircle(p.x, p.y, 2.2f, col);
  }
}

void cursorBadge(gfx::Canvas& c, gfx::Pt cursor, const char* icon, Color col) {
  const ui::Theme& t = ui::theme();
  const float cx = cursor.x + 17, cy = cursor.y + 17, r = 10;
  c.fillCircle(cx, cy + 1, r + 1.5f, palette().ink.alpha(0.3f));
  c.fillCircle(cx, cy, r, t.surface1.alpha(0.96f));
  c.strokeCircle(cx, cy, r - 0.5f, 1.4f, col);
  gfx::drawIcon(c, icon, RectF{cx - 6.5f, cy - 6.5f, 13, 13}, col, 1.1f);
}

namespace {
void addRings(gfx::Path& out, const geo::Face& f, const map::View& v) {
  for (const auto& ring : f.rings) {
    if (ring.size() < 3) continue;
    for (size_t i = 0; i < ring.size(); i++) {
      const gfx::Pt q = v.toScreen(ring[i]);
      if (i == 0) out.moveTo(q.x, q.y);
      else out.lineTo(q.x, q.y);
    }
    out.close();
  }
}
}  // namespace

gfx::Path provincePath(const World& w, const map::View& v, Id province) {
  gfx::Path out;
  auto fs = geo::faces(w);
  const geo::ProvinceShape* sh = fs->shape(province);
  if (!sh || !sh->box.intersects(v.visibleBox())) return out;
  for (int fi : sh->faces) addRings(out, fs->faces[size_t(fi)], v);
  return out;
}

gfx::Path facePath(const geo::Face& f, const map::View& v) {
  gfx::Path out;
  addRings(out, f, v);
  return out;
}

void highlightProvince(gfx::Canvas& c, const World& w, const map::View& v, Id province, Color fill, Color stroke, float width,
                       bool dashed) {
  gfx::Path p = provincePath(w, v, province);
  if (p.empty()) return;
  if (fill.a) c.fillPath(p, fill, gfx::FillRule::EvenOdd);
  gfx::Stroke s;
  s.join = gfx::Join::Round;
  s.cap = gfx::Cap::Round;
  s.width = width + 2.4f;
  c.strokePath(p, s, palette().ink.alpha(0.4f));
  s.width = width;
  if (dashed) s.dash = {7, 5};
  c.strokePath(p, s, stroke);
}

// ================================================================ прилипание
Snap snapAt(const World& w, Vec2 p, double tol) {
  Snap s;
  if (!(tol > 0)) return s;
  double best = tol * tol;
  w.nodes.each([&](const Node& n) {
    double d = dist2(n.p, p);
    if (d <= best) {
      best = d;
      s.kind = Snap::Vertex;
      s.p = n.p;
    }
  });
  w.edges.each([&](const Edge& e) {
    for (Vec2 q : e.pts) {
      double d = dist2(q, p);
      if (d < best) {
        best = d;
        s.kind = Snap::Vertex;
        s.p = q;
      }
    }
  });
  if (s.kind != Snap::None) return s;
  if (auto h = geo::hitEdge(w, p, tol, 0)) {
    s.kind = Snap::Edge;
    s.p = h->p;
  }
  return s;
}

// ================================================================ ввод контура
void Sketch::clear() {
  pts_.clear();
  free_.clear();
  pressed_ = lasso_ = lassoFromStart_ = false;
  simpleN_ = size_t(-1);
}

void Sketch::pop() {
  if (pts_.empty()) return;
  // Участок, нарисованный от руки, убирается целиком (до вершины, поставленной щелчком).
  if (free_.back()) {
    while (pts_.size() > 1 && free_.back() && free_[free_.size() - 2]) {
      pts_.pop_back();
      free_.pop_back();
    }
  }
  pts_.pop_back();
  free_.pop_back();
  simpleN_ = size_t(-1);
}

Vec2 Sketch::place(App& a, const PointerEvent& e) {
  if (!snapping) return e.map;
  Snap s = snapAt(a.world(), e.map, a.map().view().toMapLen(kSnapPx));
  return s.kind != Snap::None ? s.p : e.map;
}

std::optional<Vec2> Sketch::cursor(const App& a) const {
  if (!hasCur_ || !a.ui.cursorMap) return std::nullopt;
  return snap_.kind != Snap::None ? snap_.p : cur_;
}

bool Sketch::simple() const {
  if (pts_.size() < 3) return true;
  if (simpleN_ == pts_.size()) return simple_;
  simpleN_ = pts_.size();
  std::vector<Vec2> q;
  q.reserve(pts_.size());
  for (Vec2 p : pts_)
    if (q.empty() || dist2(q.back(), p) > 1e-12) q.push_back(p);
  if (closed && q.size() > 1 && dist2(q.front(), q.back()) <= 1e-12) q.pop_back();
  simple_ = q.size() < (closed ? 3u : 2u) || geo::isSimple(q, closed);
  return simple_;
}

std::vector<Vec2> Sketch::result(const map::View& v) const {
  std::vector<Vec2> q;
  std::vector<bool> fr;
  for (size_t i = 0; i < pts_.size(); i++) {
    if (!q.empty() && dist2(q.back(), pts_[i]) <= 1e-12) continue;
    q.push_back(pts_[i]);
    fr.push_back(free_[i]);
  }
  if (closed && q.size() > 1 && dist2(q.front(), q.back()) <= 1e-12) {
    q.pop_back();
    fr.pop_back();
  }
  bool anyFree = std::find(fr.begin(), fr.end(), true) != fr.end();
  if (!anyFree || q.size() < 4) return q;
  const double tol = v.toMapLen(0.6);
  bool anyAnchor = std::find(fr.begin(), fr.end(), false) != fr.end();
  if (!anyAnchor) return geo::simplifyDP(q, tol, closed);
  // Участки от руки между вершинами-щелчками упрощаются, сами вершины остаются.
  size_t n = q.size();
  size_t first = 0;
  while (fr[first]) first++;
  std::vector<Vec2> out;
  size_t span = closed ? n : n - first;
  out.push_back(q[first]);
  std::vector<Vec2> run{q[first]};
  for (size_t k = 1; k <= span; k++) {
    if (!closed && first + k >= n) break;
    size_t i = (first + k) % n;
    bool end = closed && k == span;
    run.push_back(q[i]);
    if (!fr[i] || end || (!closed && first + k == n - 1)) {
      std::vector<Vec2> s = run.size() > 2 ? geo::simplifyDP(run, tol, false) : run;
      out.insert(out.end(), s.begin() + 1, s.end());
      run.assign(1, q[i]);
    }
  }
  if (closed && out.size() > 1 && dist2(out.front(), out.back()) <= 1e-12) out.pop_back();
  // Начало ломаной до первой вершины-щелчка (только для открытой линии).
  if (!closed && first > 0) {
    std::vector<Vec2> head(q.begin(), q.begin() + long(first) + 1);
    std::vector<Vec2> s = head.size() > 2 ? geo::simplifyDP(head, tol, false) : head;
    s.pop_back();
    out.insert(out.begin(), s.begin(), s.end());
  }
  return out;
}

Sketch::Result Sketch::down(App& a, const PointerEvent& e) {
  if (e.button == 1) {
    if (pts_.empty() || pressed_) return Result::Ignored;
    pop();
    return Result::Changed;
  }
  if (e.button != 0) return Result::Ignored;
  move(a, e, false);
  if (e.clicks >= 2) return ready() ? Result::Finish : Result::Consumed;
  const map::View& v = a.map().view();
  if (closed && pts_.size() >= 3) {
    gfx::Pt f = v.toScreen(pts_.front());
    if (std::hypot(f.x - e.sx, f.y - e.sy) <= kClosePx) return Result::Finish;
  }
  Vec2 p = place(a, e);
  pressed_ = true;
  lasso_ = false;
  dsx_ = e.sx;
  dsy_ = e.sy;
  downCount_ = pts_.size();
  if (!pts_.empty()) {
    gfx::Pt l = v.toScreen(pts_.back());
    gfx::Pt n = v.toScreen(p);
    if (std::hypot(l.x - n.x, l.y - n.y) < 1.5f) return Result::Consumed;   // та же точка
  }
  pts_.push_back(p);
  free_.push_back(false);
  return Result::Changed;
}

Sketch::Result Sketch::move(App& a, const PointerEvent& e, bool held) {
  const map::View& v = a.map().view();
  cur_ = e.map;
  csx_ = e.sx;
  csy_ = e.sy;
  hasCur_ = true;
  if (held && pressed_ && freehand) {
    if (!lasso_ && std::hypot(e.sx - dsx_, e.sy - dsy_) > kLassoPx) {
      lasso_ = true;
      lassoFromStart_ = downCount_ == 0;
      snap_ = {};
    }
    if (lasso_) {
      gfx::Pt l = v.toScreen(pts_.back());
      if (std::hypot(l.x - e.sx, l.y - e.sy) >= kLassoStepPx) {
        pts_.push_back(e.map);
        free_.push_back(true);
        return Result::Changed;
      }
      return Result::Consumed;
    }
  }
  snap_ = snapping && !lasso_ ? snapAt(a.world(), e.map, v.toMapLen(kSnapPx)) : Snap{};
  return Result::Consumed;
}

Sketch::Result Sketch::up(App& a, const PointerEvent& e) {
  if (!pressed_) return Result::Ignored;
  pressed_ = false;
  if (!lasso_) return Result::Consumed;
  lasso_ = false;
  const map::View& v = a.map().view();
  gfx::Pt l = v.toScreen(pts_.back());
  if (std::hypot(l.x - e.sx, l.y - e.sy) >= 1.5f) {
    pts_.push_back(e.map);
    free_.push_back(true);
  }
  if (lassoFromStart_ && ready()) return Result::Finish;
  return Result::Changed;
}

Sketch::Result Sketch::key(App& a, const platform::Event& e) {
  using platform::Key;
  if (e.key == Key::Escape && e.mods == 0) {
    if (!active()) return Result::Ignored;
    clear();
    return Result::Cancel;
  }
  if ((e.key == Key::Enter || e.key == Key::NumEnter) && (e.mods & (platform::ModCtrl | platform::ModSuper)) == 0) {
    if (!active()) return Result::Ignored;
    if (ready()) return Result::Finish;
    a.toast(closed ? "Нужно не меньше трёх точек" : "Нужно не меньше двух точек", ToastKind::Info, "info");
    return Result::Consumed;
  }
  // Backspace, а пока контур не готов — и Ctrl+Z: убрать последнюю вершину (мир ещё не менялся).
  if ((e.key == Key::Backspace && e.mods == 0) || (e.key == Key::Z && e.mods == platform::primaryMod())) {
    if (!active() || pressed_) return Result::Ignored;
    pop();
    return Result::Changed;
  }
  return Result::Ignored;
}

void Sketch::draw(App& a, gfx::Canvas& c, const map::View& v, Color line, Color fill) const {
  const Palette P = palette();
  std::optional<Vec2> cur = cursor(a);
  Pts sp = toScreen(v, pts_);
  bool ok = simple();
  Color lc = ok ? line : P.danger;
  std::optional<gfx::Pt> cs;
  if (cur && !lasso_) cs = v.toScreen(*cur);
  bool nearFirst = false;
  if (closed && sp.size() >= 3 && hasCur_ && a.ui.cursorMap && !lasso_)
    nearFirst = std::hypot(sp.front().x - csx_, sp.front().y - csy_) <= kClosePx;
  if (closed && sp.size() >= 2) {
    Pts poly = sp;
    if (cs && !nearFirst) poly.push_back(*cs);
    fillPoly(c, poly, ok ? fill : P.danger.alpha(0.16f));
  }
  if (sp.size() >= 2) glowLine(c, sp, false, lc, 2.0f);
  if (nearFirst) glowLine(c, Pts{sp.back(), sp.front()}, false, lc, 2.0f);   // контур замкнётся
  if (cs && !sp.empty() && !nearFirst) {
    Pts band{sp.back(), *cs};
    strokeLine(c, band, false, P.ink.alpha(0.4f), 3.4f);
    strokeLine(c, band, false, P.light.alpha(0.9f), 1.6f);
    strokeLine(c, band, false, lc, 1.6f, {5, 4});
    if (closed && sp.size() >= 2) strokeLine(c, Pts{*cs, sp.front()}, false, lc.alpha(0.55f), 1.3f, {3, 4});
  }
  // Вершины: щелчки — точки, от руки — без точек (кроме концов).
  for (size_t i = 0; i < sp.size(); i++) {
    bool end = i == 0 || i + 1 == sp.size();
    if (free_[i] && !end) continue;
    if (i == 0 && closed && sp.size() >= 3) continue;
    handleMark(c, sp[i], Mark::Dot, end ? 3.6f : 3.0f, P.light, P.ink.alpha(0.85f));
  }
  if (closed && sp.size() >= 3) {
    // Первая вершина — цель замыкания.
    if (nearFirst) handleMark(c, sp.front(), Mark::Dot, 6.5f, lc, P.light, 5);
    else handleMark(c, sp.front(), Mark::Dot, 4.6f, P.light, lc);
  }
  if (cs && snap_.kind != Snap::None) snapRing(c, *cs, lc, snap_.kind == Snap::Edge);
}

// ================================================================ панель параметров
float textW(std::string_view s, ui::Font f) { return std::ceil(ui::measure(s, f)); }

OptionsBar::OptionsBar(App& a, const char* icon, std::string_view title, float width) {
  RectF area = a.mapArea();
  const float s = ui::uiScale();
  RectF ar{area.x / s, area.y / s, area.w / s, area.h / s};
  if (ar.w < 260 || ar.h < 120) return;
  float head = 16 + 18 + 6 + textW(title, ui::Font::Strong) + 6 + 9 + 6;
  float w = std::min(std::ceil(head + width), ar.w - 24);
  float y = ar.y + (a.readOnly() ? 52 : 0);
  RectF r{std::round(ar.cx() - w * 0.5f), y, w, 46};
  open_ = true;
  panel_.emplace("##tool.options", r, ui::PanelOpt{.pad = 8, .radius = 12, .glass = true});
  a.markUi("tool.options", r);
  row_.emplace(30, ui::Align::Left, 6);
  ui::icon(icon, ui::Ink::Accent, 18);
  ui::label(title, {.font = ui::Font::Strong});
  ui::separatorV();
}

OptionsBar::~OptionsBar() {
  row_.reset();
  panel_.reset();
}

float provinceTagW(const World& w, Id province) { return textW(provinceTitle(w, province), ui::Font::Small) + 36; }

void provinceTag(const World& w, Id province) {
  const Province* p = w.province(province);
  if (!p) return;
  Color col = w::factionColor(w, p->owner);
  if (ui::chip(provinceTitle(w, province) + "##ptag", {.color = col, .clickable = true, .tooltip = "Показать провинцию"}) == ui::ChipAction::Click)
    app().select(SelType::Province, province, true);
}

int sketchButtons(App& a, const Sketch& s, bool readOnly) {
  using platform::Key;
  int r = 0;
  ui::Disabled dis(readOnly);
  if (ui::iconButton("undo", "Убрать последнюю точку", {.disabled = !s.active()})) r = 2;
  ui::tooltip("Убрать последнюю точку", {Key::Backspace, 0});
  a.markUi("tool.options.pop");
  if (ui::iconButton("close", "Отменить контур", {.disabled = !s.active()})) r = -1;
  ui::tooltip("Отменить контур", {Key::Escape, 0});
  a.markUi("tool.options.cancel");
  if (ui::button("Готово", {.variant = ui::Variant::Primary, .icon = "check", .size = ui::Size::Small, .disabled = !s.ready()})) r = 1;
  ui::tooltip("Готово", {Key::Enter, 0});
  a.markUi("tool.options.done");
  return r;
}

float sketchButtonsW() { return 30 + 6 + 30 + 6 + textW("Готово", ui::Font::Strong) + 50; }

// ================================================================ регистрация
void installAll() {
  registerBordersTool();
  registerDrawTools();
  registerClickTools();
  registerRouteTool();
}

}  // namespace rg::app::tools
