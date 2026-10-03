// Regnum — вкладка «Экономика» инспектора провинции (ТЗ 1.a.vi, 1.d.ii, 1.d.iv–v): ресурс и добыча, базовая и
// текущая торговая ценность (маршруты +10 %), местный и общий налог (не меньше 1 %), доход государству,
// торговое влияние гильдий (сумма ≤ 100 %, кольцевая диаграмма), штабы (≤ 5, по одному на гильдию), доход гильдий.
#include "app/widgets.h"
#include "gfx/text.h"

namespace rg::app::prov {
// province_common.cpp
struct TipLine {
  std::string label, value;
  ui::Tone tone = ui::Tone::Neutral;
  bool total = false;
};
std::string num(double v);
std::string pct(double v, bool sign = false);
void breakdown(std::string_view title, const std::vector<TipLine>& lines, float width = 300);
std::vector<TipLine> tradeLines(const World& wd, const Province& p, const rules::ProvinceCalc& pc);
std::vector<TipLine> productionLines(const World& wd, const Province& p, const rules::ProvinceCalc& pc);
std::vector<TipLine> taxLines(const rules::ProvinceCalc& pc);
}  // namespace rg::app::prov

namespace rg::app {
namespace {

bool landOnly(App& a, Id pid) {
  const Province* p = a.world().province(pid);
  return p && !p->sea;
}

std::string key(const char* what, Id pid) { return std::string("province.") + what + ":" + std::to_string(pid); }

// Строки таблицы влияния: гильдии с влиянием (в порядке записи), затем гильдии только со штабом.
struct GuildRow {
  Id guild = 0;
  double pct = 0;
  bool hq = false;
};
std::vector<GuildRow> guildRows(const World& wd, const Province& p) {
  std::vector<GuildRow> rows;
  auto find = [&](Id g) -> GuildRow* {
    for (auto& r : rows)
      if (r.guild == g) return &r;
    return nullptr;
  };
  for (const Influence& in : p.influence) {
    const Faction* g = wd.faction(in.guild);
    if (!g || !g->isGuild()) continue;
    if (GuildRow* r = find(in.guild)) r->pct += std::max(0.0, in.pct);
    else rows.push_back(GuildRow{in.guild, std::max(0.0, in.pct), false});
  }
  for (Id h : p.hqs) {
    const Faction* g = wd.faction(h);
    if (!g || !g->isGuild()) continue;
    if (!find(h)) rows.push_back(GuildRow{h, 0, false});
  }
  for (auto& r : rows) r.hq = std::find(p.hqs.begin(), p.hqs.end(), r.guild) != p.hqs.end();
  return rows;
}

void resourceSection(App& a, const Province& p, const rules::ProvinceCalc& pc, bool ro) {
  const World& wd = a.world();
  Id pid = p.id;
  ui::prop("Ресурс", "resource");
  Id res = p.resource;
  if (w::catalogPicker("resource", rules::CatalogList::Resources, res, "Нет ресурса", true, ro))
    a.act("Ресурс провинции", [&](Tx& tx) { tx.province(pid).resource = res; });
  a.markUi("province.resource");
  ui::prop("Количество", "pickaxe");
  double amount = p.resourceAmount;
  if (ui::numberField("amount", amount, {.min = 0, .max = 1e9, .step = 1, .digits = 1, .disabled = ro || !p.resource}))
    a.act("Количество ресурса", [&](Tx& tx) { tx.province(pid).resourceAmount = std::max(0.0, amount); }, {.coalesce = key("amount", pid)});
  a.markUi("province.amount");
  ui::prop("Добыча за ход", "factory");
  {
    ui::HStack hs(30, ui::Align::Left, 6);
    if (p.resource) ui::iconColored(w::resourceIcon(wd, p.resource), w::resourceColor(wd, p.resource), 16);
    ui::label(p.resource ? prov::num(pc.production) : std::string("—"), {.font = ui::Font::Strong, .tooltip = "Добыча за ход"});
    if (p.resource) prov::breakdown("Добыча за ход", prov::productionLines(wd, p, pc));
    a.markUi("province.production");
  }
}

void tradeSection(App& a, const Province& p, const rules::ProvinceCalc& pc, bool ro) {
  const World& wd = a.world();
  Id pid = p.id;
  ui::prop("Базовая ценность", "trade-value");
  double base = p.baseTrade;
  if (ui::numberField("baseTrade", base, {.min = 0, .max = 1e9, .step = 1, .digits = 1, .disabled = ro}))
    a.act("Базовая торговая ценность", [&](Tx& tx) { tx.province(pid).baseTrade = std::max(0.0, base); }, {.coalesce = key("base", pid)});
  a.markUi("province.baseTrade");
  ui::prop("Текущая ценность", "trend-up");
  {
    ui::HStack hs(30, ui::Align::Left, 8);
    ui::label(prov::num(pc.tradeValue), {.font = ui::Font::Strong, .tooltip = "Текущая торговая ценность"});
    prov::breakdown("Текущая торговая ценность", prov::tradeLines(wd, p, pc));
    a.markUi("province.tradeValue");
    if (pc.tradeBase > 0 && std::fabs(pc.tradeValue - pc.tradeBase) > 1e-9) {
      double d = (pc.tradeValue / pc.tradeBase - 1) * 100;
      ui::tag(prov::pct(d, true), d > 0 ? ui::Tone::Success : ui::Tone::Danger, d > 0 ? "trend-up" : "trend-down");
    }
  }
  ui::prop("Маршруты", "route");
  {
    ui::HStack hs(30, ui::Align::Left, 8);
    ui::label(fmtNum(pc.routes), {.font = ui::Font::Strong, .tooltip = "Торговые маршруты через провинцию: +10 % базовой ценности каждый"});
    if (pc.routes > 0) ui::tag(fmtPct(pc.routes * 10.0, 0, true), ui::Tone::Success, "route");
  }
}

void taxSection(App& a, const Province& p, const rules::ProvinceCalc& pc, bool ro) {
  const World& wd = a.world();
  Id pid = p.id;
  ui::prop("Налог государства", "crown");
  {
    ui::HStack hs(30, ui::Align::Left, 8);
    ui::label(prov::pct(pc.taxState), {.font = ui::Font::Strong, .tooltip = "Задаётся во вкладке государства — одинаков для всех его провинций"});
    if (const Faction* own = wd.faction(p.owner); own && own->isState()) w::factionChip(own->id);
  }
  ui::prop("Местный налог", "percent");
  double local = p.localTax;
  if (ui::numberField("localTax", local, {.min = -100, .max = 100, .step = 1, .digits = 1, .unit = "%", .steppers = true, .sign = true, .disabled = ro,
                                          .tooltip = "Может быть отрицательным; общий налог — не меньше 1 %"}))
    a.act("Местный налог", [&](Tx& tx) { tx.province(pid).localTax = clamp(local, -100.0, 100.0); }, {.coalesce = key("tax", pid)});
  a.markUi("province.localTax");
  ui::prop("Общий налог", "scales");
  {
    ui::HStack hs(30, ui::Align::Left, 8);
    ui::label(prov::pct(pc.taxTotal), {.font = ui::Font::Strong, .tooltip = "Общий налог"});
    prov::breakdown("Общий налог", prov::taxLines(pc), 260);
    a.markUi("province.taxTotal");
    if (pc.taxState + pc.taxLocal < schema::kMinTotalTax) ui::tag("минимум 1 %", ui::Tone::Warning, "warning");
  }
}

// Кольцо влияния и легенда: доли гильдий, свободная часть, штабы.
void influenceSummary(const World& wd, const std::vector<GuildRow>& rows, double sum, int hqs) {
  const ui::Theme& t = ui::theme();
  std::vector<ui::Slice> slices;
  std::vector<std::string> names;
  names.reserve(rows.size() + 1);
  for (const GuildRow& r : rows) {
    if (r.pct <= 0) continue;
    names.push_back(wd.factionName(r.guild));
    slices.push_back(ui::Slice{r.pct, w::factionColor(wd, r.guild), {}});
  }
  double freeP = std::max(0.0, 100.0 - sum);
  if (freeP > 1e-9) {
    names.push_back("Свободно");
    slices.push_back(ui::Slice{freeP, t.track, {}});
  }
  for (size_t i = 0; i < slices.size(); i++) slices[i].label = names[i];
  // Узкая панель — кольцо по центру и легенда под ним, иначе легенда справа.
  bool narrow = ui::avail().w < 300;
  ui::Len wide[] = {ui::px(112), ui::fr(1)};
  ui::Len one[] = {ui::fr(1)};
  ui::Row row(narrow ? std::span<const ui::Len>(one) : std::span<const ui::Len>(wide), ui::kAuto, 14);
  if (narrow) {
    RectF r = ui::next(0, 108);
    ui::at(RectF{std::round(r.cx() - 54), r.y, 108, 108});
  }
  ui::pie(slices, {.size = 108, .thickness = 15, .centerValue = prov::pct(sum), .centerLabel = "занято"});
  {
    ui::Group g(0, 2);
    for (size_t i = 0; i < slices.size(); i++) {
      ui::IdScope s{int(i)};
      ui::Row rr({ui::px(12), ui::fr(1), ui::px(48)}, 22, 6);
      RectF d = ui::next(12);
      ui::draw::circle(d.cx(), d.cy(), 4.5f, slices[i].color);
      ui::label(names[i], {.font = ui::Font::Small, .ink = ui::Ink::Dim});
      ui::label(prov::pct(slices[i].value), {.font = ui::Font::Small, .align = ui::Align::Right});
    }
    ui::spacer(2);
    ui::Row rr({ui::px(12), ui::fr(1), ui::px(48)}, 22, 6);
    ui::icon("hq", hqs >= schema::kMaxHqPerProvince ? ui::Ink::Warning : ui::Ink::Muted, 14);
    ui::label("Штабы гильдий", {.font = ui::Font::Small, .ink = ui::Ink::Dim});
    ui::label(fmtNum(hqs) + " / " + fmtNum(schema::kMaxHqPerProvince),
              {.font = ui::Font::Small, .ink = hqs >= schema::kMaxHqPerProvince ? ui::Ink::Warning : ui::Ink::Normal, .align = ui::Align::Right});
  }
}

void influenceSection(App& a, const Province& p, bool ro) {
  const World& wd = a.world();
  Id pid = p.id;
  std::vector<GuildRow> rows = guildRows(wd, p);
  double sum = 0;
  int hqs = 0;
  for (const GuildRow& r : rows) {
    sum += r.pct;
    hqs += r.hq ? 1 : 0;
  }
  if (rows.empty()) {
    ui::label("Гильдии не влияют на торговлю провинции.", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
  } else {
    influenceSummary(wd, rows, sum, hqs);
    ui::spacer(4);
    ui::Column cols[] = {{"Гильдия", nullptr, ui::fr(1)},
                         {"Влияние", nullptr, ui::px(84), ui::Align::Right},
                         {"", "hq", ui::px(40), ui::Align::Center, false, "Штаб гильдии в провинции: доход только при штабе"},
                         {"", nullptr, ui::px(ro ? 0.f : 26.f)}};
    ui::Table t("influence", cols, int(rows.size()), {.rowHeight = 38, .striped = false, .selectable = false});
    int removeAt = -1;
    for (int i : t) {
      const GuildRow& r = rows[size_t(i)];
      ui::IdScope gs{i64(r.guild)};
      Id g = r.guild;
      t.cell();
      w::factionChip(g);
      t.cell();
      double v = r.pct;
      if (ui::numberField("pct", v, {.min = 0, .max = 100, .step = 1, .digits = 1, .unit = "%", .disabled = ro}))
        a.act("Влияние гильдии", [&](Tx& tx) { rules::setInfluence(tx, pid, g, v); }, {.coalesce = key("inf", pid) + ":" + std::to_string(g)});
      a.markUi("province.inf." + std::to_string(g));
      t.cell();
      {
        ui::HStack hs(30, ui::Align::Center, 0);
        bool hq = r.hq;
        if (ui::iconToggle("hq", hq ? "Штаб открыт — закрыть" : "Открыть штаб гильдии", hq, {.size = ui::Size::Small, .disabled = ro})) {
          if (hq) a.act("Открыть штаб гильдии", [&](Tx& tx) { rules::buildHq(tx, g, pid); });
          else a.act("Закрыть штаб гильдии", [&](Tx& tx) { rules::removeHq(tx, g, pid); });
        }
        a.markUi("province.hq." + std::to_string(g));
      }
      t.cell();
      if (!ro) {
        if (ui::iconButton("trash", "Убрать гильдию из провинции", {.size = ui::Size::Small})) removeAt = i;
        a.markUi("province.infDel." + std::to_string(g));
      }
    }
    if (t.footer()) {
      t.text("Итого", ui::Ink::Normal, ui::Font::Strong);
      t.text(prov::pct(sum), sum > 100 + 1e-9 ? ui::Ink::Danger : ui::Ink::Normal, ui::Font::Strong);
      t.text(fmtNum(hqs) + "/" + fmtNum(schema::kMaxHqPerProvince), hqs >= schema::kMaxHqPerProvince ? ui::Ink::Warning : ui::Ink::Dim,
             ui::Font::Small);
      t.cell();
    }
    if (removeAt >= 0) {
      GuildRow r = rows[size_t(removeAt)];
      a.act("Убрать гильдию из провинции", [&](Tx& tx) {
        if (r.pct > 0) rules::setInfluence(tx, pid, r.guild, 0);
        if (r.hq) rules::removeHq(tx, r.guild, pid);
      });
    }
  }
  // Добавить гильдию: свободная доля (до 10 %) достаётся новой гильдии.
  if (!ro) {
    std::vector<const Faction*> cand;
    wd.factions.each([&](const Faction& f) {
      if (!f.isGuild()) return;
      for (const GuildRow& r : rows)
        if (r.guild == f.id) return;
      cand.push_back(&f);
    });
    std::sort(cand.begin(), cand.end(), [](const Faction* x, const Faction* y) { return compareRu(x->name, y->name) < 0; });
    if (!cand.empty()) {
      std::vector<std::string> labels;
      labels.reserve(cand.size());
      for (const Faction* f : cand) labels.push_back(f->name.empty() ? std::string("Гильдия") : f->name);
      std::vector<ui::Option> opts;
      for (size_t i = 0; i < cand.size(); i++) opts.push_back(ui::Option{labels[i], nullptr, cand[i]->color, {}, false});
      int idx = -1;
      if (ui::combo("addGuild", idx, opts, {.placeholder = "Добавить гильдию", .icon = "plus"}) && idx >= 0) {
        Id g = cand[size_t(idx)]->id;
        double freeP = std::max(0.0, 100.0 - sum);
        if (freeP < 0.5) {
          a.toast("Свободного влияния нет — уменьшите долю другой гильдии", ToastKind::Warning, "guild");
        } else {
          double v = std::min(10.0, std::floor(freeP * 10) / 10);
          a.act("Влияние гильдии", [&](Tx& tx) { rules::setInfluence(tx, pid, g, v); });
        }
      }
      a.markUi("province.addGuild");
    }
  }
}

void guildIncome(App& a, const rules::ProvinceCalc& pc) {
  const World& wd = a.world();
  std::vector<const rules::GuildShare*> rows;
  for (const rules::GuildShare& s : pc.guilds)
    if (s.hq) rows.push_back(&s);
  if (rows.empty()) {
    ui::label("Доход получают только гильдии со штабом в провинции.", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
  } else {
    // Узкая панель — значки вместо подписей столбцов (подсказки те же).
    bool narrow = ui::avail().w < 300;
    ui::Column cols[] = {{"Гильдия", nullptr, ui::fr(1.8f)},
                         {narrow ? "" : "Валовой", narrow ? "trade-value" : nullptr, ui::fr(0.85f), ui::Align::Right, false, "Валовой доход: торговая ценность × влияние"},
                         {narrow ? "" : "Налог", narrow ? "percent" : nullptr, ui::fr(0.75f), ui::Align::Right, false, "Налог: валовой доход × общий налог — в казну государства"},
                         {narrow ? "" : "Чистый", narrow ? "coins" : nullptr, ui::fr(0.85f), ui::Align::Right, false, "Чистый доход гильдии за ход"}};
    ui::Table t("guildIncome", cols, int(rows.size()), {.rowHeight = 34, .striped = true, .selectable = false});
    double gross = 0, tax = 0, net = 0;
    for (const auto* s : rows) {
      gross += s->gross;
      tax += s->tax;
      net += s->net;
    }
    for (int i : t) {
      const rules::GuildShare& s = *rows[size_t(i)];
      RectF cell = t.cell();
      {
        ui::HStack hs(20, ui::Align::Left, 8);
        RectF d = ui::next(10, 10);
        ui::draw::circle(d.cx(), d.cy(), 4.5f, w::factionColor(wd, s.guild));
        std::string nm = gfx::ellipsize(wd.factionName(s.guild), ui::textStyle(ui::Font::Small), std::max(20.f, cell.w - 26));
        ui::label(nm, {.font = ui::Font::Small, .tooltip = wd.factionName(s.guild)});
      }
      t.text(prov::num(s.gross));
      t.text(prov::num(s.tax), ui::Ink::Dim);
      t.text(prov::num(s.net), ui::Ink::Success);
    }
    if (t.footer()) {
      t.text("Итого", ui::Ink::Normal, ui::Font::Strong);
      t.text(prov::num(gross), ui::Ink::Normal, ui::Font::Strong);
      t.text(prov::num(tax), ui::Ink::Dim, ui::Font::Strong);
      t.text(prov::num(net), ui::Ink::Success, ui::Font::Strong);
    }
  }
}

void drawEconomy(App& a, Id pid) {
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
    double d = pc.tradeBase > 0 ? (pc.tradeValue / pc.tradeBase - 1) * 100 : 0;
    ui::stat(prov::num(pc.tradeValue), "Торговля",
             {.icon = "trade-value", .tone = ui::Tone::Info, .delta = std::fabs(d) > 0.05 ? d : 0, .deltaText = std::fabs(d) > 0.05 ? prov::pct(d, true) : std::string(),
              .tooltip = "Текущая торговая ценность"});
    prov::breakdown("Текущая торговая ценность", prov::tradeLines(wd, *p, pc));
    double income = pc.provinceTax + pc.guildTax;
    ui::stat(prov::num(income), "Доход государству", {.icon = "income", .tone = ui::Tone::Success, .tooltip = "Доход государству"});
    {
      std::vector<prov::TipLine> L;
      L.push_back({"Налог с провинции", prov::num(pc.provinceTax)});
      L.push_back({"Налог гильдий", prov::num(pc.guildTax)});
      if (pc.recipient != pc.owner)
        L.push_back({"Получатель", pc.recipient ? wd.factionName(pc.recipient) : std::string("никто (оккупация)"), ui::Tone::Warning});
      L.push_back({"За ход", prov::num(income), ui::Tone::Neutral, true});
      prov::breakdown("Доход государству", L, 280);
    }
    a.markUi("province.income");
  }

  if (ui::Section s("Ресурс", "resource"); s) resourceSection(a, *p, pc, ro);
  if (ui::Section s("Торговля", "trade-value"); s) tradeSection(a, *p, pc, ro);
  if (ui::Section s("Налоги", "percent"); s) taxSection(a, *p, pc, ro);
  {
    std::vector<GuildRow> rows = guildRows(wd, *p);
    std::string badge = rows.empty() ? std::string() : fmtNum(double(rows.size()));
    if (ui::Section s("Влияние гильдий", "guild", {.badge = badge}); s) influenceSection(a, *p, ro);
  }
  if (ui::Section s("Доход гильдий", "income"); s) guildIncome(a, pc);
}

TabReg tabEconomy({"province.economy", "coins", "Экономика", 30, SelType::Province, landOnly, drawEconomy});

}  // namespace
}  // namespace rg::app
