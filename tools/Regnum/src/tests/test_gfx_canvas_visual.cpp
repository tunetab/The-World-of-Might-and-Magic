// Regnum — наглядные листы холста (PNG в каталоге артефактов): карточки интерфейса с тенями и градиентами,
// тонкие и толстые линии, соединения и концы, масштабирование и поворот изображений, отсечение.
#include "codec/png.h"
#include "gfx/canvas.h"
#include "gfx/raster.h"
#include "gfx/stroke.h"
#include "tests/test.h"

using namespace rg;
using namespace rg::gfx;

namespace {

void save(const Image& img, const char* name) {
  codec::RgbaImage out;
  out.w = img.w;
  out.h = img.h;
  out.rgba = img.toRgba();
  std::string path = test::outDir() + "/gfx_" + name + ".png";
  CHECK_MSG(codec::writePngFile(path, out, 6), path);
}

// Увеличенный фрагмент (ближайший сосед) — для разглядывания сглаживания по пикселям.
void saveZoom(const Image& img, RectI r, int k, const char* name) {
  Image crop = img.cropped(r);
  Image z(crop.w * k, crop.h * k);
  Canvas c(z);
  c.drawImage(crop, {0, 0, float(z.w), float(z.h)}, 1, false);
  save(z, name);
}

// Гладкий шум (значения в узлах решётки + плавная интерполяция).
struct Noise {
  u64 seed;
  double lattice(int x, int y) const {
    u64 h = hashMix(hashMix(seed, u64(u32(x))), u64(u32(y)));
    h = Rng(h).next();
    return double(h >> 11) / 9007199254740992.0;
  }
  double at(double x, double y) const {
    int xi = int(std::floor(x)), yi = int(std::floor(y));
    double fx = x - xi, fy = y - yi;
    auto s = [](double t) { return t * t * (3 - 2 * t); };
    double a = lattice(xi, yi), b = lattice(xi + 1, yi), c = lattice(xi, yi + 1), d = lattice(xi + 1, yi + 1);
    return lerp(lerp(a, b, s(fx)), lerp(c, d, s(fx)), s(fy));
  }
  double fbm(double x, double y, int oct) const {
    double v = 0, amp = 0.5, f = 1;
    for (int i = 0; i < oct; i++) { v += amp * at(x * f, y * f); f *= 2; amp *= 0.5; }
    return v;
  }
};

// «Фотография»: небо, солнце, горы с туманом, озеро с отражением, мелкая фактура.
Image makePhoto(int w, int h) {
  Image img(w, h);
  Canvas c(img);
  Gradient sky;
  sky.p0 = {0, 0};
  sky.p1 = {0, float(h) * 0.62f};
  sky.stops = {{0.f, Color::hex(0x1b3a6b)}, {0.55f, Color::hex(0x6d8fc0)}, {0.85f, Color::hex(0xf2b27a)}, {1.f, Color::hex(0xf7d79c)}};
  Paint sp;
  sp.gradient = &sky;
  c.fillRect({0, 0, float(w), float(h)}, sp);
  Gradient sun;
  sun.kind = Gradient::Radial;
  sun.p0 = {float(w) * 0.68f, float(h) * 0.5f};
  sun.r1 = float(h) * 0.45f;
  sun.stops = {{0.f, Color(255, 244, 214, 230)}, {0.08f, Color(255, 230, 180, 200)}, {0.35f, Color(255, 190, 120, 60)}, {1.f, Color(255, 160, 90, 0)}};
  Paint sunp;
  sunp.gradient = &sun;
  c.fillRect({0, 0, float(w), float(h)}, sunp);
  c.fillCircle(sun.p0.x, sun.p0.y, float(h) * 0.045f, Color(255, 250, 235));
  Noise n{11};
  const Color layers[3] = {Color::hex(0x6a6f8c), Color::hex(0x404766), Color::hex(0x252a40)};
  for (int L = 0; L < 3; L++) {
    Path m;
    m.moveTo(0, float(h));
    for (int x = 0; x <= w; x += 2) {
      double base = h * (0.42 + 0.07 * L);
      double y = base - h * (0.16 - 0.03 * L) * n.fbm(x / (90.0 - 18 * L) + L * 7, L * 3.1, 6);
      m.lineTo(float(x), float(y));
    }
    m.lineTo(float(w), float(h));
    m.close();
    c.fillPath(m, layers[L]);
    Gradient fog;
    fog.p0 = {0, float(h) * (0.34f + 0.07f * L)};
    fog.p1 = {0, float(h) * (0.62f)};
    fog.stops = {{0.f, Color(240, 200, 170, 0)}, {1.f, Color(240, 200, 170, u8(70 - L * 15))}};
    Paint fp;
    fp.gradient = &fog;
    c.save();
    c.clipPath(m);
    c.fillRect({0, 0, float(w), float(h)}, fp);
    c.restore();
  }
  // Озеро: отражение верхней половины с затемнением и рябью.
  const int horizon = int(h * 0.62);
  Image top = img.cropped({0, 0, w, horizon});
  for (int y = horizon; y < h; y++) {
    int sy = 2 * horizon - y - 1;
    for (int x = 0; x < w; x++) {
      int ox = std::clamp(x + int(3 * std::sin(y * 0.9 + x * 0.05)), 0, w - 1);
      u32 p = top.at(ox, std::clamp(sy, 0, horizon - 1));
      Color cc = unpremul(p);
      img.px[size_t(y) * size_t(w) + size_t(x)] = premul(Color(u8(cc.r * 0.62), u8(cc.g * 0.68), u8(cc.b * 0.8)));
    }
  }
  // Зерно.
  Rng rng(5);
  for (u32& p : img.px) {
    int d = int(rng.next() % 9) - 4;
    Color cc = unpremul(p);
    auto f = [d](u8 v) { return u8(std::clamp(int(v) + d, 0, 255)); };
    p = premul(Color(f(cc.r), f(cc.g), f(cc.b), cc.a));
  }
  return img;
}

// Зонная пластинка: максимально чувствительна к наложению спектров при уменьшении.
Image makeZonePlate(int s) {
  Image img(s, s);
  for (int y = 0; y < s; y++)
    for (int x = 0; x < s; x++) {
      double dx = x - s / 2 + 0.5, dy = y - s / 2 + 0.5;
      double v = 0.5 + 0.5 * std::cos((dx * dx + dy * dy) * kPi / s);
      u8 g = u8(v * 255 + 0.5);
      img.px[size_t(y) * size_t(s) + size_t(x)] = premul(Color(g, g, g));
    }
  return img;
}

void background(Canvas& c, int w, int h) {
  Gradient bg;
  bg.p0 = {0, 0};
  bg.p1 = {0, float(h)};
  bg.stops = {{0.f, Color::hex(0x1e2533)}, {1.f, Color::hex(0x10141c)}};
  Paint p;
  p.gradient = &bg;
  c.fillRect({0, 0, float(w), float(h)}, p);
  Gradient glow;
  glow.kind = Gradient::Radial;
  glow.p0 = {float(w) * 0.15f, 0};
  glow.r1 = float(w) * 0.6f;
  glow.stops = {{0.f, Color(90, 120, 255, 46)}, {1.f, Color(90, 120, 255, 0)}};
  Paint gp;
  gp.gradient = &glow;
  c.fillRect({0, 0, float(w), float(h)}, gp);
}

// Карточка: тень 0 14 36 rgba(0,0,0,.45), заливка, рамка 1 px внутрь.
void card(Canvas& c, RectF r, float radius = 14) {
  c.boxShadow(r, radius, 36, 0, Color(0, 0, 0, 115), {0, 14});
  c.fillRoundRect(r, radius, Color::hex(0x232a38));
  c.strokeRoundRect(r.inset(0.5f), radius - 0.5f, 1, Color(255, 255, 255, 22));
}

void bar(Canvas& c, float x, float y, float w, float h, Color col) { c.fillRoundRect({x, y, w, h}, h / 2, col); }

}  // namespace

