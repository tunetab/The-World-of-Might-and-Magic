// Regnum — вкладки фракции «Войска» и «Флот» (ТЗ 1.c.i–ii, 1.d.iii) и полноэкранные таблицы войск и флота.
//
// Таблица строк: наименование, тип (12 типов войск или 3 типа судов), численность общая, в поле (в море),
// в резерве, содержание одного, содержание общее (с модификатором содержания). Узкий инспектор показывает
// значения в две строки и правит выбранную строку в карточке под таблицей; широкая таблица правится в ячейках.
#include "app/panels/military.h"

namespace rg::app::mil {

namespace {

constexpr float kWide = 640;   // ширина, начиная с которой таблица правится в ячейках

struct ForcesState {
  Id selRow = 0;
  Id shownRow = 0;   // строка, карточка которой уже показана (новую — прокрутить в видимую часть)
};

const char* noun(bool fleet) { return fleet ? "Судно" : "Отряд"; }
const char* fieldWord(bool fleet) { return fleet ? "в море" : "в поле"; }

// ---------------------------------------------------------------- правка строк
template <class F>
void editRow(Tx& tx, Id fid, Id row, bool fleet, F&& fn) {
  Faction& f = tx.faction(fid);
  if (fleet) {
    for (FleetRow& r : f.fleet)
      if (r.id == row) fn(r);
  } else {
    for (ArmyRow& r : f.army)
      if (r.id == row) fn(r);
  }
}

void renameRow(App& a, Id fid, Id row, bool fleet, const std::string& name) {
  std::string n = trim(name);
  if (n.empty()) {
    a.toast("Название не может быть пустым", ToastKind::Warning, "warning");
    return;
  }
  a.act(fleet ? "Переименовать судно" : "Переименовать отряд", [&](Tx& tx) { editRow(tx, fid, row, fleet, [&](auto& r) { r.name = n; }); });
}

void setRowType(App& a, Id fid, Id row, bool fleet, int type) {
  a.act(fleet ? "Тип судна" : "Тип войск", [&](Tx& tx) {
    editRow(tx, fid, row, fleet, [&](auto& r) {
      using T = std::decay_t<decltype(r)>;
      if constexpr (std::is_same_v<T, FleetRow>) {
        if (r.name == schema::shipType(r.type).name) r.name = schema::shipType(ShipType(type)).name;   // имя по типу следует за типом
        r.type = ShipType(type);
      } else {
        if (r.name == schema::unitType(r.type).name) r.name = schema::unitType(UnitType(type)).name;
        r.type = UnitType(type);
      }
    });
  });
}

void setRowUpkeep(App& a, Id fid, Id row, bool fleet, double v) {
  if (!std::isfinite(v) || v < 0) return;
  a.act(fleet ? "Содержание судна" : "Содержание отряда", [&](Tx& tx) { editRow(tx, fid, row, fleet, [&](auto& r) { r.upkeep = v; }); },
        {.coalesce = "upkeep:" + std::to_string(row)});
}

void setTotal(App& a, Id fid, Id row, i64 v) {
  a.act("Численность строки", [&](Tx& tx) { rules::setRowTotal(tx, fid, row, v); }, {.coalesce = "rowtotal:" + std::to_string(row)});
}

Id addRow(App& a, Id fid, bool fleet, int type) {
  Id id = 0;
  a.act(fleet ? "Добавить судно" : "Добавить отряд", [&](Tx& tx) {
    id = fleet ? rules::addFleetRow(tx, fid, ShipType(type)) : rules::addArmyRow(tx, fid, UnitType(type));
  });
  return id;
}

// Меню выбора типа для новой строки (открывается ui::openPopup(id)).
void addRowMenu(App& a, const char* id, Id fid, bool fleet, ForcesState& st) {
  if (!ui::beginMenu(id)) return;
  ui::menuHeader(fleet ? "Тип судна" : "Тип войск");
  int n = fleet ? int(ShipType::Count) : int(UnitType::Count);
  for (int i = 0; i < n; i++) {
    const schema::EnumInfo& ti = fleet ? schema::kShipTypes[i] : schema::kUnitTypes[i];
    ui::IdScope s(i);
    if (ui::menuItem(ti.name, {.icon = ti.icon}))
      if (Id nid = addRow(a, fid, fleet, i)) st.selRow = nid;
    a.markUi(std::string("mil.addrow.") + std::to_string(i));
  }
  ui::endMenu();
}

// ---------------------------------------------------------------- показатели
// Четыре показателя: всего, в поле (в море), в резерве, содержание. wide — в одну строку.
void statTiles(App& a, Id fid, bool fleet, bool wide) {
  auto c = rules::calc(frameWorld(a));
  const rules::FactionCalc* fc = c->faction(fid);
  i64 total = fc ? (fleet ? fc->fleetTotal : fc->armyTotal) : 0;
  i64 field = fc ? (fleet ? fc->fleetField : fc->armyField) : 0;
  double upkeep = fc ? (fleet ? fc->expFleet : fc->expArmy) : 0;
  double mod = fc ? fc->fx[fleet ? Fx::FleetUpkeepPct : Fx::ArmyUpkeepPct] : 0;
  std::string modText = std::fabs(mod) > 1e-9 ? fmtPct(mod, 0, true) : std::string();
  auto tiles = [&] {
    ui::stat(fmtCount(total), fleet ? "Всего кораблей" : "Всего войск", {.icon = fleet ? "fleet" : "army", .tone = ui::Tone::Accent});
    ui::stat(fmtCount(field), fleet ? "В море" : "В поле",
             {.icon = fleet ? "sea" : "map-pin", .tone = ui::Tone::Info,
              .tooltip = fleet ? "Корабли во флотах на карте" : "Отряды в войсках на карте и в гарнизонах провинций"});
    ui::stat(fmtCount(total - field), "В резерве", {.icon = fleet ? "anchor" : "shield", .tone = ui::Tone::Success,
                                                     .tooltip = "Численность общая − численность " + std::string(fieldWord(fleet))});
    ui::stat(fmtMoney(upkeep), "Содержание",
             {.icon = fleet ? "fleet-upkeep" : "army-upkeep", .tone = ui::Tone::Warning, .delta = mod, .deltaText = modText,
              .invertDelta = true,
              .tooltip = std::fabs(mod) > 1e-9 ? std::string("С модификатором содержания ") + (fleet ? "флота " : "войск ") + modText
                                               : std::string("Сумма «содержание общее» по строкам")});
  };
  if (wide) {
    ui::Row r({ui::fr(1), ui::fr(1), ui::fr(1), ui::fr(1)}, 64, 8);
    tiles();
  } else {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 8);
    tiles();
  }
}

// ---------------------------------------------------------------- таблица
struct Line {
  UnitRow row;
  const rules::RowCalc* rc = nullptr;
  i64 field() const { return rc ? rc->field : 0; }
  i64 reserve() const { return rc ? rc->reserve : row.total; }
  double upkeepTotal() const { return rc ? rc->upkeepTotal : 0; }
};

// Сортировка широкой таблицы: тип, название, всего, в поле, резерв, сод. одного, итого.
int compareLines(const Line& x, const Line& y, int col) {
  auto cmpNum = [](double p, double q) { return p < q ? -1 : p > q ? 1 : 0; };
  switch (col) {
    case 0: return cmpNum(x.row.type, y.row.type);
    case 1: return compareRu(x.row.name, y.row.name);
    case 2: return cmpNum(double(x.row.total), double(y.row.total));
    case 3: return cmpNum(double(x.field()), double(y.field()));
    case 4: return cmpNum(double(x.reserve()), double(y.reserve()));
    case 5: return cmpNum(x.row.upkeep, y.row.upkeep);
    case 6: return cmpNum(x.upkeepTotal(), y.upkeepTotal());
    default: return 0;
  }
}

void typeCombo(std::string_view id, int& type, bool fleet, bool disabled) {
  int n = fleet ? int(ShipType::Count) : int(UnitType::Count);
  std::vector<ui::Option> opts;
  opts.reserve(size_t(n));
  for (int i = 0; i < n; i++) {
    const schema::EnumInfo& ti = fleet ? schema::kShipTypes[i] : schema::kUnitTypes[i];
    opts.push_back(ui::Option{ti.name, ti.icon});
  }
  ui::combo(id, type, opts, {.disabled = disabled});
}

void forcesTable(App& a, Id fid, bool fleet, ForcesState& st, bool wide) {
  const World& w = frameWorld(a);
  auto c = rules::calc(w);
  std::vector<Line> lines;
  for (UnitRow& r : unitRows(w, fid, fleet)) lines.push_back(Line{r, rowCalc(*c, fid, r.id, fleet)});
  int sel = -1;
  for (size_t i = 0; i < lines.size(); i++)
    if (lines[i].row.id == st.selRow) sel = int(i);
  bool ro = a.readOnly();
  const int sel0 = sel;
  i64 sumTotal = 0, sumField = 0, sumReserve = 0;
  double sumUpkeep = 0;
  for (const Line& l : lines) {
    sumTotal += l.row.total;
    sumField += l.field();
    sumReserve += l.reserve();
    sumUpkeep += l.upkeepTotal();
  }
  // Подписи живут до конца функции (столбцы таблицы ссылаются на строки).
  const std::string fieldTitle = fleet ? "В море" : "В поле";
  const std::string tipReserve = "Численность общая − численность " + std::string(fieldWord(fleet));
  const std::string tipTotal = "Численность общая и " + std::string(fieldWord(fleet));
  const std::string tipMin = "Не меньше численности " + std::string(fieldWord(fleet));
  if (wide) {
    // Во весь экран тип — выпадающий список с подписью; в широком инспекторе — значок с меню типов.
    const bool typeLabel = ui::avail().w >= 900;
    const ui::Column typeCol = typeLabel ? ui::Column{fleet ? "Тип судна" : "Тип войск", nullptr, ui::px(214), ui::Align::Left, true}
                                         : ui::Column{"", nullptr, ui::px(52), ui::Align::Left, true, fleet ? "Тип судна" : "Тип войск"};
    ui::Column cols[] = {typeCol,
                         {"Наименование", nullptr, ui::fr(1, 140), ui::Align::Left, true},
                         {"Всего", nullptr, ui::px(92), ui::Align::Left, true, "Численность общая"},
                         {fieldTitle, nullptr, ui::px(76), ui::Align::Right, true, fleet ? "Численность в море" : "Численность в поле (войска и гарнизоны)"},
                         {"Резерв", nullptr, ui::px(76), ui::Align::Right, true, tipReserve},
                         {"Сод. 1", nullptr, ui::px(88), ui::Align::Left, true, fleet ? "Содержание одного корабля" : "Содержание одного юнита"},
                         {"Итого", "coins", ui::px(98), ui::Align::Right, true, "Содержание общее за ход (с модификатором)"},
                         {"", nullptr, ui::px(52)}};
    ui::Table t("rows", cols, int(lines.size()), {.rowHeight = 40, .selected = &sel, .emptyIcon = fleet ? "fleet" : "army",
                                                  .emptyText = fleet ? "Кораблей нет" : "Отрядов нет"});
    t.sort([&](int x, int y, int col) { return compareLines(lines[size_t(x)], lines[size_t(y)], col); });
    for (int i : t) {
      const Line& l = lines[size_t(i)];
      ui::Disabled dis(ro);
      // Тип: выпадающий список или значок с меню типов.
      t.cell();
      if (typeLabel) {
        int type = l.row.type;
        typeCombo("type", type, fleet, ro);
        if (type != l.row.type) setRowType(a, fid, l.row.id, fleet, type);
      } else if (ui::iconButton(l.row.icon, l.row.typeName)) {
        ui::openPopup("type");
      }
      a.markUi("mil.row." + std::to_string(i) + ".type");
      if (!typeLabel && ui::beginMenu("type")) {
        int n = fleet ? int(ShipType::Count) : int(UnitType::Count);
        for (int k = 0; k < n; k++) {
          const schema::EnumInfo& ti = fleet ? schema::kShipTypes[k] : schema::kUnitTypes[k];
          ui::IdScope s(k);
          if (ui::menuItem(ti.name, {.icon = ti.icon, .checked = k == l.row.type}) && k != l.row.type) setRowType(a, fid, l.row.id, fleet, k);
        }
        ui::endMenu();
      }
      t.cell();
      std::string name = l.row.name;
      if (ui::textField("name", name, {.placeholder = noun(fleet), .maxLength = 60})) renameRow(a, fid, l.row.id, fleet, name);
      t.cell();
      i64 total = l.row.total;
      if (ui::numberField("total", total, {.min = double(l.field()), .max = 1e12, .tooltip = tipMin}))
        setTotal(a, fid, l.row.id, total);
      a.markUi("mil.row." + std::to_string(i) + ".total");
      t.text(fmtCount(l.field()), l.field() > 0 ? ui::Ink::Normal : ui::Ink::Muted);
      t.text(fmtCount(l.reserve()), l.reserve() > 0 ? ui::Ink::Success : ui::Ink::Muted);
      t.cell();
      double up = l.row.upkeep;
      if (ui::numberField("upkeep", up, {.min = 0, .max = 1e9, .step = 0.5, .digits = 2})) setRowUpkeep(a, fid, l.row.id, fleet, up);
      t.text(fmtMoney(l.upkeepTotal()));
      t.cell();
      if (ui::iconButton("trash", fleet ? "Удалить судно" : "Удалить отряд", {.tone = ui::Tone::Danger})) askRemoveRow(a, fid, l.row.id, fleet);
    }
    if (!lines.empty() && t.footer()) {
      t.cell();
      t.text("Итого");
      t.text(fmtCount(sumTotal));
      t.text(fmtCount(sumField));
      t.text(fmtCount(sumReserve));
      t.cell();
      t.text(fmtMoney(sumUpkeep));
    }
  } else {
    ui::Column cols[] = {{noun(fleet), nullptr, ui::fr(1, 110)},
                         {"Всего", nullptr, ui::px(84), ui::Align::Right, false, tipTotal},
                         {"Резерв", nullptr, ui::px(64), ui::Align::Right, false, tipReserve},
                         {"Сод.", "coins", ui::px(74), ui::Align::Right, false, "Содержание общее за ход и содержание одного"}};
    ui::Table t("rows", cols, int(lines.size()), {.rowHeight = 44, .selected = &sel, .emptyIcon = fleet ? "fleet" : "army",
                                                  .emptyText = fleet ? "Кораблей нет" : "Отрядов нет"});
    for (int i : t) {
      const Line& l = lines[size_t(i)];
      a.markUi("mil.row." + std::to_string(i));
      RectF cr = t.cell();
      unitCell(cr, l.row, sel == i, l.row.name != l.row.typeName ? std::string_view(l.row.typeName) : std::string_view());
      cr = t.cell();
      cellLines(cr, fmtCount(l.row.total), std::string(fieldWord(fleet)) + " " + fmtCount(l.field()), ui::Align::Right);
      cr = t.cell();
      cellLines(cr, fmtCount(l.reserve()), "", ui::Align::Right, l.reserve() > 0 ? ui::Ink::Success : ui::Ink::Muted);
      cr = t.cell();
      cellLines(cr, fmtMoney(l.upkeepTotal()), fmtMoney(l.row.upkeep) + " за ед.", ui::Align::Right);
    }
    if (!lines.empty() && t.footer()) {
      t.text("Итого");
      RectF cr = t.cell();
      cellLines(cr, fmtCount(sumTotal), std::string(fieldWord(fleet)) + " " + fmtCount(sumField), ui::Align::Right);
      t.text(fmtCount(sumReserve));
      t.text(fmtMoney(sumUpkeep));
    }
  }
  // Выбор меняется только щелчком или стрелками: новая строка из меню ещё может отсутствовать в мире кадра.
  if (sel != sel0) st.selRow = sel >= 0 && sel < int(lines.size()) ? lines[size_t(sel)].row.id : 0;
  if (st.selRow && !unitRow(a.world(), fid, st.selRow, fleet)) st.selRow = 0;   // строку удалили
}

// Где стоят отряды строки: войска (флоты) и гарнизоны.
void deploymentChips(App& a, Id fid, Id row, bool fleet) {
  const World& w = frameWorld(a);
  struct Place {
    SelType type;
    Id id;
    std::string label;
    i64 n;
  };
  std::vector<Place> places;
  w.armies.each([&](const Army& ar) {
    if (ar.isFleet() != fleet) return;
    for (const ArmyGroup& g : ar.groups)
      if (g.faction == fid)
        if (i64 n = rowCount(g, row); n > 0) places.push_back({SelType::Army, ar.id, objectName(ar), n});
  });
  if (!fleet)
    w.provinces.each([&](const Province& p) {
      if (p.owner != fid) return;
      i64 n = 0;
      for (const GarrisonEntry& g : p.garrison)
        if (g.row == row) n += g.count;
      if (n > 0) places.push_back({SelType::Province, p.id, "Гарнизон: " + (p.name.empty() ? std::string("—") : p.name), n});
    });
  if (places.empty()) {
    ui::label(fleet ? "Все корабли в резерве" : "Все отряды в резерве", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = fleet ? "anchor" : "shield"});
    return;
  }
  ui::HStack hs(0, ui::Align::Left, 6);
  for (const Place& p : places) {
    ui::IdScope s(i64(p.id) * 4 + int(p.type));
    ui::ChipOpt co;
    co.icon = p.type == SelType::Army ? (fleet ? "fleet" : "army") : "castle";
    co.color = w::factionColor(w, fid);
    co.clickable = true;
    co.tooltip = p.type == SelType::Army ? "Открыть и показать на карте" : "Открыть провинцию";
    if (ui::chip(p.label + " · " + fmtCount(p.n), co) == ui::ChipAction::Click) a.select(p.type, p.id, true);
  }
}

