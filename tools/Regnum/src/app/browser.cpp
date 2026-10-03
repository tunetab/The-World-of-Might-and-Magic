// Regnum — встроенный проводник (Linux без системных диалогов и по выбору в настройках): хлебные крошки,
// папки и файлы со значками, диски Windows, «Домашняя» и «Документы», новая папка, фильтр .regnum и
// папок миров (world.json), клавиатура: ↑↓, Enter — открыть, Backspace — вверх.
#include <filesystem>

#include "app/app_internal.h"
#include "base/fs.h"

namespace rg::app::detail {

using platform::Key;

namespace {

struct Entry {
  std::string name, path;
  bool dir = false;
  bool world = false;    // папка с world.json
  bool bundle = false;   // файл .regnum
  u64 size = 0;
  i64 mtime = 0;
};

bool isRoot(const std::string& dir) {
  std::filesystem::path p = fs::path(dir);
  return !p.has_relative_path();
}

std::string parentOf(const std::string& dir) {
  if (isRoot(dir)) return dir;
  std::filesystem::path p = fs::path(dir).parent_path();
  std::string r = fs::fromPath(p);
  if (r.size() == 2 && r[1] == ':') r += "/";   // «C:» -> «C:/»
  return r.empty() ? dir : r;
}

std::string fmtSize(u64 n) {
  if (n < 1024) return fmtInt(i64(n)) + " Б";
  if (n < 1024 * 1024) return fmtNum(double(n) / 1024.0, n < 10 * 1024 ? 1 : 0) + " КБ";
  return fmtNum(double(n) / (1024.0 * 1024.0), 1) + " МБ";
}

struct Browser : Dialog {
  BrowseMode mode = BrowseMode::OpenWorld;
  std::string title, dir, fileName;
  std::function<void(App&, const std::string&)> onPick;
  std::function<void(App&)> onCancel;
  bool decided = false;
  std::vector<Entry> entries;
  std::string listed;
  int sel = -1;
  bool scrollToSel = false;   // прокрутить к выбранной записи один раз (клавиши, новая папка), а не каждый кадр
  bool creating = false;
  std::string newFolder;
  struct Place {
    std::string label, path;
    const char* icon;
  };
  std::vector<Place> places;

  const char* id() const override { return "browser"; }
  Style style(App&) override { return {title, mode == BrowseMode::SaveBundle ? "save" : "folder-open", ui::Tone::Accent, 780}; }

  void setDir(std::string d) {
    d = fs::absolute(d);
    if (!fs::isDir(d)) return;
    dir = d;
    sel = -1;
    creating = false;
  }

  void list() {
    entries.clear();
    for (const fs::DirEntry& e : fs::list(dir)) {
      if (e.name.empty() || e.name[0] == '.' || e.name == "$RECYCLE.BIN" || e.name == "System Volume Information") continue;
      Entry x;
      x.name = e.name;
      x.path = e.path;
      x.dir = e.dir;
      x.size = e.size;
      x.mtime = e.mtime;
      if (e.dir) x.world = fs::isFile(fs::join(e.path, "world.json"));
      else {
        x.bundle = io::isBundlePath(e.path);
        if (!x.bundle) continue;   // показываются только папки и архивы миров
        if (mode == BrowseMode::OpenWorld || mode == BrowseMode::PickFolder) continue;
      }
      entries.push_back(std::move(x));
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
      if (a.dir != b.dir) return a.dir;
      return compareRu(a.name, b.name) < 0;
    });
    listed = dir;
  }

  void pick(App& a, const std::string& path) {
    decided = true;
    a.impl().browserDir = dir;
    a.impl().prefsDirty = true;
    if (onPick) {
      auto fn = onPick;
      later(a, [fn, path](App& x) { fn(x, path); });
    }
  }

  // Двойной щелчок или Enter по записи. true — диалог закрыт выбором.
  bool activate(App& a, const Entry& e) {
    if (e.dir) {
      if (mode == BrowseMode::OpenWorld && e.world) {
        pick(a, e.path);
        return true;
      }
      setDir(e.path);
      return false;
    }
    if (e.bundle && mode == BrowseMode::OpenBundle) {
      pick(a, e.path);
      return true;
    }
    if (e.bundle && mode == BrowseMode::SaveBundle) fileName = e.name;
    return false;
  }

