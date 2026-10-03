// Regnum — собственный immediate-mode интерфейс поверх gfx::Canvas.
//
// Каждый кадр интерфейс строится заново вызовами функций; состояние виджетов (фокус, прокрутка, анимации,
// буфер редактирования) хранится внутри по устойчивому ID. Подробное руководство — docs/UI.md.
//
//   // приложение (один раз)
//   ui::init();
//   ui::setClipboard([] { return platform::clipboardText(); }, [](const std::string& s) { platform::setClipboardText(s); });
//   // события окна
//   void onEvent(const platform::Event& e) { ui::onEvent(e); }
//   // кадр
//   ui::beginFrame(frame.logicalW(), frame.logicalH(), frame.scale, platform::time());
//   {
//     ui::Panel panel("inspector", {16, 16, 360, 640});
//     ui::label(province.name, {.font = ui::Font::Display});
//     ui::tabs("tabs", tab, {{"info", {}, "Обзор"}, {"coins", {}, "Экономика"}});
//     { ui::Row r({ui::fr(1), ui::fr(1)}, 64);
//       ui::stat(fmtShort(pop), "Население", {.icon = "population"});
//       ui::stat(fmtPct(rebel), "Восстание", {.icon = "rebellion", .tone = ui::Tone::Danger}); }
//     if (ui::Section s("Налоги", "treasury"); s) {
//       ui::prop("Ставка", "percent");
//       if (ui::numberField("tax", tax, {.min = 0, .max = 100, .unit = "%"})) app.act(...);
//     }
//     if (ui::button("Сохранить", {.variant = ui::Variant::Primary, .icon = "save"})) save();
//   }
//   ui::endFrame(canvas);                         // canvas — физические пиксели окна
//   platform::setCursor(ui::cursor());
//   animating = ui::needsRedraw();
//
// Единицы: все прямоугольники API — «точки интерфейса» (логические пиксели / uiScale). Высота поля 30,
// значок 18. Цвета — только из темы (ui::theme(), Ink, Tone). Строки — UTF-8, интерфейс по-русски.
// ID: строка-подпись хешируется вместе со стеком областей (pushId/IdScope). «Текст##ключ» — видна часть до «##»,
// ID — вся строка. Одинаковые ID в одной области — ошибка (пишется в журнал один раз).
// Потоки: все функции — только из главного потока (между beginFrame и endFrame, кроме init/onEvent/запросов состояния).
#pragma once
#include <initializer_list>
#include <span>
#include <type_traits>

#include "base/base.h"
#include "gfx/canvas.h"
#include "gfx/text.h"
#include "platform/platform.h"

namespace rg {
struct Flag;   // core/world.h
}

