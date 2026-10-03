// Regnum — правка областей провинций инструментами карты (ТЗ 1.a.i): операции geo и уборка провинций,
// у которых после правки не осталось области на карте (иначе запись «призрака» продолжала бы приносить доход).
#include <unordered_set>

#include "geo/ops.h"
#include "rules/internal.h"

namespace rg::rules {

using namespace detail;

namespace {

// Провинции, у которых есть область: хотя бы одна дуга графа с этой провинцией по одну из сторон.
std::unordered_set<Id> withArea(const World& w) {
  std::unordered_set<Id> r;
  w.edges.each([&](const Edge& e) {
    if (e.pl) r.insert(e.pl);
    if (e.pr) r.insert(e.pr);
  });
  return r;
}

// Удалить записи провинций, у которых до правки была область, а после — нет. Возвращает их названия.
std::vector<std::string> dropEmptied(Tx& tx, const std::unordered_set<Id>& before) {
  const std::unordered_set<Id> now = withArea(tx.w());
  std::vector<Id> gone;
  for (Id id : before)
    if (!now.count(id) && tx.w().province(id)) gone.push_back(id);
  std::sort(gone.begin(), gone.end());
  std::vector<std::string> names;
  for (Id id : gone) {
    const std::string& n = tx.w().province(id)->name;
    names.push_back(n.empty() ? std::string("без названия") : n);
    dropProvinceRecord(tx, id, "на карте не осталось её области");
  }
  return names;
}

}  // namespace

AreaEdit createProvince(Tx& tx, const std::vector<Vec2>& poly, Terrain terrain, double snap) {
  const auto before = withArea(tx.w());
  AreaEdit r;
  r.province = geo::createProvince(tx, poly, terrain, geo::EditOptions{snap});
  r.removed = dropEmptied(tx, before);
  return r;
}

AreaEdit addArea(Tx& tx, Id province, const std::vector<Vec2>& poly, double snap) {
  needProvince(tx.w(), province);
  const auto before = withArea(tx.w());
  geo::addArea(tx, province, poly, geo::EditOptions{snap});
  AreaEdit r;
  r.removed = dropEmptied(tx, before);
  r.province = tx.w().province(province) ? province : 0;
  return r;
}

AreaEdit removeArea(Tx& tx, Id province, const std::vector<Vec2>& poly, double snap) {
  needProvince(tx.w(), province);
  const auto before = withArea(tx.w());
  geo::removeArea(tx, province, poly, geo::EditOptions{snap});
  AreaEdit r;
  r.removed = dropEmptied(tx, before);
  r.province = tx.w().province(province) ? province : 0;
  return r;
}

AreaEdit fillAt(Tx& tx, Vec2 p, Id province) {
  if (province) needProvince(tx.w(), province);
  const auto before = withArea(tx.w());
  AreaEdit r;
  r.province = geo::fillAt(tx, p, province);
  r.removed = dropEmptied(tx, before);
  return r;
}

}  // namespace rg::rules
