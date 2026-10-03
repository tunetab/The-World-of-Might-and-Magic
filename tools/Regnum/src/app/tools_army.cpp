// Regnum — инструменты карты «Новое войско» и «Новый флот» (ТЗ 1.c.iii, 1.c.v): щелчок по суше (по морю) ставит
// объект выбранной фракции. Фракция выбирается на плавающей панели инструмента (по умолчанию — выделенная или
// последняя). Под указателем — фигурка-призрак; недопустимое место — красное кольцо и причина.
#include "app/tools_edit.h"
#include "app/panels/military.h"
#include "gfx/figures.h"

namespace rg::app {

namespace mil {

namespace {
Id gForcedFaction = 0;   // фракция, заданная кнопкой «Поставить войско» до выбора инструмента
}

Id& lastPlaceFaction() {
  static Id last = 0;
  return last;
}

void startPlacing(App& a, ArmyKind kind, Id faction) {
  ToolId tool = kind == ArmyKind::Fleet ? ToolId::NewFleet : ToolId::NewArmy;
  if (a.readOnly()) {
    a.act("Новый объект", [](Tx&) {});   // сообщение о просмотре прошлого хода
    return;
  }
  if (a.ui.tool == tool) {
    lastPlaceFaction() = faction;
    return;
  }
  gForcedFaction = faction;
  a.setTool(tool);
}

}  // namespace mil

namespace {

using namespace mil;

class PlaceTool final : public MapTool {
 public:
  explicit PlaceTool(ArmyKind k) : kind_(k) {}

  void activate(App& a) override { lastPlaceFaction() = defaultFaction(a); }

  bool pointerDown(App& a, const PointerEvent& e) override {
    if (e.button != 0) return false;
    if (a.readOnly()) {
      a.act("Новый объект", [](Tx&) {});
      return true;
    }
    Id fac = faction(a);
    if (!fac) {
      a.toast("Сначала выберите фракцию на панели инструмента", ToastKind::Info, "crown");
      return true;
    }
    std::string why;
    if (!rules::validPosition(a.world(), kind_, e.map, 0, &why)) {
      a.toast(why, ToastKind::Warning, "warning");
      return true;
    }
    Id id = 0;
    bool fleet = kind_ == ArmyKind::Fleet;
    if (!a.act(fleet ? "Новый флот" : "Новое войско", [&](Tx& tx) { id = rules::createArmy(tx, kind_, fac, e.map); })) return true;
    a.select(SelType::Army, id);
    a.ui.tabOf[SelType::Army] = "army.units";
    // Shift — поставить ещё один; иначе — к выбору (новый объект можно сразу перетаскивать).
    if (!(e.mods & platform::ModShift)) detail::later(a, [](App& x) { x.setTool(ToolId::Select); });
    return true;
  }

  bool pointerMove(App& a, const PointerEvent&) override {
    a.requestRedraw();
    return true;
  }

  bool key(App& a, const platform::Event& e) override {
    if (e.key == platform::Key::Escape && e.mods == 0) {
      detail::later(a, [](App& x) { x.setTool(ToolId::Select); });
      return true;
    }
    return false;
  }

  void drawOverlay(App& a, gfx::Canvas& c, const map::View& v) override {
    drawPanel(a);
    drawGhost(a, c, v);
  }

  platform::Cursor cursor(App& a) override {
    if (!a.ui.cursorMap) return platform::Cursor::Crosshair;
    return valid(a, *a.ui.cursorMap, nullptr) ? platform::Cursor::Crosshair : platform::Cursor::NotAllowed;
  }

  const char* hint(App& a) override {
    std::string why;
    if (a.ui.cursorMap && !valid(a, *a.ui.cursorMap, &why) && !why.empty()) hint_ = why;
    else hint_ = kind_ == ArmyKind::Fleet ? "Щелчок по морю — новый флот · Shift — ещё один · Esc — выход"
                                          : "Щелчок по суше — новое войско · Shift — ещё одно · Esc — выход";
    return hint_.c_str();
  }

 private:
  ArmyKind kind_;
  std::string hint_;

  static Id defaultFaction(App& a) {
    const World& w = frameWorld(a);
    auto ok = [&](Id f) { return f != 0 && w.faction(f) != nullptr; };
    if (ok(gForcedFaction)) {
      Id f = gForcedFaction;
      gForcedFaction = 0;
      return f;
    }
    gForcedFaction = 0;
    Selection s = a.ui.sel;
    if (s.type == SelType::Faction && ok(s.id)) return s.id;
    if (s.type == SelType::Army)
      if (const Army* ar = w.army(s.id); ar && ok(ar->leader())) return ar->leader();
    if (s.type == SelType::Province)
      if (const Province* p = w.province(s.id); p && ok(p->owner)) return p->owner;
    if (ok(lastPlaceFaction())) return lastPlaceFaction();
    // Первое государство по алфавиту.
    const Faction* best = nullptr;
    w.factions.each([&](const Faction& f) {
      if (!f.isState()) return;
      if (!best || compareRu(f.name, best->name) < 0) best = &f;
    });
    return best ? best->id : 0;
  }

  Id faction(App& a) const {
    Id f = lastPlaceFaction();
    return f && a.world().faction(f) ? f : 0;
  }

  bool valid(App& a, Vec2 p, std::string* why) const {
    if (!faction(a)) {
      if (why) *why = "Выберите фракцию";
      return false;
    }
    return rules::validPosition(a.world(), kind_, p, 0, why);
  }

