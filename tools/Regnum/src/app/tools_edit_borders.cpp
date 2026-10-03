// Regnum — правка границ провинции (ТЗ 1.a.iv при включённой правке): щелчок по провинции открывает её границы;
// ручки — узлы (квадраты), точки (круги), стыки с берегом (ромбы скользят вдоль берега); береговые точки
// заблокированы и не показываются. Перетаскивание — живой предпросмотр одним шагом отмены; общая граница
// двигает обе провинции (одна растёт, другая уменьшается). Двойной щелчок по границе — новая точка,
// Delete или меню правой кнопки — удалить точку, стрелки — сдвиг на пиксель (Shift — на 10), Esc — снять выбор.
#include "app/tools_edit.h"

namespace rg::app::tools {

namespace {

using platform::Key;

struct Star {
  int coast = 0, frame = 0, border = 0;
};

bool handleExists(const World& w, const geo::Handle& h) {
  if (h.kind == geo::Handle::Node) return w.nodes.get(h.node) != nullptr;
  if (h.kind == geo::Handle::Point) {
    const Edge* e = w.edges.get(h.edge);
    return e && h.index >= 0 && h.index < int(e->pts.size());
  }
  return false;
}

bool inMap(Vec2 p) { return p.x > 0.5 && p.y > 0.5 && p.x < schema::kMapWidth - 0.5 && p.y < schema::kMapHeight - 0.5; }

class BordersTool final : public MapTool {
 public:
  void deactivate(App& a) override {
    if (drag_.active && drag_.committed) a.store.endCoalesce();
    drag_ = {};
  }

  // ---------------------------------------------------------------- указатель
  bool pointerDown(App& a, const PointerEvent& e) override {
    if (a.readOnly()) return true;
    const World& w = a.world();
    Id sel = selectedProvince(a);
    const double tol = a.map().view().toMapLen(kHitPx);
    geo::Handle h = sel ? hitVisible(w, e.map, tol, sel) : geo::Handle{};
    if (e.button == 1) return contextMenu(a, e, h, sel, tol);
    if (e.button != 0) return false;
    if (sel) {
      if (e.clicks >= 2 && !h) {
        if (auto eh = geo::hitEdge(w, e.map, tol, sel)) {
          geo::Handle nh;
          if (a.act("Добавить точку границы", [&](Tx& tx) { nh = geo::insertPoint(tx, eh->edge, eh->segment, eh->p); })) {
            selH_ = nh;
            beginDrag(a, nh, e);
          }
          return true;
        }
      }
      if (h) {
        selH_ = h;
        beginDrag(a, h, e);
        return true;
      }
      // Щелчок по границе выбранной провинции не переключает выделение на соседа.
      if (geo::hitEdge(w, e.map, tol, sel)) {
        selH_ = {};
        return true;
      }
    }
    selH_ = {};
    Id prov = provinceUnder(a, e);
    if (prov) {
      if (prov != sel) a.select(SelType::Province, prov);
    } else {
      a.clearSelection();
    }
    return true;
  }

  bool pointerMove(App& a, const PointerEvent& e) override {
    if (!drag_.active) return false;
    if (!drag_.moved && std::hypot(e.sx - drag_.sx, e.sy - drag_.sy) < kDragPx) return true;
    drag_.moved = true;
    drag_.at = e.map;
    const World& w = a.world();
    if (!handleExists(w, drag_.h)) {
      drag_ = {};
      return true;
    }
    Vec2 cur = geo::handlePos(w, drag_.h);
    if (drag_.slide) {
      auto t = geo::slideTarget(w, drag_.h.node, e.map);
      drag_.valid = t.has_value();
      if (t && dist2(*t, cur) > 1e-12) {
        Id node = drag_.h.node;
        Vec2 to = e.map;
        commit(a, "Сдвинуть стык границы с берегом", [&](Tx& tx) { geo::slideJunction(tx, node, to); });
      }
    } else {
      drag_.valid = inMap(e.map) && geo::canMove(w, drag_.h, e.map);
      if (drag_.valid && dist2(e.map, cur) > 1e-12) {
        geo::Handle h = drag_.h;
        Vec2 to = e.map;
        commit(a, "Переместить точку границы", [&](Tx& tx) { geo::moveHandle(tx, h, to); });
      }
    }
    return true;
  }

