// Сценарии строительства в провинции (ТЗ 1.f.i–ii): слоты, выбор постройки (нехватка ресурсов — недоступно),
// начало строительства списывает стоимость, отмена возвращает её, завершение по ходам, улучшение, снос,
// уникальные постройки — только у своего государства.
#include "app/editors/buildings.h"
#include "tests/test_app_trees_util.h"

using namespace rg;
using namespace rg::apptest;
using namespace rg::apptest::trees;

namespace {

const ProvBuilding* pbOf(const World& w, Id pid, Id bid) {
  const Province* p = w.province(pid);
  if (!p) return nullptr;
  for (const ProvBuilding& pb : p->buildings)
    if (pb.building == bid) return &pb;
  return nullptr;
}

// Провинция государства с двумя свободными слотами и постройкой общего дерева, которой в ней нет (многоуровневой).
struct Pick {
  Id pid = 0, owner = 0, fresh = 0, built = 0;
};
Pick pickProvince(const World& w) {
  Pick r;
  auto calc = rules::calc(w);
  w.provinces.each([&](const Province& p) {
    if (r.pid || p.sea || !p.owner) return;
    const Faction* f = w.faction(p.owner);
    if (!f || !f->isState()) return;
    const rules::ProvinceCalc* pc = calc->province(p.id);
    if (!pc || pc->slots < int(p.buildings.size()) + 2) return;
    Id fresh = 0, built = 0;
    w.buildings.each([&](const Building& b) {
      if (b.owner || b.levels.size() < 2) return;
      bool have = false;
      for (const ProvBuilding& pb : p.buildings) have = have || pb.building == b.id;
      if (!have && !fresh && b.requires_.empty()) fresh = b.id;
    });
    for (const ProvBuilding& pb : p.buildings)
      if (!pb.constructing && !built) built = pb.building;
    if (fresh && built) r = Pick{p.id, p.owner, fresh, built};
  });
  return r;
}

void setStock(app::App& a, Id faction, double v) {
  CHECK(a.act("Запасы", [&](Tx& tx) {
    Faction& f = tx.faction(faction);
    f.res[kGold] = v;
    for (const CatalogItem& c : tx.w().catalogs->resources) f.res[c.id] = v;
  }));
}

void openTab(Harness& h, Id pid) {
  h->ui.tabOf[app::SelType::Province] = "province.buildings";
  h->select(app::SelType::Province, pid);
  h.settle();
}

// Открыть выбор строительства и оставить в списке одну постройку (поиск по названию).
void openPicker(Harness& h, const std::string& name) {
  clickRect(h, "prov.build");
  CHECK(h->hasDialog("build.picker"));
  clickRect(h, "build.search");
  h.type(name);
  h.settle();
}

}  // namespace

