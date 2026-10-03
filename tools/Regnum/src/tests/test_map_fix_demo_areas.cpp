// Демонстрационный мир (вымышленный пример, не канон): правка областей правилами (rules::removeArea/createProvince)
// на настоящей береговой линии — провинция, у которой не осталось области, удаляется вместе с записью.
#include "geo/topo.h"
#include "rules/rules.h"
#include "tests/test_map_view_util.h"

using namespace rg;

namespace {

// Небольшая сухопутная провинция с владельцем, рамка которой (с запасом) целиком на суше.
Id smallInland(const World& w, Box2& box) {
  auto fs = geo::faces(w);
  Id victim = 0;
  double best = 1e18;
  w.provinces.each([&](const Province& p) {
    if (p.sea || !p.owner) return;
    const geo::ProvinceShape* s = fs->shape(p.id);
    if (!s) return;
    Box2 b = s->box.inflated(25);
    Vec2 cs[] = {{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}};
    for (Vec2 c : cs)
      if (fs->terrainAt(c) != Terrain::Land) return;
    if (s->area < best) {
      best = s->area;
      victim = p.id;
      box = b;
    }
  });
  return victim;
}

std::vector<Vec2> boxPoly(const Box2& b) { return {{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}}; }

}  // namespace

TEST(map_fix_demo_cut_whole_province_removes_record) {
  Store s;
  s.replace(mvtest::demo(), "демо");
  Box2 box;
  const Id victim = smallInland(s.world(), box);
  CHECK(victim != 0);
  const Id owner = s.world().province(victim)->owner;
  const std::string name = s.world().province(victim)->name;
  rules::AreaEdit r;
  s.transact("Вырезать", [&](Tx& tx) { r = rules::removeArea(tx, victim, boxPoly(box), 4.0); });
  CHECK(!s.world().province(victim));
  CHECK(!geo::faces(s.world())->shape(victim));
  CHECK_EQ(r.province, Id(0));
  CHECK(r.removed == std::vector<std::string>{name});
  auto c = rules::calc(s.world());
  const auto& prov = c->faction(owner)->provinces;
  CHECK(std::find(prov.begin(), prov.end(), victim) == prov.end());
  CHECK(geo::validate(s.world()).empty());
}

TEST(map_fix_demo_cover_whole_province_removes_record) {
  Store s;
  s.replace(mvtest::demo(), "демо");
  Box2 box;
  const Id victim = smallInland(s.world(), box);
  CHECK(victim != 0);
  rules::AreaEdit r;
  s.transact("Новая провинция", [&](Tx& tx) { r = rules::createProvince(tx, boxPoly(box), Terrain::None, 4.0); });
  CHECK(r.province != 0 && s.world().province(r.province));
  CHECK(!s.world().province(victim));
  CHECK_EQ(r.removed.size(), size_t(1));
  CHECK(geo::validate(s.world()).empty());
}
