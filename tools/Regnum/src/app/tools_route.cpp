// Regnum — инструмент «Торговый маршрут» (ТЗ 1.d.v): щелчки — точки нового маршрута, Enter или двойной щелчок —
// готово, гильдия-владелец — на плавающей панели. Щелчок по маршруту выбирает его для правки: ручки точек
// тянутся, двойной щелчок по линии — новая точка, Delete или меню — удалить точку, меню — удалить маршрут.
// Провинции на пути подсвечиваются: каждая получает +10 % базовой торговой ценности.
#include "app/tools_edit.h"

namespace rg::app::tools {

namespace {

using platform::Key;
using Result = Sketch::Result;

// Сглаживание линии как на карте (Катмулл — Ром, натяжение 0,5); seg[k] — отрезок исходной ломаной.
std::vector<Vec2> smooth(const std::vector<Vec2>& pts, std::vector<int>* seg = nullptr) {
  std::vector<Vec2> out;
  const size_t n = pts.size();
  if (seg) seg->clear();
  if (n < 3) {
    out = pts;
    if (seg)
      for (size_t i = 0; i < n; i++) seg->push_back(int(std::min(i, n > 1 ? n - 2 : 0)));
    return out;
  }
  for (size_t i = 0; i + 1 < n; i++) {
    const Vec2 p0 = pts[i == 0 ? 0 : i - 1], p1 = pts[i], p2 = pts[i + 1], p3 = pts[i + 2 < n ? i + 2 : n - 1];
    const int steps = clamp(int(dist(p1, p2) / 12), 4, 24);
    for (int s = 0; s < steps; s++) {
      const double t = double(s) / steps, t2 = t * t, t3 = t2 * t;
      out.push_back((p1 * 2 + (p2 - p0) * t + (p0 * 2 - p1 * 5 + p2 * 4 - p3) * t2 + (p1 * 3 - p0 - p2 * 3 + p3) * t3) * 0.5);
      if (seg) seg->push_back(int(i));
    }
  }
  out.push_back(pts.back());
  if (seg) seg->push_back(int(n - 2));
  return out;
}

Color routeColorOf(const World& w, const Route& r) {
  if (r.color) return r.color->withA(255);
  if (const Faction* g = w.faction(r.guild)) return g->color;
  return ui::toneColor(ui::Tone::Accent);
}

std::string pointsText(int n) { return std::to_string(n) + " " + plural(n, "точка", "точки", "точек"); }

Id selectedRoute(const App& a) { return a.ui.sel.type == SelType::Route ? a.ui.sel.id : 0; }

// Линия маршрута как на карте: светлая подложка, полупрозрачный цвет гильдии, тёмный пунктир по центру.
void routeLine(gfx::Canvas& c, const Pts& line, Color col) {
  if (line.size() < 2) return;
  const Palette P = palette();
  strokeLine(c, line, false, P.accent.alpha(0.3f), 13);
  strokeLine(c, line, false, P.light.alpha(0.8f), 9.5f);
  strokeLine(c, line, false, col.alpha(0.6f), 7);
  strokeLine(c, line, false, Color::mix(col, P.ink, 0.25f), 2.4f, {9, 6});
}

class RouteTool final : public MapTool {
 public:
  RouteTool() {
    sk_.closed = false;
    sk_.freehand = false;
    sk_.snapping = false;
  }

  void activate(App& a) override {
    // Маршруты видны в режимах «Гильдии» и «Торговля» (ТЗ 1.d.v).
    if (a.ui.mapMode != schema::MapMode::Guilds && a.ui.mapMode != schema::MapMode::Trade) a.setMapMode(schema::MapMode::Guilds);
  }
  void deactivate(App& a) override {
    if (drag_.active && drag_.committed) a.store.endCoalesce();
    drag_ = {};
    sk_.clear();
  }

