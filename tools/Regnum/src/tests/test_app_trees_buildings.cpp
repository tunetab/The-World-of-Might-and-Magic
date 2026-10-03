// Сценарии дерева построек (ТЗ 1.h.i–ii, 1.f.ii): общее дерево и уникальные постройки государства, дорожки
// категорий, уровни (срок, стоимость, модификаторы), требования (цикл запрещён), вход из ленты и вкладки государства.
#include "app/editors/buildings.h"
#include "tests/test_app_trees_util.h"

using namespace rg;
using namespace rg::apptest;
using namespace rg::apptest::trees;

namespace {

std::vector<Id> buildingsOf(const World& w, Id owner) {
  std::vector<Id> r;
  w.buildings.each([&](const Building& b) {
    if (b.owner == owner) r.push_back(b.id);
  });
  return r;
}

Id buildingByName(const World& w, const std::string& name) {
  Id r = 0;
  w.buildings.each([&](const Building& b) {
    if (b.name == name) r = b.id;
  });
  return r;
}

bool needs(const World& w, Id b, Id req) {
  const Building* x = w.building(b);
  if (!x) return false;
  for (const BuildingReq& r : x->requires_)
    if (r.building == req) return true;
  return false;
}

// Государство с уникальной постройкой (в демонстрационном мире — одно).
Id uniqueOwner(const World& w, Id* building = nullptr) {
  Id owner = 0;
  w.buildings.each([&](const Building& b) {
    if (!owner && b.owner) {
      owner = b.owner;
      if (building) *building = b.id;
    }
  });
  return owner;
}

// Плотное общее дерево: к четырём постройкам демонстрационного мира — ещё десять с требованиями.
void denseBuildings(app::App& a) {
  CHECK(a.act("Плотное дерево построек", [&](Tx& tx) {
    Id market = 0, barracks = 0, forge = 0, quarter = 0;
    tx.w().buildings.each([&](const Building& b) {
      if (b.owner) return;
      if (b.name == "Рынок") market = b.id;
      if (b.name == "Казармы") barracks = b.id;
      if (b.name == "Кузница") forge = b.id;
      if (b.name == "Жилой квартал") quarter = b.id;
    });
    auto add = [&](const char* name, const char* icon, BuildingCat cat, int levels, std::initializer_list<std::pair<Id, int>> reqs) {
      Id id = rules::createBuilding(tx, 0, name);
      Building& b = tx.building(id);
      b.icon = icon;
      b.cat = cat;
      b.levels.clear();
      for (int l = 0; l < levels; l++) {
        BuildingLevel lv;
        lv.turns = 2 + l;
        lv.cost[kGold] = 120.0 * (l + 1);
        b.levels.push_back(lv);
      }
      for (auto [r, lvl] : reqs) b.requires_.push_back(BuildingReq{r, lvl});
      return id;
    };
    Id range = add("Стрельбище", "bow", BuildingCat::Military, 2, {{barracks, 1}});
    add("Конюшни", "horse", BuildingCat::Military, 3, {{barracks, 2}});
    add("Крепостные стены", "castle", BuildingCat::Military, 3, {});
    add("Банк", "treasury", BuildingCat::Economic, 2, {{market, 2}});
    add("Порт", "anchor", BuildingCat::Economic, 3, {});
    Id saw = add("Лесопилка", "wood", BuildingCat::Industrial, 1, {});
    Id quarry = add("Каменоломня", "stone", BuildingCat::Industrial, 2, {});
    add("Литейная", "factory", BuildingCat::Industrial, 2, {{forge, 1}, {quarry, 1}});
    add("Храм", "religion", BuildingCat::Residential, 2, {});
    add("Акведук", "house", BuildingCat::Residential, 1, {{quarter, 1}, {quarry, 2}});
    (void)range;
    (void)saw;
  }));
}

}  // namespace

