// Regnum — платформа Windows: Win32 + GDI (DIB-секция), DPI v2, IME, COM-диалоги, буфер обмена.
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <imm.h>
#include <dwmapi.h>
#include <mmsystem.h>

#include <array>
#include <atomic>
#include <deque>
#include <exception>

#include "platform/common.h"
#include "platform/platform.h"

namespace rg::platform {
namespace {

constexpr UINT kMsgWake = WM_APP + 1;     // wake() из любого потока
constexpr UINT kMsgRedraw = WM_APP + 2;   // invalidate() из фонового потока
constexpr UINT_PTR kTimerModal = 1;       // кадры во время перетаскивания рамки и системного меню
constexpr const wchar_t* kClassName = L"RegnumWindow";
constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW;
constexpr DWORD kExStyle = WS_EX_APPWINDOW;

// ---------------------------------------------------------------- функции новых версий Windows
using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
using SetProcessDPIAwareFn = BOOL(WINAPI*)();
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
using EnableNonClientDpiScalingFn = BOOL(WINAPI*)(HWND);
using CreateWaitableTimerExWFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

template <class F>
F loadProc(HMODULE m, const char* name) {
  FARPROC p = m ? GetProcAddress(m, name) : nullptr;
  return reinterpret_cast<F>(reinterpret_cast<void (*)()>(p));
}

struct Api {
  SetProcessDpiAwarenessContextFn setProcessDpiAwarenessContext = nullptr;
  SetProcessDpiAwarenessFn setProcessDpiAwareness = nullptr;
  SetProcessDPIAwareFn setProcessDPIAware = nullptr;
  GetDpiForWindowFn getDpiForWindow = nullptr;
  GetDpiForMonitorFn getDpiForMonitor = nullptr;
  AdjustWindowRectExForDpiFn adjustWindowRectExForDpi = nullptr;
  EnableNonClientDpiScalingFn enableNonClientDpiScaling = nullptr;
  CreateWaitableTimerExWFn createWaitableTimerExW = nullptr;
  Api() {
    HMODULE user = GetModuleHandleW(L"user32.dll");
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    setProcessDpiAwarenessContext = loadProc<SetProcessDpiAwarenessContextFn>(user, "SetProcessDpiAwarenessContext");
    setProcessDPIAware = loadProc<SetProcessDPIAwareFn>(user, "SetProcessDPIAware");
    getDpiForWindow = loadProc<GetDpiForWindowFn>(user, "GetDpiForWindow");
    adjustWindowRectExForDpi = loadProc<AdjustWindowRectExForDpiFn>(user, "AdjustWindowRectExForDpi");
    enableNonClientDpiScaling = loadProc<EnableNonClientDpiScalingFn>(user, "EnableNonClientDpiScaling");
    setProcessDpiAwareness = loadProc<SetProcessDpiAwarenessFn>(shcore, "SetProcessDpiAwareness");
    getDpiForMonitor = loadProc<GetDpiForMonitorFn>(shcore, "GetDpiForMonitor");
    createWaitableTimerExW = loadProc<CreateWaitableTimerExWFn>(kernel, "CreateWaitableTimerExW");
  }
};

Api& api() {
  static Api a;
  return a;
}

void ensureDpiAwareness() {
  static bool done = false;
  if (done) return;
  done = true;
  Api& a = api();
  // Порядок: Per-Monitor v2 → Per-Monitor v1 → системный DPI. Отказ «уже задано манифестом» нас устраивает.
  if (a.setProcessDpiAwarenessContext) {
    if (a.setProcessDpiAwarenessContext(reinterpret_cast<HANDLE>(intptr_t(-4)))) return;
    if (GetLastError() == ERROR_ACCESS_DENIED) return;
    if (a.setProcessDpiAwarenessContext(reinterpret_cast<HANDLE>(intptr_t(-3)))) return;
  }
  if (a.setProcessDpiAwareness) {
    HRESULT hr = a.setProcessDpiAwareness(2);
    if (SUCCEEDED(hr) || hr == E_ACCESSDENIED) return;
  }
  if (a.setProcessDPIAware) a.setProcessDPIAware();
}

UINT monitorDpi(HMONITOR mon) {
  UINT x = 0, y = 0;
  if (mon && api().getDpiForMonitor && SUCCEEDED(api().getDpiForMonitor(mon, 0, &x, &y)) && x > 0) return x;
  HDC dc = GetDC(nullptr);
  int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
  if (dc) ReleaseDC(nullptr, dc);
  return d > 0 ? UINT(d) : 96;
}

void adjustRect(RECT& r, DWORD style, DWORD ex, UINT dpi) {
  if (api().adjustWindowRectExForDpi) api().adjustWindowRectExForDpi(&r, style, FALSE, ex, dpi);
  else AdjustWindowRectEx(&r, style, FALSE, ex);
}

std::wstring wide(const std::string& s) { return utf8::toWide(s); }

std::string nativeSeparators(std::string s) {
  for (char& c : s)
    if (c == '/') c = '\\';
  return s;
}

// ---------------------------------------------------------------- курсоры «рука» (в Windows нет готовых)
const char* const kOpenHand[16] = {
    "       ##       ", "   ## #..###    ", "  #..##..#..#   ", "  #..##..#..# # ",
    "   #..#..#..##.#", "   #..#..#..#..#", " ## #.......#..#", "#..##..........#",
    "#...#.........# ", " #............# ", "  #...........# ", "  #..........#  ",
    "   #.........#  ", "    #.......#   ", "     #......#   ", "     #......#   ",
};
const char* const kClosedHand[16] = {
    "                ", "                ", "                ", "    ## ## ##    ",
    "   #..#..#..##  ", "   #........#.# ", "    #.........# ", "   ##.........# ",
    "  #...........# ", "  #...........# ", "  #..........#  ", "   #.........#  ",
    "    #.......#   ", "     #......#   ", "     #......#   ", "                ",
};

HCURSOR makeArtCursor(const char* const art[16], int k) {
  int n = 16 * k;
  BITMAPV5HEADER bi{};
  bi.bV5Size = sizeof bi;
  bi.bV5Width = n;
  bi.bV5Height = -n;
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;
  HDC dc = GetDC(nullptr);
  void* bits = nullptr;
  HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, dc);
  if (!color || !bits) return nullptr;
  u32* px = static_cast<u32*>(bits);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) {
      char c = art[y / k][x / k];
      px[size_t(y) * size_t(n) + size_t(x)] = c == '#' ? 0xFF000000u : c == '.' ? 0xFFFFFFFFu : 0u;
    }
  HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
  ICONINFO ii{};
  ii.fIcon = FALSE;
  ii.xHotspot = DWORD(8 * k);
  ii.yHotspot = DWORD(8 * k);
  ii.hbmMask = mask;
  ii.hbmColor = color;
  HCURSOR cur = reinterpret_cast<HCURSOR>(CreateIconIndirect(&ii));
  DeleteObject(color);
  if (mask) DeleteObject(mask);
  return cur;
}

