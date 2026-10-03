// Regnum — демонстрация и самопроверка слоя платформы.
//   platform-demo                       — окно: градиент, плитки курсоров, след мыши, эхо ввода в консоль.
//                                         F11 — полный экран, A — анимация, T — тёмная/светлая рамка,
//                                         Ctrl+C/Ctrl+V — буфер обмена, Ctrl+O/Ctrl+S/Ctrl+D — диалоги, Esc — выход.
//   platform-demo --selftest            — проверка настоящего окна синтетическими сообщениями, код возврата 0/1.
//   platform-demo --selftest --with-clipboard — то же плюс буфер обмена (содержимое восстанавливается).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

#include "platform/platform.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

using namespace rg;
using namespace rg::platform;

namespace {

const char* typeName(EventType t) {
  switch (t) {
    case EventType::None: return "None";
    case EventType::MouseMove: return "MouseMove";
    case EventType::MouseDown: return "MouseDown";
    case EventType::MouseUp: return "MouseUp";
    case EventType::MouseWheel: return "MouseWheel";
    case EventType::MouseLeave: return "MouseLeave";
    case EventType::KeyDown: return "KeyDown";
    case EventType::KeyUp: return "KeyUp";
    case EventType::Text: return "Text";
    case EventType::FilesDropped: return "FilesDropped";
    case EventType::Resize: return "Resize";
    case EventType::ScaleChanged: return "ScaleChanged";
    case EventType::FocusIn: return "FocusIn";
    case EventType::FocusOut: return "FocusOut";
    case EventType::Wake: return "Wake";
  }
  return "?";
}

std::string describe(const Event& e) {
  std::string s = typeName(e.type);
  switch (e.type) {
    case EventType::MouseMove:
    case EventType::MouseLeave: s += strf(" %.1f,%.1f", e.x, e.y); break;
    case EventType::MouseDown:
    case EventType::MouseUp: s += strf(" %.1f,%.1f кнопка %d щелчков %d", e.x, e.y, e.button, e.clicks); break;
    case EventType::MouseWheel: s += strf(" %.1f,%.1f dx %.2f dy %.2f%s", e.x, e.y, e.wheelX, e.wheelY, e.precise ? " точно" : ""); break;
    case EventType::KeyDown:
    case EventType::KeyUp: s += " " + shortcutText(e.key, e.mods) + (e.repeat ? " (повтор)" : ""); break;
    case EventType::Text: s += " «" + e.text + "»"; break;
    case EventType::FilesDropped:
      for (auto& f : e.files) s += " " + f;
      break;
    case EventType::Resize:
    case EventType::ScaleChanged: s += strf(" %.1f×%.1f масштаб %.2f", e.width, e.height, e.scale); break;
    default: break;
  }
  return s;
}

constexpr u32 rgb(u32 r, u32 g, u32 b) { return 0xFF000000u | (r << 16) | (g << 8) | b; }

void fillRect(Frame& f, int x, int y, int w, int h, u32 c) {
  int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(f.w, x + w), y1 = std::min(f.h, y + h);
  for (int yy = y0; yy < y1; yy++) std::fill(f.row(yy) + x0, f.row(yy) + x1, c);
}

// Диагональный градиент графитовых тонов темы.
void drawGradient(Frame& f) {
  const int r0 = 0x0e, g0 = 0x11, b0 = 0x17, r1 = 0x24, g1 = 0x2b, b1 = 0x36;
  int span = std::max(1, f.w + f.h);
  for (int y = 0; y < f.h; y++) {
    u32* row = f.row(y);
    for (int x = 0; x < f.w; x++) {
      int t = (x + y) * 256 / span;
      row[x] = rgb(u32(r0 + (r1 - r0) * t / 256), u32(g0 + (g1 - g0) * t / 256), u32(b0 + (b1 - b0) * t / 256));
    }
  }
}

// ================================================================ интерактивная демонстрация
class DemoApp : public App {
 public:
  void onEvent(const Event& e) override {
    if (e.type != EventType::MouseMove) std::printf("%s\n", describe(e).c_str());
    std::fflush(stdout);
    switch (e.type) {
      case EventType::Resize:
      case EventType::ScaleChanged: w_ = e.width; h_ = e.height; break;
      case EventType::MouseMove: {
        mx_ = e.x;
        my_ = e.y;
        trail_.push_back({e.x, e.y});
        if (trail_.size() > 96) trail_.pop_front();
        int tile = int(e.x / 56);
        setCursor(e.y < 40 && tile >= 0 && tile <= int(Cursor::Wait) ? Cursor(tile) : Cursor::Arrow);
        break;
      }
      case EventType::MouseDown: buttons_ |= 1 << e.button; break;
      case EventType::MouseUp: buttons_ &= ~(1 << e.button); break;
      case EventType::MouseLeave: trail_.clear(); break;
      case EventType::KeyDown: onKey(e); break;
      default: break;
    }
    invalidate();
  }