// Карточка выбранной строки: правка (узкая таблица) и размещение отрядов.
bool rowCard(App& a, Id fid, bool fleet, ForcesState& st, bool editors) {
  const World& w = frameWorld(a);
  auto r = unitRow(w, fid, st.selRow, fleet);
  if (!r) return false;
  auto c = rules::calc(w);
  const rules::RowCalc* rc = rowCalc(*c, fid, r->id, fleet);
  i64 field = rc ? rc->field : 0, reserve = rc ? rc->reserve : r->total;
  bool ro = a.readOnly();
  const ui::Theme& th = ui::theme();
  ui::IdScope scope(i64(r->id));
  ui::Card card({.pad = 12, .tone = ui::Tone::Accent});
  {
    ui::Row head({ui::px(36), ui::fr(1), ui::px(30)}, 36, 10);
    typeTile(ui::next(36, 36), r->icon, th.accent);
    {
      ui::Group g(0, 0);
      ui::label(r->name, {.font = ui::Font::Strong});
      ui::label(r->typeName, {.font = ui::Font::Caption, .ink = ui::Ink::Muted});
    }
    {
      ui::Disabled dis(ro);
      if (ui::iconButton("trash", fleet ? "Удалить судно из таблицы" : "Удалить отряд из таблицы", {.tone = ui::Tone::Danger}))
        askRemoveRow(a, fid, r->id, fleet);
      a.markUi("mil.detail.delete");
    }
  }
  if (editors) {
    ui::Disabled dis(ro);
    ui::prop("Наименование", "edit");
    std::string name = r->name;
    if (ui::textField("name", name, {.placeholder = noun(fleet), .maxLength = 60})) renameRow(a, fid, r->id, fleet, name);
    a.markUi("mil.detail.name");
    ui::prop(fleet ? "Тип судна" : "Тип войск", r->icon);
    int type = r->type;
    typeCombo("type", type, fleet, ro);
    if (type != r->type) setRowType(a, fid, r->id, fleet, type);
    a.markUi("mil.detail.type");
    ui::prop("Численность", fleet ? "fleet" : "users");
    i64 total = r->total;
    if (ui::numberField("total", total, {.min = double(field), .max = 1e12, .steppers = true,
                                         .tooltip = "Не меньше численности " + std::string(fieldWord(fleet)) + ": " + fmtCount(field)}))
      setTotal(a, fid, r->id, total);
    a.markUi("mil.detail.total");
    ui::prop(fleet ? "За корабль" : "За единицу", "coins");
    double up = r->upkeep;
    if (ui::numberField("upkeep", up, {.min = 0, .max = 1e9, .step = 0.5, .digits = 2})) setRowUpkeep(a, fid, r->id, fleet, up);
    a.markUi("mil.detail.upkeep");
  }
  {
    ui::HStack hs(0, ui::Align::Left, 6);
    ui::tag(std::string(fleet ? "В море " : "В поле ") + fmtCount(field), ui::Tone::Info, fleet ? "sea" : "map-pin");
    ui::tag("Резерв " + fmtCount(reserve), reserve > 0 ? ui::Tone::Success : ui::Tone::Neutral, fleet ? "anchor" : "shield");
    ui::tag(fmtMoney(rc ? rc->upkeepTotal : 0) + " за ход", ui::Tone::Warning, "coins");
  }
  ui::caption(fleet ? "Где корабли" : "Где отряды");
  deploymentChips(a, fid, r->id, fleet);
  return true;
}

