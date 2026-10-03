// Тесты сборки базовой карты (пирамиды тайлов, манифест, берег, маска, превью) и загрузчика map::Basemap,
// в том числе проверка готовых файлов tools/Regnum/assets/basemap.
#include "base/json.h"
#include "map/basemap.h"
#include "tests/test_map_basemap_util.h"

using namespace rg;
using namespace rg::map::bake;

namespace {

Options smallOptions() {
  Options o;
  o.tile = 128;
  o.levels = 3;
  o.previewLevel = 1;
  o.thumbWidth = 100;
  o.pngLevel = 6;
  return o;
}

struct Built {
  bmtest::SynthMap m;
  std::string dir;
  Report rep;
  Artifacts art;
};

const Built& built() {
  static const Built b = [] {
    Built r;
    r.m = bmtest::makeSynth();
    r.dir = fs::join(bmtest::freshDir("basemap_build"), "out");
    r.rep = build(r.m.img, r.dir, smallOptions(), &r.art);
    return r;
  }();
  return b;
}

codec::RgbaImage readPng(const std::string& path) {
  auto bytes = fs::readFile(path);
  if (!bytes) test::fail(__FILE__, __LINE__, "нет файла " + path);
  auto img = codec::decodePng(reinterpret_cast<const u8*>(bytes->data()), bytes->size());
  if (!img) test::fail(__FILE__, __LINE__, "не PNG: " + path);
  return *img;
}

void writeText(const std::string& path, const std::string& text) {
  if (!fs::writeFileAtomic(path, std::string_view(text))) test::fail(__FILE__, __LINE__, "не записать " + path);
}

// Копия собранной папки для порчи.
std::string copyOf(const std::string& src, const std::string& name) {
  const std::string dst = bmtest::freshDir(name);
  for (const std::string& f : fs::listTree(src)) {
    fs::makeDirs(fs::parent(fs::join(dst, f)));
    fs::copyFile(fs::join(src, f), fs::join(dst, f));
  }
  return dst;
}

}  // namespace

TEST(map_basemap_level_grid) {
  const auto g = levelGrid(8000, 4500, 512, 4);
  CHECK_EQ(g.size(), size_t(4));
  const int expect[4][4] = {{8000, 4500, 16, 9}, {4000, 2250, 8, 5}, {2000, 1125, 4, 3}, {1000, 563, 2, 2}};
  for (int z = 0; z < 4; z++) {
    CHECK_EQ(g[size_t(z)].z, z);
    CHECK_EQ(g[size_t(z)].w, expect[z][0]);
    CHECK_EQ(g[size_t(z)].h, expect[z][1]);
    CHECK_EQ(g[size_t(z)].cols, expect[z][2]);
    CHECK_EQ(g[size_t(z)].rows, expect[z][3]);
  }
}

