// Regnum — технологии: изучение, исследование, зависимости без циклов, раскладка дерева (ТЗ 1.b.v).
#include <unordered_set>

#include "rules/internal.h"

namespace rg::rules {

using namespace detail;

namespace {

std::string techList(const World& w, const std::vector<Id>& ids) {
  std::vector<std::string> n;
  for (Id id : ids) n.push_back(techName(w, id));
  return join(n, ", ");
}

}  // namespace

ResearchCheck canResearch(const World& w, Id tech) {
  ResearchCheck r;
  const Tech* t = w.tech(tech);
  if (!t) return r;
  for (Id p : t->prereqs) {
    const Tech* pt = w.tech(p);
    if (pt && !pt->studied && !contains(r.missing, p)) r.missing.push_back(p);
  }
  r.ok = !t->studied && r.missing.empty();
  return r;
}

void setStudied(Tx& tx, Id tech, bool studied) {
  const Tech& t = needTech(tx.w(), tech);
  if (studied) {
    if (t.studied) return;
    ResearchCheck c = canResearch(tx.w(), tech);
    if (!c.missing.empty()) fail("Сначала изучите: " + techList(tx.w(), c.missing));
    Id faction = t.faction;
    Tech& m = tx.tech(tech);
    m.studied = true;
    m.research = false;
    m.progress = m.turns;
    addLog(tx, LogKind::Tech, facName(tx.w(), faction) + ": изучена технология " + techName(tx.w(), tech), LogRefs{0, 0, {faction}});
    return;
  }
  if (!t.studied) return;
  std::vector<Id> deps = idsWhere(tx.w().techs, [&](const Tech& x) { return x.studied && contains(x.prereqs, tech); });
  if (!deps.empty()) fail("От неё зависят изученные технологии: " + techList(tx.w(), deps));
  Tech& m = tx.tech(tech);
  m.studied = false;
  m.research = false;
  m.progress = 0;
}

void startResearch(Tx& tx, Id tech) {
  const Tech& t = needTech(tx.w(), tech);
  if (t.studied) fail("Технология уже изучена");
  if (t.research) fail("Технология уже исследуется");
  ResearchCheck c = canResearch(tx.w(), tech);
  if (!c.missing.empty()) fail("Сначала изучите: " + techList(tx.w(), c.missing));
  Id faction = t.faction;
  int left = std::max(1, t.turns - std::max(0, t.progress));
  tx.tech(tech).research = true;
  addLog(tx, LogKind::Tech, facName(tx.w(), faction) + ": начато исследование " + techName(tx.w(), tech) + ", осталось " + nTurns(left),
         LogRefs{0, 0, {faction}});
}

void stopResearch(Tx& tx, Id tech) {
  const Tech& t = needTech(tx.w(), tech);
  if (!t.research) return;
  tx.tech(tech).research = false;  // пройденные ходы сохраняются
}

bool wouldCycle(const World& w, Id tech, Id prereq) {
  if (tech == prereq) return true;
  // Цикл, если prereq (через свои условия) уже зависит от tech.
  std::vector<Id> st{prereq};
  std::unordered_set<Id> seen{prereq};
  while (!st.empty()) {
    Id id = st.back();
    st.pop_back();
    const Tech* t = w.tech(id);
    if (!t) continue;
    for (Id p : t->prereqs) {
      if (p == tech) return true;
      if (seen.insert(p).second) st.push_back(p);
    }
  }
  return false;
}

void setPrereq(Tx& tx, Id tech, Id prereq, bool on) {
  const Tech& t = needTech(tx.w(), tech);
  const Tech& p = needTech(tx.w(), prereq);
  if (on) {
    if (tech == prereq) fail("Технология не может зависеть от самой себя");
    if (t.faction != p.faction) fail("Связывать можно только технологии одного дерева");
    if (contains(t.prereqs, prereq)) return;
    if (wouldCycle(tx.w(), tech, prereq)) fail("Связь создаст цикл зависимостей");
    // ТЗ 1.b.v: изученная технология не может зависеть от неизученной.
    if (t.studied && !p.studied)
      fail("Изученная технология " + techName(tx.w(), tech) + " не может зависеть от неизученной " + techName(tx.w(), prereq) +
           ": сначала изучите её или снимите изученность");
    tx.tech(tech).prereqs.push_back(prereq);
  } else if (contains(t.prereqs, prereq)) {
    eraseValue(tx.tech(tech).prereqs, prereq);
  }
}

// Раскладка: слои по длиннейшему пути от корней, длинные связи — через фиктивные узлы,
// порядок в слоях — барицентрические проходы вниз/вверх с выбором раскладки с наименьшим числом пересечений.
void autoLayout(Tx& tx, Id faction) {
  needFaction(tx.w(), faction);
  std::vector<const Tech*> ts;
  tx.w().techs.each([&](const Tech& t) { if (t.faction == faction) ts.push_back(&t); });
  if (ts.empty()) return;
  const int n = int(ts.size());
  std::unordered_map<Id, int> idx;
  for (int i = 0; i < n; i++) idx[ts[size_t(i)]->id] = i;
  std::vector<std::vector<int>> pre(static_cast<size_t>(n));
  for (int i = 0; i < n; i++)
    for (Id p : ts[size_t(i)]->prereqs) {
      auto it = idx.find(p);
      if (it != idx.end() && it->second != i && !contains(pre[size_t(i)], it->second)) pre[size_t(i)].push_back(it->second);
    }

  // Слой — длина длиннейшего пути от корня (итеративный обход в глубину; цикл повреждённых данных разрывается).
  std::vector<int> layer(size_t(n), -1);
  std::vector<u8> state(size_t(n), 0);
  for (int s = 0; s < n; s++) {
    if (state[size_t(s)]) continue;
    std::vector<std::pair<int, size_t>> st{{s, 0}};
    state[size_t(s)] = 1;
    while (!st.empty()) {
      auto& [v, k] = st.back();
      if (k < pre[size_t(v)].size()) {
        int u = pre[size_t(v)][k++];
        if (state[size_t(u)] == 0) {
          state[size_t(u)] = 1;
          st.push_back({u, 0});
        }
        continue;
      }
      int L = 0;
      for (int u : pre[size_t(v)])
        if (state[size_t(u)] == 2) L = std::max(L, layer[size_t(u)] + 1);
      layer[size_t(v)] = L;
      state[size_t(v)] = 2;
      st.pop_back();
    }
  }
  int maxL = 0;
  for (int l : layer) maxL = std::max(maxL, l);

  // Узлы раскладки: настоящие технологии и фиктивные точки длинных связей.
  struct Node { int layer; int tech; std::vector<int> up, down; };
  std::vector<Node> nodes;
  nodes.reserve(size_t(n) * 2);
  for (int i = 0; i < n; i++) nodes.push_back(Node{layer[size_t(i)], i, {}, {}});
  for (int i = 0; i < n; i++)
    for (int p : pre[size_t(i)]) {
      if (layer[size_t(p)] >= layer[size_t(i)]) continue;
      int prev = p;
      for (int l = layer[size_t(p)] + 1; l < layer[size_t(i)]; l++) {
        nodes.push_back(Node{l, -1, {prev}, {}});
        int d = int(nodes.size()) - 1;
        nodes[size_t(prev)].down.push_back(d);
        prev = d;
      }
      nodes[size_t(prev)].down.push_back(i);
      nodes[size_t(i)].up.push_back(prev);
    }
  std::vector<std::vector<int>> L(size_t(maxL) + 1);
  for (int v = 0; v < int(nodes.size()); v++) L[size_t(nodes[size_t(v)].layer)].push_back(v);
  std::vector<double> pos(nodes.size(), 0);
  auto renumber = [&]() {
    for (auto& layerNodes : L)
      for (size_t k = 0; k < layerNodes.size(); k++) pos[size_t(layerNodes[k])] = double(k);
  };
  renumber();

  auto crossings = [&]() {
    long long c = 0;
    std::vector<std::pair<double, double>> e;
    for (size_t l = 0; l + 1 < L.size(); l++) {
      e.clear();
      for (int u : L[l])
        for (int v : nodes[size_t(u)].down) e.push_back({pos[size_t(u)], pos[size_t(v)]});
      for (size_t i = 0; i < e.size(); i++)
        for (size_t j = i + 1; j < e.size(); j++)
          if ((e[i].first - e[j].first) * (e[i].second - e[j].second) < 0) c++;
    }
    return c;
  };
  auto sweep = [&](size_t l, bool useUp) {
    auto& layerNodes = L[l];
    std::vector<std::pair<double, int>> key;
    key.reserve(layerNodes.size());
    for (int v : layerNodes) {
      const auto& nb = useUp ? nodes[size_t(v)].up : nodes[size_t(v)].down;
      double b = pos[size_t(v)];
      if (!nb.empty()) {
        b = 0;
        for (int u : nb) b += pos[size_t(u)];
        b /= double(nb.size());
      }
      key.push_back({b, v});
    }
    std::stable_sort(key.begin(), key.end(), [&](auto& a, auto& b) {
      if (a.first != b.first) return a.first < b.first;
      return pos[size_t(a.second)] < pos[size_t(b.second)];
    });
    for (size_t k = 0; k < key.size(); k++) {
      layerNodes[k] = key[k].second;
      pos[size_t(key[k].second)] = double(k);
    }
  };
  // Пересечения связей узлов u и v (u левее v) с соседними слоями.
  auto pairCross = [&](int u, int v) {
    long long c = 0;
    for (int side = 0; side < 2; side++) {
      const auto& nu = side ? nodes[size_t(u)].down : nodes[size_t(u)].up;
      const auto& nv = side ? nodes[size_t(v)].down : nodes[size_t(v)].up;
      for (int a : nu)
        for (int b : nv) c += pos[size_t(a)] > pos[size_t(b)];
    }
    return c;
  };
  // Перестановка соседей, пока она уменьшает пересечения.
  auto transpose = [&]() {
    for (int guard = 0; guard < 8; guard++) {
      bool improved = false;
      for (auto& layerNodes : L)
        for (size_t k = 0; k + 1 < layerNodes.size(); k++) {
          int u = layerNodes[k], v = layerNodes[k + 1];
          if (pairCross(v, u) < pairCross(u, v)) {
            std::swap(layerNodes[k], layerNodes[k + 1]);
            pos[size_t(u)] = double(k + 1);
            pos[size_t(v)] = double(k);
            improved = true;
          }
        }
      if (!improved) break;
    }
  };
  auto best = L;
  long long bestC = crossings();
  for (int it = 0; it < 24 && bestC > 0; it++) {
    if (it % 2 == 0)
      for (size_t l = 1; l < L.size(); l++) sweep(l, true);
    else
      for (size_t l = L.size() - 1; l-- > 0;) sweep(l, false);
    transpose();
    long long c = crossings();
    if (c < bestC) {
      bestC = c;
      best = L;
    }
  }
  L = best;

  // Координаты: столбец — слой, строки по порядку настоящих узлов, слои центрированы.
  size_t maxReal = 0;
  for (auto& layerNodes : L) {
    size_t k = 0;
    for (int v : layerNodes) k += nodes[size_t(v)].tech >= 0;
    maxReal = std::max(maxReal, k);
  }
  for (size_t l = 0; l < L.size(); l++) {
    std::vector<int> real;
    for (int v : L[l])
      if (nodes[size_t(v)].tech >= 0) real.push_back(nodes[size_t(v)].tech);
    double off = (double(maxReal) - double(real.size())) * 0.5;
    for (size_t k = 0; k < real.size(); k++) {
      Vec2 p{double(l) * kTreeColStep, (double(k) + off) * kTreeRowStep};
      const Tech* t = ts[size_t(real[k])];
      if (t->pos != p) tx.tech(t->id).pos = p;
    }
  }
}

}  // namespace rg::rules