  bool pointerUp(App& a, const PointerEvent&) override {
    if (!drag_.active) return false;
    if (drag_.committed) a.store.endCoalesce();
    if (drag_.moved && !drag_.valid)
      a.toast(drag_.slide ? "Стык нельзя сдвинуть сюда: граница пересечёт другую линию или упрётся в соседний стык"
                          : "Точку нельзя переместить сюда: граница пересечёт другую линию",
              ToastKind::Warning, "warning");
    drag_ = {};
    return true;
  }

  // ---------------------------------------------------------------- клавиши
  bool key(App& a, const platform::Event& e) override {
    if (e.key == Key::Escape && e.mods == 0) {
      if (drag_.active) {
        // Отменить перетаскивание: всё, что успело примениться, — одним шагом.
        if (drag_.committed && a.store.canUndo() && a.store.undoLabel() == drag_.label) a.store.undo();
        drag_ = {};
        return true;
      }
      if (selH_) {
        selH_ = {};
        return true;
      }
      return false;
    }
    if (a.readOnly() || drag_.active) return false;
    if ((e.key == Key::Delete || e.key == Key::Backspace) && e.mods == 0) {
      geo::Handle h = selH_ ? selH_ : hover_;
      if (!h) return false;
      removeHandle(a, h);
      return true;
    }
    int dx = 0, dy = 0;
    if (e.key == Key::Left) dx = -1;
    else if (e.key == Key::Right) dx = 1;
    else if (e.key == Key::Up) dy = -1;
    else if (e.key == Key::Down) dy = 1;
    if ((dx || dy) && selH_ && (e.mods & ~u32(platform::ModShift)) == 0) {
      nudge(a, dx, dy, (e.mods & platform::ModShift) ? 10 : 1);
      return true;
    }
    return false;
  }

  // ---------------------------------------------------------------- рисование
  void drawOverlay(App& a, gfx::Canvas& c, const map::View& v) override {
    const World& w = a.world();
    Id sel = selectedProvince(a);
    if (sel != selFor_) {
      selFor_ = sel;
      selH_ = {};
    }
    if (selH_ && !handleExists(w, selH_)) selH_ = {};
    refreshStars(w);
    updateHover(a);
    if (sel) drawHandles(a, c, v, sel);
    menu(a);
    options(a);
  }

  platform::Cursor cursor(App& a) override {
    if (drag_.active) return drag_.moved && !drag_.valid ? platform::Cursor::NotAllowed : platform::Cursor::Grabbing;
    if (hover_) return platform::Cursor::Move;
    if (hoverEdge_ && isBorder(a.world(), hoverEdge_->edge)) return platform::Cursor::Crosshair;
    return a.ui.hover.type == SelType::Province && a.ui.hover.id != selectedProvince(a) ? platform::Cursor::Hand : platform::Cursor::Arrow;
  }

  const char* hint(App& a) override {
    if (a.readOnly()) return "Прошлый ход: только просмотр";
    if (!selectedProvince(a)) return "Щелчок по провинции — её границы";
    if (drag_.active && drag_.slide) return "Стык скользит вдоль берега";
    if (hover_ && hover_.kind == geo::Handle::Node && junction(hover_.node)) return "Стык с берегом: тяните вдоль берега";
    if (hoverEdge_ && !hover_) return isBorder(a.world(), hoverEdge_->edge) ? "Двойной щелчок — новая точка границы" : "Берег не редактируется";
    return "Тяните ручки · двойной щелчок — новая точка";
  }

 private:
  struct Drag {
    bool active = false, moved = false, committed = false, valid = true, slide = false;
    geo::Handle h;
    Vec2 start, at;
    float sx = 0, sy = 0;
    std::string key, label;
  } drag_;
  geo::Handle selH_;                   // выбранная ручка (Delete, стрелки)
  Id selFor_ = 0;
  geo::Handle hover_;
  std::optional<geo::EdgeHit> hoverEdge_;
  // Меню правой кнопки
  enum class MenuKind : u8 { Handle, Edge, Province } menuKind_ = MenuKind::Handle;
  geo::Handle menuH_;
  std::optional<geo::EdgeHit> menuEdge_;
  Id menuProv_ = 0;
  // Классификация узлов (по тождеству таблицы дуг)
  Table<Edge> starEdges_;
  bool starReady_ = false;
  std::unordered_map<Id, Star> stars_;
  u64 dragSeq_ = 0;

