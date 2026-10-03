// Regnum — платформа Linux: Xlib через dlopen("libX11.so.6") без заголовков X11.
// XIM/Xutf8LookupString, XPutImage, CLIPBOARD/UTF8_STRING (с INCR), XDND, курсоры (Xcursor или шрифт), Xft.dpi.
// Проверка синтаксиса на Windows: clang++ -fsyntax-only -DRG_PLATFORM_SYNTAX_CHECK -I src src/platform/x11.cpp
#ifndef RG_PLATFORM_SYNTAX_CHECK
#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>
#endif

#include <atomic>
#include <bit>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <thread>

#include "platform/common.h"
#include "platform/platform.h"
#include "platform/x11_api.h"

#ifdef RG_PLATFORM_SYNTAX_CHECK
// Объявления POSIX для проверки синтаксиса без системных заголовков.
namespace rg::platform::posix {
struct pollfd { int fd; short events; short revents; };
constexpr short kPollIn = 1;
constexpr int kRtldNow = 2, kRtldLocal = 0;
void* dlopen(const char*, int);
void* dlsym(void*, const char*);
int poll(pollfd*, unsigned long, int);
int pipe(int*);
long read(int, void*, unsigned long);
long write(int, const void*, unsigned long);
int close(int);
int fork();
int execvp(const char*, char* const*);
[[noreturn]] void _exit(int);
int waitpid(int, int*, int);
int setsid();
int getpid();
}  // namespace rg::platform::posix
#else
extern "C" pid_t waitpid(pid_t pid, int* status, int options);  // <sys/wait.h> не подключаем
namespace rg::platform::posix {
using ::pollfd;
using ::dlopen;
using ::dlsym;
using ::poll;
using ::pipe;
using ::read;
using ::write;
using ::close;
using ::fork;
using ::execvp;
using ::_exit;
using ::waitpid;
using ::setsid;
using ::getpid;
constexpr short kPollIn = POLLIN;
constexpr int kRtldNow = RTLD_NOW, kRtldLocal = RTLD_LOCAL;
}  // namespace rg::platform::posix
#endif

namespace rg::platform {
namespace {

using namespace xlib;
using posix::pollfd;
using posix::kPollIn;

// ---------------------------------------------------------------- функции libX11
using ErrorHandler = int (*)(Display*, XErrorEvent*);
using EventPredicate = Bool (*)(Display*, XEvent*, XPointer);

struct XApi {
  void* lib = nullptr;
  Display* (*OpenDisplay)(const char*) = nullptr;
  int (*CloseDisplay)(Display*) = nullptr;
  int (*DefaultScreen)(Display*) = nullptr;
  Window (*RootWindow)(Display*, int) = nullptr;
  int (*DisplayWidth)(Display*, int) = nullptr;
  int (*DisplayHeight)(Display*, int) = nullptr;
  int (*ConnectionNumber)(Display*) = nullptr;
  Status (*MatchVisualInfo)(Display*, int, int, int, XVisualInfo*) = nullptr;
  XPixmapFormatValues* (*ListPixmapFormats)(Display*, int*) = nullptr;
  Colormap (*CreateColormap)(Display*, Window, Visual*, int) = nullptr;
  int (*FreeColormap)(Display*, Colormap) = nullptr;
  Window (*CreateWindow)(Display*, Window, int, int, unsigned, unsigned, unsigned, int, unsigned, Visual*, unsigned long,
                         XSetWindowAttributes*) = nullptr;
  int (*DestroyWindow)(Display*, Window) = nullptr;
  int (*MapWindow)(Display*, Window) = nullptr;
  int (*StoreName)(Display*, Window, const char*) = nullptr;
  int (*ChangeProperty)(Display*, Window, Atom, Atom, int, int, const unsigned char*, int) = nullptr;
  int (*DeleteProperty)(Display*, Window, Atom) = nullptr;
  int (*GetWindowProperty)(Display*, Window, Atom, long, long, Bool, Atom, Atom*, int*, unsigned long*, unsigned long*,
                           unsigned char**) = nullptr;
  Atom (*InternAtom)(Display*, const char*, Bool) = nullptr;
  Status (*SetWMProtocols)(Display*, Window, Atom*, int) = nullptr;
  void (*SetWMNormalHints)(Display*, Window, XSizeHints*) = nullptr;
  int (*SelectInput)(Display*, Window, long) = nullptr;
  int (*Pending)(Display*) = nullptr;
  int (*EventsQueued)(Display*, int) = nullptr;
  int (*NextEvent)(Display*, XEvent*) = nullptr;
  int (*PeekEvent)(Display*, XEvent*) = nullptr;
  Bool (*FilterEvent)(XEvent*, Window) = nullptr;
  Bool (*CheckIfEvent)(Display*, XEvent*, EventPredicate, XPointer) = nullptr;
  int (*Flush)(Display*) = nullptr;
  GC (*CreateGC)(Display*, Drawable, unsigned long, void*) = nullptr;
  int (*FreeGC)(Display*, GC) = nullptr;
  Status (*InitImage)(XImage*) = nullptr;
  int (*PutImage)(Display*, Drawable, GC, XImage*, int, int, int, int, unsigned, unsigned) = nullptr;
  int (*Free)(void*) = nullptr;
  XCursor (*CreateFontCursor)(Display*, unsigned) = nullptr;
  int (*DefineCursor)(Display*, Window, XCursor) = nullptr;
  int (*FreeCursor)(Display*, XCursor) = nullptr;
  int (*SetSelectionOwner)(Display*, Atom, Window, Time) = nullptr;
  Window (*GetSelectionOwner)(Display*, Atom) = nullptr;
  int (*ConvertSelection)(Display*, Atom, Atom, Atom, Window, Time) = nullptr;
  Status (*SendEvent)(Display*, Window, Bool, long, XEvent*) = nullptr;
  Bool (*TranslateCoordinates)(Display*, Window, Window, int, int, int*, int*, Window*) = nullptr;
  ErrorHandler (*SetErrorHandler)(ErrorHandler) = nullptr;
  char* (*SetLocaleModifiers)(const char*) = nullptr;
  Bool (*SupportsLocale)() = nullptr;
  XIM (*OpenIM)(Display*, void*, char*, char*) = nullptr;
  Status (*CloseIM)(XIM) = nullptr;
  XIC (*CreateIC)(XIM, ...) = nullptr;
  void (*DestroyIC)(XIC) = nullptr;
  void (*SetICFocus)(XIC) = nullptr;
  void (*UnsetICFocus)(XIC) = nullptr;
  int (*Utf8LookupString)(XIC, XKeyEvent*, char*, int, KeySym*, Status*) = nullptr;
  int (*LookupString)(XKeyEvent*, char*, int, KeySym*, XComposeStatus*) = nullptr;
  KeySym (*KbKeycodeToKeysym)(Display*, KeyCode, int, int) = nullptr;
  Bool (*KbSetDetectableAutoRepeat)(Display*, Bool, Bool*) = nullptr;
  int (*RefreshKeyboardMapping)(XMappingEvent*) = nullptr;
  // libXcursor.so.1 — необязательно: курсоры из темы рабочего стола.
  XCursor (*CursorLibraryLoadCursor)(Display*, const char*) = nullptr;
};

XApi X;

template <class F>
void bind(void* lib, F& f, const char* name, std::string& missing) {
  f = reinterpret_cast<F>(posix::dlsym(lib, name));
  if (!f) missing += std::string(missing.empty() ? "" : ", ") + name;
}

void loadX() {
  if (X.lib) return;
  void* lib = posix::dlopen("libX11.so.6", posix::kRtldNow | posix::kRtldLocal);
  if (!lib) lib = posix::dlopen("libX11.so", posix::kRtldNow | posix::kRtldLocal);
  if (!lib) throw std::runtime_error("Не найдена библиотека libX11.so.6 — нужен X-сервер или XWayland");
  std::string miss;
  bind(lib, X.OpenDisplay, "XOpenDisplay", miss);
  bind(lib, X.CloseDisplay, "XCloseDisplay", miss);
  bind(lib, X.DefaultScreen, "XDefaultScreen", miss);
  bind(lib, X.RootWindow, "XRootWindow", miss);
  bind(lib, X.DisplayWidth, "XDisplayWidth", miss);
  bind(lib, X.DisplayHeight, "XDisplayHeight", miss);
  bind(lib, X.ConnectionNumber, "XConnectionNumber", miss);
  bind(lib, X.MatchVisualInfo, "XMatchVisualInfo", miss);
  bind(lib, X.ListPixmapFormats, "XListPixmapFormats", miss);
  bind(lib, X.CreateColormap, "XCreateColormap", miss);
  bind(lib, X.FreeColormap, "XFreeColormap", miss);
  bind(lib, X.CreateWindow, "XCreateWindow", miss);
  bind(lib, X.DestroyWindow, "XDestroyWindow", miss);
  bind(lib, X.MapWindow, "XMapWindow", miss);
  bind(lib, X.StoreName, "XStoreName", miss);
  bind(lib, X.ChangeProperty, "XChangeProperty", miss);
  bind(lib, X.DeleteProperty, "XDeleteProperty", miss);
  bind(lib, X.GetWindowProperty, "XGetWindowProperty", miss);
  bind(lib, X.InternAtom, "XInternAtom", miss);
  bind(lib, X.SetWMProtocols, "XSetWMProtocols", miss);
  bind(lib, X.SetWMNormalHints, "XSetWMNormalHints", miss);
  bind(lib, X.SelectInput, "XSelectInput", miss);
  bind(lib, X.Pending, "XPending", miss);
  bind(lib, X.EventsQueued, "XEventsQueued", miss);
  bind(lib, X.NextEvent, "XNextEvent", miss);
  bind(lib, X.PeekEvent, "XPeekEvent", miss);
  bind(lib, X.FilterEvent, "XFilterEvent", miss);
  bind(lib, X.CheckIfEvent, "XCheckIfEvent", miss);
  bind(lib, X.Flush, "XFlush", miss);
  bind(lib, X.CreateGC, "XCreateGC", miss);
  bind(lib, X.FreeGC, "XFreeGC", miss);
  bind(lib, X.InitImage, "XInitImage", miss);
  bind(lib, X.PutImage, "XPutImage", miss);
  bind(lib, X.Free, "XFree", miss);
  bind(lib, X.CreateFontCursor, "XCreateFontCursor", miss);
  bind(lib, X.DefineCursor, "XDefineCursor", miss);
  bind(lib, X.FreeCursor, "XFreeCursor", miss);
  bind(lib, X.SetSelectionOwner, "XSetSelectionOwner", miss);
  bind(lib, X.GetSelectionOwner, "XGetSelectionOwner", miss);
  bind(lib, X.ConvertSelection, "XConvertSelection", miss);
  bind(lib, X.SendEvent, "XSendEvent", miss);
  bind(lib, X.TranslateCoordinates, "XTranslateCoordinates", miss);
  bind(lib, X.SetErrorHandler, "XSetErrorHandler", miss);
  bind(lib, X.SetLocaleModifiers, "XSetLocaleModifiers", miss);
  bind(lib, X.SupportsLocale, "XSupportsLocale", miss);
  bind(lib, X.OpenIM, "XOpenIM", miss);
  bind(lib, X.CloseIM, "XCloseIM", miss);
  bind(lib, X.CreateIC, "XCreateIC", miss);
  bind(lib, X.DestroyIC, "XDestroyIC", miss);
  bind(lib, X.SetICFocus, "XSetICFocus", miss);
  bind(lib, X.UnsetICFocus, "XUnsetICFocus", miss);
  bind(lib, X.Utf8LookupString, "Xutf8LookupString", miss);
  bind(lib, X.LookupString, "XLookupString", miss);
  bind(lib, X.KbKeycodeToKeysym, "XkbKeycodeToKeysym", miss);
  bind(lib, X.KbSetDetectableAutoRepeat, "XkbSetDetectableAutoRepeat", miss);
  bind(lib, X.RefreshKeyboardMapping, "XRefreshKeyboardMapping", miss);
  if (!miss.empty()) throw std::runtime_error("В libX11 нет функций: " + miss);
  if (void* cur = posix::dlopen("libXcursor.so.1", posix::kRtldNow | posix::kRtldLocal))
    X.CursorLibraryLoadCursor = reinterpret_cast<XCursor (*)(Display*, const char*)>(posix::dlsym(cur, "XcursorLibraryLoadCursor"));
  X.lib = lib;
}

int onXError(Display*, XErrorEvent* e) {
  // Обработчик по умолчанию завершает процесс; ошибки протокола только записываем.
  logWarn("X11: ошибка протокола %d (запрос %d.%d)", int(e->error_code), int(e->request_code), int(e->minor_code));
  return 0;
}

// ---------------------------------------------------------------- атомы
struct Atoms {
  Atom WM_PROTOCOLS, WM_DELETE_WINDOW, NET_WM_PING, NET_WM_NAME, NET_WM_ICON_NAME, NET_WM_PID, UTF8_STRING, CLIPBOARD, TARGETS, TEXT,
      INCR, SAVE_TARGETS, CLIPBOARD_MANAGER, TEXT_PLAIN_UTF8, TEXT_PLAIN, NET_WM_STATE, NET_WM_STATE_FULLSCREEN, NET_WM_STATE_MAXIMIZED_VERT,
      NET_WM_STATE_MAXIMIZED_HORZ, NET_FRAME_EXTENTS, GTK_THEME_VARIANT, RESOURCE_MANAGER, RG_SELECTION, XdndAware, XdndEnter, XdndPosition,
      XdndStatus, XdndLeave, XdndDrop, XdndFinished, XdndSelection, XdndActionCopy, XdndTypeList, TEXT_URI_LIST;
};

// ---------------------------------------------------------------- состояние
struct IncrTransfer {  // отдача большого текста по протоколу INCR
  Window requestor;
  Atom property, type;
  std::string data;
  size_t offset;
};

struct State {
  Display* dpy = nullptr;
  int screen = 0;
  Window root = 0, win = 0;
  Visual* visual = nullptr;
  Colormap colormap = 0;
  GC gc = nullptr;
  XIM im = nullptr;
  XIC ic = nullptr;
  Atoms a{};
  int xfd = -1;
  int pipeR = -1;
  std::atomic<int> pipeW{-1};
  std::thread::id mainThread;
  bool mapped = false;
  bool detectableRepeat = false;