// Объекты фракции на карте (щелчок — выделить и показать).
void objectsOnMap(App& a, Id fid, bool fleet) {
  const World& w = frameWorld(a);
  std::vector<const Army*> list;
  w.armies.each([&](const Army& ar) {
    if (ar.isFleet() != fleet) return;
    for (const ArmyGroup& g : ar.groups)
      if (g.faction == fid) {
        list.push_back(&ar);
        return;
      }
  });
  std::sort(list.begin(), list.end(), [](const Army* x, const Army* y) { return compareRu(x->name, y->name) < 0; });
  std::string badge = std::to_string(list.size());
  ui::Section s(fleet ? "Флоты на карте" : "Войска на карте", fleet ? "fleet" : "army", {.badge = badge, .card = false});
  if (!s) return;
  if (list.empty()) {
    bool ro = a.readOnly();
    ui::Disabled dis(ro);
    if (ui::emptyState(fleet ? "fleet" : "army", fleet ? "Флотов на карте нет." : "Войск на карте нет.", fleet ? "Поставить флот" : "Поставить войско",
                       fleet ? "tool-fleet" : "tool-army"))
      startPlacing(a, fleet ? ArmyKind::Fleet : ArmyKind::Army, fid);
    a.markUi(fleet ? "mil.fleet.placeEmpty" : "mil.army.placeEmpty");
    return;
  }
  for (const Army* ar : list) {
    std::string sub = w.provinceName(provinceUnder(w, ar->pos));
    if (ar->allied()) {
      std::vector<std::string> names;
      for (Id f : factionsIn(*ar))
        if (f != fid) names.push_back(w.factionName(f));
      sub = "союз с " + join(names, ", ") + " · " + sub;
    }
    if (objectItem(w, *ar, a.ui.sel == Selection{SelType::Army, ar->id}, sub)) a.select(SelType::Army, ar->id, true);
  }
  ui::Disabled dis(a.readOnly());
  ui::HStack hs(0, ui::Align::Left, 6);
  if (ui::button(fleet ? "Поставить флот" : "Поставить войско", {.variant = ui::Variant::Ghost, .icon = fleet ? "tool-fleet" : "tool-army", .size = ui::Size::Small}))
    startPlacing(a, fleet ? ArmyKind::Fleet : ArmyKind::Army, fid);
}