  // ---- узлы
  void refreshStars(const World& w) {
    if (starReady_ && w.edges.same(starEdges_)) return;
    starEdges_ = w.edges;
    starReady_ = true;
    stars_.clear();
    w.edges.each([&](const Edge& e) {
      for (Id n : {e.a, e.b}) {
        Star& s = stars_[n];
        (e.kind == EdgeKind::Coast ? s.coast : e.kind == EdgeKind::Frame ? s.frame : s.border) += 1;
      }
    });
  }
  bool junction(Id n) const {
    auto it = stars_.find(n);
    return it != stars_.end() && it->second.coast == 2 && it->second.frame == 0 && it->second.border >= 1;
  }
  bool nodeLocked(Id n) const {
    auto it = stars_.find(n);
    if (it == stars_.end()) return true;
    const Star& s = it->second;
    if (s.frame > 0) return true;
    if (s.coast > 0) return !junction(n);
    return s.border == 0;
  }
  static bool isBorder(const World& w, Id edge) {
    const Edge* e = w.edges.get(edge);
    return e && e->kind == EdgeKind::Border;
  }
  static const char* kindName(const geo::Handle& h, bool junc) {
    if (h.kind == geo::Handle::Point) return "Точка границы";
    return junc ? "Стык с берегом" : "Узел границы";
  }

  // Ручка под точкой: только подвижные и стыки (береговые точки и рамка заблокированы).
  geo::Handle hitVisible(const World& w, Vec2 p, double tol, Id sel) {
    refreshStars(w);
    geo::Handle h = geo::hitHandle(w, p, tol, sel);
    if (!h || geo::handleLocked(w, h)) return {};
    return h;
  }

  void updateHover(App& a) {
    hover_ = {};
    hoverEdge_.reset();
    Id sel = selectedProvince(a);
    if (!sel || !a.ui.cursorMap || drag_.active) return;
    const World& w = a.world();
    const double tol = a.map().view().toMapLen(kHitPx);
    hover_ = hitVisible(w, *a.ui.cursorMap, tol, sel);
    if (!hover_) hoverEdge_ = geo::hitEdge(w, *a.ui.cursorMap, tol, sel);
  }

  // ---- действия
  template <class F>
  void commit(App& a, const char* label, F&& fn) {
    TxOptions opt;
    opt.coalesce = drag_.key;
    opt.coalesceSec = 1e9;
    if (a.act(label, fn, opt)) {
      drag_.committed = true;
      drag_.label = label;
    } else {
      drag_.valid = false;
    }
  }

  void beginDrag(App& a, const geo::Handle& h, const PointerEvent& e) {
    const World& w = a.world();
    drag_ = {};
    drag_.active = true;
    drag_.h = h;
    drag_.start = drag_.at = geo::handlePos(w, h);
    drag_.sx = e.sx;
    drag_.sy = e.sy;
    drag_.slide = h.kind == geo::Handle::Node && junction(h.node);
    drag_.key = "borders.drag." + std::to_string(++dragSeq_);
  }

  void removeHandle(App& a, const geo::Handle& h) {
    if (a.act("Удалить точку границы", [&](Tx& tx) { geo::deletePoint(tx, h); })) {
      if (selH_ == h) selH_ = {};
      hover_ = {};
    }
  }

  void insertAt(App& a, const geo::EdgeHit& eh) {
    geo::Handle nh;
    if (a.act("Добавить точку границы", [&](Tx& tx) { nh = geo::insertPoint(tx, eh.edge, eh.segment, eh.p); })) selH_ = nh;
  }

  void nudge(App& a, int dx, int dy, int k) {
    const World& w = a.world();
    if (!handleExists(w, selH_)) return;
    const double step = a.map().view().toMapLen(1) * k;
    Vec2 to = geo::handlePos(w, selH_) + Vec2{dx * step, dy * step};
    TxOptions opt;
    opt.coalesce = "borders.nudge";
    if (selH_.kind == geo::Handle::Node && junction(selH_.node)) {
      Id node = selH_.node;
      a.act("Сдвинуть стык границы с берегом", [&](Tx& tx) { geo::slideJunction(tx, node, to); }, opt);
      return;
    }
    if (!inMap(to) || !geo::canMove(w, selH_, to)) {
      a.toast("Точку нельзя переместить сюда: граница пересечёт другую линию", ToastKind::Warning, "warning");
      return;
    }
    geo::Handle h = selH_;
    a.act("Переместить точку границы", [&](Tx& tx) { geo::moveHandle(tx, h, to); }, opt);
  }

