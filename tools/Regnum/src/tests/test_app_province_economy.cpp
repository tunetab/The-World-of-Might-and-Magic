// Сценарии вкладки «Экономика» инспектора провинции: сумма влияния гильдий больше 100 % отклоняется с сообщением,
// предел штабов (5 на провинцию), добавление гильдии, местный налог (общий не меньше 1 %), количество ресурса ≥ 0,
// снимок вкладки.
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

struct HideTestRegs {
  std::vector<app::TabDef> tabs;
  std::vector<app::HeaderDef> headers;
  std::vector<app::DrawerDef> drawers;
  template <class T>
  static void strip(std::vector<T>& v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](const T& d) { return std::string_view(d.id).starts_with("test."); }), v.end());
  }
  HideTestRegs() {
    auto& t = const_cast<std::vector<app::TabDef>&>(app::tabs());
    auto& h = const_cast<std::vector<app::HeaderDef>&>(app::headers());
    auto& d = const_cast<std::vector<app::DrawerDef>&>(app::drawers());
    tabs = t;
    headers = h;
    drawers = d;
    strip(t);
    strip(h);
    strip(d);
  }
  ~HideTestRegs() {
    const_cast<std::vector<app::TabDef>&>(app::tabs()) = tabs;
    const_cast<std::vector<app::HeaderDef>&>(app::headers()) = headers;
    const_cast<std::vector<app::DrawerDef>&>(app::drawers()) = drawers;
  }
};

const Province* prov(Harness& h, Id pid) { return h->world().province(pid); }

bool hasToast(Harness& h, std::string_view part) {
  for (auto& t : h->toasts())
    if (t.text.find(part) != std::string::npos) return true;
  return false;
}

// Открыть провинцию pid (выбор и показ на карте) на вкладке tab.
void open(Harness& h, Id pid, const char* tab) {
  h->ui.tabOf[app::SelType::Province] = tab;
  h->select(app::SelType::Province, pid, true);
  h.settle();
  CHECK(h->ui.sel == (app::Selection{app::SelType::Province, pid}));
}

double influenceOf(const Province& p, Id g) {
  double s = 0;
  for (auto& i : p.influence)
    if (i.guild == g) s += i.pct;
  return s;
}

void setNumber(Harness& h, const std::string& mark, const std::string& text) {
  const RectF* r = h->uiRect(mark);
  CHECK_MSG(r != nullptr, mark);
  h.click(r->cx(), r->cy());
  h.retype(text);
  h.key(Key::Enter);
  h.settle();
}

}  // namespace

TEST(app_province_influence_over_100_rejected) {
  HideTestRegs hide;
  Harness h("province_influence", 1440, 1500);
  h.demo();
  // Провинция с влиянием двух и более гильдий.
  Id pid = 0;
  h->world().provinces.each([&](const Province& p) {
    if (!pid && !p.sea && p.influence.size() >= 2) pid = p.id;
  });
  CHECK(pid != 0);
  open(h, pid, "province.economy");
  Province before = *prov(h, pid);
  Id g1 = before.influence[0].guild;
  double others = 0;
  for (auto& i : before.influence)
    if (i.guild != g1) others += i.pct;
  CHECK(others > 0);
  CHECK(h->uiRect("province.inf." + std::to_string(g1)) != nullptr);
  // 100 % первой гильдии при занятой доле других — отказ с сообщением, мир не изменён.
  h.dropToasts();
  setNumber(h, "province.inf." + std::to_string(g1), "100");
  CHECK(hasToast(h, "превысит 100"));
  CHECK_NEAR(influenceOf(*prov(h, pid), g1), influenceOf(before, g1), 1e-9);
  // Ровно свободная доля — принято.
  double free = 100 - others;
  setNumber(h, "province.inf." + std::to_string(g1), fmtNum(free, 1));
  CHECK_NEAR(influenceOf(*prov(h, pid), g1), std::round(free * 10) / 10, 1e-6);
  h.key(Key::Z, ctrl());
  CHECK_NEAR(influenceOf(*prov(h, pid), g1), influenceOf(before, g1), 1e-9);
  // Добавить гильдию без влияния в провинции: свободная доля (до 10 %).
  double sum = 0;
  for (auto& i : prov(h, pid)->influence) sum += i.pct;
  Id missing = 0;
  h->world().factions.each([&](const Faction& f) {
    if (!missing && f.isGuild() && influenceOf(*prov(h, pid), f.id) == 0 &&
        std::find(prov(h, pid)->hqs.begin(), prov(h, pid)->hqs.end(), f.id) == prov(h, pid)->hqs.end())
      missing = f.id;
  });
  if (missing && sum < 99.5) {
    const RectF* add = h->uiRect("province.addGuild");
    CHECK(add != nullptr);
    size_t n0 = prov(h, pid)->influence.size();
    h.click(add->cx(), add->cy());
    h.key(Key::Enter);   // первая гильдия списка
    h.settle();
    CHECK_EQ(prov(h, pid)->influence.size(), n0 + 1);
    const Influence& added = prov(h, pid)->influence.back();
    CHECK(added.pct > 0 && added.pct <= 10);
    double total = 0;
    for (auto& i : prov(h, pid)->influence) total += i.pct;
    CHECK(total <= 100 + 1e-9);
  }
}

