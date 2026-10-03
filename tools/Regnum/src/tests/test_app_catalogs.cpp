// Сценарии окна «Модификаторы» (ТЗ 1.g), окна «Справочники» и панели «Персонажи»: настоящие щелчки и ввод
// на демонстрационном мире, проверки мира (в том числе отмена Ctrl+Z) и снимки экрана.
#include "codec/png.h"
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

// Короткое ожидание: события и плавная прокрутка (settle ждал бы и мигание каретки после ввода).
void quick(Harness& h) {
  h.step();
  h.frames(16);
}

// Прокрутить окно редактора колесом, пока элемент name не окажется в видимой части области area.
bool reveal(Harness& h, const std::string& name, const char* area = "editor", float top = 64) {
  for (int i = 0; i < 40; i++) {
    const RectF* r = h->uiRect(name);
    const RectF* ar = h->uiRect(area);
    if (!r || !ar) return false;
    float lo = ar->y + top, hi = ar->bottom() - 12;
    if (r->y >= lo && r->bottom() <= hi) return true;
    float notches = r->bottom() > hi ? -2.f : 2.f;
    h.wheel(r->cx(), (lo + hi) * 0.5f, notches);
    quick(h);
  }
  return false;
}

// Щелчок по элементу с прокруткой к нему.
bool clickRevealed(Harness& h, const std::string& name, const char* area = "editor") {
  if (!reveal(h, name, area)) return false;
  return h.clickUi(name);
}

// Выбрать в поле со списком пункт по тексту поиска (щелчок, ввод, Enter).
bool pickInCombo(Harness& h, const std::string& name, const std::string& query, const char* area = "editor") {
  if (!clickRevealed(h, name, area)) return false;
  h.type(query);
  h.key(Key::Enter);
  quick(h);
  return true;
}

// Ввести значение в поле (щелчок — всё выделено, ввод, Enter).
bool enterValue(Harness& h, const std::string& name, const std::string& value, const char* area = "editor") {
  if (!clickRevealed(h, name, area)) return false;
  h.retype(value);
  h.key(Key::Enter);
  quick(h);
  return true;
}

const Modifier* modifierNamed(const World& w, const std::string& name) {
  const Modifier* r = nullptr;
  w.modifiers.each([&](const Modifier& m) {
    if (m.name == name) r = &m;
  });
  return r;
}

Id landProvince(const World& w, Id owner = 0, int skip = 0) {
  Id r = 0;
  w.provinces.each([&](const Province& p) {
    if (r || p.sea || p.name.empty() || (owner && p.owner != owner)) return;
    if (skip-- > 0) return;
    r = p.id;
  });
  return r;
}

bool confirmDialog(Harness& h) {
  quick(h);
  if (!h->hasDialog("confirm")) return false;
  if (!h.clickUi("dialog.ok")) return false;
  quick(h);
  return !h->hasDialog("confirm");
}

// Небольшой портрет PNG: градиент с «лицом» (проверка декодирования и отрисовки).
std::string writeTestPortrait(const std::string& dir) {
  codec::RgbaImage img;
  img.w = 240;
  img.h = 320;
  img.rgba.resize(size_t(img.w) * size_t(img.h) * 4);
  for (int y = 0; y < img.h; y++)
    for (int x = 0; x < img.w; x++) {
      u8* p = &img.rgba[(size_t(y) * size_t(img.w) + size_t(x)) * 4];
      double dx = (x - 120) / 70.0, dy = (y - 120) / 90.0;
      bool face = dx * dx + dy * dy < 1;
      double sx = (x - 120) / 120.0, sy = (y - 330) / 120.0;
      bool body = sx * sx + sy * sy < 1;
      p[0] = u8(face ? 226 : body ? 120 : 40 + y / 6);
      p[1] = u8(face ? 186 : body ? 40 : 52 + y / 8);
      p[2] = u8(face ? 150 : body ? 52 : 90 + x / 6);
      p[3] = 255;
    }
  std::string path = fs::join(dir, "portrait.png");
  codec::writePngFile(path, img, 6);
  return path;
}

}  // namespace

