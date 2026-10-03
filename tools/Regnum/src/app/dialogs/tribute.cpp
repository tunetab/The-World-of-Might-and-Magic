// Regnum — дань и репарации (DialogReg «tribute», arg — фракция, которая навязывает выплаты и получает их):
// плательщик, вид (дань или репарации), золото за ход, срок в ходах; итог и доход сторон после выплат;
// rules::imposeTribute. ТЗ 1.e.ii. Действующие выплаты видны и отменяются в редакторе торговли и вкладке фракции.
#include "app/app_internal.h"
#include "app/dialogs/turn_ui.h"
#include "app/widgets.h"

namespace rg::app {

namespace {

struct TributeDlg : Dialog {
  Id receiver = 0, payer = 0;
  int kind = 0;          // 0 — дань, 1 — репарации
  double amount = 50;
  int turns = 5;
  const char* id() const override { return "tribute"; }
  Style style(App&) override { return {"Дань или репарации", "tribute", ui::Tone::Warning, 600}; }

  bool draw(App& a) override {
    const World& w = a.world();
    if (receiver && !w.faction(receiver)) receiver = 0;
    if (payer && (!w.faction(payer) || payer == receiver)) payer = 0;
    ui::label("Выплата золотом каждый ход на указанный срок: из казны плательщика — в казну получателя.",
              {.font = ui::Font::Small, .ink = ui::Ink::Muted, .wrap = true});
    {
      ui::Row r({ui::fr(1), ui::px(28), ui::fr(1)}, ui::kAuto, 8);
      {
        ui::Group g(0, 6);
        ui::caption("Плательщик");
        w::factionPicker("payer", payer, w::FactionFilter::Any, "Выберите фракцию", receiver);
        a.markUi("tribute.payer");
      }
      {
        ui::Group g(0, 6);
        ui::spacer(22);
        ui::icon("arrow-right", ui::Ink::Muted, 18, "платит");
      }
      {
        ui::Group g(0, 6);
        ui::caption("Получатель");
        w::factionPicker("receiver", receiver, w::FactionFilter::Any, "Выберите фракцию", payer);
        a.markUi("tribute.receiver");
      }
    }
    ui::caption("Вид выплат");
    ui::segmented("kind", kind, {{"tribute", "Дань", "Выплаты зависимой стороны"}, {"reparations", "Репарации", "Возмещение после войны"}});
    a.markUi("tribute.kind");
    {
      ui::Row r({ui::fr(1), ui::fr(1)}, 30, 10);
      ui::numberField("amount", amount, {.min = 1, .max = 1e9, .step = 10, .icon = "coins", .tooltip = "Золото за ход"});
      a.markUi("tribute.amount");
      ui::numberField("turns", turns, {.min = 1, .max = 999, .unit = "ход|хода|ходов", .icon = "hourglass", .steppers = true, .tooltip = "Срок выплат"});
      a.markUi("tribute.turns");
    }
    auto c = rules::calc(w);
    const rules::FactionCalc* pc = payer ? c->faction(payer) : nullptr;
    const rules::FactionCalc* rc = receiver ? c->faction(receiver) : nullptr;
    {
      ui::Row r({ui::fr(1), ui::fr(1), ui::fr(1)}, 64, 10);
      ui::stat(fmtNum(amount * turns), "Всего золота", {.icon = "coins", .tone = ui::Tone::Accent});
      if (pc) {
        double net = pc->net - amount;
        ui::stat(fmtSigned(net), "Доход плательщика", {.icon = "trend-down", .tone = net < 0 ? ui::Tone::Danger : ui::Tone::Neutral,
                                                       .deltaText = "−" + fmtNum(amount), .invertDelta = true,
                                                       .tooltip = "Чистый доход за ход с учётом выплаты; сейчас " + fmtSigned(pc->net)});
      } else {
        ui::stat("—", "Доход плательщика", {.icon = "trend-down", .tone = ui::Tone::Neutral});
      }
      if (rc) {
        double net = rc->net + amount;
        ui::stat(fmtSigned(net), "Доход получателя", {.icon = "trend-up", .tone = ui::Tone::Success, .deltaText = "+" + fmtNum(amount),
                                                      .tooltip = "Чистый доход за ход с учётом выплаты; сейчас " + fmtSigned(rc->net)});
      } else {
        ui::stat("—", "Доход получателя", {.icon = "trend-up", .tone = ui::Tone::Neutral});
      }
    }
    if (pc && pc->net - amount < 0 && pc->treasury > 0) {
      double turnsLeft = pc->treasury / std::max(1e-9, amount - pc->net);
      if (turnsLeft < turns)
        ui::label("Казна плательщика уйдёт в долг примерно через " + nTurns(i64(std::max(1.0, std::floor(turnsLeft)))),
                  {.font = ui::Font::Small, .ink = ui::Ink::Warning, .icon = "warning"});
    }
    ui::ModalFooter f;
    if (ui::button("Отмена")) return false;
    a.markUi("tribute.cancel");
    bool ok = receiver && payer && receiver != payer && !a.readOnly();
    if (ui::button(kind == 0 ? "Навязать дань" : "Назначить репарации", {.variant = ui::Variant::Primary, .icon = kind == 0 ? "tribute" : "reparations",
                                                                          .disabled = !ok, .isDefault = true})) {
      DealKind k = kind == 0 ? DealKind::Tribute : DealKind::Reparations;
      Id rcv = receiver, pay = payer;
      double amt = amount;
      int n = turns;
      if (a.act(kind == 0 ? "Навязать дань" : "Назначить репарации", [&](Tx& tx) { rules::imposeTribute(tx, k, rcv, pay, amt, n); })) {
        a.toast(w.factionName(pay) + (kind == 0 ? " платит дань: " : " платит репарации: ") + fmtNum(amt) + " золота за ход, " + nTurns(n),
                ToastKind::Success, kind == 0 ? "tribute" : "reparations");
        return false;
      }
    }
    a.markUi("tribute.ok");
    return true;
  }
};

DialogReg reg({"tribute", [](App& a, Id arg) -> std::unique_ptr<Dialog> {
                 if (a.readOnly()) {
                   a.toast("Открыт прошлый ход — изменения недоступны", ToastKind::Warning, "lock", "К текущему ходу", [](App& x) { x.backToCurrent(); });
                   return nullptr;
                 }
                 auto d = std::make_unique<TributeDlg>();
                 d->receiver = a.world().faction(arg) ? arg : 0;
                 return d;
               }});

}  // namespace
}  // namespace rg::app
