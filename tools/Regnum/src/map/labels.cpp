// Regnum — подписи карты: названия государств (шрифт с засечками, разрядка, мягкий ореол, размер по площади),
// провинций и войск; спрайты подписей кешируются, перекрывающиеся подписи скрываются.
#include "gfx/text.h"
#include "map/map_internal.h"

namespace rg::map::detail {

// ================================================================ спрайты
size_t LabelCache::H::operator()(const LabelKey& k) const {
  u64 h = hash64(k.text);
  h = hashMix(h, (u64(k.kind) << 48) | (u64(k.size4) << 32) | k.color);
  return size_t(h);
}

std::shared_ptr<const LabelSprite> LabelCache::get(const LabelKey& k) {
  auto it = map_.find(k);
  if (it != map_.end()) {
    it->second.used = ++clock_;
    return it->second.s;
  }
  auto s = renderLabel(k);
  map_[k] = E{s, ++clock_};
  return s;
}

std::shared_ptr<const LabelSprite> LabelCache::peek(const LabelKey& k, u16* nearSize) {
  auto it = map_.find(k);
  if (it != map_.end()) {
    it->second.used = ++clock_;
    if (nearSize) *nearSize = k.size4;
    return it->second.s;
  }
  if (!nearSize) return nullptr;
  // Та же подпись другого кегля (ближайшего) — временно, пока новый спрайт не готов.
  std::shared_ptr<const LabelSprite> best;
  int bd = 1 << 30;
  for (auto& [key, e] : map_) {
    if (key.kind != k.kind || key.color != k.color || key.text != k.text) continue;
    const int d = std::abs(int(key.size4) - int(k.size4));
    if (d < bd) {
      bd = d;
      best = e.s;
      *nearSize = key.size4;
      e.used = ++clock_;
    }
  }
  return best;
}

void LabelCache::trim(size_t maxItems) {
  if (map_.size() <= maxItems) return;
  std::vector<std::pair<u64, const LabelKey*>> order;
  for (const auto& [k, e] : map_) order.push_back({e.used, &k});
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<LabelKey> drop;
  for (size_t i = 0; i < map_.size() - maxItems; i++) drop.push_back(*order[i].second);
  for (const LabelKey& k : drop) map_.erase(k);
}

namespace {

// Размытие по прямоугольному окну (premultiplied, по всем каналам), радиус r, на месте.
void boxBlur(gfx::Image& img, int r) {
  if (r <= 0 || img.empty()) return;
  std::vector<u32> line(size_t(std::max(img.w, img.h)));
  const int n = 2 * r + 1;
  auto pass = [&](int count, int len, auto at) {
    for (int i = 0; i < count; i++) {
      for (int k = 0; k < len; k++) line[size_t(k)] = at(i, k);
      u32 s[4] = {0, 0, 0, 0};
      auto add = [&](u32 p, int sign) {
        for (int c = 0; c < 4; c++) s[c] = u32(int(s[c]) + sign * int((p >> (8 * c)) & 255));
      };
      for (int k = -r; k <= r; k++) if (k >= 0 && k < len) add(line[size_t(k)], 1);
      for (int k = 0; k < len; k++) {
        u32 v = 0;
        for (int c = 0; c < 4; c++) v |= u32((s[c] + u32(n / 2)) / u32(n)) << (8 * c);
        at(i, k) = v;
        if (k - r >= 0) add(line[size_t(k - r)], -1);
        if (k + r + 1 < len) add(line[size_t(k + r + 1)], 1);
      }
    }
  };
  pass(img.h, img.w, [&](int y, int x) -> u32& { return img.row(y)[x]; });
  pass(img.w, img.h, [&](int x, int y) -> u32& { return img.row(y)[x]; });
}

}  // namespace

std::shared_ptr<LabelSprite> renderLabel(const LabelKey& k) {
  auto sp = std::make_shared<LabelSprite>();
  const float size = k.size4 / 4.0f;
  gfx::TextStyle ts;
  std::string text = k.text;
  Color halo(255, 253, 246, 255);
  float haloR = std::max(1.6f, size * 0.13f), haloA = 0.82f;
  if (k.kind == 0) {
    ts.family = gfx::FontFamily::Display;
    ts.weight = gfx::FontWeight::Semibold;
    ts.letterSpacing = size * 0.16f;
    text = utf8::upper(text);
    haloR = std::max(2.0f, size * 0.16f);
    haloA = 0.7f;
  } else if (k.kind == 1) {
    ts.family = gfx::FontFamily::UI;
    ts.weight = gfx::FontWeight::Semibold;
    ts.letterSpacing = size * 0.02f;
  } else if (k.kind == 3) {  // морская провинция: светлый текст на тёмно-синем ореоле
    ts.family = gfx::FontFamily::Display;
    ts.weight = gfx::FontWeight::Regular;
    ts.letterSpacing = size * 0.08f;
    halo = Color(6, 22, 120, 255);
    haloA = 0.45f;
  } else {
    ts.family = gfx::FontFamily::UI;
    ts.weight = gfx::FontWeight::Semibold;
  }
  ts.size = size;
  const gfx::TextLayout lay = gfx::layoutText(text, ts);
  const float pad = std::ceil(haloR * 2 + 2);
  const int w = int(std::ceil(lay.width + 2 * pad)), h = int(std::ceil(lay.height + 2 * pad));
  sp->img = gfx::Image(std::max(1, w), std::max(1, h), 0);
  sp->w = lay.width;
  sp->h = gfx::metrics(ts).capHeight > 0 ? gfx::metrics(ts).capHeight : lay.height * 0.7f;
  sp->ox = pad + lay.width * 0.5f;
  const float baseline = lay.lines.empty() ? lay.ascent : lay.lines[0].baseline;
  sp->oy = pad + baseline - sp->h * 0.5f;
  // Ореол: покрытие глифов, размытое и усиленное (мягкое расширение), цветом ореола.
  const Color tc(u8(k.color >> 16), u8(k.color >> 8), u8(k.color), u8(k.color >> 24));
  gfx::Image glyphs(sp->img.w, sp->img.h, 0);
  {
    gfx::Canvas gc(glyphs);
    gfx::drawLayout(gc, lay, pad, pad, tc.withA(255));
  }
  {
    gfx::Image hi = glyphs;
    const int r = std::max(1, int(std::lround(haloR * 0.75f)));
    boxBlur(hi, r);
    boxBlur(hi, std::max(1, r / 2));
    const u32 hp = gfx::premul(halo.withA(255));
    for (size_t i = 0; i < hi.px.size(); i++) {
      const u32 a = std::min<u32>(255, (hi.px[i] >> 24) * 3);
      const u32 k8 = u32(a * haloA + 0.5f);
      // premultiplied цвет ореола с альфой k8
      const u32 rb = ((hp & 0x00FF00FFu) * k8 + 0x00800080u) >> 8 & 0x00FF00FFu;
      const u32 g = (((hp >> 8) & 0xFFu) * k8 + 128) >> 8;
      sp->img.px[i] = (k8 << 24) | rb | (g << 8);
    }
  }
  blendImage(sp->img, glyphs, 0, 0, gfx::RectI(0, 0, glyphs.w, glyphs.h), tc.a / 255.0f);
  return sp;
}

// ================================================================ размещение
namespace {

struct Cand {
  Vec2 anchor;
  LabelKey key;
  double prio = 0;      // больше — раньше
  float shift = 0;      // шаг смещения по вертикали для запасных позиций (пиксели устройства), 0 — без смещений
  float minW = 0;       // скрыть, если подпись шире (пиксели устройства); 0 — без ограничения
};

u32 argb(Color c) { return (u32(c.a) << 24) | (u32(c.r) << 16) | (u32(c.g) << 8) | u32(c.b); }

}  // namespace

bool Obstacles::hit(const gfx::RectI& r) const {
  for (const gfx::RectI& o : rects)
    if (!o.intersect(r).empty()) return true;
  return false;
}

bool drawLabels(const FrameCtx& f, LabelCache& cache, Obstacles& obs, float figurePx, double budgetMs) {
  const World& w = *f.w;
  const geo::FaceSet& fs = *f.fs;
  const RenderOptions& opt = *f.opt;
  const Settings& set = *w.settings;
  const schema::MapMode mode = opt.mode;
  const bool data = mode != schema::MapMode::Political && mode != schema::MapMode::Guilds && mode != schema::MapMode::Terrain;
  std::vector<Cand> cands;
  const double z = f.zoom;
  const float dpi = f.dpi;

  // Государства: площадь и точка подписи крупнейшей провинции.
  if (set.labelStates) {
    struct Acc { double area = 0, best = 0; Vec2 at; };
    std::unordered_map<Id, Acc> acc;
    for (const auto& [pid, sh] : fs.provinces) {
      const Province* p = w.province(pid);
      if (!p || p->sea || !p->owner) continue;
      const Faction* o = w.faction(p->owner);
      if (!o || !o->isState()) continue;
      Acc& a = acc[p->owner];
      a.area += sh.area;
      if (sh.area > a.best) { a.best = sh.area; a.at = sh.label; }
    }
    for (const auto& [sid, a] : acc) {
      const Faction* s = w.faction(sid);
      const double px = 0.075 * std::sqrt(a.area) * z;   // кегль, логические пиксели
      if (px < 10.5 || s->name.empty()) continue;
      Cand c;
      c.anchor = a.at;
      c.key.text = s->name;
      c.key.kind = 0;
      c.key.size4 = u16(std::lround(std::round(clamp(px, 11.0, 30.0)) * dpi * 4));
      const Color tc = data ? Color(40, 34, 28) : Color::mix(s->color, Color(22, 16, 10), 0.62f);
      c.key.color = argb(tc.withA(242));
      c.prio = 1e12 + a.area;
      c.shift = 1;
      cands.push_back(std::move(c));
    }
  }
  // Провинции: при достаточном размере на экране.
  if (set.labelProvinces) {
    for (const auto& [pid, sh] : fs.provinces) {
      const Province* p = w.province(pid);
      if (!p || p->name.empty()) continue;
      if (!f.visible.inflated(sh.box.w() * 0.5).contains(sh.label)) continue;
      const double sw = sh.box.w() * z, sa = sh.area * z * z;
      if (sw < 46 || sa < 2600) continue;
      Cand c;
      c.anchor = sh.label;
      c.key.text = p->name;
      c.key.kind = p->sea ? 3 : 1;
      const double px = std::round(clamp(10 + std::sqrt(sa) * 0.0105, 11.0, p->sea ? 17.0 : 16.0));
      c.key.size4 = u16(std::lround(px * dpi * 4));
      c.key.color = p->sea ? argb(Color(236, 242, 255, 235)) : argb(Color(36, 31, 26, 230));
      c.prio = sh.area;
      c.shift = 1;
      c.minW = float(sw * dpi * 1.15);
      cands.push_back(std::move(c));
    }
  }
  // Войска: название верхнего объекта отметки под фигуркой при крупном масштабе.
  if (set.labelArmies && z >= 0.9 && f.marks) {
    for (const MarkLayout::Mark& m : f.marks->marks) {
      const Army* a = w.army(m.members.front());
      if (!a || a->name.empty() || !f.visible.inflated(200).contains(m.pos)) continue;
      Cand c;
      c.anchor = m.pos;
      c.key.text = a->name;
      c.key.kind = 2;
      c.key.size4 = u16(std::lround(11.0 * dpi * 4));
      c.key.color = argb(Color(30, 26, 22, 235));
      c.prio = -1;
      c.shift = 0;
      cands.push_back(std::move(c));
    }
  }
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
    if (a.prio != b.prio) return a.prio > b.prio;
    return a.key.text < b.key.text;
  });

  gfx::Image& target = f.c->target();
  const double t0 = nowSeconds();
  bool complete = true;
  for (const Cand& c : cands) {
    std::shared_ptr<const LabelSprite> sp = cache.peek(c.key, nullptr);
    float k = 1;  // масштаб временного спрайта другого кегля
    if (!sp) {
      if ((nowSeconds() - t0) * 1000 < budgetMs) {
        sp = cache.get(c.key);
      } else {
        complete = false;
        u16 near = 0;
        sp = cache.peek(c.key, &near);
        if (!sp || !near) continue;
        k = float(c.key.size4) / float(near);
      }
    }
    const float sw = sp->w * k, sh = sp->h * k;
    if (c.minW > 0 && sw > c.minW) continue;
    const gfx::Pt a = f.dev(c.anchor);
    const float m = 2 * dpi;
    std::vector<float> dys{0};
    if (c.key.kind == 2) dys = {figurePx * 0.62f + sh * 0.6f + 6 * dpi};
    else if (c.shift > 0) {
      const float st = sh * 1.9f + 4 * dpi;
      dys = {0, st, -st, 2 * st};
    }
    if (a.x < f.clip.x || a.y < f.clip.y || a.x >= f.clip.right() || a.y >= f.clip.bottom()) continue;
    for (float dy : dys) {
      float cx = a.x;
      const float cy = a.y + dy;
      // Подпись целиком в области просмотра: сдвиг от края не больше половины ширины.
      const float half = sw * 0.5f + m, lo = float(f.clip.x) + half, hi = float(f.clip.right()) - half;
      if (lo <= hi) cx = clamp(cx, lo, hi);
      if (std::fabs(cx - a.x) > sw * 0.5f) continue;
      if (cy - sh * 0.5f - m < f.clip.y || cy + sh * 0.5f + m > f.clip.bottom()) continue;
      const gfx::RectI r(int(std::floor(cx - sw * 0.5f - m)), int(std::floor(cy - sh * 0.5f - m * 1.5f)), int(std::ceil(sw + 2 * m)),
                         int(std::ceil(sh + 3 * m)));
      if (obs.hit(r)) continue;
      obs.add(r);
      if (k == 1) {
        blendImage(target, sp->img, int(std::lround(cx - sp->ox)), int(std::lround(cy - sp->oy)), f.clip);
      } else {
        const double x0 = cx - sp->ox * k, y0 = cy - sp->oy * k;
        scaleImage(target, sp->img, x0, y0, x0 + sp->img.w * k, y0 + sp->img.h * k, f.clip, false);
      }
      break;
    }
  }
  return complete;
}

}  // namespace rg::map::detail
