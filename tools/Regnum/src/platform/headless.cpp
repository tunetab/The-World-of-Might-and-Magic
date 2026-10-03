// Regnum — платформа без окна: кадры в память, сценарии ввода, виртуальное время (тесты и CLI).
#include <atomic>
#include <deque>
#include <mutex>

#include "platform/common.h"
#include "platform/platform.h"

namespace rg::platform {
namespace {

constexpr double kDoubleClickTime = 0.5;   // как по умолчанию в Windows
constexpr float kDoubleClickSlop = 4;      // логические пиксели

struct Headless {
  std::mutex m;                 // очередь событий и флаг Wake (wake() — из любого потока)
  std::deque<Event> queue;
  bool wakeQueued = false;
  std::atomic<bool> dirty{true};
  std::atomic<double> now{0};

  App* app = nullptr;
  int w = 1280, h = 800;
  float scale = 1;
  std::vector<u32> px;
  Frame frame;
  bool reset = true;
  u64 frames = 0;

  detail::ClickCounter clicks;
  std::array<int, 5> lastClicks{};
  std::array<bool, size_t(Key::Count)> keyDown{};

  Cursor cursor = Cursor::Arrow;
  std::string title;
  bool quit = false;
  int exitCode = 0;
  bool fullscreen = false;
  bool dark = true;
  RectF caret;
  std::string clipboard;
  std::vector<std::string> opened, fatals;
  bool dialogs = true;
  std::deque<std::optional<std::string>> dialogResults;
  std::vector<headless::DialogCall> dialogCalls;
};

Headless& H() {
  static Headless s;
  return s;
}

int physical(float logical, float scale) { return std::max(1, int(std::lround(double(logical) * double(scale)))); }

Event resizeEvent() {
  Headless& s = H();
  Event e;
  e.type = EventType::Resize;
  e.width = float(physical(float(s.w), s.scale)) / s.scale;
  e.height = float(physical(float(s.h), s.scale)) / s.scale;
  e.scale = s.scale;
  return e;
}

void enqueue(Event e) {
  Headless& s = H();
  std::lock_guard<std::mutex> lk(s.m);
  s.queue.push_back(std::move(e));
}

Event pointerEvent(EventType t, float x, float y, u32 mods) {
  Event e;
  e.type = t;
  e.x = x;
  e.y = y;
  e.mods = mods;
  return e;
}

std::optional<std::string> dialog(headless::DialogCall call) {
  Headless& s = H();
  s.dialogCalls.push_back(std::move(call));
  if (!s.dialogs || s.dialogResults.empty()) return std::nullopt;
  std::optional<std::string> r = std::move(s.dialogResults.front());
  s.dialogResults.pop_front();
  return r;
}

}  // namespace

// ================================================================ интерфейс платформы
int run(App& app, const WindowConfig& cfg) {
  Headless& s = H();
  s.title = cfg.title;
  s.quit = false;
  s.exitCode = 0;
  headless::attach(&app);
  struct Detach {
    ~Detach() { headless::attach(nullptr); }
  } detach;
  // Кадры, пока они нужны; анимация продвигает виртуальное время. Предел — защита от бесконечной анимации.
  for (int n = 0; n < 100000 && !s.quit; n++) {
    headless::pump();
    if (s.quit || !headless::needsFrame()) break;
    headless::renderFrame();
    if (!s.quit && app.animating()) headless::advance(1.0 / 60);
  }
  return s.exitCode;
}

void quit(int exitCode) {
  H().quit = true;
  H().exitCode = exitCode;
}

void invalidate() { H().dirty = true; }

void wake() {
  Headless& s = H();
  {
    std::lock_guard<std::mutex> lk(s.m);
    if (!s.wakeQueued) {
      Event e;
      e.type = EventType::Wake;
      s.queue.push_back(std::move(e));
      s.wakeQueued = true;
    }
  }
  s.dirty = true;
}

void setCursor(Cursor c) { H().cursor = c; }
void setTitle(const std::string& title) { H().title = title; }
void setFullscreen(bool on) { H().fullscreen = on; }
bool fullscreen() { return H().fullscreen; }
void setDarkFrame(bool dark) { H().dark = dark; }
void setTextInputRect(RectF caret) { H().caret = caret; }
float scale() { return H().scale; }
void* nativeWindow() { return nullptr; }

std::string clipboardText() { return H().clipboard; }
void setClipboardText(const std::string& text) { H().clipboard = text; }

std::optional<std::string> openFileDialog(const std::string& title, const std::vector<FileFilter>& filters, const std::string& startDir) {
  headless::DialogCall c;
  c.kind = headless::DialogCall::Open;
  c.title = title;
  c.filters = filters;
  c.startDir = startDir;
  return dialog(std::move(c));
}

std::optional<std::string> saveFileDialog(const std::string& title, const std::vector<FileFilter>& filters, const std::string& startDir,
                                          const std::string& defaultName) {
  headless::DialogCall c;
  c.kind = headless::DialogCall::Save;
  c.title = title;
  c.filters = filters;
  c.startDir = startDir;
  c.defaultName = defaultName;
  return dialog(std::move(c));
}

std::optional<std::string> pickFolderDialog(const std::string& title, const std::string& startDir) {
  headless::DialogCall c;
  c.kind = headless::DialogCall::Folder;
  c.title = title;
  c.startDir = startDir;
  return dialog(std::move(c));
}

bool dialogsSupported() { return H().dialogs; }

bool openPath(const std::string& path) {
  if (path.empty()) return false;
  H().opened.push_back(path);
  return true;
}

bool openUrl(const std::string& url) {
  if (!detail::isSafeUrl(url)) return false;
  H().opened.push_back(url);
  return true;
}

void showFatal(const std::string& title, const std::string& text) {
  logError("%s: %s", title.c_str(), text.c_str());
  H().fatals.push_back(title + ": " + text);
}

double time() { return H().now.load(); }

// ================================================================ управление из тестов
namespace headless {

void reset() {
  Headless& s = H();
  {
    std::lock_guard<std::mutex> lk(s.m);
    s.queue.clear();
    s.wakeQueued = false;
  }
  s.dirty = true;
  s.now = 0;
  s.app = nullptr;
  s.w = 1280;
  s.h = 800;
  s.scale = 1;
  s.px.clear();
  s.px.shrink_to_fit();
  s.frame = Frame();
  s.reset = true;
  s.frames = 0;
  s.clicks.reset();
  s.lastClicks.fill(0);
  s.keyDown.fill(false);
  s.cursor = Cursor::Arrow;
  s.title.clear();
  s.quit = false;
  s.exitCode = 0;
  s.fullscreen = false;
  s.dark = true;
  s.caret = RectF();
  s.clipboard.clear();
  s.opened.clear();
  s.fatals.clear();
  s.dialogs = true;
  s.dialogResults.clear();
  s.dialogCalls.clear();
}

void configure(int width, int height, float sc) {
  Headless& s = H();
  bool scaleChanged = sc != s.scale;
  s.w = std::max(1, width);
  s.h = std::max(1, height);
  s.scale = sc > 0 ? sc : 1;
  s.dirty = true;
  if (s.app) {
    Event e = resizeEvent();
    enqueue(e);
    if (scaleChanged) {
      e.type = EventType::ScaleChanged;
      enqueue(std::move(e));
    }
  }
}

void attach(App* app) {
  Headless& s = H();
  s.app = app;
  s.dirty = true;
  if (app) {  // Resize — первое событие, как у настоящих окон
    std::lock_guard<std::mutex> lk(s.m);
    s.queue.push_front(resizeEvent());
  }
}

App* attached() { return H().app; }

void post(const Event& e) { enqueue(e); }

int pump() {
  Headless& s = H();
  int n = 0;
  while (!s.quit) {  // после quit() события остаются в очереди, как в настоящем цикле
    Event e;
    {
      std::lock_guard<std::mutex> lk(s.m);
      if (s.queue.empty()) break;
      e = std::move(s.queue.front());
      s.queue.pop_front();
      if (e.type == EventType::Wake) s.wakeQueued = false;
    }
    n++;
    s.dirty = true;  // каждое доставленное событие — новый кадр
    if (s.app) s.app->onEvent(e);  // исключения обработчиков доходят до теста как есть
  }
  return n;
}

bool needsFrame() {
  Headless& s = H();
  if (!s.app) return false;
  if (s.dirty) return true;
  {
    std::lock_guard<std::mutex> lk(s.m);
    if (!s.queue.empty()) return true;
  }
  return s.app->animating();
}

Frame& renderFrame() {
  Headless& s = H();
  if (!s.app) throw std::logic_error("headless::renderFrame: приложение не подключено (headless::attach)");
  pump();
  int pw = physical(float(s.w), s.scale), ph = physical(float(s.h), s.scale);
  if (pw != s.frame.w || ph != s.frame.h || s.px.empty()) {
    s.px.assign(size_t(pw) * size_t(ph), 0xFF000000u);
    s.reset = true;
  }
  s.dirty = false;
  s.frame.px = s.px.data();
  s.frame.w = pw;
  s.frame.h = ph;
  s.frame.stride = pw;
  s.frame.scale = s.scale;
  s.frame.reset = s.reset;
  s.frame.index = s.frames;
  s.app->onFrame(s.frame);
  s.frames++;
  s.reset = false;
  return s.frame;
}

const Frame& lastFrame() { return H().frame; }

u64 frames() { return H().frames; }

int settle(int maxFrames, double dt) {
  Headless& s = H();
  int n = 0;
  while (n < maxFrames && !s.quit) {
    pump();
    if (!needsFrame()) break;
    renderFrame();
    n++;
    if (s.app && s.app->animating()) advance(dt);
  }
  return n;
}

void advance(double seconds) {
  Headless& s = H();
  if (seconds > 0) s.now = s.now.load() + seconds;
}

// ---------------------------------------------------------------- сценарии ввода
void mouseMove(float x, float y, u32 mods) { enqueue(pointerEvent(EventType::MouseMove, x, y, mods)); }

void mouseDown(float x, float y, int button, u32 mods) {
  Headless& s = H();
  Event e = pointerEvent(EventType::MouseDown, x, y, mods);
  e.button = button;
  e.clicks = s.clicks.press(button, x, y, s.now.load(), kDoubleClickTime, kDoubleClickSlop);
  if (button >= 0 && button < 5) s.lastClicks[size_t(button)] = e.clicks;
  enqueue(std::move(e));
}

void mouseUp(float x, float y, int button, u32 mods) {
  Headless& s = H();
  Event e = pointerEvent(EventType::MouseUp, x, y, mods);
  e.button = button;
  e.clicks = button >= 0 && button < 5 ? std::max(1, s.lastClicks[size_t(button)]) : 1;
  enqueue(std::move(e));
}

void click(float x, float y, int button, u32 mods) {
  H().clicks.reset();  // отдельный щелчок всегда одинарный, сколько бы их ни было подряд
  mouseMove(x, y, mods);
  mouseDown(x, y, button, mods);
  mouseUp(x, y, button, mods);
}

void doubleClick(float x, float y, u32 mods) {
  H().clicks.reset();
  mouseMove(x, y, mods);
  for (int i = 0; i < 2; i++) {
    mouseDown(x, y, MouseLeft, mods);
    mouseUp(x, y, MouseLeft, mods);
  }
}

void drag(float x0, float y0, float x1, float y1, int steps, int button, u32 mods) {
  H().clicks.reset();
  steps = std::max(1, steps);
  mouseMove(x0, y0, mods);
  mouseDown(x0, y0, button, mods);
  for (int i = 1; i <= steps; i++) {
    float t = float(i) / float(steps);
    mouseMove(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, mods);
  }
  mouseUp(x1, y1, button, mods);
}

void wheel(float x, float y, float dy, float dx, bool precise, u32 mods) {
  Event e = pointerEvent(EventType::MouseWheel, x, y, mods);
  e.wheelX = dx;
  e.wheelY = dy;
  e.precise = precise;
  enqueue(std::move(e));
}

void keyDown(Key k, u32 mods, bool repeat) {
  if (k != Key::Unknown && k < Key::Count) H().keyDown[size_t(k)] = true;
  Event e;
  e.type = EventType::KeyDown;
  e.key = k;
  e.mods = mods;
  e.repeat = repeat;
  enqueue(std::move(e));
}

void keyUp(Key k, u32 mods) {
  if (k != Key::Unknown && k < Key::Count) H().keyDown[size_t(k)] = false;
  Event e;
  e.type = EventType::KeyUp;
  e.key = k;
  e.mods = mods;
  enqueue(std::move(e));
}

void press(Key k, u32 mods) {
  keyDown(k, mods);
  keyUp(k, mods);
}

void type(const std::string& text) {
  std::string t = detail::filterText(text);
  if (t.empty()) return;
  Event e;
  e.type = EventType::Text;
  e.text = std::move(t);
  enqueue(std::move(e));
}

void dropFiles(float x, float y, const std::vector<std::string>& files) {
  Event e = pointerEvent(EventType::FilesDropped, x, y, 0);
  e.files = files;
  enqueue(std::move(e));
}

void setFocus(bool focused) {
  Headless& s = H();
  if (!focused)  // как в настоящих окнах: зажатые клавиши отпускаются до FocusOut
    for (size_t k = 0; k < s.keyDown.size(); k++)
      if (s.keyDown[k]) keyUp(Key(k), 0);
  Event e;
  e.type = focused ? EventType::FocusIn : EventType::FocusOut;
  enqueue(std::move(e));
}

// ---------------------------------------------------------------- наблюдение
Cursor cursor() { return H().cursor; }
std::string title() { return H().title; }
bool quitRequested() { return H().quit; }
int exitCode() { return H().exitCode; }
RectF textInputRect() { return H().caret; }
bool darkFrame() { return H().dark; }
const std::vector<std::string>& opened() { return H().opened; }
const std::vector<std::string>& fatalMessages() { return H().fatals; }
void setDialogsSupported(bool on) { H().dialogs = on; }
void queueDialogResult(std::optional<std::string> result) { H().dialogResults.push_back(std::move(result)); }
const std::vector<DialogCall>& dialogCalls() { return H().dialogCalls; }

}  // namespace headless
}  // namespace rg::platform
