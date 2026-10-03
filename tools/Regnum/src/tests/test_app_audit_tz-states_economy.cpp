// Аудит ТЗ 1.b (государства) и 1.e (экономика), аудитор tz-states: воспроизведения найденных расхождений на
// демонстрационном мире через настоящее приложение (вкладки инспектора, диалог дани, дерево технологий).
// Ожидаемое по ТЗ поведение проверяется макросом AUDIT: без REGNUM_AUDIT=1 расхождение только пишется в журнал
// (набор тестов не падает), с REGNUM_AUDIT=1 — проваливает тест. Исправляющие превращают AUDIT в CHECK.
#include <cstdio>

#include "app/editors/techtree.h"
#include "tests/test_app_faction_util.h"

using namespace rg;
using namespace rg::apptest;
using namespace rg::factest;

namespace {

[[maybe_unused]] bool strictAudit() {
  const char* v = std::getenv("REGNUM_AUDIT");
  return v && *v == '1';
}

[[maybe_unused]] void auditExpect(bool ok, const char* file, int line, const std::string& what) {
  if (ok) return;
  std::fprintf(stderr, "AUDIT tz-states: %s\n", what.c_str());
  if (strictAudit()) test::fail(file, line, what);
}
#define AUDIT(cond, msg) auditExpect((cond), __FILE__, __LINE__, (msg))

bool has(const std::vector<Id>& v, Id x) { return std::find(v.begin(), v.end(), x) != v.end(); }

Id characterByName(const World& w, std::string_view name) {
  Id r = 0;
  w.characters.each([&](const Character& c) {
    if (c.name == name) r = c.id;
  });
  return r;
}

Id techByName(const World& w, Id faction, std::string_view name) {
  Id r = 0;
  w.techs.each([&](const Tech& t) {
    if (t.faction == faction && t.name == name) r = t.id;
  });
  return r;
}

void shotClean(Harness& h, const std::string& name) {
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot(name));
}

// Ввод в поле модального окна: щелчок, выделить всё, текст, Tab (Enter нажал бы основную кнопку).
void enterDlg(Harness& h, const std::string& name, const std::string& value) {
  CHECK_MSG(h.clickUi(name), name);
  h.key(Key::A, ctrl());
  h.type(value);
  h.key(Key::Tab);
  h.settle();
}

// Независимый пересчёт экономики фракции по формулам docs/RULES.md §2–3 (эффекты и число маршрутов берутся из мира).
struct Econ {
  double provTax = 0, guildTax = 0, hqNet = 0, trade = 0, tribute = 0;
  double army = 0, fleet = 0, spec = 0, tradeOut = 0, tributeOut = 0;
  double own() const { return provTax + guildTax + hqNet; }
  double gross() const { return own() + trade + tribute; }
};

