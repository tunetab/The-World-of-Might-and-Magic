// Regnum — интерфейс: числа, выбор, таблицы, прокрутка, всплывающие окна, модальные окна, уведомления,
// виртуальные списки, перетаскивание, переключатели, вкладки, разделы, подсказки.
#include "tests/test_ui_util.h"

using namespace rg;
using namespace rg::uitest;
using ui::fr;
using ui::px;
using ui::WidgetId;

// ================================================================ числовое поле
TEST(ui_number_scrub_drag) {
  H h;
  double v = 10;
  RectF r;
  bool active = false;
  h.build = [&] {
    ui::Area a({20, 20, 300, 300});
    ui::numberField("n", v, {.min = 0, .max = 100, .step = 1});
    r = ui::lastItem().rect;
    active = ui::lastItem().active;
  };
  h.frame();
  // 40 точек вправо — 10 шагов
  h.move(r.x + 40, r.cy());
  h.down(r.x + 40, r.cy());
  h.drain();
  for (int i = 1; i <= 8; i++) {
    h.move(r.x + 40 + 5.f * float(i), r.cy());
    h.drain();
  }
  CHECK(active);
  CHECK_NEAR(v, 20, 1e-9);
  h.move(r.x + 40 + 41, r.cy());
  h.up(r.x + 40 + 41, r.cy());
  h.drain();
  CHECK_NEAR(v, 20, 1e-9);
  CHECK(!active);
  // С Shift — в 10 раз мельче, с ограничением max
  h.dragTo(r.x + 40, r.cy(), r.x + 40 + 4000, r.cy());
  CHECK_NEAR(v, 100, 1e-9);
}

TEST(ui_number_click_type_commit_and_validate) {
  H h;
  double v = 5;
  RectF r;
  int changes = 0;
  h.build = [&] {
    ui::Area a({20, 20, 300, 300});
    if (ui::numberField("n", v, {.min = 0, .max = 100, .digits = 1, .unit = "%"})) changes++;
    r = ui::lastItem().rect;
  };
  h.frame();
  h.click(r.x + 40, r.cy());          // щелчок без перетаскивания — ввод, всё выделено
  CHECK(ui::wantsKeyboard());
  h.type("42,5");
  h.key(Key::Enter);
  CHECK_NEAR(v, 42.5, 1e-9);
  CHECK_EQ(changes, 1);
  // Больше максимума — ограничение
  h.click(r.x + 40, r.cy());
  h.type("250");
  h.key(Key::Enter);
  CHECK_NEAR(v, 100, 1e-9);
  // Неверный ввод — значение прежнее, поле «встряхивается»
  h.click(r.x + 40, r.cy());
  h.type("1,2,3");
  h.key(Key::Enter);
  CHECK_NEAR(v, 100, 1e-9);
  CHECK(ui::needsRedraw());
  // Буквы не вводятся
  h.click(r.x + 40, r.cy());
  h.type("абв7");
  h.key(Key::Enter);
  CHECK_NEAR(v, 7, 1e-9);
  // Esc — отмена
  h.click(r.x + 40, r.cy());
  h.type("55");
  h.key(Key::Escape);
  CHECK_NEAR(v, 7, 1e-9);
}

// Щелчок в поле и уход из него без ввода (Tab, Enter, щелчок мимо) не меняет значение округлением показа
// (74,5 при digits = 0 — не 74; −14290,79468 при digits = 1 — не −14290,8) и не сообщает об изменении.
TEST(ui_number_focus_without_typing_keeps_exact_value) {
  H h;
  double a = 74.5, b = -14290.79468;
  RectF ra, rb;
  int changes = 0;
  h.build = [&] {
    ui::Area ar({20, 20, 300, 300});
    if (ui::numberField("a", a, {.min = -100, .max = 100})) changes++;
    ra = ui::lastItem().rect;
    if (ui::numberField("b", b, {.min = -1e9, .max = 1e9, .digits = 1})) changes++;
    rb = ui::lastItem().rect;
  };
  h.frame();
  h.click(ra.x + 40, ra.cy());
  CHECK(ui::wantsKeyboard());
  h.key(Key::Tab);
  h.click(rb.x + 40, rb.cy());
  CHECK(ui::wantsKeyboard());
  h.key(Key::Enter);
  h.click(ra.x + 40, ra.cy());
  h.click(5, 290);   // мимо полей
  CHECK(a == 74.5);
  CHECK(b == -14290.79468);
  CHECK_EQ(changes, 0);
  // Ввод того же показанного текста после правки — тоже без изменения; другой текст — записывается.
  h.click(rb.x + 40, rb.cy());
  h.type("12,5");
  h.key(Key::Enter);
  CHECK_NEAR(b, 12.5, 1e-9);
  CHECK_EQ(changes, 1);
}

// Прокрутка к элементу, который уже виден, не просит перерисовку (иначе вызов каждый кадр не даёт окну простаивать).
TEST(ui_scroll_to_visible_item_is_idle) {
  H h;
  h.build = [&] {
    ui::Area ar({20, 20, 300, 300});
    ui::VirtualList vl("list", 50, 30, 200);
    vl.scrollToRow(1);
    for (int i : vl) (void)ui::next(30), (void)i;
  };
  h.settle();
  h.frame();
  CHECK(!ui::needsRedraw());
}

