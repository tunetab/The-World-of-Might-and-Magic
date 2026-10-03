// Тесты rules: наглядные артефакты (PNG в каталоге тестов) — битва с отступлением, роспуск союза,
// поиск свободного места, раскладка дерева технологий.
#include "tests/test_geo_util.h"
#include "tests/test_rules_util.h"

using namespace rg;
using namespace rg::rules;
using namespace rg::rulestest;

namespace {

std::vector<Vec2> circle(Vec2 c, double r, int n = 40) {
  std::vector<Vec2> p;
  for (int i = 0; i < n; i++) p.push_back({c.x + r * std::cos(2 * kPi * i / n), c.y + r * std::sin(2 * kPi * i / n)});
  return p;
}

// Карта: провинции цветом владельца, войска — круги цвета фракции (флот — кольцо), метки — точки.
void drawMap(geotest::Raster& r, const World& w, const std::vector<std::pair<Vec2, Color>>& marks = {}) {
  auto fs = geo::buildFaces(w);
  for (auto& f : fs->faces) {
    Color c = f.terrain == Terrain::Sea ? Color::hex(0xbcd4e6) : Color::hex(0xf2efe6);
    if (const Province* p = w.province(f.province); p && p->owner)
      if (const Faction* o = w.faction(p->owner)) c = Color::mix(c, o->color, 0.35f);
    r.fill(f.rings, c);
  }
  w.edges.each([&](const Edge& e) {
    auto c = geo::edgeCoords(w, e);
    Color col = e.kind == EdgeKind::Coast ? Color::hex(0x1d4f91) : Color::hex(0x555555);
    for (size_t i = 0; i + 1 < c.size(); i++) r.line(c[i], c[i + 1], col);
  });
  w.armies.each([&](const Army& a) {
    for (size_t gi = 0; gi < a.groups.size(); gi++) {
      const Faction* f = w.faction(a.groups[gi].faction);
      Color c = f ? f->color : Color::hex(0x000000);
      double rr = schema::kObjectRadius * (1.0 - 0.3 * double(gi));
      if (a.isFleet()) {
        auto ring = circle(a.pos, rr);
        for (size_t i = 0; i < ring.size(); i++) r.line(ring[i], ring[(i + 1) % ring.size()], c);
      } else {
        r.fill({circle(a.pos, rr)}, c);
      }
    }
    r.dot(a.pos, 1, Color::hex(0x000000));
  });
  for (auto& [p, c] : marks) r.dot(p, 3, c);
}

}  // namespace

