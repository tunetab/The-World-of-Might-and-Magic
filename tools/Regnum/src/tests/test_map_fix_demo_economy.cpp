// Демонстрационный мир (вымышленный пример, не канон): правдоподобная экономика государств. Большинство государств
// слегка в плюсе, одно-два в минусе; доходы и расходы одного порядка, числа читаемые (аудит ТЗ 1.e).
#include "rules/rules.h"
#include "tests/test_map_view_util.h"

using namespace rg;

TEST(map_fix_demo_economy_balanced) {
  const World& w = mvtest::demo();
  auto c = rules::calc(w);
  w.factions.each([&](const Faction& f) {
    if (const rules::FactionCalc* fc = c->faction(f.id))
      std::printf("  %-32s казна %8.1f доход %7.1f расход %7.1f (войска %6.1f, флот %5.1f, спец. %4.1f) чистый %+7.1f\n", f.name.c_str(),
                  f.treasury(), fc->incTotal, fc->expTotal, fc->expArmy, fc->expFleet, fc->expSpecialists, fc->net);
  });
  int states = 0, negative = 0, positive = 0;
  w.factions.each([&](const Faction& f) {
    const rules::FactionCalc* fc = c->faction(f.id);
    CHECK(fc != nullptr);
    if (!fc) return;
    if (!f.isState()) {
      CHECK(fc->net >= 0);  // гильдии живут доходом штабов
      return;
    }
    states++;
    CHECK(fc->incTotal > 100);                       // доход — сотни, не десятки
    CHECK(fc->expTotal > 100);
    CHECK(fc->expTotal < fc->incTotal * 1.5);         // расходы одного порядка с доходом
    CHECK(fc->expArmy > fc->expSpecialists);          // армия — главная статья расходов
    CHECK(std::fabs(fc->net) < fc->incTotal * 0.4);   // «слегка»: не больше 40 % дохода
    CHECK(f.treasury() > 10 * std::fabs(fc->net));    // казны хватает надолго
    if (fc->net < 0) negative++;
    else positive++;
  });
  CHECK_EQ(states, 8);
  CHECK(negative >= 1 && negative <= 2);
  CHECK(positive >= 6);
}