std::map<Id, Econ> recompute(const World& w, const rules::Calc& c) {
  std::map<Id, Econ> e;
  w.provinces.each([&](const Province& p) {
    if (p.sea) return;
    const Faction* o = w.faction(p.owner);
    if (o && !o->isState()) o = nullptr;
    rules::Effects fx = rules::provinceEffects(w, p.id);
    auto rit = c.routeCounts.find(p.id);
    int routes = rit == c.routeCounts.end() ? 0 : rit->second;
    double V = std::max(0.0, std::max(0.0, p.baseTrade) * (1 + fx[Fx::TradePct] / 100 + 0.10 * routes) + fx[Fx::TradeFlat]);
    double t = std::clamp((o ? std::max(0.0, o->tax) : 0.0) + p.localTax, 1.0, 100.0);
    Id rec = o ? o->id : 0;
    if (p.occupied && p.occupier && p.occupier != p.owner && w.faction(p.occupier)) {
      if (w.settings->occupiedIncome == OccupiedIncome::Occupier) rec = p.occupier;
      else if (w.settings->occupiedIncome == OccupiedIncome::None) rec = 0;
    }
    double grossHq = 0, gtax = 0;
    for (const Influence& in : p.influence) {
      const Faction* g = w.faction(in.guild);
      if (!g || !g->isGuild() || !has(p.hqs, in.guild) || !(in.pct > 0)) continue;
      double gross = V * in.pct / 100;
      grossHq += gross;
      gtax += gross * t / 100;
      e[in.guild].hqNet += gross - gross * t / 100;
    }
    if (rec) {
      e[rec].provTax += std::max(0.0, V - grossHq) * t / 100;
      e[rec].guildTax += gtax;
      if (p.resource == kGold)
        e[rec].provTax += std::max(0.0, std::max(0.0, p.resourceAmount) * (1 + fx[Fx::ResourcePct] / 100) + fx[Fx::ResourceFlat]);
    }
  });
  w.factions.each([&](const Faction& f) {
    rules::Effects fx = rules::factionEffects(w, f.id);
    for (const ArmyRow& r : f.army) e[f.id].army += double(r.total) * r.upkeep * std::max(0.0, 1 + fx[Fx::ArmyUpkeepPct] / 100);
    for (const FleetRow& r : f.fleet) e[f.id].fleet += double(r.total) * r.upkeep * std::max(0.0, 1 + fx[Fx::FleetUpkeepPct] / 100);
  });
  // Специалисты: различные персонажи с ролью — правитель, места совета (любой фракции), герои фракции.
  w.factions.each([&](const Faction& f) {
    std::vector<Id> paid;
    auto add = [&](Id ch) {
      if (ch && w.character(ch) && !has(paid, ch)) paid.push_back(ch);
    };
    add(f.ruler);
    for (const CouncilSeat& s : f.council) add(s.character);
    w.characters.each([&](const Character& ch) {
      if (ch.hero && ch.faction == f.id) add(ch.id);
    });
    for (Id ch : paid) e[f.id].spec += std::max(0.0, w.character(ch)->upkeep);
  });
  w.deals.each([&](const Deal& d) {
    if (d.status != DealStatus::Active) return;
    for (const DealItem& it : d.items) {
      if (it.mode != DealMode::PerTurn || it.left <= 0 || it.res != kGold) continue;
      Id payer = it.from == DealSide::A ? d.a : d.b, payee = it.from == DealSide::A ? d.b : d.a;
      if (d.kind == DealKind::Trade) {
        e[payee].trade += it.amount;
        e[payer].tradeOut += it.amount;
      } else {
        e[payee].tribute += it.amount;
        e[payer].tributeOut += it.amount;
      }
    }
  });
  return e;
}

}  // namespace