namespace rg::ui {

using WidgetId = u64;
using platform::Key;

// ================================================================ тема
// Токены ARCHITECTURE.md §6. Менять только через setTheme/setUiScale.
struct Theme {
  bool dark = true;
  Color bg, surface1, surface2, surface3, border, borderStrong;
  Color text, textDim, textMuted;
  Color accent, accentHover, onAccent;
  Color success, warning, danger, info;
  // Производные
  Color shadow;        // цвет тени панелей и всплывающих окон
  Color scrim;         // затемнение под модальным окном
  Color hover;         // наложение наведения на прозрачные элементы
  Color pressed;       // наложение нажатия
  Color selection;     // фон выделенного текста
  Color stripe;        // чередование строк таблиц
  Color track;         // дорожки ползунков, полос прогресса, переключателей
  // Размеры
  float radiusField = 7, radiusCard = 10, radiusPanel = 14;
  float controlH = 30, controlHSmall = 24;
  float iconSize = 18, iconSizeSmall = 16;
  float gap = 8;       // промежуток между элементами потока
  float padCard = 12, padPanel = 16;
  float focusRing = 2;
};

const Theme& theme();
void setTheme(bool dark);           // тёмная (по умолчанию) или светлая
Theme darkTheme();
Theme lightTheme();
void setUiScale(float s);           // пользовательский множитель 0,9…1,5 (умножается на DPI окна)
float uiScale();

// Шкала отступов: 4/8/12/16/24/32.
namespace sp {
constexpr float xs = 4, sm = 8, md = 12, lg = 16, xl = 24, xxl = 32;
}

// Роли шрифтов: Caption 11, Small 12, Body 13, Strong 13 полужирный, Subtitle 15 полужирный, Title 18 полужирный,
// Heading 22 с засечками жирный, Display 28 с засечками, Mono 12, Number 20 полужирный (крупные числа).
enum class Font : u8 { Caption, Small, Body, Strong, Subtitle, Title, Heading, Display, Mono, Number };
gfx::TextStyle textStyle(Font f);
float measure(std::string_view text, Font f = Font::Body);   // ширина строки, точки
float lineHeight(Font f);

// Цвет текста из темы.
enum class Ink : u8 { Normal, Dim, Muted, Accent, Success, Warning, Danger, Info, OnAccent };
// Смысловой тон (бейджи, уведомления, полосы, значки).
enum class Tone : u8 { Neutral, Accent, Success, Warning, Danger, Info };
Color inkColor(Ink k);
Color toneColor(Tone t);            // Neutral — textDim

enum class Variant : u8 { Primary, Secondary, Ghost, Danger, Subtle };
enum class Size : u8 { Small, Normal };
enum class Align : u8 { Left, Center, Right };

// Сочетание клавиш. mods — platform::Mod*, ModPrimary — Ctrl (Windows/Linux) или ⌘ (macOS).
constexpr u32 ModPrimary = 0x100;
struct Shortcut {
  Key key = Key::Unknown;
  u32 mods = 0;
  explicit operator bool() const { return key != Key::Unknown; }
};
std::string shortcutText(Shortcut s);   // «Ctrl+S» / «⌘S»

// ================================================================ кадр
void init();                        // шрифты, тема; повторный вызов ничего не делает
void shutdown();                    // освободить состояние (тесты)
void onEvent(const platform::Event& e);  // поставить событие в очередь (разбирается в beginFrame)
// logicalW/H — логические пиксели окна, dpiScale — физических на логический, timeSec — монотонное время.
void beginFrame(float logicalW, float logicalH, float dpiScale, double timeSec);
// Отрисовать записанные слои на холст (физические пиксели окна). Холст не очищается: фон рисует приложение
// (canvas.clear(ui::theme().bg)) или полноэкранная панель.
void endFrame(gfx::Canvas& canvas);

bool wantsMouse();                  // указатель над интерфейсом (панель, всплывающее окно, виджет) или идёт перетаскивание
bool wantsKeyboard();               // фокус в текстовом поле — глобальные сочетания без Ctrl не срабатывают
bool needsRedraw();                 // анимации, таймер подсказки, необработанные события — нужен следующий кадр
int pendingEvents();                // событий в очереди (разносятся по кадрам: щелчок — два кадра)
platform::Cursor cursor();
std::optional<RectF> textInputRect();   // каретка для IME, логические пиксели окна
// Буфер обмена (по умолчанию — внутренний буфер процесса).
void setClipboard(std::function<std::string()> get, std::function<void(const std::string&)> set);

double time();                      // время кадра, с
float dt();                         // длительность прошлого кадра, с
u64 frameIndex();
RectF viewport();                   // весь экран в точках интерфейса
float deviceScale();                // dpiScale × uiScale

// Глобальное сочетание: нажато в этом кадре, не поглощено виджетом и не перекрыто модальным окном —
// true и поглощается. Сочетания без модификаторов не срабатывают, пока фокус в текстовом поле.
bool shortcut(Shortcut s);
bool keyPressed(Key k, u32 mods = 0);   // без поглощения (только для собственных виджетов)
void consumeKey(Key k);

// ================================================================ ID
WidgetId id(std::string_view s);    // в текущей области
WidgetId id(i64 n);
void pushId(std::string_view s);
void pushId(i64 n);
void pushId(const void* p);
inline void pushId(const char* s) { pushId(std::string_view(s)); }   // строка, а не адрес
void popId();
struct IdScope {
  explicit IdScope(std::string_view s) { pushId(s); }
  explicit IdScope(const char* s) { pushId(std::string_view(s)); }
  // Любое целое (int, long, long long, беззнаковые): на Linux i64 — это long, на Windows — long long.
  template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>)
  explicit IdScope(T n) { pushId(i64(n)); }
  explicit IdScope(const void* p) { pushId(p); }
  ~IdScope() { popId(); }
  IdScope(const IdScope&) = delete;
  IdScope& operator=(const IdScope&) = delete;
};
std::string_view displayText(std::string_view label);   // часть до «##»

// ================================================================ последний элемент
struct Item {
  WidgetId id = 0;
  RectF rect;
  bool hovered = false;
  bool active = false;         // зажат / перетаскивается / редактируется
  bool focused = false;
  bool clicked = false;
  bool rightClicked = false;
  bool doubleClicked = false;
  bool changed = false;        // значение изменилось в этом кадре
  bool deactivated = false;    // взаимодействие закончилось в этом кадре (отпущен, фиксация текста)
};
const Item& lastItem();
// Фокус клавиатуры (с видимым кольцом): ui::setKeyboardFocus(ui::id("search")) — например, по Ctrl+F.
void setKeyboardFocus(WidgetId id);
WidgetId keyboardFocus();

// Взаимодействие для собственных виджетов (карта, дерево технологий, графики).
enum InteractFlags : u32 {
  IfNone = 0,
  IfFocusable = 1,        // участвует в Tab; Enter/Пробел — щелчок
  IfAllowOverlap = 2,     // поверх может лежать другой элемент (фон строки, холст под кнопками)
  IfRightButton = 4,      // активируется и правой кнопкой
  IfMiddleButton = 8,     // активируется и средней кнопкой (панорама)
};
struct Interaction {
  bool hovered = false, pressed = false, held = false, released = false;
  bool clicked = false, rightClicked = false, middleClicked = false, doubleClicked = false;
  bool dragging = false;       // зажат и сдвинут дальше порога (3 точки)
  int button = 0;              // кнопка, которой элемент активирован
  float dx = 0, dy = 0;        // смещение от точки нажатия
  float mx = 0, my = 0;        // указатель (точки интерфейса)
  bool focused = false;
  bool keyActivated = false;   // Enter/Пробел при фокусе
};
Interaction interact(WidgetId id, RectF r, u32 flags = IfNone);
struct Mouse {
  float x = 0, y = 0;
  bool down[3]{};              // зажаты левая, правая, средняя
  float wheelX = 0, wheelY = 0;  // за кадр, точки (не поглощено прокруткой)
  u32 mods = 0;
};
const Mouse& mouse();
void setCursor(platform::Cursor c);   // для собственных виджетов (на этот кадр)
void requestRedraw();                  // ещё один кадр (собственные анимации)

