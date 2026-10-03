// Тесты модели мира: транзакции, откат, отмена, объединение, грязные таблицы, базовые утилиты.
#include "core/world.h"
#include "tests/test.h"

using namespace rg;

TEST(world_tx_add_edit_undo) {
  Store s;
  Id pid = s.transact("add", [](Tx& tx) {
    Province p; p.name = "Арден";
    return tx.add(p).id;
  });
  CHECK(pid > 0);
  CHECK_EQ(s.world().province(pid)->name, std::string("Арден"));
  s.transact("rename", [&](Tx& tx) { tx.province(pid).name = "Беррин"; });
  CHECK_EQ(s.world().province(pid)->name, std::string("Беррин"));
  CHECK(s.undo());
  CHECK_EQ(s.world().province(pid)->name, std::string("Арден"));
  CHECK(s.undo());
  CHECK(s.world().province(pid) == nullptr);
  CHECK(s.redo());
  CHECK(s.world().province(pid) != nullptr);
}

TEST(world_tx_exception_rolls_back) {
  Store s;
  Id pid = s.transact("add", [](Tx& tx) { return tx.add(Province{}).id; });
  World before = s.world();
  CHECK_THROWS(s.transact("bad", [&](Tx& tx) { tx.province(pid).name = "X"; fail("стоп"); }));
  CHECK(s.world().province(pid)->name.empty());
  CHECK_EQ(World::diff(before, s.world()), 0u);
}

TEST(world_structural_sharing) {
  Store s;
  std::vector<Id> ids;
  s.transact("many", [&](Tx& tx) { for (int i = 0; i < 500; i++) ids.push_back(tx.add(Province{}).id); });
  World a = s.world();
  s.transact("one", [&](Tx& tx) { tx.province(ids[3]).name = "Z"; });
  World b = s.world();
  CHECK(a.province(ids[3]) != b.province(ids[3]));
  CHECK(a.province(ids[400]) == b.province(ids[400]));  // общий объект
  int changed = 0;
  b.provinces.diff(a.provinces, [&](Id) { changed++; });
  CHECK_EQ(changed, 1);
  CHECK_EQ(World::diff(a, b), u32(TB_PROVINCES));
}

TEST(world_coalesce) {
  Store s;
  Id pid = s.transact("add", [](Tx& tx) { return tx.add(Province{}).id; });
  TxOptions o; o.coalesce = "drag";
  for (int i = 0; i < 10; i++) s.transact("drag", [&](Tx& tx) { tx.province(pid).contentment = i; }, o);
  CHECK_EQ(s.world().province(pid)->contentment, 9.0);
  CHECK(s.undo());
  CHECK_EQ(s.world().province(pid)->contentment, 0.0);
  CHECK(s.undo());
  CHECK(!s.canUndo());
}

TEST(world_dirty_and_relations) {
  Store s;
  s.markSaved();
  CHECK(!s.dirty());
  Id a = s.transact("f", [](Tx& tx) { return tx.add(Faction{}).id; });
  Id b = s.transact("f", [](Tx& tx) { return tx.add(Faction{}).id; });
  s.transact("rel", [&](Tx& tx) { tx.setRelation(b, a, Relation{-150, RelStatus::War}); });
  CHECK_EQ(s.world().relation(a, b).v, -100.0);
  CHECK(s.world().relation(a, b).s == RelStatus::War);
  CHECK(s.world().relation(b, a).s == RelStatus::War);
  CHECK(s.dirtyTables() & TB_RELATIONS);
  s.markSaved();
  CHECK(!s.dirty());
}

TEST(world_ids_not_reused) {
  Store s;
  Id a = s.transact("a", [](Tx& tx) { return tx.add(Army{}).id; });
  s.transact("del", [&](Tx& tx) { tx.eraseArmy(a); });
  Id b = s.transact("b", [](Tx& tx) { return tx.add(Army{}).id; });
  CHECK(b != a);
  CHECK_EQ(s.world().armies.size(), 1u);
}

TEST(base_format_ru) {
  CHECK_EQ(fmtNum(1234567), std::string("1\xE2\x80\xAF" "234\xE2\x80\xAF" "567"));
  CHECK_EQ(fmtNum(1234), std::string("1234"));
  CHECK_EQ(fmtNum(-5.5, 1), std::string("\xE2\x88\x92" "5,5"));
  CHECK_EQ(fmtSigned(3), std::string("+3"));
  CHECK_EQ(nTurns(3), std::string("3 хода"));
  CHECK_EQ(nTurns(11), std::string("11 ходов"));
  CHECK_EQ(nTurns(21), std::string("21 ход"));
  CHECK(parseNum("1 234,5").value() == 1234.5);
  CHECK(!parseNum("abc").has_value());
}

TEST(base_utf8_and_search) {
  std::string s = "Ёжик в Тумане";
  CHECK_EQ(utf8::count(s), size_t(13));
  CHECK_EQ(utf8::lower(s), std::string("ёжик в тумане"));
  CHECK(utf8::matches(s, "ежик туман"));
  CHECK(!utf8::matches(s, "лес"));
  CHECK(compareRu("Арден", "беррин") < 0);
  CHECK(compareRu("ель", "ёж") < 0);
  CHECK(compareRu("ёж", "жук") < 0);
  CHECK(compareRu("Провинция 2", "Провинция 10") < 0);
  CHECK_EQ(utf8::fromWide(utf8::toWide("Мир 🌍")), std::string("Мир 🌍"));
}

TEST(world_tx_replace_world_undo) {
  Store s;
  Id a = s.transact("a", [](Tx& tx) { Province p; p.name = "Старая"; return tx.add(p).id; });
  World snap = s.world();
  s.transact("b", [&](Tx& tx) { tx.province(a).name = "Новая"; tx.add(Faction{}); });
  s.transact("откат", [&](Tx& tx) { tx.replaceWorld(snap); });
  CHECK_EQ(s.world().province(a)->name, std::string("Старая"));
  CHECK_EQ(s.world().factions.size(), 0u);
  CHECK(s.undo());
  CHECK_EQ(s.world().province(a)->name, std::string("Новая"));
  CHECK_EQ(s.world().factions.size(), 1u);
  // после замены можно продолжать правки в той же транзакции
  s.transact("откат+", [&](Tx& tx) { tx.replaceWorld(snap); tx.province(a).name = "Третья"; });
  CHECK_EQ(s.world().province(a)->name, std::string("Третья"));
  CHECK_EQ(s.world().factions.size(), 0u);
}