// ---------------------------------------------------------------- 1.e.i: формулы экономики, казна после хода
// Пересчёт по RULES.md совпадает с расчётом панели «Экономика»; завершение хода прибавляет к казне ровно
// «Чистый доход». Отдельно фиксируется баланс демонстрационного мира (все государства — в глубоком минусе).
TEST(app_audit_tz_states_economy_formulas) {
  HideTestRegs regs;
  Harness h("audit_tzstates_econ");
  h.demo();
  h.waitMap();
  World w = h->store.world();
  auto c = rules::calc(w);
  auto e = recompute(w, *c);
  int states = 0, negative = 0;
  w.factions.each([&](const Faction& f) {
    const rules::FactionCalc* fc = c->faction(f.id);
    CHECK(fc != nullptr);
    if (!fc) return;
    const Econ& x = e[f.id];
    CHECK_NEAR(fc->incProvinces, x.provTax, 1e-6);
    CHECK_NEAR(fc->incGuildTax, x.guildTax, 1e-6);
    CHECK_NEAR(fc->incGuilds, x.hqNet, 1e-6);
    CHECK_NEAR(fc->incTrade, x.trade, 1e-6);
    CHECK_NEAR(fc->incTribute, x.tribute, 1e-6);
    CHECK_NEAR(fc->expArmy, x.army, 1e-6);
    CHECK_NEAR(fc->expFleet, x.fleet, 1e-6);
    CHECK_NEAR(fc->expSpecialists, x.spec, 1e-6);
    rules::Effects ffx = rules::factionEffects(w, f.id);
    double inc = x.own() * std::max(0.0, 1 + ffx[Fx::IncomePct] / 100) + x.trade + x.tribute;  // переводы — без модификатора
    double exp = x.army + x.fleet + x.spec + x.tradeOut + x.tributeOut;
    CHECK_NEAR(fc->incTotal, inc, 1e-6);
    CHECK_NEAR(fc->expTotal, exp, 1e-6);
    if (f.isState()) {
      states++;
      if (fc->net < 0) negative++;
      std::fprintf(stderr, "AUDIT tz-states demo: %-32s казна %9.1f доход %8.1f расход %9.1f чистый %+10.1f (войска %.0f, флот %.0f, спец. %.0f)\n",
                   f.name.c_str(), f.treasury(), fc->incTotal, fc->expTotal, fc->net, fc->expArmy, fc->expFleet, fc->expSpecialists);
    }
  });
  CHECK_MSG(negative < states, "демо-мир: у всех " + std::to_string(states) + " государств чистый доход отрицательный — доход ~десятки, расходы ~десятки тысяч");

  // Завершение хода: казна каждой фракции меняется ровно на «Чистый доход», запасы — на «Итого за ход».
  std::map<Id, double> tre0;
  std::map<Id, std::map<Id, double>> res0;
  w.factions.each([&](const Faction& f) {
    tre0[f.id] = f.treasury();
    res0[f.id] = f.res;
  });
  CHECK(h->endTurnNow());
  h.settle();
  const World& w1 = h->store.world();
  w.factions.each([&](const Faction& f) {
    const rules::FactionCalc* fc = c->faction(f.id);
    const Faction* f1 = w1.faction(f.id);
    if (!fc || !f1) return;
    CHECK_NEAR(f1->treasury() - tre0[f.id], fc->net, 1e-6);
    for (auto& [r, flow] : fc->resources) {
      if (r == kGold) continue;
      double was = res0[f.id].count(r) ? res0[f.id][r] : 0.0;
      double expect = std::max(0.0, was + flow.net);
      AUDIT(std::fabs(f1->stock(r) - expect) < 1e-6,
            f.name + ": запас ресурса " + std::to_string(r) + " после хода " + std::to_string(f1->stock(r)) + ", ожидалось " + std::to_string(expect));
    }
  });
}

