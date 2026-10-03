// Regnum — вкладка «Экономика» (ТЗ 1.e.i, 1.e.ii, 1.d.iv, 1.b.iv): казна и чистый доход, структура доходов и
// расходов с формулами в подсказках, налог государства (≥ 0), таблица ресурсов (запас, добыча, торговля, итог),
// действующие дань и репарации (оставшиеся ходы, отмена) и вход в диалог «Навязать дань / репарации».
#include "app/panels/faction_common.h"

namespace rg::app {
namespace {

using namespace fac;

void moneyCards(const Faction& f, const rules::FactionCalc& fc) {
  const ui::Theme& t = ui::theme();
  const bool state = f.isState();
  // Доходы
  {
    Color cProv = t.accent, cGuildTax = t.info, cHq = t.success, cTrade = Color::mix(t.info, t.success, 0.5f), cTrib = t.warning;
    if (ui::Section s("Доходы", "income", {.badge = money(fc.incTotal)}); s) {
      ui::IdScope sc("income");
      Share parts[] = {{fc.incProvinces, cProv}, {fc.incGuildTax, cGuildTax}, {fc.incGuilds, cHq}, {fc.incTrade, cTrade}, {fc.incTribute, cTrib}};
      shareBar(parts);
      ui::spacer(2);
      if (state || fc.incProvinces != 0)
        moneyRow("province", cProv, "Налог с провинций", fc.incProvinces,
                 "Σ по провинциям: (торговая ценность − валовой доход гильдий со штабом) × общий налог.\n"
                 "Общий налог = налог государства + местный, не меньше 1 %. Добыча золота — тоже сюда.");
      if (state || fc.incGuildTax != 0)
        moneyRow("guild", cGuildTax, "Налог гильдий", fc.incGuildTax,
                 "Σ по провинциям: валовой доход гильдий со штабом × общий налог провинции");
      if (!state || fc.incGuilds != 0)
        moneyRow("hq", cHq, "Доход штабов", fc.incGuilds,
                 "Σ по штабам: торговая ценность × влияние гильдии − налог провинции");
      moneyRow("trade", cTrade, "Торговля", fc.incTrade, "Поступления золота по торговым сделкам «каждый ход»");
      moneyRow("tribute", cTrib, "Дань и репарации", fc.incTribute, "Поступления золота от плательщиков дани и репараций");
      if (fc.incomePct != 0)
        moneyRow("percent", fc.incomePct > 0 ? t.success : t.danger, "Модификатор дохода " + fmtPct(fc.incomePct, 0, true), fc.incTotal - fc.incGross,
                 "Сумма доходов × (1 + Σ «доход в казну» / 100) — модификаторы, технологии, постройки", true,
                 fc.incomePct > 0 ? ui::Ink::Success : ui::Ink::Danger);
      ui::separator();
      moneyRow("coins", t.textDim, "Итого доходов", fc.incTotal, "Сумма доходов за ход с учётом модификатора дохода");
    }
  }
  // Расходы
  {
    Color cArmy = t.danger, cFleet = t.info, cSpec = t.accent, cTrade = Color::mix(t.info, t.success, 0.5f), cTrib = t.warning;
    if (ui::Section s("Расходы", "expense", {.badge = money(fc.expTotal)}); s) {
      ui::IdScope sc("expense");
      Share parts[] = {{fc.expArmy, cArmy}, {fc.expFleet, cFleet}, {fc.expSpecialists, cSpec}, {fc.expTrade, cTrade}, {fc.expTribute, cTrib}};
      shareBar(parts);
      ui::spacer(2);
      moneyRow("army", cArmy, "Содержание войск", fc.expArmy,
               "Σ по строкам войск: численность общая × содержание одного × (1 + Σ «содержание войск» / 100)");
      moneyRow("fleet", cFleet, "Содержание флота", fc.expFleet,
               "Σ по строкам флота: численность общая × содержание одного × (1 + Σ «содержание флота» / 100)");
      moneyRow("council", cSpec, "Специалисты", fc.expSpecialists, "Σ содержания правителя, советников (любой фракции) и героев фракции; каждый учитывается один раз");
      moneyRow("trade", cTrade, "Торговля", fc.expTrade, "Выплаты золота по торговым сделкам «каждый ход»");
      moneyRow("tribute", cTrib, "Дань и репарации", fc.expTribute, "Выплаты золота получателям дани и репараций");
      ui::separator();
      moneyRow("coins", t.textDim, "Итого расходов", fc.expTotal, "Сумма расходов за ход");
    }
  }
}

void taxSection(App& a, const Faction& f, Id id, bool ro) {
  if (!f.isState()) return;
  if (ui::Section s("Налог", "percent"); s) {
    ui::prop("Налог государства", "percent");
    double tax = f.tax;
    if (ui::numberField("tax", tax, {.min = 0, .max = 100, .step = 1, .digits = 1, .unit = "%", .disabled = ro,
                                     .tooltip = "Одинаков для всех провинций государства, не меньше 0"}))
      a.act("Налог государства", [&](Tx& tx) { tx.faction(id).tax = std::max(0.0, tax); }, {.coalesce = "faction.tax:" + std::to_string(id)});
    a.markUi("economy.tax");
    ui::label("Общий налог провинции = налог государства + местный налог, не меньше 1 %.", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .wrap = true});
  }
}

void resourcesSection(App& a, const World& w, const Faction& f, const rules::FactionCalc& fc, Id id, bool ro) {
  std::vector<Id> res;
  for (auto& [r, flow] : fc.resources) {
    if (!w.resource(r) && r != kGold) continue;
    res.push_back(r);
  }
  // Золото первым, дальше — по справочнику.
  auto order = [&](Id r) {
    const auto& cat = w.catalogs->resources;
    for (size_t i = 0; i < cat.size(); i++)
      if (cat[i].id == r) return int(i);
    return 1 << 20;
  };
  std::stable_sort(res.begin(), res.end(), [&](Id x, Id y) {
    if ((x == kGold) != (y == kGold)) return x == kGold;
    return order(x) < order(y);
  });
  if (ui::Section s("Ресурсы", "resource", {.badge = std::to_string(res.size())}); s) {
    ui::Column cols[] = {{"Ресурс", nullptr, ui::fr(1, 92), ui::Align::Left, true},
                         {"Запас", nullptr, ui::px(72), ui::Align::Left, true, "Запас ресурса (казна — для золота)"},
                         {{}, "factory", ui::px(42), ui::Align::Right, true, "Добыча за ход"},
                         {{}, "trade", ui::px(46), ui::Align::Right, false, "Торговля за ход: приход / расход"},
                         {{}, "trend-up", ui::px(64), ui::Align::Right, true, "Итого за ход"}};
    ui::Table t("resources", cols, int(res.size()), {.rowHeight = 36, .selectable = false, .emptyIcon = "resource", .emptyText = "Ресурсов нет"});
    auto flowOf = [&](int i) -> const rules::ResourceFlow& { return fc.resources.at(res[size_t(i)]); };
    t.sort([&](int x, int y, int col) {
      auto num = [](double u, double v) { return u < v ? -1 : u > v ? 1 : 0; };
      switch (col) {
        case 1: return num(flowOf(x).stock, flowOf(y).stock);
        case 2: return num(flowOf(x).production, flowOf(y).production);
        case 4: return num(flowOf(x).net, flowOf(y).net);
        default: return compareRu(resourceName(w, res[size_t(x)]), resourceName(w, res[size_t(y)]));
      }
    });
    for (int i : t) {
      const Id r = res[size_t(i)];
      const rules::ResourceFlow& fl = flowOf(i);
      ui::IdScope sc{i64(r)};
      t.cell();
      {
        ui::Row rr({ui::px(16), ui::fr(1)}, 22, 5);
        ui::iconColored(w::resourceIcon(w, r), w::resourceColor(w, r), 16);
        ui::label(resourceName(w, r), {.font = r == kGold ? ui::Font::Strong : ui::Font::Body});
      }
      t.cell();
      double stock = r == kGold ? f.treasury() : f.stock(r);
      ui::NumberOpt no;
      no.min = r == kGold ? -1e12 : 0;   // казна может уйти в долг, прочие ресурсы — нет
      no.max = 1e12;
      no.step = r == kGold ? 10 : 1;
      no.digits = std::fabs(stock - std::round(stock)) > 1e-9 ? 1 : 0;
      no.disabled = ro;
      no.tooltip = r == kGold ? "Казна" : "Запас ресурса";
      if (ui::numberField("stock", stock, no))
        a.act(r == kGold ? "Казна" : "Запас ресурса", [&](Tx& tx) { tx.faction(id).res[r] = r == kGold ? stock : std::max(0.0, stock); },
              {.coalesce = "faction.res:" + std::to_string(id) + ":" + std::to_string(r)});
      a.markUi("economy.stock." + std::to_string(r));
      t.text(fl.production > 0 ? fmtNum(fl.production, fl.production < 10 && std::fabs(fl.production - std::round(fl.production)) > 0.05 ? 1 : 0)
                               : std::string("—"),
             fl.production > 0 ? ui::Ink::Normal : ui::Ink::Muted, ui::Font::Small);
      std::string tr;
      if (fl.tradeIn > 0) tr += "+" + fmtNum(fl.tradeIn);
      if (fl.tradeOut > 0) tr += (tr.empty() ? "" : " ") + fmtSigned(-fl.tradeOut);
      t.text(tr.empty() ? std::string("—") : tr, tr.empty() ? ui::Ink::Muted : ui::Ink::Dim, ui::Font::Small);
      ui::Ink ni = fl.net > 0.005 ? ui::Ink::Success : fl.net < -0.005 ? ui::Ink::Danger : ui::Ink::Muted;
      t.text(std::fabs(fl.net) < 0.005 ? std::string("0") : fmtSigned(fl.net, std::fabs(fl.net) < 10 && std::fabs(fl.net - std::round(fl.net)) > 0.05 ? 1 : 0), ni,
             ui::Font::Small);
    }
  }
}

// Дань и репарации, где фракция платит или получает (ТЗ 1.e.ii).
void tributeSection(App& a, const World& w, Id id, bool ro) {
  struct Row {
    const Deal* d;
    bool receive;
    double amount;
    int left, turns;
  };
  std::vector<Row> rows;
  w.deals.each([&](const Deal& d) {
    if (d.kind == DealKind::Trade || d.status != DealStatus::Active) return;
    if (d.a != id && d.b != id) return;
    Row r{&d, d.a == id, 0, 0, 0};
    for (const DealItem& it : d.items)
      if (it.res == kGold && it.mode == DealMode::PerTurn) {
        r.amount += it.amount;
        r.left = std::max(r.left, it.left);
        r.turns = std::max(r.turns, it.turns);
      }
    rows.push_back(r);
  });
  std::stable_sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
    if (x.receive != y.receive) return x.receive;
    return x.d->id < y.d->id;
  });
  ui::Section s("Дань и репарации", "tribute", {.badge = rows.empty() ? std::string() : std::to_string(rows.size())});
  if (!s) return;
  const ui::Theme& t = ui::theme();
  if (rows.empty()) ui::label("Нет действующих выплат.", {.ink = ui::Ink::Muted});
  for (const Row& r : rows) {
    const Deal& d = *r.d;
    const Id other = r.receive ? d.b : d.a;
    const Faction* of = w.faction(other);
    ui::IdScope sc{i64(d.id)};
    ui::Card c({.pad = 10, .tone = r.receive ? ui::Tone::Success : ui::Tone::Danger});
    const auto& kind = schema::kDealKinds[int(d.kind)];
    {
      ui::Row row({ui::px(20), ui::fr(1), ui::px(24)}, 24, 8);
      ui::icon(kind.icon, r.receive ? ui::Ink::Success : ui::Ink::Danger, 18, kind.name);
      ui::label(std::string(kind.name) + (r.receive ? " от" : " в пользу"), {.ink = ui::Ink::Dim});
      if (ui::iconButton("close", d.kind == DealKind::Tribute ? "Отменить дань" : "Отменить репарации",
                         {.size = ui::Size::Small, .disabled = ro, .tone = ui::Tone::Danger})) {
        Id did = d.id;
        std::string what = d.kind == DealKind::Tribute ? "дань" : "репарации";
        a.confirm(d.kind == DealKind::Tribute ? "Отменить дань?" : "Отменить репарации?",
                  "Выплаты прекратятся со следующего хода. Отменить можно сочетанием Ctrl+Z.", "Отменить " + what, true,
                  [did](App& x) { x.act("Отменить выплаты", [&](Tx& tx) { rules::cancelDeal(tx, did); }); });
      }
      a.markUi("tribute.cancel." + std::to_string(d.id));
    }
    {
      ui::Row row({ui::fr(1), ui::px(104)}, 26, 8);
      if (of) w::factionChip(other);   // в ячейке: естественная ширина, длинное название — с многоточием
      else ui::label("—", {.ink = ui::Ink::Muted});
      ui::label((r.receive ? "+" : "−") + money(r.amount) + " / ход", {.font = ui::Font::Strong, .ink = r.receive ? ui::Ink::Success : ui::Ink::Danger,
                                                                            .align = ui::Align::Right, .tooltip = "Золото за ход"});
    }
    double k = r.turns > 0 ? double(r.left) / double(r.turns) : 0;
    ui::progress(k, {.color = r.receive ? t.success : t.danger, .height = 5, .text = "ещё " + nTurns(r.left)});
  }
  ui::spacer(2);
  {
    ui::Disabled dis(ro);
    if (ui::button("Навязать дань / репарации", {.icon = "tribute", .fill = true})) {
      if (!a.openDialog("tribute", id)) a.toast("Диалог дани и репараций пока недоступен", ToastKind::Warning, "tribute");
    }
    a.markUi("economy.impose");
  }
}