  // Плавающая панель параметров (как у остальных инструментов карты): вид объекта, фракция, её резерв.
  void drawPanel(App& a) {
    if (a.hasDialog()) return;
    const World& w = frameWorld(a);
    const bool fleet = kind_ == ArmyKind::Fleet;
    const Id fid = faction(a);
    // Резерв фракции: сколько можно назначить новому объекту (ТЗ 1.c.iii).
    i64 reserve = 0;
    size_t rows = 0;
    if (const Faction* fc = w.faction(fid)) {
      rows = fleet ? fc->fleet.size() : fc->army.size();
      auto c = rules::calc(w);
      if (const rules::FactionCalc* fcc = c->faction(fid))
        for (const rules::RowCalc& r : fleet ? fcc->fleet : fcc->army) reserve += std::max<i64>(0, r.reserve);
    }
    std::string tag = !fid ? std::string("Выберите фракцию") : rows == 0 ? std::string("Таблица пуста") : "Резерв " + fmtCount(reserve);
    const float segW = 2 * 34 + 4, pickW = 220;
    float width = segW + 6 + pickW + 6 + tools::textW(tag, ui::Font::Strong) + 34 + 6 + 30;
    tools::OptionsBar bar(a, fleet ? "tool-fleet" : "tool-army", fleet ? "Новый флот" : "Новое войско", width);
    if (!bar) return;
    int k = fleet ? 1 : 0;
    if (ui::segmented("kind", k, {{"army", {}, "Войско — ставится на сушу"}, {"fleet", {}, "Флот — ставится на море"}},
                      {.size = ui::Size::Small, .fill = false})) {
      ArmyKind kk = k == 1 ? ArmyKind::Fleet : ArmyKind::Army;
      detail::later(a, [kk, fid](App& x) { startPlacing(x, kk, fid); });   // фракция сохраняется при смене вида
    }
    a.markUi("place.kind");
    {
      ui::Group g(pickW);
      Id f = fid;
      if (w::factionPicker("faction", f, w::FactionFilter::Any, "")) lastPlaceFaction() = f;
      ui::tooltip(fleet ? "Чей флот" : "Чьё войско");
    }
    a.markUi("place.faction");
    if (!fid) {
      ui::tag(tag, ui::Tone::Warning, "warning");
    } else if (rows == 0) {
      ui::tag(tag, ui::Tone::Warning, "warning");
      ui::tooltip(std::string("В таблице ") + (fleet ? "флота" : "войск") + " фракции нет строк — добавьте их во вкладке «" +
                  (fleet ? "Флот" : "Войска") + "»; объект можно поставить и пустым");
    } else {
      ui::tag(tag, reserve > 0 ? ui::Tone::Success : ui::Tone::Neutral, fleet ? "anchor" : "shield");
      ui::tooltip(fleet ? "Свободные корабли фракции — назначаются в инспекторе нового флота"
                        : "Свободные отряды фракции — назначаются в инспекторе нового войска");
    }
    a.markUi("place.reserve");
    ui::flex();
    if (ui::iconButton("close", "Закрыть инструмент")) detail::later(a, [](App& x) { x.setTool(ToolId::Select); });
    ui::tooltip("Закрыть инструмент", {platform::Key::Escape, 0});
    a.markUi("place.close");
  }

  void drawGhost(App& a, gfx::Canvas& c, const map::View& v) {
    if (!a.ui.cursorMap || a.hasDialog() || a.readOnly()) return;
    Vec2 p = *a.ui.cursorMap;
    std::string why;
    bool ok = valid(a, p, &why);
    const World& w = frameWorld(a);
    Color col = w::factionColor(w, faction(a));
    float size = a.map().figureSize();
    gfx::Pt sp = v.toScreen(p);
    const ui::Theme& th = ui::theme();
    if (!ok) {
      c.fillCircle(sp.x, sp.y, size * 0.62f, th.danger.alpha(0.18f));
      c.strokeCircle(sp.x, sp.y, size * 0.62f, 2, th.danger);
    } else {
      c.fillCircle(sp.x, sp.y, size * 0.62f, tools::palette().light.alpha(0.18f));
    }
    c.save();
    c.setOpacity(ok ? 0.88f : 0.5f);
    if (kind_ == ArmyKind::Fleet) gfx::drawFleetFigure(c, sp, size, col, ok);
    else gfx::drawArmyFigure(c, sp, size, col, ok);
    c.restore();
    if (!ok && !why.empty()) {
      // Причина — таблетка под фигуркой (точки интерфейса).
      float s = ui::uiScale();
      float tw = std::ceil(ui::measure(why, ui::Font::Small)) + 2;
      RectF pr{std::round(sp.x / s - (tw + 36) * 0.5f), std::round((sp.y + size * 0.72f) / s + 4), tw + 36, 24};
      ui::draw::shadow(pr, 12, 10, th.shadow, 2);
      ui::draw::rect(pr, th.surface1, 12);
      ui::draw::rectStroke(pr, th.danger.alpha(0.7f), 12, 1);
      ui::draw::icon("warning", RectF{pr.x + 8, pr.cy() - 7, 14, 14}, th.danger);
      ui::draw::text(why, RectF{pr.x + 26, pr.y, tw, pr.h}, ui::Font::Small, th.text);
    }
  }
};

ToolReg armyReg({ToolId::NewArmy, "tool-army", "Новое войско", "A", false, [] { return std::make_unique<PlaceTool>(ArmyKind::Army); }, 50});
ToolReg fleetReg({ToolId::NewFleet, "tool-fleet", "Новый флот", "Shift+A", false, [] { return std::make_unique<PlaceTool>(ArmyKind::Fleet); }, 51});

}  // namespace

std::unique_ptr<MapTool> mil::makePlaceTool(ArmyKind kind) { return std::make_unique<PlaceTool>(kind); }

}  // namespace rg::app
