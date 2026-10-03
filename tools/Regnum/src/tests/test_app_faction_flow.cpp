// Сценарии панелей государств и гильдий на демонстрационном мире: создание из списков и переименование в шапке,
// редактор флага (узор, эмблема, изображение PNG/JPEG, ввод пути без системных диалогов), симметричные отношения,
// совет, налог и запасы ресурсов, государственная гильдия и закреплённое государство, удаление с очисткой ссылок,
// штабы гильдии, герои, модификаторы, дань, режим только для чтения. Каждое изменение проверяется и отменой.
#include "tests/test_app_faction_util.h"
#include "tests/test_codec_util.h"

using namespace rg;
using namespace rg::apptest;
using namespace rg::factest;

// ---------------------------------------------------------------- создание и переименование (ТЗ 1.b.ii, 1.d.i)
TEST(app_faction_create_and_rename) {
  HideTestRegs regs;
  Harness h("faction_create");
  h.demo();
  h.waitMap();
  h.dropToasts();
  const u32 before = h->world().factions.size();
  std::vector<Color> colors;
  h->world().factions.each([&](const Faction& f) { colors.push_back(f.color); });

  h->openDrawer("states");
  h.step();
  CHECK(h.clickUi("states.add"));
  h.step();
  CHECK_EQ(h->world().factions.size(), before + 1);
  app::Selection sel = h->ui.sel;
  CHECK(sel.type == app::SelType::Faction);
  const Faction* f = h->world().faction(sel.id);
  CHECK(f && f->isState());
  // Новый цвет отличается от всех существующих, флаг — узор с эмблемой в цвете фракции.
  for (Color c : colors) CHECK(!(c == f->color));
  CHECK(!f->flag.image);
  CHECK(!f->flag.emblem.empty());
  CHECK(f->flag.colors[0] == f->color);
  CHECK(h->uiRect("states.row." + std::to_string(sel.id)) != nullptr);
  // Сразу после создания шапка ждёт новое название.
  CHECK(h->uiRect("faction.rename") != nullptr);
  h.retype("Новая Империя");
  h.key(Key::Enter);
  h.step();
  CHECK_EQ(h->world().faction(sel.id)->name, std::string("Новая Империя"));
  CHECK(h.shot("faction_created_state"));
  h->undo();
  h.step();
  CHECK(h->world().faction(sel.id)->name != "Новая Империя");
  h->redo();
  h.step();
  CHECK_EQ(h->world().faction(sel.id)->name, std::string("Новая Империя"));

  // Переименование щелчком по названию; Esc отменяет правку.
  CHECK(h.clickUi("faction.name"));
  CHECK(h->uiRect("faction.rename") != nullptr);
  h.retype("Не применять");
  h.key(Key::Escape);
  h.step();
  CHECK_EQ(h->world().faction(sel.id)->name, std::string("Новая Империя"));

  // Гильдия из своего списка.
  h->openDrawer("guilds");
  h.step();
  CHECK(h.clickUi("guilds.add"));
  h.step();
  CHECK_EQ(h->world().factions.size(), before + 2);
  Id gid = h->ui.sel.id;
  const Faction* g = h->world().faction(gid);
  CHECK(g && g->isGuild() && !g->stateGuild);
  h.type("Гильдия пряностей");
  h.key(Key::Enter);
  h.step();
  CHECK_EQ(h->world().faction(gid)->name, std::string("Гильдия пряностей"));
  // Поиск в списке гильдий.
  CHECK(h.clickUi("guilds.search"));
  h.type("пряност");
  h.step();
  CHECK(h->uiRect("guilds.row." + std::to_string(gid)) != nullptr);
  CHECK(h.shot("faction_drawer_guilds_search"));
  // Щелчок по строке списка открывает инспектор.
  h->clearSelection();
  h.step();
  CHECK(h.clickUi("guilds.row." + std::to_string(gid)));
  CHECK(h->ui.sel == (app::Selection{app::SelType::Faction, gid}));
}

