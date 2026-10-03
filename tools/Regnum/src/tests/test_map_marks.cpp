// Регрессии отрисовщика карты (исправления аудита ТЗ):
//  * отметки войск и флота не перекрываются: наложившиеся объекты — стопка, при приближении распадается;
//  * «Показать всю карту» и минимальный масштаб — по свободной части между панелями (setSafeArea);
//  * маршрут рисуется ровно по ломаной, по которой считаются его провинции (бонус +10 %);
//  * сухопутная провинция на морской части карты видна (заливка поверх моря).
#include "geo/ops.h"
#include "map/map_internal.h"
#include "rules/rules.h"
#include "tests/test_map_view_util.h"

using namespace rg;

namespace {

constexpr int W = 1920, H = 1080;
constexpr float DPI = 1.25f;

Army makeArmy(Id id, Vec2 pos, Id faction, i64 units, ArmyKind kind = ArmyKind::Army) {
  Army a;
  a.id = id;
  a.kind = kind;
  a.pos = pos;
  a.groups.push_back(ArmyGroup{faction, {ArmyUnit{1, units}}, {}});
  return a;
}

// Отметки при текущей камере: каждый объект ровно в одной, габариты не пересекаются, щелчок по отметке — её верх.
int checkMarks(const map::MapView& mv, const World& w, const char* when) {
  const std::vector<map::ArmyMark> marks = mv.armyMarks();
  std::vector<Id> seen;
  int stacks = 0;
  for (const map::ArmyMark& m : marks) {
    CHECK(!m.members.empty());
    CHECK_EQ(m.members.front(), m.top);
    seen.insert(seen.end(), m.members.begin(), m.members.end());
    if (m.cluster()) stacks++;
    i64 units = 0;
    for (Id id : m.members) units += map::detail::armyCount(*w.army(id));
    CHECK_EQ(units, m.units);
    const gfx::Pt s = mv.view().toScreen(m.pos);
    if (s.x > 0 && s.y > 0 && s.x < W && s.y < H) {
      CHECK_EQ(mv.armyAt(s.x, s.y), m.top);
      const std::optional<map::ArmyMark> hit = mv.markAt(s.x, s.y);
      CHECK(hit.has_value() && hit->members == m.members);
    }
  }
  std::sort(seen.begin(), seen.end());
  CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
  CHECK_EQ(seen.size(), size_t(w.armies.size()));
  int overlaps = 0;
  for (size_t i = 0; i < marks.size(); i++)
    for (size_t j = i + 1; j < marks.size(); j++)
      if (!marks[i].bounds.intersect(marks[j].bounds).empty()) overlaps++;
  CHECK_EQ(overlaps, 0);
  std::printf("  %s: zoom %.3f, marks %zu, stacks %d\n", when, mv.view().zoom, marks.size(), stacks);
  return stacks;
}

}  // namespace

// ================================================================ раскладка отметок
TEST(map_marks_layout_stack) {
  // Три войска на минимальном допустимом расстоянии (2 × 34 единицы карты).
  std::vector<Army> list = {makeArmy(1, {1000, 1000}, 7, 10), makeArmy(2, {1068, 1000}, 8, 500), makeArmy(3, {1136, 1000}, 9, 20),
                            makeArmy(4, {3000, 1000}, 7, 5)};
  std::vector<const Army*> ptrs;
  for (const Army& a : list) ptrs.push_back(&a);
  // Обзорный масштаб: фигурка 24 точки, войска в 14 точках друг от друга — одна стопка из трёх и одиночная фигурка.
  map::detail::MarkLayout L = map::detail::layoutMarks(ptrs, 0.2, 0);
  CHECK_EQ(L.marks.size(), size_t(2));
  const map::detail::MarkLayout::Mark* stack = nullptr;
  for (const auto& m : L.marks)
    if (m.members.size() > 1) stack = &m;
  CHECK(stack != nullptr);
  if (stack) {
    CHECK_EQ(stack->members.size(), size_t(3));
    CHECK_EQ(stack->members.front(), Id(2));   // сверху — самое многочисленное
    CHECK_EQ(stack->units, i64(530));
    CHECK(stack->pos == list[1].pos);
  }
  // Выделенный объект — всегда сверху своей стопки.
  L = map::detail::layoutMarks(ptrs, 0.2, 3);
  for (const auto& m : L.marks)
    if (m.members.size() > 1) CHECK_EQ(m.members.front(), Id(3));
  // Крупный масштаб: все по отдельности.
  L = map::detail::layoutMarks(ptrs, 3.0, 0);
  CHECK_EQ(L.marks.size(), size_t(4));
  // Габариты стопки шире одиночной фигурки (фигурки за верхней и значок числа объектов).
  const RectF one = map::detail::markFootprint(24, 1, 10), three = map::detail::markFootprint(24, 3, 530);
  CHECK(three.x < one.x && three.y < one.y);
  CHECK(three.w > one.w && three.h > one.h);
}

