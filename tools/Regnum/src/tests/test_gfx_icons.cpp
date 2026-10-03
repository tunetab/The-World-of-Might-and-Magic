// Regnum — значки: полнота реестра, корректность глифов, центровка, «?» для неизвестных, кеш, скорость,
// наглядные листы (тёмный и светлый фон, золотой акцент, увеличенные пиксели).
#include <unordered_set>

#include "base/fs.h"
#include "gfx/emblems.h"
#include "gfx/icons.h"
#include "tests/test_gfx_icons_util.h"

using namespace rg;
using namespace rg::gfx;
using namespace rg::test::icons;

namespace {

const char* const kRequired[] = {
    // общие
    "logo", "menu", "close", "check", "check-circle", "plus", "minus", "trash", "edit", "copy", "duplicate", "search",
    "filter", "sort", "sort-asc", "sort-desc", "settings", "sliders", "info", "warning", "error", "help", "undo", "redo",
    "save", "save-as", "folder", "folder-open", "file", "file-new", "archive", "download", "upload", "export", "import",
    "eye", "eye-off", "lock", "unlock", "pin", "link", "unlink", "chevron-left", "chevron-right", "chevron-up",
    "chevron-down", "arrow-left", "arrow-right", "arrow-up", "arrow-down", "external", "more-h", "more-v", "grip",
    "expand", "collapse", "maximize", "minimize", "fullscreen", "fullscreen-exit", "sun", "moon", "keyboard", "palette",
    "image", "zoom-in", "zoom-out", "zoom-fit", "target", "crosshair", "ruler", "layers", "grid", "magnet", "history",
    "clock", "calendar", "hourglass", "play", "next-turn", "refresh", "star", "star-filled", "heart", "user", "users",
    "user-plus", "bolt", "repeat", "note", "list", "table", "tag", "globe", "compass", "map", "map-pin", "home",
    "command", "dice", "bell",
    // предметные
    "crown", "flag", "banner", "castle", "tower", "shield", "sword", "swords", "war", "battle", "skull", "army", "fleet",
    "anchor", "horse", "bow", "staff", "wand", "sparkles", "book", "scroll", "quill", "coins", "treasury", "scales",
    "trade", "handshake", "alliance", "status-quo", "unknown", "diplomacy", "tribute", "reparations", "guild", "hq",
    "route", "province", "island", "sea", "land", "mountain", "tech-tree", "tech", "research", "building", "build",
    "hammer", "pickaxe", "factory", "house", "slots", "build-cost", "population", "contentment", "discontent",
    "rebellion", "religion", "culture", "race", "trade-value", "resource", "grain", "wood", "stone", "iron", "gem",
    "income", "expense", "percent", "trend-up", "trend-down", "army-upkeep", "fleet-upkeep", "mode-political",
    "mode-guilds", "mode-terrain", "chart-pie", "chart-bar", "occupied", "split", "merge", "disband", "retreat",
    "dissolve", "commander", "hero", "lord", "council", "ruler", "character", "portrait", "capital", "journal",
    "chronicle", "sea-province",
    // величины, поселения, постройки
    "size-s", "size-m", "size-l", "c-outpost", "c-village", "c-town", "c-city", "b-military", "b-economic",
    "b-industrial", "b-residential",
    // отряды и корабли
    "u-light-inf", "u-medium-inf", "u-heavy-inf", "u-light-cav", "u-medium-cav", "u-heavy-cav", "u-flying", "u-casters",
    "u-ranged", "u-beasts", "u-monsters", "u-machines", "s-ship-line", "s-frigate", "s-galleon",
    // инструменты
    "tool-select", "tool-pan", "tool-edit", "tool-polygon", "tool-polygon-plus", "tool-polygon-minus", "tool-lasso",
    "tool-knife", "tool-merge", "tool-fill", "tool-delete-province", "tool-army", "tool-fleet", "tool-route",
};

const Color kDarkBg = Color::hex(0x151a22);
const Color kDarkFg = Color::hex(0xece8df);
const Color kLightBg = Color::hex(0xf7f5f0);
const Color kLightFg = Color::hex(0x1f2633);
const Color kGold = Color::hex(0xd9a441);

// Значок на прозрачном холсте (для измерений).
Image renderAlone(std::string_view name, int px, int pad = 4) {
  Image img(px + 2 * pad, px + 2 * pad, 0);
  Canvas c(img);
  drawIcon(c, name, {float(pad), float(pad), float(px), float(px)}, Color(255, 255, 255));
  return img;
}

// Лист: имя и значок на 16, 20, 24 и 48 px.
void sheet(const std::vector<std::string>& names, size_t from, size_t to, Color bg, Color fg, Color accent,
           const std::string& file) {
  const int cols = 6, cw = 214, ch = 86;
  const int rows = int((to - from + cols - 1) / cols);
  Image img(cols * cw + 16, rows * ch + 16);
  Canvas c(img);
  c.clear(bg);
  for (size_t i = from; i < to; i++) {
    const int k = int(i - from);
    const float x = 8.f + float(k % cols) * cw, y = 8.f + float(k / cols) * ch;
    const Color col = (k % 5 == 4) ? accent : fg;
    label(c, names[i], x + 4, y + 2, fg.alpha(0.6f), 11);
    float ix = x + 4;
    for (int s : {16, 20, 24}) {
      drawIcon(c, names[i], {ix, y + 40, float(s), float(s)}, col);
      ix += float(s) + 12;
    }
    drawIcon(c, names[i], {ix + 6, y + 22, 48, 48}, col);
  }
  savePng(img, file);
}

// Увеличенный лист пикселей: значки на 16, 18, 20, 24 px, ×4.
void zoomSheet(const std::vector<std::string>& names, size_t from, size_t to, const std::string& file) {
  const int cols = 8, cw = 98, ch = 28;
  const int rows = int((to - from + cols - 1) / cols);
  Image img(cols * cw + 4, rows * ch + 4);
  Canvas c(img);
  c.clear(kDarkBg);
  for (size_t i = from; i < to; i++) {
    const int k = int(i - from);
    const float x = 2.f + float(k % cols) * cw, y = 2.f + float(k / cols) * ch;
    float ix = x + 1;
    for (int s : {16, 18, 20, 24}) {
      drawIcon(c, names[i], {ix, y + 2, float(s), float(s)}, kDarkFg);
      ix += float(s) + 2;
    }
  }
  savePng(zoomed(img, 3), file);
}

}  // namespace

