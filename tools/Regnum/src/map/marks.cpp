// Regnum — раскладка отметок войск и флота. ТЗ 1.c.iv: объекты на карте не наслаиваются друг на друга. Фигурка
// имеет постоянный наименьший размер на экране, поэтому при обзорном масштабе соседние объекты перекрылись бы:
// такие объекты рисуются одной отметкой-стопкой (верхняя фигурка, за ней — фигурки других объектов цветами их
// фракций, значок с числом объектов). При приближении стопка распадается на отдельные фигурки.
// Раскладка считается в логических пикселях пространства «карта × масштаб», поэтому не зависит от сдвига камеры.
#include "gfx/figures.h"
#include "map/map_internal.h"

namespace rg::map::detail {

namespace {

constexpr float kGap = 3;   // наименьший зазор между отметками, логические пиксели

RectF unite(const RectF& a, const RectF& b) {
  const float x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
  return {x0, y0, std::max(a.right(), b.right()) - x0, std::max(a.bottom(), b.bottom()) - y0};
}

}  // namespace

float figureSizeAt(double zoom) { return float(clamp(2 * schema::kObjectRadius * zoom * 0.9, 24.0, 60.0)); }

gfx::TextStyle markBadgeStyle(float dpi) {
  gfx::TextStyle ts;
  ts.size = 10.5f * dpi;
  ts.weight = gfx::FontWeight::Semibold;
  return ts;
}

int stackDepth(size_t count) { return count > 1 ? int(std::min<size_t>(count - 1, 2)) : 0; }

gfx::Pt stackOffset(int k, float size) {
  // Фигурки стопки — левее и выше верхней (значок числа объектов — справа сверху, численность — справа снизу).
  return k <= 1 ? gfx::Pt(-0.22f * size, -0.13f * size) : gfx::Pt(-0.42f * size, -0.25f * size);
}

RectF unitBadgeRect(gfx::Pt c, float size, const std::string& text, float dpi) {
  const float tw = gfx::measureText(text, markBadgeStyle(dpi));
  const float bh = std::round(14.5f * dpi), bw = std::max(bh, std::round(tw + 9 * dpi));
  return {std::round(c.x + size * 0.16f), std::round(c.y + size * 0.2f), bw, bh};
}

std::string countBadgeText(size_t count) { return count > 99 ? std::string("99+") : std::to_string(count); }

RectF countBadgeRect(gfx::Pt c, float size, const std::string& text, float dpi) {
  const float tw = gfx::measureText(text, markBadgeStyle(dpi));
  const float h = std::round(16 * dpi), w = std::max(h, std::round(tw + 9 * dpi));
  return {std::round(c.x + size * 0.46f - w * 0.5f), std::round(c.y - size * 0.56f - h * 0.5f), w, h};
}

RectF markFootprint(float size, size_t count, i64 units) {
  const gfx::Pt o(0, 0);
  RectF r = gfx::figureBounds(o, size);
  for (int k = 1; k <= stackDepth(count); k++) r = unite(r, gfx::figureBounds(stackOffset(k, size), size));
  r = unite(r, unitBadgeRect(o, size, compactCount(units), 1));
  if (count > 1) r = unite(r, countBadgeRect(o, size, countBadgeText(count), 1));
  return r.expand(1);   // округление до пикселей устройства при отрисовке
}

bool markHit(const MarkLayout::Mark& m, float size, float dx, float dy) {
  const float r = size * 0.5f;
  // Постамент фигурки (как раньше у одиночной фигурки: круг чуть выше центра).
  auto disc = [&](gfx::Pt o) {
    const float x = dx - o.x, y = dy - (o.y - r * 0.08f);
    return x * x + y * y <= r * r * 1.1f;
  };
  if (disc(gfx::Pt(0, 0))) return true;
  for (int k = 1; k <= stackDepth(m.members.size()); k++)
    if (disc(stackOffset(k, size))) return true;
  if (unitBadgeRect(gfx::Pt(0, 0), size, compactCount(m.units), 1).contains(dx, dy)) return true;
  return m.members.size() > 1 && countBadgeRect(gfx::Pt(0, 0), size, countBadgeText(m.members.size()), 1).contains(dx, dy);
}

MarkLayout layoutMarks(const std::vector<const Army*>& armies, double zoom, Id sel) {
  MarkLayout L;
  L.figure = figureSizeAt(zoom);
  const float s = L.figure;
  struct Item {
    const Army* a;
    i64 units;
  };
  std::vector<Item> items;
  items.reserve(armies.size());
  for (const Army* a : armies)
    if (a) items.push_back({a, armyCount(*a)});
  // Приоритет (он же порядок «кто сверху»): выделенный, затем многочисленный, затем меньший ID.
  std::sort(items.begin(), items.end(), [&](const Item& x, const Item& y) {
    const bool sx = sel && x.a->id == sel, sy = sel && y.a->id == sel;
    if (sx != sy) return sx;
    if (x.units != y.units) return x.units > y.units;
    return x.a->id < y.a->id;
  });
  const u32 n = u32(items.size());
  struct Node {
    std::vector<u32> members;   // индексы items по приоритету; первый — сам узел (верхний объект)
    i64 units = 0;
    RectF box;                  // габариты в пространстве «карта × масштаб»
    bool alive = true;
  };
  std::vector<Node> nodes(n);
  auto place = [&](u32 i) {
    Node& nd = nodes[i];
    const Vec2 q = items[i].a->pos * zoom;
    const RectF r = markFootprint(s, nd.members.size(), nd.units);
    nd.box = RectF(float(q.x) + r.x, float(q.y) + r.y, r.w, r.h);
  };
  for (u32 i = 0; i < n; i++) {
    nodes[i].members = {i};
    nodes[i].units = items[i].units;
    place(i);
  }
  // Слияние пересекающихся отметок: в каждом проходе пары по приоритету, каждая отметка — не больше одного
  // слияния за проход (габариты стопки растут, пересечения пересчитываются в следующем проходе).
  std::vector<u32> order;
  std::vector<std::pair<u32, u32>> pairs;
  std::vector<char> touched(n);
  for (;;) {
    order.clear();
    for (u32 i = 0; i < n; i++)
      if (nodes[i].alive) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return nodes[a].box.x != nodes[b].box.x ? nodes[a].box.x < nodes[b].box.x : a < b; });
    pairs.clear();
    for (size_t k = 0; k < order.size(); k++) {
      const RectF& a = nodes[order[k]].box;
      for (size_t m = k + 1; m < order.size(); m++) {
        const RectF& b = nodes[order[m]].box;
        if (b.x >= a.right() + kGap) break;
        if (b.y < a.bottom() + kGap && a.y < b.bottom() + kGap) pairs.push_back(std::minmax(order[k], order[m]));
      }
    }
    if (pairs.empty()) break;
    std::sort(pairs.begin(), pairs.end());
    std::fill(touched.begin(), touched.end(), 0);
    for (const auto& [a, b] : pairs) {
      if (touched[a] || touched[b]) continue;
      touched[a] = touched[b] = 1;
      Node& A = nodes[a];
      Node& B = nodes[b];
      A.members.insert(A.members.end(), B.members.begin(), B.members.end());
      std::sort(A.members.begin(), A.members.end());
      A.units += B.units;
      B.alive = false;
      B.members.clear();
      place(a);
    }
  }
  for (u32 i = 0; i < n; i++) {
    const Node& nd = nodes[i];
    if (!nd.alive) continue;
    MarkLayout::Mark m;
    m.members.reserve(nd.members.size());
    for (u32 k : nd.members) m.members.push_back(items[k].a->id);
    m.pos = items[i].a->pos;
    m.rel = markFootprint(s, nd.members.size(), nd.units);
    m.units = nd.units;
    L.marks.push_back(std::move(m));
  }
  // Порядок отрисовки: сверху вниз по экрану, затем по ID верхнего объекта.
  std::sort(L.marks.begin(), L.marks.end(), [](const MarkLayout::Mark& a, const MarkLayout::Mark& b) {
    return a.pos.y != b.pos.y ? a.pos.y < b.pos.y : a.members.front() < b.members.front();
  });
  return L;
}

}  // namespace rg::map::detail
