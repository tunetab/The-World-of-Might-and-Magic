// Тесты отрисовки карты: снимки 1920 × 1080 (dpi 1,25) демонстрационного мира во всех режимах, масштабы,
// выделение и наведение, правка границ, мини-карта, сравнение пустого мира с превью базовой карты.
#include "geo/ops.h"
#include "rules/rules.h"
#include "tests/test_map_view_util.h"

using namespace rg;

namespace {

constexpr int W = 1920, H = 1080;
constexpr float DPI = 1.25f;

const Province* provinceNamed(const World& w, const std::string& name) {
  const Province* r = nullptr;
  w.provinces.each([&](const Province& p) { if (p.name == name) r = &p; });
  return r;
}
const Faction* factionNamed(const World& w, const std::string& name) {
  const Faction* r = nullptr;
  w.factions.each([&](const Faction& f) { if (f.name == name) r = &f; });
  return r;
}

// Средняя и наибольшая разница каналов двух изображений в прямоугольнике.
struct Diff { double mean = 0; int max = 0; i64 over8 = 0; };
Diff diff(const gfx::Image& a, const gfx::Image& b, gfx::RectI r) {
  Diff d;
  i64 n = 0;
  double sum = 0;
  for (int y = r.y; y < r.bottom(); y++)
    for (int x = r.x; x < r.right(); x++) {
      const u32 p = a.at(x, y), q = b.at(x, y);
      int worst = 0;
      for (int s = 0; s < 24; s += 8) {
        const int e = std::abs(int((p >> s) & 255) - int((q >> s) & 255));
        sum += e;
        worst = std::max(worst, e);
      }
      d.max = std::max(d.max, worst);
      if (worst > 8) d.over8++;
      n += 3;
    }
  d.mean = n ? sum / double(n) : 0;
  return d;
}

gfx::Image loadPngImage(const std::string& path) {
  auto bytes = fs::readFile(path);
  if (!bytes) test::fail(__FILE__, __LINE__, "нет файла " + path);
  auto img = codec::decodePng(*bytes);
  if (!img) test::fail(__FILE__, __LINE__, "не PNG " + path);
  return gfx::Image::fromRgba(img->rgba.data(), img->w, img->h);
}

}  // namespace

// ================================================================ общий вид
TEST(map_view_fit_political) {
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(mvtest::demo());
  map::RenderOptions opt;
  const double t0 = nowSeconds();
  gfx::Image img = mvtest::renderFull(mv, opt, W, H, DPI);
  std::printf("  fit political: ready in %.0f ms\n", (nowSeconds() - t0) * 1000);
  CHECK(!mv.stats().fallback);
  CHECK(mv.stats().tilesDrawn > 0);
  mvtest::savePng(img, "map_view_fit_political.png");
  // Камера: вся карта видна с отступом.
  const Box2 vb = mv.view().visibleBox();
  CHECK(vb.x0 < 0 && vb.y0 < 0 && vb.x1 > 8000 && vb.y1 > 4500);
  CHECK_NEAR(mv.view().zoom, mv.minZoom(), 1e-12);
}

// ================================================================ масштабы: горы, реки, замки
TEST(map_view_zoom_levels) {
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(mvtest::demo());
  mv.setViewport(RectF(0, 0, W, H), DPI);
  map::RenderOptions opt;
  const struct { double zoom; Vec2 at; const char* name; } shots[] = {
      {0.8, {3950, 1180}, "map_view_zoom_1.png"},    // 1 пиксель устройства = 1 пиксель карты
      {1.6, {3830, 1260}, "map_view_zoom_2.png"},
      {3.0, {3800, 1300}, "map_view_zoom_3.png"},    // наибольший масштаб
  };
  for (const auto& s : shots) {
    mv.centerOn(s.at, s.zoom, false);
    CHECK_NEAR(mv.view().zoom, std::min(s.zoom, mv.maxZoom()), 1e-9);
    gfx::Image img = mvtest::renderFull(mv, opt, W, H, DPI);
    CHECK(!mv.stats().fallback);
    mvtest::savePng(img, s.name);
  }
  CHECK_NEAR(mv.maxZoom(), 3.0, 1e-12);
  // Подробности при наибольшем масштабе: стык двух государств (двухцветная граница) и берег (без светлого зазора).
  mv.centerOn({3651, 840}, 3.0, false);
  gfx::Image a = mvtest::renderFull(mv, opt, W, H, DPI);
  mvtest::saveCrop(a, gfx::RectI(1050, 525, 300, 300), 3, "map_view_detail_border.png");
  mv.centerOn({2938, 977}, 3.0, false);
  gfx::Image b = mvtest::renderFull(mv, opt, W, H, DPI);
  mvtest::saveCrop(b, gfx::RectI(1050, 525, 300, 300), 3, "map_view_detail_coast.png");
  // Режим данных при среднем масштабе: стыки соседних заливок разных цветов.
  map::RenderOptions data;
  data.mode = schema::MapMode::Contentment;
  mv.centerOn({3651, 840}, 1.2, false);
  gfx::Image c = mvtest::renderFull(mv, data, W, H, DPI);
  mvtest::saveCrop(c, gfx::RectI(1000, 475, 400, 400), 2, "map_view_detail_data.png");
}

