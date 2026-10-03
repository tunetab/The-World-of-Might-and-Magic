// Regnum — общие диалоги оболочки: подтверждение, выбор из нескольких действий, запрос строки, сообщение
// со списком, новый мир (название и папка), «Сохранить как» (папка или файл .regnum).
#include "app/app_internal.h"
#include "base/fs.h"

namespace rg::app::detail {

namespace {

struct Confirm : Dialog {
  std::string title, text, ok;
  bool danger = false;
  std::function<void(App&)> onYes, onNo;
  bool decided = false;
  const char* id() const override { return "confirm"; }
  void no(App& a) {
    if (decided) return;
    decided = true;
    if (onNo) later(a, onNo);
  }
  void dismissed(App& a) override { no(a); }
  Style style(App&) override { return {title, danger ? "trash" : "help", danger ? ui::Tone::Danger : ui::Tone::Accent, 440}; }
  bool draw(App& a) override {
    if (!text.empty()) ui::text(text, ui::Font::Body, ui::Ink::Dim);
    ui::ModalFooter f;
    if (ui::button("Отмена")) {
      no(a);
      return false;
    }
    a.markUi("dialog.cancel");
    if (ui::button(ok, {.variant = danger ? ui::Variant::Danger : ui::Variant::Primary, .isDefault = true})) {
      decided = true;
      if (onYes) later(a, onYes);
      return false;
    }
    a.markUi("dialog.ok");
    return true;
  }
};

struct Choice : Dialog {
  std::string title, text;
  std::vector<std::string> buttons;
  std::function<void(App&, int)> onPick;
  const char* icon = "help";
  bool firstGhost = false;
  bool decided = false;
  const char* id() const override { return "choice"; }
  Style style(App&) override { return {title, icon, ui::Tone::Accent, 480}; }
  void pick(App& a, int i) {
    decided = true;
    if (onPick) {
      auto fn = onPick;
      later(a, [fn, i](App& x) { fn(x, i); });
    }
  }
  bool draw(App& a) override {
    if (!text.empty()) ui::text(text, ui::Font::Body, ui::Ink::Dim);
    ui::ModalFooter f;
    for (size_t i = 0; i < buttons.size(); i++) {
      bool last = i + 1 == buttons.size();
      ui::ButtonOpt o;
      o.variant = last ? ui::Variant::Primary : (i == 0 && firstGhost ? ui::Variant::Ghost : ui::Variant::Secondary);
      o.isDefault = last;
      ui::IdScope s{int(i)};
      if (ui::button(buttons[i], o)) {
        pick(a, int(i));
        return false;
      }
      a.markUi("dialog.button." + std::to_string(i));
    }
    return true;
  }
  void dismissed(App& a) override {
    if (!decided && onPick) {
      auto fn = onPick;
      later(a, [fn](App& x) { fn(x, -1); });
    }
  }
};

struct Prompt : Dialog {
  std::string title, label, value;
  std::function<void(App&, const std::string&)> onOk;
  const char* id() const override { return "prompt"; }
  Style style(App&) override { return {title, "edit", ui::Tone::Accent, 420}; }
  bool draw(App& a) override {
    if (!label.empty()) ui::label(label, {.font = ui::Font::Small, .ink = ui::Ink::Dim});
    ui::textField("value", value, {.live = true, .maxLength = 160, .autofocus = true, .selectAllOnFocus = true});
    a.markUi("dialog.field");
    ui::ModalFooter f;
    if (ui::button("Отмена")) return false;
    std::string v = trim(value);
    if (ui::button("Готово", {.variant = ui::Variant::Primary, .disabled = v.empty(), .isDefault = true}) && !v.empty()) {
      if (onOk) {
        auto fn = onOk;
        later(a, [fn, v](App& x) { fn(x, v); });
      }
      return false;
    }
    a.markUi("dialog.ok");
    return true;
  }
};

struct Message : Dialog {
  std::string title, text;
  std::vector<std::string> lines;
  ToastKind kind = ToastKind::Info;
  const char* id() const override { return "message"; }
  Style style(App&) override {
    const char* ic = kind == ToastKind::Danger ? "error" : kind == ToastKind::Warning ? "warning" : kind == ToastKind::Success ? "check-circle" : "info";
    ui::Tone t = kind == ToastKind::Danger ? ui::Tone::Danger : kind == ToastKind::Warning ? ui::Tone::Warning : kind == ToastKind::Success ? ui::Tone::Success : ui::Tone::Info;
    return {title, ic, t, lines.empty() ? 440.f : 560.f};
  }
  bool draw(App& a) override {
    if (!text.empty()) ui::text(text, ui::Font::Body, ui::Ink::Dim);
    if (!lines.empty()) {
      float h = std::min(260.f, float(lines.size()) * 22 + 16);
      ui::Card c({.pad = 8});
      ui::Scroll sc("lines", h);
      for (size_t i = 0; i < lines.size(); i++) {
        ui::IdScope s{int(i)};
        ui::label(lines[i], {.font = ui::Font::Small, .ink = ui::Ink::Dim, .wrap = true});
      }
    }
    ui::ModalFooter f;
    if (!lines.empty() && ui::button("Скопировать", {.icon = "copy"})) {
      platform::setClipboardText(join(lines, "\n"));
      a.toast("Скопировано", ToastKind::Success, "copy");
    }
    if (ui::button("Понятно", {.variant = ui::Variant::Primary, .isDefault = true})) return false;
    a.markUi("dialog.ok");
    return true;
  }
};

// Новый мир: название и папка.
struct NewWorld : Dialog {
  std::string name = "Новый мир";
  std::shared_ptr<std::string> parent = std::make_shared<std::string>();
  const char* id() const override { return "world.new"; }
  Style style(App&) override { return {"Новый мир", "plus", ui::Tone::Accent, 500}; }
  bool draw(App& a) override {
    ui::text("Береговая линия возьмётся с базовой карты. Провинции, государства и гильдии вы добавите сами.", ui::Font::Body, ui::Ink::Dim);
    ui::spacer(4);
    ui::caption("Название");
    ui::textField("name", name, {.placeholder = "Название мира", .live = true, .maxLength = 80, .autofocus = true, .selectAllOnFocus = true});
    a.markUi("newworld.name");
    ui::spacer(2);
    ui::caption("Расположение");
    {
      ui::Row r({ui::fr(1), ui::px(110)}, 30, 8);
      RectF pr = ui::next(30);
      const ui::Theme& th = ui::theme();
      ui::draw::rect(pr, th.surface3, th.radiusField);
      ui::at(RectF{pr.x + 8, pr.y, pr.w - 16, pr.h});
      ui::label(*parent, {.font = ui::Font::Small, .ink = ui::Ink::Dim, .icon = "folder", .tooltip = *parent});
      if (ui::button("Изменить…", {.icon = "folder-open", .fill = true})) {
        auto p = parent;
        pickPath(a, BrowseMode::PickFolder, "Папка для нового мира", *parent, [p](App&, const std::string& dir) { *p = dir; });
      }
      a.markUi("newworld.browse");
    }
    std::string clean = trim(name);
    std::string target = newWorldFolder(*parent, clean.empty() ? std::string("Новый мир") : clean);
    ui::label("Будет создана папка «" + fs::filename(target) + "»", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "info"});
    ui::ModalFooter f;
    if (ui::button("Отмена")) return false;
    if (ui::button("Создать", {.variant = ui::Variant::Primary, .icon = "check", .disabled = clean.empty(), .isDefault = true}) && !clean.empty()) {
      a.impl().browserDir = *parent;
      a.impl().prefsDirty = true;
      later(a, [clean, target](App& x) { x.newWorld(clean, target); });
      return false;
    }
    a.markUi("newworld.create");
    return true;
  }
};

// «Сохранить как»: папка (удобно для Git) или один файл.
struct SaveAs : Dialog {
  int kind = 0;   // 0 папка, 1 файл
  bool chosen = false;
  const char* id() const override { return "world.saveas"; }
  Style style(App&) override { return {"Сохранить мир", "save-as", ui::Tone::Accent, 520}; }
  bool option(App& a, int k, const char* icon, std::string_view title, std::string_view hint) {
    const ui::Theme& th = ui::theme();
    RectF r = ui::next(64);
    ui::WidgetId wid = ui::id(i64(k) + 100);
    ui::Interaction it = ui::interact(wid, r, ui::IfFocusable);
    bool on = kind == k;
    float hv = ui::animate(wid ^ 1, it.hovered ? 1.f : 0.f);
    ui::draw::rect(r, on ? th.accent.alpha(0.10f) : Color::mix(th.surface2, th.surface3, hv * 0.6f), th.radiusCard);
    ui::draw::rectStroke(r, on ? th.accent.alpha(0.7f) : Color::mix(th.border, th.borderStrong, hv), th.radiusCard, 1);
    RectF ic{r.x + 14, r.cy() - 18, 36, 36};
    ui::draw::rect(ic, on ? th.accent : th.surface3, 10);
    ui::draw::icon(icon, ic.inset(9), on ? th.onAccent : th.textDim);
    ui::draw::text(title, RectF{r.x + 62, r.y + 12, r.w - 100, 20}, ui::Font::Strong, th.text);
    ui::draw::text(hint, RectF{r.x + 62, r.y + 33, r.w - 100, 18}, ui::Font::Small, th.textMuted);
    RectF rb{r.right() - 32, r.cy() - 9, 18, 18};
    ui::draw::ring(rb.cx(), rb.cy(), 8, 1.5f, on ? th.accent : th.borderStrong);
    if (on) ui::draw::circle(rb.cx(), rb.cy(), 4.5f, th.accent);
    if (it.focused) ui::draw::rectStroke(r.expand(3), th.accent, th.radiusCard + 3, 2);
    a.markUi(k == 0 ? "saveas.folder" : "saveas.bundle", r);
    if (it.clicked) kind = k;
    return it.doubleClicked;
  }
  bool draw(App& a) override {
    bool go = false;
    go |= option(a, 0, "folder", "Папка мира", "Каждая таблица — отдельный файл JSON: удобно для Git");
    go |= option(a, 1, "archive", "Файл .regnum", "Один файл для обмена и резервных копий");
    ui::ModalFooter f;
    if (ui::button("Отмена")) return false;
    if (ui::button("Выбрать место…", {.variant = ui::Variant::Primary, .icon = "folder-open", .isDefault = true}) || go) {
      chosen = true;
      std::string name = sanitizeName(a.worldTitle());
      auto cancel = [](App& x) { x.impl().afterSave = nullptr; };
      if (kind == 0) {
        pickPath(a, BrowseMode::PickFolder, "Папка для мира", {},
                 [](App& x, const std::string& picked) {
                   std::string target = folderTarget(x, picked);
                   if (io::isProject(target) && fs::absolute(target) != x.projectPath()) {
                     x.openDialog(confirmDialog("Заменить мир?", "В папке «" + fs::filename(target) + "» уже есть мир. Его файлы будут заменены.",
                                                "Заменить", true, [target](App& y) { y.saveTo(target); },
                                                [](App& y) { y.impl().afterSave = nullptr; }));
                     return;
                   }
                   x.saveTo(target);
                 },
                 {}, cancel);
      } else {
        pickPath(a, BrowseMode::SaveBundle, "Сохранить мир в файл", {}, [](App& x, const std::string& p) { x.saveTo(p); }, name + io::kBundleExt, cancel);
      }
      return false;
    }
    a.markUi("saveas.choose");
    return true;
  }
  void dismissed(App& a) override {
    if (!chosen) a.impl().afterSave = nullptr;
  }
};

}  // namespace

