// Тесты разложения исходной карты на слои ocean / inland / symbols и точного уменьшения слоёв.
#include "tests/test_map_basemap_util.h"

using namespace rg;
using namespace rg::map::bake;

namespace {

struct Segmented {
  bmtest::SynthMap m;
  Layers L;
  SegmentStats st;
};

const Segmented& segmented() {
  static const Segmented s = [] {
    Segmented r;
    r.m = bmtest::makeSynth();
    flattenOnWhite(r.m.img);
    r.L = segment(r.m.img, Options{}, &r.st);
    return r;
  }();
  return s;
}

// Композиция пикселя поверх основы base (как у отрисовщика: основа → ocean → inland → symbols), без округления.
void compositeOver(const Layers& L, size_t i, const double base[3], double out[3]) {
  const double ao = L.ocean[i] / 255.0, ai = L.inland[i] / 255.0, as = L.symA[i] / 255.0;
  const double blue[3] = {double(kOcean.r), double(kOcean.g), double(kOcean.b)};
  for (int c = 0; c < 3; c++) {
    double v = base[c] * (1 - ao) + blue[c] * ao;
    v = v * (1 - ai) + blue[c] * ai;
    out[c] = v * (1 - as) + L.symV[i] * as;
  }
}

}  // namespace

TEST(map_basemap_segment_water_classes) {
  const Segmented& s = segmented();
  const Layers& L = s.L;
  auto at = [&](Vec2 p) { return bmtest::idx(L, p); };
  // Открытое море и залив за цепочкой островков — море.
  CHECK_EQ(int(L.ocean[at(s.m.sea)]), 255);
  CHECK_EQ(int(L.inland[at(s.m.sea)]), 0);
  CHECK(L.ocean[at(s.m.bay)] >= 250);
  CHECK_EQ(int(L.inland[at(s.m.bay)]), 0);
  CHECK(L.field[at(s.m.bay)] >= 250);
  // Озеро и река — внутренние воды.
  CHECK(L.inland[at(s.m.lake)] >= 250);
  CHECK_EQ(int(L.ocean[at(s.m.lake)]), 0);
  CHECK(L.inland[at(s.m.river)] >= 200);
  CHECK_EQ(int(L.ocean[at(s.m.river)]), 0);
  CHECK_EQ(int(L.field[at(s.m.lake)]), 0);
  // Суша: ни воды, ни краски.
  CHECK_EQ(int(L.ocean[at(s.m.land)]), 0);
  CHECK_EQ(int(L.inland[at(s.m.land)]), 0);
  CHECK_EQ(int(L.symA[at(s.m.land)]), 0);
  // Островки цепочки не мешали поиску моря.
  CHECK(s.st.isletsFilled >= 4);
  CHECK_EQ(s.st.mountains, 1);  // две слившиеся горы — одна область краски
  CHECK(s.st.snowCaps >= 2);
}

TEST(map_basemap_segment_without_islet_fill_bay_is_cut_off) {
  // Контроль: без островков-как-воды проток перекрыт эрозией, и залив попадает во внутренние воды.
  bmtest::SynthMap m = bmtest::makeSynth();
  flattenOnWhite(m.img);
  Options o;
  o.isletFillArea = 0;
  const Layers L = segment(m.img, o);
  CHECK_EQ(int(L.ocean[bmtest::idx(L, m.bay)]), 0);
  CHECK(L.inland[bmtest::idx(L, m.bay)] >= 250);
}