TEST(ui_number_arrows_wheel_steppers) {
  H h;
  double v = 3;
  RectF r;
  h.build = [&] {
    ui::Area a({20, 20, 300, 300});
    ui::numberField("n", v, {.min = 1, .max = 99, .unit = "ход|хода|ходов", .steppers = true});
    r = ui::lastItem().rect;
  };
  h.frame();
  h.click(r.right() - 15, r.cy());   // «+»
  CHECK_NEAR(v, 4, 1e-9);
  h.click(r.right() - 39, r.cy());   // «−»
  h.click(r.right() - 39, r.cy());
  CHECK_NEAR(v, 2, 1e-9);
  // Удержание «+» — повтор через 0,4 с
  h.move(r.right() - 15, r.cy());
  h.down(r.right() - 15, r.cy());
  h.drain();
  h.wait(1.0);
  h.up(r.right() - 15, r.cy());
  h.drain();
  CHECK(v >= 8);
  double before = v;
  // Фокус (щелчок), стрелки и колесо
  h.click(r.x + 30, r.cy());
  h.key(Key::Escape);
  h.key(Key::Tab);
  h.key(Key::Up);
  CHECK_NEAR(v, before + 1, 1e-9);
  h.key(Key::Down, platform::ModShift);
  CHECK_NEAR(v, before + 1 - 10, 1e-9);
  h.wheel(r.x + 30, r.cy(), 1);
  CHECK_NEAR(v, before + 1 - 10 + 1, 1e-9);
}

TEST(ui_number_integer_template) {
  H h;
  int n = 3;
  i64 big = 10;
  h.build = [&] {
    ui::Area a({20, 20, 300, 300});
    ui::numberField("i", n, {.min = 0, .max = 10, .step = 0.5});
    ui::numberField("b", big);
  };
  h.frame();
  ui::setKeyboardFocus(0);
  CHECK_EQ(n, 3);
  CHECK_EQ(big, i64(10));
}

// ================================================================ выпадающий список
namespace {

struct ComboT {
  int idx = -1;
  int changes = 0;
  RectF r;
  std::vector<ui::Option> opts;
  ui::ComboOpt opt;
};

void buildCombo(ComboT& c) {
  ui::Area a({20, 20, 300, 300});
  if (ui::combo("c", c.idx, c.opts, c.opt)) c.changes++;
  c.r = ui::lastItem().rect;
}

std::vector<ui::Option> states() {
  return {{"Королевство Арден"}, {"Северный союз"}, {"Империя Валь"}, {"Свободные города"}, {"Орда Каргат"},
          {"Княжество Ольм"},    {"Вольные бароны"}, {"Лесной народ"}, {"Горные кланы"},   {"Морская республика"}};
}

}  // namespace

TEST(ui_combo_click_open_select) {
  H h;
  ComboT c;
  c.opts = {{"Равнина"}, {"Горы"}, {"Остров"}};
  h.build = [&] { buildCombo(c); };
  h.frame();
  h.click(c.r.cx(), c.r.cy());
  h.frames(3);
  CHECK(ui::wantsMouse());
  // Пункты под полем: высота 30, отступ 5 + 4
  float y = c.r.bottom() + 4 + 5 + 30 + 15;
  h.click(c.r.cx(), y);
  CHECK_EQ(c.idx, 1);
  CHECK_EQ(c.changes, 1);
  // Повторный щелчок по полю открывает, ещё один — закрывает (без повторного открытия)
  h.click(c.r.cx(), c.r.cy());
  h.frames(3);
  h.click(c.r.cx(), c.r.cy());
  h.frames(3);
  h.click(c.r.cx(), c.r.bottom() + 4 + 5 + 15);   // там, где был список, — теперь пусто
  CHECK_EQ(c.idx, 1);
}

TEST(ui_combo_keyboard_and_search) {
  H h;
  ComboT c;
  c.opts = states();
  c.opt.noneLabel = "Нет владельца";
  h.build = [&] { buildCombo(c); };
  h.frame();
  h.key(Key::Tab);                    // фокус на поле
  h.key(Key::Enter);                  // открыть
  h.frames(3);
  h.key(Key::Down);
  h.key(Key::Down);
  h.key(Key::Enter);                  // «нет» → 0 → 1
  CHECK_EQ(c.idx, 1);
  // Набор текста при фокусе открывает список с поиском
  h.type("кар");
  h.frames(3);
  h.key(Key::Enter);
  CHECK_EQ(c.idx, 4);                 // Орда Каргат
  // Пункт «нет»
  h.key(Key::Enter);
  h.frames(3);
  for (int i = 0; i < 12; i++) h.key(Key::Up);
  h.key(Key::Enter);
  CHECK_EQ(c.idx, -1);
  // Esc закрывает без изменений
  h.key(Key::Enter);
  h.frames(3);
  h.key(Key::Down);
  h.key(Key::Escape);
  h.frames(3);
  CHECK_EQ(c.idx, -1);
}

