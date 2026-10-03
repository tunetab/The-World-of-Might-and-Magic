// Regnum — платформа macOS: обычный C++ поверх Objective-C runtime (dlopen libobjc, AppKit, CoreGraphics).
// Класс NSView создаётся во время выполнения; кадр выводится через CGImage без копирования; Retina — backingScaleFactor.
// Проверка синтаксиса на Windows: clang++ -fsyntax-only -DRG_PLATFORM_SYNTAX_CHECK -I src src/platform/cocoa.cpp
#ifndef RG_PLATFORM_SYNTAX_CHECK
#include <dlfcn.h>
#endif

#include <atomic>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>

#include "platform/common.h"
#include "platform/platform.h"

#ifdef RG_PLATFORM_SYNTAX_CHECK
namespace rg::platform::posix {
constexpr int kRtldLazy = 1, kRtldGlobal = 8;
void* dlopen(const char*, int);
void* dlsym(void*, const char*);
}  // namespace rg::platform::posix
#else
namespace rg::platform::posix {
using ::dlopen;
using ::dlsym;
constexpr int kRtldLazy = RTLD_LAZY, kRtldGlobal = RTLD_GLOBAL;
}  // namespace rg::platform::posix
#endif

namespace rg::platform {
namespace {

// ---------------------------------------------------------------- типы Objective-C и Foundation
struct objc_object;
struct objc_selector;
struct objc_class;
using id = objc_object*;
using SEL = objc_selector*;
using Class = objc_class*;
using IMP = void (*)();
#if defined(__aarch64__) || defined(__arm64__)
using BOOL = bool;   // arm64: настоящий bool
#define RG_BOOL_ENC "B"
#else
using BOOL = signed char;  // x86_64: signed char
#define RG_BOOL_ENC "c"
#endif
using NSInteger = long;
using NSUInteger = unsigned long;
using CGFloat = double;
struct NSPoint { CGFloat x, y; };
struct NSSize { CGFloat width, height; };
struct NSRect { NSPoint origin; NSSize size; };
static_assert(sizeof(NSRect) == 32 && sizeof(NSPoint) == 16, "CGRect/CGPoint");
using CGContextRef = void*;
using CGImageRef = void*;
using CGColorSpaceRef = void*;
using CGDataProviderRef = void*;
using CFStringRef = const void*;
constexpr BOOL YES = 1, NO = 0;
constexpr id nil = nullptr;

// Типы событий NSEvent и флаги модификаторов.
constexpr NSUInteger kEvLeftDown = 1, kEvLeftUp = 2, kEvRightDown = 3, kEvRightUp = 4, kEvMouseMoved = 5, kEvLeftDragged = 6,
                     kEvRightDragged = 7, kEvMouseEntered = 8, kEvMouseExited = 9, kEvKeyDown = 10, kEvKeyUp = 11,
                     kEvApplicationDefined = 15, kEvOtherDown = 25, kEvOtherUp = 26, kEvOtherDragged = 27;
constexpr NSUInteger kFlagCaps = 1UL << 16, kFlagShift = 1UL << 17, kFlagControl = 1UL << 18, kFlagOption = 1UL << 19,
                     kFlagCommand = 1UL << 20;
constexpr short kWakeSubtype = 0x5247;  // «RG»

// ---------------------------------------------------------------- runtime и фреймворки
struct Runtime {
  void* objc = nullptr;
  Class (*getClass)(const char*) = nullptr;
  SEL (*registerName)(const char*) = nullptr;
  Class (*allocateClassPair)(Class, const char*, size_t) = nullptr;
  void (*registerClassPair)(Class) = nullptr;
  BOOL (*addMethod)(Class, SEL, IMP, const char*) = nullptr;
  void* (*poolPush)() = nullptr;
  void (*poolPop)(void*) = nullptr;
  void* msgSend = nullptr;
  void* msgSendStret = nullptr;  // только x86_64: структуры больше 16 байт
  // CoreGraphics.
  CGColorSpaceRef (*colorSpaceCreateWithName)(CFStringRef) = nullptr;
  CGColorSpaceRef (*colorSpaceCreateDeviceRGB)() = nullptr;
  CGDataProviderRef (*dataProviderCreateWithData)(void*, const void*, size_t, void (*)(void*, const void*, size_t)) = nullptr;
  CGImageRef (*imageCreate)(size_t, size_t, size_t, size_t, size_t, CGColorSpaceRef, uint32_t, CGDataProviderRef, const CGFloat*, bool,
                            int32_t) = nullptr;
  void (*imageRelease)(CGImageRef) = nullptr;
  void (*dataProviderRelease)(CGDataProviderRef) = nullptr;
  void (*contextDrawImage)(CGContextRef, NSRect, CGImageRef) = nullptr;
  void (*contextSaveGState)(CGContextRef) = nullptr;
  void (*contextRestoreGState)(CGContextRef) = nullptr;
  void (*contextTranslateCTM)(CGContextRef, CGFloat, CGFloat) = nullptr;
  void (*contextScaleCTM)(CGContextRef, CGFloat, CGFloat) = nullptr;
  void (*contextSetInterpolationQuality)(CGContextRef, int32_t) = nullptr;
  void (*contextSetBlendMode)(CGContextRef, int32_t) = nullptr;
  // Константы (адреса глобальных переменных).
  id pasteboardTypeString = nil;
  id defaultRunLoopMode = nil;
  id runLoopCommonModes = nil;
  CGColorSpaceRef colorSpace = nullptr;
  bool ok = false;
};

Runtime R;

template <class F>
void bind(void* lib, F& f, const char* name, std::string& missing) {
  f = lib ? reinterpret_cast<F>(posix::dlsym(lib, name)) : nullptr;
  if (!f) missing += std::string(missing.empty() ? "" : ", ") + name;
}

id constant(void* lib, const char* name, const char* fallback) {
  void* p = lib ? posix::dlsym(lib, name) : nullptr;
  if (p) return *static_cast<id*>(p);
  if (!fallback) return nil;
  using StrFn = id (*)(id, SEL, const char*);
  return reinterpret_cast<StrFn>(R.msgSend)(reinterpret_cast<id>(R.getClass("NSString")), R.registerName("stringWithUTF8String:"), fallback);
}

void loadRuntime() {
  if (R.ok) return;
  void* objc = posix::dlopen("/usr/lib/libobjc.A.dylib", posix::kRtldLazy | posix::kRtldGlobal);
  void* foundation = posix::dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", posix::kRtldLazy | posix::kRtldGlobal);
  void* appkit = posix::dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", posix::kRtldLazy | posix::kRtldGlobal);
  void* cg = posix::dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", posix::kRtldLazy | posix::kRtldGlobal);
  if (!cg) cg = posix::dlopen("/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices", posix::kRtldLazy | posix::kRtldGlobal);
  if (!objc || !foundation || !appkit || !cg) throw std::runtime_error("Не удалось загрузить libobjc/Foundation/AppKit/CoreGraphics");
  std::string miss;
  bind(objc, R.getClass, "objc_getClass", miss);
  bind(objc, R.registerName, "sel_registerName", miss);
  bind(objc, R.allocateClassPair, "objc_allocateClassPair", miss);
  bind(objc, R.registerClassPair, "objc_registerClassPair", miss);
  bind(objc, R.addMethod, "class_addMethod", miss);
  bind(objc, R.poolPush, "objc_autoreleasePoolPush", miss);
  bind(objc, R.poolPop, "objc_autoreleasePoolPop", miss);
  bind(objc, R.msgSend, "objc_msgSend", miss);
#if defined(__x86_64__)
  bind(objc, R.msgSendStret, "objc_msgSend_stret", miss);
#endif
  bind(cg, R.colorSpaceCreateDeviceRGB, "CGColorSpaceCreateDeviceRGB", miss);
  bind(cg, R.dataProviderCreateWithData, "CGDataProviderCreateWithData", miss);
  bind(cg, R.imageCreate, "CGImageCreate", miss);
  bind(cg, R.imageRelease, "CGImageRelease", miss);
  bind(cg, R.dataProviderRelease, "CGDataProviderRelease", miss);
  bind(cg, R.contextDrawImage, "CGContextDrawImage", miss);
  bind(cg, R.contextSaveGState, "CGContextSaveGState", miss);
  bind(cg, R.contextRestoreGState, "CGContextRestoreGState", miss);
  bind(cg, R.contextTranslateCTM, "CGContextTranslateCTM", miss);
  bind(cg, R.contextScaleCTM, "CGContextScaleCTM", miss);
  bind(cg, R.contextSetInterpolationQuality, "CGContextSetInterpolationQuality", miss);
  bind(cg, R.contextSetBlendMode, "CGContextSetBlendMode", miss);
  if (!miss.empty()) throw std::runtime_error("Нет функций Objective-C/CoreGraphics: " + miss);
  R.colorSpaceCreateWithName = reinterpret_cast<CGColorSpaceRef (*)(CFStringRef)>(posix::dlsym(cg, "CGColorSpaceCreateWithName"));
  R.pasteboardTypeString = constant(appkit, "NSPasteboardTypeString", "public.utf8-plain-text");
  R.defaultRunLoopMode = constant(foundation, "NSDefaultRunLoopMode", "kCFRunLoopDefaultMode");
  R.runLoopCommonModes = constant(foundation, "NSRunLoopCommonModes", "kCFRunLoopCommonModes");
  // sRGB: цвета темы заданы в sRGB и не перенасыщаются на экранах P3.
  void* srgb = posix::dlsym(cg, "kCGColorSpaceSRGB");
  if (srgb && R.colorSpaceCreateWithName) R.colorSpace = R.colorSpaceCreateWithName(*static_cast<CFStringRef*>(srgb));
  if (!R.colorSpace) R.colorSpace = R.colorSpaceCreateDeviceRGB();
  R.objc = objc;
  R.ok = true;
}

// ---------------------------------------------------------------- отправка сообщений
SEL sel(const char* name) { return R.registerName(name); }
id cls(const char* name) { return reinterpret_cast<id>(R.getClass(name)); }

// objc_msgSend с точным типом функции (на arm64 переменное число аргументов недопустимо).
template <class Ret = id, class... A>
Ret msg(id obj, const char* name, A... args) {
  using Fn = Ret (*)(id, SEL, A...);
  return reinterpret_cast<Fn>(R.msgSend)(obj, sel(name), args...);
}

// Возврат NSRect: на x86_64 — через objc_msgSend_stret (структура 32 байта), на arm64 — обычный вызов.
template <class... A>
NSRect msgRect(id obj, const char* name, A... args) {
#if defined(__x86_64__)
  using Fn = void (*)(NSRect*, id, SEL, A...);
  NSRect r{};
  reinterpret_cast<Fn>(R.msgSendStret)(&r, obj, sel(name), args...);
  return r;
#else
  return msg<NSRect>(obj, name, args...);
#endif
}

// NSString из UTF-8; некорректные байты заменяются (иначе stringWithUTF8String: вернёт nil).
id nsstr(const std::string& s) {
  if (utf8::valid(s)) return msg<id>(cls("NSString"), "stringWithUTF8String:", s.c_str());
  return msg<id>(cls("NSString"), "stringWithUTF8String:", utf8::fromU32(utf8::toU32(s)).c_str());
}

std::string fromNs(id s) {
  if (!s) return {};
  const char* p = msg<const char*>(s, "UTF8String");
  return p ? std::string(p) : std::string();
}

bool responds(id obj, const char* name) { return msg<BOOL>(obj, "respondsToSelector:", sel(name)) != NO; }

struct Pool {  // autoreleasepool
  void* p;
  Pool() : p(R.poolPush()) {}
  ~Pool() { R.poolPop(p); }
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;
};

// ---------------------------------------------------------------- состояние
struct PixelBuffer {  // буфер кадра; живёт, пока на него ссылается CGImage
  std::vector<u32> px;
  int w = 0, h = 0;
};

struct State {
  id app = nil;          // NSApp
  std::atomic<bool> appReady{false};  // NSApp создан: wake()/invalidate() из потоков могут им пользоваться
  id window = nil;
  id view = nil;
  id winDelegate = nil;
  id appDelegate = nil;
  id timer = nil;
  bool classesReady = false;