// ---------------------------------------------------------------- состояние окна
struct State {
  HINSTANCE inst = nullptr;
  std::atomic<HWND> hwnd{nullptr};
  HWND helper = nullptr;               // невидимое окно для буфера обмена вне run()
  DWORD mainThread = 0;
  App* app = nullptr;
  WindowConfig cfg;
  bool ready = false;                  // окно создано, события можно доставлять

  UINT dpi = 96;
  float scale = 1;
  int cw = 0, ch = 0;                  // размер области окна, физические пиксели

  // Буфер кадра: DIB-секция в памяти, выводится BitBlt.
  HDC memDC = nullptr;
  HBITMAP dib = nullptr;
  HGDIOBJ oldBmp = nullptr;
  u32* bits = nullptr;
  int bw = 0, bh = 0;
  bool bufferReset = true;
  u64 frameIndex = 0;

  std::atomic<bool> dirty{true};
  std::atomic<bool> wakePosted{false};
  std::atomic<bool> wakeEvent{false};
  int callbackDepth = 0;               // мы внутри обработчика приложения (вложенные циклы диалогов)
  int modalDepth = 0;                  // системный модальный цикл (рамка, меню)
  bool rendering = false;
  bool quitting = false;
  bool closePending = false;
  int exitCode = 0;
  std::exception_ptr error;
  std::deque<Event> queue;

  // Мышь.
  int buttonsDown = 0;
  bool trackingLeave = false;
  bool mouseInside = false;
  float mx = 0, my = 0;
  detail::ClickCounter clicks;
  std::array<int, 5> lastClicks{};
  double lastPreciseWheel = -10;

  // Клавиатура.
  std::array<bool, size_t(Key::Count)> keyDown{};
  detail::Utf16Input utf16;

  // Курсор.
  Cursor cursor = Cursor::Arrow;
  HCURSOR openHand = nullptr, closedHand = nullptr;
  int handScale = 0;

  // Полноэкранный режим.
  bool fullscreen = false;
  bool pendingFullscreen = false;
  WINDOWPLACEMENT savedPlacement{};
  bool savedMaximized = false;

  // Запоминание положения.
  RECT normalRect{};
  UINT normalDpi = 96;
  bool haveNormal = false;

  // Прочее.
  bool dark = true;
  std::string title = "Regnum";
  RectF caret;
  bool caretSet = false;
  bool comInit = false;
  double lastPace = 0;
  double lastRender = -1;
  double period = 1.0 / 60;
  HANDLE frameTimer = nullptr;
  bool timePeriodRaised = false;
};

State g;

u32 currentMods() {
  u32 m = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000) m |= ModShift;
  if (GetKeyState(VK_CONTROL) & 0x8000) m |= ModCtrl;
  if (GetKeyState(VK_MENU) & 0x8000) m |= ModAlt;
  if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) m |= ModSuper;
  return m;
}

// Код виртуальной клавиши → Key. Буквы и цифры по VK не зависят от кириллической раскладки.
Key mapKey(WPARAM vk, LPARAM lp) {
  bool extended = (lp >> 24) & 1;
  if (vk >= 'A' && vk <= 'Z') return Key(int(Key::A) + int(vk - 'A'));
  if (vk >= '0' && vk <= '9') return Key(int(Key::D0) + int(vk - '0'));
  if (vk >= VK_F1 && vk <= VK_F12) return Key(int(Key::F1) + int(vk - VK_F1));
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return Key(int(Key::NumPad0) + int(vk - VK_NUMPAD0));
  switch (vk) {
    case VK_ESCAPE: return Key::Escape;
    case VK_RETURN: return extended ? Key::NumEnter : Key::Enter;
    case VK_TAB: return Key::Tab;
    case VK_BACK: return Key::Backspace;
    case VK_DELETE: return Key::Delete;
    case VK_INSERT: return Key::Insert;
    case VK_HOME: return Key::Home;
    case VK_END: return Key::End;
    case VK_PRIOR: return Key::PageUp;
    case VK_NEXT: return Key::PageDown;
    case VK_LEFT: return Key::Left;
    case VK_RIGHT: return Key::Right;
    case VK_UP: return Key::Up;
    case VK_DOWN: return Key::Down;
    case VK_SPACE: return Key::Space;
    case VK_OEM_MINUS: return Key::Minus;
    case VK_OEM_PLUS: return Key::Equal;
    case VK_OEM_4: return Key::LBracket;
    case VK_OEM_6: return Key::RBracket;
    case VK_OEM_1: return Key::Semicolon;
    case VK_OEM_7: return Key::Quote;
    case VK_OEM_COMMA: return Key::Comma;
    case VK_OEM_PERIOD: return Key::Period;
    case VK_OEM_2: return Key::Slash;
    case VK_OEM_5: case VK_OEM_102: return Key::Backslash;
    case VK_OEM_3: return Key::Grave;
    case VK_ADD: return Key::NumAdd;
    case VK_SUBTRACT: return Key::NumSub;
    case VK_MULTIPLY: return Key::NumMul;
    case VK_DIVIDE: return Key::NumDiv;
    case VK_DECIMAL: case VK_SEPARATOR: return Key::NumDecimal;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return Key::Shift;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return Key::Ctrl;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return Key::Alt;
    case VK_LWIN: case VK_RWIN: return Key::Super;
    case VK_CAPITAL: return Key::CapsLock;
    case VK_APPS: return Key::Menu;
    default: return Key::Unknown;
  }
}

void push(Event e) {
  if (e.type == EventType::Text && !g.queue.empty() && g.queue.back().type == EventType::Text) {
    g.queue.back().text += e.text;  // склеиваем подряд идущий ввод (IME, вставка суррогатов)
    return;
  }
  g.queue.push_back(std::move(e));
}

