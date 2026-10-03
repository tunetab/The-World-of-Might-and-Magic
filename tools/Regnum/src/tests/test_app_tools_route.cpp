// Сценарии торговых маршрутов (ТЗ 1.d.v): построение щелчками с выбором гильдии, +10 % базовой торговой ценности
// каждой провинции на пути, правка точек (перетаскивание, вставка, удаление), инспектор маршрута (название,
// провинции на пути), удаление через меню и инспектор с подтверждением, отмена.
#include "tests/test_app_tools_util.h"

using namespace rg;
using namespace rg::toolstest;

namespace {

double tradeValue(const World& w, Id p) {
  auto c = rules::calc(w);
  const rules::ProvinceCalc* pc = c->province(p);
  return pc ? pc->tradeValue : 0;
}

// Цепочка из трёх соседних сухопутных провинций с владельцами и заметной базовой ценностью.
std::vector<Id> chain(const World& w) {
  auto fs = geo::faces(w);
  auto nb = fs->neighbors();
  // Точка подписи далеко от существующих маршрутов (щелчок не попадёт в чужую линию).
  auto clear = [&](Vec2 q) {
    bool far = true;
    w.routes.each([&](const Route& r) {
      for (size_t i = 1; i < r.pts.size(); i++)
        if (geo::distToSeg(q, r.pts[i - 1], r.pts[i]) < 70) far = false;
    });
    return far;
  };
  auto ok = [&](Id p) {
    const Province* pr = w.province(p);
    return pr && !pr->sea && pr->owner && pr->baseTrade > 1 && fs->shape(p) && clear(fs->shape(p)->label);
  };
  for (auto [a, b] : nb) {
    if (!ok(a) || !ok(b)) continue;
    for (auto [c, d] : nb) {
      Id third = c == b ? d : d == b ? c : 0;
      if (!third || third == a || !ok(third)) continue;
      return {a, b, third};
    }
  }
  return {};
}

}  // namespace