  // ---------------------------------------------------------------- указатель
  bool pointerDown(App& a, const PointerEvent& e) override {
    const World& w = a.world();
    if (sk_.active()) {
      Result r = sk_.down(a, e);
      if (r == Result::Finish) finish(a);
      return r != Result::Ignored;
    }
    Id rsel = selectedRoute(a);
    const Route* rt = rsel ? w.route(rsel) : nullptr;
    if (e.button == 1) return contextMenu(a, e, rt);
    if (e.button != 0) return false;
    if (rt && !a.readOnly()) {
      int vi = vertexAt(a, *rt, e.sx, e.sy);
      if (vi >= 0) {
        selV_ = vi;
        beginDrag(a, rsel, vi, e);
        return true;
      }
      if (e.clicks >= 2) {
        if (auto ins = insertionAt(a, *rt, e.sx, e.sy)) {
          int at = ins->first;
          Vec2 p = ins->second;
          std::vector<Vec2> pts = rt->pts;
          pts.insert(pts.begin() + at, p);
          if (a.act("Добавить точку маршрута", [&](Tx& tx) { rules::setRoutePoints(tx, rsel, pts); })) {
            selV_ = at;
            beginDrag(a, rsel, at, e);
          }
          return true;
        }
      }
    }
    if (Id r = a.map().routeAt(e.sx, e.sy)) {
      if (r != rsel) {
        a.select(SelType::Route, r);
        selV_ = -1;
      }
      return true;
    }
    // Пустое место: снять выделение маршрута (если он правился), иначе — начать новый маршрут.
    if (rsel) {
      a.clearSelection();
      selV_ = -1;
      return true;
    }
    if (a.readOnly()) return true;
    Result r = sk_.down(a, e);
    return r != Result::Ignored;
  }

  bool pointerMove(App& a, const PointerEvent& e) override {
    if (drag_.active) {
      if (!drag_.moved && std::hypot(e.sx - drag_.sx, e.sy - drag_.sy) < kDragPx) return true;
      drag_.moved = true;
      const Route* rt = a.world().route(drag_.route);
      if (!rt || drag_.index >= int(rt->pts.size())) {
        drag_ = {};
        return true;
      }
      Vec2 p{clamp(e.map.x, 0.0, schema::kMapWidth), clamp(e.map.y, 0.0, schema::kMapHeight)};
      std::vector<Vec2> pts = rt->pts;
      pts[size_t(drag_.index)] = p;
      TxOptions opt;
      opt.coalesce = drag_.key;
      opt.coalesceSec = 1e9;
      Id rid = drag_.route;
      if (a.act("Изменить маршрут", [&](Tx& tx) { rules::setRoutePoints(tx, rid, pts); }, opt)) drag_.committed = true;
      return true;
    }
    Result r = sk_.move(a, e, false);
    return r != Result::Ignored;
  }

  bool pointerUp(App& a, const PointerEvent& e) override {
    if (drag_.active) {
      if (drag_.committed) a.store.endCoalesce();
      drag_ = {};
      return true;
    }
    Result r = sk_.up(a, e);
    return r != Result::Ignored;
  }

  bool key(App& a, const platform::Event& e) override {
    if (sk_.active()) {
      Result r = sk_.key(a, e);
      if (r == Result::Finish) finish(a);
      return r != Result::Ignored;
    }
    if (e.key == Key::Escape && e.mods == 0) {
      if (drag_.active) {
        if (drag_.committed && a.store.canUndo() && a.store.undoLabel() == "Изменить маршрут") a.store.undo();
        drag_ = {};
        return true;
      }
      if (selV_ >= 0) {
        selV_ = -1;
        return true;
      }
      return false;
    }
    if (a.readOnly() || drag_.active) return false;
    Id rsel = selectedRoute(a);
    const Route* rt = rsel ? a.world().route(rsel) : nullptr;
    if (rt && (e.key == Key::Delete || e.key == Key::Backspace) && e.mods == 0) {
      int v = selV_ >= 0 ? selV_ : hoverV_;
      if (v >= 0 && v < int(rt->pts.size())) removeVertex(a, rsel, v);
      else askRemove(a, rsel);
      return true;
    }
    return false;
  }

