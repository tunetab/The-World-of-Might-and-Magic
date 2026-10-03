// Regnum — ядро приложения: реестры, действия с миром, выделение, инструменты, диалоги, уведомления,
// цикл кадра (карта с кешем + интерфейс), настройки приложения (app.json).
#include "app/app_internal.h"
#include "base/fs.h"
#include "base/json.h"
#include "base/jobs.h"

namespace rg::app {

// ================================================================ реестры
namespace {
template <class T>
std::vector<T>& reg() {
  static std::vector<T> v;
  return v;
}
template <class T>
void insertSorted(std::vector<T>& v, const T& d) {
  auto it = std::upper_bound(v.begin(), v.end(), d, [](const T& a, const T& b) { return a.order < b.order; });
  v.insert(it, d);
}
}  // namespace

TabReg::TabReg(const TabDef& d) { insertSorted(reg<TabDef>(), d); }
const std::vector<TabDef>& tabs() { return reg<TabDef>(); }

HeaderReg::HeaderReg(const HeaderDef& d) { insertSorted(reg<HeaderDef>(), d); }
const std::vector<HeaderDef>& headers() { return reg<HeaderDef>(); }

DrawerReg::DrawerReg(const DrawerDef& d) { insertSorted(reg<DrawerDef>(), d); }
const std::vector<DrawerDef>& drawers() { return reg<DrawerDef>(); }

EditorReg::EditorReg(const EditorDef& d) {
  auto& v = reg<EditorDef>();
  for (auto& e : v)
    if (std::string_view(e.id) == d.id) {
      e = d;
      return;
    }
  v.push_back(d);
}
const EditorDef* findEditor(std::string_view id) {
  for (auto& e : reg<EditorDef>())
    if (id == e.id) return &e;
  return nullptr;
}
const std::vector<EditorDef>& editors() { return reg<EditorDef>(); }

ToolReg::ToolReg(const ToolDef& d) {
  auto& v = reg<ToolDef>();
  // Повторная регистрация того же инструмента заменяет прежнюю (встроенный выбор можно переопределить).
  for (auto it = v.begin(); it != v.end(); ++it)
    if (it->id == d.id) {
      v.erase(it);
      break;
    }
  insertSorted(v, d);
}
const ToolDef* findTool(ToolId id) {
  for (auto& t : reg<ToolDef>())
    if (t.id == id) return &t;
  return nullptr;
}
const std::vector<ToolDef>& toolDefs() { return reg<ToolDef>(); }

CommandReg::CommandReg(const CommandDef& d) {
  auto& v = reg<CommandDef>();
  for (auto& c : v)
    if (std::string_view(c.id) == d.id) {
      c = d;
      return;
    }
  v.push_back(d);
}
const std::vector<CommandDef>& commands() { return reg<CommandDef>(); }
const CommandDef* findCommand(std::string_view id) {
  for (auto& c : reg<CommandDef>())
    if (id == c.id) return &c;
  return nullptr;
}
bool runCommand(App& a, std::string_view id) {
  const CommandDef* c = findCommand(id);
  if (!c || !c->run) return false;
  if (c->enabled && !c->enabled(a)) return false;
  try {
    c->run(a);
  } catch (const std::exception& e) {
    a.error(e);
  }
  return true;
}

DialogReg::DialogReg(const DialogDef& d) {
  auto& v = reg<DialogDef>();
  for (auto& x : v)
    if (std::string_view(x.id) == d.id) {
      x = d;
      return;
    }
  v.push_back(d);
}
const DialogDef* findDialog(std::string_view id) {
  for (auto& d : reg<DialogDef>())
    if (id == d.id) return &d;
  return nullptr;
}

// ================================================================ экземпляр
static App* gApp = nullptr;
App& app() {
  if (!gApp) throw std::logic_error("Regnum: приложение не создано");
  return *gApp;
}
bool hasApp() { return gApp != nullptr; }

// ---------------------------------------------------------------- настройки приложения
static std::string prefsPath(const std::string& dataDir) { return fs::join(dataDir, "app.json"); }

static void loadPrefs(App& a) {
  App::Impl& d = a.impl();
  if (!d.cfg.savePrefs) return;
  auto text = fs::readFile(prefsPath(d.dataDir));
  if (!text) return;
  auto v = json::tryParse(*text);
  if (!v || !v->isObj()) return;
  a.ui.darkTheme = v->str("theme", "dark") != "light";
  a.ui.uiScale = float(clamp(v->num("uiScale", 1.0), 0.9, 1.5));
  a.ui.showMinimap = v->boolean("minimap", true);
  a.ui.showLegend = v->boolean("legend", true);
  a.ui.drawerWidth = float(clamp(v->num("drawerWidth", 340), 260.0, 640.0));
  a.ui.inspectorWidth = float(clamp(v->num("inspectorWidth", 400), 320.0, 720.0));
  d.browserDir = v->str("browserDir");
  d.builtinBrowser = v->boolean("builtinBrowser", false);
}

void detail::savePrefs(App& a) {
  App::Impl& d = a.impl();
  d.prefsDirty = false;
  if (!d.cfg.savePrefs) return;
  json::Value v = json::Value::object();
  v.set("theme", a.ui.darkTheme ? "dark" : "light");
  v.set("uiScale", double(a.ui.uiScale));
  v.set("minimap", a.ui.showMinimap);
  v.set("legend", a.ui.showLegend);
  v.set("drawerWidth", double(std::round(a.ui.drawerWidth)));
  v.set("inspectorWidth", double(std::round(a.ui.inspectorWidth)));
  v.set("browserDir", d.browserDir);
  v.set("builtinBrowser", d.builtinBrowser);
  std::string text = json::write(v, json::WriteOptions{2, true, false});
  text.push_back('\n');
  std::string err;
  if (!fs::writeFileAtomic(prefsPath(d.dataDir), text, &err)) logWarn("app.json: %s", err.c_str());
}

// ---------------------------------------------------------------- создание
App::App(const AppConfig& cfg) : d_(std::make_unique<Impl>()) {
  gApp = this;
  Impl& d = *d_;
  d.cfg = cfg;
  d.dataDir = cfg.dataDir.empty() ? fs::userDataDir() : fs::absolute(cfg.dataDir);
  fs::makeDirs(d.dataDir);
  ui::init();
  ui::setClipboard([] { return platform::clipboardText(); }, [](const std::string& s) { platform::setClipboardText(s); });
  loadPrefs(*this);
  ui::setTheme(ui.darkTheme);
  ui::setUiScale(ui.uiScale);

  std::string dir = cfg.basemapDir.empty() ? detail::findBasemapDir() : cfg.basemapDir;
  if (!dir.empty()) {
    auto bm = std::make_unique<map::Basemap>();
    std::string err;
    if (bm->load(dir, &err)) d.basemap = std::move(bm);
    else logWarn("Базовая карта не загружена (%s): %s", dir.c_str(), err.c_str());
  } else {
    logWarn("Базовая карта не найдена: assets/basemap");
  }
  d.map = std::make_unique<map::MapView>(d.basemap.get());
  d.map->setWakeCallback([] { platform::wake(); });   // тайлы готовы в фоне — разбудить цикл событий
  d.map->setWorld(store.world());
  storeSub_ = store.subscribe([this](const Change& c) { onStoreChange(c); });
  setTool(ToolId::Select);
}

App::~App() {
  waitBackground();
  store.unsubscribe(storeSub_);
  if (tool_) {
    auto t = std::move(tool_);
    try {
      t->deactivate(*this);
    } catch (...) {
    }
  }
  if (d_->prefsDirty) detail::savePrefs(*this);
  dialogs_.clear();
  if (gApp == this) gApp = nullptr;
}

void App::onStoreChange(const Change& c) {
  Impl& d = *d_;
  if (!readOnly()) {
    if (c.kind == Change::Load) d.map->setWorld(*c.after);
    else d.map->worldChanged(*c.before, *c.after, c.tables);
    d.mapGen++;
  }
  platform::invalidate();
}

// ================================================================ мир
const World& App::world() const { return ui.viewTurn ? viewWorld_ : store.world(); }

bool App::act(std::string_view label, const std::function<void(Tx&)>& fn, const TxOptions& opt) {
  if (readOnly()) {
    toast("Открыт прошлый ход — изменения недоступны", ToastKind::Warning, "lock", "К текущему ходу", [](App& a) { a.backToCurrent(); });
    return false;
  }
  try {
    store.transact(label, [&](Tx& tx) { fn(tx); }, opt);
    return true;
  } catch (const std::exception& e) {
    error(e);
    return false;
  }
}

void App::undo() {
  if (readOnly()) return;
  if (!store.canUndo()) return;
  std::string l = store.undoLabel();
  store.undo();
  if (!l.empty()) toast("Отменено: " + l, ToastKind::Info, "undo");
}

void App::redo() {
  if (readOnly()) return;
  if (!store.canRedo()) return;
  std::string l = store.redoLabel();
  store.redo();
  if (!l.empty()) toast("Повторено: " + l, ToastKind::Info, "redo");
}

bool App::dirty() const { return ui.screen == Screen::Editor && (store.dirty() || d_->forceTables != 0); }

// ================================================================ интерфейс
void App::select(Selection s, bool focus) {
  if (s && !detail::selectionExists(world(), s)) s = Selection{};
  ui.sel = s;
  if (s && focus) focusSelection();
  requestRedraw();
}

void App::setTool(ToolId t) {
  const ToolDef* def = findTool(t);
  if (!def) {
    if (t != ToolId::Select) toast("Этот инструмент пока недоступен", ToastKind::Info, "info");
    return;
  }
  if (def->editMode && readOnly()) {
    toast("Прошлый ход: только просмотр — инструменты правки недоступны", ToastKind::Info, "lock");
    return;
  }
  if (def->editMode && !ui.editBorders) {
    toast("Включите правку границ (E), чтобы менять провинции", ToastKind::Info, "lock");
    return;
  }
  if (tool_ && ui.tool == t) return;
  if (tool_) {
    auto old = std::move(tool_);
    try {
      old->deactivate(*this);
    } catch (const std::exception& e) {
      error(e);
    }
  }
  d_->toolDown = false;
  ui.tool = t;
  try {
    tool_ = def->make ? def->make() : nullptr;
    if (tool_) tool_->activate(*this);
  } catch (const std::exception& e) {
    tool_.reset();
    error(e);
    if (t != ToolId::Select) setTool(ToolId::Select);
  }
  requestRedraw();
}

void App::setEditBorders(bool on) {
  if (ui.editBorders == on) return;
  // Прошлый ход — только просмотр: режим правки границ не включается (ТЗ 1.a.ii, 1.f).
  if (on && readOnly()) return;
  ui.editBorders = on;
  if (!on) {
    const ToolDef* cur = findTool(ui.tool);
    if (cur && cur->editMode) setTool(ToolId::Select);
  } else if (ui.tool == ToolId::Select && findTool(ToolId::EditBorders)) {
    // ТЗ 1.a.iv: в режиме правки щелчок по провинции открывает её границы — сразу инструмент границ.
    setTool(ToolId::EditBorders);
  }
  d_->mapGen++;
  requestRedraw();
}

void App::setMapMode(schema::MapMode m) {
  if (int(m) < 0 || m >= schema::MapMode::Count) return;
  ui.mapMode = m;
  requestRedraw();
}

void App::openDrawer(std::string_view id) {
  if (ui.drawer == id) ui.drawer.clear();
  else {
    bool found = false;
    for (auto& dr : drawers())
      if (id == dr.id) found = true;
    ui.drawer = found ? std::string(id) : std::string();
  }
  requestRedraw();
}

void App::openEditor(std::string_view id, Id arg) {
  if (!findEditor(id)) {
    toast("Редактор недоступен", ToastKind::Warning, "warning");
    return;
  }
  ui.editor = std::string(id);
  ui.editorArg = arg;
  requestRedraw();
}

void App::closeEditor() {
  ui.editor.clear();
  ui.editorArg = 0;
  d_->mapKey = 0;
  requestRedraw();
}

void App::openDialog(std::unique_ptr<Dialog> dlg) {
  if (!dlg) return;
  Impl& f = *d_;
  if (f.drawingDialogs) {
    f.pendingDialogs.push_back(std::move(dlg));
  } else {
    std::string_view id = dlg->id();
    dialogs_.erase(std::remove_if(dialogs_.begin(), dialogs_.end(), [&](auto& x) { return id == x->id(); }), dialogs_.end());
    dialogs_.push_back(std::move(dlg));
  }
  requestRedraw();
}

bool App::openDialog(std::string_view id, Id arg) {
  if (const DialogDef* def = findDialog(id)) {
    try {
      openDialog(def->make(*this, arg));
    } catch (const std::exception& e) {
      error(e);
    }
    return true;
  }
  if (id == "turn.report") {
    if (!lastReport_) return false;
    openDialog(detail::turnReportDialog(*this, *lastReport_));
    return true;
  }
  if (id == "turn.history") {
    openDialog(detail::historyDialog(*this));
    return true;
  }
  if (id == "settings") {
    openDialog(detail::settingsDialog());
    return true;
  }
  if (id == "palette") {
    openDialog(detail::paletteDialog());
    return true;
  }
  if (id == "help") {
    openDialog(detail::helpDialog());
    return true;
  }
  if (id == "world.new") {
    openDialog(detail::newWorldDialog(*this));
    return true;
  }
  return false;
}

void App::closeDialogs() {
  Impl& f = *d_;
  if (f.drawingDialogs) f.closeAllDialogs = true;
  else dialogs_.clear();
  f.pendingDialogs.clear();
  requestRedraw();
}

bool App::hasDialog(std::string_view id) const {
  for (auto& x : dialogs_)
    if (id == x->id()) return true;
  for (auto& x : d_->pendingDialogs)
    if (id == x->id()) return true;
  return false;
}

void App::toast(std::string text, ToastKind kind, std::string icon, std::string actionLabel, std::function<void(App&)> action) {
  // Над модальным окном интерфейсные панели не видны: уведомление без действия — в слой ui.
  if (ui::anyModalOpen() || hasDialog()) {
    if (actionLabel.empty()) {
      static const ui::Tone tones[] = {ui::Tone::Info, ui::Tone::Success, ui::Tone::Warning, ui::Tone::Danger};
      ui::toast(text, tones[int(kind)], icon.empty() ? nullptr : icon.c_str(), kind == ToastKind::Danger ? 8 : 4);
      requestRedraw();
      return;
    }
  }
  Toast t;
  t.id = ++toastSeq_;
  t.kind = kind;
  t.text = std::move(text);
  t.icon = std::move(icon);
  t.start = time_;
  t.duration = kind == ToastKind::Danger ? 8 : !actionLabel.empty() ? 7 : kind == ToastKind::Warning ? 5 : 3.5;
  t.actionLabel = std::move(actionLabel);
  t.action = std::move(action);
  // Не больше пяти; одинаковые подряд не повторяются.
  if (!toasts_.empty() && toasts_.back().text == t.text && toasts_.back().closeAt < 0) {
    toasts_.back().start = time_;
    toasts_.back().paused = 0;
    return;
  }
  toasts_.push_back(std::move(t));
  int live = 0;
  for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it)
    if (it->closeAt < 0 && ++live > 5) it->closeAt = time_;
  requestRedraw();
}