Event mouseEvent(EventType t, LPARAM lp) {
  Event e;
  e.type = t;
  e.x = float(GET_X_LPARAM(lp)) / g.scale;
  e.y = float(GET_Y_LPARAM(lp)) / g.scale;
  e.mods = currentMods();
  return e;
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

// ---------------------------------------------------------------- кадр
void freeBuffer() {
  if (g.dib) {
    SelectObject(g.memDC, g.oldBmp);
    DeleteObject(g.dib);
    g.dib = nullptr;
    g.bits = nullptr;
  }
  g.bw = g.bh = 0;
}

bool ensureBuffer(int w, int h) {
  if (g.dib && g.bw == w && g.bh == h) return true;
  freeBuffer();
  if (!g.memDC) g.memDC = CreateCompatibleDC(nullptr);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // строки сверху вниз
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  g.dib = CreateDIBSection(g.memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!g.dib || !bits) {
    g.dib = nullptr;
    return false;
  }
  g.oldBmp = SelectObject(g.memDC, g.dib);
  g.bits = static_cast<u32*>(bits);
  g.bw = w;
  g.bh = h;
  g.bufferReset = true;
  return true;
}

// Вывести буфер в DC окна; области вне буфера (пока кадр не догнал размер) — цветом фона.
void blit(HDC dc) {
  if (g.dib) BitBlt(dc, 0, 0, g.bw, g.bh, g.memDC, 0, 0, SRCCOPY);
  HBRUSH bg = CreateSolidBrush(RGB(0x0e, 0x11, 0x17));
  if (g.cw > g.bw) {
    RECT r{g.bw, 0, g.cw, g.ch};
    FillRect(dc, &r, bg);
  }
  if (g.ch > g.bh) {
    RECT r{0, g.bh, std::min(g.bw, g.cw), g.ch};
    FillRect(dc, &r, bg);
  }
  DeleteObject(bg);
}

bool canRender() {
  HWND h = g.hwnd.load();
  return g.app && g.ready && h && !g.rendering && g.callbackDepth == 0 && g.cw > 0 && g.ch > 0 && !IsIconic(h);
}

// Отрисовать кадр приложения; present — сразу вывести в окно.
void renderNow(bool present) {
  if (!canRender()) return;
  flushEvents();
  if (g.quitting || !canRender()) return;
  if (!ensureBuffer(g.cw, g.ch)) return;
  GdiFlush();  // GDI не должен держать незавершённые операции с памятью DIB
  g.dirty = false;
  Frame f;
  f.px = g.bits;
  f.w = g.bw;
  f.h = g.bh;
  f.stride = g.bw;
  f.scale = g.scale;
  f.reset = g.bufferReset;
  f.index = g.frameIndex++;
  g.rendering = true;
  guarded([&] { g.app->onFrame(f); });
  g.rendering = false;
  g.bufferReset = false;
  if (present) {
    HWND h = g.hwnd.load();
    HDC dc = GetDC(h);
    blit(dc);
    ReleaseDC(h, dc);
    ValidateRect(h, nullptr);
  }
}

double refreshPeriod() {
  DWM_TIMING_INFO ti{};
  ti.cbSize = sizeof ti;
  if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateRefresh.uiDenominator && ti.rateRefresh.uiNumerator) {
    double hz = double(ti.rateRefresh.uiNumerator) / double(ti.rateRefresh.uiDenominator);
    if (hz >= 24 && hz <= 500) return 1.0 / hz;
  }
  return 1.0 / 60;
}

// Точное ожидание без занятого цикла: таймер высокого разрешения (Windows 10 1803+) или timeBeginPeriod(1).
void sleepUntil(double t) {
  double now = time();
  if (t <= now) return;
  double dt = std::min(t - now, 0.1);
  if (!g.frameTimer) {
    if (api().createWaitableTimerExW) g.frameTimer = api().createWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
    if (!g.frameTimer) {
      g.frameTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
      if (!g.timePeriodRaised && timeBeginPeriod(1) == TIMERR_NOERROR) g.timePeriodRaised = true;
    }
  }
  if (g.frameTimer) {
    LARGE_INTEGER due;
    due.QuadPart = -LONGLONG(dt * 1e7);
    if (SetWaitableTimer(g.frameTimer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(g.frameTimer, 200);
      return;
    }
  }
  Sleep(DWORD(dt * 1000));
}

// Темп непрерывной отрисовки: ждём композицию DWM (вертикальная синхронизация);
// если DwmFlush вернулся мгновенно (композиция стоит) — досыпаем до конца периода.
void paceFrame() {
  double t0 = time();
  bool synced = false;
  if (SUCCEEDED(DwmFlush())) {
    double t1 = time();
    synced = (t1 - t0) > 0.0007 || (t1 - g.lastPace) >= g.period * 0.8;
  }
  if (!synced) sleepUntil(g.lastPace + g.period);
  g.lastPace = time();
}

// ---------------------------------------------------------------- курсор
HCURSOR cursorHandle(Cursor c) {
  int k = std::max(1, int(std::lround(g.scale)));
  if (k != g.handScale) {
    if (g.openHand) DestroyCursor(g.openHand);
    if (g.closedHand) DestroyCursor(g.closedHand);
    g.openHand = makeArtCursor(kOpenHand, k);
    g.closedHand = makeArtCursor(kClosedHand, k);
    g.handScale = k;
  }
  const wchar_t* id = IDC_ARROW;
  switch (c) {
    case Cursor::Arrow: id = IDC_ARROW; break;
    case Cursor::Hand: id = IDC_HAND; break;
    case Cursor::IBeam: id = IDC_IBEAM; break;
    case Cursor::Crosshair: id = IDC_CROSS; break;
    case Cursor::Move: id = IDC_SIZEALL; break;
    case Cursor::ResizeH: id = IDC_SIZEWE; break;
    case Cursor::ResizeV: id = IDC_SIZENS; break;
    case Cursor::ResizeNWSE: id = IDC_SIZENWSE; break;
    case Cursor::ResizeNESW: id = IDC_SIZENESW; break;
    case Cursor::Grab: if (g.openHand) return g.openHand; id = IDC_HAND; break;
    case Cursor::Grabbing: if (g.closedHand) return g.closedHand; id = IDC_SIZEALL; break;
    case Cursor::NotAllowed: id = IDC_NO; break;
    case Cursor::Wait: id = IDC_WAIT; break;
  }
  return LoadCursorW(nullptr, id);
}

void destroyCursors() {
  if (g.openHand) DestroyCursor(g.openHand);
  if (g.closedHand) DestroyCursor(g.closedHand);
  g.openHand = g.closedHand = nullptr;
  g.handScale = 0;
}

// ---------------------------------------------------------------- IME
void updateImePosition() {
  HWND h = g.hwnd.load();
  if (!h || !g.caretSet) return;
  HIMC imc = ImmGetContext(h);
  if (!imc) return;
  LONG x = LONG(std::lround(g.caret.x * g.scale)), y = LONG(std::lround(g.caret.y * g.scale));
  LONG r = LONG(std::lround(g.caret.right() * g.scale)), b = LONG(std::lround(g.caret.bottom() * g.scale));
  COMPOSITIONFORM cf{};
  cf.dwStyle = CFS_POINT;
  cf.ptCurrentPos = {x, y};
  ImmSetCompositionWindow(imc, &cf);
  CANDIDATEFORM cand{};
  cand.dwIndex = 0;
  cand.dwStyle = CFS_EXCLUDE;
  cand.ptCurrentPos = {x, b};
  cand.rcArea = {x, y, std::max(r, x + 1), b};
  ImmSetCandidateWindow(imc, &cand);
  ImmReleaseContext(h, imc);
}

// ---------------------------------------------------------------- положение окна
std::string userDataDir(const std::string& appName) {
  std::string base;
  PWSTR p = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) base = utf8::fromWide(p);
  if (p) CoTaskMemFree(p);
  if (base.empty()) {
    const wchar_t* env = _wgetenv(L"APPDATA");
    if (env) base = utf8::fromWide(env);
  }
  if (base.empty()) return {};
  return base + "\\" + appName;
}