// ---------------------------------------------------------------- редактор флага
TEST(app_faction_flag_editor) {
  HideTestRegs regs;
  Harness h("faction_flag");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id st = findFaction(h->world(), "Королевство Альмарин");
  openTab(h, st, "faction.overview");
  const Flag orig = h->world().faction(st)->flag;
  CHECK(h.clickUi("faction.flag"));
  h.step();
  CHECK(h->hasDialog("flag"));
  CHECK(h.clickUi("flag.pattern." + std::to_string(int(FlagPattern::Saltire))));
  CHECK(h.clickUi("flag.emblem.wolf"));
  h.step();
  CHECK(h.shot("faction_flag_editor_changed"));
  CHECK(h.clickUi("flag.ok"));
  h.step();
  CHECK(!h->hasDialog("flag"));
  const Flag& f1 = h->world().faction(st)->flag;
  CHECK(f1.pattern == FlagPattern::Saltire);
  CHECK_EQ(f1.emblem, std::string("wolf"));
  h->undo();
  h.step();
  CHECK(h->world().faction(st)->flag.pattern == orig.pattern);
  CHECK_EQ(h->world().faction(st)->flag.emblem, orig.emblem);

  // «Без эмблемы» и отмена диалога не меняют мир.
  CHECK(h.clickUi("faction.flag"));
  h.step();
  CHECK(h.clickUi("flag.emblem.none"));
  CHECK(h.clickUi("flag.cancel"));
  h.step();
  CHECK_EQ(h->world().faction(st)->flag.emblem, orig.emblem);

  // Изображение PNG через системный диалог.
  std::string png = fs::join(h.root, "flag.png");
  {
    codec::RgbaImage img;
    img.w = 90;
    img.h = 60;
    img.rgba.resize(size_t(img.w * img.h * 4));
    for (int y = 0; y < img.h; y++)
      for (int x = 0; x < img.w; x++) {
        u8* p = &img.rgba[size_t((y * img.w + x) * 4)];
        p[0] = x < 30 ? 30 : x < 60 ? 240 : 200;
        p[1] = x < 30 ? 80 : x < 60 ? 240 : 40;
        p[2] = x < 30 ? 200 : x < 60 ? 240 : 40;
        p[3] = 255;
      }
    CHECK(codec::writePngFile(png, img));
  }
  hl::setDialogsSupported(true);
  hl::queueDialogResult(png);
  CHECK(h.clickUi("faction.flag"));
  h.step();
  CHECK(h.clickUi("flag.upload"));
  h.step();
  CHECK(h->uiRect("flag.reset") != nullptr);
  CHECK(h.shot("faction_flag_editor_image"));
  CHECK(h.clickUi("flag.ok"));
  h.step();
  {
    const Flag& f2 = h->world().faction(st)->flag;
    CHECK(f2.image);
    auto dec = codec::decodePng(f2.png);
    CHECK(dec.has_value());
    CHECK(dec && dec->w == 90 && dec->h == 60);
  }
  // Без системных диалогов — ввод пути; JPEG сохраняется как PNG.
  hl::setDialogsSupported(false);
  CHECK(h.clickUi("faction.flag"));
  h.step();
  CHECK(h.clickUi("flag.upload"));
  h.step();
  CHECK(h->hasDialog("prompt"));
  CHECK(h.clickUi("dialog.field"));
  h.type(fs::absolute(test::dataPath("jpeg/b420.jpg")));
  CHECK(h.clickUi("dialog.ok"));
  h.step();
  CHECK(h->hasDialog("flag"));
  // Сброс к узору и снова изображение — затем применить изображение JPEG.
  CHECK(h.clickUi("flag.reset"));
  h.step();
  CHECK(h->uiRect("flag.reset") == nullptr);
  CHECK(h.clickUi("flag.upload"));
  h.step();
  CHECK(h.clickUi("dialog.field"));
  h.type(fs::absolute(test::dataPath("jpeg/b420.jpg")));
  CHECK(h.clickUi("dialog.ok"));
  h.step();
  CHECK(h.clickUi("flag.ok"));
  h.step();
  {
    const Flag& f3 = h->world().faction(st)->flag;
    CHECK(f3.image);
    CHECK(codec::decodePng(f3.png).has_value());   // JPEG перекодирован в PNG (FORMAT.md)
  }
  // Неверный путь — предупреждение поверх диалога, флаг прежний.
  h.dropToasts();
  CHECK(h.clickUi("faction.flag"));
  h.step();
  CHECK(h.clickUi("flag.upload"));
  h.step();
  CHECK(h.clickUi("dialog.field"));
  h.type(fs::join(h.root, "нет-такого-файла.png"));
  CHECK(h.clickUi("dialog.ok"));
  h.step();
  CHECK(h->hasDialog("flag"));
  CHECK(!h->hasDialog("prompt"));
  CHECK(ui::toastCount() > 0 || liveToasts(h) > 0);
  CHECK(h.clickUi("flag.cancel"));
  h.step();
  CHECK(h->world().faction(st)->flag.image);
}

