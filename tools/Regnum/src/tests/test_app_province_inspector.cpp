// Сценарии инспектора провинции на демонстрационном мире: шапка (переименование, меню, удаление с подтверждением),
// смена владельца (цвет карты, отмена), галочка «Морская провинция», величина и тип города (слоты), оккупация,
// снимки вкладки «Обзор» (тёмная и светлая темы) и морской провинции.
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

// Тестовые регистрации других сценариев («test.*») на время сценария убираются из реестров.
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

// Выбрать провинцию щелчком по карте и открыть вкладку.
Id openProvince(Harness& h, const char* tab, Id skip = 0) {
  auto vp = h.visibleProvince(skip);
  CHECK(vp.has_value());
  h->ui.tabOf[app::SelType::Province] = tab;
  h.click(vp->second.x, vp->second.y);
  h.settle();
  CHECK(h->ui.sel == (app::Selection{app::SelType::Province, vp->first}));
  return vp->first;
}

bool clickMark(Harness& h, const std::string& name) { return h.clickUi(name); }

// Доля пикселей кольца вокруг точки, отличающихся от снимка before.
int changedPixels(Harness& h, gfx::Pt c, const std::vector<u32>& before) {
  int n = 0, k = 0;
  for (int dy = -24; dy <= 24; dy += 8)
    for (int dx = -24; dx <= 24; dx += 8) {
      if (h.pixel(c.x + float(dx), c.y + float(dy)) != before[size_t(k)]) n++;
      k++;
    }
  return n;
}
std::vector<u32> sample(Harness& h, gfx::Pt c) {
  std::vector<u32> v;
  for (int dy = -24; dy <= 24; dy += 8)
    for (int dx = -24; dx <= 24; dx += 8) v.push_back(h.pixel(c.x + float(dx), c.y + float(dy)));
  return v;
}

}  // namespace

TEST(app_province_owner_change_and_undo) {
  HideTestRegs hide;
  Harness h("province_owner");
  h.demo();
  h.waitMap();
  auto vp = h.visibleProvince();
  CHECK(vp.has_value());
  Id pid = openProvince(h, "province.overview");
  CHECK_EQ(pid, vp->first);
  CHECK(h->uiRect("province.owner") != nullptr);
  CHECK(h->uiRect("province.ownerPill") != nullptr);
  Id oldOwner = prov(h, pid)->owner;
  CHECK(oldOwner != 0);
  // Новый владелец — другое государство; в списке государства по алфавиту после пункта «нет».
  const World& w0 = h->world();
  std::vector<const Faction*> states;
  w0.factions.each([&](const Faction& f) {
    if (f.isState()) states.push_back(&f);
  });
  std::sort(states.begin(), states.end(), [](const Faction* x, const Faction* y) { return compareRu(x->name, y->name) < 0; });
  int targetRow = -1;
  for (size_t i = 0; i < states.size() && targetRow < 0; i++)
    if (states[i]->id != oldOwner) targetRow = int(i);
  CHECK(targetRow >= 0);
  Id target = states[size_t(targetRow)]->id;
  // Карта под выделением до смены.
  h.waitMap();
  h.dropToasts();
  h.settle();
  gfx::Pt at = vp->second;
  std::vector<u32> before = sample(h, at);
  // Выпадающий список владельца: к началу, вниз до нужного государства, Enter.
  CHECK(clickMark(h, "province.owner"));
  h.key(Key::PageUp);
  for (int i = 0; i <= targetRow; i++) h.key(Key::Down);
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(prov(h, pid)->owner, target);
  CHECK_EQ(h->store.undoLabel(), std::string("Владелец провинции"));
  h.waitMap();
  h.settle();
  CHECK(changedPixels(h, at, before) >= 10);   // заливка перекрашена цветом нового владельца
  // Шапка показывает нового владельца.
  CHECK(h->uiRect("province.ownerPill") != nullptr);
  // Отмена возвращает владельца и цвет.
  h.key(Key::Z, ctrl());
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK_EQ(prov(h, pid)->owner, oldOwner);
  CHECK(changedPixels(h, at, before) <= 4);
  // «Без владельца» — пункт «нет» в начале списка.
  CHECK(clickMark(h, "province.owner"));
  h.key(Key::PageUp);
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(prov(h, pid)->owner, Id(0));
  h.key(Key::Z, ctrl());
  CHECK_EQ(prov(h, pid)->owner, oldOwner);
}