  // ---------------------------------------------------------------- рисование
  void drawOverlay(App& a, gfx::Canvas& c, const map::View& v) override {
    const World& w = a.world();
    Id rsel = selectedRoute(a);
    if (rsel != selFor_) {
      selFor_ = rsel;
      selV_ = -1;
    }
    if (const Faction* g = w.faction(guild_); !g || !g->isGuild()) guild_ = 0;
    const Route* rt = rsel ? w.route(rsel) : nullptr;
    if (rt && selV_ >= int(rt->pts.size())) selV_ = -1;
    hoverV_ = -1;
    hoverIns_.reset();
    if (rt && a.ui.cursorMap && !sk_.active() && !drag_.active) {
      gfx::Pt cs = v.toScreen(*a.ui.cursorMap);
      hoverV_ = vertexAt(a, *rt, cs.x, cs.y);
      if (hoverV_ < 0) hoverIns_ = insertionAt(a, *rt, cs.x, cs.y);
    }
    if (sk_.active()) drawSketch(a, c, v);
    else if (rt) drawSelected(a, c, v, *rt);
    if (!sk_.active() && !rt && a.ui.cursorMap && !a.readOnly()) {
      gfx::Pt cs = v.toScreen(*a.ui.cursorMap);
      if (!a.map().routeAt(cs.x, cs.y)) cursorBadge(c, cs, "plus", guildColor(w));
    }
    menu(a);
    options(a);
  }

  platform::Cursor cursor(App& a) override {
    if (drag_.active) return platform::Cursor::Grabbing;
    if (hoverV_ >= 0) return platform::Cursor::Move;
    if (sk_.active()) return platform::Cursor::Crosshair;
    if (hoverIns_) return platform::Cursor::Crosshair;
    if (a.ui.cursorMap) {
      gfx::Pt s = a.map().view().toScreen(*a.ui.cursorMap);
      if (a.map().routeAt(s.x, s.y)) return platform::Cursor::Hand;
    }
    return a.readOnly() ? platform::Cursor::Arrow : platform::Cursor::Crosshair;
  }

  const char* hint(App& a) override {
    if (a.readOnly()) return "Прошлый ход: щелчок по маршруту — сведения";
    if (sk_.active()) return "Щелчок — точка · Enter — готово · Esc — отмена";
    if (selectedRoute(a)) return "Тяните точки · двойной щелчок — новая точка";
    return "Щелчки — новый маршрут · щелчок по маршруту — правка";
  }

 private:
  Sketch sk_;
  static inline Id guild_ = 0;       // гильдия новых маршрутов (запоминается между включениями)
  struct Drag {
    bool active = false, moved = false, committed = false;
    Id route = 0;
    int index = -1;
    float sx = 0, sy = 0;
    std::string key;
  } drag_;
  int selV_ = -1, hoverV_ = -1;
  std::optional<std::pair<int, Vec2>> hoverIns_;
  Id selFor_ = 0;
  u64 seq_ = 0;
  // Меню
  Id menuRoute_ = 0;
  int menuV_ = -1;
  std::optional<std::pair<int, Vec2>> menuIns_;

  // Линия будущего маршрута: поставленные точки и указатель.
  std::vector<Vec2> preview(const App& a) const {
    std::vector<Vec2> out = sk_.points();
    if (auto cur = sk_.cursor(a)) out.push_back(*cur);
    return out;
  }
  // Сухопутные провинции на линии (они получают +10 % базовой ценности).
  static std::vector<Id> crossedLand(const World& w, const std::vector<Vec2>& line) {
    std::vector<Id> out;
    if (line.size() < 2) return out;
    auto fs = geo::faces(w);
    for (Id pid : fs->provincesOnPolyline(line)) {
      const Province* p = w.province(pid);
      if (p && !p->sea) out.push_back(pid);
    }
    return out;
  }

  Color guildColor(const World& w) const {
    const Faction* g = w.faction(guild_);
    return g ? g->color : ui::toneColor(ui::Tone::Accent);
  }