// ================================================================ состояние и анимации
namespace detail {
struct StateBox { virtual ~StateBox() = default; };
template <class T> struct StateOf final : StateBox { T value{}; };
StateBox* stateLookup(WidgetId id, const void* tag, StateBox* (*make)());
template <class T> struct Tag { static constexpr char value = 0; };
}  // namespace detail

// Устойчивое состояние по ID; удаляется, если не запрашивалось ~5 секунд и 300 кадров.
template <class T> T& state(WidgetId id) {
  auto* box = detail::stateLookup(id, &detail::Tag<T>::value, []() -> detail::StateBox* { return new detail::StateOf<T>(); });
  return static_cast<detail::StateOf<T>*>(box)->value;
}
// Плавное значение к target за duration секунд (ease-out). Первое обращение — сразу target.
float animate(WidgetId id, float target, float duration = 0.12f);
Color animateColor(WidgetId id, Color target, float duration = 0.12f);

// ================================================================ раскладка
// Поток контейнера — вертикальный: каждый виджет занимает следующий слот (ширина — по содержимому или вся,
// высота — своя), промежуток theme().gap. Строка Row делит ширину на столбцы; HStack кладёт элементы слева
// направо по их естественной ширине.
struct Len {
  float px = 0, fr = 0, min = 0;
};
constexpr Len px(float v) { return Len{v, 0, 0}; }
constexpr Len fr(float v = 1, float min = 0) { return Len{0, v, min}; }

RectF avail();                        // остаток текущего контейнера (x, y — курсор)
RectF next(float h);                  // занять слот высотой h на всю ширину (ячейку строки)
RectF next(float w, float h);         // слот заданной ширины
void at(RectF r);                     // следующий виджет — ровно в r (вне потока)
void spacer(float h = sp::sm);
void gap(float g);                    // промежуток между элементами текущего контейнера

// Контейнер в явном прямоугольнике (без фона): вертикальный поток внутри с отступом pad.
class Area {
 public:
  explicit Area(RectF r, float pad = 0);
  ~Area();
  Area(const Area&) = delete;
  Area& operator=(const Area&) = delete;
};

// Плавающая панель: фон surface1, радиус 14, мягкая тень, рамка. Перекрывает ввод того, что под ней.
struct PanelOpt {
  float pad = 16;
  bool shadow = true;
  bool border = true;
  float radius = -1;          // < 0 — theme().radiusPanel
  bool glass = false;         // размытый фон под панелью (поверх карты)
};
class Panel {
 public:
  Panel(std::string_view id, RectF r, const PanelOpt& o = {});
  ~Panel();
  Panel(const Panel&) = delete;
  Panel& operator=(const Panel&) = delete;
};

// Карточка в потоке: фон surface2, радиус 10, высота по содержимому. После закрытия — lastItem() (подсказка, цель переноса).
struct CardOpt {
  float pad = 12;
  const char* icon = nullptr;     // заголовок карточки (значок + текст)
  std::string_view title;
  Tone tone = Tone::Neutral;      // цвет полоски слева (не Neutral) — акцентная карточка
  bool hoverable = false;
};
class Card {
 public:
  explicit Card(const CardOpt& o = {});
  ~Card();
  Card(const Card&) = delete;
  Card& operator=(const Card&) = delete;
};

// Сворачиваемый раздел: заголовок (шеврон, значок, текст, бейдж), плавное раскрытие.
struct SectionOpt {
  bool defaultOpen = true;
  std::string_view badge;         // текст бейджа справа в заголовке (например, количество)
  bool card = true;               // в карточке; false — плоский раздел с линией
  const char* actionIcon = nullptr;   // кнопка-значок в заголовке справа («plus» — добавить строку)
  std::string_view actionTooltip;     // подсказка к ней (обязательна при actionIcon)
};
class Section {
 public:
  Section(std::string_view title, const char* icon = nullptr, const SectionOpt& o = {});
  ~Section();
  explicit operator bool() const { return open_; }
  bool action() const { return action_; }   // нажата кнопка заголовка
  Section(const Section&) = delete;
  Section& operator=(const Section&) = delete;

 private:
  bool open_ = false;
  bool began_ = false;
  bool action_ = false;
};