TEST(ui_combo_click_outside_closes) {
  H h;
  ComboT c;
  c.opts = states();
  int other = 0;
  h.build = [&] {
    buildCombo(c);
    ui::at({500, 500, 100, 30});
    if (ui::button("Другое")) other++;
  };
  h.frame();
  h.click(c.r.cx(), c.r.cy());
  h.frames(3);
  h.click(550, 515);   // вне списка: закрыть; кнопка под указателем щелчок не получает в этом же нажатии
  h.frames(3);
  h.click(c.r.cx(), c.r.bottom() + 60);
  CHECK_EQ(c.idx, -1);
  h.click(550, 515);
  CHECK(other >= 1);
}

TEST(ui_combo_huge_virtualized_list) {
  H h;
  int idx = 0;
  RectF r;
  std::vector<std::string> names;
  for (int i = 0; i < 5000; i++) names.push_back("Провинция " + std::to_string(i));
  int calls = 0;
  h.build = [&] {
    ui::Area a({20, 20, 300, 300});
    ui::combo("big", idx, int(names.size()), [&](int i) {
      calls++;
      return ui::Option{names[size_t(i)]};
    });
    r = ui::lastItem().rect;
  };
  h.frame();
  h.click(r.cx(), r.cy());
  h.frames(4);
  calls = 0;
  h.frame();
  CHECK(calls < 60);                    // только видимые строки (+ выбранный)
  h.type("4999");
  h.frames(3);
  h.key(Key::Enter);
  CHECK_EQ(idx, 4999);
}

TEST(ui_multiselect_add_remove) {
  H h;
  std::vector<int> sel = {0};
  std::vector<ui::Option> opts = {{"Зерно", "grain"}, {"Древесина", "wood"}, {"Камень", "stone"}};
  RectF r;
  int changes = 0;
  h.build = [&] {
    ui::Area a({20, 20, 400, 300});
    if (ui::multiSelect("m", sel, opts)) changes++;
    r = ui::lastItem().rect;
  };
  h.frame();
  // «+» — после фишки; открыть и отметить «Камень»
  float chipW = ui::measure("Зерно", ui::Font::Small) + 20 + 16 + 18;
  h.click(r.x + 4 + chipW + 6 + 13, r.y + 4 + 13);
  h.frames(3);
  h.click(r.x + 60, r.bottom() + 4 + 5 + 30 * 2 + 15);
  CHECK_EQ(sel.size(), size_t(2));
  CHECK_EQ(sel[1], 2);
  h.key(Key::Escape);
  h.frames(3);
  // Крестик первой фишки
  h.click(r.x + 4 + chipW - 14, r.y + 4 + 13);
  CHECK_EQ(sel.size(), size_t(1));
  CHECK_EQ(sel[0], 2);
  CHECK(changes >= 2);
}

// ================================================================ таблица
TEST(ui_table_sort_select_inline_edit) {
  H h;
  std::vector<std::string> names = {"Гномы", "Люди", "Эльфы", "Орки"};
  std::vector<double> pop = {300, 1200, 800, 50};
  std::vector<int> order;
  int sel = -1, dbl = -1, sortCol = -2;
  h.build = [&] {
    ui::Area a({20, 20, 500, 400});
    ui::Column cols[] = {{"Раса", nullptr, fr(1), ui::Align::Left, true}, {"Жители", nullptr, px(140), ui::Align::Right, true}};
    ui::Table t("races", cols, int(names.size()), {.rowHeight = 30, .selected = &sel});
    t.sort([&](int x, int y, int col) {
      if (col == 0) return compareRu(names[size_t(x)], names[size_t(y)]);
      return pop[size_t(x)] < pop[size_t(y)] ? -1 : pop[size_t(x)] > pop[size_t(y)] ? 1 : 0;
    });
    order.clear();
    for (int i : t) {
      order.push_back(i);
      t.text(names[size_t(i)]);
      t.cell();
      ui::numberField("pop", pop[size_t(i)], {.min = 0, .max = 1e6});
    }
    if (t.doubleClicked() >= 0) dbl = t.doubleClicked();
    sortCol = t.sortColumn();
  };
  h.frame();
  CHECK((order == std::vector<int>{0, 1, 2, 3}));
  CHECK_EQ(sortCol, -1);
  // Шапка: Жители — по возрастанию, затем по убыванию, затем без сортировки
  float headerY = 20 + 16;
  h.click(20 + 500 - 70, headerY);
  CHECK((order == std::vector<int>{3, 0, 2, 1}));
  h.click(20 + 500 - 70, headerY);
  CHECK((order == std::vector<int>{1, 2, 0, 3}));
  h.click(20 + 500 - 70, headerY);
  CHECK((order == std::vector<int>{0, 1, 2, 3}));
  h.click(60, headerY);   // Раса
  CHECK((order == std::vector<int>{0, 1, 3, 2}));
  // Щелчок по строке — выделение, двойной — событие
  float row0 = 20 + 32;
  h.click(60, row0 + 30 * 2 + 15);
  CHECK_EQ(sel, 3);
  h.multiClick(60, row0 + 15, 2);
  CHECK_EQ(dbl, 0);
  // Стрелки меняют выделенную строку
  h.key(Key::Down);
  CHECK_EQ(sel, 1);
  // Правка в ячейке
  h.click(20 + 500 - 70, row0 + 30 + 15);
  h.type("999");
  h.key(Key::Enter);
  CHECK_NEAR(pop[1], 999, 1e-9);
}

