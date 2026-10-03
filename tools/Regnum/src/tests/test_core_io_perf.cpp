// Производительность core/io: мир масштаба кампании (2000 провинций, ~60 тыс. точек геометрии,
// фракции, персонажи, войска, хроника) — полное сохранение, чтение, частичное сохранение, архив, снимок.
#include <cstdio>

#include "tests/test_core_io_util.h"

using namespace rg;
using namespace rg::iotest;

namespace {

// Сетка 50 × 40 клеток-провинций; каждая дуга — 15 промежуточных точек (координаты кратны 0,25 —
// точно переживают округление до 0,01).
World bigWorld(size_t* points) {
  World base = newWorld("Большой мир");
  Tx tx(base);
  constexpr int kCols = 50, kRows = 40, kSeg = 16;
  constexpr double kCell = 150;
  Rng rng(42);
  auto q = [](double v) { return std::round(v * 4) / 4; };

  for (int i = 0; i < 6; i++) {
    CatalogItem r;
    r.id = Id(i + 1);
    r.name = "Раса " + std::to_string(i + 1);
    r.color = Color::palette(i);
    tx.catalogs().races.push_back(r);
    r.name = "Культура " + std::to_string(i + 1);
    tx.catalogs().cultures.push_back(r);
    r.name = "Религия " + std::to_string(i + 1);
    tx.catalogs().religions.push_back(r);
  }
  tx.meta().seq[int(Seq::Race)] = tx.meta().seq[int(Seq::Culture)] = tx.meta().seq[int(Seq::Religion)] = 6;

  // 40 государств и 20 гильдий; у государств — строки армии и флота.
  for (int i = 0; i < 60; i++) {
    Faction f;
    f.kind = i < 40 ? FactionKind::State : FactionKind::Guild;
    f.name = (i < 40 ? "Королевство " : "Гильдия ") + std::to_string(i + 1);
    f.color = Color::palette(i);
    f.culture = Id(1 + i % 6);
    f.res = {{kGold, 1000.0 * i + 0.5}, {2, 40.0 + i}};
    if (f.isState()) {
      for (int k = 0; k < 4; k++) {
        Id row = tx.nextId(Seq::Row);
        f.army.push_back(ArmyRow{row, "Полк " + std::to_string(k), UnitType(k % int(UnitType::Count)), 1000 + k * 10, 1.5});
      }
      f.fleet.push_back(FleetRow{tx.nextId(Seq::Row), "Эскадра", ShipType::Frigate, 12, 20});
    } else {
      f.homeState = Id(1 + i % 40);
    }
    tx.add(std::move(f));
  }
  for (int i = 0; i < 400; i++) {
    Character c;
    c.name = "Персонаж " + std::to_string(i + 1);
    c.title = "Лорд";
    c.faction = Id(1 + i % 60);
    c.notes = "Заметки о персонаже, несколько слов для объёма.";
    tx.add(std::move(c));
  }

  // Провинции.
  for (int y = 0; y < kRows; y++)
    for (int x = 0; x < kCols; x++) {
      Province p;
      p.name = "Провинция " + std::to_string(y * kCols + x + 1);
      p.owner = Id(1 + (x / 5 + (y / 5) * 10) % 40);
      p.capital = "Город " + std::to_string(y * kCols + x + 1);
      p.size = ProvSize((x + y) % 3);
      p.city = CityType((x * 7 + y) % 4);
      p.resource = Id(1 + (x + y) % 6);
      p.resourceAmount = (x * y) % 50;
      const Faction* owner = tx.w().faction(p.owner);
      p.garrison = {{owner->army[0].id, 100 + x}, {owner->army[1].id, 50 + y}};
      p.contentment = double((x * 13 + y * 7) % 200 - 100);
      p.culture = Id(1 + x % 6);
      p.religion = Id(1 + y % 6);
      p.races = {{1, 10000 + x * 100}, {Id(2 + y % 5), 2500 + y * 10}};
      p.influence = {{Id(41 + (x + y) % 20), 30.5}, {Id(41 + (x + y + 1) % 20), 20.25}};
      p.hqs = {Id(41 + (x + y) % 20)};
      p.baseTrade = 100 + x + y;
      p.localTax = 2.5;
      p.notes = "Описание провинции: холмы, леса, тракт.";
      tx.add(std::move(p));
    }

  // Узлы сетки и дуги с извилистыми точками.
  auto nodeId = [&](int x, int y) { return Id(y * (kCols + 1) + x + 1); };
  for (int y = 0; y <= kRows; y++)
    for (int x = 0; x <= kCols; x++) tx.add(Node{nodeId(x, y), {q(100 + x * kCell), q(100 + y * kCell * 0.7)}});
  size_t pts = size_t((kCols + 1) * (kRows + 1));
  auto prov = [&](int x, int y) -> Id { return x < 0 || y < 0 || x >= kCols || y >= kRows ? 0 : Id(y * kCols + x + 1); };
  auto addEdge = [&](int x0, int y0, int x1, int y1, Id pl, Id pr) {
    Edge e;
    e.a = nodeId(x0, y0);
    e.b = nodeId(x1, y1);
    Vec2 a = tx.w().nodes.get(e.a)->p, b = tx.w().nodes.get(e.b)->p;
    Vec2 n = (b - a).perp().norm();
    for (int k = 1; k < kSeg; k++) {
      double t = double(k) / kSeg;
      Vec2 p = a + (b - a) * t + n * (rng.uniform() * 12 - 6);
      e.pts.push_back({q(p.x), q(p.y)});
    }
    e.kind = pl && pr ? EdgeKind::Border : EdgeKind::Frame;
    e.pl = pl;
    e.pr = pr;
    e.tl = pl ? Terrain::Land : Terrain::None;
    e.tr = pr ? Terrain::Land : Terrain::None;
    pts += e.pts.size();
    tx.add(std::move(e));
  };
  for (int y = 0; y <= kRows; y++)
    for (int x = 0; x < kCols; x++) addEdge(x, y, x + 1, y, prov(x, y - 1), prov(x, y));
  for (int y = 0; y < kRows; y++)
    for (int x = 0; x <= kCols; x++) addEdge(x, y, x, y + 1, prov(x, y), prov(x - 1, y));
  if (points) *points = pts;

  for (int i = 0; i < 200; i++) {
    Army a;
    a.kind = ArmyKind::Army;
    a.name = "Армия " + std::to_string(i + 1);
    a.pos = {q(200 + i * 30.5), q(300 + i * 10.25)};
    Id f = Id(1 + i % 40);
    const Faction* fp = tx.w().faction(f);
    a.groups = {ArmyGroup{f, {{fp->army[0].id, 500}, {fp->army[2].id, 120}}, {}}};
    tx.add(std::move(a));
  }
  for (int i = 0; i < 39; i++)
    for (int k = i + 1; k < 40; k += 3) tx.setRelation(Id(i + 1), Id(k + 1), Relation{double((i * k) % 200 - 100), RelStatus((i + k) % 4)});
  for (int i = 0; i < 3000; i++) {
    LogEntry l;
    l.turn = 1 + i / 100;
    l.kind = LogKind(i % int(LogKind::Count));
    l.text = "Событие хроники номер " + std::to_string(i + 1) + ": войска прошли через перевал.";
    l.province = Id(1 + i % 2000);
    l.factions = {Id(1 + i % 40)};
    l.at = "2026-10-01T12:00:00Z";
    tx.add(std::move(l));
  }
  return std::move(tx).finish();
}

template <class F> double timed(F&& f) {
  double t0 = nowSeconds();
  f();
  return (nowSeconds() - t0) * 1000;
}

}  // namespace