TEST(app_province_hq_limit_enforced) {
  HideTestRegs hide;
  Harness h("province_hq", 1440, 1500);
  h.demo();
  Id pid = 0;
  h->world().provinces.each([&](const Province& p) {
    if (!pid && !p.sea && p.owner) pid = p.id;
  });
  CHECK(pid != 0);
  // Шесть гильдий с влиянием по 10 %, у пяти — штабы.
  std::vector<Id> guilds;
  h->world().factions.each([&](const Faction& f) {
    if (f.isGuild()) guilds.push_back(f.id);
  });
  h->store.transact("Подготовка", [&](Tx& tx) {
    while (guilds.size() < 6) guilds.push_back(rules::createFaction(tx, FactionKind::Guild, "Гильдия " + std::to_string(guilds.size() + 1)));
    Province& p = tx.province(pid);
    p.influence.clear();
    p.hqs.clear();
    for (size_t i = 0; i < 6; i++) rules::setInfluence(tx, pid, guilds[i], 10);
    for (size_t i = 0; i < 5; i++) rules::buildHq(tx, guilds[i], pid);
  });
  open(h, pid, "province.economy");
  CHECK_EQ(prov(h, pid)->hqs.size(), size_t(5));
  // Шестой штаб — отказ с сообщением.
  std::string sixth = "province.hq." + std::to_string(guilds[5]);
  CHECK(h->uiRect(sixth) != nullptr);
  h.dropToasts();
  CHECK(h.clickUi(sixth));
  h.settle();
  CHECK(hasToast(h, "штабов"));
  CHECK_EQ(prov(h, pid)->hqs.size(), size_t(5));
  h.dropToasts();
  h.settle();
  CHECK(h.shot("province_economy_hq"));
  // Закрыть штаб первой гильдии — тогда шестой открывается; у гильдии один штаб (повтор — закрытие).
  CHECK(h.clickUi("province.hq." + std::to_string(guilds[0])));
  h.settle();
  CHECK_EQ(prov(h, pid)->hqs.size(), size_t(4));
  CHECK(h.clickUi(sixth));
  h.settle();
  CHECK_EQ(prov(h, pid)->hqs.size(), size_t(5));
  CHECK(std::find(prov(h, pid)->hqs.begin(), prov(h, pid)->hqs.end(), guilds[5]) != prov(h, pid)->hqs.end());
  // Доход получают только гильдии со штабом.
  auto calc = rules::calc(h->world());
  for (auto& s : calc->province(pid)->guilds) {
    if (s.hq) CHECK(s.gross > 0 || calc->province(pid)->tradeValue == 0);
    else CHECK_EQ(s.gross, 0.0);
  }
  // Убрать гильдию из провинции: влияние и штаб.
  CHECK(h.clickUi("province.infDel." + std::to_string(guilds[5])));
  h.settle();
  CHECK_EQ(influenceOf(*prov(h, pid), guilds[5]), 0.0);
  CHECK(std::find(prov(h, pid)->hqs.begin(), prov(h, pid)->hqs.end(), guilds[5]) == prov(h, pid)->hqs.end());
  h.key(Key::Z, ctrl());
  CHECK_NEAR(influenceOf(*prov(h, pid), guilds[5]), 10, 1e-9);
}