std::unique_ptr<Dialog> confirmDialog(std::string title, std::string text, std::string ok, bool danger, std::function<void(App&)> onYes,
                                      std::function<void(App&)> onNo) {
  auto d = std::make_unique<Confirm>();
  d->onNo = std::move(onNo);
  d->title = std::move(title);
  d->text = std::move(text);
  d->ok = ok.empty() ? std::string("Да") : std::move(ok);
  d->danger = danger;
  d->onYes = std::move(onYes);
  return d;
}

std::unique_ptr<Dialog> choiceDialog(std::string title, std::string text, std::vector<std::string> buttons, std::function<void(App&, int)> onPick,
                                     const char* icon, bool firstGhost) {
  auto d = std::make_unique<Choice>();
  d->title = std::move(title);
  d->text = std::move(text);
  d->buttons = std::move(buttons);
  if (d->buttons.empty()) d->buttons.push_back("Понятно");
  d->onPick = std::move(onPick);
  d->icon = icon ? icon : "help";
  d->firstGhost = firstGhost;
  return d;
}

std::unique_ptr<Dialog> promptDialog(std::string title, std::string label, std::string initial, std::function<void(App&, const std::string&)> onOk) {
  auto d = std::make_unique<Prompt>();
  d->title = std::move(title);
  d->label = std::move(label);
  d->value = std::move(initial);
  d->onOk = std::move(onOk);
  return d;
}

std::unique_ptr<Dialog> messageDialog(std::string title, std::string text, std::vector<std::string> lines, ToastKind kind) {
  auto d = std::make_unique<Message>();
  d->title = std::move(title);
  d->text = std::move(text);
  d->lines = std::move(lines);
  d->kind = kind;
  return d;
}

std::unique_ptr<Dialog> newWorldDialog(App& a) {
  auto d = std::make_unique<NewWorld>();
  const std::string& bd = a.impl().browserDir;
  *d->parent = !bd.empty() && fs::isDir(bd) ? bd : fs::join(fs::documentsDir(), "Regnum");
  return d;
}

std::unique_ptr<Dialog> saveAsDialog(App&) { return std::make_unique<SaveAs>(); }

}  // namespace rg::app::detail