void App::error(const std::exception& e) {
  bool user = dynamic_cast<const UserError*>(&e) != nullptr;
  std::string msg = e.what();
  Impl& f = *d_;
  if (msg == f.lastError && time_ - f.lastErrorAt < 2) return;   // повторяющаяся ошибка каждого кадра
  f.lastError = msg;
  f.lastErrorAt = time_;
  if (user) {
    toast(msg, ToastKind::Warning, "warning");
  } else {
    logError("Ошибка: %s", msg.c_str());
    toast("Что-то пошло не так: " + msg + ". Мир не изменён.", ToastKind::Danger, "error");
  }
}

void App::confirm(std::string title, std::string text, std::string okLabel, bool danger, std::function<void(App&)> onYes) {
  openDialog(detail::confirmDialog(std::move(title), std::move(text), std::move(okLabel), danger, std::move(onYes)));
}

void App::choose(std::string title, std::string text, std::vector<std::string> buttons, std::function<void(App&, int)> onPick,
                 const char* icon, bool firstSubtle) {
  openDialog(detail::choiceDialog(std::move(title), std::move(text), std::move(buttons), std::move(onPick), icon, firstSubtle));
}

void App::prompt(std::string title, std::string label, std::string initial, std::function<void(App&, const std::string&)> onOk) {
  openDialog(detail::promptDialog(std::move(title), std::move(label), std::move(initial), std::move(onOk)));
}