TEST(gfx_icons_registry_complete) {
  std::vector<std::string> missing;
  for (const char* n : kRequired)
    if (!hasIcon(n)) missing.push_back(n);
  CHECK_MSG(missing.empty(), "нет значков: " + join(missing, ", "));
  const auto& names = iconNames();
  std::unordered_set<std::string> uniq(names.begin(), names.end());
  CHECK_EQ(uniq.size(), names.size());
  CHECK(names.size() >= std::size(kRequired));
  const auto issues = iconRegistryIssues();
  CHECK_MSG(issues.empty(), join(issues, "\n"));
  CHECK(!hasIcon("no-such-icon"));
  CHECK(!hasIcon(""));
}

TEST(gfx_icons_render_inside_and_centered) {
  std::vector<std::string> bad;
  for (const std::string& n : iconNames()) {
    for (int px : {16, 24, 48}) {
      const int pad = 6;
      const Image img = renderAlone(n, px, pad);
      const Coverage cv = coverage(img);
      if (!cv.any() || cv.sum < px * 0.6) { bad.push_back(n + ": пусто на " + std::to_string(px)); continue; }
      // Внутри квадрата значка (обводка может касаться края).
      const int tol = px >= 48 ? 2 : 1;
      if (cv.x0 < pad - tol || cv.y0 < pad - tol || cv.x1 > pad + px - 1 + tol || cv.y1 > pad + px - 1 + tol)
        bad.push_back(n + ": выход за квадрат на " + std::to_string(px));
      // Габариты центрированы (допуск — восьмая часть стороны), значок заполняет сетку.
      const double bx = (cv.x0 + cv.x1 + 1) * 0.5 - pad - px * 0.5, by = (cv.y0 + cv.y1 + 1) * 0.5 - pad - px * 0.5;
      if (px == 48 && (std::fabs(bx) > px / 8.0 || std::fabs(by) > px / 8.0))
        bad.push_back(n + ": смещён от центра (" + fmtNum(bx, 1) + ", " + fmtNum(by, 1) + ")");
      const int span = std::max(cv.x1 - cv.x0 + 1, cv.y1 - cv.y0 + 1);
      if (px == 48 && span < px * 0.55) bad.push_back(n + ": слишком мелкий (" + std::to_string(span) + " px)");
    }
  }
  CHECK_MSG(bad.empty(), join(bad, "\n"));
}