TEST(map_marks_demo_zooms) {
  const World& w = mvtest::demo();
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(w);
  mv.setViewport(RectF(0, 0, W, H), DPI);
  int atMin = -1, atMax = -1;
  for (double z : {0.0, 0.2, 0.3, 0.45, 0.6, 1.0, 2.0, 3.0}) {
    if (z == 0) mv.fitAll(false);
    else mv.centerOn({4300, 1700}, z, false);
    const int stacks = checkMarks(mv, w, z == 0 ? "fit" : "zoom");
    if (z == 0) atMin = stacks;
    if (z == 3.0) atMax = stacks;
  }
  CHECK(atMin > 0);    // при обзорном масштабе есть стопки (иначе фигурки перекрывались бы)
  CHECK_EQ(atMax, 0);  // при наибольшем — все объекты по отдельности
  // Стопка распадается при масштабе separateZoom.
  mv.fitAll(false);
  std::vector<map::ArmyMark> marks = mv.armyMarks();
  int tried = 0;
  for (const map::ArmyMark& m : marks) {
    if (!m.cluster() || tried >= 4) continue;
    tried++;
    mv.fitAll(false);
    const double z = mv.separateZoom(m);
    CHECK(z > mv.view().zoom);
    mv.centerOn(m.pos, z, false);
    std::vector<map::ArmyMark> after = mv.armyMarks();
    for (Id id : m.members)
      for (const map::ArmyMark& a : after)
        if (std::find(a.members.begin(), a.members.end(), id) != a.members.end())
          for (Id other : m.members)
            if (other != id) CHECK_MSG(std::find(a.members.begin(), a.members.end(), other) == a.members.end(), "объекты стопки не разошлись");
  }
  CHECK(tried > 0);
  // При наибольшем масштабе приближать некуда.
  mv.centerOn({4300, 1700}, mv.maxZoom(), false);
  for (const map::ArmyMark& m : mv.armyMarks()) CHECK(!m.cluster() || mv.separateZoom(m) == 0);
}