void App::message(std::string title, std::string text, std::vector<std::string> lines, ToastKind kind) {
  openDialog(detail::messageDialog(std::move(title), std::move(text), std::move(lines), kind));
}

void App::focusMap(Box2 box) {
  if (box.empty()) return;
  map::MapView& m = *d_->map;
  RectF area = mapArea();
  if (area.w < 40 || area.h < 40) area = m.view().viewport;
  double pad = 0.12;
  double zw = area.w / std::max(1.0, box.w() * (1 + 2 * pad));
  double zh = area.h / std::max(1.0, box.h() * (1 + 2 * pad));
  double z = clamp(std::min(zw, zh), m.minZoom(), std::min(m.maxZoom(), 1.6));
  focusMap(box.center(), z);
}

void App::focusMap(Vec2 p, double zoom) {
  map::MapView& m = *d_->map;
  const map::View& v = m.view();
  double z = zoom > 0 ? clamp(zoom, m.minZoom(), m.maxZoom()) : v.zoom;
  // Точка должна оказаться в центре видимой части карты, а не всего окна.
  RectF area = mapArea();
  double ox = (double(area.cx()) - double(v.viewport.cx())) / z;
  double oy = (double(area.cy()) - double(v.viewport.cy())) / z;
  m.centerOn(Vec2{p.x - ox, p.y - oy}, z, true);
  requestRedraw();
}