  App* app = nullptr;
  WindowConfig cfg;
  bool ready = false;
  float scale = 1;
  bool envScale = false;   // масштаб задан GDK_SCALE — Xft.dpi не отслеживаем
  int cw = 0, ch = 0;

  std::vector<u32> buf;
  int bw = 0, bh = 0;
  bool bufferReset = true;
  u64 frameIndex = 0;
  double lastFrame = -1;
  double period = 1.0 / 60;
  int exX0 = 0, exY0 = 0, exX1 = 0, exY1 = 0;  // накопленная область Expose
  bool exposed = false;

  std::atomic<bool> dirty{true};
  std::atomic<bool> wakePosted{false};
  std::atomic<bool> wakeEvent{false};
  int callbackDepth = 0;
  bool quitting = false;
  bool closePending = false;
  int exitCode = 0;
  std::exception_ptr error;
  std::deque<Event> queue;

  Time lastTime = CurrentTime;
  int buttonsDown = 0;
  bool mouseInside = false;
  float mx = 0, my = 0;
  detail::ClickCounter clicks;
  std::array<int, 5> lastClicks{};
  std::array<bool, 256> keycodeDown{};
  std::array<bool, size_t(Key::Count)> keyDown{};

  Cursor cursor = Cursor::Arrow;
  std::array<XCursor, 13> cursors{};
  bool fullscreen = false, maximized = false, pendingFullscreen = false;
  bool dark = true;
  std::string title = "Regnum";
  RectF caret;

  bool ownsClipboard = false;
  std::string clipText;
  std::vector<IncrTransfer> incr;

  // Перетаскивание файлов (XDND).
  Window dndSource = 0;
  int dndVersion = 0;
  bool dndAccept = false;
  float dndX = 0, dndY = 0;

