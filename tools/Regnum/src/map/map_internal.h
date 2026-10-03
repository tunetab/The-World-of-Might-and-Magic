// Regnum — внутренности отрисовщика карты (общие для файлов src/map/*.cpp, кроме basemap*).
//
// Схема кадра. Всё, что лежит под подписями (белая суша, заливка, море, реки, границы, штриховка, символы),
// собирается в фоне в непрозрачные «составные» тайлы kTile × kTile пикселей устройства при точном масштабе
// камеры. Кадр в покое — копирование готовых тайлов; при анимации и до прихода тайлов — лучшие доступные тайлы
// других масштабов, опорный уровень kBaseScale (вся карта) или превью базовой карты. Поверх — подписи, маршруты,
// диаграммы, знаки, фигурки и выделение (векторно, в каждом кадре; подписи — из кеша спрайтов).
#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

#include "geo/topo.h"
#include "gfx/canvas.h"
#include "gfx/text.h"
#include "map/basemap.h"
#include "map/mapview.h"
#include "rules/rules.h"

namespace rg::map::detail {

constexpr int kTile = 512;            // сторона составного тайла, пиксели устройства
constexpr double kBaseScale = 0.25;   // опорный уровень: пикселей устройства на единицу карты (= уровень 2 пирамиды)

// ---------------------------------------------------------------- стиль тайлов и цвета режимов
struct TileStyle {
  schema::MapMode mode = schema::MapMode::Political;
  bool editBorders = false;
  float fillOpacity = 0.5f;
  float dpi = 1;
  bool operator==(const TileStyle&) const = default;
  u64 key() const;
};

struct ProvLook {
  Color fill{0, 0, 0, 0};    // a == 0 — без заливки (иначе непрозрачный цвет)
  Color hatch{0, 0, 0, 0};   // штриховка оккупации; a == 0 — нет
  Id state = 0;              // владелец (для границ государств)
  bool land = true;          // false — морская провинция
  bool operator==(const ProvLook&) const = default;
};

struct Looks {
  std::unordered_map<Id, ProvLook> prov;
  std::unordered_map<Id, Color> stateLine;   // цвет внутренней обводки государства
  float fillAlpha = 0.5f;                    // прозрачность наложения заливки
  float provLineAlpha = 0.34f;               // границы провинций на суше
  float seaLineAlpha = 0.55f;                // границы морских провинций
  float stateWidth = 2.2f;                   // видимая ширина полосы государства, логические пиксели
  float stateDark = 0.0f;                    // 1 — нейтральные тёмные границы государств (режимы данных)
  float seamAlpha = 0.47f;                   // тонкая линия стыка двух государств
  bool hatch = true;
};
std::shared_ptr<const Looks> computeLooks(const World& w, const TileStyle& s);
std::vector<LegendItem> legendFor(const World& w, schema::MapMode mode);
Color fallbackColor(Id id);   // цвет фракции без записи
// Цвета шкал режимов данных (t ∈ [0, 1]).
Color contentmentColor(double t);
Color rebellionColor(double t);
Color tradeColor(double t);
Color neutralColor();

// ---------------------------------------------------------------- геометрия для тайлов
struct EdgeGeo {
  Id id = 0;
  EdgeKind kind = EdgeKind::Border;
  Id pl = 0, pr = 0;
  Terrain tl = Terrain::Land, tr = Terrain::Land;
  Box2 box;
  std::vector<Vec2> pts;   // a, промежуточные, b
};

struct GeoIndex {
  std::shared_ptr<const geo::FaceSet> faces;
  std::vector<EdgeGeo> edges;
  std::unordered_map<Id, std::vector<u32>> provEdges;   // провинция -> дуги её границы
  double cell = 256;
  int cols = 0, rows = 0;
  std::vector<std::vector<u32>> grid;                   // ячейка -> дуги
  // Дуги, габарит которых пересекает b (без повторов).
  void edgesIn(const Box2& b, std::vector<u32>& out) const;
};
// Кеш по тождеству таблиц nodes/edges (потокобезопасно).
std::shared_ptr<const GeoIndex> geoIndex(const World& w);

// Полилиния, обрезанная по прямоугольнику с запасом: куски внутри box (с одной точкой снаружи на концах).
template <class F>
void clipPolyline(const std::vector<Vec2>& pts, const Box2& box, F&& emit) {
  const size_t n = pts.size();
  size_t i = 0;
  auto segIn = [&](size_t k) {
    const Vec2 a = pts[k], b = pts[k + 1];
    return !(std::max(a.x, b.x) < box.x0 || std::min(a.x, b.x) > box.x1 || std::max(a.y, b.y) < box.y0 || std::min(a.y, b.y) > box.y1);
  };
  while (i + 1 < n) {
    if (!segIn(i)) { i++; continue; }
    size_t j = i;
    while (j + 1 < n && segIn(j)) j++;
    emit(pts.data() + i, j - i + 1);
    i = j;
  }
}

// ---------------------------------------------------------------- растры базовой карты
class RasterCache {
 public:
  RasterCache(const Basemap* bm, size_t budgetBytes);
  // Тайл слоя (premultiplied); пустой тайл или ошибка — nullptr. Декодирует при промахе в вызывающем потоке.
  std::shared_ptr<const gfx::Image> get(int layer, int z, int x, int y);
  std::shared_ptr<const gfx::Image> peek(int layer, int z, int x, int y) const;
  size_t bytes() const;

