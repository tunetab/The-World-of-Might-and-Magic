// Regnum — торговля в панелях: вкладка фракции «Торговля» (государства и гильдии, ТЗ 1.b.vii, 1.d.iii) — доходы
// и расходы по сделкам и дани, сделки фракции, «Новая сделка» с заполненной стороной, дань; выдвижная панель
// «Торговля» на ленте разделов — активные сделки всего мира и вход в редактор.
#include "app/app_internal.h"
#include "app/dialogs/turn_ui.h"
#include "app/editors/trade.h"
#include "app/widgets.h"

namespace rg::app {

namespace {

void newDeal(App& a, Id faction) {
  trade::startDraft(faction, 0);
  a.openEditor("trade", faction);
}

// ---------------------------------------------------------------- вкладка фракции
void drawTab(App& a, Id fid) {
  const World& w = a.world();
  const Faction* f = w.faction(fid);
  if (!f) return;
  auto c = rules::calc(w);
  const rules::FactionCalc* fc = c->faction(fid);
  double trade = fc ? fc->incTrade - fc->expTrade : 0;
  double trib = fc ? fc->incTribute - fc->expTribute : 0;
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 8);
    std::string tt = fc ? "Поступления " + turnui::money(fc->incTrade) + ", выплаты " + turnui::money(fc->expTrade) : std::string();
    ui::stat(fmtSigned(trade), "Торговля за ход", {.icon = "trade", .tone = trade < 0 ? ui::Tone::Danger : ui::Tone::Success, .tooltip = tt});
    std::string tb = fc ? "Получаем " + turnui::money(fc->incTribute) + ", платим " + turnui::money(fc->expTribute) : std::string();
    ui::stat(fmtSigned(trib), "Дань за ход", {.icon = "tribute", .tone = trib < 0 ? ui::Tone::Danger : ui::Tone::Warning, .tooltip = tb});
  }
  {
    ui::Disabled dis(a.readOnly());
    ui::Row r({ui::fr(1.4f), ui::fr(1)}, 30, 8);
    if (ui::button("Новая сделка", {.variant = ui::Variant::Primary, .icon = "plus", .fill = true})) newDeal(a, fid);
    a.markUi("faction.trade.new");
    if (ui::button("Дань", {.icon = "tribute", .fill = true, .tooltip = "Навязать дань или репарации другой фракции"})) a.openDialog("tribute", fid);
    a.markUi("faction.trade.tribute");
  }
  std::vector<const Deal*> all = trade::dealsOf(w, fid);
  std::vector<const Deal*> active, done;
  for (const Deal* d : all) (d->status == DealStatus::Active ? active : done).push_back(d);
  if (all.empty()) {
    ui::spacer(12);
    if (ui::emptyState("handshake", "Сделок пока нет.", a.readOnly() ? std::string_view() : "Новая сделка", "plus")) newDeal(a, fid);
    return;
  }
  if (ui::Section s("Активные", "repeat", {.badge = std::to_string(active.size()), .card = false}); s) {
    if (active.empty()) ui::label("Активных сделок нет", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
    for (const Deal* d : active) trade::dealCard(a, w, *d, {.perspective = fid});
  }
  if (ui::Section s("Завершённые", "check", {.defaultOpen = false, .badge = std::to_string(done.size()), .card = false}); s) {
    size_t n = std::min<size_t>(done.size(), 30);
    for (size_t i = 0; i < n; i++) trade::dealCard(a, w, *done[i], {.perspective = fid});
    if (done.size() > n) {
      if (ui::link("Все сделки — в редакторе торговли", "trade")) a.openEditor("trade", fid);
    }
  }
}

int tabBadge(App& a, Id fid) {
  int n = 0;
  a.world().deals.each([&](const Deal& d) {
    if (d.status == DealStatus::Active && (d.a == fid || d.b == fid)) n++;
  });
  return n;
}

TabReg tab({"faction.trade", "trade", "Торговля", 45, SelType::Faction, nullptr, drawTab, tabBadge});

// ---------------------------------------------------------------- выдвижная панель
void drawDrawer(App& a) {
  const World& w = a.world();
  {
    ui::Disabled dis(a.readOnly());
    ui::Row r({ui::fr(1), ui::px(30)}, 30, 6);
    if (ui::button("Новая сделка", {.variant = ui::Variant::Primary, .icon = "plus", .fill = true})) newDeal(a, 0);
    a.markUi("trade.drawer.new");
    if (ui::iconButton("tribute", "Дань или репарации")) a.openDialog("tribute");
    a.markUi("trade.drawer.tribute");
  }
  int active = 0;
  double gold = 0;
  w.deals.each([&](const Deal& d) {
    if (d.status != DealStatus::Active) return;
    active++;
    for (auto& it : d.items)
      if (it.mode == DealMode::PerTurn && it.res == kGold && it.left > 0) gold += it.amount;
  });
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 8);
    ui::stat(std::to_string(active), "Активных", {.icon = "handshake", .tone = ui::Tone::Accent});
    ui::stat(fmtNum(gold), "Золота за ход", {.icon = "coins", .tone = ui::Tone::Success});
  }
  {
    ui::HStack hs(24, ui::Align::Left, 6);
    ui::caption("Активные сделки");
    ui::flex();
    if (ui::link("Все", "chevron-right")) a.openEditor("trade");
    a.markUi("trade.drawer.all");
  }
  bool any = false;
  for (const Deal* d : trade::dealsOf(w, 0)) {
    if (d->status != DealStatus::Active) continue;
    any = true;
    if (trade::dealCard(a, w, *d, {.clickable = true})) {
      trade::draft().highlight = d->id;
      a.openEditor("trade");
    }
  }
  if (!any) {
    ui::spacer(8);
    if (ui::emptyState("handshake", "Активных сделок нет.", a.readOnly() ? std::string_view() : "Новая сделка", "plus")) newDeal(a, 0);
  }
}

DrawerReg drawer({"trade", "trade", "Торговля", 60, drawDrawer});

}  // namespace
}  // namespace rg::app
