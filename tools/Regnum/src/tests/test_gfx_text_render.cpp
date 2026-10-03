// Тесты отрисовки текста: витрина PNG (тёмный и светлый фон), холст с преобразованиями, потоки, производительность.
#include <cstdio>
#include <thread>

#include "codec/png.h"
#include "gfx/text.h"
#include "tests/test.h"

using namespace rg;
using namespace rg::gfx;

namespace {

bool fontsOk() {
  static const bool ok = initFonts(nullptr);
  return ok;
}

TextStyle style(FontFamily f, FontWeight w, float size) {
  TextStyle s;
  s.family = f;
  s.weight = w;
  s.size = size;
  return s;
}

// Смешивание маски цветом (src-over, premultiplied) — независимая от холста проверка растров.
void blendMask(Image& img, const Mask& m, int x, int y, Color c) {
  for (int j = 0; j < m.h; j++) {
    int yy = y + j;
    if (yy < 0 || yy >= img.h) continue;
    for (int i = 0; i < m.w; i++) {
      int xx = x + i;
      if (xx < 0 || xx >= img.w) continue;
      u32 a = u32(m.row(j)[i]) * c.a / 255;
      if (!a) continue;
      u32& d = img.row(yy)[xx];
      auto ch = [&](int sh, u32 sc) {
        u32 dc = (d >> sh) & 255;
        return ((sc * a + dc * (255 - a) + 127) / 255) << sh;
      };
      u32 da = d >> 24;
      d = ((a + da * (255 - a) / 255) << 24) | ch(16, c.r) | ch(8, c.g) | ch(0, c.b);
    }
  }
}

bool savePng(const Image& img, const std::string& name) {
  codec::RgbaImage out;
  out.w = img.w;
  out.h = img.h;
  out.rgba.resize(size_t(img.w) * size_t(img.h) * 4);
  for (size_t i = 0; i < img.px.size(); i++) {
    Color c = unpremul(img.px[i]);
    out.rgba[i * 4] = c.r;
    out.rgba[i * 4 + 1] = c.g;
    out.rgba[i * 4 + 2] = c.b;
    out.rgba[i * 4 + 3] = c.a;
  }
  return codec::writePngFile(test::outDir() + "/" + name, out, 6);
}

double inkSum(const Image& img, u32 bg) {
  double s = 0;
  for (u32 p : img.px) {
    if (p == bg) continue;
    for (int sh = 0; sh < 24; sh += 8) s += std::abs(int((p >> sh) & 255) - int((bg >> sh) & 255));
  }
  return s;
}

}  // namespace