void App::focusSelection() {
  if (!ui.sel) return;
  const World& w = world();
  if (ui.sel.type == SelType::Army) {
    if (const Army* a = w.army(ui.sel.id)) focusMap(a->pos, std::max(d_->map->view().zoom, 0.45));
    return;
  }
  Box2 b = detail::selectionBox(w, ui.sel);
  if (!b.empty()) focusMap(b);
}

void App::requestRedraw() { platform::invalidate(); }

void App::setTheme(bool dark) {
  if (ui.darkTheme == dark) return;
  ui.darkTheme = dark;
  d_->prefsDirty = true;
  d_->backdropW = 0;
  d_->mapKey = 0;
  requestRedraw();
}

void App::setUiScale(float s) {
  s = clamp(s, 0.9f, 1.5f);
  if (std::fabs(ui.uiScale - s) < 1e-3f) return;
  ui.uiScale = s;
  d_->prefsDirty = true;
  requestRedraw();
}

void App::toggleFullscreen() { platform::setFullscreen(!platform::fullscreen()); }

void App::showPalette() {
  if (hasDialog("palette")) closeDialogs();
  else openDialog("palette");
}
void App::showHelp() {
  if (hasDialog("help")) closeDialogs();
  else openDialog("help");
}
void App::showSettings() { openDialog("settings"); }