// ---------------------------------------------------------------- 1.e.ii: дань — «отнимается у одного и отдаётся другому»
// Дань 100 золота за ход от Хельдвига Альмарину через диалог «Навязать дань / репарации» вкладки «Экономика».
// У Альмарина модификатор «Королевский указ» (+6 % дохода в казну): получатель получает 106, плательщик отдаёт 100 —
// выплата не сохраняется (золото создаётся из ничего); предпросмотр диалога при этом обещает +100.
TEST(app_audit_tz_states_tribute_income_modifier) {
  HideTestRegs regs;
  Harness h("audit_tzstates_tribute");
  h.demo();
  h.waitMap();
  Id alm = findFaction(h->world(), "Королевство Альмарин");
  Id hel = findFaction(h->world(), "Северный союз Хельдвиг");
  CHECK(alm && hel);
  CHECK(rules::factionEffects(h->world(), alm)[Fx::IncomePct] > 0);
  auto c0 = rules::calc(h->store.world());
  const double recv0 = c0->faction(alm)->net, pay0 = c0->faction(hel)->net;
  const double t0r = h->world().faction(alm)->treasury(), t0p = h->world().faction(hel)->treasury();

  openTab(h, alm, "faction.economy");
  CHECK(clickIn(h, "economy.impose"));
  h.settle();
  CHECK(h->hasDialog("tribute"));
  CHECK(h.clickUi("tribute.payer"));
  h.type("Хельдвиг");
  h.key(Key::Enter);
  h.settle();
  enterDlg(h, "tribute.amount", "100");
  enterDlg(h, "tribute.turns", "3");
  shotClean(h, "audit_tzstates_tribute_dialog");
  // Что обещает диалог: «Доход получателя» = чистый доход + 100.
  const double promised = recv0 + 100;
  CHECK(h.clickUi("tribute.ok"));
  h.settle();
  CHECK(!h->hasDialog("tribute"));
  auto c1 = rules::calc(h->store.world());
  const double recv1 = c1->faction(alm)->net, pay1 = c1->faction(hel)->net;
  CHECK_NEAR(pay1 - pay0, -100, 1e-6);
  CHECK_MSG(std::fabs((recv1 - recv0) - 100) < 1e-6,
        "дань 100/ход: получатель с +6 % дохода получает " + std::to_string(recv1 - recv0) + " вместо 100 (ТЗ 1.e.ii: выплата отнимается у одного и отдаётся другому)");
  CHECK_MSG(std::fabs(promised - recv1) < 1e-6,
        "диалог дани обещает чистый доход получателя " + std::to_string(promised) + ", фактически " + std::to_string(recv1));
  // После хода: сколько золота ушло и сколько пришло именно по дани.
  CHECK(h->endTurnNow());
  h.settle();
  const double dr = h->world().faction(alm)->treasury() - t0r - recv0;   // прирост сверх прежнего чистого дохода
  const double dp = h->world().faction(hel)->treasury() - t0p - pay0;
  CHECK_MSG(std::fabs(dr + dp) < 1e-6, "после хода по дани получено " + std::to_string(dr) + ", уплачено " + std::to_string(-dp));
}