TEST(gfx_canvas_visual_ui) {
  const int W = 1280, H = 800;
  Image img(W, H);
  Canvas c(img);
  background(c, W, H);
  Image photo = makePhoto(480, 360);

  // ---- карточка профиля
  {
    RectF r{48, 48, 360, 300};
    card(c, r);
    Gradient hg;
    hg.p0 = {r.x, r.y};
    hg.p1 = {r.right(), r.y + 96};
    hg.stops = {{0.f, Color::hex(0x3d6df2)}, {1.f, Color::hex(0x8a5cf6)}};
    Paint hp;
    hp.gradient = &hg;
    c.fillRoundRect({r.x, r.y, r.w, 96}, 14, 14, 0, 0, hp);
    // аватар: изображение в круге с кольцом
    const float ax = r.x + 40 + 36, ay = r.y + 96;
    c.fillCircle(ax, ay, 40, Color::hex(0x232a38));
    c.save();
    Path circle;
    circle.addCircle(ax, ay, 36);
    c.clipPath(circle);
    c.drawImage(photo, {ax - 60, ay - 45, 120, 90});
    c.restore();
    c.strokeCircle(ax, ay, 36, 1.5f, Color(255, 255, 255, 60));
    c.fillCircle(ax + 26, ay + 26, 7, Color::hex(0x232a38));
    c.fillCircle(ax + 26, ay + 26, 5, Color::hex(0x34d399));
    // строки «текста»
    bar(c, r.x + 132, r.y + 112, 150, 12, Color(255, 255, 255, 220));
    bar(c, r.x + 132, r.y + 134, 96, 9, Color(255, 255, 255, 90));
    // прогресс
    bar(c, r.x + 24, r.y + 170, r.w - 48, 8, Color(255, 255, 255, 18));
    Gradient pg;
    pg.p0 = {r.x + 24, 0};
    pg.p1 = {r.x + 24 + (r.w - 48) * 0.68f, 0};
    pg.stops = {{0.f, Color::hex(0x10b981)}, {1.f, Color::hex(0x6ee7b7)}};
    Paint pp;
    pp.gradient = &pg;
    c.fillRoundRect({r.x + 24, r.y + 170, (r.w - 48) * 0.68f, 8}, 4, pp);
    // переключатель
    const float tx = r.x + 24, ty = r.y + 200;
    bar(c, tx, ty, 44, 24, Color::hex(0x3d6df2));
    c.boxShadow({tx + 22, ty + 2, 20, 20}, 10, 6, 0, Color(0, 0, 0, 90), {0, 2});
    c.fillCircle(tx + 32, ty + 12, 10, Color(255, 255, 255));
    bar(c, tx + 58, ty + 7, 120, 10, Color(255, 255, 255, 120));
    // кнопки: основная, контурная, с фокусом
    const float by = r.y + r.h - 56;
    Gradient bgc;
    bgc.p0 = {0, by};
    bgc.p1 = {0, by + 36};
    bgc.stops = {{0.f, Color::hex(0x5b84ff)}, {1.f, Color::hex(0x3d6df2)}};
    Paint bp;
    bp.gradient = &bgc;
    c.boxShadow({r.x + 24, by, 140, 36}, 18, 12, 0, Color(61, 109, 242, 110), {0, 4});
    c.fillRoundRect({r.x + 24, by, 140, 36}, 18, bp);
    bar(c, r.x + 64, by + 14, 60, 8, Color(255, 255, 255, 230));
    c.strokeRoundRect({r.x + 180.5f, by + 0.5f, 139, 35}, 17.5f, 1, Color(255, 255, 255, 70));
    c.strokeRoundRect({r.x + 177, by - 3, 146, 42}, 21, 2, Color::hex(0xf5c542));  // фокус — золотое кольцо 2 px
    bar(c, r.x + 220, by + 14, 60, 8, Color(255, 255, 255, 160));
  }

  // ---- карточка с графиком
  {
    RectF r{440, 48, 420, 300};
    card(c, r);
    bar(c, r.x + 24, r.y + 24, 120, 12, Color(255, 255, 255, 220));
    bar(c, r.x + 24, r.y + 46, 70, 9, Color(255, 255, 255, 90));
    const RectF g{r.x + 24, r.y + 80, r.w - 48, r.h - 110};
    Stroke grid;
    grid.width = 1;
    grid.dash = {3, 4};
    for (int i = 0; i <= 4; i++) {
      float y = std::floor(g.y + g.h * i / 4) + 0.5f;
      Pt l[2] = {{g.x, y}, {g.right(), y}};
      c.polyline(l, 2, false, grid, Color(255, 255, 255, 30));
    }
    std::vector<Pt> pts;
    Noise n{3};
    for (int i = 0; i <= 60; i++) {
      float x = g.x + g.w * i / 60;
      float v = float(0.25 + 0.55 * n.fbm(i * 0.11, 0.5, 4) + 0.15 * std::sin(i * 0.12));
      pts.push_back({x, g.bottom() - v * g.h});
    }
    Path area;
    area.moveTo(g.x, g.bottom());
    for (Pt p : pts) area.lineTo(p.x, p.y);
    area.lineTo(g.right(), g.bottom());
    area.close();
    Gradient ag;
    ag.p0 = {0, g.y};
    ag.p1 = {0, g.bottom()};
    ag.stops = {{0.f, Color(91, 132, 255, 120)}, {1.f, Color(91, 132, 255, 0)}};
    Paint ap;
    ap.gradient = &ag;
    c.fillPath(area, ap);
    Stroke line;
    line.width = 2.5f;
    line.join = Join::Round;
    line.cap = Cap::Round;
    c.polyline(pts.data(), pts.size(), false, line, Color::hex(0x7ea0ff));
    for (int i : {12, 30, 47}) {
      c.fillCircle(pts[size_t(i)].x, pts[size_t(i)].y, 6, Color::hex(0x232a38));
      c.fillCircle(pts[size_t(i)].x, pts[size_t(i)].y, 4, Color::hex(0x7ea0ff));
      c.strokeCircle(pts[size_t(i)].x, pts[size_t(i)].y, 6, 1.5f, Color(255, 255, 255, 200));
    }
  }

  // ---- «стекло»: яркий фон, размытие под панелью
  {
    const RectF r{892, 48, 340, 300};
    c.save();
    c.clipRoundRect(r, 14);
    c.fillRect(r, Color::hex(0x141a26));
    const Color blobs[4] = {Color::hex(0xff5a8a), Color::hex(0x5b84ff), Color::hex(0x34d399), Color::hex(0xf5c542)};
    for (int i = 0; i < 4; i++) c.fillCircle(r.x + 60 + i * 75, r.y + 90 + (i % 2) * 110, 52, blobs[i]);
    c.drawImage(photo, {r.x + 150, r.y + 190, 160, 120}, 0.9f);
    c.restore();
    const RectF g = r.inset(36, 60);
    c.boxShadow(g, 16, 30, 0, Color(0, 0, 0, 90), {0, 10});
    c.blurRegion({int(g.x), int(g.y), int(g.w), int(g.h)}, 24);
    c.save();
    c.clipRoundRect(g, 16);
    c.fillRect(g, Color(255, 255, 255, 34));
    c.restore();
    c.strokeRoundRect(g.inset(0.5f), 15.5f, 1, Color(255, 255, 255, 70));
    bar(c, g.x + 20, g.y + 24, 140, 12, Color(255, 255, 255, 230));
    bar(c, g.x + 20, g.y + 46, 90, 9, Color(255, 255, 255, 140));
    c.strokeRoundRect({g.x + 0.5f, g.y + 0.5f, 0, 0}, 0, 1, Color(255, 255, 255));  // вырожденный — ничего
  }

  // ---- значки из контуров
  {
    RectF r{48, 384, 560, 200};
    card(c, r);
    const Color ink = Color::hex(0xdbe4ff);
    Stroke ic;
    ic.width = 1.75f * 2;
    ic.cap = Cap::Round;
    ic.join = Join::Round;
    float x = r.x + 40, y = r.y + 50;
    auto icon = [&](float ox, float oy, const std::function<void(Path&)>& build, bool fill, Color col) {
      Path p;
      build(p);
      c.save();
      c.translate(ox, oy);
      c.scale(2, 2);
      if (fill) c.fillPath(p, col);
      else c.strokePath(p, ic, col);
      c.restore();
    };
    // метка карты (дуга SVG)
    icon(x, y, [](Path& p) {
      p.moveTo(12, 21.5f);
      p.cubicTo(12, 21.5f, 5, 15, 5, 9.5f);
      p.arcTo(7, 7, 0, false, true, 19, 9.5f);
      p.cubicTo(19, 15, 12, 21.5f, 12, 21.5f);
      p.close();
      p.addCircle(12, 9.5f, 2.6f);
    }, false, ink);
    // звезда
    icon(x + 80, y, [](Path& p) {
      Pt s[10];
      for (int i = 0; i < 10; i++) {
        double a = -kPi / 2 + i * kPi / 5, rr = i % 2 ? 4.2 : 10;
        s[i] = {float(12 + rr * std::cos(a)), float(12.5 + rr * std::sin(a))};
      }
      p.addPolygon(s, 10);
    }, true, Color::hex(0xf5c542));
    // щит
    icon(x + 160, y, [](Path& p) { p.moveTo(12, 2.5f); p.lineTo(20, 5.5f); p.cubicTo(20, 13, 17, 18.5f, 12, 21.5f); p.cubicTo(7, 18.5f, 4, 13, 4, 5.5f); p.close(); }, false, ink);
    // галочка
    icon(x + 240, y, [](Path& p) { p.moveTo(4, 12.5f); p.lineTo(9.5f, 18); p.lineTo(20, 6.5f); }, false, Color::hex(0x34d399));
    // сердце
    icon(x + 320, y, [](Path& p) {
      p.moveTo(12, 20.5f);
      p.cubicTo(4, 15, 2, 11, 2, 8);
      p.cubicTo(2, 5, 4.5f, 3, 7, 3);
      p.cubicTo(9, 3, 11, 4.5f, 12, 6.5f);
      p.cubicTo(13, 4.5f, 15, 3, 17, 3);
      p.cubicTo(19.5f, 3, 22, 5, 22, 8);
      p.cubicTo(22, 11, 20, 15, 12, 20.5f);
      p.close();
    }, true, Color::hex(0xff5a8a));
    // шестерня (EvenOdd с отверстием)
    {
      Path g;
      for (int i = 0; i < 16; i++) {
        double a0 = i * kPi / 8, rr = i % 2 ? 7.5 : 10;
        double a1 = a0 + kPi / 8;
        Pt p0{float(12 + rr * std::cos(a0)), float(12 + rr * std::sin(a0))}, p1{float(12 + rr * std::cos(a1)), float(12 + rr * std::sin(a1))};
        if (i == 0) g.moveTo(p0.x, p0.y);
        else g.lineTo(p0.x, p0.y);
        g.lineTo(p1.x, p1.y);
      }
      g.close();
      g.addCircle(12, 12, 3.5f);
      c.save();
      c.translate(x + 400, y);
      c.scale(2, 2);
      c.fillPath(g, ink, FillRule::EvenOdd);
      c.restore();
    }
    // мелкий ряд тех же значков в 1× (проверка резкости на малых размерах)
    for (int i = 0; i < 6; i++) {
      c.save();
      c.translate(x + i * 34.f, r.y + 140);
      Path p;
      p.addRoundRect({2, 2, 20, 20}, 5);
      Stroke s1;
      s1.width = 1.75f;
      c.strokePath(p, s1, Color(255, 255, 255, 200 - i * 20));
      c.fillCircle(12, 12, 2.5f + i * 0.5f, Color(255, 255, 255, 220));
      c.restore();
    }
  }

  // ---- плитки с тенями разной мягкости
  {
    for (int i = 0; i < 5; i++) {
      RectF r{648.f + i * 120, 400, 96, 96};
      float blur = 4.f + i * 10;
      c.boxShadow(r, 18, blur, 0, Color(0, 0, 0, 140), {0, blur * 0.3f});
      Gradient tg;
      tg.p0 = {r.x, r.y};
      tg.p1 = {r.right(), r.bottom()};
      tg.stops = {{0.f, Color::hsl(200 + i * 30, 0.75, 0.62)}, {1.f, Color::hsl(240 + i * 30, 0.7, 0.45)}};
      Paint tp;
      tp.gradient = &tg;
      c.fillRoundRect(r, 18, tp);
    }
    // таблетки с разными радиусами углов
    for (int i = 0; i < 5; i++) {
      RectF r{648.f + i * 120, 530, 96, 40};
      c.fillRoundRect(r, float(i * 4), float(20 - i * 4), float(i * 5), 2, Color::hsl(i * 60, 0.6, 0.55).alpha(0.9f));
    }
  }

  // ---- нижняя полоса: тонкие разделители и круги на дробных координатах
  {
    for (int i = 0; i < 40; i++) {
      float x = 60 + i * 29.7f;
      c.fillCircle(x, 660.3f + 0.1f * i, 2 + (i % 5) * 0.8f, Color(255, 255, 255, 200));
      c.line(x, 690, x + 18, 760, 1, Color(255, 255, 255, 120));
    }
    c.line(48, 640.5f, 1232, 640.5f, 1, Color(255, 255, 255, 40), Cap::Butt);
  }
  save(img, "ui");
  saveZoom(img, {20, 300, 160, 100}, 4, "ui_zoom_shadow");
  saveZoom(img, {60, 236, 330, 104}, 3, "ui_zoom_controls");
  saveZoom(img, {80, 420, 470, 140}, 3, "ui_zoom_icons");
}

