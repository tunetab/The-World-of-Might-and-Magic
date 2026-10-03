// Тесты раскладки текста: метрики, измерение, перенос, многоточие, табуляция, особые пробелы, каретка и выделение.
#include <cstdio>

#include "gfx/text.h"
#include "tests/test.h"

using namespace rg;
using namespace rg::gfx;

namespace {

bool fontsOk() {
  static const bool ok = initFonts(nullptr);
  return ok;
}

TextStyle style(FontFamily f = FontFamily::UI, FontWeight w = FontWeight::Regular, float size = 13) {
  TextStyle s;
  s.family = f;
  s.weight = w;
  s.size = size;
  return s;
}

// Текст строки раскладки (по байтовому диапазону).
std::string lineText(std::string_view t, const TextLine& ln) { return std::string(t.substr(ln.begin, ln.end - ln.begin)); }

std::string visibleText(std::string_view t, const TextLayout& L) {
  std::string s;
  for (auto& g : L.glyphs) {
    if (g.kind == GlyphKind::Ellipsis) { s += "\xE2\x80\xA6"; continue; }
    if (g.kind == GlyphKind::Hyphen) { s += "-"; continue; }
    size_t i = g.byte;
    u32 cp = utf8::decode(t, i);
    if (cp != 0xAD) utf8::append(s, cp);
  }
  return s;
}

const char* kPara =
    "Королевство Арден раскинулось между Серыми горами и Тёплым морем. Его провинции славятся виноградниками, "
    "а гильдия мореходов держит половину торговых путей северо-западного побережья.";

}  // namespace

TEST(gfx_text_metrics) {
  if (!fontsOk()) return;
  for (int fam = 0; fam < 3; fam++) {
    TextStyle st = style(FontFamily(fam), FontWeight::Regular, 20);
    FontMetrics m = metrics(st);
    CHECK(m.ascent > 10 && m.ascent < 30);
    CHECK(m.descent > 2 && m.descent < 12);
    CHECK_NEAR(m.lineHeight, m.ascent + m.descent + m.lineGap, 1e-4);
    CHECK(m.capHeight > m.xHeight && m.capHeight < m.ascent);
    CHECK(m.xHeight > 5);
    CHECK(m.baseline >= m.ascent - 1e-4);
    // Метрики пропорциональны кеглю.
    FontMetrics m2 = metrics(style(FontFamily(fam), FontWeight::Regular, 40));
    CHECK_NEAR(m2.ascent, m.ascent * 2, 1e-3);
    // Заданный множитель межстрочного расстояния; базовая линия по центру добавки.
    st.lineHeight = 1.5f;
    FontMetrics m3 = metrics(st);
    CHECK_NEAR(m3.lineHeight, 30, 1e-4);
    CHECK_NEAR(m3.baseline, (30 - (m3.ascent + m3.descent)) / 2 + m3.ascent, 1e-4);
  }
}

TEST(gfx_text_measure) {
  if (!fontsOk()) return;
  TextStyle st = style();
  CHECK_EQ(measureText("", st), 0.f);
  float w1 = measureText("Провинция", st);
  CHECK(w1 > 40 && w1 < 90);
  // Линейность по кеглю (кегль квантуется до ¼ px).
  CHECK_NEAR(measureText("Провинция", style(FontFamily::UI, FontWeight::Regular, 26)), w1 * 2, 0.05);
  // Хвостовые пробелы не входят в ширину, ведущие — входят.
  CHECK_NEAR(measureText("Арден   ", st), measureText("Арден", st), 1e-4);
  CHECK(measureText("  Арден", st) > measureText("Арден", st) + 3);
  // Несколько строк — самая широкая.
  CHECK_NEAR(measureText("Ар\nКоролевство\nдо", st), measureText("Королевство", st), 1e-4);
  // Межбуквенное расстояние: + n × ls.
  TextStyle sp = st;
  sp.letterSpacing = 2;
  CHECK_NEAR(measureText("Арден", sp), measureText("Арден", st) + 10, 1e-3);
  // Жирное шире обычного.
  CHECK(measureText("Королевство", style(FontFamily::UI, FontWeight::Bold)) > measureText("Королевство", st));
  CHECK(measureText("Королевство", style(FontFamily::UI, FontWeight::Semibold)) >= measureText("Королевство", st));
  // Моноширинный: одинаковая ширина строк одной длины.
  TextStyle mono = style(FontFamily::Mono);
  CHECK_NEAR(measureText("iiiiii", mono), measureText("ЖЖЖЖЖЖ", mono), 1e-3);
  // Совпадение measureText и раскладки.
  TextLayout L = layoutText("Доход 12 345 золота", st);
  CHECK_NEAR(L.width, measureText("Доход 12 345 золота", st), 1e-4);
  CHECK_EQ(L.lines.size(), size_t(1));
  CHECK_NEAR(L.height, metrics(st).lineHeight, 1e-4);
}

