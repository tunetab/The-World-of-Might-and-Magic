// Аудит ТЗ 1.a (провинции и карта), автор tz-provinces: воспроизведения найденных дефектов.
// Проверки ожидаемого поведения, которые сейчас не выполняются, включаются переменной REGNUM_AUDIT=1
// (по умолчанию только печатаются, чтобы не ронять общий прогон). Исправляющие превратят их в регрессии.
#include "tests/test_app_tools_util.h"

using namespace rg;
using namespace rg::toolstest;

namespace {

[[maybe_unused]] bool auditStrict() {
  const char* v = std::getenv("REGNUM_AUDIT");
  return v && *v && *v != '0';
}

// Ожидаемое поведение: при REGNUM_AUDIT=1 — настоящая проверка, иначе — запись в журнал.
#define AUDIT_EXPECT(cond, msg)                                                         \
  do {                                                                                  \
    bool _ok = (cond);                                                                  \
    std::printf("  [audit %s] %s: %s\n", _ok ? "ok" : "DEFECT", #cond, std::string(msg).c_str()); \
    if (!_ok && auditStrict()) ::rg::test::fail(__FILE__, __LINE__, std::string("AUDIT: ") + (msg)); \
  } while (0)

// Морская провинция с наибольшей площадью.
Id largestSea(const World& w) {
  auto fs = geo::faces(w);
  Id best = 0;
  double ba = 0;
  w.provinces.each([&](const Province& p) {
    if (!p.sea) return;
    const geo::ProvinceShape* s = fs->shape(p.id);
    if (s && s->area > ba) {
      ba = s->area;
      best = p.id;
    }
  });
  return best;
}

Id firstState(const World& w, Id skip = 0) {
  Id best = 0;
  std::string bn;
  w.factions.each([&](const Faction& f) {
    if (!f.isState() || f.id == skip) return;
    if (!best || compareRu(f.name, bn) < 0) {
      best = f.id;
      bn = f.name;
    }
  });
  return best;
}

}  // namespace

// ---------------------------------------------------------------- базовая карта: снимки пустого мира на разных масштабах
TEST(audit_tzprov_basemap_zoom_shots) {
  ToolGuard guard;
  Harness h("audit_tzprov_basemap", 1600, 900, 1);
  CHECK(h->newWorld("Пустой мир"));
  h.settle();
  h.dropToasts();
  // Без панелей поверх карты: мини-карта и легенда скрыты.
  h->ui.showMinimap = false;
  h->ui.showLegend = false;
  struct Z {
    const char* name;
    double zoom;
  } zs[] = {{"fit", 0}, {"z050", 0.5}, {"z100", 1.0}, {"z300", 3.0}};
  for (auto& z : zs) {
    if (z.zoom == 0) h->map().fitAll(false);
    else h->map().centerOn({3000, 1000}, z.zoom, false);
    h.settle();
    h.waitMap();
    h.dropToasts();
    h.frames(3);
    const map::View& v = h->map().view();
    RectF ma = h->mapArea();
    std::printf("  shot %s: zoom %.4f center %.2f %.2f viewport %.1f %.1f %.1f %.1f mapArea %.1f %.1f %.1f %.1f\n", z.name, v.zoom, v.cx, v.cy,
                v.viewport.x, v.viewport.y, v.viewport.w, v.viewport.h, ma.x, ma.y, ma.w, ma.h);
    CHECK(h.shot(std::string("audit_tzprov_basemap_") + z.name));
  }
}

// ---------------------------------------------------------------- ТЗ 1.a.vii: «сухопутная» провинция на море не закрашивается
TEST(audit_tzprov_sea_area_land_province_unfilled) {
  ToolGuard guard;
  Harness h("audit_tzprov_sealand", 1440, 900);
  h.demo();
  Id pid = largestSea(h->world());
  CHECK(pid != 0);
  h->select(app::SelType::Province, pid);
  h.settle();
  CHECK(h->uiRect("province.sea") != nullptr);
  CHECK(h.clickUi("province.sea"));   // снять галочку «Морская провинция»
  h.settle();
  CHECK(!h->world().province(pid)->sea);
  Id st = firstState(h->world());
  CHECK(h->act("Владелец", [&](Tx& tx) { rules::setProvinceOwner(tx, pid, st); }));
  CHECK_EQ(h->world().province(pid)->owner, st);
  auto fs = geo::faces(h->world());
  const geo::ProvinceShape* sh = fs->shape(pid);
  CHECK(sh != nullptr);
  h->focusMap(sh->box);
  h.settle();
  h->clearSelection();   // без подсветки выделения — видна только заливка
  h.settle();
  h.waitMap();
  h.dropToasts();
  h.frames(3);
  CHECK(h.shot("audit_tzprov_sea_area_land_owner"));
  // Точки внутри провинции: должны быть подкрашены цветом государства (ТЗ 1.a.vii), а не чистым морем.
  int pure = 0, n = 0;
  for (int k = 0; k < 64; k++) {
    Vec2 m{sh->box.x0 + sh->box.w() * ((k % 8) + 0.5) / 8, sh->box.y0 + sh->box.h() * ((k / 8) + 0.5) / 8};
    if (fs->provinceAt(m) != pid) continue;
    gfx::Pt s = h->map().view().toScreen(m);
    if (!freeAt(h, s)) continue;
    u32 px = h.pixel(s.x, s.y);
    int r = int((px >> 16) & 255), g = int((px >> 8) & 255), b = int(px & 255);
    n++;
    if (r < 14 && std::abs(g - 38) < 14 && b > 240) pure++;
  }
  Color c = h->world().faction(st)->color;
  std::printf("  samples %d, pure sea %d, state color %02x%02x%02x\n", n, pure, c.r, c.g, c.b);
  CHECK(n > 5);
  // Исправлено (карта): морские грани сухопутной провинции закрашиваются поверх моря — регрессия.
  CHECK_MSG(pure * 2 < n, "сухопутная (без галочки «Морская») провинция с владельцем на морской части карты не закрашена цветом государства");
}

// ---------------------------------------------------------------- «Показать всю карту» (Home) прячет края карты под панелями
TEST(audit_tzprov_fit_hides_map_edges) {
  ToolGuard guard;
  Harness h("audit_tzprov_fit", 1920, 1080, 1);
  CHECK(h->newWorld("Пустой мир"));
  h.settle();
  h.dropToasts();
  h.key(Key::Home);
  h.settle();
  h.waitMap();
  h.dropToasts();
  h.frames(3);
  CHECK(h.shot("audit_tzprov_fit_1920"));
  const map::View& v = h->map().view();
  gfx::Pt tl = v.toScreen({0, 0}), br = v.toScreen({8000, 4500});
  const RectF* top = h->uiRect("topbar");
  const RectF* rail = h->uiRect("rail");
  const RectF* tb = h->uiRect("toolbar");
  RectF area = h->mapArea();
  std::printf("  map on screen %.1f,%.1f — %.1f,%.1f; mapArea %.1f,%.1f %.1fx%.1f; topbar bottom %.1f; rail right %.1f; toolbar right %.1f\n", tl.x, tl.y,
              br.x, br.y, area.x, area.y, area.w, area.h, top ? top->bottom() : -1.f, rail ? rail->right() : -1.f, tb ? tb->right() : -1.f);
  // Северный остров базовой карты (≈ 5300…5800 × 20…260) после «Показать всю карту» должен быть виден.
  gfx::Pt isl = v.toScreen({5560, 120});
  bool hidden = top && top->contains(isl.x, isl.y);
  std::printf("  north island centre on screen %.1f,%.1f hidden by top bar: %d; min zoom %.4f (whole map in mapArea needs %.4f)\n", isl.x, isl.y, int(hidden),
              h->map().minZoom(), std::min(area.w / 8000.0, area.h / 4500.0));
  // Исправлено (карта): мир вписывается в свободную часть между панелями — регрессия.
  CHECK_MSG(!hidden && tl.y >= area.y - 1 && tl.x >= area.x - 1,
            "«Показать всю карту» вписывает карту в окно целиком, а не в свободную область: верх и левый край мира под верхней панелью и лентами");
  CHECK(br.x <= area.right() + 1 && br.y <= area.bottom() + 1);
  CHECK(h->map().minZoom() <= std::min(area.w / 8000.0, area.h / 4500.0) + 1e-9);
}

// ---------------------------------------------------------------- щелчок по торговому маршруту поверх провинции
TEST(audit_tzprov_route_click_over_province) {
  ToolGuard guard;
  Harness h("audit_tzprov_route", 1440, 900);
  h.demo();
  const World& w = h->world();
  CHECK(!w.routes.empty());
  auto fs = geo::faces(w);
  // Вершина маршрута над сухопутной провинцией, не под фигуркой войска.
  Id rid = 0;
  Vec2 at;
  gfx::Pt s;
  w.routes.each([&](const Route& r) {
    // Середины звеньев (вдали от центров провинций с диаграммами гильдий).
    for (size_t i = 1; i < r.pts.size() && !rid; i++) {
      Vec2 m = r.pts[i - 1] + (r.pts[i] - r.pts[i - 1]) * 0.4;
      const Province* p = w.province(fs->provinceAt(m));
      if (!p || p->sea) continue;
      h->map().centerOn(m, 0.8, false);
      h.frames(2);
      gfx::Pt q0 = h->map().view().toScreen(m);
      for (int dy = -20; dy <= 20 && !rid; dy++)
        for (int dx = -20; dx <= 20 && !rid; dx++) {
          gfx::Pt q{q0.x + float(dx), q0.y + float(dy)};
          if (h->map().armyAt(q.x, q.y) || h->map().routeAt(q.x, q.y, 2) != r.id || !freeAt(h, q)) continue;
          const Province* pq = w.province(h->map().provinceAt(q.x, q.y));
          if (!pq || pq->sea) continue;
          rid = r.id;
          at = h->map().view().toMap(q.x, q.y);
          s = q;
        }
    }
  });
  CHECK(rid != 0);
  h.settle();
  h.waitMap();
  s = h->map().view().toScreen(at);
  CHECK(h->map().routeAt(s.x, s.y) == rid);
  CHECK(h->map().armyAt(s.x, s.y) == 0);
  h.click(s.x, s.y);
  h.settle();
  std::printf("  click on route %lld at %.0f,%.0f -> selection type %d id %lld\n", (long long)rid, s.x, s.y, int(h->ui.sel.type), (long long)h->ui.sel.id);
  // Политическая карта: маршруты не нарисованы — щелчок выбирает провинцию под (невидимой) линией.
  CHECK(h->ui.mapMode == schema::MapMode::Political);
  CHECK(h->ui.sel.type == app::SelType::Province);
  // Регрессия: режим гильдий (маршруты на виду) — щелчок точно по линии выбирает маршрут, а не провинцию.
  h->clearSelection();
  h.key(Key::D2);
  h.settle();
  std::printf("  map mode %d\n", int(h->ui.mapMode));
  CHECK(h->ui.mapMode == schema::MapMode::Guilds);
  h.click(s.x, s.y);
  h.settle();
  std::printf("  guild mode click -> selection type %d\n", int(h->ui.sel.type));
  CHECK_MSG(h->ui.sel == (app::Selection{app::SelType::Route, rid}),
            "щелчок точно по линии торгового маршрута над провинцией выбирает провинцию, маршрут не выбрать");
  CHECK(cleanShot(h, "audit_tzprov_route_click"));
  // Выбранный маршрут виден и на политической карте — повторный щелчок по линии оставляет его выбранным.
  h.key(Key::D1);
  h.settle();
  h.frames(30);   // не двойной щелчок
  h.click(s.x, s.y);
  h.settle();
  CHECK(h->ui.sel == (app::Selection{app::SelType::Route, rid}));
}

// ---------------------------------------------------------------- ТЗ 1.a.ii: «галочка» правки и инструмент «Правка границ» — один и тот же значок
TEST(audit_tzprov_edit_toggle_icon_shot) {
  ToolGuard guard;
  Harness h("audit_tzprov_toggle", 1440, 900);
  h.demo();
  h.key(Key::E);
  h.settle();
  CHECK(h->ui.editBorders);
  CHECK(h->ui.tool == app::ToolId::EditBorders);
  const app::ToolDef* bt = app::findTool(app::ToolId::EditBorders);
  CHECK(bt != nullptr);
  std::printf("  borders tool icon «%s», toggle icon when on «%s»\n", bt->icon, app::detail::bordersToggleIcon(true));
  const RectF* a = h->uiRect("tool.borders");
  const RectF* b = h->uiRect("tool.borders-tool");
  CHECK(a && b);
  CHECK(cleanShot(h, "audit_tzprov_edit_toggle"));
  // Регрессия: у «галочки» режима правки свой значок (замок), отличный от инструмента и в обоих состояниях.
  CHECK_MSG(std::string(bt->icon) != app::detail::bordersToggleIcon(true),
            "включённая «галочка» правки границ и соседний инструмент «Правка границ провинции» показаны одинаковым значком");
  CHECK(std::string(app::detail::bordersToggleIcon(true)) != app::detail::bordersToggleIcon(false));
}

// ---------------------------------------------------------------- ТЗ 1.a.iii: столичная провинция становится морской
TEST(audit_tzprov_sea_toggle_capital_and_status) {
  ToolGuard guard;
  Harness h("audit_tzprov_seacap", 1440, 900);
  h.demo();
  const World& w = h->world();
  Id st = 0, cap = 0;
  w.factions.each([&](const Faction& f) {
    if (!st && f.isState() && f.capital && w.province(f.capital)) {
      st = f.id;
      cap = f.capital;
    }
  });
  CHECK(st != 0);
  h->select(app::SelType::Province, cap);
  h.settle();
  CHECK(h.clickUi("province.sea"));
  h.settle();
  CHECK(h->world().province(cap)->sea);
  Id capNow = h->world().faction(st)->capital;
  bool occupiedLeft = h->world().province(cap)->occupied;
  std::printf("  capital of state after sea toggle: %lld (sea province %lld); occupied flag kept %d; owner kept %lld\n", (long long)capNow, (long long)cap,
              int(occupiedLeft), (long long)h->world().province(cap)->owner);
  CHECK_MSG(capNow != cap, "морская провинция осталась столицей государства (setCapital запрещает морскую столицу)");
  CHECK_MSG(!occupiedLeft, "у морской провинции осталась оккупация");
  // Сведения о государстве: вкладка «Обзор» государства показывает столицу.
  h->select(app::SelType::Faction, st);
  h.settle();
  h.waitMap();
  h.dropToasts();
  CHECK(h.shot("audit_tzprov_sea_capital_faction"));
}

// ---------------------------------------------------------------- ТЗ 1.a.ii: выключенная правка — Delete удаляет провинцию
TEST(audit_tzprov_view_mode_delete_key) {
  ToolGuard guard;
  Harness h("audit_tzprov_viewdel", 1440, 900);
  h.demo();
  CHECK(!h->ui.editBorders);
  auto vp = h.visibleProvince();
  CHECK(vp.has_value());
  h.click(vp->second.x, vp->second.y);
  h.settle();
  CHECK(h->ui.sel == (app::Selection{app::SelType::Province, vp->first}));
  size_t n0 = h->world().provinces.size();
  h.key(Key::Delete);
  h.settle();
  bool dlg = h->hasDialog();
  std::printf("  dialog after Delete in view mode: %d\n", int(dlg));
  if (dlg) {
    CHECK(h.shot("audit_tzprov_view_mode_delete"));
    h.key(Key::Enter);
    h.settle();
  }
  std::printf("  provinces %zu -> %zu\n", n0, size_t(h->world().provinces.size()));
  // Регрессия (ТЗ 1.a.ii): вне режима правки Delete не удаляет провинцию и не спрашивает об этом.
  CHECK(!dlg);
  CHECK_MSG(h->world().provinces.size() == n0, "при выключенной «Правке границ» клавиша Delete удаляет провинцию с карты");
  // В режиме правки — подтверждение удаления.
  h->setEditBorders(true);
  h.settle();
  h->select(app::SelType::Province, vp->first);
  h.settle();
  h.move(4, 4);
  h.key(Key::Delete);
  h.settle();
  CHECK(h->hasDialog("confirm"));
}

// ---------------------------------------------------------------- ТЗ 1.a.i/iv: новая провинция поглощает соседнюю целиком
TEST(audit_tzprov_ghost_province_after_cover) {
  ToolGuard guard;
  Harness h("audit_tzprov_ghost", 1440, 900);
  h.demo();
  const World& w0 = h->world();
  auto fs = geo::faces(w0);
  // Небольшая провинция с владельцем, рамка которой (с запасом) целиком на суше.
  Id victim = 0;
  Box2 vb;
  double best = 1e18;
  w0.provinces.each([&](const Province& p) {
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
      vb = b;
    }
  });
  CHECK(victim != 0);
  Id owner = w0.province(victim)->owner;
  std::printf("  victim %lld «%s» area %.0f owner %lld\n", (long long)victim, w0.province(victim)->name.c_str(), best, (long long)owner);
  focus(h, vb.inflated(40));
  h.key(Key::E);
  CHECK(h->ui.editBorders);
  h.key(Key::P);
  CHECK(h->ui.tool == app::ToolId::NewProvince);
  clickMap(h, {vb.x0, vb.y0});
  clickMap(h, {vb.x1, vb.y0});
  clickMap(h, {vb.x1, vb.y1});
  clickMap(h, {vb.x0, vb.y1});
  h.key(Key::Enter);
  h.settle();
  const World& w1 = h->world();
  auto fs1 = geo::faces(w1);
  bool recordLeft = w1.province(victim) != nullptr;
  bool hasShape = fs1->shape(victim) != nullptr;
  auto calc = rules::calc(w1);
  bool counted = false;
  if (const rules::FactionCalc* fc = calc->faction(owner))
    counted = std::find(fc->provinces.begin(), fc->provinces.end(), victim) != fc->provinces.end();
  std::printf("  after cover: record %d shape %d counted for owner %d\n", int(recordLeft), int(hasShape), int(counted));
  CHECK(cleanShot(h, "audit_tzprov_ghost_cover"));
  CHECK_MSG(!(recordLeft && !hasShape), "провинция, целиком поглощённая новой, осталась в базе без области на карте (призрак)");
  CHECK_MSG(!(recordLeft && !hasShape && counted), "призрачная провинция без области продолжает числиться за государством и давать доход");
}

