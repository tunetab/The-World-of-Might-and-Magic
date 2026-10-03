// Regnum — фигурки карты: векторная отрисовка в локальной системе (постамент радиусом 50 единиц, центр в нуле)
// и кеш готовых изображений в пикселях устройства (размер ¼ px, сдвиг ¼ px).
#include "gfx/figures.h"

#include <mutex>
#include <unordered_map>

#include "gfx/svgpath.h"

namespace rg::gfx {

namespace {

// ---------------------------------------------------------------- палитра
const Color kIvory = Color::hex(0xf4eddc);
const Color kIvoryShade = Color::hex(0xd8ccb2);
const Color kPlinthTop = Color::hex(0x323b4d);
const Color kPlinthBottom = Color::hex(0x10141c);
const Color kEdge = Color(8, 10, 16, 150);

// Габариты фигурки в единицах постамента (радиус 50).
constexpr float kFigX0 = -64, kFigX1 = 64, kFigY0 = -74, kFigY1 = 66;
// Габариты знаков (радиус 50).
constexpr float kMarkX0 = -60, kMarkX1 = 60, kMarkY0 = -60, kMarkY1 = 64;

struct Shapes {
  // Воин
  Path helmet, visor, crest, body, legs, arm, spear, spearHead, shield, shieldBoss, pennant;
  // Галеон
  Path hull, hullBand, masts, sailsMain, sailsFore, sailsMizzen, flag, wave;
  // Знаки
  Path star, crown, diamond, hall, burst, swordA, swordB;
};

Path P(std::string_view d) { return svgPath(d); }

const Shapes& shapes() {
  static const Shapes s = [] {
    Shapes x;
    // ---- воин (ось Y вниз; ступни на y = 31)
    x.helmet = P("M-1.5-45C2.6-43 6.6-37.6 6.6-30.6V-21.4C6.6-18.9 4.8-17.2 2.4-17.2H-6C-8.4-17.2-10.2-18.9-10.2-21.4V-30.6"
                 "C-10.2-37.6-5.6-43-1.5-45Z");
    x.visor = P("M-8.2-27.6H4.6");
    x.crest = P("M-1.2-43.6C-4.8-49.4-12.6-51.6-21-47.6-15.8-46.8-11.6-44.2-9.4-40.4-6.6-42-3.8-42.6-1-42Z");
    x.body = P("M-14.4-11.8C-14.4-15.4-11.8-17.4-8.2-17.4H5C8.6-17.4 11.2-15.4 11.2-11.8V7H-14.4Z");
    x.legs = P("M-10.8 6H-4.4L-4.2 27.4-1.6 29.2V31.6H-12.2V29.6L-10.4 27.4Z"
               "M0.2 6H6.6L7.4 27.4 10 29.2V31.6H-0.4L0.6 27.4Z");
    x.arm = P("M6.4-16.8C10.6-17.6 13.4-15.4 14.6-12.2L17.4-4.6C18-3 17.2-1.4 15.6-.9 14-.4 12.6-1.2 12-2.8L9.2-10.4Z"
              "M13.4-7.6C15.2-8.8 17.6-8.6 19.2-7.2L20.6-5.8C21.4-4.8 21.2-3.4 20.2-2.6 18.6-1.4 16.4-1.4 15-2.8Z");
    x.spear = P("M17.1-49.5H20.3V33.5H17.1Z");
    x.spearHead = P("M18.7-68C21.8-63.6 23.4-58.6 23.4-54.4 23.4-51.4 21.6-49.2 18.7-48 15.8-49.2 14-51.4 14-54.4"
                    "14-58.6 15.6-63.6 18.7-68Z");
    x.shield = P("M-25.4-13.4C-18.4-15.6-12.4-15.8-7.2-15.8-2-15.8 4-15.6 11-13.4L11-1.2C11 9.4 3.4 17.6-7.2 22.6"
                 "-17.8 17.6-25.4 9.4-25.4-1.2Z");
    x.shieldBoss = P("M-20.4 8.6-7.2-2.2 6 8.6");
    x.pennant = P("M20.3-47.2C25.6-48.4 31.2-47.8 37.4-45.4L32.6-41.2 37.4-37C31.2-35.4 25.6-35.6 20.3-36.8Z");
    // ---- галеон
    x.hull = P("M-35.4 4.4H-21.6L-19.6 9.4H22.6L35.6 6.2C34.2 15.4 28.4 22.6 18.6 24.6H-17.4C-27.6 23.2-33.4 15.6-35.4 4.4Z");
    x.hullBand = P("M-30.2 15.2H29.8");
    x.masts = P("M-2.4-46.5H.8V9.6H-2.4ZM15.6-31.5H18.6V9.6H15.6ZM-19.4-27.5H-16.6V4.6H-19.4ZM18.6 6.6L39.4-6.4 40.6-4.6 21.2 8.6Z");
    x.sailsMain = P("M-14.2-41.2C-6.4-43 4.6-43 12.6-41.2 13.8-36.4 13.8-31.4 12.6-26.6 4.6-28.2-6.4-28.2-14.2-26.6"
                    "-15.4-31.4-15.4-36.4-14.2-41.2Z"
                    "M-15.6-22.4C-6.8-24.4 5.4-24.4 14.2-22.4 15.8-15.6 15.8-8.4 14.2-1.6 5.4-3.6-6.8-3.6-15.6-1.6"
                    "-17.2-8.4-17.2-15.6-15.6-22.4Z");
    x.sailsFore = P("M6.2-27.2C13.2-28.6 21.4-28.6 28.4-27.2 30.2-19.2 30.2-11 28.4-3 21.4-4.6 13.2-4.6 6.2-3"
                    "C4.8-11 4.8-19.2 6.2-27.2Z");
    x.sailsMizzen = P("M-18-24.4C-22.6-19.4-27.4-11.4-31-3.6H-18Z");
    x.flag = P("M.8-46.2C5.6-47.6 10.4-47.2 15.6-45L11.6-41.6 15.6-38.2C10.4-36.6 5.6-36.8.8-38Z");
    x.wave = P("M-38 30.6C-34.4 27.2-30.8 27.2-27.2 30.6S-20 34-16.4 30.6-9.2 27.2-5.6 30.6 1.6 34 5.2 30.6"
               "12.4 27.2 16 30.6 23.2 34 26.8 30.6 34 27.2 37.6 30.6");
    // ---- знаки
    {
      std::string d;
      for (int i = 0; i < 10; i++) {
        const double a = (-90 + 36.0 * i) * kPi / 180.0;
        const double r = i % 2 == 0 ? 25.0 : 10.6;
        d += (i == 0 ? "M" : "L") + svgNum(r * std::cos(a)) + " " + svgNum(9 + r * std::sin(a));
      }
      x.star = P(d + "Z");
    }
    x.crown = P("M-15-17.6L-17.4-33.4-8.6-26.2 0-37.6 8.6-26.2 17.4-33.4 15-17.6Z");
    x.diamond = P("M-6.4-44.6C-2.8-48.2 2.8-48.2 6.4-44.6L44.6-6.4C48.2-2.8 48.2 2.8 44.6 6.4L6.4 44.6"
                  "C2.8 48.2-2.8 48.2-6.4 44.6L-44.6 6.4C-48.2 2.8-48.2-2.8-44.6-6.4Z");
    x.hall = P("M0-27.4L24-13.6V-9.8H-24V-13.6Z"
               "M-20.6-6.6H-14.6V13.4H-20.6ZM-9.6-6.6H-3.6V13.4H-9.6ZM3.6-6.6H9.6V13.4H3.6ZM14.6-6.6H20.6V13.4H14.6Z"
               "M-24.6 16.4H24.6V21.6H-24.6Z");
    {
      std::string d;
      for (int i = 0; i < 24; i++) {
        const double a = (-90 + 15.0 * i) * kPi / 180.0;
        const double r = i % 2 == 0 ? 50.0 : (i % 4 == 1 ? 34.0 : 37.0);
        d += (i == 0 ? "M" : "L") + svgNum(r * std::cos(a)) + " " + svgNum(r * std::sin(a));
      }
      x.burst = P(d + "Z");
    }
    // Меч: клинок от рукояти (низ слева) к острию (верх справа), затем отражение.
    x.swordA = P("M-24.4 18.6L12.4-18.2 19.8-24.8 15.8-14.6-21.4 22.2Z"
                 "M-27.6 9.4L-24.6 6.4-11.2 19.8-14.2 22.8Z"
                 "M-21.4 19.6L-26 24.2C-27.2 25.4-29 25.4-30.2 24.2-31.4 23-31.4 21.2-30.2 20L-25.6 15.4Z");
    x.swordB = x.swordA;
    x.swordB.transform(Affine{-1, 0, 0, 1, 0, 0});
    return x;
  }();
  return s;
}

Paint linear(Gradient& g, float y0, float y1, Color a, Color b) {
  g = Gradient{};
  g.kind = Gradient::Linear;
  g.p0 = {0, y0};
  g.p1 = {0, y1};
  g.stops = {{0.f, a}, {1.f, b}};
  Paint p;
  p.gradient = &g;
  return p;
}

Color opaque(Color c) { return c.withA(255); }

// Толщина линии не тоньше доли пикселя устройства (u — единиц на пиксель).
float hair(float units, float u, float minPx) { return std::max(units, minPx * u); }

// Тень, ореол выбора, кольцо цвета фракции (или двух союзников), тёмный постамент.
// Очень тёмный цвет кольца сливается с постаментом — подсветить, сохранив оттенок.
Color ringColor(Color c) {
  c = opaque(c);
  for (int i = 0; i < 6 && c.luminance() < 0.05f; i++) c = c.lighten(0.12f);
  return c;
}

void paintPlinth(Canvas& c, Color faction, bool selected, bool allied, Color ally2, float u) {
  faction = ringColor(faction);
  ally2 = ringColor(ally2);
  c.boxShadow({-50, -50, 100, 100}, 50, 9, 0, Color(0, 8, 24, 110), {0, 4.5f});
  if (selected) {
    c.fillCircle(0, 0, 60.5f, Color(255, 255, 255, 235));
    c.strokeCircle(0, 0, 60.5f, hair(1.6f, u, 0.8f), Color(8, 10, 16, 120));
    c.fillCircle(0, 0, 55.5f, Color::hex(0xf2c14e));
  }
  Gradient g;
  c.fillCircle(0, 0, 50, linear(g, -50, 50, faction.lighten(0.28f), faction.darken(0.3f)));
  if (allied) {
    Path half;
    half.moveTo(0, -50);
    half.arcTo(50, 50, 0, false, true, 0, 50);
    half.close();
    Gradient g2;
    c.fillPath(half, linear(g2, -50, 50, ally2.lighten(0.28f), ally2.darken(0.3f)));
    c.line(0, -50, 0, -40, hair(1.6f, u, 0.9f), Color(8, 10, 16, 140), Cap::Butt);
    c.line(0, 40, 0, 50, hair(1.6f, u, 0.9f), Color(8, 10, 16, 140), Cap::Butt);
  }
  // Блик сверху кольца и тёмная кромка (читается на светлой карте).
  Path hl;
  hl.moveTo(-40.5f, -26);
  hl.arcTo(48, 48, 0, false, true, 40.5f, -26);
  Stroke hs;
  hs.width = hair(2.2f, u, 0.6f);
  hs.cap = Cap::Round;
  c.strokePath(hl, hs, Color(255, 255, 255, 70));
  c.strokeCircle(0, 0, 50, hair(1.8f, u, 0.9f), kEdge);
  // Постамент
  Gradient gp;
  c.fillCircle(0, 0, 40.5f, linear(gp, -40, 40, kPlinthTop, kPlinthBottom));
  c.strokeCircle(0, 0, 40.5f, hair(1.6f, u, 0.7f), Color(0, 0, 0, 120));
}

void paintArmy(Canvas& c, Color faction, bool selected, bool allied, Color ally2, float u) {
  const Shapes& s = shapes();
  faction = opaque(faction);
  ally2 = opaque(ally2);
  paintPlinth(c, faction, selected, allied, ally2, u);
  // Тень фигуры на постаменте
  c.save();
  c.translate(0, 31.5f);
  c.scale(1, 0.32f);
  c.fillCircle(0, 0, 22, Color(0, 0, 0, 90));
  c.restore();
  Gradient gi;
  const Paint ivory = linear(gi, -68, 34, kIvory, kIvoryShade);
  const Color line = Color(10, 12, 18, 200);
  const float ow = hair(2.4f, u, 0.9f);
  Stroke outline;
  outline.width = ow;
  outline.join = Join::Round;
  // Контур под силуэтом: отделяет светлые части от кольца того же тона.
  for (const Path* p : {&s.spear, &s.spearHead, &s.legs, &s.body, &s.arm, &s.helmet, &s.crest}) c.strokePath(*p, outline, line);
  c.fillPath(s.spear, ivory);
  c.fillPath(s.spearHead, ivory);
  c.fillPath(s.legs, ivory);
  c.fillPath(s.body, ivory);
  c.fillPath(s.helmet, ivory);
  if (u < 1.6f) {  // смотровая щель — когда хватает пикселей
    Stroke vs;
    vs.width = 1.7f;
    vs.cap = Cap::Butt;
    c.strokePath(s.visor, vs, Color(30, 34, 44, 220));
  }
  Gradient gc;
  c.fillPath(s.crest, linear(gc, -51, -40, (allied ? ally2 : faction).lighten(0.2f), (allied ? ally2 : faction).darken(0.1f)));
  // Флажок
  c.strokePath(s.pennant, outline, line);
  Gradient gpn;
  c.fillPath(s.pennant, linear(gpn, -48, -35, (allied ? ally2 : faction).lighten(0.15f), (allied ? ally2 : faction).darken(0.1f)));
  c.strokePath(s.pennant, Stroke{hair(1.5f, u, 0.6f), Join::Round}, Color(255, 255, 255, 150));
  // Щит
  c.strokePath(s.shield, Stroke{hair(3.2f, u, 1.0f), Join::Round}, line);
  Gradient gs;
  c.fillPath(s.shield, linear(gs, -16, 23, faction.lighten(0.22f), faction.darken(0.18f)));
  if (allied) {
    c.save();
    c.clipRect({-7.2f, -20, 30, 50});
    Gradient gs2;
    c.fillPath(s.shield, linear(gs2, -16, 23, ally2.lighten(0.22f), ally2.darken(0.18f)));
    c.restore();
  }
  Stroke rim;
  rim.width = hair(2.6f, u, 0.8f);
  rim.join = Join::Round;
  c.strokePath(s.shield, rim, kIvory);
  if (u < 1.2f) {  // стропило на щите — только когда хватает пикселей
    Stroke cs;
    cs.width = 3.0f;
    cs.cap = Cap::Butt;
    cs.join = Join::Miter;
    c.save();
    c.clipPath(s.shield);
    c.strokePath(s.shieldBoss, cs, kIvory.alpha(0.9f));
    c.restore();
  }
  // Рука поверх щита
  c.strokePath(s.arm, outline, line);
  c.fillPath(s.arm, ivory);
}

void paintFleet(Canvas& c, Color faction, bool selected, bool allied, Color ally2, float u) {
  const Shapes& s = shapes();
  faction = opaque(faction);
  ally2 = opaque(ally2);
  paintPlinth(c, faction, selected, allied, ally2, u);
  const Color line = Color(10, 12, 18, 200);
  Stroke outline;
  outline.width = hair(2.4f, u, 0.9f);
  outline.join = Join::Round;
  Gradient gi;
  const Paint ivory = linear(gi, -47, 25, kIvory, kIvoryShade);
  // Волна
  Stroke ws;
  ws.width = hair(3.4f, u, 0.9f);
  ws.cap = Cap::Round;
  ws.join = Join::Round;
  c.save();
  c.clipPath([] {
    Path p;
    p.addCircle(0, 0, 39.5f);
    return p;
  }());
  c.strokePath(s.wave, ws, Color::hex(0x8fb8e0));
  c.restore();
  // Мачты, флажок
  c.strokePath(s.masts, outline, line);
  c.fillPath(s.masts, ivory);
  c.strokePath(s.flag, outline, line);
  c.fillPath(s.flag, Paint((allied ? ally2 : faction).lighten(0.1f)));
  c.strokePath(s.flag, Stroke{hair(1.4f, u, 0.6f), Join::Round}, Color(255, 255, 255, 150));
  // Паруса
  auto sail = [&](const Path& p, Color col, float y0, float y1) {
    c.strokePath(p, outline, line);
    Gradient g;
    c.fillPath(p, linear(g, y0, y1, col.lighten(0.22f), col.darken(0.16f)));
    Stroke rim;
    rim.width = hair(2.0f, u, 0.7f);
    rim.join = Join::Round;
    c.strokePath(p, rim, kIvory);
  };
  sail(s.sailsMizzen, faction, -25, -3);
  sail(s.sailsMain, faction, -43, -2);
  sail(s.sailsFore, allied ? ally2 : faction, -28, -3);
  // Корпус
  c.strokePath(s.hull, outline, line);
  c.fillPath(s.hull, ivory);
  Stroke band;
  band.width = hair(2.2f, u, 0.6f);
  c.save();
  Path hullClip = s.hull;
  c.clipPath(hullClip);
  c.strokePath(s.hullBand, band, Color(40, 30, 20, 150));
  c.restore();
}

void paintCapital(Canvas& c, Color faction, float u) {
  const Shapes& s = shapes();
  faction = opaque(faction);
  c.boxShadow({-50, -50, 100, 100}, 50, 8, 0, Color(0, 8, 24, 100), {0, 4});
  Gradient g;
  c.fillCircle(0, 0, 50, linear(g, -50, 50, faction.lighten(0.25f), faction.darken(0.3f)));
  c.strokeCircle(0, 0, 44, hair(5.0f, u, 1.0f), kIvory);
  c.strokeCircle(0, 0, 50, hair(1.8f, u, 0.9f), kEdge);
  const Color line = Color(10, 12, 18, 170);
  Stroke o;
  o.width = hair(3.0f, u, 0.9f);
  o.join = Join::Round;
  c.strokePath(s.star, o, line);
  c.strokePath(s.crown, o, line);
  Gradient gi;
  const Paint ivory = linear(gi, -38, 34, kIvory, kIvoryShade);
  c.fillPath(s.star, ivory);
  c.fillPath(s.crown, ivory);
}

void paintHq(Canvas& c, Color guild, float u) {
  const Shapes& s = shapes();
  guild = opaque(guild);
  c.boxShadow({-34, -34, 68, 68}, 10, 10, 0, Color(0, 8, 24, 110), {0, 5});
  Gradient g;
  c.fillPath(s.diamond, linear(g, -48, 48, guild.lighten(0.25f), guild.darken(0.3f)));
  Stroke rim;
  rim.width = hair(5.0f, u, 1.0f);
  rim.join = Join::Round;
  c.save();
  c.translate(0, 0);
  c.scale(0.86f, 0.86f);
  c.strokePath(s.diamond, rim, kIvory);
  c.restore();
  c.strokePath(s.diamond, Stroke{hair(1.8f, u, 0.9f), Join::Round}, kEdge);
  const Color line = Color(10, 12, 18, 170);
  c.strokePath(s.hall, Stroke{hair(2.8f, u, 0.9f), Join::Round}, line);
  Gradient gi;
  c.fillPath(s.hall, linear(gi, -28, 22, kIvory, kIvoryShade));
}

void paintBattle(Canvas& c, float u) {
  const Shapes& s = shapes();
  c.boxShadow({-40, -40, 80, 80}, 40, 12, 0, Color(40, 0, 0, 110), {0, 4});
  Gradient g;
  g.kind = Gradient::Radial;
  g.p0 = {0, 0};
  g.r1 = 50;
  g.stops = {{0.f, Color::hex(0xffd36b)}, {0.45f, Color::hex(0xf58a2e)}, {1.f, Color::hex(0xc2321c)}};
  Paint p;
  p.gradient = &g;
  c.fillPath(s.burst, p);
  c.strokePath(s.burst, Stroke{hair(2.0f, u, 0.9f), Join::Round}, Color(60, 10, 4, 170));
  const Color line = Color(30, 8, 4, 210);
  Stroke o;
  o.width = hair(3.6f, u, 1.0f);
  o.join = Join::Round;
  // Задний меч, затем передний со своей обводкой поверх.
  Gradient gi;
  const Paint ivory = linear(gi, -25, 25, Color::hex(0xfffaf0), kIvoryShade);
  c.strokePath(s.swordB, o, line);
  c.fillPath(s.swordB, ivory);
  c.strokePath(s.swordA, o, line);
  c.fillPath(s.swordA, ivory);
}

// ---------------------------------------------------------------- кеш изображений
enum class Kind : u8 { Army, Fleet, Capital, Hq, Battle };

struct Key {
  u8 kind, flags, phx, phy;
  u32 size4;     // размер в ¼ пикселя устройства
  u32 a, b;      // цвета RGBA
  bool operator==(const Key&) const = default;
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    u64 h = hashMix((u64(k.kind) << 24) | (u64(k.flags) << 16) | (u64(k.phx) << 8) | k.phy, k.size4);
    return size_t(hashMix(h, (u64(k.a) << 32) | k.b));
  }
};
struct Sprite {
  Image img;
  int ox = 0, oy = 0;  // пиксель изображения, совпадающий с целой частью центра
};

constexpr size_t kGeneration = 600;
constexpr float kMaxCachedPx = 200;

class SpriteCache {
 public:
  std::shared_ptr<const Sprite> get(const Key& k) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = cur_.find(k);
    if (it != cur_.end()) return it->second;
    auto jt = old_.find(k);
    if (jt == old_.end()) return nullptr;
    auto v = jt->second;
    insertLocked(k, v);
    return v;
  }
  void put(const Key& k, std::shared_ptr<const Sprite> v) {
    std::lock_guard<std::mutex> lock(mu_);
    insertLocked(k, std::move(v));
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mu_);
    cur_.clear();
    old_.clear();
  }
  size_t size() {
    std::lock_guard<std::mutex> lock(mu_);
    return cur_.size() + old_.size();
  }

 private:
  void insertLocked(const Key& k, std::shared_ptr<const Sprite> v) {
    if (cur_.size() >= kGeneration) {
      old_.swap(cur_);
      cur_.clear();
    }
    cur_[k] = std::move(v);
  }
  std::mutex mu_;
  std::unordered_map<Key, std::shared_ptr<const Sprite>, KeyHash> cur_, old_;
};