TEST(gfx_text_showcase_png) {
  if (!fontsOk()) return;
  const float sizes[] = {11, 12, 13, 14, 16, 20, 28, 40};
  struct Row { FontFamily f; FontWeight w; const char* name; };
  const Row rows[] = {
      {FontFamily::UI, FontWeight::Regular, "Интерфейс"},
      {FontFamily::UI, FontWeight::Semibold, "Интерфейс полужирный"},
      {FontFamily::UI, FontWeight::Bold, "Интерфейс жирный"},
      {FontFamily::Display, FontWeight::Regular, "Заголовки"},
      {FontFamily::Display, FontWeight::Semibold, "Заголовки полужирные"},
      {FontFamily::Display, FontWeight::Bold, "Заголовки жирные"},
      {FontFamily::Mono, FontWeight::Regular, "Моноширинный"},
      {FontFamily::Mono, FontWeight::Semibold, "Моноширинный полужирный"},
      {FontFamily::Mono, FontWeight::Bold, "Моноширинный жирный"},
  };
  const char* sample = "Королевство Арден — доход 12 345 золота, налог 15 %. Съешь же ещё этих мягких булок!";
  struct Theme { const char* file; Color bg, text, dim, accent; };
  const Theme themes[] = {
      {"text_showcase_dark.png", Color::hex(0x151a22), Color::hex(0xece8df), Color::hex(0xa7afbb), Color::hex(0xd9a441)},
      {"text_showcase_light.png", Color::hex(0xfbf9f4), Color::hex(0x1d1a14), Color::hex(0x5d574c), Color::hex(0xb07d1f)},
  };
  for (const Theme& th : themes) {
    // Высота: сумма шагов строк.
    float h = 24;
    for (auto& r : rows) {
      h += 26;
      for (float s : sizes) h += metrics(style(r.f, r.w, s)).lineHeight + 2;
      h += 10;
    }
    Image img(1180, int(h) + 140);
    Canvas c(img);
    c.clear(th.bg);
    float y = 16;
    for (auto& r : rows) {
      drawText(c, r.name, style(FontFamily::UI, FontWeight::Semibold, 12), 16, y, th.accent);
      y += 26;
      for (float s : sizes) {
        TextStyle st = style(r.f, r.w, s);
        drawText(c, strf("%g", s), style(FontFamily::UI, FontWeight::Regular, 11), 16, y + (metrics(st).baseline - 9), th.dim);
        drawTextBox(c, sample, st, RectF(48, y, 1110, metrics(st).lineHeight), th.text, Align::Left, VAlign::Top, false, 1, true);
        y += metrics(st).lineHeight + 2;
      }
      y += 10;
    }
    // Абзац с переносом и выравниванием, вторичный текст.
    TextStyle para = style(FontFamily::UI, FontWeight::Regular, 13);
    para.lineHeight = 1.45f;
    const char* text =
        "Гильдия мореходов держит половину торговых путей северо-западного побережья. Ставка налога — 12,5 %, "
        "срок договора — 3 хода. Войска: 1 200 пехоты, 340 всадников.";
    drawTextBox(c, text, para, RectF(16, y, 360, 120), th.text, Align::Left, VAlign::Top, true, 0, false);
    drawTextBox(c, text, para, RectF(400, y, 360, 120), th.dim, Align::Center, VAlign::Top, true, 0, false);
    drawTextBox(c, text, para, RectF(784, y, 360, 120), th.text, Align::Right, VAlign::Top, true, 3, true);
    CHECK(savePng(img, th.file));
    CHECK(inkSum(img, premul(th.bg)) > 1e6);
  }
}

TEST(gfx_text_canvas_transforms) {
  if (!fontsOk()) return;
  TextStyle st = style(FontFamily::UI, FontWeight::Regular, 16);
  const char* t = "Арден";
  u32 bg = premul(Color::hex(0xffffff));
  // Целый перенос, дробный перенос, масштаб ×2 и поворот дают видимый текст в ожидаемом месте.
  Image a(200, 60, bg);
  {
    Canvas c(a);
    c.translate(10, 10);
    drawText(c, t, st, 5, 5, Color::hex(0x000000));
  }
  Image b(200, 60, bg);
  {
    Canvas c(b);
    c.translate(10.5f, 10.25f);
    drawText(c, t, st, 5, 5, Color::hex(0x000000));
  }
  Image s2(400, 120, bg);
  {
    Canvas c(s2);
    c.scale(2, 2);
    drawText(c, t, st, 15, 15, Color::hex(0x000000));
  }
  Image r(200, 200, bg);
  {
    Canvas c(r);
    c.translate(100, 100);
    c.concat(Affine::rotate(float(kPi / 4)));
    drawText(c, t, st, 0, 0, Color::hex(0x000000));
  }
  double ia = inkSum(a, bg), ib = inkSum(b, bg), is2 = inkSum(s2, bg), ir = inkSum(r, bg);
  CHECK(ia > 1000);
  CHECK_NEAR(ib, ia, ia * 0.1);
  CHECK_NEAR(is2, ia * 4, ia * 0.6);
  CHECK_NEAR(ir, ia, ia * 0.35);
  // Положение: левый край чернил около x = 15 (перенос 10 + 5).
  int minX = 1000;
  for (int y = 0; y < a.h; y++)
    for (int x = 0; x < a.w; x++)
      if (a.at(x, y) != bg) minX = std::min(minX, x);
  CHECK(minX >= 14 && minX <= 18);
  // Повёрнутый текст уходит вниз-вправо от центра.
  double sx = 0, sy = 0, sw = 0;
  for (int y = 0; y < r.h; y++)
    for (int x = 0; x < r.w; x++)
      if (r.at(x, y) != bg) { sx += x; sy += y; sw += 1; }
  CHECK(sw > 0);
  CHECK(sx / sw > 105 && sy / sw > 105);
  Image comp(400, 200, bg);
  for (int y = 0; y < 60; y++) for (int x = 0; x < 200; x++) comp.row(y)[x] = a.at(x, y);
  for (int y = 0; y < 60; y++) for (int x = 0; x < 200; x++) comp.row(y + 60)[x] = b.at(x, y);
  for (int y = 0; y < 200; y++) for (int x = 0; x < 200; x++) comp.row(y)[x + 200] = r.at(x, y);
  CHECK(savePng(comp, "text_transforms.png"));
  CHECK(savePng(s2, "text_scale2.png"));
}

