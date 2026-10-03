// Regnum — хроника мира (выдвижная панель «Хроника»): все записи World::log по ходам (новые сверху), значки и
// цвета видов, фильтры (вид, фракция, диапазон ходов, поиск), щелчок по записи — выделить и показать объект;
// «Записать событие» — заметка текущего хода (rules::addLog, LogKind::Note). ТЗ: механика хода фиксирует все
// зависящие от хода происшествия на карте.
#include "app/app_internal.h"
#include "app/dialogs/turn_ui.h"
#include "app/widgets.h"

namespace rg::app {

namespace {

// ---------------------------------------------------------------- фильтр
struct Filter {
  std::string query;
  u32 kinds = 0;       // биты LogKind; 0 — все виды
  Id faction = 0;
  int from = 0, to = 0;  // 0 — без границы
  int limit = 120;     // сколько записей показано
  bool active() const { return kinds || faction || from || to; }
  void reset() {
    kinds = 0;
    faction = 0;
    from = to = 0;
    limit = 120;
  }
};
Filter& filter() {
  static Filter f;
  return f;
}

bool matches(const World& w, const LogEntry& e, const Filter& f) {
  if (f.kinds && int(e.kind) < 32 && !((f.kinds >> int(e.kind)) & 1u)) return false;
  if (f.from && e.turn < f.from) return false;
  if (f.to && e.turn > f.to) return false;
  if (f.faction) {
    bool hit = std::find(e.factions.begin(), e.factions.end(), f.faction) != e.factions.end();
    if (!hit && e.province)
      if (const Province* p = w.province(e.province)) hit = p->owner == f.faction;
    if (!hit) return false;
  }
  if (!trim(f.query).empty()) {
    if (utf8::matches(e.text, f.query)) return true;
    if (e.province && utf8::matches(w.provinceName(e.province), f.query)) return true;
    for (Id fid : e.factions)
      if (utf8::matches(w.factionName(fid), f.query)) return true;
    return false;
  }
  return true;
}

// Отфильтрованные записи (новые ходы сверху, внутри хода — новые сверху); кеш по таблице хроники и фильтру.
struct Cache {
  Table<LogEntry> log;
  bool valid = false;
  u64 sig = 0;
  std::vector<Id> ids;
  std::map<int, int> perTurn;
};
Cache& cache() {
  static Cache c;
  return c;
}

const Cache& entries(const World& w) {
  Cache& c = cache();
  const Filter& f = filter();
  u64 s = hash64(f.query);
  s = hashMix(s, f.kinds);
  s = hashMix(s, f.faction);
  s = hashMix(s, u64(u32(f.from)) << 32 | u32(f.to));
  if (c.valid && c.sig == s && c.log.same(w.log)) return c;
  std::vector<const LogEntry*> v;
  w.log.each([&](const LogEntry& e) {
    if (matches(w, e, f)) v.push_back(&e);
  });
  std::sort(v.begin(), v.end(), [](const LogEntry* a, const LogEntry* b) { return a->turn != b->turn ? a->turn > b->turn : a->id > b->id; });
  c.ids.clear();
  c.perTurn.clear();
  for (const LogEntry* e : v) {
    c.ids.push_back(e->id);
    c.perTurn[e->turn]++;
  }
  c.log = w.log;
  c.sig = s;
  c.valid = true;
  return c;
}

// Другой мир или приложение — фильтры сбрасываются.
void syncSession(App& a) {
  static u64 tag = 0;
  u64 t = turnui::sessionTag(a);
  if (t == tag) return;
  tag = t;
  filter().reset();
  filter().query.clear();
  cache().valid = false;
}

// ---------------------------------------------------------------- запись в хронику
struct NoteDlg : Dialog {
  std::string text;
  Id faction = 0, province = 0;
  const char* id() const override { return "chronicle.note"; }
  Style style(App&) override { return {"Запись в хронике", "quill", ui::Tone::Accent, 520}; }
  bool draw(App& a) override {
    int turn = a.store.world().turn();
    ui::label("Ход " + std::to_string(turn) + " · заметка ведущего", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "hourglass"});
    ui::textArea("text", text, 120, {.placeholder = "Что произошло?", .live = true, .maxLength = 2000, .autofocus = true});
    a.markUi("note.text");
    ui::caption("Связать с объектом");
    {
      ui::Row r({ui::fr(1), ui::fr(1)}, 30, 8);
      w::factionPicker("fac", faction, w::FactionFilter::Any, "Без фракции");
      a.markUi("note.faction");
      w::provincePicker("prov", province, 0, "Без провинции");
      a.markUi("note.province");
    }
    ui::ModalFooter f;
    if (ui::button("Отмена")) return false;
    std::string t = trim(text);
    if (ui::button("Записать", {.variant = ui::Variant::Primary, .icon = "quill", .disabled = t.empty() || a.readOnly()})) {
      rules::LogRefs refs;
      refs.province = province;
      if (faction) refs.factions.push_back(faction);
      if (a.act("Запись в хронике", [&](Tx& tx) { rules::addLog(tx, LogKind::Note, t, refs); })) {
        a.toast("Записано в хронику хода " + std::to_string(turn), ToastKind::Success, "quill");
        return false;
      }
    }
    a.markUi("note.ok");
    return true;
  }
};

void openNote(App& a) {
  if (a.readOnly()) {
    a.toast("Открыт прошлый ход — записи недоступны", ToastKind::Warning, "lock", "К текущему ходу", [](App& x) { x.backToCurrent(); });
    return;
  }
  a.openDialog(std::make_unique<NoteDlg>());
}

DialogReg noteReg({"chronicle.note", [](App& a, Id) -> std::unique_ptr<Dialog> {
                     if (a.readOnly()) return nullptr;
                     return std::make_unique<NoteDlg>();
                   }});

// ---------------------------------------------------------------- фильтры
void filtersPopup(App& a, const World& w) {
  Filter& f = filter();
  if (!ui::beginPopup("filters", {.side = ui::Side::Below, .width = 320, .pad = 12})) return;
  int current = w.turn();
  ui::caption("Вид записи");
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 28, 6);
    for (int k = 0; k < int(LogKind::Count); k++) {
      ui::IdScope s(k);
      bool on = (f.kinds >> k) & 1u;
      if (ui::chip(turnui::logName(LogKind(k)), {.icon = turnui::logIcon(LogKind(k)), .selected = on, .clickable = true}) == ui::ChipAction::Click) {
        f.kinds ^= 1u << k;
        f.limit = 120;
      }
      a.markUi("chronicle.kind." + std::to_string(k));
    }
  }
  ui::caption("Фракция");
  if (w::factionPicker("fac", f.faction, w::FactionFilter::Any, "Все фракции")) f.limit = 120;
  a.markUi("chronicle.faction");
  ui::caption("Ходы");
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 30, 8);
    int from = f.from ? f.from : 1, to = f.to ? f.to : current;
    if (ui::numberField("from", from, {.min = 1, .max = double(current), .label = "с"})) {
      f.from = from <= 1 ? 0 : from;
      if (f.to && f.from > f.to) f.to = f.from;
    }
    a.markUi("chronicle.from");
    if (ui::numberField("to", to, {.min = 1, .max = double(current), .label = "по"})) {
      f.to = to >= current ? 0 : to;
      if (f.to && f.from > f.to) f.from = f.to;
    }
    a.markUi("chronicle.to");
  }
  ui::spacer(2);
  {
    ui::HStack hs(28, ui::Align::Left, 8);
    if (f.from != current || f.to != current) {
      if (ui::button("Этот ход", {.variant = ui::Variant::Secondary, .icon = "hourglass", .size = ui::Size::Small})) f.from = f.to = current;
    }
    ui::flex();
    if (ui::button("Сбросить", {.variant = ui::Variant::Ghost, .icon = "close", .size = ui::Size::Small, .disabled = !f.active()})) f.reset();
  }
  ui::endPopup();
}