// ================================================================ модификаторы
TEST(app_catalogs_modifier_create_effects) {
  Harness h("catalogs_modifier");
  h.demo();
  h.dropToasts();
  // Лента слева → панель «Модификаторы» → окно.
  CHECK(h.clickUi("drawer.modifiers"));
  quick(h);
  CHECK_EQ(h->ui.drawer, std::string("modifiers"));
  CHECK(h.clickUi("drawer.modifiers.open"));
  quick(h);
  CHECK_EQ(h->ui.editor, std::string("modifiers"));
  u32 before = h->world().modifiers.size();
  // Новый модификатор: название получает фокус.
  CHECK(h.clickUi("modifiers.new"));
  quick(h);
  CHECK_EQ(h->world().modifiers.size(), before + 1);
  Id mid = h->ui.editorArg;
  CHECK(h->world().modifier(mid) != nullptr);
  h.retype("Благословение урожая");
  h.key(Key::Enter);
  quick(h);
  CHECK_EQ(h->world().modifier(mid)->name, std::string("Благословение урожая"));
  // Прирост населения: включить, ввести 80 — ограничится пределом ТЗ +50 %.
  CHECK(clickRevealed(h, "modifiers.fx.popGrowthPct.on"));
  CHECK(h->world().modifier(mid)->has(Fx::PopGrowthPct));
  CHECK(enterValue(h, "modifiers.fx.popGrowthPct.value", "80"));
  CHECK_NEAR(h->world().modifier(mid)->fx[size_t(Fx::PopGrowthPct)], 50.0, 1e-9);
  // Количество торговой ценности: −9000 → −5000.
  CHECK(clickRevealed(h, "modifiers.fx.tradeFlat.on"));
  CHECK(enterValue(h, "modifiers.fx.tradeFlat.value", "-9000"));
  CHECK_NEAR(h->world().modifier(mid)->fx[size_t(Fx::TradeFlat)], -5000.0, 1e-9);
  // Довольство за ход: 7.
  CHECK(clickRevealed(h, "modifiers.fx.contentmentPerTurn.on"));
  CHECK(enterValue(h, "modifiers.fx.contentmentPerTurn.value", "7"));
  CHECK_NEAR(h->world().modifier(mid)->fx[size_t(Fx::ContentmentPerTurn)], 7.0, 1e-9);
  // Содержание флота: −120 → −75.
  CHECK(clickRevealed(h, "modifiers.fx.fleetUpkeepPct.on"));
  CHECK(enterValue(h, "modifiers.fx.fleetUpkeepPct.value", "-120"));
  CHECK_NEAR(h->world().modifier(mid)->fx[size_t(Fx::FleetUpkeepPct)], -75.0, 1e-9);
  // Дипломатия: 40 → 25, цели обязательны — выбираем государство поиском.
  CHECK(clickRevealed(h, "modifiers.fx.diplomacyPerTurn.on"));
  CHECK(h->world().modifier(mid)->has(Fx::DiplomacyPerTurn));
  CHECK(h->world().modifier(mid)->targets.empty());
  CHECK(enterValue(h, "modifiers.fx.diplomacyPerTurn.value", "40"));
  CHECK_NEAR(h->world().modifier(mid)->fx[size_t(Fx::DiplomacyPerTurn)], 25.0, 1e-9);
  Id target = 0;
  std::string targetName;
  h->world().factions.each([&](const Faction& f) {
    if (!target && f.isState()) {
      target = f.id;
      targetName = f.name;
    }
  });
  CHECK(pickInCombo(h, "modifiers.targets", targetName));
  CHECK_EQ(h->world().modifier(mid)->targets.size(), size_t(1));
  CHECK_EQ(h->world().modifier(mid)->targets[0], target);
  // Выключение эффекта убирает его.
  CHECK(clickRevealed(h, "modifiers.fx.contentmentPerTurn.on"));
  CHECK(!h->world().modifier(mid)->has(Fx::ContentmentPerTurn));
  // Использование в провинции через правила: появляется в «Где используется» и в расчёте.
  const World& w0 = h->world();
  Id pid = landProvince(w0);
  CHECK(pid != 0);
  CHECK(h->act("Модификатор провинции", [&](Tx& tx) { tx.province(pid).modifiers.push_back(mid); }));
  quick(h);
  CHECK(rules::provinceEffects(h->world(), pid)[Fx::PopGrowthPct] >= 50 - 1e-9);
  // Добавить государству через поле «Добавить государству».
  std::string stateName;
  Id stateId = 0;
  h->world().factions.each([&](const Faction& f) {
    if (f.isState() && f.id != target && !stateId) {
      stateId = f.id;
      stateName = f.name;
    }
  });
  CHECK(pickInCombo(h, "modifiers.addFaction", stateName));
  {
    const Faction* f = h->world().faction(stateId);
    CHECK(f && std::find(f->modifiers.begin(), f->modifiers.end(), mid) != f->modifiers.end());
  }
  CHECK(reveal(h, "modifiers.usage"));
  h.dropToasts();
  quick(h);
  h.settle();
  CHECK(h.shot("modifiers_usage"));
  // Отмена возвращает список государства.
  h.key(Key::Z, ctrl());
  quick(h);
  {
    const Faction* f = h->world().faction(stateId);
    CHECK(f && std::find(f->modifiers.begin(), f->modifiers.end(), mid) == f->modifiers.end());
  }
  // Копия: Ctrl+D — новый модификатор с теми же эффектами.
  h.key(Key::D, ctrl());
  quick(h);
  Id copy = h->ui.editorArg;
  CHECK(copy != mid);
  const Modifier* cm = h->world().modifier(copy);
  CHECK(cm != nullptr);
  CHECK(cm && cm->has(Fx::PopGrowthPct) && cm->targets.size() == 1);
  // Вернуться к исходному и удалить с подтверждением: ссылки провинции очищаются.
  h->ui.editorArg = mid;
  quick(h);
  CHECK(clickRevealed(h, "modifiers.delete"));
  CHECK(h->hasDialog("confirm"));
  quick(h);
  h.settle();
  CHECK(h.shot("modifiers_delete_confirm"));
  CHECK(confirmDialog(h));
  CHECK(h->world().modifier(mid) == nullptr);
  {
    const Province* p = h->world().province(pid);
    CHECK(p && std::find(p->modifiers.begin(), p->modifiers.end(), mid) == p->modifiers.end());
  }
  // Ctrl+Z — модификатор и ссылка вернулись.
  h.key(Key::Z, ctrl());
  quick(h);
  CHECK(h->world().modifier(mid) != nullptr);
  {
    const Province* p = h->world().province(pid);
    CHECK(p && std::find(p->modifiers.begin(), p->modifiers.end(), mid) != p->modifiers.end());
  }
}