  // ---- меню правой кнопки
  bool contextMenu(App& a, const PointerEvent& e, const geo::Handle& h, Id sel, double tol) {
    if (a.readOnly()) return false;
    const World& w = a.world();
    menuH_ = {};
    menuEdge_.reset();
    menuProv_ = 0;
    if (h) {
      selH_ = h;
      menuH_ = h;
      menuKind_ = MenuKind::Handle;
    } else if (auto eh = sel ? geo::hitEdge(w, e.map, tol, sel) : std::nullopt; eh && isBorder(w, eh->edge)) {
      menuEdge_ = eh;
      menuKind_ = MenuKind::Edge;
    } else if (Id prov = provinceUnder(a, e)) {
      if (prov != sel) a.select(SelType::Province, prov);
      menuProv_ = prov;
      menuKind_ = MenuKind::Province;
    } else {
      return false;
    }
    ui::openContextMenu("borders.menu");
    return true;
  }

  void menu(App& a) {
    if (!ui::beginMenu("borders.menu")) return;
    const World& w = a.world();
    switch (menuKind_) {
      case MenuKind::Handle: {
        bool exists = handleExists(w, menuH_);
        bool junc = menuH_.kind == geo::Handle::Node && junction(menuH_.node);
        ui::menuHeader(kindName(menuH_, junc));
        if (ui::menuItem("Удалить точку", {.icon = "trash", .shortcut = {Key::Delete, 0}, .danger = true, .disabled = !exists || junc}))
          removeHandle(a, menuH_);
        a.markUi("borders.menu.delete");
        break;
      }
      case MenuKind::Edge:
        ui::menuHeader("Граница");
        if (ui::menuItem("Добавить точку", {.icon = "plus"}) && menuEdge_ && w.edges.get(menuEdge_->edge)) insertAt(a, *menuEdge_);
        a.markUi("borders.menu.insert");
        break;
      case MenuKind::Province: {
        ui::menuHeader(provinceTitle(w, menuProv_));
        struct Item {
          ToolId id;
          const char* label;
        };
        static const Item items[] = {{ToolId::AddArea, "Расширить"}, {ToolId::RemoveArea, "Вырезать часть"}, {ToolId::Knife, "Разрезать ножом"},
                                     {ToolId::Merge, "Объединить с соседней"}};
        for (const Item& it : items) {
          const ToolDef* d = findTool(it.id);
          if (!d) continue;
          ui::IdScope s{int(it.id)};
          if (ui::menuItem(it.label, {.icon = d->icon, .shortcut = detail::parseShortcut(d->shortcut ? d->shortcut : "")})) {
            ToolId id = it.id;
            detail::later(a, [id](App& x) { x.setTool(id); });
          }
        }
        ui::menuSeparator();
        if (ui::menuItem("Удалить провинцию", {.icon = "trash", .danger = true})) {
          Id prov = menuProv_;
          std::string name = provinceTitle(w, prov);
          a.confirm("Удалить провинцию?", "«" + name + "» исчезнет с карты, её земли станут ничьими, сведения о ней будут удалены. Ctrl+Z вернёт.",
                    "Удалить", true, [prov](App& x) {
                      if (!x.world().province(prov)) return;
                      bool wasSel = x.ui.sel == Selection{SelType::Province, prov};
                      if (x.act("Удалить провинцию", [&](Tx& tx) { rules::deleteProvince(tx, prov); }) && wasSel) x.clearSelection();
                    });
        }
        break;
      }
    }
    ui::endMenu();
  }

  // ---- панель параметров
  void options(App& a) {
    const World& w = a.world();
    Id sel = selectedProvince(a);
    const char* empty = "Щелчок по провинции — её границы";
    bool junc = selH_ && selH_.kind == geo::Handle::Node && junction(selH_.node);
    std::string kind = selH_ ? kindName(selH_, junc) : "";
    float width = sel ? provinceTagW(w, sel) + 6 : textW(empty, ui::Font::Small) + 6;
    if (!kind.empty()) width += textW(kind, ui::Font::Strong) + 34 + 6;
    width += 30 + 6 + 18;
    OptionsBar bar(a, "tool-edit", "Границы", width);
    if (!bar) return;
    if (sel) provinceTag(w, sel);
    else ui::label(empty, {.font = ui::Font::Small, .ink = ui::Ink::Muted});
    if (!kind.empty()) {
      ui::tag(kind, junc ? ui::Tone::Info : ui::Tone::Accent, junc ? "anchor" : "tool-edit");
      a.markUi("tool.options.handle");
    }
    ui::flex();
    {
      ui::Disabled dis(a.readOnly());
      bool can = selH_ && !junc && handleExists(w, selH_);
      if (ui::iconButton("trash", "Удалить точку", {.disabled = !can})) removeHandle(a, selH_);
      ui::tooltip("Удалить точку", {Key::Delete, 0});
      a.markUi("tool.options.delete");
    }
    ui::icon("help", ui::Ink::Muted, 18,
             "■ узел — стык нескольких границ\n● точка границы\n◆ стык с берегом — скользит вдоль берега\n"
             "Двойной щелчок по границе — новая точка · стрелки — сдвиг (Shift — ×10)");
  }