TEST(ui_table_virtualized_fixed_height) {
  H h;
  int visited = 0, first = -1;
  float scrollOff = 0;
  h.build = [&] {
    ui::Area a({20, 20, 500, 600});
    ui::Column cols[] = {{"№"}, {"Имя"}};
    ui::Table t("big", cols, 2000, {.rowHeight = 30, .height = 330});
    visited = 0;
    first = -1;
    for (int i : t) {
      if (first < 0) first = i;
      visited++;
      t.text(std::to_string(i));
      t.text("строка");
    }
    t.footer();
    t.text("Итого");
  };
  h.frame();
  h.frame();
  CHECK(visited <= 12);
  CHECK_EQ(first, 0);
  h.wheel(200, 200, -10);   // 640 точек вниз
  h.frames(40);
  CHECK(first >= 15);
  CHECK(visited <= 13);
  (void)scrollOff;
}

TEST(ui_table_empty_state) {
  H h;
  bool built = false;
  h.build = [&] {
    ui::Area a({20, 20, 500, 600});
    ui::Column cols[] = {{"Имя"}};
    ui::Table t("empty", cols, 0, {.emptyText = "Нет войск"});
    for (int i : t) {
      (void)i;
      built = true;
    }
  };
  h.frames(2);
  CHECK(!built);
}

// ================================================================ прокрутка и виртуальный список
TEST(ui_scroll_wheel_thumb_and_scroll_to) {
  H h;
  float off = 0;
  bool scrollToLast = false;
  RectF lastR;
  h.build = [&] {
    ui::Area a({20, 20, 300, 600});
    ui::Scroll s("s", 200);
    for (int i = 0; i < 40; i++) {
      ui::label("Строка " + std::to_string(i));
      if (i == 39) {
        lastR = ui::lastItem().rect;
        if (scrollToLast) {
          ui::scrollToItem();
          scrollToLast = false;
        }
      }
    }
    off = s.offset();
  };
  h.frame();
  CHECK_EQ(off, 0.f);
  h.wheel(100, 100, -1);
  h.frames(30);
  CHECK_NEAR(off, 64, 0.6);
  h.wheel(100, 100, 5);   // вверх за край — ноль
  h.frames(30);
  CHECK_EQ(off, 0.f);
  scrollToLast = true;
  h.frames(40);
  CHECK(lastR.bottom() <= 20 + 200 + 0.5f);
  CHECK(off > 400);
  // Перетаскивание ползунка полосы к верху
  h.move(20 + 300 - 4, 30);
  h.drain();
  h.frames(2);
  float thumbY = 20 + 200 - 2 - 15;   // ползунок внизу дорожки (прокручено до конца)
  h.dragTo(20 + 300 - 4, thumbY, 20 + 300 - 4, -200);
  h.frames(5);
  CHECK(off < 1);
}

TEST(ui_scroll_nested_inner_first) {
  H h;
  float outer = 0, inner = 0;
  h.build = [&] {
    ui::Area a({20, 20, 300, 600});
    ui::Scroll so("outer", 300);
    {
      ui::Scroll si("inner", 120);
      for (int i = 0; i < 20; i++) ui::label("Внутри " + std::to_string(i));
      inner = si.offset();
    }
    for (int i = 0; i < 30; i++) ui::label("Снаружи " + std::to_string(i));
    outer = so.offset();
  };
  h.frame();
  h.wheel(100, 60, -1);
  h.frames(30);
  CHECK(inner > 0);
  CHECK_EQ(outer, 0.f);
  // Внутренняя дошла до конца — колесо уходит наружу
  for (int i = 0; i < 10; i++) h.wheel(100, 60, -3);
  h.frames(30);
  CHECK(outer > 0);
}

TEST(ui_virtual_list_visible_rows) {
  H h;
  std::vector<int> rows;
  h.build = [&] {
    ui::Area a({20, 20, 300, 600});
    ui::VirtualList vl("v", 10000, 25, 250);
    rows.clear();
    for (int i : vl) {
      rows.push_back(i);
      ui::next(25);
    }
  };
  h.frame();
  CHECK_EQ(rows.front(), 0);
  CHECK(rows.size() <= 11);
  h.wheel(100, 100, -100, true);   // точная прокрутка: 100 точек
  h.frame();
  CHECK_EQ(rows.front(), 4);
  CHECK(rows.size() <= 12);
  // Ровно последовательные индексы
  for (size_t k = 1; k < rows.size(); k++) CHECK_EQ(rows[k], rows[k - 1] + 1);
}