 private:
  struct Entry {
    std::shared_ptr<const gfx::Image> img;
    bool loading = false;
    u64 used = 0;
  };
  const Basemap* bm_;
  size_t budget_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::unordered_map<u64, Entry> map_;
  size_t bytes_ = 0;
  u64 clock_ = 0;
  void evictLocked();
};

// ---------------------------------------------------------------- составной тайл
struct TileScene {
  World world;
  TileStyle style;
  u64 styleKey = 0;
  std::shared_ptr<const Looks> looks;
  u64 gen = 0;
};
// Нарисовать тайл (tx, ty) при масштабе ds (пикселей устройства на единицу карты). Потокобезопасно.
gfx::Image renderTile(const TileScene& sc, const GeoIndex& g, const Basemap* bm, RasterCache* rc, double ds, int tx, int ty);
// Только базовая карта (белая суша + слои) в изображение out: пиксель (i, j) = точка карты ((ox + i + 0,5) / ds, ...).
void composeBasemap(gfx::Image& out, const Basemap* bm, RasterCache* rc, double ds, double ox, double oy, int fromLayer, int toLayer);

// ---------------------------------------------------------------- хранилище и фоновая отрисовка тайлов
struct TileKey {
  u64 style = 0;
  double ds = 0;   // пикселей устройства на единицу карты
  i32 tx = 0, ty = 0;
  bool operator==(const TileKey&) const = default;
};
struct TileKeyHash { size_t operator()(const TileKey& k) const; };
struct TileRequest { TileKey key; double prio = 0; };   // меньше — раньше
struct TileView { TileKey key; std::shared_ptr<const gfx::Image> img; bool stale = false; };

class TileStore {
 public:
  TileStore(const Basemap* bm, size_t rasterBudget, size_t tileBudget);
  ~TileStore();   // останавливает работу и ждёт выполняющиеся задачи
  TileStore(const TileStore&) = delete;
  TileStore& operator=(const TileStore&) = delete;

  void setScene(std::shared_ptr<const TileScene> sc);
  // Пометить устаревшими тайлы текущего стиля, пересекающие dirty (все — при all); тайлы прочих стилей — все.
  void invalidate(const std::vector<Box2>& dirty, u64 gen, u64 styleKey, bool all);
  // Заменить очередь: недостающие и устаревшие из reqs рисуются в фоне по приоритету; used — номер кадра.
  void request(const std::vector<TileRequest>& reqs, u64 used);
  // Готовые тайлы стиля (style == 0 — любого) с габаритом карты, пересекающим box.
  void available(u64 style, const Box2& box, std::vector<TileView>& out) const;
  bool get(const TileKey& k, TileView& out) const;
  void touch(const std::vector<TileKey>& keys, u64 used);
  void trim(u64 frame, u64 keepStyle);
  bool takeArrived();
  bool arrived() const;
  bool busy() const;
  bool wait(double timeoutSec);
  void setWake(std::function<void()> fn);
  int pending() const;
  size_t bytes() const;
  RasterCache& raster();

  struct Shared;

 private:
  std::shared_ptr<Shared> s_;
};

// ---------------------------------------------------------------- пиксели
// Наложение premultiplied-изображения src (opacity 0..1) на dst со сдвигом (dx, dy), с отсечением clip.
void blendImage(gfx::Image& dst, const gfx::Image& src, int dx, int dy, const gfx::RectI& clip, float opacity = 1);
// Непрозрачная копия (src целиком непрозрачен) со сдвигом.
void copyImage(gfx::Image& dst, const gfx::Image& src, int dx, int dy, const gfx::RectI& clip);
// Масштабированная копия (билинейно, по площади при уменьшении): src -> прямоугольник dst (дробные края).
// Пиксели dst в [round(x0), round(x1)) × [round(y0), round(y1)) ∩ clip.
void scaleImage(gfx::Image& dst, const gfx::Image& src, double x0, double y0, double x1, double y1, const gfx::RectI& clip, bool opaque);

// ---------------------------------------------------------------- подписи
struct LabelKey {
  std::string text;
  u8 kind = 0;          // 0 — государство, 1 — провинция, 2 — войско
  u16 size4 = 0;        // кегль в ¼ пикселя устройства
  u32 color = 0;
  bool operator==(const LabelKey&) const = default;
};
struct LabelSprite {
  gfx::Image img;
  float ox = 0, oy = 0;   // центр текста относительно левого верхнего угла изображения
  float w = 0, h = 0;     // размер текста (без ореола), пиксели устройства
};
class LabelCache {
 public:
  std::shared_ptr<const LabelSprite> get(const LabelKey& k);            // создаёт при промахе
  // Без создания: точный спрайт или (nearSize != nullptr) та же подпись ближайшего кегля; nearSize — её кегль.
  std::shared_ptr<const LabelSprite> peek(const LabelKey& k, u16* nearSize);
  void trim(size_t maxItems);
  size_t size() const { return map_.size(); }