TEST(gfx_text_special_spaces) {
  if (!fontsOk()) return;
  TextStyle st = style();
  float sp = measureText("a b", st) - measureText("ab", st);
  float nb = measureText("a\xC2\xA0" "b", st) - measureText("ab", st);
  float thin = measureText("a\xE2\x80\xAF" "b", st) - measureText("ab", st);
  float thin2 = measureText("a\xE2\x80\x89" "b", st) - measureText("ab", st);
  float zw = measureText("a\xE2\x80\x8B" "b", st) - measureText("ab", st);
  CHECK(sp > 2);
  CHECK_NEAR(nb, sp, 0.6);
  CHECK(thin > 0.5f && thin < sp + 0.01f);
  CHECK(thin2 > 0.5f && thin2 < sp + 0.01f);
  CHECK_NEAR(zw, 0, 0.6);
  // Числа fmtNum с узким неразрывным пробелом не переносятся.
  std::string num = fmtNum(1234567);
  TextLayout L = layoutText("Казна: " + num, st, std::max(measureText("Казна: 1", st), measureText(num, st)) + 1);
  bool numberWhole = false;
  for (auto& ln : L.lines) numberWhole |= lineText("Казна: " + num, ln) == num;
  CHECK(numberWhole);
  // Невидимые символы не рисуются, но занимают позицию каретки.
  TextLayout Z = layoutText("a\xE2\x80\x8D\xEF\xBB\xBF" "b", st);
  CHECK_EQ(Z.glyphs.size(), size_t(4));
  CHECK(Z.glyphs[1].kind == GlyphKind::Invisible && Z.glyphs[2].kind == GlyphKind::Invisible);
}

TEST(gfx_text_wrap_words) {
  if (!fontsOk()) return;
  TextStyle st = style();
  // Самое широкое слово: при ширине не меньше него слова не разрываются.
  float widest = 0;
  for (auto& w : split(kPara, ' ')) widest = std::max(widest, measureText(w, st));
  for (float maxW : {widest + 0.5f, 140.f, 200.f, 333.f}) {
    if (maxW < widest) continue;
    TextLayout L = layoutText(kPara, st, maxW);
    CHECK(L.lines.size() > 1);
    CHECK(!L.truncated);
    std::string joined;
    for (size_t i = 0; i < L.lines.size(); i++) {
      const TextLine& ln = L.lines[i];
      CHECK(ln.width <= maxW + 0.01f);
      CHECK_EQ(ln.begin, i == 0 ? 0u : L.lines[i - 1].next);
      std::string t = lineText(kPara, ln);
      joined += t;
      // Перенос — после пробела или дефиса, а не внутри слова.
      if (i + 1 < L.lines.size()) {
        char last = t.empty() ? ' ' : t.back();
        CHECK_MSG(last == ' ' || last == '-', t);
      }
      CHECK_NEAR(ln.y, float(i) * L.lineHeight, 1e-3);
    }
    CHECK_EQ(joined, std::string(kPara));
    CHECK_NEAR(L.height, float(L.lines.size()) * L.lineHeight, 1e-3);
  }
  // Дефис: «северо-западного» переносится после дефиса при подходящей ширине.
  std::string t = "побережья северо-западного";
  float w = measureText("побережья северо-", st) + 1;
  TextLayout H = layoutText(t, st, w);
  CHECK_EQ(H.lines.size(), size_t(2));
  CHECK_EQ(lineText(t, H.lines[0]), std::string("побережья северо-"));
}

