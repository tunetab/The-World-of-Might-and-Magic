// Regnum — проект: открытие папки или архива, сохранение (только изменённые таблицы), «Сохранить как»,
// новый мир по береговой линии, автосохранение (данные пользователя и папка проекта), внешние изменения,
// закрытие с вопросом о несохранённых изменениях, недавние миры и их миниатюры, восстановление после сбоя.
#include "app/app_internal.h"
#include "app/dialogs/turn_ui.h"
#include "base/fs.h"
#include "base/jobs.h"
#include "codec/png.h"
#include "geo/ops.h"

namespace rg::app {

using namespace detail;

namespace {

// Перед заменой мира: несохранённые изменения — сохранить, не сохранять или отменить.
// «Не сохранять» отбрасывает и несохранённые снимки ходов (они живут в памяти до сохранения).
void whenSafe(App& a, std::function<void(App&)> then) {
  if (!a.dirty()) {
    then(a);
    return;
  }
  std::string name = a.worldTitle();
  a.choose("Сохранить изменения?", "В мире «" + name + "» есть несохранённые изменения.", {"Не сохранять", "Отмена", "Сохранить"},
           [then](App& x, int i) {
             if (i == 0) {
               x.waitBackground();   // запись автосохранения в фоне не должна вернуть отброшенную работу
               io::clearAutosave(x.projectPath(), x.dataDir());
               then(x);
             } else if (i == 2) {
               x.impl().afterSave = then;
               if (!x.save() && !x.hasDialog() && x.impl().afterSave) x.impl().afterSave = nullptr;
             }
           },
           "save", true);
}

void enterEditor(App& a) {
  App::Impl& d = a.impl();
  a.ui.screen = Screen::Editor;
  a.ui.sel = {};
  a.ui.hover = {};
  a.ui.editor.clear();
  a.ui.viewTurn.reset();
  a.setEditBorders(false);
  a.setTool(ToolId::Select);
  d.fitPending = true;
  d.mapKey = 0;
  d.lastAutosave = a.time();
  d.autosaveVersion = a.store.version();
  a.requestRedraw();
}

void resetProject(App::Impl& d) {
  d.path.clear();
  d.bundle = false;
  d.files = io::FileState{};
  d.fixedTables = d.forceTables = 0;
  d.overwriteExternal = false;
  d.ackExternal.clear();
  d.savedAt.clear();
  d.autosavedAt.clear();
  d.folderAutosaveBlocked = false;
  // История ходов прежнего мира не переходит к новому.
  d.memSnaps.clear();
  d.snapHead = 0;
  d.snapNext = 1;
  d.headUndo.clear();
  d.headRedo.clear();
  d.diskHist.reset();
  d.bundleStamp = io::FileStamp{};
  d.autoFiles.clear();
}

// Автосохранение (данные пользователя) новее последнего сохранения мира на диске (meta.updatedAt; если его
// нет — время файлов). Время снимка мира, а не файла: запись истории или копирование не прячут несохранённое.
bool autosaveNewer(const io::AutosaveInfo& info, const std::string& savedIso, const std::string& project) {
  i64 at = isoToMs(info.at);
  i64 saved = isoToMs(savedIso);
  if (at > 0 && saved > 0) return at >= saved;
  std::string probe = io::isBundlePath(project) ? project : fs::join(project, "world.json");
  auto mt = fs::mtime(probe);
  return !mt || at > *mt + 2000;
}

// Внешние изменения, о которых пользователь ещё не решал: файлы папки или сам архив .regnum.
std::string changedPath(const App::Impl& d, const io::ExternalChange& c) { return d.bundle ? d.path : fs::join(d.path, c.file); }

std::vector<io::ExternalChange> freshChanges(App& a) {
  App::Impl& d = a.impl();
  std::vector<io::ExternalChange> ch, fresh;
  if (d.path.empty()) return fresh;
  try {
    if (d.bundle) {
      if (d.bundleStamp.exists && io::fileChanged(d.path, d.bundleStamp)) {
        io::ExternalChange c;
        c.file = fs::filename(d.path);
        c.kind = fs::isFile(d.path) ? io::ChangeKind::Modified : io::ChangeKind::Removed;
        c.tables = TB_ALL;
        ch.push_back(std::move(c));
      }
    } else if (!d.files.folder.empty()) {
      ch = io::externalChanges(d.files);
    }
  } catch (const std::exception& e) {
    logWarn("Проверка внешних изменений: %s", e.what());
    return fresh;
  }
  for (auto& c : ch) {
    i64 mt = fs::mtime(changedPath(d, c)).value_or(-1);
    auto it = d.ackExternal.find(c.file);
    if (it != d.ackExternal.end() && it->second == mt) continue;
    fresh.push_back(c);
  }
  return fresh;
}

// «Оставить мои»: при сохранении эти файлы будут заменены версией редактора.
void ackChanges(App& a, const std::vector<io::ExternalChange>& changes) {
  App::Impl& d = a.impl();
  for (auto& c : changes) {
    d.forceTables |= c.tables ? c.tables : TB_ALL;
    d.ackExternal[c.file] = fs::mtime(changedPath(d, c)).value_or(-1);
  }
  d.overwriteExternal = true;
}

std::string changeLines(const std::vector<io::ExternalChange>& changes) {
  std::vector<std::string> lines;
  for (auto& c : changes) {
    const char* kind = c.kind == io::ChangeKind::Removed ? "удалён" : c.kind == io::ChangeKind::Added ? "добавлен" : "изменён";
    lines.push_back(c.file + " — " + kind);
  }
  return join(lines, "\n");
}

// Ctrl+S, а файлы мира изменены другой программой: перечитать, сохранить с заменой или отменить.
struct SaveConflict : Dialog {
  std::string text, path;
  std::vector<io::ExternalChange> changes;
  const char* id() const override { return "external"; }
  Style style(App&) override { return {"Мир изменён другой программой", "refresh", ui::Tone::Warning, 520}; }
  bool draw(App& a) override {
    ui::text(text, ui::Font::Body, ui::Ink::Dim);
    ui::ModalFooter f;
    if (ui::button("Перечитать", {.icon = "refresh", .tooltip = "Открыть версию с диска — несохранённые изменения будут потеряны"})) {
      std::string p = path;
      a.impl().afterSave = nullptr;
      later(a, [p](App& x) { x.loadProject(p); });
      return false;
    }
    a.markUi("external.reload");
    if (ui::button("Отмена", {.isDefault = true})) {
      a.impl().afterSave = nullptr;
      return false;
    }
    a.markUi("external.cancel");
    if (ui::button("Сохранить с заменой", {.variant = ui::Variant::Danger, .icon = "save", .tooltip = "Записать вашу версию поверх изменений на диске"})) {
      ackChanges(a, changes);
      std::string p = path;
      later(a, [p](App& x) {
        if (x.projectPath() == p) x.saveTo(p);
      });
      return false;
    }
    a.markUi("external.overwrite");
    return true;
  }
  void dismissed(App& a) override { a.impl().afterSave = nullptr; }
};

void askSaveConflict(App& a, std::vector<io::ExternalChange> changes) {
  if (a.hasDialog("external")) return;
  auto dlg = std::make_unique<SaveConflict>();
  dlg->text = "Пока мир был открыт, другая программа изменила его файлы:\n" + changeLines(changes) +
              "\n\n«Перечитать» — открыть версию с диска; ваши несохранённые изменения будут потеряны.\n"
              "«Сохранить с заменой» — записать вашу версию поверх чужих изменений.";
  dlg->path = a.projectPath();
  dlg->changes = std::move(changes);
  a.openDialog(std::move(dlg));
}

std::string uniqueFolder(const std::string& parent, const std::string& name) {
  std::string base = fs::join(parent, sanitizeName(name));
  std::string p = base;
  for (int i = 2; fs::exists(p) && i < 1000; i++) p = base + " " + std::to_string(i);
  return p;
}

// Сохранение: мир, затем история ходов — несохранённые снимки из памяти (время создания сохраняется) и ветвь.
// В другое место (не текущий мир): история — только прежнего пути этого мира, чужая история места удаляется.
// quiet — автосохранение в папку мира: внешние изменения не затираются, а показываются.
bool saveImpl(App& a, const std::string& target, bool quiet) {
  App::Impl& d = a.impl();
  std::string path = fs::absolute(target);
  bool bundle = io::isBundlePath(path);
  bool same = path == d.path && bundle == d.bundle;
  std::string prevPath = d.path;
  if (same) {
    std::vector<io::ExternalChange> conflicts = freshChanges(a);
    if (!conflicts.empty()) {
      if (quiet) a.checkExternalChanges();
      else askSaveConflict(a, std::move(conflicts));
      return false;
    }
  }
  try {
    a.store.transact("Сохранить",
                     [&](Tx& tx) {
                       std::string now = nowIso();
                       tx.meta().updatedAt = now;
                       if (tx.meta().createdAt.empty()) tx.meta().createdAt = now;
                     },
                     TxOptions{.history = false});
    const World& w = a.store.world();
    std::vector<io::NewSnapshot> snaps = turnui::pendingSnapshots(a);
    const std::string historyFrom = same ? path : prevPath;
    if (bundle) {
      io::saveBundleWith(path, w, historyFrom, snaps, d.snapHead);
      d.files = io::FileState{};
      d.bundleStamp = io::stampFile(path);
      d.autoFiles.clear();
    } else {
      io::FileState st = same ? d.files : io::FileState{};
      u32 tables = same ? (a.store.dirtyTables() | d.fixedTables | d.forceTables) : TB_ALL;
      io::SaveOptions so;
      so.overwriteExternal = same && (d.overwriteExternal || d.forceTables != 0);
      // Версии, записанные автосохранением, — в свою очередь копий: ручные сохранения не вытесняются.
      if (same) so.autoBackups = d.autoFiles;
      io::SaveResult res = io::save(path, w, tables, &st, so);
      if (!same) {
        io::replaceHistory(path, prevPath);
        d.autoFiles.clear();
      }
      if (!snaps.empty() || !same || !d.diskHist || d.diskHist->head != d.snapHead) {
        if (!snaps.empty() || d.snapHead) io::addSnapshots(path, snaps, d.snapHead);
      }
      for (const std::string& f : res.written) {
        auto it = std::find(d.autoFiles.begin(), d.autoFiles.end(), f);
        if (quiet && it == d.autoFiles.end()) d.autoFiles.push_back(f);
        if (!quiet && it != d.autoFiles.end()) d.autoFiles.erase(it);
      }
      d.files = std::move(st);
      d.bundleStamp = io::FileStamp{};
    }
    turnui::historySaved(a);
  } catch (const std::exception& e) {
    d.afterSave = nullptr;
    if (quiet) {
      d.folderAutosaveBlocked = true;
      a.toast(std::string("Автосохранение в папку мира остановлено: ") + e.what(), ToastKind::Warning, "warning");
    } else {
      a.error(e);
    }
    return false;
  }
  a.store.markSaved();
  bool wasNew = prevPath.empty();
  d.path = path;
  d.bundle = bundle;
  d.fixedTables = d.forceTables = 0;
  d.overwriteExternal = false;
  d.ackExternal.clear();
  d.folderAutosaveBlocked = false;
  (quiet ? d.autosavedAt : d.savedAt) = a.store.world().meta->updatedAt;
  d.autosaveVersion = a.store.version();
  io::addRecent(path, a.worldTitle(), d.dataDir);
  a.waitBackground();   // запись автосохранения в фоне не должна появиться после очистки
  if (wasNew) io::clearAutosave({}, d.dataDir);
  io::clearAutosave(path, d.dataDir);
  d.recentDirty = true;
  d.recoveryDirty = true;
  writeThumbnail(a);
  if (!quiet) a.toast(same ? "Сохранено" : "Мир сохранён: " + fs::filename(path), ToastKind::Success, "check-circle");
  if (d.afterSave) {
    auto fn = std::move(d.afterSave);
    d.afterSave = nullptr;
    fn(a);
  }
  return true;
}

}  // namespace

// ================================================================ открытие
bool App::loadProject(const std::string& pathIn) {
  Impl& d = *d_;
  std::string path = pathIn;
  if (fs::isFile(path) && fs::filename(path) == "world.json") path = fs::parent(path);
  path = fs::absolute(path);
  io::LoadResult r;
  try {
    if (!fs::exists(path)) fail("Не найдено: " + path);
    if (!io::isProject(path)) fail("Здесь нет мира Regnum: нужна папка с world.json или файл .regnum");
    r = io::load(path);
  } catch (const std::exception& e) {
    error(e);
    return false;
  }
  waitBackground();
  resetProject(d);
  d.path = path;
  d.bundle = r.bundle;
  d.files = std::move(r.files);
  d.fixedTables = r.fixedTables;
  d.savedAt = r.world.meta->updatedAt;
  lastReport_.reset();
  closeDialogs();
  ui.viewTurn.reset();
  viewWorld_ = World{};
  store.replace(std::move(r.world), "Открыть мир", true);
  if (d.bundle) d.bundleStamp = io::stampFile(path);
  enterEditor(*this);
  turnui::syncHistory(*this);
  io::addRecent(path, worldTitle(), d.dataDir);
  d.recentDirty = true;
  if (!r.warnings.empty()) {
    std::vector<std::string> lines;
    for (size_t i = 0; i < r.warnings.size() && i < 200; i++) lines.push_back(r.warnings[i].text());
    if (r.warnings.size() > 200) lines.push_back("… и ещё " + std::to_string(r.warnings.size() - 200));
    message("Мир открыт с исправлениями",
            "Некоторые значения были вне допустимых пределов или ссылались на удалённые записи. Исправления попадут в файлы при сохранении.",
            std::move(lines), ToastKind::Warning);
  }
  // Автосохранение этого проекта новее последнего сохранения мира — предложить восстановить.
  for (const io::AutosaveInfo& info : io::listAutosaves(d.dataDir)) {
    if (info.project.empty() || fs::absolute(info.project) != path) continue;
    if (autosaveNewer(info, d.savedAt, path)) {
      io::AutosaveInfo copy = info;
      choose("Восстановить несохранённую работу?",
             "Найдено автосохранение от " + localTime(info.at) + " (ход " + std::to_string(info.turn) + "), оно новее последнего сохранения мира.",
             {"Отклонить", "Восстановить"},
             [copy](App& x, int i) {
               if (i == 1) restoreAutosave(x, copy);
               else if (i == 0) io::clearAutosave(copy.project, x.dataDir());
             },
             "history");
    }
    break;
  }
  return true;
}

void App::loadWorld(World w, std::string_view label) {
  Impl& d = *d_;
  waitBackground();
  resetProject(d);
  lastReport_.reset();
  closeDialogs();
  ui.viewTurn.reset();
  viewWorld_ = World{};
  store.replace(std::move(w), label, false);
  enterEditor(*this);
  turnui::syncHistory(*this);
}

void App::openPath(const std::string& path) {
  if (path.empty()) return;
  std::string p = path;
  whenSafe(*this, [p](App& a) { a.loadProject(p); });
}

void App::openWorldDialog() {
  pickPath(*this, BrowseMode::OpenWorld, "Открыть папку мира", {}, [](App& a, const std::string& p) {
    if (!io::isProject(p) && !(fs::isFile(p) && fs::filename(p) == "world.json")) {
      a.toast("В папке нет мира Regnum (world.json)", ToastKind::Warning, "folder");
      return;
    }
    a.openPath(p);
  });
}

void App::openBundleDialog() {
  pickPath(*this, BrowseMode::OpenBundle, "Открыть файл мира", {}, [](App& a, const std::string& p) { a.openPath(p); });
}

// ================================================================ новый мир
void App::newWorldDialog() {
  whenSafe(*this, [](App& a) { a.openDialog(detail::newWorldDialog(a)); });
}

bool App::newWorld(const std::string& nameIn, const std::string& folder) {
  Impl& d = *d_;
  std::string name = trim(nameIn);
  if (name.empty()) name = "Новый мир";
  World w;
  try {
    if (!d.basemap) fail("Базовая карта не найдена (assets/basemap) — новый мир создать нельзя");
    if (!folder.empty()) {
      if (fs::exists(folder) && !fs::isDir(folder)) fail("«" + folder + "» — файл, а не папка");
      if (io::isProject(folder)) fail("В папке «" + folder + "» уже есть мир");
      if (fs::isDir(folder) && !fs::list(folder).empty()) fail("Папка «" + folder + "» не пуста — выберите пустую или новую");
    }
    geo::Coast coast = d.basemap->coast();
    World base = rg::newWorld(name);
    Tx tx(base);
    geo::initFromCoast(tx, coast);
    std::string now = nowIso();
    tx.meta().createdAt = now;
    tx.meta().updatedAt = now;
    tx.meta().basemap = d.basemap->id();
    w = std::move(tx).finish();
  } catch (const std::exception& e) {
    error(e);
    return false;
  }
  loadWorld(std::move(w), "Новый мир");
  if (!folder.empty()) {
    if (!saveTo(folder)) return false;
  }
  toast("Мир «" + name + "» создан. Включите правку границ (E), чтобы нарисовать провинции.", ToastKind::Success, "sparkles");
  return true;
}

// ================================================================ сохранение
bool App::save() {
  if (ui.screen != Screen::Editor) return false;
  Impl& d = *d_;
  if (d.path.empty()) {
    saveAs();
    return false;
  }
  // Мир не изменён, но есть несохранённые снимки ходов (например, возврат к сохранённому ходу) — записать и их.
  bool pendingHistory = std::any_of(d.memSnaps.begin(), d.memSnaps.end(), [](const MemSnapshot& m) { return m.kind != io::kSnapStart; });
  if (!dirty() && io::isProject(d.path) && !pendingHistory) {
    toast("Всё уже сохранено", ToastKind::Info, "check");
    if (d.afterSave) {
      auto fn = std::move(d.afterSave);
      d.afterSave = nullptr;
      fn(*this);
    }
    return true;
  }
  return saveTo(d.path);
}

bool App::saveTo(const std::string& path) {
  if (ui.screen != Screen::Editor) return false;
  if (path.empty()) return false;
  return saveImpl(*this, path, false);
}

void App::saveAs() {
  if (ui.screen != Screen::Editor) return;
  openDialog(saveAsDialog(*this));
}

// ================================================================ закрытие
void App::closeWorld() {
  if (ui.screen != Screen::Editor) return;
  whenSafe(*this, [](App& a) {
    App::Impl& d = a.impl();
    a.waitBackground();
    io::clearAutosave(d.path, d.dataDir);
    resetProject(d);
    a.closeDialogs();
    a.ui.viewTurn.reset();
    a.ui.sel = a.ui.hover = {};
    a.ui.editor.clear();
    a.store.replace(rg::newWorld(""), "Закрыть мир", true);
    a.ui.screen = Screen::Start;
    d.recentDirty = d.recoveryDirty = true;
    a.requestRedraw();
  });
}

void App::requestQuit() {
  whenSafe(*this, [](App& a) {
    App::Impl& d = a.impl();
    a.waitBackground();
    if (a.ui.screen == Screen::Editor) io::clearAutosave(d.path, d.dataDir);
    if (d.prefsDirty) savePrefs(a);
    platform::quit(0);
  });
}

// ================================================================ внешние изменения
void App::checkExternalChanges() {
  Impl& d = *d_;
  if (ui.screen != Screen::Editor || d.path.empty()) return;
  if (hasDialog("external")) return;
  // Папка — файлы мира, архив .regnum — сам файл (размер, время, CRC).
  std::vector<io::ExternalChange> fresh = freshChanges(*this);
  if (fresh.empty()) return;
  std::string text = d.bundle ? "Файл мира изменён другой программой." : "Файлы мира изменены другой программой.";
  if (dirty()) text += " Перечитав их, вы потеряете несохранённые изменения.";
  text += "\n" + changeLines(fresh);
  std::string path = d.path;
  struct Ext : Dialog {
    std::string text, path;
    std::vector<io::ExternalChange> changes;
    const char* id() const override { return "external"; }
    Style style(App&) override { return {"Мир изменён на диске", "refresh", ui::Tone::Warning, 480}; }
    void keep(App& a) {
      if (a.projectPath() == path) ackChanges(a, changes);
    }
    bool draw(App& a) override {
      ui::text(text, ui::Font::Body, ui::Ink::Dim);
      ui::ModalFooter f;
      if (ui::button("Оставить мои")) {
        keep(a);
        a.toast("При сохранении файлы на диске будут заменены вашей версией", ToastKind::Info, "save");
        return false;
      }
      a.markUi("external.keep");
      if (ui::button("Перечитать с диска", {.variant = ui::Variant::Primary, .icon = "refresh", .isDefault = true})) {
        std::string p = path;
        later(a, [p](App& x) { x.loadProject(p); });
        return false;
      }
      a.markUi("external.reload");
      return true;
    }
    void dismissed(App& a) override { keep(a); }
  };
  auto dlg = std::make_unique<Ext>();
  dlg->text = std::move(text);
  dlg->path = path;
  dlg->changes = std::move(fresh);
  openDialog(std::move(dlg));
}

// ================================================================ автосохранение
void App::autosaveNow() {
  Impl& d = *d_;
  if (ui.screen != Screen::Editor) return;
  d.lastAutosave = time_;
  d.autosaveVersion = store.version();
  // Автосохранение в папку мира — обычное сохранение (с историей ходов): копия в данных пользователя не нужна.
  // Не вышло (ошибка записи, внешние изменения) — копия для восстановления после сбоя.
  const Settings& s = *store.world().settings;
  if (s.autosaveFolder && !d.path.empty() && !d.folderAutosaveBlocked && store.dirty() && !readOnly() && saveImpl(*this, d.path, true)) return;
  if (!d.autosaveJob.valid()) {
    World w = store.world();
    std::string proj = d.path, dir = d.dataDir;
    d.autosaveJob = jobs::submit([w, proj, dir]() -> std::string {
      try {
        io::writeAutosave(w, proj, dir);
        return {};
      } catch (const std::exception& e) {
        return e.what();
      }
    });
  }
}

// Будильник автосохранения: отдельный спящий поток (не занимает пул jobs), будит цикл событий platform::wake.
void App::Impl::WakeTimer::arm(double seconds) {
  auto when = std::chrono::steady_clock::now() +
              std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(std::max(0.0, seconds)));
  {
    std::lock_guard<std::mutex> lk(m);
    if (armed && due <= when) return;   // и так разбудит не позже
    due = when;
    armed = true;
  }
  if (th.joinable()) {
    cv.notify_one();
    return;
  }
  th = std::thread([this] {
    std::unique_lock<std::mutex> lk(m);
    while (!stop) {
      if (!armed) {
        cv.wait(lk);
        continue;
      }
      cv.wait_until(lk, due);
      if (!stop && armed && std::chrono::steady_clock::now() >= due) {
        armed = false;
        lk.unlock();
        platform::wake();
        lk.lock();
      }
    }
  });
}