  // Запоминание положения: последний «обычный» прямоугольник рамки (корневые координаты).
  int normalX = 0, normalY = 0, normalW = 0, normalH = 0;
  bool havePos = false;
};

State g;

// ---------------------------------------------------------------- свойства окна
std::string readProperty(Window w, Atom prop, bool del, Atom* typeOut = nullptr, int* formatOut = nullptr) {
  std::string out;
  long offset = 0;
  Atom type = None;
  int format = 0;
  for (;;) {
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    if (X.GetWindowProperty(g.dpy, w, prop, offset, 1L << 20, False, AnyPropertyType, &type, &format, &n, &after, &data) != Success) break;
    if (data) {
      // Формат 32 Xlib отдаёт массивом long, 16 — short, 8 — байтами.
      size_t bytes = format == 32 ? n * sizeof(long) : format == 16 ? n * sizeof(short) : n;
      out.append(reinterpret_cast<const char*>(data), bytes);
      X.Free(data);
    }
    if (after == 0 || n == 0) break;
    offset += format == 32 ? long(n) : format == 16 ? long(n / 2) : long(n / 4);
  }
  if (del) X.DeleteProperty(g.dpy, w, prop);
  if (typeOut) *typeOut = type;
  if (formatOut) *formatOut = format;
  return out;
}

std::vector<long> readLongs(Window w, Atom prop) {
  int format = 0;
  std::string s = readProperty(w, prop, false, nullptr, &format);
  std::vector<long> r;
  if (format != 32) return r;
  r.resize(s.size() / sizeof(long));
  std::memcpy(r.data(), s.data(), r.size() * sizeof(long));
  return r;
}

void setUtf8Property(Window w, Atom prop, const std::string& s) {
  X.ChangeProperty(g.dpy, w, prop, g.a.UTF8_STRING, 8, PropModeReplace, reinterpret_cast<const unsigned char*>(s.data()), int(s.size()));
}

void sendClientMessage(Window to, Window about, Atom type, long l0, long l1, long l2, long l3, long l4, long mask) {
  XEvent ev{};
  ev.xclient.type = ClientMessage;
  ev.xclient.window = about;
  ev.xclient.message_type = type;
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = l0;
  ev.xclient.data.l[1] = l1;
  ev.xclient.data.l[2] = l2;
  ev.xclient.data.l[3] = l3;
  ev.xclient.data.l[4] = l4;
  X.SendEvent(g.dpy, to, False, mask, &ev);
}

std::string latin1ToUtf8(std::string_view s) {
  std::string out;
  for (char c : s) utf8::append(out, u8(c));
  return out;
}

std::string utf8ToLatin1(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    u32 cp = utf8::decode(s, i);
    out += cp < 256 ? char(cp) : '?';
  }
  return out;
}

// ---------------------------------------------------------------- ожидание событий с тайм-аутом
Bool predicateTrampoline(Display*, XEvent* e, XPointer arg) {
  return (*reinterpret_cast<std::function<bool(const XEvent&)>*>(arg))(*e) ? True : False;
}

// Ждать событие, подходящее под условие; остальные события остаются в очереди.
bool waitEvent(const std::function<bool(const XEvent&)>& pred, double seconds, XEvent& out) {
  double deadline = time() + seconds;
  auto* arg = reinterpret_cast<XPointer>(const_cast<std::function<bool(const XEvent&)>*>(&pred));
  for (;;) {
    X.Flush(g.dpy);
    if (X.CheckIfEvent(g.dpy, &out, predicateTrampoline, arg)) return true;
    double left = deadline - time();
    if (left <= 0) return false;
    pollfd p{g.xfd, kPollIn, 0};
    posix::poll(&p, 1, std::max(1, int(left * 1000)));
  }
}

// ---------------------------------------------------------------- клавиши
Key keyFromSym(KeySym s) {
  if (s >= 'a' && s <= 'z') return Key(int(Key::A) + int(s - 'a'));
  if (s >= 'A' && s <= 'Z') return Key(int(Key::A) + int(s - 'A'));
  if (s >= '0' && s <= '9') return Key(int(Key::D0) + int(s - '0'));
  if (s >= 0xffbe && s <= 0xffc9) return Key(int(Key::F1) + int(s - 0xffbe));
  if (s >= 0xffb0 && s <= 0xffb9) return Key(int(Key::NumPad0) + int(s - 0xffb0));
  switch (s) {
    case 0xff1b: return Key::Escape;
    case 0xff0d: return Key::Enter;
    case 0xff09: case 0xfe20: return Key::Tab;
    case 0xff08: return Key::Backspace;
    case 0xffff: case 0xff9f: return Key::Delete;
    case 0xff63: case 0xff9e: return Key::Insert;
    case 0xff50: case 0xff95: return Key::Home;
    case 0xff57: case 0xff9c: return Key::End;
    case 0xff55: case 0xff9a: return Key::PageUp;
    case 0xff56: case 0xff9b: return Key::PageDown;
    case 0xff51: case 0xff96: return Key::Left;
    case 0xff52: case 0xff97: return Key::Up;
    case 0xff53: case 0xff98: return Key::Right;
    case 0xff54: case 0xff99: return Key::Down;
    case 0x20: return Key::Space;
    case 0x2d: return Key::Minus;
    case 0x3d: return Key::Equal;
    case 0x5b: return Key::LBracket;
    case 0x5d: return Key::RBracket;
    case 0x3b: return Key::Semicolon;
    case 0x27: return Key::Quote;
    case 0x2c: return Key::Comma;
    case 0x2e: return Key::Period;
    case 0x2f: return Key::Slash;
    case 0x5c: return Key::Backslash;
    case 0x60: return Key::Grave;
    case 0xffab: return Key::NumAdd;
    case 0xffad: return Key::NumSub;
    case 0xffaa: return Key::NumMul;
    case 0xffaf: return Key::NumDiv;
    case 0xffae: case 0xffac: return Key::NumDecimal;
    case 0xff8d: return Key::NumEnter;
    case 0xffe1: case 0xffe2: return Key::Shift;
    case 0xffe3: case 0xffe4: return Key::Ctrl;
    case 0xffe9: case 0xffea: case 0xffe7: case 0xffe8: case 0xfe03: return Key::Alt;
    case 0xffeb: case 0xffec: return Key::Super;
    case 0xffe5: return Key::CapsLock;
    case 0xff67: return Key::Menu;
    default: return Key::Unknown;
  }
}

// Положение клавиши по коду evdev (+8) — когда ни одна группа раскладки не латинская.
Key keyFromPosition(unsigned kc) {
  static const Key row1[] = {Key::D1, Key::D2, Key::D3, Key::D4, Key::D5, Key::D6, Key::D7, Key::D8, Key::D9, Key::D0, Key::Minus, Key::Equal};
  static const Key row2[] = {Key::Q, Key::W, Key::E, Key::R, Key::T, Key::Y, Key::U, Key::I, Key::O, Key::P, Key::LBracket, Key::RBracket};
  static const Key row3[] = {Key::A, Key::S, Key::D, Key::F, Key::G, Key::H, Key::J, Key::K, Key::L, Key::Semicolon, Key::Quote, Key::Grave};
  static const Key row4[] = {Key::Z, Key::X, Key::C, Key::V, Key::B, Key::N, Key::M, Key::Comma, Key::Period, Key::Slash};
  if (kc >= 10 && kc <= 21) return row1[kc - 10];
  if (kc >= 24 && kc <= 35) return row2[kc - 24];
  if (kc >= 38 && kc <= 49) return row3[kc - 38];
  if (kc >= 52 && kc <= 61) return row4[kc - 52];
  if (kc == 51 || kc == 94) return Key::Backslash;
  return Key::Unknown;
}

// Клавиша не зависит от раскладки: латинская группа раскладки, иначе положение клавиши.
Key keyFor(const XKeyEvent& e) {
  KeyCode kc = KeyCode(e.keycode);
  KeySym base = X.KbKeycodeToKeysym(g.dpy, kc, 0, 0);
  if (base >= 0xff80 && base <= 0xffbd) {  // цифровой блок зависит от NumLock
    KeySym s = X.KbKeycodeToKeysym(g.dpy, kc, 0, (e.state & Mod2Mask) ? 1 : 0);
    return keyFromSym(s ? s : base);
  }
  Key k = keyFromSym(base);
  if (k != Key::Unknown) return k;
  for (int group = 1; group < 4; group++) {
    KeySym s = X.KbKeycodeToKeysym(g.dpy, kc, group, 0);
    if (s >= 0x20 && s < 0x7f) {
      k = keyFromSym(s);
      if (k != Key::Unknown) return k;
    }
  }
  return keyFromPosition(e.keycode);
}

// Keysym → Unicode (если метод ввода недоступен): Latin-1, кириллица, прямые коды 0x01000000+.
u32 keysymToUnicode(KeySym s) {
  static const char16_t cyr6a1[] = u"ђѓёєѕіїјљњћќґўџ№ЂЃЁЄЅІЇЈЉЊЋЌҐЎЏ";
  static const char16_t cyr6c0[] = u"юабцдефгхийклмнопярстужвьызшэщчъ";
  if ((s >= 0x20 && s <= 0x7e) || (s >= 0xa0 && s <= 0xff)) return u32(s);
  if (s >= 0x01000100 && s <= 0x0110ffff) return u32(s - 0x01000000);
  if (s >= 0x6a1 && s <= 0x6bf) return cyr6a1[s - 0x6a1];
  if (s >= 0x6c0 && s <= 0x6df) return cyr6c0[s - 0x6c0];
  if (s >= 0x6e0 && s <= 0x6ff) return u32(cyr6c0[s - 0x6e0]) - 0x20;  // заглавные: U+0410–U+042F
  return 0;
}

u32 modsFromState(unsigned state) {
  u32 m = 0;
  if (state & ShiftMask) m |= ModShift;
  if (state & ControlMask) m |= ModCtrl;
  if (state & Mod1Mask) m |= ModAlt;
  if (state & Mod4Mask) m |= ModSuper;
  return m;
}

// ---------------------------------------------------------------- очередь и вызовы приложения
void push(Event e) {
  if (e.type == EventType::Text && !g.queue.empty() && g.queue.back().type == EventType::Text) {
    g.queue.back().text += e.text;
    return;
  }
  g.queue.push_back(std::move(e));
}

template <class F>
void guarded(F&& f) {
  g.callbackDepth++;
  try {
    f();
  } catch (...) {
    if (!g.error) g.error = std::current_exception();
    g.quitting = true;
  }
  g.callbackDepth--;
}

void flushEvents() {
  if (!g.app || !g.ready || g.callbackDepth > 0) return;
  while (!g.queue.empty() && !g.quitting) {
    Event e = std::move(g.queue.front());
    g.queue.pop_front();
    g.dirty = true;  // каждое доставленное событие — новый кадр
    guarded([&] { g.app->onEvent(e); });
  }
}

bool callAnimating() {
  bool anim = false;
  if (g.app && g.callbackDepth == 0) guarded([&] { anim = g.app->animating(); });
  return anim;
}

void requestClose() {
  if (!g.app || g.quitting) return;
  if (g.callbackDepth > 0) {
    g.closePending = true;
    return;
  }
  flushEvents();
  bool ok = false;
  guarded([&] { ok = g.app->onCloseRequest(); });
  if (ok && !g.quitting) {
    g.quitting = true;
    g.exitCode = 0;
  }
}

// ---------------------------------------------------------------- кадр и вывод
void putImage(int x, int y, int w, int h) {
  if (!g.win || g.buf.empty() || w <= 0 || h <= 0) return;
  x = std::max(0, x);
  y = std::max(0, y);
  w = std::min(w, g.bw - x);
  h = std::min(h, g.bh - y);
  if (w <= 0 || h <= 0) return;
  XImage img{};
  img.width = g.bw;
  img.height = g.bh;
  img.xoffset = 0;
  img.format = ZPixmap;
  img.data = reinterpret_cast<char*>(g.buf.data());
  img.byte_order = std::endian::native == std::endian::little ? LSBFirst : MSBFirst;  // u32 0xAARRGGBB в памяти
  img.bitmap_unit = 32;
  img.bitmap_bit_order = img.byte_order;
  img.bitmap_pad = 32;
  img.depth = 24;
  img.bytes_per_line = g.bw * 4;
  img.bits_per_pixel = 32;
  img.red_mask = 0xFF0000;
  img.green_mask = 0x00FF00;
  img.blue_mask = 0x0000FF;
  if (!X.InitImage(&img)) return;
  X.PutImage(g.dpy, g.win, g.gc, &img, x, y, x, y, unsigned(w), unsigned(h));
}

bool canRender() { return g.app && g.ready && g.mapped && g.callbackDepth == 0 && g.cw > 0 && g.ch > 0; }

void renderNow() {
  if (!canRender()) return;
  flushEvents();
  if (g.quitting || !canRender()) return;
  if (g.bw != g.cw || g.bh != g.ch) {
    g.buf.assign(size_t(g.cw) * size_t(g.ch), 0xFF0E1117u);
    g.bw = g.cw;
    g.bh = g.ch;
    g.bufferReset = true;
  }
  g.dirty = false;
  Frame f;
  f.px = g.buf.data();
  f.w = g.bw;
  f.h = g.bh;
  f.stride = g.bw;
  f.scale = g.scale;
  f.reset = g.bufferReset;
  f.index = g.frameIndex++;
  guarded([&] { g.app->onFrame(f); });
  g.bufferReset = false;
  putImage(0, 0, g.bw, g.bh);
  X.Flush(g.dpy);
  g.exposed = false;
}

// ---------------------------------------------------------------- масштаб (Xft.dpi, GDK_SCALE)
float readScale(bool& fromEnv) {
  fromEnv = false;
  if (const char* e = std::getenv("GDK_SCALE")) {
    int v = std::atoi(e);
    if (v >= 1 && v <= 4) {
      fromEnv = true;
      return float(v);
    }
  }
  std::string res = readProperty(g.root, g.a.RESOURCE_MANAGER, false);
  for (const std::string& line : split(res, '\n')) {
    std::string l = trim(line);
    if (!startsWith(l, "Xft.dpi:")) continue;
    if (auto v = parseNum(trim(l.substr(8)))) {
      if (*v >= 48 && *v <= 480) return float(*v / 96.0);
    }
  }
  return 1;
}

// ---------------------------------------------------------------- курсоры
XCursor cursorFor(Cursor c) {
  size_t i = size_t(c);
  if (g.cursors[i]) return g.cursors[i];
  static const char* const names[] = {"default", "pointer", "text", "crosshair", "move", "ew-resize", "ns-resize",
                                      "nwse-resize", "nesw-resize", "grab", "grabbing", "not-allowed", "wait"};
  static const char* const legacy[] = {"left_ptr", "hand2", "xterm", "crosshair", "fleur", "sb_h_double_arrow", "sb_v_double_arrow",
                                       "bottom_right_corner", "bottom_left_corner", "openhand", "closedhand", "crossed_circle", "watch"};
  static const unsigned font[] = {XC_left_ptr, XC_hand2, XC_xterm, XC_crosshair, XC_fleur, XC_sb_h_double_arrow, XC_sb_v_double_arrow,
                                  XC_bottom_right_corner, XC_bottom_left_corner, XC_hand1, XC_fleur, XC_circle, XC_watch};
  XCursor cur = None;
  if (X.CursorLibraryLoadCursor) {
    cur = X.CursorLibraryLoadCursor(g.dpy, names[i]);
    if (!cur) cur = X.CursorLibraryLoadCursor(g.dpy, legacy[i]);
  }
  if (!cur) cur = X.CreateFontCursor(g.dpy, font[i]);
  g.cursors[i] = cur;
  return cur;
}

// ---------------------------------------------------------------- состояние окна (_NET_WM_STATE)
void readWmState() {
  bool fs = false, mv = false, mh = false;
  for (long v : readLongs(g.win, g.a.NET_WM_STATE)) {
    Atom at = Atom(v);
    if (at == g.a.NET_WM_STATE_FULLSCREEN) fs = true;
    if (at == g.a.NET_WM_STATE_MAXIMIZED_VERT) mv = true;
    if (at == g.a.NET_WM_STATE_MAXIMIZED_HORZ) mh = true;
  }
  g.fullscreen = fs;
  g.maximized = mv && mh;
}

void setWmState(Atom state, bool on) {
  if (g.mapped) {
    // _NET_WM_STATE: 0 — снять, 1 — добавить; источник 1 — обычное приложение.
    sendClientMessage(g.root, g.win, g.a.NET_WM_STATE, on ? 1 : 0, long(state), 0, 1, 0, SubstructureNotifyMask | SubstructureRedirectMask);
  } else {
    std::vector<long> list;
    for (long v : readLongs(g.win, g.a.NET_WM_STATE))
      if (Atom(v) != state) list.push_back(v);
    if (on) list.push_back(long(state));
    X.ChangeProperty(g.dpy, g.win, g.a.NET_WM_STATE, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(list.data()),
                     int(list.size()));
  }
  X.Flush(g.dpy);
}

void applyDarkFrame() {
  if (!g.win) return;
  setUtf8Property(g.win, g.a.GTK_THEME_VARIANT, g.dark ? "dark" : "light");
  X.Flush(g.dpy);
}

void applyTitle() {
  if (!g.win) return;
  setUtf8Property(g.win, g.a.NET_WM_NAME, g.title);
  setUtf8Property(g.win, g.a.NET_WM_ICON_NAME, g.title);
  X.StoreName(g.dpy, g.win, utf8ToLatin1(g.title).c_str());  // для старых диспетчеров окон
  X.Flush(g.dpy);
}

// ---------------------------------------------------------------- положение окна
std::string userDataDir(const std::string& appName) {
  std::string name = utf8::lower(appName);
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x == '/') return std::string(x) + "/" + name;
  if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.config/" + name;
  return {};
}

