// Аудит (tz-build-mods-turn): строительство (ТЗ 1.f), модификаторы (ТЗ 1.g), дерево построек (ТЗ 1.h) и механика хода.
// Каждый сценарий воспроизводит найденный дефект. Ожидаемое поведение проверяется строго только при REGNUM_AUDIT=1;
// без переменной расхождение лишь печатается, чтобы общий набор тестов не падал до исправления.
#include <cstdio>
#include <cstdlib>

#include "app/dialogs/turn_ui.h"
#include "app/editors/buildings.h"
#include "app/widgets.h"
#include "tests/test_app_trees_util.h"

using namespace rg;
using namespace rg::apptest;
using namespace rg::apptest::trees;

namespace {

[[maybe_unused]] bool auditStrict() {
  const char* v = std::getenv("REGNUM_AUDIT");
  return v && *v == '1';
}

// Ожидание аудита: при REGNUM_AUDIT=1 — обычная проверка, иначе — запись о дефекте в вывод теста.
#define AUDIT_EXPECT(cond, msg)                                                                         \
  do {                                                                                                  \
    if (!(cond)) {                                                                                      \
      if (auditStrict()) ::rg::test::fail(__FILE__, __LINE__, std::string("AUDIT: ") + (msg));          \
      else std::fprintf(stderr, "AUDIT DEFECT (%s:%d): %s\n", __FILE__, __LINE__, std::string(msg).c_str()); \
    }                                                                                                   \
  } while (0)

// Провинция государства со свободным слотом и общая постройка без требований, которой в ней нет; цена — не нулевая.
struct BuildPick {
  Id pid = 0, owner = 0, building = 0;
};
BuildPick pickBuild(const World& w) {
  BuildPick r;
  auto calc = rules::calc(w);
  w.provinces.each([&](const Province& p) {
    if (r.pid || p.sea || !p.owner) return;
    const Faction* f = w.faction(p.owner);
    if (!f || !f->isState()) return;
    const rules::ProvinceCalc* pc = calc->province(p.id);
    if (!pc || pc->slots <= int(p.buildings.size())) return;
    w.buildings.each([&](const Building& b) {
      if (r.pid || b.owner || !b.requires_.empty() || b.levels.empty() || b.levels[0].cost.empty()) return;
      for (const ProvBuilding& pb : p.buildings)
        if (pb.building == b.id) return;
      r = BuildPick{p.id, p.owner, b.id};
    });
  });
  return r;
}

void fillStock(app::App& a, Id faction, double v) {
  CHECK(a.act("Запасы", [&](Tx& tx) {
    Faction& f = tx.faction(faction);
    f.res[kGold] = v;
    for (const CatalogItem& c : tx.w().catalogs->resources) f.res[c.id] = v;
  }));
}

double stockSum(const World& w, Id faction) {
  const Faction* f = w.faction(faction);
  double s = f->treasury();
  for (const CatalogItem& c : w.catalogs->resources)
    if (c.id != kGold) s += f->stock(c.id);
  return s;
}

Id addCostModifier(app::App& a, Id pid, double pct) {
  Id mid = 0;
  CHECK(a.act("Стоимость строительства", [&](Tx& tx) {
    mid = rules::createModifier(tx, "Аудит: стоимость");
    Modifier& m = tx.modifier(mid);
    m.fxMask |= 1u << int(Fx::BuildCostPct);
    m.fx[size_t(int(Fx::BuildCostPct))] = pct;
    tx.province(pid).modifiers.push_back(mid);
  }));
  return mid;
}

}  // namespace