  void onFrame(Frame& f) override {
    double t = time();
    drawGradient(f);
    float s = f.scale;
    for (int i = 0; i <= int(Cursor::Wait); i++)  // плитки курсоров: навести — сменится курсор
      fillRect(f, int((i * 56 + 4) * s), int(4 * s), int(48 * s), int(32 * s), rgb(0x2b + u32(i) * 6, 0x33, 0x40));
    for (int b = 0; b < 5; b++)
      fillRect(f, int((8 + b * 28) * s), int(48 * s), int(24 * s), int(24 * s), (buttons_ >> b) & 1 ? rgb(0xd9, 0xa4, 0x41) : rgb(0x3a, 0x44, 0x54));
    int n = 0;
    for (auto& p : trail_) {
      int sz = int((2 + n++ * 6 / 96) * s);
      fillRect(f, int(p.first * s) - sz / 2, int(p.second * s) - sz / 2, sz, sz, rgb(0xe8, 0xb7, 0x5c));
    }
    if (anim_) {  // вращающаяся полоса
      double a = t * 3;
      float cx = float(f.w) / 2, cy = float(f.h) / 2;
      for (int i = -60; i <= 60; i++) {
        int x = int(cx + std::cos(a) * i * s), y = int(cy + std::sin(a) * i * s);
        fillRect(f, x - int(3 * s), y - int(3 * s), int(6 * s), int(6 * s), rgb(0x4a, 0x9f, 0xe0));
      }
      frames_++;
      if (t - fpsT0_ >= 1) {
        setTitle(strf("Regnum — платформа, %.0f кадров/с", frames_ / (t - fpsT0_)));
        frames_ = 0;
        fpsT0_ = t;
      }
    }
  }

  bool animating() override { return anim_; }
  bool onCloseRequest() override { return true; }

 private:
  void onKey(const Event& e) {
    bool prim = (e.mods & primaryMod()) != 0;
    if (e.key == Key::Escape) quit(0);
    else if (e.key == Key::F11) setFullscreen(!fullscreen());
    else if (e.key == Key::A && !prim) {
      anim_ = !anim_;
      fpsT0_ = time();
      frames_ = 0;
      if (!anim_) setTitle("Regnum — платформа");
    } else if (e.key == Key::T && !prim) setDarkFrame(dark_ = !dark_);
    else if (e.key == Key::C && prim) setClipboardText("Regnum: проверка буфера обмена ✓");
    else if (e.key == Key::V && prim) std::printf("буфер обмена: «%s»\n", clipboardText().c_str());
    else if (e.key == Key::O && prim) {
      auto r = openFileDialog("Открыть мир", {{"Миры Regnum", {"regnum", "zip"}}, {"Все файлы", {}}});
      std::printf("открыть: %s\n", r ? r->c_str() : "(отмена)");
    } else if (e.key == Key::S && prim) {
      auto r = saveFileDialog("Сохранить мир", {{"Мир Regnum", {"regnum"}}}, {}, "Новый мир.regnum");
      std::printf("сохранить: %s\n", r ? r->c_str() : "(отмена)");
    } else if (e.key == Key::D && prim) {
      auto r = pickFolderDialog("Папка мира");
      std::printf("папка: %s\n", r ? r->c_str() : "(отмена)");
    }
  }

  float w_ = 0, h_ = 0, mx_ = 0, my_ = 0;
  int buttons_ = 0;
  bool anim_ = false, dark_ = true;
  int frames_ = 0;
  double fpsT0_ = 0;
  std::deque<std::pair<float, float>> trail_;
};

// ================================================================ самопроверка
class TestApp : public App {
 public:
  std::mutex m;
  std::condition_variable cv;
  std::vector<Event> events;
  std::deque<std::function<void()>> tasks;
  int wakes = 0;
  u64 frames = 0;
  int fw = 0, fh = 0, fstride = 0, resets = 0;
  float fscale = 0;
  std::vector<double> frameTimes;
  std::atomic<bool> animate{false};
  int closeRequests = 0;
  double renderMs = 0;

  void onEvent(const Event& e) override {
    if (e.type == EventType::Wake) {
      std::deque<std::function<void()>> run;
      {
        std::lock_guard<std::mutex> lk(m);
        run.swap(tasks);
      }
      for (auto& t : run) t();
      {
        std::lock_guard<std::mutex> lk(m);
        wakes++;
      }
      cv.notify_all();
      return;
    }
    std::lock_guard<std::mutex> lk(m);
    events.push_back(e);
  }

  void onFrame(Frame& f) override {
    double t0 = time();
    drawGradient(f);
    fillRect(f, 0, 0, 16, 16, rgb(0x12, 0x34, 0x56));  // метка для проверки вывода на экран
    {
      std::lock_guard<std::mutex> lk(m);
      frames++;
      fw = f.w;
      fh = f.h;
      fstride = f.stride;
      fscale = f.scale;
      if (f.reset) resets++;
      frameTimes.push_back(t0);
      renderMs = (time() - t0) * 1000;
    }
    cv.notify_all();
  }

  bool animating() override { return animate; }

  bool onCloseRequest() override {
    std::lock_guard<std::mutex> lk(m);
    closeRequests++;
    return false;
  }
};

class Checker {
 public:
  explicit Checker(TestApp& a) : app(a) {}
  TestApp& app;
  int checks = 0, failures = 0;

  void check(bool ok, const std::string& what) {
    checks++;
    if (!ok) {
      failures++;
      std::printf("  FAIL %s\n", what.c_str());
      std::fflush(stdout);
    }
  }
  void step(const char* name) {
    std::printf("-- %s\n", name);
    std::fflush(stdout);
  }

  bool waitFor(const std::function<bool()>& pred, double seconds = 5) {
    std::unique_lock<std::mutex> lk(app.m);
    return app.cv.wait_for(lk, std::chrono::milliseconds(int(seconds * 1000)), pred);
  }

  // Дождаться обработки всего, что уже в очереди окна (два круга: TranslateMessage добавляет WM_CHAR в конец).
  bool sync() {
    for (int i = 0; i < 2; i++) {
      int w0;
      {
        std::lock_guard<std::mutex> lk(app.m);
        w0 = app.wakes;
      }
      wake();
      if (!waitFor([&] { return app.wakes > w0; })) return false;
    }
    return true;
  }