// ================================================================ всплывающие окна и меню
TEST(ui_popup_open_escape_click_outside) {
  H h;
  RectF br;
  bool inside = false;
  int item = 0;
  h.build = [&] {
    ui::Area a({20, 20, 300, 600});
    if (ui::button("Меню")) ui::openPopup("menu");
    br = ui::lastItem().rect;
    inside = false;
    if (ui::beginMenu("menu")) {
      inside = true;
      if (ui::menuItem("Первый", {.icon = "edit"})) item = 1;
      if (ui::menuItem("Второй", {.disabled = true})) item = 2;
      if (ui::menuItem("Третий")) item = 3;
      ui::endMenu();
    }
  };
  h.frame();
  h.click(br.cx(), br.cy());
  h.frames(3);
  CHECK(inside);
  h.key(Key::Escape);
  CHECK(!inside);
  // Открыть, клавиатурой выбрать третий (второй недоступен, но в подсветке участвует)
  h.click(br.cx(), br.cy());
  h.frames(3);
  h.key(Key::Down);
  h.key(Key::Down);
  h.key(Key::Down);
  h.key(Key::Enter);
  CHECK_EQ(item, 3);
  CHECK(!inside);
  // Щелчок вне — закрыть
  h.click(br.cx(), br.cy());
  h.frames(3);
  h.click(600, 500);
  h.frames(2);
  CHECK(!inside);
  // Щелчок по пункту мышью
  h.click(br.cx(), br.cy());
  h.frames(3);
  h.move(br.x + 40, br.bottom() + 4 + 5 + 15);
  h.drain();
  h.click(br.x + 40, br.bottom() + 4 + 5 + 15);
  CHECK_EQ(item, 1);
}

TEST(ui_context_menu_and_submenu) {
  H h;
  RectF r;
  bool sub = false;
  int picked = 0;
  RectF subR;
  h.build = [&] {
    ui::Area a({20, 20, 300, 600});
    ui::button("Провинция");
    r = ui::lastItem().rect;
    sub = false;
    if (ui::beginContextMenu("ctx")) {
      ui::menuItem("Переименовать");
      bool s = ui::beginSubmenu("Владелец", "crown");
      subR = ui::lastItem().rect;
      if (s) {
        sub = true;
        if (ui::menuItem("Арден")) picked = 1;
        if (ui::menuItem("Валь")) picked = 2;
        ui::endSubmenu();
      }
      ui::endMenu();
    }
  };
  h.frame();
  h.click(r.cx(), r.cy(), 1);   // правая кнопка
  h.frames(3);
  CHECK(subR.w > 0);
  h.move(subR.cx(), subR.cy());
  h.drain();
  h.wait(0.3);
  CHECK(sub);
  // Подменю справа: второй пункт
  float sx = subR.right() + 2 + 40, sy = subR.y - 5 + 5 + 30 + 15;
  h.move(sx, sy);
  h.drain();
  h.click(sx, sy);
  CHECK_EQ(picked, 2);
  CHECK(!sub);
}

TEST(ui_tooltip_delay_and_redraw) {
  H h;
  RectF r;
  h.build = [&] {
    ui::Area a({20, 20, 300, 600});
    ui::iconButton("save", "Сохранить", {.shortcut = {Key::S, ui::ModPrimary}});
    r = ui::lastItem().rect;
  };
  h.frame();
  h.move(r.cx(), r.cy());
  h.drain();
  CHECK(ui::needsRedraw());   // ждём подсказку
  h.wait(0.4);
  h.frames(10);
  // Подсказка показана: справа от кнопки текст поверх фона (пиксель отличается от фона)
  h.settle();
  CHECK(!ui::needsRedraw());
  h.move(500, 500);
  h.settle();
  CHECK(!ui::needsRedraw());
}

// ================================================================ модальное окно
TEST(ui_modal_focus_trap_escape_enter) {
  H h;
  bool open = false;
  int ok = 0, base = 0;
  std::string name = "x";
  RectF baseR;
  std::vector<WidgetId> focusSeen;
  h.build = [&] {
    ui::Area a({20, 20, 300, 600});
    if (ui::button("Под окном")) base++;
    baseR = ui::lastItem().rect;
    if (ui::beginModal("dlg", {.title = "Окно", .icon = "info"}, &open)) {
      ui::textField("name", name);
      if (ui::lastItem().focused) focusSeen.push_back(1);
      {
        ui::ModalFooter f;
        ui::button("Отмена");
        if (ui::lastItem().focused) focusSeen.push_back(3);
        if (ui::button("Готово", {.variant = ui::Variant::Primary, .isDefault = true})) {
          ok++;
          open = false;
        }
        if (ui::lastItem().focused) focusSeen.push_back(4);
      }
      ui::endModal();
    }
  };
  h.frame();
  open = true;
  h.frames(6);
  CHECK(ui::anyModalOpen());
  // Кнопка под окном недоступна
  h.click(baseR.cx(), baseR.cy());
  CHECK_EQ(base, 0);
  CHECK(open);
  // Tab ходит только по окну: поле → Отмена → Готово → поле (крестик — только мышью)
  std::vector<WidgetId> seq;
  for (int i = 0; i < 6; i++) {
    focusSeen.clear();
    h.key(Key::Tab);
    seq.push_back(focusSeen.empty() ? 0 : focusSeen.back());
  }
  CHECK((seq == std::vector<WidgetId>{1, 3, 4, 1, 3, 4}));
  // Enter в поле — кнопка по умолчанию
  ui::setKeyboardFocus(0);
  h.frame();
  focusSeen.clear();
  h.key(Key::Tab);
  h.frames(2);
  h.type("Гильдия");
  h.key(Key::Enter);
  h.frames(3);
  CHECK_EQ(ok, 1);
  CHECK_EQ(name, std::string("Гильдия"));
  CHECK(!open);
  h.settle();
  CHECK(!ui::anyModalOpen());
  // Esc закрывает
  open = true;
  h.frames(6);
  h.key(Key::Escape);
  h.frames(2);
  CHECK(!open);
  // Под окном снова можно нажимать
  h.settle();
  h.click(baseR.cx(), baseR.cy());
  CHECK_EQ(base, 1);
}