TEST(gfx_icons_crisp_axis_strokes) {
  // Горизонтальные и вертикальные штрихи на 16/18/20/24 px ложатся в целые пиксели: поперёк штриха нет
  // полупрозрачных «размытых» пикселей (только почти пустые или почти полные).
  auto sharp = [](const Image& img, int x0, int y0, int dx, int dy, int n, std::string& why) {
    int solid = 0;
    for (int i = 0; i < n; i++) {
      const u32 a = img.at(x0 + i * dx, y0 + i * dy) >> 24;
      if (a >= 215) solid++;
      else if (a > 40) { why = "альфа " + std::to_string(a) + " в позиции " + std::to_string(i); return false; }
    }
    if (solid == 0) { why = "нет сплошных пикселей"; return false; }
    return true;
  };
  for (int px : {16, 18, 20, 24}) {
    const int pad = 4;
    std::string why;
    const Image minus = renderAlone("minus", px, pad);
    CHECK_MSG(sharp(minus, pad + px / 2, 0, 0, 1, minus.h, why), "minus " + std::to_string(px) + ": " + why);
    const Image menu = renderAlone("menu", px, pad);
    CHECK_MSG(sharp(menu, pad + px / 2, 0, 0, 1, menu.h, why), "menu " + std::to_string(px) + ": " + why);
    const Image up = renderAlone("arrow-up", px, pad);
    CHECK_MSG(sharp(up, 0, pad + px * 3 / 4, 1, 0, up.w, why), "arrow-up " + std::to_string(px) + ": " + why);
    const Image plus = renderAlone("plus", px, pad);
    CHECK_MSG(sharp(plus, 0, pad + px / 3, 1, 0, plus.w, why), "plus " + std::to_string(px) + ": " + why);
    CHECK_MSG(sharp(plus, pad + px / 3, 0, 0, 1, plus.h, why), "plus " + std::to_string(px) + ": " + why);
  }
  // При дробном положении значок всё равно попадает в сетку пикселей.
  Image a(40, 40), b(40, 40);
  Canvas ca(a), cb(b);
  drawIcon(ca, "plus", {8, 8, 24, 24}, Color(255, 255, 255));
  drawIcon(cb, "plus", {8.3f, 7.8f, 24, 24}, Color(255, 255, 255));
  CHECK(a.px == b.px);
}

TEST(gfx_icons_unknown_name) {
  const std::string logPath = test::outDir() + "/icons_unknown.log";
  fs::remove(logPath);
  setLogFile(logPath);
  Image img(32, 32, 0);
  Canvas c(img);
  drawIcon(c, "definitely-missing-icon", {4, 4, 24, 24}, Color(255, 255, 255));
  drawIcon(c, "definitely-missing-icon", {4, 4, 24, 24}, Color(255, 255, 255));
  setLogFile("");
  const Coverage cv = coverage(img);
  CHECK(cv.any() && cv.sum > 30);
  const auto text = fs::readFile(logPath);
  CHECK(text.has_value());
  size_t count = 0;
  for (size_t p = 0; text && (p = text->find("definitely-missing-icon", p)) != std::string::npos; p++) count++;
  CHECK_EQ(count, size_t(1));
}

TEST(gfx_icons_transform_and_style) {
  // Масштаб холста = крупнее значок; поворот — та же площадь; толщина влияет на покрытие.
  Image a(64, 64, 0), b(64, 64, 0), r(64, 64, 0), t(64, 64, 0);
  {
    Canvas c(a);
    drawIcon(c, "settings", {8, 8, 48, 48}, Color(255, 255, 255));
  }
  {
    Canvas c(b);
    c.scale(2, 2);
    drawIcon(c, "settings", {4, 4, 24, 24}, Color(255, 255, 255));
  }
  {
    Canvas c(r);
    c.translate(32, 32);
    c.concat(Affine::rotate(0.5f));
    drawIcon(c, "settings", {-24, -24, 48, 48}, Color(255, 255, 255));
  }
  {
    Canvas c(t);
    drawIcon(c, "settings", {8, 8, 48, 48}, Color(255, 255, 255), 1.6f);
  }
  const double sa = coverage(a).sum, sb = coverage(b).sum, sr = coverage(r).sum, st = coverage(t).sum;
  CHECK_NEAR(sa, sb, sa * 0.02);
  CHECK_NEAR(sa, sr, sa * 0.05);
  CHECK(st > sa * 1.3);
  // Полупрозрачный цвет: без двойного наложения на пересечениях слоёв (максимум альфы = альфа цвета).
  Image h(40, 40, 0);
  Canvas ch(h);
  drawIcon(ch, "swords", {4, 4, 32, 32}, Color(255, 255, 255, 128));
  u32 maxA = 0;
  for (u32 v : h.px) maxA = std::max(maxA, v >> 24);
  CHECK(maxA <= 129);
  // Пустые и вырожденные прямоугольники ничего не рисуют и не падают.
  Image e(16, 16, 0);
  Canvas ce(e);
  drawIcon(ce, "plus", {0, 0, 0, 0}, Color(255, 255, 255));
  drawIcon(ce, "plus", {0, 0, -5, 10}, Color(255, 255, 255));
  drawIcon(ce, "plus", {std::nanf(""), 0, 10, 10}, Color(255, 255, 255));
  drawIcon(ce, "plus", {1e30f, 0, 10, 10}, Color(255, 255, 255));
  drawIcon(ce, "plus", {0, 0, 10, 10}, Color(255, 255, 255), std::nanf(""));
  CHECK(coverage(e).sum > 0);
}