// Прокручиваемая область. height = 0 — весь остаток контейнера (в контейнере без высоты — 240). Колесо, плавная прокрутка, тонкая полоса,
// перетаскивание полосы, щелчок по дорожке — на страницу.
struct ScrollOpt {
  bool horizontal = false;        // прокрутка и по горизонтали (ширина содержимого — по элементам)
  float pad = 0;
  float contentWidth = 0;         // ширина содержимого при horizontal (0 — по элементам)
};
class Scroll {
 public:
  Scroll(std::string_view id, float height = 0, const ScrollOpt& o = {});
  ~Scroll();
  float offset() const;           // текущая прокрутка по вертикали
  void scrollTo(float y, bool smooth = true);
  Scroll(const Scroll&) = delete;
  Scroll& operator=(const Scroll&) = delete;

 private:
  WidgetId id_;
};
void scrollToItem();                  // прокрутить ближайшую область так, чтобы последний элемент был виден

// Строка со столбцами: элементы занимают ячейки по порядку; после последней ячейки начинается новая строка
// тех же столбцов (сетка). height: высота строки (0 — theme().controlH, kAuto — по самому высокому элементу).
constexpr float kAuto = -1;
class Row {
 public:
  Row(std::initializer_list<Len> cols, float height = 0, float gap = sp::sm);
  Row(std::span<const Len> cols, float height = 0, float gap = sp::sm);
  ~Row();
  Row(const Row&) = delete;
  Row& operator=(const Row&) = delete;
};

// Горизонтальный ряд по естественной ширине элементов. Align::Right/Center — выравнивание всего ряда.
class HStack {
 public:
  explicit HStack(float height = 0, Align align = Align::Left, float gap = sp::sm);
  ~HStack();
  HStack(const HStack&) = delete;
  HStack& operator=(const HStack&) = delete;
};
void flex();                          // в HStack: растяжимый промежуток (элементы после него — к правому краю)

// Вертикальная группа в одном слоте (ячейке строки, элементе HStack); высота — по содержимому.
class Group {
 public:
  explicit Group(float width = 0, float gap = -1);
  ~Group();
  Group(const Group&) = delete;
  Group& operator=(const Group&) = delete;
};

class Indent {
 public:
  explicit Indent(float amount = sp::lg);
  ~Indent();
  Indent(const Indent&) = delete;
  Indent& operator=(const Indent&) = delete;

 private:
  float a_;
};

// Все виджеты внутри — недоступны (без взаимодействия, приглушены).
class Disabled {
 public:
  explicit Disabled(bool on = true);
  ~Disabled();
  Disabled(const Disabled&) = delete;
  Disabled& operator=(const Disabled&) = delete;

 private:
  bool on_;
};

// Разделитель двух областей: pos — положение границы от начала area (точки), перетаскивается.
enum class Axis : u8 { Horizontal, Vertical };   // Horizontal — области слева и справа
struct Split {
  RectF a, b;
  bool changed = false;
};
Split splitter(std::string_view id, RectF area, float& pos, Axis axis = Axis::Horizontal, float minA = 80, float minB = 80);

// Виртуальный список: только видимые строки. for (int i : list) { ... ui::next(...) ... }
class VirtualList {
 public:
  VirtualList(std::string_view id, int count, float rowHeight, float height = 0);
  ~VirtualList();
  struct It {
    VirtualList* l;
    int i;
    int operator*() const { return i; }
    It& operator++();
    bool operator!=(const It& o) const { return i != o.i; }
  };
  It begin();
  It end();
  RectF rowRect(int i) const;
  void scrollToRow(int i);
  int first() const { return first_; }
  int last() const { return last_; }
  VirtualList(const VirtualList&) = delete;
  VirtualList& operator=(const VirtualList&) = delete;

 private:
  void enterRow(int i);
  Scroll scroll_;
  int count_ = 0, first_ = 0, last_ = 0;
  float rowH_ = 0, top_ = 0, x_ = 0, w_ = 0;
  bool rowOpen_ = false;
};

// ================================================================ текст
struct LabelOpt {
  Font font = Font::Body;
  Ink ink = Ink::Normal;
  Color color{0, 0, 0, 0};        // a > 0 — вместо ink (цвет сущности)
  Align align = Align::Left;
  const char* icon = nullptr;     // значок слева
  bool wrap = false;              // перенос по словам (иначе одна строка с многоточием)
  int maxLines = 0;
  std::string_view tooltip;
};
void label(std::string_view text, const LabelOpt& o = {});
void text(std::string_view text, Font f = Font::Body, Ink ink = Ink::Normal);   // абзац с переносом
void caption(std::string_view text);  // мелкий заголовок группы (прописные, приглушённый)
// Строка свойства: подпись слева (доля ширины), следующий виджет — справа в той же строке.
void prop(std::string_view label, const char* icon = nullptr, float labelFraction = 0.42f);
bool link(std::string_view text, const char* icon = nullptr);
void kbd(std::string_view keys);      // «Ctrl+Shift+S» — клавиши-колпачки
void kbd(Shortcut s);
void separator();
void separatorV();                    // в HStack
void icon(const char* name, Ink ink = Ink::Dim, float size = 0, std::string_view tooltip = {});
void iconColored(const char* name, Color c, float size = 0, std::string_view tooltip = {});
void image(const gfx::Image& img, float w, float h, float radius = 0);   // изображение должно жить до endFrame

