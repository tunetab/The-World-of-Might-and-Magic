// Regnum — объявления Xlib (ABI libX11.so.6) без системных заголовков X11.
// Раскладка структур совпадает с <X11/Xlib.h> на 64-битном Linux (LP64); проверяется static_assert ниже.
// Только для x11.cpp. Не включает стандартных заголовков — проверяется отдельно под целью Linux.
#pragma once

namespace rg::platform::xlib {

using XID = unsigned long;
using Window = XID;
using Drawable = XID;
using Pixmap = XID;
using XCursor = XID;   // не путать с rg::platform::Cursor
using Colormap = XID;
using KeySym = XID;
using Atom = unsigned long;
using Time = unsigned long;
using VisualID = unsigned long;
using Bool = int;
using Status = int;
using KeyCode = unsigned char;
using XPointer = char*;
using XIMStyle = unsigned long;

struct Display;
struct Visual;
struct XIMRec;
struct XICRec;
struct GCRec;
using XIM = XIMRec*;
using XIC = XICRec*;
using GC = GCRec*;

// ---------------------------------------------------------------- события
struct XAnyEvent { int type; unsigned long serial; Bool send_event; Display* display; Window window; };
struct XKeyEvent {
  int type; unsigned long serial; Bool send_event; Display* display; Window window;
  Window root; Window subwindow; Time time; int x, y; int x_root, y_root; unsigned int state; unsigned int keycode; Bool same_screen;
};
struct XButtonEvent {
  int type; unsigned long serial; Bool send_event; Display* display; Window window;
  Window root; Window subwindow; Time time; int x, y; int x_root, y_root; unsigned int state; unsigned int button; Bool same_screen;
};
struct XMotionEvent {
  int type; unsigned long serial; Bool send_event; Display* display; Window window;
  Window root; Window subwindow; Time time; int x, y; int x_root, y_root; unsigned int state; char is_hint; Bool same_screen;
};
struct XCrossingEvent {
  int type; unsigned long serial; Bool send_event; Display* display; Window window;
  Window root; Window subwindow; Time time; int x, y; int x_root, y_root; int mode; int detail; Bool same_screen; Bool focus; unsigned int state;
};
struct XFocusChangeEvent { int type; unsigned long serial; Bool send_event; Display* display; Window window; int mode; int detail; };
struct XExposeEvent { int type; unsigned long serial; Bool send_event; Display* display; Window window; int x, y; int width, height; int count; };
struct XConfigureEvent {
  int type; unsigned long serial; Bool send_event; Display* display; Window event; Window window;
  int x, y; int width, height; int border_width; Window above; Bool override_redirect;
};
struct XPropertyEvent { int type; unsigned long serial; Bool send_event; Display* display; Window window; Atom atom; Time time; int state; };
struct XSelectionClearEvent { int type; unsigned long serial; Bool send_event; Display* display; Window window; Atom selection; Time time; };
struct XSelectionRequestEvent {
  int type; unsigned long serial; Bool send_event; Display* display; Window owner; Window requestor; Atom selection; Atom target; Atom property; Time time;
};
struct XSelectionEvent {
  int type; unsigned long serial; Bool send_event; Display* display; Window requestor; Atom selection; Atom target; Atom property; Time time;
};
struct XClientMessageEvent {
  int type; unsigned long serial; Bool send_event; Display* display; Window window; Atom message_type; int format;
  union { char b[20]; short s[10]; long l[5]; } data;
};
struct XMappingEvent { int type; unsigned long serial; Bool send_event; Display* display; Window window; int request; int first_keycode; int count; };
union XEvent {
  int type;
  XAnyEvent xany;
  XKeyEvent xkey;
  XButtonEvent xbutton;
  XMotionEvent xmotion;
  XCrossingEvent xcrossing;
  XFocusChangeEvent xfocus;
  XExposeEvent xexpose;
  XConfigureEvent xconfigure;
  XPropertyEvent xproperty;
  XSelectionClearEvent xselectionclear;
  XSelectionRequestEvent xselectionrequest;
  XSelectionEvent xselection;
  XClientMessageEvent xclient;
  XMappingEvent xmapping;
  long pad[24];
};
struct XErrorEvent { int type; Display* display; XID resourceid; unsigned long serial; unsigned char error_code; unsigned char request_code; unsigned char minor_code; };

// ---------------------------------------------------------------- прочие структуры
struct XVisualInfo {
  Visual* visual; VisualID visualid; int screen; int depth; int c_class;
  unsigned long red_mask; unsigned long green_mask; unsigned long blue_mask; int colormap_size; int bits_per_rgb;
};
struct XSetWindowAttributes {
  Pixmap background_pixmap; unsigned long background_pixel; Pixmap border_pixmap; unsigned long border_pixel;
  int bit_gravity; int win_gravity; int backing_store; unsigned long backing_planes; unsigned long backing_pixel;
  Bool save_under; long event_mask; long do_not_propagate_mask; Bool override_redirect; Colormap colormap; XCursor cursor;
};
struct XSizeHints {
  long flags; int x, y; int width, height; int min_width, min_height; int max_width, max_height; int width_inc, height_inc;
  struct { int x; int y; } min_aspect, max_aspect;
  int base_width, base_height; int win_gravity;
};
struct XImage;
struct XImageFuncs {
  XImage* (*create_image)(Display*, Visual*, unsigned int, int, int, char*, unsigned int, unsigned int, int, int);
  int (*destroy_image)(XImage*);
  unsigned long (*get_pixel)(XImage*, int, int);
  int (*put_pixel)(XImage*, int, int, unsigned long);
  XImage* (*sub_image)(XImage*, int, int, unsigned int, unsigned int);
  int (*add_pixel)(XImage*, long);
};
struct XImage {
  int width, height; int xoffset; int format; char* data; int byte_order; int bitmap_unit; int bitmap_bit_order; int bitmap_pad;
  int depth; int bytes_per_line; int bits_per_pixel; unsigned long red_mask; unsigned long green_mask; unsigned long blue_mask;
  XPointer obdata; XImageFuncs f;
};
struct XPixmapFormatValues { int depth; int bits_per_pixel; int scanline_pad; };
struct XComposeStatus { XPointer compose_ptr; int chars_matched; };

// ---------------------------------------------------------------- константы
constexpr int KeyPress = 2, KeyRelease = 3, ButtonPress = 4, ButtonRelease = 5, MotionNotify = 6, EnterNotify = 7, LeaveNotify = 8,
              FocusIn = 9, FocusOut = 10, Expose = 12, DestroyNotify = 17, UnmapNotify = 18, MapNotify = 19, ConfigureNotify = 22,
              PropertyNotify = 28, SelectionClear = 29, SelectionRequest = 30, SelectionNotify = 31, ClientMessage = 33, MappingNotify = 34;

constexpr long NoEventMask = 0, KeyPressMask = 1L << 0, KeyReleaseMask = 1L << 1, ButtonPressMask = 1L << 2, ButtonReleaseMask = 1L << 3,
               EnterWindowMask = 1L << 4, LeaveWindowMask = 1L << 5, PointerMotionMask = 1L << 6, ExposureMask = 1L << 15,
               StructureNotifyMask = 1L << 17, SubstructureNotifyMask = 1L << 19, SubstructureRedirectMask = 1L << 20,
               FocusChangeMask = 1L << 21, PropertyChangeMask = 1L << 22;

constexpr unsigned int ShiftMask = 1u << 0, LockMask = 1u << 1, ControlMask = 1u << 2, Mod1Mask = 1u << 3, Mod2Mask = 1u << 4,
                       Mod3Mask = 1u << 5, Mod4Mask = 1u << 6, Mod5Mask = 1u << 7;

constexpr unsigned long CWBackPixel = 1UL << 1, CWBorderPixel = 1UL << 3, CWBitGravity = 1UL << 4, CWEventMask = 1UL << 11,
                        CWColormap = 1UL << 13;

constexpr long USPosition = 1L << 0, USSize = 1L << 1, PPosition = 1L << 2, PSize = 1L << 3, PMinSize = 1L << 4;

constexpr int InputOutput = 1, TrueColor = 4, AllocNone = 0, ZPixmap = 2, LSBFirst = 0, MSBFirst = 1, NorthWestGravity = 1;
constexpr int PropModeReplace = 0, PropModeAppend = 2, PropertyNewValue = 0, PropertyDelete = 1;
constexpr int NotifyNormal = 0, NotifyGrab = 1, NotifyUngrab = 2;
constexpr Atom XA_PRIMARY = 1, XA_ATOM = 4, XA_CARDINAL = 6, XA_STRING = 31, XA_WINDOW = 33, XA_WM_CLASS = 67;
constexpr Atom AnyPropertyType = 0;
constexpr XID None = 0;
constexpr Time CurrentTime = 0;
constexpr Bool True = 1, False = 0;
constexpr int Success = 0;
constexpr int QueuedAlready = 0, QueuedAfterReading = 1;

constexpr XIMStyle XIMPreeditNothing = 0x0008L, XIMStatusNothing = 0x0400L;
constexpr int XBufferOverflow = -1, XLookupNone = 1, XLookupChars = 2, XLookupKeySym = 3, XLookupBoth = 4;

// Шрифт курсоров (cursorfont.h).
constexpr unsigned int XC_X_cursor = 0, XC_bottom_left_corner = 12, XC_bottom_right_corner = 14, XC_circle = 24, XC_crosshair = 34,
                       XC_fleur = 52, XC_hand1 = 58, XC_hand2 = 60, XC_left_ptr = 68, XC_sb_h_double_arrow = 108,
                       XC_sb_v_double_arrow = 116, XC_watch = 150, XC_xterm = 152;

// ---------------------------------------------------------------- проверка раскладки (LP64)
#if defined(__linux__) && defined(__LP64__)
#define RG_XOFF(T, f) __builtin_offsetof(T, f)
static_assert(sizeof(XEvent) == 192, "XEvent");
static_assert(sizeof(XAnyEvent) == 40 && RG_XOFF(XAnyEvent, window) == 32, "XAnyEvent");
static_assert(sizeof(XKeyEvent) == 96 && RG_XOFF(XKeyEvent, time) == 56 && RG_XOFF(XKeyEvent, x) == 64 &&
              RG_XOFF(XKeyEvent, state) == 80 && RG_XOFF(XKeyEvent, keycode) == 84 && RG_XOFF(XKeyEvent, same_screen) == 88, "XKeyEvent");
static_assert(sizeof(XButtonEvent) == 96 && RG_XOFF(XButtonEvent, button) == 84, "XButtonEvent");
static_assert(sizeof(XMotionEvent) == 96 && RG_XOFF(XMotionEvent, is_hint) == 84 && RG_XOFF(XMotionEvent, same_screen) == 88, "XMotionEvent");
static_assert(sizeof(XCrossingEvent) == 104 && RG_XOFF(XCrossingEvent, mode) == 80 && RG_XOFF(XCrossingEvent, state) == 96, "XCrossingEvent");
static_assert(sizeof(XFocusChangeEvent) == 48 && RG_XOFF(XFocusChangeEvent, mode) == 40, "XFocusChangeEvent");
static_assert(sizeof(XExposeEvent) == 64 && RG_XOFF(XExposeEvent, count) == 56, "XExposeEvent");
static_assert(sizeof(XConfigureEvent) == 88 && RG_XOFF(XConfigureEvent, width) == 56 && RG_XOFF(XConfigureEvent, above) == 72, "XConfigureEvent");
static_assert(sizeof(XPropertyEvent) == 64 && RG_XOFF(XPropertyEvent, atom) == 40 && RG_XOFF(XPropertyEvent, state) == 56, "XPropertyEvent");
static_assert(sizeof(XSelectionClearEvent) == 56 && RG_XOFF(XSelectionClearEvent, selection) == 40, "XSelectionClearEvent");
static_assert(sizeof(XSelectionRequestEvent) == 80 && RG_XOFF(XSelectionRequestEvent, requestor) == 40 &&
              RG_XOFF(XSelectionRequestEvent, property) == 64 && RG_XOFF(XSelectionRequestEvent, time) == 72, "XSelectionRequestEvent");
static_assert(sizeof(XSelectionEvent) == 72 && RG_XOFF(XSelectionEvent, property) == 56, "XSelectionEvent");
static_assert(sizeof(XClientMessageEvent) == 96 && RG_XOFF(XClientMessageEvent, format) == 48 && RG_XOFF(XClientMessageEvent, data) == 56,
              "XClientMessageEvent");
static_assert(sizeof(XMappingEvent) == 56 && RG_XOFF(XMappingEvent, count) == 48, "XMappingEvent");
static_assert(sizeof(XErrorEvent) == 40 && RG_XOFF(XErrorEvent, error_code) == 32, "XErrorEvent");
static_assert(sizeof(XVisualInfo) == 64 && RG_XOFF(XVisualInfo, depth) == 20 && RG_XOFF(XVisualInfo, red_mask) == 32, "XVisualInfo");
static_assert(sizeof(XSetWindowAttributes) == 112 && RG_XOFF(XSetWindowAttributes, event_mask) == 72 &&
              RG_XOFF(XSetWindowAttributes, colormap) == 96, "XSetWindowAttributes");
static_assert(sizeof(XSizeHints) == 80 && RG_XOFF(XSizeHints, min_width) == 24 && RG_XOFF(XSizeHints, win_gravity) == 72, "XSizeHints");
static_assert(sizeof(XImage) == 136 && RG_XOFF(XImage, data) == 16 && RG_XOFF(XImage, bytes_per_line) == 44 &&
              RG_XOFF(XImage, red_mask) == 56 && RG_XOFF(XImage, obdata) == 80 && RG_XOFF(XImage, f) == 88, "XImage");
static_assert(sizeof(XPixmapFormatValues) == 12, "XPixmapFormatValues");
#undef RG_XOFF
#endif

}  // namespace rg::platform::xlib