 private:
  struct H { size_t operator()(const LabelKey& k) const; };
  struct E { std::shared_ptr<const LabelSprite> s; u64 used = 0; };
  std::unordered_map<LabelKey, E, H> map_;
  u64 clock_ = 0;
};
std::shared_ptr<LabelSprite> renderLabel(const LabelKey& k);

// ---------------------------------------------------------------- отметки войск и флота (marks.cpp)
// Раскладка в пространстве «карта × масштаб» (логические пиксели): не зависит от сдвига камеры.
struct MarkLayout {
  struct Mark {
    std::vector<Id> members;   // top — первым, далее по приоритету
    Vec2 pos;                  // точка карты (позиция top)
    RectF rel;                 // габариты относительно центра верхней фигурки, логические пиксели
    i64 units = 0;
  };
  std::vector<Mark> marks;     // в порядке отрисовки (по y точки, затем по ID верхнего объекта)
  float figure = 24;           // размер фигурки, логические пиксели
};
// Размер фигурки войска на экране (логические пиксели) при масштабе zoom.
float figureSizeAt(double zoom);
// Раскладка: объекты, отметки которых пересеклись бы (с зазором), собираются в стопки. Верхний объект стопки —
// выделенный (sel), иначе самый многочисленный, затем с меньшим ID.
MarkLayout layoutMarks(const std::vector<const Army*>& armies, double zoom, Id sel);
// Геометрия отметки (общая для раскладки, отрисовки и попадания): c — центр верхней фигурки, size — её размер,
// dpi — масштаб постоянных размеров значков (1 — логические пиксели).
gfx::TextStyle markBadgeStyle(float dpi);
gfx::Pt stackOffset(int k, float size);   // сдвиг k-й (1, 2) фигурки за верхней
int stackDepth(size_t count);             // сколько фигурок рисуется за верхней (0…2)
RectF unitBadgeRect(gfx::Pt c, float size, const std::string& text, float dpi);
RectF countBadgeRect(gfx::Pt c, float size, const std::string& text, float dpi);
std::string countBadgeText(size_t count);
RectF markFootprint(float size, size_t count, i64 units);   // относительно центра, логические пиксели
bool markHit(const MarkLayout::Mark& m, float size, float dx, float dy);   // dx, dy — от центра верхней фигурки

// ---------------------------------------------------------------- кадр
// Контекст отрисовки наложений: точка карты -> пиксель устройства (сетка тайлов выровнена по целым пикселям).
struct FrameCtx {
  gfx::Canvas* c = nullptr;
  const World* w = nullptr;
  const geo::FaceSet* fs = nullptr;
  const RenderOptions* opt = nullptr;
  double X0 = 0, Y0 = 0, ds = 1;   // устройство = (X0 + x·ds, Y0 + y·ds)
  float dpi = 1;
  double zoom = 1;
  gfx::RectI clip;                  // видимая область, пиксели устройства
  Box2 visible;                     // видимая область карты
  const MarkLayout* marks = nullptr;   // отметки войск и флота кадра
  gfx::Pt dev(Vec2 m) const { return {float(X0 + m.x * ds), float(Y0 + m.y * ds)}; }
};

// Линия маршрута на карте — ровно та ломаная, по которой считаются провинции маршрута (geo::provincesOnPolyline,
// бонус +10 %): без сглаживания, только без повторяющихся точек; изломы скругляются при обводке.
std::vector<Vec2> routeLine(const std::vector<Vec2>& pts);
Color routeColor(const World& w, const Route& r);
// Путь колец провинции в пикселях устройства.
void provincePath(const FrameCtx& f, Id province, gfx::Path& out);
// Наложения кадра (overlay.cpp).
struct Obstacles {
  std::vector<gfx::RectI> rects;
  bool hit(const gfx::RectI& r) const;
  void add(const gfx::RectI& r) { rects.push_back(r); }
};
void drawHover(const FrameCtx& f);
void drawRoutes(const FrameCtx& f);
void drawGuildPies(const FrameCtx& f, Obstacles& obs);
void drawMarkers(const FrameCtx& f, Obstacles& obs, bool capitals, bool hqs);
void collectMarkerObstacles(const FrameCtx& f, Obstacles& obs, bool capitals, bool hqs, bool pies);
void drawArmies(const FrameCtx& f);                          // отметки f.marks
void collectArmyObstacles(const FrameCtx& f, Obstacles& obs);
void drawSelection(const FrameCtx& f);
// budgetMs — время на новые спрайты в этом кадре; false — часть подписей отложена (нужен ещё кадр).
bool drawLabels(const FrameCtx& f, LabelCache& cache, Obstacles& obs, float figurePx, double budgetMs);
float pieRadius(double zoom);        // радиус диаграммы гильдий, логические пиксели
bool pieVisible(const FrameCtx& f, Id province);
i64 armyCount(const Army& a);
std::string compactCount(i64 n);

}  // namespace rg::map::detail
