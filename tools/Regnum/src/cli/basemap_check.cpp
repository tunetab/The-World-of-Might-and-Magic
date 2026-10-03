// Regnum — проверочные изображения и проверка готовой базовой карты (см. basemap_check.h).
#include "cli/basemap_check.h"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "base/fs.h"
#include "base/jobs.h"
#include "map/basemap.h"

namespace rg::cli::basemap {

using codec::RgbaImage;
using namespace map::bake;

namespace {

RgbaImage blank(int w, int h, Color c) {
  RgbaImage im;
  im.w = w;
  im.h = h;
  im.rgba.resize(size_t(w) * size_t(h) * 4);
  for (size_t i = 0; i < size_t(w) * size_t(h); i++) {
    im.rgba[i * 4] = c.r;
    im.rgba[i * 4 + 1] = c.g;
    im.rgba[i * 4 + 2] = c.b;
    im.rgba[i * 4 + 3] = 255;
  }
  return im;
}

inline void put(RgbaImage& im, int x, int y, Color c) {
  if (x < 0 || y < 0 || x >= im.w || y >= im.h) return;
  u8* p = im.rgba.data() + (size_t(y) * size_t(im.w) + size_t(x)) * 4;
  p[0] = c.r;
  p[1] = c.g;
  p[2] = c.b;
  p[3] = 255;
}

void line(RgbaImage& im, double x0, double y0, double x1, double y1, Color c) {
  const double len = std::hypot(x1 - x0, y1 - y0);
  const int n = int(std::ceil(len * 3)) + 1;
  for (int i = 0; i <= n; i++) {
    const double t = double(i) / n;
    put(im, int(std::floor(x0 + (x1 - x0) * t)), int(std::floor(y0 + (y1 - y0) * t)), c);
  }
}

RgbaImage crop(const RgbaImage& src, const Region& r) {
  RgbaImage im = blank(r.w, r.h, Color(0, 0, 0));
  for (int y = 0; y < r.h; y++)
    for (int x = 0; x < r.w; x++) {
      const int sx = x + r.x, sy = y + r.y;
      if (sx < 0 || sy < 0 || sx >= src.w || sy >= src.h) continue;
      std::memcpy(im.rgba.data() + (size_t(y) * size_t(r.w) + size_t(x)) * 4, src.rgba.data() + (size_t(sy) * size_t(src.w) + size_t(sx)) * 4, 4);
    }
  return im;
}

RgbaImage upscale(const RgbaImage& src, int k) {
  RgbaImage im = blank(src.w * k, src.h * k, Color(0, 0, 0));
  for (int y = 0; y < im.h; y++)
    for (int x = 0; x < im.w; x++)
      std::memcpy(im.rgba.data() + (size_t(y) * size_t(im.w) + size_t(x)) * 4, src.rgba.data() + (size_t(y / k) * size_t(src.w) + size_t(x / k)) * 4, 4);
  return im;
}

// Один слой поверх шахматки (прозрачность видна).
RgbaImage layerOnChecker(const Layers& L, int layer, const Region& r) {
  RgbaImage im = blank(r.w, r.h, Color(0, 0, 0));
  for (int y = 0; y < r.h; y++)
    for (int x = 0; x < r.w; x++) {
      const int sx = x + r.x, sy = y + r.y;
      if (sx < 0 || sy < 0 || sx >= L.w || sy >= L.h) continue;
      const size_t i = size_t(sy) * size_t(L.w) + size_t(sx);
      const double bg = ((x / 8 + y / 8) & 1) ? 200 : 245;
      double a, c[3];
      if (layer == 2) {
        a = L.symA[i] / 255.0;
        c[0] = c[1] = c[2] = L.symV[i];
      } else {
        a = (layer == 0 ? L.ocean[i] : L.inland[i]) / 255.0;
        c[0] = kOcean.r; c[1] = kOcean.g; c[2] = kOcean.b;
      }
      u8* p = im.rgba.data() + (size_t(y) * size_t(r.w) + size_t(x)) * 4;
      for (int k = 0; k < 3; k++) p[k] = u8(std::lround(bg * (1 - a) + c[k] * a));
    }
  return im;
}

Color classColor(const Layers& L, size_t i) {
  const u8 k = L.kind[i];
  if ((k & KindInk) && L.symA[i] >= 128) return (k & KindMountain) ? Color(230, 120, 0) : Color(0, 0, 0);
  if (k & KindMountain) return Color(255, 200, 140);
  if (L.inland[i] >= 128) return Color(0, 170, 60);
  if (L.inland[i]) return Color(150, 230, 170);
  if (L.ocean[i] >= 128) return Color(0, 38, 255);
  if (L.ocean[i]) return Color(150, 170, 255);
  return Color(255, 255, 255);
}

int pixelError(const RgbaImage& flat, const Layers& L, size_t i) {
  u8 rgb[3];
  compositePixel(L, i, rgb);
  int e = 0;
  for (int c = 0; c < 3; c++) e = std::max(e, std::abs(int(rgb[c]) - int(flat.rgba[i * 4 + size_t(c)])));
  return e;
}

Color errorColor(int e) {
  if (e == 0) return Color(16, 16, 16);
  if (e == 1) return Color(90, 90, 90);
  if (e == 2) return Color(255, 200, 0);
  return Color(255, 0, 0);
}

void save(const std::string& path, const RgbaImage& im) { writePng(path, im, 6); }

// Уменьшение непрозрачного изображения в f раз средним по блокам (крайние блоки — по существующим пикселям),
// как у пирамиды слоёв.
RgbaImage boxDown(const RgbaImage& src, int f) {
  RgbaImage out;
  out.w = (src.w + f - 1) / f;
  out.h = (src.h + f - 1) / f;
  out.rgba.assign(size_t(out.w) * size_t(out.h) * 4, 255);
  jobs::parallelFor(size_t(out.h), [&](size_t oy) {
    const int y0 = int(oy) * f, y1 = std::min(src.h, y0 + f);
    for (int ox = 0; ox < out.w; ox++) {
      const int x0 = ox * f, x1 = std::min(src.w, x0 + f);
      u32 sum[3] = {0, 0, 0};
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
          for (int c = 0; c < 3; c++) sum[c] += src.rgba[(size_t(y) * size_t(src.w) + size_t(x)) * 4 + size_t(c)];
      const u32 n = u32((x1 - x0) * (y1 - y0));
      for (int c = 0; c < 3; c++) out.rgba[(oy * size_t(out.w) + size_t(ox)) * 4 + size_t(c)] = u8((sum[c] + n / 2) / n);
    }
  });
  return out;
}

}  // namespace

std::vector<Region> pickRegions(const Artifacts& a, const std::vector<Region>& user) {
  std::vector<Region> out = user;
  const Layers& L = a.layers;
  constexpr int B = 128, WX = 4, WY = 3;  // окно 512 × 384
  const int bx = (L.w + B - 1) / B, by = (L.h + B - 1) / B;
  enum { Inland, Coast, Mountain, Ink, Count };
  std::vector<std::array<double, Count>> cnt(size_t(bx) * size_t(by));
  for (auto& c : cnt) c.fill(0);
  for (int y = 0; y < L.h; y++)
    for (int x = 0; x < L.w; x++) {
      const size_t i = size_t(y) * size_t(L.w) + size_t(x);
      auto& c = cnt[size_t(y / B) * size_t(bx) + size_t(x / B)];
      if (L.inland[i] >= 128) c[Inland]++;
      if (L.kind[i] & KindMountain) c[Mountain]++;
      else if (L.symA[i] >= 128) c[Ink]++;
    }
  for (const IRing& r : a.rings) {
    // Мелкие острова весят больше: ищем «рябь» островков у берега.
    const double wt = ringArea(r) < 400 ? 40 : 1;
    for (const IPt& p : r) {
      const int x = clamp(p.x / kCoordScale / B, 0, bx - 1), y = clamp(p.y / kCoordScale / B, 0, by - 1);
      cnt[size_t(y) * size_t(bx) + size_t(x)][Coast] += wt;
    }
  }
  const char* names[Count] = {"inland", "coast", "mountains", "castles"};
  for (int k = 0; k < Count; k++) {
    double best = -1;
    int bxBest = 0, byBest = 0;
    for (int y = 0; y + WY <= by; y++)
      for (int x = 0; x + WX <= bx; x++) {
        double s = 0;
        for (int dy = 0; dy < WY; dy++)
          for (int dx = 0; dx < WX; dx++) s += cnt[size_t(y + dy) * size_t(bx) + size_t(x + dx)][size_t(k)];
        if (s > best) { best = s; bxBest = x; byBest = y; }
      }
    out.push_back({names[k], bxBest * B, byBest * B, std::min(WX * B, L.w), std::min(WY * B, L.h)});
  }
  return out;
}

void writeChecks(const std::string& dir, const Artifacts& a, const std::vector<Region>& regions) {
  std::string err;
  if (!fs::makeDirs(dir, &err)) fail("Не удалось создать папку проверок: " + err);
  const Layers& L = a.layers;
  const int S = 4, ow = (L.w + S - 1) / S, oh = (L.h + S - 1) / S;

  // Обзор классов: в блоке S × S — самый важный класс.
  {
    RgbaImage cls = blank(ow, oh, Color(255, 255, 255)), dif = blank(ow, oh, Color(16, 16, 16));
    jobs::parallelFor(size_t(oh), [&](size_t oy) {
      for (int ox = 0; ox < ow; ox++) {
        int prio = -1, emax = 0;
        Color best(255, 255, 255);
        for (int y = int(oy) * S; y < std::min(L.h, int(oy) * S + S); y++)
          for (int x = ox * S; x < std::min(L.w, ox * S + S); x++) {
            const size_t i = size_t(y) * size_t(L.w) + size_t(x);
            int p = 0;
            const Color c = classColor(L, i);
            if (c == Color(0, 170, 60)) p = 6;
            else if (c == Color(0, 0, 0)) p = 5;
            else if (c == Color(230, 120, 0)) p = 4;
            else if (c == Color(0, 38, 255)) p = 3;
            else if (c == Color(150, 230, 170)) p = 2;
            else if (c != Color(255, 255, 255)) p = 1;
            if (p > prio) { prio = p; best = c; }
            emax = std::max(emax, pixelError(a.flat, L, i));
          }
        put(cls, ox, int(oy), best);
        put(dif, ox, int(oy), errorColor(emax));
      }
    });
    save(fs::join(dir, "overview_classes.png"), cls);
    save(fs::join(dir, "overview_error.png"), dif);
  }
  // Обзор берега: исходник 1:4 и кольца.
  {
    RgbaImage ov = resampleArea(a.flat, ow, oh);
    for (const IRing& r : a.rings)
      for (size_t k = 0; k < r.size(); k++) {
        const IPt p = r[k], q = r[(k + 1) % r.size()];
        line(ov, p.x / (100.0 * S), p.y / (100.0 * S), q.x / (100.0 * S), q.y / (100.0 * S), Color(255, 0, 0));
      }
    save(fs::join(dir, "overview_coast.png"), ov);
  }

  for (size_t n = 0; n < regions.size(); n++) {
    const Region& r = regions[n];
    const std::string base = fs::join(dir, strf("r%zu_%s_", n, r.name.c_str()));
    save(base + "a_original.png", crop(a.flat, r));
    save(base + "b_ocean.png", layerOnChecker(L, 0, r));
    save(base + "c_inland.png", layerOnChecker(L, 1, r));
    save(base + "d_symbols.png", layerOnChecker(L, 2, r));
    save(base + "e_tint.png", composite(L, r.x, r.y, r.w, r.h, Color(200, 40, 40, 128)));
    RgbaImage cls = blank(r.w, r.h, Color(0, 0, 0)), dif = blank(r.w, r.h, Color(0, 0, 0));
    for (int y = 0; y < r.h; y++)
      for (int x = 0; x < r.w; x++) {
        const int sx = x + r.x, sy = y + r.y;
        if (sx >= L.w || sy >= L.h) continue;
        const size_t i = size_t(sy) * size_t(L.w) + size_t(sx);
        put(cls, x, y, classColor(L, i));
        put(dif, x, y, errorColor(pixelError(a.flat, L, i)));
      }
    save(base + "f_error.png", dif);
    save(base + "g_classes.png", cls);
    // Берег 2:1 поверх исходника.
    RgbaImage co = upscale(crop(a.flat, r), 2);
    for (const IRing& ring : a.rings)
      for (size_t k = 0; k < ring.size(); k++) {
        const IPt p = ring[k], q = ring[(k + 1) % ring.size()];
        const double x0 = p.x / 100.0, y0 = p.y / 100.0, x1 = q.x / 100.0, y1 = q.y / 100.0;
        if (std::max(x0, x1) < r.x - 1 || std::min(x0, x1) > r.x + r.w + 1 || std::max(y0, y1) < r.y - 1 || std::min(y0, y1) > r.y + r.h + 1)
          continue;
        line(co, (x0 - r.x) * 2, (y0 - r.y) * 2, (x1 - r.x) * 2, (y1 - r.y) * 2, Color(255, 0, 0));
        put(co, int(std::floor((x0 - r.x) * 2)), int(std::floor((y0 - r.y) * 2)), Color(255, 220, 0));
      }
    save(base + "h_coast2x.png", co);
  }
}

bool verifyOutput(const std::string& dir, const Artifacts& a, const Report& rep) {
  map::Basemap bm;
  std::string err;
  const double t0 = nowSeconds();
  if (!bm.load(dir, &err)) {
    std::printf("  проверка: не загружается: %s\n", err.c_str());
    return false;
  }
  const double tLoad = nowSeconds() - t0;
  bool ok = true;
  const Layers& L = a.layers;
  if (bm.width() != L.w || bm.height() != L.h || bm.layers().size() != 3) {
    std::printf("  проверка: размеры или слои манифеста не совпадают\n");
    return false;
  }
  // Декодирование всех тайлов всех уровней.
  std::vector<Layers> lv(size_t(bm.levels()));
  for (int z = 0; z < bm.levels(); z++) {
    const auto& l = bm.level(z);
    Layers& D = lv[size_t(z)];
    D.w = l.w;
    D.h = l.h;
    const size_t n = size_t(l.w) * size_t(l.h);
    D.ocean.assign(n, 0);
    D.inland.assign(n, 0);
    D.symA.assign(n, 0);
    D.symV.assign(n, 0);
  }
  struct Job { int z, layer, x, y; };
  std::vector<Job> jl;
  for (int z = 0; z < bm.levels(); z++)
    for (int layer = 0; layer < 3; layer++)
      for (int y = 0; y < bm.level(z).rows; y++)
        for (int x = 0; x < bm.level(z).cols; x++) jl.push_back({z, layer, x, y});
  std::atomic<i64> colored{0}, bad{0};
  std::mutex mu;
  std::string firstErr;
  const double t1 = nowSeconds();
  jobs::parallelFor(jl.size(), [&](size_t k) {
    const Job& j = jl[k];
    const char* name = kLayerNames[j.layer];
    if (!bm.tileExists(name, j.z, j.x, j.y)) return;
    std::string e;
    auto img = bm.readTile(name, j.z, j.x, j.y, &e);
    if (!img) {
      bad++;
      std::lock_guard<std::mutex> g(mu);
      if (firstErr.empty()) firstErr = e;
      return;
    }
    const map::TileRect r = bm.tileRect(j.z, j.x, j.y);
    Layers& D = lv[size_t(j.z)];
    i64 col = 0;
    for (int y = 0; y < r.h; y++)
      for (int x = 0; x < r.w; x++) {
        const u8* p = img->rgba.data() + (size_t(y) * size_t(r.w) + size_t(x)) * 4;
        const size_t i = size_t(r.y + y) * size_t(D.w) + size_t(r.x + x);
        if (j.layer == 2) {
          D.symA[i] = p[3];
          D.symV[i] = p[3] ? p[0] : 0;
          if (p[3] && (p[0] != p[1] || p[1] != p[2])) col++;
        } else {
          (j.layer == 0 ? D.ocean : D.inland)[i] = p[3];
          if (p[3] && (p[0] != kOcean.r || p[1] != kOcean.g || p[2] != kOcean.b)) col++;
        }
      }
    colored += col;
  });
  const double tDecode = nowSeconds() - t1;
  if (bad) {
    std::printf("  проверка: не читаются %lld тайлов: %s\n", (long long)bad.load(), firstErr.c_str());
    ok = false;
  }
  if (colored) {
    std::printf("  проверка: %lld пикселей с посторонним цветом (символы не серые или вода не #0026ff)\n", (long long)colored.load());
    ok = false;
  }
  // Уровень 0 — побайтно как слои сборки; композиция против исходника.
  i64 diff0 = 0;
  for (size_t i = 0; i < L.ocean.size(); i++)
    diff0 += (lv[0].ocean[i] != L.ocean[i]) + (lv[0].inland[i] != L.inland[i]) + (lv[0].symA[i] != L.symA[i]) + (lv[0].symV[i] != L.symV[i]);
  const CompositeError ce = compareComposite(a.flat, lv[0]);
  std::printf("  проверка: загрузка %.0f мс, декодирование %zu тайлов %.0f мс\n", tLoad * 1000, jl.size(), tDecode * 1000);
  std::printf("  проверка: уровень 0 из тайлов — расхождений со слоями %lld; композиция: средняя ошибка %.4f, max %d, пикселей с ошибкой > 1: %lld, > 2: %lld\n",
              (long long)diff0, ce.mean, ce.max, (long long)ce.over1, (long long)ce.over2);
  if (diff0) ok = false;
  for (int z = 1; z < bm.levels(); z++) {
    const Layers ds = downsample(lv[0], 1 << z);
    i64 d = 0;
    for (size_t i = 0; i < ds.ocean.size(); i++)
      d += (ds.ocean[i] != lv[size_t(z)].ocean[i]) + (ds.inland[i] != lv[size_t(z)].inland[i]) + (ds.symA[i] != lv[size_t(z)].symA[i]) +
           (ds.symV[i] != lv[size_t(z)].symV[i]);
    const CompositeError ze = compareComposite(boxDown(a.flat, 1 << z), lv[size_t(z)]);
    std::printf("  проверка: уровень %d (%d × %d) — расхождений с уменьшением уровня 0: %lld; композиция против уменьшенного исходника: "
                "средняя ошибка %.4f, max %d\n",
                z, ds.w, ds.h, (long long)d, ze.mean, ze.max);
    if (d || ze.mean >= 1.0 || ze.max > 8) ok = false;
  }
  // Берег.
  const geo::Coast coast = bm.coast();
  i64 pts = 0, mism = 0;
  for (size_t r = 0; r < coast.landRings.size(); r++) {
    pts += i64(coast.landRings[r].size());
    if (r >= a.rings.size() || coast.landRings[r].size() != a.rings[r].size()) {
      mism++;
      continue;
    }
    for (size_t k = 0; k < a.rings[r].size(); k++)
      if (std::abs(coast.landRings[r][k].x * 100 - a.rings[r][k].x) > 1e-6 || std::abs(coast.landRings[r][k].y * 100 - a.rings[r][k].y) > 1e-6) {
        mism++;
        break;
      }
  }
  std::printf("  проверка: берег — колец %zu, точек %lld, расхождений %lld\n", coast.landRings.size(), (long long)pts, (long long)mism);
  if (mism || int(coast.landRings.size()) != rep.coast.rings) ok = false;
  // Берег из coast.json — в настоящее ядро геометрии.
  const GeoCheck g = geoCheck(coast);
  std::printf("  проверка: ядро геометрии по coast.json — %s, граней суши %d, моря %d, замечаний geo::validate %zu (%.0f мс)\n",
              g.built ? "построено" : g.error.c_str(), g.landFaces, g.seaFaces, g.issues.size(), g.seconds * 1000);
  for (size_t k = 0; k < g.issues.size() && k < 5; k++) std::printf("    %s\n", g.issues[k].c_str());
  if (!g.ok()) ok = false;
  if (ce.mean >= 1.0 || ce.max > 8) ok = false;
  // Маска против слоя моря (в центре клетки маски).
  i64 agree = 0, total = 0;
  for (int my = 0; my < bm.maskHeight(); my++)
    for (int mx = 0; mx < bm.maskWidth(); mx++) {
      const int x = std::min(L.w - 1, mx * bm.maskScale() + bm.maskScale() / 2), y = std::min(L.h - 1, my * bm.maskScale() + bm.maskScale() / 2);
      const bool sea = L.field[size_t(y) * size_t(L.w) + size_t(x)] >= 128;
      agree += sea == bm.isOcean(Vec2(x + 0.5, y + 0.5));
      total++;
    }
  std::printf("  проверка: маска моря совпадает с полем берега в %.3f%% клеток\n", 100.0 * double(agree) / double(total));
  std::printf("  проверка: %s\n", ok ? "пройдена" : "НЕ ПРОЙДЕНА");
  return ok;
}

}  // namespace rg::cli::basemap