void trackNormalRect() {
  HWND h = g.hwnd.load();
  if (!h || g.fullscreen || IsIconic(h) || IsZoomed(h)) return;
  GetWindowRect(h, &g.normalRect);
  g.normalDpi = g.dpi;
  g.haveNormal = true;
}

void savePlacementNow() {
  HWND h = g.hwnd.load();
  if (!h || !g.haveNormal) return;
  std::string path = detail::placementPath(g.cfg, userDataDir(g.cfg.appName));
  if (path.empty()) return;
  detail::Placement p;
  p.x = g.normalRect.left;
  p.y = g.normalRect.top;
  p.w = g.normalRect.right - g.normalRect.left;
  p.h = g.normalRect.bottom - g.normalRect.top;
  WINDOWPLACEMENT wp{};
  wp.length = sizeof wp;
  bool iconicMax = IsIconic(h) && GetWindowPlacement(h, &wp) && (wp.flags & WPF_RESTORETOMAXIMIZED);
  p.maximized = g.fullscreen ? g.savedMaximized : (IsZoomed(h) != 0 || iconicMax);
  p.dpi = int(g.normalDpi);
  if (!detail::savePlacement(path, p)) logWarn("Не удалось сохранить положение окна: %s", path.c_str());
}

// Начальный прямоугольник окна (экранные координаты) и признак «развернуть».
RECT initialRect(bool& maximize) {
  maximize = g.cfg.maximized;
  std::string path = detail::placementPath(g.cfg, userDataDir(g.cfg.appName));
  if (auto p = detail::loadPlacement(path)) {
    RECT r{p->x, p->y, p->x + p->w, p->y + p->h};
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    UINT mdpi = monitorDpi(mon);
    if (int(mdpi) != p->dpi) {  // монитор сменил масштаб — сохраняем логический размер
      r.right = r.left + MulDiv(p->w, int(mdpi), p->dpi);
      r.bottom = r.top + MulDiv(p->h, int(mdpi), p->dpi);
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    RECT strip{r.left + 32, r.top, std::max(r.left + 33, r.right - 32), r.top + 24};
    HMONITOR visible = MonitorFromRect(&strip, MONITOR_DEFAULTTONULL);
    if (visible && GetMonitorInfoW(visible, &mi)) {
      LONG ww = mi.rcWork.right - mi.rcWork.left, wh = mi.rcWork.bottom - mi.rcWork.top;
      if (r.right - r.left > ww) r.right = r.left + ww;
      if (r.bottom - r.top > wh) r.bottom = r.top + wh;
      maximize = p->maximized;
      return r;
    }
  }
  // По умолчанию — по центру рабочей области основного монитора.
  POINT origin{0, 0};
  HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof mi;
  GetMonitorInfoW(mon, &mi);
  UINT dpi = monitorDpi(mon);
  RECT r{0, 0, MulDiv(g.cfg.width, int(dpi), 96), MulDiv(g.cfg.height, int(dpi), 96)};
  adjustRect(r, kStyle, kExStyle, dpi);
  LONG w = std::min(r.right - r.left, mi.rcWork.right - mi.rcWork.left);
  LONG h = std::min(r.bottom - r.top, mi.rcWork.bottom - mi.rcWork.top);
  LONG x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
  LONG y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
  return RECT{x, y, x + w, y + h};
}

void applyDarkFrame() {
  HWND h = g.hwnd.load();
  if (!h) return;
  BOOL v = g.dark ? TRUE : FALSE;
  // 20 — DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 20H1+), 19 — то же в ранних сборках.
  if (FAILED(DwmSetWindowAttribute(h, 20, &v, sizeof v))) DwmSetWindowAttribute(h, 19, &v, sizeof v);
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

void releaseMouseButtons() {
  for (int b = 0; b < 5; b++) {
    if (!(g.buttonsDown & (1 << b))) continue;
    Event e;
    e.type = EventType::MouseUp;
    e.x = g.mx;
    e.y = g.my;
    e.button = b;
    e.clicks = g.lastClicks[size_t(b)];
    e.mods = currentMods();
    push(std::move(e));
  }
  g.buttonsDown = 0;
}

void releaseKeys() {
  for (size_t k = 0; k < g.keyDown.size(); k++) {
    if (!g.keyDown[k]) continue;
    g.keyDown[k] = false;
    Event e;
    e.type = EventType::KeyUp;
    e.key = Key(k);
    push(std::move(e));
  }
}

void onMouseButton(int button, bool down, LPARAM lp) {
  HWND h = g.hwnd.load();
  Event e = mouseEvent(down ? EventType::MouseDown : EventType::MouseUp, lp);
  e.button = button;
  g.mx = e.x;
  g.my = e.y;
  if (down) {
    if (g.buttonsDown == 0) SetCapture(h);
    g.buttonsDown |= 1 << button;
    float slop = float(std::max(1, GetSystemMetrics(SM_CXDOUBLECLK) / 2)) / g.scale;
    e.clicks = g.clicks.press(button, e.x, e.y, time(), double(GetDoubleClickTime()) / 1000.0, slop);
    g.lastClicks[size_t(button)] = e.clicks;
  } else {
    if (!(g.buttonsDown & (1 << button))) return;  // отпускание без нажатия в нашем окне
    g.buttonsDown &= ~(1 << button);
    e.clicks = g.lastClicks[size_t(button)];
    if (g.buttonsDown == 0 && GetCapture() == h) ReleaseCapture();
  }
  push(std::move(e));
  if (!down && g.buttonsDown == 0 && !g.mouseInside) {  // перетаскивание закончилось за пределами окна
    Event leave;
    leave.type = EventType::MouseLeave;
    leave.x = g.mx;
    leave.y = g.my;
    leave.mods = currentMods();
    push(std::move(leave));
  }
}

void onWheel(bool horizontal, WPARAM wp, LPARAM lp) {
  POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  ScreenToClient(g.hwnd.load(), &p);
  int delta = GET_WHEEL_DELTA_WPARAM(wp);
  double t = time();
  // Тачпады и колёса с плавной прокруткой присылают доли щелчка (не кратно 120).
  bool fraction = delta % WHEEL_DELTA != 0;
  if (fraction) g.lastPreciseWheel = t;
  bool precise = fraction || t - g.lastPreciseWheel < 0.35;
  float notches = float(delta) / float(WHEEL_DELTA);
  UINT lines = 3;
  SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
  if (lines == 0 || lines > 100) lines = 3;
  Event e;
  e.type = EventType::MouseWheel;
  e.x = float(p.x) / g.scale;
  e.y = float(p.y) / g.scale;
  e.mods = currentMods();
  e.precise = precise;
  float v = precise ? notches * float(lines) * (100.f / 3.f) : notches;
  if (horizontal) e.wheelX = v;
  else e.wheelY = v;
  push(std::move(e));
}

void onSize(WPARAM type, int w, int h) {
  if (type == SIZE_MINIMIZED) return;
  bool changed = w != g.cw || h != g.ch;
  g.cw = w;
  g.ch = h;
  trackNormalRect();
  if (!changed || !g.ready) return;
  Event e;
  e.type = EventType::Resize;
  e.width = float(w) / g.scale;
  e.height = float(h) / g.scale;
  e.scale = g.scale;
  push(std::move(e));
  g.dirty = true;
  // Синхронная отрисовка: содержимое следует за рамкой при перетаскивании края.
  renderNow(true);
}

void onDpiChanged(UINT dpi, const RECT* suggested) {
  HWND h = g.hwnd.load();
  g.dpi = dpi ? dpi : 96;
  g.scale = float(g.dpi) / 96.f;
  if (g.fullscreen) {
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(h, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
  } else if (suggested) {
    SetWindowPos(h, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                 suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  RECT rc;
  GetClientRect(h, &rc);
  g.cw = rc.right;
  g.ch = rc.bottom;
  Event e;
  e.type = EventType::ScaleChanged;
  e.width = float(g.cw) / g.scale;
  e.height = float(g.ch) / g.scale;
  e.scale = g.scale;
  push(std::move(e));
  g.dirty = true;
  if (g.mouseInside) SetCursor(cursorHandle(g.cursor));
  updateImePosition();
  renderNow(true);
}

void onDropFiles(HDROP drop) {
  Event e;
  e.type = EventType::FilesDropped;
  UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT i = 0; i < n; i++) {
    UINT len = DragQueryFileW(drop, i, nullptr, 0);
    std::wstring buf(size_t(len) + 1, L'\0');
    UINT got = DragQueryFileW(drop, i, buf.data(), len + 1);
    buf.resize(got);
    if (!buf.empty()) e.files.push_back(utf8::fromWide(buf));
  }
  POINT p{0, 0};
  DragQueryPoint(drop, &p);
  DragFinish(drop);
  e.x = float(p.x) / g.scale;
  e.y = float(p.y) / g.scale;
  e.mods = currentMods();
  if (!e.files.empty()) push(std::move(e));
}

void modalTick() {
  if (g.callbackDepth > 0 || !g.ready) return;
  flushEvents();
  if (g.dirty || callAnimating()) renderNow(true);
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_NCCREATE:
      // Для Per-Monitor v1 (Windows 10 до 1703) рамка масштабируется только так; в v2 вызов безвреден.
      if (api().enableNonClientDpiScaling) api().enableNonClientDpiScaling(h);
      break;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      if (g.dirty || !g.dib || g.bw != g.cw || g.bh != g.ch) renderNow(false);
      blit(dc);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_SIZE:
      onSize(wp, LOWORD(lp), HIWORD(lp));
      return 0;
    case WM_MOVE:
      trackNormalRect();
      break;
    case WM_ENTERSIZEMOVE:
    case WM_ENTERMENULOOP:
      if (g.modalDepth++ == 0) SetTimer(h, kTimerModal, 15, nullptr);
      break;
    case WM_EXITSIZEMOVE:
    case WM_EXITMENULOOP:
      if (g.modalDepth > 0 && --g.modalDepth == 0) KillTimer(h, kTimerModal);
      break;
    case WM_TIMER:
      if (wp == kTimerModal) {
        modalTick();
        return 0;
      }
      break;
    case WM_DPICHANGED:
      onDpiChanged(HIWORD(wp), reinterpret_cast<const RECT*>(lp));
      return 0;
    case WM_DISPLAYCHANGE:
      g.period = refreshPeriod();
      g.dirty = true;
      break;
    case WM_GETMINMAXINFO: {
      if (!g.ready || g.fullscreen) break;
      RECT r{0, 0, LONG(std::lround(float(g.cfg.minWidth) * g.scale)), LONG(std::lround(float(g.cfg.minHeight) * g.scale))};
      adjustRect(r, kStyle, kExStyle, g.dpi);
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMinTrackSize.x = r.right - r.left;
      mm->ptMinTrackSize.y = r.bottom - r.top;
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        SetCursor(cursorHandle(g.cursor));
        return TRUE;
      }
      break;
    case WM_MOUSEMOVE: {
      int px = GET_X_LPARAM(lp), py = GET_Y_LPARAM(lp);
      bool inside = px >= 0 && py >= 0 && px < g.cw && py < g.ch;  // при захвате указатель может быть снаружи
      if (inside && !g.trackingLeave) {
        TRACKMOUSEEVENT t{};
        t.cbSize = sizeof t;
        t.dwFlags = TME_LEAVE;
        t.hwndTrack = h;
        TrackMouseEvent(&t);
        g.trackingLeave = true;
      }
      g.mouseInside = inside;
      Event e = mouseEvent(EventType::MouseMove, lp);
      g.mx = e.x;
      g.my = e.y;
      push(std::move(e));
      return 0;
    }
    case WM_MOUSELEAVE:
      g.trackingLeave = false;
      g.mouseInside = false;
      if (g.buttonsDown == 0) {
        Event e;
        e.type = EventType::MouseLeave;
        e.x = g.mx;
        e.y = g.my;
        e.mods = currentMods();
        push(std::move(e));
      }
      return 0;
    case WM_LBUTTONDOWN: onMouseButton(MouseLeft, true, lp); return 0;
    case WM_LBUTTONUP: onMouseButton(MouseLeft, false, lp); return 0;
    case WM_RBUTTONDOWN: onMouseButton(MouseRight, true, lp); return 0;
    case WM_RBUTTONUP: onMouseButton(MouseRight, false, lp); return 0;
    case WM_MBUTTONDOWN: onMouseButton(MouseMiddle, true, lp); return 0;
    case WM_MBUTTONUP: onMouseButton(MouseMiddle, false, lp); return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
      onMouseButton(GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? MouseForward : MouseBack, msg == WM_XBUTTONDOWN, lp);
      return TRUE;
    case WM_MOUSEWHEEL:
      onWheel(false, wp, lp);
      return 0;
    case WM_MOUSEHWHEEL:
      onWheel(true, wp, lp);
      return 0;
    case WM_CAPTURECHANGED:
      if (reinterpret_cast<HWND>(lp) != h && g.buttonsDown) releaseMouseButtons();  // захват отняли (Alt+Tab и т. п.)
      return 0;
    case WM_CANCELMODE:
      if (g.buttonsDown && GetCapture() == h) ReleaseCapture();
      break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
      Key k = mapKey(wp, lp);
      if (k != Key::Unknown) {
        Event e;
        e.type = down ? EventType::KeyDown : EventType::KeyUp;
        e.key = k;
        e.mods = currentMods();
        e.repeat = down && ((lp >> 30) & 1);
        g.keyDown[size_t(k)] = down;
        push(std::move(e));
      }
      if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) break;  // Alt+F4 и Alt+Пробел обрабатывает система
      return 0;
    }
    case WM_SYSCOMMAND:
      // Alt, F10 и Alt+буква не включают несуществующее меню (иначе следующее нажатие «съедается» и звучит сигнал).
      if ((wp & 0xFFF0) == SC_KEYMENU && lp != ' ') return 0;
      break;
    case WM_MENUCHAR:
      return MAKELRESULT(0, MNC_CLOSE);
    case WM_CHAR: {
      std::string s = g.utf16.push(u16(wp));
      if (!s.empty()) {
        Event e;
        e.type = EventType::Text;
        e.text = std::move(s);
        e.mods = currentMods();
        push(std::move(e));
      }
      return 0;
    }
    case WM_UNICHAR:
      if (wp == UNICODE_NOCHAR) return TRUE;
      if (detail::isTextCodepoint(u32(wp))) {
        Event e;
        e.type = EventType::Text;
        e.text = utf8::encode(u32(wp));
        push(std::move(e));
      }
      return FALSE;
    case WM_IME_STARTCOMPOSITION:
      updateImePosition();
      break;
    case WM_SETFOCUS: {
      Event e;
      e.type = EventType::FocusIn;
      push(std::move(e));
      updateImePosition();
      return 0;
    }
    case WM_KILLFOCUS: {
      releaseKeys();
      g.utf16 = detail::Utf16Input();
      Event e;
      e.type = EventType::FocusOut;
      push(std::move(e));
      return 0;
    }
    case WM_DROPFILES:
      onDropFiles(reinterpret_cast<HDROP>(wp));
      return 0;
    case WM_CLOSE:
      requestClose();
      return 0;
    case WM_ENDSESSION:
      if (wp) savePlacementNow();
      return 0;
    case WM_DESTROY:
      if (g.ready) g.quitting = true;
      return 0;
    case kMsgWake: {
      g.wakePosted = false;
      if (g.wakeEvent.exchange(false)) {
        Event e;
        e.type = EventType::Wake;
        push(std::move(e));
      }
      g.dirty = true;
      if (g.modalDepth > 0) modalTick();
      return 0;
    }
    case kMsgRedraw:
      if (g.modalDepth > 0) modalTick();
      return 0;
    default:
      break;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

bool registerClass() {
  static bool registered = false;
  if (registered) return true;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = g.inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // фон рисует кадр; без кисти нет белой вспышки
  wc.lpszClassName = kClassName;
  // Значок из ресурса 1 (assets/regnum.rc), если он есть.
  if (FindResourceW(g.inst, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */)) {
    wc.hIcon = static_cast<HICON>(LoadImageW(g.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(g.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_SHARED));
  }
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
  registered = true;
  return true;
}

void ensureCom() {
  if (g.comInit) return;
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (SUCCEEDED(hr)) g.comInit = true;  // S_FALSE тоже требует парного CoUninitialize
}

HWND clipboardOwner() {
  HWND h = g.hwnd.load();
  if (h) return h;
  if (!g.helper) {
    if (!g.inst) g.inst = GetModuleHandleW(nullptr);
    g.helper = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g.inst, nullptr);
  }
  return g.helper;
}

bool openClipboard() {
  HWND owner = clipboardOwner();
  for (int i = 0; i < 20; i++) {
    if (OpenClipboard(owner)) return true;
    Sleep(DWORD(1 + i));  // буфер занят другой программой — короткие повторы
  }
  return false;
}

enum class DialogKind { Open, Save, Folder };

std::optional<std::string> fileDialog(DialogKind kind, const std::string& title, const std::vector<FileFilter>& filters,
                                      const std::string& startDir, const std::string& defaultName) {
  ensureCom();
  IFileDialog* dlg = nullptr;
  HRESULT hr = kind == DialogKind::Save
                   ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog, reinterpret_cast<void**>(&dlg))
                   : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg));
  if (FAILED(hr) || !dlg) {
    logError("Системный диалог недоступен (0x%08lx)", static_cast<unsigned long>(hr));
    return std::nullopt;
  }
  FILEOPENDIALOGOPTIONS opts = 0;
  dlg->GetOptions(&opts);
  opts |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
  if (kind == DialogKind::Folder) opts |= FOS_PICKFOLDERS | FOS_PATHMUSTEXIST;
  if (kind == DialogKind::Open) opts |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
  if (kind == DialogKind::Save) opts |= FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST;
  dlg->SetOptions(opts);
  if (!title.empty()) dlg->SetTitle(wide(title).c_str());

  std::vector<std::wstring> names, specs;
  if (kind != DialogKind::Folder) {
    for (const FileFilter& f : filters) {
      std::string spec;
      for (const std::string& e : f.exts) {
        if (!spec.empty()) spec += ';';
        spec += "*." + e;
      }
      if (spec.empty()) spec = "*.*";
      names.push_back(wide(f.name.empty() ? spec : f.name));
      specs.push_back(wide(spec));
    }
    std::vector<COMDLG_FILTERSPEC> fs;
    for (size_t i = 0; i < names.size(); i++) fs.push_back({names[i].c_str(), specs[i].c_str()});
    if (!fs.empty()) {
      dlg->SetFileTypes(UINT(fs.size()), fs.data());
      dlg->SetFileTypeIndex(1);
      if (kind == DialogKind::Save && !filters[0].exts.empty()) dlg->SetDefaultExtension(wide(filters[0].exts[0]).c_str());
    }
  }

  // Начальная папка; если указан файл — его папка и имя.
  std::wstring folder, file = wide(defaultName);
  if (!startDir.empty()) {
    std::wstring sd = wide(nativeSeparators(startDir));
    DWORD attr = GetFileAttributesW(sd.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
      size_t slash = sd.find_last_of(L'\\');
      if (slash != std::wstring::npos) {
        folder = sd.substr(0, slash);
        if (file.empty()) file = sd.substr(slash + 1);
      }
    } else if (attr != INVALID_FILE_ATTRIBUTES) {
      folder = sd;
    }
  }
  if (!folder.empty()) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(&item))) && item) {
      dlg->SetFolder(item);
      item->Release();
    }
  }
  if (!file.empty() && kind != DialogKind::Folder) dlg->SetFileName(file.c_str());

  std::optional<std::string> result;
  g.callbackDepth++;  // вложенный цикл диалога: события окна откладываются
  hr = dlg->Show(g.hwnd.load());
  g.callbackDepth--;
  if (SUCCEEDED(hr)) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item)) && item) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        result = utf8::fromWide(path);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    logError("Ошибка системного диалога (0x%08lx)", static_cast<unsigned long>(hr));
  }
  dlg->Release();
  g.dirty = true;
  return result;
}