// ================================================================ режимы карты
TEST(map_view_modes) {
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(mvtest::demo());
  mv.setViewport(RectF(0, 0, W, H), DPI);
  for (int m = 0; m < int(schema::MapMode::Count); m++) {
    map::RenderOptions opt;
    opt.mode = schema::MapMode(m);
    gfx::Image img = mvtest::renderFull(mv, opt, W, H, DPI);
    CHECK(!mv.stats().fallback);
    mvtest::savePng(img, std::string("map_view_mode_") + schema::kMapModes[m].id + ".png");
    const auto legend = mv.legend(mvtest::demo(), opt.mode);
    CHECK_MSG(legend.size() >= 2, schema::kMapModes[m].id);
    for (const auto& li : legend) CHECK(!li.label.empty());
  }
  // Режим гильдий крупнее: диаграммы, штабы, маршруты.
  map::RenderOptions opt;
  opt.mode = schema::MapMode::Guilds;
  mv.centerOn({4300, 2300}, 0.55, false);
  gfx::Image g = mvtest::renderFull(mv, opt, W, H, DPI);
  mvtest::savePng(g, "map_view_mode_guilds_zoom.png");
  // Столица Ольсты: диаграмма, штабы и знак столицы — крупно.
  const World& w = mvtest::demo();
  const Faction* olsta = nullptr;
  w.factions.each([&](const Faction& f) { if (f.name == "Вольные города Ольсты") olsta = &f; });
  CHECK(olsta && olsta->capital);
  const gfx::Pt cp = mv.view().toScreen(geo::faces(w)->shape(olsta->capital)->label);
  mvtest::saveCrop(g, gfx::RectI(int(cp.x * DPI) - 110, int(cp.y * DPI) - 90, 240, 170), 4, "map_view_detail_guilds.png");
}

// ================================================================ правка границ
TEST(map_view_edit_borders) {
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(mvtest::demo());
  mv.setViewport(RectF(0, 0, W, H), DPI);
  mv.centerOn({3700, 1100}, 0.62, false);
  map::RenderOptions opt;
  opt.editBorders = true;
  const World& w = mvtest::demo();
  if (const Province* p = provinceNamed(w, "Вальдра")) opt.hoverProvince = p->id;
  mvtest::savePng(mvtest::renderFull(mv, opt, W, H, DPI), "map_view_edit_borders.png");
}