TEST(app_trees_construction_flow) {
  HideTestRegs hide;
  Harness h("trees_construction", 1440, 1200);
  h.demo();
  h.dropToasts();
  Pick pk = pickProvince(h->world());
  CHECK(pk.pid != 0);
  const std::string freshName = h->world().building(pk.fresh)->name;
  openTab(h, pk.pid);
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK_EQ(h->ui.tabOf[app::SelType::Province], std::string("province.buildings"));
  CHECK(h.shot("trees_province_buildings"));

  // Нехватка ресурсов — постройка недоступна: кнопка не действует, мир прежний.
  setStock(h.a(), pk.owner, 0);
  h.settle();
  openPicker(h, freshName);
  CHECK(h->uiRect("build.option." + std::to_string(pk.fresh)) != nullptr);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_build_picker_lack"));
  u64 v = h->store.version();
  clickRect(h, "build.option." + std::to_string(pk.fresh));
  CHECK_EQ(h->store.version(), v);
  CHECK(pbOf(h->world(), pk.pid, pk.fresh) == nullptr);
  h.key(Key::Escape);
  h.settle();
  CHECK(!h->hasDialog("build.picker"));

  // Ресурсов достаточно — строительство начато, стоимость списана.
  setStock(h.a(), pk.owner, 10000);
  std::map<Id, double> cost;
  int turns = 0;
  for (const rules::BuildOption& o : rules::buildOptions(h->world(), pk.pid))
    if (o.building == pk.fresh) {
      cost = o.cost;
      turns = o.turns;
      CHECK(o.can);
    }
  CHECK(!cost.empty());
  h.settle();
  clickRect(h, "prov.build");
  CHECK(h->hasDialog("build.picker"));
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_build_picker"));
  CHECK(h.clickUi("dialog.cancel"));
  h.settle();
  openPicker(h, freshName);
  clickRect(h, "build.option." + std::to_string(pk.fresh));
  CHECK(!h->hasDialog("build.picker"));
  const ProvBuilding* pb = pbOf(h->world(), pk.pid, pk.fresh);
  CHECK(pb != nullptr);
  CHECK(pb->constructing);
  CHECK_EQ(pb->left, turns);
  for (auto& [res, c] : cost) CHECK_NEAR(h->world().faction(pk.owner)->stock(res), 10000 - c, 1e-6);
  h->toasts().clear();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_province_constructing"));

  // Отмена — стоимость возвращается полностью (с подтверждением).
  clickRect(h, "prov.cancel." + std::to_string(pk.fresh));
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK(pbOf(h->world(), pk.pid, pk.fresh) == nullptr);
  for (auto& [res, c] : cost) CHECK_NEAR(h->world().faction(pk.owner)->stock(res), 10000, 1e-6);
  // Отмена отмены — снова строится; ходы доводят до конца.
  undo(h);
  CHECK(pbOf(h->world(), pk.pid, pk.fresh) != nullptr);
  for (int t = 0; t < turns; t++) {
    CHECK(pbOf(h->world(), pk.pid, pk.fresh)->constructing);
    CHECK(h->endTurnNow());
    h->toasts().clear();   // уведомления хода не держат кадры
  }
  pb = pbOf(h->world(), pk.pid, pk.fresh);
  CHECK(pb != nullptr && !pb->constructing);
  CHECK_EQ(pb->level, 1);
  h.settle();

  // Улучшение до второго уровня — стоимость следующего уровня.
  setStock(h.a(), pk.owner, 10000);
  std::map<Id, double> up;
  for (const rules::BuildOption& o : rules::buildOptions(h->world(), pk.pid))
    if (o.building == pk.fresh) {
      CHECK(o.upgrade);
      CHECK_EQ(o.level, 2);
      up = o.cost;
    }
  h.settle();
  clickRect(h, "prov.upgrade." + std::to_string(pk.fresh));
  pb = pbOf(h->world(), pk.pid, pk.fresh);
  CHECK(pb && pb->constructing && pb->level == 2);
  for (auto& [res, c] : up) CHECK_NEAR(h->world().faction(pk.owner)->stock(res), 10000 - c, 1e-6);
  h->toasts().clear();

  // Снос готовой постройки — с подтверждением; отменяется Ctrl+Z.
  h.settle();
  clickRect(h, "prov.demolish." + std::to_string(pk.built));
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK(pbOf(h->world(), pk.pid, pk.built) == nullptr);
  undo(h);
  CHECK(pbOf(h->world(), pk.pid, pk.built) != nullptr);
}