void pumpMessages() {
  MSG m;
  while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
    if (m.message == WM_QUIT) {
      g.quitting = true;
      g.exitCode = int(m.wParam);
      return;
    }
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
}

void cleanup() {
  HWND h = g.hwnd.load();
  if (h) {
    KillTimer(h, kTimerModal);
    savePlacementNow();
    DragAcceptFiles(h, FALSE);
    g.ready = false;
    DestroyWindow(h);
  }
  g.hwnd = nullptr;
  freeBuffer();
  if (g.memDC) DeleteDC(g.memDC);
  g.memDC = nullptr;
  destroyCursors();
  if (g.frameTimer) CloseHandle(g.frameTimer);
  g.frameTimer = nullptr;
  if (g.timePeriodRaised) timeEndPeriod(1);
  g.timePeriodRaised = false;
  g.app = nullptr;
  g.queue.clear();
  g.callbackDepth = g.modalDepth = 0;
  g.buttonsDown = 0;
  g.keyDown.fill(false);
  g.fullscreen = false;
  g.haveNormal = false;
  g.trackingLeave = g.mouseInside = false;
  g.closePending = false;
  g.cw = g.ch = 0;
  g.frameIndex = 0;
  if (g.comInit) CoUninitialize();
  g.comInit = false;
}

}  // namespace

