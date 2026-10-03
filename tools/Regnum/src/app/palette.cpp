// Regnum — палитра команд (Ctrl+K: команды и сущности мира — выбор и показ на карте) и справка F1.
#include "app/app_internal.h"

namespace rg::app::detail {

using platform::Key;

namespace {

struct Palette : Dialog {
  struct Item {
    const CommandDef* cmd = nullptr;
    Selection sel;
    std::string label, sub, kind;
    const char* icon = nullptr;
    Color dot{0, 0, 0, 0};
    std::string keys;
    int score = 0;
  };
  std::string q;
  std::string builtQ = "\x01";
  u64 builtVersion = 0;
  std::vector<Item> items;
  int hi = 0;
  bool scrollToHi = true;   // прокрутить к подсвеченной строке один раз — после смены её клавишами или нового поиска
  bool focused = false;
  float lastMx = -1, lastMy = -1;

  const char* id() const override { return "palette"; }
  Style style(App&) override {
    Style s;
    s.width = 640;
    s.closeButton = false;
    s.dismissOnBackdrop = true;
    return s;
  }

  static int scoreOf(std::string_view label, const std::string& key) {
    if (key.empty()) return 0;
    std::string l = utf8::searchKey(label);
    if (startsWith(l, key)) return 3;
    if (l.find(" " + key) != std::string::npos) return 2;
    return 1;
  }

  void rebuild(App& a) {
    items.clear();
    std::string key = utf8::searchKey(q);
    for (auto& c : commands()) {
      if (!c.run) continue;
      if (c.enabled && !c.enabled(a)) continue;
      if (!key.empty() && !utf8::matches(c.title, q)) continue;
      Item it;
      it.cmd = &c;
      it.label = c.title;
      it.icon = c.icon;
      it.kind = "команда";
      it.keys = c.shortcut ? shortcutLabel(c.shortcut) : std::string();
      it.score = scoreOf(c.title, key) + 1;
      items.push_back(std::move(it));
    }
    if (a.ui.screen == Screen::Editor && !key.empty()) {
      const World& w = a.world();
      auto add = [&](Selection s, std::string label, std::string sub, std::string kind, const char* icon, Color dot) {
        if (!utf8::matches(label, q)) return;
        Item it;
        it.sel = s;
        it.label = std::move(label);
        it.sub = std::move(sub);
        it.kind = std::move(kind);
        it.icon = icon;
        it.dot = dot;
        it.score = scoreOf(it.label, key);
        items.push_back(std::move(it));
      };
      w.factions.each([&](const Faction& f) {
        add({SelType::Faction, f.id}, entityName(w, {SelType::Faction, f.id}), f.isGuild() ? "Торговая гильдия" : "Государство", f.isGuild() ? "гильдия" : "государство",
            f.isGuild() ? "guild" : "crown", f.color);
      });
      w.provinces.each([&](const Province& p) {
        const Faction* own = w.faction(p.owner);
        add({SelType::Province, p.id}, entityName(w, {SelType::Province, p.id}), own ? own->name : std::string(p.sea ? "Море" : "Без владельца"), "провинция",
            p.sea ? "sea" : "province", own ? own->color : Color(0, 0, 0, 0));
      });
      w.characters.each([&](const Character& c) {
        add({SelType::Character, c.id}, entityName(w, {SelType::Character, c.id}), c.title.empty() ? w.factionName(c.faction) : c.title + " · " + w.factionName(c.faction),
            "персонаж", "character", Color(0, 0, 0, 0));
      });
      w.armies.each([&](const Army& ar) {
        const Faction* f = w.faction(ar.leader());
        add({SelType::Army, ar.id}, entityName(w, {SelType::Army, ar.id}), f ? f->name : std::string(), ar.isFleet() ? "флот" : "войско",
            ar.isFleet() ? "fleet" : "army", f ? f->color : Color(0, 0, 0, 0));
      });
      w.routes.each([&](const Route& r) {
        add({SelType::Route, r.id}, entityName(w, {SelType::Route, r.id}), w.factionName(r.guild), "маршрут", "route", Color(0, 0, 0, 0));
      });
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.score > y.score; });
    if (items.size() > 400) items.resize(400);
    builtQ = q;
    builtVersion = a.store.version();
    hi = 0;
    scrollToHi = true;
  }

