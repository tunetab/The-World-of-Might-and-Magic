// Regnum — слой платформы: окно, ввод, буфер обмена, системные диалоги.
// Реализации: win32.cpp (Windows), x11.cpp (Linux), cocoa.cpp (macOS), headless.cpp (тесты и CLI).
// Заголовок не зависит от gfx: кадр — буфер premultiplied BGRA (u32 = 0xAARRGGBB, байты B, G, R, A).
//
// Потоки: всё вызывается из главного потока, кроме wake(), invalidate() и time() — они потокобезопасны.
// Координаты событий — логические пиксели (физические / scale), начало — левый верхний угол области окна.
#pragma once
#include "base/base.h"

namespace rg::platform {

// ---------------------------------------------------------------- клавиши
// Клавиша определяется по положению/виртуальному коду и не зависит от раскладки:
// Ctrl+Z работает и в русской раскладке. Набранный текст приходит отдельно (Event::Text).
enum class Key : u16 {
  Unknown,
  A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
  D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,
  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
  Escape, Enter, Tab, Backspace, Delete, Insert, Home, End, PageUp, PageDown,
  Left, Right, Up, Down, Space, Minus, Equal, LBracket, RBracket, Semicolon, Quote, Comma, Period, Slash,
  Backslash, Grave,
  NumPad0, NumPad1, NumPad2, NumPad3, NumPad4, NumPad5, NumPad6, NumPad7, NumPad8, NumPad9,
  NumAdd, NumSub, NumMul, NumDiv, NumDecimal, NumEnter,
  Shift, Ctrl, Alt, Super,
  CapsLock, Menu,
  Count
};

enum Mod : u32 { ModShift = 1, ModCtrl = 2, ModAlt = 4, ModSuper = 8 };
u32 primaryMod();                               // Ctrl на Windows/Linux, Super (⌘) на macOS

// Название клавиши для подсказок: «A», «F5», «Del», «Пробел», «←» (на macOS — «⌫», «↩» и т. п.).
std::string keyName(Key k);
// Сочетание для подсказок: «Ctrl+Shift+S» на Windows/Linux, «⇧⌘S» на macOS.
std::string shortcutText(Key k, u32 mods);

// ---------------------------------------------------------------- события
enum class EventType : u8 {
  None,
  MouseMove,      // x, y, mods
  MouseDown,      // x, y, button, clicks (1, 2 — двойной, 3 — тройной), mods
  MouseUp,        // x, y, button, clicks (как у нажатия), mods
  MouseWheel,     // x, y, wheelX, wheelY, precise, mods
  MouseLeave,     // указатель ушёл из окна
  KeyDown,        // key, mods, repeat
  KeyUp,          // key, mods
  Text,           // text — UTF-8, без управляющих символов
  FilesDropped,   // x, y, files (UTF-8 пути)
  Resize,         // width, height (логические), scale — первое событие после запуска тоже Resize
  ScaleChanged,   // width, height, scale — окно перешло на монитор с другим DPI
  FocusIn,        // окно получило фокус клавиатуры
  FocusOut,       // окно потеряло фокус (перед ним приходят KeyUp для всех зажатых клавиш)
  Wake,           // вызван wake() (например, из фоновой задачи)
};

enum MouseButton : int { MouseLeft = 0, MouseRight = 1, MouseMiddle = 2, MouseBack = 3, MouseForward = 4 };

struct Event {
  using Type = EventType;
  Type type = Type::None;
  float x = 0, y = 0;            // положение указателя, логические пиксели
  int button = MouseLeft;        // MouseButton
  int clicks = 0;                // число щелчков подряд
  // Колесо: wheelY > 0 — прокрутка вверх (от себя), wheelX > 0 — вправо.
  // Обычное колесо: 1 щелчок = 1. Тачпад и плавные колёса: precise = true, значения в логических пикселях.
  float wheelX = 0, wheelY = 0;
  bool precise = false;
  Key key = Key::Unknown;
  u32 mods = 0;                  // Mod*, текущее состояние модификаторов
  bool repeat = false;           // автоповтор клавиши
  std::string text;
  std::vector<std::string> files;
  float width = 0, height = 0;   // Resize/ScaleChanged: размер области окна, логические пиксели
  float scale = 1;               // Resize/ScaleChanged: физических пикселей на логический
};

enum class Cursor : u8 { Arrow, Hand, IBeam, Crosshair, Move, ResizeH, ResizeV, ResizeNWSE, ResizeNESW, Grab, Grabbing, NotAllowed, Wait };

// Кадр — физические пиксели окна. Приложение обязано заполнить его целиком непрозрачными пикселями (A = 255).
// Буфер сохраняется между кадрами, пока не изменился размер (reset = true — содержимое не определено).
struct Frame {
  u32* px = nullptr;             // строки сверху вниз
  int w = 0, h = 0;              // физические пиксели
  int stride = 0;                // длина строки в пикселях (u32), stride >= w
  float scale = 1;               // физических пикселей на логический
  bool reset = true;             // буфер создан заново
  u64 index = 0;                 // номер кадра с начала run()
  u32* row(int y) const { return px + size_t(y) * size_t(stride); }
  float logicalW() const { return float(w) / scale; }
  float logicalH() const { return float(h) / scale; }
};

class App {
 public:
  virtual ~App() = default;
  virtual void onEvent(const Event& e) = 0;
  virtual void onFrame(Frame& f) = 0;
  virtual bool animating() = 0;          // true — кадры идут непрерывно (~60 в секунду)
  virtual bool onCloseRequest() = 0;     // кнопка закрытия окна, Alt+F4, ⌘Q; true — закрыть
};

struct WindowConfig {
  std::string title = "Regnum";
  int width = 1280, height = 800;            // логические пиксели области окна (при первом запуске)
  int minWidth = 800, minHeight = 520;
  bool maximized = false;                    // при первом запуске
  bool darkFrame = true;                     // тёмная рамка/заголовок окна
  std::string appName = "Regnum";            // каталог настроек в userDataDir
  std::string placementFile = "window.ini";  // положение окна: имя в userDataDir или абсолютный путь; пусто — не запоминать
};

// Цикл событий. Кадр рисуется после каждого доставленного события (ввод, размер, фокус, Wake), после invalidate()
// и непрерывно, пока animating(); не чаще частоты экрана. Без этого поток спит (процессор не занят).
// Исключение из обработчика приложения завершает цикл и пробрасывается из run().
// Возвращает код из quit() (0 при закрытии окна).
int run(App& app, const WindowConfig& cfg = {});
void quit(int exitCode = 0);                  // завершить run() без onCloseRequest (после «Сохранить и выйти»)
void invalidate();                            // перерисовать кадр (потокобезопасно)
void wake();                                  // разбудить цикл: событие Wake + кадр (потокобезопасно, с объединением)

void setCursor(Cursor c);
void setTitle(const std::string& title);
void setFullscreen(bool on);
bool fullscreen();
void setDarkFrame(bool dark);                 // при смене темы
void setTextInputRect(RectF caret);           // каретка для окна IME (Windows); в X11/macOS окно кандидатов ставит система
float scale();                                // текущий масштаб окна (до run — масштаб основного монитора)
void* nativeWindow();                         // HWND / Window (X11) / NSWindow*; nullptr вне run()

// Буфер обмена: текст UTF-8, переводы строк — LF. В X11 работает только во время run() (нужно окно-владелец).
std::string clipboardText();
void setClipboardText(const std::string& text);

// Системные диалоги. nullopt — отмена или dialogsSupported() == false (тогда приложение показывает свой проводник).
struct FileFilter {
  std::string name;                           // «Миры Regnum»
  std::vector<std::string> exts;              // без точки: {"regnum", "zip"}; пусто — все файлы
};
std::optional<std::string> openFileDialog(const std::string& title, const std::vector<FileFilter>& filters = {},
                                          const std::string& startDir = {});
std::optional<std::string> saveFileDialog(const std::string& title, const std::vector<FileFilter>& filters = {},
                                          const std::string& startDir = {}, const std::string& defaultName = {});
std::optional<std::string> pickFolderDialog(const std::string& title, const std::string& startDir = {});
bool dialogsSupported();

bool openPath(const std::string& path);       // файл или папка в системной программе (проводник)
bool openUrl(const std::string& url);         // только http, https, mailto
void showFatal(const std::string& title, const std::string& text);   // работает и до run()
double time();                                // монотонные секунды; для анимаций интерфейса (в headless — виртуальное время)

// ---------------------------------------------------------------- headless
// Только в сборках с headless.cpp (тесты, regnum-cli). Без окна: события ставятся в очередь
// и доставляются приложению при pump()/renderFrame(). Время виртуальное: time() меняется только через advance().
namespace headless {

struct DialogCall {
  enum Kind : u8 { Open, Save, Folder } kind = Open;
  std::string title, startDir, defaultName;
  std::vector<FileFilter> filters;
};

void reset();                                     // 1280×800, масштаб 1, пустые очередь и буфер обмена, time() = 0
void configure(int width, int height, float scale = 1);  // логический размер; подключённому приложению — Resize
void attach(App* app);                            // подключить приложение (Resize в очередь); nullptr — отключить
App* attached();

void post(const Event& e);                        // в очередь (потокобезопасно)
int pump();                                       // доставить очередь; вернуть число событий
bool needsFrame();                                // invalidate()/wake() или animating()
Frame& renderFrame();                             // pump() + onFrame в буфер; кадр остаётся доступен до следующего
const Frame& lastFrame();
u64 frames();                                     // число отрисованных кадров
int settle(int maxFrames = 600, double dt = 1.0 / 60);  // кадры, пока нужны, с продвижением времени на dt; вернуть число
void advance(double seconds);                     // виртуальное время вперёд

// Сценарии ввода (ставят события в очередь; модификаторы — как при реальном вводе).
void mouseMove(float x, float y, u32 mods = 0);
void mouseDown(float x, float y, int button = MouseLeft, u32 mods = 0);   // clicks — по времени и расстоянию
void mouseUp(float x, float y, int button = MouseLeft, u32 mods = 0);
void click(float x, float y, int button = MouseLeft, u32 mods = 0);
void doubleClick(float x, float y, u32 mods = 0);
void drag(float x0, float y0, float x1, float y1, int steps = 8, int button = MouseLeft, u32 mods = 0);
void wheel(float x, float y, float dy, float dx = 0, bool precise = false, u32 mods = 0);
void keyDown(Key k, u32 mods = 0, bool repeat = false);
void keyUp(Key k, u32 mods = 0);
void press(Key k, u32 mods = 0);                  // KeyDown + KeyUp
void type(const std::string& text);               // событие Text
void dropFiles(float x, float y, const std::vector<std::string>& files);
void setFocus(bool focused);                      // FocusIn / FocusOut

// Наблюдение за тем, что приложение потребовало от платформы.
Cursor cursor();
std::string title();
bool quitRequested();
int exitCode();
RectF textInputRect();
bool darkFrame();
const std::vector<std::string>& opened();         // openPath/openUrl по порядку
const std::vector<std::string>& fatalMessages();  // showFatal: «заголовок: текст»

// Диалоги: ответы по порядку вызовов; без заготовленного ответа — nullopt.
void setDialogsSupported(bool on);
void queueDialogResult(std::optional<std::string> result);
const std::vector<DialogCall>& dialogCalls();

}  // namespace headless
}  // namespace rg::platform
