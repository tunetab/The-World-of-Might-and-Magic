// Regnum — инспектор торгового маршрута (ТЗ 1.d.v): название, гильдия-владелец, цвет линии, длина, провинции
// на пути и их бонус: каждый маршрут через провинцию повышает её текущую торговую ценность на 10 % базовой.
#include "app/tools_edit.h"

namespace rg::app {

namespace {

using platform::Key;

struct RouteInfo {
  std::vector<Id> provinces;   // по порядку прохождения
  int land = 0;                // сухопутные (только они получают бонус: морские провинции без сведений)
  double length = 0;
  double bonus = 0;            // Σ 10 % базовой ценности
};

RouteInfo infoOf(const World& w, const Route& r) {
  RouteInfo in;
  for (size_t i = 1; i < r.pts.size(); i++) in.length += dist(r.pts[i - 1], r.pts[i]);
  if (r.pts.size() >= 2) in.provinces = geo::faces(w)->provincesOnPolyline(r.pts);
  for (Id pid : in.provinces) {
    const Province* p = w.province(pid);
    if (!p || p->sea) continue;
    in.land++;
    in.bonus += p->baseTrade * schema::kRouteBonus;
  }
  return in;
}

std::string routeName(const Route& r) { return r.name.empty() ? std::string("Торговый маршрут") : r.name; }

Color lineColor(const World& w, const Route& r) {
  if (r.color) return r.color->withA(255);
  if (const Faction* g = w.faction(r.guild)) return g->color;
  return ui::toneColor(ui::Tone::Accent);
}

void askRemove(App& a, Id rid) {
  const Route* r = a.world().route(rid);
  if (!r || a.readOnly()) return;
  a.confirm("Удалить маршрут?",
            "«" + routeName(*r) + "» исчезнет с карты; провинции на пути потеряют его +10 % торговой ценности. Ctrl+Z вернёт.", "Удалить", true,
            [rid](App& x) {
              if (!x.world().route(rid)) return;
              bool wasSel = x.ui.sel == Selection{SelType::Route, rid};
              if (x.act("Удалить маршрут", [&](Tx& tx) { rules::removeRoute(tx, rid); }) && wasSel) x.clearSelection();
            });
}

// ---------------------------------------------------------------- шапка
void header(App& a, Id rid) {
  const World& w = a.world();
  const Route* r = w.route(rid);
  if (!r) return;
  const Faction* g = w.faction(r->guild);
  ui::caption("Торговый маршрут");
  ui::label(routeName(*r), {.font = ui::Font::Display});
  RouteInfo in = infoOf(w, *r);
  ui::HStack hs(26, ui::Align::Left, 6);
  if (g) w::factionChip(g->id, true);
  else ui::chip("Без гильдии", {.icon = "guild"});
  ui::tag(std::to_string(in.land) + " " + plural(in.land, "провинция", "провинции", "провинций") + " · +10 %", ui::Tone::Success, "trade-value");
  ui::tooltip("Каждая провинция на пути: +10 % базовой торговой ценности");
}

// ---------------------------------------------------------------- вкладка «Маршрут»
void overview(App& a, Id rid) {
  const World& w = a.world();
  const Route* r = w.route(rid);
  if (!r) return;
  const bool ro = a.readOnly();
  RouteInfo in = infoOf(w, *r);
  auto calc = rules::calc(w);
  {
    ui::Row row({ui::fr(1), ui::fr(1)}, 64, 10);
    ui::stat(std::to_string(in.land), "Провинций", {.icon = "province", .tooltip = "Сухопутные провинции, через которые проходит маршрут"});
    ui::stat("+" + fmtNum(in.bonus, in.bonus < 100 ? 1 : 0), "Торговая ценность",
             {.icon = "trade-value", .tone = ui::Tone::Success, .tooltip = "Прибавка к текущей торговой ценности провинций: по 10 % базовой"});
    ui::stat(fmtNum(in.length), "Длина", {.icon = "ruler", .tone = ui::Tone::Info, .tooltip = "Длина линии в пикселях карты"});
    ui::stat(std::to_string(r->pts.size()), plural(i64(r->pts.size()), "Точка", "Точки", "Точек"),
             {.icon = "map-pin", .tone = ui::Tone::Info, .tooltip = "Точки маршрута — правка на карте инструментом «Торговый маршрут» (R)"});
  }
  a.markUi("route.stats");

  if (ui::Section s("Свойства", "info"); s) {
    ui::Disabled dis(ro);
    std::string name = r->name;
    ui::prop("Название", "edit");
    if (ui::textField("name", name, {.placeholder = "Название маршрута", .maxLength = 80})) {
      std::string nm = trim(name);
      if (!nm.empty() && nm != r->name) a.act("Переименовать маршрут", [&](Tx& tx) { tx.route(rid).name = nm; });
    }
    a.markUi("route.name");
    ui::prop("Гильдия", "guild");
    Id gid = r->guild;
    if (w::factionPicker("guild", gid, w::FactionFilter::Guilds, "Без гильдии", 0, ro)) {
      const Faction* ng = w.faction(gid);
      std::string text = ng ? "Маршрут " + routeName(*r) + " передан гильдии " + ng->name : "Маршрут " + routeName(*r) + " больше не принадлежит гильдии";
      a.act("Гильдия маршрута", [&](Tx& tx) {
        if (gid && (!tx.w().faction(gid) || !tx.w().faction(gid)->isGuild())) fail("Владельцем маршрута может быть только торговая гильдия");
        tx.route(rid).guild = gid;
        rules::LogRefs refs;
        if (gid) refs.factions.push_back(gid);
        rules::addLog(tx, LogKind::Trade, text, refs);
      });
    }
    a.markUi("route.guild");
    ui::prop("Цвет линии", "palette");
    {
      ui::HStack hs(30, ui::Align::Left, 6);
      Color c = lineColor(w, *r);
      if (ui::colorButton("color", c, {.tooltip = "Цвет линии маршрута на карте"})) {
        Color nc = c.withA(255);
        a.act("Цвет маршрута", [&](Tx& tx) { tx.route(rid).color = nc; }, {.coalesce = "route.color." + std::to_string(rid)});
      }
      a.markUi("route.color");
      if (r->color) {
        if (ui::iconButton("refresh", r->guild ? "Цвет гильдии" : "Цвет по умолчанию"))
          a.act("Цвет маршрута", [&](Tx& tx) { tx.route(rid).color.reset(); });
        a.markUi("route.color.reset");
      } else {
        ui::label(r->guild ? "как у гильдии" : "по умолчанию", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
      }
    }
  }

  std::string badge = std::to_string(in.provinces.size());
  if (ui::Section s("Провинции на пути", "province", {.badge = badge}); s) {
    if (in.provinces.empty()) {
      ui::emptyState("route", "Маршрут пока не проходит по провинциям.");
    } else {
      ui::Column cols[] = {{"Провинция", nullptr, ui::fr(1.7f, 120)},
                           {"", "route", ui::px(44), ui::Align::Right, false, "Маршрутов через провинцию"},
                           {"База", nullptr, ui::px(58), ui::Align::Right, false, "Базовая торговая ценность"},
                           {"Бонус", nullptr, ui::px(64), ui::Align::Right, false, "+10 % базовой ценности от этого маршрута"}};
      ui::Table t("provs", cols, int(in.provinces.size()), {.selectable = false});
      for (int i : t) {
        Id pid = in.provinces[size_t(i)];
        const Province* p = w.province(pid);
        t.cell();
        w::provinceChip(pid);
        if (!p || p->sea) {
          t.text("—", ui::Ink::Muted);
          t.text("—", ui::Ink::Muted);
          t.text("—", ui::Ink::Muted);
          continue;
        }
        const rules::ProvinceCalc* pc = calc->province(pid);
        t.text(std::to_string(pc ? pc->routes : 0), ui::Ink::Dim);
        t.text(fmtNum(p->baseTrade, std::fabs(p->baseTrade - std::round(p->baseTrade)) > 1e-9 ? 1 : 0));
        double b = p->baseTrade * schema::kRouteBonus;
        t.text("+" + fmtNum(b, std::fabs(b - std::round(b)) > 1e-9 ? 1 : 0), ui::Ink::Success, ui::Font::Strong);
      }
      if (t.footer()) {
        t.text("Итого", ui::Ink::Dim, ui::Font::Strong);
        t.text("");
        t.text("");
        t.text("+" + fmtNum(in.bonus, std::fabs(in.bonus - std::round(in.bonus)) > 1e-9 ? 1 : 0), ui::Ink::Success, ui::Font::Strong);
      }
      a.markUi("route.provinces");
    }
  }

  ui::spacer(4);
  {
    ui::HStack hs(30, ui::Align::Left, 8);
    if (ui::button("Править на карте", {.icon = "tool-route", .disabled = ro})) {
      a.setTool(ToolId::Route);
      a.focusSelection();
    }
    ui::tooltip("Точки маршрута: тянуть, двойной щелчок — новая, Delete — удалить", {Key::R, 0});
    a.markUi("route.edit");
    ui::flex();
    if (ui::button("Удалить", {.variant = ui::Variant::Danger, .icon = "trash", .disabled = ro})) askRemove(a, rid);
    a.markUi("route.delete");
  }
}

HeaderReg headerReg({"route.header", SelType::Route, 0, header});
TabReg tabReg({"route.overview", "route", "Маршрут", 10, SelType::Route, nullptr, overview});

}  // namespace

}  // namespace rg::app
