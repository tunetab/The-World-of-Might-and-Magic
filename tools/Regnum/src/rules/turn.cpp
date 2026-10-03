// Regnum — завершение хода (RULES.md §4). Детерминированно: обход по возрастанию ID, случайность — Rng с зерном.
#include "rules/internal.h"

namespace rg::rules {

using namespace detail;

namespace {

constexpr i64 kMaxCount = 1000000000000000LL;

// Зерно броска восстания: «ход + провинция».
u64 rebellionSeed(int turn, Id province) { return hashMix(hashMix(0x5245424C4C494F4Eull, u64(u32(turn))), u64(province)); }

std::string dealDoneText(const World& w, const Deal& d) {
  switch (d.kind) {
    case DealKind::Tribute: return "Завершена выплата дани: " + facName(w, d.b) + " → " + facName(w, d.a);
    case DealKind::Reparations: return "Завершена выплата репараций: " + facName(w, d.b) + " → " + facName(w, d.a);
    case DealKind::Trade: break;
  }
  return "Выполнена сделка " + facName(w, d.a) + " и " + facName(w, d.b);
}

}  // namespace

TurnReport endTurn(Tx& tx) {
  TurnReport rep;
  const int turn = tx.w().turn();
  rep.turnFrom = turn;
  rep.turnTo = turn + 1;
  auto log = [&](LogKind kind, const std::string& text, LogRefs refs) { rep.logIds.push_back(addLog(tx, kind, text, refs)); };

  // Расчёт по состоянию на начало процедуры.
  const std::shared_ptr<const Calc> c = calc(tx);
  const std::vector<Id> factionIds = tx.w().factions.ids();
  std::vector<std::map<Id, double>> before;
  before.reserve(factionIds.size());
  for (Id id : factionIds) before.push_back(tx.w().faction(id)->res);

  // 2. Казна — чистый доход; ресурсы — добыча провинций.
  for (Id id : factionIds) {
    const FactionCalc* fc = c->faction(id);
    if (!fc) continue;
    bool prod = std::any_of(fc->resources.begin(), fc->resources.end(), [](auto& kv) { return kv.first != kGold && kv.second.production > 0; });
    if (fc->net == 0 && !prod) continue;
    double was = tx.w().faction(id)->treasury();
    Faction& f = tx.faction(id);
    addStock(f, kGold, fc->net);
    for (auto& [r, flow] : fc->resources)
      if (r != kGold && flow.production > 0) addStock(f, r, flow.production);
    double now = f.treasury();
    if (now < 0)
      log(LogKind::Economy,
          was >= 0 ? "Казна " + facName(tx.w(), id) + " ушла в долг: " + fmtNum(now, 2) : "Долг казны " + facName(tx.w(), id) + ": " + fmtNum(now, 2),
          LogRefs{0, 0, {id}});
  }

  // 3. Сделки «каждый ход»: ресурсы кроме золота; оставшиеся ходы; завершение.
  for (Id did : idsWhere(tx.w().deals, [](const Deal& d) { return d.status == DealStatus::Active; })) {
    Deal d = *tx.w().deal(did);
    bool anyLeft = false;
    for (DealItem& it : d.items) {
      if (it.mode != DealMode::PerTurn || it.left <= 0) continue;
      if (it.res != kGold && it.amount > 0) {
        Id payer = it.from == DealSide::A ? d.a : d.b, payee = it.from == DealSide::A ? d.b : d.a;
        const Faction* fp = tx.w().faction(payer);
        const Faction* fr = tx.w().faction(payee);
        if (fp && fr) {
          double give = std::min(it.amount, std::max(0.0, fp->stock(it.res)));
          if (give > 0) {
            addStock(tx.faction(payer), it.res, -give);
            addStock(tx.faction(payee), it.res, give);
          }
          if (give + 1e-9 < it.amount)
            log(LogKind::Trade,
                "Недостача по сделке: " + facName(tx.w(), payer) + " → " + facName(tx.w(), payee) + ", " + resName(tx.w(), it.res) + " " +
                    amount(give) + " из " + amount(it.amount),
                LogRefs{0, 0, {payer, payee}});
        }
      }
      it.left--;
      if (it.left > 0) anyLeft = true;
    }
    if (!anyLeft) {
      d.status = DealStatus::Done;
      log(d.kind == DealKind::Trade ? LogKind::Trade : LogKind::Diplomacy, dealDoneText(tx.w(), d), LogRefs{0, 0, {d.a, d.b}});
    }
    tx.deal(did) = std::move(d);
  }

  // 4. Строительство. В морской провинции стройка приостановлена (море не участвует в расчётах).
  int built = 0;
  for (Id pid : idsWhere(tx.w().provinces, [](const Province& p) {
         return !p.sea && std::any_of(p.buildings.begin(), p.buildings.end(), [](const ProvBuilding& b) { return b.constructing; });
       })) {
    std::vector<std::pair<Id, int>> done;
    Province& p = tx.province(pid);
    for (ProvBuilding& pb : p.buildings) {
      if (!pb.constructing) continue;
      if (--pb.left <= 0) {
        pb.left = 0;
        pb.constructing = false;
        pb.paid.clear();  // уплаченное нужно только для возврата при отмене
        pb.payer = 0;
        done.push_back({pb.building, pb.level});
      }
    }
    Id owner = p.owner;
    for (auto& [b, lvl] : done) {
      built++;
      log(LogKind::Build,
          "Достроено: " + buildingName(tx.w(), b) + (lvl > 1 ? " (уровень " + std::to_string(lvl) + ")" : std::string()) + " в провинции " +
              provName(tx.w(), pid),
          LogRefs{pid, 0, owner ? std::vector<Id>{owner} : std::vector<Id>{}});
    }
  }

  // 5. Исследования.
  int studied = 0;
  for (Id tid : idsWhere(tx.w().techs, [](const Tech& t) { return t.research && !t.studied; })) {
    const Tech& t0 = *tx.w().tech(tid);
    Id faction = t0.faction;
    if (!canResearch(tx.w(), tid).missing.empty()) {
      tx.tech(tid).research = false;
      log(LogKind::Tech, facName(tx.w(), faction) + ": исследование " + techName(tx.w(), tid) + " остановлено — не изучены предшествующие технологии",
          LogRefs{0, 0, {faction}});
      continue;
    }
    Tech& t = tx.tech(tid);
    t.progress = std::max(0, t.progress) + 1;
    if (t.progress >= t.turns) {
      t.progress = std::max(1, t.turns);
      t.studied = true;
      t.research = false;
      studied++;
      log(LogKind::Tech, facName(tx.w(), faction) + ": изучена технология " + techName(tx.w(), tid), LogRefs{0, 0, {faction}});
    }
  }

  // 6–7. Население и довольство (эффекты — на начало хода).
  for (Id pid : tx.w().provinces.ids()) {
    const ProvinceCalc* pc = c->province(pid);
    if (!pc || pc->sea) continue;
    const Province& p = *tx.w().province(pid);
    const double growth = pc->fx[Fx::PopGrowthPct];
    const double k = 1.0 + growth / 100.0;
    std::vector<RacePop> races = p.races;
    bool ch = false;
    for (RacePop& r : races) {
      const i64 was = std::max<i64>(0, r.pop);
      double v = std::round(double(was) * k);
      i64 n = v <= 0 ? 0 : (v >= double(kMaxCount) ? kMaxCount : i64(v));
      // Небольшая группа при ненулевом приросте меняется хотя бы на 1 за ход (иначе округление её «замораживает»).
      if (n == was && was > 0) {
        if (growth > 0 && was < kMaxCount) n = was + 1;
        else if (growth < 0) n = was - 1;
      }
      if (n != r.pop) {
        r.pop = n;
        ch = true;
      }
    }
    double cont = clamp(p.contentment + pc->fx[Fx::ContentmentPerTurn], -100.0, 100.0);
    if (!ch && cont == p.contentment) continue;
    Province& m = tx.province(pid);
    m.races = std::move(races);
    m.contentment = cont;
  }

  // 8. Дипломатия: модификаторы отношений с целями.
  for (Id id : factionIds) {
    const FactionCalc* fc = c->faction(id);
    if (!fc) continue;
    for (auto& [target, delta] : fc->fx.diplomacy) {
      if (delta == 0 || target == id || !tx.w().faction(target)) continue;
      Relation r = tx.w().relation(id, target);
      r.v = clamp(r.v + delta, -100.0, 100.0);
      tx.setRelation(id, target, r);
    }
  }

  // 9. Восстания (по настройке): бросок с зерном «ход + провинция».
  if (tx.w().settings->rebellionRoll) {
    for (Id pid : tx.w().provinces.ids()) {
      const ProvinceCalc* pc = c->province(pid);
      const Province& p = *tx.w().province(pid);
      if (!pc || pc->sea || !p.owner || !(pc->rebellion > 0)) continue;
      Rng rng(rebellionSeed(turn, pid));
      double roll = rng.uniform() * 100.0;
      if (roll < pc->rebellion) {
        rep.rebellions.push_back(pid);
        log(LogKind::Province, "Восстание в провинции " + provName(tx.w(), pid) + " (вероятность " + fmtPct(pc->rebellion, 1) + ")",
            LogRefs{pid, 0, {p.owner}});
      }
    }
  }

  // Итог по фракциям.
  for (size_t i = 0; i < factionIds.size(); i++) {
    Id id = factionIds[i];
    const Faction* f = tx.w().faction(id);
    const FactionCalc* fc = c->faction(id);
    if (!f || !fc) continue;
    TurnFactionLine line;
    line.faction = id;
    auto bt = before[i].find(kGold);
    line.treasuryBefore = bt == before[i].end() ? 0 : bt->second;
    line.treasuryAfter = f->treasury();
    line.income = fc->incTotal;
    line.expenses = fc->expTotal;
    std::map<Id, double> keys = before[i];
    for (auto& [r, v] : f->res) keys[r];
    for (auto& [r, v] : keys) {
      if (r == kGold) continue;
      auto b = before[i].find(r);
      double d = f->stock(r) - (b == before[i].end() ? 0.0 : b->second);
      if (std::fabs(d) > 1e-9) line.resources[r] = d;
    }
    rep.factions.push_back(std::move(line));
  }

  // 10. Номер хода; итог — в хронику.
  log(LogKind::Turn,
      "Завершён ход " + std::to_string(turn) + ". Построек достроено: " + std::to_string(built) + ", технологий изучено: " + std::to_string(studied) +
          (tx.w().settings->rebellionRoll ? ", восстаний: " + std::to_string(rep.rebellions.size()) : std::string()),
      LogRefs{});
  tx.meta().turn = turn + 1;
  return rep;
}

TurnReport previewTurn(const World& w) {
  Tx tx(w);  // черновик отбрасывается: исходный мир неизменяем
  return endTurn(tx);
}

}  // namespace rg::rules