TEST(gfx_text_wrap_long_words_and_newlines) {
  if (!fontsOk()) return;
  TextStyle st = style();
  std::string w = "Превысокомногорассмотрительствующий";
  TextLayout L = layoutText(w, st, 60);
  CHECK(L.lines.size() >= 3);
  std::string joined;
  for (auto& ln : L.lines) {
    CHECK(ln.width <= 60.01f || ln.glyphEnd - ln.glyphBegin == 1);
    CHECK(ln.glyphEnd > ln.glyphBegin);
    joined += lineText(w, ln);
  }
  CHECK_EQ(joined, w);
  // Очень узко: по символу в строке, без бесконечного цикла.
  TextLayout N = layoutText("Абв", st, 0.5f);
  CHECK_EQ(N.lines.size(), size_t(3));
  // Переводы строк: \n, \r\n, U+2028; пустые строки сохраняются.
  std::string t = "Первая\nВторая\r\nТретья\xE2\x80\xA8\n";
  TextLayout M = layoutText(t, st);
  CHECK_EQ(M.lines.size(), size_t(5));
  CHECK_EQ(lineText(t, M.lines[0]), std::string("Первая"));
  CHECK_EQ(lineText(t, M.lines[1]), std::string("Вторая"));
  CHECK_EQ(lineText(t, M.lines[2]), std::string("Третья"));
  CHECK_EQ(lineText(t, M.lines[3]), std::string(""));
  CHECK_EQ(M.lines[4].begin, u32(t.size()));
  CHECK(M.lines[0].hardBreak && M.lines[1].hardBreak);
  CHECK_EQ(M.lines[1].next, M.lines[2].begin);
  // Пустой текст — одна пустая строка (для каретки).
  TextLayout E = layoutText("", st);
  CHECK_EQ(E.lines.size(), size_t(1));
  CHECK(E.height > 0);
  CHECK_EQ(E.width, 0.f);
}

TEST(gfx_text_soft_hyphen) {
  if (!fontsOk()) return;
  TextStyle st = style();
  std::string t = "Гос\xC2\xAD" "ударство";
  CHECK_NEAR(measureText(t, st), measureText("Государство", st), 0.5);
  float w = std::max(measureText("Гос-", st), measureText("ударство", st)) + 2;
  TextLayout L = layoutText(t, st, w);
  CHECK_EQ(L.lines.size(), size_t(2));
  CHECK(L.glyphs[L.lines[0].glyphEnd - 1].kind == GlyphKind::Hyphen);
  CHECK_EQ(visibleText(t, L), std::string("Гос-ударство"));
  // Без переноса мягкий дефис невидим.
  TextLayout one = layoutText(t, st);
  CHECK_EQ(visibleText(t, one), std::string("Государство"));
}

