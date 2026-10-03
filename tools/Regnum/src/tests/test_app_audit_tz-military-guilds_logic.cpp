// Аудит ТЗ 1.c (войска и флот) и 1.d (торговые гильдии): воспроизведение найденных дефектов.
// Проверки ожидаемого поведения включаются переменной окружения REGNUM_AUDIT=1; без неё тесты только пишут
// наблюдения в вывод и снимки (app_audit_tzmil_*.png) и не роняют общий прогон.
#include <cstdio>

#include "map/map_internal.h"
#include "tests/test_app_military_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

[[maybe_unused]] bool auditOn() {
  const char* v = std::getenv("REGNUM_AUDIT");
  return v && *v && *v != '0';
}

// Ожидаемое поведение: при REGNUM_AUDIT=1 — проверка, иначе — запись в вывод.
#define AUDIT_EXPECT(cond, msg)                                                         \
  do {                                                                                  \
    bool _ok = (cond);                                                                  \
    if (!_ok) std::printf("  [audit tzmil] %s:%d ДЕФЕКТ: %s\n", __FILE__, __LINE__, msg); \
    if (auditOn()) CHECK_MSG(_ok, msg);                                                 \
  } while (0)

Id alliedArmy(const World& w, ArmyKind kind) {
  Id found = 0;
  w.armies.each([&](const Army& a) {
    if (!found && a.allied() && a.kind == kind) found = a.id;
  });
  return found;
}

}  // namespace

// ТЗ 1.c.iv: союзное войско существует только между союзниками (регрессия аудита). Войско фракции на союзном объекте
// со своей группой складывается в неё (не битва); при объявлении войны между участниками союзное войско распускается
// на отдельные объекты; битва, где фракция есть в обоих объектах, правилами не принимается.
TEST(app_audit_tzmil_allied_army_survives_war) {
  Harness h("audit_tzmil_allied_war");
  RealArmyTools tools;
  h.demo();
  const World& w0 = h->world();
  Id alm = factionByName(w0, "Альмарин");
  Id hel = factionByName(w0, "Хельдвиг");
  Id allied = alliedArmy(w0, ArmyKind::Army);
  CHECK(alm && hel && allied);
  CHECK(w0.relation(alm, hel).s == RelStatus::Alliance);
  Id x = armyOf(w0, hel, ArmyKind::Army);
  CHECK(x != 0);
  // Пока союз: войско Хельдвига на союзном войске со своей группой — объединение в свою группу.
  CHECK(rules::encounter(h->world(), x, h->world().army(allied)->pos).type == rules::EncounterType::Merge);
  // Повреждённые данные (война внутри союзного объекта, как из файла): встреча — не битва, итог битвы не принимается.
  CHECK(h->act("Сырые отношения", [&](Tx& tx) { tx.setRelation(alm, hel, Relation{-60, RelStatus::War}); }));
  rules::Encounter e = rules::encounter(h->world(), x, h->world().army(allied)->pos);
  CHECK_MSG(e.type != rules::EncounterType::Battle, "войско Хельдвига вступает в битву с объектом, где стоят отряды самого Хельдвига");
  const ArmyGroup* helGroup = nullptr;
  for (const ArmyGroup& g : h->world().army(allied)->groups)
    if (g.faction == hel) helGroup = &g;
  CHECK(helGroup && !helGroup->units.empty());
  rules::BattleResult r;
  r.attacker = x;
  r.defender = allied;
  r.attackerWins = true;
  r.attackerOrigin = h->world().army(x)->pos;
  if (helGroup) r.losses[allied][{hel, helGroup->units[0].row}] = 100;
  bool applied = h->act("Битва", [&](Tx& tx) { rules::resolveBattle(tx, r); });
  CHECK_MSG(!applied, "битва, где Хельдвиг теряет отряды в собственной группе союзного войска, принята правилами");
  h->undo();
  h.settle();
  CHECK(h->world().relation(alm, hel).s == RelStatus::Alliance);
  // Пользователь меняет состояние отношений во вкладке «Дипломатия» на «в войне»: союзное войско распускается.
  const u32 armies0 = h->world().armies.size();
  CHECK(h->act("Отношения", [&](Tx& tx) { rules::setRelation(tx, alm, hel, -60, RelStatus::War); }));
  const Army* al = h->world().army(allied);
  CHECK(al != nullptr);
  CHECK_MSG(!al->allied(), "союзное войско Альмарина и Хельдвига осталось одним объектом после объявления войны между ними");
  CHECK_EQ(h->world().armies.size(), armies0 + 1);
  // Теперь это обычная битва Хельдвига с войском Альмарина.
  CHECK(rules::encounter(h->world(), x, al->pos).type == rules::EncounterType::Battle);
  h.settle();
  app::mil::openBattle(h.a(), x, allied, h->world().army(x)->pos);
  h.settle();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("audit_tzmil_battle_vs_own_group"));
}