// ---------------------------------------------------------------- 1.b.iv / 1.e.i: совет и герои — расход «специалисты»
// Вкладки «Совет» и «Герои» пишут «Содержание за ход … входит в расход «специалисты»». Но расход считается по всем
// персонажам фракции: убранный из совета или из героев продолжает получать содержание, а советник другой фракции
// показан в сумме совета, но в расход этого государства не входит.
TEST(app_audit_tz_states_specialists_upkeep) {
  HideTestRegs regs;
  Harness h("audit_tzstates_specialists");
  h.demo();
  h.waitMap();
  Id alm = findFaction(h->world(), "Королевство Альмарин");
  CHECK(alm);
  auto specOf = [&] { return rules::calc(h->store.world())->faction(alm)->expSpecialists; };
  auto councilSum = [&] {
    double s = 0;
    for (const CouncilSeat& seat : h->world().faction(alm)->council)
      if (const Character* ch = h->world().character(seat.character)) s += std::max(0.0, ch->upkeep);
    return s;
  };

  // 1. Убрать место совета (корзина в строке).
  openTab(h, alm, "faction.council");
  const Faction* f = h->world().faction(alm);
  CHECK(!f->council.empty());
  const Id who = f->council[0].character;
  const double up = h->world().character(who)->upkeep;
  CHECK(up > 0);
  const double spec0 = specOf(), council0 = councilSum();
  CHECK(clickIn(h, "council.remove.0"));
  h.step();
  CHECK_EQ(h->world().faction(alm)->council.size(), f->council.size() - 1);
  CHECK_NEAR(councilSum(), council0 - up, 1e-9);   // вкладка «Совет»: «Содержание за ход» уменьшилось
  CHECK_MSG(std::fabs(specOf() - (spec0 - up)) < 1e-6,
        "место совета убрано, «Содержание за ход» совета −" + std::to_string(up) + ", а расход «специалисты» не изменился: " + std::to_string(specOf()));
  shotClean(h, "audit_tzstates_council_removed");
  h->undo();
  h.step();

  // 2. Советник другого государства: сумма совета растёт, расход «специалисты» Альмарина — нет.
  // Герой Хельдвига с содержанием 15 (у снятого советника Альмарина — 8): сумма совета меняется на +7.
  Id foreign = characterByName(h->world(), "Бьорн Медведь");
  CHECK(foreign);
  const double fup = h->world().character(foreign)->upkeep;
  CHECK(h->world().character(foreign)->faction != alm);
  CHECK(std::fabs(fup - up) > 1e-9);
  const double spec1 = specOf(), council1 = councilSum();
  openTab(h, alm, "faction.council");
  CHECK(clickIn(h, "council.who.0"));
  h.type("Бьорн");
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(h->world().faction(alm)->council[0].character, foreign);
  const double council2 = councilSum();
  CHECK_NEAR(council2, council1 - up + fup, 1e-9);
  CHECK_MSG(std::fabs((specOf() - spec1) - (council2 - council1)) < 1e-6,
        "советник другой фракции: сумма совета изменилась на " + std::to_string(council2 - council1) + ", расход «специалисты» — на " +
            std::to_string(specOf() - spec1));
  shotClean(h, "audit_tzstates_council_foreign");
  h->undo();
  h.step();

  // 3. «Убрать из героев»: вкладка «Герои» уменьшает «Содержание за ход», расход «специалисты» — нет.
  openTab(h, alm, "faction.heroes");
  std::vector<const Character*> heroes;
  h->world().characters.each([&](const Character& ch) {
    if (ch.faction == alm && ch.hero) heroes.push_back(&ch);
  });
  std::sort(heroes.begin(), heroes.end(), [](const Character* x, const Character* y) { return compareRu(x->name, y->name) < 0; });
  int k = -1;
  for (size_t i = 0; i < heroes.size(); i++)
    if (heroes[i]->id != h->world().faction(alm)->ruler && heroes[i]->upkeep > 0) {
      k = int(i);
      break;
    }
  CHECK(k >= 0);
  const Id hid = heroes[size_t(k)]->id;
  const double hup = heroes[size_t(k)]->upkeep;
  const double spec2 = specOf();
  CHECK(clickIn(h, "heroes.remove." + std::to_string(k)));
  h.step();
  CHECK(!h->world().character(hid)->hero);
  CHECK_MSG(std::fabs(specOf() - (spec2 - hup)) < 1e-6,
        "герой убран из списка, содержание героев −" + std::to_string(hup) + ", расход «специалисты» не изменился: " + std::to_string(specOf()));
}

// ---------------------------------------------------------------- 1.b.v: изученная технология с неизученным условием
// В дереве технологий к уже изученной «Обработке железа» можно добавить условие «Мореходство» (не изучено).
// Получается изученная технология, для которой не изучена предшествующая, — противоречит ТЗ 1.b.v.
TEST(app_audit_tz_states_tech_studied_prereq) {
  HideTestRegs regs;
  Harness h("audit_tzstates_tech");
  h.demo();
  Id alm = findFaction(h->world(), "Королевство Альмарин");
  Id iron = techByName(h->world(), alm, "Обработка железа");
  Id sea = techByName(h->world(), alm, "Мореходство");
  CHECK(alm && iron && sea);
  CHECK(h->world().tech(iron)->studied);
  CHECK(!h->world().tech(sea)->studied);
  app::openTechTree(h.a(), alm, iron);
  h.settle();
  CHECK(h->uiRect("tt.side.addpre") != nullptr);
  // Пункты списка — технологии дерева по порядку ID, кроме самой и её условий.
  int idx = -1, n = 0;
  h->world().techs.each([&](const Tech& t) {
    if (t.faction != alm || t.id == iron || has(h->world().tech(iron)->prereqs, t.id)) return;
    if (t.id == sea) idx = n;
    n++;
  });
  CHECK(idx >= 0);
  CHECK(h.clickUi("tt.side.addpre"));
  for (int i = 0; i < idx; i++) h.key(Key::Down);
  h.key(Key::Enter);
  h.settle();
  const Tech* t = h->world().tech(iron);
  CHECK(t->studied);
  CHECK(!h->world().tech(sea)->studied);
  CHECK_MSG(!has(t->prereqs, sea), "изученная технология «Обработка железа» получила неизученное условие «Мореходство» — связь должна быть запрещена");
  shotClean(h, "audit_tzstates_tech_studied_prereq");
}

