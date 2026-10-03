// Сценарии основного экрана: выбор провинции и инспектор с зарегистрированными в тесте вкладками и шапкой,
// правка границ (переключение панели инструментов), палитра команд, отмена и повтор, карта (колесо, панорама),
// настройки, справка, светлая тема.
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

int gOverviewDrawn = 0, gSeaDrawn = 0, gHeaderDrawn = 0, gDrawerDrawn = 0;

void testHeader(app::App& a, Id pid) {
  gHeaderDrawn++;
  const World& w = a.world();
  const Province* p = w.province(pid);
  if (!p) return;
  const Faction* own = w.faction(p->owner);
  ui::caption(own ? "Провинция · " + own->name : std::string("Провинция"));
  ui::label(p->name, {.font = ui::Font::Display});
  if (own) {
    ui::HStack hs(26, ui::Align::Left, 6);
    ui::chip(own->name, {.color = own->color, .clickable = true, .tooltip = "Владелец"});
    ui::tag(schema::cityType(p->city).name, ui::Tone::Neutral, schema::cityType(p->city).icon);
  }
}

void testOverview(app::App& a, Id pid) {
  gOverviewDrawn++;
  const World& w = a.world();
  const Province* p = w.province(pid);
  if (!p) return;
  auto calc = rules::calc(w);
  const rules::ProvinceCalc* pc = calc->province(pid);
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 10);
    ui::stat(fmtShort(double(pc ? pc->population : 0)), "Население", {.icon = "population"});
    ui::stat(fmtNum(pc ? pc->tradeValue : 0), "Торговая ценность", {.icon = "trade-value", .tone = ui::Tone::Info});
    ui::stat(fmtPct(pc ? pc->rebellion : 0), "Восстание", {.icon = "rebellion", .tone = ui::Tone::Danger});
    ui::stat(fmtNum(pc ? pc->production : 0), "Производство", {.icon = "factory", .tone = ui::Tone::Success});
  }
  if (ui::Section s("Свойства", "info"); s) {
    std::string name = p->name;
    ui::prop("Название", "edit");
    if (ui::textField("name", name)) a.act("Переименовать провинцию", [&](Tx& tx) { tx.province(pid).name = name; });
    ui::prop("Довольство", "contentment");
    double c = p->contentment;
    if (ui::numberField("cont", c, {.min = -100, .max = 100})) a.act("Довольство", [&](Tx& tx) { tx.province(pid).contentment = c; }, {.coalesce = "cont"});
  }
}

void testSea(app::App&, Id) { gSeaDrawn++; ui::label("Морская провинция"); }
bool onlySea(app::App& a, Id pid) {
  const Province* p = a.world().province(pid);
  return p && p->sea;
}
int badgeTwo(app::App&, Id) { return 2; }
void testEconomy(app::App&, Id) { ui::label("Экономика"); }
void testDrawer(app::App& a) {
  gDrawerDrawn++;
  const World& w = a.world();
  w.factions.each([&](const Faction& f) {
    if (!f.isState()) return;
    ui::IdScope s{i64(f.id)};
    if (ui::listItem(f.name, {.dot = f.color, .subtitle = "Государство", .selected = a.ui.sel == app::Selection{app::SelType::Faction, f.id}}))
      a.select(app::SelType::Faction, f.id, true);
  });
}

app::TabReg tabOverview({"test.overview", "info", "Обзор", 10, app::SelType::Province, nullptr, testOverview});
app::TabReg tabEconomy({"test.economy", "coins", "Экономика", 20, app::SelType::Province, nullptr, testEconomy, badgeTwo});
app::TabReg tabSea({"test.sea", "sea", "Море", 30, app::SelType::Province, onlySea, testSea});
app::HeaderReg header({"test.header", app::SelType::Province, 0, testHeader});
app::DrawerReg drawer({"test.states", "crown", "Государства", 10, testDrawer, "Ctrl+1"});

// Инструмент правки границ (настоящий напишут авторы инструментов; здесь — для переключения панели).
struct FakeBorders final : app::MapTool {
  int downs = 0;
  bool pointerDown(app::App&, const app::PointerEvent& e) override {
    downs++;
    return e.button == 0;
  }
  const char* hint(app::App&) override { return "Тяните ручки границы"; }
};
app::ToolReg bordersTool({app::ToolId::EditBorders, "tool-edit", "Правка границ провинции", "B", true, [] { return std::make_unique<FakeBorders>(); }, 10});
app::ToolReg knifeTool({app::ToolId::Knife, "tool-knife", "Нож", "K", true, [] { return std::make_unique<FakeBorders>(); }, 20});
app::ToolReg armyTool({app::ToolId::NewArmy, "army", "Новое войско", "A", false, [] { return std::make_unique<FakeBorders>(); }, 50});

}  // namespace

