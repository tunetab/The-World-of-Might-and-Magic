// Тесты слоя платформы: общие помощники (клавиши, щелчки, UTF-16, положение окна) и headless-реализация.
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

#include "codec/png.h"
#include "platform/common.h"
#include "platform/platform.h"
#include "tests/test.h"

using namespace rg;
using namespace rg::platform;
namespace hl = rg::platform::headless;

namespace {

// Приложение-регистратор: записывает события, рисует градиент и прямоугольник под указателем.
struct Recorder : App {
  std::vector<Event> events;
  int frames = 0;
  bool anim = false;
  double animUntil = -1;   // анимация до момента виртуального времени
  bool allowClose = true;
  int closeRequests = 0;
  float mx = -100, my = -100;
  int invalidateFrames = 0;   // сколько кадров подряд просить ещё один кадр
  bool lastReset = false;
  std::vector<double> frameTimes;

  void onEvent(const Event& e) override {
    events.push_back(e);
    if (e.type == EventType::MouseMove) {
      mx = e.x;
      my = e.y;
    }
    invalidate();
  }
  void onFrame(Frame& f) override {
    frames++;
    lastReset = f.reset;
    frameTimes.push_back(platform::time());
    for (int y = 0; y < f.h; y++) {
      u32* row = f.row(y);
      for (int x = 0; x < f.w; x++) {
        u32 r = u32(x * 255 / std::max(1, f.w - 1)), g = u32(y * 255 / std::max(1, f.h - 1));
        row[x] = 0xFF000000u | (r << 16) | (g << 8) | 0x40u;
      }
    }
    int cx = int(mx * f.scale), cy = int(my * f.scale), half = int(6 * f.scale);
    for (int y = std::max(0, cy - half); y < std::min(f.h, cy + half); y++)
      for (int x = std::max(0, cx - half); x < std::min(f.w, cx + half); x++) f.row(y)[x] = 0xFFFFFFFFu;
    if (invalidateFrames > 0) {
      invalidateFrames--;
      invalidate();
    }
  }
  bool animating() override { return anim || platform::time() < animUntil; }
  bool onCloseRequest() override {
    closeRequests++;
    return allowClose;
  }

  std::vector<Event> of(EventType t) const {
    std::vector<Event> r;
    for (auto& e : events)
      if (e.type == t) r.push_back(e);
    return r;
  }
};

void savePng(const Frame& f, const std::string& name) {
  codec::RgbaImage img;
  img.w = f.w;
  img.h = f.h;
  img.rgba.resize(size_t(f.w) * size_t(f.h) * 4);
  for (int y = 0; y < f.h; y++)
    for (int x = 0; x < f.w; x++) {
      u32 p = f.row(y)[x];
      u8* d = &img.rgba[(size_t(y) * size_t(f.w) + size_t(x)) * 4];
      d[0] = u8(p >> 16);
      d[1] = u8(p >> 8);
      d[2] = u8(p);
      d[3] = u8(p >> 24);
    }
  CHECK(codec::writePngFile(test::outDir() + "/" + name, img));
}

}  // namespace

// ================================================================ общие помощники
TEST(app_platform_key_names) {
  CHECK_EQ(keyName(Key::A), std::string("A"));
  CHECK_EQ(keyName(Key::Z), std::string("Z"));
  CHECK_EQ(keyName(Key::D7), std::string("7"));
  CHECK_EQ(keyName(Key::F12), std::string("F12"));
  CHECK_EQ(keyName(Key::NumPad3), std::string("Num 3"));
  CHECK_EQ(keyName(Key::Space), std::string("Пробел"));
  CHECK_EQ(keyName(Key::Left), std::string("←"));
  for (int k = 1; k < int(Key::Count); k++) CHECK_MSG(!keyName(Key(k)).empty(), "нет названия клавиши " + std::to_string(k));
  CHECK(keyName(Key::Unknown).empty());
#ifdef __APPLE__
  CHECK_EQ(primaryMod(), u32(ModSuper));
  CHECK_EQ(shortcutText(Key::S, ModSuper | ModShift), std::string("⇧⌘S"));
  CHECK_EQ(shortcutText(Key::Z, ModCtrl | ModAlt), std::string("⌃⌥Z"));
  CHECK_EQ(keyName(Key::Backspace), std::string("⌫"));
#else
  CHECK_EQ(primaryMod(), u32(ModCtrl));
  CHECK_EQ(shortcutText(Key::S, ModCtrl | ModShift), std::string("Ctrl+Shift+S"));
  CHECK_EQ(shortcutText(Key::Delete, ModAlt), std::string("Alt+Del"));
  CHECK_EQ(shortcutText(Key::F5, 0), std::string("F5"));
  CHECK_EQ(shortcutText(Key::Ctrl, ModCtrl), std::string("Ctrl"));  // сама клавиша-модификатор не дублируется
  CHECK_EQ(shortcutText(Key::Unknown, ModCtrl | ModAlt), std::string("Ctrl+Alt"));
  CHECK_EQ(shortcutText(Key::K, primaryMod()), std::string("Ctrl+K"));
#endif
}