TEST(app_catalogs_modifier_list_and_readonly) {
  Harness h("catalogs_modlist");
  h.demo();
  h.dropToasts();
  // Фишка модификатора открывает окно на нём (аргумент — модификатор).
  const Modifier* m = modifierNamed(h->world(), "Торговый тракт");
  CHECK(m != nullptr);
  Id mid = m ? m->id : 0;
  h->openEditor("modifiers", mid);
  quick(h);
  CHECK_EQ(h->ui.editorArg, mid);
  CHECK(h->uiRect("modifiers.selected") != nullptr);
  // ↑ — предыдущий по алфавиту, ↓ — обратно.
  const Modifier* drill = modifierNamed(h->world(), "Строевая муштра");
  CHECK(drill != nullptr);
  h.key(Key::Up);
  CHECK_EQ(h->ui.editorArg, drill ? drill->id : 0);
  h.key(Key::Down);
  CHECK_EQ(h->ui.editorArg, mid);
  // Поиск и фильтр: Ctrl+F, «шахт».
  h.key(Key::F, ctrl());
  h.type("шахт");
  quick(h);
  const Modifier* mines = modifierNamed(h->world(), "Глубокие шахты");
  CHECK(mines != nullptr);
  CHECK_EQ(h->ui.editorArg, mines ? mines->id : 0);
  // Esc из поиска, затем ещё Esc — назад к карте.
  h.key(Key::Escape);
  h.key(Key::Escape);
  quick(h);
  CHECK(h->ui.editor.empty());
  // Просмотр прошлого хода — правка недоступна.
  CHECK(h->endTurnNow());
  quick(h);
  CHECK(h->viewTurn(1));
  h->openEditor("modifiers", mid);
  quick(h);
  std::string name = h->world().modifier(mid)->name;
  CHECK(h.clickUi("modifiers.name"));
  h.type("Испорчено");
  h.key(Key::Enter);
  quick(h);
  CHECK_EQ(h->world().modifier(mid)->name, name);
  CHECK_EQ(h->store.world().modifier(mid)->name, name);
  h->backToCurrent();
  quick(h);
}

