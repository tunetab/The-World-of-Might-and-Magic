// Regnum — инструменты правки карты: границы провинций (ТЗ 1.a.i, 1.a.ii, 1.a.iv) и торговые маршруты (ТЗ 1.d.v).
// Общие помощники: наложения поверх карты (линии со свечением, ручки, кольца прилипания), прилипание к узлам
// и границам, ввод контура (щелчки и рисование от руки), плавающая панель параметров инструмента.
// Только для src/app/tools_*.cpp, src/app/panels/route*.cpp и их тестов.
#pragma once
#include "app/app_internal.h"
#include "app/widgets.h"
#include "geo/geom.h"
#include "geo/ops.h"
#include "gfx/canvas.h"

namespace rg::app::tools {

// ---------------------------------------------------------------- размеры (логические пиксели окна)
constexpr float kHitPx = 9;        // захват ручки и границы
constexpr float kSnapPx = 9;       // прилипание вершины к узлу или границе
constexpr float kClosePx = 11;     // щелчок у первой вершины замыкает контур
constexpr float kLassoPx = 6;      // сдвиг нажатия, после которого начинается рисование от руки
constexpr float kLassoStepPx = 4;  // шаг точек рисования от руки
constexpr float kDragPx = 3;       // порог перетаскивания ручки

// Выбранная провинция (0 — выделено не провинция).
Id selectedProvince(const App& a);
// Провинция под указателем.
Id provinceUnder(App& a, const PointerEvent& e);
// Название для подписей: «Без названия», если пусто.
std::string provinceTitle(const World& w, Id province);
// Уникальное название новой провинции: «Новая провинция», «Новая провинция 2»…
std::string newProvinceName(const World& w);

// ---------------------------------------------------------------- цвета наложений (из темы)
struct Palette {
  Color accent, danger, success, info;
  Color light;   // светлая сердцевина линий и ручек
  Color ink;     // тёмная обводка для контраста на любой заливке карты
};
Palette palette();

// ---------------------------------------------------------------- рисование (холст — логические пиксели окна)
using Pts = std::vector<gfx::Pt>;
Pts toScreen(const map::View& v, const std::vector<Vec2>& pts);
gfx::Path polyPath(const Pts& p, bool closed);
void strokeLine(gfx::Canvas& c, const Pts& p, bool closed, Color col, float width, std::vector<float> dash = {});
// Линия «как на карте»: мягкое свечение, тёмная подложка, цветная сердцевина.
void glowLine(gfx::Canvas& c, const Pts& p, bool closed, Color col, float width, bool dashed = false);
void fillPoly(gfx::Canvas& c, const Pts& p, Color col);
// Ручки: точка (круг), узел (квадрат), стык с берегом (ромб). r — полуразмер.
enum class Mark : u8 { Dot, Square, Diamond };
void handleMark(gfx::Canvas& c, gfx::Pt p, Mark m, float r, Color fill, Color ring, float glow = 0);
// Место новой точки на линии (двойной щелчок): золотой кружок с плюсом.
void insertGhost(gfx::Canvas& c, gfx::Pt p);
// Кольцо прилипания (edge — к границе, иначе к вершине).
void snapRing(gfx::Canvas& c, gfx::Pt p, Color col, bool edge);
// Значок рядом с указателем (подсказка режима: «+», «−», нож, корзина).
void cursorBadge(gfx::Canvas& c, gfx::Pt cursor, const char* icon, Color col);
// Контур провинции (все грани, с дырами) / грани — в экранных координатах.
gfx::Path provincePath(const World& w, const map::View& v, Id province);
gfx::Path facePath(const geo::Face& f, const map::View& v);
// Подсветить провинцию: заливка и обводка (dashed — пунктир).
void highlightProvince(gfx::Canvas& c, const World& w, const map::View& v, Id province, Color fill, Color stroke, float width,
                       bool dashed = false);

// ---------------------------------------------------------------- прилипание
struct Snap {
  enum Kind : u8 { None, Vertex, Edge } kind = None;
  Vec2 p;
};
// Ближайший узел/точка границы, иначе ближайшая граница в радиусе tol (единицы карты).
Snap snapAt(const World& w, Vec2 p, double tol);

// ---------------------------------------------------------------- ввод контура и линии
// Щелчки — вершины (с прилипанием), нажатие с протяжкой — рисование от руки, двойной щелчок или Enter — готово,
// Backspace или правая кнопка — убрать вершину, Esc — отмена. Щелчок у первой вершины замыкает многоугольник.
class Sketch {
 public:
  enum class Result : u8 { Ignored, Consumed, Changed, Finish, Cancel };
  bool closed = true;        // многоугольник (иначе ломаная)
  bool freehand = true;      // рисование от руки протяжкой
  bool snapping = true;      // прилипание к узлам и границам