void trackNormalRect(const XConfigureEvent& c) {
  if (g.fullscreen || g.maximized) return;
  g.normalW = c.width;
  g.normalH = c.height;
  if (c.send_event) {  // синтетический ConfigureNotify от диспетчера окон — корневые координаты
    std::vector<long> ext = readLongs(g.win, g.a.NET_FRAME_EXTENTS);
    long left = ext.size() == 4 ? ext[0] : 0, top = ext.size() == 4 ? ext[2] : 0;
    g.normalX = c.x - int(left);
    g.normalY = c.y - int(top);
    g.havePos = true;
  }
}

void savePlacementNow() {
  if (!g.win || g.normalW <= 0) return;
  std::string path = detail::placementPath(g.cfg, userDataDir(g.cfg.appName));
  if (path.empty()) return;
  readWmState();
  detail::Placement p;
  p.x = g.havePos ? g.normalX : 0;
  p.y = g.havePos ? g.normalY : 0;
  p.w = g.normalW;
  p.h = g.normalH;
  p.maximized = g.maximized;
  p.dpi = int(std::lround(g.scale * 96));
  if (!detail::savePlacement(path, p)) logWarn("Не удалось сохранить положение окна: %s", path.c_str());
}

// ---------------------------------------------------------------- буфер обмена
void serveSelectionRequest(const XSelectionRequestEvent& r) {
  XEvent reply{};
  reply.xselection.type = SelectionNotify;
  reply.xselection.requestor = r.requestor;
  reply.xselection.selection = r.selection;
  reply.xselection.target = r.target;
  reply.xselection.time = r.time;
  reply.xselection.property = None;
  Atom prop = r.property != None ? r.property : r.target;  // устаревшие клиенты не указывают свойство
  const Atoms& a = g.a;
  if (r.selection == a.CLIPBOARD && g.ownsClipboard) {
    if (r.target == a.TARGETS) {
      long targets[] = {long(a.TARGETS), long(a.UTF8_STRING), long(a.TEXT_PLAIN_UTF8), long(a.TEXT_PLAIN), long(XA_STRING), long(a.TEXT)};
      X.ChangeProperty(g.dpy, r.requestor, prop, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(targets), 6);
      reply.xselection.property = prop;
    } else if (r.target == a.UTF8_STRING || r.target == a.TEXT_PLAIN_UTF8 || r.target == a.TEXT || r.target == XA_STRING ||
               r.target == a.TEXT_PLAIN) {
      bool latin = r.target == XA_STRING || r.target == a.TEXT_PLAIN;
      std::string data = latin ? utf8ToLatin1(g.clipText) : g.clipText;
      Atom type = r.target == a.TEXT ? a.UTF8_STRING : r.target;
      if (data.size() > (256u << 10)) {
        // Большой текст — порциями (INCR): получатель удаляет свойство, мы пишем следующую порцию.
        long size = long(data.size());
        X.SelectInput(g.dpy, r.requestor, PropertyChangeMask);
        X.ChangeProperty(g.dpy, r.requestor, prop, a.INCR, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&size), 1);
        g.incr.push_back({r.requestor, prop, type, std::move(data), 0});
      } else {
        X.ChangeProperty(g.dpy, r.requestor, prop, type, 8, PropModeReplace, reinterpret_cast<const unsigned char*>(data.data()), int(data.size()));
      }
      reply.xselection.property = prop;
    }
  }
  X.SendEvent(g.dpy, r.requestor, False, NoEventMask, &reply);
  X.Flush(g.dpy);
}

