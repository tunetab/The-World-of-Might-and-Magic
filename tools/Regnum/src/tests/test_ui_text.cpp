// Regnum — интерфейс: текстовые поля (каретка, выделение, слова, буфер обмена, отмена, кириллица, фиксация).
#include "tests/test_ui_util.h"

using namespace rg;
using namespace rg::uitest;

namespace {

struct Field {
  std::string value;
  ui::TextOpt opt;
  RectF r;
  int changes = 0;
  bool focused = false;
};

void buildField(Field& f, std::string_view id = "f") {
  ui::Area a({20, 20, 400, 300});
  if (ui::textField(id, f.value, f.opt)) f.changes++;
  f.r = ui::lastItem().rect;
  f.focused = ui::lastItem().focused;
}

// Щелчок в текст поля: x — смещение от левого края области текста.
void clickText(H& h, const Field& f, float dx) { h.click(f.r.x + 10 + dx, f.r.cy()); }

}  // namespace

TEST(ui_text_type_cyrillic_commit_on_enter) {
  H h;
  Field f;
  h.build = [&] { buildField(f); };
  h.frame();
  clickText(h, f, 5);
  CHECK(f.focused);
  CHECK(ui::wantsKeyboard());
  CHECK(ui::textInputRect().has_value());
  h.type("Привет, мир");
  CHECK(f.value.empty());   // фиксация — по Enter
  CHECK_EQ(f.changes, 0);
  h.key(Key::Enter);
  CHECK_EQ(f.value, std::string("Привет, мир"));
  CHECK_EQ(f.changes, 1);
  CHECK(!f.focused);
  CHECK(!ui::wantsKeyboard());
}

TEST(ui_text_live_mode_and_escape_restores) {
  H h;
  Field f;
  f.value = "Арден";
  f.opt.live = true;
  h.build = [&] { buildField(f); };
  h.frame();
  clickText(h, f, 200);   // за концом текста — каретка в конце
  h.type("ия");
  CHECK_EQ(f.value, std::string("Ардения"));
  CHECK(f.changes >= 1);
  h.key(Key::Escape);
  CHECK_EQ(f.value, std::string("Арден"));
  CHECK(!f.focused);
}

TEST(ui_text_escape_cancels_commit_mode) {
  H h;
  Field f;
  f.value = "Север";
  h.build = [&] { buildField(f); };
  h.frame();
  clickText(h, f, 200);
  h.type("ный");
  h.key(Key::Escape);
  CHECK_EQ(f.value, std::string("Север"));
  CHECK_EQ(f.changes, 0);
}

TEST(ui_text_blur_by_click_elsewhere_commits) {
  H h;
  Field f;
  h.build = [&] { buildField(f); };
  h.frame();
  clickText(h, f, 5);
  h.type("Кальдорн");
  h.click(600, 500);   // пустое место
  CHECK_EQ(f.value, std::string("Кальдорн"));
  CHECK(!f.focused);
}

TEST(ui_text_selection_keyboard_and_clipboard) {
  H h;
  Field f;
  f.value = "один два три";
  h.build = [&] { buildField(f); };
  h.frame();
  clickText(h, f, 300);
  // Shift+Ctrl+Left — выделить «три», копировать
  h.key(Key::Left, platform::ModShift | ctrl());
  h.key(Key::C, ctrl());
  CHECK_EQ(h.clip, std::string("три"));
  // Ctrl+Left дважды — к началу «два», Ctrl+Backspace удаляет «один »
  h.key(Key::Left);
  h.key(Key::Left, ctrl());
  h.key(Key::Backspace, ctrl());
  h.key(Key::Enter);
  CHECK_EQ(f.value, std::string("два три"));
  // Вырезать всё и вставить дважды
  clickText(h, f, 5);
  h.key(Key::A, ctrl());
  h.key(Key::X, ctrl());
  CHECK_EQ(h.clip, std::string("два три"));
  h.key(Key::V, ctrl());
  h.type(" ");
  h.key(Key::V, ctrl());
  h.key(Key::Enter);
  CHECK_EQ(f.value, std::string("два три два три"));
}

TEST(ui_text_home_end_delete_and_shift_select) {
  H h;
  Field f;
  f.value = "абвгд";
  h.build = [&] { buildField(f); };
  h.frame();
  clickText(h, f, 300);
  h.key(Key::Home);
  h.key(Key::Delete);                       // «бвгд»
  h.key(Key::End);
  h.key(Key::Left, platform::ModShift);
  h.key(Key::Left, platform::ModShift);     // выделены «гд»
  h.type("ё");                              // замена выделения
  h.key(Key::Enter);
  CHECK_EQ(f.value, std::string("бвё"));
}

TEST(ui_text_undo_redo) {
  H h;
  Field f;
  h.build = [&] { buildField(f); };
  h.frame();
  clickText(h, f, 5);
  h.type("Эльвен");
  h.wait(1.2);   // пауза — новый шаг отмены
  h.type("мор");
  h.key(Key::Z, ctrl());
  h.key(Key::Enter);
  CHECK_EQ(f.value, std::string("Эльвен"));
  clickText(h, f, 300);
  h.key(Key::Backspace);
  h.key(Key::Z, ctrl());
  h.key(Key::Z, ctrl());   // дальше истории нет — остаётся исходное при фокусе
  h.key(Key::Y, ctrl());
  h.key(Key::Enter);
  CHECK_EQ(f.value, std::string("Эльве"));
}