  static int vertexAt(App& a, const Route& r, float sx, float sy) {
    const map::View& v = a.map().view();
    int best = -1;
    float bd = kHitPx;
    for (int i = 0; i < int(r.pts.size()); i++) {
      gfx::Pt s = v.toScreen(r.pts[size_t(i)]);
      float d = std::hypot(s.x - sx, s.y - sy);
      if (d <= bd) {
        bd = d;
        best = i;
      }
    }
    return best;
  }

  // Место новой точки на линии маршрута: индекс вставки и точка (по сглаженной линии, как на карте).
  static std::optional<std::pair<int, Vec2>> insertionAt(App& a, const Route& r, float sx, float sy) {
    if (r.pts.size() < 2) return std::nullopt;
    const map::View& v = a.map().view();
    std::vector<int> seg;
    std::vector<Vec2> sm = smooth(r.pts, &seg);
    double bd = double(kHitPx) * kHitPx;
    std::optional<std::pair<int, Vec2>> best;
    for (size_t i = 1; i < sm.size(); i++) {
      gfx::Pt p = v.toScreen(sm[i - 1]), q = v.toScreen(sm[i]);
      double abx = q.x - p.x, aby = q.y - p.y, l2 = abx * abx + aby * aby;
      double t = l2 > 0 ? clamp(((sx - p.x) * abx + (sy - p.y) * aby) / l2, 0.0, 1.0) : 0;
      double dx = p.x + abx * t - sx, dy = p.y + aby * t - sy, d = dx * dx + dy * dy;
      if (d <= bd) {
        bd = d;
        best = std::make_pair(seg[i - 1] + 1, sm[i - 1] + (sm[i] - sm[i - 1]) * t);
      }
    }
    return best;
  }

  void beginDrag(App&, Id route, int index, const PointerEvent& e) {
    drag_ = {};
    drag_.active = true;
    drag_.route = route;
    drag_.index = index;
    drag_.sx = e.sx;
    drag_.sy = e.sy;
    drag_.key = "route.drag." + std::to_string(++seq_);
  }

  void finish(App& a) {
    if (!sk_.ready()) return;
    std::vector<Vec2> pts = sk_.result(a.map().view());
    Id rid = 0, guild = guild_;
    if (a.act("Новый торговый маршрут", [&](Tx& tx) { rid = rules::createRoute(tx, pts, guild); })) {
      sk_.clear();
      a.select(SelType::Route, rid);
    }
  }

  void removeVertex(App& a, Id route, int v) {
    const Route* r = a.world().route(route);
    if (!r) return;
    if (r->pts.size() <= 2) {
      a.toast("В маршруте не меньше двух точек — удалите маршрут целиком", ToastKind::Info, "info");
      return;
    }
    std::vector<Vec2> pts = r->pts;
    pts.erase(pts.begin() + v);
    if (a.act("Удалить точку маршрута", [&](Tx& tx) { rules::setRoutePoints(tx, route, pts); })) selV_ = -1;
  }

  static void askRemove(App& a, Id route) {
    const Route* r = a.world().route(route);
    if (!r) return;
    std::string name = r->name.empty() ? std::string("Торговый маршрут") : r->name;
    a.confirm("Удалить маршрут?", "«" + name + "» исчезнет с карты; провинции на пути потеряют его +10 % торговой ценности. Ctrl+Z вернёт.",
              "Удалить", true, [route](App& x) {
                if (!x.world().route(route)) return;
                bool wasSel = x.ui.sel == Selection{SelType::Route, route};
                if (x.act("Удалить маршрут", [&](Tx& tx) { rules::removeRoute(tx, route); }) && wasSel) x.clearSelection();
              });
  }