  // ---- ручки
  void drawHandles(App& a, gfx::Canvas& c, const map::View& v, Id sel) {
    const World& w = a.world();
    const Palette P = palette();
    const Box2 vis = v.visibleBox().inflated(v.toMapLen(12));
    std::vector<Id> nodes;
    struct PointMark {
      geo::Handle h;
      gfx::Pt s;
      bool dense;
    };
    std::vector<PointMark> points;
    std::vector<const Edge*> coastNear;
    Id slideNode = drag_.active && drag_.slide ? drag_.h.node : (hover_.kind == geo::Handle::Node && junction(hover_.node) ? hover_.node : 0);
    w.edges.each([&](const Edge& e) {
      if (e.pl != sel && e.pr != sel) {
        if (slideNode && e.kind == EdgeKind::Coast && (e.a == slideNode || e.b == slideNode)) coastNear.push_back(&e);
        return;
      }
      nodes.push_back(e.a);
      nodes.push_back(e.b);
      if (slideNode && e.kind == EdgeKind::Coast && (e.a == slideNode || e.b == slideNode)) coastNear.push_back(&e);
      if (e.kind != EdgeKind::Border || e.pts.empty()) return;
      // Частые точки при мелком масштабе — мелкими метками.
      std::vector<Vec2> co = geo::edgeCoords(w, e);
      double len = 0;
      for (size_t i = 1; i < co.size(); i++) len += dist(co[i - 1], co[i]);
      bool dense = len * v.zoom / double(e.pts.size() + 1) < 7;
      for (int i = 0; i < int(e.pts.size()); i++) {
        Vec2 p = e.pts[size_t(i)];
        if (!vis.contains(p)) continue;
        geo::Handle h;
        h.kind = geo::Handle::Point;
        h.edge = e.id;
        h.index = i;
        points.push_back({h, v.toScreen(p), dense});
      }
    });
    std::sort(nodes.begin(), nodes.end());
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());