// Списание из казны за ход: войска + флот (ТЗ 1.c.ii).
void upkeepCard(App& a, Id fid, bool fleet) {
  auto c = rules::calc(frameWorld(a));
  const rules::FactionCalc* fc = c->faction(fid);
  double army = fc ? fc->expArmy : 0, fl = fc ? fc->expFleet : 0;
  ui::Card card({.pad = 12});
  ui::Row r({ui::px(36), ui::fr(1), ui::px(120)}, 40, 10);
  {
    RectF ic = ui::next(36, 36);
    ui::draw::rect(ic, ui::toneColor(ui::Tone::Danger).alpha(0.14f), 10);
    ui::draw::icon("expense", ic.inset(9), ui::toneColor(ui::Tone::Danger));
  }
  {
    ui::Group g(0, 0);
    ui::label("Из казны за ход", {.font = ui::Font::Strong});
    std::string parts = std::string(fleet ? "флот " : "войска ") + fmtMoney(fleet ? fl : army) + " + " + (fleet ? "войска " : "флот ") + fmtMoney(fleet ? army : fl);
    ui::label(parts, {.font = ui::Font::Small, .ink = ui::Ink::Muted});
  }
  ui::label(fmtSigned(-(army + fl), (army + fl) - std::floor(army + fl) > 1e-6 ? 1 : 0),
            {.font = ui::Font::Number, .ink = army + fl > 0 ? ui::Ink::Danger : ui::Ink::Muted, .align = ui::Align::Right,
             .tooltip = "Сумма содержания общего из таблиц войск и флота"});
}

