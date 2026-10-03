// Regnum — внутреннее устройство модуля geo: изменяемая копия графа, построение граней, проверка.
// Не для использования вне geo (публичный интерфейс — topo.h и ops.h).
#pragma once
#include "geo/geom.h"
#include "geo/grid.h"
#include "geo/topo.h"

namespace rg::geo::detail {

// Метка стороны дуги: провинция и рельеф.
struct Side {
  Id prov = 0;
  Terrain ter = Terrain::None;
  bool operator==(const Side&) const = default;
};
inline Side sideOf(const Edge& e, bool left) { return left ? Side{e.pl, e.tl} : Side{e.pr, e.tr}; }

// ---------------------------------------------------------------- изменяемый граф
struct GNode {
  Id id = 0;          // 0 — новый (ID выдаётся при записи)
  Vec2 p;
  bool alive = true;
};
struct GEdge {
  Id id = 0;
  int a = -1, b = -1; // индексы узлов; -1 — ссылка на несуществующий узел
  std::vector<Vec2> pts;
  EdgeKind kind = EdgeKind::Border;
  Id pl = 0, pr = 0;
  Terrain tl = Terrain::Land, tr = Terrain::Land;
  bool alive = true;

  Side side(bool left) const { return left ? Side{pl, tl} : Side{pr, tr}; }
  void setSide(bool left, Side s) {
    if (left) { pl = s.prov; tl = s.ter; } else { pr = s.prov; tr = s.ter; }
  }
  int ncoords() const { return int(pts.size()) + 2; }
  void reverse() {
    std::swap(a, b);
    std::reverse(pts.begin(), pts.end());
    std::swap(pl, pr);
    std::swap(tl, tr);
  }
};

struct Graph {
  std::vector<GNode> nodes;
  std::vector<GEdge> edges;

  void load(const World& w);
  // Записать изменения в транзакцию: новые узлы и дуги получают ID, изменённые — переписываются, удалённые — стираются.
  void store(Tx& tx) const;

  Vec2 coord(int e, int k) const {
    const GEdge& E = edges[size_t(e)];
    int m = int(E.pts.size());
    return k == 0 ? nodes[size_t(E.a)].p : (k == m + 1 ? nodes[size_t(E.b)].p : E.pts[size_t(k - 1)]);
  }
  std::vector<Vec2> coords(int e) const;
  bool valid(int e) const {
    const GEdge& E = edges[size_t(e)];
    return E.alive && E.a >= 0 && E.b >= 0;
  }
  int addNode(Vec2 p) {
    GNode n;
    n.p = p;
    nodes.push_back(n);
    return int(nodes.size()) - 1;
  }
  int addEdge(GEdge e) {
    e.id = 0;
    edges.push_back(std::move(e));
    return int(edges.size()) - 1;
  }
  Box2 frameBox() const;  // габариты дуг рамки (пусто, если рамки нет)
};

// ---------------------------------------------------------------- построение граней
// Вход: позиции узлов и дуги (индексы дуг = индексы в Graph::edges или порядок таблицы World).
// Полуребро h = 2*i (по ходу a->b) или 2*i+1 (обратно). Грань полуребра лежит слева от него.
struct FaceInput {
  std::vector<Vec2> pos;
  struct E { int a = -1, b = -1; const std::vector<Vec2>* pts = nullptr; };
  std::vector<E> edges;   // a < 0 — дуга не участвует

  static FaceInput from(const Graph& g);
  int ncoords(int e) const { return int(edges[size_t(e)].pts->size()) + 2; }
  Vec2 coord(int e, int k) const {
    const E& x = edges[size_t(e)];
    int m = int(x.pts->size());
    return k == 0 ? pos[size_t(x.a)] : (k == m + 1 ? pos[size_t(x.b)] : (*x.pts)[size_t(k - 1)]);
  }
  bool usable(int e) const;  // концы существуют и дуга не вырождена в точку
};

// Поиск грани по точке: луч вправо до первого отрезка (с точным правилом для вершин на луче).
struct Locator {
  SegGrid grid;
  std::vector<int> left, right;   // грань слева/справа от отрезка (в направлении a->b дуги); -1 — внешняя область
  int locate(Vec2 p) const;
  // Первый отрезок, пересекаемый лучом вправо от p (-1 — нет), и сторона, обращённая к p (true — левая).
  int firstHit(Vec2 p, bool& leftSide, const std::vector<int>* comp = nullptr, int skipComp = -1) const;
};

struct FaceBuild {
  int ne = 0;
  std::vector<int> halfCycle;            // 2*ne; -1 — полуребро не участвует
  std::vector<int> cycOff, cycHalf;      // CSR: полурёбра каждого цикла по порядку обхода
  std::vector<double> cycArea;           // знаковая площадь цикла (> 0 — внешний контур грани)
  std::vector<int> cycFace;              // грань цикла (своя или содержащая дыру); -1 — внешняя область
  std::vector<int> faceOuter;            // цикл внешнего контура грани
  std::vector<std::vector<int>> faceHoles;
  std::vector<double> faceArea;
  std::vector<Box2> faceBox;
  Locator loc;

  int nfaces() const { return int(faceOuter.size()); }
  int faceOfHalf(int h) const {
    int c = halfCycle[size_t(h)];
    return c < 0 ? -1 : cycFace[size_t(c)];
  }
  int locate(Vec2 p) const { return loc.locate(p); }
  // Все полурёбра грани (внешний контур и дыры).
  template <class F> void eachHalf(int f, F&& fn) const {
    auto cyc = [&](int c) {
      for (int k = cycOff[size_t(c)]; k < cycOff[size_t(c) + 1]; k++) fn(cycHalf[size_t(k)]);
    };
    cyc(faceOuter[size_t(f)]);
    for (int c : faceHoles[size_t(f)]) cyc(c);
  }
  std::vector<Vec2> cycleCoords(const FaceInput& in, int c) const;
};

FaceBuild buildFaceCore(const FaceInput& in);

// Метка грани по полурёбрам (большинство; при равенстве — меньшая провинция). consistent = все совпадают.
template <class SideOfHalf>
Side faceSide(const FaceBuild& fb, int f, SideOfHalf&& sideOfHalf, bool* consistent = nullptr) {
  std::vector<std::pair<Side, int>> votes;
  fb.eachHalf(f, [&](int h) {
    Side s = sideOfHalf(h);
    for (auto& v : votes)
      if (v.first == s) { v.second++; return; }
    votes.push_back({s, 1});
  });
  if (consistent) *consistent = votes.size() <= 1;
  if (votes.empty()) return {};
  auto best = votes[0];
  for (auto& v : votes)
    if (v.second > best.second || (v.second == best.second && (v.first.prov < best.first.prov ||
        (v.first.prov == best.first.prov && int(v.first.ter) < int(best.first.ter)))))
      best = v;
  return best.first;
}

// ---------------------------------------------------------------- проверка
// Полная проверка графа (см. topo.h validate). provinceExists — есть ли запись провинции.
std::vector<Issue> checkGraph(const Graph& g, const std::function<bool(Id)>& provinceExists, size_t maxIssues = 200);

}  // namespace rg::geo::detail