TEST(app_province_sea_checkbox_hides_info) {
  HideTestRegs hide;
  Harness h("province_sea");
  h.demo();
  Id pid = openProvince(h, "province.overview");
  Province before = *prov(h, pid);
  CHECK(!before.sea);
  CHECK(h->uiRect("province.owner") != nullptr);
  CHECK(h->uiRect("province.sea") != nullptr);
  CHECK(clickMark(h, "province.sea"));
  h.settle();
  CHECK(prov(h, pid)->sea);
  // Вкладки сведений скрыты, видна только заметка о море; данные сохранены.
  for (const char* id : {"province.overview", "province.population", "province.economy", "province.modifiers"}) {
    for (auto& t : app::tabs())
      if (std::string_view(t.id) == id) CHECK(t.visible && !t.visible(h.a(), pid));
  }
  CHECK(h->uiRect("province.seanote") != nullptr);
  CHECK(h->uiRect("province.owner") == nullptr);
  CHECK(h->uiRect("province.ownerPill") == nullptr);
  CHECK_EQ(prov(h, pid)->owner, before.owner);
  CHECK_EQ(prov(h, pid)->races.size(), before.races.size());
  // Морская провинция не участвует в расчётах (нет населения и восстания).
  auto calc = rules::calc(h->world());
  CHECK(calc->province(pid) && calc->province(pid)->sea);
  CHECK_EQ(calc->province(pid)->population, i64(0));
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("province_sea"));
  // Обратно — кнопкой в заметке: сведения возвращаются.
  const RectF* note = h->uiRect("province.seanote");
  CHECK(note != nullptr);
  h.click(note->cx(), note->bottom() - 31);   // кнопка внизу пустого состояния
  h.settle();
  CHECK(!prov(h, pid)->sea);
  CHECK_EQ(prov(h, pid)->owner, before.owner);
  CHECK(h->uiRect("province.seanote") == nullptr);
  // Отмена — снова морская, ещё раз — сухопутная.
  h.key(Key::Z, ctrl());
  CHECK(prov(h, pid)->sea);
  h.key(Key::Z, ctrl());
  CHECK(!prov(h, pid)->sea);
}

TEST(app_province_overview_fields) {
  HideTestRegs hide;
  Harness h("province_overview", 1440, 1180);
  h.demo();
  Id pid = openProvince(h, "province.overview");
  const Province* p = prov(h, pid);
  // Величина: «Большая» — третий сегмент; слоты пересчитаны.
  const RectF* sz = h->uiRect("province.size");
  CHECK(sz != nullptr);
  int slots0 = rules::calc(h->world())->province(pid)->slots;
  int was = int(p->size);
  h.click(sz->x + sz->w * (2.5f / 3), sz->cy());
  h.settle();
  CHECK_EQ(int(prov(h, pid)->size), int(ProvSize::Large));
  int slots1 = rules::calc(h->world())->province(pid)->slots;
  CHECK_EQ(slots1 - slots0, schema::kProvSizes[2].value - schema::kProvSizes[was].value);
  // Тип города: «Аванпост» — первый сегмент.
  const RectF* ct = h->uiRect("province.city");
  CHECK(ct != nullptr);
  h.click(ct->x + ct->w * (0.5f / 4), ct->cy());
  h.settle();
  CHECK_EQ(int(prov(h, pid)->city), int(CityType::Outpost));
  // Столица: ввод и Enter.
  const RectF* cap = h->uiRect("province.capital");
  CHECK(cap != nullptr);
  h.click(cap->cx(), cap->cy());
  h.retype("Новоград");
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(prov(h, pid)->capital, std::string("Новоград"));
  // Оккупация: переключатель включает с оккупантом по умолчанию (не владелец).
  if (prov(h, pid)->occupied) {
    CHECK(clickMark(h, "province.occupied"));
    h.settle();
  }
  CHECK(!prov(h, pid)->occupied);
  CHECK(clickMark(h, "province.occupied"));
  h.settle();
  CHECK(prov(h, pid)->occupied);
  CHECK(prov(h, pid)->occupier != 0 && prov(h, pid)->occupier != prov(h, pid)->owner);
  CHECK(h->uiRect("province.occupierPill") != nullptr);
  // Отмена шаг за шагом.
  h.key(Key::Z, ctrl());
  CHECK(!prov(h, pid)->occupied);
  h.key(Key::Z, ctrl());
  CHECK(prov(h, pid)->capital != "Новоград");
  h.key(Key::Z, ctrl());
  CHECK_EQ(int(prov(h, pid)->city), int(p->city));
  h.key(Key::Z, ctrl());
  CHECK_EQ(int(prov(h, pid)->size), was);
}

