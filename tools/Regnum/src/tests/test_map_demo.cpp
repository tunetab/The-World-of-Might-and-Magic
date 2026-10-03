// Тесты демонстрационного мира (вымышленный пример, не канон): целостность геометрии и данных, состав,
// варианты построения (Tx, Store), расчёты правил.
#include "core/io.h"
#include "geo/topo.h"
#include "gfx/flag.h"
#include "gfx/text.h"
#include "rules/rules.h"
#include "tests/test_map_view_util.h"

using namespace rg;

TEST(map_demo_world_valid) {
  const double t0 = nowSeconds();
  const World& w = mvtest::demo();
  std::printf("  demo world: %.0f ms (cached after first build)\n", (nowSeconds() - t0) * 1000);
  // Геометрия без ошибок.
  const auto issues = geo::validate(w);
  CHECK_MSG(issues.empty(), issues.empty() ? std::string() : issues[0].code + ": " + issues[0].msg);
  // Нормализация ничего не исправляет.
  World copy = w;
  io::Warnings warns;
  const u32 fixed = io::normalize(copy, warns);
  CHECK_MSG(warns.empty(), warns.empty() ? std::string() : warns[0].text());
  CHECK_EQ(fixed, u32(0));
  // Чтение и запись JSON сохраняют мир.
  const io::LoadResult back = io::fromJson(io::toJson(w));
  CHECK(back.warnings.empty());
  CHECK_EQ(io::toJson(back.world), io::toJson(w));
  CHECK_EQ(w.meta->basemap, mvtest::basemap().id());
  CHECK(w.meta->notes.find("Не канон") != std::string::npos);
}

TEST(map_demo_world_content) {
  const World& w = mvtest::demo();
  int states = 0, guilds = 0, stateGuilds = 0;
  w.factions.each([&](const Faction& f) {
    if (f.isState()) {
      states++;
      CHECK(f.ruler != 0);
      CHECK(f.capital != 0);
      CHECK(!f.rulerTitle.empty());
      CHECK(f.council.size() >= 3);
      CHECK(f.culture && f.religion && f.government);
    } else {
      guilds++;
      stateGuilds += f.stateGuild;
      CHECK(f.homeState != 0);
    }
  });
  CHECK_EQ(states, 8);
  CHECK_EQ(guilds, 4);
  CHECK_EQ(stateGuilds, 1);
  // Цвета государств различимы.
  std::vector<Color> cols;
  w.factions.each([&](const Faction& f) { if (f.isState()) cols.push_back(f.color); });
  for (size_t i = 0; i < cols.size(); i++)
    for (size_t j = i + 1; j < cols.size(); j++)
      CHECK(std::abs(cols[i].r - cols[j].r) + std::abs(cols[i].g - cols[j].g) + std::abs(cols[i].b - cols[j].b) > 60);

  auto fs = geo::faces(w);
  int land = 0, sea = 0, owned = 0, unowned = 0, occupied = 0, withHq = 0, withInf = 0, withBuild = 0;
  w.provinces.each([&](const Province& p) {
    CHECK_MSG(fs->shape(p.id) != nullptr, p.name);
    CHECK(!p.name.empty());
    if (p.sea) {
      sea++;
      return;
    }
    land++;
    if (p.owner) owned++; else unowned++;
    occupied += p.occupied;
    withHq += !p.hqs.empty();
    withInf += !p.influence.empty();
    withBuild += !p.buildings.empty();
    CHECK(!p.races.empty());
    CHECK(p.resource != 0);
    if (p.owner) CHECK(p.culture != 0 && p.religion != 0);
  });
  std::printf("  demo: %d land (%d owned, %d unowned), %d sea, %d occupied, %d with HQ, %d with influence\n", land, owned, unowned, sea,
              occupied, withHq, withInf);
  CHECK(land >= 55 && land <= 75);
  CHECK_EQ(sea, 6);
  CHECK(unowned >= 3);
  CHECK_EQ(occupied, 3);
  CHECK(withHq >= 6);
  CHECK(withInf >= 15);
  CHECK(withBuild >= 20);

  int armies = 0, fleets = 0, allied = 0;
  w.armies.each([&](const Army& a) {
    (a.isFleet() ? fleets : armies)++;
    allied += a.allied();
    CHECK(rules::validPosition(w, a.kind, a.pos, a.id));
  });
  CHECK(armies >= 7);
  CHECK(fleets >= 4);
  CHECK_EQ(allied, 2);
  CHECK(w.routes.size() >= 4);
  int prereqs = 0, studied = 0, research = 0;
  w.techs.each([&](const Tech& t) {
    prereqs += int(t.prereqs.size());
    studied += t.studied;
    research += t.research;
  });
  CHECK(prereqs >= 10);
  CHECK(studied >= 4);
  CHECK(research >= 2);
  CHECK(w.buildings.size() >= 5);
  CHECK(w.modifiers.size() >= 5);
  CHECK(w.deals.size() >= 3);
  int wars = 0, alliances = 0;
  for (const auto& [k, r] : *w.relations) {
    wars += r.s == RelStatus::War;
    alliances += r.s == RelStatus::Alliance;
  }
  CHECK_EQ(wars, 2);
  CHECK_EQ(alliances, 3);  // два союза государств и государственная гильдия со своим государством
  int heroes = 0;
  w.characters.each([&](const Character& c) { heroes += c.hero; });
  CHECK(heroes >= 16);
  // Каталоги для режимов карты.
  CHECK(w.catalogs->races.size() >= 5);
  CHECK(w.catalogs->cultures.size() >= 8);
  CHECK(w.catalogs->religions.size() >= 4);
  // Расчёт правил: доходы, торговля, восстания.
  auto calc = rules::calc(w);
  int traded = 0;
  for (const auto& [pid, pc] : calc->provinces) traded += pc.tradeValue > 0;
  CHECK(traded > 40);
  const rules::TurnReport rep = rules::previewTurn(w);
  CHECK_EQ(rep.turnTo, rep.turnFrom + 1);
}