TEST(app_platform_click_counter) {
  detail::ClickCounter c;
  CHECK_EQ(c.press(0, 10, 10, 1.0, 0.5, 4), 1);
  CHECK_EQ(c.press(0, 11, 12, 1.2, 0.5, 4), 2);
  CHECK_EQ(c.press(0, 12, 10, 1.4, 0.5, 4), 3);
  CHECK_EQ(c.press(0, 12, 10, 1.6, 0.5, 4), 4);
  CHECK_EQ(c.press(0, 12, 10, 2.2, 0.5, 4), 1);    // пауза больше интервала
  CHECK_EQ(c.press(0, 30, 10, 2.3, 0.5, 4), 1);    // далеко
  CHECK_EQ(c.press(1, 30, 10, 2.4, 0.5, 4), 1);    // другая кнопка
  CHECK_EQ(c.press(1, 30, 10, 2.5, 0.5, 4), 2);
  CHECK_EQ(c.press(1, 30, 10, 2.4, 0.5, 4), 1);    // время пошло назад — не цепочка
  c.reset();
  CHECK_EQ(c.press(1, 30, 10, 2.45, 0.5, 4), 1);
}

TEST(app_platform_utf16_input) {
  detail::Utf16Input in;
  std::string out;
  for (char16_t u : std::u16string(u"Ёж 😀\x0001\x007F\x0085!")) out += in.push(u16(u));
  CHECK_EQ(out, std::string("Ёж 😀!"));
  // Одиночные половины суррогатной пары не дают текста и не ломают следующий символ.
  CHECK(in.push(0xDC00).empty());
  CHECK(in.push(0xD83D).empty());
  CHECK_EQ(in.push(u16('a')), std::string("a"));
  CHECK(in.push(0xD83D).empty());
  CHECK_EQ(in.push(0xDE80), std::string("🚀"));
  CHECK(in.push(0x0D).empty());
  CHECK(in.push(0x09).empty());
  CHECK_EQ(in.push(0x00A0), std::string("\xC2\xA0"));
}

TEST(app_platform_text_helpers) {
  CHECK_EQ(detail::filterText("a\tб\r\nв\x7F\xC2\x85г"), std::string("aбвг"));
  CHECK_EQ(detail::toCrlf("a\nb\r\nc\rd"), std::string("a\r\nb\r\nc\r\nd"));
  CHECK_EQ(detail::fromCrlf("a\r\nb\rc\nd\r\n"), std::string("a\nb\nc\nd\n"));
  CHECK_EQ(detail::fromCrlf(detail::toCrlf("x\ny\n\nz")), std::string("x\ny\n\nz"));
  CHECK(detail::isSafeUrl("https://example.com/путь?q=1"));
  CHECK(detail::isSafeUrl("HTTP://EXAMPLE.COM"));
  CHECK(detail::isSafeUrl("mailto:user@example.com"));
  CHECK(!detail::isSafeUrl("file:///C:/Windows/system32/calc.exe"));
  CHECK(!detail::isSafeUrl("C:\\Windows\\notepad.exe"));
  CHECK(!detail::isSafeUrl("javascript:alert(1)"));
  CHECK(!detail::isSafeUrl("https://"));
  CHECK(!detail::isSafeUrl("https://a.b/\nrm -rf"));
  CHECK(!detail::isSafeUrl(""));
}

