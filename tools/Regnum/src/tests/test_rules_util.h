// Regnum — помощники тестов правил: маленький мир с геометрией (остров 8 провинций + 2 морские), фракции.
//
//   y=0 ┌──────────────────────────────── море ───────────────────────────────┐
//       │ seaW │  p0 (200,200) │  p1 (400,200) │  p2 (600,200) │  p3 (800,200) │ seaE │
//       │      │  p4 (200,400) │  p5 (400,400) │  p6 (600,400) │  p7 (800,400) │      │
//   y=600└────────────────────────────────────────────────────────────────────────┘
// Суша — прямоугольник [100,900] × [100,500]; провинции суши 200 × 200; seaW/seaE — морские полосы.
#pragma once
#include <string>

#include "geo/ops.h"
#include "geo/topo.h"
#include "rules/rules.h"
#include "tests/test.h"

namespace rg::rulestest {

constexpr double W = 1000, H = 600;

inline std::vector<Vec2> rect(double x0, double y0, double x1, double y1) { return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}; }
inline Vec2 center(int i) { return {200.0 + 200.0 * (i % 4), 200.0 + 200.0 * (i / 4)}; }
inline const Vec2 kSeaNorth{500, 50};   // неназначенное море над островом
inline const Vec2 kSeaWest{35, 300};    // морская провинция seaW

struct Base {
  World w;
  Id p[8]{};
  Id seaW = 0, seaE = 0;
  Id A = 0, B = 0, C = 0, G = 0, H = 0;  // государства A, B, C; гильдии G, H
};

// Мир собирается один раз (геометрия дорогая), тесты получают его копию (копия World — O(1)).
inline const Base& base() {
  static const Base b = [] {
    Base r;
    Store s;
    s.transact("init", [&](Tx& tx) {
      geo::Coast c;
      c.width = W;
      c.height = H;
      c.landRings.push_back(rect(100, 100, 900, 500));
      geo::initFromCoast(tx, c);
      for (int i = 0; i < 8; i++) {
        int col = i % 4, row = i / 4;
        double x0 = 100 + 200 * col, x1 = x0 + 200, y0 = 100 + 200 * row, y1 = y0 + 200;
        if (col == 0) x0 = 50;
        if (col == 3) x1 = 950;
        if (row == 0) y0 = 50;
        if (row == 1) y1 = 550;
        r.p[i] = geo::createProvince(tx, rect(x0, y0, x1, y1), Terrain::Land);
        tx.province(r.p[i]).name = "П" + std::to_string(i);
      }
      r.seaW = geo::createProvince(tx, rect(10, 10, 60, 590), Terrain::Sea);
      tx.province(r.seaW).name = "Западное море";
      r.seaE = geo::createProvince(tx, rect(940, 10, 990, 590), Terrain::Sea);
      tx.province(r.seaE).name = "Восточное море";
      r.A = rules::createFaction(tx, FactionKind::State, "Арден");
      r.B = rules::createFaction(tx, FactionKind::State, "Бельмар");
      r.C = rules::createFaction(tx, FactionKind::State, "Церис");
      r.G = rules::createFaction(tx, FactionKind::Guild, "Гильдия весов");
      r.H = rules::createFaction(tx, FactionKind::Guild, "Дом Хорна");
    });
    r.w = s.world();
    return r;
  }();
  return b;
}

// Хранилище с копией базового мира.
struct Fix {
  Store s;
  Id p[8]{};
  Id seaW = 0, seaE = 0, A = 0, B = 0, C = 0, G = 0, H = 0;
  mutable std::shared_ptr<const rules::Calc> cache;

  Fix() {
    const Base& b = base();
    s.replace(b.w, "test");
    for (int i = 0; i < 8; i++) p[i] = b.p[i];
    seaW = b.seaW;
    seaE = b.seaE;
    A = b.A;
    B = b.B;
    C = b.C;
    G = b.G;
    H = b.H;
  }
  const World& w() const { return s.world(); }
  template <class F> decltype(auto) tx(F&& f) { return s.transact("test", std::forward<F>(f)); }
  const rules::ProvinceCalc& pc(Id id) const {
    cache = rules::calc(w());
    return *cache->province(id);
  }
  const rules::FactionCalc& fc(Id id) const {
    cache = rules::calc(w());
    return *cache->faction(id);
  }
};

// Текст UserError, брошенной f (пусто — исключения не было).
template <class F> std::string errorOf(F&& f) {
  try {
    f();
  } catch (const UserError& e) {
    return e.what();
  }
  return {};
}

inline bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

// Модификатор с заданными эффектами (и целями дипломатии).
inline Id makeMod(Tx& tx, std::initializer_list<std::pair<rg::Fx, double>> fx, std::vector<Id> targets = {}) {
  Id id = rules::createModifier(tx, "Мод");
  Modifier& m = tx.modifier(id);
  for (auto& [f, v] : fx) {
    m.fx[size_t(f)] = v;
    m.fxMask |= 1u << unsigned(f);
  }
  m.targets = std::move(targets);
  return id;
}

// Строка хроники, содержащая подстроку (последняя по ID), или пусто.
inline std::string lastLog(const World& w, const std::string& sub = {}) {
  std::string r;
  w.log.each([&](const LogEntry& e) { if (sub.empty() || has(e.text, sub)) r = e.text; });
  return r;
}
inline int logCount(const World& w, LogKind k) {
  int n = 0;
  w.log.each([&](const LogEntry& e) { n += e.kind == k; });
  return n;
}

}  // namespace rg::rulestest