TEST(gfx_canvas_visual_strokes) {
  const int W = 1280, H = 820;
  Image img(W, H, 0xFFFAFAF7u);
  Canvas c(img);
  const Color ink = Color::hex(0x1b2433);
  // веер линий трёх толщин
  for (int k = 0; k < 3; k++) {
    float cx = 140 + k * 260.f, cy = 150, wdt[3] = {0.5f, 1, 2.5f};
    for (int i = 0; i < 72; i++) {
      double a = i * kPi / 36;
      c.line(cx + float(12 * std::cos(a)), cy + float(12 * std::sin(a)), cx + float(110 * std::cos(a)), cy + float(110 * std::sin(a)),
             wdt[k], ink, Cap::Butt);
    }
  }
  // лестница толщин
  const float widths[12] = {0.1f, 0.25f, 0.5f, 0.75f, 1, 1.5f, 2, 3, 5, 8, 13, 20};
  for (int i = 0; i < 12; i++) {
    float y = 300 + i * 14.f + (i > 8 ? (i - 8) * 14.f : 0);
    c.line(830, y, 1240, y + 18, widths[i], ink, Cap::Round);
  }
  // концентрические окружности
  for (int i = 0; i < 24; i++) c.strokeCircle(140, 420, 6 + i * 4.3f, 0.6f + i * 0.05f, Color::hsl(i * 15, 0.7, 0.4));
  // кривые Безье разной толщины с круглыми концами
  for (int i = 0; i < 6; i++) {
    Path p;
    float y = 300 + i * 34.f;
    p.moveTo(300, y);
    p.cubicTo(380, y - 80, 520, y + 110, 600, y);
    p.quadTo(660, y - 60, 760, y + 10);
    Stroke s;
    s.width = 0.75f + i * 2.2f;
    s.cap = Cap::Round;
    s.join = Join::Round;
    c.strokePath(p, s, Color::hsl(210 + i * 22, 0.65, 0.45).alpha(0.85f));
  }
  // спираль (тонкая ломаная) и случайное блуждание (плотная ломаная)
  std::vector<Pt> sp;
  for (int i = 0; i < 2000; i++) {
    double a = i * 0.05, rr = 2 + i * 0.055;
    sp.push_back({float(140 + rr * std::cos(a)), float(680 + rr * std::sin(a) * 0.9)});
  }
  Stroke thin;
  thin.width = 0.8f;
  thin.join = Join::Round;
  c.polyline(sp.data(), sp.size(), false, thin, ink);
  std::vector<Pt> walk;
  Rng rng(9);
  double x = 300, y = 700, dir = 0;
  for (int i = 0; i < 3000; i++) {
    dir += (rng.uniform() - 0.5) * 0.9;
    x += 3 * std::cos(dir);
    y += 3 * std::sin(dir);
    if (x < 300 || x > 780) dir = kPi - dir;
    if (y < 560 || y > 800) dir = -dir;
    x = std::clamp(x, 300.0, 780.0);
    y = std::clamp(y, 560.0, 800.0);
    walk.push_back({float(x), float(y)});
  }
  Stroke w2;
  w2.width = 2;
  w2.join = Join::Miter;
  c.polyline(walk.data(), walk.size(), false, w2, Color(200, 40, 60, 150));
  // повёрнутые и масштабированные (неравномерно) обводки
  for (int i = 0; i < 8; i++) {
    c.save();
    c.translate(1000, 650);
    c.concat(Affine::rotate(float(i * kPi / 8)));
    c.scale(1, 0.35f);
    c.strokeCircle(0, 0, 130, 3, Color::hsl(i * 45, 0.7, 0.45).alpha(0.7f));
    c.restore();
  }
  save(img, "strokes");
  saveZoom(img, {600, 90, 120, 120}, 5, "strokes_zoom_fan");
  saveZoom(img, {80, 90, 120, 120}, 5, "strokes_zoom_fan_thin");
  saveZoom(img, {826, 292, 170, 110}, 5, "strokes_zoom_ladder");
}

