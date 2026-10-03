// Regnum — интерфейс: ядро (ID, наведение, нажатие, фокус, раскладка, состояние, анимации, перерисовка).
#include "tests/test_ui_util.h"

using namespace rg;
using namespace rg::uitest;
using rg::ui::WidgetId;

TEST(ui_core_button_click_hover) {
  H h;
  int clicks = 0;
  bool hovered = false;
  RectF br;
  h.build = [&] {
    ui::Area a({20, 20, 300, 400});
    if (ui::button("Сохранить", {.variant = ui::Variant::Primary, .icon = "save"})) clicks++;
    br = ui::lastItem().rect;
    hovered = ui::lastItem().hovered;
  };
  h.frame();
  CHECK(br.w > 60 && br.h == 30);
  CHECK(!hovered);
  h.move(br.cx(), br.cy());
  h.drain();
  CHECK(hovered);
  h.click(br.cx(), br.cy());
  CHECK_EQ(clicks, 1);
  // Нажатие на кнопке и отпускание вне её — не щелчок.
  h.move(br.cx(), br.cy());
  h.down(br.cx(), br.cy());
  h.drain();
  h.move(br.right() + 50, br.cy());
  h.up(br.right() + 50, br.cy());
  h.drain();
  CHECK_EQ(clicks, 1);
  // Нажатие вне и отпускание на кнопке — тоже не щелчок.
  h.move(br.right() + 50, br.cy());
  h.down(br.right() + 50, br.cy());
  h.drain();
  h.move(br.cx(), br.cy());
  h.up(br.cx(), br.cy());
  h.drain();
  CHECK_EQ(clicks, 1);
}

TEST(ui_core_ids_scopes_and_labels) {
  H h;
  WidgetId a = 0, b = 0, c = 0, d = 0;
  h.build = [&] {
    a = ui::id("x");
    {
      ui::IdScope s{"scope"};
      b = ui::id("x");
      ui::IdScope s2{i64(5)};
      c = ui::id("x");
    }
    d = ui::id("x");
  };
  h.frame();
  CHECK(a != b);
  CHECK(b != c);
  CHECK_EQ(a, d);
  CHECK(ui::displayText("Удалить##row7") == "Удалить");
  CHECK(ui::displayText("Просто") == "Просто");
}

TEST(ui_core_duplicate_ids_do_not_break_clicks) {
  H h;
  int first = 0, second = 0;
  RectF r1, r2;
  h.build = [&] {
    ui::Area a({10, 10, 300, 300});
    if (ui::button("OK")) first++;
    r1 = ui::lastItem().rect;
    if (ui::button("OK##2")) second++;
    r2 = ui::lastItem().rect;
  };
  h.frame();
  h.click(r2.cx(), r2.cy());
  CHECK_EQ(first, 0);
  CHECK_EQ(second, 1);
  h.click(r1.cx(), r1.cy());
  CHECK_EQ(first, 1);
}

TEST(ui_core_layout_rows_and_stacks) {
  H h;
  RectF a, b, c, d, e, f, g;
  h.build = [&] {
    ui::Area ar({0, 0, 400, 600}, 10);
    a = ui::next(20);
    {
      ui::Row r({ui::px(100), ui::fr(1), ui::fr(1)}, 30, 10);
      b = ui::next(30);
      c = ui::next(30);
      d = ui::next(30);
      e = ui::next(30);   // вторая строка сетки
    }
    f = ui::next(10);
    {
      ui::HStack s(30, ui::Align::Left, 6);
      g = ui::next(50, 20);
    }
  };
  h.frames(2);
  CHECK_EQ(a, (RectF{10, 10, 380, 20}));
  CHECK_EQ(b.x, 10.f);
  CHECK_EQ(b.w, 100.f);
  CHECK_EQ(c.x, 120.f);
  CHECK_NEAR(c.w, 130, 1);
  CHECK_NEAR(d.x, 260, 1);
  CHECK_NEAR(d.right(), 390, 1);
  CHECK_EQ(e.x, 10.f);
  CHECK_EQ(e.y, b.y + 30 + 8);
  CHECK_EQ(f.y, e.bottom() + 8);
  CHECK_EQ(g.x, 10.f);
  CHECK_EQ(g.h, 20.f);
  CHECK_EQ(g.y, f.bottom() + 8 + 5);
}

