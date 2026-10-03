// Тесты шрифтов: разбор TrueType/TTC, устойчивость к повреждённым файлам, точность растеризатора, утолщение, кеш.
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>

#include "gfx/text.h"
#include "tests/test.h"

using namespace rg;
using namespace rg::gfx;

namespace {

bool fontsOk() {
  static const bool ok = [] {
    std::string err;
    bool r = initFonts(&err);
    if (!r) std::printf("  [шрифты] системных шрифтов нет: %s\n", err.c_str());
    return r;
  }();
  return ok;
}

GlyphOutline rectOutline(float x0, float y0, float x1, float y1, bool cw) {
  GlyphOutline o;
  if (cw) o.pts = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
  else o.pts = {{x0, y0}, {x0, y1}, {x1, y1}, {x1, y0}};
  o.onCurve = {1, 1, 1, 1};
  o.contourEnds = {3};
  return o;
}

void appendOutline(GlyphOutline& dst, const GlyphOutline& src) {
  u32 base = u32(dst.pts.size());
  dst.pts.insert(dst.pts.end(), src.pts.begin(), src.pts.end());
  dst.onCurve.insert(dst.onCurve.end(), src.onCurve.begin(), src.onCurve.end());
  for (u32 e : src.contourEnds) dst.contourEnds.push_back(base + e);
}

double maskSum(const Mask& m) {
  double s = 0;
  for (u8 v : m.a) s += v;
  return s / 255.0;
}

int at(const GlyphBitmap& b, int x, int y) {
  int lx = x - b.left, ly = y - b.top;
  if (lx < 0 || ly < 0 || lx >= b.mask.w || ly >= b.mask.h) return 0;
  return b.mask.row(ly)[lx];
}

std::vector<std::filesystem::path> allFontFiles() {
  std::vector<std::filesystem::path> out;
  SystemFonts sys;
  if (!findSystemFonts(sys)) return out;
  for (const std::string& d : sys.dirs) {
    std::error_code ec;
    std::filesystem::path dir(reinterpret_cast<const char8_t*>(d.c_str()));
    std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (!it->is_regular_file(e2)) continue;
      auto ext = it->path().extension().u8string();
      std::string e(reinterpret_cast<const char*>(ext.data()), ext.size());
      for (char& c : e) c = char(std::tolower(u8(c)));
      if (e == ".ttf" || e == ".ttc" || e == ".otf" || e == ".otc") out.push_back(it->path());
    }
  }
  return out;
}

}  // namespace

TEST(gfx_text_raster_rect_exact) {
  // Прямоугольник (1,25; 0,5) — (3,75; 2,5): покрытие — произведение долей по осям.
  for (bool cw : {true, false}) {
    GlyphBitmap b = rasterizeOutline(rectOutline(1.25f, 0.5f, 3.75f, 2.5f, cw));
    CHECK_EQ(b.left, 1);
    CHECK_EQ(b.top, 0);
    CHECK_EQ(b.mask.w, 3);
    CHECK_EQ(b.mask.h, 3);
    const float fx[3] = {0.75f, 1.f, 0.75f}, fy[3] = {0.5f, 1.f, 0.5f};
    for (int y = 0; y < 3; y++)
      for (int x = 0; x < 3; x++) CHECK_NEAR(at(b, x + 1, y), fx[x] * fy[y] * 255, 1.0);
  }
}