TEST(gfx_canvas_visual_joins) {
  const int W = 1200, H = 900;
  Image img(W, H, 0xFFFFFFFFu);
  Canvas c(img);
  const Join joins[3] = {Join::Miter, Join::Round, Join::Bevel};
  const Cap caps[3] = {Cap::Butt, Cap::Round, Cap::Square};
  for (int j = 0; j < 3; j++)
    for (int k = 0; k < 3; k++) {
      const float ox = 40 + k * 390.f, oy = 30 + j * 200.f;
      c.fillRoundRect({ox - 10, oy - 10, 370, 180}, 10, Color::hex(0xf1f3f7));
      const Pt pts[6] = {{ox + 30, oy + 140}, {ox + 90, oy + 30}, {ox + 150, oy + 140}, {ox + 230, oy + 60}, {ox + 260, oy + 140}, {ox + 330, oy + 25}};
      Stroke s;
      s.width = 26;
      s.join = joins[j];
      s.cap = caps[k];
      s.miterLimit = 4;
      c.polyline(pts, 6, false, s, Color(40, 90, 220, 140));  // полупрозрачный: видно отсутствие двойного смешивания
      Stroke t;
      t.width = 1;
      c.polyline(pts, 6, false, t, Color(10, 20, 40, 230));
      for (const Pt& p : pts) c.fillCircle(p.x, p.y, 3, Color(220, 50, 50));
    }
  // пунктиры
  float y = 650;
  struct D { std::vector<float> dash; Cap cap; float width; } ds[] = {
      {{12, 6}, Cap::Butt, 3}, {{0.01f, 8}, Cap::Round, 5}, {{20, 5, 4, 5}, Cap::Butt, 2}, {{10, 10}, Cap::Square, 4}, {{30, 8}, Cap::Round, 8}};
  for (auto& d : ds) {
    Stroke s;
    s.width = d.width;
    s.cap = d.cap;
    s.dash = d.dash;
    Pt l[2] = {{40, y}, {700, y}};
    c.polyline(l, 2, false, s, Color::hex(0x1b2433));
    y += 40;
  }
  // пунктир по кривым и замкнутым фигурам
  Stroke dr;
  dr.width = 2;
  dr.dash = {9, 6};
  dr.dashOffset = 3;
  Path rr;
  rr.addRoundRect({760, 640, 200, 140}, 24);
  c.strokePath(rr, dr, Color::hex(0x5b84ff));
  Path circ;
  circ.addCircle(1070, 710, 70);
  Stroke dc = dr;
  dc.width = 6;
  dc.cap = Cap::Round;
  dc.dash = {0.01f, 14};
  c.strokePath(circ, dc, Color::hex(0xe24a6a));
  Path wave;
  wave.moveTo(760, 840);
  for (int i = 0; i < 8; i++) wave.quadTo(785.f + i * 50, i % 2 ? 870.f : 810.f, 810.f + i * 50, 840);
  Stroke dw;
  dw.width = 3;
  dw.dash = {14, 6, 2, 6};
  dw.cap = Cap::Round;
  c.strokePath(wave, dw, Color::hex(0x10b981));
  save(img, "joins");
  saveZoom(img, {40, 20, 200, 170}, 4, "joins_zoom");
}

