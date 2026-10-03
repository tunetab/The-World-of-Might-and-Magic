// Regnum — торговля, подарки, дань и репарации (ТЗ 1.b.vii, 1.e.ii; RULES.md §8).
#include "rules/internal.h"

namespace rg::rules {

using namespace detail;

namespace detail {

namespace {
std::string itemText(const World& w, const DealItem& it) {
  std::string s = resName(w, it.res) + " " + amount(it.amount);
  if (it.mode == DealMode::PerTurn) s += " за ход, " + nTurns(it.turns);
  return s;
}
std::string sideText(const World& w, const Deal& d, DealSide side) {
  std::vector<std::string> parts;
  for (const DealItem& it : d.items)
    if (it.from == side) parts.push_back(itemText(w, it));
  return join(parts, ", ");
}
}  // namespace

std::string dealText(const World& w, const Deal& d) {
  std::string A = facName(w, d.a), B = facName(w, d.b);
  std::string sa = sideText(w, d, DealSide::A), sb = sideText(w, d, DealSide::B);
  if (d.kind != DealKind::Trade)
    return B + " выплачивает " + A + (d.kind == DealKind::Tribute ? " дань: " : " репарации: ") + sb;
  if (sb.empty()) return A + " дарит " + B + ": " + sa;
  if (sa.empty()) return B + " дарит " + A + ": " + sb;
  return "Сделка " + A + " и " + B + ". " + A + " передаёт: " + sa + "; " + B + " передаёт: " + sb;
}

Id conclude(Tx& tx, Deal d, bool log) {
  DealCheck c = validateDeal(tx.w(), d);
  if (!c.ok) fail(join(c.problems, "; "));
  bool perTurn = false;
  for (DealItem& it : d.items) {
    if (it.mode == DealMode::PerTurn) {
      it.left = it.turns;
      perTurn = true;
    } else {
      it.left = 0;
      Id payer = it.from == DealSide::A ? d.a : d.b, payee = it.from == DealSide::A ? d.b : d.a;
      addStock(tx.faction(payer), it.res, -it.amount);
      addStock(tx.faction(payee), it.res, it.amount);
    }
  }
  d.id = 0;
  d.turn = tx.w().turn();
  d.status = perTurn ? DealStatus::Active : DealStatus::Done;
  Id id = tx.add(d).id;
  if (log) addLog(tx, LogKind::Trade, dealText(tx.w(), *tx.w().deal(id)), LogRefs{0, 0, {d.a, d.b}});
  return id;
}

}  // namespace detail

DealCheck validateDeal(const World& w, const Deal& d) {
  DealCheck c;
  auto bad = [&](std::string s) {
    if (!contains(c.problems, s)) c.problems.push_back(std::move(s));
  };
  if (int(d.kind) < 0 || int(d.kind) > 2) bad("Неизвестный вид сделки");
  if (d.a == 0 || d.b == 0) bad("Не выбрана сторона сделки");
  else if (!w.faction(d.a) || !w.faction(d.b)) bad("Сторона сделки не найдена");
  else if (d.a == d.b) bad("Стороны сделки совпадают");
  if (d.items.empty()) bad("В сделке нет ни одной позиции");
  std::map<std::pair<Id, Id>, double> once;  // (плательщик, ресурс) -> сумма разовых позиций
  for (const DealItem& it : d.items) {
    if (int(it.from) < 0 || int(it.from) > 1 || int(it.mode) < 0 || int(it.mode) > 1) {
      bad("Неверная позиция сделки");
      continue;
    }
    if (!w.resource(it.res)) {
      bad("Ресурс позиции не найден");
      continue;
    }
    if (!std::isfinite(it.amount) || !(it.amount > 0)) bad("Количество ресурса «" + resName(w, it.res) + "» должно быть больше нуля");
    if (it.mode == DealMode::PerTurn && it.turns < 1) bad("Срок выплат должен быть не меньше одного хода");
    if (d.kind != DealKind::Trade && (it.res != kGold || it.mode != DealMode::PerTurn || it.from != DealSide::B))
      bad("Дань и репарации выплачиваются золотом каждый ход");
    if (it.mode == DealMode::Once && std::isfinite(it.amount) && it.amount > 0)
      once[{it.from == DealSide::A ? d.a : d.b, it.res}] += it.amount;
  }
  for (auto& [key, need] : once) {
    const Faction* f = w.faction(key.first);
    if (!f) continue;
    double have = f->stock(key.second);
    if (have + 1e-9 < need)
      bad("Недостаточно ресурса «" + resName(w, key.second) + "» у " + facName(w, key.first) + ": нужно " + amount(need) + ", есть " +
          amount(std::max(0.0, have)));
  }
  c.ok = c.problems.empty();
  return c;
}

Id concludeDeal(Tx& tx, Deal d) { return conclude(tx, std::move(d), true); }

void cancelDeal(Tx& tx, Id deal) {
  const Deal* d = tx.w().deal(deal);
  if (!d) fail(deal ? "Сделка не найдена" : "Не выбрана сделка");
  if (d->status == DealStatus::Done) fail("Сделка уже выполнена");
  if (d->status == DealStatus::Cancelled) fail("Сделка уже расторгнута");
  DealKind kind = d->kind;
  Id a = d->a, b = d->b;
  tx.deal(deal).status = DealStatus::Cancelled;
  std::string text = kind == DealKind::Trade ? "Расторгнута сделка " + facName(tx.w(), a) + " и " + facName(tx.w(), b)
                                             : std::string(kind == DealKind::Tribute ? "Отменена дань: " : "Отменены репарации: ") +
                                                   facName(tx.w(), b) + " больше не платит " + facName(tx.w(), a);
  addLog(tx, kind == DealKind::Trade ? LogKind::Trade : LogKind::Diplomacy, text, LogRefs{0, 0, {a, b}});
}

Id imposeTribute(Tx& tx, DealKind kind, Id receiver, Id payer, double amountPerTurn, int turns) {
  if (kind != DealKind::Tribute && kind != DealKind::Reparations) fail("Выберите дань или репарации");
  needFaction(tx.w(), receiver);
  needFaction(tx.w(), payer);
  if (receiver == payer) fail("Фракция не может платить дань самой себе");
  needFinite(amountPerTurn, "Выплата за ход");
  if (!(amountPerTurn > 0)) fail("Выплата за ход должна быть больше нуля");
  if (turns < 1) fail("Срок выплат должен быть не меньше одного хода");
  Deal d;
  d.kind = kind;
  d.a = receiver;
  d.b = payer;
  d.items.push_back(DealItem{DealSide::B, kGold, amountPerTurn, DealMode::PerTurn, turns, turns});
  Id id = conclude(tx, std::move(d), false);
  addLog(tx, LogKind::Diplomacy,
         facName(tx.w(), payer) + " выплачивает " + facName(tx.w(), receiver) + (kind == DealKind::Tribute ? " дань: " : " репарации: ") +
             amount(amountPerTurn) + " золота за ход, " + nTurns(turns),
         LogRefs{0, 0, {receiver, payer}});
  return id;
}

}  // namespace rg::rules