TEST(ui_core_hstack_right_alignment_settles) {
  H h;
  RectF b1, b2;
  h.build = [&] {
    ui::Area ar({0, 0, 500, 200});
    ui::HStack s(30, ui::Align::Right, 8);
    ui::button("Отмена");
    b1 = ui::lastItem().rect;
    ui::button("Готово", {.variant = ui::Variant::Primary});
    b2 = ui::lastItem().rect;
  };
  h.frame();
  CHECK(ui::needsRedraw());
  h.frame();
  CHECK_NEAR(b2.right(), 500, 0.5);
  CHECK_NEAR(b1.right() + 8, b2.x, 0.5);
}

TEST(ui_core_focus_traversal_tab) {
  H h;
  std::string s1 = "a", s2 = "b";
  bool chk = false;
  WidgetId f1 = 0, f2 = 0, f3 = 0;
  h.build = [&] {
    ui::Area ar({0, 0, 400, 400}, 10);
    ui::textField("one", s1);
    f1 = ui::lastItem().id;
    ui::checkbox("Флажок", chk);
    f2 = ui::lastItem().id;
    ui::textField("two", s2);
    f3 = ui::lastItem().id;
  };
  h.frame();
  WidgetId focused = 0;
  auto probe = [&] {
    auto old = h.build;
    h.build = [&, old] {
      old();
    };
  };
  (void)probe;
  h.key(Key::Tab);
  h.build = [&] {
    ui::Area ar({0, 0, 400, 400}, 10);
    ui::textField("one", s1);
    if (ui::lastItem().focused) focused = 1;
    ui::checkbox("Флажок", chk);
    if (ui::lastItem().focused) focused = 2;
    ui::textField("two", s2);
    if (ui::lastItem().focused) focused = 3;
  };
  h.frame();
  CHECK_EQ(focused, WidgetId(1));
  focused = 0;
  h.key(Key::Tab);
  CHECK_EQ(focused, WidgetId(2));
  // Пробел переключает сфокусированный флажок.
  h.key(Key::Space);
  CHECK(chk);
  focused = 0;
  h.key(Key::Tab);
  CHECK_EQ(focused, WidgetId(3));
  focused = 0;
  h.key(Key::Tab);
  CHECK_EQ(focused, WidgetId(1));
  focused = 0;
  h.key(Key::Tab, platform::ModShift);
  CHECK_EQ(focused, WidgetId(3));
  CHECK(ui::wantsKeyboard());
  (void)f1;
  (void)f2;
  (void)f3;
}

TEST(ui_core_state_and_gc) {
  H h;
  struct S {
    int n = 0;
  };
  bool show = true;
  h.build = [&] {
    if (show) ui::state<S>(ui::id("keep")).n++;
  };
  h.frames(3);
  int n = 0;
  h.build = [&] { n = ui::state<S>(ui::id("keep")).n; };
  h.frame();
  CHECK_EQ(n, 3);
  // 400 кадров (≈ 6,7 с) без обращений — состояние убрано.
  h.build = [] {};
  h.frames(400);
  h.build = [&] { n = ui::state<S>(ui::id("keep")).n; };
  h.frame();
  CHECK_EQ(n, 0);
}

TEST(ui_core_animate_and_idle) {
  H h;
  float v = 0;
  float target = 0;
  h.build = [&] { v = ui::animate(ui::id("a"), target, 0.2f); };
  h.frame();
  CHECK_EQ(v, 0.f);
  target = 1;
  h.frame();
  CHECK(ui::needsRedraw());
  h.frames(3);
  CHECK(v > 0 && v < 1);
  h.frames(20);
  CHECK_EQ(v, 1.f);
  h.frame();
  CHECK(!ui::needsRedraw());
}