// Снимок стопок при обзорном масштабе: видны значок числа объектов (золотой круг) и фигурки за верхней.
TEST(map_marks_render_stacks) {
  const World& w = mvtest::demo();
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(w);
  mv.setViewport(RectF(0, 0, W, H), DPI);
  mv.fitAll(false);
  map::RenderOptions opt;
  gfx::Image img = mvtest::renderFull(mv, opt, W, H, DPI);
  mvtest::savePng(img, "map_view_marks_fit.png");
  int stacks = 0, gold = 0;
  for (const map::ArmyMark& m : mv.armyMarks()) {
    if (!m.cluster()) continue;
    const gfx::RectI r(int(m.bounds.x * DPI) - 6, int(m.bounds.y * DPI) - 6, int(m.bounds.w * DPI) + 12, int(m.bounds.h * DPI) + 12);
    if (stacks < 3) mvtest::saveCrop(img, r, 4, "map_view_marks_stack_" + std::to_string(stacks + 1) + ".png");
    stacks++;
    // Золотой значок числа объектов справа сверху от верхней фигурки.
    const gfx::Pt c = mv.view().toScreen(m.pos);
    const float s = mv.figureSize();
    const int bx = int((c.x + s * 0.46f) * DPI), by = int((c.y - s * 0.56f) * DPI);
    bool found = false;
    for (int y = by - 6; y <= by + 6 && !found; y++)
      for (int x = bx - 6; x <= bx + 6 && !found; x++) {
        if (x < 0 || y < 0 || x >= img.w || y >= img.h) continue;
        const Color p = gfx::unpremul(img.at(x, y));
        found = p.r > 220 && p.g > 170 && p.g < 215 && p.b < 120;
      }
    gold += found ? 1 : 0;
  }
  CHECK(stacks > 0);
  CHECK_EQ(gold, stacks);
  // Выделенный объект стопки — сверху (нарисован с выделением), щелчок по стопке — он.
  for (const map::ArmyMark& m : mv.armyMarks()) {
    if (!m.cluster()) continue;
    opt.selArmy = m.members.back();
    gfx::Image sel = mvtest::renderFull(mv, opt, W, H, DPI);
    bool top = false;
    for (const map::ArmyMark& a : mv.armyMarks())
      if (std::find(a.members.begin(), a.members.end(), opt.selArmy) != a.members.end()) {
        top = a.top == opt.selArmy;
        const gfx::Pt c = mv.view().toScreen(a.pos);
        CHECK_EQ(mv.armyAt(c.x, c.y), opt.selArmy);
        mvtest::saveCrop(sel, gfx::RectI(int(a.bounds.x * DPI) - 6, int(a.bounds.y * DPI) - 6, int(a.bounds.w * DPI) + 12, int(a.bounds.h * DPI) + 12), 4,
                         "map_view_marks_stack_selected.png");
      }
    CHECK(top);
    break;
  }
}

// ================================================================ свободная часть области просмотра
TEST(map_view_safe_area_fit) {
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(mvtest::demo());
  mv.setViewport(RectF(0, 0, W, H), DPI);
  const double full = mv.minZoom();
  // Панели: верхняя полоса 56, лента слева 72, инспектор справа 420.
  const RectF area(72, 56, W - 72 - 420, H - 56);
  mv.setSafeArea(area);
  CHECK(mv.safeArea() == area);
  CHECK(mv.minZoom() < full);
  CHECK(mv.minZoom() <= std::min(area.w / 8000.0, area.h / 4500.0) + 1e-9);
  mv.fitAll(false);
  const gfx::Pt tl = mv.view().toScreen({0, 0}), br = mv.view().toScreen({8000, 4500});
  CHECK(tl.x >= area.x - 0.01f && tl.y >= area.y - 0.01f);
  CHECK(br.x <= area.right() + 0.01f && br.y <= area.bottom() + 0.01f);
  CHECK_NEAR(mv.view().zoom, mv.minZoom(), 1e-12);
  // Колесо: отдаление до minZoom возможно, весь мир по-прежнему в свободной части.
  mv.zoomAt(area.cx(), area.cy(), 3, false);
  mv.zoomAt(area.cx(), area.cy(), 1e-6, false);
  CHECK_NEAR(mv.view().zoom, mv.minZoom(), 1e-12);
  // Панель закрылась (свободная часть больше): камера не дёргается, колесо от этого масштаба не приближает.
  mv.fitAll(false);
  const map::View before = mv.view();
  mv.setSafeArea(RectF());
  mv.setViewport(RectF(0, 0, W, H), DPI);
  CHECK_EQ(mv.view().zoom, before.zoom);
  CHECK_EQ(mv.view().cx, before.cx);
  mv.zoomAt(960, 540, 0.5, false);
  CHECK(mv.view().zoom <= before.zoom + 1e-12);
  // «Показать область» — тоже в свободной части.
  mv.setSafeArea(area);
  mv.fit(Box2(3000, 1000, 3400, 1300), false);
  const gfx::Pt a = mv.view().toScreen({3000, 1000}), b = mv.view().toScreen({3400, 1300});
  CHECK(a.x >= area.x && a.y >= area.y && b.x <= area.right() && b.y <= area.bottom());
}