TEST(app_editor_select_province) {
  Harness h("editor_select");
  h.demo();
  h.waitMap();
  h.dropToasts();
  CHECK(h->ui.screen == app::Screen::Editor);
  CHECK(h.shot("editor_demo"));
  auto vp = h.visibleProvince();
  CHECK(vp.has_value());
  Id pid = vp->first;
  gOverviewDrawn = gHeaderDrawn = 0;
  h.click(vp->second.x, vp->second.y);
  h.settle();
  CHECK(h->ui.sel == (app::Selection{app::SelType::Province, pid}));
  CHECK(h->uiRect("inspector") != nullptr);
  CHECK(h->uiRect("inspector.tabs") != nullptr);
  CHECK(gOverviewDrawn > 0);
  CHECK(gHeaderDrawn > 0);
  CHECK(gSeaDrawn == 0);   // вкладка только для морских провинций скрыта
  CHECK_EQ(h->ui.tabOf[app::SelType::Province], std::string("test.overview"));
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("editor_inspector"));
  // Наведение показывает провинцию в строке состояния.
  CHECK(h->ui.hover.type == app::SelType::Province);
  // Esc снимает выделение.
  h.key(Key::Escape);
  CHECK(!h->ui.sel);
  CHECK(h->uiRect("inspector") == nullptr);
}

TEST(app_editor_drawer_and_inspector_resize) {
  Harness h("editor_drawer");
  h.demo();
  CHECK(h.clickUi("drawer.test.states"));
  h.settle();
  CHECK_EQ(h->ui.drawer, std::string("test.states"));
  CHECK(gDrawerDrawn > 0);
  // Ширина панели меняется перетаскиванием края.
  const RectF* dr = h->uiRect("drawer");
  CHECK(dr != nullptr);
  float w0 = h->ui.drawerWidth;
  h.drag(dr->right() - 3, dr->cy(), dr->right() + 57, dr->cy());
  CHECK(h->ui.drawerWidth > w0 + 40);
  // Повторный щелчок по значку закрывает панель.
  CHECK(h.clickUi("drawer.test.states"));
  CHECK(h->ui.drawer.empty());
}

TEST(app_editor_borders_toggle) {
  Harness h("editor_borders");
  h.demo();
  CHECK(!h->ui.editBorders);
  CHECK(h->uiRect("tool.borders") != nullptr);
  CHECK(h->uiRect("tool.knife") == nullptr);           // инструменты правки скрыты
  CHECK(h->uiRect("tool.army") != nullptr);            // войска — всегда
  // Инструмент правки по сочетанию при выключенной правке не выбирается.
  h.key(Key::K);
  CHECK(h->ui.tool == app::ToolId::Select);
  // E — включить правку: панель показывает инструменты правки.
  h.key(Key::E);
  CHECK(h->ui.editBorders);
  CHECK(h->uiRect("tool.knife") != nullptr);
  CHECK(h->uiRect("tool.borders-tool") != nullptr);
  // Щелчок по провинции в режиме правки — её границы (инструмент правки границ).
  auto vp = h.visibleProvince();
  CHECK(vp.has_value());
  h.click(vp->second.x, vp->second.y);
  CHECK(h->ui.sel == (app::Selection{app::SelType::Province, vp->first}));
  CHECK(h->ui.tool == app::ToolId::EditBorders);
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("editor_borders"));
  // Выключение правки возвращает выбор.
  CHECK(h.clickUi("tool.borders"));
  CHECK(!h->ui.editBorders);
  CHECK(h->ui.tool == app::ToolId::Select);
  CHECK(h->uiRect("tool.knife") == nullptr);
  // При выключенной правке щелчок — инспектор, инструмент не меняется.
  h.click(vp->second.x, vp->second.y);
  CHECK(h->ui.tool == app::ToolId::Select);
}

TEST(app_editor_palette_search) {
  Harness h("editor_palette");
  h.demo();
  // Провинция с уникальным именем.
  const World& w = h->world();
  std::string name;
  Id pid = 0;
  w.provinces.each([&](const Province& p) {
    if (pid || p.sea || p.name.size() < 8) return;
    int same = 0;
    w.provinces.each([&](const Province& q) { same += utf8::matches(q.name, p.name) ? 1 : 0; });
    if (same == 1) {
      pid = p.id;
      name = p.name;
    }
  });
  CHECK(pid != 0);
  h.key(Key::K, ctrl());
  CHECK(h->hasDialog("palette"));
  h.type(name);
  h.settle();
  CHECK(h.shot("palette"));
  h.key(Key::Enter);
  h.settle();
  CHECK(!h->hasDialog("palette"));
  CHECK(h->ui.sel == (app::Selection{app::SelType::Province, pid}));
  // Команда из палитры: режим карты.
  h.key(Key::K, ctrl());
  h.type("Гильдии");
  h.key(Key::Enter);
  h.settle();
  CHECK(h->ui.mapMode == schema::MapMode::Guilds);
}