// ТЗ 1.f.ii / RULES §9: отмена строительства должна возвращать ровно уплаченное. Возврат считается по ТЕКУЩЕМУ
// множителю стоимости провинции (rules/build.cpp cancelBuilding → levelCost(..., ctxOf(...).factor)), поэтому:
// начать при −100 % (бесплатно), снять модификатор, отменить — государство получает полную цену из ничего.
TEST(audit_tbmt_cancel_refund_not_equal_paid) {
  Harness h("audit_tbmt_refund");
  h.demo();
  BuildPick pk = pickBuild(h->world());
  CHECK(pk.pid != 0);
  fillStock(h.a(), pk.owner, 10000);
  Id mid = addCostModifier(h.a(), pk.pid, -100);   // строительство бесплатно
  const double before = stockSum(h->world(), pk.owner);
  CHECK(h->act("Построить", [&](Tx& tx) { rules::startBuilding(tx, pk.pid, pk.building); }));
  const double paid = before - stockSum(h->world(), pk.owner);
  CHECK_NEAR(paid, 0, 1e-6);
  // Модификатор убран (например, технология забыта или провинция сменила модификаторы).
  CHECK(h->act("Убрать модификатор", [&](Tx& tx) { { auto& v = tx.province(pk.pid).modifiers; v.erase(std::remove(v.begin(), v.end(), mid), v.end()); } }));
  CHECK(h->act("Отменить", [&](Tx& tx) { rules::cancelBuilding(tx, pk.pid, pk.building); }));
  const double after = stockSum(h->world(), pk.owner);
  CHECK_MSG(std::fabs(after - before) < 1e-6,
               "отмена бесплатной стройки вернула " + fmtNum(after - before, 2) + " ресурсов, хотя уплачено " + fmtNum(paid, 2));

  // Обратный случай: заплачено по полной цене, затем стоимость −50 % — возвращается половина.
  const double b2 = stockSum(h->world(), pk.owner);
  CHECK(h->act("Построить", [&](Tx& tx) { rules::startBuilding(tx, pk.pid, pk.building); }));
  const double paid2 = b2 - stockSum(h->world(), pk.owner);
  CHECK(paid2 > 0);
  addCostModifier(h.a(), pk.pid, -50);
  CHECK(h->act("Отменить", [&](Tx& tx) { rules::cancelBuilding(tx, pk.pid, pk.building); }));
  const double lost = b2 - stockSum(h->world(), pk.owner);
  CHECK_MSG(std::fabs(lost) < 1e-6, "после отмены потеряно " + fmtNum(lost, 2) + " из уплаченных " + fmtNum(paid2, 2) +
                                           " — интерфейс обещает «Стоимость вернётся полностью»");
}

// ТЗ 1.g.ii.2: глобальные эффекты «применяются только к государству». Список модификаторов провинции предлагает
// любые модификаторы (app/widgets.cpp modifierList — без фильтра по локальным эффектам), и глобальный модификатор,
// добавленный провинции, молча ничего не делает, хотя его фишка и подсказка («+6 % дохода в казну») видны в списке.
TEST(audit_tbmt_province_accepts_global_only_modifier) {
  Harness h("audit_tbmt_global_on_province");
  h.demo();
  // Модификатор только с глобальными эффектами из демонстрационного мира.
  Id global = 0;
  h->world().modifiers.each([&](const Modifier& m) {
    if (global) return;
    bool anyLocal = false, anyGlobal = false;
    for (int f = 0; f < kFxCount; f++)
      if (m.has(Fx(f))) (schema::kEffects[f].local ? anyLocal : anyGlobal) = true;
    if (anyGlobal && !anyLocal) global = m.id;
  });
  CHECK(global != 0);
  BuildPick pk = pickBuild(h->world());
  CHECK(pk.pid != 0);
  auto fBefore = rules::calc(h->world())->faction(pk.owner);
  const double netBefore = fBefore->net;
  CHECK(h->act("Модификатор провинции", [&](Tx& tx) {
    auto& v = tx.province(pk.pid).modifiers;
    if (std::find(v.begin(), v.end(), global) == v.end()) v.push_back(global);
  }));
  auto fAfter = rules::calc(h->world())->faction(pk.owner);
  // Эффект не действует (так задумано правилами), но интерфейс не предупреждает и не мешает добавить.
  CHECK_NEAR(fAfter->net, netBefore, 1e-6);
  h->ui.tabOf[app::SelType::Province] = "province.modifiers";
  h->select(app::SelType::Province, pk.pid);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("audit_tbmt_global_mod_on_province"));
  // Регрессия: фишка такого модификатора в провинции помечена предупреждением, а выбор «Добавить модификатор» его
  // не предлагает (w::modifierList с ModScope::Local).
  CHECK(app::w::modifierInert(*h->world().modifier(global), app::w::ModScope::Local));
  CHECK(!app::w::modifierInert(*h->world().modifier(global), app::w::ModScope::Any));
  CHECK_MSG(h->uiRect("mods.warn." + std::to_string(global)) != nullptr,
            std::string("модификатор «") + h->world().modifier(global)->name +
                "» только с глобальными эффектами добавлен в список провинции без предупреждения и ни на что не влияет");
}

