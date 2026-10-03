// Regnum — эмблемы: полнота каталога, подписи, заполнение и центровка, неизвестные имена, наглядные листы.
#include <unordered_set>

#include "gfx/emblems.h"
#include "tests/test_gfx_icons_util.h"

using namespace rg;
using namespace rg::gfx;
using namespace rg::test::icons;

namespace {

const char* const kRequiredEmblems[] = {"crown", "tower", "castle", "star", "sun", "moon", "tree", "lily", "sword",
                                        "swords", "shield", "skull", "anchor", "ship", "wheat", "gem", "hammer", "axe",
                                        "bow", "key", "eye", "flame", "eagle", "lion", "dragon", "wolf", "bull", "horse",
                                        "serpent", "kraken", "rune", "rose", "griffin", "bear"};

Image renderEmblem(std::string_view name, int px, int pad) {
  Image img(px + 2 * pad, px + 2 * pad, 0);
  Canvas c(img);
  drawEmblem(c, name, {float(pad), float(pad), float(px), float(px)}, Color(255, 255, 255));
  return img;
}

}  // namespace

TEST(gfx_icons_emblems_catalog) {
  std::vector<std::string> missing;
  for (const char* n : kRequiredEmblems)
    if (!hasEmblem(n)) missing.push_back(n);
  CHECK_MSG(missing.empty(), "нет эмблем: " + join(missing, ", "));
  const auto& names = emblemNames();
  CHECK_EQ(names.size(), std::size(kRequiredEmblems));
  std::unordered_set<std::string> uniq(names.begin(), names.end());
  CHECK_EQ(uniq.size(), names.size());
  const auto issues = emblemRegistryIssues();
  CHECK_MSG(issues.empty(), join(issues, "\n"));
  CHECK_EQ(std::string(emblemTitle("crown")), std::string("Корона"));
  CHECK(emblemTitle("nothing").empty());
  CHECK(emblemGlyph("lion") != nullptr);
  CHECK(emblemGlyph("") == nullptr);
}

TEST(gfx_icons_emblems_fill_and_center) {
  std::vector<std::string> bad;
  for (const std::string& n : emblemNames()) {
    const int px = 100, pad = 8;
    const Image img = renderEmblem(n, px, pad);
    const Coverage cv = coverage(img);
    const double area = cv.sum / (px * px);
    // Силуэт заметен, но не сплошной квадрат.
    if (area < 0.12 || area > 0.8) bad.push_back(n + ": площадь " + fmtNum(area * 100) + "%");
    if (cv.x0 < pad - 1 || cv.y0 < pad - 1 || cv.x1 > pad + px || cv.y1 > pad + px) bad.push_back(n + ": выход за квадрат");
    const double bx = (cv.x0 + cv.x1 + 1) * 0.5 - pad - px * 0.5, by = (cv.y0 + cv.y1 + 1) * 0.5 - pad - px * 0.5;
    if (std::fabs(bx) > 6 || std::fabs(by) > 6) bad.push_back(n + ": габариты смещены (" + fmtNum(bx) + ", " + fmtNum(by) + ")");
    const int span = std::max(cv.x1 - cv.x0 + 1, cv.y1 - cv.y0 + 1);
    if (span < 80) bad.push_back(n + ": мелко (" + std::to_string(span) + ")");
    // Малые размеры: что-то видно.
    for (int s : {12, 16, 24}) {
      const Coverage small = coverage(renderEmblem(n, s, 2));
      if (small.sum < s * s * 0.12) bad.push_back(n + ": пусто на " + std::to_string(s));
    }
  }
  CHECK_MSG(bad.empty(), join(bad, "\n"));
}

TEST(gfx_icons_emblems_unknown) {
  Image img(40, 40, 0);
  Canvas c(img);
  drawEmblem(c, "", {0, 0, 40, 40}, Color(255, 255, 255));
  drawEmblem(c, "no-such-emblem", {0, 0, 40, 40}, Color(255, 255, 255));
  drawEmblem(c, "no-such-emblem", {0, 0, 40, 40}, Color(255, 255, 255));
  CHECK(!coverage(img).any());
  drawEmblem(c, "lion", {0, 0, 40, 40}, Color(255, 255, 255, 0));
  CHECK(!coverage(img).any());
}

TEST(gfx_icons_emblems_sheet) {
  const auto& names = emblemNames();
  const int cols = 7, cell = 190;
  const int rows = int((names.size() + cols - 1) / cols);
  Image img(cols * cell, rows * cell);
  Canvas c(img);
  c.clear(Color::hex(0x151a22));
  const Color bgs[] = {Color::hex(0x7a2430), Color::hex(0x1f4e79), Color::hex(0x2f5d3a), Color::hex(0x5b3f8c),
                       Color::hex(0x1d2333), Color::hex(0x8c5a1b)};
  for (size_t i = 0; i < names.size(); i++) {
    const float x = float(int(i) % cols) * cell, y = float(int(i) / cols) * cell;
    const Color bg = bgs[i % std::size(bgs)];
    c.fillRoundRect({x + 6, y + 6, cell - 12.f, cell - 12.f}, 10, bg);
    label(c, names[i] + "  " + std::string(emblemTitle(names[i])), x + 14, y + 10, Color(255, 255, 255, 170), 11);
    drawEmblem(c, names[i], {x + 18, y + 28, 120, 120}, Color::hex(0xf2e3b3));
    float iy = y + 30;
    for (int s : {16, 24, 32}) {
      drawEmblem(c, names[i], {x + 146, iy, float(s), float(s)}, Color::hex(0xf2e3b3));
      iy += float(s) + 8;
    }
  }
  savePng(img, "gfx_emblems");
  // Тёмные эмблемы на светлом фоне.
  Image light(cols * 110, rows * 110);
  Canvas cl(light);
  cl.clear(Color::hex(0xf2e3b3));
  for (size_t i = 0; i < names.size(); i++)
    drawEmblem(cl, names[i], {float(int(i) % cols) * 110 + 10, float(int(i) / cols) * 110 + 10, 90, 90}, Color::hex(0x1d2333));
  savePng(light, "gfx_emblems_light");
}