TEST(gfx_text_raster_area_and_rules) {
  // Треугольник: сумма покрытия = площадь.
  GlyphOutline tri;
  tri.pts = {{0.3f, 0.2f}, {17.7f, 3.1f}, {6.2f, 13.9f}};
  tri.onCurve = {1, 1, 1};
  tri.contourEnds = {2};
  double area = 0.5 * std::fabs((17.7 - 0.3) * (13.9 - 0.2) - (6.2 - 0.3) * (3.1 - 0.2));
  CHECK_NEAR(maskSum(rasterizeOutline(tri).mask), area, area * 0.004);

  // Окружность из квадратичных дуг: площадь ≈ πr² (погрешность аппроксимации кривыми < 0,1 %).
  GlyphOutline circ;
  const int segs = 16;
  float r = 9.5f, cx = 10.3f, cy = 10.6f;
  for (int i = 0; i < segs; i++) {
    double a0 = 2 * kPi * i / segs, am = a0 + kPi / segs;
    double rc = r / std::cos(kPi / segs);
    circ.pts.push_back({float(cx + r * std::cos(a0)), float(cy + r * std::sin(a0))});
    circ.onCurve.push_back(1);
    circ.pts.push_back({float(cx + rc * std::cos(am)), float(cy + rc * std::sin(am))});
    circ.onCurve.push_back(0);
  }
  circ.contourEnds = {u32(circ.pts.size() - 1)};
  // Точная площадь фигуры из квадратичных кривых — по мелкой ломаной.
  double exact = 0;
  Pt prev = circ.pts[0];
  for (int i = 0; i < segs; i++) {
    Pt p0 = circ.pts[size_t(2 * i)], c = circ.pts[size_t(2 * i + 1)], p1 = circ.pts[size_t((2 * i + 2) % circ.pts.size())];
    for (int k = 1; k <= 400; k++) {
      double t = k / 400.0, mt = 1 - t;
      double x = mt * mt * p0.x + 2 * mt * t * c.x + t * t * p1.x, y = mt * mt * p0.y + 2 * mt * t * c.y + t * t * p1.y;
      exact += double(prev.x) * y - x * double(prev.y);
      prev = {float(x), float(y)};
    }
  }
  exact = std::fabs(exact) / 2;
  CHECK_NEAR(exact, kPi * r * r, kPi * r * r * 0.003);
  // Кривые аппроксимируются хордами с отклонением ≤ 0,02 px: недобор площади ≤ периметр × 0,02.
  double s = maskSum(rasterizeOutline(circ).mask);
  CHECK(s <= exact + 0.01);
  CHECK_NEAR(s, exact, 2 * kPi * r * 0.02);

  // Два перекрывающихся квадрата одного направления — насыщение, без «дыры».
  GlyphOutline two = rectOutline(0, 0, 6, 6, true);
  appendOutline(two, rectOutline(3, 3, 9, 9, true));
  GlyphBitmap b2 = rasterizeOutline(two);
  CHECK_EQ(at(b2, 4, 4), 255);
  CHECK_EQ(at(b2, 1, 1), 255);
  CHECK_EQ(at(b2, 7, 1), 0);
  // Квадрат с дыркой обратного направления.
  GlyphOutline holed = rectOutline(0, 0, 10, 10, true);
  appendOutline(holed, rectOutline(3, 3, 7, 7, false));
  GlyphBitmap b3 = rasterizeOutline(holed);
  CHECK_EQ(at(b3, 5, 5), 0);
  CHECK_EQ(at(b3, 1, 5), 255);
  CHECK_NEAR(maskSum(b3.mask), 100 - 16, 0.01);
}

TEST(gfx_text_raster_degenerate) {
  GlyphOutline empty;
  CHECK(rasterizeOutline(empty).mask.empty());
  GlyphOutline line;
  line.pts = {{1, 1}, {5, 5}};
  line.onCurve = {1, 1};
  line.contourEnds = {1};
  GlyphBitmap b = rasterizeOutline(line);
  CHECK_NEAR(maskSum(b.mask), 0, 1e-6);
  GlyphOutline nan = rectOutline(0, 0, std::numeric_limits<float>::quiet_NaN(), 3, true);
  CHECK(rasterizeOutline(nan).mask.empty());
  GlyphOutline huge = rectOutline(0, 0, 1e7f, 1e7f, true);
  CHECK(rasterizeOutline(huge).mask.empty());
}