// ================================================================ маршруты
TEST(map_route_drawn_along_polyline) {
  const World& w = mvtest::demo();
  auto fs = geo::faces(w);
  int checked = 0;
  w.routes.each([&](const Route& r) {
    const std::vector<Vec2> line = map::detail::routeLine(r.pts);
    CHECK(line.size() >= 2);
    CHECK(fs->provincesOnPolyline(line) == fs->provincesOnPolyline(r.pts));
    checked++;
  });
  CHECK(checked > 0);
  // Повторяющиеся точки убираются, остальные — без изменений (без сглаживания).
  const std::vector<Vec2> zig = {{100, 100}, {100, 100}, {400, 600}, {700, 100}};
  const std::vector<Vec2> got = map::detail::routeLine(zig);
  CHECK_EQ(got.size(), size_t(3));
  CHECK(got[1] == zig[2]);
  // Щелчок точно по середине отрезка ломаной попадает в маршрут (линия не отходит от ломаной на изгибе).
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(w);
  mv.setViewport(RectF(0, 0, W, H), DPI);
  int hits = 0;
  w.routes.each([&](const Route& r) {
    for (size_t i = 1; i + 1 < r.pts.size() && hits < 6; i++) {
      const Vec2 mid = (r.pts[i] + r.pts[i + 1]) * 0.5;
      mv.centerOn(mid, 1.0, false);
      const gfx::Pt s = mv.view().toScreen(mid);
      CHECK_EQ(mv.routeAt(s.x, s.y, 2), r.id);
      hits++;
    }
  });
  CHECK(hits > 0);
}

// ================================================================ сухопутная провинция на морской части карты
TEST(map_land_province_on_sea_filled) {
  const World& base = mvtest::demo();
  auto fs = geo::faces(base);
  Id pid = 0, state = 0;
  double best = 0;
  base.provinces.each([&](const Province& p) {
    const geo::ProvinceShape* sh = fs->shape(p.id);
    if (p.sea && sh && sh->area > best) {
      best = sh->area;
      pid = p.id;
    }
  });
  base.factions.each([&](const Faction& f) {
    if (!state && f.isState()) state = f.id;
  });
  CHECK(pid && state);
  World w = [&] {
    Tx tx(base);
    rules::setProvinceSea(tx, pid, false);
    rules::setProvinceOwner(tx, pid, state);
    return std::move(tx).finish();
  }();
  CHECK(!w.province(pid)->sea);
  CHECK_EQ(w.province(pid)->owner, state);
  const geo::ProvinceShape* sh = geo::faces(w)->shape(pid);
  CHECK(sh != nullptr);
  if (!sh) return;
  map::RenderOptions opt;
  auto shot = [&](const World& world, const char* name) {
    map::MapView mv(&mvtest::basemap());
    mv.setWorld(world);
    mv.setViewport(RectF(0, 0, W, H), DPI);
    mv.fit(sh->box, false);
    gfx::Image img = mvtest::renderFull(mv, opt, W, H, DPI);
    mvtest::savePng(img, name);
    return std::make_pair(img, mv.view());
  };
  auto [sea, v] = shot(base, "map_view_sea_province.png");
  auto [land, v2] = shot(w, "map_view_sea_province_land.png");
  const Color col = w.faction(state)->color;
  int n = 0, tinted = 0;
  for (int k = 0; k < 144; k++) {
    const Vec2 m{sh->box.x0 + sh->box.w() * ((k % 12) + 0.5) / 12, sh->box.y0 + sh->box.h() * ((k / 12) + 0.5) / 12};
    if (geo::faces(w)->provinceAt(m) != pid || geo::faces(w)->terrainAt(m) != Terrain::Sea) continue;
    const gfx::Pt s = v2.toScreen(m);
    const int x = int(s.x * DPI), y = int(s.y * DPI);
    if (x < 0 || y < 0 || x >= land.w || y >= land.h) continue;
    const Color a = gfx::unpremul(sea.at(x, y)), b = gfx::unpremul(land.at(x, y));
    n++;
    // Ближе к цвету государства, чем чистое море.
    auto d = [&](Color p) { return std::abs(p.r - col.r) + std::abs(p.g - col.g) + std::abs(p.b - col.b); };
    if (d(b) + 40 < d(a)) tinted++;
  }
  std::printf("  sea samples %d, tinted %d\n", n, tinted);
  CHECK(n > 10);
  CHECK(tinted * 10 >= n * 9);
}
