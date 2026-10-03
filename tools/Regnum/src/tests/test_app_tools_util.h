// Regnum — помощники сценарных тестов инструментов правки карты (test_app_tools_*.cpp).
#pragma once
#include "app/tools_edit.h"
#include "tests/test_app_util.h"

namespace rg::toolstest {

using namespace rg::apptest;

// Подставные инструменты других тестов (test_app_editor.cpp) могли перекрыть настоящие: вернуть настоящие
// на время теста и восстановить прежние регистрации после.
struct ToolGuard {
  std::vector<app::ToolDef> saved;
  ToolGuard() {
    for (app::ToolId id : {app::ToolId::EditBorders, app::ToolId::NewProvince, app::ToolId::AddArea, app::ToolId::RemoveArea, app::ToolId::Fill,
                           app::ToolId::Knife, app::ToolId::Merge, app::ToolId::DeleteProvince, app::ToolId::Route})
      if (const app::ToolDef* d = app::findTool(id)) saved.push_back(*d);
    app::tools::installAll();
  }
  ~ToolGuard() {
    for (auto& d : saved) app::ToolReg r(d);
  }
  ToolGuard(const ToolGuard&) = delete;
  ToolGuard& operator=(const ToolGuard&) = delete;
};

inline double provArea(const World& w, Id p) {
  auto fs = geo::buildFaces(w);
  const geo::ProvinceShape* s = fs->shape(p);
  return s ? s->area : 0;
}
inline Box2 provBox(const World& w, Id p) {
  auto fs = geo::buildFaces(w);
  const geo::ProvinceShape* s = fs->shape(p);
  return s ? s->box : Box2{};
}
inline int provFaces(const World& w, Id p) {
  auto fs = geo::buildFaces(w);
  const geo::ProvinceShape* s = fs->shape(p);
  return s ? int(s->faces.size()) : 0;
}

inline gfx::Pt scr(Harness& h, Vec2 p) { return h->map().view().toScreen(p); }

// Показать область карты и дождаться конца анимации камеры.
inline void focus(Harness& h, Box2 b) {
  h->focusMap(b);
  h.settle();
}

// Точка экрана свободна (над картой, не под панелями оболочки и панелью инструмента).
inline bool freeAt(Harness& h, gfx::Pt s) {
  RectF area = h->mapArea();
  if (!area.inset(12).contains(s.x, s.y)) return false;
  for (const char* n : {"legend", "status", "zoom", "minimap", "toolbar", "inspector", "banner", "rail", "drawer", "topbar", "tool.options"})
    if (const RectF* r = h->uiRect(n))
      if (r->expand(8).contains(s.x, s.y)) return false;
  return true;
}

inline void clickMap(Harness& h, Vec2 p, int button = platform::MouseLeft, u32 mods = 0) {
  gfx::Pt s = scr(h, p);
  h.click(s.x, s.y, button, mods);
}
inline void moveMap(Harness& h, Vec2 p) {
  gfx::Pt s = scr(h, p);
  h.move(s.x, s.y);
}

// Нажать, провести через точки (экран) и (по желанию) отпустить.
inline void stroke(Harness& h, const std::vector<gfx::Pt>& pts, bool release = true) {
  hl::mouseMove(pts[0].x, pts[0].y);
  h.step();
  hl::mouseDown(pts[0].x, pts[0].y);
  h.step();
  for (size_t i = 1; i < pts.size(); i++) {
    hl::mouseMove(pts[i].x, pts[i].y);
    h.step();
  }
  if (release) {
    hl::mouseUp(pts.back().x, pts.back().y);
    h.step();
    hl::advance(0.6);
  }
}
inline void release(Harness& h, gfx::Pt p) {
  hl::mouseUp(p.x, p.y);
  h.step();
  hl::advance(0.6);
}

// Перетащить точку карты в другую точку карты за n шагов.
inline void dragMap(Harness& h, Vec2 from, Vec2 to, int steps = 8, bool rel = true) {
  std::vector<gfx::Pt> pts;
  for (int i = 0; i <= steps; i++) pts.push_back(scr(h, from + (to - from) * (double(i) / steps)));
  stroke(h, pts, rel);
}

// Снимок без уведомлений, с готовыми тайлами.
inline bool cleanShot(Harness& h, const std::string& name) {
  h.waitMap();
  h.dropToasts();
  h.frames(3);
  return h.shot(name);
}

}  // namespace rg::toolstest