TEST(app_platform_placement_file) {
  std::string dir = test::outDir() + "/platform-placement";
  std::string path = dir + "/окно.ini";
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(dir.c_str()))), ec);
  CHECK(!detail::loadPlacement(path));
  detail::Placement p;
  p.x = -1910;
  p.y = 40;
  p.w = 1600;
  p.h = 900;
  p.maximized = true;
  p.dpi = 144;
  CHECK(detail::savePlacement(path, p));  // каталог создаётся
  auto q = detail::loadPlacement(path);
  CHECK(q.has_value());
  CHECK_EQ(q->x, -1910);
  CHECK_EQ(q->y, 40);
  CHECK_EQ(q->w, 1600);
  CHECK_EQ(q->h, 900);
  CHECK(q->maximized);
  CHECK_EQ(q->dpi, 144);
  p.maximized = false;
  CHECK(detail::savePlacement(path, p));  // перезапись
  CHECK(!detail::loadPlacement(path)->maximized);

  auto writeRaw = [&](const std::string& text) {
    std::ofstream out(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.c_str()))), std::ios::binary | std::ios::trunc);
    out << text;
  };
  writeRaw("x=1\ny=2\nw=3\n");  // нет высоты
  CHECK(!detail::loadPlacement(path));
  writeRaw("x=1\ny=2\nw=30\nh=40\n");  // слишком маленькое
  CHECK(!detail::loadPlacement(path));
  writeRaw("x=1\ny=2\nw=800\nh=600\ndpi=9000\n");  // нелепый DPI
  CHECK(!detail::loadPlacement(path));
  writeRaw("мусор\n\nx = 5\n y=6\nw=800\nh=600\n# комментарий\n");
  auto r = detail::loadPlacement(path);
  CHECK(r.has_value());
  CHECK_EQ(r->x, 5);
  CHECK_EQ(r->y, 6);
  CHECK_EQ(r->dpi, 96);

  WindowConfig cfg;
  CHECK_EQ(detail::placementPath(cfg, "/home/u/.config/regnum"), std::string("/home/u/.config/regnum/window.ini"));
  CHECK_EQ(detail::placementPath(cfg, "C:\\Users\\u\\AppData\\Roaming\\Regnum\\"), std::string("C:\\Users\\u\\AppData\\Roaming\\Regnum\\window.ini"));
  CHECK(detail::placementPath(cfg, "").empty());
  cfg.placementFile.clear();
  CHECK(detail::placementPath(cfg, "/tmp").empty());
  auto abs = std::filesystem::absolute(std::filesystem::path("abs.ini")).u8string();
  cfg.placementFile.assign(abs.begin(), abs.end());
  CHECK_EQ(detail::placementPath(cfg, "/x"), cfg.placementFile);
}

// ================================================================ headless
TEST(app_platform_headless_attach_and_frames) {
  hl::reset();
  hl::configure(320, 200, 1.5f);
  Recorder app;
  hl::attach(&app);
  CHECK(hl::attached() == &app);
  CHECK(hl::needsFrame());
  Frame& f = hl::renderFrame();
  CHECK_EQ(app.events.size(), size_t(1));
  CHECK(app.events[0].type == EventType::Resize);
  CHECK_NEAR(app.events[0].width, 320, 1e-4);
  CHECK_NEAR(app.events[0].height, 200, 1e-4);
  CHECK_NEAR(app.events[0].scale, 1.5, 1e-6);
  CHECK_EQ(f.w, 480);
  CHECK_EQ(f.h, 300);
  CHECK_EQ(f.stride, 480);
  CHECK_NEAR(f.logicalW(), 320, 1e-4);
  CHECK(f.reset);
  CHECK_EQ(f.index, u64(0));
  CHECK_NEAR(platform::scale(), 1.5, 1e-6);
  // onEvent вызвал invalidate — но кадр уже отрисован после событий, новый не нужен.
  CHECK(!hl::needsFrame());
  hl::mouseMove(100, 50);
  CHECK(hl::needsFrame());
  Frame& f2 = hl::renderFrame();
  CHECK(!f2.reset);
  CHECK_EQ(f2.index, u64(1));
  CHECK_EQ(hl::frames(), u64(2));
  CHECK_EQ(f2.row(75)[150], 0xFFFFFFFFu);  // прямоугольник под указателем: 100×1,5 = 150
  CHECK(f2.row(0)[0] != 0xFFFFFFFFu);
  savePng(f2, "app_platform_headless.png");
  // Смена размера: новый буфер, Resize и ScaleChanged.
  app.events.clear();
  hl::configure(200, 100, 2);
  Frame& f3 = hl::renderFrame();
  CHECK_EQ(f3.w, 400);
  CHECK_EQ(f3.h, 200);
  CHECK(f3.reset);
  CHECK_EQ(app.events.size(), size_t(2));
  CHECK(app.events[0].type == EventType::Resize);
  CHECK(app.events[1].type == EventType::ScaleChanged);
  CHECK_NEAR(app.events[1].scale, 2, 1e-6);
  hl::attach(nullptr);
  CHECK(!hl::needsFrame());
  CHECK_THROWS(hl::renderFrame());
}