// Фишки действующих фильтров (крестик снимает).
void activeChips(const World& w) {
  Filter& f = filter();
  if (!f.active()) return;
  ui::HStack hs(26, ui::Align::Left, 6);
  if (f.kinds) {
    int n = 0, one = 0;
    for (int k = 0; k < int(LogKind::Count); k++)
      if ((f.kinds >> k) & 1u) {
        n++;
        one = k;
      }
    std::string label = n == 1 ? std::string(turnui::logName(LogKind(one))) : "Видов: " + std::to_string(n);
    if (ui::chip(label, {.icon = n == 1 ? turnui::logIcon(LogKind(one)) : "filter", .removable = true}) == ui::ChipAction::Remove) f.kinds = 0;
  }
  if (f.faction) {
    const Faction* fa = w.faction(f.faction);
    if (ui::chip(fa ? fa->name : std::string("—"), {.color = fa ? fa->color : Color(0, 0, 0, 0), .removable = true}) == ui::ChipAction::Remove) f.faction = 0;
  }
  if (f.from || f.to) {
    std::string label = f.from && f.from == f.to ? "Ход " + std::to_string(f.from)
                        : "Ходы " + std::to_string(f.from ? f.from : 1) + "–" + (f.to ? std::to_string(f.to) : std::to_string(w.turn()));
    if (ui::chip(label, {.icon = "hourglass", .removable = true}) == ui::ChipAction::Remove) f.from = f.to = 0;
  }
}