    // Берег, вдоль которого скользит стык.
    for (const Edge* e : coastNear) glowLine(c, toScreen(v, geo::edgeCoords(w, *e)), false, P.info, 1.8f, true);
    // Наведённая граница и место новой точки.
    if (hoverEdge_ && !hover_ && !drag_.active) {
      if (const Edge* e = w.edges.get(hoverEdge_->edge)) {
        std::vector<Vec2> co = geo::edgeCoords(w, *e);
        int s = hoverEdge_->segment;
        if (s >= 0 && s + 1 < int(co.size())) {
          bool border = e->kind == EdgeKind::Border;
          Pts seg{v.toScreen(co[size_t(s)]), v.toScreen(co[size_t(s + 1)])};
          strokeLine(c, seg, false, (border ? P.accent : P.info).alpha(0.3f), 10);
          strokeLine(c, seg, false, P.ink.alpha(0.5f), 4.6f);
          strokeLine(c, seg, false, P.light, 2.6f);
          if (border) insertGhost(c, v.toScreen(hoverEdge_->p));
        }
      }
    }
    // Перетаскивание: след от начального положения.
    if (drag_.active && drag_.moved && handleExists(w, drag_.h)) {
      gfx::Pt s0 = v.toScreen(drag_.start), s1 = v.toScreen(geo::handlePos(w, drag_.h));
      strokeLine(c, Pts{s0, s1}, false, P.light.alpha(0.8f), 3);
      strokeLine(c, Pts{s0, s1}, false, P.ink.alpha(0.75f), 1.3f, {3, 3.5f});
      c.strokeCircle(s0.x, s0.y, 4.6f, 3.2f, P.light.alpha(0.85f));
      c.strokeCircle(s0.x, s0.y, 4.6f, 1.4f, P.ink.alpha(0.75f));
    }
    // Точки.
    for (const PointMark& m : points) {
      bool hot = m.h == hover_ || m.h == selH_ || (drag_.active && m.h == drag_.h);
      if (hot) continue;
      if (m.dense) handleMark(c, m.s, Mark::Dot, 1.7f, P.light, P.ink.alpha(0.6f));
      else handleMark(c, m.s, Mark::Dot, 3.2f, P.light, P.ink.alpha(0.85f));
    }
    // Узлы и стыки.
    std::vector<std::pair<geo::Handle, gfx::Pt>> nodeMarks;
    for (Id n : nodes) {
      const Node* nd = w.nodes.get(n);
      if (!nd || nodeLocked(n) || !vis.contains(nd->p)) continue;
      geo::Handle h;
      h.kind = geo::Handle::Node;
      h.node = n;
      bool hot = h == hover_ || h == selH_ || (drag_.active && h == drag_.h);
      if (hot) continue;
      gfx::Pt s = v.toScreen(nd->p);
      if (junction(n)) handleMark(c, s, Mark::Diamond, 4.6f, P.info.lighten(0.25f), P.ink.alpha(0.9f));
      else handleMark(c, s, Mark::Square, 4.4f, P.light, P.ink.alpha(0.9f));
    }
    // Горячие ручки — поверх.
    auto hot = [&](const geo::Handle& h, bool hovered, bool selected) {
      if (!handleExists(w, h)) return;
      bool junc = h.kind == geo::Handle::Node && junction(h.node);
      Mark m = h.kind == geo::Handle::Point ? Mark::Dot : junc ? Mark::Diamond : Mark::Square;
      float r = h.kind == geo::Handle::Point ? 4.8f : 5.6f;
      gfx::Pt sp = v.toScreen(geo::handlePos(w, h));
      if (selected) {
        // Выбранная: золотая с белым ободком и свечением.
        c.fillCircle(sp.x, sp.y, r + 9, P.accent.alpha(0.22f));
        handleMark(c, sp, m, r + 2.2f, P.light, P.ink, 0);
        handleMark(c, sp, m, r, P.accent, P.ink.alpha(0.0f), 0);
      } else {
        handleMark(c, sp, m, hovered ? r + 0.8f : r, hovered ? P.accent : P.light, P.ink, hovered ? 3.f : 0.f);
      }
    };
    if (drag_.active) {
      hot(drag_.h, false, true);
      // Недопустимое положение: красная ручка у указателя и пунктир к соседним вершинам.
      if (drag_.moved && !drag_.valid) {
        gfx::Pt at = v.toScreen(drag_.at);
        if (!drag_.slide) {
          for (Vec2 nb : neighbours(w, drag_.h)) strokeLine(c, Pts{v.toScreen(nb), at}, false, P.danger, 1.6f, {5, 4});
        }
        Mark m = drag_.h.kind == geo::Handle::Point ? Mark::Dot : drag_.slide ? Mark::Diamond : Mark::Square;
        handleMark(c, at, m, 5.4f, P.danger, P.light, 3);
      }
    } else {
      if (selH_ && selH_ != hover_) hot(selH_, false, true);
      if (hover_) hot(hover_, true, hover_ == selH_);
    }
  }

  // Соседние вершины ручки (для предпросмотра недопустимого положения).
  static std::vector<Vec2> neighbours(const World& w, const geo::Handle& h) {
    std::vector<Vec2> out;
    if (h.kind == geo::Handle::Point) {
      if (const Edge* e = w.edges.get(h.edge)) {
        std::vector<Vec2> co = geo::edgeCoords(w, *e);
        size_t i = size_t(h.index) + 1;
        if (i >= 1 && i + 1 < co.size()) {
          out.push_back(co[i - 1]);
          out.push_back(co[i + 1]);
        }
      }
    } else if (h.kind == geo::Handle::Node) {
      w.edges.each([&](const Edge& e) {
        if (e.a != h.node && e.b != h.node) return;
        std::vector<Vec2> co = geo::edgeCoords(w, e);
        if (co.size() < 2) return;
        if (e.a == h.node) out.push_back(co[1]);
        if (e.b == h.node) out.push_back(co[co.size() - 2]);
      });
    }
    return out;
  }
};

ToolDef bordersDef() {
  return {ToolId::EditBorders, "tool-edit", "Правка границ провинции", "B", true, [] { return std::make_unique<BordersTool>(); }, 10};
}
ToolReg regBorders(bordersDef());

}  // namespace

void registerBordersTool() { ToolReg r(bordersDef()); }

}  // namespace rg::app::tools
