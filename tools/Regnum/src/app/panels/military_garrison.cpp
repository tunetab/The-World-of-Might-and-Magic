// Regnum — вкладка провинции «Гарнизон» (ТЗ 1.a.vi): отряды из таблицы войск владельца, назначенные в гарнизон
// провинции (rules::setGarrison), не больше резерва владельца.
#include "app/panels/military.h"

namespace rg::app::mil {

namespace {

struct AddState {
  int row = -1;
  i64 count = 0;
  Id lastRow = 0;
};

void setGarrison(App& a, Id pid, Id row, i64 n, bool coalesce) {
  TxOptions o;
  if (coalesce) o.coalesce = "garrison:" + std::to_string(pid) + ":" + std::to_string(row);
  a.act("Гарнизон провинции", [&](Tx& tx) { rules::setGarrison(tx, pid, row, n); }, o);
}

// Всплывающая панель «Назначить из резерва»: строка таблицы владельца и численность.
void addPopup(App& a, const Province& p, const std::vector<UnitRow>& avail) {
  const World& w = frameWorld(a);
  auto& st = ui::state<AddState>(ui::id("garrison-add"));
  if (!ui::beginPopup("garrison-add", {.width = 300})) return;
  ui::caption(std::string("Резерв · ") + w.factionName(p.owner));
  std::vector<std::string> hints;
  std::vector<i64> reserves;
  for (const UnitRow& r : avail) {
    reserves.push_back(reserveOf(w, p.owner, r.id, false));
    hints.push_back(fmtCount(reserves.back()));
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
  ui::combo("row", st.row, opts, {.placeholder = "Отряд"});
  a.markUi("garrison.add.row");
  i64 res = st.row >= 0 ? reserves[size_t(st.row)] : 0;
  Id rowId = st.row >= 0 ? avail[size_t(st.row)].id : 0;
  if (st.row != before || rowId != st.lastRow) st.count = res;   // по умолчанию — весь резерв строки
  st.lastRow = rowId;
  st.count = clamp<i64>(st.count, 0, res);
  ui::prop("Численность", "users");
  ui::numberField("count", st.count, {.min = 0, .max = double(res), .steppers = true, .tooltip = "Не больше резерва: " + fmtCount(res)});
  a.markUi("garrison.add.count");
  if (res <= 0) ui::label("В резерве нет свободных отрядов", {.font = ui::Font::Small, .ink = ui::Ink::Warning, .icon = "warning"});
  if (ui::button("Назначить", {.variant = ui::Variant::Primary, .icon = "check", .fill = true, .disabled = rowId == 0 || st.count <= 0})) {
    Id pid = p.id;
    i64 n = st.count;
    if (a.act("Гарнизон провинции", [&](Tx& tx) { rules::setGarrison(tx, pid, rowId, n); })) ui::closePopup();
  }
  a.markUi("garrison.add.ok");
  ui::endPopup();
}

void drawGarrison(App& a, Id pid) {
  const World& w = frameWorld(a);
  const Province* p = w.province(pid);
  if (!p || p->sea) return;
  if (!p->owner || !w.faction(p->owner)) {
    ui::emptyState("castle", "У провинции нет владельца — гарнизон некому назначить.");
    return;
  }
  const Faction& owner = *w.faction(p->owner);
  bool ro = a.readOnly();
  std::vector<UnitRow> rows = unitRows(w, p->owner, false);
  auto c = rules::calc(w);
  i64 total = 0;
  for (const GarrisonEntry& g : p->garrison) total += g.count;
  i64 reserve = 0;
  for (const UnitRow& r : rows)
    if (const rules::RowCalc* rc = rowCalc(*c, p->owner, r.id, false)) reserve += std::max<i64>(0, rc->reserve);
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 8);
    ui::stat(fmtCount(total), "В гарнизоне", {.icon = "castle", .tone = ui::Tone::Accent});
    ui::stat(fmtCount(reserve), "Резерв владельца", {.icon = "shield", .tone = ui::Tone::Success,
                                                      .tooltip = "Свободные отряды " + (owner.name.empty() ? std::string("владельца") : owner.name)});
  }
  {
    ui::prop("Владелец", "crown");
    w::factionChip(p->owner);
  }
  if (rows.empty()) {
    if (ui::emptyState("army", "В таблице войск владельца нет отрядов.", "Открыть войска", "army")) {
      a.ui.tabOf[SelType::Faction] = "faction.army";
      a.select(SelType::Faction, p->owner);
    }
    return;
  }
  std::vector<UnitRow> avail;
  for (const UnitRow& r : rows) {
    bool in = false;
    for (const GarrisonEntry& g : p->garrison) in = in || g.row == r.id;
    if (!in) avail.push_back(r);
  }
  if (p->garrison.empty()) {
    ui::Disabled dis(ro);
    if (ui::emptyState("castle", "Гарнизона нет. Отряды берутся из резерва владельца.", "Назначить отряды", "plus")) ui::openPopup("garrison-add");
    a.markUi("garrison.add");
    addPopup(a, *p, avail);
    return;
  }
  struct L {
    UnitRow row;
    i64 count;
    i64 reserve;
  };
  std::vector<L> lines;
  for (const GarrisonEntry& g : p->garrison) {
    auto r = unitRow(w, p->owner, g.row, false);
    if (!r) continue;
    const rules::RowCalc* rc = rowCalc(*c, p->owner, g.row, false);
    lines.push_back(L{*r, g.count, rc ? std::max<i64>(0, rc->reserve) : 0});
  }
  {   // таблица заканчивается (и занимает место в потоке) до кнопки под ней
    ui::Column cols[] = {{"Отряд", nullptr, ui::fr(1, 110)},
                         {"Численность", nullptr, ui::px(104), ui::Align::Left, false, "Не больше резерва владельца"},
                         {"Резерв", nullptr, ui::px(68), ui::Align::Right, false, "Свободно в резерве владельца"},
                         {"", nullptr, ui::px(36)}};
    ui::Table t("garrison", cols, int(lines.size()), {.rowHeight = 44, .selectable = false});
    for (int i : t) {
      const L& l = lines[size_t(i)];
      RectF cr = t.cell();
      unitCell(cr, l.row, false, l.row.name != l.row.typeName ? std::string_view(l.row.typeName) : std::string_view());
      ui::Disabled dis(ro);
      t.cell();
      i64 n = l.count;
      if (ui::numberField("n", n, {.min = 0, .max = double(l.count + l.reserve), .tooltip = "Не больше резерва: " + fmtCount(l.reserve) + " свободно"}))
        setGarrison(a, pid, l.row.id, n, true);
      a.markUi("garrison.row." + std::to_string(l.row.id));
      t.text(fmtCount(l.reserve), l.reserve > 0 ? ui::Ink::Success : ui::Ink::Muted);
      t.cell();
      if (ui::iconButton("close", "Вернуть в резерв")) setGarrison(a, pid, l.row.id, 0, false);
    }
    if (t.footer()) {
      t.text("Итого");
      t.text(fmtCount(total));
    }
  }
  if (!avail.empty()) {
    ui::Disabled dis(ro);
    if (ui::button("Назначить из резерва", {.variant = ui::Variant::Ghost, .icon = "plus", .size = ui::Size::Small})) ui::openPopup("garrison-add");
    a.markUi("garrison.add");
    addPopup(a, *p, avail);
  }
}

// Морская провинция не содержит сведений (ТЗ 1.a.iii) — вкладки нет; провинция без владельца — пустое состояние.
bool visibleGarrison(App& a, Id pid) {
  const Province* p = a.world().province(pid);
  return p && !p->sea;
}

TabReg garrisonTab({"province.garrison", "castle", "Гарнизон", 35, SelType::Province, visibleGarrison, drawGarrison});

}  // namespace
}  // namespace rg::app::mil