TEST(app_province_rename_and_menu) {
  HideTestRegs hide;
  Harness h("province_rename");
  h.demo();
  Id pid = openProvince(h, "province.overview");
  std::string old = prov(h, pid)->name;
  // F2 — переименование на месте.
  h.key(Key::F2);
  h.settle();
  CHECK(h->uiRect("province.rename") != nullptr);
  h.retype("Златоземье");
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(prov(h, pid)->name, std::string("Златоземье"));
  CHECK(h->uiRect("province.rename") == nullptr);
  // Esc отменяет правку.
  const RectF* nm = h->uiRect("province.name");
  CHECK(nm != nullptr);
  h.doubleClick(nm->x + 20, nm->cy());
  h.settle();
  CHECK(h->uiRect("province.rename") != nullptr);
  h.retype("Не то");
  h.key(Key::Escape);
  h.settle();
  CHECK_EQ(prov(h, pid)->name, std::string("Златоземье"));
  CHECK(h->uiRect("province.rename") == nullptr);
  h.key(Key::Z, ctrl());
  CHECK_EQ(prov(h, pid)->name, old);
  // Меню: копировать название.
  CHECK(clickMark(h, "province.more"));
  h.settle();
  CHECK(h->uiRect("province.menu.copy") != nullptr);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("province_menu"));
  CHECK(clickMark(h, "province.menu.copy"));
  h.settle();
  CHECK_EQ(platform::clipboardText(), old);
  // Меню: удалить — только в режиме правки границ (ТЗ 1.a.ii); подтверждение, удаление, отмена.
  CHECK(!app::runCommand(h.a(), "province.delete"));
  h.settle();
  CHECK(!h->hasDialog("confirm"));
  h->setEditBorders(true);
  h.settle();
  CHECK(clickMark(h, "province.more"));
  h.settle();
  CHECK(clickMark(h, "province.menu.delete"));
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(clickMark(h, "dialog.ok"));
  h.settle();
  CHECK(prov(h, pid) == nullptr);
  CHECK(!h->ui.sel);
  h.key(Key::Z, ctrl());
  h.settle();
  CHECK(prov(h, pid) != nullptr);
  CHECK_EQ(prov(h, pid)->name, old);
  // Delete при выбранной провинции — тоже подтверждение; «Отмена» ничего не меняет.
  h->select(app::SelType::Province, pid);
  h.settle();
  h.key(Key::Delete);
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(clickMark(h, "dialog.cancel"));
  h.settle();
  CHECK(prov(h, pid) != nullptr);
  // Команда «Показать провинцию» доступна при выбранной провинции.
  CHECK(app::runCommand(h.a(), "province.show"));
}

TEST(app_province_overview_screens) {
  HideTestRegs hide;
  {
    Harness h("province_shot_dark", 1440, 1000);
    h.demo();
    // Оккупированная провинция с владельцем — видны обе фишки.
    Id pid = 0;
    h->world().provinces.each([&](const Province& p) {
      if (!pid && p.occupied && p.owner && !p.sea) pid = p.id;
    });
    CHECK(pid != 0);
    h->ui.tabOf[app::SelType::Province] = "province.overview";
    h->select(app::SelType::Province, pid, true);
    h.settle();
    h.waitMap();
    h.dropToasts();
    h.settle();
    CHECK(h->uiRect("province.occupierPill") != nullptr);
    CHECK(h.shot("province_overview"));
    // Подсказка-разбивка восстания: довольство × −0,5 и модификаторы.
    const RectF* reb = h->uiRect("province.overview.rebellion");
    CHECK(reb != nullptr);
    h.move(reb->cx(), reb->cy());
    for (int i = 0; i < 40; i++) h.frame();
    h.settle();
    CHECK(h.shot("province_tooltip"));
    // Низ вкладки: оккупация, заметки, карточка кампании.
    const RectF* ins = h->uiRect("inspector");
    CHECK(ins != nullptr);
    h.wheel(ins->cx(), ins->bottom() - 100, -16);
    h.move(ins->x - 200, ins->cy());
    h.settle();
    CHECK(h->uiRect("province.notes") != nullptr);
    CHECK(h->uiRect("province.entity") != nullptr);
    CHECK(h.shot("province_overview_bottom"));
  }
  {
    Harness h("province_shot_light", 1440, 1000, 1, false);
    h.demo();
    Id pid = openProvince(h, "province.overview");
    (void)pid;
    h.waitMap();
    h.dropToasts();
    h.settle();
    CHECK(h.shot("province_overview_light"));
  }
}

TEST(app_province_read_only_history) {
  HideTestRegs hide;
  Harness h("province_readonly");
  h.demo();
  CHECK(h->endTurnNow());
  h.dropToasts();
  h.settle();
  CHECK(h->viewTurn(1));
  h.settle();
  CHECK(h->readOnly());
  Id pid = openProvince(h, "province.overview");
  // Прошлый ход: правка недоступна — нет карандаша, галочка и команды не меняют мир.
  CHECK(h->uiRect("province.name") != nullptr);
  CHECK(h->uiRect("province.renameBtn") == nullptr);
  bool sea0 = prov(h, pid)->sea;
  CHECK(clickMark(h, "province.sea"));
  h.settle();
  CHECK_EQ(prov(h, pid)->sea, sea0);
  h.key(Key::F2);
  h.settle();
  CHECK(h->uiRect("province.rename") == nullptr);
  h.key(Key::Delete);
  h.settle();
  CHECK(!h->hasDialog("confirm"));
  // Владельца не сменить: список недоступен.
  Id owner = prov(h, pid)->owner;
  CHECK(clickMark(h, "province.owner"));
  h.key(Key::PageUp);
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(prov(h, pid)->owner, owner);
  h->backToCurrent();
  h.settle();
  CHECK(!h->readOnly());
  CHECK(h->uiRect("province.renameBtn") != nullptr);
}
