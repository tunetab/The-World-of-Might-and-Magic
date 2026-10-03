// Regnum — помощник сценарных тестов приложения: настоящий app::App на headless-платформе.
//
//   apptest::Harness h("my_test");              // временная папка данных пользователя, окно 1440×900
//   h.demo();                                   // демонстрационный мир (map::makeDemoWorld)
//   h.clickUi("topbar.endturn");                // щелчок по элементу оболочки (App::uiRect)
//   h.key(Key::K, apptest::ctrl());             // сочетание
//   h.type("Арден");                            // ввод текста
//   h.shot("editor");                           // снимок .wmma/regnum-tests/app_editor.png
//
// Кадры идут с виртуальным временем (1/60 с на кадр). Щелчок разносится по кадрам интерфейса (step).
#pragma once
#include "app/app_internal.h"
#include "base/fs.h"
#include "codec/png.h"
#include "geo/topo.h"
#include "map/demo_world.h"
#include "tests/test.h"

namespace rg::apptest {

using platform::Key;
namespace hl = platform::headless;

inline u32 ctrl() { return platform::primaryMod(); }

// Чистая временная папка теста (внутри каталога артефактов).
inline std::string tempDir(const std::string& name) {
  std::string d = fs::join(test::outDir(), "app-tmp/" + name);
  fs::removeAll(d);
  fs::makeDirs(d);
  return fs::absolute(d);
}

struct Harness {
  std::string root, dataDir;
  std::unique_ptr<app::App> app;

  explicit Harness(const std::string& name, int w = 1440, int h = 900, float scale = 1, bool dark = true) {
    root = tempDir(name);
    dataDir = tempDir(name + ".data");   // рядом, а не внутри: в папке теста — только миры
    hl::reset();
    hl::configure(w, h, scale);
    ui::shutdown();
    ui::init();
    app::AppConfig cfg;
    cfg.dataDir = dataDir;
    app = std::make_unique<app::App>(cfg);
    app->ui.darkTheme = dark;
    app->impl().browserDir = root;
    hl::attach(app.get());
    frames(2);
  }
  ~Harness() {
    hl::attach(nullptr);
    app.reset();
    ui::shutdown();
    hl::reset();
  }
  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;

  app::App& a() { return *app; }
  app::App* operator->() { return app.get(); }

  // ---- кадры
  void frame() {
    hl::renderFrame();
    hl::advance(1.0 / 60);
  }
  void frames(int n) {
    for (int i = 0; i < n; i++) frame();
  }
  // Доставить события и отложенные действия: кадры, пока интерфейс разбирает очередь, и ещё три.
  void step(int max = 200) {
    frame();
    for (int i = 0; i < max && ui::pendingEvents() > 0; i++) frame();
    frames(3);
  }
  // Кадры, пока приложению нужна перерисовка (анимации камеры, появление окон), не больше max.
  void settle(int max = 240) {
    step();
    for (int i = 0; i < max && (ui::needsRedraw() || app->map().animating()); i++) frame();
  }
  // Дождаться фоновой отрисовки тайлов карты и показать их.
  void waitMap() {
    for (int i = 0; i < 6; i++) {
      app->map().waitIdle(30);
      frames(2);
      if (!app->map().loading()) break;
    }
  }
  // Уведомления оболочки закрыть (снимки без случайных карточек).
  void dropToasts() {
    app->toasts().clear();
    for (int i = 0; i < 40 && ui::toastCount() > 0; i++) {
      hl::advance(1);
      frame();
    }
  }