App::Impl::WakeTimer::~WakeTimer() {
  {
    std::lock_guard<std::mutex> lk(m);
    stop = true;
  }
  cv.notify_one();
  if (th.joinable()) th.join();
}

namespace detail {

void autosaveTick(App& a) {
  App::Impl& d = a.impl();
  if (d.autosaveJob.valid() && d.autosaveJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    std::string err;
    try {
      err = d.autosaveJob.get();
    } catch (const std::exception& e) {
      err = e.what();
    }
    if (!err.empty()) a.toast("Не удалось записать автосохранение: " + err, ToastKind::Warning, "warning");
    else if (d.path.empty() || !a.store.world().settings->autosaveFolder) d.autosavedAt = nowIso();
    d.recoveryDirty = true;
  }
  if (a.store.version() == d.autosaveVersion) return;
  if (!a.dirty()) {
    d.autosaveVersion = a.store.version();
    d.lastAutosave = a.time();
    return;
  }
  double period = clamp(double(a.store.world().settings->autosaveSec), 10.0, 3600.0);
  double left = period - (a.time() - d.lastAutosave);
  if (left <= 0) a.autosaveNow();
  else d.wakeTimer.arm(left + 0.05);   // окно может простаивать — кадр к сроку
}

// ================================================================ миниатюры и данные экрана запуска
std::string thumbPath(const std::string& dataDir, const std::string& project) {
  return fs::join(dataDir, "thumbs/" + strf("%016llx", (unsigned long long)hash64(fs::absolute(project))) + ".png");
}

void writeThumbnail(App& a) {
  App::Impl& d = a.impl();
  if (!d.basemap || d.path.empty()) return;
  try {
    static gfx::Image base;
    static std::string baseFrom;
    if (baseFrom != d.basemap->thumbPath()) {
      baseFrom = d.basemap->thumbPath();
      base = gfx::Image();
      if (auto img = loadImage(baseFrom)) {
        gfx::Image t = img->scaled(320, 180);
        for (u32& p : t.px) {
          float r = float((p >> 16) & 255) / 255.f, g = float((p >> 8) & 255) / 255.f;
          float k = clamp((r + g) * 0.5f, 0.f, 1.f);
          p = gfx::premul(Color::mix(Color::hex(0x1c2a3d), Color::hex(0xe8e2d4), k));
        }
        base = std::move(t);
      }
    }
    gfx::Image img = base.empty() ? gfx::Image(320, 180, gfx::premul(Color::hex(0x1c2a3d))) : base;
    gfx::Canvas c(img);
    const World& w = a.store.world();
    auto faces = geo::faces(w);
    float k = float(img.w) / float(schema::kMapWidth);
    for (const geo::Face& f : faces->faces) {
      if (!f.province) continue;
      const Province* p = w.province(f.province);
      if (!p || p->sea) continue;
      const Faction* own = w.faction(p->owner);
      Color col = own ? own->color : Color::hex(0xb9b2a3);
      gfx::Path path;
      for (const auto& ring : f.rings) {
        if (ring.size() < 3) continue;
        path.moveTo(float(ring[0].x) * k, float(ring[0].y) * k);
        for (size_t i = 1; i < ring.size(); i++) path.lineTo(float(ring[i].x) * k, float(ring[i].y) * k);
        path.close();
      }
      c.fillPath(path, gfx::Paint(col.alpha(own ? 0.78f : 0.35f)), gfx::FillRule::EvenOdd);
    }
    codec::RgbaImage out;
    out.w = img.w;
    out.h = img.h;
    out.rgba = img.toRgba();
    std::string file = thumbPath(d.dataDir, d.path);
    fs::makeDirs(fs::parent(file));
    codec::writePngFile(file, out, 6);
    d.thumbs.erase(d.path);
  } catch (const std::exception& e) {
    logWarn("Миниатюра мира не записана: %s", e.what());
  }
}

void refreshStartData(App& a) {
  App::Impl& d = a.impl();
  if (d.recentDirty) {
    d.recent = io::recentProjects(d.dataDir);
    d.recentDirty = false;
  }
  if (d.recoveryDirty) {
    d.recovery.clear();
    for (const io::AutosaveInfo& info : io::listAutosaves(d.dataDir)) {
      if (info.project.empty()) {
        d.recovery.push_back(info);
        continue;
      }
      if (!io::isProject(info.project)) continue;
      // Новее последнего сохранения мира на диске (время из world.json, а не время файла).
      if (autosaveNewer(info, io::savedTime(info.project), info.project)) d.recovery.push_back(info);
    }
    d.recoveryDirty = false;
  }
}

void restoreAutosave(App& a, const io::AutosaveInfo& info) {
  App::Impl& d = a.impl();
  std::string err;
  auto as = io::loadAutosaveFor(info.project, d.dataDir, &err);
  if (!as) {
    a.toast("Автосохранение не читается: " + err, ToastKind::Danger, "error");
    io::clearAutosave(info.project, d.dataDir);
    d.recoveryDirty = true;
    return;
  }
  if (!info.project.empty() && io::isProject(info.project) && a.loadProject(info.project)) {
    // Файлы проекта — сохранённое состояние, автосохранение — несохранённые изменения поверх него.
    a.closeDialogs();
    a.store.replace(std::move(as->world), "Восстановить автосохранение", false);
    d.fitPending = true;
    // Ветвь и начало хода — для восстановленного мира (его ход может быть новее сохранённого).
    d.memSnaps.clear();
    turnui::syncHistory(a);
  } else {
    a.loadWorld(std::move(as->world), "Восстановить автосохранение");
  }
  d.recoveryDirty = true;
  a.toast("Несохранённая работа восстановлена", ToastKind::Success, "history");
}

// ================================================================ выбор пути
void pickPath(App& a, BrowseMode mode, std::string title, std::string startDir, std::function<void(App&, const std::string&)> onPick,
              std::string defaultName, std::function<void(App&)> onCancel) {
  App::Impl& d = a.impl();
  if (startDir.empty()) startDir = !d.browserDir.empty() && fs::isDir(d.browserDir) ? d.browserDir : fs::documentsDir();
  bool native = platform::dialogsSupported() && !d.builtinBrowser;
  if (!native) {
    a.openDialog(browserDialog(mode, std::move(title), std::move(startDir), std::move(onPick), std::move(defaultName), std::move(onCancel)));
    return;
  }
  later(a, [mode, title, startDir, onPick, defaultName, onCancel](App& x) {
    std::optional<std::string> r;
    std::vector<platform::FileFilter> worlds{{"Миры Regnum", {"regnum"}}};
    switch (mode) {
      case BrowseMode::OpenWorld:
      case BrowseMode::PickFolder: r = platform::pickFolderDialog(title, startDir); break;
      case BrowseMode::OpenBundle: r = platform::openFileDialog(title, worlds, startDir); break;
      case BrowseMode::SaveBundle: r = platform::saveFileDialog(title, worlds, startDir, defaultName); break;
    }
    if (!r || r->empty()) {
      if (onCancel) onCancel(x);
      return;
    }
    std::string p = *r;
    if (mode == BrowseMode::SaveBundle && !io::isBundlePath(p)) p += io::kBundleExt;
    x.impl().browserDir = mode == BrowseMode::OpenWorld || mode == BrowseMode::PickFolder ? fs::parent(p) : fs::parent(p);
    x.impl().prefsDirty = true;
    onPick(x, p);
  });
}

// Папка для сохранения по выбору пользователя: пустая — сама; мир — сама; иначе — вложенная по названию мира.
std::string folderTarget(App& a, const std::string& picked) {
  if (!fs::exists(picked) || (fs::isDir(picked) && fs::list(picked).empty())) return picked;
  if (io::isProject(picked)) return picked;
  return uniqueFolder(picked, a.worldTitle());
}

std::string newWorldFolder(const std::string& parent, const std::string& name) { return uniqueFolder(parent, name); }

}  // namespace detail
}  // namespace rg::app
