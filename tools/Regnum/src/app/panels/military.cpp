// Regnum — войска и флот: общие помощники (данные строк, фигурки, строки списков, подтверждения).
#include "app/panels/military.h"

#include "geo/topo.h"
#include "gfx/figures.h"

namespace rg::app::mil {

// ================================================================ данные
const World& frameWorld(App& a) {
  struct Holder {
    u64 frame = ~0ull;
    const App* app = nullptr;
    World w;
  };
  // Состояние интерфейса (а не static): очищается вместе с ui::shutdown, новое приложение не увидит старый мир.
  Holder& h = ui::state<Holder>(hash64("rg.app.mil.frameWorld"));
  u64 f = ui::frameIndex();
  if (h.frame != f || h.app != &a) {
    h.w = a.world();
    h.frame = f;
    h.app = &a;
  }
  return h.w;
}

std::vector<UnitRow> unitRows(const World& w, Id faction, bool fleet) {
  std::vector<UnitRow> out;
  const Faction* f = w.faction(faction);
  if (!f) return out;
  if (fleet) {
    out.reserve(f->fleet.size());
    for (const FleetRow& r : f->fleet) {
      const schema::EnumInfo& ti = schema::shipType(r.type);
      out.push_back(UnitRow{r.id, r.name.empty() ? std::string(ti.name) : r.name, ti.icon, ti.name, int(r.type), r.total, r.upkeep});
    }
  } else {
    out.reserve(f->army.size());
    for (const ArmyRow& r : f->army) {
      const schema::EnumInfo& ti = schema::unitType(r.type);
      out.push_back(UnitRow{r.id, r.name.empty() ? std::string(ti.name) : r.name, ti.icon, ti.name, int(r.type), r.total, r.upkeep});
    }
  }
  return out;
}

std::optional<UnitRow> unitRow(const World& w, Id faction, Id row, bool fleet) {
  for (UnitRow& r : unitRows(w, faction, fleet))
    if (r.id == row) return r;
  return std::nullopt;
}

const rules::RowCalc* rowCalc(const rules::Calc& c, Id faction, Id row, bool fleet) {
  const rules::FactionCalc* fc = c.faction(faction);
  if (!fc) return nullptr;
  for (const rules::RowCalc& r : fleet ? fc->fleet : fc->army)
    if (r.row == row) return &r;
  return nullptr;
}

i64 reserveOf(const World& w, Id faction, Id row, bool fleet) {
  auto c = rules::calc(w);
  const rules::RowCalc* r = rowCalc(*c, faction, row, fleet);
  return r ? std::max<i64>(0, r->reserve) : 0;
}

i64 groupCount(const ArmyGroup& g) {
  i64 n = 0;
  for (const ArmyUnit& u : g.units) n += u.count;
  return n;
}

i64 unitCount(const Army& a) {
  i64 n = 0;
  for (const ArmyGroup& g : a.groups) n += groupCount(g);
  return n;
}

i64 rowCount(const ArmyGroup& g, Id row) {
  i64 n = 0;
  for (const ArmyUnit& u : g.units)
    if (u.row == row) n += u.count;
  return n;
}

std::vector<Id> factionsIn(const Army& a) {
  std::vector<Id> r;
  for (const ArmyGroup& g : a.groups)
    if (std::find(r.begin(), r.end(), g.faction) == r.end()) r.push_back(g.faction);
  return r;
}

std::string objectCaption(const Army& a) {
  if (a.isFleet()) return a.allied() ? "Союзный флот" : "Флот";
  return a.allied() ? "Союзное войско" : "Войско";
}

const char* objectIcon(const Army& a) { return a.isFleet() ? "fleet" : "army"; }

std::string objectName(const Army& a) { return a.name.empty() ? std::string("Без названия") : a.name; }

Id provinceUnder(const World& w, Vec2 p) {
  auto fs = geo::faces(w);
  return fs ? fs->provinceAt(p) : 0;
}

Id heroLocation(const World& w, Id character, Id skip) {
  Id found = 0;
  w.armies.each([&](const Army& a) {
    if (found || a.id == skip) return;
    for (const ArmyGroup& g : a.groups)
      if (std::find(g.heroes.begin(), g.heroes.end(), character) != g.heroes.end()) found = a.id;
  });
  return found;
}

std::string fmtCount(i64 n) { return fmtInt(n); }

std::string fmtMoney(double v) {
  double frac = std::fabs(v - std::round(v));
  return fmtNum(v, frac < 1e-6 ? 0 : (std::fabs(v * 10 - std::round(v * 10)) < 1e-6 ? 1 : 2));
}

Color leaderColor(const World& w, const Army& a) { return w::factionColor(w, a.leader()); }

Color allyColor(const World& w, const Army& a) {
  if (!a.allied()) return Color(0, 0, 0, 0);
  return w::factionColor(w, a.groups[1].faction);
}

// ================================================================ виджеты
void figureIn(RectF r, ArmyKind kind, Color c1, bool allied, Color c2, bool selected) {
  ui::custom(r, [=](gfx::Canvas& c, RectF dev, float) {
    // Габариты фигурки: ширина 1,28 диаметра постамента, высота 1,40 (наконечник выше, тень ниже).
    float size = std::min(dev.w / 1.28f, dev.h / 1.40f);
    gfx::Pt ctr{dev.cx(), dev.y + (dev.h - size * 1.40f) * 0.5f + size * 0.74f};
    if (kind == ArmyKind::Fleet) gfx::drawFleetFigure(c, ctr, size, c1, selected, allied, c2);
    else gfx::drawArmyFigure(c, ctr, size, c1, selected, allied, c2);
  });
}

void figure(const World& w, const Army& a, float size, bool selected) {
  RectF r = ui::next(size, size);
  figureIn(r, a.kind, leaderColor(w, a), a.allied(), allyColor(w, a), selected);
}

void factionFlag(const World& w, Id faction, float width, float height) {
  const Faction* f = w.faction(faction);
  float h = height > 0 ? height : std::round(width * 2.f / 3.f);
  if (!f) {
    RectF r = ui::next(width, h);
    ui::draw::rect(r, ui::theme().surface3, 3);
    return;
  }
  ui::flag(f->flag, width, h, 3, f->name);
}

void factionLabel(const World& w, Id faction, float flagW) {
  ui::IdScope s(i64(faction) + 0x51000000LL);
  ui::HStack hs(24, ui::Align::Left, 6);
  factionFlag(w, faction, flagW);
  w::factionChip(faction);
}

void typeTile(RectF r, const char* icon, Color tint) {
  const ui::Theme& t = ui::theme();
  ui::draw::rect(r, t.surface3, 7);
  float s = std::min(r.w, r.h) * 0.62f;
  ui::draw::icon(icon, RectF{r.cx() - s * 0.5f, r.cy() - s * 0.5f, s, s}, tint);
}

bool objectItem(const World& w, const Army& a, bool selected, std::string_view subtitle) {
  ui::IdScope s(i64(a.id) + 0x52000000LL);
  i64 n = unitCount(a);
  std::string hint = n >= 100000 ? fmtShort(double(n)) : fmtCount(n);
  bool clicked = ui::listItem(objectName(a), {.dot = leaderColor(w, a),
                                              .subtitle = subtitle,
                                              .hint = hint,
                                              .badge = a.allied() ? std::string_view("союз") : std::string_view(),
                                              .selected = selected,
                                              .tooltip = objectCaption(a)});
  // Фигурка поверх цветной точки списка: та же, что на карте.
  RectF r = ui::lastItem().rect;
  figureIn(RectF{r.x + 4, r.cy() - 13, 26, 26}, a.kind, leaderColor(w, a), a.allied(), allyColor(w, a));
  return clicked;
}

void cellLines(RectF r, std::string_view top, std::string_view bottom, ui::Align align, ui::Ink topInk) {
  const ui::Theme& t = ui::theme();
  float h1 = ui::lineHeight(ui::Font::Body), h2 = ui::lineHeight(ui::Font::Caption);
  if (bottom.empty()) {
    ui::draw::text(top, RectF{r.x, r.y, r.w, r.h}, ui::Font::Body, ui::inkColor(topInk), align);
    return;
  }
  float y0 = std::round(r.cy() - (h1 + h2) * 0.5f);
  ui::draw::text(top, RectF{r.x, y0, r.w, h1}, ui::Font::Body, ui::inkColor(topInk), align);
  ui::draw::text(bottom, RectF{r.x, y0 + h1, r.w, h2}, ui::Font::Caption, t.textMuted, align);
}

void nameCell(RectF r, std::string_view name, std::string_view caption) {
  const ui::Theme& t = ui::theme();
  if (ui::measure(name, ui::Font::Body) <= r.w || name.find(' ') == std::string_view::npos) {
    cellLines(r, name, caption, ui::Align::Left);
    return;
  }
  // Перенос по последнему пробелу, при котором первая строка помещается.
  size_t cut = std::string_view::npos;
  for (size_t p = name.find(' '); p != std::string_view::npos; p = name.find(' ', p + 1)) {
    if (ui::measure(name.substr(0, p), ui::Font::Body) <= r.w) cut = p;
    else break;
  }
  if (cut == std::string_view::npos) {
    cellLines(r, name, caption, ui::Align::Left);
    return;
  }
  float h = ui::lineHeight(ui::Font::Body) - 1;
  float y0 = std::round(r.cy() - h);
  ui::draw::text(name.substr(0, cut), RectF{r.x, y0, r.w, h}, ui::Font::Body, t.text);
  ui::draw::text(name.substr(cut + 1), RectF{r.x, y0 + h, r.w, h}, ui::Font::Body, t.text);
}

void unitCell(RectF r, const UnitRow& row, bool accent, std::string_view caption) {
  const ui::Theme& t = ui::theme();
  float s = std::min(28.f, r.h - 8);
  RectF tile{r.x, std::round(r.cy() - s * 0.5f), s, s};
  ui::draw::rect(tile, t.surface3, 7);
  float is = std::round(s * 0.62f);
  ui::at(RectF{std::round(tile.cx() - is * 0.5f), std::round(tile.cy() - is * 0.5f), is, is});
  ui::icon(row.icon, accent ? ui::Ink::Accent : ui::Ink::Dim, is, row.typeName);   // подсказка — тип войск
  nameCell(RectF{r.x + s + 10, r.y, r.w - s - 10, r.h}, row.name, caption);
}

// ================================================================ действия
void askDisband(App& a, Id army) {
  const Army* ar = a.world().army(army);
  if (!ar) return;
  if (a.readOnly()) {
    a.act("Расформировать", [](Tx&) {});   // сообщение о просмотре прошлого хода
    return;
  }
  bool fleet = ar->isFleet();
  i64 n = unitCount(*ar);
  std::string title = fleet ? "Расформировать флот?" : "Расформировать войско?";
  std::string text = objectName(*ar) + ": " + fmtCount(n) + " " +
                     (fleet ? plural(n, "корабль вернётся", "корабля вернутся", "кораблей вернутся")
                            : plural(n, "воин вернётся", "воина вернутся", "воинов вернутся")) +
                     " в резерв. Герои останутся у фракции.";
  std::string label = std::string(fleet ? "Расформировать флот " : "Расформировать войско ") + objectName(*ar);
  a.confirm(title, text, "Расформировать", true, [army, label](App& x) {
    if (x.act(label, [&](Tx& tx) { rules::disband(tx, army); })) x.toast("Отряды возвращены в резерв", ToastKind::Success, "disband");
  });
}

void dissolveAllied(App& a, Id army) {
  const Army* ar = a.world().army(army);
  if (!ar || !ar->allied()) return;
  bool fleet = ar->isFleet();
  std::vector<Id> out;
  if (a.act(fleet ? "Распустить союзный флот" : "Распустить союзное войско", [&](Tx& tx) { out = rules::dissolveAllied(tx, army); })) {
    a.toast(std::string(fleet ? "Союзный флот распущен: " : "Союзное войско распущено: ") + std::to_string(out.size()) + " " +
                plural(i64(out.size()), "объект", "объекта", "объектов") + " на карте",
            ToastKind::Success, "dissolve");
  }
}

void askRemoveRow(App& a, Id faction, Id row, bool fleet) {
  auto r = unitRow(a.world(), faction, row, fleet);
  if (!r) return;
  if (a.readOnly()) {
    a.act("Удалить строку", [](Tx&) {});
    return;
  }
  auto c = rules::calc(a.world());
  const rules::RowCalc* rc = rowCalc(*c, faction, row, fleet);
  i64 field = rc ? rc->field : 0;
  std::string text = field > 0 ? std::string(fleet ? "Корабли этой строки уйдут из флотов на карте: " : "Отряды этой строки уйдут из войск и гарнизонов: ") +
                                     fmtCount(field) + "."
                               : std::string("Строка будет удалена из таблицы.");
  a.confirm(std::string(fleet ? "Удалить судно «" : "Удалить отряд «") + r->name + "»?", text, "Удалить", true, [faction, row, fleet](App& x) {
    x.act(fleet ? "Удалить судно из таблицы флота" : "Удалить отряд из таблицы войск", [&](Tx& tx) { rules::removeRow(tx, faction, row); });
  });
}

}  // namespace rg::app::mil