// ================================================================ публичный интерфейс
int run(App& app, const WindowConfig& cfg) {
  if (g.app) throw std::logic_error("platform::run уже выполняется");
  ensureDpiAwareness();
  ensureCom();
  g.inst = GetModuleHandleW(nullptr);
  g.mainThread = GetCurrentThreadId();
  g.cfg = cfg;
  g.title = cfg.title;
  g.dark = cfg.darkFrame;
  g.quitting = false;
  g.exitCode = 0;
  g.error = nullptr;
  g.ready = false;
  g.app = &app;
  g.period = refreshPeriod();
  if (!registerClass()) {
    g.app = nullptr;
    throw std::runtime_error("Не удалось зарегистрировать класс окна");
  }

  bool maximize = false;
  RECT r = initialRect(maximize);
  HWND h = CreateWindowExW(kExStyle, kClassName, wide(g.title).c_str(), kStyle, r.left, r.top, r.right - r.left,
                           r.bottom - r.top, nullptr, nullptr, g.inst, nullptr);
  if (!h) {
    g.app = nullptr;
    throw std::runtime_error("Не удалось создать окно (код " + std::to_string(GetLastError()) + ")");
  }
  g.hwnd = h;
  g.dpi = api().getDpiForWindow ? api().getDpiForWindow(h) : monitorDpi(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST));
  if (!g.dpi) g.dpi = 96;
  g.scale = float(g.dpi) / 96.f;
  applyDarkFrame();
  DragAcceptFiles(h, TRUE);
  GetWindowRect(h, &g.normalRect);
  g.normalDpi = g.dpi;
  g.haveNormal = true;

  RECT rc;
  GetClientRect(h, &rc);
  g.cw = rc.right;
  g.ch = rc.bottom;
  g.ready = true;
  {
    Event e;
    e.type = EventType::Resize;
    e.width = float(g.cw) / g.scale;
    e.height = float(g.ch) / g.scale;
    e.scale = g.scale;
    push(std::move(e));
  }
  if (g.wakeEvent.exchange(false)) {  // wake() до запуска цикла
    Event e;
    e.type = EventType::Wake;
    push(std::move(e));
  }
  g.wakePosted = false;
  g.dirty = true;
  flushEvents();
  if (!maximize) renderNow(false);  // первый кадр готов до показа окна — без вспышки фона
  ShowWindow(h, maximize ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
  UpdateWindow(h);
  if (g.pendingFullscreen) {
    g.pendingFullscreen = false;
    setFullscreen(true);
  }
  g.lastPace = time();

  while (!g.quitting) {
    pumpMessages();
    if (g.quitting) break;
    flushEvents();
    if (g.closePending && g.callbackDepth == 0) {
      g.closePending = false;
      requestClose();
    }
    if (g.quitting) break;
    bool anim = callAnimating();
    if (g.quitting) break;
    bool visible = g.cw > 0 && g.ch > 0 && !IsIconic(h);
    if (visible && (g.dirty || anim)) {
      // Одиночный кадр (щелчок, клавиша) — сразу; серия кадров (анимация, перетаскивание, invalidate из кадра)
      // — не чаще частоты экрана: ожидание композиции DWM, события за это время объединяются.
      double now = time();
      bool burst = now - g.lastRender < g.period * 1.5;
      g.lastRender = now;
      renderNow(true);
      if (anim || g.dirty || burst) paceFrame();
      continue;
    }
    MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
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
  if (HWND h = g.hwnd.load()) PostMessageW(h, kMsgRedraw, 0, 0);
}

void invalidate() {
  bool was = g.dirty.exchange(true);
  if (!was && GetCurrentThreadId() != g.mainThread)
    if (HWND h = g.hwnd.load()) PostMessageW(h, kMsgRedraw, 0, 0);
}

void wake() {
  g.wakeEvent = true;
  if (!g.wakePosted.exchange(true)) {
    HWND h = g.hwnd.load();
    if (!h || !PostMessageW(h, kMsgWake, 0, 0)) g.wakePosted = false;
  }
}

void setCursor(Cursor c) {
  g.cursor = c;
  HWND h = g.hwnd.load();
  if (h && (g.mouseInside || GetCapture() == h)) SetCursor(cursorHandle(c));
}

void setTitle(const std::string& title) {
  g.title = title;
  if (HWND h = g.hwnd.load()) SetWindowTextW(h, wide(title).c_str());
}

void setFullscreen(bool on) {
  HWND h = g.hwnd.load();
  if (!h) {
    g.pendingFullscreen = on;
    return;
  }
  if (on == g.fullscreen) return;
  LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
  if (on) {
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    g.savedPlacement.length = sizeof g.savedPlacement;
    if (!GetWindowPlacement(h, &g.savedPlacement) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return;
    g.savedMaximized = IsZoomed(h) != 0;
    g.fullscreen = true;
    SetWindowLongPtrW(h, GWL_STYLE, style & ~LONG_PTR(WS_OVERLAPPEDWINDOW));
    SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  } else {
    g.fullscreen = false;
    SetWindowLongPtrW(h, GWL_STYLE, style | LONG_PTR(WS_OVERLAPPEDWINDOW));
    SetWindowPlacement(h, &g.savedPlacement);
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  g.dirty = true;
}

bool fullscreen() { return g.hwnd.load() ? g.fullscreen : g.pendingFullscreen; }

void setDarkFrame(bool dark) {
  g.dark = dark;
  HWND h = g.hwnd.load();
  if (!h) return;
  applyDarkFrame();
  SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void setTextInputRect(RectF caret) {
  g.caret = caret;
  g.caretSet = true;
  HWND h = g.hwnd.load();
  if (h && GetFocus() == h) updateImePosition();
}

float scale() {
  if (g.hwnd.load()) return g.scale;
  ensureDpiAwareness();
  POINT origin{0, 0};
  return float(monitorDpi(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY))) / 96.f;
}

void* nativeWindow() { return g.hwnd.load(); }

std::string clipboardText() {
  if (!openClipboard()) return {};
  std::string r;
  if (HANDLE hdata = GetClipboardData(CF_UNICODETEXT)) {
    if (const auto* p = static_cast<const wchar_t*>(GlobalLock(hdata))) {
      size_t cap = GlobalSize(hdata) / sizeof(wchar_t);
      size_t n = 0;
      while (n < cap && p[n]) n++;
      r = utf8::fromWide(std::wstring_view(p, n));
      GlobalUnlock(hdata);
    }
  }
  CloseClipboard();
  return detail::fromCrlf(r);
}

void setClipboardText(const std::string& text) {
  std::wstring w = utf8::toWide(detail::toCrlf(text));
  if (!openClipboard()) {
    logWarn("Буфер обмена занят другой программой");
    return;
  }
  EmptyClipboard();
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
  if (mem) {
    if (void* dst = GlobalLock(mem)) {
      std::memcpy(dst, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
      GlobalUnlock(mem);
      if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    } else {
      GlobalFree(mem);
    }
  }
  CloseClipboard();
}

std::optional<std::string> openFileDialog(const std::string& title, const std::vector<FileFilter>& filters, const std::string& startDir) {
  return fileDialog(DialogKind::Open, title, filters, startDir, {});
}

std::optional<std::string> saveFileDialog(const std::string& title, const std::vector<FileFilter>& filters, const std::string& startDir,
                                          const std::string& defaultName) {
  return fileDialog(DialogKind::Save, title, filters, startDir, defaultName);
}

std::optional<std::string> pickFolderDialog(const std::string& title, const std::string& startDir) {
  return fileDialog(DialogKind::Folder, title, {}, startDir, {});
}

bool dialogsSupported() { return true; }

bool openPath(const std::string& path) {
  if (path.empty()) return false;
  ensureCom();
  auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide(nativeSeparators(path)).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  return r > 32;
}

bool openUrl(const std::string& url) {
  if (!detail::isSafeUrl(url)) return false;
  ensureCom();
  auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  return r > 32;
}

void showFatal(const std::string& title, const std::string& text) {
  logError("%s: %s", title.c_str(), text.c_str());
  g.callbackDepth++;
  MessageBoxW(g.hwnd.load(), wide(text).c_str(), wide(title).c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
  g.callbackDepth--;
}

double time() { return detail::monotonicSeconds(); }

}  // namespace rg::platform