  int minPts() const { return closed ? 3 : 2; }
  bool active() const { return !pts_.empty(); }
  bool ready() const { return int(pts_.size()) >= minPts(); }
  int count() const { return int(pts_.size()); }
  const std::vector<Vec2>& points() const { return pts_; }
  bool drawingFreehand() const { return lasso_; }
  bool simple() const;                     // без самопересечений (для предпросмотра)
  // Итоговые вершины: без повторов, ломаные от руки упрощены до долей пикселя экрана.
  std::vector<Vec2> result(const map::View& v) const;

  Result down(App& a, const PointerEvent& e);
  Result move(App& a, const PointerEvent& e, bool held);
  Result up(App& a, const PointerEvent& e);
  Result key(App& a, const platform::Event& e);
  void clear();
  void pop();

  // Предпросмотр: линия, заливка (для многоугольника), вершины, резиновая нить к указателю, прилипание.
  void draw(App& a, gfx::Canvas& c, const map::View& v, Color line, Color fill) const;
  std::optional<Vec2> cursor(const App& a) const;   // точка под указателем (если он над картой)

 private:
  Vec2 place(App& a, const PointerEvent& e);
  std::vector<Vec2> pts_;
  std::vector<bool> free_;                 // вершина нарисована от руки
  Vec2 cur_;
  float csx_ = 0, csy_ = 0;
  bool hasCur_ = false;
  Snap snap_;
  bool pressed_ = false, lasso_ = false, lassoFromStart_ = false;
  float dsx_ = 0, dsy_ = 0;
  size_t downCount_ = 0;
  mutable size_t simpleN_ = size_t(-1);
  mutable bool simple_ = true;
};

// ---------------------------------------------------------------- плавающая панель параметров
// Стеклянная панель вверху по центру видимой части карты (под баннером прошлого хода). Рисуется из drawOverlay:
// ui:: внутри — ряд HStack высотой 30. width — точки интерфейса. Прямоугольник помечается «tool.options».
class OptionsBar {
 public:
  OptionsBar(App& a, const char* icon, std::string_view title, float width);
  ~OptionsBar();
  explicit operator bool() const { return open_; }
  OptionsBar(const OptionsBar&) = delete;
  OptionsBar& operator=(const OptionsBar&) = delete;

 private:
  bool open_ = false;
  std::optional<ui::Panel> panel_;
  std::optional<ui::HStack> row_;
};
// Ширина подписи для расчёта ширины панели.
float textW(std::string_view s, ui::Font f = ui::Font::Body);
// Фишка провинции (цвет владельца) с переходом к ней.
void provinceTag(const World& w, Id province);
float provinceTagW(const World& w, Id province);
// Кнопки «Убрать точку», «Отменить контур», «Готово». Возвращает 1 — готово, 2 — убрать точку, −1 — отмена, 0 — нет.
int sketchButtons(App& a, const Sketch& s, bool readOnly);
float sketchButtonsW();

// ---------------------------------------------------------------- регистрация
// Настоящие инструменты регистрируются статически; тесты, где подставные инструменты перекрыли настоящие,
// вызывают installAll(), чтобы вернуть их.
void registerBordersTool();
void registerDrawTools();
void registerClickTools();
void registerRouteTool();
void installAll();

}  // namespace rg::app::tools