TEST(gfx_text_rasterize_layout_threads) {
  if (!fontsOk()) return;
  // Одинаковый результат в нескольких потоках одновременно (общий кеш глифов).
  const char* words[] = {"Арден", "Валория", "Гильдия мореходов", "Казна 12 345", "Северо-запад", "Ёлки и ели", "Kingdom AV To"};
  auto render = [&](int k) {
    TextStyle st = style(FontFamily(k % 3), FontWeight((k / 3) % 3), 11.f + float(k % 7) * 1.75f);
    TextLayout L = layoutText(words[k % 7], st);
    Image img(220, 50, 0);
    rasterizeLayout(L, 3.25f + float(k % 4) * 0.25f, 4, 1, TextTone(k % 3), [&](const Mask& m, int x, int y) { blendMask(img, m, x, y, Color::hex(0xffffff)); });
    return img;
  };
  std::vector<Image> ref;
  for (int k = 0; k < 63; k++) ref.push_back(render(k));
  glyphCache().clear();
  std::vector<int> mismatches(4, 0);
  std::vector<std::thread> th;
  for (int t = 0; t < 4; t++)
    th.emplace_back([&, t] {
      for (int rep = 0; rep < 3; rep++)
        for (int k = 0; k < 63; k++) {
          int kk = (k * 7 + t * 13 + rep) % 63;
          if (render(kk).px != ref[size_t(kk)].px) mismatches[size_t(t)]++;
        }
    });
  for (auto& x : th) x.join();
  for (int m : mismatches) CHECK_EQ(m, 0);
}

TEST(gfx_text_perf_labels) {
  if (!fontsOk()) return;
  // 2000 коротких подписей (~12 символов) кеглем 13: раскладка + отрисовка после прогрева кеша.
  std::vector<std::string> labels;
  const char* names[] = {"Арден", "Валория", "Кирена", "Морвель", "Тарск", "Эльмир", "Дарн", "Озёрный", "Северск"};
  for (int i = 0; i < 2000; i++) labels.push_back(std::string(names[i % 9]) + " " + std::to_string(100 + i * 37 % 900));
  Image img(1920, 1080, premul(Color::hex(0x151a22)));
  Canvas c(img);
  TextStyle st = style(FontFamily::UI, FontWeight::Regular, 13);
  auto pass = [&] {
    for (int i = 0; i < 2000; i++) {
      float x = float((i % 12) * 158) + float(i % 4) * 0.25f, y = float((i / 12) % 60) * 18;
      drawText(c, labels[size_t(i)], st, x, y, Color::hex(0xece8df));
    }
  };
  pass();
  double best = 1e9;
  for (int rep = 0; rep < 5; rep++) {
    double t0 = nowSeconds();
    pass();
    best = std::min(best, (nowSeconds() - t0) * 1000);
  }
  // Отдельно — только раскладка.
  double t0 = nowSeconds();
  TextLayout L;
  for (int i = 0; i < 2000; i++) layoutTextInto(L, labels[size_t(i)], st, LayoutOptions{});
  double layoutMs = (nowSeconds() - t0) * 1000;
  // Масштаб интерфейса 125 %: растр под масштаб, отрисовка в пикселях устройства.
  c.save();
  c.scale(1.25f, 1.25f);
  pass();
  double t1 = nowSeconds();
  pass();
  double scaledMs = (nowSeconds() - t1) * 1000;
  c.restore();
  auto cs = glyphCache().stats();
  std::printf("  [текст] 2000 подписей: раскладка+отрисовка %.2f мс (раскладка %.2f мс, при масштабе 125 %% — %.2f мс); кеш глифов %zu шт., %zu КБ\n",
              best, layoutMs, scaledMs, cs.entries, cs.bytes / 1024);
#ifdef NDEBUG
  CHECK(best < rg::test::perf(10.0));
#endif
}