RectF App::mapArea() const {
  auto it = d_->rects.find("map.area");
  if (it != d_->rects.end()) return it->second;
  return RectF{0, 0, d_->lw, d_->lh};
}

const RectF* App::uiRect(std::string_view name) const {
  auto it = d_->rects.find(std::string(name));
  return it == d_->rects.end() ? nullptr : &it->second;
}

void App::markUi(std::string_view name) { markUi(name, ui::lastItem().rect); }

void App::markUi(std::string_view name, RectF r) {
  float s = ui::uiScale();
  d_->rectsNext[std::string(name)] = RectF{r.x * s, r.y * s, r.w * s, r.h * s};
}

map::MapView& App::map() { return *d_->map; }
const map::Basemap* App::basemap() const { return d_->basemap.get(); }
std::string App::dataDir() const { return d_->dataDir; }
const std::string& App::projectPath() const { return d_->path; }
bool App::projectIsBundle() const { return d_->bundle; }
std::string App::worldTitle() const {
  const std::string& n = store.world().meta->name;
  return n.empty() ? std::string("Без названия") : n;
}

void App::waitBackground() {
  Impl& d = *d_;
  if (d.autosaveJob.valid()) {
    try {
      std::string err = d.autosaveJob.get();
      if (!err.empty()) logWarn("Автосохранение: %s", err.c_str());
    } catch (const std::exception& e) {
      logWarn("Автосохранение: %s", e.what());
    }
  }
}