TEST(map_basemap_segment_symbols) {
  const Segmented& s = segmented();
  const Layers& L = s.L;
  auto at = [&](Vec2 p) { return bmtest::idx(L, p); };
  // Снежная шапка — непрозрачно белая (заливка провинции не окрашивает снег).
  CHECK_EQ(int(L.symA[at(s.m.snow)]), 255);
  CHECK(L.symV[at(s.m.snow)] >= 250);
  // Тело горы — непрозрачный серый.
  CHECK_EQ(int(L.symA[at(s.m.body)]), 255);
  CHECK(std::abs(int(L.symV[at(s.m.body)]) - 96) <= 1);
  CHECK(L.kind[at(s.m.body)] & KindMountain);
  // Фон между слившимися горами (внутри общей оболочки) прозрачен.
  CHECK(L.kind[at(s.m.gap)] & KindMountain);
  CHECK_EQ(int(L.symA[at(s.m.gap)]), 0);
  // Замок: линии непрозрачны, внутренность прозрачна.
  CHECK(L.symA[at(s.m.castleLine)] >= 250);
  CHECK(L.symV[at(s.m.castleLine)] <= 8);
  CHECK_EQ(int(L.symA[at(s.m.castleInside)]), 0);
  CHECK(!(L.kind[at(s.m.castleInside)] & KindMountain));
  // Башня — непрозрачно чёрная, на суше.
  CHECK_EQ(int(L.symA[at(s.m.tower)]), 255);
  CHECK(L.symV[at(s.m.tower)] <= 8);
  CHECK_EQ(int(L.ocean[at(s.m.tower)]), 0);
  // Точка пунктира в море: краска непрозрачна, вода и поле под ней продолжены от соседей.
  CHECK_EQ(int(L.symA[at(s.m.dot)]), 255);
  CHECK(L.ocean[at(s.m.dot)] >= 250);
  CHECK(L.field[at(s.m.dot)] >= 250);
  CHECK(s.st.inpaintedPx > 0);
}

TEST(map_basemap_segment_composite_reproduces_source) {
  const Segmented& s = segmented();
  const CompositeError e = compareComposite(s.m.img, s.L);
  CHECK_MSG(e.mean < 0.05, strf("mean %.4f", e.mean));
  CHECK_MSG(e.max <= 2, strf("max %d", e.max));
  // Символы — только нейтральный серый: в тайле символов R = G = B.
  bool empty = false;
  const codec::RgbaImage t = tileImage(s.L, 2, 0, 0, s.L.w, s.L.h, &empty);
  CHECK(!empty);
  i64 colored = 0;
  for (size_t i = 0; i < t.rgba.size(); i += 4) colored += t.rgba[i] != t.rgba[i + 1] || t.rgba[i + 1] != t.rgba[i + 2];
  CHECK_EQ(colored, i64(0));
  // Вода в тайлах — ровно #0026FF.
  const codec::RgbaImage o = tileImage(s.L, 0, 0, 0, s.L.w, s.L.h, &empty);
  for (size_t i = 0; i < o.rgba.size(); i += 4)
    if (o.rgba[i + 3]) colored += o.rgba[i] != 0 || o.rgba[i + 1] != 38 || o.rgba[i + 2] != 255;
  CHECK_EQ(colored, i64(0));
  // Артефакты для просмотра: исходник, композиция с заливкой провинции и классы.
  const std::string dir = bmtest::freshDir("basemap_segment");
  writePng(fs::join(dir, "source.png"), s.m.img, 6);
  writePng(fs::join(dir, "tinted.png"), composite(s.L, 0, 0, s.L.w, s.L.h, Color(200, 40, 40, 128)), 6);
  codec::RgbaImage cls = s.m.img;
  for (size_t i = 0; i < size_t(s.L.w) * size_t(s.L.h); i++) {
    u8* p = cls.rgba.data() + i * 4;
    const u8 k = s.L.kind[i];
    Color c(255, 255, 255);
    if (s.L.ocean[i] >= 128) c = Color(0, 38, 255);
    else if (s.L.ocean[i]) c = Color(150, 170, 255);
    if (s.L.inland[i] >= 128) c = Color(0, 170, 60);
    else if (s.L.inland[i]) c = Color(150, 230, 170);
    if (k & KindMountain) c = s.L.symA[i] >= 128 ? Color(230, 120, 0) : Color(255, 200, 140);
    else if (s.L.symA[i] >= 128) c = Color(0, 0, 0);
    p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 255;
  }
  writePng(fs::join(dir, "classes.png"), cls, 6);
}

