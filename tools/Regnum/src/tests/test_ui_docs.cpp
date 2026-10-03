// Regnum — примеры из docs/UI.md: должны компилироваться и работать как написано.
#include "core/world.h"
#include "tests/test_ui_util.h"

using namespace rg;
using namespace rg::uitest;

namespace {

struct Race {
  std::string name;
  double pop, share;
};

}  // namespace

TEST(ui_docs_examples_build_and_run) {
  H h(1280, 900);
  // Данные примеров
  double tax = 12, turns = 3, gold = 1250, rebellion = -4, opacity = 60;
  int view = 0, mode = 1, size = 0, ownerIdx = 1, t = -1, provIdx = 0, tab = 0, t2 = 0, selRace = -1, sel = -1, menuRow = -1;
  bool editBorders = true, sea = false, showLabels = true, askOpen = false;
  ui::Check all = ui::Check::Mixed;
  std::string name = "Эльвенмор", query, notes;
  std::vector<ui::Option> owners = {{"Арден", nullptr, Color::hex(0xa4262c)}, {"Валь", nullptr, Color::hex(0x7a1f2b)}};
  std::vector<int> resources = {0};
  std::vector<ui::Option> options = {{"Зерно", "grain"}, {"Камень", "stone"}};
  std::vector<int> ids(3000);
  for (size_t i = 0; i < ids.size(); i++) ids[i] = int(i);
  Color color = Color::hex(0x1f4e9c), color2 = Color(30, 60, 90, 200);
  std::vector<Race> races = {{"Люди", 69336, 0.54}, {"Эльфы", 26964, 0.21}};
  std::vector<ui::Slice> slices = {{38, Color::hex(0xd9a441), "Лига"}, {62, Color::hex(0x6d8fd8), "Прочие"}};
  std::vector<float> history = {1, 3, 2, 5, 4};
  rg::Flag flag;
  flag.pattern = rg::FlagPattern::H3;
  flag.emblem = "crown";
  int created = 0, deleted = 0, picked = 0;
  float splitPos = 300;
  h.build = [&] {
    ui::Split sp = ui::splitter("split", {0, 0, 1280, 900}, splitPos);
    {
      ui::Panel p("inspector", {sp.a.x + 8, 8, sp.a.w - 16, 884});
      ui::label("Эльвенмор", {.font = ui::Font::Display});
      {
        ui::Row r({ui::px(90), ui::fr(1), ui::fr(2)}, 30, 8);
        ui::label("Налог");
        ui::numberField("tax", tax);
        ui::slider("tax2", tax, 0, 60);
      }
      {
        ui::HStack hs(30, ui::Align::Left, 6);
        ui::iconButton("undo", "Отменить");
        ui::flex();
        ui::button("Применить", {.variant = ui::Variant::Primary});
      }
      ui::Scroll sc("body");
      ui::label("Население", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "population"});
      ui::text("Абзац переносится по словам.", ui::Font::Body, ui::Ink::Dim);
      ui::caption("Расы");
      if (ui::link("Открыть хронику", "chronicle")) picked++;
      ui::kbd("Ctrl+Shift+S");
      ui::kbd({Key::S, ui::ModPrimary});
      ui::separator();
      ui::icon("warning", ui::Ink::Warning, 18, "Есть ошибки");
      if (ui::button("Применить##2", {.variant = ui::Variant::Primary, .icon = "check"})) picked++;
      ui::button("Удалить", {.variant = ui::Variant::Danger, .icon = "trash"});
      ui::button("Ещё", {.size = ui::Size::Small, .iconRight = "chevron-down"});
      ui::button("Сохранить", {.fill = true, .shortcut = {Key::S, ui::ModPrimary}});
      ui::iconButton("tool-knife", "Нож", {.toggled = view == 1, .shortcut = {Key::K, 0}});
      ui::iconButton("bell", "Уведомления", {.badge = true});
      ui::iconToggle("eye", "Показать подписи", showLabels);
      ui::toggle("Правка границ", editBorders);
      ui::checkbox("Морская", sea);
      ui::checkbox("Все", all);
      ui::radio("Малая", size, 0);
      ui::radio("Средняя", size, 1);
      ui::segmented("view", view, {{"map", "Карта"}, {"table", "Таблица"}});
      ui::segmented("mode", mode, {{"mode-political", {}, "Политическая"}, {"mode-guilds", {}, "Гильдии"}}, {.fill = false});
      ui::slider("opacity", opacity, 0, 100, {.step = 5, .unit = "%"});
      ui::numberField("tax3", tax, {.min = 0, .max = 60, .unit = "%", .icon = "percent"});
      ui::numberField("turns", turns, {.min = 1, .max = 99, .unit = "ход|хода|ходов", .steppers = true});
      ui::numberField("gold", gold, {.digits = 1, .label = "Казна"});
      ui::numberField("reb", rebellion, {.sign = true, .readOnly = true});
      ui::textField("name", name, {.placeholder = "Название", .icon = "edit", .clearButton = true, .maxLength = 40});
      ui::searchField("q", query);
      ui::textArea("notes", notes, 120, {.placeholder = "Заметки"});
      ui::combo("owner", ownerIdx, owners, {.noneLabel = "Нет владельца"});
      ui::combo("terrain", t, {{"Равнина", "land"}, {"Горы", "mountain"}});
      ui::combo("prov", provIdx, int(ids.size()), [&](int i) { return ui::Option{"Провинция"}; });
      ui::multiSelect("res", resources, options);
      ui::colorButton("color", color);
      ui::colorPicker("inline", color2, {.alpha = true});
      ui::tabs("ptabs", tab, {{"info", {}, "Обзор"}, {"coins", {}, "Экономика", 3}, {"guild", {}, "Гильдии", -1, ui::Tone::Danger}}, {.fill = true});
      ui::tabs("t2", t2, {{"info", "Обзор"}, {"army", "Войска"}}, {.style = ui::TabStyle::Pill});
      ui::stat("128 400", "Население", {.icon = "population", .delta = 2.1, .deltaText = "+2,1 %"});
      ui::stat("18 %", "Восстание", {.icon = "rebellion", .tone = ui::Tone::Danger, .delta = 3, .invertDelta = true});
      ui::progress(0.4, {.color = Color::hex(0xc9a36a), .label = true});
      ui::meter(-30);
      ui::pie(slices, {.size = 108, .thickness = 15, .centerValue = "38 %", .centerLabel = "лидер"});
      ui::sparkline(history, {.height = 32});
      ui::badge("12");
      ui::badge("Новое", ui::Tone::Success);
      ui::tag("Война", ui::Tone::Danger, "war");
      if (ui::chip("Арден", {.color = Color::hex(0xa4262c), .clickable = true}) == ui::ChipAction::Click) picked++;
      ui::avatar("Эдрик Третий", {.size = 36, .ring = true});
      ui::flag(flag, 66, 44);
      if (ui::emptyState("army", "Войск пока нет.", "Новое войско", "plus")) created++;
      // Таблица
      ui::Column cols[] = {{"Раса", nullptr, ui::fr(1.3f)}, {"Жители", "population", ui::fr(1), ui::Align::Right, true}, {"Доля", nullptr, ui::fr(1.3f)}};
      {
        ui::Table tb("races", cols, int(races.size()), {.selected = &selRace});
        tb.sort([&](int a, int b, int col) { return col == 1 ? (races[size_t(a)].pop < races[size_t(b)].pop ? -1 : 1) : compareRu(races[size_t(a)].name, races[size_t(b)].name); });
        for (int i : tb) {
          tb.text(races[size_t(i)].name);
          tb.text(fmtNum(races[size_t(i)].pop));
          tb.cell();
          ui::progress(races[size_t(i)].share, {.label = true});
        }
        if (tb.footer()) {
          tb.text("Итого");
          tb.text(fmtNum(96300));
        }
        if (int r = tb.rightClicked(); r >= 0) {
          menuRow = r;
          ui::openContextMenu("row");
        }
      }
      if (ui::beginMenu("row")) {
        ui::menuItem("Открыть");
        ui::endMenu();
      }
      {
        ui::TreeNode n("Королевство Арден", {.icon = "crown", .selected = sel == 1, .badge = "12"});
        if (n.clicked()) sel = 1;
        if (n) {
          ui::TreeNode c("Эльвенмор", {.dot = Color::hex(0xa4262c), .leaf = true});
        }
      }
      ui::iconButton("save", "Сохранить##tip", {.shortcut = {Key::S, ui::ModPrimary}});
      ui::tooltip("Своя подсказка", {Key::F2, 0});
      if (ui::beginTooltip(280)) {
        ui::label("Богатая");
        ui::progress(0.4);
        ui::endTooltip();
      }
      if (ui::button("Ещё##menu", {.iconRight = "chevron-down"})) ui::openPopup("more");
      if (ui::beginMenu("more")) {
        ui::menuHeader("Провинция");
        if (ui::menuItem("Переименовать", {.icon = "edit", .shortcut = {Key::F2, 0}})) picked++;
        if (ui::beginSubmenu("Владелец", "crown")) {
          ui::menuItem("Арден", {.checked = true});
          ui::endSubmenu();
        }
        ui::menuSeparator();
        if (ui::menuItem("Удалить", {.icon = "trash", .danger = true})) askOpen = true;
        ui::endMenu();
      }
      ui::label("Эльвенмор##ctx");
      if (ui::beginContextMenu("ctx")) {
        ui::menuItem("Копировать");
        ui::endMenu();
      }
      if (ui::beginPopup("filter", {.side = ui::Side::Below, .width = 260})) {
        ui::label("Фильтр");
        ui::endPopup();
      }
      ui::chip("Пехота", {.icon = "army", .clickable = true});
      ui::dragSource("unit", 7, "Пехота", "army");
      ui::button("Сюда");
      if (auto uid = ui::dropTarget("unit")) picked += int(*uid);
      RectF r = ui::next(0, 120);
      ui::custom(r, [](gfx::Canvas& c, RectF dev, float scale) { c.fillRect(dev, Color(20, 30, 40)); });
      ui::Interaction it = ui::interact(ui::id("tree"), r, ui::IfFocusable);
      if (it.dragging) picked++;
      ui::draw::rect(r.inset(8), ui::theme().surface3, 8);
      ui::draw::icon("star", RectF{r.x + 10, r.y + 10, 24, 24}, ui::toneColor(ui::Tone::Accent));
      if (ui::Section sec("Провинции", "province", {.badge = "3", .actionIcon = "plus", .actionTooltip = "Новая"}); sec) {
        if (sec.action()) picked++;
        for (int pid = 1; pid <= 3; pid++) {
          ui::IdScope s{i64(pid)};
          if (ui::listItem("Провинция", {.dot = Color::palette(pid), .subtitle = std::string("Регион ") + std::to_string(pid), .hint = "12 400", .selected = sel == pid})) sel = pid;
          ui::dragSource("province", u64(pid), "Провинция", "province");
          if (ui::beginContextMenu("ctx")) {
            ui::menuItem("Открыть");
            ui::endMenu();
          }
        }
      }
      ui::spinner(16);
    }
    if (ui::beginModal("delete", {.title = "Удалить провинцию?", .icon = "trash", .tone = ui::Tone::Danger}, &askOpen)) {
      ui::text("Земли отойдут соседям. Действие можно отменить Ctrl+Z.", ui::Font::Body, ui::Ink::Dim);
      {
        ui::ModalFooter f;
        if (ui::button("Отмена")) ui::closeModal();
        if (ui::button("Удалить", {.variant = ui::Variant::Danger, .isDefault = true})) {
          deleted++;
          ui::closeModal();
        }
      }
      ui::endModal();
    }
    {
      ui::Area a(sp.b, 16);
      ui::Card c({.title = "Гарнизон"});
      ui::label("Пусто");
    }
  };
  h.settle();
  ui::toast("Мир сохранён", ui::Tone::Success);
  ui::toast("Не удалось открыть архив", ui::Tone::Danger, "error", 8);
  askOpen = true;
  h.frames(20);
  CHECK(ui::anyModalOpen());
  h.key(Key::Enter);
  h.frames(3);
  CHECK_EQ(deleted, 1);
  CHECK(!askOpen);
  h.settle();
  CHECK(h.save("docs_examples"));
  CHECK_EQ(created, 0);
  (void)menuRow;
}