// ---------------------------------------------------------------- отношения (ТЗ 1.b.vi)
TEST(app_faction_relations_symmetric) {
  HideTestRegs regs;
  Harness h("faction_relations");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id a = findFaction(h->world(), "Королевство Альмарин");
  Id b = findFaction(h->world(), "Вольные города Ольсты");
  CHECK(a && b);
  CHECK(h->world().relation(a, b).s != RelStatus::War);
  openTab(h, a, "faction.diplomacy");
  // Таблица заполнена всеми прочими фракциями.
  h->world().factions.each([&](const Faction& f) {
    if (f.id != a) CHECK(h->uiRect("dip.row." + std::to_string(f.id)) != nullptr);
  });
  CHECK(clickIn(h, "dip.status." + std::to_string(b)));
  CHECK(h.clickUi("status.item." + std::to_string(int(RelStatus::War))));
  h.step();
  CHECK(h->world().relation(a, b).s == RelStatus::War);
  CHECK(typeNumber(h, "dip.value." + std::to_string(b), "-40"));
  CHECK_NEAR(h->world().relation(a, b).v, -40, 1e-9);
  CHECK(h.shot("faction_diplomacy_war_a"));
  // Вкладка другой стороны показывает то же самое.
  openTab(h, b, "faction.diplomacy");
  CHECK(h->uiRect("dip.status." + std::to_string(a)) != nullptr);
  bool seen = false;
  for (auto& r : rules::relationsOf(h->world(), b))
    if (r.other == a) {
      seen = true;
      CHECK(r.status == RelStatus::War);
      CHECK_NEAR(r.value, -40, 1e-9);
    }
  CHECK(seen);
  CHECK(h.shot("faction_diplomacy_war_b"));
  // Изменение со стороны B — союз; видно у A.
  CHECK(clickIn(h, "dip.status." + std::to_string(a)));
  CHECK(h.clickUi("status.item." + std::to_string(int(RelStatus::Alliance))));
  h.step();
  CHECK(h->world().relation(b, a).s == RelStatus::Alliance);
  CHECK_NEAR(h->world().relation(a, b).v, -40, 1e-9);
  // Фильтр «Гильдии» скрывает государства.
  CHECK(h->uiRect("dip.filter") != nullptr);
  {
    RectF fr = *h->uiRect("dip.filter");
    h.click(fr.x + fr.w * 5 / 6, fr.cy());
    h.step();
    CHECK(h->uiRect("dip.row." + std::to_string(a)) == nullptr);
    h.click(fr.x + fr.w / 6, fr.cy());
    h.step();
  }
  // Отмена трёх шагов возвращает исходное.
  h->undo();
  h->undo();
  h->undo();
  h.step();
  CHECK(h->world().relation(a, b).s != RelStatus::War);
  CHECK(h->world().relation(a, b).s != RelStatus::Alliance);
}

// ---------------------------------------------------------------- совет (ТЗ 1.b.iv)
TEST(app_faction_council_rows) {
  HideTestRegs regs;
  Harness h("faction_council");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id a = findFaction(h->world(), "Республика Корвен");
  openTab(h, a, "faction.council");
  const size_t n0 = h->world().faction(a)->council.size();
  CHECK(n0 > 0);
  CHECK(clickIn(h, "council.add"));
  h.step();
  const Faction* f = h->world().faction(a);
  CHECK_EQ(f->council.size(), n0 + 1);
  // Новое место получает свободную должность справочника.
  const std::string pos = f->council.back().position;
  CHECK(!pos.empty());
  for (size_t i = 0; i + 1 < f->council.size(); i++) CHECK(f->council[i].position != pos);
  CHECK(f->council.back().character == 0);
  h.dropToasts();
  CHECK(h.shot("faction_council_added"));
  // Удалить новое место; уведомление предлагает отмену.
  CHECK(clickIn(h, "council.remove." + std::to_string(n0)));
  h.step();
  CHECK_EQ(h->world().faction(a)->council.size(), n0);
  CHECK(!h->toasts().empty());
  CHECK(!h->toasts().back().actionLabel.empty());
  h->undo();
  h.step();
  CHECK_EQ(h->world().faction(a)->council.size(), n0 + 1);
}

// ---------------------------------------------------------------- налог и ресурсы (ТЗ 1.d.iv, 1.b.iv)
TEST(app_faction_tax_and_resources) {
  HideTestRegs regs;
  Harness h("faction_economy");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id a = findFaction(h->world(), "Королевство Альмарин");
  openTab(h, a, "faction.economy");
  const double tax0 = h->world().faction(a)->tax;
  CHECK(typeNumber(h, "economy.tax", "17"));
  CHECK_NEAR(h->world().faction(a)->tax, 17, 1e-9);
  // Налог государства не бывает отрицательным.
  CHECK(typeNumber(h, "economy.tax", "-5"));
  CHECK_NEAR(h->world().faction(a)->tax, 0, 1e-9);
  // Запас зерна правится, ниже нуля не опускается; казна может уйти в долг.
  CHECK(typeNumber(h, "economy.stock.2", "1234"));
  CHECK_NEAR(h->world().faction(a)->stock(2), 1234, 1e-9);
  CHECK(typeNumber(h, "economy.stock.2", "-7"));
  CHECK_NEAR(h->world().faction(a)->stock(2), 0, 1e-9);
  CHECK(typeNumber(h, "economy.stock.1", "-50"));
  CHECK_NEAR(h->world().faction(a)->treasury(), -50, 1e-9);
  CHECK(h.shot("faction_economy_edited"));
  for (int i = 0; i < 5; i++) h->undo();
  h.step();
  CHECK_NEAR(h->world().faction(a)->tax, tax0, 1e-9);
  // Казна в шапке ведёт на вкладку экономики.
  openTab(h, a, "faction.overview");
  CHECK(h.clickUi("faction.treasury"));
  CHECK_EQ(h->ui.tabOf[app::SelType::Faction], std::string("faction.economy"));
}