// ТЗ 1.h.i: дерево построек государства = общее дерево + уникальные постройки. Ссылка «Дерево построек государства»
// во вкладке «Постройки» провинции открывает дерево владельца, где видны ТОЛЬКО уникальные постройки
// (app/editors/buildings.cpp drawBuildingTree: `if (b.owner != owner) return;`). У государства без уникальных
// построек дерево пустое.
TEST(audit_tbmt_state_tree_hides_common_buildings) {
  Harness h("audit_tbmt_state_tree", 1440, 900);
  h.demo();
  // Государство без уникальных построек.
  Id state = 0;
  h->world().factions.each([&](const Faction& f) {
    if (state || !f.isState()) return;
    bool uniq = false;
    h->world().buildings.each([&](const Building& b) { uniq = uniq || b.owner == f.id; });
    if (!uniq) state = f.id;
  });
  Id common = 0;
  h->world().buildings.each([&](const Building& b) {
    if (!common && b.owner == 0) common = b.id;
  });
  CHECK(common != 0);
  if (!state) {   // в демонстрационном мире у всех есть уникальные — берём любое государство
    h->world().factions.each([&](const Faction& f) {
      if (!state && f.isState()) state = f.id;
    });
  }
  CHECK(state != 0);
  app::openBuildingTree(h.a(), state);
  h.dropToasts();
  h.settle();
  h.settle();
  CHECK(h.shot("audit_tbmt_state_tree"));
  // Регрессия: дерево государства показывает и постройки общего дерева (только просмотр, со ссылкой на правку).
  CHECK_MSG(h->uiRect("bt.common." + std::to_string(common)) != nullptr,
            "дерево построек государства «" + h->world().faction(state)->name + "» не показывает постройки общего дерева");
  CHECK(h->uiRect("bt.common.edit") != nullptr);
  // Щелчок по постройке общего дерева открывает её в общем дереве.
  CHECK(h.clickUi("bt.common." + std::to_string(common)));
  h.settle();
  CHECK(h->ui.editor == "buildings");
  CHECK(h->uiRect("bt.common." + std::to_string(common)) == nullptr);   // общее дерево — без этого списка
  // Вкладка государства «Постройки» — тоже общее дерево вместе с уникальными.
  h->closeEditor();
  h->select(app::SelType::Faction, state);
  h->ui.tabOf[app::SelType::Faction] = "faction.buildings";
  h.settle();
  CHECK(h->uiRect("faction.buildings.commonItem." + std::to_string(common)) != nullptr);
}