SpriteCache& cache() {
  static SpriteCache c;
  return c;
}

u32 packColor(Color c) { return (u32(c.r) << 24) | (u32(c.g) << 16) | (u32(c.b) << 8) | c.a; }

struct Spec {
  Kind kind;
  Color a, b;
  bool selected, allied;
};

void paint(Canvas& c, const Spec& s, float u) {
  switch (s.kind) {
    case Kind::Army: paintArmy(c, s.a, s.selected, s.allied, s.b, u); break;
    case Kind::Fleet: paintFleet(c, s.a, s.selected, s.allied, s.b, u); break;
    case Kind::Capital: paintCapital(c, s.a, u); break;
    case Kind::Hq: paintHq(c, s.a, u); break;
    case Kind::Battle: paintBattle(c, u); break;
  }
}

void extents(Kind k, float& x0, float& y0, float& x1, float& y1) {
  if (k == Kind::Army || k == Kind::Fleet) { x0 = kFigX0; y0 = kFigY0; x1 = kFigX1; y1 = kFigY1; }
  else { x0 = kMarkX0; y0 = kMarkY0; x1 = kMarkX1; y1 = kMarkY1; }
}

void draw(Canvas& c, Pt center, float size, const Spec& spec) {
  if (!(size > 0) || !std::isfinite(size) || !std::isfinite(center.x) || !std::isfinite(center.y)) return;
  if (spec.a.a == 0 && spec.kind != Kind::Battle) return;
  float x0, y0, x1, y1;
  extents(spec.kind, x0, y0, x1, y1);
  const float k = size / 100.f;
  if (c.quickReject({center.x + x0 * k, center.y + y0 * k, (x1 - x0) * k, (y1 - y0) * k})) return;
  const Affine& M = c.transform();
  const bool axis = M.b == 0 && M.c == 0 && M.a > 0 && std::fabs(M.a - M.d) <= 1e-4f * M.a;
  const float dev = size * (axis ? M.a : M.scaleFactor());
  if (!(dev >= 2.f)) return;
  if (!axis || dev > kMaxCachedPx) {
    c.save();
    c.translate(center.x, center.y);
    c.scale(k, k);
    paint(c, spec, 1.f / (k * M.scaleFactor()));
    c.restore();
    return;
  }
  const float S = std::round(dev * 4) / 4;
  const Pt d = M.apply(center);
  if (!(std::fabs(d.x) < 1e8f && std::fabs(d.y) < 1e8f)) return;
  float fx = std::floor(d.x), fy = std::floor(d.y);
  int qx = int(std::lround((d.x - fx) * 4)), qy = int(std::lround((d.y - fy) * 4));
  if (qx == 4) { qx = 0; fx += 1; }
  if (qy == 4) { qy = 0; fy += 1; }
  const u8 flags = u8((spec.selected ? 1 : 0) | (spec.allied ? 2 : 0));
  const Key key{u8(spec.kind), flags, u8(qx), u8(qy), u32(S * 4), packColor(spec.a),
                spec.allied ? packColor(spec.b) : 0u};
  auto spr = cache().get(key);
  if (!spr) {
    const float ks = S / 100.f;
    auto fresh = std::make_shared<Sprite>();
    fresh->ox = int(std::ceil(-x0 * ks)) + 2;
    fresh->oy = int(std::ceil(-y0 * ks)) + 2;
    const int w = fresh->ox + int(std::ceil(x1 * ks)) + 3;
    const int h = fresh->oy + int(std::ceil(y1 * ks)) + 3;
    fresh->img = Image(w, h);
    Canvas sc(fresh->img);
    sc.setTransform(Affine{ks, 0, 0, ks, float(fresh->ox) + qx * 0.25f, float(fresh->oy) + qy * 0.25f});
    paint(sc, spec, 1.f / ks);
    spr = fresh;
    cache().put(key, spr);
  }
  c.save();
  c.setTransform(Affine{});
  c.drawImage(spr->img, RectF{fx - float(spr->ox), fy - float(spr->oy), float(spr->img.w), float(spr->img.h)}, 1, false);
  c.restore();
}

}  // namespace