void drawForces(App& a, Id fid, bool fleet, bool fullscreen) {
  const World& w = frameWorld(a);
  const Faction* f = w.faction(fid);
  if (!f) return;
  ui::IdScope scope(fleet ? "fleet" : "army");
  auto& st = ui::state<ForcesState>(ui::id("forces"));
  bool wide = ui::avail().w >= kWide;
  size_t n = fleet ? f->fleet.size() : f->army.size();
  if (n > 0) {   // пустая таблица — без нулевых показателей
    statTiles(a, fid, fleet, wide);
    ui::spacer(4);
  }
  {
    ui::Section s(fleet ? "Корабли" : "Отряды", fleet ? "fleet" : "army",
                  {.badge = std::to_string(n), .card = false, .actionIcon = a.readOnly() ? nullptr : "plus",
                   .actionTooltip = fleet ? "Добавить судно" : "Добавить отряд"});
    if (s.action()) ui::openPopup("addrow");
    a.markUi(fleet ? "mil.fleet.add" : "mil.army.add");
    addRowMenu(a, "addrow", fid, fleet, st);
    if (s) {
      if (n == 0) {
        ui::Disabled dis(a.readOnly());
        if (ui::emptyState(fleet ? "fleet" : "army", fleet ? "В таблице флота пока нет судов." : "В таблице войск пока нет отрядов.",
                           fleet ? "Добавить судно" : "Добавить отряд", "plus"))
          ui::openPopup("addrow2");
        a.markUi(fleet ? "mil.fleet.empty" : "mil.army.empty");
        addRowMenu(a, "addrow2", fid, fleet, st);
      } else {
        forcesTable(a, fid, fleet, st, wide);
        if (st.selRow) {
          if (rowCard(a, fid, fleet, st, !wide)) {
            if (st.shownRow != st.selRow) ui::scrollToItem();   // новая карточка — в видимую часть
            st.shownRow = st.selRow;
          }
        } else {
          ui::label(wide ? (fleet ? "Выберите строку — покажем, где стоят корабли." : "Выберите строку — покажем, где стоят отряды.") : "Выберите строку, чтобы изменить её.",
                    {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "info"});
        }
      }
    }
  }
  if (!fullscreen) {   // во весь экран списание за ход — одна карточка над обеими таблицами
    ui::spacer(4);
    upkeepCard(a, fid, fleet);
    ui::spacer(4);
    objectsOnMap(a, fid, fleet);
    ui::spacer(2);
    if (ui::button("Таблицы войск и флота", {.variant = ui::Variant::Ghost, .icon = "maximize", .size = ui::Size::Small,
                                             .tooltip = "Открыть таблицы во весь экран"}))
      a.openEditor("military", fid);
    a.markUi("mil.fullscreen");
  }
}