TEST(app_platform_headless_mouse) {
  hl::reset();
  Recorder app;
  hl::attach(&app);
  hl::mouseDown(10, 10);
  hl::mouseUp(10, 10);
  hl::mouseDown(11, 9);
  hl::mouseUp(11, 9);
  hl::mouseDown(11, 9);
  hl::mouseUp(11, 9);
  hl::advance(1.0);
  hl::mouseDown(11, 9);
  hl::pump();
  auto downs = app.of(EventType::MouseDown);
  auto ups = app.of(EventType::MouseUp);
  CHECK_EQ(downs.size(), size_t(4));
  CHECK_EQ(downs[0].clicks, 1);
  CHECK_EQ(downs[1].clicks, 2);
  CHECK_EQ(downs[2].clicks, 3);
  CHECK_EQ(downs[3].clicks, 1);  // прошла секунда
  CHECK_EQ(ups[1].clicks, 2);

  // click() всегда одинарный, doubleClick() — 1 и 2.
  app.events.clear();
  hl::click(50, 60, MouseRight, ModShift);
  hl::click(50, 60, MouseRight, ModShift);
  hl::doubleClick(70, 80);
  hl::pump();
  downs = app.of(EventType::MouseDown);
  CHECK_EQ(downs.size(), size_t(4));
  CHECK(downs[0].clicks == 1 && downs[1].clicks == 1);
  CHECK(downs[0].button == MouseRight && downs[0].mods == ModShift);
  CHECK(downs[2].clicks == 1 && downs[3].clicks == 2);
  CHECK_NEAR(downs[3].x, 70, 1e-6);
  CHECK_EQ(app.of(EventType::MouseMove).size(), size_t(3));

  // Перетаскивание: перемещение, нажатие, шаги, отпускание в конечной точке.
  app.events.clear();
  hl::drag(0, 0, 100, 50, 4);
  hl::pump();
  CHECK_EQ(app.events.size(), size_t(7));
  CHECK(app.events[0].type == EventType::MouseMove);
  CHECK(app.events[1].type == EventType::MouseDown);
  CHECK(app.events[5].type == EventType::MouseMove);
  CHECK_NEAR(app.events[5].x, 100, 1e-5);
  CHECK_NEAR(app.events[3].x, 50, 1e-5);
  CHECK(app.events[6].type == EventType::MouseUp);
  CHECK_NEAR(app.events[6].y, 50, 1e-5);

  app.events.clear();
  hl::wheel(5, 6, -2.5f, 1, true, ModCtrl);
  hl::pump();
  CHECK_EQ(app.events.size(), size_t(1));
  CHECK(app.events[0].type == EventType::MouseWheel);
  CHECK_NEAR(app.events[0].wheelY, -2.5, 1e-6);
  CHECK_NEAR(app.events[0].wheelX, 1, 1e-6);
  CHECK(app.events[0].precise);
  CHECK_EQ(app.events[0].mods, u32(ModCtrl));
  hl::attach(nullptr);
}

TEST(app_platform_headless_keyboard_and_focus) {
  hl::reset();
  Recorder app;
  hl::attach(&app);
  hl::pump();
  app.events.clear();
  hl::press(Key::Z, primaryMod());
  hl::keyDown(Key::Left);
  hl::keyDown(Key::Left, 0, true);
  hl::keyDown(Key::Shift, ModShift);
  hl::type("При\x01вет 😀\n");
  hl::type("\t\r");  // только управляющие — события нет
  hl::setFocus(false);
  hl::setFocus(true);
  hl::pump();
  std::vector<EventType> types;
  for (auto& e : app.events) types.push_back(e.type);
  std::vector<EventType> want = {EventType::KeyDown, EventType::KeyUp, EventType::KeyDown, EventType::KeyDown, EventType::KeyDown,
                                 EventType::Text,    EventType::KeyUp, EventType::KeyUp,   EventType::FocusOut, EventType::FocusIn};
  CHECK(types == want);
  CHECK(app.events[0].key == Key::Z && app.events[0].mods == primaryMod());
  CHECK(!app.events[2].repeat && app.events[3].repeat);
  CHECK_EQ(app.events[5].text, std::string("Привет 😀"));
  // При потере фокуса отпускаются ровно зажатые клавиши (Left и Shift), Z уже отпущена.
  CHECK(app.events[6].key == Key::Left || app.events[6].key == Key::Shift);
  CHECK(app.events[7].key == Key::Left || app.events[7].key == Key::Shift);
  CHECK(app.events[6].key != app.events[7].key);
  hl::attach(nullptr);
}