// ================================================================ кнопки и переключатели
struct ButtonOpt {
  Variant variant = Variant::Secondary;
  const char* icon = nullptr;
  Size size = Size::Normal;
  bool fill = false;              // на всю ширину слота
  bool disabled = false;
  bool isDefault = false;         // Enter в модальном окне (кнопка подтверждения)
  const char* iconRight = nullptr;
  std::string_view tooltip;
  Shortcut shortcut;              // срабатывает и по сочетанию (пока кнопка видна); показывается в подсказке
};
bool button(std::string_view label, const ButtonOpt& o = {});

struct IconButtonOpt {
  Variant variant = Variant::Ghost;
  Size size = Size::Normal;
  bool toggled = false;           // включённое состояние (инструмент выбран)
  bool badge = false;             // точка-бейдж в углу
  Color badgeColor{0, 0, 0, 0};   // a = 0 — danger
  bool disabled = false;
  Shortcut shortcut;
  Tone tone = Tone::Neutral;      // цвет значка
};
// tooltip обязателен: название действия (сочетание добавляется само).
bool iconButton(const char* icon, std::string_view tooltip, const IconButtonOpt& o = {});
bool iconToggle(const char* icon, std::string_view tooltip, bool& on, const IconButtonOpt& o = {});

struct Segment {
  const char* icon = nullptr;
  std::string_view label;
  std::string_view tooltip;
};
struct SegmentedOpt {
  Size size = Size::Normal;
  bool fill = true;               // на всю ширину; false — по содержимому
  bool disabled = false;
};
bool segmented(std::string_view id, int& index, std::initializer_list<Segment> items, const SegmentedOpt& o = {});
bool segmented(std::string_view id, int& index, std::span<const Segment> items, const SegmentedOpt& o = {});

bool toggle(std::string_view label, bool& on, bool disabled = false);         // переключатель; подпись слева
bool checkbox(std::string_view label, bool& on, bool disabled = false);
enum class Check : u8 { Off, On, Mixed };
bool checkbox(std::string_view label, Check& state, bool disabled = false);   // Mixed → On при щелчке
bool radio(std::string_view label, int& value, int option, bool disabled = false);

// ================================================================ числа
struct SliderOpt {
  double step = 0;                // 0 — непрерывно
  int digits = 0;
  const char* unit = nullptr;     // «%», «ходов»
  bool bubble = true;             // пузырь со значением при перетаскивании
  bool showValue = true;          // значение справа
  bool disabled = false;
  Tone tone = Tone::Accent;
};
bool slider(std::string_view id, double& v, double min, double max, const SliderOpt& o = {});
template <class T>
  requires std::is_arithmetic_v<T>
bool slider(std::string_view id, T& v, double min, double max, SliderOpt o = {}) {
  if constexpr (std::is_integral_v<T>) { if (o.step <= 0) o.step = 1; }
  double d = double(v);
  bool r = slider(id, d, min, max, o);
  if (r) v = std::is_integral_v<T> ? T(std::llround(d)) : T(d);
  return r;
}

struct NumberOpt {
  double min = -1e300, max = 1e300;
  double step = 1;                // шаг стрелок, колеса, кнопок; перетаскивание — step за 4 точки
  int digits = 0;                 // знаков после запятой
  const char* unit = nullptr;     // суффикс: «%», «ходов» (для «ходов» склонение не делается)
  const char* icon = nullptr;     // значок-подпись слева (за него можно тянуть)
  std::string_view label;         // короткая подпись слева (тянется)
  bool steppers = false;          // кнопки − и +
  bool sign = false;              // показывать «+» у положительных
  bool disabled = false;
  bool readOnly = false;
  std::string_view tooltip;
};
// Возвращает true, когда значение изменено: при перетаскивании и кнопках — сразу (lastItem().active — ещё тянут),
// при вводе — по Enter или уходу фокуса. Неверный ввод — встряхивание и возврат прежнего значения.
bool numberField(std::string_view id, double& v, const NumberOpt& o = {});
template <class T>
  requires std::is_arithmetic_v<T>
bool numberField(std::string_view id, T& v, NumberOpt o = {}) {
  if constexpr (std::is_integral_v<T>) {
    o.digits = 0;
    if (o.step < 1) o.step = 1;
    o.min = std::max(o.min, double(std::numeric_limits<T>::lowest()));
    o.max = std::min(o.max, double(std::numeric_limits<T>::max()));
  }
  double d = double(v);
  bool r = numberField(id, d, o);
  if (r) v = std::is_integral_v<T> ? T(std::llround(d)) : T(d);
  return r;
}

// ================================================================ текстовые поля
struct TextOpt {
  std::string_view placeholder;
  const char* icon = nullptr;     // значок слева (search, edit…)
  bool clearButton = false;       // крестик очистки при непустом тексте
  bool live = false;              // value меняется при каждом изменении; иначе — по Enter/уходу фокуса
  int maxLength = 0;              // в символах; 0 — без ограничения
  bool readOnly = false;
  bool disabled = false;
  bool autofocus = false;         // получить фокус при первом появлении
  bool selectAllOnFocus = false;
  bool (*filter)(u32 cp) = nullptr;   // разрешённые символы
  std::string_view tooltip;
};
// true — value изменено (live: каждое изменение; иначе — фиксация). Esc — отмена правки и уход фокуса.
bool textField(std::string_view id, std::string& value, const TextOpt& o = {});
// Многострочное: перенос по словам, прокрутка; Enter — новая строка, Ctrl+Enter — фиксация.
bool textArea(std::string_view id, std::string& value, float height, const TextOpt& o = {});
bool searchField(std::string_view id, std::string& query, std::string_view placeholder = "Поиск");