TEST(map_demo_world_flags) {
  // Флаги и цвета фракций демонстрационного мира — для просмотра.
  const World& w = mvtest::demo();
  const float s = 2;
  gfx::Image img(int(900 * s), int(330 * s), gfx::premul(Color::hex(0x151a22)));
  gfx::Canvas c(img);
  c.scale(s, s);
  int i = 0;
  w.factions.each([&](const Faction& f) {
    const float x = 20 + float(i % 3) * 296, y = 18 + float(i / 3) * 76;
    gfx::drawFlag(c, f.flag, RectF(x, y, 72, 48), 4);
    c.fillRoundRect(RectF(x + 82, y + 4, 10, 10), 3, f.color);
    gfx::TextStyle ts;
    ts.size = 13;
    ts.weight = gfx::FontWeight::Semibold;
    gfx::drawText(c, f.name, ts, x + 98, y + 1, Color::hex(0xece8df));
    ts.weight = gfx::FontWeight::Regular;
    ts.size = 11.5f;
    const std::string sub = f.isState() ? f.rulerTitle + " " + w.characterName(f.ruler) : (f.stateGuild ? "государственная гильдия" : "гильдия");
    gfx::drawText(c, sub, ts, x + 82, y + 22, Color::hex(0xa7afbb));
    i++;
  });
  CHECK_EQ(i, 12);
  mvtest::savePng(img, "map_demo_flags.png");
}

TEST(map_demo_world_build_variants) {
  // В пустую транзакцию.
  World base = newWorld("Пустой");
  Tx tx(base);
  map::buildDemoWorld(tx, mvtest::basemap());
  const World w = std::move(tx).finish();
  CHECK_EQ(io::toJson(w), io::toJson(mvtest::demo()));
  // Повторно в непустой мир — отказ.
  Tx again(w);
  CHECK_THROWS(map::buildDemoWorld(again, mvtest::basemap()));
  // Хранилище.
  Store s;
  map::buildDemoWorld(s, mvtest::basemap());
  CHECK_EQ(s.world().provinces.size(), mvtest::demo().provinces.size());
  CHECK(!s.canUndo());
  // Без базовой карты.
  map::Basemap none;
  CHECK_THROWS(map::makeDemoWorld(none));
}
