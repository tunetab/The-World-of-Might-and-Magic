// Regnum — каталог геральдических эмблем: силуэты описаны строками путей SVG на сетке 100 × 100,
// повторяющиеся части (зёрна колоса, мечи, крылья) строятся преобразованиями.
#include "gfx/emblems.h"

#include <mutex>
#include <unordered_set>

#include "gfx/icons_shapes.h"
#include "gfx/svgpath.h"

namespace rg::gfx {

namespace {

using namespace shapes;
using G = GlyphBuilder;

// Путь после аффинного преобразования (строка SVG).
std::string xf(std::string_view d, const Affine& m) {
  Path p = svgPath(d);
  p.transform(m);
  return toSvgPath(p, 3);
}
Affine rotAt(float deg, float cx, float cy) {
  return Affine::translate(cx, cy) * Affine::rotate(deg * float(kPi) / 180.f) * Affine::translate(-cx, -cy);
}
const Affine kMirror{-1, 0, 0, 1, 100, 0};

std::string pt(double cx, double cy, double r, double deg) {
  const double t = deg * kPi / 180.0;
  return n(cx + r * std::cos(t)) + " " + n(cy + r * std::sin(t));
}

// Лучи солнца: n треугольников, длинные и короткие через один, основание на радиусе r0.
std::string sunRays(double cx, double cy, int count, double r0, double rLong, double rShort, double halfDeg) {
  std::string s;
  for (int i = 0; i < count; i++) {
    const double a = -90.0 + 360.0 * i / count;
    const double R = i % 2 == 0 ? rLong : rShort;
    s += "M" + pt(cx, cy, r0, a - halfDeg) + "L" + pt(cx, cy, R, a) + "L" + pt(cx, cy, r0, a + halfDeg) + "Z";
  }
  return s;
}

// Колос: стебель вниз от (0, 0), зёрна парами вверх. Локальные координаты, затем преобразование.
std::string ear() {
  std::string s;
  const std::string grain = ell(0, 0, 3.6, 7.2);
  for (int i = 0; i < 4; i++) {
    const float y = -8.f - 9.f * float(i);
    s += xf(grain, Affine::translate(-4.2f, y) * Affine::rotate(-0.5f));
    s += xf(grain, Affine::translate(4.2f, y) * Affine::rotate(0.5f));
  }
  s += xf(grain, Affine::translate(0, -43));
  return s;
}

// Меч остриём вверх (сетка 100): клинок, гарда, рукоять, навершие.
const std::string kSwordBody =
    "M50 2 58.5 15V63h-17V15Z"
    "M17 59c8.5 4 19.5 6 33 6s24.5-2 33-6c2.2 2.6 2 6.6-.6 8.6C74.4 72.4 63.2 75.5 50 75.5S25.6 72.4 17.6 67.6c-2.6-2-2.8-6-.6-8.6Z"
    "M45 75h10v13H45Z";
const std::string kSwordPommel = "M50 86.5a6.5 6.5 0 1 1 0 13 6.5 6.5 0 0 1 0-13Z";

const Affine kSkullXf{1.12f, 0, 0, 1.12f, -6, -1.5f};

void defineEmblems(GlyphBuilder& b) {
  // ---- корона: пять зубцов с жемчужинами, обруч с камнями
  b.add("crown", {G::F("M20 66 12 33 25 50 31 25 41 46 50 15 59 46 69 25 75 50 88 33 80 66Z" + rrect(16, 69, 68, 14, 3) +
                       circ(12, 29, 5) + circ(31, 21, 5) + circ(50, 11, 5.5) + circ(69, 21, 5) + circ(88, 29, 5)),
                  G::X(circ(33, 76, 3.4) + "M50 71.5l4.5 4.5-4.5 4.5-4.5-4.5Z" + circ(67, 76, 3.4))});
  // ---- башня
  b.add("tower", {G::F("M24 9h11v9h7.5V9h15v9H65V9h11v25l-7 7v46H31V41l-7-7Z" + rrect(17, 86, 66, 7, 3)),
                  G::X("M42 87V72a8 8 0 0 1 16 0v15Z" + rrect(45.5, 48, 9, 14, 4.5) + "M27 34h46v3H27Z")});
  // ---- замок: три башни, стены, ворота
  b.add("castle", {G::F("M37 9h7v8h2.5V9h7v8H56V9h7v79H37Z"
                        "M9 24h6v7h2.75v-7h6v7h2.75v-7H33v64H9Z"
                        "M67 24h6v7h2.75v-7h6v7h2.75v-7H91v64H67Z"
                        "M33 46h4v42h-4ZM63 46h4v42h-4Z" + rrect(5, 86, 90, 8, 3)),
                   G::X("M42 87V73a8 8 0 0 1 16 0v14Z" + rrect(46.5, 30, 7, 13, 3.5) + rrect(18, 44, 6, 11, 3) + rrect(76, 44, 6, 11, 3) +
                        "M37 22h26v2.5H37ZM9 36h24v2.5H9ZM67 36h24v2.5H67Z")});
  // ---- звезда
  b.add("star", {G::F(star(50, 54.4, 46, 18.5, 5, -90))});
  // ---- солнце: лучи и диск
  b.add("sun", {G::F(sunRays(50, 50, 16, 24, 48, 39, 7.5)), G::X(circ(50, 50, 26.5)), G::F(circ(50, 50, 22))});
  // ---- луна: полумесяц
  b.add("moon", {G::F(circ(53, 50, 41)), G::X(circ(69, 42, 34))});
  // ---- дерево: крона из кругов, ствол с корнями
  b.add("tree", {G::F(circ(50, 27, 19) + circ(30, 39, 15) + circ(70, 39, 15) + circ(37, 54, 14) + circ(63, 54, 14) +
                      circ(50, 47, 18) + "M45 60h10c-.5 9 1 17 5 24l10 5H30l10-5c4-7 5.5-15 5-24Z"),
                 G::XS("M50 62V44M50 52l-8-7M50 49l9-8", 3.2f)});
  // ---- лилия
  const std::string lilySide = "M53 64C55 47 63 33 77 28c9-3 17 3 15 12-1 6-6 8-10 6 3-4 1-9-4-8-9 2-14 13-15 26Z";
  b.add("lily", {G::F("M50 4c10 12 15 26 13 40-1 8-5 15-13 21-8-6-12-13-13-21-2-14 3-28 13-40Z" + lilySide + xf(lilySide, kMirror) +
                      rrect(27, 63, 46, 9, 4) +
                      "M44 72h12l1 10c5-3 11-3 15 2-6 0-10 2-12 7H40c-2-5-6-7-12-7 4-5 10-5 15-2Z")});
  // ---- меч
  b.add("sword", {G::F(kSwordBody + kSwordPommel), G::X("M48.6 18h2.8v42h-2.8ZM45 79h10v2.2H45ZM45 83.5h10v2.2H45Z")});
  // ---- скрещённые мечи
  {
    const Affine back = rotAt(-40, 50, 50) * Affine{1.06f, 0, 0, 1.06f, -3, -3};
    const Affine front = rotAt(40, 50, 50) * Affine{1.06f, 0, 0, 1.06f, -3, -3};
    const std::string a = xf(kSwordBody + kSwordPommel, back), f = xf(kSwordBody + kSwordPommel, front);
    b.add("swords", {G::F(a), G::XS(f, 5), G::X(f), G::F(f)});
  }
  // ---- щит с каймой
  b.add("shield", {G::F("M50 5 89 17v27c0 25-16 43-39 53C27 87 11 69 11 44V17Z"),
                   G::XS("M50 15.5 80 24.5V44c0 19.8-12.4 34.5-30 43-17.6-8.5-30-23.2-30-43V24.5Z", 4)});
  // ---- череп
  b.add("skull", {G::T(G::F("M50 7C28 7 15 23 15 42c0 12 5.5 20.5 13.5 25.5V79a6 6 0 0 0 6 6h31a6 6 0 0 0 6-6V67.5"
                            "C79.5 62.5 85 54 85 42 85 23 72 7 50 7Z"), kSkullXf),
                  G::T(G::X(ell(35.5, 45, 9.5, 10.5) + ell(64.5, 45, 9.5, 10.5) + "M50 56l5.5 10h-11Z" + rrect(39, 74, 4, 11, 1.5) +
                            rrect(48, 74, 4, 11, 1.5) + rrect(57, 74, 4, 11, 1.5)), kSkullXf)});
  // ---- якорь
  b.add("anchor", {G::S(circ(50, 14, 7.5), 7), G::F(rrect(45, 21, 10, 66, 4) + rrect(27, 30, 46, 9, 4.5)),
                   G::S("M17 57c1 19 15 33 33 33s32-14 33-33", 9),
                   G::F("M17 44 27 62 7 62ZM83 44 93 62 73 62Z")});
  // ---- корабль
  b.add("ship", {G::F("M7 60h86l-9 19a10 10 0 0 1-9 6H25a10 10 0 0 1-9-6Z" + std::string("M7 60V49h17v11ZM78 60v-8h15v8Z") +
                      rrect(48, 9, 4, 52, 1.5) +
                      "M26 18c16-4 32-4 48 0 3 10 3 21 0 31-16-4-32-4-48 0-3-10-3-21 0-31Z" + "M52 7h15l-4 4 4 4H52Z"),
                 G::X("M24 54.5h52v2.5H24Z" + circ(30, 70, 3) + circ(43, 70, 3) + circ(57, 70, 3) + circ(70, 70, 3))});
  // ---- колос: три колоса, перевязь
  {
    const std::string e = ear();
    const std::string stalks = "M50 50V95M50 62 37 95M50 62 63 95";
    b.add("wheat", {G::F(xf(e, Affine::translate(50, 53)) + xf(e, rotAt(-24, 50, 72) * Affine::translate(50, 55)) +
                         xf(e, rotAt(24, 50, 72) * Affine::translate(50, 55))),
                    G::S(stalks, 3.4f), G::F(rrect(39, 66, 22, 7, 3.5))});
  }
  // ---- самоцвет
  b.add("gem", {G::F("M28 14h44l22 26-44 50L6 40Z"),
                G::XS("M6 40h88M39 14l-8 26 19 50 19-50-8-26M31 40 50 14l19 26", 3.2f)});
  // ---- молот
  b.add("hammer", {G::F("M17 10h66l7 8v20l-7 8H17l-7-8V18Z" + rrect(43.5, 46, 13, 40, 3) + circ(50, 90, 7)),
                   G::XS("M21 16h58l4.5 5v14L79 40H21l-4.5-5V21Z", 2.4f),
                   G::X(circ(50, 90, 3) + "M43.5 54l13-5v3.5l-13 5ZM43.5 64l13-5v3.5l-13 5ZM43.5 74l13-5v3.5l-13 5Z")});
  // ---- топор (двусторонний)
  const std::string axeBlade = "M45 27C34 25 22 19 14 7 6 19 3 31 3 41s3 22 11 34c8-12 20-18 31-20Z";
  b.add("axe", {G::F(axeBlade + xf(axeBlade, kMirror) + rrect(44.5, 9, 11, 85, 5) + "M50 1l6 10H44Z"),
                G::XS("M45 27v28M55 27v28", 2.5f), G::X("M44.5 70h11v2.5h-11ZM44.5 76h11v2.5h-11Z")});
  // ---- лук со стрелой
  b.add("bow", {G::S("M33 6c25 11 37 27 37 44S58 83 33 94", 8), G::S("M33 6v88", 2.5f),
                G::F("M8 47.5h76v5H8Z" + std::string("M96 50 81 41v18Z") + "M8 47.5 3 40h9l5 7.5ZM8 52.5 3 60h9l5-7.5Z")});
  // ---- ключ
  b.add("key", {G::F(circ(50, 23, 18) + rrect(45, 38, 10, 56, 3) + "M55 64h18v9h-6v8h-7v-8h-5ZM55 82h12v10H55Z"),
                G::X(circ(50, 23, 8.5)), G::F(circ(50, 23, 3.5))});
  // ---- глаз
  b.add("eye", {G::F("M4 50C17 29 32 19 50 19s33 10 46 31C83 71 68 81 50 81S17 71 4 50Z"), G::X(circ(50, 50, 20)),
                G::F(circ(50, 50, 14.5)), G::X(circ(56, 44, 4.5))});
  // ---- пламя
  b.add("flame", {G::F("M50 4c4 16 24 26 24 50 0 22-11 40-24 42-13-2-24-20-24-42 0-12 5-21 12-27 0 11 4 18 10 21-3-16 0-30 2-44Z"
                       "M71 34c8 6 12 15 12 25 0 13-7 24-15 30 4-7 6-15 5-24-1-10-3-18-2-31ZM29 42c-7 5-12 13-12 23 0 11 6 20 13 24"
                       "-3-6-5-13-4-21 1-9 2-16 3-26Z"),
                  G::X("M50 52c6 8 11 13 11 22 0 9-5 15-11 15s-11-6-11-15c0-8 6-14 11-22Z")});
  // ---- орёл с распростёртыми крыльями
  {
    const std::string feathers = "M38 41 13 11M38 43 8 23M38 46 6 35M39 49 9 47M41 52 16 58";
    const std::string covert = "M45 35C37 29 28 25 19 21L15 40 22 55 41 58Z";
    const std::string leg = "M44 66 35 75M35 75h-6M35 75l-3.5 6M35 75l1 6.5";
    b.add("eagle", {G::S(feathers + xf(feathers, kMirror), 8), G::F(covert + xf(covert, kMirror) + ell(50, 52, 11.5, 20)),
                    G::S("M50 68 43 91M50 68v26M50 68l7 23", 7), G::S(leg + xf(leg, kMirror), 3.4f),
                    G::F(circ(48, 23, 9) + "M43 26h12v14H43Z" + "M41 18C34 18 29 22 28 29c3-2.5 6.5-3 10-2l3.5-1Z"),
                    G::X(circ(47, 21, 2.1))});
  }
  // ---- лев (идущий, голова вперёд)
  {
    std::string mane = circ(38, 27, 15);
    for (int i = 0; i < 12; i++) {
      const double a = (-75 + 30 * i) * kPi / 180;
      mane += circ(38 + 14.5 * std::cos(a), 27 + 14.5 * std::sin(a), 6.2);
    }
    const std::string paw = "M18 28l-4.5-1.5M18 28l-3 3M18 28l-1-4.5";
    b.add("lion", {G::F(xf(ell(0, 0, 15, 25), Affine::translate(57, 55) * Affine::rotate(-0.4f)) + circ(47, 42, 15) + circ(61, 71, 11) +
                        mane + xf(ell(0, 0, 10.5, 8), Affine::translate(20, 31) * Affine::rotate(0.25f))),
                   G::S("M41 43 28 38 19 29M47 54 32 60 23 54M59 72 45 80 36 76M65 74l3 15-10 6", 9),
                   G::S(paw + xf(paw, Affine::translate(4, 25)) + xf(paw, Affine::translate(17, 47)), 3),
                   G::S("M69 75C83 73 87 61 83 49 80 40 83 32 90 29", 5),
                   G::F(xf(ell(0, 0, 8, 5), Affine::translate(91, 24) * Affine::rotate(-1.1f))),
                   G::X(circ(24, 26, 2) + "M10.5 34.5l11.5-.5-7 5Z")});
  }
  // ---- дракон (голова с рогами и гребнем)
  b.add("dragon", {G::F("M5 40 22 33 32 25 44 8 42 25 59 12 53 30C63 34 70 42 74 54L84 50 80 60 91 62 84 70 95 77 84 80 88 93H51"
                        "C54 80 52 70 46 62 42 58 36 56 30 56L13 59 20 52 8 48Z"),
                   G::X("M29 33c3-3 8-3 10 0-3 2-7 2-10 0Z" + circ(12, 41, 1.6))});
  // ---- волк (голова, воющий)
  b.add("wolf", {G::F("M5 40 24 32 34 24 38 21 44 3 52 22 58 8 62 27C70 33 77 44 80 58L89 70 80 72 89 84 77 84 80 95H44"
                      "L40 84 46 77 36 72 41 64C36 61 33 58 30 56L11 51 26 45 8 43Z"),
                 G::X("M31 29c3-2.5 7.5-2 9 1-3 1.8-6.4 1.5-9-1Z")});
  // ---- бык (голова анфас)
  const std::string horn = "M37 31C23 34 10 29 7 10c7 9 17 13 30 12Z";
  const std::string ear = "M32 35 15 41 33 46Z";
  b.add("bull", {G::F(horn + xf(horn, kMirror) + ear + xf(ear, kMirror) +
                      "M38 21h24c6 0 10 6 8.5 13L66 60c0 6-3 9-5 13 0 9-4 17-11 17s-11-8-11-17c-2-4-5-7-5-13L29.5 34C28 27 32 21 38 21Z"),
                 G::X(ell(40.5, 44, 3.5, 2.5) + ell(59.5, 44, 3.5, 2.5) + ell(45, 82, 2.5, 3.2) + ell(55, 82, 2.5, 3.2))});
  // ---- конь (голова с гривой)
  b.add("horse", {G::F("M10 53C8 49 9 45 12 42L30 18 34 7 40 16C48 14 56 18 62 26L70 21 68 30 78 28 74 38 86 38 80 48 91 52 82 58"
                       " 88 68 82 70 86 92H44C46 79 44 68 38 62 34 58 30 56 26 58L18 60C14 60 12 58 10 53Z"),
                  G::X(circ(30, 31, 2.6) + ell(14.5, 51, 1.8, 2.4))});
  // ---- змей
  b.add("serpent", {G::S("M22 90C40 95 72 90 72 74 72 57 32 63 29 45 26 30 46 24 58 22", 10),
                    G::F(xf(ell(0, 0, 13, 8.5), Affine::translate(66, 20) * Affine::rotate(-0.25f))),
                    G::X(circ(69, 17, 1.9)), G::S("M78 17l9-2M87 15l4-4M87 15l4 2", 2)});
  // ---- кракен
  {
    const std::string t1 = "M40 50C30 57 17 56 13 66c-3 8 4 13 9 8";
    const std::string t2 = "M44 54C39 66 30 76 34 86c3 6 10 4 9-2";
    const std::string t3 = "M48 56c0 14-4 24 0 36";
    b.add("kraken", {G::F(ell(50, 30, 23, 25)),
                     G::S(t1 + xf(t1, kMirror) + t2 + xf(t2, kMirror) + t3 + xf(t3, kMirror), 7),
                     G::X(ell(41, 35, 3.5, 4.5) + ell(59, 35, 3.5, 4.5))});
  }
  // ---- руна
  b.add("rune", {G::S("M50 8v84M50 46 27 19M50 46 73 19M50 62 29 85M50 62 71 85", 9)});
  // ---- роза
  {
    std::string petals;
    std::string inner;
    for (int i = 0; i < 5; i++) {
      const double a = (-90 + 72 * i) * kPi / 180, ai = (-54 + 72 * i) * kPi / 180;
      petals += circ(50 + 24 * std::cos(a), 50 + 24 * std::sin(a), 18.5);
      inner += arc(50 + 12 * std::cos(ai), 50 + 12 * std::sin(ai), 12, (-54 + 72 * i) - 110, (-54 + 72 * i) + 110);
    }
    b.add("rose", {G::F(star(50, 50, 48, 24, 5, -54)), G::F(petals + circ(50, 50, 26)),
                   G::XS(inner, 2.6f), G::X(circ(50, 50, 8.5)), G::F(circ(50, 50, 6))});
  }
  // ---- грифон (голова)
  b.add("griffin", {G::F("M30 21C19 19 9 24 7 37c0 3 2 5 4 3 3-4 7-5 11-4L28 39 16 44C22 48 28 50 32 52L29 62 38 60 33 72 42 70 39 82"
                         " 48 80 46 95H85L80 83 88 80 79 69 88 65 76 56C76 44 71 34 63 28L71 8 54 22C47 18 38 18 30 21Z"),
                    G::X(circ(35, 28, 2.7))});
  // ---- медведь (идущий)
  b.add("bear", {G::T(G::F("M6 50C7 47 9 45 12 44L20 40C21 36 23 33 26 32 28 30 31 31 32 34 37 30 43 26 50 24 60 22 72 24 80 29 87 33 92 40 93 47"
                           "L95 49 93 53C93 60 92 66 90 70L91 84C91 86 90 87 88 87H80C78 87 77 86 77 84V72C74 71 72 70 70 68V84"
                           "C70 86 69 87 67 87H61C59 87 58 86 58 84V67C53 67 48 67 44 66L43 84C43 86 42 87 40 87H34C32 87 31 86 31 84"
                           "V70L29 84C29 86 28 87 26 87H21C19 87 18 86 18 84L20 64C17 62 15 60 13 58L9 57C7 56 6 53 6 50Z"),
                      Affine::translate(0, -4.5f)),
                 G::X(circ(19, 40.5, 1.9))});
}

struct EmblemRegistry {
  GlyphBuilder b{100, 0x656d626cull << 32};
};

const EmblemRegistry& registry() {
  static const EmblemRegistry r = [] {
    EmblemRegistry x;
    defineEmblems(x.b);
    return x;
  }();
  return r;
}

struct Title {
  const char* name;
  const char* title;
};
const Title kTitles[] = {
    {"crown", "Корона"}, {"tower", "Башня"}, {"castle", "Замок"}, {"star", "Звезда"}, {"sun", "Солнце"}, {"moon", "Луна"},
    {"tree", "Дерево"}, {"lily", "Лилия"}, {"sword", "Меч"}, {"swords", "Скрещённые мечи"}, {"shield", "Щит"},
    {"skull", "Череп"}, {"anchor", "Якорь"}, {"ship", "Корабль"}, {"wheat", "Колосья"}, {"gem", "Самоцвет"},
    {"hammer", "Молот"}, {"axe", "Топор"}, {"bow", "Лук"}, {"key", "Ключ"}, {"eye", "Глаз"}, {"flame", "Пламя"},
    {"eagle", "Орёл"}, {"lion", "Лев"}, {"dragon", "Дракон"}, {"wolf", "Волк"}, {"bull", "Бык"}, {"horse", "Конь"},
    {"serpent", "Змей"}, {"kraken", "Кракен"}, {"rune", "Руна"}, {"rose", "Роза"}, {"griffin", "Грифон"},
    {"bear", "Медведь"},
};

std::mutex gUnknownMu;
std::unordered_set<std::string> gUnknown;

}  // namespace

const std::vector<std::string>& emblemNames() { return registry().b.names(); }
bool hasEmblem(std::string_view name) { return registry().b.find(name) != nullptr; }
const VecGlyph* emblemGlyph(std::string_view name) { return registry().b.find(name); }

std::vector<std::string> emblemRegistryIssues() {
  std::vector<std::string> out = registry().b.issues();
  for (const Title& t : kTitles)
    if (!hasEmblem(t.name)) out.push_back(std::string("подпись без эмблемы: ") + t.name);
  for (const std::string& nm : emblemNames())
    if (emblemTitle(nm).empty()) out.push_back("эмблема без подписи: " + nm);
  return out;
}

std::string_view emblemTitle(std::string_view name) {
  for (const Title& t : kTitles)
    if (name == t.name) return t.title;
  return {};
}

void drawEmblem(Canvas& c, std::string_view name, RectF rect, Color color) {
  if (name.empty()) return;
  if (const VecGlyph* g = registry().b.find(name)) {
    drawGlyph(c, *g, rect, color);
    return;
  }
  std::lock_guard<std::mutex> lock(gUnknownMu);
  if (gUnknown.insert(std::string(name)).second) logWarn("Эмблема «%.*s» не найдена", int(name.size()), name.data());
}

}  // namespace rg::gfx