TEST(app_catalogs_modifier_light) {
  Harness h("catalogs_light", 1440, 900, 1, false);
  h->setTheme(false);
  h.demo();
  h.dropToasts();
  const Modifier* m = modifierNamed(h->world(), "Мятежные настроения");
  CHECK(m != nullptr);
  h->openEditor("modifiers", m ? m->id : 0);
  quick(h);
  h.settle();
  CHECK(h.shot("modifiers_light"));
}

// ================================================================ справочники
TEST(app_catalogs_lists_add_remove) {
  Harness h("catalogs_lists");
  h.demo();
  h.dropToasts();
  // Лента → «Справочники» → «Расы».
  CHECK(h.clickUi("drawer.catalogs"));
  quick(h);
  h.settle();
  CHECK(h.shot("catalogs_drawer"));
  CHECK(h.clickUi("drawer.catalogs.1"));
  quick(h);
  CHECK_EQ(h->ui.editor, std::string("catalogs"));
  size_t races = h->world().catalogs->races.size();
  CHECK(h.clickUi("catalogs.add"));
  quick(h);
  CHECK_EQ(h->world().catalogs->races.size(), races + 1);
  Id rid = h->world().catalogs->races.back().id;
  h.retype("Драконорождённые");
  h.key(Key::Enter);
  quick(h);
  CHECK_EQ(h->world().catalogs->races.back().name, std::string("Драконорождённые"));
  // Дубликат названия — отказ, название прежнее.
  std::string first = h->world().catalogs->races.front().name;
  CHECK(h.clickUi("catalogs.selected.name"));
  h.retype(first);
  h.key(Key::Enter);
  quick(h);
  CHECK_EQ(h->world().catalogs->races.back().name, std::string("Драконорождённые"));
  // Раса живёт в провинции — карточка показывает использование.
  Id pid = landProvince(h->world());
  CHECK(h->act("Население", [&](Tx& tx) { tx.province(pid).races.push_back(RacePop{rid, 1500}); }));
  h.dropToasts();
  quick(h);
  CHECK(h->uiRect("catalogs.usage") != nullptr);
  h.settle();
  CHECK(h.shot("catalogs_races"));
  // Удаление с подтверждением чистит провинцию; Ctrl+Z возвращает.
  CHECK(h.clickUi("catalogs.delete"));
  CHECK(h->hasDialog("confirm"));
  CHECK(confirmDialog(h));
  CHECK(Catalogs::find(h->world().catalogs->races, rid) == nullptr);
  {
    const Province* p = h->world().province(pid);
    bool has = false;
    for (auto& r : p->races) has = has || r.race == rid;
    CHECK(!has);
  }
  h.key(Key::Z, ctrl());
  quick(h);
  CHECK(Catalogs::find(h->world().catalogs->races, rid) != nullptr);
  // «Золото» закреплено: Delete не удаляет.
  h->openEditor("catalogs", 1);   // ресурсы
  quick(h);
  CHECK(h->world().resource(kGold) != nullptr);
  h.key(Key::Delete);
  quick(h);
  CHECK(!h->hasDialog("confirm"));
  CHECK(h->world().resource(kGold) != nullptr);
  // Должности: переименование меняет и места в совете.
  h->openEditor("catalogs", 6);
  quick(h);
  std::string pos = h->world().catalogs->positions.front().name;
  int seats = 0;
  h->world().factions.each([&](const Faction& f) {
    for (auto& s : f.council) seats += s.position == pos ? 1 : 0;
  });
  CHECK(seats > 0);
  CHECK(h.clickUi("catalogs.selected.name"));
  h.retype("Верховный " + utf8::lower(pos));
  h.key(Key::Enter);
  quick(h);
  std::string renamed = h->world().catalogs->positions.front().name;
  CHECK_EQ(renamed, "Верховный " + utf8::lower(pos));
  int seats2 = 0;
  h->world().factions.each([&](const Faction& f) {
    for (auto& s : f.council) seats2 += s.position == renamed ? 1 : 0;
  });
  CHECK_EQ(seats2, seats);
  h.dropToasts();
  quick(h);
  h.settle();
  CHECK(h.shot("catalogs_positions"));
}

