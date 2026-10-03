// Regnum — вкладка «Население» инспектора провинции (ТЗ 1.a.vi): таблица рас (численность и доля), итог,
// культура и религия, довольство −100…+100 и вероятность восстания (2 довольства : 1 % + модификаторы).
#include "app/widgets.h"

namespace rg::app::prov {
// province_common.cpp
struct TipLine {
  std::string label, value;
  ui::Tone tone = ui::Tone::Neutral;
  bool total = false;
};
std::string pct(double v, bool sign = false);
std::string signedNum(double v);
std::string popText(double v);
void breakdown(std::string_view title, const std::vector<TipLine>& lines, float width = 300);
std::vector<TipLine> rebellionLines(const World& wd, const Province& p, const rules::ProvinceCalc& pc);
bool bipolarSlider(std::string_view key, double& v, double mn, double mx, bool disabled, RectF* rectOut);
}  // namespace rg::app::prov

namespace rg::app {
namespace {

bool landOnly(App& a, Id pid) {
  const Province* p = a.world().province(pid);
  return p && !p->sea;
}

std::string key(const char* what, Id pid) { return std::string("province.") + what + ":" + std::to_string(pid); }

// Добавить строку расы: первая раса справочника, которой ещё нет; если все есть — новая раса по названию.
void addRace(App& a, const Province& p) {
  const World& wd = a.world();
  Id pid = p.id;
  Id pick = 0;
  for (const CatalogItem& c : wd.catalogs->races) {
    bool used = std::any_of(p.races.begin(), p.races.end(), [&](const RacePop& r) { return r.race == c.id; });
    if (!used) {
      pick = c.id;
      break;
    }
  }
  if (pick) {
    a.act("Добавить расу", [&](Tx& tx) { tx.province(pid).races.push_back(RacePop{pick, 0}); });
    return;
  }
  a.prompt("Новая раса", "Название расы", "", [pid](App& x, const std::string& name) {
    x.act("Добавить расу", [&](Tx& tx) {
      Id r = rules::addCatalogItem(tx, rules::CatalogList::Races, name);
      tx.province(pid).races.push_back(RacePop{r, 0});
    });
  });
}

void racesTable(App& a, const Province& p, bool ro) {
  const World& wd = a.world();
  Id pid = p.id;
  i64 total = 0;
  for (const RacePop& r : p.races) total += std::max<i64>(0, r.pop);
  if (p.races.empty()) {
    if (ui::emptyState("race", "Расы не указаны.", ro ? std::string_view() : "Добавить расу", "plus")) addRace(a, p);
    a.markUi("province.races.empty");
    return;
  }
  ui::Column cols[] = {{"Раса", nullptr, ui::fr(1.45f)},
                       {"Жители", nullptr, ui::fr(1.15f), ui::Align::Right},
                       {"Доля", nullptr, ui::fr(1.1f)},
                       {"", nullptr, ui::px(ro ? 0.f : 26.f)}};
  ui::Table t("races", cols, int(p.races.size()), {.rowHeight = 38, .striped = false, .selectable = false});
  int removeAt = -1;
  for (int i : t) {
    const RacePop& rp = p.races[size_t(i)];
    const CatalogItem* rc = Catalogs::find(wd.catalogs->races, rp.race);
    t.cell();
    Id race = rp.race;
    if (w::catalogPicker("race", rules::CatalogList::Races, race, "—", true, ro) && race) {
      size_t idx = size_t(i);
      a.act("Раса в провинции", [&](Tx& tx) {
        auto& rs = tx.province(pid).races;
        if (idx >= rs.size()) return;
        for (size_t k = 0; k < rs.size(); k++)
          if (k != idx && rs[k].race == race) fail("Эта раса уже есть в провинции — измените её численность");
        rs[idx].race = race;
      });
    }
    a.markUi("province.race." + std::to_string(i));
    t.cell();
    i64 pop = rp.pop;
    if (ui::numberField("pop", pop, {.min = 0, .max = 1e15, .step = 100, .disabled = ro})) {
      size_t idx = size_t(i);
      a.act("Численность расы", [&](Tx& tx) {
        auto& rs = tx.province(pid).races;
        if (idx < rs.size()) rs[idx].pop = std::max<i64>(0, pop);
      }, {.coalesce = key("race", pid) + ":" + std::to_string(i)});
    }
    a.markUi("province.racePop." + std::to_string(i));
    t.cell();
    double share = total > 0 ? double(std::max<i64>(0, rp.pop)) / double(total) : 0.0;
    ui::progress(share, {.color = rc ? rc->color : ui::theme().textMuted, .height = 6, .label = true});
    t.cell();
    if (!ro) {
      if (ui::iconButton("trash", "Убрать расу", {.size = ui::Size::Small})) removeAt = i;
      a.markUi("province.raceDel." + std::to_string(i));
    }
  }
  if (t.footer()) {
    t.text("Итого", ui::Ink::Normal, ui::Font::Strong);
    t.text(fmtNum(double(total)), ui::Ink::Normal, ui::Font::Strong);
    t.cell();
    ui::label(total > 0 ? "100 %" : "—", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .align = ui::Align::Right});
    t.cell();
  }
  if (removeAt >= 0) {
    size_t idx = size_t(removeAt);
    a.act("Убрать расу", [&](Tx& tx) {
      auto& rs = tx.province(pid).races;
      if (idx < rs.size()) rs.erase(rs.begin() + long(idx));
    });
  }
}

void drawPopulation(App& a, Id pid) {
  const World& wd = a.world();
  const Province* p = wd.province(pid);
  if (!p) return;
  ui::IdScope ps{i64(pid)};
  bool ro = a.readOnly();
  auto calc = rules::calc(wd);
  rules::ProvinceCalc none;
  const rules::ProvinceCalc* pcp = calc->province(pid);
  const rules::ProvinceCalc& pc = pcp ? *pcp : none;

  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 8);
    double growth = pc.fx[Fx::PopGrowthPct];
    ui::stat(prov::popText(double(pc.population)), "Население",
             {.icon = "population", .deltaText = growth != 0 ? prov::pct(growth, true) : std::string(), .tooltip = "Сумма численности рас"});
    if (growth != 0) {
      prov::breakdown("Население", {{"Жителей", fmtNum(double(pc.population))}, {"Прирост за ход", prov::pct(growth, true), growth > 0 ? ui::Tone::Success : ui::Tone::Danger}});
    }
    ui::stat(prov::pct(pc.rebellion), "Восстание", {.icon = "rebellion", .tone = ui::Tone::Danger, .tooltip = "Вероятность восстания"});
    prov::breakdown("Вероятность восстания", prov::rebellionLines(wd, *p, pc));
    a.markUi("province.rebellion");
  }

  // Довольство: двуполярный ползунок −100…+100.
  if (ui::Section s("Довольство", "contentment"); s) {
    double c = p->contentment;
    RectF sr;
    if (prov::bipolarSlider("contentment", c, -100, 100, ro, &sr))
      a.act("Довольство населения", [&](Tx& tx) { tx.province(pid).contentment = clamp(c, -100.0, 100.0); }, {.coalesce = key("content", pid)});
    a.markUi("province.contentment", sr);
    double per = pc.fx[Fx::ContentmentPerTurn];
    {
      ui::HStack hs(26, ui::Align::Left, 6);
      ui::tag("2 довольства = 1 % восстания", ui::Tone::Neutral, "scales");
      if (per != 0) {
        ui::tag(prov::signedNum(per) + " за ход", per > 0 ? ui::Tone::Success : ui::Tone::Danger, "repeat");
        ui::tooltip("Изменение довольства каждый ход от модификаторов");
      }
    }
  }

  {
    std::string badge = fmtNum(double(p->races.size()));
    ui::Section rs("Расы", "race", {.badge = badge, .actionIcon = ro ? nullptr : "plus", .actionTooltip = "Добавить расу"});
    a.markUi("province.races.header");
    if (rs.action()) addRace(a, *p);
    if (rs) racesTable(a, *p, ro);
  }

  if (ui::Section s("Культура и вера", "culture"); s) {
    ui::prop("Культура", "culture");
    Id cu = p->culture;
    if (w::catalogPicker("culture", rules::CatalogList::Cultures, cu, "Не указана", true, ro))
      a.act("Культура провинции", [&](Tx& tx) { tx.province(pid).culture = cu; });
    a.markUi("province.culture");
    ui::prop("Религия", "religion");
    Id re = p->religion;
    if (w::catalogPicker("religion", rules::CatalogList::Religions, re, "Не указана", true, ro))
      a.act("Религия провинции", [&](Tx& tx) { tx.province(pid).religion = re; });
    a.markUi("province.religion");
  }
}

TabReg tabPopulation({"province.population", "population", "Население", 20, SelType::Province, landOnly, drawPopulation});

}  // namespace
}  // namespace rg::app