TEST(app_tools_route_create_edit_delete) {
  ToolGuard guard;
  Harness h("tools_route");
  h.demo();
  const World w0 = h->world();
  std::vector<Id> ch = chain(w0);
  CHECK_EQ(ch.size(), size_t(3));
  auto fs = geo::faces(w0);
  std::vector<Vec2> pts;
  Box2 box;
  for (Id p : ch) {
    pts.push_back(fs->shape(p)->label);
    box.add(fs->shape(p)->label);
  }
  std::vector<double> before;
  for (Id p : ch) before.push_back(tradeValue(w0, p));

  // Инструмент доступен и без правки границ; режим карты — «Гильдии».
  CHECK(!h->ui.editBorders);
  h.key(Key::R);
  CHECK(h->ui.tool == app::ToolId::Route);
  CHECK(h->ui.mapMode == schema::MapMode::Guilds);
  focus(h, box.inflated(60));
  for (Vec2 p : pts) {
    gfx::Pt s = scr(h, p);
    CHECK(freeAt(h, s));
    CHECK(h->map().routeAt(s.x, s.y) == 0);
  }
  // Гильдия-владелец — на панели инструмента.
  CHECK(h.clickUi("tool.options.guild"));
  h.key(Key::Down);
  h.key(Key::Enter);
  h.settle();
  size_t r0 = h->world().routes.size();
  clickMap(h, pts[0]);
  clickMap(h, pts[1]);
  moveMap(h, pts[2]);
  CHECK(cleanShot(h, "tools_route_draw"));
  clickMap(h, pts[2]);
  h.key(Key::Enter);
  CHECK_EQ(h->world().routes.size(), r0 + 1);
  CHECK(h->ui.sel.type == app::SelType::Route);
  Id rid = h->ui.sel.id;
  const Route* rt = h->world().route(rid);
  CHECK(rt != nullptr);
  CHECK_EQ(rt->pts.size(), size_t(3));
  const std::vector<Vec2> made = rt->pts;
  const Faction* g = h->world().faction(rt->guild);
  CHECK(g != nullptr && g->isGuild());
  // +10 % базовой ценности каждой провинции на пути (ТЗ 1.d.v).
  for (size_t i = 0; i < ch.size(); i++) {
    double base = h->world().province(ch[i])->baseTrade;
    CHECK_NEAR(tradeValue(h->world(), ch[i]) - before[i], base * schema::kRouteBonus, 1e-6);
  }
  CHECK(h->uiRect("route.stats") != nullptr);
  CHECK(h->uiRect("route.provinces") != nullptr);
  CHECK(cleanShot(h, "tools_route_inspector"));

  // Перетаскивание точки — один шаг отмены.
  Vec2 moved = pts[1] + Vec2{h->map().view().toMapLen(30), h->map().view().toMapLen(24)};
  dragMap(h, pts[1], moved, 6);
  rt = h->world().route(rid);
  CHECK(dist(rt->pts[1], moved) < h->map().view().toMapLen(1.5));
  CHECK_EQ(h->store.undoLabel(), std::string("Изменить маршрут"));
  h.key(Key::Z, ctrl());
  CHECK(h->world().route(rid)->pts[1] == made[1]);

  // Двойной щелчок по линии — новая точка.
  Vec2 mid = (pts[0] + pts[1]) * 0.5;
  gfx::Pt ms = scr(h, mid);
  bool onLine = false;
  for (float d = 0; d < 30 && !onLine; d += 1)
    for (float sgn : {1.f, -1.f}) {
      gfx::Pt q{ms.x, ms.y + d * sgn};
      if (h->map().routeAt(q.x, q.y, 3) == rid) {
        ms = q;
        onLine = true;
        break;
      }
    }
  CHECK(onLine);
  h.move(ms.x, ms.y);
  CHECK(cleanShot(h, "tools_route_edit"));
  h.doubleClick(ms.x, ms.y);
  CHECK_EQ(h->world().route(rid)->pts.size(), size_t(4));
  // Выбрать точку щелчком и удалить Delete.
  Vec2 nv = h->world().route(rid)->pts[1];
  clickMap(h, nv);
  h.key(Key::Delete);
  CHECK_EQ(h->world().route(rid)->pts.size(), size_t(3));
  CHECK(!h->hasDialog());

  // Инспектор: переименование.
  CHECK(h.clickUi("route.name"));
  h.retype("Янтарный путь");
  h.key(Key::Enter);
  CHECK_EQ(h->world().route(rid)->name, std::string("Янтарный путь"));

  // Меню правой кнопки на линии — удалить маршрут (с подтверждением).
  gfx::Pt vs = scr(h, h->world().route(rid)->pts[2]);
  h.click(vs.x, vs.y, platform::MouseRight);
  h.settle();
  CHECK(cleanShot(h, "tools_route_menu"));
  CHECK(h.clickUi("route.menu.remove"));
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK(h->world().route(rid) == nullptr);
  CHECK(!h->ui.sel);
  for (size_t i = 0; i < ch.size(); i++) CHECK_NEAR(tradeValue(h->world(), ch[i]), before[i], 1e-6);
  h.key(Key::Z, ctrl());
  CHECK(h->world().route(rid) != nullptr);

  // Удаление из инспектора.
  h->select(app::SelType::Route, rid);
  h.settle();
  CHECK(h.clickUi("route.delete"));
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK(h->world().route(rid) == nullptr);
  h.key(Key::Z, ctrl());
  CHECK(h->world().route(rid) != nullptr);
}

TEST(app_tools_route_light_and_cancel) {
  ToolGuard guard;
  Harness h("tools_route_light", 1440, 900, 1, false);
  h.demo();
  // Маршрут демонстрационного мира: выбор щелчком в режиме маршрутов, инспектор в светлой теме.
  Id rid = 0;
  h->world().routes.each([&](const Route& r) {
    if (!rid && r.pts.size() >= 3) rid = r.id;
  });
  CHECK(rid != 0);
  h.key(Key::R);
  h->select(app::SelType::Route, rid);
  h.frames(2);
  h->focusSelection();
  h.settle();
  CHECK(cleanShot(h, "tools_route_light"));
  // Начатый маршрут отменяется Esc; Enter с одной точкой ничего не создаёт.
  h->clearSelection();
  h.frames(2);
  size_t n = h->world().routes.size();
  RectF area = h->mapArea();
  gfx::Pt p{area.cx(), area.cy() + 60};
  for (int i = 0; i < 40 && h->map().routeAt(p.x, p.y, 12); i++) p.y += 9;
  h.click(p.x, p.y);
  h.key(Key::Enter);
  CHECK_EQ(h->world().routes.size(), n);
  h.key(Key::Escape);
  CHECK(h->ui.tool == app::ToolId::Route);
  h.key(Key::Enter);
  CHECK_EQ(h->world().routes.size(), n);
  // Esc без начатого маршрута — к инструменту выбора.
  h.key(Key::Escape);
  CHECK(h->ui.tool == app::ToolId::Select);
}