TEST(io_perf_big_world) {
  size_t points = 0;
  World w = bigWorld(&points);
  CHECK_EQ(w.provinces.size(), 2000u);
  CHECK(points >= 60000);
  {
    io::Warnings warns;
    World n = w;
    io::normalize(n, warns);
    CHECK_MSG(warns.empty(), warningsText(warns));
  }

  // Полное сохранение в пустую папку + чтение (лучшая из трёх попыток: в дереве параллельно идут сборки).
  double bestSave = kInf, bestLoad = kInf, bestTotal = kInf;
  io::LoadResult r;
  std::string dir;
  for (int i = 0; i < 3; i++) {
    dir = tempDir("perf");
    double s = timed([&] { io::save(dir, w, TB_ALL); });
    double l = timed([&] { r = io::load(dir); });
    bestSave = std::min(bestSave, s);
    bestLoad = std::min(bestLoad, l);
    bestTotal = std::min(bestTotal, s + l);
  }
  CHECK_MSG(r.warnings.empty(), warningsText(r.warnings));
  std::string d = diffWorld(w, r.world);
  CHECK_MSG(d.empty(), d);

  // Частичное сохранение: одна провинция изменена.
  Tx tx(r.world);
  tx.province(77).name = "Переименована";
  World w2 = std::move(tx).finish();
  io::SaveResult sr;
  double dirty = timed([&] { sr = io::save(dir, w2, World::diff(r.world, w2), &r.files); });
  CHECK_EQ(sr.written.size(), size_t(1));

  // Архив, снимок, автосохранение.
  std::string bundle = fs::join(dir, "big.regnum");
  double tBundle = timed([&] { io::saveBundle(bundle, w2); });
  double tBundleLoad = timed([&] { r = io::load(bundle); });
  CHECK(diffWorld(w2, r.world).empty());
  std::vector<u8> gz;
  double tPack = timed([&] { gz = io::pack(w2); });
  double tUnpack = timed([&] { r = io::unpack(gz); });
  CHECK(diffWorld(w2, r.world).empty());

  u64 bytes = 0;
  for (auto& f : io::projectFiles()) bytes += fs::fileSize(fs::join(dir, f.path)).value_or(0);
  std::printf("  io perf: %u провинций, %zu точек, %.1f МБ JSON; сохранение %.0f мс, чтение %.0f мс, вместе %.0f мс; "
              "одна таблица %.0f мс; архив %.0f/%.0f мс (%.1f МБ); снимок gzip %.0f/%.0f мс (%.1f МБ)\n",
              w.provinces.size(), points, double(bytes) / 1048576.0, bestSave, bestLoad, bestTotal, dirty, tBundle, tBundleLoad,
              double(fs::fileSize(bundle).value_or(0)) / 1048576.0, tPack, tUnpack, double(gz.size()) / 1048576.0);
#ifndef RG_DEBUG
  CHECK_MSG(bestTotal < 400, "сохранение + чтение " + std::to_string(bestTotal) + " мс (предел 400 мс)");
#endif
}