// ================================================================ выделение и наведение
TEST(map_view_selection) {
  const World& w = mvtest::demo();
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(w);
  mv.setViewport(RectF(0, 0, W, H), DPI);
  mv.centerOn({3500, 1200}, 0.7, false);
  map::RenderOptions opt;
  const Province* sel = provinceNamed(w, "Сосновец");
  const Province* hov = provinceNamed(w, "Серая Гавань");
  CHECK(sel && hov);
  opt.selProvince = sel->id;
  opt.hoverProvince = hov->id;
  w.armies.each([&](const Army& a) { if (a.allied() && !a.isFleet()) opt.selArmy = a.id; });
  CHECK(opt.selArmy != 0);
  gfx::Image sp = mvtest::renderFull(mv, opt, W, H, DPI);
  mvtest::savePng(sp, "map_view_select_province.png");
  // Фигурка союзного войска со значком численности — крупно.
  const gfx::Pt ap = mv.view().toScreen(w.army(opt.selArmy)->pos);
  mvtest::saveCrop(sp, gfx::RectI(int(ap.x * DPI) - 90, int(ap.y * DPI) - 70, 220, 150), 4, "map_view_detail_army.png");

  map::RenderOptions o2;
  const Faction* kel = factionNamed(w, "Империя Валь-Кетра");
  CHECK(kel != nullptr);
  o2.selFaction = kel->id;
  mv.centerOn({4600, 900}, 0.42, false);
  mvtest::savePng(mvtest::renderFull(mv, o2, W, H, DPI), "map_view_select_faction.png");

  map::RenderOptions o3;
  o3.mode = schema::MapMode::Guilds;
  w.routes.each([&](const Route& r) { if (r.name == "Путь чародеев") o3.selRoute = r.id; });
  CHECK(o3.selRoute != 0);
  w.armies.each([&](const Army& a) { if (a.isFleet() && !o3.hoverArmy) o3.hoverArmy = a.id; });
  mv.centerOn({4300, 1900}, 0.5, false);
  mvtest::savePng(mvtest::renderFull(mv, o3, W, H, DPI), "map_view_select_route.png");
}

// ================================================================ мини-карта
TEST(map_view_minimap) {
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(mvtest::demo());
  mv.setViewport(RectF(0, 0, W, H), DPI);
  mv.centerOn({4300, 1500}, 0.8, false);
  const RectF rect(12, 12, 360, 202.5f);
  gfx::Image img(int(384 * DPI), int(227 * DPI), gfx::premul(Color::hex(0x151a22)));
  gfx::Canvas c(img);
  mv.renderMinimap(c, rect, DPI);
  mvtest::savePng(img, "map_view_minimap.png");
  const Vec2 a = mv.minimapToMap(rect, rect.x, rect.y), b = mv.minimapToMap(rect, rect.right(), rect.bottom());
  CHECK_NEAR(a.x, 0, 1e-9);
  CHECK_NEAR(a.y, 0, 1e-9);
  CHECK_NEAR(b.x, 8000, 1e-6);
  CHECK_NEAR(b.y, 4500, 1e-6);
  const Vec2 m = mv.minimapToMap(rect, rect.cx(), rect.cy());
  CHECK_NEAR(m.x, 4000, 1e-6);
  CHECK_NEAR(m.y, 2250, 1e-6);
}

// ================================================================ пустой мир = превью базовой карты
TEST(map_view_empty_matches_preview) {
  const gfx::Image pv = loadPngImage(mvtest::basemap().previewPath());
  CHECK_EQ(pv.w, 2000);
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(mvtest::emptyWorld());
  // Масштаб превью (уровень 2 пирамиды, 0,25 пикселя на единицу): совпадение до пикселя.
  mv.setViewport(RectF(0, 0, 2000, 1125), 1);
  mv.centerOn({4000, 2250}, 0.25, false);
  map::RenderOptions opt;
  gfx::Image img = mvtest::renderFull(mv, opt, 2000, 1125, 1);
  mvtest::savePng(img, "map_view_empty_l2.png");
  const Diff d = diff(img, pv, gfx::RectI(0, 0, 2000, 1125));
  std::printf("  empty vs preview at 0.25: mean %.4f, max %d\n", d.mean, d.max);
  CHECK(d.max <= 2);
  // Обычный масштаб «вся карта» 1920 × 1080 при dpi 1,25: превью, уменьшенное так же, почти не отличается.
  map::MapView fit(&mvtest::basemap());
  fit.setWorld(mvtest::emptyWorld());
  gfx::Image f = mvtest::renderFull(fit, opt, W, H, DPI);
  mvtest::savePng(f, "map_view_empty_fit.png");
  const map::View& v = fit.view();
  const double ds = v.zoom * DPI;
  const double X0 = std::round(v.viewport.cx() * DPI - v.cx * ds), Y0 = std::round(v.viewport.cy() * DPI - v.cy * ds);
  const gfx::RectI mr(int(X0) + 2, int(Y0) + 2, int(8000 * ds) - 4, int(4500 * ds) - 4);
  gfx::Image ref(f.w, f.h, 0xff000000u);
  gfx::Canvas rc(ref);
  rc.drawImage(pv, RectF(float(X0), float(Y0), float(8000 * ds), float(4500 * ds)));
  const Diff e = diff(f, ref, mr);
  std::printf("  empty vs preview at fit: mean %.3f, max %d, >8: %lld of %d px\n", e.mean, e.max, (long long)e.over8, mr.w * mr.h);
  CHECK(e.mean < 2.0);
}

