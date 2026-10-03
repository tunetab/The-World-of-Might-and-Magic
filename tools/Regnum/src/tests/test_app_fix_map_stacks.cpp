// Регрессии исправлений карты в приложении:
//  * стопка наложившихся войск: щелчок приближает карту так, что объекты расходятся (выделение не меняется),
//    наведение — на верхний объект стопки; фигурки при открытии демо-мира не перекрываются;
//  * «Показать всю карту» (Home) при открытом инспекторе вписывает мир в свободную часть между панелями.
#include "tests/test_app_military_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

// Отметки не пересекаются, каждый объект — ровно в одной.
void checkNoOverlap(Harness& h) {
  std::vector<map::ArmyMark> marks = h->map().armyMarks();
  std::vector<Id> seen;
  for (const map::ArmyMark& m : marks) seen.insert(seen.end(), m.members.begin(), m.members.end());
  std::sort(seen.begin(), seen.end());
  CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
  CHECK_EQ(seen.size(), size_t(h->world().armies.size()));
  for (size_t i = 0; i < marks.size(); i++)
    for (size_t j = i + 1; j < marks.size(); j++) CHECK(marks[i].bounds.intersect(marks[j].bounds).empty());
}

}  // namespace

TEST(app_fix_map_stack_click_separates) {
  Harness h("fix_map_stack", 1600, 900);
  h.demo();
  h.waitMap();
  h.dropToasts();
  checkNoOverlap(h);
  h.key(Key::Home);
  h.settle();
  h.waitMap();
  checkNoOverlap(h);
  // Стопка в свободной части карты.
  std::optional<map::ArmyMark> stack;
  for (const map::ArmyMark& m : h->map().armyMarks()) {
    const gfx::Pt s = screenOf(h, m.pos);
    if (m.cluster() && freeOnScreen(h, s.x, s.y)) stack = m;
  }
  CHECK(stack.has_value());
  if (!stack) return;
  const gfx::Pt s = screenOf(h, stack->pos);
  CHECK(h.shot("fix_map_stack_before"));
  // Наведение — верхний объект стопки.
  h.move(s.x, s.y);
  CHECK(h->ui.hover == (app::Selection{app::SelType::Army, stack->top}));
  const double z0 = h->map().view().zoom;
  h.click(s.x, s.y);
  h.settle();
  h.waitMap();
  h.dropToasts();
  CHECK(h->map().view().zoom > z0);
  CHECK(!h->ui.sel);   // щелчок по стопке не выделяет объект
  // Объекты стопки — отдельные отметки, каждая в свободной части окна.
  for (Id id : stack->members) {
    std::optional<map::ArmyMark> own;
    for (const map::ArmyMark& m : h->map().armyMarks())
      if (std::find(m.members.begin(), m.members.end(), id) != m.members.end()) own = m;
    CHECK(own.has_value());
    if (!own) continue;
    for (Id other : stack->members)
      if (other != id) CHECK(std::find(own->members.begin(), own->members.end(), other) == own->members.end());
    const gfx::Pt p = screenOf(h, own->pos);
    CHECK(h->mapArea().contains(p.x, p.y));
  }
  checkNoOverlap(h);
  CHECK(h.shot("fix_map_stack_after"));
  // Теперь щелчок по объекту выделяет его.
  const gfx::Pt p = screenOf(h, h->world().army(stack->top)->pos);
  h.click(p.x, p.y);
  h.settle();
  CHECK(h->ui.sel == (app::Selection{app::SelType::Army, stack->top}));
}

TEST(app_fix_map_fit_with_inspector) {
  Harness h("fix_map_fit_inspector", 1600, 900);
  h.demo();
  Id prov = 0;
  h->world().provinces.each([&](const Province& p) {
    if (!prov && !p.sea) prov = p.id;
  });
  h->select(app::SelType::Province, prov);
  h.settle();
  CHECK(h->uiRect("inspector") != nullptr);
  h.key(Key::Home);
  h.settle();
  h.waitMap();
  h.dropToasts();
  CHECK(h.shot("fix_map_fit_inspector"));
  const map::View& v = h->map().view();
  const RectF area = h->mapArea();
  const gfx::Pt tl = v.toScreen({0, 0}), br = v.toScreen({8000, 4500});
  std::printf("  map %.1f,%.1f — %.1f,%.1f; area %.1f,%.1f %.1fx%.1f; zoom %.4f\n", tl.x, tl.y, br.x, br.y, area.x, area.y, area.w, area.h, v.zoom);
  CHECK(tl.x >= area.x - 1 && tl.y >= area.y - 1);
  CHECK(br.x <= area.right() + 1 && br.y <= area.bottom() + 1);
  if (const RectF* ins = h->uiRect("inspector")) CHECK(br.x <= ins->x + 1);
  CHECK_NEAR(v.zoom, h->map().minZoom(), 1e-9);
  // Колесо дальше не отдаляет: весь мир уже виден.
  h.wheel(area.cx(), area.cy(), -3);
  h.settle();
  CHECK(h->map().view().zoom >= v.zoom - 1e-9);
  // Инспектор закрыт — Home снова вписывает мир в (большую) свободную часть.
  h->clearSelection();
  h.settle();
  h.key(Key::Home);
  h.settle();
  const RectF area2 = h->mapArea();
  CHECK(area2.w > area.w);
  const gfx::Pt tl2 = h->map().view().toScreen({0, 0}), br2 = h->map().view().toScreen({8000, 4500});
  CHECK(tl2.x >= area2.x - 1 && tl2.y >= area2.y - 1 && br2.x <= area2.right() + 1 && br2.y <= area2.bottom() + 1);
}
