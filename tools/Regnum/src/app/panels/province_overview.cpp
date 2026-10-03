// Regnum — вкладка «Обзор» инспектора провинции (ТЗ 1.a.vi, 1.a.ix, 1.f.i): показатели, владелец, лорд, столица,
// величина и тип города со слотами построек, оккупация, заметки и связь с карточкой кампании.
// Вкладка «Море» — короткая заметка для морской провинции (ТЗ 1.a.iii).
#include "app/widgets.h"

namespace rg::app::prov {
// province_common.cpp
struct TipLine {
  std::string label, value;
  ui::Tone tone = ui::Tone::Neutral;
  bool total = false;
};
std::string num(double v);
std::string pct(double v, bool sign = false);
std::string popText(double v);
void breakdown(std::string_view title, const std::vector<TipLine>& lines, float width = 300);
std::vector<TipLine> rebellionLines(const World& wd, const Province& p, const rules::ProvinceCalc& pc);
std::vector<TipLine> tradeLines(const World& wd, const Province& p, const rules::ProvinceCalc& pc);
std::vector<TipLine> productionLines(const World& wd, const Province& p, const rules::ProvinceCalc& pc);
std::vector<TipLine> slotLines(const World& wd, const rules::ProvinceCalc& pc);
void hatchSwatch(RectF r, Color c);
Id defaultOccupier(const World& wd, const Province& p);
}  // namespace rg::app::prov

namespace rg::app {
namespace {

bool landOnly(App& a, Id pid) {
  const Province* p = a.world().province(pid);
  return p && !p->sea;
}
bool seaOnly(App& a, Id pid) {
  const Province* p = a.world().province(pid);
  return p && p->sea;
}

const char* plSlots(int n) { return plural(n, "слот", "слота", "слотов"); }

// Величина провинции и тип города: значки с подсказками, под ними — выбранное значение и вклад в слоты.
void settlement(App& a, const Province& p, bool ro) {
  Id pid = p.id;
  ui::Row row({ui::fr(3), ui::fr(4)}, ui::kAuto, 12);
  {
    ui::Group g(0, 6);
    ui::caption("Величина");
    std::array<std::string, 3> tips;
    std::array<ui::Segment, 3> segs;
    for (int i = 0; i < 3; i++) {
      const schema::EnumInfo& e = schema::kProvSizes[i];
      tips[size_t(i)] = std::string(e.name) + " · " + fmtSigned(e.value) + " " + plSlots(e.value);
      segs[size_t(i)] = ui::Segment{e.icon, {}, tips[size_t(i)]};
    }
    int size = int(p.size);
    if (ui::segmented("size", size, std::span<const ui::Segment>(segs), {.disabled = ro}))
      a.act("Величина провинции", [&](Tx& tx) { tx.province(pid).size = ProvSize(clamp(size, 0, 2)); });
    a.markUi("province.size");
    const schema::EnumInfo& cur = schema::provSize(p.size);
    ui::label(std::string(cur.name) + " · " + fmtSigned(cur.value), {.font = ui::Font::Small, .ink = ui::Ink::Muted});
  }
  {
    ui::Group g(0, 6);
    ui::caption("Тип города");
    std::array<std::string, 4> tips;
    std::array<ui::Segment, 4> segs;
    for (int i = 0; i < 4; i++) {
      const schema::EnumInfo& e = schema::kCityTypes[i];
      tips[size_t(i)] = std::string(e.name) + " · " + fmtSigned(e.value) + " " + plSlots(e.value);
      segs[size_t(i)] = ui::Segment{e.icon, {}, tips[size_t(i)]};
    }
    int city = int(p.city);
    if (ui::segmented("city", city, std::span<const ui::Segment>(segs), {.disabled = ro}))
      a.act("Тип города", [&](Tx& tx) { tx.province(pid).city = CityType(clamp(city, 0, 3)); });
    a.markUi("province.city");
    const schema::EnumInfo& cur = schema::cityType(p.city);
    ui::label(std::string(cur.name) + " · " + fmtSigned(cur.value), {.font = ui::Font::Small, .ink = ui::Ink::Muted});
  }
}

void drawOverview(App& a, Id pid) {
  const World& wd = a.world();
  const Province* p = wd.province(pid);
  if (!p) return;
  ui::IdScope ps{i64(pid)};
  bool ro = a.readOnly();
  auto calc = rules::calc(wd);
  rules::ProvinceCalc none;
  const rules::ProvinceCalc* pcp = calc->province(pid);
  const rules::ProvinceCalc& pc = pcp ? *pcp : none;

  // Показатели
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 8);
    ui::stat(prov::popText(double(pc.population)), "Население", {.icon = "population", .tooltip = "Сумма численности рас"});
    ui::stat(prov::num(pc.tradeValue), "Торговля", {.icon = "trade-value", .tone = ui::Tone::Info, .tooltip = "Торговая ценность"});
    prov::breakdown("Текущая торговая ценность", prov::tradeLines(wd, *p, pc));
    ui::stat(prov::pct(pc.rebellion), "Восстание", {.icon = "rebellion", .tone = ui::Tone::Danger, .tooltip = "Вероятность восстания"});
    prov::breakdown("Вероятность восстания", prov::rebellionLines(wd, *p, pc));
    a.markUi("province.overview.rebellion");
    const CatalogItem* res = wd.resource(p->resource);
    ui::stat(prov::num(pc.production), "Добыча за ход",
             {.icon = res ? w::resourceIcon(wd, p->resource) : "resource", .tone = ui::Tone::Success, .tooltip = "Добыча за ход"});
    if (res) prov::breakdown("Добыча за ход · " + res->name, prov::productionLines(wd, *p, pc));
  }