TEST(rules_visual_battle_and_spots) {
  Fix f;
  Id rA = 0, rB = 0, rC = 0, aA = 0, bB = 0, al = 0;
  f.tx([&](Tx& tx) {
    tx.faction(f.A).color = Color::hex(0xc0392b);
    tx.faction(f.B).color = Color::hex(0x2e86c1);
    tx.faction(f.C).color = Color::hex(0x27ae60);
    for (int i : {0, 4}) tx.province(f.p[i]).owner = f.A;
    for (int i : {1, 2, 5}) tx.province(f.p[i]).owner = f.B;
    for (int i : {3, 6, 7}) tx.province(f.p[i]).owner = f.C;
    rA = addArmyRow(tx, f.A, UnitType::LightInf, "", 100, 1);
    rB = addArmyRow(tx, f.B, UnitType::HeavyInf, "", 100, 1);
    rC = addArmyRow(tx, f.C, UnitType::Beasts, "", 100, 1);
    aA = createArmy(tx, ArmyKind::Army, f.A, {220, 210});
    bB = createArmy(tx, ArmyKind::Army, f.B, {420, 230});
    setUnits(tx, aA, f.A, rA, 60);
    setUnits(tx, bB, f.B, rB, 60);
    // Тесное окружение места боя: свободное место ищется по спирали.
    createArmy(tx, ArmyKind::Army, f.B, {420 + 2 * schema::kObjectRadius, 230});
    createArmy(tx, ArmyKind::Army, f.B, {420, 230 + 2 * schema::kObjectRadius});
    createArmy(tx, ArmyKind::Fleet, f.B, {420, 50});
    declareWar(tx, f.A, f.B);
    setRelation(tx, f.A, f.C, 60, RelStatus::Alliance);
    al = createArmy(tx, ArmyKind::Army, f.A, {700, 420});
    Id ac = createArmy(tx, ArmyKind::Army, f.C, {800, 420});
    setUnits(tx, al, f.A, rA, 20);
    setUnits(tx, ac, f.C, rC, 30);
    formAllied(tx, al, ac);
  });
  World before = f.w();
  BattleResult r;
  r.attacker = aA;
  r.defender = bB;
  r.attackerWins = true;
  r.attackerOrigin = {220, 210};
  r.losses[bB][{f.B, rB}] = 25;
  f.tx([&](Tx& tx) {
    resolveBattle(tx, r);
    dissolveAllied(tx, al);
  });
  const World& w = f.w();
  w.armies.each([&](const Army& a) {
    std::string why;
    CHECK_MSG(validPosition(w, a.kind, a.pos, a.id, &why), why);
  });
  CHECK(w.army(aA)->pos == Vec2(420, 230));
  auto spot = findFreeSpot(w, ArmyKind::Army, {420, 230});
  CHECK(spot.has_value());

  geotest::Raster img(2 * int(W) + 20, int(H), 1.0);
  {
    geotest::Raster a(int(W), int(H), 1.0);
    drawMap(a, before, {{{220, 210}, Color::hex(0x000000)}});
    geotest::Raster b(int(W), int(H), 1.0);
    drawMap(b, w, {{*spot, Color::hex(0xff00ff)}});
    for (int y = 0; y < int(H); y++)
      for (int x = 0; x < int(W); x++) {
        std::memcpy(&img.rgba[(size_t(y) * size_t(img.w) + size_t(x)) * 4], &a.rgba[(size_t(y) * size_t(a.w) + size_t(x)) * 4], 4);
        std::memcpy(&img.rgba[(size_t(y) * size_t(img.w) + size_t(x) + size_t(W) + 20) * 4], &b.rgba[(size_t(y) * size_t(b.w) + size_t(x)) * 4], 4);
      }
  }
  CHECK(img.save("rules_battle.png"));
}

TEST(rules_visual_tech_layout) {
  Fix f;
  std::vector<Id> t;
  f.tx([&](Tx& tx) {
    // Дерево с длинными связями и перекрёстными зависимостями.
    const char* names[] = {"Огонь", "Камень", "Бронза", "Письмо", "Колесо", "Железо", "Право", "Математика", "Сталь", "Механика", "Астрономия", "Порох"};
    for (const char* n : names) t.push_back(createTech(tx, f.A, n));
    auto dep = [&](int a, int b) { setPrereq(tx, t[size_t(a)], t[size_t(b)], true); };
    dep(2, 0); dep(2, 1); dep(4, 1); dep(5, 2); dep(6, 3); dep(7, 3); dep(7, 4);
    dep(8, 5); dep(9, 7); dep(9, 4); dep(10, 7); dep(11, 8); dep(11, 0); dep(11, 10); dep(6, 1);
    autoLayout(tx, f.A);
  });
  const World& w = f.w();
  // Узлы в одном столбце не совпадают; связи идут слева направо.
  for (Id a : t)
    for (Id b : t)
      if (a != b) CHECK(!(w.tech(a)->pos == w.tech(b)->pos));
  for (Id a : t)
    for (Id p : w.tech(a)->prereqs) CHECK(w.tech(p)->pos.x < w.tech(a)->pos.x);
  double maxX = 0, maxY = 0;
  for (Id a : t) {
    maxX = std::max(maxX, w.tech(a)->pos.x);
    maxY = std::max(maxY, w.tech(a)->pos.y);
  }
  const double s = 0.5, bw = 180, bh = 60;
  geotest::Raster img(int((maxX + bw + 80) * s), int((maxY + bh + 80) * s), s, {-40, -40});
  for (Id a : t)
    for (Id p : w.tech(a)->prereqs) {
      Vec2 from = w.tech(p)->pos + Vec2{bw, bh / 2}, to = w.tech(a)->pos + Vec2{0, bh / 2};
      img.line(from, to, Color::hex(0x333333));
    }
  for (Id a : t) {
    Vec2 q = w.tech(a)->pos;
    img.fill({rect(q.x, q.y, q.x + bw, q.y + bh)}, Color::palette(int(a)));
  }
  CHECK(img.save("rules_tech_layout.png"));
}
