// Общее для сценариев деревьев технологий и построек (test_app_trees_*.cpp): скрытие тестовых регистраций
// других сценариев, отмена/повтор сочетаниями, щелчки по отмеченным элементам, поиск уведомлений.
#pragma once
#include "tests/test_app_util.h"

namespace rg::apptest::trees {

// Тестовые регистрации других сценариев («test.*»: вкладки, шапки, панели) на время сценария убираются из реестров,
// чтобы инспектор и снимки были как в приложении.
struct HideTestRegs {
  std::vector<app::TabDef> tabs;
  std::vector<app::HeaderDef> headers;
  std::vector<app::DrawerDef> drawers;
  template <class T>
  static void strip(std::vector<T>& v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](const T& d) { return std::string_view(d.id).starts_with("test."); }), v.end());
  }
  HideTestRegs() {
    auto& t = const_cast<std::vector<app::TabDef>&>(app::tabs());
    auto& h = const_cast<std::vector<app::HeaderDef>&>(app::headers());
    auto& d = const_cast<std::vector<app::DrawerDef>&>(app::drawers());
    tabs = t;
    headers = h;
    drawers = d;
    strip(t);
    strip(h);
    strip(d);
  }
  ~HideTestRegs() {
    const_cast<std::vector<app::TabDef>&>(app::tabs()) = tabs;
    const_cast<std::vector<app::HeaderDef>&>(app::headers()) = headers;
    const_cast<std::vector<app::DrawerDef>&>(app::drawers()) = drawers;
  }
  HideTestRegs(const HideTestRegs&) = delete;
  HideTestRegs& operator=(const HideTestRegs&) = delete;
};

// Отмена и повтор сочетаниями; уведомления «Отменено…» убираются, чтобы не держать кадры анимации.
inline void undo(Harness& h) {
  h.key(Key::Z, ctrl());
  h->toasts().clear();
}
inline void redo(Harness& h) {
  h.key(Key::Y, ctrl());
  h->toasts().clear();
}

inline RectF rectOf(app::App& a, const std::string& name) {
  const RectF* r = a.uiRect(name);
  if (!r) test::fail(__FILE__, __LINE__, "нет элемента " + name);
  return *r;
}

// Щелчок по отмеченному элементу (доли ширины и высоты) и ожидание покоя.
inline void clickRect(Harness& h, const std::string& name, float fx = 0.5f, float fy = 0.5f) {
  RectF r = rectOf(h.a(), name);
  h.click(r.x + r.w * fx, r.y + r.h * fy);
  h.settle();
}

inline bool hasToast(app::App& a, const std::string& part, app::ToastKind kind) {
  for (const app::Toast& t : a.toasts())
    if (t.kind == kind && t.text.find(part) != std::string::npos) return true;
  return false;
}

// Вкладка инспектора по ID (для проверки видимости).
inline const app::TabDef* tabDef(std::string_view id) {
  for (const app::TabDef& t : app::tabs())
    if (id == t.id) return &t;
  return nullptr;
}

}  // namespace rg::apptest::trees
