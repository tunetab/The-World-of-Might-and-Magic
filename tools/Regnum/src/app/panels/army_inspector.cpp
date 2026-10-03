// Regnum — инспектор войска и флота (ТЗ 1.c.iii–v): фигурка, название, флаги фракций; состав по фракциям
// (союзное войско — отдельные плитки), отряды из резерва своей фракции, герои и главный полководец
// (флотоводец), кнопки «Разделить», «Распустить союз», «Расформировать»; провинция под объектом; события.
#include "app/panels/military.h"

namespace rg::app::mil {

namespace {

const char* leaderWord(const Army& a) { return a.isFleet() ? "Главный флотоводец" : "Главный полководец"; }

// ---------------------------------------------------------------- шапка
void drawHeader(App& a, Id id) {
  const World& w = frameWorld(a);
  const Army* ar = w.army(id);
  if (!ar) return;
  ui::Row r({ui::px(54), ui::fr(1)}, ui::kAuto, 10);
  figure(w, *ar, 54, false);
  ui::Group g(0, 2);
  std::string cap = objectCaption(*ar);
  std::string name = objectName(*ar);
  // Название совпадает с видом («Союзное войско») — в подписи провинция.
  if (utf8::searchKey(name) == utf8::searchKey(cap)) {
    Id pid = provinceUnder(w, ar->pos);
    cap = pid ? w.provinceName(pid) : std::string(ar->isFleet() ? "Открытое море" : "Вне провинций");
  }
  ui::caption(cap);
  {
    float aw = ui::avail().w;
    float nw = std::min(ui::measure(name, ui::Font::Heading) + 2, std::max(40.f, aw - 34));
    ui::Row nr({ui::px(nw), ui::px(26)}, 32, 4);
    ui::label(name, {.font = ui::Font::Heading});
    ui::Disabled dis(a.readOnly());
    if (ui::iconButton("edit", "Переименовать", {.size = ui::Size::Small, .shortcut = {platform::Key::F2, 0}})) {
      bool fleet = ar->isFleet();
      a.prompt(fleet ? "Переименовать флот" : "Переименовать войско", "Название", ar->name, [id, fleet](App& x, const std::string& v) {
        x.act(fleet ? "Переименовать флот" : "Переименовать войско", [&](Tx& tx) { rules::renameArmy(tx, id, v); });
      });
    }
    a.markUi("army.rename");
  }
  std::vector<Id> fs = factionsIn(*ar);
  if (fs.size() == 1) {
    factionLabel(w, fs[0], 22);
  } else {
    ui::HStack hs(24, ui::Align::Left, 6);
    for (Id f : fs) {
      ui::IdScope s{i64(f)};
      factionFlag(w, f, 26, 17);
    }
    ui::label(std::to_string(fs.size()) + " " + plural(i64(fs.size()), "фракция", "фракции", "фракций") + " в союзе",
              {.font = ui::Font::Small, .ink = ui::Ink::Muted});
  }
}

// ---------------------------------------------------------------- отряды группы
struct AddUnit {
  int row = -1;
  i64 count = 0;
  Id lastRow = 0;
};

void setCount(App& a, const Army& ar, Id faction, Id row, i64 n) {
  Id id = ar.id;
  a.act(ar.isFleet() ? "Корабли во флоте" : "Отряды в войске", [&](Tx& tx) { rules::setUnits(tx, id, faction, row, n); },
        {.coalesce = "units:" + std::to_string(id) + ":" + std::to_string(row)});
}

void unitsTable(App& a, const Army& ar, const ArmyGroup& g) {
  const World& w = frameWorld(a);
  bool fleet = ar.isFleet();
  bool ro = a.readOnly();
  auto c = rules::calc(w);
  struct L {
    UnitRow row;
    i64 count;
    i64 reserve;
  };
  std::vector<L> lines;
  for (const ArmyUnit& u : g.units) {
    auto r = unitRow(w, g.faction, u.row, fleet);
    if (!r) continue;
    const rules::RowCalc* rc = rowCalc(*c, g.faction, u.row, fleet);
    lines.push_back(L{*r, u.count, rc ? std::max<i64>(0, rc->reserve) : 0});
  }
  if (!lines.empty()) {
    ui::Column cols[] = {{fleet ? "Судно" : "Отряд", nullptr, ui::fr(1, 100)},
                         {"Численность", nullptr, ui::px(100), ui::Align::Left, false, "Назначено из резерва фракции"},
                         {"Резерв", nullptr, ui::px(64), ui::Align::Right, false, "Свободно в резерве фракции"},
                         {"", nullptr, ui::px(36)}};
    ui::Table t("units", cols, int(lines.size()), {.rowHeight = 40, .selectable = false});
    for (int i : t) {
      const L& l = lines[size_t(i)];
      RectF cr = t.cell();
      unitCell(cr, l.row, false, l.row.name != l.row.typeName ? std::string_view(l.row.typeName) : std::string_view());
      ui::Disabled dis(ro);
      t.cell();
      i64 n = l.count;
      if (ui::numberField("n", n, {.min = 0, .max = double(l.count + l.reserve),
                                   .tooltip = "Не больше резерва: " + fmtCount(l.reserve) + " свободно"}))
        setCount(a, ar, g.faction, l.row.id, n);
      a.markUi("army.unit." + std::to_string(g.faction) + "." + std::to_string(l.row.id));
      t.text(fmtCount(l.reserve), l.reserve > 0 ? ui::Ink::Success : ui::Ink::Muted);
      t.cell();
      if (ui::iconButton("close", fleet ? "Вернуть корабли в резерв" : "Вернуть отряд в резерв")) setCount(a, ar, g.faction, l.row.id, 0);
    }
    if (t.footer()) {
      t.text("Итого");
      t.text(fmtCount(groupCount(g)));
    }
  }
  // Добавить строку из резерва своей фракции (ТЗ 1.c.iii: только из списка государства и не больше резерва).
  std::vector<UnitRow> all = unitRows(w, g.faction, fleet);
  std::vector<UnitRow> avail;
  for (const UnitRow& r : all)
    if (rowCount(g, r.id) == 0) avail.push_back(r);
  ui::Disabled dis(ro);
  if (all.empty()) {
    ui::HStack hs(0, ui::Align::Left, 6);
    ui::label(std::string("В таблице ") + (fleet ? "флота" : "войск") + " фракции нет строк", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "info"});
    if (ui::link("Открыть", fleet ? "fleet" : "army")) {
      a.ui.tabOf[SelType::Faction] = fleet ? "faction.fleet" : "faction.army";
      a.select(SelType::Faction, g.faction);
    }
    return;
  }
  if (avail.empty()) return;
  if (lines.empty())
    ui::label(fleet ? "Кораблей нет — назначьте из резерва фракции." : "Отрядов нет — назначьте из резерва фракции.",
              {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "info"});
  auto& st = ui::state<AddUnit>(ui::id("addunit"));
  if (ui::button(fleet ? "Корабли из резерва" : "Отряд из резерва", {.variant = ui::Variant::Ghost, .icon = "plus", .size = ui::Size::Small}))
    ui::openPopup("addunit");
  a.markUi("army.addunit." + std::to_string(g.faction));
  if (ui::beginPopup("addunit", {.width = 300})) {
    ui::caption(std::string("Резерв · ") + w.factionName(g.faction));
    std::vector<std::string> hints;
    std::vector<i64> reserves;
    for (const UnitRow& r : avail) {
      i64 res = reserveOf(w, g.faction, r.id, fleet);
      reserves.push_back(res);
      hints.push_back(fmtCount(res));
    }
    std::vector<ui::Option> opts;
    for (size_t i = 0; i < avail.size(); i++) opts.push_back(ui::Option{avail[i].name, avail[i].icon, Color(0, 0, 0, 0), hints[i], reserves[i] <= 0});
    if (st.row < 0 || st.row >= int(avail.size()) || reserves[size_t(st.row)] <= 0) {
      st.row = -1;
      for (size_t i = 0; i < avail.size(); i++)
        if (reserves[i] > 0) {
          st.row = int(i);
          break;
        }
    }
    int before = st.row;
    ui::combo("row", st.row, opts, {.placeholder = "Строка таблицы"});
    a.markUi("army.addunit.row");
    i64 res = st.row >= 0 ? reserves[size_t(st.row)] : 0;
    Id rowId = st.row >= 0 ? avail[size_t(st.row)].id : 0;
    if (st.row != before || rowId != st.lastRow) st.count = res;   // по умолчанию — весь резерв строки
    st.lastRow = rowId;
    st.count = clamp<i64>(st.count, 0, res);
    ui::prop("Численность", fleet ? "fleet" : "users");
    ui::numberField("count", st.count, {.min = 0, .max = double(res), .steppers = true, .tooltip = "Не больше резерва: " + fmtCount(res)});
    a.markUi("army.addunit.count");
    if (res <= 0) ui::label("В резерве нет свободных отрядов", {.font = ui::Font::Small, .ink = ui::Ink::Warning, .icon = "warning"});
    if (ui::button("Назначить", {.variant = ui::Variant::Primary, .icon = "check", .fill = true, .disabled = rowId == 0 || st.count <= 0})) {
      Id id = ar.id, fac = g.faction;
      i64 n = st.count;
      if (a.act(fleet ? "Корабли во флоте" : "Отряды в войске", [&](Tx& tx) { rules::setUnits(tx, id, fac, rowId, n); })) ui::closePopup();
    }
    a.markUi("army.addunit.ok");
    ui::endPopup();
  }
}

// ---------------------------------------------------------------- герои
void setHeroes(App& a, const Army& ar, Id character, bool on) {
  Id id = ar.id;
  a.act(on ? "Герой в войске" : "Убрать героя", [&](Tx& tx) {
    rules::setHero(tx, id, character, on);
    // Главный полководец — всегда один из героев, если они есть.
    const Army* cur = tx.w().army(id);
    if (cur && cur->commander == 0)
      for (const ArmyGroup& g : cur->groups)
        if (!g.heroes.empty()) {
          rules::setCommander(tx, id, g.heroes.front());
          break;
        }
  });
}

void heroesList(App& a, const Army& ar, const ArmyGroup& g) {
  const World& w = frameWorld(a);
  bool ro = a.readOnly();
  for (Id h : g.heroes) {
    const Character* ch = w.character(h);
    if (!ch) continue;
    ui::IdScope s{i64(h)};
    bool cmd = ar.commander == h;
    ui::Row r({ui::px(22), ui::px(30), ui::fr(1), ui::px(30)}, 38, 8);
    {
      ui::Disabled dis(ro);
      int v = cmd ? 1 : 0;
      if (ui::radio("##cmd", v, 1)) {
        Id id = ar.id;
        a.act(ar.isFleet() ? "Главный флотоводец" : "Главный полководец", [&](Tx& tx) { rules::setCommander(tx, id, h); });
      }
      ui::tooltip(leaderWord(ar));
      a.markUi("army.cmd." + std::to_string(h));
    }
    ui::avatar(ch->name, {.size = 28, .ring = cmd});
    {
      ui::Group gg(0, 0);
      ui::label(ch->name.empty() ? std::string("Без имени") : ch->name, {.font = cmd ? ui::Font::Strong : ui::Font::Body});
      ui::label(cmd ? std::string(leaderWord(ar)) : (ch->title.empty() ? std::string("Герой") : ch->title),
                {.font = ui::Font::Caption, .ink = cmd ? ui::Ink::Accent : ui::Ink::Muted});
    }
    ui::Disabled dis(ro);
    if (ui::iconButton("close", "Убрать героя из войска")) setHeroes(a, ar, h, false);
  }
  if (ro) return;
  // Добавить героя: персонажи этой фракции, не сопровождающие другие объекты.
  std::vector<const Character*> list;
  w.characters.each([&](const Character& c) {
    if (c.faction != g.faction) return;
    for (const ArmyGroup& x : ar.groups)
      if (std::find(x.heroes.begin(), x.heroes.end(), c.id) != x.heroes.end()) return;
    list.push_back(&c);
  });
  std::sort(list.begin(), list.end(), [](const Character* x, const Character* y) {
    if (x->hero != y->hero) return x->hero;
    return compareRu(x->name, y->name) < 0;
  });
  // Последний пункт — новый герой фракции (сразу в составе; если полководца нет — он и главный).
  std::vector<std::string> hints;
  std::vector<Id> ids;
  std::vector<bool> busy;
  for (const Character* c : list) {
    Id at = heroLocation(w, c->id, ar.id);
    busy.push_back(at != 0);
    ids.push_back(c->id);
    if (at) hints.push_back("в «" + objectName(*w.army(at)) + "»");
    else hints.push_back(c->title.empty() ? std::string(c->hero ? "герой" : "") : c->title);
  }
  Color fc = w::factionColor(w, g.faction);
  std::vector<ui::Option> opts;
  for (size_t i = 0; i < list.size(); i++)
    opts.push_back(ui::Option{list[i]->name, list[i]->hero ? "hero" : "character", fc, hints[i], bool(busy[i])});
  opts.push_back(ui::Option{"Новый герой", "user-plus", Color(0, 0, 0, 0), {}, false});
  int idx = -1;
  if (ui::combo("addhero", idx, opts, {.placeholder = "Добавить героя", .search = 1, .icon = "user-plus"})) {
    if (idx >= 0 && idx < int(ids.size()) && !busy[size_t(idx)]) {
      setHeroes(a, ar, ids[size_t(idx)], true);
    } else if (idx == int(ids.size())) {
      Id id = ar.id, fac = g.faction, nh = 0;
      a.act("Новый герой в войске", [&](Tx& tx) {
        nh = rules::createCharacter(tx, fac, "Новый герой");
        tx.character(nh).hero = true;
        rules::setHero(tx, id, nh, true);
        if (!tx.w().army(id)->commander) rules::setCommander(tx, id, nh);
      });
    }
  }
  a.markUi("army.addhero." + std::to_string(g.faction));
}

// ---------------------------------------------------------------- плитка фракции
void groupTile(App& a, const Army& ar, const ArmyGroup& g) {
  const World& w = frameWorld(a);
  ui::IdScope s(i64(g.faction) + 0x60000000LL);
  {
    ui::Card card({.pad = 12});
    {
      i64 n = groupCount(g);
      std::string cnt = fmtCount(n);
      float cw = ui::measure(cnt, ui::Font::Strong) + 4;
      ui::Row head({ui::px(30), ui::fr(1), ui::px(cw)}, 26, 10);
      factionFlag(w, g.faction, 30, 20);
      const Faction* f = w.faction(g.faction);
      if (ui::link(f ? (f->name.empty() ? std::string("Без названия") : f->name) : std::string("—"))) a.select(SelType::Faction, g.faction);
      ui::label(cnt, {.font = ui::Font::Strong, .align = ui::Align::Right,
                      .tooltip = ar.isFleet() ? "Кораблей этой фракции" : "Воинов этой фракции"});
    }
    unitsTable(a, ar, g);
    ui::spacer(2);
    {
      ui::HStack hs(18, ui::Align::Left, 6);
      ui::caption("Герои");
      if (ar.commander == 0 && &g == &ar.groups.front()) {
        bool any = false;
        for (const ArmyGroup& x : ar.groups) any = any || !x.heroes.empty();
        if (!any) ui::badge(ar.isFleet() ? "нет флотоводца" : "нет полководца", ui::Tone::Warning);
      }
    }
    heroesList(a, ar, g);
  }
  // Полоска цвета фракции слева на плитке.
  RectF cr = ui::lastItem().rect;
  ui::draw::rect(RectF{cr.x, cr.y + 12, 3, std::max(0.f, cr.h - 24)}, w::factionColor(w, g.faction), 1.5f);
}

// ---------------------------------------------------------------- вкладка «Состав»
void drawUnits(App& a, Id id) {
  const World& w = frameWorld(a);
  const Army* ar = w.army(id);
  if (!ar) return;
  bool fleet = ar->isFleet();
  bool ro = a.readOnly();
  i64 heroes = 0;
  for (const ArmyGroup& g : ar->groups) heroes += i64(g.heroes.size());
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 8);
    i64 n = unitCount(*ar);
    ui::stat(fmtCount(n), fleet ? plural(n, "корабль", "корабля", "кораблей") : plural(n, "воин", "воина", "воинов"),
             {.icon = fleet ? "fleet" : "army", .tone = ui::Tone::Accent});
    std::string cmd = ar->commander ? w.characterName(ar->commander) : std::string();
    ui::stat(std::to_string(heroes), plural(heroes, "герой", "героя", "героев"),
             {.icon = "commander", .tone = ui::Tone::Info,
              .tooltip = ar->commander ? std::string(leaderWord(*ar)) + ": " + cmd : std::string(ar->isFleet() ? "Флотоводец не назначен" : "Полководец не назначен")});
  }
  {
    ui::prop(fleet ? "Воды" : "Провинция", "map-pin");
    Id pid = provinceUnder(w, ar->pos);
    if (pid) w::provinceChip(pid);
    else ui::label(fleet ? "открытое море" : "вне провинций", {.ink = ui::Ink::Muted});
  }
  // Действия
  {
    ui::Disabled dis(ro);
    if (ar->allied()) {
      ui::Row r({ui::fr(1), ui::fr(1.25f), ui::px(36)}, 30, 8);
      if (ui::button("Разделить", {.icon = "split", .fill = true, .tooltip = "Выделить часть в отдельный объект"})) openSplit(a, id);
      a.markUi("army.split");
      if (ui::button("Распустить союз", {.icon = "dissolve", .fill = true,
                                         .tooltip = fleet ? "Флоты фракций станут отдельными объектами рядом" : "Войска фракций станут отдельными объектами рядом"}))
        dissolveAllied(a, id);
      a.markUi("army.dissolve");
      if (ui::iconButton("disband", fleet ? "Расформировать флот" : "Расформировать войско", {.shortcut = {platform::Key::Delete, 0}, .tone = ui::Tone::Danger}))
        askDisband(a, id);
      a.markUi("army.disband");
    } else {
      ui::Row r({ui::fr(1), ui::fr(1)}, 30, 8);
      if (ui::button("Разделить", {.icon = "split", .fill = true, .tooltip = "Выделить часть в отдельный объект"})) openSplit(a, id);
      a.markUi("army.split");
      if (ui::button("Расформировать", {.variant = ui::Variant::Danger, .icon = "disband", .fill = true,
                                        .tooltip = fleet ? "Убрать флот с карты, корабли — в резерв" : "Убрать войско с карты, отряды — в резерв",
                                        .shortcut = {platform::Key::Delete, 0}}))
        askDisband(a, id);
      a.markUi("army.disband");
    }
  }
  ui::spacer(2);
  for (const ArmyGroup& g : ar->groups) groupTile(a, *ar, g);
}