TEST(map_basemap_segment_tint_keeps_snow_and_colors_land) {
  const Segmented& s = segmented();
  const Color tint(200, 40, 40, 128);
  const codec::RgbaImage c = composite(s.L, 0, 0, s.L.w, s.L.h, tint);
  auto px = [&](Vec2 p) { return c.rgba.data() + bmtest::idx(s.L, p) * 4; };
  CHECK(px(s.m.snow)[1] >= 250);          // снег белый
  CHECK(px(s.m.land)[1] < 200);           // суша окрашена
  CHECK(px(s.m.castleInside)[1] < 200);   // внутренность замка окрашена
  CHECK(px(s.m.gap)[1] < 200);            // фон между горами окрашен
  CHECK(px(s.m.sea)[2] == 255 && px(s.m.sea)[0] == 0);  // море — без заливки
}

TEST(map_basemap_segment_deterministic) {
  bmtest::SynthMap m = bmtest::makeSynth();
  flattenOnWhite(m.img);
  const Layers a = segment(m.img, Options{});
  const Layers b = segment(m.img, Options{});
  CHECK(a.ocean == b.ocean && a.inland == b.inland && a.symA == b.symA && a.symV == b.symV && a.field == b.field && a.kind == b.kind);
}

TEST(map_basemap_flatten_on_white) {
  codec::RgbaImage im;
  im.w = 2;
  im.h = 1;
  im.rgba = {0, 0, 0, 0, 0, 38, 255, 128};
  flattenOnWhite(im);
  CHECK_EQ(int(im.rgba[0]), 255);
  CHECK_EQ(int(im.rgba[3]), 255);
  CHECK_EQ(int(im.rgba[4]), 127);  // 0·128/255 + 255·127/255
  CHECK_EQ(int(im.rgba[6]), 255);
  CHECK_EQ(int(im.rgba[7]), 255);
}

TEST(map_basemap_segment_rejects_empty) {
  codec::RgbaImage im;
  CHECK_THROWS(segment(im, Options{}));
}

TEST(map_basemap_downsample_is_composite_exact) {
  // Случайные слои: вода моря и внутренних вод (в том числе вперемешку — устья), краска разной плотности.
  Rng rng(77);
  Layers L;
  L.w = 131;
  L.h = 97;
  const size_t n = size_t(L.w) * size_t(L.h);
  L.ocean.assign(n, 0);
  L.inland.assign(n, 0);
  L.symA.assign(n, 0);
  L.symV.assign(n, 0);
  for (size_t i = 0; i < n; i++) {
    const int x = int(i % size_t(L.w)), y = int(i / size_t(L.w));
    const int region = (x / 9 + y / 7) % 4;
    if (region == 0) L.ocean[i] = u8(rng.range(0, 255));
    else if (region == 1) L.inland[i] = u8(rng.range(0, 255));
    else if (region == 2) (rng.uniform() < 0.5 ? L.ocean : L.inland)[i] = 255;
    if (rng.uniform() < 0.3) {
      L.symA[i] = u8(rng.uniform() < 0.5 ? 255 : rng.range(1, 255));
      L.symV[i] = u8(rng.range(0, 255));
    }
  }
  const double bases[2][3] = {{255, 255, 255}, {227, 147, 147}};
  for (int f : {2, 4, 8}) {
    const Layers D = downsample(L, f);
    CHECK_EQ(D.w, (L.w + f - 1) / f);
    CHECK_EQ(D.h, (L.h + f - 1) / f);
    for (const auto& base : bases) {
      double worst = 0;
      for (int oy = 0; oy < D.h; oy++)
        for (int ox = 0; ox < D.w; ox++) {
          double ref[3] = {0, 0, 0}, v[3];
          int cnt = 0;
          for (int y = oy * f; y < std::min(L.h, oy * f + f); y++)
            for (int x = ox * f; x < std::min(L.w, ox * f + f); x++) {
              compositeOver(L, size_t(y) * size_t(L.w) + size_t(x), base, v);
              for (int c = 0; c < 3; c++) ref[c] += v[c];
              cnt++;
            }
          compositeOver(D, size_t(oy) * size_t(D.w) + size_t(ox), base, v);
          for (int c = 0; c < 3; c++) worst = std::max(worst, std::fabs(v[c] - ref[c] / cnt));
        }
      CHECK_MSG(worst <= 2.5, strf("f=%d: ошибка %.2f", f, worst));
    }
  }
}
