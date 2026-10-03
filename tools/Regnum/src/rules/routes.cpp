// Regnum — торговые маршруты (ТЗ 1.d.v): +10 % базовой ценности каждой провинции на пути.
#include "rules/internal.h"

namespace rg::rules {

using namespace detail;

namespace {

std::vector<Vec2> cleanRoute(const Tx& tx, const std::vector<Vec2>& pts) {
  std::vector<Vec2> r;
  for (Vec2 p : pts) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) fail("Недопустимые координаты точки маршрута");
    if (r.empty() || dist2(r.back(), p) > 1e-12) r.push_back(p);
  }
  if (r.size() < 2) fail("Маршрут должен содержать не менее двух разных точек");
  Box2 b;
  auto fs = facesFor(tx);
  if (fs && !fs->faces.empty()) b = fs->bounds;
  else b = Box2{0, 0, schema::kMapWidth, schema::kMapHeight};
  b = b.inflated(1e-6);
  for (Vec2 p : r)
    if (!b.contains(p)) fail("Точка маршрута вне карты");
  return r;
}

}  // namespace

Id createRoute(Tx& tx, const std::vector<Vec2>& pts, Id guild) {
  if (guild) needGuild(tx.w(), guild);
  Route r;
  r.pts = cleanRoute(tx, pts);
  r.guild = guild;
  std::vector<std::string> taken;
  tx.w().routes.each([&](const Route& x) { taken.push_back(x.name); });
  r.name = uniqueName(taken, "Торговый путь " + std::to_string(tx.w().routes.size() + 1));
  Id id = tx.add(std::move(r)).id;
  const Route& made = *tx.w().route(id);
  addLog(tx, LogKind::Trade, "Проложен торговый маршрут " + q(made.name) + (guild ? " гильдии " + facName(tx.w(), guild) : std::string()),
         LogRefs{0, 0, guild ? std::vector<Id>{guild} : std::vector<Id>{}});
  return id;
}

void setRoutePoints(Tx& tx, Id route, const std::vector<Vec2>& pts) {
  if (!tx.w().route(route)) fail(route ? "Маршрут не найден" : "Не выбран маршрут");
  std::vector<Vec2> r = cleanRoute(tx, pts);
  if (tx.w().route(route)->pts == r) return;
  tx.route(route).pts = std::move(r);
}

void removeRoute(Tx& tx, Id route) {
  const Route* r = tx.w().route(route);
  if (!r) fail(route ? "Маршрут не найден" : "Не выбран маршрут");
  std::string name = q(r->name.empty() ? "Без названия" : r->name);
  Id guild = r->guild;
  tx.eraseRoute(route);
  addLog(tx, LogKind::Trade, "Упразднён торговый маршрут " + name, LogRefs{0, 0, guild ? std::vector<Id>{guild} : std::vector<Id>{}});
}

}  // namespace rg::rules