TEST(gfx_icons_cache_and_speed) {
  clearGlyphCache();
  CHECK_EQ(glyphCacheSize(), size_t(0));
  Image img(1200, 800);
  Canvas c(img);
  c.clear(kDarkBg);
  const auto& names = iconNames();
  // Холодный проход: растеризация каждого значка на 20 px.
  double t0 = nowSeconds();
  for (size_t i = 0; i < names.size(); i++)
    drawIcon(c, names[i], {float(i % 50) * 24.f, float(i / 50) * 24.f, 20, 20}, kDarkFg);
  const double cold = (nowSeconds() - t0) * 1000;
  CHECK(glyphCacheSize() > 0);
  // Тёплый проход: 20 000 значков из кеша.
  t0 = nowSeconds();
  for (int i = 0; i < 20000; i++) {
    const std::string& n = names[size_t(i) % names.size()];
    drawIcon(c, n, {float(i % 49) * 24.f, float((i / 49) % 32) * 24.f, 20, 20}, kDarkFg);
  }
  const double warm = (nowSeconds() - t0) * 1000;
  std::printf("  значки: холодный проход %zu шт. %.1f мс, 20 000 из кеша %.1f мс (%.2f мкс/шт.)\n", names.size(), cold, warm,
              warm * 1000 / 20000);
  CHECK(warm < 400);
  clearGlyphCache();
  CHECK_EQ(glyphCacheSize(), size_t(0));
}

TEST(gfx_icons_sheets) {
  const auto& names = iconNames();
  const size_t per = 48;
  for (size_t from = 0, page = 1; from < names.size(); from += per, page++) {
    const size_t to = std::min(names.size(), from + per);
    sheet(names, from, to, kDarkBg, kDarkFg, kGold, "gfx_icons_dark_" + std::to_string(page));
    sheet(names, from, to, kLightBg, kLightFg, Color::hex(0xa8741a), "gfx_icons_light_" + std::to_string(page));
  }
  const size_t zper = 96;
  for (size_t from = 0, page = 1; from < names.size(); from += zper, page++)
    zoomSheet(names, from, std::min(names.size(), from + zper), "gfx_icons_zoom_" + std::to_string(page));
}

// Лист выбранных значков крупно и мелко (имена через запятую в REGNUM_ICON_FOCUS, «e:имя» — эмблема;
// без переменной — пропуск).
TEST(gfx_icons_sheet_focus) {
  const char* env = std::getenv("REGNUM_ICON_FOCUS");
  if (!env || !*env) return;
  std::vector<std::string> names;
  for (const std::string& s : split(env, ','))
    if (!trim(s).empty()) names.push_back(trim(s));
  const int cols = 6, cw = 200, ch = 150;
  const int rows = int((names.size() + cols - 1) / cols);
  Image img(cols * cw, rows * ch * 2);
  Canvas c(img);
  c.clear(kDarkBg);
  c.fillRect({0, float(rows * ch), float(img.w), float(rows * ch)}, kLightBg);
  for (int half = 0; half < 2; half++) {
    const Color fg = half ? kLightFg : kDarkFg;
    for (size_t i = 0; i < names.size(); i++) {
      const float x = float(int(i) % cols) * cw, y = float(int(i) / cols) * ch + float(half * rows * ch);
      label(c, names[i], x + 8, y + 4, fg.alpha(0.6f), 11);
      const bool emblem = startsWith(names[i], "e:");
      auto draw = [&](RectF r) {
        if (emblem) drawEmblem(c, std::string_view(names[i]).substr(2), r, fg);
        else drawIcon(c, names[i], r, fg);
      };
      draw({x + 8, y + 24, 96, 96});
      float iy = y + 24;
      for (int s : {16, 20, 24, 32}) {
        draw({x + 120, iy, float(s), float(s)});
        iy += float(s) + 6;
      }
    }
  }
  savePng(img, "gfx_icons_focus");
}
