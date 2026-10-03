// Regnum — помощник тестов интерфейса: кадры без окна, синтетические события, сохранение PNG.
#pragma once
#include "codec/png.h"
#include "gfx/canvas.h"
#include "tests/test.h"
#include "ui/ui.h"

namespace rg::uitest {

using platform::Event;
using platform::EventType;
using platform::Key;

struct H {
  float w, h, scale;
  double t = 1;
  gfx::Image img;
  std::string clip;
  std::function<void()> build;

  explicit H(float w_ = 800, float h_ = 600, float s = 1, bool dark = true) : w(w_), h(h_), scale(s), img(int(w_ * s), int(h_ * s)) {
    ui::shutdown();
    ui::init();
    ui::setTheme(dark);
    ui::setUiScale(1);
    ui::setClipboard([this] { return clip; }, [this](const std::string& s) { clip = s; });
  }
  ~H() { ui::shutdown(); }
  H(const H&) = delete;
  H& operator=(const H&) = delete;

  void frame() {
    ui::beginFrame(w, h, scale, t);
    if (build) build();
    gfx::Canvas c(img);
    c.clear(ui::theme().bg);
    ui::endFrame(c);
    t += 1.0 / 60;
  }
  void frames(int n) {
    for (int i = 0; i < n; i++) frame();
  }
  // Разобрать очередь событий (по кадрам) и сделать ещё один кадр.
  void drain(int max = 200) {
    frame();
    for (int i = 0; ui::pendingEvents() > 0 && i < max; i++) frame();
    frame();
  }
  // Кадры, пока интерфейс просит перерисовку (анимации), не больше max.
  int settle(int max = 240) {
    drain();
    int i = 0;
    while (ui::needsRedraw() && i < max) {
      frame();
      i++;
    }
    return i;
  }
  void wait(double sec) {
    int n = int(sec * 60 + 0.5);
    frames(std::max(1, n));
  }

  void post(const Event& e) { ui::onEvent(e); }
  void move(float x, float y, u32 mods = 0) {
    Event e;
    e.type = EventType::MouseMove;
    e.x = x;
    e.y = y;
    e.mods = mods;
    post(e);
  }
  void down(float x, float y, int b = 0, int clicks = 1, u32 mods = 0) {
    Event e;
    e.type = EventType::MouseDown;
    e.x = x;
    e.y = y;
    e.button = b;
    e.clicks = clicks;
    e.mods = mods;
    post(e);
  }
  void up(float x, float y, int b = 0, int clicks = 1, u32 mods = 0) {
    Event e;
    e.type = EventType::MouseUp;
    e.x = x;
    e.y = y;
    e.button = b;
    e.clicks = clicks;
    e.mods = mods;
    post(e);
  }
  void click(float x, float y, int b = 0, u32 mods = 0) {
    move(x, y, mods);
    down(x, y, b, 1, mods);
    up(x, y, b, 1, mods);
    drain();
  }
  void multiClick(float x, float y, int n) {
    move(x, y);
    for (int i = 1; i <= n; i++) {
      down(x, y, 0, i);
      up(x, y, 0, i);
    }
    drain();
  }
  void dragTo(float x0, float y0, float x1, float y1, int steps = 8, u32 mods = 0) {
    move(x0, y0, mods);
    down(x0, y0, 0, 1, mods);
    drain();
    for (int i = 1; i <= steps; i++) {
      move(x0 + (x1 - x0) * float(i) / float(steps), y0 + (y1 - y0) * float(i) / float(steps), mods);
      drain();
    }
    up(x1, y1, 0, 1, mods);
    drain();
  }
  void key(Key k, u32 mods = 0) {
    Event e;
    e.type = EventType::KeyDown;
    e.key = k;
    e.mods = mods;
    post(e);
    e.type = EventType::KeyUp;
    post(e);
    drain();
  }
  void type(const std::string& s) {
    Event e;
    e.type = EventType::Text;
    e.text = s;
    post(e);
    drain();
  }
  void wheel(float x, float y, float dy, bool precise = false) {
    move(x, y);
    Event e;
    e.type = EventType::MouseWheel;
    e.x = x;
    e.y = y;
    e.wheelY = dy;
    e.precise = precise;
    post(e);
    drain();
  }
  bool save(const std::string& name) const {
    codec::RgbaImage out;
    out.w = img.w;
    out.h = img.h;
    out.rgba = img.toRgba();
    return codec::writePngFile(test::outDir() + "/ui_" + name + ".png", out, 6);
  }
  u32 px(float x, float y) const {
    int ix = int(x * scale), iy = int(y * scale);
    if (ix < 0 || iy < 0 || ix >= img.w || iy >= img.h) return 0;
    return img.at(ix, iy);
  }
};

inline u32 ctrl() { return platform::primaryMod(); }

}  // namespace rg::uitest