// ================================================================ персонажи
TEST(app_catalogs_character_create_edit) {
  Harness h("catalogs_chars");
  h.demo();
  h.dropToasts();
  CHECK(h.clickUi("drawer.characters"));
  quick(h);
  u32 before = h->world().characters.size();
  CHECK(h.clickUi("characters.new"));
  quick(h);
  CHECK_EQ(h->world().characters.size(), before + 1);
  CHECK(h->ui.sel.type == app::SelType::Character);
  Id cid = h->ui.sel.id;
  // Имя, титул.
  CHECK(enterValue(h, "character.name", "Леди Морвен", "inspector"));
  CHECK_EQ(h->world().character(cid)->name, std::string("Леди Морвен"));
  CHECK(enterValue(h, "character.title", "Хранительница печати", "inspector"));
  CHECK_EQ(h->world().character(cid)->title, std::string("Хранительница печати"));
  // Фракция — выбор поиском.
  Id fac = 0;
  std::string facName;
  h->world().factions.each([&](const Faction& f) {
    if (!fac && f.isState()) {
      fac = f.id;
      facName = f.name;
    }
  });
  CHECK(pickInCombo(h, "character.faction", facName, "inspector"));
  CHECK_EQ(h->world().character(cid)->faction, fac);
  // Герой и содержание (расход на специалистов).
  CHECK(clickRevealed(h, "character.hero", "inspector"));
  CHECK(h->world().character(cid)->hero);
  double spec0 = rules::calc(h->world())->faction(fac)->expSpecialists;
  CHECK(enterValue(h, "character.upkeep", "40", "inspector"));
  CHECK_NEAR(h->world().character(cid)->upkeep, 40.0, 1e-9);
  CHECK_NEAR(rules::calc(h->world())->faction(fac)->expSpecialists, spec0 + 40, 1e-6);
  // Портрет из файла PNG (системный диалог headless возвращает путь).
  std::string png = writeTestPortrait(h.root);
  hl::queueDialogResult(png);
  CHECK(clickRevealed(h, "character.loadPortrait", "inspector"));
  quick(h);
  CHECK(!h->world().character(cid)->portrait.empty());
  CHECK(codec::decodePng(h->world().character(cid)->portrait).has_value());
  // Повреждённый файл — отказ, портрет прежний.
  std::string bad = fs::join(h.root, "bad.png");
  fs::writeFileAtomic(bad, std::string_view("not a picture"));
  std::string keep = h->world().character(cid)->portrait;
  hl::queueDialogResult(bad);
  CHECK(clickRevealed(h, "character.loadPortrait", "inspector"));
  quick(h);
  CHECK(h->world().character(cid)->portrait == keep);
  h.dropToasts();
  quick(h);
  CHECK(reveal(h, "character.portrait", "inspector", 120));
  quick(h);
  h.settle();
  CHECK(h.shot("character_portrait"));
  // Роли: лорд провинции, место в совете.
  h->ui.tabOf[app::SelType::Character] = "character.roles";
  quick(h);
  Id pid = landProvince(h->world(), fac);
  CHECK(pid != 0);
  CHECK(pickInCombo(h, "character.addLord", h->world().province(pid)->name, "inspector"));
  CHECK_EQ(h->world().province(pid)->lord, cid);
  CHECK(clickRevealed(h, "character.addSeat", "inspector"));
  h.key(Key::Enter);
  quick(h);
  bool seated = false;
  for (auto& s : h->world().faction(fac)->council) seated = seated || s.character == cid;
  CHECK(seated);
  h.dropToasts();
  quick(h);
  h.move(700, 300);
  quick(h);
  h.settle();
  CHECK(h.shot("character_roles_assigned"));
  // Удаление персонажа: подтверждение, ссылки очищены; Ctrl+Z возвращает всё.
  h->ui.tabOf[app::SelType::Character] = "character.info";
  quick(h);
  CHECK(clickRevealed(h, "character.delete", "inspector"));
  CHECK(confirmDialog(h));
  CHECK(h->world().character(cid) == nullptr);
  CHECK_EQ(h->world().province(pid)->lord, Id(0));
  for (auto& s : h->world().faction(fac)->council) CHECK(s.character != cid);
  CHECK(!h->ui.sel);
  h.key(Key::Z, ctrl());
  quick(h);
  CHECK(h->world().character(cid) != nullptr);
  CHECK_EQ(h->world().province(pid)->lord, cid);
}