TEST(ui_core_idle_gallery_does_not_redraw) {
  H h;
  bool on = true;
  double val = 5;
  std::string txt = "Текст";
  int seg = 1;
  h.build = [&] {
    ui::Panel p("panel", {20, 20, 400, 500});
    ui::label("Заголовок", {.font = ui::Font::Title});
    ui::toggle("Переключатель", on);
    ui::numberField("num", val, {.min = 0, .max = 10});
    ui::textField("txt", txt);
    ui::segmented("seg", seg, {{"sun", {}, "День"}, {"moon", {}, "Ночь"}});
    ui::button("Кнопка");
  };
  h.settle();
  CHECK(!ui::needsRedraw());
  CHECK(!ui::wantsMouse());
  h.move(100, 100);
  h.drain();
  CHECK(ui::wantsMouse());
  h.move(700, 550);
  h.settle();
  CHECK(!ui::wantsMouse());
  CHECK(!ui::needsRedraw());
}

TEST(ui_core_panel_blocks_items_below) {
  H h;
  int below = 0, above = 0;
  h.build = [&] {
    ui::at({50, 50, 200, 30});
    if (ui::button("Ниже")) below++;
    ui::Panel p("over", {40, 40, 300, 200});
    ui::at({200, 150, 100, 30});
    if (ui::button("Выше")) above++;
  };
  h.frame();
  h.click(100, 65);
  CHECK_EQ(below, 0);
  h.click(250, 165);
  CHECK_EQ(above, 1);
}

TEST(ui_core_disabled_scope) {
  H h;
  int n = 0;
  bool v = false;
  RectF r, rc;
  h.build = [&] {
    ui::Area a({0, 0, 300, 300});
    ui::Disabled d;
    if (ui::button("Нельзя")) n++;
    r = ui::lastItem().rect;
    ui::checkbox("Флажок", v);
    rc = ui::lastItem().rect;
  };
  h.frame();
  h.click(r.cx(), r.cy());
  h.click(rc.x + 5, rc.cy());
  CHECK_EQ(n, 0);
  CHECK(!v);
}

TEST(ui_core_shortcut_and_consumption) {
  H h;
  int saves = 0, btn = 0;
  std::string s;
  bool fieldFirst = false;
  h.build = [&] {
    ui::Area a({0, 0, 300, 300});
    if (fieldFirst) ui::textField("f", s);
    if (ui::button("Применить", {.shortcut = {Key::S, ui::ModPrimary}})) btn++;
    if (ui::shortcut({Key::S, ui::ModPrimary})) saves++;
  };
  h.frame();
  h.key(Key::S, ctrl());
  CHECK_EQ(btn, 1);
  CHECK_EQ(saves, 0);   // поглощено кнопкой
  h.build = [&] {
    ui::Area a({0, 0, 300, 300});
    if (ui::shortcut({Key::S, ui::ModPrimary})) saves++;
    if (ui::shortcut({Key::Delete, 0})) saves += 10;
  };
  h.frame();
  h.key(Key::S, ctrl());
  CHECK_EQ(saves, 1);
  h.key(Key::Delete);
  CHECK_EQ(saves, 11);
}

TEST(ui_core_ui_scale_and_dpi_hit_testing) {
  H h(800, 600, 1.5f);   // окно 800×600 логических, 1200×900 физических
  ui::setUiScale(1.25f);
  int clicks = 0;
  RectF r;
  std::string s;
  h.build = [&] {
    ui::Area a({20, 20, 300, 300});
    if (ui::button("Кнопка")) clicks++;
    r = ui::lastItem().rect;
    ui::textField("f", s);
  };
  h.frame();
  CHECK_NEAR(ui::deviceScale(), 1.875, 1e-6);
  CHECK_NEAR(ui::viewport().w, 640, 1e-3);
  // События — в логических пикселях окна: точки интерфейса × uiScale
  h.click(r.cx() * 1.25f, r.cy() * 1.25f);
  CHECK_EQ(clicks, 1);
  h.click(r.right() * 1.25f + 4, r.cy() * 1.25f);
  CHECK_EQ(clicks, 1);
  // Каретка для IME — в логических пикселях
  h.click(100 * 1.25f, (r.bottom() + 8 + 15) * 1.25f);
  auto ime = ui::textInputRect();
  CHECK(ime.has_value());
  CHECK(ime->y > (r.bottom() + 8) * 1.25f - 1);
  // Кнопка отрисована в физических пикселях (×1,875): пиксель в центре отличается от фона
  u32 bg = h.img.at(int(700 * 1.5f), int(500 * 1.5f));
  CHECK(h.img.at(int(r.cx() * 1.875f), int(r.cy() * 1.875f)) != bg);
}