  // ---- меню правой кнопки
  bool contextMenu(App& a, const PointerEvent& e, const Route* rt) {
    if (a.readOnly()) return false;
    menuV_ = -1;
    menuIns_.reset();
    menuRoute_ = 0;
    if (rt) {
      menuV_ = vertexAt(a, *rt, e.sx, e.sy);
      if (menuV_ < 0) menuIns_ = insertionAt(a, *rt, e.sx, e.sy);
      if (menuV_ >= 0 || menuIns_) menuRoute_ = rt->id;
    }
    if (!menuRoute_) {
      Id r = a.map().routeAt(e.sx, e.sy);
      if (!r) return false;
      a.select(SelType::Route, r);
      menuRoute_ = r;
    }
    if (menuV_ >= 0) selV_ = menuV_;
    ui::openContextMenu("route.menu");
    return true;
  }

  void menu(App& a) {
    if (!ui::beginMenu("route.menu")) return;
    const World& w = a.world();
    const Route* r = w.route(menuRoute_);
    ui::menuHeader(r ? (r->name.empty() ? std::string("Торговый маршрут") : r->name) : std::string("Маршрут"));
    if (r && menuV_ >= 0) {
      if (ui::menuItem("Удалить точку", {.icon = "minus", .shortcut = {Key::Delete, 0}, .disabled = r->pts.size() <= 2 || menuV_ >= int(r->pts.size())}))
        removeVertex(a, menuRoute_, menuV_);
      a.markUi("route.menu.vertex");
    } else if (r && menuIns_) {
      if (ui::menuItem("Добавить точку", {.icon = "plus"})) {
        std::vector<Vec2> pts = r->pts;
        int at = std::min(menuIns_->first, int(pts.size()));
        pts.insert(pts.begin() + at, menuIns_->second);
        Id rid = menuRoute_;
        if (a.act("Добавить точку маршрута", [&](Tx& tx) { rules::setRoutePoints(tx, rid, pts); })) selV_ = at;
      }
      a.markUi("route.menu.insert");
    }
    ui::menuSeparator();
    if (ui::menuItem("Удалить маршрут", {.icon = "trash", .danger = true, .disabled = !r})) askRemove(a, menuRoute_);
    a.markUi("route.menu.remove");
    ui::endMenu();
  }

  // ---- предпросмотр нового маршрута
  void drawSketch(App& a, gfx::Canvas& c, const map::View& v) {
    const World& w = a.world();
    const Palette P = palette();
    std::vector<Vec2> pts = sk_.points();
    std::optional<Vec2> cur = sk_.cursor(a);
    std::vector<Vec2> withCur = preview(a);
    // Провинции на пути (+10 % каждой) — лёгкая подсветка.
    for (Id pid : crossedLand(w, withCur)) highlightProvince(c, w, v, pid, P.success.alpha(0.2f), P.success, 1.8f, true);
    Color col = guildColor(w);
    Pts line = toScreen(v, smooth(pts));
    routeLine(c, line, col);
    if (cur && !pts.empty()) {
      std::vector<Vec2> tail = smooth(withCur);
      // Последний участок до указателя — пунктир.
      Pts band{v.toScreen(pts.back()), v.toScreen(*cur)};
      if (pts.size() >= 2) {
        size_t from = 0;
        std::vector<int> seg;
        tail = smooth(withCur, &seg);
        for (size_t i = 0; i < seg.size(); i++)
          if (seg[i] >= int(pts.size()) - 1) {
            from = i;
            break;
          }
        band.assign(1, v.toScreen(tail[from]));
        for (size_t i = from + 1; i < tail.size(); i++) band.push_back(v.toScreen(tail[i]));
      }
      strokeLine(c, band, false, P.ink.alpha(0.35f), 4.4f);
      strokeLine(c, band, false, P.light.alpha(0.95f), 2.6f);
      strokeLine(c, band, false, col, 2.6f, {6, 5});
    }
    for (size_t i = 0; i < pts.size(); i++) {
      bool end = i == 0 || i + 1 == pts.size();
      handleMark(c, v.toScreen(pts[i]), Mark::Dot, end ? 4.4f : 3.4f, end ? col : P.light, end ? P.light : P.ink.alpha(0.85f));
    }
  }