// ---------------------------------------------------------------- дань и репарации (ТЗ 1.e.ii)
TEST(app_faction_tributes) {
  HideTestRegs regs;
  Harness h("faction_tribute");
  h.demo();
  h.waitMap();
  h.dropToasts();
  // Плательщик репараций в демонстрационном мире.
  Id payer = 0, deal = 0;
  h->world().deals.each([&](const Deal& d) {
    if (!deal && d.kind == DealKind::Reparations && d.status == DealStatus::Active) {
      deal = d.id;
      payer = d.b;
    }
  });
  CHECK(deal != 0);
  openTab(h, payer, "faction.economy");
  CHECK(ensureVisible(h, "tribute.cancel." + std::to_string(deal)));
  CHECK(h.shot("faction_economy_tribute"));
  CHECK(h.clickUi("tribute.cancel." + std::to_string(deal)));
  h.step();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.step();
  CHECK(h->world().deal(deal)->status == DealStatus::Cancelled);
  h->undo();
  h.step();
  CHECK(h->world().deal(deal)->status == DealStatus::Active);
  // «Навязать дань» открывает диалог «tribute» (или сообщает, что его нет).
  h.dropToasts();
  CHECK(clickIn(h, "economy.impose"));
  h.step();
  if (app::findDialog("tribute")) CHECK(h->hasDialog("tribute"));
  else CHECK(!h->toasts().empty());
}

// ---------------------------------------------------------------- государственная гильдия (ТЗ 1.d.vi)
TEST(app_faction_state_guild) {
  HideTestRegs regs;
  Harness h("faction_state_guild");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id a = findFaction(h->world(), "Республика Корвен");
  openTab(h, a, "faction.guilds");
  const u32 n0 = h->world().factions.size();
  CHECK(clickIn(h, "guilds.create"));
  h.step();
  CHECK_EQ(h->world().factions.size(), n0 + 1);
  Id g = 0;
  h->world().factions.each([&](const Faction& f) {
    if (f.isGuild() && f.stateGuild && f.homeState == a) g = f.id;
  });
  CHECK(g != 0);
  CHECK(h->uiRect("guilds.row." + std::to_string(g)) != nullptr);
  CHECK(h.shot("faction_state_guild_created"));
  // Государство расположения закреплено: выбор недоступен, правило отказывает.
  CHECK(h.clickUi("guilds.row." + std::to_string(g)));
  h.step();
  CHECK(h->ui.sel == (app::Selection{app::SelType::Faction, g}));
  openTab(h, g, "faction.overview");
  CHECK(clickIn(h, "overview.home"));
  h.key(Key::Down);
  h.key(Key::Enter);
  h.step();
  CHECK_EQ(h->world().faction(g)->homeState, a);
  CHECK(!h->act("Государство гильдии", [&](Tx& tx) { rules::setHomeState(tx, g, findFaction(tx.w(), "Княжество Мирель")); }));
  CHECK_EQ(h->world().faction(g)->homeState, a);
  // Обычная гильдия меняет государство через выбор.
  Id amber = findFaction(h->world(), "Янтарная лига");
  Id mirel = findFaction(h->world(), "Княжество Мирель");
  openTab(h, amber, "faction.overview");
  CHECK(clickIn(h, "overview.home"));
  // Список государств по алфавиту: «Вольные города Ольсты» (текущее) → «Империя…» → «Княжество Мирель».
  h.key(Key::Down);
  h.key(Key::Down);
  h.key(Key::Enter);
  h.step();
  CHECK_EQ(h->world().faction(amber)->homeState, mirel);
  // Меню шапки государства тоже учреждает государственную гильдию.
  openTab(h, a, "faction.overview");
  CHECK(h.clickUi("faction.more"));
  h.step();
  CHECK(h.clickUi("faction.menu.stateguild"));
  h.step();
  CHECK_EQ(h->world().factions.size(), n0 + 2);
}

