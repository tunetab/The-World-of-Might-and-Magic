// Regnum — отрисовка карты: камера, тайловые слои, режимы карты, подписи, объекты, попадание мышью.
//
// MapView не знает об интерфейсе приложения: получает мир, параметры отрисовки и прямоугольник экрана,
// рисует в gfx::Canvas. Порядок слоёв (снизу вверх): белая суша → заливка провинций (режим карты) →
// море → реки и озёра → заливка сухопутных провинций на морской части (полупрозрачно) → границы провинций
// и государств, штриховка оккупации → символы карты (горы, замки) → подписи → маршруты, диаграммы гильдий,
// штабы, столицы → войска и флот → выделение/наведение.
//
// Войска и флот не наслаиваются на экране (ТЗ 1.c.iv): фигурки, которые при текущем масштабе перекрылись бы,
// рисуются одной отметкой-стопкой (верхняя фигурка, за ней фигурки других объектов цветами их фракций,
// значок с числом объектов). При приближении стопка распадается на отдельные фигурки (см. ArmyMark).
#pragma once
#include <optional>

#include "core/schema.h"
#include "core/world.h"
#include "gfx/canvas.h"

namespace rg::map {

class Basemap;

// Снимок камеры для инструментов и наложений.
struct View {
  double cx = 4000, cy = 2250;   // точка карты в центре области просмотра
  double zoom = 0.2;             // логических пикселей на единицу карты
  RectF viewport;                // область карты на экране (логические пиксели)
  float dpi = 1;                 // логический пиксель -> пиксели устройства

  Vec2 toMap(float sx, float sy) const {
    return {cx + (double(sx) - double(viewport.cx())) / zoom, cy + (double(sy) - double(viewport.cy())) / zoom};
  }
  gfx::Pt toScreen(Vec2 m) const {
    return {float(double(viewport.cx()) + (m.x - cx) * zoom), float(double(viewport.cy()) + (m.y - cy) * zoom)};
  }
  double toMapLen(double screenPx) const { return screenPx / zoom; }   // экранная длина -> единицы карты
  Box2 visibleBox() const {
    return {cx - viewport.w * 0.5 / zoom, cy - viewport.h * 0.5 / zoom, cx + viewport.w * 0.5 / zoom, cy + viewport.h * 0.5 / zoom};
  }
};

struct RenderOptions {
  schema::MapMode mode = schema::MapMode::Political;
  Id selProvince = 0, selFaction = 0, selArmy = 0, selRoute = 0;
  Id hoverProvince = 0, hoverArmy = 0;
  bool editBorders = false;          // режим правки: все границы видны чётко, заливка приглушена
  std::vector<Id> hideArmies;        // объекты, которые сейчас перетаскиваются (рисует инструмент)
  bool labels = true;
  bool darkUi = true;                // для цвета рамки/фона за пределами карты
};

struct LegendItem { Color color; std::string label; std::string icon; };

// Отметка войска/флота на экране: одиночная фигурка или стопка объектов, которые при текущем масштабе
// наложились бы друг на друга. Раскладка зависит от масштаба, выделения и скрытых (перетаскиваемых) объектов
// последнего кадра, но не от сдвига камеры.
struct ArmyMark {
  Id top = 0;                 // объект, нарисованный сверху (выделенный, если он в стопке)
  std::vector<Id> members;    // все объекты отметки, top — первым
  Vec2 pos;                   // точка карты, где нарисована отметка (позиция top)
  RectF bounds;               // габариты на экране при текущей камере (логические пиксели): фигурки и значки
  i64 units = 0;              // численность всех объектов отметки
  bool cluster() const { return members.size() > 1; }
};

// Сведения о последнем кадре (для проверок производительности и отладки).
struct RenderStats {
  double frameMs = 0;      // render() целиком
  double composeMs = 0;    // фон и тайлы (без наложений)
  double labelsMs = 0;     // подписи (размещение и новые спрайты)
  int tilesDrawn = 0;      // готовых тайлов точного масштаба
  int fallbackOps = 0;     // запасных изображений (другой масштаб, опорный уровень, превью)
  bool fallback = false;   // часть области показана запасными изображениями
  int pending = 0;         // тайлов в очереди и в работе
  bool labelsDeferred = false;  // часть подписей отложена до следующих кадров
};

class MapView {
 public:
  explicit MapView(const Basemap* basemap);   // basemap может быть nullptr (рисуется только белая суша и море цветом)
  ~MapView();

