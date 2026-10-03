// Regnum — войска и флот: объекты на карте, отряды и резерв, гарнизоны, встречи, союзные войска, битвы.
#include "rules/internal.h"

namespace rg::rules {

using namespace detail;

namespace {

constexpr double R = schema::kObjectRadius;
constexpr double kMinGap = 2 * R;          // фигурки не перекрываются
constexpr double kSearchStep = R * 0.5;    // шаг спирали поиска свободного места
constexpr double kSearchRadius = 1600;     // дальше не ищем

const char* objNoun(ArmyKind k) { return k == ArmyKind::Fleet ? "Флот" : "Войско"; }

i64 getOr0(const std::map<Id, i64>& m, Id k) {
  auto it = m.find(k);
  return it == m.end() ? 0 : it->second;
}

// Название нового объекта: «Войско №N» — следующий свободный номер среди объектов фракции этого вида.
std::string defaultArmyName(const World& w, ArmyKind kind, Id faction) {
  std::vector<std::string> taken;
  w.armies.each([&](const Army& a) {
    if (a.kind == kind && a.leader() == faction) taken.push_back(utf8::searchKey(a.name));
  });
  const std::string base = std::string(objNoun(kind)) + " №";
  for (size_t n = 1;; n++) {  // наименьший свободный номер (после расформирования номера освобождаются)
    std::string name = base + std::to_string(n);
    if (!contains(taken, utf8::searchKey(name))) return name;
  }
}

std::vector<Id> factionsOf(const Army& a) {
  std::vector<Id> r;
  for (const ArmyGroup& g : a.groups)
    if (!contains(r, g.faction)) r.push_back(g.faction);
  return r;
}

i64 unitsOf(const Army& a) {
  i64 n = 0;
  for (const ArmyGroup& g : a.groups)
    for (const ArmyUnit& u : g.units) n += u.count;
  return n;
}

ArmyGroup* groupOf(Army& a, Id faction) {
  for (ArmyGroup& g : a.groups)
    if (g.faction == faction) return &g;
  return nullptr;
}

// Слить группу в войско: численности по строкам складываются, герои добавляются.
void addGroup(Army& a, const ArmyGroup& src) {
  ArmyGroup* g = groupOf(a, src.faction);
  if (!g) {
    a.groups.push_back(src);
    return;
  }
  for (const ArmyUnit& u : src.units) {
    auto it = std::find_if(g->units.begin(), g->units.end(), [&](const ArmyUnit& x) { return x.row == u.row; });
    if (it != g->units.end()) it->count += u.count;
    else g->units.push_back(u);
  }
  for (Id h : src.heroes)
    if (!contains(g->heroes, h)) g->heroes.push_back(h);
}

// Где сейчас персонаж-герой (0 — нигде).
Id heroArmy(const World& w, Id character, Id skip = 0) {
  Id found = 0;
  w.armies.each([&](const Army& a) {
    if (found || a.id == skip) return;
    for (const ArmyGroup& g : a.groups)
      if (contains(g.heroes, character)) found = a.id;
  });
  return found;
}

// Отношения между составами двух объектов.
struct PairRel {
  bool sameSet = false;
  bool subset = false;               // все фракции первого объекта есть во втором
  std::vector<Id> shared;            // фракции, чьи отряды есть в обоих объектах
  bool war = false, allAllied = true;
  Id warA = 0, warB = 0, otherA = 0, otherB = 0;
};
PairRel analyze(const World& w, const std::vector<Id>& fm, const std::vector<Id>& ft) {
  PairRel r;
  std::vector<Id> a = fm, b = ft;
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  r.sameSet = a == b;
  for (Id x : fm)
    if (contains(ft, x)) r.shared.push_back(x);
  r.subset = !fm.empty() && r.shared.size() == fm.size();
  for (Id x : fm)
    for (Id y : ft) {
      if (x == y) continue;
      RelStatus s = w.relation(x, y).s;
      if (s == RelStatus::Alliance) continue;
      r.allAllied = false;
      if (s == RelStatus::War) {
        if (!r.war) { r.warA = x; r.warB = y; }
        r.war = true;
      } else if (!r.otherA) {
        r.otherA = x;  // первая пара «статус-кво» или «незнакомы»
        r.otherB = y;
      }
    }
  return r;
}

}  // namespace

// ================================================================ размещение
namespace detail {

Placement::Placement(const World& w, std::shared_ptr<const geo::FaceSet> fs) : w_(&w), fs_(std::move(fs)) {
  w.armies.each([&](const Army& a) { objs_.push_back({a.id, a.pos}); });
}

bool Placement::valid(ArmyKind kind, Vec2 p, Id exclude, std::string* why) const {
  auto no = [&](std::string s) {
    if (why) *why = std::move(s);
    return false;
  };
  if (!std::isfinite(p.x) || !std::isfinite(p.y)) return no("Недопустимые координаты");
  if (!fs_ || fs_->faces.empty()) return no("Карта ещё не создана");
  Terrain t = fs_->terrainAt(p);
  if (t == Terrain::None) return no("Точка вне карты");
  if (kind == ArmyKind::Army && t != Terrain::Land) return no("Войско может стоять только на суше");
  if (kind == ArmyKind::Fleet && t != Terrain::Sea) return no("Флот может стоять только на море");
  const double lim = kMinGap * kMinGap - 1e-9;
  for (auto& [id, q] : objs_)
    if (id != exclude && dist2(p, q) < lim) return no("Место занято: " + armyName(*w_, id));
  return true;
}

std::optional<Vec2> Placement::freeSpot(ArmyKind kind, Vec2 near, Id exclude) const {
  if (!fs_ || fs_->faces.empty() || !std::isfinite(near.x) || !std::isfinite(near.y)) return std::nullopt;
  if (valid(kind, near, exclude, nullptr)) return near;
  const int rings = int(kSearchRadius / kSearchStep);
  for (int k = 1; k <= rings; k++) {
    double r = k * kSearchStep;
    int n = std::max(6, int(std::ceil(2 * kPi * r / kSearchStep)));
    double phase = (k & 1) ? 0.0 : kPi / n;  // соседние кольца со сдвигом — плотнее покрытие
    for (int i = 0; i < n; i++) {
      double a = phase + 2 * kPi * i / n;
      Vec2 p{near.x + r * std::cos(a), near.y + r * std::sin(a)};
      if (!fs_->bounds.contains(p)) continue;
      if (valid(kind, p, exclude, nullptr)) return p;
    }
  }
  return std::nullopt;
}

void Placement::set(Id id, Vec2 p) {
  for (auto& o : objs_)
    if (o.first == id) {
      o.second = p;
      return;
    }
  objs_.push_back({id, p});
}

void Placement::remove(Id id) {
  objs_.erase(std::remove_if(objs_.begin(), objs_.end(), [&](auto& o) { return o.first == id; }), objs_.end());
}

Id Placement::nearest(Vec2 p, double maxDist, Id exclude) const {
  Id best = 0;
  double bd = maxDist * maxDist;
  for (auto& [id, q] : objs_) {
    if (id == exclude) continue;
    double d = dist2(p, q);
    if (d < bd || (d == bd && best && id < best)) {
      bd = d;
      best = id;
    }
  }
  return best;
}

}  // namespace detail

namespace {
Placement placementTx(Tx& tx) { return Placement(tx.w(), facesFor(tx)); }
}  // namespace

std::optional<Vec2> findFreeSpot(const World& w, ArmyKind kind, Vec2 near, Id exclude) {
  return Placement(w, geo::faces(w)).freeSpot(kind, near, exclude);
}

bool validPosition(const World& w, ArmyKind kind, Vec2 pos, Id exclude, std::string* why) {
  return Placement(w, geo::faces(w)).valid(kind, pos, exclude, why);
}

Id armyAt(const World& w, Vec2 pos, Id exclude) {
  Id best = 0;
  double bd = R * R;
  w.armies.each([&](const Army& a) {
    if (a.id == exclude) return;
    double d = dist2(pos, a.pos);
    if (d <= bd && (best == 0 || d < bd)) {
      bd = d;
      best = a.id;
    }
  });
  return best;
}

// ================================================================ объекты и отряды
Id createArmy(Tx& tx, ArmyKind kind, Id faction, Vec2 pos) {
  if (kind != ArmyKind::Army && kind != ArmyKind::Fleet) fail("Неизвестный вид объекта");
  needFaction(tx.w(), faction);
  std::string why;
  Placement pl = placementTx(tx);
  if (!pl.valid(kind, pos, 0, &why)) fail(why);
  Army a;
  a.kind = kind;
  a.name = defaultArmyName(tx.w(), kind, faction);
  a.pos = pos;
  a.groups.push_back(ArmyGroup{faction, {}, {}});
  Id id = tx.add(std::move(a)).id;
  addLog(tx, kind == ArmyKind::Fleet ? LogKind::Fleet : LogKind::Army,
         facName(tx.w(), faction) + (kind == ArmyKind::Fleet ? ": собран флот " : ": собрано войско ") + armyName(tx.w(), id),
         LogRefs{pl.provinceAt(pos), id, {faction}});
  return id;
}

void renameArmy(Tx& tx, Id army, const std::string& name) {
  needArmy(tx.w(), army);
  std::string n = trim(name);
  if (n.empty()) fail("Название не может быть пустым");
  tx.army(army).name = n;
}

void setUnits(Tx& tx, Id army, Id faction, Id row, i64 count) {
  const Army& a = needArmy(tx.w(), army);
  const Faction& f = needFaction(tx.w(), faction);
  const ArmyGroup* g0 = nullptr;
  for (const ArmyGroup& g : a.groups)
    if (g.faction == faction) g0 = &g;
  if (!g0) fail("В составе " + armyName(tx.w(), army) + " нет отрядов " + facName(tx.w(), faction));
  const bool fleet = a.isFleet();
  i64 total = 0;
  if (fleet) {
    const FleetRow* r = f.fleetRow(row);
    if (!r) fail("Такого судна нет в таблице флота " + facName(tx.w(), faction));
    total = r->total;
  } else {
    const ArmyRow* r = f.armyRow(row);
    if (!r) fail("Такого отряда нет в таблице войск " + facName(tx.w(), faction));
    total = r->total;
  }
  if (count < 0) fail("Численность не может быть отрицательной");
  i64 cur = 0;
  for (const ArmyUnit& u : g0->units)
    if (u.row == row) cur += u.count;
  if (count > cur) {
    Deployed d = deployed(tx.w(), faction);
    i64 field = fleet ? getOr0(d.fleet, row) : getOr0(d.army, row) + getOr0(d.garrison, row);
    i64 reserve = total - field;
    if (count - cur > reserve)
      fail("В резерве недостаточно: нужно " + fmtInt(count - cur) + ", в резерве " + fmtInt(std::max<i64>(0, reserve)));
  }
  if (count == cur) return;
  ArmyGroup* g = groupOf(tx.army(army), faction);
  auto it = std::find_if(g->units.begin(), g->units.end(), [&](const ArmyUnit& u) { return u.row == row; });
  if (count == 0) {
    g->units.erase(std::remove_if(g->units.begin(), g->units.end(), [&](const ArmyUnit& u) { return u.row == row; }), g->units.end());
  } else if (it != g->units.end()) {
    it->count = count;  // строка остаётся на прежнем месте списка
    g->units.erase(std::remove_if(it + 1, g->units.end(), [&](const ArmyUnit& u) { return u.row == row; }), g->units.end());
  } else {
    g->units.push_back(ArmyUnit{row, count});
  }
}

void setHero(Tx& tx, Id army, Id character, bool on) {
  const Army& a = needArmy(tx.w(), army);
  const Character& c = needCharacter(tx.w(), character);
  bool here = false;
  for (const ArmyGroup& g : a.groups) here = here || contains(g.heroes, character);
  if (!on) {
    if (!here) return;
    Army& m = tx.army(army);
    for (ArmyGroup& g : m.groups) eraseValue(g.heroes, character);
    if (m.commander == character) m.commander = 0;
    return;
  }
  if (here) return;
  if (Id other = heroArmy(tx.w(), character, army))
    fail(q(tx.w().characterName(character)) + " уже сопровождает " + armyName(tx.w(), other));
  if (!c.faction) fail(q(tx.w().characterName(character)) + " не состоит ни в одной фракции");
  Army& m = tx.army(army);
  ArmyGroup* g = groupOf(m, c.faction);
  if (!g) fail(q(tx.w().characterName(character)) + " — герой " + facName(tx.w(), c.faction) + ", а в составе нет её отрядов");
  g->heroes.push_back(character);
}

void setCommander(Tx& tx, Id army, Id character) {
  const Army& a = needArmy(tx.w(), army);
  if (character == 0) {
    if (a.commander) tx.army(army).commander = 0;
    return;
  }
  setHero(tx, army, character, true);  // главный полководец — один из героев войска
  tx.army(army).commander = character;
}

void setGarrison(Tx& tx, Id province, Id row, i64 count) {
  const Province& p = needProvince(tx.w(), province);
  if (p.sea) fail("В морской провинции не бывает гарнизона");
  if (!p.owner) fail("У провинции нет владельца — гарнизон некому назначить");
  const Faction& f = needState(tx.w(), p.owner);
  const ArmyRow* r = f.armyRow(row);
  if (!r) fail("Такого отряда нет в таблице войск " + facName(tx.w(), p.owner));
  if (count < 0) fail("Численность не может быть отрицательной");
  i64 cur = 0;
  for (const GarrisonEntry& g : p.garrison)
    if (g.row == row) cur += g.count;
  if (count > cur) {
    Deployed d = deployed(tx.w(), p.owner);
    i64 reserve = r->total - getOr0(d.army, row) - getOr0(d.garrison, row);
    if (count - cur > reserve)
      fail("В резерве недостаточно: нужно " + fmtInt(count - cur) + ", в резерве " + fmtInt(std::max<i64>(0, reserve)));
  }
  auto& gs = tx.province(province).garrison;
  auto it = std::find_if(gs.begin(), gs.end(), [&](const GarrisonEntry& g) { return g.row == row; });
  if (count == 0) {
    gs.erase(std::remove_if(gs.begin(), gs.end(), [&](const GarrisonEntry& g) { return g.row == row; }), gs.end());
  } else if (it != gs.end()) {
    it->count = count;
    gs.erase(std::remove_if(it + 1, gs.end(), [&](const GarrisonEntry& g) { return g.row == row; }), gs.end());
  } else {
    gs.push_back(GarrisonEntry{row, count});
  }
}

void disband(Tx& tx, Id army) {
  const Army& a = needArmy(tx.w(), army);
  std::string name = armyName(tx.w(), army);
  bool fleet = a.isFleet();
  std::vector<Id> fs = factionsOf(a);
  tx.eraseArmy(army);
  addLog(tx, fleet ? LogKind::Fleet : LogKind::Army,
         std::string(fleet ? "Флот " : "Войско ") + name + (fleet ? " расформирован, корабли возвращены в резерв" : " расформировано, отряды возвращены в резерв"),
         LogRefs{0, army, fs});
}

void moveArmy(Tx& tx, Id army, Vec2 pos) {
  const Army& a = needArmy(tx.w(), army);
  if (a.pos == pos) return;
  std::string why;
  if (!placementTx(tx).valid(a.kind, pos, army, &why)) fail(why);
  tx.army(army).pos = pos;
}

// ================================================================ встречи
Encounter encounter(const World& w, Id moving, Vec2 pos) {
  Encounter e;
  const Army* m = w.army(moving);
  if (!m) {
    e.type = EncounterType::Blocked;
    e.reason = "Войско не найдено";
    return e;
  }
  Placement pl(w, geo::faces(w));
  Id t = pl.nearest(pos, kMinGap, moving);
  if (!t) {
    std::string why;
    if (pl.valid(m->kind, pos, moving, &why)) return e;  // свободное перемещение
    e.type = EncounterType::Blocked;
    e.reason = why;
    return e;
  }
  const Army& o = *w.army(t);
  e.target = t;
  if (o.kind != m->kind) {
    e.type = EncounterType::Blocked;
    e.reason = m->isFleet() ? "Флот не взаимодействует с войском на суше" : "Войско не взаимодействует с флотом";
    return e;
  }
  PairRel r = analyze(w, factionsOf(*m), factionsOf(o));
  if (r.sameSet || r.subset) {
    // Все фракции перемещаемого объекта уже есть на месте: отряды складываются в свои группы (битвы с собственными
    // отрядами не бывает, новый союз не создаётся).
    e.type = EncounterType::Merge;
    e.us = m->leader();
    e.them = o.leader();
    e.reason = std::string("Объединить ") + armyName(w, moving) + " и " + armyName(w, t) + "?";
  } else if (!r.shared.empty() && !r.allAllied) {
    // Общая фракция в обоих объектах, но не все стороны в союзе: ни битвы (фракция против себя), ни союза.
    e.type = EncounterType::Blocked;
    e.reason = "В обоих объектах есть отряды " + facName(w, r.shared.front()) + " — сначала разделите войско";
  } else if (r.war) {
    e.type = EncounterType::Battle;
    e.us = r.warA;
    e.them = r.warB;
    e.reason = "Битва: " + facName(w, r.warA) + " и " + facName(w, r.warB) + " в войне";
  } else if (r.allAllied) {
    e.type = EncounterType::Alliance;
    e.us = m->leader();
    e.them = o.leader();
    e.reason = std::string(m->isFleet() ? "Создать союзный флот" : "Создать союзное войско") + " из " + armyName(w, moving) + " и " + armyName(w, t) + "?";
  } else {
    e.type = EncounterType::DeclareWar;
    e.us = r.otherA;
    e.them = r.otherB;
    e.reason = "Объявить войну " + facName(w, r.otherB) + "? Отношения станут " + fmtNum(schema::kWarRelation);
  }
  return e;
}

void mergeArmies(Tx& tx, Id target, Id source) {
  if (target == source) fail("Нельзя объединить войско с самим собой");
  const Army& t = needArmy(tx.w(), target);
  const Army s = needArmy(tx.w(), source);
  if (t.kind != s.kind) fail("Войско не объединяется с флотом");
  if (!analyze(tx.w(), factionsOf(s), factionsOf(t)).subset)
    fail("Объединить можно только объекты одной фракции (или войско с союзным, где уже есть его фракция) — для союзников создайте союзное войско");
  std::string tn = armyName(tx.w(), target), sn = armyName(tx.w(), source);
  tx.eraseArmy(source);
  Army& m = tx.army(target);
  for (const ArmyGroup& g : s.groups) addGroup(m, g);
  if (!m.commander) m.commander = s.commander;
  addLog(tx, s.isFleet() ? LogKind::Fleet : LogKind::Army, "Объединены " + tn + " и " + sn, LogRefs{0, target, factionsOf(m)});
}

void formAllied(Tx& tx, Id target, Id source) {
  if (target == source) fail("Нельзя объединить войско с самим собой");
  const Army& t = needArmy(tx.w(), target);
  const Army s = needArmy(tx.w(), source);
  if (t.kind != s.kind) fail("Войско не объединяется с флотом");
  for (Id x : factionsOf(s))
    for (Id y : factionsOf(t))
      if (x != y && tx.w().relation(x, y).s != RelStatus::Alliance)
        fail(facName(tx.w(), x) + " и " + facName(tx.w(), y) + " не в союзе");
  bool wasAllied = t.allied();
  tx.eraseArmy(source);
  Army& m = tx.army(target);
  for (const ArmyGroup& g : s.groups) addGroup(m, g);
  if (!m.commander) m.commander = s.commander;
  if (!wasAllied && m.allied()) m.name = m.isFleet() ? "Союзный флот" : "Союзное войско";
  std::vector<std::string> names;
  for (Id f : factionsOf(m)) names.push_back(facName(tx.w(), f));
  addLog(tx, m.isFleet() ? LogKind::Fleet : LogKind::Army,
         std::string(m.isFleet() ? "Союзный флот: " : "Союзное войско: ") + join(names, ", "), LogRefs{0, target, factionsOf(m)});
}

std::vector<Id> dissolveAllied(Tx& tx, Id army) {
  const Army a = needArmy(tx.w(), army);
  if (!a.allied()) fail(armyName(tx.w(), army) + (a.isFleet() ? " — не союзный флот" : " — не союзное войско"));
  Placement pl = placementTx(tx);
  std::vector<Id> out{army};
  Army& keep = tx.army(army);
  keep.groups.resize(1);
  bool cmdMoved = a.commander && !contains(a.groups[0].heroes, a.commander);
  if (cmdMoved) keep.commander = 0;
  if (keep.name == "Союзное войско" || keep.name == "Союзный флот") keep.name = defaultArmyName(tx.w(), a.kind, a.groups[0].faction);
  for (size_t i = 1; i < a.groups.size(); i++) {
    auto spot = pl.freeSpot(a.kind, a.pos, 0);
    if (!spot) fail("Рядом нет свободного места, чтобы распустить союз");
    Army n;
    n.kind = a.kind;
    n.name = defaultArmyName(tx.w(), a.kind, a.groups[i].faction);
    n.pos = *spot;
    n.groups.push_back(a.groups[i]);
    if (cmdMoved && contains(a.groups[i].heroes, a.commander)) n.commander = a.commander;
    Id nid = tx.add(std::move(n)).id;
    pl.set(nid, *spot);
    out.push_back(nid);
  }
  addLog(tx, a.isFleet() ? LogKind::Fleet : LogKind::Army,
         std::string(a.isFleet() ? "Союзный флот " : "Союзное войско ") + q(a.name.empty() ? "—" : a.name) + " распущен" + (a.isFleet() ? "" : "о"),
         LogRefs{0, army, factionsOf(a)});
  return out;
}

Id splitArmy(Tx& tx, Id army, const SplitSpec& spec) {
  const Army a = needArmy(tx.w(), army);
  std::vector<ArmyGroup> moved;  // группы нового объекта
  i64 movedUnits = 0;
  for (auto& [key, n] : spec.units) {
    auto [faction, row] = key;
    if (n == 0) continue;
    if (n < 0) fail("Численность не может быть отрицательной");
    const ArmyGroup* g = nullptr;
    for (const ArmyGroup& x : a.groups)
      if (x.faction == faction) g = &x;
    if (!g) fail("В составе " + armyName(tx.w(), army) + " нет отрядов " + facName(tx.w(), faction));
    i64 have = 0;
    for (const ArmyUnit& u : g->units)
      if (u.row == row) have += u.count;
    if (n > have) fail("Нельзя выделить больше, чем есть: в составе " + fmtInt(have) + ", выделяется " + fmtInt(n));
    auto it = std::find_if(moved.begin(), moved.end(), [&](const ArmyGroup& x) { return x.faction == faction; });
    if (it == moved.end()) {
      moved.push_back(ArmyGroup{faction, {}, {}});
      it = moved.end() - 1;
    }
    it->units.push_back(ArmyUnit{row, n});
    movedUnits += n;
  }
  for (Id h : spec.heroes) {
    const ArmyGroup* g = nullptr;
    for (const ArmyGroup& x : a.groups)
      if (contains(x.heroes, h)) g = &x;
    if (!g) fail(q(tx.w().characterName(h)) + " не сопровождает " + armyName(tx.w(), army));
    auto it = std::find_if(moved.begin(), moved.end(), [&](const ArmyGroup& x) { return x.faction == g->faction; });
    if (it == moved.end()) {
      moved.push_back(ArmyGroup{g->faction, {}, {}});
      it = moved.end() - 1;
    }
    if (!contains(it->heroes, h)) it->heroes.push_back(h);
  }
  if (movedUnits <= 0) fail("Выберите отряды для нового объекта");
  if (movedUnits >= unitsOf(a)) fail("Нельзя выделить весь состав — оставьте хотя бы один отряд");

  Placement pl = placementTx(tx);
  auto spot = pl.freeSpot(a.kind, a.pos, 0);
  if (!spot) fail("Рядом нет свободного места для нового объекта");

  // Исходный объект теряет выделенное; опустевшие группы без героев уходят.
  Army& src = tx.army(army);
  for (const ArmyGroup& mg : moved) {
    ArmyGroup* g = groupOf(src, mg.faction);
    for (const ArmyUnit& mu : mg.units) {
      i64 left = mu.count;
      for (ArmyUnit& u : g->units)
        if (u.row == mu.row && left > 0) {
          i64 take = std::min(left, u.count);
          u.count -= take;
          left -= take;
        }
      g->units.erase(std::remove_if(g->units.begin(), g->units.end(), [](const ArmyUnit& u) { return u.count <= 0; }), g->units.end());
    }
    for (Id h : mg.heroes) eraseValue(g->heroes, h);
  }
  src.groups.erase(std::remove_if(src.groups.begin(), src.groups.end(), [](const ArmyGroup& g) { return g.units.empty() && g.heroes.empty(); }),
                   src.groups.end());
  Army n;
  n.kind = a.kind;
  n.name = defaultArmyName(tx.w(), a.kind, moved[0].faction);
  n.pos = *spot;
  n.groups = moved;
  for (const ArmyGroup& g : moved)
    if (a.commander && contains(g.heroes, a.commander)) n.commander = a.commander;
  if (n.commander) src.commander = 0;
  Id nid = tx.add(std::move(n)).id;
  addLog(tx, a.isFleet() ? LogKind::Fleet : LogKind::Army,
         std::string(a.isFleet() ? "Флот " : "Войско ") + q(a.name.empty() ? "—" : a.name) +
             (a.isFleet() ? " разделён: выделен " : " разделено: выделено ") + armyName(tx.w(), nid),
         LogRefs{0, army, factionsOf(a)});
  return nid;
}

void declareWar(Tx& tx, Id a, Id b) {
  needFaction(tx.w(), a);
  needFaction(tx.w(), b);
  if (a == b) fail("Фракция не может объявить войну самой себе");
  if (tx.w().relation(a, b).s == RelStatus::War) fail(facName(tx.w(), a) + " и " + facName(tx.w(), b) + " уже в войне");
  tx.setRelation(a, b, Relation{schema::kWarRelation, RelStatus::War});
  addLog(tx, LogKind::War, facName(tx.w(), a) + " объявляет войну " + facName(tx.w(), b), LogRefs{0, 0, {a, b}});
  splitBrokenAlliances(tx, a, b);
}

namespace detail {
void splitBrokenAlliances(Tx& tx, Id a, Id b) {
  if (a == b || tx.w().relation(a, b).s == RelStatus::Alliance) return;
  for (Id aid : idsWhere(tx.w().armies, [&](const Army& x) {
         auto fs = factionsOf(x);
         return x.allied() && contains(fs, a) && contains(fs, b);
       }))
    dissolveAllied(tx, aid);
}
}  // namespace detail

// ================================================================ битва
void resolveBattle(Tx& tx, const BattleResult& r) {
  if (r.attacker == r.defender) fail("Войско не может сражаться само с собой");
  const Army A = needArmy(tx.w(), r.attacker);
  const Army D = needArmy(tx.w(), r.defender);
  if (A.kind != D.kind) fail("Войско не сражается с флотом");
  const PairRel pr = analyze(tx.w(), factionsOf(A), factionsOf(D));
  if (!pr.shared.empty()) fail("Отряды " + facName(tx.w(), pr.shared.front()) + " есть в обоих объектах — фракция не сражается сама с собой");
  if (!pr.war) fail("Стороны не находятся в состоянии войны");

  // Проверка потерь: только участники, строки состава, не больше численности.
  for (auto& [aid, rows] : r.losses) {
    if (aid != r.attacker && aid != r.defender) fail("Потери указаны для войска, не участвующего в битве");
    const Army& x = aid == r.attacker ? A : D;
    for (auto& [key, n] : rows) {
      auto [faction, row] = key;
      if (n < 0) fail("Потери не могут быть отрицательными");
      if (n == 0) continue;
      i64 have = 0;
      bool found = false;
      for (const ArmyGroup& g : x.groups)
        if (g.faction == faction)
          for (const ArmyUnit& u : g.units)
            if (u.row == row) { have += u.count; found = true; }
      if (!found) fail("Отряда с потерями нет в составе " + armyName(tx.w(), aid));
      if (n > have) fail("Потери больше численности отряда в " + armyName(tx.w(), aid) + ": " + fmtInt(n) + " из " + fmtInt(have));
    }
  }
  // Победителем может быть только сторона, у которой после потерь остались отряды (если уничтожены обе — победы нет,
  // оба объекта исчезают).
  auto leftAfter = [&](const Army& x) {
    i64 left = unitsOf(x);
    if (auto it = r.losses.find(x.id); it != r.losses.end())
      for (auto& [key, n] : it->second) left -= std::max<i64>(0, n);
    return left;
  };
  const i64 leftW = leftAfter(r.attackerWins ? A : D), leftL = leftAfter(r.attackerWins ? D : A);
  if (leftW <= 0 && leftL > 0)
    fail("Победителем не может быть " + armyName(tx.w(), r.attackerWins ? r.attacker : r.defender) +
         ": после потерь у него не осталось отрядов");

  const Vec2 battlePos = D.pos;
  const std::string an = armyName(tx.w(), r.attacker), dn = armyName(tx.w(), r.defender);
  std::vector<Id> all = factionsOf(A);
  for (Id f : factionsOf(D))
    if (!contains(all, f)) all.push_back(f);

  // Потери: из отрядов объекта и из общей численности фракции.
  i64 lossA = 0, lossD = 0;
  for (auto& [aid, rows] : r.losses) {
    for (auto& [key, n] : rows) {
      if (n <= 0) continue;
      auto [faction, row] = key;
      (aid == r.attacker ? lossA : lossD) += n;
      ArmyGroup* g = groupOf(tx.army(aid), faction);
      i64 left = n;
      for (ArmyUnit& u : g->units)
        if (u.row == row && left > 0) {
          i64 take = std::min(left, u.count);
          u.count -= take;
          left -= take;
        }
      Faction& f = tx.faction(faction);
      if (A.isFleet()) {
        for (FleetRow& fr : f.fleet)
          if (fr.id == row) fr.total = std::max<i64>(0, fr.total - n);
      } else {
        for (ArmyRow& ar : f.army)
          if (ar.id == row) ar.total = std::max<i64>(0, ar.total - n);
      }
    }
  }
  // Группы без отрядов уходят с поля (их герои остаются у фракции); объект без отрядов исчезает.
  std::vector<Id> destroyed;
  for (Id aid : {r.attacker, r.defender}) {
    const Army& cur = *tx.w().army(aid);
    bool emptyUnits = false, emptyGroup = false;
    for (const ArmyGroup& g : cur.groups) {
      emptyGroup = emptyGroup || std::none_of(g.units.begin(), g.units.end(), [](const ArmyUnit& u) { return u.count > 0; });
      for (const ArmyUnit& u : g.units) emptyUnits = emptyUnits || u.count <= 0;
    }
    if (!emptyUnits && !emptyGroup) continue;
    Army& x = tx.army(aid);
    for (ArmyGroup& g : x.groups) g.units.erase(std::remove_if(g.units.begin(), g.units.end(), [](const ArmyUnit& u) { return u.count <= 0; }), g.units.end());
    for (const ArmyGroup& g : x.groups)
      if (g.units.empty() && x.commander && contains(g.heroes, x.commander)) x.commander = 0;
    x.groups.erase(std::remove_if(x.groups.begin(), x.groups.end(), [](const ArmyGroup& g) { return g.units.empty(); }), x.groups.end());
    if (x.groups.empty()) destroyed.push_back(aid);
  }
  for (Id id : destroyed) tx.eraseArmy(id);

  // Победитель стоит на месте боя, проигравший смещается от него на свободное место.
  Placement pl = placementTx(tx);
  const Id winner = r.attackerWins ? r.attacker : r.defender;
  const Id loser = r.attackerWins ? r.defender : r.attacker;
  Vec2 origin = std::isfinite(r.attackerOrigin.x) && std::isfinite(r.attackerOrigin.y) ? r.attackerOrigin : A.pos;
  if (dist2(origin, battlePos) < 1e-12) origin = A.pos;
  Vec2 dir = (origin - battlePos).norm();  // от места боя к исходной позиции нападавшего
  if (dir.len2() == 0) dir = Vec2{-1, 0};
  const bool winnerAlive = tx.w().army(winner) != nullptr;
  if (winnerAlive && winner == r.attacker) {
    tx.army(winner).pos = battlePos;
    pl.set(winner, battlePos);
  }
  if (winnerAlive && tx.w().army(loser)) {
    // Нападавший отходит туда, откуда пришёл; обороняющийся — в противоположную сторону.
    Vec2 away = loser == r.attacker ? dir : -dir;
    Vec2 want = battlePos + away * (kMinGap + R * 0.5);
    auto spot = pl.freeSpot(A.kind, want, loser);
    if (!spot) fail("Рядом с местом боя нет свободной позиции для отступления");
    tx.army(loser).pos = *spot;
  }

  std::string text = "Битва: " + an + " против " + dn + (leftW <= 0 ? std::string(". Обе стороны уничтожены") : ". Победа: " + (r.attackerWins ? an : dn)) +
                     ". Потери: " + fmtInt(lossA) + " и " + fmtInt(lossD);
  for (Id id : destroyed) text += std::string(A.isFleet() ? ". Уничтожен флот " : ". Уничтожено войско ") + (id == r.attacker ? an : dn);
  addLog(tx, LogKind::Battle, text, LogRefs{pl.provinceAt(battlePos), r.attacker, all});
}

}  // namespace rg::rules
