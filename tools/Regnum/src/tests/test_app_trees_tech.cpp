// Сценарии дерева технологий (ТЗ 1.b.v): вкладка фракции, полноэкранный редактор — создание, связи (цикл
// запрещён), перемещение, удаление, «галочка» изученности, исследование по ходам, копирование дерева, снимки.
#include "app/editors/techtree.h"
#include "tests/test_app_trees_util.h"

using namespace rg;
using namespace rg::apptest;
using namespace rg::apptest::trees;

namespace {

Id firstStateWithTechs(app::App& a) {
  Id fid = 0;
  a.world().techs.each([&](const Tech& t) {
    if (!fid) {
      const Faction* f = a.world().faction(t.faction);
      if (f && f->isState()) fid = t.faction;
    }
  });
  return fid;
}

// Государство без технологий (для копирования дерева).
Id stateWithoutTechs(app::App& a) {
  Id fid = 0;
  a.world().factions.each([&](const Faction& f) {
    if (fid || !f.isState()) return;
    bool any = false;
    a.world().techs.each([&](const Tech& t) { any = any || t.faction == f.id; });
    if (!any) fid = f.id;
  });
  return fid;
}

std::vector<Id> techsOf(const World& w, Id fid) {
  std::vector<Id> r;
  w.techs.each([&](const Tech& t) {
    if (t.faction == fid) r.push_back(t.id);
  });
  return r;
}

Id techByName(const World& w, Id fid, const std::string& name) {
  Id r = 0;
  w.techs.each([&](const Tech& t) {
    if (t.faction == fid && t.name == name) r = t.id;
  });
  return r;
}

bool hasPrereq(const World& w, Id tech, Id pre) {
  const Tech* t = w.tech(tech);
  return t && std::find(t->prereqs.begin(), t->prereqs.end(), pre) != t->prereqs.end();
}

// Плотное дерево: к шести технологиям демонстрационного мира — ещё пятнадцать со связями.
void denseTree(app::App& a, Id fid) {
  CHECK(a.act("Плотное дерево", [&](Tx& tx) {
    std::vector<Id> base = techsOf(tx.w(), fid);
    const char* names[] = {"Гончарное дело", "Письменность", "Каменная кладка", "Мостостроение", "Астрономия",   "Навигация",  "Монетное дело",
                           "Банковское дело", "Алхимия",      "Руническая магия", "Боевые маги",  "Драконья сбруя", "Укрепления", "Великие стены",
                           "Порох"};
    std::vector<Id> ids;
    for (int k = 0; k < 15; k++) {
      Id id = rules::createTech(tx, fid, names[k]);
      tx.tech(id).turns = 2 + k % 5;
      tx.tech(id).desc = k % 3 == 0 ? "+10 % к торговой ценности провинций" : (k % 3 == 1 ? "Открывает новые постройки" : "");
      ids.push_back(id);
    }
    auto link = [&](int a2, int b) { rules::setPrereq(tx, ids[size_t(a2)], ids[size_t(b)], true); };
    link(1, 0);
    link(3, 2);
    link(4, 1);
    link(5, 4);
    link(5, 3);
    link(7, 6);
    link(6, 1);
    link(9, 8);
    link(9, 1);
    link(10, 9);
    link(11, 10);
    link(13, 12);
    link(12, 2);
    link(14, 8);
    link(14, 12);
    if (base.size() >= 6) {
      rules::setPrereq(tx, ids[11], base[2], true);
      rules::setPrereq(tx, ids[13], base[5], true);
      rules::setPrereq(tx, ids[6], base[3], true);
    }
    rules::setStudied(tx, ids[0], true);
    rules::setStudied(tx, ids[2], true);
    rules::setStudied(tx, ids[1], true);
    rules::startResearch(tx, ids[4]);
    tx.tech(ids[4]).progress = 1;
    rules::startResearch(tx, ids[12]);
    rules::autoLayout(tx, fid);
  }));
}

}  // namespace