// ================================================================ события
void App::onEvent(const platform::Event& e) {
  using T = platform::EventType;
  Impl& d = *d_;
  switch (e.type) {
    case T::KeyDown:
      if (e.key == platform::Key::Space && !ui::wantsKeyboard()) d.spaceDown = true;
      break;
    case T::KeyUp:
      if (e.key == platform::Key::Space) d.spaceDown = false;
      break;
    case T::FocusOut:
      d.spaceDown = false;
      break;
    case T::FocusIn:
      d_->later.push_back([](App& a) { a.checkExternalChanges(); });
      break;
    case T::FilesDropped:
      if (!e.files.empty()) {
        std::string p = e.files.front();
        d_->later.push_back([p](App& a) { a.openPath(p); });
      }
      break;
    default:
      break;
  }
  ui::onEvent(e);
  platform::invalidate();
}

bool App::animating() {
  Impl& d = *d_;
  if (ui::needsRedraw()) return true;
  if (!toasts_.empty()) return true;
  if (!d_->later.empty()) return true;
  if (ui.screen == Screen::Editor && ui.editor.empty()) {
    if (d.map->animating() || d.map->needsRedraw()) return true;
    if (tool_ && tool_->animating(*this)) return true;
  }
  if (d.autosaveJob.valid()) return true;
  return false;
}

bool App::onCloseRequest() {
  if (!dirty()) {
    waitBackground();
    if (ui.screen == Screen::Editor) io::clearAutosave(d_->path, d_->dataDir);
    if (d_->prefsDirty) detail::savePrefs(*this);
    return true;
  }
  requestQuit();
  return false;
}