// ================================================================ 1 пиксель устройства = 1 пиксель карты: исходный рисунок без искажений
TEST(map_view_empty_matches_source_l0) {
  const map::Basemap& bm = mvtest::basemap();
  map::MapView mv(&bm);
  mv.setWorld(mvtest::emptyWorld());
  mv.setViewport(RectF(0, 0, 1600, 900), 1);
  mv.centerOn({3000, 1000}, 1.0, false);
  map::RenderOptions opt;
  gfx::Image img = mvtest::renderFull(mv, opt, 1600, 900, 1);
  // Эталон: тайлы уровня 0 поверх белого, обычное наложение в числах с плавающей точкой.
  const int x0 = 2200, y0 = 550;
  std::vector<float> ref(size_t(1600) * 900 * 3, 255.f);
  for (const char* layer : {"ocean", "inland", "symbols"})
    for (int ty = y0 / 512; ty <= (y0 + 899) / 512; ty++)
      for (int tx = x0 / 512; tx <= (x0 + 1599) / 512; tx++) {
        if (!bm.tileExists(layer, 0, tx, ty)) continue;
        auto t = bm.readTile(layer, 0, tx, ty);
        CHECK(t.has_value());
        for (int y = 0; y < t->h; y++)
          for (int x = 0; x < t->w; x++) {
            const int X = tx * 512 + x - x0, Y = ty * 512 + y - y0;
            if (X < 0 || Y < 0 || X >= 1600 || Y >= 900) continue;
            const u8* p = t->rgba.data() + (size_t(y) * size_t(t->w) + size_t(x)) * 4;
            const float a = p[3] / 255.f;
            float* r = &ref[(size_t(Y) * 1600 + size_t(X)) * 3];
            for (int c = 0; c < 3; c++) r[c] = p[c] * a + r[c] * (1 - a);
          }
      }
  int worst = 0;
  i64 over1 = 0;
  for (int y = 0; y < 900; y++)
    for (int x = 0; x < 1600; x++) {
      const Color c = gfx::unpremul(img.at(x, y));
      const float* r = &ref[(size_t(y) * 1600 + size_t(x)) * 3];
      const int e = std::max({std::abs(int(c.r) - int(std::lround(r[0]))), std::abs(int(c.g) - int(std::lround(r[1]))),
                              std::abs(int(c.b) - int(std::lround(r[2])))});
      worst = std::max(worst, e);
      over1 += e > 1;
    }
  std::printf("  empty vs L0 source at 1:1: max %d, >1: %lld px\n", worst, (long long)over1);
  CHECK(worst <= 2);
  mvtest::savePng(img, "map_view_empty_l0.png");
}