TEST(app_trees_construction_slots_and_unique) {
  HideTestRegs hide;
  Harness h("trees_construction_unique", 1440, 1000);
  h.demo();
  h.dropToasts();
  // Уникальная постройка и её государство.
  Id uniq = 0, owner = 0;
  h->world().buildings.each([&](const Building& b) {
    if (!uniq && b.owner) {
      uniq = b.id;
      owner = b.owner;
    }
  });
  CHECK(uniq && owner);
  const std::string uname = h->world().building(uniq)->name;
  // Провинция владельца (без этой постройки, со свободным слотом) и провинция другого государства.
  auto calc = rules::calc(h->world());
  Id own = 0, other = 0;
  h->world().provinces.each([&](const Province& p) {
    if (p.sea || !p.owner) return;
    const rules::ProvinceCalc* pc = calc->province(p.id);
    if (!pc || pc->slots <= int(p.buildings.size())) return;
    bool has = false;
    for (const ProvBuilding& pb : p.buildings) has = has || pb.building == uniq;
    if (p.owner == owner && !has && !own) own = p.id;
    const Faction* f = h->world().faction(p.owner);
    if (p.owner != owner && f && f->isState() && !other) other = p.id;
  });
  CHECK(own && other);
  setStock(h.a(), owner, 10000);
  openTab(h, own);
  openPicker(h, uname);
  CHECK(h->uiRect("build.option." + std::to_string(uniq)) != nullptr);
  CHECK(h.clickUi("dialog.cancel"));
  h.settle();
  CHECK(!h->hasDialog("build.picker"));
  openTab(h, other);
  openPicker(h, uname);
  CHECK(h->uiRect("build.option." + std::to_string(uniq)) == nullptr);
  CHECK(h.clickUi("dialog.cancel"));
  h.settle();
  CHECK(!h->hasDialog("build.picker"));
  // Свободный слот в сетке — тоже вход в выбор строительства.
  int used = int(h->world().province(other)->buildings.size());
  clickRect(h, "prov.slot." + std::to_string(used));
  CHECK(h->hasDialog("build.picker"));
  CHECK(h.clickUi("dialog.cancel"));
  h.settle();
  // Переполнение слотов (модификатор отнял слоты): предупреждение, «Построить» недоступно.
  Id crowded = 0;
  h->world().provinces.each([&](const Province& p) {
    const Faction* f = h->world().faction(p.owner);
    if (!crowded && !p.sea && f && f->isState() && p.buildings.size() >= 2) crowded = p.id;
  });
  CHECK(crowded != 0);
  CHECK(h->act("Минус слоты", [&](Tx& tx) {
    Id m = rules::createModifier(tx, "Разруха");
    Modifier& md = tx.modifier(m);
    md.fx[size_t(int(Fx::Slots))] = -6;
    md.fxMask |= 1u << int(Fx::Slots);
    tx.province(crowded).modifiers.push_back(m);
  }));
  openTab(h, crowded);
  CHECK(rules::calc(h->world())->province(crowded)->slots < int(h->world().province(crowded)->buildings.size()));
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_province_overfull"));
  clickRect(h, "prov.build");
  CHECK(!h->hasDialog("build.picker"));
}

TEST(app_trees_construction_sea_owner_filters) {
  HideTestRegs hide;
  Harness h("trees_construction_filters", 1440, 1000);
  h.demo();
  h.dropToasts();
  // Морская провинция — вкладки «Постройки» нет (ТЗ 1.a.iii: без информации); сухопутная — есть.
  const app::TabDef* tab = tabDef("province.buildings");
  CHECK(tab != nullptr && tab->visible != nullptr);
  Id sea = 0, land = 0;
  h->world().provinces.each([&](const Province& p) {
    if (p.sea && !sea) sea = p.id;
    if (!p.sea && p.owner && !land) land = p.id;
  });
  CHECK(sea && land);
  CHECK(!tab->visible(h.a(), sea));
  CHECK(tab->visible(h.a(), land));
  // Выбор строительства по имени диалога для морской провинции сразу закрывается.
  CHECK(h->openDialog("build.picker", sea));
  h.settle();
  CHECK(!h->hasDialog("build.picker"));
  // «Только доступные»: при пустой казне постройки с ценой скрываются; без фильтра видны с причинами.
  Pick pk = pickProvince(h->world());
  CHECK(pk.pid != 0);
  setStock(h.a(), pk.owner, 0);
  openTab(h, pk.pid);
  clickRect(h, "prov.build");
  CHECK(h->hasDialog("build.picker"));
  CHECK(h->uiRect("build.option." + std::to_string(pk.fresh)) != nullptr);
  clickRect(h, "build.onlyAvail", 0.9f, 0.5f);
  CHECK(h->uiRect("build.option." + std::to_string(pk.fresh)) == nullptr);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_build_picker_only_avail"));
  CHECK(h.clickUi("dialog.cancel"));
  h.settle();
  // Провинция без владельца: строить некому — «Построить» недоступна.
  CHECK(h->act("Без владельца", [&](Tx& tx) { rules::setProvinceOwner(tx, pk.pid, 0); }));
  h.settle();
  clickRect(h, "prov.build");
  CHECK(!h->hasDialog("build.picker"));
  for (const rules::BuildOption& o : rules::buildOptions(h->world(), pk.pid)) CHECK(!o.can);
}