  // Выполнить в главном потоке (внутри обработчика Wake).
  void onMain(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lk(app.m);
      app.tasks.push_back(std::move(fn));
    }
    check(sync(), "задача главного потока выполнена");
  }

  std::vector<Event> take() {
    sync();
    std::lock_guard<std::mutex> lk(app.m);
    std::vector<Event> r = std::move(app.events);
    app.events.clear();
    return r;
  }

  static std::vector<Event> only(const std::vector<Event>& ev, EventType t) {
    std::vector<Event> r;
    for (auto& e : ev)
      if (e.type == t) r.push_back(e);
    return r;
  }

  u64 frames() {
    std::lock_guard<std::mutex> lk(app.m);
    return app.frames;
  }
};

bool approx(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

// Проверки, общие для всех ОС: кадры, wake/invalidate из потоков, темп анимации.
void genericChecks(Checker& c, std::string& perf) {
  TestApp& app = c.app;
  c.step("первый кадр и Resize");
  c.check(c.waitFor([&] { return app.frames >= 1; }), "первый кадр отрисован");
  {
    auto ev = c.take();
    c.check(!ev.empty() && ev[0].type == EventType::Resize, "первое событие — Resize");
    if (!ev.empty() && ev[0].type == EventType::Resize) {
      c.check(ev[0].width > 0 && ev[0].height > 0, "Resize: ненулевой размер");
      c.check(approx(ev[0].scale, scale()), "Resize: масштаб совпадает с platform::scale()");
      std::lock_guard<std::mutex> lk(app.m);
      c.check(std::abs(app.fw - int(std::lround(ev[0].width * ev[0].scale))) <= 1, "ширина кадра = логическая × масштаб");
      c.check(app.fstride >= app.fw, "stride >= w");
      c.check(app.resets >= 1, "первый кадр помечен reset");
    }
  }

  c.step("invalidate() из фонового потока");
  {
    u64 f0 = c.frames();
    std::thread([] { invalidate(); }).join();
    c.check(c.waitFor([&] { return app.frames > f0; }, 3), "кадр после invalidate() из потока");
  }

  c.step("лавина wake() из 4 потоков");
  {
    int w0;
    {
      std::lock_guard<std::mutex> lk(app.m);
      w0 = app.wakes;
    }
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; i++)
      ts.emplace_back([] {
        for (int k = 0; k < 2000; k++) wake();
      });
    for (auto& t : ts) t.join();
    c.check(c.sync(), "wake() после лавины доходит");
    int got;
    {
      std::lock_guard<std::mutex> lk(app.m);
      got = app.wakes - w0;
    }
    c.check(got >= 1 && got < 8000, "события Wake объединяются (" + std::to_string(got) + " из 8000)");
    perf += strf("wake: 8000 вызовов → %d событий\n", got);
  }

  c.step("анимация ~ частота экрана без занятого цикла");
  {
    u64 f0 = c.frames();
    double t0 = time();
    app.animate = true;
    invalidate();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    app.animate = false;
    double dt = time() - t0;
    c.sync();
    double fps = double(c.frames() - f0) / dt;
    c.check(fps >= 20 && fps <= 400, strf("кадров в секунду при анимации: %.1f", fps));
    perf += strf("анимация: %.1f кадров/с\n", fps);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // кадры после последних событий Wake
    u64 f1 = c.frames();
    size_t e1;
    {
      std::lock_guard<std::mutex> lk(app.m);
      e1 = app.events.size();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    size_t e2;
    {
      std::lock_guard<std::mutex> lk(app.m);
      e2 = app.events.size();
    }
    // Каждый кадр в простое объясняется событием (например, настоящей мышью над окном).
    c.check(c.frames() - f1 <= u64(e2 - e1), strf("без событий, анимации и invalidate кадры не рисуются (%d кадров, %d событий)",
                                                   int(c.frames() - f1), int(e2 - e1)));
  }
}

#ifdef _WIN32
LPARAM xy(int x, int y) { return MAKELPARAM(x, y); }

std::wstring w16(const char* s) { return utf8::toWide(s); }

void win32Checks(Checker& c, bool withClipboard, std::string& perf) {
  TestApp& app = c.app;
  HWND h = static_cast<HWND>(nativeWindow());
  c.check(h != nullptr && IsWindow(h), "nativeWindow() — окно");
  if (!h) return;
  float s = scale();
  auto post = [&](UINT msg, WPARAM wp, LPARAM lp) { PostMessageW(h, msg, wp, lp); };

  c.step("вывод кадра на экран");
  c.onMain([&] {
    HDC dc = GetDC(h);
    COLORREF px = GetPixel(dc, 5, 5);
    ReleaseDC(h, dc);
    c.check(px == RGB(0x12, 0x34, 0x56), strf("пиксель окна (5,5) = метка кадра (получено %06lx)", static_cast<unsigned long>(px)));
  });

  c.step("мышь: координаты, щелчки, кнопки");
  {
    post(WM_MOUSEMOVE, 0, xy(100, 50));
    for (int i = 0; i < 3; i++) {
      post(WM_LBUTTONDOWN, MK_LBUTTON, xy(100, 50));
      post(WM_LBUTTONUP, 0, xy(101, 51));
    }
    post(WM_LBUTTONDOWN, MK_LBUTTON, xy(300, 200));  // далеко — счёт заново
    post(WM_LBUTTONUP, 0, xy(300, 200));
    post(WM_RBUTTONDOWN, MK_RBUTTON, xy(30, 40));
    post(WM_RBUTTONUP, 0, xy(30, 40));
    post(WM_MBUTTONDOWN, MK_MBUTTON, xy(30, 40));
    post(WM_MBUTTONUP, 0, xy(30, 40));
    post(WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON2), xy(30, 40));
    post(WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), xy(30, 40));
    post(WM_LBUTTONUP, 0, xy(30, 40));  // отпускание без нажатия — игнорируется
    auto ev = c.take();
    auto moves = Checker::only(ev, EventType::MouseMove);
    bool found = false;
    for (auto& e : moves)
      if (approx(e.x, 100 / s) && approx(e.y, 50 / s)) found = true;
    c.check(found, "MouseMove в логических координатах");
    auto downs = Checker::only(ev, EventType::MouseDown);
    auto ups = Checker::only(ev, EventType::MouseUp);
    c.check(downs.size() == 7, "7 нажатий (получено " + std::to_string(downs.size()) + ")");
    c.check(ups.size() == 7, "7 отпусканий, лишнее отброшено (получено " + std::to_string(ups.size()) + ")");
    if (downs.size() == 7 && ups.size() == 7) {
      c.check(downs[0].clicks == 1 && downs[1].clicks == 2 && downs[2].clicks == 3, "одинарный, двойной, тройной щелчок");
      c.check(ups[1].clicks == 2, "отпускание несёт число щелчков");
      c.check(downs[3].clicks == 1, "щелчок в другом месте — снова 1");
      c.check(downs[4].button == MouseRight && downs[5].button == MouseMiddle && downs[6].button == MouseForward, "правая, средняя, «вперёд»");
      c.check(approx(downs[4].x, 30 / s) && approx(downs[4].y, 40 / s), "координаты нажатия");
      c.check(approx(ups[0].x, 101 / s), "координаты отпускания");
    }
  }

  c.step("перетаскивание за пределы окна и потеря захвата");
  {
    post(WM_MOUSEMOVE, 0, xy(50, 50));
    post(WM_LBUTTONDOWN, MK_LBUTTON, xy(50, 50));
    post(WM_MOUSEMOVE, MK_LBUTTON, xy(-20, 30));  // при захвате координаты бывают отрицательными
    post(WM_LBUTTONUP, 0, xy(-20, 30));
    auto ev = c.take();
    int iUp = -1, iLeave = -1;
    for (int i = 0; i < int(ev.size()); i++) {
      if (ev[size_t(i)].type == EventType::MouseUp) iUp = i;
      if (ev[size_t(i)].type == EventType::MouseLeave && iUp >= 0 && iLeave < 0) iLeave = i;
    }
    c.check(iUp >= 0 && approx(ev[size_t(iUp)].x, -20 / s), "MouseUp за пределами окна");
    c.check(iLeave > iUp, "MouseLeave после отпускания снаружи");

    post(WM_MOUSEMOVE, 0, xy(60, 60));
    post(WM_RBUTTONDOWN, MK_RBUTTON, xy(60, 60));
    c.sync();
    c.onMain([&] { SendMessageW(h, WM_CAPTURECHANGED, 0, 0); });  // захват отнят (Alt+Tab, системное окно)
    post(WM_RBUTTONUP, 0, xy(60, 60));                          // запоздалое отпускание — уже не нужно
    ev = c.take();
    c.onMain([&] {
      if (GetCapture() == h) ReleaseCapture();  // настоящий захват после имитации снимаем
    });
    auto ups = Checker::only(ev, EventType::MouseUp);
    c.check(ups.size() == 1 && ups[0].button == MouseRight, "потеря захвата отпускает кнопку ровно один раз");
  }

  c.step("Alt+F4 и F10 (без меню окна)");
  {
    post(WM_SYSKEYDOWN, VK_F10, 1 | (0x44 << 16));
    post(WM_SYSKEYUP, VK_F10, 1 | (0x44 << 16) | (1 << 30) | (LPARAM(1) << 31));
    post(WM_KEYDOWN, VK_F6, 1 | (0x40 << 16));  // после F10 клавиши по-прежнему приходят окну
    post(WM_KEYUP, VK_F6, 1 | (0x40 << 16) | (1 << 30) | (LPARAM(1) << 31));
    auto ev = Checker::only(c.take(), EventType::KeyDown);
    bool f10 = false, f6 = false;
    for (auto& e : ev) {
      f10 |= e.key == Key::F10;
      f6 |= e.key == Key::F6;
    }
    c.check(f10 && f6, "F10 не включает меню, следующая клавиша доходит");
    int before;
    {
      std::lock_guard<std::mutex> lk(app.m);
      before = app.closeRequests;
    }
    // Alt+F4 обрабатывает система (DefWindowProc) и только у активного окна; фоновый запуск теста её пропускает.
    bool active = GetForegroundWindow() == h;
    for (int i = 0; i < 5; i++) {
      post(WM_SYSKEYDOWN, VK_F4, 1 | (0x3E << 16) | (1 << 29));  // бит 29 — нажат Alt
      c.take();
    }
    int got;
    {
      std::lock_guard<std::mutex> lk(app.m);
      got = app.closeRequests - before;
    }
    if (active) c.check(got == 5, strf("Alt+F4 спрашивает приложение о закрытии (%d из 5)", got));
    else std::printf("  (окно не активно: Alt+F4 проверяется через SC_CLOSE, системой пропущено %d из 5)\n", 5 - got);
    // Системная команда «Закрыть» (Alt+F4, меню окна) внутри обработчика — запрос откладывается до выхода из него.
    c.onMain([&] { SendMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0); });
    c.sync();
    std::lock_guard<std::mutex> lk(app.m);
    c.check(app.closeRequests == before + got + 1, "SC_CLOSE из обработчика: onCloseRequest после выхода из него");
  }

  c.step("колесо и тачпад");
  {
    POINT p{100, 50};
    ClientToScreen(h, &p);
    post(WM_MOUSEWHEEL, MAKEWPARAM(0, 240), MAKELPARAM(p.x, p.y));
    post(WM_MOUSEHWHEEL, MAKEWPARAM(0, u16(-120)), MAKELPARAM(p.x, p.y));
    auto ev = Checker::only(c.take(), EventType::MouseWheel);
    c.check(ev.size() == 2, "два события колеса");
    if (ev.size() == 2) {
      c.check(approx(ev[0].wheelY, 2) && !ev[0].precise && approx(ev[0].x, 100 / s) && approx(ev[0].y, 50 / s), "2 щелчка вверх, координаты в окне");
      c.check(approx(ev[1].wheelX, -1) && approx(ev[1].wheelY, 0), "горизонтальное колесо влево");
    }
    post(WM_MOUSEWHEEL, MAKEWPARAM(0, 30), MAKELPARAM(p.x, p.y));
    post(WM_MOUSEWHEEL, MAKEWPARAM(0, u16(-120)), MAKELPARAM(p.x, p.y));
    ev = Checker::only(c.take(), EventType::MouseWheel);
    c.check(ev.size() == 2 && ev[0].precise && ev[0].wheelY > 0, "доля щелчка — точная прокрутка в пикселях");
    if (ev.size() == 2) c.check(ev[1].precise && ev[1].wheelY < 0, "жест тачпада продолжается точным");
  }

  c.step("клавиши: виртуальные коды, повтор, Num Enter, Ctrl");
  {
    LPARAM down = 1 | (0x3F << 16), rep = down | (1 << 30), up = down | (1 << 30) | (LPARAM(1) << 31);
    post(WM_KEYDOWN, VK_F5, down);
    post(WM_KEYDOWN, VK_F5, rep);
    post(WM_KEYUP, VK_F5, up);
    post(WM_KEYDOWN, VK_RETURN, 1 | (0x1C << 16) | (1 << 24));
    post(WM_KEYUP, VK_RETURN, 1 | (0x1C << 16) | (1 << 24) | (1 << 30) | (LPARAM(1) << 31));
    post(WM_KEYDOWN, VK_OEM_4, 1 | (0x1A << 16));
    post(WM_KEYUP, VK_OEM_4, 1 | (0x1A << 16) | (1 << 30) | (LPARAM(1) << 31));
    auto ev = c.take();
    auto kd = Checker::only(ev, EventType::KeyDown);
    auto ku = Checker::only(ev, EventType::KeyUp);
    c.check(kd.size() == 4 && ku.size() == 3, "4 нажатия и 3 отпускания");
    if (kd.size() == 4) {
      c.check(kd[0].key == Key::F5 && !kd[0].repeat, "F5");
      c.check(kd[1].key == Key::F5 && kd[1].repeat, "F5 автоповтор");
      c.check(kd[2].key == Key::NumEnter, "Enter цифрового блока");
      c.check(kd[3].key == Key::LBracket, "[ (Х в русской раскладке)");
    }
    c.check(Checker::only(ev, EventType::Text).empty() || Checker::only(ev, EventType::Text)[0].text != "\r", "Enter не даёт текста");

    BYTE saved[256], st[256];
    c.onMain([&] {
      GetKeyboardState(saved);
      std::memcpy(st, saved, sizeof st);
      st[VK_CONTROL] = st[VK_LCONTROL] = 0x80;
      SetKeyboardState(st);
    });
    post(WM_KEYDOWN, 'Z', 1 | (0x2C << 16));
    post(WM_KEYUP, 'Z', 1 | (0x2C << 16) | (1 << 30) | (LPARAM(1) << 31));
    ev = c.take();
    c.onMain([&] { SetKeyboardState(saved); });
    kd = Checker::only(ev, EventType::KeyDown);
    c.check(kd.size() == 1 && kd[0].key == Key::Z && kd[0].mods == ModCtrl, "Ctrl+Z: клавиша Z с модификатором Ctrl");
    c.check(Checker::only(ev, EventType::Text).empty(), "Ctrl+Z не даёт текста");
  }

  c.step("текст: кириллица, суррогатные пары, управляющие символы");
  {
    std::wstring txt = w16("Привет, мир! 😀 ё");
    post(WM_CHAR, 0xDC00, 1);  // одиночная младшая половина — отбрасывается
    for (size_t i = 0; i < txt.size(); i++) {
      post(WM_CHAR, WPARAM(txt[i]), 1);
      if (i == 3) post(WM_CHAR, 0x08, 1);  // Backspace — не текст
    }
    auto ev = Checker::only(c.take(), EventType::Text);
    std::string all;
    for (auto& e : ev) all += e.text;
    c.check(all == "Привет, мир! 😀 ё", "набранный текст: «" + all + "»");
  }

  c.step("фокус: отпускание зажатых клавиш");
  {
    post(WM_KEYDOWN, VK_LEFT, 1 | (0x4B << 16) | (1 << 24));
    c.sync();
    c.onMain([&] { SendMessageW(h, WM_KILLFOCUS, 0, 0); });
    c.onMain([&] { SendMessageW(h, WM_SETFOCUS, 0, 0); });
    auto ev = c.take();
    int iUp = -1, iOut = -1, iIn = -1;
    for (int i = 0; i < int(ev.size()); i++) {
      if (ev[size_t(i)].type == EventType::KeyUp && ev[size_t(i)].key == Key::Left) iUp = i;
      if (ev[size_t(i)].type == EventType::FocusOut) iOut = i;
      if (ev[size_t(i)].type == EventType::FocusIn) iIn = i;
    }
    c.check(iUp >= 0 && iOut > iUp, "KeyUp(←) перед FocusOut");
    c.check(iIn > iOut, "FocusIn после FocusOut");
  }

  c.step("перетаскивание файлов");
  {
    std::wstring list = w16("C:\\Миры\\Арден.regnum");
    list += L'\0';
    list += w16("D:\\карта 😀.png");
    list += L'\0';
    list += L'\0';
    size_t bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GHND, bytes);
    auto* df = static_cast<DROPFILES*>(GlobalLock(mem));
    df->pFiles = sizeof(DROPFILES);
    df->pt = {12, 34};
    df->fNC = FALSE;
    df->fWide = TRUE;
    std::memcpy(reinterpret_cast<char*>(df) + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
    GlobalUnlock(mem);
    post(WM_DROPFILES, reinterpret_cast<WPARAM>(mem), 0);
    auto ev = Checker::only(c.take(), EventType::FilesDropped);
    c.check(ev.size() == 1, "одно событие FilesDropped");
    if (ev.size() == 1) {
      c.check(ev[0].files.size() == 2 && ev[0].files[0] == "C:\\Миры\\Арден.regnum" && ev[0].files[1] == "D:\\карта 😀.png", "пути UTF-8");
      c.check(approx(ev[0].x, 12 / s) && approx(ev[0].y, 34 / s), "точка сброса");
    }
  }

  c.step("изменение размера окна");
  int clientW = 900, clientH = 600;
  {
    c.onMain([&] {
      RECT r{0, 0, clientW, clientH};
      AdjustWindowRectEx(&r, DWORD(GetWindowLongPtrW(h, GWL_STYLE)), FALSE, DWORD(GetWindowLongPtrW(h, GWL_EXSTYLE)));
      SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      RECT rc;
      GetClientRect(h, &rc);
      clientW = rc.right;  // DPI-зависимая рамка может дать другой размер — берём фактический
      clientH = rc.bottom;
    });
    auto ev = Checker::only(c.take(), EventType::Resize);
    c.check(!ev.empty() && approx(ev.back().width, float(clientW) / s) && approx(ev.back().height, float(clientH) / s),
            strf("Resize %d×%d физ.", clientW, clientH));
    c.check(c.waitFor([&] { return app.fw == clientW && app.fh == clientH; }), "кадр нового размера");
    // Как при перетаскивании края: SetWindowPos из другого потока ждёт обработки WM_SIZE,
    // а кадр нового размера должен быть готов к возврату (синхронная отрисовка в WM_SIZE).
    for (int step = 1; step <= 2; step++) {
      int tw = clientW + (step == 1 ? 40 : 0), th = clientH + (step == 1 ? 30 : 0);
      RECT r{0, 0, tw, th};
      AdjustWindowRectEx(&r, DWORD(GetWindowLongPtrW(h, GWL_STYLE)), FALSE, DWORD(GetWindowLongPtrW(h, GWL_EXSTYLE)));
      SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      RECT rc;
      GetClientRect(h, &rc);
      std::lock_guard<std::mutex> lk(app.m);
      c.check(app.fw == rc.right && app.fh == rc.bottom, strf("кадр %d×%d готов сразу после изменения размера (кадр %d×%d)", int(rc.right), int(rc.bottom), app.fw, app.fh));
    }
    c.take();
  }

  c.step("смена DPI (WM_DPICHANGED)");
  {
    UINT dpi0 = UINT(std::lround(s * 96)), dpi2 = dpi0 * 2;
    RECT wr;
    c.onMain([&] {
      GetWindowRect(h, &wr);
      RECT big{wr.left, wr.top, wr.left + (wr.right - wr.left) * 2, wr.top + (wr.bottom - wr.top) * 2};
      SendMessageW(h, WM_DPICHANGED, MAKEWPARAM(dpi2, dpi2), reinterpret_cast<LPARAM>(&big));
    });
    post(WM_MOUSEMOVE, 0, xy(200, 100));
    auto ev = c.take();
    auto sc = Checker::only(ev, EventType::ScaleChanged);
    c.check(sc.size() == 1 && approx(sc[0].scale, float(dpi2) / 96.f), "ScaleChanged с новым масштабом");
    c.check(approx(scale(), float(dpi2) / 96.f), "platform::scale() обновлён");
    bool ok = false;
    for (auto& e : Checker::only(ev, EventType::MouseMove))
      if (approx(e.x, 200.f * 96.f / float(dpi2)) && approx(e.y, 100.f * 96.f / float(dpi2))) ok = true;
    c.check(ok, "координаты мыши пересчитаны по новому масштабу");
    c.check(c.waitFor([&] { return approx(app.fscale, float(dpi2) / 96.f); }), "кадр с новым масштабом");
    c.onMain([&] { SendMessageW(h, WM_DPICHANGED, MAKEWPARAM(dpi0, dpi0), reinterpret_cast<LPARAM>(&wr)); });
    c.take();
    c.check(approx(scale(), s), "масштаб восстановлен");
  }

  c.step("полноэкранный режим");
  {
    float lw = 0, lh = 0;
    c.onMain([&] { setFullscreen(true); });
    auto ev = Checker::only(c.take(), EventType::Resize);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    c.check(fullscreen(), "fullscreen() == true");
    if (!ev.empty()) {
      lw = ev.back().width;
      lh = ev.back().height;
    }
    c.check(approx(lw * s, float(mi.rcMonitor.right - mi.rcMonitor.left), 1) && approx(lh * s, float(mi.rcMonitor.bottom - mi.rcMonitor.top), 1),
            "окно во весь монитор");
    c.onMain([&] { setFullscreen(false); });
    ev = Checker::only(c.take(), EventType::Resize);
    c.check(!fullscreen(), "fullscreen() == false");
    c.check(!ev.empty() && approx(ev.back().width * s, float(clientW), 1) && approx(ev.back().height * s, float(clientH), 1), "прежний размер восстановлен");
  }

  c.step("закрытие окна спрашивает приложение");
  {
    int before;
    {
      std::lock_guard<std::mutex> lk(app.m);
      before = app.closeRequests;
    }
    post(WM_CLOSE, 0, 0);
    c.sync();
    std::lock_guard<std::mutex> lk(app.m);
    c.check(app.closeRequests == before + 1, "onCloseRequest вызван");
    c.check(IsWindow(h) != 0, "окно осталось (приложение отказало)");
  }

  c.step("системные диалоги (IFileOpenDialog/IFileSaveDialog)");
  {
    namespace sfs = std::filesystem;
    sfs::path tmp = sfs::temp_directory_path() / "regnum-platform-selftest";
    std::error_code ec;
    sfs::create_directories(tmp, ec);
    sfs::path existing = tmp / u8"Мир для открытия.regnum";
    { std::ofstream(existing) << "{}"; }
    sfs::path toSave = tmp / u8"Новый мир.regnum";
    sfs::remove(toSave, ec);
    auto u8str = [](const sfs::path& p) {
      auto s = p.u8string();
      return std::string(s.begin(), s.end());
    };
    // Диалог открывается в главном потоке (модально), этот поток находит его по заголовку и нажимает кнопку.
    auto runDialog = [&](const std::string& title, std::function<std::optional<std::string>()> open, WPARAM cmd) {
      std::atomic<bool> done{false};
      std::optional<std::string> result;
      {
        std::lock_guard<std::mutex> lk(app.m);
        app.tasks.push_back([&] {
          result = open();
          done = true;
        });
      }
      wake();
      std::wstring wt = utf8::toWide(title);
      HWND dlg = nullptr;
      for (int i = 0; i < 200 && !dlg; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        dlg = FindWindowW(L"#32770", wt.c_str());
      }
      c.check(dlg != nullptr, "диалог «" + title + "» показан");
      // Диалог заполняет папку и имя асинхронно: кнопку «нажимаем» повторно, пока он не закроется.
      for (int attempt = 0; dlg && attempt < 6 && !done; attempt++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        PostMessageW(dlg, WM_COMMAND, cmd, 0);
        for (int i = 0; i < 40 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
      if (!done && dlg) PostMessageW(dlg, WM_CLOSE, 0, 0);
      for (int i = 0; i < 200 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(25));
      c.check(done, "диалог «" + title + "» закрыт");
      c.sync();
      return result;
    };
    auto same = [&](const std::optional<std::string>& got, const sfs::path& want) {
      if (!got) return false;
      std::error_code e2;
      return sfs::equivalent(sfs::path(utf8::toWide(*got)), want, e2);
    };
    auto r1 = runDialog("Проверка: отмена", [&] { return openFileDialog("Проверка: отмена", {{"Миры", {"regnum"}}}, u8str(tmp)); }, IDCANCEL);
    c.check(!r1, "отмена — nullopt");
    auto r2 = runDialog("Проверка: открыть", [&] { return openFileDialog("Проверка: открыть", {{"Миры", {"regnum"}}, {"Все", {}}}, u8str(existing)); }, IDOK);
    c.check(same(r2, existing), "открыть: путь с кириллицей (" + r2.value_or("нет") + ")");
    auto r3 = runDialog("Проверка: сохранить", [&] { return saveFileDialog("Проверка: сохранить", {{"Мир", {"regnum"}}}, u8str(tmp), "Новый мир"); }, IDOK);
    c.check(r3 && endsWith(*r3, "Новый мир.regnum") && sfs::path(utf8::toWide(*r3)).parent_path() == tmp, "сохранить: имя + расширение по фильтру (" + r3.value_or("нет") + ")");
    auto r4 = runDialog("Проверка: папка", [&] { return pickFolderDialog("Проверка: папка", u8str(tmp)); }, IDOK);
    c.check(same(r4, tmp), "выбор папки (" + r4.value_or("нет") + ")");
    sfs::remove_all(tmp, ec);
  }

  c.step("курсоры, заголовок, IME, тёмная рамка");
  c.onMain([&] {
    for (int i = 0; i <= int(Cursor::Wait); i++) setCursor(Cursor(i));
    setCursor(Cursor::Arrow);
    setTitle("Regnum — самопроверка ✓");
    wchar_t buf[64] = {};
    GetWindowTextW(h, buf, 64);
    c.check(utf8::fromWide(buf) == "Regnum — самопроверка ✓", "заголовок в UTF-16");
    setTextInputRect(RectF(40, 60, 2, 18));
    setDarkFrame(false);
    setDarkFrame(true);
  });

  c.step("простой без нагрузки на процессор");
  {
    auto cpu = [] {
      FILETIME a, b, k, u;
      GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
      auto v = [](FILETIME f) { return double((u64(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e4; };  // мс
      return v(k) + v(u);
    };
    c.sync();
    double c0 = cpu(), t0 = time();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    double ms = cpu() - c0, wall = (time() - t0) * 1000;
    c.check(ms < wall * 0.1, strf("процессор в простое: %.1f мс за %.0f мс", ms, wall));
    perf += strf("простой: %.1f мс процессора за %.0f мс\n", ms, wall);
    app.animate = true;
    invalidate();
    c0 = cpu();
    t0 = time();
    u64 f0 = c.frames();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    app.animate = false;
    ms = cpu() - c0;
    wall = (time() - t0) * 1000;
    double frames = double(c.frames() - f0);
    perf += strf("анимация: %.1f мс процессора за %.0f мс, %.2f мс на кадр (с отрисовкой градиента демо)\n", ms, wall, ms / std::max(1.0, frames));
    c.sync();
  }

  c.step("скорость вывода кадра");
  c.onMain([&] {
    RECT rc;
    GetClientRect(h, &rc);
    int w = rc.right, hh = rc.bottom;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    HDC dc = GetDC(h);
    double t0 = time();
    const int n = 60;
    for (int i = 0; i < n; i++) BitBlt(dc, 0, 0, w, hh, mem, 0, 0, SRCCOPY);
    GdiFlush();
    double ms = (time() - t0) * 1000 / n;
    ReleaseDC(h, dc);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    perf += strf("BitBlt %d×%d: %.3f мс на кадр (масштаб %.2f)\n", w, hh, ms, scale());
    invalidate();
  });

  if (withClipboard) {
    c.step("буфер обмена (UTF-8 ↔ CF_UNICODETEXT)");
    c.onMain([&] {
      // Только если в буфере нет ничего, кроме текста: иначе восстановить содержимое нельзя.
      bool onlyText = true;
      if (OpenClipboard(h)) {
        for (UINT f = EnumClipboardFormats(0); f; f = EnumClipboardFormats(f))
          if (f != CF_UNICODETEXT && f != CF_TEXT && f != CF_OEMTEXT && f != CF_LOCALE) onlyText = false;
        CloseClipboard();
      }
      if (!onlyText) {
        std::printf("  (пропуск: в буфере обмена не только текст)\n");
        return;
      }
      std::string old = clipboardText();
      std::string sample = "Строка 1\nСтрока 2 😀\r\nконец";
      setClipboardText(sample);
      std::string back = clipboardText();
      c.check(back == "Строка 1\nСтрока 2 😀\nконец", "текст вернулся без изменений (переводы строк → \\n)");
      setClipboardText(old);
    });
  }
}
#endif

int selftest(bool withClipboard) {
  TestApp app;
  Checker c(app);
  std::string perf;
  std::filesystem::path placementFs = std::filesystem::temp_directory_path() / "regnum-platform-selftest-window.ini";
  auto placementU8 = placementFs.u8string();
  std::string placement(placementU8.begin(), placementU8.end());  // UTF-8, как все пути Regnum
  std::error_code ec;
  std::filesystem::remove(placementFs, ec);

  WindowConfig cfg;
  cfg.title = "Regnum — самопроверка";
  cfg.width = 800;
  cfg.height = 500;
  cfg.minWidth = 320;
  cfg.minHeight = 240;
  cfg.placementFile = placement;

  std::thread driver([&] {
    genericChecks(c, perf);
#ifdef _WIN32
    win32Checks(c, withClipboard, perf);
#else
    (void)withClipboard;
#endif
    c.step("quit(7) из обработчика");
    {
      std::lock_guard<std::mutex> lk(app.m);
      app.tasks.push_back([] { quit(7); });
    }
    wake();
  });
  int code = -1;
  try {
    code = run(app, cfg);
  } catch (const std::exception& e) {
    c.check(false, std::string("run() бросил исключение: ") + e.what());
  }
  driver.join();
  c.check(code == 7, "run() вернул код из quit() (" + std::to_string(code) + ")");

  // Положение окна сохранено и восстанавливается при следующем запуске.
  c.step("запоминание положения окна и исключение из обработчика");
#ifndef __APPLE__  // macOS хранит рамку окна в NSUserDefaults (setFrameAutosaveName:)
  {
    std::ifstream in(placementFs);
    c.check(bool(in), "файл положения окна создан");
  }
#endif
  struct ThrowApp : App {
    int fw = 0, fh = 0;
    void onEvent(const Event&) override {}
    void onFrame(Frame& f) override {
      fw = f.w;
      fh = f.h;
      throw std::runtime_error("проверка исключения");
    }
    bool animating() override { return false; }
    bool onCloseRequest() override { return true; }
  } thrower;
  bool caught = false;
  try {
    run(thrower, cfg);
  } catch (const std::runtime_error& e) {
    caught = std::string(e.what()) == "проверка исключения";
  }
  c.check(caught, "исключение из onFrame пробрасывается из run()");
#ifdef _WIN32
  c.check(thrower.fw == app.fw && thrower.fh == app.fh,
          strf("размер окна восстановлен: %d×%d (было %d×%d)", thrower.fw, thrower.fh, app.fw, app.fh));
  {
    std::ifstream in(placementFs);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    c.check(all.find("w=") != std::string::npos && all.find("maximized=0") != std::string::npos, "содержимое файла положения");
  }
#endif
  std::filesystem::remove(placementFs, ec);

  std::printf("%s", perf.c_str());
  std::printf("%s самопроверка платформы: проверок %d, ошибок %d\n", c.failures ? "FAILED" : "OK", c.checks, c.failures);
  return c.failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
#endif
  bool test = false, clip = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--selftest") test = true;
    else if (a == "--with-clipboard") clip = true;
  }
  try {
    if (test) return selftest(clip);
    DemoApp app;
    WindowConfig cfg;
    cfg.title = "Regnum — платформа";
    cfg.placementFile = "platform-demo.ini";
    return run(app, cfg);
  } catch (const std::exception& e) {
    showFatal("Regnum", e.what());
    return 2;
  }
}