TEST(map_basemap_sha256) {
  CHECK_EQ(sha256Hex("", 0), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(sha256Hex("abc", 3), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  const std::string m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";  // 56 байт: дополнение во второй блок
  CHECK_EQ(sha256Hex(m.data(), m.size()), std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  const std::string a(1000, 'a');
  CHECK_EQ(sha256Hex(a.data(), a.size()), std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
}

TEST(map_basemap_resample_area) {
  codec::RgbaImage im;
  im.w = 4;
  im.h = 2;
  im.rgba = {0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255,
             0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255};
  const codec::RgbaImage r = resampleArea(im, 2, 1);
  CHECK_EQ(r.w, 2);
  CHECK_EQ(r.h, 1);
  CHECK_EQ(int(r.rgba[0]), 128);
  CHECK_EQ(int(r.rgba[3]), 255);
  const codec::RgbaImage r3 = resampleArea(im, 3, 2);  // дробные доли: 4/3 пикселя на выход
  CHECK_EQ(int(r3.rgba[0]), 64);                       // 1·0 + 1/3·255 → 255/4
}

TEST(map_basemap_build_report) {
  const Built& b = built();
  const Report& r = b.rep;
  CHECK(r.check.ok());
  CHECK(r.geo.ok());
  CHECK_EQ(r.geo.landFaces, r.coast.rings);
  CHECK_MSG(r.error.mean < 0.05 && r.error.max <= 2, strf("композиция: %.4f / %d", r.error.mean, r.error.max));
  CHECK_MSG(r.previewError.max <= 2, strf("превью: %d", r.previewError.max));
  CHECK_EQ(r.tiles + r.emptyTiles, 3 * (4 * 3 + 2 * 2 + 1 * 1));
  CHECK(r.emptyTiles > 0);
  CHECK(r.bytes > r.tileBytes);
  // Во временной папке ничего не осталось.
  CHECK(!fs::exists(fs::join(fs::parent(b.dir), ".out.building")));
}

TEST(map_basemap_build_manifest) {
  const Built& b = built();
  auto text = fs::readFile(fs::join(b.dir, "manifest.json"));
  CHECK(text.has_value());
  const json::Value m = json::parse(*text);
  CHECK_EQ(m.str("id"), std::string("wmm-expanded-v1"));
  CHECK_EQ(m.str("source"), std::string("Expanded Map.png"));
  CHECK_EQ(int(m.num("width")), 480);
  CHECK_EQ(int(m.num("height")), 320);
  CHECK_EQ(int(m.num("tile")), 128);
  const auto& lv = m.arr("levels");
  CHECK_EQ(lv.size(), size_t(3));
  CHECK_EQ(int(lv[1].num("w")), 240);
  CHECK_EQ(int(lv[1].num("h")), 160);
  CHECK_EQ(int(lv[1].num("cols")), 2);
  CHECK_EQ(int(lv[1].num("rows")), 2);
  CHECK_EQ(m.arr("layers").size(), size_t(3));
  CHECK_EQ(m.arr("layers")[0].asStr(), std::string("ocean"));
  CHECK_EQ(m.arr("layers")[2].asStr(), std::string("symbols"));
  CHECK_EQ(m.obj("colors").str("ocean"), std::string("#0026ff"));
  CHECK_EQ(m.obj("colors").str("land"), std::string("#ffffff"));
  size_t empties = 0;
  for (const auto& [layer, list] : m.obj("empty").members()) {
    (void)layer;
    empties += list.size();
  }
  CHECK_EQ(int(empties), b.rep.emptyTiles);
}

TEST(map_basemap_build_tiles_roundtrip) {
  const Built& b = built();
  map::Basemap bm;
  std::string err;
  CHECK_MSG(bm.load(b.dir, &err), err);
  CHECK_EQ(bm.width(), 480);
  CHECK_EQ(bm.height(), 320);
  CHECK_EQ(bm.levels(), 3);
  CHECK_EQ(bm.tileSize(), 128);
  CHECK(bm.oceanColor() == kOcean);
  CHECK(bm.landColor() == kLand);
  int checked = 0, empty = 0;
  for (int z = 0; z < bm.levels(); z++) {
    const Layers Lz = z == 0 ? b.art.layers : downsample(b.art.layers, 1 << z);
    CHECK_EQ(bm.level(z).w, Lz.w);
    CHECK_EQ(bm.level(z).h, Lz.h);
    for (int li = 0; li < 3; li++)
      for (int y = 0; y < bm.level(z).rows; y++)
        for (int x = 0; x < bm.level(z).cols; x++) {
          const map::TileRect r = bm.tileRect(z, x, y);
          bool isEmpty = false;
          const codec::RgbaImage ref = tileImage(Lz, li, r.x, r.y, r.w, r.h, &isEmpty);
          CHECK_EQ(bm.tileExists(kLayerNames[li], z, x, y), !isEmpty);
          const std::string path = bm.tilePath(kLayerNames[li], z, x, y);
          CHECK_EQ(fs::exists(path), !isEmpty);
          if (isEmpty) {
            empty++;
            CHECK(!bm.readTile(kLayerNames[li], z, x, y).has_value());
            continue;
          }
          std::string e;
          auto img = bm.readTile(kLayerNames[li], z, x, y, &e);
          CHECK_MSG(img.has_value(), e);
          CHECK_EQ(img->w, r.w);
          CHECK_EQ(img->h, r.h);
          // Пустые пиксели в PNG могут храниться с любым цветом: сравниваем альфу и цвет непустых.
          bool same = true;
          for (size_t i = 0; i < ref.rgba.size() && same; i += 4) {
            if (img->rgba[i + 3] != ref.rgba[i + 3]) same = false;
            else if (ref.rgba[i + 3] && (img->rgba[i] != ref.rgba[i] || img->rgba[i + 1] != ref.rgba[i + 1] || img->rgba[i + 2] != ref.rgba[i + 2]))
              same = false;
          }
          CHECK_MSG(same, strf("тайл %s z%d %d,%d", kLayerNames[li], z, x, y));
          checked++;
        }
  }
  CHECK_EQ(checked, b.rep.tiles);
  CHECK_EQ(empty, b.rep.emptyTiles);
}

TEST(map_basemap_loader_queries) {
  const Built& b = built();
  map::Basemap bm;
  CHECK(bm.load(b.dir));
  // Пути тайлов.
  CHECK(endsWith(bm.tilePath("ocean", 1, 1, 0), "/L1/ocean_1_0.png"));
  CHECK(bm.tilePath("rivers", 0, 0, 0).empty());
  CHECK(bm.tilePath("ocean", 0, 4, 0).empty());
  CHECK(bm.tilePath("ocean", 3, 0, 0).empty());
  CHECK(!bm.tileExists("ocean", -1, 0, 0));
  CHECK_EQ(bm.layerIndex("inland"), 1);
  CHECK_EQ(bm.layerIndex("nope"), -1);
  // Крайний тайл обрезан по размеру уровня.
  const map::TileRect r = bm.tileRect(0, 3, 2);
  CHECK_EQ(r.x, 384);
  CHECK_EQ(r.y, 256);
  CHECK_EQ(r.w, 96);
  CHECK_EQ(r.h, 64);
  CHECK_EQ(bm.tileRect(0, 4, 0).w, 0);
  // Уровень для масштаба: 1:1 и крупнее — 0, мельче — грубее, но не грубее последнего.
  CHECK_EQ(bm.levelFor(2.0), 0);
  CHECK_EQ(bm.levelFor(1.0), 0);
  CHECK_EQ(bm.levelFor(0.6), 0);
  CHECK_EQ(bm.levelFor(0.5), 1);
  CHECK_EQ(bm.levelFor(0.3), 1);
  CHECK_EQ(bm.levelFor(0.25), 2);
  CHECK_EQ(bm.levelFor(0.01), 2);
  // Маска моря.
  CHECK_EQ(bm.maskScale(), 4);
  CHECK_EQ(bm.maskWidth(), 120);
  CHECK_EQ(bm.maskHeight(), 80);
  CHECK(bm.isOcean(b.m.sea));
  CHECK(bm.isOcean(b.m.bay));
  CHECK(!bm.isOcean(b.m.land));
  CHECK(!bm.isOcean(b.m.lake));
  CHECK(!bm.isOcean(Vec2(-1, 5)));
  CHECK(!bm.isOcean(Vec2(480, 5)));
  CHECK(!bm.isOcean(Vec2(std::nan(""), 5)));
  // Берег из coast.json совпадает с кольцами сборки (сотые доли).
  const geo::Coast c = bm.coast();
  CHECK_EQ(c.width, 480.0);
  CHECK_EQ(c.height, 320.0);
  CHECK_EQ(c.landRings.size(), b.art.rings.size());
  for (size_t k = 0; k < c.landRings.size(); k++) {
    CHECK_EQ(c.landRings[k].size(), b.art.rings[k].size());
    for (size_t i = 0; i < c.landRings[k].size(); i++) {
      CHECK_NEAR(c.landRings[k][i].x, b.art.rings[k][i].x / 100.0, 1e-9);
      CHECK_NEAR(c.landRings[k][i].y, b.art.rings[k][i].y / 100.0, 1e-9);
    }
  }
  CHECK(geoCheck(c).ok());
  // Превью и миниатюра.
  const codec::RgbaImage pv = readPng(bm.previewPath());
  CHECK_EQ(pv.w, 240);
  CHECK_EQ(pv.h, 160);
  const codec::RgbaImage th = readPng(bm.thumbPath());
  CHECK_EQ(th.w, 100);
  CHECK_EQ(th.h, 67);
}

TEST(map_basemap_build_deterministic) {
  const Built& b = built();
  bmtest::SynthMap m = bmtest::makeSynth();
  const std::string dir2 = fs::join(bmtest::freshDir("basemap_build_again"), "out");
  build(m.img, dir2, smallOptions());
  const auto a = fs::listTree(b.dir), c = fs::listTree(dir2);
  CHECK(a == c);
  for (const std::string& f : a) CHECK_MSG(fs::readFile(fs::join(b.dir, f)) == fs::readFile(fs::join(dir2, f)), f);
}

TEST(map_basemap_build_rejects_bad_options) {
  bmtest::SynthMap m = bmtest::makeSynth();
  const std::string dir = fs::join(bmtest::freshDir("basemap_build_bad"), "out");
  Options o = smallOptions();
  o.tile = 16;
  CHECK_THROWS(build(m.img, dir, o));
  o = smallOptions();
  o.previewLevel = 5;
  CHECK_THROWS(build(m.img, dir, o));
  CHECK_THROWS(build(codec::RgbaImage{}, dir, smallOptions()));
  CHECK_THROWS(build(m.img, "", smallOptions()));
}

TEST(map_basemap_loader_errors) {
  const Built& b = built();
  std::string err;
  map::Basemap bm;
  CHECK(!bm.load(fs::join(b.dir, "nope"), &err));
  CHECK(!err.empty());
  CHECK(!bm.loaded());
  CHECK_THROWS(bm.coast());

  // Нет непустого тайла.
  {
    const std::string d = copyOf(b.dir, "basemap_bad_tile");
    std::string victim;
    for (const std::string& f : fs::listTree(d))
      if (startsWith(f, "L0/symbols_")) victim = f;
    CHECK(!victim.empty());
    fs::remove(fs::join(d, victim));
    err.clear();
    CHECK(!bm.load(d, &err));
    CHECK_MSG(err.find("нет тайла") != std::string::npos, err);
  }
  // Уровень не согласован с размерами карты.
  {
    const std::string d = copyOf(b.dir, "basemap_bad_level");
    json::Value m = json::parse(*fs::readFile(fs::join(d, "manifest.json")));
    json::Value lv = m.get("levels");
    lv[1].set("w", 241);
    m.set("levels", lv);
    writeText(fs::join(d, "manifest.json"), json::write(m));
    CHECK(!bm.load(d, &err));
  }
  // Пустой тайл неизвестного слоя.
  {
    const std::string d = copyOf(b.dir, "basemap_bad_empty");
    json::Value m = json::parse(*fs::readFile(fs::join(d, "manifest.json")));
    json::Value e = m.get("empty");
    e.set("rivers", json::Value::array());
    m.set("empty", e);
    writeText(fs::join(d, "manifest.json"), json::write(m));
    CHECK(!bm.load(d, &err));
  }
  // Маска другого размера.
  {
    const std::string d = copyOf(b.dir, "basemap_bad_mask");
    codec::RgbaImage mk;
    mk.w = 10;
    mk.h = 10;
    mk.rgba.assign(400, 255);
    writePng(fs::join(d, "mask.png"), mk, 1);
    CHECK(!bm.load(d, &err));
  }
  // Повреждённый манифест.
  {
    const std::string d = copyOf(b.dir, "basemap_bad_json");
    writeText(fs::join(d, "manifest.json"), "{ \"id\": ");
    CHECK(!bm.load(d, &err));
  }
  // Повреждённый coast.json: загрузка проходит, разбор берега — ошибка.
  {
    const std::string d = copyOf(b.dir, "basemap_bad_coast");
    writeText(fs::join(d, "coast.json"), "{\"width\": 480, \"height\": 320, \"rings\": [[1, 2, 3]]}");
    CHECK(bm.load(d, &err));
    CHECK_THROWS(bm.coast());
    writeText(fs::join(d, "coast.json"), "{\"width\": 480, \"height\": 320, \"rings\": [[1, 2, 3, 4, 5000, 6]]}");
    CHECK_THROWS(bm.coast());
    writeText(fs::join(d, "coast.json"), "{\"width\": 400, \"height\": 320, \"rings\": []}");
    CHECK_THROWS(bm.coast());
    writeText(fs::join(d, "coast.json"), "{\"width\": 480, \"height\": 320, \"rings\": []}");
    CHECK(bm.coast().landRings.empty());
  }
}

TEST(map_basemap_real_assets) {
  // Готовая базовая карта в репозитории (собрана regnum-cli build-basemap из assets/source/Expanded Map.png).
  const std::string dir = "tools/Regnum/assets/basemap";
  map::Basemap bm;
  std::string err;
  CHECK_MSG(bm.load(dir, &err), err);
  CHECK_EQ(bm.id(), std::string("wmm-expanded-v1"));
  CHECK_EQ(bm.width(), 8000);
  CHECK_EQ(bm.height(), 4500);
  CHECK_EQ(bm.tileSize(), 512);
  CHECK_EQ(bm.levels(), 4);
  CHECK_EQ(bm.level(3).w, 1000);
  CHECK_EQ(bm.level(3).h, 563);
  CHECK_EQ(bm.maskWidth(), 2000);
  CHECK_EQ(bm.maskHeight(), 1125);
  CHECK_EQ(bm.sourceSha256().size(), size_t(64));
  // Уровень 2 из тайлов в композиции совпадает с preview.png.
  const int z = 2;
  Layers L;
  L.w = bm.level(z).w;
  L.h = bm.level(z).h;
  const size_t n = size_t(L.w) * size_t(L.h);
  L.ocean.assign(n, 0);
  L.inland.assign(n, 0);
  L.symA.assign(n, 0);
  L.symV.assign(n, 0);
  for (int li = 0; li < 3; li++)
    for (int y = 0; y < bm.level(z).rows; y++)
      for (int x = 0; x < bm.level(z).cols; x++) {
        if (!bm.tileExists(kLayerNames[li], z, x, y)) continue;
        auto img = bm.readTile(kLayerNames[li], z, x, y, &err);
        CHECK_MSG(img.has_value(), err);
        const map::TileRect r = bm.tileRect(z, x, y);
        for (int yy = 0; yy < r.h; yy++)
          for (int xx = 0; xx < r.w; xx++) {
            const u8* p = img->rgba.data() + (size_t(yy) * size_t(r.w) + size_t(xx)) * 4;
            const size_t i = size_t(r.y + yy) * size_t(L.w) + size_t(r.x + xx);
            if (li == 0) L.ocean[i] = p[3];
            else if (li == 1) L.inland[i] = p[3];
            else {
              L.symA[i] = p[3];
              L.symV[i] = p[3] ? p[0] : 0;
              CHECK(!p[3] || (p[0] == p[1] && p[1] == p[2]));  // символы без цвета
            }
          }
      }
  const codec::RgbaImage pv = readPng(bm.previewPath());
  CHECK_EQ(pv.w, L.w);
  CHECK_EQ(pv.h, L.h);
  const CompositeError e = compareComposite(pv, L);
  CHECK_EQ(e.max, 0);
  const codec::RgbaImage th = readPng(bm.thumbPath());
  CHECK_EQ(th.w, 480);
  CHECK_EQ(th.h, 270);
  // Берег — в настоящее ядро геометрии.
  const geo::Coast c = bm.coast();
  CHECK(c.landRings.size() > 100);
  const GeoCheck g = geoCheck(c);
  CHECK_MSG(g.ok(), g.built ? (g.issues.empty() ? "площадь" : g.issues[0]) : g.error);
  // Маска: угол карты — море, середина крупного материка и озеро — нет.
  CHECK(bm.isOcean(Vec2(10, 10)));
  CHECK(!bm.isOcean(Vec2(4000, 2600)));
  CHECK(!bm.isOcean(Vec2(3960, 640)));
}