// ТЗ 1.d.iv: общий налог = налог государства (0…100 %) + местный (−100…100 %) без верхней границы. При общем
// налоге больше 100 % чистый доход гильдии со штабом отрицателен, а налог государству превышает ценность провинции.
TEST(app_audit_tzmil_total_tax_over_100) {
  Harness h("audit_tzmil_tax");
  h.demo();
  const World& w = h->world();
  // Провинция со штабом гильдии и её влиянием.
  Id pid = 0, guild = 0;
  w.provinces.each([&](const Province& p) {
    if (pid || p.sea || !p.owner) return;
    for (const Influence& in : p.influence)
      if (in.pct > 0 && std::find(p.hqs.begin(), p.hqs.end(), in.guild) != p.hqs.end()) {
        pid = p.id;
        guild = in.guild;
        return;
      }
  });
  CHECK(pid && guild);
  Id owner = w.province(pid)->owner;
  // Значения в пределах полей интерфейса: налог государства до 100 %, местный налог до 100 %.
  CHECK(h->act("Налоги", [&](Tx& tx) {
    tx.faction(owner).tax = 100;
    tx.province(pid).localTax = 100;
  }));
  auto c = rules::calc(h->world());
  const rules::ProvinceCalc* pc = c->province(pid);
  CHECK(pc != nullptr);
  double net = 0;
  for (const rules::GuildShare& s : pc->guilds)
    if (s.guild == guild) net = s.net;
  std::printf("  [audit tzmil] общий налог %.1f %%, ценность %.2f, налог провинции %.2f, налог гильдий %.2f, чистый доход гильдии %.2f\n", pc->taxTotal,
              pc->tradeValue, pc->provinceTax, pc->guildTax, net);
  CHECK_MSG(pc->taxTotal <= 100.0 + 1e-9, "общий налог провинции больше 100 %");
  CHECK_MSG(net >= 0, "чистый доход гильдии со штабом отрицателен (налог больше валового дохода)");
  CHECK_MSG(pc->provinceTax + pc->guildTax <= pc->tradeValue + 1e-6, "налог государству больше торговой ценности провинции");
  // Снимок вкладки «Экономика» провинции.
  h->ui.tabOf[app::SelType::Province] = "province.economy";
  h->select(app::SelType::Province, pid);
  h.settle();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("audit_tzmil_tax_over_100"));
}