TEST(gfx_text_ellipsis) {
  if (!fontsOk()) return;
  TextStyle st = style();
  // Одна строка: обрезка по символам.
  float maxW = 120;
  TextLayout L = layoutText(kPara, st, maxW, 1, true);
  CHECK_EQ(L.lines.size(), size_t(1));
  CHECK(L.truncated);
  CHECK(L.lines[0].width <= maxW + 0.01f);
  CHECK(L.glyphs.back().kind == GlyphKind::Ellipsis);
  std::string vis = visibleText(kPara, L);
  CHECK(endsWith(vis, "\xE2\x80\xA6"));
  CHECK(startsWith(kPara, vis.substr(0, vis.size() - 3)));
  CHECK(vis.substr(vis.size() - 4, 1) != " ");
  // Помещается — без многоточия.
  TextLayout F = layoutText("Арден", st, 200, 1, true);
  CHECK(!F.truncated);
  CHECK(F.glyphs.back().kind == GlyphKind::Normal);
  // Несколько строк: последняя строка обрезана с многоточием.
  TextLayout M = layoutText(kPara, st, 160, 3, true);
  CHECK_EQ(M.lines.size(), size_t(3));
  CHECK(M.truncated);
  for (auto& ln : M.lines) CHECK(ln.width <= 160.01f);
  CHECK(M.glyphs.back().kind == GlyphKind::Ellipsis);
  // Без многоточия: просто обрезка по строкам.
  TextLayout C = layoutText(kPara, st, 160, 2, false);
  CHECK_EQ(C.lines.size(), size_t(2));
  CHECK(C.truncated);
  CHECK(C.glyphs.back().kind != GlyphKind::Ellipsis);
  // Без переноса: каждая строка обрезается отдельно.
  LayoutOptions o;
  o.maxWidth = 70;
  o.wrap = false;
  o.ellipsis = true;
  TextLayout P = layoutText("Первая длинная строка\nВторая длинная строка\nОк", st, o);
  CHECK_EQ(P.lines.size(), size_t(3));
  CHECK(P.truncated);
  for (auto& ln : P.lines) CHECK(ln.width <= 70.01f);
  CHECK(P.glyphs[P.lines[0].glyphEnd - 1].kind == GlyphKind::Ellipsis);
  CHECK(P.glyphs[P.lines[2].glyphEnd - 1].kind == GlyphKind::Normal);
  CHECK_EQ(P.lines[0].next, P.lines[1].begin);
  // Строки по переводам с maxLines и без ширины: многоточие в конце последней показанной.
  TextLayout Q = layoutText("один\nдва\nтри", st, 0, 2, true);
  CHECK_EQ(Q.lines.size(), size_t(2));
  CHECK(Q.glyphs.back().kind == GlyphKind::Ellipsis);
  // ellipsize.
  std::string e = ellipsize(kPara, st, 150);
  CHECK(endsWith(e, "\xE2\x80\xA6"));
  CHECK(measureText(e, st) <= 150.01f);
  CHECK_EQ(ellipsize("Арден", st, 200), std::string("Арден"));
  // Крайний случай: ширина меньше многоточия.
  TextLayout T = layoutText("Королевство", st, 2, 1, true);
  CHECK_EQ(T.lines.size(), size_t(1));
  CHECK(T.glyphs.size() >= 1);
}

TEST(gfx_text_tabs_and_align) {
  if (!fontsOk()) return;
  TextStyle st = style(FontFamily::Mono);
  TextLayout L = layoutText("a\tb\nabc\td", st);
  CHECK_EQ(L.lines.size(), size_t(2));
  // Табуляция выравнивает следующий символ по одной позиции.
  float xb = 0, xd = 0;
  for (auto& g : L.glyphs) {
    if (g.byte == 2) xb = g.x;
    if (g.byte == 8) xd = g.x;
  }
  CHECK(xb > 0);
  CHECK_NEAR(xb, xd, 1e-3);
  // Выравнивание по центру и вправо внутри ширины.
  TextStyle ui = style();
  LayoutOptions o;
  o.maxWidth = 300;
  o.align = Align::Center;
  TextLayout C = layoutText("Арден", ui, o);
  float w = measureText("Арден", ui);
  CHECK_NEAR(C.lines[0].x, (300 - w) / 2, 1e-3);
  CHECK_NEAR(C.glyphs[0].x, (300 - w) / 2, 1e-3);
  o.align = Align::Right;
  TextLayout R = layoutText("Арден", ui, o);
  CHECK_NEAR(R.lines[0].x + R.lines[0].width, 300, 1e-3);
  CHECK_NEAR(R.boxWidth, 300, 1e-6);
}

