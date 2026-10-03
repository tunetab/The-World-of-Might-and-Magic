// Regnum — помощники сценариев панелей государств и гильдий (test_app_faction_*.cpp).
#pragma once
#include "tests/test_app_util.h"

namespace rg::factest {

using namespace rg::apptest;

// Тестовые регистрации других сценариев («test.*»: вкладки, шапка, выдвижная панель) на время сценария
// убираются из реестров и возвращаются после.
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

inline Id findFaction(const World& w, std::string_view name) {
  Id r = 0;
  w.factions.each([&](const Faction& f) {
    if (f.name == name) r = f.id;
  });
  return r;
}

// Прокрутить тело инспектора колесом (положительное — вверх).
inline void scrollInspector(Harness& h, float notches) {
  const RectF* r = h->uiRect("inspector");
  if (!r) return;
  h.wheel(r->cx(), r->bottom() - 120, notches);
  h.settle();
}

// Выбрать фракцию, открыть вкладку и вернуть прокрутку инспектора к началу (она общая для вкладок).
inline void openTab(Harness& h, Id faction, const char* tab) {
  h->ui.tabOf[app::SelType::Faction] = tab;
  h->select(app::SelType::Faction, faction);
  h.settle();
  h.dropToasts();
  if (const RectF* r = h->uiRect("inspector")) {
    h.wheel(r->cx(), r->bottom() - 120, 40);
    h.frames(24);   // плавная прокрутка — 120–180 мс
  }
}

// Прокрутить инспектор так, чтобы элемент оболочки name был виден целиком.
inline bool ensureVisible(Harness& h, const std::string& name) {
  int missing = 0;
  for (int k = 0; k < 60; k++) {
    const RectF* r = h->uiRect(name);
    const RectF* in = h->uiRect("inspector");
    const RectF* tabs = h->uiRect("inspector.tabs");
    if (!in) return false;
    if (!r) {
      // Строка таблицы вне видимой части не строится: сначала вниз, потом вверх.
      h.wheel(in->cx(), in->cy() + 40, missing++ < 20 ? -2.f : 2.f);
      h.frames(12);
      continue;
    }
    float top = (tabs ? tabs->bottom() : in->y) + 8, bottom = in->bottom() - 16;
    if (r->y >= top && r->bottom() <= bottom) return true;
    h.wheel(in->cx(), in->cy() + 40, r->y < top ? 2.f : -2.f);
    h.frames(12);
  }
  return false;
}

inline bool clickIn(Harness& h, const std::string& name) {
  if (!ensureVisible(h, name)) return false;
  return h.clickUi(name);
}

// Ввод числа в поле: щелчок (поле выделяет всё), текст, Enter.
inline bool typeNumber(Harness& h, const std::string& field, const std::string& value) {
  if (!clickIn(h, field)) return false;
  h.retype(value);
  h.key(Key::Enter);
  h.step();
  return true;
}

// Выбор в раскрывающемся списке с поиском: открыть, набрать, Enter.
inline bool pickInCombo(Harness& h, const std::string& field, const std::string& text) {
  if (!clickIn(h, field)) return false;
  h.type(text);
  h.key(Key::Enter);
  h.step();
  return true;
}

// Ответ в окне запроса строки (App::prompt).
inline bool answerPrompt(Harness& h, const std::string& text) {
  if (!h->hasDialog("prompt")) return false;
  if (!h.clickUi("dialog.field")) return false;
  h.retype(text);
  if (!h.clickUi("dialog.ok")) return false;
  h.step();
  return true;
}

inline size_t liveToasts(Harness& h) {
  size_t n = 0;
  for (auto& t : h->toasts())
    if (t.closeAt < 0) n++;
  return n;
}

}  // namespace rg::factest
