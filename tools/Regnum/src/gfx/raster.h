// Regnum — сглаживающий растеризатор контуров с точной площадью покрытия.
//
// Рёбра отсекаются по рамке в двойной точности (части левее рамки становятся вертикалями на её левой границе,
// правее и вне по вертикали — отбрасываются), затем переводятся в фиксированную точку 24.8 (1/256 пикселя).
// Каждое ребро раскладывается по ячейкам-пикселям: cover (смещение по y) и area (площадь слева от ребра).
// Покрытие пикселя = накопленный слева cover минус доля area; между ячейками строки покрытие постоянно,
// поэтому выход — разреженные отрезки: одиночные пиксели с переменным покрытием и сплошные пролёты.
// Очень большие наборы ячеек обрабатываются полосами по высоте (ограничение памяти).
#pragma once
#include "gfx/canvas.h"

namespace rg::gfx {

// Допуск аппроксимации кривых при заливке (пиксели устройства).
constexpr float kFillTolerance = 0.1f;

// Отрезок покрытия строки: cov != nullptr — значения по пикселям (len штук), иначе постоянное value.
struct Span {
  i32 x = 0, len = 0;
  const u8* cov = nullptr;
  u8 value = 0;
};

// Приёмник строк покрытия. Вызывается для строк по возрастанию y; отрезки по возрастанию x, не пересекаются.
struct SpanSink {
  virtual ~SpanSink() = default;
  virtual void row(int y, const Span* spans, int count) = 0;
};

class Rasterizer {
 public:
  Rasterizer() = default;

  // Начать новую фигуру с рамкой отсечения (пиксели устройства).
  void reset(const RectI& clip);
  const RectI& clip() const { return clip_; }

  // Контуры в координатах устройства. Каждый подпуть неявно замыкается.
  void moveTo(float x, float y);
  void lineTo(float x, float y);
  void close();
  void addPath(const Path& p, const Affine& m = {}, float tol = kFillTolerance);
  void addPolygon(const Pt* p, size_t n);

  bool empty() const { return edges_.empty() && !pend_; }
  // Встретилась нечисловая координата — фигура не рисуется.
  bool invalid() const { return invalid_; }
  // Пиксели, которые могут получить покрытие (внутри рамки отсечения).
  RectI bounds() const;

  // Обход строк покрытия.
  void sweep(FillRule rule, SpanSink& sink);

  // Внутренние типы (используются реализацией): ребро в фиксированной точке 24.8 и ячейка-пиксель.
  struct Edge { i32 x1, y1, x2, y2; };
  struct Cell { i32 x, y, cover, area; };

 private:
  void edge(double x1, double y1, double x2, double y2);
  void pushEdge(i32 x1, i32 y1, i32 x2, i32 y2);
  void pushLeft(i32 y1, i32 y2);
  void flushLeft();
  void closeContour();
  void sweepBand(int row0, int row1, bool evenOdd, SpanSink& sink);
  void sortRowX(Cell* c, Cell* e);

  RectI clip_;
  double cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0;
  std::vector<Edge> edges_;
  std::vector<Cell> cells_, sorted_, radixTmp_;
  std::vector<i32> rowStart_, rowPos_;
  std::vector<Span> spans_;
  std::vector<u8> covs_;
  double sx_ = 0, sy_ = 0, px_ = 0, py_ = 0;
  bool hasContour_ = false, invalid_ = false;
  bool pend_ = false;
  i32 pendY0_ = 0, pendY1_ = 0;
  i32 minX_ = 0, minY_ = 0, maxX_ = 0, maxY_ = 0;
};

// Растеризовать контур в маску покрытия (значения записываются, а не смешиваются).
// Маска покрывает область устройства [ox, ox + m.w) × [oy, oy + m.h); xf — из координат контура в устройство.
void rasterizeToMask(const Path& p, const Affine& xf, Mask& m, int ox = 0, int oy = 0,
                     FillRule rule = FillRule::NonZero, float tol = kFillTolerance);

// Доля площади, покрытой контуром (сумма покрытия / 255), — для проверок и измерений.
double coverageSum(const Mask& m);

}  // namespace rg::gfx