// ================================================================ заливка не выходит за берег
TEST(map_view_coast_no_tint_leak) {
  // Пустой мир + одна большая провинция государства на материке (заливка 0,5 ярко-красным).
  World w = mvtest::emptyWorld();
  {
    Tx tx(w);
    const Id pid = geo::createProvince(tx, {{2300, 900}, {3400, 900}, {3400, 1900}, {2300, 1900}}, Terrain::Land);
    const Id st = rules::createFaction(tx, FactionKind::State, "Проба");
    tx.faction(st).color = Color(255, 0, 0);
    rules::setProvinceOwner(tx, pid, st);
    w = std::move(tx).finish();
  }
  const map::Basemap& bm = mvtest::basemap();
  map::RenderOptions opt;
  opt.labels = false;
  for (double zoom : {0.5, 1.4, 3.0}) {
    map::MapView a(&bm), b(&bm);
    a.setWorld(w);
    b.setWorld(mvtest::emptyWorld());
    a.setViewport(RectF(0, 0, 960, 540), 1.25f);
    b.setViewport(RectF(0, 0, 960, 540), 1.25f);
    a.centerOn({2935, 990}, zoom, false);
    b.centerOn({2935, 990}, zoom, false);
    gfx::Image ia = mvtest::renderFull(a, opt, 960, 540, 1.25f), ib = mvtest::renderFull(b, opt, 960, 540, 1.25f);
    int sea = 0, leaks = 0, land = 0, tinted = 0;
    const map::View& v = a.view();
    const double margin = std::max(3.0, 3.0 / (zoom * 1.25));
    auto coast = geo::faces(mvtest::emptyWorld());   // точный берег (граф), а не грубая маска
    for (int y = 0; y < ia.h; y += 2)
      for (int x = 0; x < ia.w; x += 2) {
        const Vec2 m = v.toMap((x + 0.5f) / 1.25f, (y + 0.5f) / 1.25f);
        bool ocean = true, solid = true;
        for (Vec2 d : {Vec2(0, 0), Vec2(margin, 0), Vec2(-margin, 0), Vec2(0, margin), Vec2(0, -margin)}) {
          ocean = ocean && coast->terrainAt(m + d) == Terrain::Sea;
          solid = solid && coast->terrainAt(m + d) == Terrain::Land;
        }
        const u32 pa = ia.at(x, y), pb = ib.at(x, y);
        if (ocean) {
          sea++;
          int e = 0;
          for (int s = 0; s < 24; s += 8) e = std::max(e, std::abs(int((pa >> s) & 255) - int((pb >> s) & 255)));
          if (e > 2) {
            // Отличие допустимо только рядом с сушей (мелкие острова провинции): проверка плотным кругом.
            bool nearLand = false;
            for (int k = 0; k < 24 && !nearLand; k++)
              for (double r : {margin * 0.33, margin * 0.66, margin}) {
                const double ang = k * kPi / 12;
                if (coast->terrainAt(m + Vec2(std::cos(ang), std::sin(ang)) * r) == Terrain::Land) nearLand = true;
              }
            if (!nearLand) {
              if (leaks < 6) std::printf("    leak at map (%.1f, %.1f) err %d: %08x vs %08x\n", m.x, m.y, e, pa, pb);
              leaks++;
            }
          }
        } else if (solid && m.x > 2330 && m.x < 3370 && m.y > 930 && m.y < 1870) {
          land++;
          tinted += pa != pb;
        }
      }
    std::printf("  zoom %.1f: sea samples %d, leaks %d; land samples %d, tinted %d\n", zoom, sea, leaks, land, tinted);
    CHECK(sea > 1000);
    CHECK_EQ(leaks, 0);
    CHECK(land > 1000);
    CHECK(tinted > land * 9 / 10);
    if (zoom == 3.0) mvtest::savePng(ia, "map_view_coast_tint.png");
  }
}

// ================================================================ без базовой карты
TEST(map_view_no_basemap) {
  map::MapView mv(nullptr);
  mv.setWorld(mvtest::demo());
  map::RenderOptions opt;
  gfx::Image img = mvtest::renderFull(mv, opt, 960, 540, 1);
  mvtest::savePng(img, "map_view_no_basemap.png");
  const map::View& v = mv.view();
  // Открытое море (угол карты) — цвет моря, середина неназначенного острова на западе — белая суша.
  const gfx::Pt sea = v.toScreen({60, 60});
  const Color cs = gfx::unpremul(img.at(int(sea.x), int(sea.y)));
  CHECK(cs.b > 200 && cs.r < 40);
  const gfx::Pt land = v.toScreen({1000, 500});
  const Color cl = gfx::unpremul(img.at(int(land.x), int(land.y)));
  CHECK(cl.r > 200 && cl.g > 200 && cl.b > 200);
  gfx::Image mini(200, 120, 0xff000000u);
  gfx::Canvas c(mini);
  mv.renderMinimap(c, RectF(0, 0, 200, 112.5f), 1);
  // Мир ещё не задан: только базовая карта, без ошибок.
  map::MapView fresh(&mvtest::basemap());
  gfx::Image f = mvtest::renderFull(fresh, opt, 640, 360, 1);
  CHECK(!fresh.stats().fallback);
  CHECK_EQ(fresh.provinceAt(320, 180), Id(0));
  CHECK_EQ(fresh.armyAt(320, 180), Id(0));
  CHECK(fresh.legend(World(), schema::MapMode::Political).size() >= 1);
}