TEST(gfx_text_embolden) {
  GlyphOutline o = rectOutline(10, 10, 20, 20, true);
  appendOutline(o, rectOutline(13, 13, 17, 17, false));
  o.embolden(1.f);
  RectF b = o.bounds();
  CHECK_NEAR(b.x, 9, 1e-4);
  CHECK_NEAR(b.y, 9, 1e-4);
  CHECK_NEAR(b.right(), 21, 1e-4);
  CHECK_NEAR(b.bottom(), 21, 1e-4);
  // Дырка сузилась: 4×4 → 2×2.
  double s = maskSum(rasterizeOutline(o).mask);
  CHECK_NEAR(s, 12 * 12 - 2 * 2, 0.01);
  // Противоположная ориентация всего контура даёт тот же результат.
  GlyphOutline o2 = rectOutline(10, 10, 20, 20, false);
  o2.embolden(1.f);
  CHECK_NEAR(o2.bounds().w, 12, 1e-4);
}

TEST(gfx_text_font_rejects_garbage) {
  auto mk = [](std::string s) { return std::make_shared<const std::string>(std::move(s)); };
  std::string err;
  CHECK(!Font::load(nullptr, 0, &err));
  CHECK(!Font::load(mk(""), 0, &err));
  CHECK(!err.empty());
  CHECK(!Font::load(mk("hello world, not a font at all"), 0));
  std::string ttc = "ttcf";
  ttc += std::string("\x00\x01\x00\x00\xff\xff\xff\xff", 8);
  CHECK(!Font::load(mk(ttc), 0));
  CHECK_EQ(Font::faceCount(ttc), 0);
  std::string sfnt("\x00\x01\x00\x00\x7f\xff", 6);
  CHECK(!Font::load(mk(sfnt), 0));
  CHECK(!Font::loadFile("нет/такого/файла.ttf", 0, &err));
  Rng rng(7);
  for (int i = 0; i < 200; i++) {
    std::string s(size_t(rng.range(0, 400)), '\0');
    for (char& c : s) c = char(rng.next());
    if (s.size() >= 4 && (i & 1)) { s[0] = 0; s[1] = 1; s[2] = 0; s[3] = 0; }
    auto f = Font::load(mk(s), 0);
    if (f) {
      GlyphOutline o;
      for (int g = 0; g < std::min(f->glyphCount(), 20); g++) f->outline(u16(g), o);
    }
  }
}

TEST(gfx_text_font_primary_faces) {
  if (!fontsOk()) return;
  for (int fam = 0; fam < 3; fam++)
    for (int w = 0; w < 3; w++) {
      Synth syn;
      const Font* f = primaryFont(FontFamily(fam), FontWeight(w), &syn);
      CHECK(f != nullptr);
      CHECK(hasCyrillic(*f));
      CHECK(f->unitsPerEm() >= 16);
      CHECK(f->ascender() > 0);
      CHECK(f->descender() < 0);
      CHECK(f->capHeight() > f->xHeight());
      CHECK(f->capHeight() < f->ascender());
      CHECK(!f->family().empty());
      // Контуры кириллицы и латиницы непусты, у пробела контура нет.
      GlyphOutline o;
      for (u32 cp : {u32(0x0416), u32(0x044F), u32('A'), u32('g'), u32('8')}) {
        CHECK(f->outline(f->glyphIndex(cp), o));
        CHECK(o.contourEnds.size() >= 1);
        RectF b = o.bounds();
        CHECK(b.w > 0 && b.h > 0);
      }
      CHECK(!f->outline(f->glyphIndex(' '), o));
      CHECK(f->advance(f->glyphIndex(' ')) > 0);
    }
  const Font* mono = primaryFont(FontFamily::Mono, FontWeight::Regular);
  CHECK_EQ(mono->advance(mono->glyphIndex('i')), mono->advance(mono->glyphIndex('W')));
  CHECK_EQ(mono->advance(mono->glyphIndex(0x0436)), mono->advance(mono->glyphIndex('.')));
  for (auto& line : fontReport()) std::printf("  [шрифты] %s\n", line.c_str());
}