TEST(gfx_text_caret_hit_roundtrip) {
  if (!fontsOk()) return;
  TextStyle st = style();
  std::string t = std::string(kPara) + "\nНовая строка — «кавычки», числа 1 234 и ё.\n";
  for (float maxW : {0.f, 150.f, 260.f}) {
    TextLayout L = layoutText(t, st, maxW);
    size_t checked = 0;
    for (size_t b = 0;; b = utf8::next(t, b)) {
      Pt p = caretPos(L, b);
      size_t h = hitTest(L, p.x + 0.01f, p.y + L.lineHeight * 0.5f);
      CHECK_MSG(h == b, strf("maxW %.0f: byte %zu -> (%.2f, %.2f) -> %zu", maxW, b, p.x, p.y, h));
      checked++;
      if (b >= t.size()) break;
    }
    CHECK(checked == utf8::count(t) + 1);
    // Каретка монотонна внутри строки.
    for (auto& ln : L.lines) {
      float prev = -1;
      for (size_t b = ln.begin; b <= ln.end && b <= t.size(); b = utf8::next(t, b)) {
        Pt p = caretPos(L, b);
        if (b < ln.end || ln.hardBreak || &ln == &L.lines.back()) {
          CHECK(p.x >= prev - 1e-4f);
          prev = p.x;
        }
        if (b >= t.size()) break;
      }
    }
  }
  // Попадание за пределами: левее — начало строки, правее — конец, выше/ниже — крайние строки.
  TextLayout L = layoutText("Первая\nВторая", st);
  CHECK_EQ(hitTest(L, -10, 3), size_t(0));
  CHECK_EQ(hitTest(L, 1000, 3), size_t(12));
  CHECK_EQ(hitTest(L, 1000, -50), size_t(12));
  CHECK_EQ(hitTest(L, 1000, 500), size_t(25));
  CHECK_EQ(lineOf(L, 12), size_t(0));
  CHECK_EQ(lineOf(L, 13), size_t(1));
  CHECK_NEAR(caretPos(L, 13).y, L.lineHeight, 1e-4);
}

TEST(gfx_text_selection_rects) {
  if (!fontsOk()) return;
  TextStyle st = style();
  std::string t = "Первая строка\nВторая строка\nТретья";
  TextLayout L = layoutText(t, st);
  CHECK(selectionRects(L, 5, 5).empty());
  auto one = selectionRects(L, 0, 12);
  CHECK_EQ(one.size(), size_t(1));
  CHECK_NEAR(one[0].x, 0, 1e-4);
  CHECK_NEAR(one[0].w, caretPos(L, 12).x, 1e-4);
  // Через две строки: хвост перевода строки у первой; обратный порядок концов — тот же результат.
  auto two = selectionRects(L, 13, 30);
  CHECK_EQ(two.size(), size_t(2));
  CHECK(two[0].right() > caretPos(L, 13).x);
  CHECK_NEAR(two[1].y, L.lineHeight, 1e-4);
  auto rev = selectionRects(L, 30, 13);
  CHECK_EQ(rev.size(), two.size());
  auto all = selectionRects(L, 0, t.size());
  CHECK_EQ(all.size(), size_t(3));
  // Перенос по словам: каждая строка — свой прямоугольник.
  TextLayout W = layoutText(kPara, st, 150);
  auto ws = selectionRects(W, 0, std::string_view(kPara).size());
  CHECK_EQ(ws.size(), W.lines.size());
}

TEST(gfx_text_fallback_glyphs) {
  if (!fontsOk()) return;
  TextStyle st = style();
  // Символы вне основного шрифта ищутся в запасных; совсем неизвестные — .notdef основного.
  std::string t = "\xE2\x9A\x94 \xE2\x98\x85 \xE2\x86\x92";  // ⚔ ★ →
  TextLayout L = layoutText(t, st);
  int visible = 0;
  for (auto& g : L.glyphs)
    if (g.kind == GlyphKind::Normal) {
      visible++;
      CHECK(g.advance > 0);
    }
  CHECK_EQ(visible, 3);
  TextLayout U = layoutText("\xF3\xB0\x80\x80", st);  // U+F0000 (частная область)
  CHECK_EQ(U.glyphs.size(), size_t(1));
  CHECK(U.glyphs[0].advance > 0);
  // Некорректный UTF-8 не роняет раскладку.
  std::string bad = "Ар\xFF\xFE\xC0ден\xE2\x82";
  TextLayout B = layoutText(bad, st, 30);
  CHECK(!B.lines.empty());
  CHECK(measureText(bad, st) > 0);
}