// ================================================================ камера
TEST(map_view_camera) {
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(mvtest::demo());
  mv.setViewport(RectF(0, 0, W, H), DPI);
  const double z0 = mv.view().zoom;
  CHECK_NEAR(z0, mv.minZoom(), 1e-12);
  // Колесо без анимации: точка под курсором неподвижна.
  const Vec2 before = mv.view().toMap(700, 400);
  mv.zoomAt(700, 400, 2.0, false);
  const Vec2 after = mv.view().toMap(700, 400);
  CHECK_NEAR(before.x, after.x, 1e-6);
  CHECK_NEAR(before.y, after.y, 1e-6);
  CHECK_NEAR(mv.view().zoom, z0 * 2, 1e-12);
  // С анимацией: в середине и в конце точка под курсором та же, длительность около 180 мс.
  const Vec2 p = mv.view().toMap(1200, 600);
  mv.zoomAt(1200, 600, 1.5, true);
  CHECK(mv.animating());
  mv.update(10.0);   // начало отсчёта
  mv.update(10.09);
  CHECK(mv.animating());
  const Vec2 mid = mv.view().toMap(1200, 600);
  CHECK_NEAR(mid.x, p.x, 1e-6);
  CHECK_NEAR(mid.y, p.y, 1e-6);
  CHECK(mv.view().zoom > z0 * 2 && mv.view().zoom < z0 * 3);
  mv.update(10.19);
  CHECK(!mv.animating());
  CHECK_NEAR(mv.view().zoom, z0 * 3, 1e-9);
  const Vec2 end = mv.view().toMap(1200, 600);
  CHECK_NEAR(end.x, p.x, 1e-6);
  // Серия щелчков колеса во время анимации: цель накапливается, точка под курсором неподвижна.
  const double zb = mv.view().zoom;
  const Vec2 q = mv.view().toMap(500, 300);
  mv.zoomAt(500, 300, 1.25, true);
  mv.update(11.0);
  mv.update(11.05);
  mv.zoomAt(500, 300, 1.25, true);
  mv.update(11.06);
  for (double t = 11.07; t < 11.5; t += 0.016) mv.update(t);
  CHECK(!mv.animating());
  CHECK_NEAR(mv.view().zoom, zb * 1.25 * 1.25, 1e-9);
  CHECK_NEAR(mv.view().toMap(500, 300).x, q.x, 1e-6);
  CHECK_NEAR(mv.view().toMap(500, 300).y, q.y, 1e-6);
  // Пределы.
  mv.zoomAt(960, 540, 1000, false);
  CHECK_NEAR(mv.view().zoom, mv.maxZoom(), 1e-12);
  mv.zoomAt(960, 540, 1e-6, false);
  CHECK_NEAR(mv.view().zoom, mv.minZoom(), 1e-12);
  // Панорама и ограничение: край карты не уходит дальше 40 % области просмотра.
  mv.centerOn({4000, 2250}, 1.0, false);
  mv.panBy(100, -50);
  CHECK_NEAR(mv.view().cx, 3900, 1e-9);
  CHECK_NEAR(mv.view().cy, 2300, 1e-9);
  mv.panBy(1e6, 1e6);
  const gfx::Pt corner = mv.view().toScreen({0, 0});
  CHECK(corner.x <= W * 0.4f + 0.01f && corner.y <= H * 0.4f + 0.01f);
  // Показать область.
  mv.fit(Box2(3000, 1000, 3400, 1300), false);
  const Box2 vb = mv.view().visibleBox();
  CHECK(vb.x0 <= 3000 && vb.x1 >= 3400 && vb.y0 <= 1000 && vb.y1 >= 1300);
  CHECK(vb.w() < 1000);
  mv.fitAll(true);
  mv.update(20);
  mv.update(20.3);
  CHECK(!mv.animating());
  CHECK_NEAR(mv.view().zoom, mv.minZoom(), 1e-12);
  CHECK_NEAR(mv.view().cx, 4000, 1e-9);
  // Фигурка войска растёт с масштабом в разумных пределах.
  const float small = mv.figureSize();
  mv.centerOn({4000, 2250}, 2.5, false);
  CHECK(mv.figureSize() > small);
  CHECK(mv.figureSize() <= 64);
}