TEST(gfx_text_font_kerning) {
  if (!fontsOk()) return;
  const Font* f = primaryFont(FontFamily::UI, FontWeight::Regular);
  if (!f->hasKerning()) {
    std::printf("  [шрифты] у %s нет кернинга — проверка пропущена\n", f->fullName().c_str());
    return;
  }
  // Классические пары с отрицательным кернингом.
  int av = f->kerning(f->glyphIndex('A'), f->glyphIndex('V'));
  int to = f->kerning(f->glyphIndex('T'), f->glyphIndex('o'));
  CHECK(av < 0 || to < 0);
  CHECK_EQ(f->kerning(f->glyphIndex('l'), f->glyphIndex('l')), 0);
  CHECK_EQ(f->kerning(0xFFFF, 0xFFFF), 0);
  // Измерение учитывает кернинг.
  TextStyle st;
  st.size = 40;
  float pair = measureText("AV", st), sep = measureText("A", st) + measureText("V", st);
  if (av < 0) CHECK(pair < sep - 0.5f);
}

TEST(gfx_text_font_parse_all_system) {
  if (!fontsOk()) return;
  auto files = allFontFiles();
  int loaded = 0, skipped = 0, faces = 0, ttc = 0;
  size_t glyphs = 0;
  double t0 = nowSeconds();
  for (auto& p : files) {
    auto u = p.u8string();
    std::string path(reinterpret_cast<const char*>(u.data()), u.size());
    std::string err;
    auto f0 = Font::loadFile(path, 0, &err);
    std::string_view data = f0 ? f0->data() : std::string_view();
    int n = f0 ? Font::faceCount(data) : 0;
    if (!f0) { skipped++; continue; }
    loaded++;
    if (n > 1) ttc++;
    for (int i = 0; i < n && i < 16; i++) {
      auto f = i == 0 ? f0 : Font::load(std::make_shared<const std::string>(data), i, &err);
      if (!f) continue;
      faces++;
      GlyphOutline o;
      int step = std::max(1, f->glyphCount() / 40);
      for (int g = 0; g < f->glyphCount(); g += step) {
        f->outline(u16(g), o);
        GlyphBitmap b = renderGlyph(*f, u16(g), 13, g & 3, Synth(g % 3), TextTone(g % 3));
        CHECK(b.mask.w < 400 && b.mask.h < 400);
        glyphs++;
      }
      for (u32 cp : {u32('A'), u32(0x0410), u32(0x2026), u32(0x1F600)}) {
        u16 g = f->glyphIndex(cp);
        CHECK(g < f->glyphCount() || g == 0);
      }
      f->kerning(f->glyphIndex('A'), f->glyphIndex('V'));
    }
  }
  std::printf("  [шрифты] файлов %zu: загружено %d (TTC %d, начертаний %d), пропущено %d (CFF и прочие); глифов %zu за %.0f мс\n",
              files.size(), loaded, ttc, faces, skipped, glyphs, (nowSeconds() - t0) * 1000);
  CHECK(loaded > 0);
}

TEST(gfx_text_font_ttc_faces) {
  if (!fontsOk()) return;
  // Начертания коллекций загружаются по номеру и различаются именами.
  for (auto& p : allFontFiles()) {
    auto ext = p.extension().u8string();
    if (ext != u8".ttc" && ext != u8".TTC") continue;
    auto u = p.u8string();
    auto f0 = Font::loadFile(std::string(reinterpret_cast<const char*>(u.data()), u.size()), 0);
    if (!f0) continue;
    std::string_view d = f0->data();
    int n = Font::faceCount(d);
    CHECK(n >= 1);
    auto shared = std::make_shared<const std::string>(d);
    std::set<std::string> names;
    for (int i = 0; i < n; i++) {
      auto f = Font::load(shared, i);
      if (f) names.insert(f->fullName());
    }
    CHECK(Font::load(shared, n) == nullptr);
    CHECK(Font::load(shared, -1) == nullptr);
    if (n > 1) CHECK(names.size() > 1);
  }
}