// ТЗ 1.d.vi: государственная гильдия создаётся из вкладки государства и закреплена за ним, но по отношению к своему
// государству она «незнакомы»: войско государственной гильдии, встретив войско своего государства, получает
// предложение объявить ему войну.
TEST(app_audit_tzmil_state_guild_vs_home_state) {
  Harness h("audit_tzmil_stateguild");
  RealArmyTools tools;
  h.demo();
  const World& w = h->world();
  Id alm = factionByName(w, "Альмарин");
  Id comp = factionByName(w, "Альмарская торговая");
  CHECK(alm && comp);
  CHECK(w.faction(comp)->stateGuild && w.faction(comp)->homeState == alm);
  Relation rel = w.relation(alm, comp);
  std::printf("  [audit tzmil] отношения Альмарина и его государственной гильдии: %.0f, состояние %d (3 = незнакомы)\n", rel.v, int(rel.s));
  // Новая государственная гильдия из вкладки государства — то же.
  Id ng = 0;
  CHECK(h->act("Гильдия", [&](Tx& tx) { ng = rules::createStateGuild(tx, alm, ""); }));
  Relation rel2 = h->world().relation(alm, ng);
  CHECK_MSG(rel2.s == RelStatus::Alliance, "новая государственная гильдия «незнакома» своему государству");
  CHECK_MSG(rel.s == RelStatus::Alliance, "государственная гильдия демонстрационного мира не в союзе со своим государством");
  // Войско гильдии рядом с войсками Альмарина: собственное войско государства — союзник (не «объявить войну»);
  // у союзного войска Альмарина и Хельдвига войну предлагают разве что незнакомому Хельдвигу, но не своему государству.
  Id aa = alliedArmy(h->world(), ArmyKind::Army);
  CHECK(aa != 0);
  Vec2 apos = h->world().army(aa)->pos;
  Id ga = 0, sa = 0;
  CHECK(h->act("Войска гильдии и государства", [&](Tx& tx) {
    Id row = rules::addArmyRow(tx, comp, UnitType::Ranged, "Охрана караванов", 300, 1);
    auto spot = rules::findFreeSpot(tx.w(), ArmyKind::Army, apos + Vec2(200, 0));
    if (!spot) fail("нет места");
    ga = rules::createArmy(tx, ArmyKind::Army, comp, *spot);
    rules::setUnits(tx, ga, comp, row, 200);
    Id srow = rules::addArmyRow(tx, alm, UnitType::LightInf, "Ополчение", 300, 0.05);
    auto spot2 = rules::findFreeSpot(tx.w(), ArmyKind::Army, apos + Vec2(-200, 120));
    if (!spot2) fail("нет места");
    sa = rules::createArmy(tx, ArmyKind::Army, alm, *spot2);
    rules::setUnits(tx, sa, alm, srow, 200);
  }));
  rules::Encounter es = rules::encounter(h->world(), ga, h->world().army(sa)->pos);
  CHECK_MSG(es.type == rules::EncounterType::Alliance, "войско государственной гильдии не союзно войску своего государства");
  rules::Encounter e = rules::encounter(h->world(), ga, apos);
  std::printf("  [audit tzmil] встреча войска государственной гильдии с союзным войском её государства: тип %d (4 = объявить войну)\n", int(e.type));
  CHECK_MSG(!(e.type == rules::EncounterType::DeclareWar && (e.them == alm || e.us == alm)),
            "войску государственной гильдии предлагают объявить войну своему государству");
  if (e.type == rules::EncounterType::DeclareWar) {
    app::mil::openEncounter(h.a(), ga, aa, e, h->world().army(ga)->pos, [](app::App&, bool) {});
    h.settle();
    h.dropToasts();
    h.settle();
    CHECK(h.shot("audit_tzmil_stateguild_declare_war"));
  }
}