TEST(app_trees_tech_editor_visual) {
  HideTestRegs hide;
  Harness h("trees_tech_visual", 1600, 1000);
  h.demo();
  Id fid = firstStateWithTechs(h.a());
  CHECK(fid != 0);
  denseTree(h.a(), fid);
  app::openTechTree(h.a(), fid);
  h.settle();
  h.dropToasts();
  h.settle();
  CHECK_EQ(h->ui.editor, std::string("techtree"));
  CHECK(h.shot("trees_tech"));
  // Выбор карточки: панель свойств и карточка сведений при наведении.
  Id sel = techByName(h->world(), fid, "Руническая магия");
  CHECK(sel != 0);
  RectF nr = rectOf(h.a(), "tt.node." + std::to_string(sel));
  h.click(nr.x + nr.w * 0.4f, nr.y + nr.h * 0.6f);
  h.settle();
  CHECK(h->uiRect("tt.side.name") != nullptr);
  h.move(nr.x + nr.w * 0.4f, nr.y + nr.h * 0.6f);
  h.frames(40);
  h.settle();
  CHECK(h.shot("trees_tech_selected"));
  // Масштаб колесом и панорама фоном.
  double z0 = 0;
  RectF cv = rectOf(h.a(), "tt.canvas");
  RectF fit = rectOf(h.a(), "tt.zoom.fit");
  (void)fit;
  RectF n0 = rectOf(h.a(), "tt.node." + std::to_string(sel));
  h.wheel(n0.cx(), n0.cy(), 2);
  h.settle();
  RectF n1 = rectOf(h.a(), "tt.node." + std::to_string(sel));
  CHECK(n1.w > n0.w * 1.2f);
  z0 = n1.w;
  h.drag(cv.x + 20, cv.y + 20, cv.x + 120, cv.y + 80);
  h.settle();
  RectF n2 = rectOf(h.a(), "tt.node." + std::to_string(sel));
  CHECK_NEAR(n2.x - n1.x, 100, 6);
  CHECK_NEAR(n2.w, z0, 0.5);
}

TEST(app_trees_tech_editor_scale) {
  HideTestRegs hide;
  // Экран 125 %: попадание по карточкам и чёткость схемы при масштабе устройства.
  Harness h("trees_tech_scale", 1600, 1000, 1.25f);
  h.demo();
  h.dropToasts();
  Id fid = firstStateWithTechs(h.a());
  app::openTechTree(h.a(), fid);
  h.settle();
  std::vector<Id> ts = techsOf(h->world(), fid);
  RectF nr = rectOf(h.a(), "tt.node." + std::to_string(ts[2]));
  h.click(nr.x + nr.w * 0.4f, nr.y + nr.h * 0.6f);
  h.settle();
  CHECK(h->uiRect("tt.side.name") != nullptr);
  CHECK(h->uiRect("tt.side.delete") != nullptr);
  CHECK(h.shot("trees_tech_scale"));
}

TEST(app_trees_tech_tab) {
  HideTestRegs hide;
  Harness h("trees_tech_tab");
  h.demo();
  Id fid = firstStateWithTechs(h.a());
  CHECK(fid != 0);
  h->ui.tabOf[app::SelType::Faction] = "faction.tech";
  h->select(app::SelType::Faction, fid);
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_tech_tab"));
  // Доступная технология — начать исследование прямо из вкладки.
  Id avail = 0;
  for (Id t : techsOf(h->world(), fid))
    if (!avail && rules::canResearch(h->world(), t).ok && !h->world().tech(t)->research) avail = t;
  CHECK(avail != 0);
  clickRect(h, "faction.tech.start." + std::to_string(avail));
  CHECK(h->world().tech(avail)->research);
  undo(h);
  CHECK(!h->world().tech(avail)->research);
  // Кнопка «Открыть дерево технологий».
  clickRect(h, "faction.tech.open");
  CHECK_EQ(h->ui.editor, std::string("techtree"));
  CHECK_EQ(h->ui.editorArg, fid);
  h.key(Key::Escape);
  CHECK(h->ui.editor.empty());
}