TEST(app_editor_undo_redo) {
  Harness h("editor_undo");
  h.demo();
  auto vp = h.visibleProvince();
  CHECK(vp.has_value());
  Id pid = vp->first;
  std::string old = h->world().province(pid)->name;
  CHECK(h->act("Переименовать", [&](Tx& tx) { tx.province(pid).name = "Новое имя"; }));
  CHECK(h->dirty());
  CHECK(platform::headless::title().rfind("• ", 0) == 0);
  h.key(Key::Z, ctrl());
  CHECK_EQ(h->world().province(pid)->name, old);
  h.key(Key::Y, ctrl());
  CHECK_EQ(h->world().province(pid)->name, std::string("Новое имя"));
  h.key(Key::Z, ctrl());
  h.key(Key::Z, ctrl() | platform::ModShift);
  CHECK_EQ(h->world().province(pid)->name, std::string("Новое имя"));
  // Кнопки верхней панели.
  CHECK(h.clickUi("topbar.undo"));
  CHECK_EQ(h->world().province(pid)->name, old);
  CHECK(h.clickUi("topbar.redo"));
  CHECK_EQ(h->world().province(pid)->name, std::string("Новое имя"));
  // Ошибка правил — уведомление, мир прежний.
  u64 v = h->store.version();
  CHECK(!h->act("Ошибка", [&](Tx& tx) {
    tx.province(pid).name = "x";
    fail("Так нельзя");
  }));
  CHECK_EQ(h->store.version(), v);
  CHECK(!h->toasts().empty());
  CHECK(h->toasts().back().kind == app::ToastKind::Warning);
  // Внутренняя ошибка — уведомление об ошибке, без падения.
  CHECK(!h->act("Сбой", [&](Tx&) { throw std::runtime_error("сбой"); }));
  CHECK(h->toasts().back().kind == app::ToastKind::Danger);
}

TEST(app_editor_map_navigation) {
  Harness h("editor_nav");
  h.demo();
  const map::View v0 = h->map().view();
  RectF area = h->mapArea();
  float cx = area.cx(), cy = area.cy();
  h.wheel(cx, cy, 2);
  h.settle();
  CHECK(h->map().view().zoom > v0.zoom * 1.2);
  // Средняя кнопка — панорама.
  map::View v1 = h->map().view();
  h.drag(cx, cy, cx - 120, cy - 60, platform::MouseMiddle);
  CHECK(std::fabs(h->map().view().cx - v1.cx) > 10);
  // Режимы карты 1–9.
  h.key(Key::D3);
  CHECK(h->ui.mapMode == schema::MapMode::Contentment);
  CHECK(h.clickUi("mode.1"));
  CHECK(h->ui.mapMode == schema::MapMode::Political);
  // Home — вся карта.
  h.key(Key::Home);
  h.settle();
  CHECK_NEAR(h->map().view().zoom, h->map().minZoom(), 1e-6);
  // Мини-карта и легенда переключаются.
  bool mm = h->ui.showMinimap;
  h.key(Key::M);
  CHECK(h->ui.showMinimap != mm);
  h.key(Key::M);
  // Инструмент «Перемещение».
  h.key(Key::H);
  CHECK(h->ui.tool == app::ToolId::Pan);
  h.key(Key::V);
  CHECK(h->ui.tool == app::ToolId::Select);
}

TEST(app_editor_settings_help) {
  Harness h("editor_settings");
  h.demo();
  CHECK(h.clickUi("topbar.settings"));
  h.settle();
  CHECK(h->hasDialog("settings"));
  CHECK(h.shot("settings"));
  // Светлая тема.
  const RectF* tr = h->uiRect("settings.theme");
  CHECK(tr != nullptr);
  h.click(tr->x + tr->w * 0.75f, tr->cy());
  h.settle();
  CHECK(!h->ui.darkTheme);
  CHECK(!ui::theme().dark);
  // Настройка мира — через историю (Ctrl+Z).
  const RectF* tabs = h->uiRect("settings.tabs");
  CHECK(tabs != nullptr);
  h.click(tabs->x + tabs->w * 0.375f, tabs->cy());   // «Карта»
  h.settle();
  bool before = h->world().settings->labelStates;
  CHECK(h.clickUi("settings.labelStates"));
  CHECK(h->world().settings->labelStates != before);
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK(!h->hasDialog());
  h.key(Key::Z, ctrl());
  CHECK(h->world().settings->labelStates == before);
  // Справка F1.
  h.key(Key::F1);
  h.settle();
  CHECK(h->hasDialog("help"));
  CHECK(h.shot("help"));
  h.key(Key::F1);
  h.settle();
  CHECK(!h->hasDialog("help"));
  // Светлая тема основного экрана с выделенной провинцией.
  auto vp = h.visibleProvince();
  CHECK(vp.has_value());
  h->select(app::SelType::Province, vp->first);
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("editor_light"));
}