  void crumbs(App& a) {
    // Сегменты пути: «C:/», «Users», ... или «/», «home», ...
    std::vector<std::pair<std::string, std::string>> segs;
    std::string acc;
    std::vector<std::string> parts = split(dir, '/');
    for (size_t i = 0; i < parts.size(); i++) {
      if (i == 0) {
        acc = parts[0].empty() ? "/" : parts[0] + "/";
        segs.push_back({parts[0].empty() ? std::string("/") : parts[0], acc});
        continue;
      }
      if (parts[i].empty()) continue;
      acc = fs::join(acc, parts[i]);
      segs.push_back({parts[i], acc});
    }
    size_t from = segs.size() > 4 ? segs.size() - 4 : 0;
    if (from > 0) {
      ui::label("…", {.ink = ui::Ink::Muted});
      ui::icon("chevron-right", ui::Ink::Muted, 14);
    }
    for (size_t i = from; i < segs.size(); i++) {
      ui::IdScope s{int(i)};
      bool last = i + 1 == segs.size();
      if (ui::button(segs[i].first, {.variant = last ? ui::Variant::Secondary : ui::Variant::Ghost, .size = ui::Size::Small})) setDir(segs[i].second);
      if (!last) ui::icon("chevron-right", ui::Ink::Muted, 14);
    }
    (void)a;
  }

  std::string chosenPath() const {
    switch (mode) {
      case BrowseMode::OpenWorld:
        if (sel >= 0 && sel < int(entries.size()) && entries[size_t(sel)].world) return entries[size_t(sel)].path;
        return fs::isFile(fs::join(dir, "world.json")) ? dir : std::string();
      case BrowseMode::PickFolder:
        if (sel >= 0 && sel < int(entries.size()) && entries[size_t(sel)].dir) return entries[size_t(sel)].path;
        return dir;
      case BrowseMode::OpenBundle:
        if (sel >= 0 && sel < int(entries.size()) && entries[size_t(sel)].bundle) return entries[size_t(sel)].path;
        return {};
      case BrowseMode::SaveBundle: {
        std::string n = trim(fileName);
        if (n.empty()) return {};
        if (!io::isBundlePath(n)) n += io::kBundleExt;
        return fs::join(dir, sanitizeName(n.substr(0, n.size() - 7)) + io::kBundleExt);
      }
    }
    return {};
  }

  bool draw(App& a) override {
    const ui::Theme& th = ui::theme();
    if (listed != dir) list();
    bool typing = ui::wantsKeyboard();
    // Клавиатура списка
    if (!typing) {
      if (ui::keyPressed(Key::Down)) {
        ui::consumeKey(Key::Down);
        sel = std::min(int(entries.size()) - 1, sel + 1);
        scrollToSel = true;
      }
      if (ui::keyPressed(Key::Up)) {
        ui::consumeKey(Key::Up);
        sel = std::max(entries.empty() ? -1 : 0, sel - 1);
        scrollToSel = true;
      }
      if (ui::keyPressed(Key::Backspace)) {
        ui::consumeKey(Key::Backspace);
        setDir(parentOf(dir));
        list();
      }
      if (sel >= 0 && sel < int(entries.size()) && ui::keyPressed(Key::Enter) && entries[size_t(sel)].dir && mode != BrowseMode::PickFolder) {
        ui::consumeKey(Key::Enter);
        Entry e = entries[size_t(sel)];
        if (activate(a, e)) return false;
        list();
      }
    }
    // Панель пути
    {
      ui::HStack hs(30, ui::Align::Left, 4);
      {
        ui::Disabled dis(isRoot(dir));
        if (ui::iconButton("arrow-up", "Вверх")) {
          setDir(parentOf(dir));
          list();
        }
        ui::tooltip("Вверх", {Key::Backspace, 0});
      }
      a.markUi("browser.up");
      crumbs(a);
      ui::flex();
      if (ui::iconButton("folder", "Новая папка")) {
        creating = !creating;
        newFolder = "Новая папка";
      }
      a.markUi("browser.newfolder");
    }
    if (creating) {
      ui::Row r({ui::fr(1), ui::px(100), ui::px(32)}, 30, 8);
      ui::textField("newfolder", newFolder, {.icon = "folder", .live = true, .maxLength = 80, .autofocus = true, .selectAllOnFocus = true});
      std::string n = sanitizeName(trim(newFolder));
      if (ui::button("Создать", {.variant = ui::Variant::Secondary, .fill = true})) {
        std::string p = fs::join(dir, n);
        std::string err;
        if (fs::exists(p)) a.toast("Папка «" + n + "» уже есть", ToastKind::Warning, "folder");
        else if (!fs::makeDirs(p, &err)) a.toast("Не удалось создать папку: " + err, ToastKind::Danger, "error");
        else {
          creating = false;
          list();
          for (size_t i = 0; i < entries.size(); i++)
            if (entries[i].path == fs::absolute(p)) {
              sel = int(i);
              scrollToSel = true;
            }
        }
      }
      if (ui::iconButton("close", "Отмена")) creating = false;
    }
    // Места и список
    {
      ui::Row r({ui::px(176), ui::fr(1)}, 360, 12);
      {
        ui::Group g(176, 2);
        ui::caption("Места");
        for (size_t i = 0; i < places.size(); i++) {
          ui::IdScope s(int(i) + 1000);
          bool cur = fs::absolute(places[i].path) == dir;
          if (ui::listItem(places[i].label, {.icon = places[i].icon, .selected = cur})) {
            setDir(places[i].path);
            list();
          }
        }
      }
      {
        RectF lr = ui::next(360);
        ui::draw::rect(lr, th.dark ? th.bg.alpha(0.5f) : th.surface2, th.radiusCard);
        ui::draw::rectStroke(lr, th.border, th.radiusCard, 1);
        ui::Area ar(lr.inset(4), 0);
        if (entries.empty()) {
          ui::spacer(110);
          ui::label(mode == BrowseMode::OpenBundle ? "Здесь нет файлов .regnum" : "Папка пуста", {.ink = ui::Ink::Muted, .align = ui::Align::Center});
        } else {
          ui::VirtualList vl("list", int(entries.size()), 32, 352);
          if (scrollToSel && sel >= 0) vl.scrollToRow(sel);
          scrollToSel = false;
          for (int i : vl) {
            const Entry& e = entries[size_t(i)];
            std::string hint = e.dir ? (e.world ? std::string("мир") : std::string()) : fmtSize(e.size);
            const char* ic = e.dir ? (e.world ? "map" : "folder") : "archive";
            bool clicked = ui::listItem(e.name, {.icon = ic, .hint = hint, .selected = sel == i});
            bool dbl = ui::lastItem().doubleClicked;
            if (e.world || e.bundle) {
              RectF ir = ui::lastItem().rect;
              ui::draw::rect(RectF{ir.x + 2, ir.y + 8, 2, ir.h - 16}, th.accent, 1);
            }
            if (clicked) {
              sel = i;
              if (e.bundle && mode == BrowseMode::SaveBundle) fileName = e.name;
            }
            if (dbl) {
              Entry copy = e;
              if (activate(a, copy)) return false;
              list();
              break;
            }
          }
        }
      }
    }
    a.markUi("browser.list");
    if (mode == BrowseMode::SaveBundle) {
      ui::prop("Имя файла", "file");
      ui::textField("name", fileName, {.placeholder = "мир.regnum", .live = true, .maxLength = 120});
      a.markUi("browser.filename");
    }
    std::string chosen = chosenPath();
    ui::ModalFooter f;
    if (ui::button("Отмена")) {
      cancel(a);
      return false;
    }
    const char* okText = mode == BrowseMode::SaveBundle ? "Сохранить" : mode == BrowseMode::PickFolder ? "Выбрать папку" : "Открыть";
    if (ui::button(okText, {.variant = ui::Variant::Primary, .disabled = chosen.empty(), .isDefault = true}) && !chosen.empty()) {
      if (mode == BrowseMode::SaveBundle && fs::exists(chosen)) {
        std::string c = chosen;
        auto fn = onPick;
        decided = true;
        a.impl().browserDir = dir;
        a.openDialog(confirmDialog("Заменить файл?", "Файл «" + fs::filename(c) + "» уже существует и будет заменён.", "Заменить", true,
                                   [fn, c](App& x) {
                                     if (fn) fn(x, c);
                                   },
                                   onCancel));
        return false;
      }
      pick(a, chosen);
      return false;
    }
    a.markUi("browser.ok");
    return true;
  }