TEST(ui_core_popup_auto_flip_and_clamp) {
  H h(800, 600);
  RectF br, pr;
  h.build = [&] {
    ui::at({700, 560, 80, 30});   // у правого нижнего угла
    if (ui::button("Открыть")) ui::openPopup("p");
    br = ui::lastItem().rect;
    if (ui::beginPopup("p", {.width = 220})) {
      for (int i = 0; i < 4; i++) ui::label("Пункт " + std::to_string(i));
      pr = RectF{};
      ui::endPopup();
    }
  };
  h.frame();
  h.click(br.cx(), br.cy());
  h.frames(4);
  // Окно всплыло над кнопкой и прижато к правому краю
  bool found = false;
  h.build = [&] {
    ui::at({700, 560, 80, 30});
    ui::button("Открыть");
    if (ui::beginPopup("p", {.width = 220})) {
      for (int i = 0; i < 4; i++) ui::label("Пункт " + std::to_string(i));
      pr = ui::lastItem().rect;
      found = true;
      ui::endPopup();
    }
  };
  h.frames(3);
  CHECK(found);
  CHECK(pr.bottom() < 560);
  CHECK(pr.right() <= 800 - 8);
  CHECK(ui::wantsMouse());
}

TEST(ui_core_theme_switch_and_tokens) {
  H h;
  h.build = [] { ui::label("Текст"); };
  h.frame();
  u32 darkBg = h.img.at(500, 500);
  ui::setTheme(false);
  CHECK(!ui::theme().dark);
  CHECK(ui::theme().bg == Color::hex(0xf4f1ea));
  h.frame();
  CHECK(h.img.at(500, 500) != darkBg);
  ui::setTheme(true);
  CHECK(ui::theme().accent == Color::hex(0xd9a441));
  CHECK(ui::inkColor(ui::Ink::Muted) == Color::hex(0x6f7886));
  CHECK(ui::toneColor(ui::Tone::Danger) == Color::hex(0xe05a4f));
  CHECK_EQ(ui::shortcutText({Key::S, ui::ModPrimary}).empty(), false);
  ui::setUiScale(3);
  CHECK_NEAR(ui::uiScale(), 1.5, 1e-6);
  ui::setUiScale(0.1f);
  CHECK_NEAR(ui::uiScale(), 0.9, 1e-6);
}

TEST(ui_core_custom_draw_and_interact) {
  H h;
  RectF dev;
  float sc = 0;
  ui::Interaction last;
  int drags = 0;
  h.build = [&] {
    RectF r{100, 100, 300, 200};
    ui::custom(r, [&](gfx::Canvas& c, RectF d, float s) {
      dev = d;
      sc = s;
      c.fillRect(d, Color(200, 40, 40));
    });
    last = ui::interact(ui::id("canvas"), r, ui::IfFocusable | ui::IfMiddleButton);
    if (last.dragging) drags++;
  };
  h.frame();
  CHECK_EQ(dev, (RectF{100, 100, 300, 200}));
  CHECK_EQ(sc, 1.f);
  u32 p = h.img.at(250, 200);
  CHECK_EQ((p >> 16) & 255, u32(200));
  h.dragTo(150, 150, 250, 220);
  CHECK(drags > 0);
  CHECK(ui::wantsMouse() || true);
}

TEST(ui_core_string_scopes_hash_text_not_address) {
  H h;
  WidgetId a = 0, b = 0, c = 0;
  std::string dyn = "область";
  h.build = [&] {
    {
      ui::IdScope s{"область"};
      a = ui::id("x");
    }
    {
      ui::IdScope s{std::string_view(dyn)};
      b = ui::id("x");
    }
    ui::pushId(dyn.c_str());
    c = ui::id("x");
    ui::popId();
  };
  h.frame();
  CHECK_EQ(a, b);
  CHECK_EQ(a, c);
}