bool continueIncr(const XPropertyEvent& p) {
  for (size_t i = 0; i < g.incr.size(); i++) {
    IncrTransfer& t = g.incr[i];
    if (t.requestor != p.window || t.property != p.atom || p.state != PropertyDelete) continue;
    size_t n = std::min<size_t>(64u << 10, t.data.size() - t.offset);
    X.ChangeProperty(g.dpy, t.requestor, t.property, t.type, 8, PropModeReplace,
                     reinterpret_cast<const unsigned char*>(t.data.data() + t.offset), int(n));
    t.offset += n;
    if (n == 0) {  // нулевая порция — конец передачи
      X.SelectInput(g.dpy, t.requestor, NoEventMask);
      g.incr.erase(g.incr.begin() + long(i));
    }
    X.Flush(g.dpy);
    return true;
  }
  return false;
}

std::string receiveSelection(Atom selection, Atom target, Atom* typeOut) {
  X.DeleteProperty(g.dpy, g.win, g.a.RG_SELECTION);
  X.ConvertSelection(g.dpy, selection, target, g.a.RG_SELECTION, g.win, g.lastTime);
  XEvent ev{};
  Window w = g.win;
  bool got = waitEvent([&](const XEvent& e) { return e.type == SelectionNotify && e.xselection.requestor == w && e.xselection.selection == selection; },
                       1.5, ev);
  *typeOut = None;
  if (!got || ev.xselection.property == None) return {};
  Atom type = None;
  std::string data = readProperty(g.win, g.a.RG_SELECTION, true, &type);
  if (type == g.a.INCR) {  // по частям: каждое удаление свойства просит следующую порцию
    data.clear();
    for (;;) {
      XEvent pe{};
      Atom prop = g.a.RG_SELECTION;
      if (!waitEvent([&](const XEvent& e) {
            return e.type == PropertyNotify && e.xproperty.window == w && e.xproperty.atom == prop && e.xproperty.state == PropertyNewValue;
          }, 1.5, pe))
        break;
      Atom chunkType = None;
      std::string chunk = readProperty(g.win, g.a.RG_SELECTION, true, &chunkType);
      if (chunk.empty()) break;
      type = chunkType;
      data += chunk;
    }
  }
  *typeOut = type;
  return data;
}

// Передать текст менеджеру буфера обмена, чтобы он пережил закрытие программы.
void handOffClipboard() {
  if (!g.ownsClipboard || X.GetSelectionOwner(g.dpy, g.a.CLIPBOARD) != g.win) return;
  if (!X.GetSelectionOwner(g.dpy, g.a.CLIPBOARD_MANAGER)) return;
  X.ConvertSelection(g.dpy, g.a.CLIPBOARD_MANAGER, g.a.SAVE_TARGETS, None, g.win, g.lastTime);
  double deadline = time() + 1.0;
  Window w = g.win;
  Atom mgr = g.a.CLIPBOARD_MANAGER;
  while (time() < deadline) {
    XEvent ev{};
    if (!waitEvent([&](const XEvent& e) {
          return e.type == SelectionRequest || (e.type == SelectionNotify && e.xselection.requestor == w && e.xselection.selection == mgr) ||
                 e.type == PropertyNotify;
        }, deadline - time(), ev))
      break;
    if (ev.type == SelectionRequest) serveSelectionRequest(ev.xselectionrequest);
    else if (ev.type == PropertyNotify) continueIncr(ev.xproperty);
    else break;
  }
}

// ---------------------------------------------------------------- перетаскивание файлов (XDND)
std::vector<std::string> parseUriList(std::string_view s) {
  std::vector<std::string> files;
  for (const std::string& raw : split(s, '\n')) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#' || !startsWith(line, "file://")) continue;
    std::string rest = line.substr(7);
    size_t slash = rest.find('/');  // file://host/путь
    if (slash == std::string::npos) continue;
    rest = rest.substr(slash);
    std::string path;
    for (size_t i = 0; i < rest.size(); i++) {
      auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
      if (rest[i] == '%' && i + 2 < rest.size() && hex(rest[i + 1]) >= 0 && hex(rest[i + 2]) >= 0) {
        path += char(hex(rest[i + 1]) * 16 + hex(rest[i + 2]));
        i += 2;
      } else {
        path += rest[i];
      }
    }
    if (!path.empty()) files.push_back(path);
  }
  return files;
}

void onDndEnter(const XClientMessageEvent& c) {
  g.dndSource = Window(c.data.l[0]);
  g.dndVersion = int((unsigned long)c.data.l[1] >> 24);
  g.dndAccept = false;
  std::vector<long> types;
  if (c.data.l[1] & 1) types = readLongs(g.dndSource, g.a.XdndTypeList);
  else types = {c.data.l[2], c.data.l[3], c.data.l[4]};
  for (long t : types)
    if (Atom(t) == g.a.TEXT_URI_LIST) g.dndAccept = true;
}

void onDndPosition(const XClientMessageEvent& c) {
  int rx = int((unsigned long)c.data.l[2] >> 16), ry = int(c.data.l[2] & 0xFFFF), wx = 0, wy = 0;
  Window child = None;
  X.TranslateCoordinates(g.dpy, g.root, g.win, rx, ry, &wx, &wy, &child);
  g.dndX = float(wx) / g.scale;
  g.dndY = float(wy) / g.scale;
  // Принимаем копирование; пустой прямоугольник — присылать новые позиции.
  sendClientMessage(Window(c.data.l[0]), g.win, g.a.XdndStatus, long(g.win), g.dndAccept ? 1 : 0, 0, 0,
                    g.dndAccept ? long(g.a.XdndActionCopy) : 0, NoEventMask);
  X.Flush(g.dpy);
}

void onDndDrop(const XClientMessageEvent& c) {
  Window source = Window(c.data.l[0]);
  if (!g.dndAccept) {
    sendClientMessage(source, g.win, g.a.XdndFinished, long(g.win), 0, 0, 0, 0, NoEventMask);
    X.Flush(g.dpy);
    return;
  }
  Time t = g.dndVersion >= 1 ? Time(c.data.l[2]) : g.lastTime;
  X.ConvertSelection(g.dpy, g.a.XdndSelection, g.a.TEXT_URI_LIST, g.a.XdndSelection, g.win, t);
  X.Flush(g.dpy);
}

void onDndData(const XSelectionEvent& s) {
  std::vector<std::string> files;
  if (s.property != None) files = parseUriList(readProperty(g.win, g.a.XdndSelection, true));
  bool ok = !files.empty();
  if (ok) {
    Event e;
    e.type = EventType::FilesDropped;
    e.x = g.dndX;
    e.y = g.dndY;
    e.files = std::move(files);
    push(std::move(e));
  }
  if (g.dndSource && g.dndVersion >= 2)
    sendClientMessage(g.dndSource, g.win, g.a.XdndFinished, long(g.win), ok ? 1 : 0, long(g.a.XdndActionCopy), 0, 0, NoEventMask);
  g.dndSource = None;
  X.Flush(g.dpy);
}

// ---------------------------------------------------------------- обработка событий X
void releaseKeys() {
  for (size_t k = 0; k < g.keyDown.size(); k++) {
    if (!g.keyDown[k]) continue;
    g.keyDown[k] = false;
    Event e;
    e.type = EventType::KeyUp;
    e.key = Key(k);
    push(std::move(e));
  }
  g.keycodeDown.fill(false);
}

void onKey(XEvent& ev) {
  XKeyEvent& k = ev.xkey;
  g.lastTime = k.time;
  bool press = ev.type == KeyPress;
  unsigned kc = k.keycode & 0xFF;
  if (!press && !g.detectableRepeat && X.EventsQueued(g.dpy, QueuedAfterReading)) {
    // Без «обнаруживаемого автоповтора» повтор — пара KeyRelease+KeyPress с одним временем.
    XEvent next{};
    X.PeekEvent(g.dpy, &next);
    if (next.type == KeyPress && next.xkey.keycode == k.keycode && next.xkey.time == k.time) return;
  }
  Key key = keyFor(k);
  u32 mods = modsFromState(k.state);
  if (key != Key::Unknown) {
    Event e;
    e.type = press ? EventType::KeyDown : EventType::KeyUp;
    e.key = key;
    e.mods = mods;
    e.repeat = press && g.keycodeDown[kc];
    g.keyDown[size_t(key)] = press;
    push(std::move(e));
  }
  g.keycodeDown[kc] = press;
  if (!press) return;
  // Текст: метод ввода (XIM) или прямое преобразование keysym. Сочетания с Ctrl/Alt/Super текста не дают.
  if (k.state & (ControlMask | Mod1Mask | Mod4Mask)) return;
  std::string text;
  if (g.ic) {
    char small[64];
    KeySym sym = 0;
    Status st = 0;
    int n = X.Utf8LookupString(g.ic, &k, small, int(sizeof small), &sym, &st);
    if (st == XBufferOverflow) {
      std::string big(size_t(n) + 1, '\0');
      n = X.Utf8LookupString(g.ic, &k, big.data(), n + 1, &sym, &st);
      if (st == XLookupChars || st == XLookupBoth) text.assign(big.data(), size_t(std::max(0, n)));
    } else if (st == XLookupChars || st == XLookupBoth) {
      text.assign(small, size_t(std::max(0, n)));
    }
  } else {
    char buf[32];
    KeySym sym = 0;
    X.LookupString(&k, buf, int(sizeof buf), &sym, nullptr);
    if (u32 cp = keysymToUnicode(sym)) text = utf8::encode(cp);
  }
  text = detail::filterText(text);
  if (!text.empty()) {
    Event e;
    e.type = EventType::Text;
    e.text = std::move(text);
    e.mods = mods;
    push(std::move(e));
  }
}