  void cancel(App& a) {
    if (decided) return;
    decided = true;
    if (onCancel) {
      auto fn = onCancel;
      later(a, fn);
    }
  }
  void dismissed(App& a) override { cancel(a); }
};

}  // namespace

std::unique_ptr<Dialog> browserDialog(BrowseMode mode, std::string title, std::string startDir, std::function<void(App&, const std::string&)> onPick,
                                      std::string defaultName, std::function<void(App&)> onCancel) {
  auto b = std::make_unique<Browser>();
  b->mode = mode;
  b->title = std::move(title);
  b->onPick = std::move(onPick);
  b->onCancel = std::move(onCancel);
  b->fileName = std::move(defaultName);
  std::string start = !startDir.empty() && fs::isDir(startDir) ? startDir : fs::documentsDir();
  if (!fs::isDir(start)) start = fs::homeDir();
  b->setDir(start);
  b->places.push_back({"Домашняя", fs::homeDir(), "home"});
  std::string docs = fs::documentsDir();
  if (fs::isDir(docs) && fs::absolute(docs) != fs::absolute(fs::homeDir())) b->places.push_back({"Документы", docs, "file"});
  std::string desk = fs::join(fs::homeDir(), "Desktop");
  if (fs::isDir(desk)) b->places.push_back({"Рабочий стол", desk, "image"});
  // Диски (на Windows существуют пути «C:/»; на других системах их нет).
  for (char c = 'C'; c <= 'Z'; c++) {
    std::string root = std::string(1, c) + ":/";
    if (fs::isDir(root)) b->places.push_back({std::string(1, c) + ":", root, "archive"});
  }
  if (fs::isDir("/") && !fs::isDir("C:/")) b->places.push_back({"Корень", "/", "folder"});
  return b;
}

}  // namespace rg::app::detail