  // ---- мир ----
  void setWorld(const World& w);              // текущий отображаемый мир (вызывается при каждом изменении)
  // Точечная инвалидация: what — биты TableBit изменённых таблиц.
  void worldChanged(const World& before, const World& after, u32 what);

  // ---- камера ----
  void setViewport(RectF logical, float dpi);
  // Свободная часть области просмотра между панелями интерфейса (логические пиксели; пустая — вся область).
  // «Показать всю карту» вписывает мир в неё, минимальный масштаб позволяет увидеть в ней весь мир.
  void setSafeArea(RectF logical);
  RectF safeArea() const;                     // заданная свободная часть (пустая — не задана)
  const View& view() const;
  void zoomAt(float sx, float sy, double factor, bool animate = true);   // колесо мыши: точка под курсором неподвижна
  void panBy(float dxScreen, float dyScreen);
  void centerOn(Vec2 p, double zoom = 0, bool animate = true);          // zoom 0 — не менять
  void fit(Box2 box, bool animate = true);                               // показать область с отступом
  void fitAll(bool animate = true);                                      // весь мир в свободной части (setSafeArea)
  double minZoom() const;
  double maxZoom() const;
  void update(double timeSec);               // анимации камеры
  bool animating() const;

  // ---- отрисовка ----
  // Рисует область view().viewport в canvas (canvas в пикселях устройства; масштаб dpi учитывается внутри).
  void render(gfx::Canvas& c, const RenderOptions& opt);
  // Тайлы догружаются в фоне: true — пришли новые данные, нужен ещё кадр. Без setWakeCallback — также true,
  // пока идёт фоновая работа (приложение перерисовывает кадры само).
  bool needsRedraw() const;
  // Вызывается из фоновых потоков, когда готовы новые тайлы (например, platform::wake). Необязательно.
  void setWakeCallback(std::function<void()> fn);
  bool loading() const;                       // есть тайлы в очереди или в работе
  bool waitIdle(double timeoutSec = 10);      // дождаться фоновой отрисовки запрошенных тайлов (тесты, CLI)
  const RenderStats& stats() const;
  // Мини-карта в прямоугольнике экрана (логические пиксели) с рамкой видимой области.
  void renderMinimap(gfx::Canvas& c, RectF rect, float dpi);
  // Точка мини-карты -> точка карты (для перехода щелчком).
  Vec2 minimapToMap(RectF rect, float sx, float sy) const;
  std::vector<LegendItem> legend(const World& w, schema::MapMode mode) const;

  // ---- попадание ----
  Id provinceAt(float sx, float sy) const;   // экранные координаты
  Id armyAt(float sx, float sy) const;       // фигурка войска/флота под курсором (у стопки — верхний объект)
  std::optional<ArmyMark> markAt(float sx, float sy) const;   // отметка под курсором (одиночная или стопка)
  std::vector<ArmyMark> armyMarks() const;   // все отметки при текущей камере в порядке отрисовки
  // Масштаб, при котором объекты стопки m разойдутся на отдельные фигурки (не больше maxZoom); 0 — при текущем
  // масштабе приблизить уже некуда (объекты стоят слишком тесно).
  double separateZoom(const ArmyMark& m) const;
  Id routeAt(float sx, float sy, float tolPx = 6) const;

  // Размер фигурки войска на экране (логические пиксели) при текущем масштабе.
  float figureSize() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace rg::map