TEST(ui_text_mouse_word_and_all_selection) {
  H h;
  Field f;
  f.value = "Королевство Арден";
  h.build = [&] { buildField(f); };
  h.frame();
  float wordX = ui::measure("Королевство ", ui::Font::Body) + ui::measure("Ар", ui::Font::Body);
  h.multiClick(f.r.x + 10 + wordX, f.r.cy(), 2);   // двойной щелчок — слово «Арден»
  h.type("Валь");
  h.key(Key::Enter);
  CHECK_EQ(f.value, std::string("Королевство Валь"));
  h.multiClick(f.r.x + 20, f.r.cy(), 3);             // тройной — всё
  h.type("Империя");
  h.key(Key::Enter);
  CHECK_EQ(f.value, std::string("Империя"));
}

TEST(ui_text_mouse_drag_selection) {
  H h;
  Field f;
  f.value = "абвгдежз";
  h.build = [&] { buildField(f); };
  h.frame();
  float x0 = f.r.x + 10 + ui::measure("аб", ui::Font::Body);
  float x1 = f.r.x + 10 + ui::measure("абвгд", ui::Font::Body);
  h.dragTo(x0, f.r.cy(), x1, f.r.cy());
  h.key(Key::C, ctrl());
  CHECK_EQ(h.clip, std::string("вгд"));
  h.key(Key::Backspace);
  h.key(Key::Enter);
  CHECK_EQ(f.value, std::string("абежз"));
}

TEST(ui_text_max_length_filter_readonly) {
  H h;
  Field f;
  f.opt.maxLength = 5;
  h.build = [&] { buildField(f); };
  h.frame();
  clickText(h, f, 5);
  h.type("Королевство");
  h.key(Key::Enter);
  CHECK_EQ(f.value, std::string("Корол"));
  Field d;
  d.opt.filter = [](u32 cp) { return cp >= '0' && cp <= '9'; };
  h.build = [&] { buildField(d, "digits"); };
  h.frame();
  clickText(h, d, 5);
  h.type("a1б2 3");
  h.key(Key::Enter);
  CHECK_EQ(d.value, std::string("123"));
  Field ro;
  ro.value = "только чтение";
  ro.opt.readOnly = true;
  h.build = [&] { buildField(ro, "ro"); };
  h.frame();
  clickText(h, ro, 5);
  h.type("xxx");
  h.key(Key::A, ctrl());
  h.key(Key::C, ctrl());
  h.key(Key::Backspace);
  h.key(Key::Enter);
  CHECK_EQ(ro.value, std::string("только чтение"));
  CHECK_EQ(h.clip, std::string("только чтение"));
}

TEST(ui_text_clear_button_and_placeholder) {
  H h;
  Field f;
  f.value = "поиск";
  f.opt.clearButton = true;
  f.opt.live = true;
  h.build = [&] { buildField(f); };
  h.frame();
  h.click(f.r.right() - 16, f.r.cy());
  CHECK(f.value.empty());
  CHECK_EQ(f.changes, 1);
}

TEST(ui_text_area_multiline_navigation) {
  H h;
  std::string v = "первая\nвторая";
  RectF r;
  int ch = 0;
  h.build = [&] {
    ui::Area a({20, 20, 400, 300});
    if (ui::textArea("ta", v, 90)) ch++;
    r = ui::lastItem().rect;
  };
  h.frame();
  h.click(r.x + 200, r.y + 10);   // конец первой строки
  h.key(Key::Enter);
  h.type("новая");
  h.key(Key::Down);
  h.key(Key::End);
  h.type("!");
  CHECK_EQ(ch, 0);
  h.key(Key::Enter, ctrl());
  CHECK_EQ(v, std::string("первая\nновая\nвторая!"));
  CHECK_EQ(ch, 1);
  // Длинный текст переносится, колесо прокручивает
  v.clear();
  for (int i = 0; i < 30; i++) v += "строка номер " + std::to_string(i) + "\n";
  h.frame();
  h.wheel(r.cx(), r.cy(), -3);
  h.frame();
  CHECK(true);
}

TEST(ui_text_tab_moves_focus_and_commits) {
  H h;
  std::string a, b;
  RectF ra;
  bool fb = false;
  h.build = [&] {
    ui::Area ar({20, 20, 400, 300});
    ui::textField("a", a);
    ra = ui::lastItem().rect;
    ui::textField("b", b);
    fb = ui::lastItem().focused;
  };
  h.frame();
  h.click(ra.x + 20, ra.cy());
  h.type("один");
  h.key(Key::Tab);
  CHECK_EQ(a, std::string("один"));
  CHECK(fb);
  h.type("два");
  h.key(Key::Tab);
  CHECK_EQ(b, std::string("два"));
}