// ---------------------------------------------------------------- панель
void drawChronicle(App& a) {
  syncSession(a);
  const World& w = a.world();
  Filter& f = filter();
  {
    ui::Row r({ui::fr(1), ui::px(30), ui::px(30)}, 30, 6);
    if (ui::searchField("q", f.query, "Поиск в хронике")) f.limit = 120;
    a.markUi("chronicle.search");
    if (ui::iconButton("filter", "Фильтры: вид, фракция, ходы", {.toggled = f.active(), .badge = f.active()})) ui::openPopup("filters");
    a.markUi("chronicle.filter");
    filtersPopup(a, w);
    if (ui::iconButton("quill", "Записать событие", {.disabled = a.readOnly()})) openNote(a);
    a.markUi("chronicle.note");
  }
  activeChips(w);
  const Cache& c = entries(w);
  if (w.log.empty()) {
    ui::spacer(16);
    if (ui::emptyState("chronicle", "Хроника пуста: события появятся по ходу игры.", a.readOnly() ? std::string_view() : "Записать событие", "quill"))
      openNote(a);
    return;
  }
  if (c.ids.empty()) {
    ui::spacer(16);
    if (ui::emptyState("search", "Ничего не найдено.", "Сбросить фильтры", "close")) {
      f.reset();
      f.query.clear();
    }
    return;
  }
  {
    ui::HStack hs(18, ui::Align::Left, 6);
    std::string n = fmtInt(i64(c.ids.size())) + " " + plural(i64(c.ids.size()), "запись", "записи", "записей");
    if (c.ids.size() != w.log.size()) n += " из " + fmtInt(i64(w.log.size()));
    ui::label(n, {.font = ui::Font::Small, .ink = ui::Ink::Muted});
  }
  int shown = std::min<int>(int(c.ids.size()), std::max(20, f.limit));
  int turn = std::numeric_limits<int>::min();
  int current = a.store.world().turn();
  const LogEntry* pick = nullptr;
  for (int i = 0; i < shown; i++) {
    const LogEntry* e = w.log.get(c.ids[size_t(i)]);
    if (!e) continue;
    if (e->turn != turn) {
      turn = e->turn;
      ui::IdScope s(i64(turn) + 0x7000000);
      if (i > 0) ui::spacer(4);
      ui::HStack hs(24, ui::Align::Left, 8);
      ui::label("Ход " + std::to_string(turn), {.font = ui::Font::Subtitle});
      if (turn == current) ui::badge("текущий", ui::Tone::Accent);
      ui::flex();
      auto it = c.perTurn.find(turn);
      int n = it == c.perTurn.end() ? 0 : it->second;
      ui::label(std::to_string(n), {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "chronicle"});
    }
    if (turnui::logRow(w, *e)) pick = e;
    a.markUi("chronicle.entry." + std::to_string(e->id));
  }
  if (int(c.ids.size()) > shown) {
    ui::spacer(4);
    int more = std::min(120, int(c.ids.size()) - shown);
    if (ui::button("Показать ещё " + std::to_string(more), {.variant = ui::Variant::Secondary, .icon = "chevron-down", .fill = true})) f.limit = shown + 120;
    a.markUi("chronicle.more");
  }
  if (pick) {
    LogEntry e = *pick;
    detail::later(a, [e](App& x) { turnui::focusEntry(x, e); });
  }
}

DrawerReg drawer({"chronicle", "chronicle", "Хроника", 70, drawChronicle, "Ctrl+9"});

bool inEditor(App& a) { return a.ui.screen == Screen::Editor; }
CommandReg cmdOpen({"chronicle.open", "Хроника", "chronicle", nullptr, [](App& a) { turnui::showChronicle(a, -1); }, inEditor, false, "Ход"});
CommandReg cmdNote({"chronicle.note", "Записать событие в хронику", "quill", nullptr, [](App& a) { openNote(a); },
                    [](App& a) { return inEditor(a) && !a.readOnly(); }, false, "Ход"});

}  // namespace

// turn < 0 — открыть, не трогая фильтры; 0 — без фильтров; иначе — записи этого хода.
void turnui::showChronicle(App& a, int turn) {
  syncSession(a);
  Filter& f = filter();
  if (turn >= 0) {
    f.reset();
    f.query.clear();
    if (turn > 0) f.from = f.to = turn;
  }
  if (!a.ui.editor.empty()) a.closeEditor();
  if (a.ui.drawer != "chronicle") a.openDrawer("chronicle");
}

}  // namespace rg::app