TEST(app_platform_headless_wake_from_threads) {
  hl::reset();
  Recorder app;
  hl::attach(&app);
  hl::renderFrame();
  CHECK(!hl::needsFrame());
  std::vector<std::thread> ts;
  for (int i = 0; i < 4; i++)
    ts.emplace_back([] {
      for (int k = 0; k < 5000; k++) {
        wake();
        invalidate();
        (void)platform::time();
      }
    });
  for (auto& t : ts) t.join();
  CHECK(hl::needsFrame());
  app.events.clear();
  hl::renderFrame();
  CHECK_EQ(app.of(EventType::Wake).size(), size_t(1));  // 20 000 вызовов объединены в одно событие
  wake();
  hl::pump();
  CHECK_EQ(app.of(EventType::Wake).size(), size_t(2));  // после доставки — снова доходит
  // Событие из фонового потока через post().
  std::thread([] {
    Event e;
    e.type = EventType::Text;
    e.text = "из потока";
    hl::post(e);
  }).join();
  hl::pump();
  CHECK_EQ(app.events.back().text, std::string("из потока"));
  hl::attach(nullptr);
}

TEST(app_platform_headless_settle_and_virtual_time) {
  hl::reset();
  Recorder app;
  hl::attach(&app);
  CHECK_NEAR(platform::time(), 0, 1e-12);
  app.animUntil = 0.5;  // анимация полсекунды виртуального времени
  int n = hl::settle();
  CHECK(n >= 30 && n <= 32);
  CHECK(platform::time() >= 0.5 - 1e-9);
  CHECK(!hl::needsFrame());
  CHECK_NEAR(app.frameTimes[1] - app.frameTimes[0], 1.0 / 60, 1e-9);
  // Приложение, просящее кадры бесконечно, ограничено maxFrames.
  app.anim = true;
  CHECK_EQ(hl::settle(25), 25);
  app.anim = false;
  // Цепочка invalidate из кадра: ровно столько кадров, сколько просили, время не идёт.
  double t = platform::time();
  hl::renderFrame();
  app.invalidateFrames = 3;
  invalidate();
  CHECK_EQ(hl::settle(), 4);
  CHECK_NEAR(platform::time(), t, 1e-12);
  hl::advance(-5);  // назад время не идёт
  CHECK_NEAR(platform::time(), t, 1e-12);
  hl::attach(nullptr);
}

TEST(app_platform_headless_run_and_quit) {
  hl::reset();
  hl::configure(200, 120, 1);
  struct QuitApp : Recorder {
    void onEvent(const Event& e) override {
      Recorder::onEvent(e);
      if (e.type == EventType::KeyDown && e.key == Key::Escape) quit(3);
    }
  } app;
  // Пустая очередь: run рисует первый кадр и возвращается.
  WindowConfig cfg;
  cfg.title = "Проверка";
  CHECK_EQ(run(app, cfg), 0);
  CHECK_EQ(app.frames, 1);
  CHECK_EQ(hl::title(), std::string("Проверка"));
  CHECK(hl::attached() == nullptr);
  CHECK_EQ(hl::lastFrame().w, 200);
  // Анимация продвигает виртуальное время, quit() из обработчика завершает цикл с кодом.
  app.animUntil = platform::time() + 0.25;
  hl::press(Key::A);
  CHECK_EQ(run(app, cfg), 0);
  CHECK(app.frames >= 15);
  hl::press(Key::Escape);
  hl::press(Key::B);
  int frames = app.frames;
  CHECK_EQ(run(app, cfg), 3);
  CHECK(hl::quitRequested());
  CHECK_EQ(hl::exitCode(), 3);
  CHECK_EQ(app.frames, frames);  // кадр после quit не рисуется
  // Исключение из обработчика доходит до вызывающего.
  struct Thrower : Recorder {
    void onFrame(Frame&) override { throw std::runtime_error("сбой кадра"); }
  } bad;
  CHECK_THROWS(run(bad, cfg));
}