// ================================================================ выбор
struct Option {
  std::string_view label;
  const char* icon = nullptr;
  Color color{0, 0, 0, 0};        // a > 0 — цветная точка (фракция, ресурс)
  std::string_view hint;          // приглушённый текст справа
  bool disabled = false;
};
struct ComboOpt {
  std::string_view placeholder = "—";
  std::string_view noneLabel;     // непусто — пункт «нет» (индекс −1) первым
  int search = -1;                // −1 — поиск при > 8 пунктах; 0 — нет; 1 — всегда
  const char* icon = nullptr;     // значок слева в поле (если у пункта нет своего)
  bool disabled = false;
  float popupWidth = 0;           // 0 — ширина поля
  int maxVisible = 10;
  std::string_view tooltip;
};
bool combo(std::string_view id, int& index, std::span<const Option> options, const ComboOpt& o = {});
bool combo(std::string_view id, int& index, std::initializer_list<Option> options, const ComboOpt& o = {});
// Для длинных и вычисляемых списков (тысячи провинций): get(i) возвращает пункт (строки живут до конца кадра).
bool combo(std::string_view id, int& index, int count, const std::function<Option(int)>& get, const ComboOpt& o = {});

struct MultiOpt {
  std::string_view placeholder = "—";
  std::string_view addTooltip = "Добавить";
  bool disabled = false;
};
// Выбранные пункты — фишки с крестиком, «+» открывает список с флажками и поиском. Возвращает true при изменении.
bool multiSelect(std::string_view id, std::vector<int>& selected, std::span<const Option> options, const MultiOpt& o = {});

struct ColorOpt {
  bool alpha = false;
  std::span<const Color> palette;   // пусто — геральдическая палитра
  std::string_view tooltip;
  Size size = Size::Normal;
  bool hex = true;                  // показывать hex рядом с образцом
};
// Образец цвета; щелчок открывает выбор (палитра + HSV + оттенок + hex). true — цвет изменён (живое изменение).
bool colorButton(std::string_view id, Color& c, const ColorOpt& o = {});
bool colorPicker(std::string_view id, Color& c, const ColorOpt& o = {});   // встроенный выбор без всплывающего окна
std::span<const Color> heraldicPalette();

// ================================================================ вкладки
struct Tab {
  const char* icon = nullptr;
  std::string_view label;           // пусто — только значок
  std::string_view tooltip;         // обязателен для вкладок без подписи
  int badge = 0;                    // > 0 — число в бейдже, −1 — точка
  Tone badgeTone = Tone::Accent;
};
enum class TabStyle : u8 { Underline, Pill };
struct TabsOpt {
  TabStyle style = TabStyle::Underline;
  bool fill = false;                // равные доли ширины
  Size size = Size::Normal;
};
bool tabs(std::string_view id, int& active, std::initializer_list<Tab> items, const TabsOpt& o = {});
bool tabs(std::string_view id, int& active, std::span<const Tab> items, const TabsOpt& o = {});

// ================================================================ таблица
struct Column {
  std::string_view title;
  const char* icon = nullptr;       // значок в шапке (вместо или вместе с текстом)
  Len width = fr(1);
  Align align = Align::Left;
  bool sortable = false;
  std::string_view tooltip;
};
struct TableOpt {
  float rowHeight = 34;
  float height = 0;                 // 0 — по содержимому (виртуализация по видимой части родителя); > 0 — своя прокрутка
  bool header = true;
  bool striped = true;
  bool selectable = true;           // щелчок выделяет строку
  int* selected = nullptr;          // внешнее хранение выделенной строки (индекс данных); nullptr — внутри
  const char* emptyIcon = "list";
  std::string_view emptyText = "Нет записей";
  bool compact = false;
};
// for (int i : table) — только видимые строки в порядке показа (с учётом сортировки); i — индекс данных.
class Table {
 public:
  Table(std::string_view id, std::span<const Column> cols, int rows, const TableOpt& o = {});
  Table(std::string_view id, std::initializer_list<Column> cols, int rows, const TableOpt& o = {});
  ~Table();
  // Сортировка по шапке: столбец (−1 — нет) и направление. sort(cmp) упорядочивает строки: cmp(a, b, col) < 0 — a выше.
  int sortColumn() const;
  bool sortDescending() const;
  void sort(const std::function<int(int a, int b, int col)>& cmp);

  struct It {
    Table* t;
    int k;
    int operator*() const;
    It& operator++();
    bool operator!=(const It& o) const { return k != o.k; }
  };
  It begin();
  It end();