void onButton(XEvent& ev) {
  XButtonEvent& b = ev.xbutton;
  g.lastTime = b.time;
  bool press = ev.type == ButtonPress;
  float x = float(b.x) / g.scale, y = float(b.y) / g.scale;
  u32 mods = modsFromState(b.state);
  if (b.button >= 4 && b.button <= 7) {  // колесо: 4/5 — вверх/вниз, 6/7 — влево/вправо
    if (!press) return;
    Event e;
    e.type = EventType::MouseWheel;
    e.x = x;
    e.y = y;
    e.mods = mods;
    if (b.button == 4) e.wheelY = 1;
    else if (b.button == 5) e.wheelY = -1;
    else if (b.button == 6) e.wheelX = -1;
    else e.wheelX = 1;
    push(std::move(e));
    return;
  }
  int button = b.button == 1 ? MouseLeft : b.button == 3 ? MouseRight : b.button == 2 ? MouseMiddle : b.button == 8 ? MouseBack
             : b.button == 9 ? MouseForward : -1;
  if (button < 0) return;
  Event e;
  e.type = press ? EventType::MouseDown : EventType::MouseUp;
  e.x = x;
  e.y = y;
  e.button = button;
  e.mods = mods;
  g.mx = x;
  g.my = y;
  if (press) {
    g.buttonsDown |= 1 << button;
    e.clicks = g.clicks.press(button, x, y, double(b.time) / 1000.0, 0.4, 4.f / g.scale);
    g.lastClicks[size_t(button)] = e.clicks;
  } else {
    if (!(g.buttonsDown & (1 << button))) return;
    g.buttonsDown &= ~(1 << button);
    e.clicks = g.lastClicks[size_t(button)];
  }
  push(std::move(e));
  if (!press && g.buttonsDown == 0 && (b.x < 0 || b.y < 0 || b.x >= g.cw || b.y >= g.ch)) {
    Event leave;  // перетаскивание закончилось за пределами окна
    leave.type = EventType::MouseLeave;
    leave.x = x;
    leave.y = y;
    push(std::move(leave));
  }
}

void onClientMessage(const XClientMessageEvent& c) {
  const Atoms& a = g.a;
  if (c.message_type == a.WM_PROTOCOLS) {
    Atom proto = Atom(c.data.l[0]);
    if (proto == a.WM_DELETE_WINDOW) {
      requestClose();
    } else if (proto == a.NET_WM_PING) {  // «программа отвечает»
      XEvent reply{};
      reply.xclient = c;
      reply.xclient.window = g.root;
      X.SendEvent(g.dpy, g.root, False, SubstructureNotifyMask | SubstructureRedirectMask, &reply);
      X.Flush(g.dpy);
    }
  } else if (c.message_type == a.XdndEnter) {
    onDndEnter(c);
  } else if (c.message_type == a.XdndPosition) {
    onDndPosition(c);
  } else if (c.message_type == a.XdndDrop) {
    onDndDrop(c);
  } else if (c.message_type == a.XdndLeave) {
    g.dndSource = None;
    g.dndAccept = false;
  }
}

void handleEvent(XEvent& ev) {
  switch (ev.type) {
    case KeyPress:
    case KeyRelease:
      onKey(ev);
      break;
    case ButtonPress:
    case ButtonRelease:
      onButton(ev);
      break;
    case MotionNotify: {
      const XMotionEvent& m = ev.xmotion;
      g.lastTime = m.time;
      Event e;
      e.type = EventType::MouseMove;
      e.x = float(m.x) / g.scale;
      e.y = float(m.y) / g.scale;
      e.mods = modsFromState(m.state);
      g.mx = e.x;
      g.my = e.y;
      g.mouseInside = m.x >= 0 && m.y >= 0 && m.x < g.cw && m.y < g.ch;
      push(std::move(e));
      break;
    }
    case EnterNotify:
      g.mouseInside = true;
      break;
    case LeaveNotify:
      if (ev.xcrossing.mode != NotifyNormal) break;
      g.mouseInside = false;
      if (g.buttonsDown == 0) {
        Event e;
        e.type = EventType::MouseLeave;
        e.x = float(ev.xcrossing.x) / g.scale;
        e.y = float(ev.xcrossing.y) / g.scale;
        push(std::move(e));
      }
      break;
    case FocusIn:
    case FocusOut: {
      int mode = ev.xfocus.mode;
      if (mode == NotifyGrab || mode == NotifyUngrab) break;  // захваты клавиатуры диспетчером окон
      Event e;
      if (ev.type == FocusIn) {
        if (g.ic) X.SetICFocus(g.ic);
        e.type = EventType::FocusIn;
      } else {
        if (g.ic) X.UnsetICFocus(g.ic);
        releaseKeys();
        e.type = EventType::FocusOut;
      }
      push(std::move(e));
      break;
    }
    case Expose: {
      const XExposeEvent& x = ev.xexpose;
      if (!g.exposed) {
        g.exX0 = x.x;
        g.exY0 = x.y;
        g.exX1 = x.x + x.width;
        g.exY1 = x.y + x.height;
        g.exposed = true;
      } else {
        g.exX0 = std::min(g.exX0, x.x);
        g.exY0 = std::min(g.exY0, x.y);
        g.exX1 = std::max(g.exX1, x.x + x.width);
        g.exY1 = std::max(g.exY1, x.y + x.height);
      }
      if (x.count == 0 && g.exposed && !g.dirty && g.bw == g.cw && g.bh == g.ch) {
        putImage(g.exX0, g.exY0, g.exX1 - g.exX0, g.exY1 - g.exY0);  // повторный вывод готового кадра
        X.Flush(g.dpy);
        g.exposed = false;
      } else if (x.count == 0) {
        g.dirty = true;
      }
      break;
    }
    case ConfigureNotify: {
      const XConfigureEvent& c = ev.xconfigure;
      if (c.window != g.win) break;
      trackNormalRect(c);
      if (c.width != g.cw || c.height != g.ch) {
        g.cw = c.width;
        g.ch = c.height;
        Event e;
        e.type = EventType::Resize;
        e.width = float(g.cw) / g.scale;
        e.height = float(g.ch) / g.scale;
        e.scale = g.scale;
        push(std::move(e));
        g.dirty = true;
      }
      break;
    }
    case MapNotify:
      if (ev.xany.window == g.win) {
        g.mapped = true;
        g.dirty = true;
      }
      break;
    case UnmapNotify:
      if (ev.xany.window == g.win) g.mapped = false;
      break;
    case PropertyNotify: {
      const XPropertyEvent& p = ev.xproperty;
      g.lastTime = p.time;
      if (continueIncr(p)) break;
      if (p.window == g.win && p.atom == g.a.NET_WM_STATE) {
        readWmState();
      } else if (p.window == g.root && p.atom == g.a.RESOURCE_MANAGER && !g.envScale) {
        bool env = false;
        float s = readScale(env);
        if (std::fabs(s - g.scale) > 1e-3f) {  // Xft.dpi изменился (настройки рабочего стола)
          g.scale = s;
          Event e;
          e.type = EventType::ScaleChanged;
          e.width = float(g.cw) / g.scale;
          e.height = float(g.ch) / g.scale;
          e.scale = g.scale;
          push(std::move(e));
          g.dirty = true;
        }
      }
      break;
    }
    case SelectionRequest:
      serveSelectionRequest(ev.xselectionrequest);
      break;
    case SelectionClear:
      if (ev.xselectionclear.selection == g.a.CLIPBOARD) {
        g.ownsClipboard = false;
        g.clipText.clear();
      }
      break;
    case SelectionNotify:
      if (ev.xselection.selection == g.a.XdndSelection) onDndData(ev.xselection);
      break;
    case ClientMessage:
      onClientMessage(ev.xclient);
      break;
    case MappingNotify:
      X.RefreshKeyboardMapping(&ev.xmapping);
      break;
    default:
      break;
  }
}

void drainX() {
  while (X.Pending(g.dpy)) {
    XEvent ev{};
    X.NextEvent(g.dpy, &ev);
    if (X.FilterEvent(&ev, None)) continue;  // событие забрал метод ввода
    handleEvent(ev);
  }
}

void drainPipe() {
  char buf[64];
  pollfd p{g.pipeR, kPollIn, 0};
  bool any = false;
  while (posix::poll(&p, 1, 0) > 0 && (p.revents & kPollIn)) {
    if (posix::read(g.pipeR, buf, sizeof buf) <= 0) break;
    any = true;
  }
  if (!any) return;
  g.wakePosted = false;
  if (g.wakeEvent.exchange(false)) {
    Event e;
    e.type = EventType::Wake;
    push(std::move(e));
  }
  g.dirty = true;
}

void openInputMethod() {
  // XIM работает в кодировке локали: временно включаем локаль среды, если программа в «C».
  const char* cur = std::setlocale(LC_CTYPE, nullptr);
  std::string saved = cur ? cur : "C";
  bool changed = false;
  if (saved == "C" || saved == "POSIX") changed = std::setlocale(LC_CTYPE, "") || std::setlocale(LC_CTYPE, "C.UTF-8");
  if (X.SupportsLocale()) {
    X.SetLocaleModifiers("");
    g.im = X.OpenIM(g.dpy, nullptr, nullptr, nullptr);
    if (!g.im) {
      X.SetLocaleModifiers("@im=none");
      g.im = X.OpenIM(g.dpy, nullptr, nullptr, nullptr);
    }
  }
  if (changed) std::setlocale(LC_CTYPE, saved.c_str());
  if (!g.im) return;
  g.ic = X.CreateIC(g.im, "inputStyle", XIMStyle(XIMPreeditNothing | XIMStatusNothing), "clientWindow", g.win, "focusWindow", g.win,
                    static_cast<void*>(nullptr));
  if (!g.ic) logWarn("X11: метод ввода не поддерживает нужный стиль; текст — без IME");
}