// ТЗ 1.d.v: «За каждый торговый маршрут, который проходит через провинцию, ...+10 %». Раньше маршрут рисовался
// сглаженной кривой (Катмулл — Ром), а провинции маршрута (бонус, подсветка, список в инспекторе) считались по
// исходной ломаной. Исправлено (карта): линия на карте — ровно та ломаная (map::detail::routeLine) — регрессия.
TEST(app_audit_tzmil_route_drawn_vs_counted) {
  Harness h("audit_tzmil_routes");
  h.demo();
  const World& w = h->world();
  auto fs = geo::faces(w);
  int mismatches = 0;
  auto land = [&](const std::vector<Id>& ids) {
    std::vector<Id> out;
    for (Id p : ids)
      if (const Province* pr = w.province(p); pr && !pr->sea) out.push_back(p);
    std::sort(out.begin(), out.end());
    return out;
  };
  w.routes.each([&](const Route& r) {
    std::vector<Id> raw = land(fs->provincesOnPolyline(r.pts));
    std::vector<Id> drawn = land(fs->provincesOnPolyline(map::detail::routeLine(r.pts)));
    if (raw != drawn) {
      mismatches++;
      std::string onlyDrawn, onlyRaw;
      for (Id p : drawn)
        if (!std::binary_search(raw.begin(), raw.end(), p)) onlyDrawn += " «" + w.provinceName(p) + "»";
      for (Id p : raw)
        if (!std::binary_search(drawn.begin(), drawn.end(), p)) onlyRaw += " «" + w.provinceName(p) + "»";
      std::printf("  [audit tzmil] маршрут «%s»: линия на карте проходит через%s без бонуса; бонус без линии:%s\n", r.name.c_str(),
                  onlyDrawn.empty() ? " —" : onlyDrawn.c_str(), onlyRaw.empty() ? " —" : onlyRaw.c_str());
    }
  });
  // Синтетический маршрут-зигзаг через центр провинции: изгиб кривой выходит за ломаную.
  Id pid = 0;
  Vec2 c;
  double best = 0;
  w.provinces.each([&](const Province& p) {
    if (p.sea) return;
    const geo::ProvinceShape* sh = fs->shape(p.id);
    if (sh && sh->area > best) {
      best = sh->area;
      pid = p.id;
      c = sh->label;
    }
  });
  CHECK(pid != 0);
  std::vector<Vec2> zig = {c + Vec2(-600, 0), c + Vec2(-200, -500), c + Vec2(200, 500), c + Vec2(600, 0)};
  std::vector<Id> raw = land(fs->provincesOnPolyline(zig));
  std::vector<Id> drawn = land(fs->provincesOnPolyline(map::detail::routeLine(zig)));
  if (raw != drawn) mismatches++;
  std::printf("  [audit tzmil] зигзаг: провинций по ломаной %zu, по нарисованной кривой %zu\n", raw.size(), drawn.size());
  CHECK_MSG(mismatches == 0, "провинции с бонусом маршрута не совпадают с провинциями, через которые маршрут нарисован на карте");
  // Снимок: маршрут «Восточный тракт» и провинция «Рассветный берег» (бонус есть, линии нет).
  Id route = 0, prov = 0;
  w.routes.each([&](const Route& r) {
    if (r.name == "Восточный тракт") route = r.id;
  });
  w.provinces.each([&](const Province& p) {
    if (p.name == "Рассветный берег") prov = p.id;
  });
  if (route && prov) {
    h->setMapMode(schema::MapMode::Guilds);
    h->select(app::SelType::Route, route);
    h.settle();
    showAt(h, fs->shape(prov)->label, 0.35);
    h.dropToasts();
    h.settle();
    CHECK(h.shot("audit_tzmil_route_mismatch"));
    auto c = rules::calc(h->world());
    std::printf("  [audit tzmil] «Рассветный берег»: маршрутов по расчёту %d, текущая ценность %.2f при базе %.2f\n", c->province(prov)->routes,
                c->province(prov)->tradeValue, c->province(prov)->tradeBase);
  }
}

