// Regnum — строительство: варианты с ценой и причинами, начало, отмена с возвратом, снос (ТЗ 1.f; RULES.md §9).
#include "rules/internal.h"

namespace rg::rules {

using namespace detail;

namespace detail {
std::map<Id, double> levelCost(const Building& b, int level, double factor) {
  std::map<Id, double> r;
  if (level < 1 || level > int(b.levels.size())) return r;
  for (auto& [res, v] : b.levels[size_t(level - 1)].cost) {
    double x = v * factor;
    if (std::isfinite(x) && x > 0) r[res] = x;
  }
  return r;
}

Id refundPaid(Tx& tx, const ProvBuilding& pb) {
  if (!pb.constructing || !pb.payer || !tx.w().faction(pb.payer)) return 0;
  bool any = false;
  for (auto& [res, v] : pb.paid) any = any || (std::isfinite(v) && v > 0);
  if (!any) return 0;
  Faction& f = tx.faction(pb.payer);
  for (auto& [res, v] : pb.paid)
    if (std::isfinite(v) && v > 0) addStock(f, res, v);
  return pb.payer;
}
}  // namespace detail

namespace {

const ProvBuilding* findPb(const Province& p, Id building) {
  for (const ProvBuilding& pb : p.buildings)
    if (pb.building == building) return &pb;
  return nullptr;
}

const Faction* ownerState(const World& w, const Province& p) {
  const Faction* f = w.faction(p.owner);
  return f && f->isState() ? f : nullptr;
}

// Доступна ли постройка провинции вообще: общее дерево или уникальная постройка владельца.
bool inTree(const Building& b, const Province& p) { return b.owner == 0 || b.owner == p.owner; }

BuildOption optionFor(const World& w, const Province& p, const Building& b, double factor, int slots) {
  BuildOption o;
  o.building = b.id;
  const int maxLvl = int(b.levels.size());
  const ProvBuilding* pb = findPb(p, b.id);
  if (pb && pb->constructing) {
    o.level = pb->level;
    o.upgrade = pb->level > 1;
    o.turns = pb->left;
    o.cost = levelCost(b, o.level, factor);
    o.reasons.push_back("Уже строится: осталось " + nTurns(pb->left));
    return o;
  }
  if (pb && pb->level >= maxLvl) {
    o.level = pb->level;
    o.reasons.push_back("Достигнут наибольший уровень");
    return o;
  }
  o.level = pb ? pb->level + 1 : 1;
  o.upgrade = pb != nullptr;
  o.turns = maxLvl ? std::max(1, b.levels[size_t(o.level - 1)].turns) : 1;
  o.cost = levelCost(b, o.level, factor);
  const Faction* owner = ownerState(w, p);
  if (p.sea) o.reasons.push_back("В морской провинции нельзя строить");
  else if (!owner) o.reasons.push_back("У провинции нет владельца");
  if (maxLvl == 0) o.reasons.push_back("У постройки нет уровней");
  if (!pb && int(p.buildings.size()) >= slots)
    o.reasons.push_back("Нет свободных слотов: занято " + std::to_string(p.buildings.size()) + " из " + std::to_string(slots));
  for (const BuildingReq& rq : b.requires_) {
    const Building* rb = w.building(rq.building);
    if (!rb || rq.building == b.id) continue;
    const ProvBuilding* have = findPb(p, rq.building);
    if (!have || have->builtLevel() < rq.level)
      o.reasons.push_back("Нужна постройка " + buildingName(w, rq.building) + (rq.level > 1 ? " уровня " + std::to_string(rq.level) : ""));
  }
  if (owner)
    for (auto& [res, need] : o.cost) {
      double have = owner->stock(res);
      if (have + 1e-9 < need)
        o.reasons.push_back("Недостаточно ресурса «" + resName(w, res) + "»: нужно " + amount(need) + ", есть " + amount(std::max(0.0, have)));
    }
  o.can = o.reasons.empty();
  return o;
}

struct ProvCtx {
  double factor;
  int slots;
};
ProvCtx ctxOf(const World& w, const Province& p) {
  Effects fx = provinceFx(w, SourceIndex(w), p);
  return ProvCtx{costFactorOf(fx), slotsOf(p, fx)};
}

}  // namespace

std::vector<BuildOption> buildOptions(const World& w, Id province) {
  std::vector<BuildOption> out;
  const Province* p = w.province(province);
  if (!p) return out;
  ProvCtx c = ctxOf(w, *p);
  std::vector<const Building*> list;
  w.buildings.each([&](const Building& b) { if (inTree(b, *p)) list.push_back(&b); });
  std::stable_sort(list.begin(), list.end(), [](const Building* a, const Building* b) {
    if (a->cat != b->cat) return a->cat < b->cat;
    int r = compareRu(a->name, b->name);
    if (r != 0) return r < 0;
    return a->id < b->id;
  });
  out.reserve(list.size());
  for (const Building* b : list) out.push_back(optionFor(w, *p, *b, c.factor, c.slots));
  return out;
}

void startBuilding(Tx& tx, Id province, Id building) {
  const Province& p = needProvince(tx.w(), province);
  const Building& b = needBuilding(tx.w(), building);
  if (!inTree(b, p)) fail(buildingName(tx.w(), building) + " — уникальная постройка другого государства");
  ProvCtx c = ctxOf(tx.w(), p);
  BuildOption o = optionFor(tx.w(), p, b, c.factor, c.slots);
  if (!o.can) fail(o.reasons.front());
  const Id owner = p.owner;
  Faction& f = tx.faction(owner);
  for (auto& [res, v] : o.cost) addStock(f, res, -v);
  Province& m = tx.province(province);
  if (o.upgrade) {
    for (ProvBuilding& pb : m.buildings)
      if (pb.building == building) {
        pb.level = o.level;
        pb.constructing = true;
        pb.left = o.turns;
        pb.paid = o.cost;
        pb.payer = owner;
      }
  } else {
    m.buildings.push_back(ProvBuilding{building, 1, true, o.turns, o.cost, owner});
  }
  addLog(tx, LogKind::Build,
         "Начато строительство " + buildingName(tx.w(), building) + (o.upgrade ? " (уровень " + std::to_string(o.level) + ")" : std::string()) +
             " в провинции " + provName(tx.w(), province) + ": " + nTurns(o.turns),
         LogRefs{province, 0, {owner}});
}

void cancelBuilding(Tx& tx, Id province, Id building) {
  const Province& p = needProvince(tx.w(), province);
  needBuilding(tx.w(), building);
  const ProvBuilding* found = findPb(p, building);
  if (!found) fail("Постройки " + buildingName(tx.w(), building) + " нет в провинции");
  if (!found->constructing) fail(buildingName(tx.w(), building) + " не строится");
  const ProvBuilding pb = *found;
  const int level = pb.level;
  const Id owner = p.owner;
  Province& m = tx.province(province);
  if (level > 1) {
    for (ProvBuilding& x : m.buildings)
      if (x.building == building) {
        x.level = level - 1;
        x.constructing = false;
        x.left = 0;
        x.paid.clear();
        x.payer = 0;
      }
  } else {
    m.buildings.erase(std::remove_if(m.buildings.begin(), m.buildings.end(), [&](const ProvBuilding& x) { return x.building == building; }),
                      m.buildings.end());
  }
  // Возврат — ровно уплаченное и тому, кто платил (провинция могла сменить владельца, цена — измениться).
  const Id got = refundPaid(tx, pb);
  std::string text = "Отменено строительство " + buildingName(tx.w(), building) + " в провинции " + provName(tx.w(), province);
  if (got) text += got == owner ? ", стоимость возвращена" : ", стоимость возвращена " + facName(tx.w(), got);
  LogRefs refs{province, 0, {}};
  if (owner) refs.factions.push_back(owner);
  if (got && got != owner) refs.factions.push_back(got);
  addLog(tx, LogKind::Build, text, refs);
}

void demolish(Tx& tx, Id province, Id building) {
  const Province& p = needProvince(tx.w(), province);
  needBuilding(tx.w(), building);
  const ProvBuilding* pb = findPb(p, building);
  if (!pb) fail("Постройки " + buildingName(tx.w(), building) + " нет в провинции");
  if (pb->constructing) fail(buildingName(tx.w(), building) + " строится — сначала отмените строительство");
  const Id owner = p.owner;
  auto& list = tx.province(province).buildings;
  list.erase(std::remove_if(list.begin(), list.end(), [&](const ProvBuilding& x) { return x.building == building; }), list.end());
  addLog(tx, LogKind::Build, "Снесена постройка " + buildingName(tx.w(), building) + " в провинции " + provName(tx.w(), province),
         LogRefs{province, 0, owner ? std::vector<Id>{owner} : std::vector<Id>{}});
}

}  // namespace rg::rules