  // ---- ручки выбранного маршрута
  void drawSelected(App& a, gfx::Canvas& c, const map::View& v, const Route& r) {
    const World& w = a.world();
    const Palette P = palette();
    Color col = routeColorOf(w, r);
    if (hoverIns_ && !a.readOnly()) insertGhost(c, v.toScreen(hoverIns_->second));
    for (int i = 0; i < int(r.pts.size()); i++) {
      gfx::Pt s = v.toScreen(r.pts[size_t(i)]);
      bool sel = i == selV_ || (drag_.active && i == drag_.index);
      bool hov = i == hoverV_;
      bool end = i == 0 || i + 1 == int(r.pts.size());
      Color fill = sel ? P.accent : hov ? P.accent.lighten(0.35f) : end ? col : P.light;
      handleMark(c, s, Mark::Dot, sel || hov ? 5.4f : end ? 4.6f : 3.8f, fill, P.ink, sel ? 4.f : hov ? 2.f : 0.f);
    }
  }

  // ---- панель параметров
  void options(App& a) {
    const World& w = a.world();
    Id rsel = selectedRoute(a);
    const Route* rt = rsel ? w.route(rsel) : nullptr;
    if (rt && !sk_.active()) {
      std::string name = rt->name.empty() ? std::string("Торговый маршрут") : rt->name;
      std::string cnt = pointsText(int(rt->pts.size()));
      float width = textW(name, ui::Font::Small) + 36 + 6 + textW(cnt, ui::Font::Small) + 6 + 30 + 6 + 30;
      OptionsBar bar(a, "tool-route", "Маршрут", width);
      if (!bar) return;
      ui::chip(name + "##rname", {.color = routeColorOf(w, *rt)});
      ui::label(cnt, {.font = ui::Font::Small, .ink = ui::Ink::Dim});
      ui::flex();
      ui::Disabled dis(a.readOnly());
      bool canV = selV_ >= 0 && rt->pts.size() > 2;
      if (ui::iconButton("minus", "Удалить точку", {.disabled = !canV})) removeVertex(a, rsel, selV_);
      ui::tooltip("Удалить точку", {Key::Delete, 0});
      a.markUi("tool.options.vertex");
      if (ui::iconButton("trash", "Удалить маршрут", {.tone = ui::Tone::Danger})) askRemove(a, rsel);
      a.markUi("tool.options.remove");
      return;
    }
    std::string cnt = sk_.active() ? pointsText(sk_.count()) : std::string();
    int crossed = int(crossedLand(w, preview(a)).size());
    std::string bonus = crossed ? std::to_string(crossed) + " пров. · +10 %" : std::string();
    float width = 220 + 6 + (cnt.empty() ? 0 : textW(cnt, ui::Font::Small) + 6) + (bonus.empty() ? 0 : textW(bonus, ui::Font::Strong) + 34 + 6) +
                  sketchButtonsW();
    OptionsBar bar(a, "tool-route", "Новый маршрут", width);
    if (!bar) return;
    {
      ui::Group g(220);
      Id gid = guild_;
      if (w::factionPicker("guild", gid, w::FactionFilter::Guilds, "Без гильдии", 0, a.readOnly())) guild_ = gid;
      ui::tooltip("Гильдия-владелец нового маршрута");
    }
    a.markUi("tool.options.guild");
    if (!cnt.empty()) ui::label(cnt, {.font = ui::Font::Small, .ink = ui::Ink::Dim});
    if (!bonus.empty()) {
      ui::tag(bonus, ui::Tone::Success, "trade-value");
      ui::tooltip("Каждая провинция на пути получает +10 % базовой торговой ценности");
    }
    ui::flex();
    int r = sketchButtons(a, sk_, a.readOnly());
    if (r == 1) finish(a);
    else if (r == 2) sk_.pop();
    else if (r == -1) sk_.clear();
  }
};

ToolDef routeDef() {
  return {ToolId::Route, "tool-route", "Торговый маршрут", "R", false, [] { return std::make_unique<RouteTool>(); }, 90};
}
ToolReg regRoute(routeDef());

}  // namespace

void registerRouteTool() { ToolReg r(routeDef()); }

}  // namespace rg::app::tools