void drawArmyFigure(Canvas& c, Pt center, float size, Color faction, bool selected, bool allied, Color ally2) {
  draw(c, center, size, {Kind::Army, faction, allied ? ally2 : faction, selected, allied && ally2.a > 0});
}

void drawFleetFigure(Canvas& c, Pt center, float size, Color faction, bool selected, bool allied, Color ally2) {
  draw(c, center, size, {Kind::Fleet, faction, allied ? ally2 : faction, selected, allied && ally2.a > 0});
}

void drawCapitalMarker(Canvas& c, Pt center, float size, Color faction) {
  draw(c, center, size, {Kind::Capital, faction, faction, false, false});
}

void drawHqMarker(Canvas& c, Pt center, float size, Color guild) { draw(c, center, size, {Kind::Hq, guild, guild, false, false}); }

void drawBattleMarker(Canvas& c, Pt center, float size) {
  draw(c, center, size, {Kind::Battle, Color::hex(0xc2321c), Color::hex(0xc2321c), false, false});
}

RectF figureBounds(Pt center, float size) {
  const float k = size / 100.f;
  return {center.x + kFigX0 * k, center.y + kFigY0 * k, (kFigX1 - kFigX0) * k, (kFigY1 - kFigY0) * k};
}

RectF markerBounds(Pt center, float size) {
  const float k = size / 100.f;
  return {center.x + kMarkX0 * k, center.y + kMarkY0 * k, (kMarkX1 - kMarkX0) * k, (kMarkY1 - kMarkY0) * k};
}

void clearFigureCache() { cache().clear(); }
size_t figureCacheSize() { return cache().size(); }

}  // namespace rg::gfx
