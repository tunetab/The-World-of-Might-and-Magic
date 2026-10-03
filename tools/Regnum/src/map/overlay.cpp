// Regnum — наложения карты в каждом кадре: наведение, маршруты, диаграммы гильдий, столицы и штабы,
// войска и флот (фигурки и стопки наложившихся объектов со значками численности), выделение.
#include "gfx/figures.h"
#include "gfx/text.h"
#include "map/map_internal.h"

namespace rg::map::detail {

namespace {

const Color kGold = Color::hex(0xf2c14e);

gfx::Stroke roundStroke(float w) {
  gfx::Stroke s;
  s.width = w;
  s.join = gfx::Join::Round;
  s.cap = gfx::Cap::Round;
  return s;
}

// Обход знаков (столицы, диаграммы, штабы) — общий для препятствий подписей и отрисовки.
// fn(kind, provinceOrFaction, center, size): kind 0 — столица, 1 — диаграмма (size — радиус), 2 — штаб.
template <class F>
void eachMarker(const FrameCtx& f, bool capitals, bool hqs, bool pies, F&& fn) {
  const World& w = *f.w;
  const geo::FaceSet& fs = *f.fs;
  const float dpi = f.dpi;
  const float pr = pieRadius(f.zoom) * dpi;
  if (pies || hqs) {
    // По возрастанию ID провинций — порядок наложения не зависит от хеш-таблиц.
    w.provinces.each([&](const Province& p) {
      if (p.sea) return;
      const geo::ProvinceShape* sh = fs.shape(p.id);
      if (!sh || !f.visible.inflated(sh->box.w()).contains(sh->label)) return;
      const bool pie = pies && !p.influence.empty() && pieVisible(f, p.id);
      const gfx::Pt c = f.dev(sh->label);
      if (pie) fn(1, p.id, c, pr);
      if (hqs && !p.hqs.empty() && pieVisible(f, p.id)) {
        // Штабы — в ряд справа от диаграммы (без диаграммы — по центру точки подписи).
        const float hs = float(clamp(f.zoom * 44, 14.0, 22.0)) * dpi, step = hs + 1.5f * dpi;
        const float x0 = pie ? c.x + pr + 3 * dpi + hs * 0.5f : c.x - step * float(p.hqs.size() - 1) * 0.5f;
        for (size_t i = 0; i < p.hqs.size(); i++) fn(2, p.hqs[i], gfx::Pt(x0 + float(i) * step, c.y), hs);
      }
    });
  }
  if (capitals) {
    w.factions.each([&](const Faction& s) {
      if (!s.isState() || !s.capital) return;
      const Province* p = w.province(s.capital);
      const geo::ProvinceShape* sh = fs.shape(s.capital);
      if (!p || !sh || p->owner != s.id) return;
      if (!f.visible.inflated(200).contains(sh->label)) return;
      const float cs = float(clamp(f.zoom * 48, 15.0, 22.0)) * dpi;
      gfx::Pt c = f.dev(sh->label);
      if (pies && !p->influence.empty() && pieVisible(f, s.capital)) c.y -= pr + cs * 0.65f;   // над диаграммой
      fn(0, s.id, c, cs);
    });
  }
}

}  // namespace

float pieRadius(double zoom) { return float(clamp(zoom * 38, 9.0, 21.0)); }

bool pieVisible(const FrameCtx& f, Id province) {
  const geo::ProvinceShape* sh = f.fs->shape(province);
  if (!sh) return false;
  const double r = pieRadius(f.zoom);
  return sh->box.w() * f.zoom >= 3.2 * r && sh->box.h() * f.zoom >= 2.6 * r;
}

i64 armyCount(const Army& a) {
  i64 n = 0;
  for (const ArmyGroup& g : a.groups)
    for (const ArmyUnit& u : g.units) n += u.count;
  return n;
}

std::string compactCount(i64 n) {
  if (n < 0) n = 0;
  auto dec = [](double v, const char* suf) {
    const double r = std::round(v * 10) / 10;
    if (r >= 10 || std::fabs(r - std::round(r)) < 1e-9) return std::to_string(i64(std::round(v))) + suf;
    std::string s = strf("%.1f", r);
    std::replace(s.begin(), s.end(), '.', ',');
    return s + suf;
  };
  if (n < 1000) return std::to_string(n);
  if (n < 1000000) return dec(double(n) / 1000, "К");
  return dec(double(n) / 1e6, "М");
}

Color routeColor(const World& w, const Route& r) {
  if (r.color) return r.color->withA(255);
  if (const Faction* g = w.faction(r.guild)) return g->color;
  return Color::hex(0xd9a441);
}

std::vector<Vec2> routeLine(const std::vector<Vec2>& pts) {
  std::vector<Vec2> out;
  out.reserve(pts.size());
  for (Vec2 p : pts)
    if (out.empty() || dist(out.back(), p) > 1e-9) out.push_back(p);
  return out;
}

void provincePath(const FrameCtx& f, Id province, gfx::Path& out) {
  out.clear();
  const geo::ProvinceShape* sh = f.fs->shape(province);
  if (!sh) return;
  for (int fi : sh->faces)
    for (const auto& ring : f.fs->faces[size_t(fi)].rings) {
      if (ring.size() < 3) continue;
      for (size_t i = 0; i < ring.size(); i++) {
        const gfx::Pt p = f.dev(ring[i]);
        if (i == 0) out.moveTo(p.x, p.y);
        else out.lineTo(p.x, p.y);
      }
      out.close();
    }
}

// ================================================================ наведение
void drawHover(const FrameCtx& f) {
  const RenderOptions& o = *f.opt;
  if (!o.hoverProvince || o.hoverProvince == o.selProvince) return;
  const geo::ProvinceShape* sh = f.fs->shape(o.hoverProvince);
  if (!sh || !sh->box.intersects(f.visible)) return;
  gfx::Path p;
  provincePath(f, o.hoverProvince, p);
  const Province* pr = f.w->province(o.hoverProvince);
  const bool sea = pr && pr->sea;
  f.c->fillPath(p, sea ? Color(255, 255, 255, 34) : Color(255, 255, 255, 46), gfx::FillRule::EvenOdd);
  f.c->strokePath(p, roundStroke(1.4f * f.dpi), Color(255, 255, 255, 150));
}

// ================================================================ маршруты
void drawRoutes(const FrameCtx& f) {
  const World& w = *f.w;
  const RenderOptions& o = *f.opt;
  const bool all = o.mode == schema::MapMode::Guilds || o.mode == schema::MapMode::Trade;
  const float dpi = f.dpi;
  const float scale = float(clamp(f.zoom * 3.2, 0.8, 1.35));
  std::vector<const Route*> list;
  w.routes.each([&](const Route& r) {
    if (all || r.id == o.selRoute) list.push_back(&r);
  });
  // выбранный — последним (поверх)
  std::stable_sort(list.begin(), list.end(), [&](const Route* a, const Route* b) { return (a->id == o.selRoute) < (b->id == o.selRoute); });
  for (const Route* r : list) {
    if (r->pts.size() < 2) continue;
    Box2 rb;
    for (Vec2 p : r->pts) rb.add(p);
    if (!rb.inflated(40).intersects(f.visible)) continue;
    // Ровно по ломаной маршрута (как считаются его провинции); изломы скруглены обводкой.
    const std::vector<Vec2> line = routeLine(r->pts);
    if (line.size() < 2) continue;
    std::vector<gfx::Pt> pts;
    pts.reserve(line.size());
    for (Vec2 p : line) pts.push_back(f.dev(p));
    gfx::Path path;
    path.addPolygon(pts.data(), pts.size(), false);
    const Color col = routeColor(w, *r);
    const bool sel = r->id == o.selRoute;
    if (sel) {
      f.c->strokePath(path, roundStroke(13 * dpi * scale), kGold.withA(90));
      f.c->strokePath(path, roundStroke(9.5f * dpi * scale), Color(255, 248, 225, 200));
    }
    f.c->strokePath(path, roundStroke(7 * dpi * scale), col.withA(sel ? 150 : 92));
    gfx::Stroke core = roundStroke(2.4f * dpi * scale);
    core.cap = gfx::Cap::Butt;
    core.dash = {9 * dpi * scale, 6 * dpi * scale};
    f.c->strokePath(path, core, Color::mix(col, Color(20, 16, 10), 0.18f).withA(240));
    // Стрелки направления вдоль пути.
    double acc = 0;
    const double stepPx = 84 * dpi * scale;
    double next = stepPx * 0.5;
    gfx::Path chev;
    const float cs = 4.6f * dpi * scale;
    for (size_t i = 1; i < pts.size(); i++) {
      const double dx = pts[i].x - pts[i - 1].x, dy = pts[i].y - pts[i - 1].y, len = std::hypot(dx, dy);
      while (len > 0 && acc + len >= next) {
        const double t = (next - acc) / len;
        const float x = float(pts[i - 1].x + dx * t), y = float(pts[i - 1].y + dy * t);
        const float ux = float(dx / len), uy = float(dy / len);
        chev.moveTo(x - ux * cs - uy * cs, y - uy * cs + ux * cs);
        chev.lineTo(x + ux * cs * 0.4f, y + uy * cs * 0.4f);
        chev.lineTo(x - ux * cs + uy * cs, y - uy * cs - ux * cs);
        next += stepPx;
      }
      acc += len;
    }
    if (!chev.empty()) {
      f.c->strokePath(chev, roundStroke(3.6f * dpi * scale), Color(255, 252, 242, 230));
      f.c->strokePath(chev, roundStroke(1.7f * dpi * scale), Color::mix(col, Color(20, 16, 10), 0.35f));
    }
    // Концы маршрута.
    for (const gfx::Pt& e : {pts.front(), pts.back()}) {
      f.c->fillCircle(e.x, e.y, 4.6f * dpi * scale, Color(255, 252, 242, 240));
      f.c->fillCircle(e.x, e.y, 3.0f * dpi * scale, col);
    }
  }
}

// ================================================================ диаграммы гильдий
void drawGuildPies(const FrameCtx& f, Obstacles&) {
  const World& w = *f.w;
  const float dpi = f.dpi;
  eachMarker(f, false, false, true, [&](int kind, Id pid, gfx::Pt c, float R) {
    if (kind != 1) return;
    const Province* p = w.province(pid);
    const float ri = R * 0.5f;
    f.c->fillCircle(c.x, c.y + 1.2f * dpi, R + 1.5f * dpi, Color(0, 0, 0, 55));
    f.c->fillCircle(c.x, c.y, R + 1.4f * dpi, Color(255, 252, 244, 245));
    double a0 = -kPi / 2, used = 0;
    auto sector = [&](double from, double to, Color col) {
      if (to - from < 1e-4) return;
      gfx::Path s;
      const int n = std::max(4, int(std::ceil((to - from) / (kPi / 40))));
      for (int i = 0; i <= n; i++) {
        const double a = from + (to - from) * i / n;
        const float x = c.x + float(std::cos(a)) * R, y = c.y + float(std::sin(a)) * R;
        if (i == 0) s.moveTo(x, y); else s.lineTo(x, y);
      }
      for (int i = n; i >= 0; i--) {
        const double a = from + (to - from) * i / n;
        s.lineTo(c.x + float(std::cos(a)) * ri, c.y + float(std::sin(a)) * ri);
      }
      s.close();
      f.c->fillPath(s, col);
    };
    for (const Influence& inf : p->influence) {
      if (inf.pct <= 0) continue;
      const Faction* g = w.faction(inf.guild);
      const double sweep = clamp(inf.pct, 0.0, 100.0 - used) / 100 * 2 * kPi;
      sector(a0, a0 + sweep, g ? g->color : fallbackColor(inf.guild));
      a0 += sweep;
      used += inf.pct;
    }
    sector(a0, -kPi / 2 + 2 * kPi, Color(168, 163, 154, 235));
    // разделители долей
    f.c->strokeCircle(c.x, c.y, R, 0.9f * dpi, Color(255, 252, 244, 200));
    f.c->strokeCircle(c.x, c.y, ri, 0.9f * dpi, Color(255, 252, 244, 200));
  });
}

void collectMarkerObstacles(const FrameCtx& f, Obstacles& obs, bool capitals, bool hqs, bool pies) {
  eachMarker(f, capitals, hqs, pies, [&](int kind, Id, gfx::Pt c, float s) {
    const float r = kind == 1 ? s + 2 * f.dpi : s * 0.55f;
    obs.add(gfx::RectI(int(c.x - r), int(c.y - r), int(2 * r), int(2 * r)));
  });
}

void drawMarkers(const FrameCtx& f, Obstacles&, bool capitals, bool hqs) {
  const World& w = *f.w;
  eachMarker(f, capitals, hqs, f.opt->mode == schema::MapMode::Guilds, [&](int kind, Id id, gfx::Pt c, float s) {
    if (kind == 0) {
      const Faction* st = w.faction(id);
      gfx::drawCapitalMarker(*f.c, c, s, st ? st->color : fallbackColor(id));
    } else if (kind == 2) {
      const Faction* g = w.faction(id);
      gfx::drawHqMarker(*f.c, c, s, g ? g->color : fallbackColor(id));
    }
  });
}

// ================================================================ войска и флот
// Отметки раскладки f.marks (marks.cpp): одиночные фигурки и стопки наложившихся объектов.
void collectArmyObstacles(const FrameCtx& f, Obstacles& obs) {
  if (!f.marks) return;
  const float dpi = f.dpi;
  for (const MarkLayout::Mark& m : f.marks->marks) {
    const gfx::Pt c = f.dev(m.pos);
    const RectF r(c.x + m.rel.x * dpi, c.y + m.rel.y * dpi, m.rel.w * dpi, m.rel.h * dpi);
    if (r.right() < f.clip.x || r.x > f.clip.right() || r.bottom() < f.clip.y || r.y > f.clip.bottom()) continue;
    obs.add(gfx::RectI(int(std::floor(r.x)), int(std::floor(r.y)), int(std::ceil(r.w)) + 1, int(std::ceil(r.h)) + 1));
  }
}

namespace {

void drawFigure(const FrameCtx& f, const Army& a, gfx::Pt c, float size, bool sel) {
  const World& w = *f.w;
  const Faction* f1 = w.faction(a.leader());
  const Color col = f1 ? f1->color : fallbackColor(a.leader());
  Color col2(0, 0, 0, 0);
  if (a.allied())
    if (const Faction* f2 = w.faction(a.groups[1].faction)) col2 = f2->color;
  if (a.isFleet()) gfx::drawFleetFigure(*f.c, c, size, col, sel, a.allied(), col2);
  else gfx::drawArmyFigure(*f.c, c, size, col, sel, a.allied(), col2);
}

Color leaderColor(const World& w, const Army& a) {
  const Faction* f1 = w.faction(a.leader());
  return f1 ? f1->color : fallbackColor(a.leader());
}

}  // namespace

void drawArmies(const FrameCtx& f) {
  if (!f.marks) return;
  const World& w = *f.w;
  const RenderOptions& o = *f.opt;
  const float dpi = f.dpi;
  const float size = f.marks->figure * dpi;
  const gfx::TextStyle ts = markBadgeStyle(dpi);
  const gfx::FontMetrics fm = gfx::metrics(ts);
  for (const MarkLayout::Mark& m : f.marks->marks) {
    const Army* top = w.army(m.members.front());
    if (!top) continue;
    const gfx::Pt c = f.dev(m.pos);
    const RectF bounds(c.x + m.rel.x * dpi, c.y + m.rel.y * dpi, m.rel.w * dpi, m.rel.h * dpi);
    if (bounds.right() < f.clip.x || bounds.x > f.clip.right() || bounds.bottom() < f.clip.y || bounds.y > f.clip.bottom()) continue;
    const Color col = leaderColor(w, *top);
    const bool sel = top->id == o.selArmy;
    // Стопка: за верхней фигуркой — фигурки других объектов, по возможности других фракций (их цвета).
    const int depth = stackDepth(m.members.size());
    if (depth > 0) {
      std::vector<const Army*> back;
      std::vector<Id> used{top->leader()};
      for (size_t i = 1; i < m.members.size() && int(back.size()) < depth; i++) {
        const Army* a = w.army(m.members[i]);
        if (a && std::find(used.begin(), used.end(), a->leader()) == used.end()) {
          back.push_back(a);
          used.push_back(a->leader());
        }
      }
      for (size_t i = 1; i < m.members.size() && int(back.size()) < depth; i++) {
        const Army* a = w.army(m.members[i]);
        if (a && std::find(back.begin(), back.end(), a) == back.end()) back.push_back(a);
      }
      for (int k = int(back.size()); k >= 1; k--) {
        const gfx::Pt off = stackOffset(k, size);
        drawFigure(f, *back[size_t(k - 1)], gfx::Pt(c.x + off.x, c.y + off.y), size, false);
      }
    }
    if (top->id == o.hoverArmy && !sel) f.c->fillCircle(c.x, c.y, size * 0.62f, Color(255, 255, 255, 70));
    drawFigure(f, *top, c, size, sel);
    // Значок численности (у стопки — всех её объектов).
    const std::string txt = compactCount(m.units);
    const RectF br = unitBadgeRect(c, size, txt, dpi);
    const float tw = gfx::measureText(txt, ts);
    f.c->boxShadow(br, br.h * 0.5f, 4 * dpi, 0, Color(0, 0, 0, 80), gfx::Pt(0, 1 * dpi));
    f.c->fillRoundRect(br, br.h * 0.5f, Color(20, 24, 31, 238));
    f.c->strokeRoundRect(br.inset(0.5f * dpi), br.h * 0.5f, 1.0f * dpi, col.withA(230));
    gfx::drawText(*f.c, txt, ts, std::round(br.x + (br.w - tw) * 0.5f), std::round(br.y + (br.h - fm.lineHeight) * 0.5f), Color(244, 240, 230));
    // Значок стопки: число объектов на золотом круге.
    if (m.members.size() > 1) {
      const std::string ct = countBadgeText(m.members.size());
      const RectF cb = countBadgeRect(c, size, ct, dpi);
      const float cw = gfx::measureText(ct, ts);
      f.c->boxShadow(cb, cb.h * 0.5f, 4 * dpi, 0, Color(0, 0, 0, 90), gfx::Pt(0, 1 * dpi));
      f.c->fillRoundRect(cb, cb.h * 0.5f, kGold);
      f.c->strokeRoundRect(cb.inset(0.5f * dpi), cb.h * 0.5f, 1.2f * dpi, Color(70, 46, 8, 230));
      gfx::drawText(*f.c, ct, ts, std::round(cb.x + (cb.w - cw) * 0.5f), std::round(cb.y + (cb.h - fm.lineHeight) * 0.5f), Color(38, 26, 6));
    }
  }
}

// ================================================================ выделение
void drawSelection(const FrameCtx& f) {
  const World& w = *f.w;
  const RenderOptions& o = *f.opt;
  const float dpi = f.dpi;
  if (o.selFaction) {
    const Faction* fac = w.faction(o.selFaction);
    std::vector<Id> provs;
    if (fac) {
      w.provinces.each([&](const Province& p) {
        if (fac->isState() ? p.owner == fac->id : std::find(p.hqs.begin(), p.hqs.end(), fac->id) != p.hqs.end()) provs.push_back(p.id);
      });
    }
    std::sort(provs.begin(), provs.end());
    auto in = [&](Id p) { return p && std::binary_search(provs.begin(), provs.end(), p); };
    auto g = geoIndex(w);
    gfx::Path outer, inner;
    std::vector<u32> seen;
    for (Id pid : provs) {
      auto it = g->provEdges.find(pid);
      if (it == g->provEdges.end()) continue;
      for (u32 ei : it->second) seen.push_back(ei);
    }
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    const Box2 vb = f.visible.inflated(20 / f.zoom);
    for (u32 ei : seen) {
      const EdgeGeo& e = g->edges[ei];
      if (!e.box.intersects(vb)) continue;
      gfx::Path& dst = in(e.pl) && in(e.pr) ? inner : outer;
      clipPolyline(e.pts, vb, [&](const Vec2* p, size_t n) {
        for (size_t i = 0; i < n; i++) {
          const gfx::Pt q = f.dev(p[i]);
          if (i == 0) dst.moveTo(q.x, q.y); else dst.lineTo(q.x, q.y);
        }
      });
    }
    if (!inner.empty()) f.c->strokePath(inner, roundStroke(1.2f * dpi), kGold.withA(150));
    if (!outer.empty()) {
      f.c->strokePath(outer, roundStroke(11 * dpi), kGold.withA(50));
      f.c->strokePath(outer, roundStroke(6 * dpi), kGold.withA(90));
      f.c->strokePath(outer, roundStroke(3.2f * dpi), Color(120, 78, 10, 150));
      f.c->strokePath(outer, roundStroke(2.2f * dpi), Color(255, 210, 96));
    }
  }
  if (o.selProvince) {
    const geo::ProvinceShape* sh = f.fs->shape(o.selProvince);
    if (sh && sh->box.inflated(20 / f.zoom).intersects(f.visible)) {
      gfx::Path p;
      provincePath(f, o.selProvince, p);
      f.c->fillPath(p, Color(255, 236, 170, 46), gfx::FillRule::EvenOdd);
      f.c->strokePath(p, roundStroke(12 * dpi), kGold.withA(46));
      f.c->strokePath(p, roundStroke(6.5f * dpi), kGold.withA(96));
      f.c->strokePath(p, roundStroke(3.2f * dpi), Color(120, 78, 10, 150));
      f.c->strokePath(p, roundStroke(2.2f * dpi), Color(255, 210, 96));
    }
  }
  if (o.hoverProvince && o.hoverProvince != o.selProvince && f.opt->editBorders) {
    gfx::Path p;
    provincePath(f, o.hoverProvince, p);
    f.c->strokePath(p, roundStroke(1.6f * dpi), Color(255, 255, 255, 200));
  }
}

}  // namespace rg::map::detail