TEST(app_trees_buildings_visual) {
  HideTestRegs hide;
  Harness h("trees_buildings_visual", 1600, 1000);
  h.demo();
  h.dropToasts();
  denseBuildings(h.a());
  app::openBuildingTree(h.a(), 0);
  h.settle();
  // Авторасстановка по требованиям.
  clickRect(h, "bt.layout");
  h.settle();
  h.dropToasts();
  h.settle();
  CHECK_EQ(h->ui.editor, std::string("buildings"));
  CHECK(h.shot("trees_buildings"));
  // Выбор постройки: панель свойств с уровнями.
  Id market = buildingByName(h->world(), "Рынок");
  CHECK(market != 0);
  RectF nr = rectOf(h.a(), "bt.node." + std::to_string(market));
  h.click(nr.x + nr.w * 0.5f, nr.y + nr.h * 0.3f);
  h.settle();
  CHECK(h->uiRect("bt.side.name") != nullptr);
  h.move(nr.x + nr.w * 0.5f, nr.y + nr.h * 0.3f);
  h.frames(40);
  h.settle();
  CHECK(h.shot("trees_buildings_selected"));
}

TEST(app_trees_buildings_edit) {
  HideTestRegs hide;
  Harness h("trees_buildings_edit");
  h.demo();
  h.dropToasts();
  size_t n0 = buildingsOf(h->world(), 0).size();
  app::openBuildingTree(h.a(), 0);
  h.settle();
  // Новая постройка: в общем дереве, выбрана, имя в фокусе.
  clickRect(h, "bt.add");
  std::vector<Id> list = buildingsOf(h->world(), 0);
  CHECK_EQ(list.size(), n0 + 1);
  Id b = list.back();
  CHECK_EQ(h->world().building(b)->owner, Id(0));
  CHECK(h->uiRect("bt.side.name") != nullptr);
  h.retype("Мастерская");
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(h->world().building(b)->name, std::string("Мастерская"));
  // Категория — сегментами (третий — промышленные).
  RectF cat = rectOf(h.a(), "bt.side.cat");
  h.click(cat.x + cat.w * 0.625f, cat.cy());
  h.settle();
  CHECK(h->world().building(b)->cat == BuildingCat::Industrial);
  // Уровни: добавить второй (копия первого), срок первого «+», стоимость — новый ресурс.
  CHECK_EQ(h->world().building(b)->levels.size(), size_t(1));
  RectF lv = rectOf(h.a(), "bt.side.addlevel");
  h.click(lv.right() - 21, lv.cy());
  h.settle();
  CHECK_EQ(h->world().building(b)->levels.size(), size_t(2));
  int t0 = h->world().building(b)->levels[0].turns;
  RectF tr = rectOf(h.a(), "bt.level.0.turns");
  h.click(tr.right() - 10, tr.cy());
  h.settle();
  CHECK_EQ(h->world().building(b)->levels[0].turns, t0 + 1);
  clickRect(h, "bt.level.0.addcost");
  CHECK(h->world().building(b)->levels[0].cost.count(kGold) == 1);
  CHECK_NEAR(h->world().building(b)->levels[0].cost.at(kGold), 100, 1e-9);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_buildings_levels"));
  // Удалить последний уровень — с подтверждением (панель свойств прокручена вниз).
  RectF sideAt = rectOf(h.a(), "bt.side.addlevel");
  h.wheel(sideAt.cx(), sideAt.cy(), -12);
  h.settle();
  clickRect(h, "bt.level.1.delete");
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK_EQ(h->world().building(b)->levels.size(), size_t(1));
  // Требование: от выхода «Кузницы» к «Мастерской».
  Id forge = buildingByName(h->world(), "Кузница");
  CHECK(forge != 0);
  h.key(Key::F);
  h.settle();
  RectF out = rectOf(h.a(), "bt.out." + std::to_string(forge));
  RectF nb = rectOf(h.a(), "bt.node." + std::to_string(b));
  h.drag(out.cx(), out.cy(), nb.cx(), nb.cy());
  h.settle();
  CHECK(needs(h->world(), b, forge));
  // Обратное требование — цикл, отказ.
  u64 v = h->store.version();
  RectF outB = rectOf(h.a(), "bt.out." + std::to_string(b));
  RectF nf = rectOf(h.a(), "bt.node." + std::to_string(forge));
  h.drag(outB.cx(), outB.cy(), nf.cx(), nf.cy());
  h.settle();
  CHECK(!needs(h->world(), forge, b));
  CHECK_EQ(h->store.version(), v);
  CHECK(hasToast(h.a(), "цикл", app::ToastKind::Warning));
  h->toasts().clear();
  // Перетаскивание на другую дорожку меняет категорию (одно действие, отменяется).
  nb = rectOf(h.a(), "bt.node." + std::to_string(b));
  const RectF* cv = h->uiRect("bt.canvas");
  CHECK(cv != nullptr);
  BuildingCat c0 = h->world().building(b)->cat;
  float dy = c0 == BuildingCat::Residential ? -nb.h * 2.2f : nb.h * 2.2f;
  h.drag(nb.x + nb.w * 0.5f, nb.y + nb.h * 0.25f, nb.x + nb.w * 0.5f, nb.y + nb.h * 0.25f + dy);
  h.settle();
  CHECK(h->world().building(b)->cat != c0);
  undo(h);
  CHECK(h->world().building(b)->cat == c0);
  // Удаление постройки — с подтверждением, требования к ней снимаются.
  clickRect(h, "bt.node." + std::to_string(forge), 0.5f, 0.25f);
  h.key(Key::Delete);
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK(h->world().building(forge) == nullptr);
  CHECK(!needs(h->world(), b, forge));
  undo(h);
  CHECK(h->world().building(forge) != nullptr);
}

