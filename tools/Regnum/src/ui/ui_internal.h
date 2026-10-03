// Regnum — внутреннее устройство интерфейса: контекст, окна-слои, списки отрисовки, раскладка.
// Только для src/ui/*.cpp.
#pragma once
#include <deque>
#include <unordered_map>
#include <unordered_set>

#include "gfx/icons.h"
#include "ui/ui.h"

namespace rg::ui::in {

// ---------------------------------------------------------------- список отрисовки
enum class Op : u8 { None, Rect, RectStroke, Gradient, Shadow, Circle, Ring, Line, Path, PathStroke, Icon, Text, Image, Custom, ClipPush, ClipPop };

struct TextEntry;

struct Cmd {
  Op op = Op::None;
  u8 flags = 0;           // Gradient: 1 — горизонтальный; PathStroke: Cap
  float w = 0;            // толщина / размытие
  RectF r;                // прямоугольник; Line: (x0, y0, x1, y1); Circle: (cx, cy, r, —)
  float rad = 0;
  float dy = 0, spread = 0;
  Color c, c2;
  u32 a = 0, n = 0;       // индекс в боковых массивах / длина строки в арене
  TextEntry* text = nullptr;
  const gfx::Image* img = nullptr;
};

struct DrawList {
  std::vector<Cmd> cmds;
  std::vector<gfx::Path> paths;
  std::vector<CustomDraw> customs;
  std::vector<std::pair<float, Color>> stops;   // градиенты с несколькими точками (Cmd::n > 0)
  std::string arena;      // имена значков
  void clear() {
    cmds.clear();
    paths.clear();
    customs.clear();
    stops.clear();
    arena.clear();
  }
};

// Внутренний флаг interact: поле ввода (Enter/Пробел не «нажимают» его).
constexpr u32 IfTextInput = 1u << 30;

// ---------------------------------------------------------------- кеш раскладок текста
struct TextEntry {
  std::string text;
  gfx::TextLayout layout;
  u64 key = 0;
  u64 lastFrame = 0;
};

// ---------------------------------------------------------------- окна-слои
enum class WinKind : u8 { Root, Panel, Popup, Modal, Tooltip, Toast, Drag };

// Ранги слоёв: основной 0, всплывающие 10, модальное k-го уровня 20 + 10k (его всплывающие +5),
// уведомления 1000, подсказка 1100, перетаскиваемый объект 1200.
constexpr int kRankBase = 0, kRankPopup = 10, kRankModal = 20, kRankToast = 1000, kRankTooltip = 1100, kRankDrag = 1200;

struct Window {
  WidgetId id = 0;
  WinKind kind = WinKind::Root;
  int rank = 0;
  u32 order = 0;
  RectF rect;             // этого кадра
  RectF prevRect;         // прошлого кадра (наведение)
  RectF inputRect;        // прямоугольник для наведения (по прошлому кадру)
  u64 lastFrame = 0;
  bool hidden = false;    // кадр замера: не рисуется, ввод не принимает
  bool wasHidden = false;
  float alpha = 1;
  float dy = 0;           // сдвиг появления
  WidgetId scope = 0;     // область фокуса (0 — основная)
  bool blur = false;      // размыть фон под окном (стеклянная панель)
  float blurRadius = 0;
  bool backdrop = false;  // затемнение и размытие всего ниже (модальное)
  DrawList list;
  Vec2 contentSize;       // замер содержимого (для всплывающих и модальных)
  Vec2 prevContentSize;
  bool measured = false;
};

// ---------------------------------------------------------------- раскладка
enum class Flow : u8 { Vertical, Row, HStack };

struct Frame {
  Window* win = nullptr;
  Flow flow = Flow::Vertical;
  RectF box;              // область содержимого: x, w; y — верх; h — может быть бесконечной
  float x = 0, y = 0;     // курсор
  float gap = 8;
  float bottom = 0;       // нижний край размещённого
  float right = 0;
  RectF clip;             // действующее отсечение (точки)
  bool pushedClip = false;
  float indent = 0;
  // строка
  std::vector<float> colX, colW;
  int col = 0, ncols = 0;
  float rowH = 0, rowTop = 0, rowMax = 0;
  bool rowAuto = false;
  // HStack
  float stackH = 0;
  Align align = Align::Left;
  WidgetId stackId = 0;
  float stackStart = 0, flexAt = -1, flexShift = 0;
  // прокрутка
  WidgetId scrollId = 0;
  float scrollY = 0;
  Align slotAlign = Align::Left;
  // учёт контейнера
  enum Kind : u8 { Plain, CardK, SectionK, ScrollK, RowK, StackK, GroupK, PanelK, AreaK, PopupK } kind = Plain;
  RectF slot;             // слот в родителе (для прокрутки — видимая область)
  bool slotOneShot = false;
  size_t marks[3] = {size_t(-1), size_t(-1), size_t(-1)};
  WidgetId cid = 0;
  float t = 1;            // раскрытие раздела
  float bodyTop = 0;
  float pad = 0;
  float headerH = 0;
  bool horizontal = false;
  bool card = true;
  bool placed = false;    // что-то размещено
  Color surface{0, 0, 0, 0};   // цвет подложки (для затуханий у краёв прокрутки); a = 0 — как у родителя
};

// ---------------------------------------------------------------- всплывающие и модальные
struct PopupEntry {
  WidgetId id = 0;
  WidgetId opener = 0;
  RectF anchor;
  u32 seq = 0;
  int parentRank = 0;
  bool seen = false;      // begin вызван в этом кадре
  bool menu = false;
  double shownAt = -1;
  // меню: подсветка с клавиатуры
  int highlight = -1, count = 0, run = 0;
  bool activate = false;
};

struct ModalEntry {
  WidgetId id = 0;
  u32 seq = 0;
  bool seen = false;
  bool dismiss = false;      // закрывать щелчком по фону (с прошлого кадра)
  bool closeReq = false;
  double shownAt = -1;
};

struct ToastEntry {
  u64 id = 0;
  std::string text;
  Tone tone = Tone::Info;
  std::string icon;
  double start = 0, duration = 4, paused = 0, closeAt = -1;
  float height = 0;
  bool hovered = false;
};

struct Ghost {                 // исчезающее модальное окно (копия последнего списка)
  DrawList list;
  RectF rect;
  int rank = 0;
  double start = 0;
  bool backdrop = false;
};

struct KeyEv {
  platform::Event e;
  bool consumed = false;
};

struct StateEntry {
  std::unique_ptr<detail::StateBox> box;
  const void* tag = nullptr;
  u64 lastFrame = 0;
  double lastTime = 0;
};

struct Anim {
  float from = 0, to = 0, value = 0;
  double start = 0;
  float dur = 0;
  u64 lastFrame = 0;
};

struct Ctx {
  bool inited = false;
  Theme th;
  float uiScale = 1;
  // кадр
  float dpi = 1, ds = 1;       // ds = dpi × uiScale
  RectF view;
  double time = 0, prevTime = 0;
  float dt = 0;
  u64 frame = 0;
  bool inFrame = false;
  // ввод
  std::deque<platform::Event> queue;
  Mouse m;
  bool pressedEv[3]{}, releasedEv[3]{};
  int clicks = 0;
  bool mouseInside = false;
  bool mouseMoved = false;     // указатель сдвинулся в этом кадре
  float pressX = 0, pressY = 0;
  bool rightReleaseClaimed = false;
  std::vector<KeyEv> kbd;
  bool wheelPrecise = false;
  // ID
  std::vector<u64> idStack;
  std::unordered_set<WidgetId> seenIds;
  std::unordered_set<WidgetId> warnedIds;
  // взаимодействие
  WidgetId hot = 0;            // наведённый в этом кадре
  WidgetId hotLast = 0, hotLastPrev = 0;   // последний элемент под указателем (для перекрытий)
  WidgetId active = 0;
  int activeButton = 0;
  bool activeSeen = false;
  bool activeDragging = false;
  bool pressClaimed = false;
  WidgetId focus = 0;
  bool focusVisible = false;
  bool focusSeen = false;
  WidgetId focusScope = 0;
  bool focusJustMoved = false;  // фокус перешёл с клавиатуры — прокрутить к элементу
  WidgetId autofocusDone = 0;
  struct Focusable { WidgetId id; WidgetId scope; };
  std::vector<Focusable> focusList, prevFocusList;
  // окна
  std::unordered_map<WidgetId, std::unique_ptr<Window>> windows;
  std::vector<Window*> winStack;
  Window* win = nullptr;
  Window* root = nullptr;
  Window* hoveredWin = nullptr;
  std::vector<Window*> frameWins;
  u32 panelSeq = 0;
  // раскладка
  std::vector<Frame> frames;
  std::optional<RectF> oneShot;
  Align oneShotAlign = Align::Left;
  u32 autoSeq = 0;
  int disabled = 0;
  // последний элемент
  Item last;
  // состояние
  std::unordered_map<WidgetId, StateEntry> states;
  std::unordered_map<WidgetId, Anim> anims;
  std::unordered_map<WidgetId, float> sticky;   // долговременные мелочи (раскрытие разделов, сортировка)
  bool animating = false;
  bool redraw = false;
  // текст
  std::unordered_map<u64, std::unique_ptr<TextEntry>> texts;
  // всплывающие
  std::vector<PopupEntry> popups;
  u32 popupSeq = 0;
  WidgetId closedOpener = 0;   // всплывающее закрыто нажатием на свой открыватель в этом кадре
  WidgetId suppressPopup = 0;  // это всплывающее закрыто нажатием на открыватель — не открывать по тому же щелчку
  std::vector<ModalEntry> modals;
  u32 modalSeq = 0;
  WidgetId pendingDefault = 0;
  struct ModalCtx { WidgetId id; WidgetId defaultBtn; bool* open; bool dismiss; bool closeReq; };
  std::vector<ModalCtx> modalStack;
  std::vector<Ghost> ghosts;
  // подсказка
  WidgetId tipItem = 0;
  double tipStart = 0, tipLastShown = -10;
  bool tipRequested = false, tipPending = false;
  RectF tipAnchor;
  std::string tipText;
  Shortcut tipShortcut;
  bool tipRich = false;
  bool tipTouched = false;
  double tipShownAt = 0;
  WidgetId tipShownItem = 0;
  // уведомления
  std::vector<ToastEntry> toasts;
  u64 toastSeq = 0;
  // перетаскивание
  struct Drag {
    bool active = false;
    std::string type;
    u64 payload = 0;
    std::string label, icon;
    WidgetId source = 0;
  } drag;
  // буфер обмена
  std::function<std::string()> clipGet;
  std::function<void(const std::string&)> clipSet;
  std::string clipLocal;
  // кеш размытого фона под модальным окном
  gfx::Image backdrop;
  u64 backdropHash = 0;
  bool backdropValid = false;
  // вывод
  platform::Cursor cursor = platform::Cursor::Arrow;
  std::optional<RectF> textInput;
  bool caretBlink = false;
  double lastInputTime = 0;
};

Ctx& C();

// Недоступность на время виджета (опция disabled).
struct DisableGuard {
  bool on;
  explicit DisableGuard(bool d) : on(d) {
    if (on) C().disabled++;
  }
  ~DisableGuard() {
    if (on && C().disabled > 0) C().disabled--;
  }
  DisableGuard(const DisableGuard&) = delete;
  DisableGuard& operator=(const DisableGuard&) = delete;
};

// ---------------------------------------------------------------- служебное
inline Color withAlpha(Color c, float k) { return c.alpha(k); }
inline Color mixc(Color a, Color b, float t) { return Color::mix(a, b, clamp(t, 0.f, 1.f)); }
float easeOut(float t);
const gfx::TextStyle& styleOf(Font f);
gfx::TextStyle styleWith(Font f, gfx::FontWeight w);
// Раскладка из кеша (живёт до конца кадра и дольше, пока используется).
TextEntry* cachedText(std::string_view s, const gfx::TextStyle& st, float maxW = 0, int maxLines = 0, bool ellipsis = false,
                      Align align = Align::Left, bool wrap = false);
float textWidth(std::string_view s, const gfx::TextStyle& st);

// Запись в список текущего окна (координаты — точки).
void cmdRect(RectF r, Color c, float rad = 0);
void cmdRect4(RectF r, Color c, float tl, float tr, float br, float bl);
void cmdStroke(RectF r, Color c, float rad, float w);
void cmdShadow(RectF r, float rad, float blur, Color c, float dy = 0, float spread = 0);
void cmdText(TextEntry* t, float x, float y, Color c);
void cmdIcon(std::string_view name, RectF r, Color c);
void cmdGradient(RectF r, float rad, bool horizontal, std::span<const std::pair<float, Color>> stops);
size_t cmdMark();                       // место для заполнения позже (фон карточки)
void cmdPatchRect(size_t at, RectF r, Color c, float rad);
void cmdPatchStroke(size_t at, RectF r, Color c, float rad, float w);
void cmdPatchShadow(size_t at, RectF r, float rad, float blur, Color c, float dy);
// Текст в прямоугольнике: выравнивание по горизонтали, по вертикали — по центру; многоточие при нехватке ширины.
void textIn(std::string_view s, RectF r, const gfx::TextStyle& st, Color c, Align a = Align::Left);
float alphaMul();                       // приглушение недоступных
std::string unitFor(const char* unit, double v, int digits);   // единица с русским склонением («ход|хода|ходов»)

// Раскладка
Frame& frame();
RectF place(float w, float h);          // w ≤ 0 — на всю ширину слота
RectF slotBegin(float w, float h, bool* oneShot);   // слот для контейнера (высота станет известна в slotEnd)
void slotEnd(RectF used);
void pushFrame(const Frame& f);
void popFrame();
RectF currentClip();
void pushClipFrame(RectF clip);         // для собственных контейнеров (отсечение и команда)
WidgetId autoId(const char* kind);
void hstackBegin(float height, Align align, float gap);
void hstackEnd();

// Окна
Window* beginWindow(WidgetId id, WinKind kind, int rank, u32 order, RectF r);
void endWindow();
bool isDisabled();
bool modalBlocks();                     // текущее окно ниже верхнего модального

// Взаимодействие и фокус
void setFocus(WidgetId id, bool visible);
void clearFocus();
void registerFocusable(WidgetId id);
bool registerId(WidgetId id);
void setLast(WidgetId id, RectF r, const Interaction& it);
void focusRing(RectF r, float radius);
bool takeKey(Key k, u32 mods = 0);      // найти непоглощённое нажатие и поглотить
bool hasKey(Key k, u32 mods = 0);
u32 normMods(u32 mods);
std::string clipboardGet();
void clipboardSet(const std::string& s);
void markInput();                       // перезапустить мигание каретки

// Всплывающие (общая основа для меню, выпадающих списков, выбора цвета)
int currentPopupLevel();
void openPopupAt(WidgetId id, RectF anchor, WidgetId opener, bool menu);
bool popupOpen(WidgetId id);
bool beginPopupId(WidgetId id, const PopupOpt& o, bool menu);
void closePopupsFrom(int level);
int popupIndex(WidgetId id);

// Текстовый редактор (общий для полей, чисел, поиска)
struct TextEdit {
  std::string buf, original;
  size_t caret = 0, anchor = 0;
  float scrollX = 0, scrollY = 0;
  float desiredX = -1;
  bool editing = false;
  bool mouseSelecting = false;
  int selMode = 0;              // 0 символы, 1 слова, 2 всё
  size_t wordA = 0, wordB = 0;
  struct Snap { std::string text; size_t caret, anchor; };
  std::vector<Snap> undo, redo;
  double lastTypeTime = -10;
  bool typingGroup = false;
  bool autoDone = false;        // автофокус уже выполнен
  size_t prevCaret = size_t(-1), prevSize = size_t(-1);
};
struct EditParams {
  bool multiline = false;
  bool readOnly = false;
  int maxLength = 0;
  bool (*filter)(u32) = nullptr;
  RectF textRect;               // область текста (точки)
  const gfx::TextStyle* style = nullptr;
};
struct EditResult {
  bool edited = false;          // буфер изменён
  bool enter = false;           // Enter (однострочное) / Ctrl+Enter
  bool escape = false;
};
// Обработка мыши и клавиатуры для сфокусированного поля; it — взаимодействие с областью поля.
EditResult editProcess(TextEdit& te, const EditParams& p, const Interaction& it, WidgetId id);
// Отрисовка текста, выделения, каретки (с прокруткой и отсечением).
void editDraw(TextEdit& te, const EditParams& p, bool focused, Color textColor);
void editBegin(TextEdit& te, const std::string& value, bool selectAll);

// Этапы кадра (раскладка, всплывающие слои, отрисовка)
void layoutBeginFrame();
void layoutEndFrame();
void overlaysBeginFrame();
void overlaysEndFrame();
void renderAll(gfx::Canvas& canvas);

// Общие кусочки отрисовки
void drawFieldFrame(RectF r, const Interaction& it, bool focused, bool disabled, bool invalid = false, float invalidT = 0);
void drawChevron(RectF r, Color c, float angleDeg);
void drawCheckGlyph(RectF box, Color c, float t);

}  // namespace rg::ui::in
