// Regnum — холст: программная отрисовка со сглаживанием поверх gfx::Image.
// Все координаты — в текущем преобразовании (стек save/restore). Цвета — непремультиплицированные Color.
#pragma once
#include "gfx/image.h"
#include "gfx/path.h"

namespace rg::gfx {

enum class FillRule : u8 { NonZero, EvenOdd };
enum class Blend : u8 { Normal, Multiply, Screen, Add };
enum class Join : u8 { Miter, Round, Bevel };
enum class Cap : u8 { Butt, Round, Square };

struct Stroke {
  float width = 1;
  Join join = Join::Miter;
  Cap cap = Cap::Butt;
  float miterLimit = 4;
  std::vector<float> dash;   // длины штрихов и пропусков; пусто — сплошная
  float dashOffset = 0;
};

struct Gradient {
  enum Kind : u8 { Linear, Radial } kind = Linear;
  Pt p0, p1;                 // линейный: от p0 к p1; радиальный: центр p0, радиус r1 (p1 не используется)
  float r1 = 0;
  std::vector<std::pair<float, Color>> stops;  // позиция 0..1 -> цвет
};

struct Paint {
  Color color = Color(0, 0, 0, 255);
  const Gradient* gradient = nullptr;   // если задан — вместо цвета (в координатах текущего преобразования)
  const Image* image = nullptr;         // если задан — заливка изображением
  Affine imageXf;                       // изображение -> координаты контура
  bool bilinear = true;
  float opacity = 1;
  Blend blend = Blend::Normal;
  bool tile = false;                    // изображение повторяется (узор); иначе за краем — крайние пиксели
  Paint() = default;
  Paint(Color c) : color(c) {}  // NOLINT: неявное преобразование удобно
};

class Canvas {
 public:
  explicit Canvas(Image& target);
  Image& target() { return *img_; }
  int width() const;
  int height() const;

  // Состояние: преобразование, отсечение, прозрачность, режим смешивания.
  void save();
  void restore();
  void translate(float x, float y);
  void scale(float sx, float sy);
  void concat(const Affine& m);           // m применяется перед текущим
  void setTransform(const Affine& m);
  const Affine& transform() const;
  // Пересечение с текущим отсечением. Без поворота — по сетке пикселей (края округляются),
  // с поворотом или наклоном — сглаженная маска.
  void clipRect(const RectF& r);
  void clipRoundRect(const RectF& r, float radius);
  void clipPath(const Path& p, FillRule rule = FillRule::NonZero);
  RectI clipBounds() const;               // в пикселях устройства
  bool quickReject(const RectF& r) const; // прямоугольник целиком вне отсечения
  void setOpacity(float a);               // умножается на текущую
  float opacity() const;
  void setBlend(Blend b);

  void clear(Color c);                    // без учёта отсечения и преобразования
  void fillPath(const Path& p, const Paint& paint, FillRule rule = FillRule::NonZero);
  void strokePath(const Path& p, const Stroke& s, const Paint& paint);
  void fillRect(const RectF& r, const Paint& paint);
  void fillRoundRect(const RectF& r, float radius, const Paint& paint);
  void fillRoundRect(const RectF& r, float tl, float tr, float br, float bl, const Paint& paint);
  // Обводки strokeRoundRect/strokeCircle/line идут по центру контура (половина толщины снаружи).
  void strokeRoundRect(const RectF& r, float radius, float width, const Paint& paint);
  void fillCircle(float cx, float cy, float r, const Paint& paint);
  void strokeCircle(float cx, float cy, float r, float width, const Paint& paint);
  void line(float x0, float y0, float x1, float y1, float width, const Paint& paint, Cap cap = Cap::Round);
  void polyline(const Pt* pts, size_t n, bool closed, const Stroke& s, const Paint& paint);

  // Изображения (билинейно, с учётом преобразования и прозрачности). Края сглажены, за краем выборки —
  // крайние пиксели; уменьшение более чем в 2 раза без поворота — усреднение по площади.
  void drawImage(const Image& img, const RectF& dst, float opacity = 1, bool bilinear = true);
  void drawImage(const Image& img, const RectF& src, const RectF& dst, float opacity = 1, bool bilinear = true);
  void drawImageXf(const Image& img, const Affine& imageToUser, float opacity = 1, bool bilinear = true);

  // Маска покрытия (глифы): левый верхний угол в координатах пользователя; поворот/масштаб не применяются,
  // только перенос текущего преобразования (кегль учитывается при растеризации глифа). Точка (x, y)
  // преобразуется текущей матрицей и округляется до целого пикселя.
  void fillMask(const Mask& m, float x, float y, const Paint& paint);

  // Мягкая тень прямоугольника со скруглением (кешируется по размеру/радиусу/размытию).
  // blur — как в CSS box-shadow (σ = blur / 2), spread расширяет прямоугольник и радиус.
  void boxShadow(const RectF& r, float radius, float blur, float spread, Color c, Pt offset = {});
  // Размытие области цели (фон под стеклянными панелями). Область — в пикселях устройства,
  // ограничивается отсечением; radius — как blur тени (σ = radius / 2); за краем — крайние пиксели.
  void blurRegion(const RectI& region, float radius);

 private:
  struct State;
  Image* img_;
  std::vector<std::shared_ptr<State>> stack_;
  std::shared_ptr<State> st_;
  friend struct CanvasImpl;
};

}  // namespace rg::gfx