TEST(gfx_text_font_fuzz) {
  if (!fontsOk()) return;
  const Font* base = primaryFont(FontFamily::UI, FontWeight::Regular);
  std::string_view src = base->data();
  Rng rng(12345);
  int ok = 0;
  for (int iter = 0; iter < 120; iter++) {
    std::string s(src);
    int mode = iter % 4;
    if (mode == 0) {
      // Порча случайных байтов по всему файлу.
      for (int k = 0; k < 64; k++) s[size_t(rng.next() % s.size())] = char(rng.next());
    } else if (mode == 1) {
      // Порча каталога таблиц и заголовков.
      for (int k = 0; k < 24; k++) s[size_t(rng.next() % std::min<size_t>(s.size(), 600))] = char(rng.next());
    } else if (mode == 2) {
      s.resize(size_t(rng.next() % s.size()));
    } else {
      // Длинные серии 0xFF.
      size_t at = size_t(rng.next() % s.size());
      for (size_t k = at; k < std::min(s.size(), at + 512); k++) s[k] = char(0xFF);
    }
    auto f = Font::load(std::make_shared<const std::string>(std::move(s)), 0);
    if (!f) continue;
    ok++;
    GlyphOutline o;
    int n = f->glyphCount();
    for (int k = 0; k < 60; k++) {
      u16 g = u16(rng.next() % u64(std::max(1, n) + 10));
      f->outline(g, o);
      GlyphBitmap b = renderGlyph(*f, g, float(8 + k % 30), k & 3, Synth(k % 3), TextTone::Mid);
      (void)b;
      f->kerning(g, u16(rng.next()));
      f->glyphIndex(u32(rng.next() % 0x110000));
      f->advance(g);
    }
  }
  CHECK(ok > 0);
}

TEST(gfx_text_glyph_cache_lru) {
  if (!fontsOk()) return;
  const Font* f = primaryFont(FontFamily::UI, FontWeight::Regular);
  GlyphCache cache(48 * 1024);
  GlyphRequest r;
  r.font = f;
  r.size4 = 4 * 24;
  r.glyph = f->glyphIndex(0x0416);
  auto a = cache.get(r);
  auto b = cache.get(r);
  CHECK(a.get() == b.get());
  CHECK(!a->mask.empty());
  CHECK_EQ(cache.stats().hits, u64(1));
  for (u32 cp = 0x0410; cp < 0x0450; cp++) {
    r.glyph = f->glyphIndex(cp);
    for (u8 sx = 0; sx < 4; sx++) {
      r.subX = sx;
      cache.get(r);
    }
  }
  auto st = cache.stats();
  CHECK(st.bytes <= st.capacity);
  CHECK(st.entries < 64 * 4);
  CHECK(st.entries > 10);
  // Вытесненный растр остаётся живым у держателя.
  CHECK(!a->mask.empty());
  // Субпиксельные сдвиги дают разные растры, кегль 24 — разумного размера.
  r.glyph = f->glyphIndex('l');
  r.subX = 0;
  auto s0 = cache.get(r);
  r.subX = 2;
  auto s2 = cache.get(r);
  CHECK(s0->mask.a != s2->mask.a || s0->left != s2->left);
  CHECK(s0->mask.h >= 15 && s0->mask.h <= 22);
  // Пакетный запрос с повторами.
  std::vector<GlyphRequest> reqs(10, r);
  std::vector<std::shared_ptr<const GlyphBitmap>> out(reqs.size());
  cache.getMany(reqs.data(), reqs.size(), out.data());
  for (auto& o : out) CHECK(o.get() == out[0].get());
  cache.clear();
  CHECK_EQ(cache.stats().entries, size_t(0));
}