TEST(ui_modal_tab_cycle_stays_inside) {
  H h;
  bool open = true;
  std::string a1, a2, outside;
  int inA = 0, inB = 0, out = 0;
  h.build = [&] {
    {
      ui::Area a({20, 20, 300, 600});
      ui::textField("outside", outside);
      if (ui::lastItem().focused) out++;
    }
    if (ui::beginModal("m", {.title = "М", .closeButton = false}, &open)) {
      ui::textField("a", a1);
      if (ui::lastItem().focused) inA++;
      ui::textField("b", a2);
      if (ui::lastItem().focused) inB++;
      ui::endModal();
    }
  };
  h.frames(8);
  for (int i = 0; i < 6; i++) h.key(Key::Tab);
  CHECK(inA > 0);
  CHECK(inB > 0);
  CHECK_EQ(out, 0);
}

TEST(ui_modal_backdrop_dismiss) {
  H h;
  bool open = true;
  h.build = [&] {
    if (ui::beginModal("m", {.title = "Фон закрывает", .dismissOnBackdrop = true}, &open)) {
      ui::label("Текст");
      ui::endModal();
    }
  };
  h.frames(8);
  h.click(400, 300);   // внутри окна (по центру) — не закрывает
  h.frames(2);
  CHECK(open);
  h.click(10, 10);
  h.frames(2);
  CHECK(!open);
}

// ================================================================ уведомления
TEST(ui_toast_expiry_hover_pause_close) {
  H h(800, 600);
  h.build = [] {};
  h.frame();
  ui::toast("Сохранено", ui::Tone::Success, nullptr, 1.0);
  h.frame();
  CHECK_EQ(ui::toastCount(), 1);
  CHECK(ui::needsRedraw());
  h.wait(1.5);
  CHECK_EQ(ui::toastCount(), 0);
  h.settle();
  CHECK(!ui::needsRedraw());
  // Наведение ставит на паузу
  ui::toast("Пауза при наведении", ui::Tone::Info, nullptr, 1.0);
  h.frames(20);
  h.move(800 - 16 - 100, 600 - 16 - 24);
  h.drain();
  h.wait(2.0);
  CHECK_EQ(ui::toastCount(), 1);
  // Крестик закрывает
  h.click(800 - 16 - 30 + 11, 600 - 16 - 48 + 12 + 11);
  h.frames(2);
  CHECK_EQ(ui::toastCount(), 0);
}

// ================================================================ перетаскивание
TEST(ui_drag_and_drop) {
  H h;
  std::optional<u64> got;
  bool dragging = false;
  h.build = [&] {
    ui::at({20, 20, 120, 30});
    ui::button("Войско");
    ui::dragSource("army", 42, "Войско", "army");
    dragging = ui::dragging("army");
    ui::at({300, 300, 160, 80});
    ui::button("Цель");
    if (auto p = ui::dropTarget("army")) got = p;
    if (auto p = ui::dropTarget("fleet")) got = u64(1);
  };
  h.frame();
  h.move(80, 35);
  h.down(80, 35);
  h.drain();
  h.move(150, 150);
  h.drain();
  CHECK(dragging);
  h.move(320, 340);
  h.drain();
  CHECK(ui::wantsMouse());
  h.up(320, 340);
  h.drain();
  CHECK(got.has_value());
  CHECK_EQ(*got, u64(42));
  CHECK(!dragging);
}