TEST(gfx_canvas_visual_images) {
  const int W = 1280, H = 900;
  Image img(W, H, 0xFF20242Cu);
  Canvas c(img);
  Image photo = makePhoto(512, 384);
  // 1:1
  c.drawImage(photo, {24, 24, 512, 384});
  // уменьшение 1/3 (площадной фильтр) и Image::scaled
  c.drawImage(photo, {560, 24, 512 / 3.f, 384 / 3.f});
  Image small = photo.scaled(171, 128);
  c.drawImage(small, {560, 170, 171, 128});
  // увеличение фрагмента: билинейно / ближайший сосед / бикубика Image::scaled
  const RectF crop{300, 140, 48, 36};
  c.drawImage(photo, crop, {760, 24, 240, 180});
  c.drawImage(photo, crop, {1016, 24, 240, 180}, 1, false);
  Image cub = photo.cropped({300, 140, 48, 36}).scaled(240, 180);
  c.drawImage(cub, {760, 220, 240, 180});
  // зонная пластинка: исходник, площадное уменьшение, ближайший сосед (наложение спектров)
  Image zp = makeZonePlate(512);
  c.drawImage(zp, {24, 430, 256, 256});
  c.drawImage(zp, {300, 430, 128, 128});
  c.drawImage(zp, {300, 572, 128, 128}, 1, false);
  c.drawImage(zp.scaled(128, 128), {444, 430, 128, 128});
  // повороты с прозрачностью
  for (int i = 0; i < 4; i++) {
    c.save();
    c.translate(760 + i * 120.f, 560);
    c.concat(Affine::rotate(float(-0.5 + i * 0.35)));
    c.drawImage(photo, {-90, -68, 180, 135}, 0.55f + i * 0.15f);
    c.restore();
  }
  // изображение в скруглённой рамке и в круге (аватары) с тенями
  for (int i = 0; i < 5; i++) {
    RectF r{600.f + i * 130, 700, 110, 110};
    c.boxShadow(r, i % 2 ? 55 : 18, 20, 0, Color(0, 0, 0, 150), {0, 6});
    c.save();
    c.clipRoundRect(r, i % 2 ? 55 : 18);
    c.drawImage(photo, {float(60 + i * 60), 60, 220, 220}, r);
    c.restore();
  }
  // билинейное изображение как заливка контура (узор)
  Paint pat;
  Image tile(16, 16);
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) tile.px[size_t(y * 16 + x)] = ((x / 8 + y / 8) % 2) ? 0xFFE8C468u : 0xFF3B4A6Bu;
  pat.image = &tile;
  pat.imageXf = Affine::rotate(0.4f) * Affine::scale(1.5f);
  pat.tile = true;
  Path star;
  Pt s[10];
  for (int i = 0; i < 10; i++) {
    double a = -kPi / 2 + i * kPi / 5, rr = i % 2 ? 50 : 110;
    s[i] = {float(160 + rr * std::cos(a)), float(800 + rr * std::sin(a) * 0.85)};
  }
  star.addPolygon(s, 10);
  c.fillPath(star, pat);
  // повёрнутая карточка с тенью (общий путь тени: маска в собственной сетке)
  c.save();
  c.translate(430, 800);
  c.concat(Affine::rotate(-0.18f));
  c.boxShadow({-110, -60, 220, 120}, 16, 36, 0, Color(0, 0, 0, 160), {0, 14});
  c.fillRoundRect({-110, -60, 220, 120}, 16, Color::hex(0x2b3446));
  c.strokeRoundRect({-109.5f, -59.5f, 219, 119}, 15.5f, 1, Color(255, 255, 255, 30));
  c.fillRoundRect({-86, -36, 120, 12}, 6, Color(255, 255, 255, 220));
  c.fillRoundRect({-86, -14, 80, 9}, 4.5f, Color(255, 255, 255, 110));
  c.restore();
  save(img, "images");
  saveZoom(img, {300, 690, 260, 200}, 3, "images_zoom_shadow");
  saveZoom(img, {640, 450, 200, 120}, 4, "images_zoom_rotated");
  saveZoom(img, {40, 700, 180, 120}, 3, "images_zoom_pattern");
}