// ---------------------------------------------------------------- 1.d.iv / 1.e.i: общий налог без верхней границы
// Налог государства до 100 % и местный до +100 % дают общий налог до 200 %: государство забирает больше торговой
// ценности провинции, чистый доход штаба гильдии становится отрицательным.
TEST(app_audit_tz_states_total_tax_over_100) {
  HideTestRegs regs;
  Harness h("audit_tzstates_tax");
  h.demo();
  h.waitMap();
  Id alm = findFaction(h->world(), "Королевство Альмарин");
  CHECK(alm);
  openTab(h, alm, "faction.economy");
  CHECK(typeNumber(h, "economy.tax", "100"));
  CHECK_NEAR(h->world().faction(alm)->tax, 100, 1e-9);
  // Провинция Альмарина со штабом гильдии; местный налог +30 (поле провинции допускает до +100).
  Id pid = 0;
  h->world().provinces.each([&](const Province& p) {
    if (!pid && p.owner == alm && !p.hqs.empty() && !p.influence.empty()) pid = p.id;
  });
  CHECK(pid);
  CHECK(h->act("Местный налог", [&](Tx& tx) { tx.province(pid).localTax = 30; }));
  h.step();
  auto c = rules::calc(h->store.world());
  const rules::ProvinceCalc* pc = c->province(pid);
  CHECK(pc != nullptr);
  double guildNet = 0;
  for (const rules::GuildShare& s : pc->guilds)
    if (s.hq) guildNet += s.net;
  CHECK_MSG(pc->taxTotal <= 100 + 1e-9, "общий налог провинции " + std::to_string(pc->taxTotal) + " % — больше 100 %");
  CHECK_MSG(pc->provinceTax + pc->guildTax <= pc->tradeValue + 1e-9,
        "налог провинции " + std::to_string(pc->provinceTax + pc->guildTax) + " больше её торговой ценности " + std::to_string(pc->tradeValue));
  CHECK_MSG(guildNet >= -1e-9, "чистый доход штабов гильдий в провинции отрицательный: " + std::to_string(guildNet));
  openTab(h, alm, "faction.economy");
  shotClean(h, "audit_tzstates_tax_over_100");
}

// ---------------------------------------------------------------- 1.b.iii: «Действующие эффекты» вкладки «Модификаторы»
// У Альмарина изучена «Торговые гильдии» (модификатор «Торговый тракт»: +6 к торговой ценности каждой провинции).
// Вкладка «Модификаторы» государства в «Действующих эффектах» показывает только свои модификаторы: локальные эффекты
// изученных технологий не видны ни в «В каждой провинции», ни в «Источниках».
TEST(app_audit_tz_states_modifiers_tab_tech_effects) {
  HideTestRegs regs;
  Harness h("audit_tzstates_mods");
  h.demo();
  h.waitMap();
  Id alm = findFaction(h->world(), "Королевство Альмарин");
  CHECK(alm);
  const World& w = h->world();
  // Локальные эффекты изученных технологий Альмарина.
  std::vector<Id> techLocal;
  w.techs.each([&](const Tech& t) {
    if (t.faction != alm || !t.studied) return;
    for (Id mid : t.modifiers)
      if (const Modifier* m = w.modifier(mid))
        for (int k = 0; k < kFxCount; k++)
          if (schema::kEffects[k].local && m->has(Fx(k)) && m->fx[size_t(k)] != 0 && std::find(techLocal.begin(), techLocal.end(), t.id) == techLocal.end()) techLocal.push_back(t.id);
  });
  CHECK(!techLocal.empty());
  openTab(h, alm, "faction.modifiers");
  shotClean(h, "audit_tzstates_modifiers_tab");
  // Регрессия: каждая изученная технология с локальными эффектами есть в «Источниках» вкладки, её эффекты — в сводке
  // «В каждой провинции».
  for (Id tid : techLocal)
    CHECK_MSG(h->uiRect("mods.source." + std::to_string(int(rules::EffectSource::Tech)) + "." + std::to_string(tid)) != nullptr,
              "вкладка «Модификаторы» не показывает локальные эффекты изученных технологий (+6 к торговой ценности в каждой провинции)");
}