  RectF cell();                     // следующая ячейка текущей строки; следующий виджет займёт её
  void text(std::string_view s, Ink ink = Ink::Normal, Font f = Font::Body);   // текст в ячейке по выравниванию столбца
  bool footer();                    // итоговая строка (ячейки — cell()); true всегда, если строк > 0
  RectF rowRect() const;            // прямоугольник текущей строки

  int selected() const;             // индекс данных или −1
  void select(int row);
  int clicked() const;              // строка, по которой щёлкнули в этом кадре (−1)
  int doubleClicked() const;
  int rightClicked() const;
  int hovered() const;
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  struct Impl;   // внутреннее устройство (ui/table.cpp)

 private:
  std::unique_ptr<Impl> d_;
};

// ================================================================ дерево
struct TreeOpt {
  const char* icon = nullptr;
  Color dot{0, 0, 0, 0};
  bool leaf = false;
  bool selected = false;
  bool defaultOpen = false;
  std::string_view badge;
};
class TreeNode {
 public:
  TreeNode(std::string_view label, const TreeOpt& o = {});
  ~TreeNode();
  explicit operator bool() const { return open_; }
  bool clicked() const { return clicked_; }
  bool doubleClicked() const { return dbl_; }
  TreeNode(const TreeNode&) = delete;
  TreeNode& operator=(const TreeNode&) = delete;

 private:
  bool open_ = false, clicked_ = false, dbl_ = false;
  float gx_ = 0, gy_ = 0;
};

// ================================================================ отображение
void badge(std::string_view text, Tone tone = Tone::Accent, bool solid = false);
void tag(std::string_view text, Tone tone = Tone::Neutral, const char* icon = nullptr);
enum class ChipAction : u8 { None, Click, Remove };
struct ChipOpt {
  const char* icon = nullptr;
  Color color{0, 0, 0, 0};          // цветная точка (фракция)
  Tone tone = Tone::Neutral;
  bool removable = false;
  bool selected = false;
  bool clickable = false;
  std::string_view tooltip;
};
ChipAction chip(std::string_view label, const ChipOpt& o = {});

struct ProgressOpt {
  Tone tone = Tone::Accent;
  Color color{0, 0, 0, 0};
  float height = 6;
  bool label = false;               // процент справа
  std::string_view text;            // свой текст справа
  float width = 0;                  // 0 — вся ширина слота
};
void progress(double t, const ProgressOpt& o = {});
struct MeterOpt {
  double min = -100, max = 100;
  float height = 8;
  bool label = true;
  float width = 0;
};
void meter(double value, const MeterOpt& o = {});   // двуполярный индикатор с отметкой нуля

struct Slice {
  double value = 0;
  Color color;
  std::string_view label;
};
struct PieOpt {
  float size = 96;
  float thickness = 0;              // 0 — круговая; > 0 — кольцо этой толщины
  std::string_view centerValue, centerLabel;
  bool legend = false;              // легенда справа
  bool tooltips = true;
};
int pie(std::span<const Slice> slices, const PieOpt& o = {});   // наведённый сектор или −1
int pie(std::initializer_list<Slice> slices, const PieOpt& o = {});

struct SparkOpt {
  float height = 28;
  float width = 0;
  Color color{0, 0, 0, 0};          // a = 0 — accent
  bool fill = true;
  bool dot = true;
};
void sparkline(std::span<const float> values, const SparkOpt& o = {});

struct StatOpt {
  const char* icon = nullptr;
  Tone tone = Tone::Accent;         // цвет значка
  double delta = 0;                 // изменение (стрелка и цвет); 0 и пустой deltaText — не показывать
  std::string_view deltaText;       // своя подпись изменения («+12 %»)
  bool invertDelta = false;         // рост — плохо (восстание)
  std::string_view tooltip;
  std::span<const float> spark;     // мини-график под числом
};
void stat(std::string_view value, std::string_view label, const StatOpt& o = {});

struct AvatarOpt {
  const gfx::Image* image = nullptr;
  Color color{0, 0, 0, 0};          // фон инициалов; a = 0 — по хешу имени
  float size = 32;
  bool ring = false;                // золотое кольцо (правитель)
  std::string_view tooltip;
};
void avatar(std::string_view name, const AvatarOpt& o = {});

// Флаг государства (gfx::drawFlag: узор, эмблема, светотень, рамка). flag должен жить до endFrame — обычно это
// запись мира (const World&). Высота по умолчанию — 2/3 ширины.
void flag(const Flag& f, float w, float h = 0, float radius = 4, std::string_view tooltip = {});

// Пустое состояние: значок, одна строка, действие. true — нажата кнопка действия.
bool emptyState(const char* icon, std::string_view text, std::string_view action = {}, const char* actionIcon = nullptr);

// Строка списка (левые панели: провинции, государства, персонажи). true — щелчок (выбор).
// lastItem() — для контекстного меню (beginContextMenu) и перетаскивания (dragSource).
struct ListItemOpt {
  const char* icon = nullptr;
  Color dot{0, 0, 0, 0};          // цветная точка (фракция) вместо значка
  std::string_view subtitle;      // вторая строка, мелко и приглушённо (высота строки 44 вместо 32)
  std::string_view hint;          // справа: число, статус
  std::string_view badge;
  bool selected = false;
  bool disabled = false;
  std::string_view tooltip;
};
bool listItem(std::string_view label, const ListItemOpt& o = {});
// Индикатор ожидания (вращающаяся дуга): загрузка, сохранение, расчёт хода.
void spinner(float size = 18, Tone tone = Tone::Accent);