TEST(gfx_canvas_visual_clip) {
  const int W = 1200, H = 700;
  Image img(W, H, 0xFFF4F1EAu);
  Canvas c(img);
  Image photo = makePhoto(400, 300);
  // повёрнутое прямоугольное отсечение с градиентом
  c.save();
  c.translate(170, 170);
  c.concat(Affine::rotate(0.35f));
  c.clipRect({-110, -110, 220, 220});
  Gradient g;
  g.p0 = {-110, -110};
  g.p1 = {110, 110};
  g.stops = {{0.f, Color::hex(0xff7a59)}, {0.5f, Color::hex(0xffd166)}, {1.f, Color::hex(0x06d6a0)}};
  Paint gp;
  gp.gradient = &g;
  c.fillRect({-200, -200, 400, 400}, gp);
  c.setTransform({});

  for (int i = 0; i < 12; i++) c.line(0, i * 30.f, 400, i * 30.f + 80, 3, Color(255, 255, 255, 150));
  c.restore();
  // звезда EvenOdd как маска для фотографии
  c.save();
  Path star;
  Pt s[5];
  for (int i = 0; i < 5; i++) {
    double a = -kPi / 2 + i * 4 * kPi / 5;
    s[i] = {float(530 + 160 * std::cos(a)), float(190 + 160 * std::sin(a))};
  }
  star.addPolygon(s, 5);
  c.clipPath(star, FillRule::EvenOdd);
  c.drawImage(photo, {370, 30, 320, 320});
  c.restore();
  Stroke so;
  so.width = 2;
  so.join = Join::Round;
  c.strokePath(star, so, Color::hex(0x1b2433));
  // вложенные отсечения: скруглённый прямоугольник ∩ круг ∩ прямоугольник
  c.save();
  c.clipRoundRect({760, 40, 400, 300}, 40);
  c.fillRect({760, 40, 400, 300}, Color::hex(0x2b3a55));
  c.save();
  Path circ;
  circ.addCircle(960, 190, 130);
  c.clipPath(circ);
  c.drawImage(photo, {800, 40, 400, 300});
  c.clipRect({960, 0, 300, 400});
  c.fillRect({760, 40, 400, 300}, Color(30, 60, 140, 120));
  c.restore();
  c.strokeCircle(960, 190, 130, 3, Color(255, 255, 255, 200));
  c.restore();
  // маски-«глифы»: растеризованные контуры, выведенные fillMask с градиентом и прозрачностью
  Mask glyph(14, 18);
  {
    Path p;
    p.moveTo(2, 16);
    p.lineTo(7, 2);
    p.lineTo(12, 16);
    p.lineTo(9.6f, 16);
    p.lineTo(8.5f, 12.5f);
    p.lineTo(5.5f, 12.5f);
    p.lineTo(4.4f, 16);
    p.close();
    p.moveTo(6.1f, 10.5f);
    p.lineTo(7.9f, 10.5f);
    p.lineTo(7, 7.4f);
    p.close();
    rasterizeToMask(p, Affine::translate(0, 0.3f), glyph, 0, 0, FillRule::EvenOdd);
  }
  Gradient tg;
  tg.p0 = {40, 400};
  tg.p1 = {1160, 660};
  tg.stops = {{0.f, Color::hex(0x5b84ff)}, {1.f, Color::hex(0xe24a6a)}};
  Paint tp;
  tp.gradient = &tg;
  for (int row = 0; row < 14; row++)
    for (int col = 0; col < 76; col++) {
      if (((row * 7 + col * 3) % 11) == 0) continue;
      tp.opacity = 0.35f + 0.65f * float(row) / 13;
      c.fillMask(glyph, 40 + col * 14.7f, 400 + row * 19.f, tp);
    }
  // отсечение режет маски: круг поверх «текста»
  c.save();
  Path cc;
  cc.addCircle(600, 530, 90);
  c.clipPath(cc);
  c.fillRect({0, 380, 1200, 320}, Color(255, 255, 255, 200));
  for (int row = 0; row < 14; row++)
    for (int col = 30; col < 50; col++) c.fillMask(glyph, 40 + col * 14.7f, 400 + row * 19.f, Color::hex(0x1b2433));
  c.restore();
  save(img, "clip");
  saveZoom(img, {40, 395, 200, 60}, 5, "clip_zoom_glyphs");
  saveZoom(img, {470, 100, 140, 110}, 4, "clip_zoom_star");
}
