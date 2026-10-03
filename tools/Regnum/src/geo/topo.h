// Regnum — плоский граф провинций: грани, поиск, проверка целостности.
//
// Граф хранится в World::nodes и World::edges. Дуга (Edge) — полилиния a -> pts -> b.
// Стороны дуги помечены провинцией (pl/pr) и рельефом (tl/tr). Грань — замкнутая область,
// ограниченная дугами; провинция = набор граней. Внешняя рамка карты — дуги Frame.
// Береговые дуги (Coast) берутся из базовой карты (geo::initFromCoast) и не редактируются.
#pragma once
#include <unordered_map>

#include "core/world.h"

namespace rg::geo {

struct HalfEdge { Id edge = 0; bool forward = true; };  // forward: обход a->b

struct Face {
  Id province = 0;                               // 0 — не назначено
  Terrain terrain = Terrain::Land;
  double area = 0;
  Box2 box;
  std::vector<std::vector<Vec2>> rings;          // [0] — внешний контур, далее дыры
  std::vector<std::vector<HalfEdge>> ringEdges;  // полурёбра колец в порядке обхода
  Vec2 label;                                    // точка внутри (полюс недоступности)
};

struct ProvinceShape {
  std::vector<int> faces;
  double area = 0;
  Box2 box;
  Vec2 label;                                    // точка подписи: полюс недоступности крупнейшей грани
};

struct FaceSet {
  std::vector<Face> faces;
  std::unordered_map<Id, ProvinceShape> provinces;
  Box2 bounds;

  int locate(Vec2 p) const;                      // индекс грани или -1
  Id provinceAt(Vec2 p) const;                   // 0, если не назначено или вне карты
  Terrain terrainAt(Vec2 p) const;               // None — вне карты
  const ProvinceShape* shape(Id province) const;
  // Провинции, через которые проходит полилиния (включая начало и конец), без повторов, по порядку первого входа.
  std::vector<Id> provincesOnPolyline(const std::vector<Vec2>& line) const;
  // Пары соседних провинций (общая дуга), a < b.
  std::vector<std::pair<Id, Id>> neighbors() const;

  // Внутреннее ускорение поиска (заполняется при построении).
  struct Index;
  std::shared_ptr<const Index> index;
};

// Построить грани (чистая функция).
std::shared_ptr<const FaceSet> buildFaces(const World& w);
// Кеш граней по тождеству таблиц nodes/edges. Потокобезопасно.
// Внутри транзакции tx.w() меняется на месте: после правки геометрии в той же транзакции используйте buildFaces.
// (Запись кеша сверяется и с числом узлов и дуг, так что промежуточное состояние не подменит итог операций,
// меняющих граф; перемещение ручки в той же транзакции после faces(tx.w()) кеш не заметит.)
std::shared_ptr<const FaceSet> faces(const World& w);

// Координаты дуги целиком: a, pts..., b.
std::vector<Vec2> edgeCoords(const World& w, const Edge& e);

struct Issue { std::string code; std::string msg; Vec2 at; };
// Проверка графа: пересечения дуг, нулевые отрезки, висячие узлы, несогласованные метки сторон,
// дубли дуг, ссылки на несуществующие узлы/провинции. Пустой результат — граф корректен.
std::vector<Issue> validate(const World& w);

}  // namespace rg::geo