// ================================================================ подсказки и всплывающие окна
// Подсказка к последнему элементу (появляется через 350 мс наведения).
void tooltip(std::string_view text, Shortcut s = {});
// Своё содержимое подсказки: if (ui::beginTooltip()) { ...; ui::endTooltip(); }
bool beginTooltip(float width = 260);
void endTooltip();

enum class Side : u8 { Below, Above, Right, Left };
void openPopup(std::string_view id);  // якорь — последний элемент
void closePopup();                    // закрыть текущее (изнутри) всплывающее окно
bool isPopupOpen(std::string_view id);
struct PopupOpt {
  Side side = Side::Below;
  float width = 0;                    // 0 — по содержимому
  float maxHeight = 0;                // 0 — до края экрана; при переполнении — прокрутка
  float pad = 8;
};
bool beginPopup(std::string_view id, const PopupOpt& o = {});
void endPopup();

// Меню: контекстное (правый щелчок по последнему элементу) или по openPopup.
bool beginContextMenu(std::string_view id);
void openContextMenu(std::string_view id);     // открыть меню у указателя (например, по Table::rightClicked())
bool beginMenu(std::string_view id);
void endMenu();
struct MenuItemOpt {
  const char* icon = nullptr;
  Shortcut shortcut;                  // только показывается
  bool checked = false;
  bool danger = false;
  bool disabled = false;
};
bool menuItem(std::string_view label, const MenuItemOpt& o = {});
void menuSeparator();
void menuHeader(std::string_view text);
bool beginSubmenu(std::string_view label, const char* icon = nullptr);
void endSubmenu();

// Модальное окно: затемнение и размытие фона, заголовок со значком и крестиком, ловушка фокуса,
// Esc — закрыть, Enter — кнопка isDefault. open == nullptr — управление через openModal/closeModal.
struct ModalOpt {
  std::string_view title;
  const char* icon = nullptr;
  Tone tone = Tone::Accent;           // цвет значка заголовка
  float width = 440;
  bool closeButton = true;
  bool dismissOnBackdrop = false;
};
void openModal(std::string_view id);
bool beginModal(std::string_view id, const ModalOpt& o = {}, bool* open = nullptr);
void endModal();
void closeModal();                    // изнутри: закрыть текущее модальное окно
bool isModalOpen(std::string_view id);
bool anyModalOpen();
// Подвал модального окна: линия-разделитель и ряд кнопок, прижатый вправо (порядок — как вызваны, слева направо).
class ModalFooter {
 public:
  ModalFooter();
  ~ModalFooter();
  ModalFooter(const ModalFooter&) = delete;
  ModalFooter& operator=(const ModalFooter&) = delete;
};

// Уведомления: стопка справа внизу, значок и текст, автоскрытие (наведение — пауза).
void toast(std::string text, Tone tone = Tone::Info, const char* icon = nullptr, double seconds = 4);
int toastCount();

// ================================================================ перетаскивание
// Источник — последний элемент: при перетаскивании дальше порога начинается перенос payload типа type.
// Возвращает true, пока перенос из этого элемента идёт.
bool dragSource(std::string_view type, u64 payload, std::string_view label = {}, const char* icon = nullptr);
// Цель — последний элемент: подсветка при наведении совместимого переноса; при отпускании — payload.
std::optional<u64> dropTarget(std::string_view type);
bool dragging(std::string_view type = {});

// ================================================================ свободное рисование
// Холст для карты, дерева технологий, графиков: fn вызывается при отрисовке слоя с отсечением deviceRect,
// единичным преобразованием (физические пиксели), scale — физических пикселей на точку интерфейса.
using CustomDraw = std::function<void(gfx::Canvas& c, RectF deviceRect, float scale)>;
void custom(RectF r, CustomDraw fn);

// Примитивы в точках интерфейса (записываются в текущий слой с текущим отсечением).
namespace draw {
void rect(RectF r, Color c, float radius = 0);
void rectStroke(RectF r, Color c, float radius = 0, float width = 1);   // обводка внутри r
void gradient(RectF r, Color top, Color bottom, float radius = 0, bool horizontal = false);
void shadow(RectF r, float radius, float blur, Color c, float dy = 0, float spread = 0);
void circle(float cx, float cy, float r, Color c);
void ring(float cx, float cy, float r, float width, Color c);
void line(float x0, float y0, float x1, float y1, Color c, float width = 1);
void path(const gfx::Path& p, Color c);
void pathStroke(const gfx::Path& p, Color c, float width, gfx::Cap cap = gfx::Cap::Round);
void icon(std::string_view name, RectF r, Color c);
void text(std::string_view s, RectF r, Font f, Color c, Align a = Align::Left, bool ellipsis = true);
void image(const gfx::Image& img, RectF r, float radius = 0, float opacity = 1);
void pushClip(RectF r);
void popClip();
}  // namespace draw

}  // namespace rg::ui
