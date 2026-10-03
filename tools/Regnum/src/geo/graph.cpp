// Regnum — изменяемая копия графа, построение граней (полурёбра) и полная проверка целостности.
#include "geo/graph.h"

#include <unordered_map>

namespace rg::geo::detail {

// ================================================================ Graph
void Graph::load(const World& w) {
  nodes.clear();
  edges.clear();
  nodes.reserve(w.nodes.size());
  edges.reserve(w.edges.size());
  std::unordered_map<Id, int> idx;
  idx.reserve(size_t(w.nodes.size()) * 2 + 1);
  w.nodes.each([&](const Node& n) {
    GNode g;
    g.id = n.id;
    g.p = n.p;
    idx[n.id] = int(nodes.size());
    nodes.push_back(g);
  });
  w.edges.each([&](const Edge& e) {
    GEdge g;
    g.id = e.id;
    auto ia = idx.find(e.a), ib = idx.find(e.b);
    g.a = ia == idx.end() ? -1 : ia->second;
    g.b = ib == idx.end() ? -1 : ib->second;
    g.pts = e.pts;
    g.kind = e.kind;
    g.pl = e.pl;
    g.pr = e.pr;
    g.tl = e.tl;
    g.tr = e.tr;
    edges.push_back(std::move(g));
  });
}

void Graph::store(Tx& tx) const {
  std::vector<Id> nid(nodes.size(), 0);
  for (size_t i = 0; i < nodes.size(); i++) {
    const GNode& n = nodes[i];
    if (n.id == 0) {
      if (n.alive) nid[i] = tx.add(Node{0, n.p}).id;
    } else if (!n.alive) {
      tx.eraseNode(n.id);
    } else {
      nid[i] = n.id;
      const Node* cur = tx.w().nodes.get(n.id);
      if (!cur || cur->p != n.p) tx.node(n.id).p = n.p;
    }
  }
  for (const GEdge& e : edges) {
    if (e.id != 0 && !e.alive) {
      tx.eraseEdge(e.id);
      continue;
    }
    if (!e.alive || e.a < 0 || e.b < 0) continue;
    Edge x;
    x.id = e.id;
    x.a = nid[size_t(e.a)];
    x.b = nid[size_t(e.b)];
    x.pts = e.pts;
    x.kind = e.kind;
    x.pl = e.pl;
    x.pr = e.pr;
    x.tl = e.tl;
    x.tr = e.tr;
    if (e.id == 0) {
      tx.add(std::move(x));
      continue;
    }
    const Edge* cur = tx.w().edges.get(e.id);
    if (cur && cur->a == x.a && cur->b == x.b && cur->pts == x.pts && cur->kind == x.kind && cur->pl == x.pl &&
        cur->pr == x.pr && cur->tl == x.tl && cur->tr == x.tr)
      continue;
    tx.edge(e.id) = std::move(x);
  }
}

std::vector<Vec2> Graph::coords(int e) const {
  const GEdge& E = edges[size_t(e)];
  std::vector<Vec2> c;
  c.reserve(E.pts.size() + 2);
  c.push_back(nodes[size_t(E.a)].p);
  c.insert(c.end(), E.pts.begin(), E.pts.end());
  c.push_back(nodes[size_t(E.b)].p);
  return c;
}

Box2 Graph::frameBox() const {
  Box2 b;
  for (int e = 0; e < int(edges.size()); e++) {
    if (!valid(e) || edges[size_t(e)].kind != EdgeKind::Frame) continue;
    for (int k = 0, m = edges[size_t(e)].ncoords(); k < m; k++) b.add(coord(e, k));
  }
  return b;
}

// ================================================================ FaceInput
FaceInput FaceInput::from(const Graph& g) {
  FaceInput in;
  in.pos.resize(g.nodes.size());
  for (size_t i = 0; i < g.nodes.size(); i++) in.pos[i] = g.nodes[i].p;
  in.edges.resize(g.edges.size());
  for (size_t i = 0; i < g.edges.size(); i++) {
    const GEdge& e = g.edges[i];
    in.edges[i].pts = &e.pts;
    if (e.alive && e.a >= 0 && e.b >= 0) {
      in.edges[i].a = e.a;
      in.edges[i].b = e.b;
    }
  }
  return in;
}

bool FaceInput::usable(int e) const {
  const E& x = edges[size_t(e)];
  if (x.a < 0 || x.b < 0 || !x.pts) return false;
  Vec2 o = pos[size_t(x.a)];
  if (pos[size_t(x.b)] != o) return true;
  for (auto& p : *x.pts)
    if (p != o) return true;
  return false;
}

// ================================================================ Locator
int Locator::firstHit(Vec2 p, bool& leftSide, const std::vector<int>* comp, int skipComp) const {
  int best = -1;
  double bx = kInf, bslope = 0;
  grid.rayRight(
      p,
      [&](int s) {
        if (comp && (*comp)[size_t(s)] == skipComp) return;
        Vec2 a = grid.a(s), b = grid.b(s);
        bool ua = a.y > p.y, ub = b.y > p.y;
        if (ua == ub) return;
        double x;
        if (a.y == p.y) x = a.x;
        else if (b.y == p.y) x = b.x;
        else x = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
        if (x < p.x) return;
        // Луч мысленно сдвинут чуть выше (y + ε): при равных x раньше встречается отрезок с меньшим dx/dy.
        double slope = (b.x - a.x) / (b.y - a.y);
        if (x < bx || (x == bx && (slope < bslope || (slope == bslope && s < best)))) {
          bx = x;
          bslope = slope;
          best = s;
        }
      },
      [&](double right) { return bx < right; });
  if (best >= 0) leftSide = grid.b(best).y - grid.a(best).y > 0;
  return best;
}

int Locator::locate(Vec2 p) const {
  if (!finite(p)) return -1;
  bool ls = false;
  int s = firstHit(p, ls);
  if (s < 0) return -1;
  return ls ? left[size_t(s)] : right[size_t(s)];
}

// ================================================================ построение граней
namespace {

struct DSU {
  std::vector<int> p;
  explicit DSU(int n) : p(size_t(n)) { for (int i = 0; i < n; i++) p[size_t(i)] = i; }
  int find(int x) {
    while (p[size_t(x)] != x) { p[size_t(x)] = p[size_t(p[size_t(x)])]; x = p[size_t(x)]; }
    return x;
  }
  void unite(int a, int b) { a = find(a); b = find(b); if (a != b) p[size_t(std::max(a, b))] = std::min(a, b); }
};

// Координаты полуребра от начала, без последней точки (она — начало следующего полуребра цикла).
template <class F> void halfCoords(const FaceInput& in, int h, F&& f) {
  int e = h >> 1, m = in.ncoords(e);
  if ((h & 1) == 0)
    for (int k = 0; k < m - 1; k++) f(in.coord(e, k));
  else
    for (int k = m - 1; k > 0; k--) f(in.coord(e, k));
}

}  // namespace

std::vector<Vec2> FaceBuild::cycleCoords(const FaceInput& in, int c) const {
  std::vector<Vec2> r;
  for (int k = cycOff[size_t(c)]; k < cycOff[size_t(c) + 1]; k++) halfCoords(in, cycHalf[size_t(k)], [&](Vec2 p) { r.push_back(p); });
  return r;
}

FaceBuild buildFaceCore(const FaceInput& in) {
  FaceBuild fb;
  const int ne = int(in.edges.size()), nn = int(in.pos.size());
  fb.ne = ne;
  std::vector<char> ok(size_t(ne), 0);
  for (int e = 0; e < ne; e++) ok[size_t(e)] = in.usable(e) ? 1 : 0;

  // Исходящие полурёбра узлов (CSR) и точка направления каждого полуребра.
  std::vector<int> off(size_t(nn) + 1, 0);
  for (int e = 0; e < ne; e++) {
    if (!ok[size_t(e)]) continue;
    off[size_t(in.edges[size_t(e)].a) + 1]++;
    off[size_t(in.edges[size_t(e)].b) + 1]++;
  }
  for (int i = 0; i < nn; i++) off[size_t(i) + 1] += off[size_t(i)];
  std::vector<int> out(static_cast<size_t>(off[size_t(nn)]));
  std::vector<int> fillPos(off.begin(), off.end() - 1);
  std::vector<Vec2> dirp(static_cast<size_t>(2 * ne));
  for (int e = 0; e < ne; e++) {
    if (!ok[size_t(e)]) continue;
    const auto& E = in.edges[size_t(e)];
    out[size_t(fillPos[size_t(E.a)]++)] = 2 * e;
    out[size_t(fillPos[size_t(E.b)]++)] = 2 * e + 1;
    int m = in.ncoords(e);
    Vec2 oa = in.pos[size_t(E.a)], ob = in.pos[size_t(E.b)];
    dirp[size_t(2 * e)] = oa;
    for (int k = 1; k < m; k++) {
      Vec2 c = in.coord(e, k);
      if (c != oa) { dirp[size_t(2 * e)] = c; break; }
    }
    dirp[size_t(2 * e + 1)] = ob;
    for (int k = m - 2; k >= 0; k--) {
      Vec2 c = in.coord(e, k);
      if (c != ob) { dirp[size_t(2 * e + 1)] = c; break; }
    }
  }
  // Сортировка по углу против часовой (математически), точно.
  std::vector<int> posIn(size_t(2 * ne), -1);
  for (int v = 0; v < nn; v++) {
    int b = off[size_t(v)], e = off[size_t(v) + 1];
    if (e - b > 1) {
      Vec2 o = in.pos[size_t(v)];
      auto half = [&](Vec2 p) { return (p.y > o.y || (p.y == o.y && p.x > o.x)) ? 0 : 1; };
      std::sort(out.begin() + b, out.begin() + e, [&](int h1, int h2) {
        Vec2 p = dirp[size_t(h1)], q = dirp[size_t(h2)];
        int a1 = half(p), a2 = half(q);
        if (a1 != a2) return a1 < a2;
        int s = orient(o, p, q);
        if (s != 0) return s > 0;
        return h1 < h2;
      });
    }
    for (int k = b; k < e; k++) posIn[size_t(out[size_t(k)])] = k - b;
  }
  auto next = [&](int h) {
    int e = h >> 1;
    int v = (h & 1) ? in.edges[size_t(e)].a : in.edges[size_t(e)].b;
    int t = h ^ 1;
    int d = off[size_t(v) + 1] - off[size_t(v)];
    int k = posIn[size_t(t)];
    return out[size_t(off[size_t(v)] + (k - 1 + d) % d)];
  };

  // Циклы.
  fb.halfCycle.assign(size_t(2 * ne), -1);
  fb.cycOff.push_back(0);
  for (int h = 0; h < 2 * ne; h++) {
    if (!ok[size_t(h >> 1)] || fb.halfCycle[size_t(h)] >= 0) continue;
    int c = int(fb.cycOff.size()) - 1;
    int cur = h;
    do {
      fb.halfCycle[size_t(cur)] = c;
      fb.cycHalf.push_back(cur);
      cur = next(cur);
    } while (cur != h);
    fb.cycOff.push_back(int(fb.cycHalf.size()));
  }
  const int nc = int(fb.cycOff.size()) - 1;

  // Площади и крайние правые вершины циклов.
  fb.cycArea.assign(size_t(nc), 0);
  std::vector<Vec2> rightmost(static_cast<size_t>(nc));
  for (int c = 0; c < nc; c++) {
    bool first = true;
    Vec2 o, prev, rm;
    double s = 0;
    for (int k = fb.cycOff[size_t(c)]; k < fb.cycOff[size_t(c) + 1]; k++) {
      halfCoords(in, fb.cycHalf[size_t(k)], [&](Vec2 p) {
        if (first) { o = prev = rm = p; first = false; return; }
        s += (prev - o).cross(p - o);
        prev = p;
        if (p.x > rm.x || (p.x == rm.x && p.y > rm.y)) rm = p;
      });
    }
    fb.cycArea[size_t(c)] = s * 0.5;
    rightmost[size_t(c)] = rm;
  }

  // Компоненты связности.
  DSU dsu(std::max(nn, 1));
  for (int e = 0; e < ne; e++)
    if (ok[size_t(e)]) dsu.unite(in.edges[size_t(e)].a, in.edges[size_t(e)].b);
  std::vector<int> cycComp(static_cast<size_t>(nc));
  for (int c = 0; c < nc; c++) {
    int h = fb.cycHalf[size_t(fb.cycOff[size_t(c)])];
    cycComp[size_t(c)] = dsu.find(in.edges[size_t(h >> 1)].a);
  }

  // Грани — циклы с положительной площадью.
  fb.cycFace.assign(size_t(nc), -2);
  for (int c = 0; c < nc; c++) {
    if (fb.cycArea[size_t(c)] > 0) {
      fb.cycFace[size_t(c)] = int(fb.faceOuter.size());
      fb.faceOuter.push_back(c);
    }
  }
  const int nf = int(fb.faceOuter.size());

  // Сетка отрезков (с циклами сторон) для поиска.
  std::vector<Vec2> sa, sb;
  std::vector<int> segComp;
  size_t total = 0;
  for (int e = 0; e < ne; e++)
    if (ok[size_t(e)]) total += size_t(in.ncoords(e) - 1);
  sa.reserve(total);
  sb.reserve(total);
  fb.loc.left.reserve(total);
  fb.loc.right.reserve(total);
  segComp.reserve(total);
  for (int e = 0; e < ne; e++) {
    if (!ok[size_t(e)]) continue;
    int m = in.ncoords(e);
    int cl = fb.halfCycle[size_t(2 * e)], cr = fb.halfCycle[size_t(2 * e + 1)];
    int comp = dsu.find(in.edges[size_t(e)].a);
    Vec2 prev = in.coord(e, 0);
    for (int k = 1; k < m; k++) {
      Vec2 p = in.coord(e, k);
      if (p == prev) continue;
      sa.push_back(prev);
      sb.push_back(p);
      fb.loc.left.push_back(cl);
      fb.loc.right.push_back(cr);
      segComp.push_back(comp);
      prev = p;
    }
  }
  fb.loc.grid.build(std::move(sa), std::move(sb), {}, 0, 2.5);

  // Дыры: внешний цикл компоненты относится к грани, в которой лежит его крайняя правая вершина. Луч вправо от неё
  // упирается в отрезок другой компоненты; если сторона этого отрезка — тоже внешний цикл компоненты (её дыра),
  // ответ берётся у той компоненты (цепочка идёт строго вправо и конечна).
  for (int c0 = 0; c0 < nc; c0++) {
    if (fb.cycFace[size_t(c0)] != -2) continue;
    std::vector<int> chain{c0};
    int result = -1;
    for (;;) {
      int c = chain.back();
      bool ls = false;
      int s = fb.loc.firstHit(rightmost[size_t(c)], ls, &segComp, cycComp[size_t(c)]);
      if (s < 0) break;  // снаружи всего — внешняя область
      int cc = ls ? fb.loc.left[size_t(s)] : fb.loc.right[size_t(s)];
      if (fb.cycFace[size_t(cc)] != -2) {
        result = fb.cycFace[size_t(cc)];
        break;
      }
      if (std::find(chain.begin(), chain.end(), cc) != chain.end()) break;  // защита для некорректного графа
      chain.push_back(cc);
    }
    for (int x : chain) fb.cycFace[size_t(x)] = result;
  }

  fb.faceHoles.assign(size_t(nf), {});
  fb.faceArea.assign(size_t(nf), 0);
  fb.faceBox.assign(size_t(nf), Box2());
  for (int c = 0; c < nc; c++) {
    int f = fb.cycFace[size_t(c)];
    if (f < 0) continue;
    if (fb.faceOuter[size_t(f)] != c) fb.faceHoles[size_t(f)].push_back(c);
    fb.faceArea[size_t(f)] += fb.cycArea[size_t(c)];
  }
  for (int f = 0; f < nf; f++) {
    int c = fb.faceOuter[size_t(f)];
    Box2 b;
    for (int k = fb.cycOff[size_t(c)]; k < fb.cycOff[size_t(c) + 1]; k++) halfCoords(in, fb.cycHalf[size_t(k)], [&](Vec2 p) { b.add(p); });
    fb.faceBox[size_t(f)] = b;
  }
  for (auto& v : fb.loc.left) v = fb.cycFace[size_t(v)];
  for (auto& v : fb.loc.right) v = fb.cycFace[size_t(v)];
  return fb;
}

// ================================================================ проверка
namespace {

const char* terrainName(Terrain t) { return t == Terrain::Land ? "суша" : (t == Terrain::Sea ? "море" : "нет"); }

}  // namespace

std::vector<Issue> checkGraph(const Graph& g, const std::function<bool(Id)>& provinceExists, size_t maxIssues) {
  std::vector<Issue> out;
  bool structural = false;  // грани некорректного по структуре графа не проверяются
  auto add = [&](const char* code, std::string msg, Vec2 at) {
    std::string c = code;
    if (c == "nan" || c == "node-missing" || c == "degenerate-edge" || c == "zero-length" || c == "duplicate-node" ||
        c == "crossing" || c == "touch" || c == "overlap" || c == "near")
      structural = true;
    if (out.size() < maxIssues) out.push_back(Issue{code, std::move(msg), at});
  };
  const int ne = int(g.edges.size()), nn = int(g.nodes.size());
  auto eid = [&](int e) { return "#" + std::to_string(g.edges[size_t(e)].id); };

  // 1) ссылки, конечность координат, вырожденность, метки
  std::vector<char> ok(size_t(ne), 0);
  for (int i = 0; i < nn; i++) {
    const GNode& n = g.nodes[size_t(i)];
    if (n.alive && !finite(n.p)) add("nan", "Узел #" + std::to_string(n.id) + ": недопустимые координаты", {});
  }
  for (int e = 0; e < ne; e++) {
    const GEdge& E = g.edges[size_t(e)];
    if (!E.alive) continue;
    if (E.a < 0 || E.b < 0 || !g.nodes[size_t(E.a)].alive || !g.nodes[size_t(E.b)].alive) {
      add("node-missing", "Дуга " + eid(e) + " ссылается на несуществующий узел", {});
      continue;
    }
    bool fin = finite(g.nodes[size_t(E.a)].p) && finite(g.nodes[size_t(E.b)].p);
    for (auto& p : E.pts) fin = fin && finite(p);
    if (!fin) {
      add("nan", "Дуга " + eid(e) + ": недопустимые координаты", {});
      continue;
    }
    if (E.a == E.b && E.pts.empty()) {
      add("degenerate-edge", "Дуга " + eid(e) + " вырождена: начало совпадает с концом", g.nodes[size_t(E.a)].p);
      continue;
    }
    ok[size_t(e)] = 1;
    for (Id p : {E.pl, E.pr})
      if (p != 0 && !provinceExists(p))
        add("province-missing", "Дуга " + eid(e) + " ссылается на несуществующую провинцию #" + std::to_string(p),
            g.coord(e, 0));
    switch (E.kind) {
      case EdgeKind::Border:
        if (E.tl != E.tr || E.tl == Terrain::None)
          add("terrain", "Граница " + eid(e) + " разделяет разный рельеф (" + terrainName(E.tl) + " / " + terrainName(E.tr) + ")",
              g.coord(e, 0));
        break;
      case EdgeKind::Coast:
        if (E.tl == E.tr || E.tl == Terrain::None || E.tr == Terrain::None)
          add("terrain", "Береговая дуга " + eid(e) + " должна разделять сушу и море", g.coord(e, 0));
        break;
      case EdgeKind::Frame:
        if ((E.tl == Terrain::None) == (E.tr == Terrain::None))
          add("terrain", "Дуга рамки " + eid(e) + ": ровно одна сторона должна быть вне карты", g.coord(e, 0));
        break;
    }
  }

  // 2) отрезки нулевой длины, степени узлов
  std::vector<int> deg(size_t(nn), 0);
  for (int e = 0; e < ne; e++) {
    if (!ok[size_t(e)]) continue;
    const GEdge& E = g.edges[size_t(e)];
    deg[size_t(E.a)]++;
    deg[size_t(E.b)]++;
    for (int k = 0, m = E.ncoords(); k + 1 < m; k++) {
      Vec2 p = g.coord(e, k), q = g.coord(e, k + 1);
      if (dist(p, q) < kTinyLen) add("zero-length", "Дуга " + eid(e) + ": отрезок нулевой длины", p);
    }
  }
  for (int i = 0; i < nn; i++) {
    const GNode& n = g.nodes[size_t(i)];
    if (n.alive && deg[size_t(i)] < 2)
      add("dangling", "Висячий узел #" + std::to_string(n.id) + " (дуг: " + std::to_string(deg[size_t(i)]) + ")", n.p);
  }
  {
    std::vector<int> order;
    for (int i = 0; i < nn; i++)
      if (g.nodes[size_t(i)].alive && finite(g.nodes[size_t(i)].p)) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](int x, int y) {
      Vec2 p = g.nodes[size_t(x)].p, q = g.nodes[size_t(y)].p;
      return p.x < q.x || (p.x == q.x && (p.y < q.y || (p.y == q.y && x < y)));
    });
    for (size_t k = 1; k < order.size(); k++)
      if (g.nodes[size_t(order[k])].p == g.nodes[size_t(order[k - 1])].p)
        add("duplicate-node", "Узлы #" + std::to_string(g.nodes[size_t(order[k - 1])].id) + " и #" +
                                  std::to_string(g.nodes[size_t(order[k])].id) + " в одной точке",
            g.nodes[size_t(order[k])].p);
  }

  // 3) пересечения, касания, наложения (точно)
  {
    std::vector<Vec2> sa, sb;
    std::vector<int> se, sk;
    for (int e = 0; e < ne; e++) {
      if (!ok[size_t(e)]) continue;
      for (int k = 0, m = g.edges[size_t(e)].ncoords(); k + 1 < m; k++) {
        Vec2 p = g.coord(e, k), q = g.coord(e, k + 1);
        if (p == q) continue;
        sa.push_back(p);
        sb.push_back(q);
        se.push_back(e);
        sk.push_back(k);
      }
    }
    auto vkey = [&](int e, int k) -> i64 {  // тождество вершины: узел или промежуточная точка дуги
      const GEdge& E = g.edges[size_t(e)];
      if (k == 0) return E.a;
      if (k == E.ncoords() - 1) return E.b;
      return -1 - ((i64(e) << 32) | i64(k));
    };
    SegGrid grid;
    grid.build(sa, sb, {}, 0, 3.0);
    std::vector<u32> stamp(sa.size(), 0);
    for (size_t i = 0; i < sa.size() && out.size() < maxIssues; i++) {
      Box2 q;
      q.add(sa[i]);
      q.add(sb[i]);
      grid.queryUnique(q, stamp, u32(i + 1), [&](int jj) {
        size_t j = size_t(jj);
        if (j <= i) return;
        SegRel r = segRelation(sa[i], sb[i], sa[j], sb[j]);
        if (r == SegRel::None) return;
        i64 ka = vkey(se[i], sk[i]), kb = vkey(se[i], sk[i] + 1), kc = vkey(se[j], sk[j]), kd = vkey(se[j], sk[j] + 1);
        bool shared = (ka == kc && sa[i] == sa[j]) || (ka == kd && sa[i] == sb[j]) || (kb == kc && sb[i] == sa[j]) ||
                      (kb == kd && sb[i] == sb[j]);
        int nshared = int(ka == kc) + int(ka == kd) + int(kb == kc) + int(kb == kd);
        if (r == SegRel::Touch && shared && nshared == 1) return;
        Vec2 at = r == SegRel::Proper ? crossPoint(sa[i], sb[i], sa[j], sb[j]) : sa[i];
        std::string who = "дуги " + eid(se[i]) + (se[i] == se[j] ? "" : " и " + eid(se[j]));
        if (r == SegRel::Overlap || nshared == 2) add("overlap", "Наложение: " + who + " идут по одной линии", at);
        else if (r == SegRel::Proper) add("crossing", "Пересечение: " + who, at);
        else add("touch", "Касание без общего узла: " + who, at);
      });
    }
    // Почти-касания: вершина ближе kNearLen к отрезку, концом которого не является (численно хрупкий граф).
    for (size_t i = 0; i < sa.size() && out.size() < maxIssues; i++) {
      for (int end = 0; end < 2; end++) {
        Vec2 v = end ? sb[i] : sa[i];
        i64 kv = vkey(se[i], sk[i] + end);
        Box2 q(v.x - kNearLen, v.y - kNearLen, v.x + kNearLen, v.y + kNearLen);
        grid.query(q, [&](int jj) {
          size_t j = size_t(jj);
          if (vkey(se[j], sk[j]) == kv || vkey(se[j], sk[j] + 1) == kv) return;
          if (v == sa[j] || v == sb[j]) return;  // совпадение вершин — уже ошибка «касание» или «дубль узла»
          if (distToSeg2(v, sa[j], sb[j]) < kNearLen * kNearLen)
            add("near", "Граница почти касается другой линии: дуги " + eid(se[i]) + " и " + eid(se[j]), v);
        });
      }
    }
  }
  if (structural || out.size() >= maxIssues) return out;

  // 4) грани: согласованность меток, внешняя область, рамка
  FaceInput in = FaceInput::from(g);
  FaceBuild fb = buildFaceCore(in);
  auto sideOfHalf = [&](int h) { return g.edges[size_t(h >> 1)].side((h & 1) == 0); };
  for (int f = 0; f < fb.nfaces(); f++) {
    bool cons = true;
    Side s = faceSide(fb, f, sideOfHalf, &cons);
    if (!cons) {
      Vec2 at;
      fb.eachHalf(f, [&](int h) {
        if (!(sideOfHalf(h) == s)) at = in.coord(h >> 1, (h & 1) ? in.ncoords(h >> 1) - 1 : 0);
      });
      add("label-mismatch", "Несогласованные метки сторон грани (провинция/рельеф различаются вдоль границы)", at);
    }
    if (s.ter == Terrain::None) add("terrain", "Грань внутри карты помечена как «вне карты»", fb.faceBox[size_t(f)].center());
  }
  int outerCycles = 0;
  for (int c = 0; c + 1 < int(fb.cycOff.size()); c++)
    if (fb.cycFace[size_t(c)] < 0) outerCycles++;
  for (int h = 0; h < 2 * ne; h++) {
    if (fb.halfCycle[size_t(h)] < 0 || fb.faceOfHalf(h) >= 0) continue;
    const GEdge& E = g.edges[size_t(h >> 1)];
    Vec2 at = g.coord(h >> 1, 0);
    if (E.kind != EdgeKind::Frame) add("outside", "Дуга " + eid(h >> 1) + " лежит вне рамки карты", at);
    else if (!(sideOfHalf(h) == Side{0, Terrain::None}))
      add("frame", "Внешняя сторона рамки " + eid(h >> 1) + " должна быть «вне карты»", at);
  }
  bool any = false, anyFrame = false;
  Box2 fbx = g.frameBox();
  std::vector<char> frameNode(size_t(nn), 0);
  std::vector<int> frameDeg(size_t(nn), 0);
  for (int e = 0; e < ne; e++) {
    if (!ok[size_t(e)]) continue;
    any = true;
    const GEdge& E = g.edges[size_t(e)];
    if (E.kind != EdgeKind::Frame) continue;
    anyFrame = true;
    frameNode[size_t(E.a)] = frameNode[size_t(E.b)] = 1;
    frameDeg[size_t(E.a)]++;
    frameDeg[size_t(E.b)]++;
    for (int k = 0, m = E.ncoords(); k + 1 < m; k++) {
      Vec2 p = g.coord(e, k), q = g.coord(e, k + 1);
      bool onX = p.x == q.x && (p.x == fbx.x0 || p.x == fbx.x1);
      bool onY = p.y == q.y && (p.y == fbx.y0 || p.y == fbx.y1);
      if (!onX && !onY) add("frame", "Дуга рамки " + eid(e) + " не лежит на границе карты", p);
    }
  }
  if (any && !anyFrame) add("frame", "Нет рамки карты", {});
  if (anyFrame) {
    if (outerCycles != 1) add("outside", "Часть графа лежит вне рамки карты или рамка не замкнута", fbx.center());
    for (int i = 0; i < nn; i++)
      if (frameNode[size_t(i)] && frameDeg[size_t(i)] != 2)
        add("frame", "Рамка карты разорвана или ветвится в узле #" + std::to_string(g.nodes[size_t(i)].id), g.nodes[size_t(i)].p);
    auto inside = [&](Vec2 p) { return p.x > fbx.x0 && p.x < fbx.x1 && p.y > fbx.y0 && p.y < fbx.y1; };
    for (int e = 0; e < ne; e++) {
      if (!ok[size_t(e)]) continue;
      const GEdge& E = g.edges[size_t(e)];
      if (E.kind == EdgeKind::Frame) continue;
      bool bad = false;
      for (auto& p : E.pts) bad = bad || !inside(p);
      for (int n : {E.a, E.b}) bad = bad || (!frameNode[size_t(n)] && !inside(g.nodes[size_t(n)].p));
      if (bad) add("outside", "Дуга " + eid(e) + " выходит на рамку или за неё вне узла рамки", g.coord(e, 0));
    }
    double total = 0;
    for (int f = 0; f < fb.nfaces(); f++) total += fb.faceArea[size_t(f)];
    double area = fbx.w() * fbx.h();
    if (std::fabs(total - area) > 1e-7 * area + 1e-6)
      add("area", "Сумма площадей граней не равна площади карты (" + std::to_string(total) + " ≠ " + std::to_string(area) + ")",
          fbx.center());
  }
  return out;
}

}  // namespace rg::geo::detail