// ================================================================ попадание
TEST(map_view_hit_testing) {
  const World& w = mvtest::demo();
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(w);
  mv.setViewport(RectF(0, 0, W, H), DPI);
  mv.centerOn({4300, 1700}, 0.6, false);
  auto fs = geo::faces(w);
  int checked = 0;
  w.provinces.each([&](const Province& p) {
    const geo::ProvinceShape* sh = fs->shape(p.id);
    if (!sh) return;
    const gfx::Pt s = mv.view().toScreen(sh->label);
    if (s.x < 0 || s.y < 0 || s.x >= W || s.y >= H) return;
    CHECK_EQ(mv.provinceAt(s.x, s.y), p.id);
    checked++;
  });
  CHECK(checked > 5);
  w.armies.each([&](const Army& a) {
    const gfx::Pt s = mv.view().toScreen(a.pos);
    if (s.x < 0 || s.y < 0 || s.x >= W || s.y >= H) return;
    CHECK_EQ(mv.armyAt(s.x, s.y), a.id);
    CHECK_EQ(mv.armyAt(s.x + mv.figureSize() * 2, s.y + mv.figureSize() * 2) == a.id, false);
  });
  int routes = 0;
  w.routes.each([&](const Route& r) {
    const gfx::Pt s = mv.view().toScreen(r.pts[1]);
    if (s.x < 0 || s.y < 0 || s.x >= W || s.y >= H) return;
    CHECK_EQ(mv.routeAt(s.x + 2, s.y + 1), r.id);
    routes++;
  });
  CHECK(routes > 0);
  CHECK_EQ(mv.routeAt(-500, -500), Id(0));
  CHECK_EQ(mv.provinceAt(-10000, -10000), Id(0));
}

// ================================================================ точечная инвалидация
TEST(map_view_invalidation) {
  const World& w0 = mvtest::demo();
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(w0);
  map::RenderOptions opt;
  gfx::Image a = mvtest::renderFull(mv, opt, W, H, DPI);
  // Сменить владельца провинции.
  const Province* p = provinceNamed(w0, "Тальвин");
  const Faction* other = factionNamed(w0, "Республика Корвен");
  CHECK(p && other);
  Tx tx(w0);
  rules::setProvinceOwner(tx, p->id, other->id);
  const World w1 = std::move(tx).finish();
  mv.worldChanged(w0, w1, World::diff(w0, w1));
  // Сразу после изменения — прежние тайлы видны (без «дыр»), часть устарела.
  gfx::Image mid(a.w, a.h, 0xff000000u);
  {
    gfx::Canvas c(mid);
    mv.render(c, opt);
  }
  CHECK(!mv.stats().fallback);
  gfx::Image b = mvtest::renderFull(mv, opt, W, H, DPI);
  auto fs = geo::faces(w1);
  const Box2 pb = fs->shape(p->id)->box;
  const map::View& v = mv.view();
  const gfx::Pt q0 = v.toScreen({pb.x0, pb.y0}), q1 = v.toScreen({pb.x1, pb.y1});
  const gfx::RectI changed(int(q0.x * DPI) - 40, int(q0.y * DPI) - 40, int((q1.x - q0.x) * DPI) + 80, int((q1.y - q0.y) * DPI) + 80);
  // Внутри изменённой области картинка другая, вдали — та же до пикселя.
  CHECK(diff(a, b, changed).mean > 1.0);
  const gfx::RectI far(int(1500 * DPI), int(700 * DPI), int(300 * DPI), int(300 * DPI));
  CHECK_EQ(diff(a, b, far).max, 0);
  mvtest::savePng(b, "map_view_invalidation.png");
}