TEST(app_trees_buildings_unique_and_entries) {
  HideTestRegs hide;
  Harness h("trees_buildings_unique");
  h.demo();
  Id uniq = 0;
  Id owner = uniqueOwner(h->world(), &uniq);
  CHECK(owner != 0 && uniq != 0);
  // Лента: «Дерево построек» — общее дерево по категориям.
  CHECK(h.clickUi("drawer.buildings"));
  h.settle();
  CHECK_EQ(h->ui.drawer, std::string("buildings"));
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_buildings_drawer"));
  Id market = buildingByName(h->world(), "Рынок");
  clickRect(h, "drawer.buildings.item." + std::to_string(market));
  CHECK_EQ(h->ui.editor, std::string("buildings"));
  CHECK_EQ(h->ui.editorArg, Id(0));
  CHECK(h->uiRect("bt.side.name") != nullptr);
  // Уникальная постройка в общем дереве не показывается.
  CHECK(h->uiRect("bt.node." + std::to_string(uniq)) == nullptr);
  h.key(Key::Escape);
  // Вкладка государства «Постройки».
  h->ui.tabOf[app::SelType::Faction] = "faction.buildings";
  h->select(app::SelType::Faction, owner);
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_buildings_faction_tab"));
  clickRect(h, "faction.buildings.unique");
  CHECK_EQ(h->ui.editor, std::string("buildings"));
  CHECK_EQ(h->ui.editorArg, owner);
  h.settle();
  CHECK(h->uiRect("bt.node." + std::to_string(uniq)) != nullptr);
  CHECK(h->uiRect("bt.node." + std::to_string(market)) == nullptr);
  // Новая постройка здесь — уникальная для государства.
  clickRect(h, "bt.add");
  std::vector<Id> mine = buildingsOf(h->world(), owner);
  CHECK_EQ(mine.size(), size_t(2));
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_buildings_unique"));
  // Уникальная постройка доступна только провинциям владельца.
  bool inOwn = false, inOther = false;
  h->world().provinces.each([&](const Province& p) {
    if (p.sea || !p.owner) return;
    for (const rules::BuildOption& o : rules::buildOptions(h->world(), p.id))
      if (o.building == uniq) (p.owner == owner ? inOwn : inOther) = true;
  });
  CHECK(inOwn);
  CHECK(!inOther);
}