TEST(gfx_text_fontsys_weight_fallback) {
  if (!fontsOk()) return;
  namespace sfs = std::filesystem;
  SystemFonts sys;
  CHECK(findSystemFonts(sys));
  CHECK(!sys.dirs.empty());
  // Каталог только с обычными начертаниями интерфейсного и моноширинного шрифтов.
  std::string dir = test::outDir() + "/fonts-min";
  sfs::path d(reinterpret_cast<const char8_t*>(dir.c_str()));
  std::error_code ec;
  sfs::remove_all(d, ec);
  sfs::create_directories(d / "empty", ec);
  for (int fam : {0, 2}) {
    const std::string& p = sys.faces[fam][0].path;
    sfs::path src(reinterpret_cast<const char8_t*>(p.c_str()));
    sfs::copy_file(src, d / src.filename(), sfs::copy_options::overwrite_existing, ec);
  }
  SystemFonts mini;
  std::string err;
  CHECK(findFontsIn({dir}, mini, &err));
  const Font* ui = mini.faces[0][0].font.get();
  CHECK(ui != nullptr);
  CHECK_EQ(ui->family(), sys.faces[0][0].font->family());
  CHECK(mini.faces[0][0].synth == Synth::None);
  bool ttc = Font::faceCount(ui->data()) > 1;
  if (!ttc) {
    // Нет файлов полужирного и жирного — синтетическое утолщение того же шрифта.
    CHECK(mini.faces[0][1].synth == Synth::Semibold && mini.faces[0][1].font.get() == ui);
    CHECK(mini.faces[0][2].synth == Synth::Bold && mini.faces[0][2].font.get() == ui);
  }
  // Нет шрифта с засечками — интерфейсный.
  CHECK_EQ(mini.faces[1][0].path, mini.faces[0][0].path);
  const Font* mono = mini.faces[2][0].font.get();
  CHECK(mono != nullptr);
  if (sys.faces[2][0].path != sys.faces[0][0].path) {
    CHECK(mono->monospace());
    CHECK(mini.faces[2][2].synth != Synth::None || Font::faceCount(mono->data()) > 1);
    // Синтетический жирный моноширинного не ломает сетку.
    CHECK_EQ(synthAdvance(*mono, Synth::Bold, 13), 0.f);
  }
  CHECK(synthAdvance(*ui, Synth::Bold, 13) > 0);
  // Пустой каталог — понятная ошибка.
  SystemFonts none;
  std::string err2;
  CHECK(!findFontsIn({dir + "/empty"}, none, &err2));
  CHECK(startsWith(err2, "Не найден"));
  CHECK(!systemFontDirs().empty());
  // Копии системных шрифтов не оставляем среди артефактов.
  for (auto& f : mini.faces) for (auto& w : f) w.font.reset();
  sfs::remove_all(d, ec);
}

TEST(gfx_text_synthetic_bold_raster) {
  if (!fontsOk()) return;
  const Font* f = primaryFont(FontFamily::UI, FontWeight::Regular);
  for (float size : {13.f, 24.f, 48.f}) {
    u16 g = f->glyphIndex(0x0428);  // Ш
    GlyphBitmap r = renderGlyph(*f, g, size, 0, Synth::None, TextTone::Mid);
    GlyphBitmap s = renderGlyph(*f, g, size, 0, Synth::Semibold, TextTone::Mid);
    GlyphBitmap b = renderGlyph(*f, g, size, 0, Synth::Bold, TextTone::Mid);
    double ir = maskSum(r.mask), is = maskSum(s.mask), ib = maskSum(b.mask);
    CHECK(is > ir * 1.05);
    CHECK(ib > is * 1.05);
    // Левый край на месте (утолщение сдвинуто вправо), высота прописной не растёт на мелком кегле.
    CHECK(std::abs(b.left - r.left) <= 1);
    if (size <= 24) CHECK_EQ(b.top, r.top);
  }
}
