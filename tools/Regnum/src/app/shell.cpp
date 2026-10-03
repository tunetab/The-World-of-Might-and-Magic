// Regnum — оболочка основного экрана: карта на всё окно, верхняя панель, лента разделов и выдвижная панель,
// плавающая панель инструментов карты, инспектор справа, строка состояния, легенда, масштаб и мини-карта,
// уведомления, полноэкранные редакторы, диалоги, глобальные сочетания, ввод мышью над картой.
#include "app/app_internal.h"
#include "app/logo.h"
#include "base/fs.h"
#include "gfx/text.h"

namespace rg::app::detail {

using platform::Key;
namespace {

constexpr float kM = 12;        // поля окна
constexpr float kTopH = 52;     // высота верхней панели
constexpr float kRailW = 44;    // лента разделов и панель инструментов
constexpr float kBtn = 30;      // кнопка-значок
constexpr float kMiniW = 232;   // мини-карта

App::Impl& D(App& a) { return a.impl(); }

// Кнопка команды реестра: значок, подсказка с сочетанием, доступность.
bool commandButton(App& a, const char* cmd, const char* icon = nullptr, bool toggled = false, std::string_view tip = {}) {
  const CommandDef* c = findCommand(cmd);
  if (!c) return false;
  bool en = !c->enabled || c->enabled(a);
  std::string t = tip.empty() ? std::string(c->title) : std::string(tip);
  bool clicked = ui::iconButton(icon ? icon : c->icon, t, {.toggled = toggled, .disabled = !en});
  if (c->shortcut && *c->shortcut) {
    std::string_view sc = c->shortcut;
    if (sc == "+" || sc == "-") ui::tooltip(t + " · " + std::string(sc));   // «=» на клавише «+» понятнее как «+»
    else ui::tooltip(t, parseShortcut(sc));
  }
  if (clicked) runCommand(a, cmd);
  return clicked;
}

// Левый край вертикальной разделительной линии (в вертикальном потоке).
void vsep(float w) {
  RectF r = ui::next(w, 9);
  const ui::Theme& t = ui::theme();
  ui::draw::line(r.x + 3, r.cy(), r.right() - 3, r.cy(), t.border, 1);
}

// ---------------------------------------------------------------- верхняя панель
void topBar(App& a, RectF r) {
  App::Impl& d = D(a);
  const ui::Theme& th = ui::theme();
  ui::Panel bar("topbar", r, {.pad = 10, .radius = 12});
  a.markUi("topbar", r);
  ui::HStack hs(32, ui::Align::Left, 6);
  // Знак и меню мира
  {
    RectF lr = ui::next(30, 30);
    ui::custom(lr, [](gfx::Canvas& c, RectF dev, float) { drawLogoMark(c, dev.inset(dev.w * 0.08f), dev.w < 40); });
  }
  bool dirty = a.dirty();
  bool compact = r.w < 1240;   // узкое окно: короче название, состояние — только точкой, без кнопки справки
  std::string name = gfx::ellipsize(a.worldTitle(), ui::textStyle(ui::Font::Strong), compact ? 130.f : 260.f);
  if (ui::button(name + "##world", {.variant = ui::Variant::Ghost, .iconRight = "chevron-down", .tooltip = "Мир: файл, сохранение, папка проекта"}))
    ui::openPopup("worldmenu");
  a.markUi("topbar.world");
  if (ui::beginMenu("worldmenu")) {
    ui::menuHeader(a.projectPath().empty() ? std::string("Мир не сохранён") : fs::filename(a.projectPath()));
    auto item = [&](const char* cmd) {
      const CommandDef* c = findCommand(cmd);
      if (!c) return;
      bool en = !c->enabled || c->enabled(a);
      if (ui::menuItem(c->title, {.icon = c->icon, .shortcut = parseShortcut(c->shortcut ? c->shortcut : ""), .disabled = !en})) later(a, [id = std::string(cmd)](App& x) { runCommand(x, id); });
    };
    item("file.new");
    item("file.open");
    item("file.openBundle");
    ui::menuSeparator();
    item("file.save");
    item("file.saveAs");
    item("file.reveal");
    ui::menuSeparator();
    item("turn.history");
    item("app.settings");
    ui::menuSeparator();
    item("file.close");
    ui::endMenu();
  }
  // Состояние сохранения: точка и короткая подпись.
  {
    std::string status;
    ui::Tone tone;
    if (a.readOnly()) {
      status = "только просмотр";
      tone = ui::Tone::Info;
    } else if (dirty) {
      status = a.projectPath().empty() ? "не сохранён" : "изменён";
      tone = ui::Tone::Warning;
    } else {
      std::string at = !d.autosavedAt.empty() && d.autosavedAt >= d.savedAt && !a.projectPath().empty() ? d.autosavedAt : d.savedAt;
      status = at.empty() || compact ? std::string("сохранён") : "сохранён " + localTime(at);
      tone = ui::Tone::Success;
    }
    std::string tip = a.projectPath().empty() ? std::string("Мир ещё не записан на диск — Ctrl+S")
                                              : (dirty ? "Есть несохранённые изменения — Ctrl+S" : "Все изменения записаны") +
                                                    std::string("\n") + a.projectPath();
    if (compact) {
      std::string full = "Мир " + status + "\n" + tip;
      ui::label("●", {.font = ui::Font::Small, .color = ui::toneColor(tone), .tooltip = full});
    } else {
      RectF dot = ui::next(8, 30);
      ui::draw::circle(dot.cx(), dot.cy(), 3.5f, ui::toneColor(tone));
      ui::label(status, {.font = ui::Font::Small, .ink = ui::Ink::Muted, .tooltip = tip});
    }
    a.markUi("topbar.status");
  }
  ui::separatorV();
  // Режимы карты 1–9
  {
    RectF track = ui::next(9 * kBtn + 8 * 2 + 6, 32);
    ui::draw::rect(track, th.dark ? th.bg.alpha(0.55f) : th.surface3.alpha(0.8f), 9);
    ui::Area ar(track.inset(3, 1), 0);
    ui::HStack modes(30, ui::Align::Left, 2);
    for (int i = 0; i < int(schema::MapMode::Count); i++) {
      const schema::EnumInfo& mi = schema::kMapModes[i];
      ui::IdScope s(i);
      std::string tip = mi.name;
      if (ui::iconButton(mi.icon, tip, {.toggled = int(a.ui.mapMode) == i})) a.setMapMode(schema::MapMode(i));
      ui::tooltip(tip, {Key(int(Key::D1) + i), 0});
      a.markUi("mode." + std::to_string(i + 1));
    }
  }
  ui::flex();
  commandButton(a, "edit.undo", "undo", false, a.store.canUndo() ? "Отменить: " + a.store.undoLabel() : std::string("Отменить"));
  a.markUi("topbar.undo");
  commandButton(a, "edit.redo", "redo", false, a.store.canRedo() ? "Повторить: " + a.store.redoLabel() : std::string("Повторить"));
  a.markUi("topbar.redo");
  ui::separatorV();
  commandButton(a, "app.palette", "search");
  a.markUi("topbar.palette");
  commandButton(a, "app.settings", "settings");
  a.markUi("topbar.settings");
  if (!compact) commandButton(a, "app.help", "keyboard");
  ui::separatorV();
  // Ход
  {
    std::string turn = "Ход " + std::to_string(a.store.world().turn());
    if (ui::button(turn, {.variant = ui::Variant::Ghost, .icon = "hourglass", .tooltip = "История ходов"})) a.openDialog("turn.history");
    a.markUi("topbar.turn");
  }
  {
    ui::Disabled dis(a.readOnly());
    if (ui::button("Завершить ход", {.variant = ui::Variant::Primary, .icon = "next-turn"})) a.endTurn();
    ui::tooltip("Завершить ход: доходы, расходы, стройки, исследования", parseShortcut("Ctrl+Enter"));
    a.markUi("topbar.endturn");
  }
}

// ---------------------------------------------------------------- баннер просмотра прошлого хода
void readOnlyBanner(App& a, RectF r) {
  ui::Panel p("viewturn", r, {.pad = 8, .radius = 12, .glass = true});
  a.markUi("banner", r);
  ui::HStack hs(28, ui::Align::Left, 8);
  ui::icon("history", ui::Ink::Info, 18);
  ui::label("Ход " + std::to_string(*a.ui.viewTurn) + " · только просмотр", {.font = ui::Font::Strong});
  ui::flex();
  if (ui::button("Вернуться к текущему ходу", {.variant = ui::Variant::Primary, .icon = "arrow-right", .size = ui::Size::Small})) a.backToCurrent();
  a.markUi("banner.back");
}

// ---------------------------------------------------------------- лента разделов и выдвижная панель
float railHeight(int n) { return 2 * 7 + n * kBtn + std::max(0, n - 1) * 4; }

void rail(App& a, RectF r) {
  ui::Panel p("rail", r, {.pad = 7, .radius = 12});
  a.markUi("rail", r);
  ui::gap(4);
  for (auto& dr : drawers()) {
    ui::IdScope s(dr.id);
    std::string tip = dr.title;
    if (ui::iconButton(dr.icon, tip, {.toggled = a.ui.drawer == dr.id})) a.openDrawer(dr.id);
    if (dr.shortcut) ui::tooltip(tip, parseShortcut(dr.shortcut));
    a.markUi(std::string("drawer.") + dr.id);
  }
}

// Ручка изменения ширины панели (край внутри панели). Возвращает true, пока тянут.
bool resizeHandle(const char* id, RectF r, float& width, float minW, float maxW, bool leftEdge) {
  struct S {
    float start = 0;
  };
  ui::WidgetId wid = ui::id(id);
  auto& st = ui::state<S>(wid);
  ui::Interaction it = ui::interact(wid, r);
  if (it.pressed) st.start = width;
  if (it.hovered || it.held) {
    ui::setCursor(platform::Cursor::ResizeH);
    const ui::Theme& th = ui::theme();
    float k = ui::animate(wid ^ 7, 1.f, 0.12f);
    ui::draw::rect(RectF{leftEdge ? r.x + 1 : r.right() - 3, r.y + r.h * 0.5f - 18, 2, 36}, th.accent.alpha(0.7f * k), 1);
  }
  if (it.held && it.dragging) {
    width = clamp(st.start + (leftEdge ? -it.dx : it.dx), minW, maxW);
    return true;
  }
  return it.held;
}

void drawerPanel(App& a, const DrawerDef& dr, RectF r, float maxW) {
  ui::Panel p("drawer", r, {.pad = 14});
  a.markUi("drawer", r);
  if (resizeHandle("##drawer-resize", RectF{r.right() - 6, r.y + 12, 6, r.h - 24}, a.ui.drawerWidth, 260, maxW, false)) D(a).prefsDirty = true;
  {
    ui::Row head({ui::px(22), ui::fr(1), ui::px(kBtn)}, kBtn, 8);
    ui::icon(dr.icon, ui::Ink::Accent, 18);
    ui::label(dr.title, {.font = ui::Font::Title});
    if (ui::iconButton("close", "Закрыть панель")) a.openDrawer(dr.id);
  }
  ui::spacer(2);
  ui::Scroll sc("body");
  ui::IdScope s(dr.id);
  try {
    dr.draw(a);
  } catch (const std::exception& e) {
    a.error(e);
  }
}

// ---------------------------------------------------------------- панель инструментов карты
struct ToolLayout {
  std::vector<const ToolDef*> base, edit, extra;
};
ToolLayout toolLayout(App& a) {
  ToolLayout L;
  for (auto& t : toolDefs()) {
    if (t.id == ToolId::Select || t.id == ToolId::Pan) L.base.push_back(&t);
    else if (t.editMode) {
      if (a.ui.editBorders) L.edit.push_back(&t);
    } else {
      L.extra.push_back(&t);
    }
  }
  return L;
}
float toolbarHeight(const ToolLayout& L) {
  int items = int(L.base.size()) + 1 + int(L.edit.size()) + int(L.extra.size());
  int seps = 1 + (L.extra.empty() ? 0 : 1);
  float h = 2 * 7 + items * kBtn + seps * 9 + std::max(0, items + seps - 1) * 4;
  return h;
}

void toolButton(App& a, const ToolDef& t) {
  ui::IdScope s{int(t.id)};
  std::string tip = t.title;
  if (ui::iconButton(t.icon, tip, {.toggled = a.ui.tool == t.id})) a.setTool(t.id);
  if (t.shortcut && *t.shortcut) ui::tooltip(tip, parseShortcut(t.shortcut));
  static const char* names[] = {"select", "pan", "borders-tool", "new-province", "add-area", "remove-area", "fill", "knife", "merge", "delete", "army", "fleet", "route"};
  a.markUi(std::string("tool.") + (int(t.id) < int(std::size(names)) ? names[int(t.id)] : "other"));
}

void toolbar(App& a, RectF r, const ToolLayout& L) {
  const ui::Theme& th = ui::theme();
  ui::Panel p("maptools", r, {.pad = 7, .radius = 12});
  a.markUi("toolbar", r);
  ui::gap(4);
  for (auto* t : L.base) toolButton(a, *t);
  vsep(kBtn);
  // «Галочка» правки границ (ТЗ 1.a.ii): закрытый замок — границы закреплены, открытый — правка. На прошлом ходу
  // (только просмотр) недоступна.
  {
    bool on = a.ui.editBorders;
    bool ro = a.readOnly();
    std::string tip = ro   ? "Прошлый ход: только просмотр — правка границ недоступна"
                      : on ? "Режим правки границ включён — нажмите, чтобы закрепить границы"
                           : "Режим правки границ выключен: границы и области закреплены — нажмите, чтобы править";
    if (ui::iconButton(bordersToggleIcon(on), tip, {.toggled = on, .disabled = ro})) a.setEditBorders(!on);
    ui::tooltip(tip, ro ? ui::Shortcut{} : parseShortcut("E"));
    RectF br = ui::lastItem().rect;
    a.markUi("tool.borders");
    // Флажок в углу кнопки.
    RectF cb{br.right() - 11, br.bottom() - 11, 10, 10};
    ui::draw::rect(cb.expand(1.5f), th.surface1, 4);
    if (on) {
      ui::draw::rect(cb, th.accent, 3);
      ui::draw::icon("check", cb.inset(0.5f), th.onAccent);
    } else {
      ui::draw::rectStroke(cb, ro ? th.border : th.borderStrong, 3, 1.2f);
    }
  }
  for (auto* t : L.edit) toolButton(a, *t);
  if (!L.extra.empty()) {
    vsep(kBtn);
    for (auto* t : L.extra) toolButton(a, *t);
  }
}

// ---------------------------------------------------------------- инспектор
void inspector(App& a, RectF r, float maxW) {
  const World& w = a.world();
  Selection sel = a.ui.sel;
  ui::Panel p("inspector", r, {.pad = 16});
  a.markUi("inspector", r);
  if (resizeHandle("##insp-resize", RectF{r.x, r.y + 12, 6, r.h - 24}, a.ui.inspectorWidth, 320, maxW, true)) D(a).prefsDirty = true;
  ui::IdScope scope{int(sel.type)};
  // Шапка
  {
    ui::Row head({ui::fr(1), ui::px(kBtn), ui::px(kBtn)}, ui::kAuto, 4);
    {
      ui::Group g(0, 4);
      bool any = false;
      for (auto& h : headers()) {
        if (h.type != sel.type || !h.draw) continue;
        any = true;
        ui::IdScope hs(h.id);
        try {
          h.draw(a, sel.id);
        } catch (const std::exception& e) {
          a.error(e);
        }
      }
      if (!any) {
        std::string cap = selCaption(sel.type);
        if (sel.type == SelType::Faction)
          if (const Faction* f = w.faction(sel.id)) cap = f->isGuild() ? "Торговая гильдия" : "Государство";
        ui::caption(cap);
        ui::label(entityName(w, sel), {.font = ui::Font::Heading});
      }
    }
    if (ui::iconButton("target", "Показать на карте")) a.focusSelection();
    ui::tooltip("Показать на карте", parseShortcut("F"));
    a.markUi("inspector.focus");
    if (ui::iconButton("close", "Закрыть")) a.clearSelection();
    ui::tooltip("Закрыть", {Key::Escape, 0});
    a.markUi("inspector.close");
  }
  // Вкладки
  std::vector<const TabDef*> list;
  for (auto& t : tabs()) {
    if (t.type != sel.type || !t.draw) continue;
    bool vis = true;
    if (t.visible) {
      try {
        vis = t.visible(a, sel.id);
      } catch (const std::exception& e) {
        a.error(e);
        vis = false;
      }
    }
    if (vis) list.push_back(&t);
  }
  if (list.empty()) {
    ui::spacer(8);
    ui::emptyState(selIcon(sel.type), "Подробности появятся здесь.");
    return;
  }
  std::string& cur = a.ui.tabOf[sel.type];
  int idx = 0;
  for (size_t i = 0; i < list.size(); i++)
    if (cur == list[i]->id) idx = int(i);
  if (list.size() > 1) {
    std::vector<ui::Tab> items;
    items.reserve(list.size());
    for (auto* t : list) {
      int badge = 0;
      if (t->badge) {
        try {
          badge = t->badge(a, sel.id);
        } catch (...) {
          badge = 0;
        }
      }
      items.push_back(ui::Tab{t->icon, {}, t->title, badge});
    }
    ui::tabs("tabs", idx, std::span<const ui::Tab>(items), {.fill = true});
    a.markUi("inspector.tabs");
    ui::spacer(2);
  }
  idx = clamp(idx, 0, int(list.size()) - 1);
  cur = list[size_t(idx)]->id;
  // Своя прокрутка у каждой вкладки: при переключении не оказываемся в середине чужой страницы.
  const std::string scrollId = std::string("body.") + list[size_t(idx)]->id;
  ui::Scroll sc(scrollId);
  ui::IdScope ts(list[size_t(idx)]->id);
  try {
    list[size_t(idx)]->draw(a, sel.id);
  } catch (const std::exception& e) {
    a.error(e);
  }
}

// ---------------------------------------------------------------- строка состояния и легенда
void statusBar(App& a, float x, float y, float maxW) {
  const World& w = a.world();
  const ui::Theme& th = ui::theme();
  std::string coords = a.ui.cursorMap ? fmtInt(i64(std::lround(a.ui.cursorMap->x))) + " · " + fmtInt(i64(std::lround(a.ui.cursorMap->y))) : std::string("—");
  const char* hint = nullptr;
  if (MapTool* t = a.activeTool()) hint = t->hint(a);
  if (!hint) hint = a.ui.editBorders ? "Щелчок по провинции — правка её границ" : "Щелчок — сведения · колесо — масштаб";
  const float pad = 12, gap = 10, cw = 92;
  float tw = ui::measure(hint, ui::Font::Small) + 2;
  float fixed = pad + 20 + cw + 2 * gap + 1 + 2 * gap + 1 + tw + pad;
  float hw = clamp(maxW - fixed, 150.f, 230.f);   // объект под указателем уступает место подсказке
  float W = std::min(maxW, fixed + hw);
  RectF r{x, y, std::round(W), 30};
  ui::Panel p("status", r, {.pad = 0, .radius = 10, .glass = true});
  a.markUi("status", r);
  float cx = r.x + pad;
  ui::draw::icon("crosshair", RectF{cx, r.cy() - 7, 14, 14}, th.textMuted);
  cx += 20;
  ui::draw::text(coords, RectF{cx, r.y, cw, r.h}, ui::Font::Mono, th.textDim);
  cx += cw + gap;
  auto sep = [&] {
    ui::draw::line(cx, r.y + 8, cx, r.bottom() - 8, th.border, 1);
    cx += 1 + gap;
  };
  sep();
  // Объект под указателем (точка цвета владельца, название, владелец).
  {
    float right = std::min(r.right() - pad, cx + hw);
    Selection h = a.ui.hover;
    if (h && right - cx > 40) {
      std::string name = entityName(w, h), sub;
      if (h.type == SelType::Province) {
        const Province* pr = w.province(h.id);
        const Faction* own = pr ? w.faction(pr->owner) : nullptr;
        if (own) ui::draw::circle(cx + 4, r.cy(), 4.f, own->color);
        else ui::draw::ring(cx + 4, r.cy(), 3.5f, 1.2f, th.textMuted);
        if (own) sub = own->name;
        else if (pr && !pr->sea) sub = "без владельца";
      } else {
        ui::draw::icon(selIcon(h.type), RectF{cx - 2, r.cy() - 7, 14, 14}, th.textDim);
      }
      float nx = cx + 16;
      float nw = std::min(ui::measure(name, ui::Font::Small) + 2, right - nx);
      ui::draw::text(name, RectF{nx, r.y, nw, r.h}, ui::Font::Small, th.text);
      if (!sub.empty() && right - (nx + nw + 6) > 30) ui::draw::text(sub, RectF{nx + nw + 6, r.y, right - (nx + nw + 6), r.h}, ui::Font::Small, th.textMuted);
    } else {
      ui::draw::text("—", RectF{cx, r.y, 20, r.h}, ui::Font::Small, th.textMuted);
    }
    cx = right + gap;
  }
  if (r.right() - pad - cx > 40) {
    sep();
    ui::draw::text(hint, RectF{cx, r.y, r.right() - pad - cx, r.h}, ui::Font::Small, th.textMuted);
  }
}

float legendHeight(int rows) { return 12 + 24 + 4 + rows * 22 + 10; }

void legend(App& a, RectF r, const std::vector<map::LegendItem>& items, int shown) {
  ui::Panel p("legend", r, {.pad = 12, .radius = 12, .glass = true});
  a.markUi("legend", r);
  {
    ui::Row head({ui::fr(1), ui::px(24)}, 24, 4);
    ui::caption(schema::kMapModes[int(a.ui.mapMode)].name);
    bool hide = ui::iconButton("chevron-down", "Скрыть легенду", {.size = ui::Size::Small});
    ui::tooltip("Скрыть легенду", parseShortcut("L"));
    if (hide) {
      a.ui.showLegend = false;
      D(a).prefsDirty = true;
    }
  }
  ui::spacer(2);
  ui::gap(0);
  for (int i = 0; i < shown; i++) {
    const map::LegendItem& it = items[size_t(i)];
    ui::IdScope s(i);
    ui::Row row({ui::px(16), ui::fr(1)}, 22, 8);
    RectF sw = ui::next(16, 22);
    if (!it.icon.empty()) ui::draw::icon(it.icon, RectF{sw.x, sw.cy() - 8, 16, 16}, it.color);
    else {
      ui::draw::rect(RectF{sw.x, sw.cy() - 5, 16, 10}, it.color, 3);
      ui::draw::rectStroke(RectF{sw.x, sw.cy() - 5, 16, 10}, Color(0, 0, 0, 40), 3, 1);
    }
    ui::label(it.label, {.font = ui::Font::Small});
  }
  if (int(items.size()) > shown) ui::label("ещё " + std::to_string(items.size() - size_t(shown)), {.font = ui::Font::Small, .ink = ui::Ink::Muted});
}

// ---------------------------------------------------------------- масштаб и мини-карта
void zoomColumn(App& a, RectF r) {
  ui::Panel p("zoom", r, {.pad = 7, .radius = 12});
  a.markUi("zoom", r);
  ui::gap(4);
  commandButton(a, "map.zoomIn");
  a.markUi("zoom.in");
  commandButton(a, "map.zoomOut");
  a.markUi("zoom.out");
  commandButton(a, "map.fit");
  a.markUi("zoom.fit");
  vsep(kBtn);
  commandButton(a, "map.minimap", nullptr, a.ui.showMinimap);
  a.markUi("zoom.minimap");
  commandButton(a, "map.legend", nullptr, a.ui.showLegend);
  a.markUi("zoom.legend");
}
float zoomColumnHeight() { return 2 * 7 + 5 * kBtn + 9 + 5 * 4; }

void minimap(App& a, RectF r) {
  App::Impl& d = D(a);
  ui::Panel p("minimap", r, {.pad = 5, .radius = 12, .glass = true});
  a.markUi("minimap", r);
  RectF in = r.inset(5);
  ui::WidgetId wid = ui::id("##minimap");
  ui::Interaction it = ui::interact(wid, in);
  map::MapView* mv = d.map.get();
  float dpi = d.dpi;
  ui::custom(in, [mv, dpi](gfx::Canvas& c, RectF dev, float) {
    c.save();
    c.clipRoundRect(dev, 8 * dpi);
    mv->renderMinimap(c, RectF{dev.x / dpi, dev.y / dpi, dev.w / dpi, dev.h / dpi}, dpi);
    c.restore();
  });
  ui::draw::rectStroke(in, ui::theme().border, 8, 1);
  if (it.hovered) ui::setCursor(platform::Cursor::Hand);
  if (it.held || it.clicked) {
    float s = ui::uiScale();
    RectF lr{in.x * s, in.y * s, in.w * s, in.h * s};
    Vec2 m = mv->minimapToMap(lr, it.mx * s, it.my * s);
    mv->centerOn(m, 0, !it.dragging);
    a.requestRedraw();
  }
  ui::tooltip("Мини-карта: щелчок — перейти", parseShortcut("M"));
}

// ---------------------------------------------------------------- полноэкранный редактор
void editorHost(App& a, const EditorDef& ed, RectF V) {
  RectF r{kM, kM, V.w - 2 * kM, V.h - 2 * kM};
  ui::Panel p("editor", r, {.pad = 0});
  a.markUi("editor", r);
  RectF head{r.x + 12, r.y + 10, r.w - 24, 34};
  {
    ui::Area ar(head, 0);
    ui::HStack hs(34, ui::Align::Left, 8);
    if (ui::iconButton("arrow-left", "Назад к карте")) later(a, [](App& x) { x.closeEditor(); });
    ui::tooltip("Назад к карте", {Key::Escape, 0});
    a.markUi("editor.back");
    if (ed.icon) ui::icon(ed.icon, ui::Ink::Accent, 20);
    ui::label(ed.title, {.font = ui::Font::Title});
    ui::flex();
    commandButton(a, "edit.undo", "undo");
    commandButton(a, "edit.redo", "redo");
  }
  ui::draw::line(r.x, head.bottom() + 10, r.right(), head.bottom() + 10, ui::theme().border, 1);
  ui::Area body(RectF{r.x, head.bottom() + 11, r.w, r.bottom() - head.bottom() - 11}, 16);
  ui::IdScope s(ed.id);
  try {
    ed.draw(a, a.ui.editorArg);
  } catch (const std::exception& e) {
    a.error(e);
  }
}

const char* toastIconOf(ToastKind k) {
  switch (k) {
    case ToastKind::Success: return "check-circle";
    case ToastKind::Warning: return "warning";
    case ToastKind::Danger: return "error";
    default: return "info";
  }
}
ui::Tone toneOf(ToastKind k) {
  switch (k) {
    case ToastKind::Success: return ui::Tone::Success;
    case ToastKind::Warning: return ui::Tone::Warning;
    case ToastKind::Danger: return ui::Tone::Danger;
    default: return ui::Tone::Info;
  }
}

}  // namespace

// ================================================================ уведомления
void drawToasts(App& a, RectF area) {
  auto& list = a.toasts();
  if (list.empty()) return;
  const ui::Theme& th = ui::theme();
  const float W = 340;
  double now = a.time();
  float dt = ui::dt();
  float y = area.bottom();
  gfx::TextStyle st = ui::textStyle(ui::Font::Body);
  for (size_t i = list.size(); i-- > 0;) {
    Toast& t = list[i];
    bool hasAction = !t.actionLabel.empty();
    float textW = W - 50 - 36 - (hasAction ? ui::measure(t.actionLabel, ui::Font::Strong) + 30 : 0);
    gfx::TextLayout L = gfx::layoutText(t.text, st, textW, 4, true);
    float h = std::max(48.f, std::ceil(L.height) + 28);
    float tin = clamp(float((now - t.start) / 0.22), 0.f, 1.f);
    float tout = t.closeAt >= 0 ? clamp(float((now - t.closeAt) / 0.18), 0.f, 1.f) : 0.f;
    float ein = 1 - (1 - tin) * (1 - tin);
    float ty = ui::animate(ui::id(i64(t.id) * 7 + 1), y - h, 0.18f);
    RectF r{std::round(area.right() - W + 40 * (1 - ein) + 60 * tout), std::round(ty), W, h};
    {
      ui::Panel p("toast#" + std::to_string(t.id), r, {.pad = 0, .radius = 10});
      a.markUi("toast." + std::to_string(i), r);
      const ui::Mouse& m = ui::mouse();
      t.hovered = r.contains(m.x, m.y);
      if (t.hovered && t.closeAt < 0) t.paused += dt;
      Color tc = ui::toneColor(toneOf(t.kind));
      RectF ic{r.x + 14, r.y + std::round((h - 22) * 0.5f), 22, 22};
      ui::draw::rect(ic.expand(3), tc.alpha(0.16f), 8);
      ui::draw::icon(t.icon.empty() ? toastIconOf(t.kind) : t.icon, ic.inset(1), tc);
      {
        ui::Area ta(RectF{r.x + 50, r.y + std::round((h - L.height) * 0.5f), textW + 1, std::ceil(L.height) + 2}, 0);
        ui::text(t.text, ui::Font::Body, ui::Ink::Normal);
      }
      if (hasAction) {
        float bw = ui::measure(t.actionLabel, ui::Font::Strong) + 22;
        ui::at(RectF{r.right() - 34 - bw, r.y + std::round((h - 24) * 0.5f), bw, 24});
        if (ui::button(t.actionLabel, {.variant = ui::Variant::Secondary, .size = ui::Size::Small})) {
          auto fn = t.action;
          later(a, [fn](App& x) {
            if (fn) fn(x);
          });
          t.closeAt = now;
        }
        a.markUi("toast." + std::to_string(i) + ".action");
      }
      RectF xr{r.right() - 30, r.y + std::round((h - 22) * 0.5f), 22, 22};
      ui::WidgetId xid = ui::id("##x");
      ui::Interaction xi = ui::interact(xid, xr);
      float xh = ui::animate(xid ^ 3, xi.hovered ? 1.f : 0.f);
      if (xh > 0.01f) ui::draw::rect(xr, th.hover.alpha(xh * 1.5f), 6);
      ui::draw::icon("close", xr.inset(4), Color::mix(th.textMuted, th.text, xh));
      if (xi.clicked && t.closeAt < 0) t.closeAt = now;
      double left = t.duration - (now - t.start - t.paused);
      if (t.closeAt < 0) {
        float k = clamp(float(left / std::max(0.1, t.duration)), 0.f, 1.f);
        ui::draw::rect(RectF{r.x + 10, r.bottom() - 3, (r.w - 20) * k, 2}, tc.alpha(0.55f), 1);
        if (left <= 0) t.closeAt = now;
      }
    }
    y -= (h + 8) * (1 - tout);
  }
  while (!list.empty() && list.front().closeAt >= 0 && now - list.front().closeAt > 0.2) list.pop_front();
  for (auto it = list.begin(); it != list.end();) {
    if (it->closeAt >= 0 && now - it->closeAt > 0.2) it = list.erase(it);
    else ++it;
  }
  ui::requestRedraw();
}

// ================================================================ основной экран
void drawEditorScreen(App& a) {
  App::Impl& d = D(a);
  RectF V = ui::viewport();
  float s = ui::uiScale();
  if (!a.ui.editor.empty()) {
    if (const EditorDef* ed = findEditor(a.ui.editor)) {
      editorHost(a, *ed, V);
      drawToasts(a, RectF{kM, kM, V.w - 2 * kM - 8, V.h - 2 * kM - 8});
      return;
    }
  }
  float top = kM + kTopH + kM;
  topBar(a, RectF{kM, kM, V.w - 2 * kM, kTopH});

  // Левая часть: лента разделов, выдвижная панель, инструменты.
  float left = kM;
  float maxSide = std::max(320.f, V.w * 0.42f);
  if (!drawers().empty()) {
    rail(a, RectF{left, top, kRailW, railHeight(int(drawers().size()))});
    left += kRailW + kM;
  }
  if (!a.ui.drawer.empty()) {
    const DrawerDef* dr = nullptr;
    for (auto& x : drawers())
      if (a.ui.drawer == x.id) dr = &x;
    if (dr) {
      float dw = clamp(a.ui.drawerWidth, 260.f, std::min(640.f, maxSide));
      drawerPanel(a, *dr, RectF{left, top, dw, V.h - top - kM}, std::min(640.f, maxSide));
      left += dw + kM;
    } else {
      a.ui.drawer.clear();
    }
  }
  ToolLayout L = toolLayout(a);
  toolbar(a, RectF{left, top, kRailW, toolbarHeight(L)}, L);
  float mapLeft = left + kRailW + kM;

  // Правая часть: инспектор.
  float right = V.w - kM;
  if (a.ui.sel) {
    float iw = clamp(a.ui.inspectorWidth, 320.f, std::min(720.f, maxSide));
    inspector(a, RectF{right - iw, top, iw, V.h - top - kM}, std::min(720.f, maxSide));
    right -= iw + kM;
  }
  RectF area{mapLeft, top, std::max(0.f, right - mapLeft), V.h - top - kM};
  d.rectsNext["map.area"] = RectF{area.x * s, area.y * s, area.w * s, area.h * s};

  if (a.readOnly()) {
    float bw = std::min(560.f, std::max(360.f, area.w - 24));
    readOnlyBanner(a, RectF{std::round(area.cx() - bw * 0.5f), top, bw, 44});
  }

  // Низ слева: строка состояния и легенда.
  float statusW = std::min(area.w - (a.ui.showMinimap ? kMiniW + 10 + 8 : 0) - kRailW - 16, 720.f);
  if (statusW > 200) statusBar(a, area.x, V.h - kM - 30, statusW);
  float bottomY = V.h - kM - 30 - 8;
  if (a.ui.showLegend && area.w >= 248 + kRailW + 24) {
    std::vector<map::LegendItem> items = d.map->legend(a.world(), a.ui.mapMode);
    if (!items.empty()) {
      int maxRows = int((bottomY - top - 120) / 22);
      int shown = std::min<int>(int(items.size()), std::clamp(maxRows, 1, 10));
      float lh = legendHeight(shown + (int(items.size()) > shown ? 1 : 0));
      if (lh < bottomY - top - 40) legend(a, RectF{area.x, bottomY - lh, 248, lh}, items, shown);
    }
  }

  // Низ справа: масштаб и мини-карта.
  float zh = zoomColumnHeight();
  RectF zr{area.right() - kRailW, V.h - kM - zh, kRailW, zh};
  zoomColumn(a, zr);
  float stackTop = zr.y;
  if (a.ui.showMinimap && area.w > kMiniW + kRailW + 40) {
    float mh = std::round(kMiniW * float(schema::kMapHeight / schema::kMapWidth)) + 10;
    RectF mr{zr.x - 8 - kMiniW - 10, V.h - kM - mh, kMiniW + 10, mh};
    minimap(a, mr);
    stackTop = std::min(stackTop, mr.y);
  }
  drawToasts(a, RectF{area.x, top, area.w, stackTop - 10 - top});
}

// ================================================================ диалоги
void drawDialogs(App& a) {
  auto& st = a.dialogStack();
  std::vector<Dialog*> closed;
  size_t n = st.size();
  for (size_t i = 0; i < n && i < st.size(); i++) {
    Dialog* dlg = st[i].get();
    Dialog::Style sty;
    try {
      sty = dlg->style(a);
    } catch (const std::exception& e) {
      a.error(e);
      closed.push_back(dlg);
      continue;
    }
    bool open = true;
    ui::ModalOpt o;
    o.title = sty.title;
    o.icon = sty.icon;
    o.tone = sty.tone;
    o.width = sty.width;
    o.closeButton = sty.closeButton;
    o.dismissOnBackdrop = sty.dismissOnBackdrop;
    bool keep = true;
    if (ui::beginModal(dlg->id(), o, &open)) {
      try {
        keep = dlg->draw(a);
      } catch (const std::exception& e) {
        a.error(e);
        keep = false;
      }
      ui::endModal();
    }
    if (!open) {
      try {
        dlg->dismissed(a);
      } catch (const std::exception& e) {
        a.error(e);
      }
      closed.push_back(dlg);
    } else if (!keep) {
      closed.push_back(dlg);
    }
  }
  if (!closed.empty())
    st.erase(std::remove_if(st.begin(), st.end(), [&](auto& p) { return std::find(closed.begin(), closed.end(), p.get()) != closed.end(); }), st.end());
}

// ================================================================ клавиши
void globalKeys(App& a) {
  App::Impl& d = D(a);
  bool editorScreen = a.ui.screen == Screen::Editor;
  bool onMap = editorScreen && a.ui.editor.empty() && !a.hasDialog() && !ui::anyModalOpen();
  // 1. Инструмент карты — первым (Esc отменяет построение, Enter завершает, Delete удаляет точку).
  if (onMap && !ui::wantsKeyboard()) {
    if (MapTool* t = a.activeTool()) {
      static const u32 modsets[] = {0, platform::ModShift, platform::ModCtrl, platform::ModCtrl | platform::ModShift, platform::ModAlt,
                                    platform::ModSuper, platform::ModSuper | platform::ModShift};
      for (int k = 1; k < int(Key::Count); k++) {
        Key key = Key(k);
        for (u32 m : modsets) {
          if (!ui::keyPressed(key, m)) continue;
          platform::Event e;
          e.type = platform::EventType::KeyDown;
          e.key = key;
          e.mods = m;
          bool used = false;
          try {
            used = t->key(a, e);
          } catch (const std::exception& ex) {
            a.error(ex);
            used = true;
          }
          if (used) ui::consumeKey(key);
          if (a.activeTool() != t) break;   // инструмент сменился — остальное разберут сочетания
        }
        if (a.activeTool() != t) break;
      }
    }
  }
  // 2. Команды реестра.
  for (auto& c : commands()) {
    if (!c.shortcut || !*c.shortcut || !c.run) continue;
    ui::Shortcut sc = parseShortcut(c.shortcut);
    if (!sc) continue;
    bool hit = false;
    if (c.global) {
      if (ui::keyPressed(sc.key, sc.mods)) {
        ui::consumeKey(sc.key);
        hit = true;
      }
    } else {
      hit = ui::shortcut(sc);
    }
    if (hit) runCommand(a, c.id);
  }
  if (editorScreen && ui::shortcut({Key::Z, ui::ModPrimary | platform::ModShift})) a.redo();
  // 3. Выдвижные панели и инструменты.
  if (editorScreen && a.ui.editor.empty()) {
    for (auto& dr : drawers()) {
      if (!dr.shortcut) continue;
      ui::Shortcut sc = parseShortcut(dr.shortcut);
      if (sc && ui::shortcut(sc)) a.openDrawer(dr.id);
    }
  }
  if (onMap) {
    for (auto& t : toolDefs()) {
      if (!t.shortcut || !*t.shortcut) continue;
      ui::Shortcut sc = parseShortcut(t.shortcut);
      if (sc && ui::shortcut(sc)) a.setTool(t.id);
    }
  }
  // 4. Esc: редактор → выделение → инструмент.
  if (editorScreen && ui::shortcut({Key::Escape, 0})) {
    if (!a.ui.editor.empty()) a.closeEditor();
    else if (a.ui.sel) a.clearSelection();
    else if (a.ui.tool != ToolId::Select) a.setTool(ToolId::Select);
    else if (!a.ui.drawer.empty()) a.openDrawer(a.ui.drawer);
  }
  (void)d;
}

// ================================================================ ввод над картой
void mapInput(App& a) {
  App::Impl& d = D(a);
  if (a.hasDialog()) {
    d.panning = false;
    return;
  }
  RectF V = ui::viewport();
  ui::Interaction it = ui::interact(ui::id("##map"), V, ui::IfRightButton | ui::IfMiddleButton);
  // Пробел над картой — панорама: кнопка с фокусом не должна «нажаться» им.
  if ((it.hovered || it.held) && !ui::wantsKeyboard() && ui::keyPressed(Key::Space)) ui::consumeKey(Key::Space);
  float s = ui::uiScale();
  map::MapView& mv = *d.map;
  const map::View& v = mv.view();
  float lx = it.mx * s, ly = it.my * s;
  bool moved = std::fabs(it.mx - d.lastMx) > 0.01f || std::fabs(it.my - d.lastMy) > 0.01f;
  float dxl = (it.mx - d.lastMx) * s, dyl = (it.my - d.lastMy) * s;
  bool hadLast = d.lastMx > -1e8f;
  d.lastMx = it.mx;
  d.lastMy = it.my;
  d.mapHovered = it.hovered || it.held;
  Vec2 mp = v.toMap(lx, ly);
  PointerEvent pe;
  pe.map = mp;
  pe.sx = lx;
  pe.sy = ly;
  pe.button = it.button;
  pe.mods = ui::mouse().mods;
  MapTool* tool = a.activeTool();
  auto call = [&](auto fn) {
    try {
      return fn();
    } catch (const std::exception& e) {
      a.error(e);
      return true;
    }
  };
  if (it.pressed) {
    pe.clicks = it.doubleClicked ? 2 : 1;
    bool pan = it.button == 2 || (it.button == 0 && (d.spaceDown || a.ui.tool == ToolId::Pan));
    if (pan) {
      d.panning = true;
    } else {
      bool handled = tool ? call([&] { return tool->pointerDown(a, pe); }) : false;
      d.toolDown = tool && a.activeTool() == tool;   // инструмент мог смениться (выбор -> правка границ)
      if (!handled && it.doubleClicked && it.button == 0) {
        Id army = mv.armyAt(lx, ly);
        Id prov = army ? 0 : mv.provinceAt(lx, ly);
        if (army) a.select(SelType::Army, army, true);
        else if (prov) a.select(SelType::Province, prov, true);
      }
    }
  } else if (it.held && moved && hadLast) {
    if (d.panning) {
      mv.panBy(dxl, dyl);
      a.requestRedraw();
    } else if (d.toolDown && tool) {
      call([&] { return tool->pointerMove(a, pe); });
    }
  }
  if (it.released) {
    if (d.panning) d.panning = false;
    else if (d.toolDown) {
      d.toolDown = false;
      if (tool && a.activeTool() == tool) call([&] { return tool->pointerUp(a, pe); });
    }
  }
  if (!it.held && !it.pressed && !it.released && it.hovered && moved && tool) call([&] { return tool->pointerMove(a, pe); });
  if (!it.held) d.panning = false;
  // Наведение и координаты
  if (it.hovered || it.held) {
    a.ui.cursorMap = mp;
    if (!d.panning) {
      Id army = mv.armyAt(lx, ly);
      // Видимая линия маршрута (режимы гильдий и торговли или выбранный маршрут) — над провинцией: так же решает
      // и щелчок инструмента «Выбор».
      Id route = 0;
      if (!army && a.ui.tool == ToolId::Select && !a.ui.editBorders) {
        Id r = mv.routeAt(lx, ly);
        bool shown = a.ui.mapMode == schema::MapMode::Guilds || a.ui.mapMode == schema::MapMode::Trade;
        if (r && (shown || a.ui.sel == Selection{SelType::Route, r})) route = r;
      }
      if (army) a.ui.hover = {SelType::Army, army};
      else if (route) a.ui.hover = {SelType::Route, route};
      else if (Id prov = mv.provinceAt(lx, ly)) a.ui.hover = {SelType::Province, prov};
      else a.ui.hover = {};
    }
  } else {
    a.ui.hover = {};
    a.ui.cursorMap.reset();
  }
}

void mapWheel(App& a) {
  App::Impl& d = D(a);
  if (!d.mapHovered || d.panning) return;
  const ui::Mouse& m = ui::mouse();
  if (m.wheelY == 0) return;
  float s = ui::uiScale();
  double notches = double(m.wheelY) / 64.0;
  double factor = std::pow(1.2, clamp(notches, -6.0, 6.0));
  d.map->zoomAt(m.x * s, m.y * s, factor, std::fabs(notches) >= 0.99);
  a.requestRedraw();
}

// ================================================================ отрисовка карты
map::RenderOptions renderOptions(App& a) {
  map::RenderOptions o;
  o.mode = a.ui.mapMode;
  switch (a.ui.sel.type) {
    case SelType::Province: o.selProvince = a.ui.sel.id; break;
    case SelType::Faction: o.selFaction = a.ui.sel.id; break;
    case SelType::Army: o.selArmy = a.ui.sel.id; break;
    case SelType::Route: o.selRoute = a.ui.sel.id; break;
    default: break;
  }
  if (a.ui.hover.type == SelType::Province) o.hoverProvince = a.ui.hover.id;
  if (a.ui.hover.type == SelType::Army) o.hoverArmy = a.ui.hover.id;
  o.editBorders = a.ui.editBorders;
  const Settings& st = *a.world().settings;
  o.labels = st.labelStates || st.labelProvinces || st.labelArmies;
  o.darkUi = a.ui.darkTheme;
  if (MapTool* t = a.activeTool()) {
    try {
      t->renderOptions(a, o);
    } catch (const std::exception& e) {
      a.error(e);
    }
  }
  return o;
}

static u64 hashOf(const map::RenderOptions& o, const map::View& v, u64 gen) {
  u64 h = hash64("map");
  auto mixd = [&](double x) {
    u64 b;
    std::memcpy(&b, &x, sizeof b);
    h = hashMix(h, b);
  };
  mixd(v.cx);
  mixd(v.cy);
  mixd(v.zoom);
  mixd(v.viewport.x);
  mixd(v.viewport.y);
  mixd(v.viewport.w);
  mixd(v.viewport.h);
  mixd(v.dpi);
  h = hashMix(h, u64(o.mode));
  h = hashMix(h, (u64(o.selProvince) << 32) | o.selFaction);
  h = hashMix(h, (u64(o.selArmy) << 32) | o.selRoute);
  h = hashMix(h, (u64(o.hoverProvince) << 32) | o.hoverArmy);
  h = hashMix(h, u64(o.editBorders) | (u64(o.labels) << 1) | (u64(o.darkUi) << 2));
  for (Id x : o.hideArmies) h = hashMix(h, x);
  return hashMix(h, gen);
}

void renderMap(App& a, gfx::Canvas& c) {
  App::Impl& d = D(a);
  map::RenderOptions o = renderOptions(a);
  const map::View& v = d.map->view();
  u64 key = hashOf(o, v, d.mapGen);
  gfx::Image& out = c.target();
  bool need = key != d.mapKey || d.map->needsRedraw() || d.map->animating() || d.mapImg.w != out.w || d.mapImg.h != out.h;
  if (need) {
    if (d.mapImg.w != out.w || d.mapImg.h != out.h) d.mapImg.resize(out.w, out.h);
    gfx::Canvas mc(d.mapImg);
    d.map->render(mc, o);
    d.mapKey = key;
  }
  std::memcpy(out.px.data(), d.mapImg.px.data(), out.px.size() * sizeof(u32));
  if (MapTool* t = a.activeTool()) {
    c.save();
    c.scale(d.dpi, d.dpi);
    try {
      t->drawOverlay(a, c, v);
    } catch (const std::exception& e) {
      a.error(e);
    }
    c.restore();
  }
}

const char* bordersToggleIcon(bool on) { return on ? "unlock" : "lock"; }

}  // namespace rg::app::detail