TEST(app_trees_buildings_layout_external_icon) {
  HideTestRegs hide;
  Harness h("trees_buildings_external", 1600, 1000);
  h.demo();
  h.dropToasts();
  denseBuildings(h.a());
  // Авторасстановка: в дорожке категории карточки не перекрываются.
  app::openBuildingTree(h.a(), 0);
  h.settle();
  clickRect(h, "bt.layout");
  std::vector<Id> all = buildingsOf(h->world(), 0);
  for (size_t i = 0; i < all.size(); i++)
    for (size_t j = i + 1; j < all.size(); j++) {
      const Building* x = h->world().building(all[i]);
      const Building* y = h->world().building(all[j]);
      if (x->cat == y->cat) CHECK(!(x->pos == y->pos));
    }
  undo(h);
  // Значок постройки — из набора (подписи по-русски).
  Id market = buildingByName(h->world(), "Рынок");
  clickRect(h, "bt.node." + std::to_string(market), 0.5f, 0.3f);
  clickRect(h, "bt.side.icon");
  CHECK(h->uiRect("bt.icon.castle") != nullptr);
  clickRect(h, "bt.icon.castle");
  CHECK_EQ(h->world().building(market)->icon, std::string("castle"));
  undo(h);
  CHECK(h->world().building(market)->icon != "castle");
  // Уровень требования: «Банк» требует «Рынок» II — поле уровня в списке требований.
  Id bank = buildingByName(h->world(), "Банк");
  h.key(Key::Escape);
  app::openBuildingTree(h.a(), 0, bank);
  h.settle();
  RectF lv = rectOf(h.a(), "bt.req." + std::to_string(market) + ".level");
  h.click(lv.right() - 39, lv.cy());   // «−» поля
  h.settle();
  int level = 0;
  for (const BuildingReq& r : h->world().building(bank)->requires_)
    if (r.building == market) level = r.level;
  CHECK_EQ(level, 1);
  undo(h);
  // Уникальная постройка требует общую («Казармы» II): плашка слева от карточки, щелчок — общее дерево.
  Id uniq = 0;
  Id owner = uniqueOwner(h->world(), &uniq);
  Id barracks = buildingByName(h->world(), "Казармы");
  CHECK(owner && uniq && barracks);
  CHECK(h->act("Требование", [&](Tx& tx) { tx.building(uniq).requires_.push_back(BuildingReq{barracks, 2}); }));
  h.key(Key::Escape);
  app::openBuildingTree(h.a(), owner, uniq);
  h.settle();
  std::string ext = "bt.ext." + std::to_string(uniq) + "." + std::to_string(barracks);
  CHECK(h->uiRect(ext) != nullptr);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_buildings_external"));
  clickRect(h, ext);
  CHECK_EQ(h->ui.editor, std::string("buildings"));
  CHECK_EQ(h->ui.editorArg, Id(0));
  h.settle();
  // В общем дереве выбраны «Казармы»: Delete спрашивает об удалении именно их.
  CHECK(h->uiRect("bt.side.delete") != nullptr);
  h.key(Key::Delete);
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(h->dialogStack().back()->style(h.a()).title.find("Казармы") != std::string::npos);
  CHECK(h.clickUi("dialog.cancel"));
  h.settle();
  CHECK(h->world().building(barracks) != nullptr);
}

TEST(app_trees_buildings_faction_built) {
  HideTestRegs hide;
  Harness h("trees_buildings_faction_built");
  h.demo();
  h.dropToasts();
  // Государство с постройками в провинциях: раздел «В провинциях» — щелчок открывает дерево с постройкой.
  Id fid = 0, bid = 0;
  h->world().provinces.each([&](const Province& p) {
    const Faction* f = h->world().faction(p.owner);
    if (fid || !f || !f->isState()) return;
    for (const ProvBuilding& pb : p.buildings)
      if (!fid && pb.builtLevel() > 0) {
        fid = p.owner;
        bid = pb.building;
      }
  });
  CHECK(fid && bid);
  h->ui.tabOf[app::SelType::Faction] = "faction.buildings";
  h->select(app::SelType::Faction, fid);
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_buildings_faction_built"));
  clickRect(h, "faction.buildings.built." + std::to_string(bid));
  CHECK_EQ(h->ui.editor, std::string("buildings"));
  CHECK_EQ(h->ui.editorArg, h->world().building(bid)->owner);
  CHECK(h->uiRect("bt.side.name") != nullptr);
}