TEST(app_trees_tech_graph_edit) {
  HideTestRegs hide;
  Harness h("trees_tech_graph");
  h.demo();
  Id fid = firstStateWithTechs(h.a());
  size_t n0 = techsOf(h->world(), fid).size();
  app::openTechTree(h.a(), fid);
  h.settle();
  // Новая технология кнопкой «+»: выбрана, имя — в поле панели свойств.
  clickRect(h, "tt.add");
  std::vector<Id> after = techsOf(h->world(), fid);
  CHECK_EQ(after.size(), n0 + 1);
  Id a = after.back();
  CHECK(h->uiRect("tt.side.name") != nullptr);
  clickRect(h, "tt.side.name");
  h.retype("Магия огня");
  h.key(Key::Enter);
  h.settle();
  CHECK_EQ(h->world().tech(a)->name, std::string("Магия огня"));
  // Вторая — двойным щелчком по пустому фону.
  RectF cv = rectOf(h.a(), "tt.canvas");
  h.doubleClick(cv.x + 30, cv.y + 30);
  h.settle();
  after = techsOf(h->world(), fid);
  CHECK_EQ(after.size(), n0 + 2);
  Id b = after.back();
  // Разнести на известные места и показать всё дерево (F).
  CHECK(h->act("Места", [&](Tx& tx) {
    tx.tech(a).pos = {0, -400};
    tx.tech(b).pos = {420, -400};
  }));
  h.click(cv.x + 20, cv.bottom() - 80);   // фон: снять выделение и фокус поля
  h.key(Key::F);
  h.settle();
  CHECK_EQ(h->ui.editor, std::string("techtree"));
  // Связь: от выхода A к карточке B — B требует A.
  RectF out = rectOf(h.a(), "tt.out." + std::to_string(a));
  RectF nb = rectOf(h.a(), "tt.node." + std::to_string(b));
  h.drag(out.cx(), out.cy(), nb.cx(), nb.cy());
  h.settle();
  CHECK(hasPrereq(h->world(), b, a));
  // Обратная связь создала бы цикл — отказ с объяснением, мир прежний.
  u64 v = h->store.version();
  RectF outB = rectOf(h.a(), "tt.out." + std::to_string(b));
  RectF na = rectOf(h.a(), "tt.node." + std::to_string(a));
  h.drag(outB.cx(), outB.cy(), na.cx(), na.cy());
  h.settle();
  CHECK(!hasPrereq(h->world(), a, b));
  CHECK_EQ(h->store.version(), v);
  CHECK(hasToast(h.a(), "цикл", app::ToastKind::Warning));
  h->toasts().clear();
  // Перемещение перетаскиванием — одна запись отмены.
  Vec2 p0 = h->world().tech(a)->pos;
  na = rectOf(h.a(), "tt.node." + std::to_string(a));
  h.drag(na.x + na.w * 0.3f, na.y + na.h * 0.7f, na.x + na.w * 0.3f + 90, na.y + na.h * 0.7f + 50);
  h.settle();
  Vec2 p1 = h->world().tech(a)->pos;
  CHECK(p1.x > p0.x + 20);
  CHECK(p1.y > p0.y + 10);
  undo(h);
  CHECK(h->world().tech(a)->pos == p0);
  redo(h);
  CHECK(h->world().tech(a)->pos == p1);
  // Щелчок по связи — выбор; кнопка удаления на связи.
  out = rectOf(h.a(), "tt.out." + std::to_string(a));
  RectF in = rectOf(h.a(), "tt.in." + std::to_string(b));
  h.click((out.cx() + in.cx()) * 0.5f, (out.cy() + in.cy()) * 0.5f);
  h.settle();
  CHECK(h->uiRect("tt.link.delete") != nullptr);
  CHECK(h->uiRect("tt.side.unlink") != nullptr);
  CHECK(h.shot("trees_tech_link"));
  clickRect(h, "tt.link.delete");
  CHECK(!hasPrereq(h->world(), b, a));
  undo(h);
  CHECK(hasPrereq(h->world(), b, a));
  // «Галочка» на карточке: A без условий — изучена и снова снята.
  clickRect(h, "tt.check." + std::to_string(a));
  CHECK(h->world().tech(a)->studied);
  clickRect(h, "tt.check." + std::to_string(a));
  CHECK(!h->world().tech(a)->studied);
  // B зависит от неизученной A — отметка отклонена правилами.
  clickRect(h, "tt.check." + std::to_string(b));
  CHECK(!h->world().tech(b)->studied);
  CHECK(hasToast(h.a(), "Сначала изучите", app::ToastKind::Warning));
  h->toasts().clear();
  // Удаление технологии — с подтверждением; связи исчезают; отмена возвращает.
  clickRect(h, "tt.node." + std::to_string(a), 0.3f, 0.7f);
  h.key(Key::Delete);
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK(h->world().tech(a) == nullptr);
  CHECK(!hasPrereq(h->world(), b, a));
  undo(h);
  CHECK(h->world().tech(a) != nullptr);
  CHECK(hasPrereq(h->world(), b, a));
}