// ТЗ 1.c.iv: «все эти объекты не должны наслаиваться друг на друга». Минимальный зазор задан в единицах карты
// (2 × 34), а фигурка рисуется не меньше 24 точек экрана: при обзорном масштабе соседние фигурки перекрывались.
// Исправлено (карта): наложившиеся объекты рисуются одной отметкой-стопкой — проверяются нарисованные отметки
// (регрессия): они не пересекаются, каждый объект — ровно в одной отметке.
TEST(app_audit_tzmil_figures_overlap_on_screen) {
  Harness h("audit_tzmil_overlap");
  h.demo();
  h.waitMap();
  auto count = [&](const char* when) {
    const World& w = h->world();
    const map::View& v = h->map().view();
    std::vector<map::ArmyMark> marks = h->map().armyMarks();
    int overlaps = 0, stacks = 0;
    std::vector<Id> seen;
    for (const map::ArmyMark& m : marks) {
      stacks += m.cluster() ? 1 : 0;
      seen.insert(seen.end(), m.members.begin(), m.members.end());
    }
    std::sort(seen.begin(), seen.end());
    CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
    CHECK_EQ(seen.size(), size_t(w.armies.size()));
    for (size_t i = 0; i < marks.size(); i++)
      for (size_t j = i + 1; j < marks.size(); j++) {
        const RectF a = marks[i].bounds, b = marks[j].bounds;
        if (a.intersect(b).empty()) continue;
        overlaps++;
        std::printf("  [audit tzmil] %s: отметки «%s» и «%s» перекрываются (масштаб %.3f)\n", when, w.army(marks[i].top)->name.c_str(),
                    w.army(marks[j].top)->name.c_str(), v.zoom);
        RectF r{std::min(a.x, b.x) - 10, std::min(a.y, b.y) - 10, std::max(a.right(), b.right()) - std::min(a.x, b.x) + 20,
                std::max(a.bottom(), b.bottom()) - std::min(a.y, b.y) + 20};
        cropShot(std::string("audit_tzmil_overlap_") + when + "_" + std::to_string(overlaps), r, 4);
      }
    std::printf("  [audit tzmil] %s: масштаб %.3f, объектов %u, отметок %zu, из них стопок %d\n", when, v.zoom, w.armies.size(), marks.size(), stacks);
    return overlaps;
  };
  h.dropToasts();
  h.settle();
  CHECK(h.shot("audit_tzmil_overlap_default"));
  int n1 = count("default");
  h->focusMap(geo::faces(h->world())->bounds);
  h.settle();
  h.waitMap();
  h.dropToasts();
  h.settle();
  int n2 = count("world");
  CHECK(h.shot("audit_tzmil_overlap_world"));
  CHECK_MSG(n1 == 0, "фигурки войск перекрываются на экране при масштабе по умолчанию");
  CHECK_MSG(n2 == 0, "фигурки войск перекрываются на экране при показе всей карты");
}

// ТЗ 1.c.iv: в панели битвы можно выбрать победителем сторону, у которой после потерь не осталось отрядов.
// Тогда «победитель» исчезает, проигравший остаётся на месте, а хроника пишет «Победа: …» уничтоженному.
TEST(app_audit_tzmil_battle_destroyed_winner) {
  Harness h("audit_tzmil_battle_winner");
  RealArmyTools tools;
  h.demo();
  const World& w = h->world();
  Id hel = factionByName(w, "Хельдвиг");
  Id vk = factionByName(w, "Валь-Кетра");
  CHECK(w.relation(hel, vk).s == RelStatus::War);
  Id x = armyOf(w, hel, ArmyKind::Army);
  Id y = armyOf(w, vk, ArmyKind::Army);
  CHECK(x && y);
  Vec2 ypos = w.army(y)->pos;
  rules::BattleResult r;
  r.attacker = x;
  r.defender = y;
  r.attackerWins = true;
  r.attackerOrigin = w.army(x)->pos;
  for (const ArmyGroup& g : w.army(x)->groups)
    for (const ArmyUnit& u : g.units) r.losses[x][{g.faction, u.row}] = u.count;   // нападающий теряет всё
  bool ok = h->act("Битва", [&](Tx& tx) { rules::resolveBattle(tx, r); });
  CHECK_MSG(!ok, "победителем принята сторона без отрядов");
  CHECK(h->world().army(x) != nullptr);                                        // мир не изменился
  CHECK(h->world().army(y) && dist(h->world().army(y)->pos, ypos) < 1e-6);
  bool toasted = false;
  for (const auto& t : h->toasts()) toasted = toasted || t.text.find("не осталось отрядов") != std::string::npos;
  CHECK_MSG(toasted, "отказ без понятной причины");
}

