// Regnum — операции над провинциями (внутри транзакции Store::transact).
// Любое нарушение (пересечение, выход за карту, вырожденный контур) — rg::fail("понятное сообщение"),
// транзакция откатывается целиком.
#pragma once
#include <functional>

#include "geo/topo.h"

namespace rg::geo {

// Береговая линия базовой карты: внешние контуры участков суши (ориентация нормализуется).
// «Суша» — всё, что не море: озёра и реки внутри суши в граф не входят (это часть изображения).
// Кольца не пересекаются и не вложены друг в друга.
struct Coast {
  double width = 8000, height = 4500;
  std::vector<std::vector<Vec2>> landRings;
};

// Заполнить пустой граф: рамка карты + береговые кольца. Все грани не назначены.
void initFromCoast(Tx& tx, const Coast& coast);

// Создание новой провинции по многоугольнику (вершины по порядку, замыкание неявное).
// terrain: Land/Sea — какую часть брать (берег «прилипает»: другая часть отбрасывается);
// None — по рельефу первой вершины. Соседние провинции теряют попавшую площадь.
// Возвращает ID новой записи Province (поля по умолчанию, sea = (terrain == Sea)).
Id createProvince(Tx& tx, const std::vector<Vec2>& poly, Terrain terrain = Terrain::None);

// Расширить провинцию многоугольником (только её рельеф); соседи уменьшаются.
void addArea(Tx& tx, Id province, const std::vector<Vec2>& poly);
// Вырезать площадь провинции внутри многоугольника в «не назначено».
void removeArea(Tx& tx, Id province, const std::vector<Vec2>& poly);
// Назначить грань в точке p провинции (0 — создать новую). Возвращает ID провинции.
Id fillAt(Tx& tx, Vec2 p, Id province);

// Разрезать провинцию линией (нож) от границы до границы. Меньшая часть получает новую провинцию,
// созданную функцией makeNew(tx, исходная) (по умолчанию — пустая запись). Возвращает её ID.
// makeNew создаёт только запись провинции того же типа (sea как у исходной) и не меняет узлы и дуги;
// вызывается до разбора линии (при любом отказе транзакция откатывается вместе с записью).
// Нож может проходить через дыры и чужие провинции: режутся только грани этой провинции.
using NewProvinceFn = std::function<Id(Tx&, Id from)>;
Id split(Tx& tx, Id province, const std::vector<Vec2>& line, const NewProvinceFn& makeNew = {});

// Допуск прилипания ввода (единицы карты): вершины контура/ножа ближе snap к существующим узлам, точкам
// и границам переносятся на них, а существующие вершины ближе snap к линии ввода становятся её вершинами.
// Инструменты карты передают его в пикселях экрана, делённых на масштаб. Варианты без EditOptions — snap = 1.
struct EditOptions { double snap = 1.0; };
Id createProvince(Tx& tx, const std::vector<Vec2>& poly, Terrain terrain, const EditOptions& opt);
void addArea(Tx& tx, Id province, const std::vector<Vec2>& poly, const EditOptions& opt);
void removeArea(Tx& tx, Id province, const std::vector<Vec2>& poly, const EditOptions& opt);
Id split(Tx& tx, Id province, const std::vector<Vec2>& line, const NewProvinceFn& makeNew, const EditOptions& opt);

// Геометрия: грани source становятся target, лишние границы удаляются. Запись source не трогается.
// Провинции могут быть несмежными (острова); сухопутную с морской объединить нельзя.
void merge(Tx& tx, Id target, Id source);
// Геометрия провинции становится «не назначено» (соседние пустые грани сливаются). Запись не трогается.
// Удаление провинции: unassign + eraseProvince в одной транзакции (порядок любой). Ссылки дуг на уже удалённую
// запись тоже снимаются — так чинится граф с ошибкой province-missing.
void unassign(Tx& tx, Id province);

// ---- ручки границ (режим правки) ----
struct Handle {
  enum Kind : u8 { None, Node, Point } kind = None;
  Id node = 0;     // Node: узел графа
  Id edge = 0;     // Point: дуга и индекс промежуточной точки
  int index = -1;
  bool operator==(const Handle&) const = default;
  explicit operator bool() const { return kind != None; }
};
struct EdgeHit { Id edge = 0; int segment = 0; Vec2 p; double dist = 0; };

// Ближайшая ручка в радиусе tol (единицы карты). province != 0 — только на границе этой провинции.
Handle hitHandle(const World& w, Vec2 p, double tol, Id province = 0);
std::optional<EdgeHit> hitEdge(const World& w, Vec2 p, double tol, Id province = 0);
Vec2 handlePos(const World& w, const Handle& h);
// Заблокированные ручки: точки береговых дуг и рамки, узлы рамки.
bool handleLocked(const World& w, const Handle& h);
// Узел на береговой линии (стык границы с берегом) — двигается скольжением вдоль берега.
bool isCoastJunction(const World& w, Id node);
// Можно ли переместить ручку в точку (без пересечений и вырожденных отрезков).
bool canMove(const World& w, const Handle& h, Vec2 to);
// Перемещение: общая граница меняет обе соседние провинции.
void moveHandle(Tx& tx, const Handle& h, Vec2 to);
// Вставить точку в дугу (двойной щелчок по границе). Возвращает новую ручку.
Handle insertPoint(Tx& tx, Id edge, int segment, Vec2 p);
// Удалить промежуточную точку (или узел степени 2 между двумя пограничными дугами).
void deletePoint(Tx& tx, const Handle& h);
// Сдвинуть стык границы с берегом вдоль береговой линии к ближайшей к to точке берега.
void slideJunction(Tx& tx, Id node, Vec2 to);
// Ближайшая точка берега для скольжения (предпросмотр).
std::optional<Vec2> slideTarget(const World& w, Id node, Vec2 to);

}  // namespace rg::geo