void drawEconomy(App& a, Id id) {
  const World& w = frameWorld(a);
  const Faction* f = w.faction(id);
  if (!f) return;
  const bool ro = a.readOnly();
  auto calc = rules::calc(w);
  const rules::FactionCalc* fc = calc->faction(id);
  if (!fc) return;
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 10);
    ui::stat(money(f->treasury()), f->treasury() < 0 ? "Казна в долгу" : "Казна",
             {.icon = "treasury", .tone = f->treasury() < 0 ? ui::Tone::Danger : ui::Tone::Accent,
              .tooltip = "Текущая казна; при завершении хода прибавляется чистый доход"});
    a.markUi("economy.treasury");
    ui::stat(moneySigned(fc->net), "Чистый доход", {.icon = fc->net >= 0 ? "trend-up" : "trend-down",
                                                         .tone = fc->net >= 0 ? ui::Tone::Success : ui::Tone::Danger,
                                                         .tooltip = "Доходы − расходы за ход; прибавляется к казне при завершении хода"});
  }
  ui::spacer(2);
  moneyCards(*f, *fc);
  taxSection(a, *f, id, ro);
  resourcesSection(a, w, *f, *fc, id, ro);
  tributeSection(a, w, id, ro);
}

int economyBadge(App& a, Id id) {
  auto calc = rules::calc(a.world());
  const rules::FactionCalc* fc = calc->faction(id);
  return fc && fc->net < -0.5 ? -1 : 0;
}

TabReg tab({kTabEconomy, "treasury", "Экономика", 30, SelType::Faction, nullptr, drawEconomy, economyBadge});

}  // namespace
}  // namespace rg::app