TEST(app_trees_tech_research_turns) {
  HideTestRegs hide;
  Harness h("trees_tech_research");
  h.demo();
  Id fid = firstStateWithTechs(h.a());
  // Новая технология без условий, 2 хода.
  Id t = 0;
  CHECK(h->act("Новая", [&](Tx& tx) {
    t = rules::createTech(tx, fid, "Картография");
    tx.tech(t).turns = 2;
    tx.tech(t).pos = {0, -300};
  }));
  app::openTechTree(h.a(), fid, t);
  h.settle();
  CHECK(h->uiRect("tt.side.research") != nullptr);
  clickRect(h, "tt.side.research");
  CHECK(h->world().tech(t)->research);
  CHECK_EQ(h->world().tech(t)->progress, 0);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_tech_research"));
  CHECK(h->endTurnNow());
  h->toasts().clear();
  CHECK_EQ(h->world().tech(t)->progress, 1);
  CHECK(!h->world().tech(t)->studied);
  CHECK(h->endTurnNow());
  h->toasts().clear();
  CHECK(h->world().tech(t)->studied);
  CHECK(!h->world().tech(t)->research);
  // Отменить ход — снова исследуется.
  undo(h);
  CHECK(!h->world().tech(t)->studied);
  CHECK(h->world().tech(t)->research);
  // «Галочка» в панели свойств — отметить изученной сразу.
  h.settle();
  clickRect(h, "tt.side.studied", 0.1f, 0.5f);
  CHECK(h->world().tech(t)->studied);
  // Срок изучения: кнопка «+» поля.
  CHECK(h->act("Снять", [&](Tx& tx) { rules::setStudied(tx, t, false); }));
  h.settle();
  RectF tr = rectOf(h.a(), "tt.side.turns");
  h.click(tr.right() - 10, tr.cy());
  h.settle();
  CHECK_EQ(h->world().tech(t)->turns, 3);
}

TEST(app_trees_tech_copy_tree) {
  HideTestRegs hide;
  Harness h("trees_tech_copy");
  h.demo();
  Id from = firstStateWithTechs(h.a());
  Id to = stateWithoutTechs(h.a());
  CHECK(from && to);
  size_t n = techsOf(h->world(), from).size();
  app::openTechTree(h.a(), to);
  h.settle();
  CHECK(h->uiRect("tt.empty.add") != nullptr);
  CHECK(h.shot("trees_tech_empty"));
  clickRect(h, "tt.copy");
  CHECK(h->uiRect("tt.copy." + std::to_string(from)) != nullptr);
  clickRect(h, "tt.copy." + std::to_string(from));
  std::vector<Id> copied = techsOf(h->world(), to);
  CHECK_EQ(copied.size(), n);
  // Копия без изученности, связи — внутри нового дерева.
  size_t links = 0;
  for (Id id : copied) {
    const Tech* t = h->world().tech(id);
    CHECK(!t->studied && !t->research);
    links += t->prereqs.size();
    for (Id p : t->prereqs) CHECK_EQ(h->world().tech(p)->faction, to);
  }
  CHECK(links > 0);
  undo(h);
  CHECK(techsOf(h->world(), to).empty());
  // Авторасстановка разводит наложенные карточки.
  redo(h);
  CHECK(h->act("Сбить", [&](Tx& tx) {
    for (Id id : techsOf(tx.w(), to)) tx.tech(id).pos = {0, 0};
  }));
  h.settle();
  clickRect(h, "tt.layout");
  std::vector<Vec2> ps;
  for (Id id : techsOf(h->world(), to)) ps.push_back(h->world().tech(id)->pos);
  for (size_t i = 0; i < ps.size(); i++)
    for (size_t j = i + 1; j < ps.size(); j++) CHECK(!(ps[i] == ps[j]));
}

TEST(app_trees_tech_light_and_readonly) {
  HideTestRegs hide;
  Harness h("trees_tech_light", 1440, 900, 1, false);
  h.demo();
  Id fid = firstStateWithTechs(h.a());
  denseTree(h.a(), fid);
  CHECK(h->endTurnNow());
  // Прошлый ход — только просмотр: правка недоступна.
  CHECK(h->viewTurn(1));
  app::openTechTree(h.a(), fid);
  h.settle();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_tech_light"));
  size_t n = techsOf(h->world(), fid).size();
  RectF cv = rectOf(h.a(), "tt.canvas");
  h.doubleClick(cv.x + 30, cv.y + 30);
  h.settle();
  CHECK_EQ(techsOf(h->world(), fid).size(), n);
  CHECK_EQ(techsOf(h->store.world(), fid).size(), n);
}