Atom atom(const char* name) { return X.InternAtom(g.dpy, name, False); }

void createWindow() {
  Atoms& a = g.a;
  a.WM_PROTOCOLS = atom("WM_PROTOCOLS");
  a.WM_DELETE_WINDOW = atom("WM_DELETE_WINDOW");
  a.NET_WM_PING = atom("_NET_WM_PING");
  a.NET_WM_NAME = atom("_NET_WM_NAME");
  a.NET_WM_ICON_NAME = atom("_NET_WM_ICON_NAME");
  a.NET_WM_PID = atom("_NET_WM_PID");
  a.UTF8_STRING = atom("UTF8_STRING");
  a.CLIPBOARD = atom("CLIPBOARD");
  a.TARGETS = atom("TARGETS");
  a.TEXT = atom("TEXT");
  a.INCR = atom("INCR");
  a.SAVE_TARGETS = atom("SAVE_TARGETS");
  a.CLIPBOARD_MANAGER = atom("CLIPBOARD_MANAGER");
  a.TEXT_PLAIN_UTF8 = atom("text/plain;charset=utf-8");
  a.TEXT_PLAIN = atom("text/plain");
  a.NET_WM_STATE = atom("_NET_WM_STATE");
  a.NET_WM_STATE_FULLSCREEN = atom("_NET_WM_STATE_FULLSCREEN");
  a.NET_WM_STATE_MAXIMIZED_VERT = atom("_NET_WM_STATE_MAXIMIZED_VERT");
  a.NET_WM_STATE_MAXIMIZED_HORZ = atom("_NET_WM_STATE_MAXIMIZED_HORZ");
  a.NET_FRAME_EXTENTS = atom("_NET_FRAME_EXTENTS");
  a.GTK_THEME_VARIANT = atom("_GTK_THEME_VARIANT");
  a.RESOURCE_MANAGER = atom("RESOURCE_MANAGER");
  a.RG_SELECTION = atom("RG_SELECTION");
  a.XdndAware = atom("XdndAware");
  a.XdndEnter = atom("XdndEnter");
  a.XdndPosition = atom("XdndPosition");
  a.XdndStatus = atom("XdndStatus");
  a.XdndLeave = atom("XdndLeave");
  a.XdndDrop = atom("XdndDrop");
  a.XdndFinished = atom("XdndFinished");
  a.XdndSelection = atom("XdndSelection");
  a.XdndActionCopy = atom("XdndActionCopy");
  a.XdndTypeList = atom("XdndTypeList");
  a.TEXT_URI_LIST = atom("text/uri-list");

  // 24-битный TrueColor с 32 битами на пиксель — формат BGRX, совпадает с нашим буфером.
  XVisualInfo vi{};
  if (!X.MatchVisualInfo(g.dpy, g.screen, 24, TrueColor, &vi) || vi.red_mask != 0xFF0000 || vi.green_mask != 0xFF00 || vi.blue_mask != 0xFF)
    throw std::runtime_error("X-сервер не поддерживает 24-битный цвет TrueColor (RGB 8:8:8)");
  int nf = 0;
  bool bpp32 = false;
  if (XPixmapFormatValues* f = X.ListPixmapFormats(g.dpy, &nf)) {
    for (int i = 0; i < nf; i++)
      if (f[i].depth == 24 && f[i].bits_per_pixel == 32) bpp32 = true;
    X.Free(f);
  }
  if (!bpp32) throw std::runtime_error("X-сервер хранит 24-битный цвет не в 32 битах на пиксель");
  g.visual = vi.visual;
  g.colormap = X.CreateColormap(g.dpy, g.root, g.visual, AllocNone);

  bool env = false;
  g.scale = readScale(env);
  g.envScale = env;

  // Размер и положение: сохранённые или из настроек.
  int w = int(std::lround(float(g.cfg.width) * g.scale)), h = int(std::lround(float(g.cfg.height) * g.scale));
  int x = 0, y = 0;
  bool havePos = false, maximize = g.cfg.maximized;
  int rootW = X.DisplayWidth(g.dpy, g.screen), rootH = X.DisplayHeight(g.dpy, g.screen);
  if (auto p = detail::loadPlacement(detail::placementPath(g.cfg, userDataDir(g.cfg.appName)))) {
    int dpi = int(std::lround(g.scale * 96));
    w = p->dpi == dpi ? p->w : p->w * dpi / p->dpi;
    h = p->dpi == dpi ? p->h : p->h * dpi / p->dpi;
    maximize = p->maximized;
    if (p->x > -w + 64 && p->y >= 0 && p->x < rootW - 64 && p->y < rootH - 32) {
      x = p->x;
      y = p->y;
      havePos = true;
    }
  }
  w = std::clamp(w, 64, std::max(64, rootW));
  h = std::clamp(h, 64, std::max(64, rootH));

  XSetWindowAttributes attrs{};
  attrs.background_pixel = 0x0E1117;   // цвет фона темы — без белой вспышки
  attrs.border_pixel = 0;
  attrs.colormap = g.colormap;
  attrs.bit_gravity = NorthWestGravity;
  attrs.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | EnterWindowMask | LeaveWindowMask |
                     PointerMotionMask | ExposureMask | StructureNotifyMask | FocusChangeMask | PropertyChangeMask;
  g.win = X.CreateWindow(g.dpy, g.root, x, y, unsigned(w), unsigned(h), 0, 24, InputOutput, g.visual,
                         CWBackPixel | CWBorderPixel | CWColormap | CWEventMask | CWBitGravity, &attrs);
  if (!g.win) throw std::runtime_error("Не удалось создать окно X11");
  g.cw = w;
  g.ch = h;
  g.normalW = w;
  g.normalH = h;
  g.normalX = x;
  g.normalY = y;
  g.havePos = havePos;

  Atom protocols[] = {a.WM_DELETE_WINDOW, a.NET_WM_PING};
  X.SetWMProtocols(g.dpy, g.win, protocols, 2);
  XSizeHints hints{};
  hints.flags = PMinSize | (havePos ? (USPosition | PPosition) : 0) | USSize | PSize;
  hints.x = x;
  hints.y = y;
  hints.width = w;
  hints.height = h;
  hints.min_width = int(std::lround(float(g.cfg.minWidth) * g.scale));
  hints.min_height = int(std::lround(float(g.cfg.minHeight) * g.scale));
  X.SetWMNormalHints(g.dpy, g.win, &hints);
  std::string cls = utf8::lower(g.cfg.appName) + '\0' + g.cfg.appName + '\0';
  X.ChangeProperty(g.dpy, g.win, XA_WM_CLASS, XA_STRING, 8, PropModeReplace, reinterpret_cast<const unsigned char*>(cls.data()), int(cls.size()));
  long pid = long(posix::getpid());
  X.ChangeProperty(g.dpy, g.win, a.NET_WM_PID, XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&pid), 1);
  long dndVersion = 5;
  X.ChangeProperty(g.dpy, g.win, a.XdndAware, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&dndVersion), 1);
  applyTitle();
  applyDarkFrame();
  if (maximize) {
    setWmState(a.NET_WM_STATE_MAXIMIZED_VERT, true);
    setWmState(a.NET_WM_STATE_MAXIMIZED_HORZ, true);
  }
  if (g.pendingFullscreen) setWmState(a.NET_WM_STATE_FULLSCREEN, true);
  g.fullscreen = g.pendingFullscreen;
  g.pendingFullscreen = false;
  g.maximized = maximize;
  X.SelectInput(g.dpy, g.root, PropertyChangeMask);  // изменения Xft.dpi
  g.gc = X.CreateGC(g.dpy, g.win, 0, nullptr);
  openInputMethod();
  Bool supported = False;
  X.KbSetDetectableAutoRepeat(g.dpy, True, &supported);
  g.detectableRepeat = supported != False;
  X.DefineCursor(g.dpy, g.win, cursorFor(g.cursor));
}

void cleanup() {
  if (g.dpy) {
    if (g.win) {
      savePlacementNow();
      handOffClipboard();
    }
    if (g.ic) X.DestroyIC(g.ic);
    if (g.im) X.CloseIM(g.im);
    for (XCursor& c : g.cursors)
      if (c) X.FreeCursor(g.dpy, c);
    if (g.gc) X.FreeGC(g.dpy, g.gc);
    if (g.win) X.DestroyWindow(g.dpy, g.win);
    if (g.colormap) X.FreeColormap(g.dpy, g.colormap);
    X.CloseDisplay(g.dpy);
  }
  int w = g.pipeW.exchange(-1);
  if (w >= 0) posix::close(w);
  if (g.pipeR >= 0) posix::close(g.pipeR);
  g.pipeR = -1;
  g.dpy = nullptr;
  g.win = g.root = 0;
  g.ic = nullptr;
  g.im = nullptr;
  g.gc = nullptr;
  g.colormap = 0;
  g.cursors.fill(0);
  g.app = nullptr;
  g.ready = false;
  g.mapped = false;
  g.queue.clear();
  g.buf.clear();
  g.bw = g.bh = g.cw = g.ch = 0;
  g.callbackDepth = 0;
  g.buttonsDown = 0;
  g.keyDown.fill(false);
  g.keycodeDown.fill(false);
  g.ownsClipboard = false;
  g.clipText.clear();
  g.incr.clear();
  g.fullscreen = g.maximized = false;
  g.frameIndex = 0;
}