TEST(app_editor_ui_scale) {
  Harness h("editor_scale", 1600, 1000, 1.25f);
  h.demo();
  h->setUiScale(1.1f);
  auto vp = h.visibleProvince();
  CHECK(vp.has_value());
  h.click(vp->second.x, vp->second.y);
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h->ui.sel.type == app::SelType::Province);
  CHECK(h.shot("editor_scale"));
}

TEST(app_editor_min_window) {
  // Наименьшее окно 1100×700: верхняя панель компактна, панели не налезают друг на друга.
  Harness h("editor_min", 1100, 700);
  h.demo();
  CHECK(h.clickUi("drawer.test.states"));
  auto vp = h.visibleProvince();
  if (vp) h.click(vp->second.x, vp->second.y);
  else h->select(app::SelType::Province, h->world().provinces.ids().front());
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("editor_min"));
  const RectF* top = h->uiRect("topbar");
  const RectF* end = h->uiRect("topbar.endturn");
  const RectF* modes = h->uiRect("mode.9");
  CHECK(top && end && modes);
  CHECK(end->right() <= top->right() + 0.5f);
  CHECK(modes->right() < end->x);
  const RectF* tools = h->uiRect("toolbar");
  const RectF* ins = h->uiRect("inspector");
  CHECK(tools && ins);
  CHECK(tools->right() < ins->x);
}

namespace {
int gEditorDrawn = 0;
Id gEditorArg = 0;
void testEditor(app::App& a, Id arg) {
  gEditorDrawn++;
  gEditorArg = arg;
  const Faction* f = a.world().faction(arg);
  ui::label(f ? "Дерево технологий: " + f->name : std::string("Дерево технологий"), {.font = ui::Font::Title});
  ui::text("Полноэкранный редактор занимает всё окно; Esc или стрелка — назад к карте.", ui::Font::Body, ui::Ink::Dim);
}
app::EditorReg testEditorReg({"test.techtree", "Дерево технологий", testEditor, "tech-tree"});
}  // namespace

TEST(app_editor_fullscreen_editor) {
  Harness h("editor_fullscreen");
  h.demo();
  Id fid = 0;
  h->world().factions.each([&](const Faction& f) {
    if (!fid && f.isState()) fid = f.id;
  });
  h->openEditor("test.techtree", fid);
  h.settle();
  CHECK(gEditorDrawn > 0);
  CHECK_EQ(gEditorArg, fid);
  CHECK(h->uiRect("editor") != nullptr);
  CHECK(h->uiRect("topbar") == nullptr);   // карта и панели скрыты
  CHECK(h.shot("editor_fullscreen"));
  // Отмена работает и в редакторе.
  double tax0 = h->world().faction(fid)->tax;
  CHECK(h->act("Правка", [&](Tx& tx) { tx.faction(fid).tax = 33; }));
  h.key(Key::Z, ctrl());
  CHECK_NEAR(h->world().faction(fid)->tax, tax0, 1e-9);
  h.key(Key::Escape);
  CHECK(h->ui.editor.empty());
  CHECK(h->uiRect("topbar") != nullptr);
  // Неизвестный редактор не открывается.
  h->openEditor("нет.такого");
  CHECK(h->ui.editor.empty());
}

TEST(app_prefs_persist) {
  // Тема, масштаб, ширина панелей и мини-карта запоминаются в app.json папки данных пользователя.
  std::string dir = tempDir("prefs");
  hl::reset();
  ui::shutdown();
  ui::init();
  {
    app::App a(app::AppConfig{dir});
    a.setTheme(false);
    a.setUiScale(1.25f);
    a.ui.inspectorWidth = 480;
    a.ui.showMinimap = false;
  }
  CHECK(fs::isFile(fs::join(dir, "app.json")));
  {
    app::App a(app::AppConfig{dir});
    CHECK(!a.ui.darkTheme);
    CHECK_NEAR(a.ui.uiScale, 1.25, 1e-6);
    CHECK_NEAR(a.ui.inspectorWidth, 480, 1e-6);
    CHECK(!a.ui.showMinimap);
  }
  {
    app::App a(app::AppConfig{dir, {}, false});   // без запоминания — значения по умолчанию
    CHECK(a.ui.darkTheme);
  }
  ui::shutdown();
}