// ---------------------------------------------------------------- удаление с очисткой ссылок
TEST(app_faction_delete_cleanup) {
  HideTestRegs regs;
  Harness h("faction_delete");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id a = findFaction(h->world(), "Королевство Альмарин");
  Id sg = findFaction(h->world(), "Альмарская торговая компания");
  std::vector<Id> owned;
  h->world().provinces.each([&](const Province& p) {
    if (p.owner == a) owned.push_back(p.id);
  });
  CHECK(!owned.empty());
  CHECK(h->world().faction(sg)->homeState == a);
  openTab(h, a, "faction.overview");
  CHECK(h.clickUi("faction.more"));
  h.step();
  CHECK(h.clickUi("faction.menu.delete"));
  h.step();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.shot("faction_delete_confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.step();
  CHECK(h->world().faction(a) == nullptr);
  CHECK(!h->ui.sel);
  for (Id p : owned) CHECK_EQ(h->world().province(p)->owner, Id(0));
  CHECK_EQ(h->world().faction(sg)->homeState, Id(0));
  CHECK(!h->world().faction(sg)->stateGuild);
  for (auto& [k, r] : *h->world().relations) CHECK(Id(k >> 32) != a && Id(k & 0xFFFFFFFFu) != a);
  h->undo();
  h.step();
  CHECK(h->world().faction(a) != nullptr);
  for (Id p : owned) CHECK_EQ(h->world().province(p)->owner, a);
  CHECK(h->world().faction(sg)->stateGuild);
  // Удаление из контекстного меню списка; «Отмена» в подтверждении ничего не меняет.
  h->openDrawer("guilds");
  h.step();
  CHECK(h.clickUi("guilds.row." + std::to_string(sg), platform::MouseRight));
  h.step();
  CHECK(h.clickUi("guilds.ctx.delete"));
  h.step();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.cancel"));
  h.step();
  CHECK(h->world().faction(sg) != nullptr);
}

// ---------------------------------------------------------------- штабы гильдии (ТЗ 1.d.ii)
TEST(app_faction_guild_hqs) {
  HideTestRegs regs;
  Harness h("faction_hqs");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id g = findFaction(h->world(), "Гильдия магов Аркана");
  CHECK(g != 0);
  // Провинция без штаба этой гильдии и со свободным местом, с уникальным названием.
  std::map<std::string, int> names;
  h->world().provinces.each([&](const Province& p) { names[p.name]++; });
  Id target = 0;
  std::string tname;
  h->world().provinces.each([&](const Province& p) {
    if (target || p.sea || p.name.empty() || names[p.name] > 1) return;
    if (int(p.hqs.size()) >= 3 || std::find(p.hqs.begin(), p.hqs.end(), g) != p.hqs.end()) return;
    target = p.id;
    tname = p.name;
  });
  CHECK(target != 0);
  openTab(h, g, "faction.hqs");
  size_t hq0 = rules::calc(h->world())->faction(g)->provinces.size();
  CHECK(h.clickUi("hq.pick"));
  h.type(tname);
  h.key(Key::Enter);
  h.step();
  CHECK(h.clickUi("hq.add"));
  h.step();
  const Province* p = h->world().province(target);
  CHECK(std::find(p->hqs.begin(), p->hqs.end(), g) != p->hqs.end());
  CHECK_EQ(rules::calc(h->world())->faction(g)->provinces.size(), hq0 + 1);
  CHECK(h.shot("faction_guild_hq_added"));
  // Закрыть первый штаб таблицы (с подтверждением).
  CHECK(clickIn(h, "hq.remove.0"));
  h.step();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.step();
  CHECK_EQ(rules::calc(h->world())->faction(g)->provinces.size(), hq0);
  h->undo();
  h->undo();
  h.step();
  CHECK_EQ(rules::calc(h->world())->faction(g)->provinces.size(), hq0);
  p = h->world().province(target);
  CHECK(std::find(p->hqs.begin(), p->hqs.end(), g) == p->hqs.end());
}

// ---------------------------------------------------------------- герои и модификаторы (ТЗ 1.b.iv, 1.b.iii)
TEST(app_faction_heroes_and_modifiers) {
  HideTestRegs regs;
  Harness h("faction_heroes");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id a = findFaction(h->world(), "Княжество Мирель");
  auto heroes = [&] {
    int n = 0;
    h->world().characters.each([&](const Character& c) {
      if (c.faction == a && c.hero) n++;
    });
    return n;
  };
  openTab(h, a, "faction.heroes");
  int n0 = heroes();
  CHECK(n0 > 0);
  CHECK(clickIn(h, "heroes.new"));
  h.step();
  CHECK_EQ(heroes(), n0 + 1);
  CHECK(clickIn(h, "heroes.remove.0"));
  h.step();
  CHECK_EQ(heroes(), n0);
  h->undo();
  h.step();
  CHECK_EQ(heroes(), n0 + 1);
  // Модификатор фракции из выбора: добавляется в список и меняет эффекты.
  openTab(h, a, "faction.modifiers");
  const size_t m0 = h->world().faction(a)->modifiers.size();
  std::string mname;
  h->world().modifiers.each([&](const Modifier& m) {
    if (mname.empty() && std::find(h->world().faction(a)->modifiers.begin(), h->world().faction(a)->modifiers.end(), m.id) ==
                             h->world().faction(a)->modifiers.end())
      mname = m.name;
  });
  CHECK(!mname.empty());
  CHECK(clickIn(h, "mods.add"));
  h.type(mname);
  h.key(Key::Enter);
  h.step();
  CHECK_EQ(h->world().faction(a)->modifiers.size(), m0 + 1);
  CHECK(h.shot("faction_modifiers_added"));
  h->undo();
  h.step();
  CHECK_EQ(h->world().faction(a)->modifiers.size(), m0);
  // Много модификаторов: фишки переносятся по строкам и не выходят за панель; крестик убирает модификатор.
  std::vector<Id> all = h->world().modifiers.ids();
  CHECK(all.size() >= 5);
  CHECK(h->act("Модификаторы фракции", [&](Tx& tx) { tx.faction(a).modifiers = all; }));
  h.settle();
  const RectF* in = h->uiRect("inspector");
  CHECK(in != nullptr);
  float top = 1e9f, bottom = -1e9f;
  for (size_t i = 0; i < all.size(); i++) {
    const RectF* r = h->uiRect("mods.chip." + std::to_string(i));
    CHECK(r != nullptr);
    if (!r || !in) continue;
    CHECK(r->right() <= in->right() - 8);
    top = std::min(top, r->y);
    bottom = std::max(bottom, r->bottom());
  }
  CHECK(bottom - top > 40);   // больше одной строки
  CHECK(h.shot("faction_modifiers_many"));
  const RectF* c0 = h->uiRect("mods.chip.0");
  CHECK(c0 != nullptr);
  if (c0) {
    RectF r = *c0;
    h.click(r.right() - 14, r.cy());
  }
  CHECK_EQ(h->world().faction(a)->modifiers.size(), all.size() - 1);
  CHECK(std::find(h->world().faction(a)->modifiers.begin(), h->world().faction(a)->modifiers.end(), all[0]) == h->world().faction(a)->modifiers.end());
}

// ---------------------------------------------------------------- просмотр прошлого хода
TEST(app_faction_read_only) {
  HideTestRegs regs;
  Harness h("faction_readonly");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id a = findFaction(h->world(), "Королевство Альмарин");
  CHECK(h->endTurnNow());
  h.step();
  CHECK(h->viewTurn(1));
  h.step();
  CHECK(h->readOnly());
  openTab(h, a, "faction.council");
  CHECK(h->uiRect("council.add") == nullptr);
  CHECK(h.clickUi("faction.name"));
  CHECK(h->uiRect("faction.rename") == nullptr);
  openTab(h, a, "faction.economy");
  const double tax = h->world().faction(a)->tax;
  CHECK(typeNumber(h, "economy.tax", "33"));
  CHECK_NEAR(h->world().faction(a)->tax, tax, 1e-9);
  CHECK(h.shot("faction_read_only"));
  h->backToCurrent();
  h.step();
  CHECK(!h->readOnly());
}

// ---------------------------------------------------------------- обзор: столица, правитель, справочники (ТЗ 1.b.iv)
TEST(app_faction_overview_fields) {
  HideTestRegs regs;
  Harness h("faction_overview");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id a = findFaction(h->world(), "Республика Корвен");
  CHECK(a != 0);
  openTab(h, a, "faction.overview");
  // Столица — только своя провинция; список: «Не выбрана», затем провинции по алфавиту.
  std::vector<const Province*> own;
  h->world().provinces.each([&](const Province& p) {
    if (p.owner == a && !p.sea) own.push_back(&p);
  });
  std::sort(own.begin(), own.end(), [](const Province* x, const Province* y) { return compareRu(x->name, y->name) < 0; });
  CHECK(own.size() >= 2);
  const Id cap0 = h->world().faction(a)->capital;
  const size_t target = own[0]->id == cap0 ? 1 : 0;
  const Id newCap = own[target]->id;
  CHECK(clickIn(h, "overview.capital"));
  h.key(Key::PageUp);
  for (size_t k = 0; k <= target; k++) h.key(Key::Down);
  h.key(Key::Enter);
  h.step();
  CHECK_EQ(h->world().faction(a)->capital, newCap);
  h->undo();
  h.step();
  CHECK_EQ(h->world().faction(a)->capital, cap0);

  // Титул правителя.
  CHECK(clickIn(h, "overview.title"));
  h.retype("Верховный консул");
  h.key(Key::Enter);
  h.step();
  CHECK_EQ(h->world().faction(a)->rulerTitle, std::string("Верховный консул"));

  // Правитель — выбор персонажа (поиск по имени).
  Id candidate = 0;
  std::string cname;
  h->world().characters.each([&](const Character& c) {
    if (!candidate && c.faction == a && c.id != h->world().faction(a)->ruler && !c.name.empty()) {
      candidate = c.id;
      cname = c.name;
    }
  });
  CHECK(candidate != 0);
  CHECK(pickInCombo(h, "overview.ruler", cname));
  CHECK_EQ(h->world().faction(a)->ruler, candidate);

  // Новая форма правления: «Добавить…» → название → запись создана и назначена одним действием.
  const size_t g0 = h->world().catalogs->governments.size();
  CHECK(clickIn(h, "overview.gov"));
  h.key(Key::PageDown);
  h.key(Key::PageDown);
  h.key(Key::Enter);
  h.step();
  CHECK(h->hasDialog("prompt"));
  CHECK(answerPrompt(h, "Триумвират"));
  CHECK_EQ(h->world().catalogs->governments.size(), g0 + 1);
  const CatalogItem* gov = Catalogs::find(h->world().catalogs->governments, h->world().faction(a)->government);
  CHECK(gov && gov->name == "Триумвират");
  CHECK(h.shot("faction_overview_fields"));
  h->undo();
  h.step();
  CHECK_EQ(h->world().catalogs->governments.size(), g0);
  const CatalogItem* gov2 = Catalogs::find(h->world().catalogs->governments, h->world().faction(a)->government);
  CHECK(!gov2 || gov2->name != "Триумвират");
}

// ---------------------------------------------------------------- совет: своя должность и содержание
TEST(app_faction_council_edit) {
  HideTestRegs regs;
  Harness h("faction_council_edit");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id a = findFaction(h->world(), "Королевство Альмарин");
  openTab(h, a, "faction.council");
  const Faction* f = h->world().faction(a);
  CHECK(!f->council.empty());
  const Id seat0 = f->council[0].id;
  const Id who0 = f->council[0].character;
  CHECK(who0 != 0);
  // Своя должность: последний пункт списка — запрос названия.
  CHECK(clickIn(h, "council.pos.0"));
  h.key(Key::PageDown);
  h.key(Key::PageDown);
  h.key(Key::Enter);
  h.step();
  CHECK(h->hasDialog("prompt"));
  CHECK(answerPrompt(h, "Хранитель печати"));
  CHECK_EQ(h->world().faction(a)->council[0].position, std::string("Хранитель печати"));
  CHECK_EQ(h->world().faction(a)->council[0].id, seat0);
  // Содержание советника правится в строке совета и попадает в расход «специалисты».
  const double spec0 = rules::calc(h->world())->faction(a)->expSpecialists;
  const double up0 = h->world().character(who0)->upkeep;
  CHECK(typeNumber(h, "council.upkeep.0", "40"));
  CHECK_NEAR(h->world().character(who0)->upkeep, 40, 1e-9);
  CHECK_NEAR(rules::calc(h->world())->faction(a)->expSpecialists, spec0 + 40 - up0, 1e-6);
  // Отрицательное содержание не принимается.
  CHECK(typeNumber(h, "council.upkeep.0", "-5"));
  CHECK(h->world().character(who0)->upkeep >= 0);
  CHECK(h.shot("faction_council_edit"));
  // Вакантное место: поля содержания нет.
  CHECK(h->act("Вакансия", [&](Tx& tx) { tx.faction(a).council[0].character = 0; }));
  h.step();
  CHECK(h->uiRect("council.upkeep.0") == nullptr);
  h->undo();
  h.step();
  CHECK_EQ(h->world().faction(a)->council[0].character, who0);
}

// ---------------------------------------------------------------- списки: сочетания, сортировка, двойной щелчок
TEST(app_faction_drawers_keys_sort) {
  HideTestRegs regs;
  Harness h("faction_drawers");
  h.demo();
  h.waitMap();
  h.dropToasts();
  h.key(Key::D2, ctrl());
  CHECK_EQ(h->ui.drawer, std::string("states"));
  h.key(Key::D3, ctrl());
  CHECK_EQ(h->ui.drawer, std::string("guilds"));
  h.key(Key::D2, ctrl());
  CHECK_EQ(h->ui.drawer, std::string("states"));
  // Сортировка по казне: строка самой богатой фракции — первая.
  CHECK(h.clickUi("states.sort"));
  h.step();
  h.key(Key::Down);
  h.key(Key::Down);
  h.key(Key::Enter);
  h.step();
  Id richest = 0;
  double best = -1e18;
  h->world().factions.each([&](const Faction& f) {
    if (f.isState() && f.treasury() > best) {
      best = f.treasury();
      richest = f.id;
    }
  });
  float topY = 1e9f;
  Id top = 0;
  h->world().factions.each([&](const Faction& f) {
    if (!f.isState()) return;
    if (const RectF* r = h->uiRect("states.row." + std::to_string(f.id)); r && r->y < topY) {
      topY = r->y;
      top = f.id;
    }
  });
  CHECK_EQ(top, richest);
  // Двойной щелчок — выбрать и показать на карте.
  const RectF* row = h->uiRect("states.row." + std::to_string(richest));
  CHECK(row != nullptr);
  if (row) {
    RectF rr = *row;
    h.doubleClick(rr.cx(), rr.cy());
  }
  CHECK(h->ui.sel == (app::Selection{app::SelType::Faction, richest}));
  h.settle();
  CHECK(h.shot("faction_drawer_sorted"));
}

// ---------------------------------------------------------------- влияние штаба (ТЗ 1.d.iv)
TEST(app_faction_hq_influence) {
  HideTestRegs regs;
  Harness h("faction_hq_influence");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id g = findFaction(h->world(), "Альмарская торговая компания");
  openTab(h, g, "faction.hqs");
  auto calc0 = rules::calc(h->world());
  const rules::FactionCalc* fc = calc0->faction(g);
  CHECK(fc && !fc->provinces.empty());
  const Id pid = fc->provinces[0];
  const double inc0 = fc->incGuilds;
  auto influenceOf = [&](Id guild) {
    double v = 0;
    for (const Influence& in : h->world().province(pid)->influence)
      if (in.guild == guild) v += in.pct;
    return v;
  };
  double total0 = 0;
  for (const Influence& in : h->world().province(pid)->influence) total0 += in.pct;
  const double pct0 = influenceOf(g);
  const double want = std::min(100.0 - (total0 - pct0), pct0 + 10);
  CHECK(want > pct0);
  CHECK(typeNumber(h, "hq.influence." + std::to_string(pid), fmtNum(want)));
  CHECK_NEAR(influenceOf(g), want, 1e-9);
  CHECK(rules::calc(h->world())->faction(g)->incGuilds > inc0);
  // Сумма влияния в провинции не превышает 100 %.
  h.dropToasts();
  CHECK(typeNumber(h, "hq.influence." + std::to_string(pid), "100"));
  double total = 0;
  for (const Influence& in : h->world().province(pid)->influence) total += in.pct;
  CHECK(total <= 100 + 1e-9);
  h->undo();
  h.step();
  CHECK_NEAR(influenceOf(g), pct0, 1e-9);
}

// ---------------------------------------------------------------- герои: назначить существующего персонажа
TEST(app_faction_heroes_assign) {
  HideTestRegs regs;
  Harness h("faction_heroes_assign");
  h.demo();
  h.waitMap();
  h.dropToasts();
  Id a = findFaction(h->world(), "Королевство Альмарин");
  Id cand = 0;
  std::string cname;
  h->world().characters.each([&](const Character& c) {
    if (!cand && c.faction == a && !c.hero && !c.name.empty()) {
      cand = c.id;
      cname = c.name;
    }
  });
  CHECK(cand != 0);
  openTab(h, a, "faction.heroes");
  CHECK(clickIn(h, "heroes.assign"));
  // Порядок списка: кандидаты фракции первыми, по алфавиту; длинный список — с поиском.
  std::vector<const Character*> c;
  h->world().characters.each([&](const Character& x) {
    if (!x.hero && (x.faction == a || x.faction == 0)) c.push_back(&x);
  });
  std::sort(c.begin(), c.end(), [a](const Character* x, const Character* y) {
    if ((x->faction == a) != (y->faction == a)) return x->faction == a;
    return compareRu(x->name, y->name) < 0;
  });
  size_t k = 0;
  while (k < c.size() && c[k]->id != cand) k++;
  CHECK(k < c.size());
  if (c.size() > 8) {
    h.type(cname);
  } else {
    for (size_t i = 0; i < k; i++) h.key(Key::Down);
  }
  h.key(Key::Enter);
  h.step();
  CHECK(h->world().character(cand)->hero);
  CHECK_EQ(h->world().character(cand)->faction, a);
  h->undo();
  h.step();
  CHECK(!h->world().character(cand)->hero);
}