// Удаление последнего уровня постройки проверяет только провинции, но не требования других построек
// (app/editors/buildings.cpp, кнопка «Удалить уровень»). Требование «R уровня N» к удалённому уровню остаётся,
// и зависящая постройка становится недоступной навсегда; после сохранения и повторного открытия нормализация
// молча меняет требование на другой уровень (поведение мира меняется от перезагрузки).
TEST(audit_tbmt_delete_level_leaves_impossible_requirement) {
  Harness h("audit_tbmt_req_level", 1440, 2400);
  h.demo();
  // Требование к последнему уровню постройки, этот уровень нигде не построен.
  Id dep = 0, req = 0;
  int lvl = 0;
  h->world().buildings.each([&](const Building& b) {
    for (const BuildingReq& r : b.requires_) {
      const Building* rb = h->world().building(r.building);
      if (dep || !rb || r.level < 2 || r.level != int(rb->levels.size())) continue;
      bool built = false;
      h->world().provinces.each([&](const Province& p) {
        for (const ProvBuilding& pb : p.buildings) built = built || (pb.building == rb->id && pb.level >= r.level);
      });
      if (!built) {
        dep = b.id;
        req = rb->id;
        lvl = r.level;
      }
    }
  });
  if (!dep) {   // создать такое требование самим
    h->world().buildings.each([&](const Building& b) {
      if (!req && b.owner == 0 && b.levels.size() >= 2) req = b.id;
    });
    CHECK(req != 0);
    lvl = int(h->world().building(req)->levels.size());
    CHECK(h->act("Требование", [&](Tx& tx) {
      // Уровень lvl нигде не построен и не строится.
      for (Id pid : tx.w().provinces.ids())
        for (const ProvBuilding& pb : tx.w().province(pid)->buildings)
          if (pb.building == req && pb.level >= lvl) {
            auto& v = tx.province(pid).buildings;
            for (ProvBuilding& x : v)
              if (x.building == req) {
                x.level = lvl - 1;
                x.constructing = false;
                x.left = 0;
              }
          }
      dep = rules::createBuilding(tx, 0, "Аудит: зависимая");
      tx.building(dep).requires_.push_back(BuildingReq{req, lvl});
    }));
  }
  CHECK(dep && req && lvl >= 2);
  const Id owner = h->world().building(req)->owner;
  app::openBuildingTree(h.a(), owner, req);
  h.dropToasts();
  h.settle();
  h.settle();
  CHECK(h.shot("audit_tbmt_req_level_side"));
  std::string del = "bt.level." + std::to_string(lvl - 1) + ".delete";
  const RectF* r = h->uiRect(del);
  CHECK(r != nullptr);
  if (!r) return;
  h.click(r->cx(), r->cy());
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK_EQ(int(h->world().building(req)->levels.size()), lvl - 1);
  int still = 0;
  for (const BuildingReq& q : h->world().building(dep)->requires_)
    if (q.building == req) still = q.level;
  CHECK_MSG(still <= lvl - 1, "после удаления уровня " + std::to_string(lvl) + " постройка «" + h->world().building(dep)->name +
                                     "» по-прежнему требует несуществующий уровень " + std::to_string(still));
}

