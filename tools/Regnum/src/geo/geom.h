// Regnum — геометрические примитивы плоского графа провинций.
//
// Соглашение о сторонах: точка c лежит СЛЕВА от направленного отрезка a->b, если cross(b - a, c - a) > 0
// (математическая система координат). На карте ось Y направлена вниз, поэтому визуально это правая сторона.
// Метки Edge::pl/pr (провинция и рельеф «слева/справа») используют именно это определение.
//
// Предикаты (orient, segRelation, onSegment) точные: знак определителя вычисляется адаптивно
// (быстрая оценка с гарантированной границей ошибки, при сомнении — точная сумма разложений).
// Точки пересечения, проекции и расстояния — обычная арифметика double.
#pragma once
#include "base/base.h"

namespace rg::geo {

// ---------------------------------------------------------------- допуски (единицы карты, карта 8000 × 4500)
constexpr double kTinyLen = 1e-7;      // отрезок короче — вырожденный (validate)
constexpr double kNearLen = 1e-5;      // вершина ближе к чужому отрезку — почти-касание (validate)
constexpr double kMinLen = 1e-4;       // операции не создают отрезков короче
constexpr double kMinArea = 1e-6;      // контур с меньшей площадью — вырожденный
constexpr double kDefaultSnap = 1.0;   // прилипание ввода к существующей геометрии

// ---------------------------------------------------------------- предикаты
// Точный знак cross(b - a, c - a): +1 — c слева от a->b, -1 — справа, 0 — на прямой.
int orient(Vec2 a, Vec2 b, Vec2 c);
// Приближённое значение того же определителя (удвоенная площадь треугольника).
inline double cross3(Vec2 a, Vec2 b, Vec2 c) { return (b - a).cross(c - a); }

enum class SegRel : u8 {
  None,     // общих точек нет
  Touch,    // ровно одна общая точка, и это конец хотя бы одного отрезка
  Proper,   // пересечение во внутренних точках обоих отрезков
  Overlap,  // коллинеарны и имеют общую часть положительной длины
};
// Точная классификация взаимного положения замкнутых отрезков ab и cd.
SegRel segRelation(Vec2 a, Vec2 b, Vec2 c, Vec2 d);
// Точно: p лежит на замкнутом отрезке ab.
bool onSegment(Vec2 p, Vec2 a, Vec2 b);
// Точка пересечения прямых ab и cd (для непараллельных); t, u — параметры на ab и cd, ограничены [0, 1].
Vec2 crossPoint(Vec2 a, Vec2 b, Vec2 c, Vec2 d, double* t = nullptr, double* u = nullptr);
// Квадрат расстояния между отрезками (0 при пересечении).
double segSegDist2(Vec2 a, Vec2 b, Vec2 c, Vec2 d);
// Пересечение с допуском: отрезки ближе eps друг к другу.
inline bool segmentsNear(Vec2 a, Vec2 b, Vec2 c, Vec2 d, double eps) { return segSegDist2(a, b, c, d) <= eps * eps; }

// Проекция точки на отрезок: параметр t ∈ [0, 1], точка, квадрат расстояния.
struct Proj { double t = 0; Vec2 p; double d2 = 0; };
Proj project(Vec2 p, Vec2 a, Vec2 b);
double distToSeg2(Vec2 p, Vec2 a, Vec2 b);
inline double distToSeg(Vec2 p, Vec2 a, Vec2 b) { return std::sqrt(distToSeg2(p, a, b)); }

inline bool finite(Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }

// ---------------------------------------------------------------- кольца и многоугольники
// Кольцо — замкнутая ломаная без повторения первой точки в конце.
double signedArea(const std::vector<Vec2>& ring);                  // > 0 — против часовой (математически)
double polygonArea(const std::vector<std::vector<Vec2>>& rings);   // |внешний| − Σ|дыр|
Box2 bounds(const std::vector<Vec2>& pts);
Vec2 ringCentroid(const std::vector<Vec2>& ring);                  // центр масс площади (вырожденное — среднее)
// Чёт-нечет; луч вправо, полуоткрытое правило по y (точки на границе — произвольно, но согласованно).
bool pointInRing(Vec2 p, const std::vector<Vec2>& ring);
bool pointInPolygon(Vec2 p, const std::vector<std::vector<Vec2>>& rings);   // [0] внешний, далее дыры
double distToRings(Vec2 p, const std::vector<std::vector<Vec2>>& rings);
// Знаковое расстояние до границы: > 0 внутри, < 0 снаружи.
double signedDist(Vec2 p, const std::vector<std::vector<Vec2>>& rings);
// Ломаная (или кольцо) без самопересечений и самокасаний (точно). Соседние звенья могут касаться только общей вершиной.
bool isSimple(const std::vector<Vec2>& pts, bool closed);

// ---------------------------------------------------------------- полюс недоступности (polylabel)
// Точка внутри многоугольника с дырами, наиболее удалённая от границы, с точностью precision.
// dist — найденное расстояние до границы (≤ 0, если многоугольник вырожден).
Vec2 polylabel(const std::vector<std::vector<Vec2>>& rings, double precision = 1.0, double* dist = nullptr);

// ---------------------------------------------------------------- упрощение
// Дуглас — Пекер. Концы ломаной сохраняются; для кольца (closed) — не менее трёх точек.
std::vector<Vec2> simplifyDP(const std::vector<Vec2>& pts, double tol, bool closed = false);
// Упрощение набора линий с сохранением топологии: концы линий неподвижны, новые пересечения и касания
// не появляются, ни одна оставшаяся вершина не переходит на другую сторону упрощённого звена.
// Исходный набор должен быть корректным (линии не пересекаются, кольца простые).
std::vector<std::vector<Vec2>> simplifyTopo(const std::vector<std::vector<Vec2>>& lines,
                                            const std::vector<bool>& closed, double tol);

}  // namespace rg::geo