void drawArmyTab(App& a, Id fid) { drawForces(a, fid, false, false); }
void drawFleetTab(App& a, Id fid) { drawForces(a, fid, true, false); }

// ---------------------------------------------------------------- полноэкранные таблицы
void drawEditor(App& a, Id arg) {
  const World& w = frameWorld(a);
  struct EditorState {
    Id cur = 0;   // показанная фракция (меняется выбором вверху)
    Id arg = 0;   // фракция, с которой редактор открыт
  };
  auto& es = ui::state<EditorState>(ui::id("faction"));
  if (arg != es.arg) {
    es.arg = arg;
    if (arg) es.cur = arg;
  }
  Id& cur = es.cur;
  if (!w.faction(cur)) {
    cur = 0;
    std::vector<const Faction*> all = w.factions.all();
    std::sort(all.begin(), all.end(), [](const Faction* x, const Faction* y) {
      if (x->kind != y->kind) return x->kind < y->kind;
      return compareRu(x->name, y->name) < 0;
    });
    if (!all.empty()) cur = all.front()->id;
  }
  if (!cur) {
    ui::emptyState("crown", "Фракций пока нет.");
    return;
  }
  const Faction& f = *w.faction(cur);
  {
    ui::Row head({ui::px(54), ui::fr(1), ui::px(300)}, 40, 12);
    factionFlag(w, cur, 54, 36);
    {
      ui::Group g(0, 0);
      ui::caption(f.isGuild() ? "Торговая гильдия" : "Государство");
      ui::label(f.name.empty() ? std::string("Без названия") : f.name, {.font = ui::Font::Heading});
    }
    Id pick = cur;
    if (w::factionPicker("pick", pick, w::FactionFilter::Any, "")) cur = pick;
    a.markUi("mil.editor.faction");
  }
  ui::spacer(4);
  ui::Scroll sc("body");
  ui::IdScope s{i64(cur)};
  upkeepCard(a, cur, false);   // ТЗ 1.c.ii: содержание из обеих таблиц — из казны каждый ход
  ui::spacer(8);
  ui::label("Войска", {.font = ui::Font::Title, .icon = "army"});
  drawForces(a, cur, false, true);
  ui::spacer(12);
  ui::label("Флот", {.font = ui::Font::Title, .icon = "fleet"});
  drawForces(a, cur, true, true);
  ui::spacer(8);
}

TabReg armyTab({"faction.army", "army", "Войска", 50, SelType::Faction, nullptr, drawArmyTab});
TabReg fleetTab({"faction.fleet", "fleet", "Флот", 52, SelType::Faction, nullptr, drawFleetTab});
EditorReg editorReg({"military", "Войска и флот", drawEditor, "army"});

}  // namespace
}  // namespace rg::app::mil
