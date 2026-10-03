// Тесты core/io по итогам аудита ТЗ: уплаченное за строительство (paid/payer) в файлах и нормализации,
// уникальная постройка гильдии, изученная технология с неизученными условиями.
#include "base/json.h"
#include "core/io_internal.h"
#include "tests/test_core_io_util.h"

using namespace rg;
using namespace rg::iotest;

namespace {

World normalized(World w, io::Warnings& warns) {
  io::normalize(w, warns);
  return w;
}

}  // namespace

// Уплаченное и плательщик строящегося уровня сохраняются в data/provinces.json и читаются обратно (FORMAT.md).
TEST(core_io_fix_paid_round_trip) {
  World w = richWorld();
  const std::string text = io::fileText(w, "data/provinces.json");
  CHECK(text.find("\"paid\"") != std::string::npos);
  CHECK(text.find("\"payer\"") != std::string::npos);
  std::string dir = tempDir("fix_paid_rt");
  io::save(dir, w, TB_ALL);
  io::LoadResult r = io::load(dir);
  CHECK_MSG(r.warnings.empty(), warningsText(r.warnings));
  const Province* p = r.world.province(1);
  CHECK(p != nullptr);
  CHECK(p->buildings[0].constructing);
  CHECK_EQ(p->buildings[0].payer, Id(1));
  CHECK_NEAR(p->buildings[0].paid.at(kGold), 120.5, 1e-9);
  CHECK_NEAR(p->buildings[0].paid.at(2), 40, 1e-9);
  CHECK(p->buildings[1].paid.empty());
  CHECK_EQ(p->buildings[1].payer, Id(0));
}

// Нормализация уплаченного: у достроенной — удаляется; неизвестный ресурс, отрицательное число, плательщик-гильдия
// или несуществующий — исправляются с предупреждениями.
TEST(core_io_fix_paid_normalize) {
  World w = richWorld();
  {
    Tx tx(w);
    Province& p = tx.province(1);
    p.buildings[0].paid = {{kGold, -5}, {999, 3}};
    p.buildings[0].payer = 2;  // f2 — гильдия
    p.buildings[1].paid = {{kGold, 10}};
    p.buildings[1].payer = 1;
    w = std::move(tx).finish();
  }
  io::Warnings warns;
  World n = normalized(w, warns);
  const Province& p = *n.province(1);
  CHECK_NEAR(p.buildings[0].paid.at(kGold), 0, 1e-9);
  CHECK(!p.buildings[0].paid.count(999));
  CHECK_EQ(p.buildings[0].payer, Id(0));
  CHECK(p.buildings[1].paid.empty());
  CHECK_EQ(p.buildings[1].payer, Id(0));
  CHECK_MSG(hasWarning(warns, "data/provinces.json", "p1.buildings[0].payer", "не государство"), warningsText(warns));
  CHECK_MSG(hasWarning(warns, "data/provinces.json", "p1.buildings[0].paid", "нет ресурса"), warningsText(warns));
  CHECK_MSG(hasWarning(warns, "data/provinces.json", "p1.buildings[1].paid", "достроенной"), warningsText(warns));
}

// Файл прежней версии: строящаяся постройка без «paid» — плательщик — владелец, возврата нет, предупреждение.
TEST(core_io_fix_paid_legacy_file) {
  World w = richWorld();
  std::string dir = tempDir("fix_paid_legacy");
  io::save(dir, w, TB_ALL);
  json::Value doc = json::parse(fs::readFile(fs::join(dir, "data/provinces.json")).value());
  for (auto& e : doc["provinces"].items())
    for (auto& b : e["buildings"].items()) {
      b.erase("paid");
      b.erase("payer");
    }
  CHECK(fs::writeFileAtomic(fs::join(dir, "data/provinces.json"), json::write(doc, json::WriteOptions{2, true, true})));
  io::LoadResult r = io::load(dir);
  const Province* p = r.world.province(1);
  CHECK(p != nullptr);
  CHECK(p->buildings[0].paid.empty());
  CHECK_EQ(p->buildings[0].payer, p->owner);
  bool warned = false;
  for (auto& x : r.warnings) warned = warned || (x.file == "data/provinces.json" && x.msg.find("не записана") != std::string::npos);
  CHECK_MSG(warned, warningsText(r.warnings));
}

// Уникальная постройка с владельцем-гильдией (ручная правка): предупреждение, постройка становится общей.
TEST(core_io_fix_guild_owned_building) {
  World w = richWorld();
  Id bid = 0;
  w.buildings.each([&](const Building& b) {
    if (!bid) bid = b.id;
  });
  CHECK(bid != 0);
  {
    Tx tx(w);
    tx.building(bid).owner = 2;  // f2 — гильдия
    w = std::move(tx).finish();
  }
  io::Warnings warns;
  World n = normalized(w, warns);
  CHECK_EQ(n.building(bid)->owner, Id(0));
  CHECK_MSG(hasWarning(warns, "data/buildings.json", io::detail::refStr(Seq::Building, bid) + ".owner", "гильдия"), warningsText(warns));
}

// Изученная технология с неизученным условием (ручная правка): изученность снимается по цепочке, с предупреждениями.
TEST(core_io_fix_studied_without_prereqs) {
  World w = newWorld("Технологии");
  Id t1 = 0, t2 = 0, t3 = 0;
  {
    Tx tx(w);
    Faction f;
    f.name = "Арден";
    Id fid = tx.add(std::move(f)).id;
    Tech a, b, c;
    a.faction = b.faction = c.faction = fid;
    a.name = "Основа";
    a.turns = 3;
    t1 = tx.add(std::move(a)).id;
    b.name = "Следствие";
    b.turns = 4;
    b.prereqs = {t1};
    b.studied = true;
    b.progress = 4;
    t2 = tx.add(std::move(b)).id;
    c.name = "Вершина";
    c.prereqs = {t2};
    c.studied = true;
    c.progress = 1;
    t3 = tx.add(std::move(c)).id;
    w = std::move(tx).finish();
  }
  io::Warnings warns;
  World n = normalized(w, warns);
  CHECK(!n.tech(t1)->studied);
  CHECK(!n.tech(t2)->studied);
  CHECK(!n.tech(t3)->studied);
  CHECK(n.tech(t2)->progress < n.tech(t2)->turns);
  CHECK_MSG(hasWarning(warns, "data/techs.json", io::detail::refStr(Seq::Tech, t2) + ".studied", "изученность снята"), warningsText(warns));
  CHECK_MSG(hasWarning(warns, "data/techs.json", io::detail::refStr(Seq::Tech, t3) + ".studied", "изученность снята"), warningsText(warns));
  // Согласованный мир повторно не меняется.
  io::Warnings again;
  World n2 = normalized(n, again);
  CHECK_MSG(again.empty(), warningsText(again));
}