TEST(app_catalogs_character_list_filter) {
  Harness h("catalogs_charlist");
  h.demo();
  h.dropToasts();
  h.key(Key::D4, ctrl());   // Ctrl+4 — панель «Персонажи»
  quick(h);
  CHECK_EQ(h->ui.drawer, std::string("characters"));
  // Только герои.
  CHECK(h.clickUi("characters.heroes"));
  quick(h);
  // Первая строка — герой; щелчок открывает инспектор.
  CHECK(h.clickUi("characters.first"));
  quick(h);
  CHECK(h->ui.sel.type == app::SelType::Character);
  const Character* c = h->world().character(h->ui.sel.id);
  CHECK(c && c->hero);
  // Фильтр по фракции.
  Id fac = c ? c->faction : 0;
  CHECK(pickInCombo(h, "characters.faction", h->world().factionName(fac), "drawer"));
  quick(h);
  h.waitMap();
  h.dropToasts();
  quick(h);
  h.settle();
  CHECK(h.shot("characters_filtered"));
  // Удаление из контекстного меню строки.
  Id del = h->ui.sel.id;
  const RectF* r = h->uiRect("characters.selected");
  CHECK(r != nullptr);
  h.click(r->cx(), r->cy(), platform::MouseRight);
  quick(h);
  h.settle();
  CHECK(h.shot("characters_menu"));
  // Последний пункт меню — «Удалить» (↑ из начала — к последнему), затем подтверждение.
  h.key(Key::Up);
  h.key(Key::Enter);
  quick(h);
  CHECK(h->hasDialog("confirm"));
  CHECK(confirmDialog(h));
  CHECK(h->world().character(del) == nullptr);
  CHECK(!h->ui.sel);
}