namespace {
// Габариты «чернил» (пикселей, отличных от фона).
RectI inkBounds(const Image& img, u32 bg) {
  int x0 = img.w, y0 = img.h, x1 = -1, y1 = -1;
  for (int y = 0; y < img.h; y++)
    for (int x = 0; x < img.w; x++)
      if (img.at(x, y) != bg) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
  return x1 < 0 ? RectI{} : RectI{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}
}  // namespace

TEST(gfx_text_box_alignment) {
  if (!fontsOk()) return;
  TextStyle st = style(FontFamily::UI, FontWeight::Regular, 14);
  u32 bg = premul(Color::hex(0xffffff));
  RectF box(20, 10, 200, 60);
  struct Case { Align a; VAlign v; };
  const Case cases[] = {{Align::Left, VAlign::Top}, {Align::Center, VAlign::Middle}, {Align::Right, VAlign::Bottom}};
  float w = measureText("Нива", st);
  FontMetrics m = metrics(st);
  for (const Case& cs : cases) {
    Image img(240, 80, bg);
    Canvas c(img);
    RectF r = drawTextBox(c, "Нива", st, box, Color::hex(0x000000), cs.a, cs.v);
    CHECK_NEAR(r.w, w, 1e-3);
    CHECK_NEAR(r.h, m.lineHeight, 1e-3);
    float ex = cs.a == Align::Left ? box.x : cs.a == Align::Center ? box.cx() - w / 2 : box.right() - w;
    float ey = cs.v == VAlign::Top ? box.y : cs.v == VAlign::Middle ? box.cy() - m.lineHeight / 2 : box.bottom() - m.lineHeight;
    CHECK_NEAR(r.x, ex, 1e-3);
    CHECK_NEAR(r.y, ey, 1e-3);
    RectI ink = inkBounds(img, bg);
    CHECK(!ink.empty());
    // Чернила (без выносных элементов): от базовой линии вверх на высоту прописных.
    float base = r.y + m.baseline;
    CHECK_NEAR(float(ink.bottom()), base, 1.5);
    CHECK_NEAR(float(ink.y), base - m.capHeight, 1.5);
    CHECK(float(ink.x) >= r.x - 1 && float(ink.right()) <= r.right() + 1);
  }
  // Перенос и предел строк по высоте прямоугольника при многоточии.
  Image img(240, 80, bg);
  Canvas c(img);
  RectF r = drawTextBox(c, "Гильдия мореходов держит половину торговых путей побережья", st, RectF(0, 0, 120, 2.5f * m.lineHeight),
                        Color::hex(0x000000), Align::Left, VAlign::Top, true, 0, true);
  CHECK_NEAR(r.h, 2 * m.lineHeight, 1e-3);
  CHECK(r.w <= 120.01f);
}

TEST(gfx_text_paths_and_scale) {
  if (!fontsOk()) return;
  TextStyle st = style(FontFamily::Display, FontWeight::Semibold, 24);
  TextLayout L = layoutText("Арден Вэл", st);
  Path p;
  layoutPath(L, 10, 20, p);
  CHECK(!p.empty());
  RectF b = p.bounds();
  FontMetrics m = metrics(st);
  CHECK(b.x >= 10 - 2 && b.x <= 10 + 4);
  CHECK_NEAR(b.right(), 10 + L.width, 4);
  CHECK(b.y >= 20 + m.baseline - m.ascent - 2);
  CHECK(b.bottom() <= 20 + m.baseline + m.descent + 2);
  // Растр при масштабе ×2 — вчетверо больше чернил.
  auto ink = [&](float scale) {
    Image img(600, 120, 0);
    rasterizeLayout(L, 5, 5, scale, TextTone::Mid, [&](const Mask& mk, int x, int y) { blendMask(img, mk, x, y, Color::hex(0xffffff)); });
    double s = 0;
    for (u32 px : img.px) s += px >> 24;
    return s / 255;
  };
  double i1 = ink(1), i2 = ink(2);
  CHECK(i1 > 50);
  CHECK_NEAR(i2, 4 * i1, 0.25 * 4 * i1);
  // Синтетический полужирный (у Display нет файла полужирного) — шире и темнее обычного.
  Synth syn;
  primaryFont(FontFamily::Display, FontWeight::Semibold, &syn);
  if (syn != Synth::None) {
    TextStyle reg = style(FontFamily::Display, FontWeight::Regular, 24);
    CHECK(measureText("Арден", st) > measureText("Арден", reg));
    TextLayout R = layoutText("Арден Вэл", reg);
    double ir = 0;
    Image img(600, 120, 0);
    rasterizeLayout(R, 5, 5, 1, TextTone::Mid, [&](const Mask& mk, int x, int y) { blendMask(img, mk, x, y, Color::hex(0xffffff)); });
    for (u32 px : img.px) ir += (px >> 24) / 255.0;
    CHECK(i1 > ir * 1.08);
  }
}

TEST(gfx_text_tones) {
  if (!fontsOk()) return;
  CHECK(toneOf(Color::hex(0x1d1a14)) == TextTone::Dark);
  CHECK(toneOf(Color::hex(0xece8df)) == TextTone::Light);
  CHECK(toneOf(Color::hex(0x6f7886)) == TextTone::Mid);
  // Светлый тон усиливает покрытие сильнее тёмного; полное покрытие не меняется.
  const Font* f = primaryFont(FontFamily::UI, FontWeight::Regular);
  u16 g = f->glyphIndex(0x0436);
  GlyphBitmap d = renderGlyph(*f, g, 13, 1, Synth::None, TextTone::Dark);
  GlyphBitmap l = renderGlyph(*f, g, 13, 1, Synth::None, TextTone::Light);
  CHECK_EQ(d.mask.w, l.mask.w);
  CHECK_EQ(d.mask.h, l.mask.h);
  double sd = 0, sl = 0;
  for (size_t i = 0; i < d.mask.a.size(); i++) {
    CHECK(l.mask.a[i] >= d.mask.a[i]);
    if (d.mask.a[i] == 255) CHECK_EQ(l.mask.a[i], 255);
    sd += d.mask.a[i];
    sl += l.mask.a[i];
  }
  CHECK(sl > sd);
  // Высота строчных на мелком кегле притянута к целому пикселю: верхняя строка «н» — сплошная.
  GlyphBitmap n = renderGlyph(*f, f->glyphIndex(0x043D), 13, 0, Synth::None, TextTone::Mid);
  CHECK_EQ(n.top + n.mask.h, 0);
  int row0 = 0, row1 = 0;
  for (int x = 0; x < n.mask.w; x++) { row0 += n.mask.row(0)[x]; row1 += n.mask.row(1)[x]; }
  CHECK(row0 * 10 >= row1 * 9);
}

TEST(gfx_text_draw_offscreen_and_degenerate) {
  if (!fontsOk()) return;
  Image img(64, 32, 0);
  Canvas c(img);
  TextStyle st = style(FontFamily::UI, FontWeight::Regular, 13);
  drawText(c, "Арден", st, -500, -500, Color::hex(0xffffff));
  drawText(c, "Арден", st, 1e9f, 5, Color::hex(0xffffff));
  drawText(c, "Арден", st, std::numeric_limits<float>::quiet_NaN(), 5, Color::hex(0xffffff));
  TextStyle bad = st;
  bad.size = std::numeric_limits<float>::infinity();
  drawText(c, "Арден", bad, 0, 0, Color::hex(0xffffff));
  bad.size = -3;
  drawText(c, "Арден", bad, 0, 0, Color::hex(0xffffff));
  bad.size = 3000;
  drawText(c, "А", bad, -100, -100, Color::hex(0xffffff));
  for (u32 p : img.px) (void)p;
  // Частично видимый текст рисуется без выхода за буфер.
  drawText(c, "Арден Валория", st, -20, 20, Color::hex(0xffffff));
  CHECK(inkBounds(img, 0).w > 10);
}