TEST(app_trees_tech_copy_confirm_and_keys) {
  HideTestRegs hide;
  Harness h("trees_tech_copy_confirm");
  h.demo();
  h.dropToasts();
  Id from = firstStateWithTechs(h.a());
  Id to = stateWithoutTechs(h.a());
  CHECK(from && to);
  size_t n = techsOf(h->world(), from).size();
  // Пустое дерево: «Скопировать дерево другой фракции» прямо с пустого холста — без вопросов.
  app::openTechTree(h.a(), to);
  h.settle();
  clickRect(h, "tt.empty.alt");
  CHECK(h->uiRect("tt.copy." + std::to_string(from)) != nullptr);
  clickRect(h, "tt.copy." + std::to_string(from));
  CHECK_EQ(techsOf(h->world(), to).size(), n);
  h->toasts().clear();
  double maxY = -kInf;
  for (Id id : techsOf(h->world(), to)) maxY = std::max(maxY, h->world().tech(id)->pos.y);
  // Дерево уже не пусто: копия добавляется только после подтверждения и встаёт ниже существующих.
  h.settle();
  clickRect(h, "tt.copy");
  clickRect(h, "tt.copy." + std::to_string(from));
  CHECK(h->hasDialog("confirm"));
  CHECK(h->dialogStack().back()->style(h.a()).title.find("Добавить копию") != std::string::npos);
  CHECK_EQ(techsOf(h->world(), to).size(), n);
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  std::vector<Id> all = techsOf(h->world(), to);
  CHECK_EQ(all.size(), 2 * n);
  for (size_t i = n; i < all.size(); i++) CHECK(h->world().tech(all[i])->pos.y > maxY);
  undo(h);
  CHECK_EQ(techsOf(h->world(), to).size(), n);
  // Связь соседних слоёв: щелчок по середине кривой — выбор, Delete — удаление (без вопроса), Ctrl+Z — назад.
  CHECK(h->act("Расставить", [&](Tx& tx) { rules::autoLayout(tx, to); }));
  h.key(Key::F);
  h.settle();
  Id tech = 0, pre = 0;
  for (Id id : techsOf(h->world(), to)) {
    const Tech* t = h->world().tech(id);
    for (Id p : t->prereqs)
      if (!tech && std::fabs(h->world().tech(p)->pos.x + rules::kTreeColStep - t->pos.x) < 1) {
        tech = id;
        pre = p;
      }
  }
  CHECK(tech && pre);
  RectF out = rectOf(h.a(), "tt.out." + std::to_string(pre));
  RectF in = rectOf(h.a(), "tt.in." + std::to_string(tech));
  h.click((out.cx() + in.cx()) * 0.5f, (out.cy() + in.cy()) * 0.5f);
  h.settle();
  CHECK(h->uiRect("tt.side.unlink") != nullptr);
  h.key(Key::Delete);
  h.settle();
  CHECK(!h->hasDialog("confirm"));
  CHECK(!hasPrereq(h->world(), tech, pre));
  undo(h);
  CHECK(hasPrereq(h->world(), tech, pre));
  // Стрелки клавиатуры ходят по дереву; Esc закрывает редактор.
  clickRect(h, "tt.node." + std::to_string(pre), 0.3f, 0.75f);
  CHECK(h->uiRect("tt.side.name") != nullptr);
  h.key(Key::Escape);
  CHECK(h->ui.editor.empty());
}

TEST(app_trees_tech_guild) {
  HideTestRegs hide;
  Harness h("trees_tech_guild");
  h.demo();
  h.dropToasts();
  Id guild = 0;
  h->world().factions.each([&](const Faction& f) {
    if (!guild && f.isGuild()) guild = f.id;
  });
  CHECK(guild != 0);
  // Вкладка «Технологии» есть и у гильдии (ТЗ 1.d.ii: структура как у государства).
  const app::TabDef* tab = tabDef("faction.tech");
  CHECK(tab != nullptr);
  CHECK(tab->visible == nullptr || tab->visible(h.a(), guild));
  h->ui.tabOf[app::SelType::Faction] = "faction.tech";
  h->select(app::SelType::Faction, guild);
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_tech_guild_tab"));
  clickRect(h, "faction.tech.open");
  CHECK_EQ(h->ui.editor, std::string("techtree"));
  CHECK_EQ(h->ui.editorArg, guild);
  size_t n0 = techsOf(h->world(), guild).size();
  clickRect(h, "tt.add");
  std::vector<Id> ts = techsOf(h->world(), guild);
  CHECK_EQ(ts.size(), n0 + 1);
  // Новая технология — не изучена (ТЗ 1.b.v), «галочка» ставит отметку.
  Id t = ts.back();
  CHECK(!h->world().tech(t)->studied);
  clickRect(h, "tt.check." + std::to_string(t));
  CHECK(h->world().tech(t)->studied);
  h.dropToasts();
  h.settle();
  CHECK(h.shot("trees_tech_guild"));
}