  if (ui::Section s("Владение", "crown"); s) {
    ui::prop("Государство", "crown");
    Id owner = p->owner;
    if (w::factionPicker("owner", owner, w::FactionFilter::States, "Без владельца", 0, ro))
      a.act("Владелец провинции", [&](Tx& tx) { rules::setProvinceOwner(tx, pid, owner); });
    a.markUi("province.owner");
    ui::prop("Лорд", "lord");
    Id lord = p->lord;
    if (w::characterPicker("lord", lord, p->owner, "Не назначен", true, ro))
      a.act("Лорд провинции", [&](Tx& tx) { tx.province(pid).lord = lord; });
    a.markUi("province.lord");
    ui::prop("Столица", "capital");
    std::string cap = p->capital;
    if (ui::textField("capital", cap, {.placeholder = "Название города", .maxLength = 80, .disabled = ro}))
      a.act("Столица провинции", [&](Tx& tx) { tx.province(pid).capital = trim(cap); });
    a.markUi("province.capital");
  }

  if (ui::Section s("Поселение и слоты", "slots"); s) {
    settlement(a, *p, ro);
    ui::spacer(2);
    std::string val = fmtNum(pc.slotsUsed) + " / " + fmtNum(pc.slots);
    ui::stat(val, "Слоты построек: занято / всего", {.icon = "slots", .tone = pc.slotsUsed > pc.slots ? ui::Tone::Danger : ui::Tone::Success,
                                                    .tooltip = "Слоты построек"});
    prov::breakdown("Слоты построек", prov::slotLines(wd, pc), 280);
    a.markUi("province.slots");
  }

  if (ui::Section s("Оккупация", "occupied", {.defaultOpen = true}); s) {
    bool occ = p->occupied;
    if (ui::toggle("Оккупирована", occ, ro)) {
      if (occ) {
        Id def = p->occupier && p->occupier != p->owner ? p->occupier : prov::defaultOccupier(wd, *p);
        if (!def) a.toast("Нет государства, которое могло бы оккупировать провинцию", ToastKind::Warning, "occupied");
        else a.act("Оккупация провинции", [&](Tx& tx) { rules::setOccupied(tx, pid, def); });
      } else {
        a.act("Снять оккупацию", [&](Tx& tx) { rules::setOccupied(tx, pid, 0); });
      }
    }
    a.markUi("province.occupied");
    {
      ui::Row r({ui::fr(1), ui::px(30)}, 30, 8);
      Id oc = p->occupied ? p->occupier : 0;
      if (w::factionPicker("occupier", oc, w::FactionFilter::States, "", p->owner, ro || !p->occupied) && oc)
        a.act("Оккупант провинции", [&](Tx& tx) { rules::setOccupied(tx, pid, oc); });
      a.markUi("province.occupier");
      RectF sw = ui::next(30, 30);
      if (const Faction* of = p->occupied ? wd.faction(p->occupier) : nullptr) prov::hatchSwatch(sw.inset(5), of->color);
      else ui::draw::rectStroke(sw.inset(5), ui::theme().border, 3, 1);
    }
  }

  if (ui::Section s("Заметки", "note", {.defaultOpen = true}); s) {
    std::string notes = p->notes;
    if (ui::textArea("notes", notes, 96, {.placeholder = "Заметки ведущего о провинции", .disabled = ro}))
      a.act("Заметки провинции", [&](Tx& tx) { tx.province(pid).notes = notes; });
    a.markUi("province.notes");
    ui::prop("Карточка", "link");
    std::string ent = p->entity;
    if (ui::textField("entity", ent, {.placeholder = "ID карточки", .maxLength = 120, .disabled = ro, .tooltip = "Связь с карточкой канона кампании (необязательно)"}))
      a.act("Карточка провинции", [&](Tx& tx) { tx.province(pid).entity = trim(ent); });
    a.markUi("province.entity");
  }
}

// Морская провинция: без сведений, одна строка и действие.
void drawSea(App& a, Id pid) {
  ui::spacer(8);
  bool ro = a.readOnly();
  if (ui::emptyState("sea", "Морская провинция: без сведений и заливки на карте. Данные суши сохранены.", ro ? std::string_view() : "Сделать сухопутной", "land"))
    a.act("Сделать провинцию сухопутной", [&](Tx& tx) { rules::setProvinceSea(tx, pid, false); });
  a.markUi("province.seanote");
}

TabReg tabSea({"province.sea", "sea", "Морская провинция", 5, SelType::Province, seaOnly, drawSea});
TabReg tabOverview({"province.overview", "info", "Обзор", 10, SelType::Province, landOnly, drawOverview});

}  // namespace
}  // namespace rg::app
