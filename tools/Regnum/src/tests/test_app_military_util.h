// Помощники сценариев войск и флота: настоящие инструменты постановки (тест оболочки подменяет их своим),
// поиск фракций и объектов демонстрационного мира, точки карты на экране.
#pragma once
#include "app/panels/military.h"
#include "tests/test_app_util.h"

namespace rg::apptest {

// Настоящие инструменты «Новое войско» и «Новый флот» на время теста (прежние регистрации — обратно).
struct RealArmyTools {
  std::optional<app::ToolDef> army, fleet;
  RealArmyTools() {
    if (const app::ToolDef* t = app::findTool(app::ToolId::NewArmy)) army = *t;
    if (const app::ToolDef* t = app::findTool(app::ToolId::NewFleet)) fleet = *t;
    app::ToolReg a({app::ToolId::NewArmy, "tool-army", "Новое войско", "A", false, [] { return app::mil::makePlaceTool(ArmyKind::Army); }, 50});
    app::ToolReg f({app::ToolId::NewFleet, "tool-fleet", "Новый флот", "Shift+A", false, [] { return app::mil::makePlaceTool(ArmyKind::Fleet); }, 51});
  }
  ~RealArmyTools() {
    if (army) app::ToolReg a(*army);
    if (fleet) app::ToolReg f(*fleet);
  }
  RealArmyTools(const RealArmyTools&) = delete;
  RealArmyTools& operator=(const RealArmyTools&) = delete;
};

inline Id factionByName(const World& w, std::string_view part) {
  Id found = 0;
  w.factions.each([&](const Faction& f) {
    if (!found && f.name.find(part) != std::string::npos) found = f.id;
  });
  return found;
}

// Объект фракции (лидер) данного вида, не союзный.
inline Id armyOf(const World& w, Id faction, ArmyKind kind) {
  Id found = 0;
  w.armies.each([&](const Army& a) {
    if (!found && a.kind == kind && !a.allied() && a.leader() == faction) found = a.id;
  });
  return found;
}

inline gfx::Pt screenOf(Harness& h, Vec2 p) { return h->map().view().toScreen(p); }

// Точка свободной части карты (не под панелями оболочки).
inline bool freeOnScreen(Harness& h, float x, float y) {
  RectF area = h->mapArea().inset(24);
  if (!area.contains(x, y)) return false;
  for (const char* n : {"legend", "status", "zoom", "minimap", "toolbar", "inspector", "banner", "rail", "drawer", "topbar", "tool.options"})
    if (const RectF* r = h->uiRect(n))
      if (r->expand(8).contains(x, y)) return false;
  return true;
}

// Ищет на экране точку карты, где можно поставить объект вида kind (сетка с шагом step).
inline std::optional<std::pair<Vec2, gfx::Pt>> freeSpot(Harness& h, ArmyKind kind, float step = 23) {
  RectF area = h->mapArea();
  const World& w = h->world();
  for (float y = area.y + 40; y < area.bottom() - 40; y += step)
    for (float x = area.x + 40; x < area.right() - 40; x += step) {
      if (!freeOnScreen(h, x, y)) continue;
      Vec2 m = h->map().view().toMap(x, y);
      if (!rules::validPosition(w, kind, m)) continue;
      // Запас: соседние точки тоже допустимы (щелчок не попадёт на берег или край).
      bool ok = true;
      for (Vec2 d : {Vec2{20, 0}, Vec2{-20, 0}, Vec2{0, 20}, Vec2{0, -20}}) ok = ok && rules::validPosition(w, kind, m + d);
      if (!ok || h->map().armyAt(x, y)) continue;
      return std::make_pair(m, gfx::Pt{x, y});
    }
  return std::nullopt;
}

// Снимок части окна (логические пиксели), увеличенный в k раз — для проверки мелких деталей.
inline bool cropShot(const std::string& name, RectF r, int k = 2) {
  const platform::Frame& f = hl::lastFrame();
  int x0 = std::max(0, int(r.x * f.scale)), y0 = std::max(0, int(r.y * f.scale));
  int x1 = std::min(f.w, int(r.right() * f.scale)), y1 = std::min(f.h, int(r.bottom() * f.scale));
  if (x1 <= x0 || y1 <= y0) return false;
  codec::RgbaImage img;
  img.w = (x1 - x0) * k;
  img.h = (y1 - y0) * k;
  img.rgba.resize(size_t(img.w) * size_t(img.h) * 4);
  for (int y = 0; y < img.h; y++)
    for (int x = 0; x < img.w; x++) {
      u32 p = f.row(y0 + y / k)[x0 + x / k];
      u8* d = &img.rgba[(size_t(y) * size_t(img.w) + size_t(x)) * 4];
      d[0] = u8(p >> 16);
      d[1] = u8(p >> 8);
      d[2] = u8(p);
      d[3] = 255;
    }
  return codec::writePngFile(test::outDir() + "/app_" + name + ".png", img, 6);
}

// Показать точку карты в центре видимой части с масштабом zoom и дождаться камеры.
inline void showAt(Harness& h, Vec2 p, double zoom) {
  h->focusMap(p, zoom);
  h.settle();
  h.waitMap();
}

}  // namespace rg::apptest