TEST(app_platform_headless_services) {
  hl::reset();
  // Буфер обмена.
  CHECK(clipboardText().empty());
  setClipboardText("Строка 1\nСтрока 2 😀");
  CHECK_EQ(clipboardText(), std::string("Строка 1\nСтрока 2 😀"));
  // Диалоги: ответы по порядку, без ответа — отмена.
  CHECK(dialogsSupported());
  hl::queueDialogResult(std::string("/миры/Арден.regnum"));
  hl::queueDialogResult(std::nullopt);
  hl::queueDialogResult(std::string("/миры"));
  auto a = openFileDialog("Открыть мир", {{"Миры", {"regnum", "zip"}}}, "/миры");
  auto b = saveFileDialog("Сохранить", {}, {}, "Новый.regnum");
  auto c = pickFolderDialog("Папка");
  auto d = openFileDialog("Ещё раз");
  CHECK(a && *a == "/миры/Арден.regnum");
  CHECK(!b);
  CHECK(c && *c == "/миры");
  CHECK(!d);
  const auto& calls = hl::dialogCalls();
  CHECK_EQ(calls.size(), size_t(4));
  CHECK(calls[0].kind == hl::DialogCall::Open && calls[0].title == "Открыть мир" && calls[0].startDir == "/миры");
  CHECK(calls[0].filters.size() == 1 && calls[0].filters[0].exts.size() == 2);
  CHECK(calls[1].kind == hl::DialogCall::Save && calls[1].defaultName == "Новый.regnum");
  CHECK(calls[2].kind == hl::DialogCall::Folder);
  hl::setDialogsSupported(false);
  hl::queueDialogResult(std::string("/x"));
  CHECK(!dialogsSupported());
  CHECK(!openFileDialog("Нельзя"));
  // Открытие путей и ссылок: опасные схемы отклоняются.
  CHECK(openPath("/миры/Арден"));
  CHECK(!openPath(""));
  CHECK(openUrl("https://example.com"));
  CHECK(!openUrl("file:///etc/passwd"));
  CHECK_EQ(hl::opened().size(), size_t(2));
  CHECK_EQ(hl::opened()[1], std::string("https://example.com"));
  // Окно: курсор, заголовок, полный экран, рамка, каретка IME.
  setCursor(Cursor::Grabbing);
  CHECK(hl::cursor() == Cursor::Grabbing);
  setTitle("Мир — Арден*");
  CHECK_EQ(hl::title(), std::string("Мир — Арден*"));
  CHECK(!fullscreen());
  setFullscreen(true);
  CHECK(fullscreen());
  setDarkFrame(false);
  CHECK(!hl::darkFrame());
  setTextInputRect(RectF(10, 20, 2, 16));
  CHECK(hl::textInputRect() == RectF(10, 20, 2, 16));
  CHECK(nativeWindow() == nullptr);
  showFatal("Ошибка", "Нет шрифта с кириллицей");
  CHECK_EQ(hl::fatalMessages().size(), size_t(1));
  CHECK_EQ(hl::fatalMessages()[0], std::string("Ошибка: Нет шрифта с кириллицей"));
  // reset возвращает всё к умолчаниям.
  hl::reset();
  CHECK(clipboardText().empty());
  CHECK(hl::dialogCalls().empty());
  CHECK(hl::cursor() == Cursor::Arrow);
  CHECK(!fullscreen());
  CHECK(dialogsSupported());
}

TEST(app_platform_headless_perf) {
  // Кадр 1920×1080 при масштабе 2 (3840×2160): накладные расходы платформы — только вызов onFrame.
  hl::reset();
  hl::configure(1920, 1080, 2);
  struct Fill : App {
    void onEvent(const Event&) override {}
    void onFrame(Frame& f) override { f.px[0] = 0xFF000000u; }
    bool animating() override { return false; }
    bool onCloseRequest() override { return true; }
  } app;
  hl::attach(&app);
  hl::renderFrame();
  double t0 = nowSeconds();
  for (int i = 0; i < 1000; i++) {
    hl::mouseMove(float(i % 300), 5);
    hl::renderFrame();
  }
  double us = (nowSeconds() - t0) * 1e6 / 1000;
  CHECK_EQ(hl::lastFrame().w, 3840);
  CHECK_MSG(us < 200, strf("кадр headless: %.1f мкс", us));
  hl::attach(nullptr);
  hl::reset();
}