// ================================================================ кадр
void App::onFrame(platform::Frame& f) {
  Impl& d = *d_;
  frames_++;
  time_ = platform::time();
  d.lw = f.logicalW();
  d.lh = f.logicalH();
  d.dpi = f.scale;
  if (ui::theme().dark != ui.darkTheme) {
    ui::setTheme(ui.darkTheme);
    platform::setDarkFrame(ui.darkTheme);
  }
  if (std::fabs(ui::uiScale() - ui.uiScale) > 1e-3f) ui::setUiScale(ui.uiScale);
  if (d.frameImg.w != f.w || d.frameImg.h != f.h) d.frameImg.resize(f.w, f.h);
  d.map->setViewport(RectF{0, 0, d.lw, d.lh}, d.dpi);
  d.map->update(time_);
  d.rectsNext.clear();

  ui::beginFrame(d.lw, d.lh, d.dpi, time_);
  try {
    buildFrame();
  } catch (const std::exception& e) {
    error(e);
  }
  // Свободная часть карты между панелями (раскладка этого кадра): в неё «Показать всю карту» вписывает мир,
  // по ней же минимальный масштаб. Вся карта при открытии мира — когда эта часть уже известна.
  {
    auto it = d.rectsNext.find("map.area");
    d.map->setSafeArea(it != d.rectsNext.end() ? it->second : RectF{});
  }
  if (d.fitPending && d.lw > 1 && d.lh > 1) {
    d.map->fitAll(false);
    d.fitPending = false;
  }
  gfx::Canvas c(d.frameImg);
  try {
    if (ui.screen == Screen::Editor && ui.editor.empty()) detail::renderMap(*this, c);
    else if (ui.screen == Screen::Start) detail::renderStartBackdrop(*this, c);
    else c.clear(ui::theme().bg);
  } catch (const std::exception& e) {
    c.clear(ui::theme().bg);
    error(e);
  }
  ui::endFrame(c);
  d.rects.swap(d.rectsNext);

  // Курсор: интерфейс, иначе инструмент карты.
  platform::Cursor cur = ui::cursor();
  if (cur == platform::Cursor::Arrow && d.mapHovered && ui.screen == Screen::Editor && ui.editor.empty() && !hasDialog()) {
    if (d.panning) cur = platform::Cursor::Grabbing;
    else if (d.spaceDown || ui.tool == ToolId::Pan) cur = platform::Cursor::Grab;
    else if (tool_) cur = tool_->cursor(*this);
  }
  platform::setCursor(cur);
  if (auto r = ui::textInputRect()) platform::setTextInputRect(*r);

  // Заголовок окна: «• Мир — Regnum».
  std::string title = ui.screen == Screen::Editor ? (dirty() ? "• " : "") + worldTitle() + " — Regnum" : std::string("Regnum");
  if (title != d.title) {
    d.title = title;
    platform::setTitle(title);
  }

  // Кадр в буфер окна.
  for (int y = 0; y < f.h; y++) std::memcpy(f.row(y), d.frameImg.row(y), size_t(f.w) * sizeof(u32));

  // Отложенные действия (после сведения кадра: системные диалоги, открытие файлов).
  Impl& fs = *d_;
  if (!fs.later.empty()) {
    auto later = std::move(fs.later);
    fs.later.clear();
    for (auto& fn : later) {
      try {
        fn(*this);
      } catch (const std::exception& e) {
        error(e);
      }
    }
    platform::invalidate();
  }
  if (d.prefsDirty && frames_ % 120 == 0) detail::savePrefs(*this);
}

void App::buildFrame() {
  Impl& d = *d_;
  if (ui.sel && !detail::selectionExists(world(), ui.sel)) ui.sel = Selection{};
  if (ui.hover && !detail::selectionExists(world(), ui.hover)) ui.hover = Selection{};
  if (!ui.editor.empty() && !findEditor(ui.editor)) ui.editor.clear();
  d.mapHovered = false;

  if (ui.screen == Screen::Start) {
    detail::drawStartScreen(*this);
  } else {
    if (ui.editor.empty()) detail::mapInput(*this);
    detail::drawEditorScreen(*this);
  }
  // Диалоги
  Impl& fs = *d_;
  fs.drawingDialogs = true;
  try {
    detail::drawDialogs(*this);
  } catch (...) {
    fs.drawingDialogs = false;
    throw;
  }
  fs.drawingDialogs = false;
  if (fs.closeAllDialogs) {
    dialogs_.clear();
    fs.closeAllDialogs = false;
  }
  auto pending = std::move(fs.pendingDialogs);
  fs.pendingDialogs.clear();
  for (auto& p : pending) openDialog(std::move(p));

  detail::globalKeys(*this);
  if (ui.screen == Screen::Editor && ui.editor.empty()) detail::mapWheel(*this);
  if (ui.screen == Screen::Editor) detail::autosaveTick(*this);
}

}  // namespace rg::app

namespace rg::app::detail {
// Отложить действие до конца кадра (после сведения интерфейса).
void later(App& a, std::function<void(App&)> fn) {
  a.impl().later.push_back(std::move(fn));
  platform::invalidate();
}
}  // namespace rg::app::detail