// ТЗ 1.c.iv: «Отступить» и изменение численности по потерям — обе возможности панели битвы. Введённые потери при
// отступлении молча отбрасываются.
TEST(app_audit_tzmil_retreat_discards_losses) {
  Harness h("audit_tzmil_retreat");
  RealArmyTools tools;
  h.demo();
  Id hel = factionByName(h->world(), "Хельдвиг");
  Id vk = factionByName(h->world(), "Валь-Кетра");
  Id x = armyOf(h->world(), hel, ArmyKind::Army);
  Id y = armyOf(h->world(), vk, ArmyKind::Army);
  CHECK(x && y);
  Id row = h->world().army(x)->groups[0].units[0].row;
  i64 before = h->world().army(x)->groups[0].units[0].count;
  app::mil::openBattle(h.a(), x, y, h->world().army(x)->pos);
  h.settle();
  CHECK(h->hasDialog("battle"));
  CHECK(h.clickUi("battle.loss." + std::to_string(x) + "." + std::to_string(row)));
  h.retype("250");
  h.key(Key::Enter);
  h.settle();
  CHECK(h.shot("audit_tzmil_retreat_with_losses"));
  CHECK(h.clickUi("battle.retreat"));
  h.settle();
  i64 after = h->world().army(x)->groups[0].units[0].count;
  std::printf("  [audit tzmil] потери 250 введены, «Отступить»: было %lld, стало %lld\n", (long long)before, (long long)after);
  // Регрессия: «Отступить» с введёнными потерями переспрашивает (подробно — test_app_fix_ui_battle.cpp).
  CHECK_MSG(h->hasDialog("confirm"), "введённые в панели битвы потери отброшены при отступлении без предупреждения");
  CHECK_EQ(after, before);
}

// ТЗ 1.c.iv: войско фракции A, перенесённое на союзное войско {A, B}, получает диалог «Создать союзное войско?»
// с текстом «Отряды фракций не сложатся», хотя отряды A складываются с группой A, а союзное войско уже есть.
TEST(app_audit_tzmil_join_existing_allied_text) {
  Harness h("audit_tzmil_join_allied");
  RealArmyTools tools;
  h.demo();
  const World& w = h->world();
  Id hel = factionByName(w, "Хельдвиг");
  Id allied = alliedArmy(w, ArmyKind::Army);
  Id x = armyOf(w, hel, ArmyKind::Army);
  CHECK(allied && x);
  rules::Encounter e = rules::encounter(w, x, w.army(allied)->pos);
  std::printf("  [audit tzmil] встреча Хельдвиг -> союзное войско с группой Хельдвига: тип %d (3 = союз)\n", int(e.type));
  app::mil::openEncounter(h.a(), x, allied, e, w.army(x)->pos, [](app::App&, bool) {});
  h.settle();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("audit_tzmil_join_allied_dialog"));
  i64 groupsBefore = i64(w.army(allied)->groups.size());
  CHECK(h.clickUi("encounter.ok"));
  h.settle();
  const Army* m = h->world().army(allied);
  CHECK(m != nullptr);
  std::printf("  [audit tzmil] групп было %lld, стало %zu (отряды Хельдвига сложились в его плитку)\n", (long long)groupsBefore, m->groups.size());
  CHECK_MSG(e.type != rules::EncounterType::Alliance || i64(m->groups.size()) != groupsBefore,
               "диалог «Создать союзное войско» обещает раздельные плитки, а отряды складываются в существующую группу");
}