// ---------------------------------------------------------------- 1.e.i: щелчок по полю казны меняет казну
// После хода казна дробная (чистый доход дробный). Поле «Запас» золота во вкладке «Экономика» показывает один знак
// после запятой; щелчок в поле и уход фокуса (Tab) без ввода записывают округлённое значение в мир — появляется шаг
// отмены «Казна», мир помечается несохранённым, точное значение теряется.
TEST(app_audit_tz_states_treasury_click_rounds) {
  HideTestRegs regs;
  Harness h("audit_tzstates_treasury");
  h.demo();
  h.waitMap();
  Id alm = findFaction(h->world(), "Королевство Альмарин");
  CHECK(alm);
  CHECK(h->endTurnNow());
  h.settle();
  h.dropToasts();
  const double t0 = h->world().faction(alm)->treasury();
  CHECK(std::fabs(t0 * 10 - std::round(t0 * 10)) > 1e-6);   // больше одного знака после запятой
  const std::string undo0 = h->store.canUndo() ? h->store.undoLabel() : std::string();
  openTab(h, alm, "faction.economy");
  CHECK(clickIn(h, "economy.stock.1"));
  h.key(Key::Tab);
  h.settle();
  const double t1 = h->world().faction(alm)->treasury();
  const std::string undo1 = h->store.canUndo() ? h->store.undoLabel() : std::string();
  CHECK_MSG(t1 == t0, "щелчок по полю казны и Tab без ввода изменили казну: " + fmtNum(t0, 6) + " → " + fmtNum(t1, 6) + " (шаг отмены «" + undo1 + "»)");
  CHECK_MSG(undo1 == undo0, "после щелчка по полю казны появился шаг отмены «" + undo1 + "»");
}

// ---------------------------------------------------------------- снимки: экономика с обменом в обе стороны
// Ольсты получают золото по сделке и репарации; после новой дани в пользу Мирели у них в столбце «Торговля» золота
// и приход, и расход: проверка, что значения помещаются в узкий столбец таблицы ресурсов.
TEST(app_audit_tz_states_economy_shots) {
  HideTestRegs regs;
  Harness h("audit_tzstates_shots");
  h.demo();
  h.waitMap();
  Id ols = findFaction(h->world(), "Вольные города Ольсты");
  Id mir = findFaction(h->world(), "Княжество Мирель");
  CHECK(ols && mir);
  CHECK(h->act("Дань", [&](Tx& tx) { rules::imposeTribute(tx, DealKind::Tribute, mir, ols, 1250, 4); }));
  openTab(h, ols, "faction.economy");
  shotClean(h, "audit_tzstates_economy_top");
  CHECK(ensureVisible(h, "economy.stock.1"));
  shotClean(h, "audit_tzstates_economy_resources");
  openTab(h, ols, "faction.trade");
  shotClean(h, "audit_tzstates_trade_tab");
  openTab(h, ols, "faction.diplomacy");
  shotClean(h, "audit_tzstates_diplomacy");
  openTab(h, ols, "faction.overview");
  shotClean(h, "audit_tzstates_overview");
  openTab(h, ols, "faction.tech");
  shotClean(h, "audit_tzstates_tech_tab");
}