// Запуск внешней программы без ожидания (двойной fork — без зомби); дескрипторы не наследуются.
bool spawnDetached(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  for (const std::string& s : args) argv.push_back(const_cast<char*>(s.c_str()));
  argv.push_back(nullptr);
  auto pid = posix::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    auto pid2 = posix::fork();
    if (pid2 == 0) {
      posix::setsid();
      for (int fd = 3; fd < 1024; fd++) posix::close(fd);
      posix::execvp(argv[0], argv.data());
      posix::_exit(127);
    }
    posix::_exit(pid2 < 0 ? 1 : 0);
  }
  int status = 0;
  posix::waitpid(pid, &status, 0);
  return (status & 0x7F) == 0 && ((status >> 8) & 0xFF) == 0;
}

}  // namespace

// ================================================================ публичный интерфейс
int run(App& app, const WindowConfig& cfg) {
  if (g.app) throw std::logic_error("platform::run уже выполняется");
  loadX();
  g.dpy = X.OpenDisplay(nullptr);
  if (!g.dpy) {
    const char* d = std::getenv("DISPLAY");
    throw std::runtime_error(std::string("Не удалось подключиться к X-серверу (DISPLAY=") + (d ? d : "не задан") + ")");
  }
  X.SetErrorHandler(onXError);
  g.screen = X.DefaultScreen(g.dpy);
  g.root = X.RootWindow(g.dpy, g.screen);
  g.xfd = X.ConnectionNumber(g.dpy);
  g.cfg = cfg;
  g.title = cfg.title;
  g.dark = cfg.darkFrame;
  g.mainThread = std::this_thread::get_id();
  g.quitting = false;
  g.closePending = false;
  g.exitCode = 0;
  g.error = nullptr;
  g.app = &app;
  int fds[2] = {-1, -1};
  if (posix::pipe(fds) != 0) {
    cleanup();
    throw std::runtime_error("Не удалось создать канал пробуждения");
  }
  g.pipeR = fds[0];
  g.pipeW = fds[1];
  try {
    createWindow();
  } catch (...) {
    cleanup();
    throw;
  }

  g.ready = true;
  {
    Event e;
    e.type = EventType::Resize;
    e.width = float(g.cw) / g.scale;
    e.height = float(g.ch) / g.scale;
    e.scale = g.scale;
    push(std::move(e));
  }
  if (g.wakeEvent.exchange(false)) {
    Event e;
    e.type = EventType::Wake;
    push(std::move(e));
  }
  g.wakePosted = false;
  g.dirty = true;
  X.MapWindow(g.dpy, g.win);
  X.Flush(g.dpy);

  while (!g.quitting) {
    drainX();
    drainPipe();
    flushEvents();
    if (g.closePending && g.callbackDepth == 0) {
      g.closePending = false;
      requestClose();
    }
    if (g.quitting) break;
    bool anim = callAnimating();
    if (g.quitting) break;
    int timeout = -1;
    if (g.mapped && (g.dirty || anim)) {
      // Не чаще ~60 кадров в секунду: события мыши между кадрами объединяются.
      double now = time(), due = g.lastFrame + g.period;
      if (now >= due - 0.0005) {
        g.lastFrame = now;
        renderNow();
        continue;
      }
      timeout = std::max(1, int((due - now) * 1000));
    }
    X.Flush(g.dpy);
    if (X.EventsQueued(g.dpy, QueuedAlready) > 0) continue;
    pollfd p[2] = {{g.xfd, kPollIn, 0}, {g.pipeR, kPollIn, 0}};
    posix::poll(p, 2, timeout);
  }

  std::exception_ptr err = g.error;
  int code = g.exitCode;
  g.error = nullptr;
  cleanup();
  if (err) std::rethrow_exception(err);
  return code;
}

void quit(int exitCode) {
  g.quitting = true;
  g.exitCode = exitCode;
}

void invalidate() {
  bool was = g.dirty.exchange(true);
  if (!was && std::this_thread::get_id() != g.mainThread) {
    int fd = g.pipeW.load();
    if (fd >= 0) {
      char c = 'r';
      (void)posix::write(fd, &c, 1);
    }
  }
}

void wake() {
  g.wakeEvent = true;
  if (!g.wakePosted.exchange(true)) {
    int fd = g.pipeW.load();
    char c = 'w';
    if (fd < 0 || posix::write(fd, &c, 1) != 1) g.wakePosted = false;
  }
}

void setCursor(Cursor c) {
  if (c == g.cursor) return;
  g.cursor = c;
  if (g.win) {
    X.DefineCursor(g.dpy, g.win, cursorFor(c));
    X.Flush(g.dpy);
  }
}

void setTitle(const std::string& title) {
  g.title = title;
  applyTitle();
}

void setFullscreen(bool on) {
  if (!g.win) {
    g.pendingFullscreen = on;
    return;
  }
  if (on == g.fullscreen) return;
  g.fullscreen = on;  // подтверждение придёт в _NET_WM_STATE
  setWmState(g.a.NET_WM_STATE_FULLSCREEN, on);
}

bool fullscreen() { return g.win ? g.fullscreen : g.pendingFullscreen; }

void setDarkFrame(bool dark) {
  g.dark = dark;
  applyDarkFrame();
}

void setTextInputRect(RectF caret) {
  // Окно кандидатов XIM располагает сам метод ввода (стиль «без предредактирования»).
  g.caret = caret;
}

float scale() {
  if (g.dpy) return g.scale;
  if (const char* e = std::getenv("GDK_SCALE")) {
    int v = std::atoi(e);
    if (v >= 1 && v <= 4) return float(v);
  }
  return 1;
}

void* nativeWindow() { return g.win ? reinterpret_cast<void*>(g.win) : nullptr; }

std::string clipboardText() {
  if (!g.dpy || !g.win) return {};
  if (g.ownsClipboard) return g.clipText;
  if (!X.GetSelectionOwner(g.dpy, g.a.CLIPBOARD)) return {};
  Atom type = None;
  std::string s = receiveSelection(g.a.CLIPBOARD, g.a.UTF8_STRING, &type);
  if (type == None) {
    s = receiveSelection(g.a.CLIPBOARD, XA_STRING, &type);
    if (type == XA_STRING) s = latin1ToUtf8(s);
  }
  if (type == None) return {};
  return detail::fromCrlf(s);
}

void setClipboardText(const std::string& text) {
  if (!g.dpy || !g.win) return;
  g.clipText = text;
  X.SetSelectionOwner(g.dpy, g.a.CLIPBOARD, g.win, g.lastTime);
  g.ownsClipboard = X.GetSelectionOwner(g.dpy, g.a.CLIPBOARD) == g.win;
  if (!g.ownsClipboard) logWarn("X11: не удалось занять буфер обмена");
}

std::optional<std::string> openFileDialog(const std::string&, const std::vector<FileFilter>&, const std::string&) { return std::nullopt; }
std::optional<std::string> saveFileDialog(const std::string&, const std::vector<FileFilter>&, const std::string&, const std::string&) {
  return std::nullopt;
}
std::optional<std::string> pickFolderDialog(const std::string&, const std::string&) { return std::nullopt; }
bool dialogsSupported() { return false; }

bool openPath(const std::string& path) {
  if (path.empty()) return false;
  return spawnDetached({"xdg-open", path});
}

bool openUrl(const std::string& url) {
  if (!detail::isSafeUrl(url)) return false;
  return spawnDetached({"xdg-open", url});
}

void showFatal(const std::string& title, const std::string& text) {
  std::fprintf(stderr, "%s: %s\n", title.c_str(), text.c_str());
  std::fflush(stderr);
  // Окно сообщения: zenity, kdialog или xmessage — что найдётся. Ждём закрытия.
  std::string markup;
  for (char c : text) markup += c == '&' ? std::string("&amp;") : c == '<' ? std::string("&lt;") : c == '>' ? std::string("&gt;") : std::string(1, c);
  std::string titleArg = "--title=" + title, textArg = "--text=" + markup;
  const char* zenity[] = {"zenity", "--error", titleArg.c_str(), textArg.c_str(), nullptr};
  const char* kdialog[] = {"kdialog", "--title", title.c_str(), "--error", text.c_str(), nullptr};
  std::string both = title + "\n\n" + text;
  const char* xmessage[] = {"xmessage", "-center", both.c_str(), nullptr};
  if (!std::getenv("DISPLAY")) return;
  auto pid = posix::fork();
  if (pid < 0) return;
  if (pid == 0) {
    for (int fd = 3; fd < 1024; fd++) posix::close(fd);
    posix::execvp(zenity[0], const_cast<char* const*>(zenity));
    posix::execvp(kdialog[0], const_cast<char* const*>(kdialog));
    posix::execvp(xmessage[0], const_cast<char* const*>(xmessage));
    posix::_exit(127);
  }
  int status = 0;
  posix::waitpid(pid, &status, 0);
}

double time() { return detail::monotonicSeconds(); }

}  // namespace rg::platform