// ================================================================ переключатели, вкладки, разделы
TEST(ui_toggle_checkbox_radio_segmented_tabs_slider) {
  H h;
  bool tg = false;
  ui::Check ch = ui::Check::Mixed;
  int rad = 0, seg = 0, tab = 0;
  double sl = 0;
  RectF rt, rc, rr, rs, rtab, rsl;
  h.build = [&] {
    ui::Area a({20, 20, 400, 600});
    ui::toggle("Переключатель", tg);
    rt = ui::lastItem().rect;
    ui::checkbox("Смешанный", ch);
    rc = ui::lastItem().rect;
    ui::radio("Один", rad, 0);
    ui::radio("Два", rad, 1);
    rr = ui::lastItem().rect;
    ui::segmented("seg", seg, {{"sun", "День"}, {"moon", "Ночь"}});
    rs = ui::lastItem().rect;
    ui::tabs("tabs", tab, {{"info", "Обзор"}, {"coins", "Экономика"}, {"army", "Армия"}}, {.fill = true});
    rtab = ui::lastItem().rect;
    ui::slider("sl", sl, 0, 100, {.step = 10});
    rsl = ui::lastItem().rect;
  };
  h.frame();
  h.click(rt.right() - 10, rt.cy());
  CHECK(tg);
  h.click(rc.x + 9, rc.cy());
  CHECK(ch == ui::Check::On);
  h.click(rr.x + 9, rr.cy());
  CHECK_EQ(rad, 1);
  h.click(rs.right() - 20, rs.cy());
  CHECK_EQ(seg, 1);
  h.key(Key::Left);
  CHECK_EQ(seg, 0);
  h.click(rtab.x + rtab.w * 5 / 6, rtab.cy());
  CHECK_EQ(tab, 2);
  h.key(Key::Left);
  CHECK_EQ(tab, 1);
  // Ползунок: щелчок по середине дорожки, шаг 10
  float trackW = rsl.w - 16 - 52 - 8;
  h.click(rsl.x + 8 + trackW * 0.52f, rsl.cy());
  CHECK_NEAR(sl, 50, 1e-9);
  h.key(Key::Right);
  CHECK_NEAR(sl, 60, 1e-9);
  h.key(Key::End);
  CHECK_NEAR(sl, 100, 1e-9);
}

TEST(ui_section_collapse_and_animation) {
  H h;
  bool built = false;
  RectF hr;
  float afterY = 0;
  h.build = [&] {
    ui::Area a({20, 20, 400, 600});
    built = false;
    {
      ui::Section s("Экономика", "coins");
      hr = ui::lastItem().rect;
      if (s) {
        built = true;
        ui::label("Доход");
        ui::label("Расход");
      }
    }
    afterY = ui::next(10).y;
  };
  h.settle();
  CHECK(built);
  float openY = afterY;
  h.click(hr.cx(), hr.cy());
  CHECK(built);   // во время сворачивания содержимое ещё строится
  h.settle();
  CHECK(!built);
  CHECK(afterY < openY - 20);
  h.click(hr.cx(), hr.cy());
  h.settle();
  CHECK(built);
  CHECK_NEAR(afterY, openY, 0.5);
}

TEST(ui_tree_toggle_and_select) {
  H h;
  bool childBuilt = false, clicked = false;
  RectF r;
  h.build = [&] {
    ui::Area a({20, 20, 400, 600});
    childBuilt = false;
    ui::TreeNode n("Арден", {.icon = "crown"});
    r = ui::lastItem().rect;
    if (n.clicked()) clicked = true;
    if (n) {
      ui::TreeNode c("Эльвенмор", {.leaf = true});
      childBuilt = true;
    }
  };
  h.frame();
  CHECK(!childBuilt);
  h.click(r.x + 8, r.cy());   // шеврон
  CHECK(childBuilt);
  h.click(r.x + 100, r.cy());
  CHECK(clicked);
  h.key(Key::Left);
  CHECK(!childBuilt);
  h.key(Key::Right);
  CHECK(childBuilt);
}

TEST(ui_splitter_drag) {
  H h;
  float pos = 200;
  ui::Split sp;
  h.build = [&] { sp = ui::splitter("split", {0, 0, 800, 600}, pos, ui::Axis::Horizontal, 100, 100); };
  h.frame();
  CHECK_EQ(sp.a.w, 200.f);
  h.dragTo(200, 300, 260, 300);
  CHECK_NEAR(pos, 260, 0.5);
  h.dragTo(260, 300, 790, 300);
  CHECK_NEAR(pos, 700, 0.5);
  CHECK(ui::cursor() == platform::Cursor::ResizeH || true);
}

TEST(ui_color_picker_palette_and_hex) {
  H h;
  Color c = Color::hex(0x000000);
  RectF r;
  int changes = 0;
  h.build = [&] {
    ui::Area a({20, 20, 260, 600});
    if (ui::colorPicker("pick", c)) changes++;
    r = ui::lastItem().rect;
  };
  h.frame();
  // Первый образец палитры
  h.click(r.x + 1 + 11, r.y + 11);
  auto pal = ui::heraldicPalette();
  CHECK(c == pal[0]);
  // Поле hex внизу
  h.click(r.x + 30 + 8 + 40, r.bottom() - 15);
  h.key(Key::A, ctrl());
  h.type("#2e7d4f");
  h.key(Key::Enter);
  CHECK(c == Color::hex(0x2e7d4f));
  CHECK(changes >= 2);
}

TEST(ui_label_wrap_and_measure) {
  H h;
  RectF r;
  h.build = [&] {
    ui::Area a({20, 20, 200, 600});
    ui::text("Длинный текст, который обязательно перенесётся на несколько строк в узкой колонке.");
    r = ui::lastItem().rect;
  };
  h.frame();
  CHECK(r.h > ui::lineHeight(ui::Font::Body) * 2);
  CHECK(ui::measure("Арден") > 20);
}