// ================================================================ снимки окон (тёмная тема, 1440 × 900)
TEST(app_catalogs_shots) {
  Harness h("catalogs_shots");
  h.demo();
  h.waitMap();
  h.dropToasts();
  // Панель «Модификаторы» на ленте.
  CHECK(h.clickUi("drawer.modifiers"));
  h.move(700, 450);
  quick(h);
  h.settle();
  CHECK(h.shot("modifiers_drawer"));
  // Окно модификаторов: модификатор с локальными и глобальными эффектами и целями дипломатии.
  Id mid = 0;
  CHECK(h->act("Модификатор для снимка", [&](Tx& tx) {
    mid = rules::createModifier(tx, "Посольство при дворе");
    Modifier& m = tx.modifier(mid);
    m.icon = "diplomacy";
    m.color = Color::hex(0x4a9fe0);
    m.desc = "Постоянное посольство: дипломаты сглаживают споры, купцы получают охрану.";
    m.fxMask = (1u << int(Fx::DiplomacyPerTurn)) | (1u << int(Fx::TradePct)) | (1u << int(Fx::IncomePct));
    m.fx[size_t(Fx::DiplomacyPerTurn)] = 3;
    m.fx[size_t(Fx::TradePct)] = 15;
    m.fx[size_t(Fx::IncomePct)] = -4;
    tx.w().factions.each([&](const Faction& f) {
      if (f.isState() && m.targets.size() < 2) m.targets.push_back(f.id);
    });
    Id p = 0;
    tx.w().provinces.each([&](const Province& pr) {
      if (!p && !pr.sea && pr.owner) p = pr.id;
    });
    tx.province(p).modifiers.push_back(mid);
    tx.faction(m.targets.back()).modifiers.push_back(mid);
  }));
  h->openEditor("modifiers", mid);
  h.move(700, 880);
  quick(h);
  h.settle();
  CHECK(h.shot("modifiers"));
  // Справочники: ресурсы.
  h->openEditor("catalogs", 1);
  quick(h);
  h.settle();
  CHECK(h.shot("catalogs"));
  // Персонажи: правитель с ролями.
  h->closeEditor();
  quick(h);
  h.dropToasts();
  CHECK(h.clickUi("drawer.characters"));
  Id ruler = 0;
  h->world().factions.each([&](const Faction& f) {
    if (!ruler && f.isState() && f.ruler) ruler = f.ruler;
  });
  h->select(app::SelType::Character, ruler);
  h.move(700, 300);
  h.waitMap();
  h.dropToasts();
  quick(h);
  h.settle();
  CHECK(h.shot("characters"));
  h->ui.tabOf[app::SelType::Character] = "character.roles";
  quick(h);
  h.settle();
  CHECK(h.shot("character_roles"));
}

// Карточка кампании: поиск по имени рядом с папкой мира и открытие файла карточки.
TEST(app_catalogs_character_canon_card) {
  Harness h("catalogs_canon");
  h.demo();
  CHECK(h->saveTo(fs::join(h.root, "world")));
  std::string cards = fs::join(h.root, "03_Персонажи");
  fs::makeDirs(cards);
  fs::writeFileAtomic(fs::join(cards, "Леди_Морвен.md"),
                      std::string_view("# Леди Морвен\n\n---\ntype: character\nid: CHAR-9001\naliases: [\"Морвен\"]\n---\n"));
  Id cid = 0;
  CHECK(h->act("Персонаж", [&](Tx& tx) { cid = rules::createCharacter(tx, 0, "Морвен"); }));
  h->select(app::SelType::Character, cid);
  h.dropToasts();
  quick(h);
  // Поиск по имени находит карточку по псевдониму и записывает её ID.
  CHECK(clickRevealed(h, "character.findCard", "inspector"));
  CHECK_EQ(h->world().character(cid)->entity, std::string("CHAR-9001"));
  // Открыть карточку — файл передаётся системе.
  size_t n = hl::opened().size();
  CHECK(clickRevealed(h, "character.openCard", "inspector"));
  CHECK_EQ(hl::opened().size(), n + 1);
  CHECK(!hl::opened().empty() && hl::opened().back().find("Леди_Морвен.md") != std::string::npos);
}
