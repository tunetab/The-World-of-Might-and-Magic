// Регрессии исправлений интерфейса войск (ТЗ 1.c.iv): панель битвы предупреждает, что «Отступить» не учтёт
// введённые потери, и не даёт выбрать победителем сторону без отрядов; текст диалога союза точен, когда отряды
// складываются с плиткой своей фракции в существующем союзном войске.
#include "tests/test_app_military_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

// Ввести потери в поле отряда панели битвы.
bool enterLoss(Harness& h, Id army, Id row, i64 n) {
  if (!h.clickUi("battle.loss." + std::to_string(army) + "." + std::to_string(row))) return false;
  h.retype(std::to_string(n));
  h.key(Key::Enter);
  h.settle();
  return true;
}

}  // namespace

// «Отступить» с введёнными потерями: заметка над кнопками и вопрос; «Отмена» — панель битвы остаётся,
// «Отступить без потерь» — панель закрыта, численность не изменилась.
TEST(app_fix_ui_battle_retreat_warns_about_losses) {
  Harness h("fix_ui_battle_retreat", 1440, 1000);
  RealArmyTools tools;
  h.demo();
  Id hel = factionByName(h->world(), "Хельдвиг");
  Id vk = factionByName(h->world(), "Валь-Кетра");
  Id x = armyOf(h->world(), hel, ArmyKind::Army);
  Id y = armyOf(h->world(), vk, ArmyKind::Army);
  CHECK(x && y);
  if (!x || !y) return;
  Id row = h->world().army(x)->groups[0].units[0].row;
  const i64 before = h->world().army(x)->groups[0].units[0].count;
  CHECK(before > 1);
  app::mil::openBattle(h.a(), x, y, h->world().army(x)->pos);
  h.settle();
  CHECK(h->hasDialog("battle"));
  // Без потерь — заметки нет.
  CHECK(h->uiRect("battle.retreatNote") == nullptr);
  CHECK(enterLoss(h, x, row, 1));
  CHECK(h->uiRect("battle.retreatNote") != nullptr);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("fix_ui_battle_retreat_note"));
  // Вопрос; «Отмена» — назад к панели битвы, потери сохранены.
  CHECK(h.clickUi("battle.retreat"));
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(h->hasDialog("battle"));
  CHECK(h.clickUi("dialog.cancel"));
  h.settle();
  CHECK(!h->hasDialog("confirm"));
  CHECK(h->hasDialog("battle"));
  CHECK(h->uiRect("battle.retreatNote") != nullptr);
  // Подтверждение — отступление без потерь.
  CHECK(h.clickUi("battle.retreat"));
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK(!h->hasDialog("battle"));
  CHECK(!h->hasDialog("confirm"));
  CHECK(h->world().army(x) != nullptr);
  CHECK_EQ(h->world().army(x)->groups[0].units[0].count, before);
}

// Нападающий теряет все отряды: победителем его не выбрать (щелчок по его половине ничего не меняет), итог —
// победа защитника, нападающий исчезает.
TEST(app_fix_ui_battle_winner_needs_units) {
  Harness h("fix_ui_battle_winner", 1440, 1000);
  RealArmyTools tools;
  h.demo();
  Id hel = factionByName(h->world(), "Хельдвиг");
  Id vk = factionByName(h->world(), "Валь-Кетра");
  Id x = armyOf(h->world(), hel, ArmyKind::Army);
  Id y = armyOf(h->world(), vk, ArmyKind::Army);
  CHECK(x && y);
  if (!x || !y) return;
  CHECK_EQ(h->world().army(x)->groups.size(), size_t(1));
  std::vector<std::pair<Id, i64>> rows;
  for (const ArmyUnit& u : h->world().army(x)->groups[0].units) rows.push_back({u.row, u.count});
  app::mil::openBattle(h.a(), x, y, h->world().army(x)->pos);
  h.settle();
  CHECK(h->hasDialog("battle"));
  for (auto& [row, n] : rows) CHECK(enterLoss(h, x, row, n));
  CHECK(h.clickUi("battle.winner.attacker"));
  h.settle();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("fix_ui_battle_winner_no_units"));
  CHECK(h.clickUi("battle.apply"));
  h.settle();
  CHECK(!h->hasDialog("battle"));
  CHECK(h->world().army(x) == nullptr);
  CHECK(h->world().army(y) != nullptr);
}

// Союз: войско фракции, уже представленной в союзном войске, складывается с её плиткой — текст так и говорит;
// новый союз из двух объектов разных фракций — «не сложатся».
TEST(app_fix_ui_alliance_text_accurate) {
  Harness h("fix_ui_alliance_text");
  RealArmyTools tools;
  h.demo();
  const World& w = h->world();
  Id allied = 0;
  w.armies.each([&](const Army& a) {
    if (!allied && a.allied() && a.kind == ArmyKind::Army) allied = a.id;
  });
  CHECK(allied != 0);
  if (!allied) return;
  // Объект фракции из союзного войска.
  Id member = 0;
  for (const ArmyGroup& g : w.army(allied)->groups)
    if (!member) member = armyOf(w, g.faction, ArmyKind::Army);
  CHECK(member != 0);
  if (member) {
    std::string t = app::mil::allianceText(w, *w.army(member), *w.army(allied));
    std::printf("  join: %s\n", t.c_str());
    CHECK(t.find("сложатся с её плиткой") != std::string::npos);
    CHECK(t.find("не сложатся") == std::string::npos);
  }
  // Новый союз: два несоюзных объекта союзных фракций.
  Id m = 0, tg = 0;
  w.armies.each([&](const Army& a) {
    if (m || a.allied()) return;
    w.armies.each([&](const Army& b) {
      if (m || b.allied() || b.id == a.id || b.kind != a.kind) return;
      if (rules::encounter(w, a.id, b.pos).type == rules::EncounterType::Alliance) {
        m = a.id;
        tg = b.id;
      }
    });
  });
  if (m) {
    std::string t = app::mil::allianceText(w, *w.army(m), *w.army(tg));
    std::printf("  new: %s\n", t.c_str());
    CHECK(t.find("не сложатся") != std::string::npos);
  }
}