// ---------------------------------------------------------------- ТЗ 1.a.ix: штриховка оккупации поверх рек и озёр
TEST(audit_tzprov_hatch_over_water_shot) {
  ToolGuard guard;
  Harness h("audit_tzprov_hatch", 1440, 900, 1);
  h.demo();
  h->ui.showMinimap = false;
  h->ui.showLegend = false;
  const World& w = h->world();
  Id occ = 0;
  w.provinces.each([&](const Province& p) {
    if (!occ && p.occupied && p.name == "Фьорн") occ = p.id;
  });
  if (!occ)
    w.provinces.each([&](const Province& p) {
      if (!occ && p.occupied) occ = p.id;
    });
  CHECK(occ != 0);
  auto fs = geo::faces(w);
  const geo::ProvinceShape* sh = fs->shape(occ);
  CHECK(sh != nullptr);
  h->map().centerOn(sh->label, 1.0, false);
  h.settle();
  h.waitMap();
  h.dropToasts();
  h.frames(3);
  const map::View& v = h->map().view();
  std::printf("  hatch shot: zoom %.3f center %.1f %.1f viewport %.1f %.1f %.1f %.1f\n", v.zoom, v.cx, v.cy, v.viewport.x, v.viewport.y, v.viewport.w,
              v.viewport.h);
  CHECK(h.shot("audit_tzprov_hatch_1to1"));
}