  void run(App& a, const Item& it) {
    if (it.cmd) {
      std::string id = it.cmd->id;
      later(a, [id](App& x) { runCommand(x, id); });
    } else if (it.sel) {
      Selection s = it.sel;
      later(a, [s](App& x) { x.select(s, true); });
    }
  }

  bool draw(App& a) override {
    const ui::Theme& th = ui::theme();
    if (ui::keyPressed(Key::Escape)) {
      ui::consumeKey(Key::Escape);
      return false;
    }
    if (q != builtQ || builtVersion != a.store.version()) rebuild(a);
    int n = int(items.size());
    const ui::Mouse& mo = ui::mouse();
    bool mouseMoved = std::fabs(mo.x - lastMx) > 0.5f || std::fabs(mo.y - lastMy) > 0.5f;
    if (lastMx < 0) mouseMoved = false;
    lastMx = mo.x;
    lastMy = mo.y;
    if (ui::keyPressed(Key::Down)) {
      ui::consumeKey(Key::Down);
      if (n) hi = (hi + 1) % n;
      scrollToHi = true;
    }
    if (ui::keyPressed(Key::Up)) {
      ui::consumeKey(Key::Up);
      if (n) hi = (hi - 1 + n) % n;
      scrollToHi = true;
    }
    bool enter = false;
    if (ui::keyPressed(Key::Enter)) {
      ui::consumeKey(Key::Enter);
      enter = true;
    }
    if (!focused) {
      ui::setKeyboardFocus(ui::id("q"));
      focused = true;
    }
    ui::searchField("q", q, a.ui.screen == Screen::Editor ? "Команда, провинция, государство, персонаж…" : "Команда…");
    a.markUi("palette.search");
    if (q != builtQ) rebuild(a);
    n = int(items.size());
    if (enter && hi >= 0 && hi < n) {
      run(a, items[size_t(hi)]);
      return false;
    }
    if (n == 0) {
      ui::spacer(8);
      ui::label("Ничего не найдено", {.ink = ui::Ink::Muted, .align = ui::Align::Center});
      ui::spacer(8);
    } else {
      float rowH = 40;
      float h = std::min(float(n) * rowH, 420.f);
      ui::VirtualList vl("results", n, rowH, h);
      if (scrollToHi && hi >= 0) vl.scrollToRow(hi);
      scrollToHi = false;
      for (int i : vl) {
        const Item& it = items[size_t(i)];
        RectF r = ui::next(rowH);
        ui::WidgetId wid = ui::id("row");
        ui::Interaction in = ui::interact(wid, r);
        if (in.hovered && mouseMoved) hi = i;
        bool active = i == hi;
        if (active) ui::draw::rect(r, th.accent.alpha(th.dark ? 0.14f : 0.12f), 8);
        else if (in.hovered) ui::draw::rect(r, th.hover, 8);
        if (active) ui::draw::rect(RectF{r.x, r.y + 9, 3, r.h - 18}, th.accent, 1.5f);
        RectF ic{r.x + 12, r.cy() - 9, 18, 18};
        if (it.dot.a > 0) {
          ui::draw::circle(ic.cx(), ic.cy(), 5, it.dot);
        } else {
          ui::draw::icon(it.icon ? it.icon : "command", ic, active ? th.accent : th.textDim);
        }
        float tx = r.x + 42;
        float right = r.right() - 12;
        if (!it.keys.empty()) {
          float kw = ui::measure(it.keys, ui::Font::Caption) + 16;
          RectF kr{right - kw, r.cy() - 10, kw, 20};
          ui::draw::rect(kr, th.surface3, 5);
          ui::draw::rectStroke(kr, th.border, 5, 1);
          ui::draw::text(it.keys, kr, ui::Font::Caption, th.textDim, ui::Align::Center);
          right = kr.x - 8;
        }
        float kindW = ui::measure(it.kind, ui::Font::Caption) + 4;
        ui::draw::text(it.kind, RectF{right - kindW, r.y, kindW, r.h}, ui::Font::Caption, th.textMuted, ui::Align::Right);
        right -= kindW + 10;
        if (it.sub.empty()) {
          ui::draw::text(it.label, RectF{tx, r.y, right - tx, r.h}, ui::Font::Body, th.text);
        } else {
          float lw = std::min(ui::measure(it.label, ui::Font::Body) + 2, (right - tx) * 0.62f);
          ui::draw::text(it.label, RectF{tx, r.y, lw, r.h}, ui::Font::Body, th.text);
          ui::draw::text(it.sub, RectF{tx + lw + 10, r.y, right - tx - lw - 10, r.h}, ui::Font::Small, th.textMuted);
        }
        if (in.clicked) {
          run(a, it);
          return false;
        }
      }
    }
    ui::separator();
    ui::label("↑↓ — выбор · Enter — выполнить · Esc — закрыть", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
    return true;
  }
};

struct Help : Dialog {
  const char* id() const override { return "help"; }
  Style style(App&) override {
    Style s;
    s.title = "Сочетания клавиш";
    s.icon = "keyboard";
    s.width = 780;
    s.dismissOnBackdrop = true;
    return s;
  }
  static void row(std::string_view what, std::string_view keys) {
    ui::Row r({ui::fr(1), ui::px(150)}, 26, 8);
    ui::label(what, {.ink = ui::Ink::Dim});
    ui::Shortcut sc = parseShortcut(keys);
    ui::HStack hs(24, ui::Align::Right, 4);
    if (sc && keys != "+" && keys != "-") ui::kbd(sc);
    else ui::kbd(keys);
  }
  bool draw(App& a) override {
    if (ui::keyPressed(Key::F1)) {
      ui::consumeKey(Key::F1);
      return false;
    }
    std::vector<std::string> groups;
    for (auto& c : commands()) {
      std::string g = c.group ? c.group : "Общие";
      if (c.shortcut && *c.shortcut && std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
    }
    ui::Scroll sc("body", std::min(560.f, ui::viewport().h - 180));
    ui::Row cols({ui::fr(1), ui::fr(1)}, ui::kAuto, 28);
    {
      ui::Group g(0, 2);
      for (auto& gname : groups) {
        ui::IdScope s(gname);
        ui::caption(gname);
        for (auto& c : commands()) {
          if (!c.shortcut || !*c.shortcut) continue;
          if ((c.group ? std::string(c.group) : std::string("Общие")) != gname) continue;
          ui::IdScope cs(c.id);
          row(c.title, c.shortcut);
        }
        ui::spacer(6);
      }
    }
    {
      ui::Group g(0, 2);
      ui::caption("Карта");
      row("Масштаб", "Колесо");
      row("Перемещение", "Пробел+мышь");
      row("Перемещение", "Средняя кнопка");
      row("Показать объект", "Двойной щелчок");
      row("Снять выделение", "Esc");
      ui::spacer(6);
      ui::caption("Инструменты");
      for (auto& t : toolDefs()) {
        if (!t.shortcut || !*t.shortcut) continue;
        ui::IdScope ts(int(t.id) + 500);
        row(t.editMode ? std::string(t.title) + " · правка" : std::string(t.title), t.shortcut);
      }
      ui::spacer(6);
      ui::caption("Панели");
      for (auto& d : drawers()) {
        if (!d.shortcut) continue;
        ui::IdScope ds(d.id);
        row(d.title, d.shortcut);
      }
      row("Поле ввода: отменить правку", "Esc");
      row("Следующее поле", "Tab");
    }
    (void)a;
    return true;
  }
};

}  // namespace

std::unique_ptr<Dialog> paletteDialog() { return std::make_unique<Palette>(); }
std::unique_ptr<Dialog> helpDialog() { return std::make_unique<Help>(); }

}  // namespace rg::app::detail
