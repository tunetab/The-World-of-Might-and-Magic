// Сценарии вкладки «Население» инспектора провинции: добавление и правка рас (повтор расы отклоняется),
// удаление строки, довольство двуполярным ползунком меняет вероятность восстания, культура, снимок вкладки.
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

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
};

const Province* prov(Harness& h, Id pid) { return h->world().province(pid); }

Id openProvince(Harness& h, const char* tab) {
  auto vp = h.visibleProvince();
  CHECK(vp.has_value());
  h->ui.tabOf[app::SelType::Province] = tab;
  h.click(vp->second.x, vp->second.y);
  h.settle();
  CHECK(h->ui.sel == (app::Selection{app::SelType::Province, vp->first}));
  return vp->first;
}

bool hasToast(Harness& h, std::string_view part) {
  for (auto& t : h->toasts())
    if (t.text.find(part) != std::string::npos) return true;
  return false;
}

double rebellion(Harness& h, Id pid) { return rules::calc(h->world())->province(pid)->rebellion; }

// Дорожка ползунка довольства (как в province_common.cpp: отступ 9, значение 46 + 6 справа).
struct Track {
  float x0, x1, y;
};
Track track(const RectF& r) { return Track{r.x + 9, r.x + 9 + std::max(20.f, r.w - 18 - 46 - 6), r.y + 15}; }

}  // namespace

TEST(app_province_races_add_edit) {
  HideTestRegs hide;
  Harness h("province_races", 1440, 1200);
  h.demo();
  Id pid = openProvince(h, "province.population");
  size_t n0 = prov(h, pid)->races.size();
  CHECK(n0 >= 1);
  const RectF* hd = h->uiRect("province.races.header");
  CHECK(hd != nullptr);
  // «+» в заголовке раздела — новая строка с первой свободной расой справочника.
  h.click(hd->right() - 21, hd->cy());
  h.settle();
  CHECK_EQ(prov(h, pid)->races.size(), n0 + 1);
  Id added = prov(h, pid)->races.back().race;
  CHECK(added != 0);
  for (size_t i = 0; i < n0; i++) CHECK(prov(h, pid)->races[i].race != added);
  CHECK_EQ(prov(h, pid)->races.back().pop, i64(0));
  // Численность новой строки: ввод и Enter.
  const RectF* pf = h->uiRect("province.racePop." + std::to_string(n0));
  CHECK(pf != nullptr);
  h.click(pf->cx(), pf->cy());
  h.retype("12500");
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(prov(h, pid)->races.back().pop, i64(12500));
  i64 total = 0;
  for (auto& r : prov(h, pid)->races) total += r.pop;
  CHECK_EQ(rules::calc(h->world())->province(pid)->population, total);
  // Повтор расы отклоняется с сообщением: выбрать в новой строке расу первой строки.
  Id first = prov(h, pid)->races[0].race;
  const auto& cat = h->world().catalogs->races;
  int ci = -1;
  for (size_t i = 0; i < cat.size(); i++)
    if (cat[i].id == first) ci = int(i);
  CHECK(ci >= 0);
  const RectF* rc = h->uiRect("province.race." + std::to_string(n0));
  CHECK(rc != nullptr);
  h.click(rc->cx(), rc->cy());
  h.key(Key::PageUp);
  for (int i = 0; i <= ci; i++) h.key(Key::Down);
  h.key(Key::Enter);
  h.settle();
  CHECK(hasToast(h, "уже есть"));
  CHECK_EQ(prov(h, pid)->races.back().race, added);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("province_population"));
  // Удаление строки и отмена.
  const RectF* del = h->uiRect("province.raceDel." + std::to_string(n0));
  CHECK(del != nullptr);
  h.click(del->cx(), del->cy());
  h.settle();
  CHECK_EQ(prov(h, pid)->races.size(), n0);
  h.key(Key::Z, ctrl());
  CHECK_EQ(prov(h, pid)->races.size(), n0 + 1);
  CHECK_EQ(prov(h, pid)->races.back().pop, i64(12500));
}

TEST(app_province_contentment_changes_rebellion) {
  HideTestRegs hide;
  Harness h("province_content", 1440, 1000);
  h.demo();
  Id pid = openProvince(h, "province.population");
  double c0 = prov(h, pid)->contentment;
  double r0 = rebellion(h, pid);
  double mods = rules::calc(h->world())->province(pid)->fx[Fx::RebellionPct];
  CHECK(h->uiRect("province.rebellion") != nullptr);
  const RectF* sr = h->uiRect("province.contentment");
  CHECK(sr != nullptr);
  Track t = track(*sr);
  float kx = t.x0 + (t.x1 - t.x0) * float((c0 + 100) / 200);
  // Перетащить в −100: восстание = 50 % + модификаторы.
  h.drag(kx, t.y, t.x0 - 30, t.y);
  h.settle();
  CHECK_NEAR(prov(h, pid)->contentment, -100, 1e-9);
  CHECK_NEAR(rebellion(h, pid), clamp(50 + mods, 0.0, 100.0), 1e-9);
  CHECK(rebellion(h, pid) >= r0);
  // Стрелка вправо — +1 (ползунок в фокусе после перетаскивания).
  h.key(Key::Right);
  CHECK_NEAR(prov(h, pid)->contentment, -99, 1e-9);
  // Щелчок у правого края — +100: восстание падает.
  h.click(t.x1 + 4, t.y);
  h.settle();
  CHECK_NEAR(prov(h, pid)->contentment, 100, 1e-9);
  CHECK_NEAR(rebellion(h, pid), clamp(-50 + mods, 0.0, 100.0), 1e-9);
  CHECK(rebellion(h, pid) <= r0);
  // Отмена возвращает исходное довольство (правки ползунка сливаются).
  for (int i = 0; i < 6 && h->store.undoLabel() == "Довольство населения"; i++) h.key(Key::Z, ctrl());
  CHECK_NEAR(prov(h, pid)->contentment, c0, 1e-9);
  CHECK_NEAR(rebellion(h, pid), r0, 1e-9);
}

TEST(app_province_culture_religion) {
  HideTestRegs hide;
  Harness h("province_culture", 1440, 1200);
  h.demo();
  Id pid = openProvince(h, "province.population");
  const RectF* cu = h->uiRect("province.culture");
  CHECK(cu != nullptr);
  // Первая культура справочника: PageUp («не указана»), ↓, Enter.
  Id want = h->world().catalogs->cultures.front().id;
  Id was = prov(h, pid)->culture;
  h.click(cu->cx(), cu->cy());
  h.key(Key::PageUp);
  h.key(Key::Down);
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(prov(h, pid)->culture, want);
  if (was != want) {
    h.key(Key::Z, ctrl());
    CHECK_EQ(prov(h, pid)->culture, was);
  }
  // Религия «не указана».
  const RectF* re = h->uiRect("province.religion");
  CHECK(re != nullptr);
  h.click(re->cx(), re->cy());
  h.key(Key::PageUp);
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(prov(h, pid)->religion, Id(0));
}
