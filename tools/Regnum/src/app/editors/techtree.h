// Regnum — общие части полноэкранных схем деревьев (технологии, постройки): камера с панорамой и масштабом,
// связи-кривые со стрелками, точечная сетка, мини-карта, полоса масштаба, всплывающая карточка сведений.
// Узлы рисуются в мировых единицах схемы на холсте ui::custom с преобразованием камеры: текст и значки
// остаются чёткими при любом масштабе. Ввод — ui::interact по экранным прямоугольникам (точки интерфейса).
#pragma once
#include "app/app.h"
#include "gfx/text.h"

namespace rg::app {

// Открыть дерево технологий фракции и выделить технологию (0 — без выделения).
void openTechTree(App& a, Id faction, Id tech = 0);

}  // namespace rg::app

namespace rg::app::tree {

// ---------------------------------------------------------------- камера
constexpr double kMinZoom = 0.2, kMaxZoom = 2.0;

struct Camera {
  double x = 0, y = 0;   // мировая точка в левом верхнем углу холста
  double z = 1;          // точек интерфейса на единицу схемы
  bool ready = false;    // вписана при первом показе
  // Плавный переход (вписать, показать узел).
  double fx = 0, fy = 0, fz = 1, tx = 0, ty = 0, tz = 1;
  double t0 = -1;
  float insetLeft = 0;   // левая часть холста закрыта колонкой (подписи дорожек): вписывать правее
};

Vec2 toWorld(const Camera& c, RectF canvas, float sx, float sy);
gfx::Pt toScreen(const Camera& c, RectF canvas, Vec2 p);
RectF toScreen(const Camera& c, RectF canvas, double x, double y, double w, double h);
void zoomAt(Camera& c, RectF canvas, float sx, float sy, double factor);
void zoomCenter(Camera& c, RectF canvas, double factor);
// Вписать прямоугольник схемы (с полями); масштаб не больше maxZoom.
void fit(Camera& c, RectF canvas, const Box2& bounds, bool animate, double maxZoom = 1.0);
// Показать область, если она не видна целиком (центр — в середину холста).
void reveal(Camera& c, RectF canvas, const Box2& box, bool animate);

// Шаг анимации камеры, панорама фоном (левая или средняя кнопка) и масштаб колесом (wheel — указатель над
// холстом, а не над наложенной панелью). Вызывать каждый кадр.
struct Pan {
  bool on = false;
  double x = 0, y = 0;
};
void panZoom(Camera& cam, RectF canvas, const ui::Interaction& bg, Pan& pan, bool wheel);

// ---------------------------------------------------------------- связи
// Кубическая кривая от выхода (правый край узла-источника) ко входу (левый край узла-цели).
struct Curve {
  Vec2 p0, c0, c1, p1;
};
Curve curve(Vec2 from, Vec2 to);
Vec2 curveAt(const Curve& k, double t);
double curveDistance(const Curve& k, Vec2 p);   // расстояние в мировых единицах

// ---------------------------------------------------------------- отрисовка в мировых единицах
// Цвета темы — копия на кадр (колбэк ui::custom выполняется при сведении слоя).
struct Ink {
  Color bg, grid, surface, surfaceHi, border, borderStrong, text, textDim, textMuted;
  Color accent, success, warning, danger, info, onAccent, shadow;
  bool dark = true;
};
Ink ink();
// Перейти от пикселей устройства холста к мировым единицам схемы.
void applyCamera(gfx::Canvas& c, RectF dev, float scale, const Camera& cam);
// Подложка и точечная сетка видимой части.
void drawGrid(gfx::Canvas& c, RectF dev, float scale, const Camera& cam, const Ink& k);
// Связь (в мировых единицах; толщина — в точках интерфейса, не растёт с масштабом сверх 1,6×).
void drawCurve(gfx::Canvas& c, const Curve& k, Color col, float width, double zoom, bool arrow, bool dashed = false);
// Гнездо связи узла: кольцо (пустое) или точка (связано); hot — увеличено, со знаком «+».
void drawPort(gfx::Canvas& c, Vec2 p, Color ring, Color fill, bool filled, bool hot, double zoom);
gfx::TextStyle textStyle(float size, gfx::FontWeight weight = gfx::FontWeight::Regular, gfx::FontFamily family = gfx::FontFamily::UI);
// Текст в прямоугольнике мировых единиц (одна строка с многоточием или перенос до maxLines).
void text(gfx::Canvas& c, std::string_view s, const gfx::TextStyle& st, RectF box, Color col, gfx::Align align = gfx::Align::Left,
          int maxLines = 1, gfx::VAlign valign = gfx::VAlign::Middle);
void icon(gfx::Canvas& c, std::string_view name, RectF box, Color col);

// ---------------------------------------------------------------- поверх холста (ui)
// Полоса масштаба в левом нижнем углу холста: −, процент (щелчок — 100 %), +, вписать.
void zoomBar(App& a, Camera& cam, RectF canvas, const Box2& bounds, std::string_view mark);
// Мини-карта в правом нижнем углу: узлы и рамка видимой части; щелчок и перетаскивание — переход.
struct MiniItem {
  Box2 box;
  Color color;
  bool selected = false;
};
void minimap(Camera& cam, RectF canvas, const Box2& bounds, const std::vector<MiniItem>& items, std::string_view mark);

// Карточка сведений рядом с узлом (после задержки наведения): заголовок, строки со значками, абзац.
struct TipLine {
  std::string icon;
  std::string text;
  Color color;
  bool wrap = false;      // абзац с переносом
  bool strong = false;
};
struct Tip {
  std::string title, subtitle;
  Color accent;           // полоска и значок
  std::string icon;
  std::vector<TipLine> lines;
};
// anchor — экранный прямоугольник узла, area — где можно показать.
void tipCard(const Tip& t, RectF anchor, RectF area);
// Задержка подсказки: true, когда указатель держится над key не меньше 0,45 с (без кнопок мыши).
bool hoverDelay(u64 key);

// Пустое состояние по центру холста: 1 — нажата основная кнопка, 2 — дополнительная (secondary непусто), 0 — нет.
// mark непусто — прямоугольники кнопок: «mark.add» и «mark.alt» (App::markUi).
int canvasEmpty(RectF canvas, const char* icon, std::string_view text, std::string_view action, const char* actionIcon, bool disabled,
                std::string_view secondary = {}, const char* secondaryIcon = nullptr, std::string_view mark = {});

// Прямоугольник кнопки только что нарисованного ui::emptyState (lastItem — весь блок): для App::markUi и тестов.
RectF emptyActionRect(std::string_view action, const char* actionIcon);

// Римская цифра уровня (1 → I).
std::string roman(int n);

// ---------------------------------------------------------------- фишки с переносом
// Ряд ui::HStack не переносит, а фишек (условия, эффекты, модификаторы) бывает много. Внутри ChipFlow фишки
// tree::chip встают в ряды по ширине текущей области; при закрытии место занимается в потоке.
//   { tree::ChipFlow f; for (...) if (tree::chip(name, {...}) == ui::ChipAction::Click) ...; }
struct ChipFlow {
  ChipFlow();
  ~ChipFlow();
  ChipFlow(const ChipFlow&) = delete;
  ChipFlow& operator=(const ChipFlow&) = delete;
};
ui::ChipAction chip(std::string_view label, const ui::ChipOpt& o = {});
// Фишки эффектов набора модификаторов (польза — зелёным, вред — красным) с переносом. false — эффектов нет.
bool effectChips(const World& w, const std::vector<Id>& modifiers);
// Список модификаторов фишками с переносом (щелчок — окно модификаторов, крестик — убрать) и выбор для
// добавления. Как w::modifierList, но без пустого ряда, когда список пуст. true — список изменился.
bool modifierList(std::string_view id, std::vector<Id>& ids, bool disabled = false);

// Переключатель дерева: выбор фракции (states — только государства). Доступен и при просмотре прошлого хода
// (это переход, а не правка). noneLabel непусто — пункт «0» первым (например, «Общее дерево»).
bool factionSwitch(std::string_view id, Id& value, bool states, std::string_view noneLabel = {});

}  // namespace rg::app::tree