// ---------------------------------------------------------------- вкладка «События»
void drawLog(App& a, Id id) {
  const World& w = frameWorld(a);
  std::vector<const LogEntry*> list;
  w.log.each([&](const LogEntry& e) {
    if (e.army == id) list.push_back(&e);
  });
  std::sort(list.begin(), list.end(), [](const LogEntry* x, const LogEntry* y) { return x->turn != y->turn ? x->turn > y->turn : x->id > y->id; });
  if (list.empty()) {
    ui::emptyState("chronicle", "Событий с этим объектом пока нет.");
    return;
  }
  int lastTurn = -1;
  for (const LogEntry* e : list) {
    ui::IdScope s(i64(e->id));
    if (e->turn != lastTurn) {
      ui::caption("Ход " + std::to_string(e->turn));
      lastTurn = e->turn;
    }
    ui::Row r({ui::px(18), ui::fr(1)}, ui::kAuto, 10);
    ui::icon(schema::logKind(e->kind).icon, e->kind == LogKind::Battle || e->kind == LogKind::War ? ui::Ink::Danger : ui::Ink::Dim, 16);
    ui::text(e->text, ui::Font::Body, ui::Ink::Normal);
  }
}


HeaderReg header({"army.header", SelType::Army, 0, drawHeader});
TabReg unitsTab({"army.units", "army", "Состав", 10, SelType::Army, nullptr, drawUnits});
TabReg logTab({"army.log", "chronicle", "События", 20, SelType::Army, nullptr, drawLog});

}  // namespace
}  // namespace rg::app::mil
