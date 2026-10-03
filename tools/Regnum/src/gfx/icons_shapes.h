// Regnum — внутренний построитель реестров векторных глифов (значки, эмблемы) и генераторы строк путей SVG.
// Не для использования вне gfx: глифы описываются строками SVG, слои — обводка/заливка/ластик/вставка.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx/icons.h"

namespace rg::gfx {

// ---------------------------------------------------------------- строки путей
namespace shapes {
std::string n(double v);                                                       // число для пути
std::string circ(double cx, double cy, double r);                              // окружность (по часовой)
std::string ell(double cx, double cy, double rx, double ry);                   // эллипс
std::string rrect(double x, double y, double w, double h, double r);           // скруглённый прямоугольник
std::string poly(std::initializer_list<double> xy, bool closed = true);        // ломаная x0 y0 x1 y1 ...
std::string star(double cx, double cy, double R, double r, int points, double rotDeg);
std::string gear(double cx, double cy, double ro, double ri, int teeth, double topHalfDeg, double baseHalfDeg);
std::string arc(double cx, double cy, double r, double fromDeg, double toDeg);  // открытая дуга (градусы, ось Y вниз)
std::string dashedCircle(double cx, double cy, double r, int dashes, double fill);
// Отражение пути относительно вертикали x = axis (точки и флаги дуг пересчитываются разбором).
std::string mirrorX(std::string_view d, double axis);
}  // namespace shapes

// ---------------------------------------------------------------- построитель
class GlyphBuilder {
 public:
  // Слой описания: строка пути с видом слоя, вставка другого глифа или преобразование.
  struct L {
    enum Op : u8 { Layer, Use } op = Layer;
    VecLayer::Kind kind = VecLayer::Stroke;
    float width = 0;
    std::string d;      // путь SVG или имя вставляемого глифа
    Affine xf;          // преобразование слоя (в единицах сетки)
  };
  static L S(std::string d, float width = 0) { return {L::Layer, VecLayer::Stroke, width, std::move(d), {}}; }
  static L F(std::string d) { return {L::Layer, VecLayer::Fill, 0, std::move(d), {}}; }
  static L FE(std::string d) { return {L::Layer, VecLayer::FillEvenOdd, 0, std::move(d), {}}; }
  static L X(std::string d) { return {L::Layer, VecLayer::Erase, 0, std::move(d), {}}; }
  static L XS(std::string d, float width) { return {L::Layer, VecLayer::EraseStroke, width, std::move(d), {}}; }
  static L U(std::string name, float width = 0) { return {L::Use, VecLayer::Stroke, width, std::move(name), {}}; }
  static L T(L l, const Affine& xf) { l.xf = xf * l.xf; return l; }

  GlyphBuilder(float grid, u64 idBase);

  void add(const char* name, std::initializer_list<L> layers) { addList(name, std::vector<L>(layers)); }
  void addList(const char* name, const std::vector<L>& layers);

  const VecGlyph* find(std::string_view name) const;
  const std::vector<std::string>& names() const { return names_; }
  const std::vector<std::string>& issues() const { return issues_; }
  float grid() const { return grid_; }

 private:
  struct SvHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return size_t(hash64(s)); }
  };
  struct SvEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const { return a == b; }
  };
  float grid_;
  u64 idBase_;
  std::vector<VecGlyph> glyphs_;
  std::unordered_map<std::string, size_t, SvHash, SvEq> index_;
  std::vector<std::string> names_;
  std::vector<std::string> issues_;
};

}  // namespace rg::gfx