TEST(app_province_tax_and_resource) {
  HideTestRegs hide;
  Harness h("province_tax", 1440, 1500);
  h.demo();
  Id pid = 0;
  h->world().provinces.each([&](const Province& p) {
    if (!pid && !p.sea && p.owner && p.resource) pid = p.id;
  });
  CHECK(pid != 0);
  open(h, pid, "province.economy");
  double stateTax = rules::calc(h->world())->province(pid)->taxState;
  // Местный налог может быть отрицательным; общий — не меньше 1 %.
  setNumber(h, "province.localTax", "-90");
  CHECK_NEAR(prov(h, pid)->localTax, -90, 1e-9);
  CHECK_NEAR(rules::calc(h->world())->province(pid)->taxTotal, std::max(1.0, stateTax - 90), 1e-9);
  setNumber(h, "province.localTax", "5");
  CHECK_NEAR(rules::calc(h->world())->province(pid)->taxTotal, stateTax + 5, 1e-9);
  // Количество ресурса не бывает отрицательным.
  setNumber(h, "province.amount", "-5");
  CHECK_NEAR(prov(h, pid)->resourceAmount, 0, 1e-9);
  CHECK_NEAR(rules::calc(h->world())->province(pid)->production, std::max(0.0, rules::calc(h->world())->province(pid)->fx[Fx::ResourceFlat]), 1e-9);
  setNumber(h, "province.amount", "40");
  CHECK_NEAR(prov(h, pid)->resourceAmount, 40, 1e-9);
  // Базовая торговая ценность → текущая (маршруты и модификаторы — по формуле).
  setNumber(h, "province.baseTrade", "200");
  const rules::ProvinceCalc* pc = rules::calc(h->world())->province(pid);
  CHECK_NEAR(pc->tradeBase, 200, 1e-9);
  CHECK_NEAR(pc->tradeValue, std::max(0.0, 200 * (1 + pc->fx[Fx::TradePct] / 100 + 0.1 * pc->routes) + pc->fx[Fx::TradeFlat]), 1e-6);
  CHECK(h->uiRect("province.tradeValue") != nullptr);
  CHECK(h->uiRect("province.income") != nullptr);
}

TEST(app_province_economy_screen) {
  HideTestRegs hide;
  Harness h("province_economy_shot", 1440, 1000);
  h.demo();
  // Провинция со штабами и влиянием нескольких гильдий.
  Id pid = 0;
  size_t best = 0;
  h->world().provinces.each([&](const Province& p) {
    size_t score = p.hqs.size() * 10 + p.influence.size();
    if (!p.sea && score > best) {
      best = score;
      pid = p.id;
    }
  });
  CHECK(pid != 0);
  open(h, pid, "province.economy");
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("province_economy"));
  // Нижняя часть вкладки: прокрутка инспектора.
  const RectF* ins = h->uiRect("inspector");
  CHECK(ins != nullptr);
  h.wheel(ins->cx(), ins->bottom() - 120, -12);
  h.settle();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("province_economy_guilds"));
}

TEST(app_province_narrow_inspector) {
  HideTestRegs hide;
  Harness h("province_narrow", 1280, 900);
  h.demo();
  h->ui.inspectorWidth = 320;
  Id pid = 0;
  size_t best = 0;
  h->world().provinces.each([&](const Province& p) {
    size_t score = p.hqs.size() * 10 + p.influence.size();
    if (!p.sea && score > best) {
      best = score;
      pid = p.id;
    }
  });
  CHECK(pid != 0);
  open(h, pid, "province.economy");
  const RectF* ins = h->uiRect("inspector");
  CHECK(ins != nullptr);
  CHECK_NEAR(ins->w, 320, 0.5);
  h.wheel(ins->cx(), ins->bottom() - 120, -16);
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("province_economy_narrow"));
}