  App* client = nullptr;
  WindowConfig cfg;
  bool ready = false;
  float scale = 1;
  float lw = 0, lh = 0;  // логический размер области окна (точки)
  int cw = 0, ch = 0;    // физический

  std::shared_ptr<PixelBuffer> buf;
  bool bufferReset = true;
  u64 frameIndex = 0;
  double lastFrame = -1;
  double period = 1.0 / 60;

  std::atomic<bool> dirty{true};
  std::atomic<bool> wakePosted{false};
  std::atomic<bool> wakeEvent{false};
  int callbackDepth = 0;
  bool quitting = false;
  bool closePending = false;
  int exitCode = 0;
  std::exception_ptr error;
  std::deque<Event> queue;

  int buttonsDown = 0;
  bool ctrlClick = false;  // Ctrl+щелчок превращён в правую кнопку
  bool mouseInside = false;
  float mx = 0, my = 0;
  std::array<bool, size_t(Key::Count)> keyDown{};

  Cursor cursor = Cursor::Arrow;
  bool fullscreen = false, pendingFullscreen = false;
  bool dark = true;
  std::string title = "Regnum";
  RectF caret;
};

State g;

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
  if (!g.client || !g.ready || g.callbackDepth > 0) return;
  while (!g.queue.empty() && !g.quitting) {
    Event e = std::move(g.queue.front());
    g.queue.pop_front();
    g.dirty = true;  // каждое доставленное событие — новый кадр
    guarded([&] { g.client->onEvent(e); });
  }
}

bool callAnimating() {
  bool anim = false;
  if (g.client && g.callbackDepth == 0) guarded([&] { anim = g.client->animating(); });
  return anim;
}

void requestClose() {
  if (!g.client || g.quitting) return;
  if (g.callbackDepth > 0) {
    g.closePending = true;
    return;
  }
  flushEvents();
  bool ok = false;
  guarded([&] { ok = g.client->onCloseRequest(); });
  if (ok && !g.quitting) {
    g.quitting = true;
    g.exitCode = 0;
  }
}

// ---------------------------------------------------------------- размер и кадр
void updateGeometry() {
  if (!g.view || !g.window) return;
  NSRect b = msgRect(g.view, "bounds");
  g.scale = float(msg<CGFloat>(g.window, "backingScaleFactor"));
  if (g.scale <= 0) g.scale = 1;
  g.lw = float(b.size.width);
  g.lh = float(b.size.height);
  g.cw = int(std::lround(b.size.width * g.scale));
  g.ch = int(std::lround(b.size.height * g.scale));
}

void pushSizeEvent(EventType t) {
  Event e;
  e.type = t;
  e.width = g.lw;
  e.height = g.lh;
  e.scale = g.scale;
  push(std::move(e));
}

bool canRender() {
  return g.client && g.ready && g.callbackDepth == 0 && g.cw > 0 && g.ch > 0 && msg<BOOL>(g.window, "isMiniaturized") == NO;
}

// Отрисовать кадр приложения в буфер (без вывода на экран).
bool renderFrame() {
  if (!canRender()) return false;
  flushEvents();
  if (g.quitting || !canRender()) return false;
  if (!g.buf || g.buf->w != g.cw || g.buf->h != g.ch) {
    auto nb = std::make_shared<PixelBuffer>();  // старый буфер освободится, когда его отпустит CoreGraphics
    nb->px.assign(size_t(g.cw) * size_t(g.ch), 0xFF0E1117u);
    nb->w = g.cw;
    nb->h = g.ch;
    g.buf = nb;
    g.bufferReset = true;
  }
  g.dirty = false;
  Frame f;
  f.px = g.buf->px.data();
  f.w = g.buf->w;
  f.h = g.buf->h;
  f.stride = g.buf->w;
  f.scale = g.scale;
  f.reset = g.bufferReset;
  f.index = g.frameIndex++;
  guarded([&] { g.client->onFrame(f); });
  g.bufferReset = false;
  return true;
}

// Кадр и немедленный вывод (drawRect: вызывается синхронно).
void renderNow() {
  if (!renderFrame()) return;
  msg<void>(g.view, "setNeedsDisplay:", YES);
  msg<void>(g.view, "displayIfNeeded");
}

void releaseBuffer(void* info, const void*, size_t) { delete static_cast<std::shared_ptr<PixelBuffer>*>(info); }

// drawRect: — вывести готовый буфер (CGImage над нашей памятью, без копирования).
void drawRectImp(id, SEL, NSRect) {
  // Кадра нет или он устарел (изменение размера) — рисуем без вложенного вывода на экран.
  if (!g.buf || g.dirty || g.buf->w != g.cw || g.buf->h != g.ch) renderFrame();
  if (!g.buf || g.buf->px.empty()) return;
  std::shared_ptr<PixelBuffer> b = g.buf;
  id ctx = msg<id>(cls("NSGraphicsContext"), "currentContext");
  if (!ctx) return;
  CGContextRef cg = responds(ctx, "CGContext") ? msg<CGContextRef>(ctx, "CGContext") : msg<CGContextRef>(ctx, "graphicsPort");
  if (!cg) return;
  size_t bytes = b->px.size() * sizeof(u32);
  auto* ref = new std::shared_ptr<PixelBuffer>(b);  // освобождает releaseBuffer, когда CoreGraphics отпустит данные
  CGDataProviderRef provider = R.dataProviderCreateWithData(ref, b->px.data(), bytes, releaseBuffer);
  if (!provider) {
    delete ref;
    return;
  }
  // kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little: байты B, G, R, X — как наш буфер.
  CGImageRef img = R.imageCreate(size_t(b->w), size_t(b->h), 8, 32, size_t(b->w) * 4, R.colorSpace, 6u | (2u << 12), provider, nullptr, false, 0);
  if (img) {
    double iw = double(b->w) / g.scale, ih = double(b->h) / g.scale;
    R.contextSaveGState(cg);
    R.contextTranslateCTM(cg, 0, g.lh);  // вид перевёрнут (isFlipped) — изображение рисуем в обычной системе
    R.contextScaleCTM(cg, 1, -1);
    R.contextSetInterpolationQuality(cg, 1);  // kCGInterpolationNone
    R.contextSetBlendMode(cg, 17);            // kCGBlendModeCopy
    R.contextDrawImage(cg, NSRect{{0, double(g.lh) - ih}, {iw, ih}}, img);
    R.contextRestoreGState(cg);
    R.imageRelease(img);
  }
  R.dataProviderRelease(provider);
}

// ---------------------------------------------------------------- ввод
u32 modsFrom(NSUInteger f) {
  u32 m = 0;
  if (f & kFlagShift) m |= ModShift;
  if (f & kFlagControl) m |= ModCtrl;
  if (f & kFlagOption) m |= ModAlt;
  if (f & kFlagCommand) m |= ModSuper;
  return m;
}

NSPoint eventPoint(id event) {
  NSPoint p = msg<NSPoint>(event, "locationInWindow");
  return msg<NSPoint>(g.view, "convertPoint:fromView:", p, nil);  // вид перевёрнут: y вниз, точки = логические пиксели
}

// Виртуальные коды клавиш macOS (kVK_*) → Key. Положение клавиши, не символ.
Key keyFromCode(unsigned short code) {
  static const Key table[128] = {
      Key::A, Key::S, Key::D, Key::F, Key::H, Key::G, Key::Z, Key::X,                                   // 0x00
      Key::C, Key::V, Key::Backslash, Key::B, Key::Q, Key::W, Key::E, Key::R,                           // 0x08 (0x0A — ISO §)
      Key::Y, Key::T, Key::D1, Key::D2, Key::D3, Key::D4, Key::D6, Key::D5,                             // 0x10
      Key::Equal, Key::D9, Key::D7, Key::Minus, Key::D8, Key::D0, Key::RBracket, Key::O,                // 0x18
      Key::U, Key::LBracket, Key::I, Key::P, Key::Enter, Key::L, Key::J, Key::Quote,                    // 0x20
      Key::K, Key::Semicolon, Key::Backslash, Key::Comma, Key::Slash, Key::N, Key::M, Key::Period,      // 0x28
      Key::Tab, Key::Space, Key::Grave, Key::Backspace, Key::Unknown, Key::Escape, Key::Super, Key::Super,  // 0x30
      Key::Shift, Key::CapsLock, Key::Alt, Key::Ctrl, Key::Shift, Key::Alt, Key::Ctrl, Key::Unknown,    // 0x38
      Key::Unknown, Key::NumDecimal, Key::Unknown, Key::NumMul, Key::Unknown, Key::NumAdd, Key::Unknown, Key::Unknown,  // 0x40
      Key::Unknown, Key::Unknown, Key::Unknown, Key::NumDiv, Key::NumEnter, Key::Unknown, Key::NumSub, Key::Unknown,     // 0x48
      Key::Unknown, Key::Equal, Key::NumPad0, Key::NumPad1, Key::NumPad2, Key::NumPad3, Key::NumPad4, Key::NumPad5,     // 0x50
      Key::NumPad6, Key::NumPad7, Key::Unknown, Key::NumPad8, Key::NumPad9, Key::Unknown, Key::Unknown, Key::Unknown,   // 0x58
      Key::F5, Key::F6, Key::F7, Key::F3, Key::F8, Key::F9, Key::Unknown, Key::F11,                     // 0x60
      Key::Unknown, Key::Unknown, Key::Unknown, Key::Unknown, Key::Unknown, Key::F10, Key::Menu, Key::F12,  // 0x68
      Key::Unknown, Key::Unknown, Key::Insert, Key::Home, Key::PageUp, Key::Delete, Key::F4, Key::End,  // 0x70 (0x72 — Help/Insert)
      Key::F2, Key::PageDown, Key::F1, Key::Left, Key::Right, Key::Down, Key::Up, Key::Unknown,         // 0x78
  };
  return code < 128 ? table[code] : Key::Unknown;
}

Key keyFor(id event) {
  unsigned short code = msg<unsigned short>(event, "keyCode");
  Key k = keyFromCode(code);
  // Латинские раскладки (AZERTY, Dvorak): буква — по символу без модификаторов, как принято в macOS.
  if (k >= Key::A && k <= Key::Z) {
    std::string c = fromNs(msg<id>(event, "charactersIgnoringModifiers"));
    if (c.size() == 1) {
      char ch = c[0];
      if (ch >= 'a' && ch <= 'z') return Key(int(Key::A) + (ch - 'a'));
      if (ch >= 'A' && ch <= 'Z') return Key(int(Key::A) + (ch - 'A'));
    }
  }
  return k;
}

void applyCursor();

void onMouse(id event) {
  NSUInteger type = msg<NSUInteger>(event, "type");
  NSUInteger flags = msg<NSUInteger>(event, "modifierFlags");
  NSPoint p = eventPoint(event);
  float x = float(p.x), y = float(p.y);
  g.mx = x;
  g.my = y;
  Event e;
  e.x = x;
  e.y = y;
  e.mods = modsFrom(flags);
  switch (type) {
    case kEvMouseMoved:
    case kEvLeftDragged:
    case kEvRightDragged:
    case kEvOtherDragged:
      g.mouseInside = x >= 0 && y >= 0 && x < g.lw && y < g.lh;
      if (type == kEvMouseMoved && g.mouseInside) applyCursor();  // AppKit мог сменить курсор (край окна и т. п.)
      e.type = EventType::MouseMove;
      push(std::move(e));
      return;
    case kEvMouseEntered:
      g.mouseInside = true;
      return;
    case kEvMouseExited:
      g.mouseInside = false;
      if (g.buttonsDown == 0) {
        e.type = EventType::MouseLeave;
        push(std::move(e));
      }
      return;
    default:
      break;
  }
  bool down = type == kEvLeftDown || type == kEvRightDown || type == kEvOtherDown;
  int button = MouseLeft;
  if (type == kEvRightDown || type == kEvRightUp) button = MouseRight;
  if (type == kEvOtherDown || type == kEvOtherUp) {
    NSInteger n = msg<NSInteger>(event, "buttonNumber");
    button = n == 2 ? MouseMiddle : n == 3 ? MouseBack : n == 4 ? MouseForward : -1;
    if (button < 0) return;
  }
  // Ctrl+щелчок — правая кнопка (привычно для трекпадов без правого щелчка).
  if (type == kEvLeftDown && (flags & kFlagControl)) {
    g.ctrlClick = true;
    button = MouseRight;
  } else if (type == kEvLeftUp && g.ctrlClick) {
    g.ctrlClick = false;
    button = MouseRight;
  }
  e.type = down ? EventType::MouseDown : EventType::MouseUp;
  e.button = button;
  e.clicks = int(std::max<NSInteger>(1, msg<NSInteger>(event, "clickCount")));
  if (down) {
    g.buttonsDown |= 1 << button;
  } else {
    if (!(g.buttonsDown & (1 << button))) return;
    g.buttonsDown &= ~(1 << button);
  }
  push(std::move(e));
  if (!down && g.buttonsDown == 0 && !(x >= 0 && y >= 0 && x < g.lw && y < g.lh)) {
    Event leave;
    leave.type = EventType::MouseLeave;
    leave.x = x;
    leave.y = y;
    push(std::move(leave));
  }
}

void mouseImp(id, SEL, id event) { onMouse(event); }

void scrollImp(id, SEL, id event) {
  NSPoint p = eventPoint(event);
  Event e;
  e.type = EventType::MouseWheel;
  e.x = float(p.x);
  e.y = float(p.y);
  e.mods = modsFrom(msg<NSUInteger>(event, "modifierFlags"));
  e.precise = msg<BOOL>(event, "hasPreciseScrollingDeltas") != NO;
  // dy > 0 — содержимое вниз (прокрутка вверх); dx > 0 у macOS — содержимое вправо, у нас — прокрутка вправо.
  e.wheelY = float(msg<CGFloat>(event, "scrollingDeltaY"));
  e.wheelX = -float(msg<CGFloat>(event, "scrollingDeltaX"));
  if (e.wheelX != 0 || e.wheelY != 0) push(std::move(e));
}

void magnifyImp(id, SEL, id event) {
  // Щипок на трекпаде — как Ctrl+колесо (так же его присылает Windows): масштаб карты без отдельного события.
  NSPoint p = eventPoint(event);
  Event e;
  e.type = EventType::MouseWheel;
  e.x = float(p.x);
  e.y = float(p.y);
  e.mods = modsFrom(msg<NSUInteger>(event, "modifierFlags")) | ModCtrl;
  e.precise = true;
  e.wheelY = float(msg<CGFloat>(event, "magnification")) * 300.f;
  if (e.wheelY != 0) push(std::move(e));
}

void keyImp(id, SEL, id event) {
  bool down = msg<NSUInteger>(event, "type") == kEvKeyDown;
  NSUInteger flags = msg<NSUInteger>(event, "modifierFlags");
  Key k = keyFor(event);
  if (k != Key::Unknown) {
    Event e;
    e.type = down ? EventType::KeyDown : EventType::KeyUp;
    e.key = k;
    e.mods = modsFrom(flags);
    e.repeat = down && msg<BOOL>(event, "isARepeat") != NO;
    g.keyDown[size_t(k)] = down;
    push(std::move(e));
  }
  if (!down || (flags & (kFlagCommand | kFlagControl))) return;  // сочетания с ⌘/⌃ текста не дают
  std::string chars = fromNs(msg<id>(event, "characters"));
  std::string text;
  for (size_t i = 0; i < chars.size();) {
    u32 cp = utf8::decode(chars, i);
    if (cp >= 0xF700 && cp <= 0xF8FF) continue;  // функциональные клавиши AppKit
    if (detail::isTextCodepoint(cp)) utf8::append(text, cp);
  }
  if (!text.empty()) {
    Event e;
    e.type = EventType::Text;
    e.text = std::move(text);
    e.mods = modsFrom(flags);
    push(std::move(e));
  }
}

void flagsImp(id, SEL, id event) {
  unsigned short code = msg<unsigned short>(event, "keyCode");
  NSUInteger flags = msg<NSUInteger>(event, "modifierFlags");
  Key k = keyFromCode(code);
  bool down = false;
  // Аппаратно-зависимые биты различают левую и правую клавишу (NX_DEVICE*KEYMASK).
  switch (code) {
    case 0x38: down = flags & 0x0002; break;   // левый Shift
    case 0x3C: down = flags & 0x0004; break;   // правый Shift
    case 0x3B: down = flags & 0x0001; break;   // левый Control
    case 0x3E: down = flags & 0x2000; break;   // правый Control
    case 0x3A: down = flags & 0x0020; break;   // левый Option
    case 0x3D: down = flags & 0x0040; break;   // правый Option
    case 0x37: down = flags & 0x0008; break;   // левая Command
    case 0x36: down = flags & 0x0010; break;   // правая Command
    case 0x39: down = flags & kFlagCaps; break;
    default: return;
  }
  if (k == Key::Unknown || g.keyDown[size_t(k)] == down) return;
  g.keyDown[size_t(k)] = down;
  Event e;
  e.type = down ? EventType::KeyDown : EventType::KeyUp;
  e.key = k;
  e.mods = modsFrom(flags);
  push(std::move(e));
}

id cursorObject(Cursor c) {
  id C = cls("NSCursor");
  auto privateOr = [&](const char* priv, const char* pub) { return responds(C, priv) ? msg<id>(C, priv) : msg<id>(C, pub); };
  switch (c) {
    case Cursor::Arrow: return msg<id>(C, "arrowCursor");
    case Cursor::Hand: return msg<id>(C, "pointingHandCursor");
    case Cursor::IBeam: return msg<id>(C, "IBeamCursor");
    case Cursor::Crosshair: return msg<id>(C, "crosshairCursor");
    case Cursor::Move: return privateOr("_moveCursor", "openHandCursor");
    case Cursor::ResizeH: return msg<id>(C, "resizeLeftRightCursor");
    case Cursor::ResizeV: return msg<id>(C, "resizeUpDownCursor");
    case Cursor::ResizeNWSE: return privateOr("_windowResizeNorthWestSouthEastCursor", "crosshairCursor");
    case Cursor::ResizeNESW: return privateOr("_windowResizeNorthEastSouthWestCursor", "crosshairCursor");
    case Cursor::Grab: return msg<id>(C, "openHandCursor");
    case Cursor::Grabbing: return msg<id>(C, "closedHandCursor");
    case Cursor::NotAllowed: return msg<id>(C, "operationNotAllowedCursor");
    case Cursor::Wait: return privateOr("_waitCursor", "arrowCursor");
  }
  return msg<id>(C, "arrowCursor");
}

void applyCursor() {
  if (id c = cursorObject(g.cursor)) msg<void>(c, "set");
}

void cursorUpdateImp(id, SEL, id) { applyCursor(); }

BOOL yesImp(id, SEL) { return YES; }
BOOL yesWithArgImp(id, SEL, id) { return YES; }

// Сверить размер и масштаб окна; изменения — события Resize/ScaleChanged и (по желанию) синхронный кадр.
void syncGeometry(bool render) {
  float os = g.scale, olw = g.lw, olh = g.lh;
  updateGeometry();
  bool sizeChanged = std::fabs(olw - g.lw) > 1e-3f || std::fabs(olh - g.lh) > 1e-3f;
  bool scaleChanged = std::fabs(os - g.scale) > 1e-4f;
  if (sizeChanged) pushSizeEvent(EventType::Resize);
  if (scaleChanged) pushSizeEvent(EventType::ScaleChanged);
  if (!sizeChanged && !scaleChanged) return;
  g.dirty = true;
  if (render) renderNow();  // содержимое следует за рамкой при перетаскивании края
}

void backingImp(id, SEL) { syncGeometry(false); }

// ---------------------------------------------------------------- делегаты окна и приложения
BOOL shouldCloseImp(id, SEL, id) {
  requestClose();
  return NO;  // окно закрывает сам цикл после согласия приложения
}

void didResizeImp(id, SEL, id) { syncGeometry(true); }

void didBecomeKeyImp(id, SEL, id) {
  Event e;
  e.type = EventType::FocusIn;
  push(std::move(e));
  g.dirty = true;
}

void didResignKeyImp(id, SEL, id) {
  for (size_t k = 0; k < g.keyDown.size(); k++) {
    if (!g.keyDown[k]) continue;
    g.keyDown[k] = false;
    Event e;
    e.type = EventType::KeyUp;
    e.key = Key(k);
    push(std::move(e));
  }
  Event e;
  e.type = EventType::FocusOut;
  push(std::move(e));
}

void didEnterFullScreenImp(id, SEL, id) { g.fullscreen = true; }
void didExitFullScreenImp(id, SEL, id) { g.fullscreen = false; }
void didDeminiaturizeImp(id, SEL, id) { g.dirty = true; }

NSUInteger shouldTerminateImp(id, SEL, id) {
  requestClose();  // ⌘Q и «Завершить» — тот же запрос, что и кнопка закрытия
  return 0;        // NSTerminateCancel: цикл завершается сам
}

void tickImp(id, SEL, id) {
  // Таймер анимации в общих режимах цикла — работает и во время перетаскивания рамки и меню.
  if (g.callbackDepth > 0 || !g.ready) return;
  flushEvents();
  double now = time();
  bool anim = callAnimating();
  if ((anim || g.dirty) && now - g.lastFrame >= g.period * 0.5) {
    g.lastFrame = now;
    renderNow();
  }
  if (!anim && !g.dirty && g.timer) {  // кадр, запрошенный изнутри кадра, ещё нужен — таймер продолжает
    msg<void>(g.timer, "invalidate");
    g.timer = nil;
  }
}

void addMethod(Class c, const char* name, IMP imp, const char* types) {
  if (!R.addMethod(c, sel(name), imp, types)) throw std::runtime_error(std::string("class_addMethod: ") + name);
}

template <class F>
IMP imp(F* f) {
  return reinterpret_cast<IMP>(f);
}

void registerClasses() {
  if (g.classesReady) return;
  const char* kRect = "v@:{CGRect={CGPoint=dd}{CGSize=dd}}";
  const char* kBool = RG_BOOL_ENC "@:";
  const char* kBoolObj = RG_BOOL_ENC "@:@";
  Class view = R.allocateClassPair(R.getClass("NSView"), "RegnumView", 0);
  if (!view) throw std::runtime_error("Не удалось создать класс RegnumView");
  addMethod(view, "drawRect:", imp(&drawRectImp), kRect);
  addMethod(view, "isFlipped", imp(&yesImp), kBool);
  addMethod(view, "isOpaque", imp(&yesImp), kBool);
  addMethod(view, "acceptsFirstResponder", imp(&yesImp), kBool);
  addMethod(view, "acceptsFirstMouse:", imp(&yesWithArgImp), kBoolObj);
  for (const char* m : {"mouseDown:", "mouseUp:", "mouseDragged:", "mouseMoved:", "rightMouseDown:", "rightMouseUp:", "rightMouseDragged:",
                        "otherMouseDown:", "otherMouseUp:", "otherMouseDragged:", "mouseEntered:", "mouseExited:"})
    addMethod(view, m, imp(&mouseImp), "v@:@");
  addMethod(view, "scrollWheel:", imp(&scrollImp), "v@:@");
  addMethod(view, "magnifyWithEvent:", imp(&magnifyImp), "v@:@");
  addMethod(view, "keyDown:", imp(&keyImp), "v@:@");
  addMethod(view, "keyUp:", imp(&keyImp), "v@:@");
  addMethod(view, "flagsChanged:", imp(&flagsImp), "v@:@");
  addMethod(view, "cursorUpdate:", imp(&cursorUpdateImp), "v@:@");
  addMethod(view, "viewDidChangeBackingProperties", imp(&backingImp), "v@:");
  R.registerClassPair(view);

  Class wd = R.allocateClassPair(R.getClass("NSObject"), "RegnumWindowDelegate", 0);
  if (!wd) throw std::runtime_error("Не удалось создать класс RegnumWindowDelegate");
  addMethod(wd, "windowShouldClose:", imp(&shouldCloseImp), kBoolObj);
  addMethod(wd, "windowDidResize:", imp(&didResizeImp), "v@:@");
  addMethod(wd, "windowDidChangeBackingProperties:", imp(&didResizeImp), "v@:@");
  addMethod(wd, "windowDidBecomeKey:", imp(&didBecomeKeyImp), "v@:@");
  addMethod(wd, "windowDidResignKey:", imp(&didResignKeyImp), "v@:@");
  addMethod(wd, "windowDidEnterFullScreen:", imp(&didEnterFullScreenImp), "v@:@");
  addMethod(wd, "windowDidExitFullScreen:", imp(&didExitFullScreenImp), "v@:@");
  addMethod(wd, "windowDidDeminiaturize:", imp(&didDeminiaturizeImp), "v@:@");
  R.registerClassPair(wd);

  Class ad = R.allocateClassPair(R.getClass("NSObject"), "RegnumAppDelegate", 0);
  if (!ad) throw std::runtime_error("Не удалось создать класс RegnumAppDelegate");
  addMethod(ad, "applicationShouldTerminate:", imp(&shouldTerminateImp), "Q@:@");
  addMethod(ad, "tick:", imp(&tickImp), "v@:@");
  R.registerClassPair(ad);
  g.classesReady = true;
}

id newObject(const char* className) { return msg<id>(msg<id>(cls(className), "alloc"), "init"); }

void addMenuItem(id menu, const std::string& title, const char* action, const char* key, NSUInteger mask) {
  id item = msg<id>(menu, "addItemWithTitle:action:keyEquivalent:", nsstr(title), sel(action), nsstr(key));
  if (mask) msg<void>(item, "setKeyEquivalentModifierMask:", mask);
}

void buildMenu() {
  id bar = newObject("NSMenu");
  id appItem = newObject("NSMenuItem");
  msg<void>(bar, "addItem:", appItem);
  id appMenu = newObject("NSMenu");
  const std::string& name = g.cfg.appName;
  addMenuItem(appMenu, "Скрыть " + name, "hide:", "h", 0);
  addMenuItem(appMenu, "Скрыть остальные", "hideOtherApplications:", "h", kFlagOption | kFlagCommand);
  msg<void>(appMenu, "addItem:", msg<id>(cls("NSMenuItem"), "separatorItem"));
  addMenuItem(appMenu, "Завершить " + name, "terminate:", "q", 0);
  msg<void>(appItem, "setSubmenu:", appMenu);

  id winItem = newObject("NSMenuItem");
  msg<void>(bar, "addItem:", winItem);
  id winMenu = msg<id>(msg<id>(cls("NSMenu"), "alloc"), "initWithTitle:", nsstr("Окно"));
  addMenuItem(winMenu, "Свернуть", "performMiniaturize:", "m", 0);
  addMenuItem(winMenu, "Во весь экран", "toggleFullScreen:", "f", kFlagControl | kFlagCommand);
  msg<void>(winItem, "setSubmenu:", winMenu);
  msg<void>(g.app, "setMainMenu:", bar);
  msg<void>(g.app, "setWindowsMenu:", winMenu);
  for (id o : {bar, appItem, appMenu, winItem, winMenu}) msg<void>(o, "release");
}

void applyDarkFrame() {
  if (!g.window || !responds(g.window, "setAppearance:")) return;
  id appearance = msg<id>(cls("NSAppearance"), "appearanceNamed:", nsstr(g.dark ? "NSAppearanceNameDarkAqua" : "NSAppearanceNameAqua"));
  msg<void>(g.window, "setAppearance:", appearance);  // до macOS 10.14 тёмной темы нет — nil, оформление по умолчанию
}

std::string autosaveName() {
  if (g.cfg.placementFile.empty()) return {};
  return g.cfg.appName + " " + g.cfg.placementFile;
}

void createWindow() {
  NSRect rect{{0, 0}, {double(g.cfg.width), double(g.cfg.height)}};
  NSUInteger style = 1 | 2 | 4 | 8;  // заголовок, закрытие, сворачивание, изменение размера
  g.window = msg<id>(msg<id>(cls("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", rect, style, NSUInteger(2), NO);
  if (!g.window) throw std::runtime_error("Не удалось создать окно NSWindow");
  msg<void>(g.window, "setReleasedWhenClosed:", NO);
  msg<void>(g.window, "setTitle:", nsstr(g.title));
  msg<void>(g.window, "setAcceptsMouseMovedEvents:", YES);
  msg<void>(g.window, "setContentMinSize:", NSSize{double(g.cfg.minWidth), double(g.cfg.minHeight)});
  msg<void>(g.window, "setCollectionBehavior:", NSUInteger(1) << 7);  // NSWindowCollectionBehaviorFullScreenPrimary
  g.winDelegate = newObject("RegnumWindowDelegate");
  msg<void>(g.window, "setDelegate:", g.winDelegate);

  g.view = msg<id>(msg<id>(cls("RegnumView"), "alloc"), "initWithFrame:", rect);
  msg<void>(g.window, "setContentView:", g.view);
  msg<void>(g.window, "makeFirstResponder:", g.view);
  // Отслеживание мыши во всей видимой области (InVisibleRect не требует обновления при изменении размера).
  NSUInteger opts = 0x01 | 0x02 | 0x04 | 0x40 | 0x200;  // EnteredAndExited | MouseMoved | CursorUpdate | ActiveInActiveApp | InVisibleRect
  id area = msg<id>(msg<id>(cls("NSTrackingArea"), "alloc"), "initWithRect:options:owner:userInfo:", NSRect{{0, 0}, {0, 0}}, opts, g.view, nil);
  msg<void>(g.view, "addTrackingArea:", area);
  msg<void>(area, "release");
  applyDarkFrame();

  std::string name = autosaveName();
  bool restored = !name.empty() && msg<BOOL>(g.window, "setFrameUsingName:", nsstr(name)) != NO;
  if (!name.empty()) msg<BOOL>(g.window, "setFrameAutosaveName:", nsstr(name));
  if (!restored) {
    msg<void>(g.window, "center");
    if (g.cfg.maximized) msg<void>(g.window, "zoom:", nil);
  }
  updateGeometry();
}

void cleanup() {
  if (g.timer) msg<void>(g.timer, "invalidate");
  g.timer = nil;
  if (g.window) {
    msg<void>(g.window, "setDelegate:", nil);
    msg<void>(g.window, "orderOut:", nil);
    msg<void>(g.window, "close");
    msg<void>(g.window, "release");
  }
  if (g.view) msg<void>(g.view, "release");
  if (g.winDelegate) msg<void>(g.winDelegate, "release");
  g.window = g.view = g.winDelegate = nil;
  g.client = nullptr;
  g.ready = false;
  g.queue.clear();
  g.buf.reset();
  g.cw = g.ch = 0;
  g.callbackDepth = 0;
  g.buttonsDown = 0;
  g.keyDown.fill(false);
  g.fullscreen = false;
  g.frameIndex = 0;
}

id wakeEventObject() {
  using Fn = id (*)(id, SEL, NSUInteger, NSPoint, NSUInteger, double, NSInteger, id, short, NSInteger, NSInteger);
  return reinterpret_cast<Fn>(R.msgSend)(cls("NSEvent"), sel("otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:subtype:data1:data2:"),
                                         kEvApplicationDefined, NSPoint{0, 0}, NSUInteger(0), 0.0, NSInteger(0), nil, kWakeSubtype, NSInteger(0),
                                         NSInteger(0));
}

void postWake() {
  Pool pool;  // вызывается и из фоновых потоков
  if (id ev = wakeEventObject()) msg<void>(g.app, "postEvent:atStart:", ev, NO);
}

void onWakeEvent() {
  g.wakePosted = false;
  if (g.wakeEvent.exchange(false)) {
    Event e;
    e.type = EventType::Wake;
    push(std::move(e));
  }
  g.dirty = true;
}

void ensureTimer() {
  if (g.timer) return;
  using Fn = id (*)(id, SEL, double, id, SEL, id, BOOL);
  g.timer = reinterpret_cast<Fn>(R.msgSend)(cls("NSTimer"), sel("timerWithTimeInterval:target:selector:userInfo:repeats:"), g.period,
                                            g.appDelegate, sel("tick:"), nil, YES);
  id loop = msg<id>(cls("NSRunLoop"), "currentRunLoop");
  msg<void>(loop, "addTimer:forMode:", g.timer, R.runLoopCommonModes);
}

void ensureApp() {
  loadRuntime();
  if (g.app) return;
  Pool pool;
  g.app = msg<id>(cls("NSApplication"), "sharedApplication");
  msg<BOOL>(g.app, "setActivationPolicy:", NSInteger(0));  // обычное приложение: Dock и меню
  g.appReady.store(true, std::memory_order_release);
}

}  // namespace

// ================================================================ публичный интерфейс
int run(App& client, const WindowConfig& cfg) {
  if (g.client) throw std::logic_error("platform::run уже выполняется");
  ensureApp();
  if (msg<BOOL>(cls("NSThread"), "isMainThread") == NO) throw std::logic_error("platform::run на macOS — только из главного потока");
  Pool outer;
  registerClasses();
  g.cfg = cfg;
  g.title = cfg.title;
  g.dark = cfg.darkFrame;
  g.quitting = false;
  g.closePending = false;
  g.exitCode = 0;
  g.error = nullptr;
  g.client = &client;
  if (!g.appDelegate) {
    g.appDelegate = newObject("RegnumAppDelegate");
    msg<void>(g.app, "setDelegate:", g.appDelegate);
    buildMenu();
    msg<void>(g.app, "finishLaunching");
  }
  try {
    createWindow();
  } catch (...) {
    cleanup();
    throw;
  }
  g.ready = true;
  pushSizeEvent(EventType::Resize);
  if (g.wakeEvent.exchange(false)) {
    Event e;
    e.type = EventType::Wake;
    push(std::move(e));
  }
  g.wakePosted = false;
  g.dirty = true;
  flushEvents();
  renderNow();
  msg<void>(g.window, "makeKeyAndOrderFront:", nil);
  msg<void>(g.app, "activateIgnoringOtherApps:", YES);
  if (g.pendingFullscreen) {
    g.pendingFullscreen = false;
    msg<void>(g.window, "toggleFullScreen:", nil);
  }

  id distantFuture = msg<id>(cls("NSDate"), "distantFuture");
  id distantPast = msg<id>(cls("NSDate"), "distantPast");
  while (!g.quitting) {
    Pool pool;
    // Ждать событие: без дела — бесконечно, при незавершённом кадре — до срока следующего кадра.
    id until = distantFuture;
    double now = time();
    if (g.dirty) {
      double due = g.lastFrame + g.period;
      until = now >= due ? distantPast : msg<id>(cls("NSDate"), "dateWithTimeIntervalSinceNow:", due - now);
    }
    using NextFn = id (*)(id, SEL, unsigned long long, id, id, BOOL);
    auto next = reinterpret_cast<NextFn>(R.msgSend);
    SEL nextSel = sel("nextEventMatchingMask:untilDate:inMode:dequeue:");
    for (id ev = next(g.app, nextSel, ~0ULL, until, R.defaultRunLoopMode, YES); ev;
         ev = next(g.app, nextSel, ~0ULL, distantPast, R.defaultRunLoopMode, YES)) {
      NSUInteger type = msg<NSUInteger>(ev, "type");
      if (type == kEvApplicationDefined && msg<short>(ev, "subtype") == kWakeSubtype) {
        onWakeEvent();
        continue;
      }
      // NSApplication не доставляет окну KeyUp при зажатой ⌘ — отдаём напрямую.
      if (type == kEvKeyUp && (msg<NSUInteger>(ev, "modifierFlags") & kFlagCommand)) msg<void>(g.window, "sendEvent:", ev);
      else msg<void>(g.app, "sendEvent:", ev);
      if (g.quitting) break;
    }
    if (g.quitting) break;
    flushEvents();
    if (g.closePending && g.callbackDepth == 0) {
      g.closePending = false;
      requestClose();
    }
    if (g.quitting) break;
    bool anim = callAnimating();
    if (anim) ensureTimer();  // кадры анимации рисует таймер (и во время модальных циклов AppKit)
    now = time();
    if (g.dirty && !anim && now - g.lastFrame >= g.period - 0.0005) {
      g.lastFrame = now;
      renderNow();
    }
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
  if (g.appReady.load(std::memory_order_acquire)) postWake();
}

void invalidate() {
  bool was = g.dirty.exchange(true);
  if (was || !g.appReady.load(std::memory_order_acquire)) return;
  if (msg<BOOL>(cls("NSThread"), "isMainThread") == NO) postWake();
  else if (g.ready) ensureTimer();  // из обратного вызова AppKit (цикл ждёт событие) — кадр нарисует таймер
}

void wake() {
  g.wakeEvent = true;
  if (!g.appReady.load(std::memory_order_acquire)) return;  // до run(): Wake придёт первым событием
  if (!g.wakePosted.exchange(true)) postWake();
}

void setCursor(Cursor c) {
  g.cursor = c;
  if (g.window && g.mouseInside) applyCursor();
}

void setTitle(const std::string& title) {
  g.title = title;
  if (g.window) {
    Pool pool;
    msg<void>(g.window, "setTitle:", nsstr(title));
  }
}

void setFullscreen(bool on) {
  if (!g.window) {
    g.pendingFullscreen = on;
    return;
  }
  if (on != g.fullscreen) msg<void>(g.window, "toggleFullScreen:", nil);  // анимация системы; флаг — по уведомлению
}

bool fullscreen() { return g.window ? g.fullscreen : g.pendingFullscreen; }

void setDarkFrame(bool dark) {
  g.dark = dark;
  Pool pool;
  applyDarkFrame();
}

void setTextInputRect(RectF caret) { g.caret = caret; }

float scale() {
  if (g.window) return g.scale;
  ensureApp();
  Pool pool;
  id screen = msg<id>(cls("NSScreen"), "mainScreen");
  return screen ? float(msg<CGFloat>(screen, "backingScaleFactor")) : 1.f;
}

void* nativeWindow() { return g.window; }

std::string clipboardText() {
  ensureApp();
  Pool pool;
  id pb = msg<id>(cls("NSPasteboard"), "generalPasteboard");
  return detail::fromCrlf(fromNs(msg<id>(pb, "stringForType:", R.pasteboardTypeString)));
}

void setClipboardText(const std::string& text) {
  ensureApp();
  Pool pool;
  id pb = msg<id>(cls("NSPasteboard"), "generalPasteboard");
  msg<NSInteger>(pb, "clearContents");
  msg<BOOL>(pb, "setString:forType:", nsstr(text), R.pasteboardTypeString);
}

namespace {

enum class DialogKind { Open, Save, Folder };

std::optional<std::string> fileDialog(DialogKind kind, const std::string& title, const std::vector<FileFilter>& filters,
                                      const std::string& startDir, const std::string& defaultName) {
  ensureApp();
  Pool pool;
  id panel = kind == DialogKind::Save ? msg<id>(cls("NSSavePanel"), "savePanel") : msg<id>(cls("NSOpenPanel"), "openPanel");
  if (!title.empty()) {
    msg<void>(panel, "setTitle:", nsstr(title));
    msg<void>(panel, "setMessage:", nsstr(title));
  }
  msg<void>(panel, "setCanCreateDirectories:", YES);
  if (kind != DialogKind::Save) {
    msg<void>(panel, "setCanChooseFiles:", kind == DialogKind::Open ? YES : NO);
    msg<void>(panel, "setCanChooseDirectories:", kind == DialogKind::Folder ? YES : NO);
    msg<void>(panel, "setAllowsMultipleSelection:", NO);
  }
  if (kind != DialogKind::Folder) {
    bool any = filters.empty();
    id types = msg<id>(cls("NSMutableArray"), "array");
    for (const FileFilter& f : filters) {
      if (f.exts.empty()) any = true;
      for (const std::string& e : f.exts) msg<void>(types, "addObject:", nsstr(e));
    }
    if (!any) msg<void>(panel, "setAllowedFileTypes:", types);
  }
  if (!startDir.empty()) msg<void>(panel, "setDirectoryURL:", msg<id>(cls("NSURL"), "fileURLWithPath:", nsstr(startDir)));
  if (kind == DialogKind::Save && !defaultName.empty()) msg<void>(panel, "setNameFieldStringValue:", nsstr(defaultName));
  g.callbackDepth++;  // модальный цикл панели: события окна откладываются
  NSInteger r = msg<NSInteger>(panel, "runModal");
  g.callbackDepth--;
  if (g.window) msg<void>(g.window, "makeKeyAndOrderFront:", nil);
  g.dirty = true;
  if (r != 1) return std::nullopt;  // NSModalResponseOK
  id url = msg<id>(panel, "URL");
  if (!url) return std::nullopt;
  std::string path = fromNs(msg<id>(url, "path"));
  if (path.empty()) return std::nullopt;
  return path;
}

}  // namespace

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
  ensureApp();
  Pool pool;
  id url = msg<id>(cls("NSURL"), "fileURLWithPath:", nsstr(path));
  id ws = msg<id>(cls("NSWorkspace"), "sharedWorkspace");
  return url && msg<BOOL>(ws, "openURL:", url) != NO;
}

bool openUrl(const std::string& u) {
  if (!detail::isSafeUrl(u)) return false;
  ensureApp();
  Pool pool;
  id url = msg<id>(cls("NSURL"), "URLWithString:", nsstr(u));
  id ws = msg<id>(cls("NSWorkspace"), "sharedWorkspace");
  return url && msg<BOOL>(ws, "openURL:", url) != NO;
}

void showFatal(const std::string& title, const std::string& text) {
  std::fprintf(stderr, "%s: %s\n", title.c_str(), text.c_str());
  std::fflush(stderr);
  try {
    ensureApp();
  } catch (const std::exception&) {
    return;  // AppKit недоступен — остаётся сообщение в stderr
  }
  Pool pool;
  msg<void>(g.app, "activateIgnoringOtherApps:", YES);
  id alert = newObject("NSAlert");
  msg<void>(alert, "setMessageText:", nsstr(title));
  msg<void>(alert, "setInformativeText:", nsstr(text));
  msg<void>(alert, "setAlertStyle:", NSUInteger(2));  // NSAlertStyleCritical
  g.callbackDepth++;
  msg<NSInteger>(alert, "runModal");
  g.callbackDepth--;
  msg<void>(alert, "release");
}

double time() { return detail::monotonicSeconds(); }

}  // namespace rg::platform