// Механика хода: «Начать ход заново» обещает «мир станет таким, каким был в начале хода», а загружает снимок
// с тем же номером хода из ЛЮБОЙ ветки: после возврата к прошлому ходу и повторного прохождения ходов снимок
// «до возврата» из брошенной ветки остаётся под номером текущего хода (app/turn.cpp keepSnapshot, снимки
// хранятся по номеру хода). «Начать ход заново» подменяет текущую ветку брошенной.
TEST(audit_tbmt_restart_turn_loads_abandoned_branch) {
  Harness h("audit_tbmt_restart");
  h.demo();
  std::string dir = fs::join(h.root, "Мир");
  CHECK(h->saveTo(dir));
  BuildPick pk = pickBuild(h->world());
  CHECK(pk.pid != 0);
  const int t0 = h->store.world().turn();
  CHECK(h->endTurnNow());
  CHECK(h->endTurnNow());
  h->toasts().clear();
  CHECK_EQ(h->store.world().turn(), t0 + 2);
  CHECK(h->act("Имя", [&](Tx& tx) { tx.province(pk.pid).name = "Старая ветка"; }));
  // Возврат к первому ходу и повторное прохождение.
  CHECK(app::turnui::rollbackToTurn(h.a(), t0));
  h.settle();
  CHECK_EQ(h->store.world().turn(), t0);
  CHECK(h->endTurnNow());
  CHECK(h->endTurnNow());
  h->toasts().clear();
  CHECK_EQ(h->store.world().turn(), t0 + 2);
  CHECK(h->act("Имя", [&](Tx& tx) { tx.province(pk.pid).name = "Новая ветка"; }));
  // История: снимок текущего хода есть — значит, предлагается «Начать этот ход заново».
  bool hasCur = false;
  for (const app::TurnSnapshot& s : h->snapshots()) hasCur = hasCur || s.turn == t0 + 2;
  CHECK(hasCur);
  h->openDialog("turn.history");
  h.dropToasts();
  h.settle();
  h.settle();
  CHECK(h.clickUi("history.rollback." + std::to_string(t0 + 2)));
  h.settle();
  CHECK(h->hasDialog("confirm"));
  h.dropToasts();
  h.settle();
  CHECK(h.shot("audit_tbmt_restart_confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  const std::string name = h->store.world().province(pk.pid)->name;
  // Исправлено (снимки уникальны: ход + вид + номер, ветвь — цепочка parent): регрессионная проверка.
  CHECK_MSG(name != "Старая ветка", "«Начать ход заново» загрузил брошенную ветку (провинция снова «Старая ветка»), а не начало текущего хода");
  CHECK_MSG(name != "Новая ветка", "«Начать ход заново» оставил правку, сделанную в ходе");
  CHECK_EQ(h->store.world().turn(), t0 + 2);
}

// ТЗ 1.g.ii.1.a: прирост населения «на указанный процент каждый ход». Численность каждой расы округляется
// до целого на каждом ходу (rules/turn.cpp, шаг 6), поэтому небольшие группы при малом проценте не растут
// и не убывают никогда: 40 × 1,01 = 40,4 → 40.
TEST(audit_tbmt_population_growth_rounding_stalls) {
  Harness h("audit_tbmt_pop");
  h.demo();
  BuildPick pk = pickBuild(h->world());
  CHECK(pk.pid != 0);
  Id race = h->world().catalogs->races.empty() ? 0 : h->world().catalogs->races.front().id;
  CHECK(race != 0);
  CHECK(h->act("Население", [&](Tx& tx) {
    Province& p = tx.province(pk.pid);
    p.races = {RacePop{race, 40}};
    p.modifiers.clear();
    Id m = rules::createModifier(tx, "Аудит: прирост");
    Modifier& md = tx.modifier(m);
    md.fxMask |= 1u << int(Fx::PopGrowthPct);
    md.fx[size_t(int(Fx::PopGrowthPct))] = 1;
    p.modifiers.push_back(m);
  }));
  double expect = 40 * (1 + rules::provinceEffects(h->world(), pk.pid)[Fx::PopGrowthPct] / 100.0);
  for (int i = 0; i < 10; i++) {
    CHECK(h->endTurnNow());
    h->toasts().clear();
  }
  for (int i = 0; i < 9; i++) expect *= 1 + rules::provinceEffects(h->world(), pk.pid)[Fx::PopGrowthPct] / 100.0;
  i64 pop = h->store.world().province(pk.pid)->races.front().pop;
  CHECK_MSG(pop > 40, "за 10 ходов при приросте +" + fmtNum(rules::provinceEffects(h->world(), pk.pid)[Fx::PopGrowthPct], 2) +
                             " % население 40 не изменилось (ожидалось около " + fmtNum(expect, 1) + ")");
}

// ТЗ 1.a.iii: морская провинция «не содержит информации». Галочка «Морская провинция» сохраняет постройки,
// и завершение хода продолжает строительство в морской провинции (rules/turn.cpp, шаг 4 — без проверки sea),
// записывая в хронику «Достроено … в провинции» для моря; отменить такую стройку из интерфейса нельзя
// (вкладка «Постройки» у морской провинции скрыта), ресурсы остаются списанными.
TEST(audit_tbmt_sea_province_keeps_constructing) {
  Harness h("audit_tbmt_sea_build");
  h.demo();
  BuildPick pk = pickBuild(h->world());
  CHECK(pk.pid != 0);
  fillStock(h.a(), pk.owner, 10000);
  CHECK(h->act("Построить", [&](Tx& tx) { rules::startBuilding(tx, pk.pid, pk.building); }));
  CHECK(h->act("Сделать провинцию морской", [&](Tx& tx) { tx.province(pk.pid).sea = true; }));
  const size_t logBefore = h->store.world().log.size();
  for (int i = 0; i < 12; i++) {
    CHECK(h->endTurnNow());
    h->toasts().clear();
  }
  bool built = false;
  for (const ProvBuilding& pb : h->store.world().province(pk.pid)->buildings)
    if (pb.building == pk.building) built = !pb.constructing;
  int seaLogs = 0;
  h->store.world().log.each([&](const LogEntry& e) {
    if (e.province == pk.pid && e.text.find("Достроено") != std::string::npos) seaLogs++;
  });
  (void)logBefore;
  CHECK_MSG(!built && seaLogs == 0, "в морской провинции строительство продолжилось и завершилось; записей хроники «Достроено»: " +
                                           std::to_string(seaLogs));
}

// Провинция перешла к другому государству вместе с уникальной постройкой прежнего владельца: кнопка «Улучшить»
// недоступна, но подсказка говорит «Начать строительство следующего уровня», причина не показана
// (app/panels/construction.cpp: optionOf() == nullptr для постройки вне дерева нового владельца).
TEST(audit_tbmt_foreign_unique_upgrade_without_reason) {
  HideTestRegs hide;
  Harness h("audit_tbmt_foreign_unique", 1440, 1200);
  h.demo();
  Id uniq = 0, from = 0;
  h->world().buildings.each([&](const Building& b) {
    if (!uniq && b.owner && b.levels.size() >= 2) {
      uniq = b.id;
      from = b.owner;
    }
  });
  if (!uniq) {
    h->world().buildings.each([&](const Building& b) {
      if (!uniq && b.owner) {
        uniq = b.id;
        from = b.owner;
      }
    });
    CHECK(uniq != 0);
    CHECK(h->act("Второй уровень", [&](Tx& tx) { tx.building(uniq).levels.push_back(BuildingLevel{}); }));
  }
  Id pid = 0, to = 0;
  h->world().provinces.each([&](const Province& p) {
    if (!pid && !p.sea && p.owner == from) pid = p.id;
  });
  h->world().factions.each([&](const Faction& f) {
    if (!to && f.isState() && f.id != from) to = f.id;
  });
  CHECK(pid && to);
  CHECK(h->act("Постройка", [&](Tx& tx) {
    auto& v = tx.province(pid).buildings;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const ProvBuilding& x) { return x.building == uniq; }), v.end());
    v.push_back(ProvBuilding{uniq, 1, false, 0});
  }));
  CHECK(h->act("Смена владельца", [&](Tx& tx) { rules::setProvinceOwner(tx, pid, to); }));
  fillStock(h.a(), to, 10000);
  h->ui.tabOf[app::SelType::Province] = "province.buildings";
  h->select(app::SelType::Province, pid);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("audit_tbmt_foreign_unique"));
  const RectF* r = h->uiRect("prov.upgrade." + std::to_string(uniq));
  CHECK(r != nullptr);
  bool offered = false;
  for (auto& o : rules::buildOptions(h->world(), pid)) offered = offered || o.building == uniq;
  CHECK(!offered);
  // Регрессия: причина показана рядом с кнопкой (и в её подсказке).
  CHECK_MSG(h->uiRect("prov.upgradeWhy." + std::to_string(uniq)) != nullptr,
            "«Улучшить» для уникальной постройки прежнего владельца отключена без причины; подсказка — «Начать строительство следующего уровня»");
}