  // ---- ввод (логические пиксели окна)
  void move(float x, float y) {
    hl::mouseMove(x, y);
    step();
  }
  void click(float x, float y, int button = platform::MouseLeft, u32 mods = 0) {
    hl::click(x, y, button, mods);
    step();
    hl::advance(0.6);   // следующий щелчок не станет двойным
  }
  void doubleClick(float x, float y) {
    hl::doubleClick(x, y);
    step();
    hl::advance(0.6);
  }
  bool clickUi(std::string_view name, int button = platform::MouseLeft) {
    const RectF* r = app->uiRect(name);
    if (!r) return false;
    RectF c = *r;
    click(c.cx(), c.cy(), button);
    return true;
  }
  void drag(float x0, float y0, float x1, float y1, int button = platform::MouseLeft, u32 mods = 0) {
    hl::mouseMove(x0, y0, mods);
    step();
    hl::mouseDown(x0, y0, button, mods);
    step();
    for (int i = 1; i <= 8; i++) {
      hl::mouseMove(x0 + (x1 - x0) * float(i) / 8, y0 + (y1 - y0) * float(i) / 8, mods);
      step();
    }
    hl::mouseUp(x1, y1, button, mods);
    step();
    hl::advance(0.6);
  }
  void wheel(float x, float y, float notches) {
    hl::mouseMove(x, y);
    step();
    hl::wheel(x, y, notches);
    step();
  }
  void key(Key k, u32 mods = 0) {
    hl::press(k, mods);
    step();
  }
  void type(const std::string& s) {
    hl::type(s);
    step();
  }
  // Выделить всё в поле с фокусом и заменить текстом.
  void retype(const std::string& s) {
    key(Key::A, ctrl());
    type(s);
  }

  // ---- мир
  void demo() {
    const map::Basemap* bm = app->basemap();
    if (!bm) test::fail(__FILE__, __LINE__, "базовая карта не найдена");
    app->loadWorld(map::makeDemoWorld(*bm), "Демонстрационный мир");
    settle();
  }
  // Сухопутная провинция с владельцем, точка подписи которой видна в свободной части карты (не под панелями).
  std::optional<std::pair<Id, gfx::Pt>> visibleProvince(Id skip = 0) {
    const World& w = app->world();
    auto fs = geo::faces(w);
    // Точка должна быть свободна и в текущей раскладке (сюда придётся щелчок), и в раскладке с открытым
    // инспектором (мини-карта, масштаб и легенда сдвигаются).
    std::vector<RectF> covers;
    auto collect = [&] {
      for (const char* n : {"legend", "status", "zoom", "minimap", "toolbar", "inspector", "banner", "rail", "drawer", "topbar"})
        if (const RectF* r = app->uiRect(n)) covers.push_back(r->expand(6));
    };
    collect();
    app::Selection keep = app->ui.sel;
    if (!keep) {
      Id any = 0;
      w.provinces.each([&](const Province& p) {
        if (!any && !p.sea) any = p.id;
      });
      app->select(app::SelType::Province, any);
      frames(2);
    }
    struct Restore {
      Harness* h;
      app::Selection s;
      ~Restore() {
        if (!s) {
          h->app->clearSelection();
          h->frames(2);
        }
      }
    } restore{this, keep};
    RectF area = app->mapArea();
    collect();
    std::optional<std::pair<Id, gfx::Pt>> best;
    double bestArea = 0;
    w.provinces.each([&](const Province& p) {
      if (p.sea || !p.owner || p.id == skip) return;
      const geo::ProvinceShape* sh = fs->shape(p.id);
      if (!sh) return;
      gfx::Pt s = app->map().view().toScreen(sh->label);
      if (!area.inset(20).contains(s.x, s.y)) return;
      for (auto& c : covers)
        if (c.contains(s.x, s.y)) return;
      if (app->map().provinceAt(s.x, s.y) != p.id || app->map().armyAt(s.x, s.y) != 0) return;
      if (sh->area > bestArea) {
        bestArea = sh->area;
        best = std::make_pair(p.id, s);
      }
    });
    return best;
  }

  // ---- снимки
  bool shot(const std::string& name) const {
    const platform::Frame& f = hl::lastFrame();
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
        d[3] = 255;
      }
    return codec::writePngFile(test::outDir() + "/app_" + name + ".png", img, 6);
  }
  u32 pixel(float x, float y) const {
    const platform::Frame& f = hl::lastFrame();
    int ix = int(x * f.scale), iy = int(y * f.scale);
    if (ix < 0 || iy < 0 || ix >= f.w || iy >= f.h) return 0;
    return f.row(iy)[ix];
  }
};

}  // namespace rg::apptest