// ================================================================ производительность
TEST(map_view_perf) {
  // Новая версия мира: построение тайлов «с нуля» (грани и расчёт — в фоне).
  Tx tx(mvtest::demo());
  tx.meta().name = "Проверка скорости";
  const World w = std::move(tx).finish();
  map::MapView mv(&mvtest::basemap());
  mv.setWorld(w);
  mv.setViewport(RectF(0, 0, W, H), DPI);
  map::RenderOptions opt;
  gfx::Image img(int(W * DPI), int(H * DPI), 0xff000000u);
  const double t0 = nowSeconds();
  double firstFrame = 0;
  {
    gfx::Canvas c(img);
    mv.render(c, opt);
    firstFrame = (nowSeconds() - t0) * 1000;
    std::printf("  first frame: tiles %.1f ms, labels %.1f ms\n", mv.stats().composeMs, mv.stats().labelsMs);
  }
  CHECK(mv.waitIdle(10));
  const double ready = (nowSeconds() - t0) * 1000;
  // Кадры с готовыми тайлами.
  double sum = 0, worst = 0;
  const int N = 30;
  for (int i = 0; i < N; i++) {
    gfx::Canvas c(img);
    const double s = nowSeconds();
    mv.render(c, opt);
    const double ms = (nowSeconds() - s) * 1000;
    sum += ms;
    worst = std::max(worst, ms);
  }
  CHECK(!mv.stats().fallback);
  // Панорама: кадры не ждут фоновой отрисовки.
  double panWorst = 0;
  for (int i = 0; i < 40; i++) {
    mv.panBy(-23, 7);
    gfx::Canvas c(img);
    const double s = nowSeconds();
    mv.render(c, opt);
    panWorst = std::max(panWorst, (nowSeconds() - s) * 1000);
  }
  // Анимация масштаба: запасные изображения масштабируются полосами в пуле.
  mv.waitIdle(10);
  double animWorst = 0, animCompose = 0;
  mv.zoomAt(900, 500, 1.6, true);
  double t = 100;
  mv.update(t);
  while (mv.animating()) {
    t += 1.0 / 60;
    mv.update(t);
    gfx::Canvas c(img);
    const double s = nowSeconds();
    mv.render(c, opt);
    animWorst = std::max(animWorst, (nowSeconds() - s) * 1000);
    animCompose = std::max(animCompose, mv.stats().composeMs);
  }
  // «Холодный» мир: изменённая геометрия — грани и индекс строятся заново (в фоне и для наложений).
  double coldFirst = 0, coldReady = 0, facesMs = 0;
  {
    Tx t2(mvtest::demo());
    geo::Handle h;
    t2.w().edges.each([&](const Edge& e) {
      if (!h && e.kind == EdgeKind::Border && !e.pts.empty()) {
        h.kind = geo::Handle::Point;
        h.edge = e.id;
        h.index = 0;
      }
    });
    geo::moveHandle(t2, h, geo::handlePos(t2.w(), h) + Vec2(0.5, 0.5));
    const World cold = std::move(t2).finish();
    const double f0 = nowSeconds();
    geo::buildFaces(cold);
    facesMs = (nowSeconds() - f0) * 1000;
    map::MapView mc(&mvtest::basemap());
    mc.setWorld(cold);
    mc.setViewport(RectF(0, 0, W, H), DPI);
    const double c0 = nowSeconds();
    {
      gfx::Canvas c(img);
      mc.render(c, opt);
    }
    coldFirst = (nowSeconds() - c0) * 1000;
    CHECK(mc.waitIdle(10));
    coldReady = (nowSeconds() - c0) * 1000;
  }
  std::printf("  cold world: buildFaces %.1f ms, first frame %.1f ms, tiles ready %.0f ms\n", facesMs, coldFirst, coldReady);
  std::printf("  perf 1920x1080@1.25: first frame %.1f ms, all tiles ready %.0f ms, cached frame avg %.2f ms (max %.2f), "
              "pan max %.1f ms, zoom animation max %.1f ms (tiles %.1f ms)\n",
              firstFrame, ready, sum / N, worst, panWorst, animWorst, animCompose);
#ifdef NDEBUG
  CHECK(ready < rg::test::perf(300));
  CHECK(sum / N < rg::test::perf(12));
  CHECK(panWorst < rg::test::perf(40));
  CHECK(animWorst < rg::test::perf(40));
#endif
}