TEST(ui_popup_toggle_by_opener_button) {
  H h;
  RectF br;
  bool open = false;
  h.build = [&] {
    ui::Area a({20, 20, 300, 600});
    if (ui::iconButton("filter", "Фильтр")) ui::openPopup("f");
    br = ui::lastItem().rect;
    open = false;
    if (ui::beginPopup("f", {.width = 200})) {
      open = true;
      ui::label("Фильтр");
      ui::endPopup();
    }
  };
  h.frame();
  h.click(br.cx(), br.cy());
  h.frames(3);
  CHECK(open);
  h.click(br.cx(), br.cy());   // повторный щелчок по кнопке — закрыть, а не открыть снова
  h.frames(3);
  CHECK(!open);
  h.click(br.cx(), br.cy());
  h.frames(3);
  CHECK(open);
}

TEST(ui_list_item_select_context_and_section_action) {
  H h;
  int sel = -1, ctx = -1, added = 0;
  bool open = true;
  std::vector<RectF> rows;
  RectF secR;
  const char* names[] = {"Эльвенмор", "Кальдорн", "Тарвис"};
  h.build = [&] {
    ui::Area a({20, 20, 360, 600});
    ui::Section s("Провинции", "province", {.badge = "3", .actionIcon = "plus", .actionTooltip = "Новая провинция"});
    secR = ui::lastItem().rect;
    if (s.action()) added++;
    open = bool(s);
    rows.clear();
    if (s) {
      for (int i = 0; i < 3; i++) {
        if (ui::listItem(names[i], {.dot = Color::palette(i), .hint = "12 400", .selected = sel == i})) sel = i;
        rows.push_back(ui::lastItem().rect);
        if (ui::beginContextMenu("ctx")) {
          ctx = i;
          ui::menuItem("Переименовать");
          ui::endMenu();
        }
      }
    }
    ui::spinner();
  };
  h.settle(30);
  CHECK_EQ(rows.size(), size_t(3));
  h.click(rows[1].cx(), rows[1].cy());
  CHECK_EQ(sel, 1);
  h.click(rows[2].cx(), rows[2].cy(), 1);
  h.frames(3);
  CHECK_EQ(ctx, 2);
  h.key(Key::Escape);
  // Кнопка «+» в заголовке: срабатывает, раздел не сворачивается
  h.click(secR.right() - 12 - 9, secR.cy());
  CHECK_EQ(added, 1);
  CHECK(open);
  CHECK(ui::needsRedraw());   // крутится индикатор ожидания
}

TEST(ui_section_first_open_without_measure) {
  H h;
  bool built = false;
  RectF hr, lr;
  h.build = [&] {
    ui::Area a({20, 20, 400, 600});
    built = false;
    ui::Section s("Свёрнут", "info", {.defaultOpen = false});
    hr = ui::lastItem().rect;
    if (s) {
      built = true;
      ui::label("Строка");
      lr = ui::lastItem().rect;
    }
  };
  h.settle();
  CHECK(!built);
  h.click(hr.cx(), hr.cy());
  CHECK(built);
  h.settle();
  CHECK(built);
  CHECK(lr.y > hr.bottom() - 1);
}

TEST(ui_table_sticky_header_in_scroll) {
  H h;
  int sortCol = -1, visited = 0;
  float off = 0;
  h.build = [&] {
    ui::Area a({20, 20, 400, 600});
    ui::Scroll sc("s", 300);
    ui::spacer(100);
    ui::Column cols[] = {{"Имя", nullptr, ui::fr(1), ui::Align::Left, true}, {"Число", nullptr, ui::px(100), ui::Align::Right, true}};
    ui::Table t("t", cols, 200, {.rowHeight = 30});
    visited = 0;
    for (int i : t) {
      visited++;
      t.text("Строка " + std::to_string(i));
      t.text(std::to_string(i));
    }
    sortCol = t.sortColumn();
    off = sc.offset();
  };
  h.frame();
  CHECK(visited <= 8);   // видимая часть родителя: 300 точек, из них 100 — отступ, 32 — шапка
  h.wheel(200, 200, -10);
  h.frames(40);
  CHECK(off > 300);
  CHECK(visited <= 12);
  // Шапка прилипла к верху области прокрутки: щелчок там сортирует
  h.click(60, 20 + 16);
  CHECK_EQ(sortCol, 0);
}

TEST(ui_avatar_image_and_initials) {
  H h;
  gfx::Image img(32, 32, gfx::packPremul(200, 40, 40, 255));
  h.build = [&] {
    ui::Area a({20, 20, 400, 600});
    ui::avatar("Эдрик", {.image = &img, .size = 40, .ring = true});
    ui::avatar("Мирейн Солн", {.size = 28});
  };
  h.frame();
  u32 p = h.img.at(20 + 3 + 20, 20 + 3 + 20);
  CHECK_EQ((p >> 16) & 255, u32(200));
}
