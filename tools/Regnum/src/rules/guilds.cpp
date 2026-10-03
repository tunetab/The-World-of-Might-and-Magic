// Regnum — торговые гильдии: штабы, влияние, государство расположения, государственные гильдии (ТЗ 1.d).
#include "rules/internal.h"

namespace rg::rules {

using namespace detail;

// ================================================================ гильдии
void buildHq(Tx& tx, Id guild, Id province) {
  needGuild(tx.w(), guild);
  const Province& p = needProvince(tx.w(), province);
  if (p.sea) fail("В морской провинции нельзя построить штаб");
  if (contains(p.hqs, guild)) fail("У " + facName(tx.w(), guild) + " уже есть штаб в провинции " + provName(tx.w(), province));
  if (int(p.hqs.size()) >= schema::kMaxHqPerProvince)
    fail("В провинции уже " + std::to_string(schema::kMaxHqPerProvince) + " штабов гильдий — больше нельзя");
  tx.province(province).hqs.push_back(guild);
  LogRefs refs{province, 0, {guild}};
  if (p.owner) refs.factions.push_back(p.owner);
  addLog(tx, LogKind::Guild, "Гильдия " + facName(tx.w(), guild) + ": открыт штаб в провинции " + provName(tx.w(), province), refs);
}

void removeHq(Tx& tx, Id guild, Id province) {
  needGuild(tx.w(), guild);
  const Province& p = needProvince(tx.w(), province);
  if (!contains(p.hqs, guild)) fail("У " + facName(tx.w(), guild) + " нет штаба в провинции " + provName(tx.w(), province));
  eraseValue(tx.province(province).hqs, guild);
  addLog(tx, LogKind::Guild, "Гильдия " + facName(tx.w(), guild) + ": закрыт штаб в провинции " + provName(tx.w(), province),
         LogRefs{province, 0, {guild}});
}

void setInfluence(Tx& tx, Id province, Id guild, double pct) {
  needGuild(tx.w(), guild);
  const Province& p = needProvince(tx.w(), province);
  if (p.sea) fail("Морская провинция не содержит сведений о торговле");
  needFinite(pct, "Влияние");
  if (pct < 0 || pct > 100) fail("Влияние задаётся в пределах от 0 до 100 %");
  double others = 0, cur = 0;
  bool present = false;
  for (const Influence& i : p.influence) {
    if (i.guild == guild) {
      cur += i.pct;
      present = true;
    } else {
      others += i.pct;
    }
  }
  if (others + pct > 100.0 + 1e-9)
    fail("Сумма влияния гильдий превысит 100 %: свободно " + fmtNum(std::max(0.0, 100.0 - others), 2) + " %");
  if (pct <= 0 ? !present : (present && cur == pct)) return;  // без изменений
  auto& inf = tx.province(province).influence;
  auto it = std::find_if(inf.begin(), inf.end(), [&](const Influence& i) { return i.guild == guild; });
  if (pct <= 0) {
    inf.erase(std::remove_if(inf.begin(), inf.end(), [&](const Influence& i) { return i.guild == guild; }), inf.end());
  } else if (it != inf.end()) {
    it->pct = pct;
    inf.erase(std::remove_if(it + 1, inf.end(), [&](const Influence& i) { return i.guild == guild; }), inf.end());
  } else {
    inf.push_back(Influence{guild, pct});
  }
}

void setHomeState(Tx& tx, Id guild, Id state) {
  const Faction& g = needGuild(tx.w(), guild);
  if (g.stateGuild) fail("Государственная гильдия не может сменить государство расположения");
  if (state) needState(tx.w(), state);
  if (g.homeState == state) return;
  Id old = g.homeState;
  tx.faction(guild).homeState = state;
  LogRefs refs{0, 0, {guild}};
  if (old) refs.factions.push_back(old);
  if (state) refs.factions.push_back(state);
  addLog(tx, LogKind::Guild,
         state ? "Гильдия " + facName(tx.w(), guild) + " перенесла основное государство расположения в " + facName(tx.w(), state)
               : "Гильдия " + facName(tx.w(), guild) + " больше не привязана к государству",
         refs);
}

Id createStateGuild(Tx& tx, Id state, const std::string& name) {
  needState(tx.w(), state);
  std::string n = trim(name);
  if (n.empty()) {
    std::vector<std::string> taken;
    tx.w().factions.each([&](const Faction& f) { taken.push_back(f.name); });
    n = uniqueName(taken, "Гильдия " + facName(tx.w(), state));
  }
  Id id = createFaction(tx, FactionKind::Guild, n);
  Faction& g = tx.faction(id);
  g.homeState = state;
  g.stateGuild = true;
  // Государственная гильдия — в союзе со своим государством (её войска не встречают своё государство как чужое).
  tx.setRelation(state, id, Relation{100, RelStatus::Alliance});
  addLog(tx, LogKind::Guild, facName(tx.w(), state) + ": учреждена государственная гильдия " + facName(tx.w(), id), LogRefs{0, 0, {state, id}});
  return id;
}

}  // namespace rg::rules
