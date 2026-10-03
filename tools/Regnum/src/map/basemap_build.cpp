// Regnum — сборка базовой карты: пирамиды тайлов, береговая линия, маска, превью, манифест (см. basemap_build.h).
#include <chrono>
#include <cstring>
#include <thread>

#include "base/fs.h"
#include "base/jobs.h"
#include "base/json.h"
#include "map/basemap_build.h"

namespace rg::map::bake {

using codec::RgbaImage;

const char* const kLayerNames[3] = {"ocean", "inland", "symbols"};

// ---------------------------------------------------------------- пирамида
std::vector<Level> levelGrid(int w, int h, int tile, int levels) {
  std::vector<Level> out;
  for (int z = 0; z < levels; z++) {
    const int f = 1 << z;
    Level l;
    l.z = z;
    l.w = (w + f - 1) / f;
    l.h = (h + f - 1) / f;
    l.cols = (l.w + tile - 1) / tile;
    l.rows = (l.h + tile - 1) / tile;
    out.push_back(l);
  }
  return out;
}

// Уменьшение, точное для композиции поверх однородной основы (белой суши или заливки провинции):
//   символы — среднее в premultiplied;
//   покрытие водой cov = 1 − (1 − ocean)(1 − inland) усредняется с весом прозрачности символов (1 − symA),
//   ocean — так же взвешенное среднее, inland — недостающее до cov покрытие поверх ocean.
// Тогда symA·symV + (1 − symA)·(основа·(1 − cov) + синий·cov) совпадает со средним композиций исходных пикселей,
// в том числе в устьях рек (стык моря и внутренних вод) и под краской знаков над водой.
Layers downsample(const Layers& L, int f) {
  Layers o;
  o.w = (L.w + f - 1) / f;
  o.h = (L.h + f - 1) / f;
  const size_t n = size_t(o.w) * size_t(o.h);
  o.ocean.assign(n, 0);
  o.inland.assign(n, 0);
  o.symA.assign(n, 0);
  o.symV.assign(n, 0);
  jobs::parallelFor(size_t(o.h), [&](size_t oy) {
    struct Acc { u64 a = 0, p = 0, t = 0, to = 0, tc = 0, o = 0, i = 0; };
    std::vector<Acc> acc(size_t(o.w));
    const int y0 = int(oy) * f, y1 = std::min(L.h, y0 + f);
    for (int y = y0; y < y1; y++) {
      const size_t row = size_t(y) * size_t(L.w);
      for (int x = 0; x < L.w; x++) {
        const size_t i = row + size_t(x);
        Acc& s = acc[size_t(x / f)];
        const u32 a = L.symA[i], t = 255 - a, wo = L.ocean[i], wi = L.inland[i];
        const u32 cov = 65025 - (255 - wo) * (255 - wi);  // покрытие водой · 255²
        s.a += a;
        s.p += a * L.symV[i];
        s.t += t;
        s.to += t * wo;
        s.tc += u64(t) * cov;
        s.o += wo;
        s.i += wi;
      }
    }
    for (int ox = 0; ox < o.w; ox++) {
      const int x0 = ox * f, x1 = std::min(L.w, x0 + f);
      const u64 cnt = u64((x1 - x0) * (y1 - y0));
      const size_t i = oy * size_t(o.w) + size_t(ox);
      const Acc& s = acc[size_t(ox)];
      o.symA[i] = u8((s.a + cnt / 2) / cnt);
      o.symV[i] = (o.symA[i] && s.a) ? u8((s.p + s.a / 2) / s.a) : 0;
      if (s.t == 0) {  // знак закрывает блок целиком: вода не видна, обычное среднее
        o.ocean[i] = u8((s.o + cnt / 2) / cnt);
        o.inland[i] = u8((s.i + cnt / 2) / cnt);
        continue;
      }
      const u8 oc = u8((s.to + s.t / 2) / s.t);
      const double cov = double(s.tc) / (double(s.t) * 65025.0);
      double in = 0;
      if (oc < 255) in = 1.0 - (1.0 - cov) / (1.0 - oc / 255.0);
      o.ocean[i] = oc;
      o.inland[i] = u8(clamp(int(std::lround(in * 255.0)), 0, 255));
    }
  });
  return o;
}

RgbaImage tileImage(const Layers& L, int layer, int x0, int y0, int w, int h, bool* empty) {
  RgbaImage img;
  img.w = w;
  img.h = h;
  img.rgba.assign(size_t(w) * size_t(h) * 4, 0);
  bool any = false;
  for (int y = 0; y < h; y++) {
    const size_t row = size_t(y + y0) * size_t(L.w) + size_t(x0);
    u8* o = img.rgba.data() + size_t(y) * size_t(w) * 4;
    for (int x = 0; x < w; x++, o += 4) {
      const size_t i = row + size_t(x);
      if (layer == 2) {
        const u8 a = L.symA[i];
        if (!a) continue;
        o[0] = o[1] = o[2] = L.symV[i];
        o[3] = a;
      } else {
        const u8 a = layer == 0 ? L.ocean[i] : L.inland[i];
        if (!a) continue;
        o[0] = kOcean.r;
        o[1] = kOcean.g;
        o[2] = kOcean.b;
        o[3] = a;
      }
      any = true;
    }
  }
  if (empty) *empty = !any;
  return img;
}

// ---------------------------------------------------------------- изображения
void writePng(const std::string& path, const RgbaImage& img, int level) {
  codec::PngEncodeOptions po;
  po.level = clamp(level, 0, 9);
  po.reduce = true;
  const std::vector<u8> data = codec::encodePng(img, po);
  std::string err;
  if (data.empty() || !fs::writeFileAtomic(path, std::span<const u8>(data), &err))
    fail("Не удалось записать " + path + (err.empty() ? std::string() : ": " + err));
}

RgbaImage resampleArea(const RgbaImage& src, int nw, int nh) {
  struct Tap { int i; float wt; };
  auto taps = [](int sn, int dn) {
    std::vector<std::vector<Tap>> t(static_cast<size_t>(dn));
    const double s = double(sn) / dn;
    for (int d = 0; d < dn; d++) {
      const double a = d * s, b = std::min(double(sn), (d + 1) * s);
      for (int i = int(std::floor(a)); i < sn && i < b; i++) {
        const double ov = std::min(b, i + 1.0) - std::max(a, double(i));
        if (ov > 1e-12) t[size_t(d)].push_back({i, float(ov / (b - a))});
      }
    }
    return t;
  };
  const auto tx = taps(src.w, nw), ty = taps(src.h, nh);
  // Горизонтальный проход в premultiplied float.
  std::vector<float> mid(size_t(nw) * size_t(src.h) * 4);
  jobs::parallelFor(size_t(src.h), [&](size_t y) {
    const u8* s = src.rgba.data() + y * size_t(src.w) * 4;
    float* m = mid.data() + y * size_t(nw) * 4;
    for (int x = 0; x < nw; x++) {
      float acc[4] = {0, 0, 0, 0};
      for (const Tap& t : tx[size_t(x)]) {
        const u8* p = s + size_t(t.i) * 4;
        const float a = p[3] / 255.f * t.wt;
        acc[0] += p[0] * a;
        acc[1] += p[1] * a;
        acc[2] += p[2] * a;
        acc[3] += a;
      }
      std::memcpy(m + size_t(x) * 4, acc, sizeof(acc));
    }
  });
  RgbaImage out;
  out.w = nw;
  out.h = nh;
  out.rgba.assign(size_t(nw) * size_t(nh) * 4, 0);
  jobs::parallelFor(size_t(nh), [&](size_t y) {
    for (int x = 0; x < nw; x++) {
      float acc[4] = {0, 0, 0, 0};
      for (const Tap& t : ty[y]) {
        const float* m = mid.data() + (size_t(t.i) * size_t(nw) + size_t(x)) * 4;
        for (int c = 0; c < 4; c++) acc[c] += m[c] * t.wt;
      }
      u8* o = out.rgba.data() + (y * size_t(nw) + size_t(x)) * 4;
      if (acc[3] <= 1e-6f) continue;
      for (int c = 0; c < 3; c++) o[c] = u8(clamp(int(std::lround(acc[c] / acc[3])), 0, 255));
      o[3] = u8(clamp(int(std::lround(acc[3] * 255.f)), 0, 255));
    }
  });
  return out;
}

// ---------------------------------------------------------------- SHA-256
std::string sha256Hex(const void* data, size_t size) {
  static const u32 K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
      0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
      0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
      0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
      0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
      0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  u32 hs[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  auto rotr = [](u32 x, int n) { return (x >> n) | (x << (32 - n)); };
  auto block = [&](const u8* p) {
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = (u32(p[i * 4]) << 24) | (u32(p[i * 4 + 1]) << 16) | (u32(p[i * 4 + 2]) << 8) | u32(p[i * 4 + 3]);
    for (int i = 16; i < 64; i++) {
      const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = hs[0], b = hs[1], c = hs[2], d = hs[3], e = hs[4], f = hs[5], g = hs[6], h = hs[7];
    for (int i = 0; i < 64; i++) {
      const u32 S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g);
      const u32 t1 = h + S1 + ch + K[i] + w[i];
      const u32 S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
      const u32 t2 = S0 + mj;
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    hs[0] += a; hs[1] += b; hs[2] += c; hs[3] += d; hs[4] += e; hs[5] += f; hs[6] += g; hs[7] += h;
  };
  const u8* p = static_cast<const u8*>(data);
  size_t full = size / 64;
  for (size_t i = 0; i < full; i++) block(p + i * 64);
  u8 tail[128] = {};
  const size_t rest = size - full * 64;
  if (rest) std::memcpy(tail, p + full * 64, rest);
  tail[rest] = 0x80;
  const size_t tl = rest + 9 <= 64 ? 64 : 128;
  const u64 bits = u64(size) * 8;
  for (int i = 0; i < 8; i++) tail[tl - 1 - size_t(i)] = u8(bits >> (8 * i));
  for (size_t o = 0; o < tl; o += 64) block(tail + o);
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (u32 v : hs)
    for (int s = 28; s >= 0; s -= 4) out += hex[(v >> s) & 15];
  return out;
}

// ---------------------------------------------------------------- сборка
namespace {

// Целое в сотых долях -> десятичная запись без лишних нулей.
void appendFixed(std::string& out, i32 v) {
  if (v < 0) {
    out += '-';
    v = -v;
  }
  out += std::to_string(v / kCoordScale);
  const int frac = v % kCoordScale;
  if (frac) {
    out += '.';
    out += char('0' + frac / 10);
    if (frac % 10) out += char('0' + frac % 10);
  }
}

std::string coastJson(const std::vector<IRing>& rings, int w, int h) {
  std::string s = "{\n\"width\": " + std::to_string(w) + ",\n\"height\": " + std::to_string(h) + ",\n\"rings\": [\n";
  for (size_t r = 0; r < rings.size(); r++) {
    s += '[';
    for (size_t k = 0; k < rings[r].size(); k++) {
      if (k) s += ',';
      appendFixed(s, rings[r][k].x);
      s += ',';
      appendFixed(s, rings[r][k].y);
    }
    s += r + 1 < rings.size() ? "],\n" : "]\n";
  }
  s += "]\n}\n";
  return s;
}

void writeText(const std::string& path, const std::string& text) {
  std::string err;
  if (!fs::writeFileAtomic(path, std::string_view(text), &err)) fail("Не удалось записать " + path + ": " + err);
}

std::string hexColor(Color c) { return c.toHex(); }

// Операция с файлами с повтором: только что записанные файлы ненадолго блокирует антивирус или индексатор
// (на Windows — отказ в доступе). Всего до ~10 с.
template <class F>
bool retry(F&& op) {
  for (int attempt = 0;; attempt++) {
    if (op()) return true;
    if (attempt >= 30) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20 + 20 * attempt));
  }
}

// Заменить папку out готовой папкой tmp: переименование целиком, иначе перенос по файлам.
void replaceDir(const std::string& tmp, const std::string& out) {
  std::string err;
  if (!retry([&] { return fs::removeAll(out, &err); })) fail("Не удалось удалить прежнюю папку " + out + ": " + err);
  if (retry([&] { return fs::rename(tmp, out, &err); })) return;
  for (const std::string& f : fs::listTree(tmp))
    if (!retry([&] { return fs::rename(fs::join(tmp, f), fs::join(out, f), &err); }))
      fail("Не удалось перенести " + f + " в " + out + ": " + err);
  if (!retry([&] { return fs::removeAll(tmp, &err); })) fail("Не удалось удалить временную папку " + tmp + ": " + err);
}

}  // namespace

Report build(RgbaImage src, const std::string& outDir, const Options& opt, Artifacts* keep) {
  const double t0 = nowSeconds();
  if (opt.tile < 64 || opt.tile > 4096) fail("Размер тайла должен быть от 64 до 4096.");
  if (opt.levels < 1 || opt.levels > 8) fail("Число уровней должно быть от 1 до 8.");
  if (opt.maskScale < 1 || opt.thumbWidth < 16 || opt.previewLevel < 0 || opt.previewLevel >= opt.levels)
    fail("Неверные параметры маски, превью или миниатюры.");
  if (src.empty() || src.rgba.size() < size_t(src.w) * size_t(src.h) * 4) fail("Пустое изображение карты.");
  if (outDir.empty()) fail("Не задана папка результата.");
  Report rep;
  const int w = src.w, h = src.h;

  // 1. Слои.
  flattenOnWhite(src);
  Layers L0 = segment(src, opt, &rep.seg);
  rep.error = compareComposite(src, L0);
  rep.tSegment = nowSeconds() - t0;

  // 2. Берег.
  double t1 = nowSeconds();
  std::vector<IRing> raw = traceCoast(L0.field.data(), w, h, opt.minIsletArea, &rep.coast);
  std::vector<IRing> rings = simplifyCoast(raw, opt.simplifyTol, w, h, &rep.coast);
  raw.clear();
  rep.check = checkRings(rings, w, h);
  if (!rep.check.ok())
    fail(strf("Береговая линия некорректна: самопересечений %lld, пересечений колец %lld, вложений %lld, вырожденных %lld, вне карты %lld.",
              (long long)rep.check.selfHits, (long long)rep.check.ringHits, (long long)rep.check.nested,
              (long long)rep.check.degenerate, (long long)rep.check.outside));
  rep.tCoast = nowSeconds() - t1;
  t1 = nowSeconds();
  rep.geo = geoCheck(toCoast(rings, w, h));
  if (!rep.geo.ok())
    fail(rep.geo.built ? strf("Ядро геометрии отвергло береговую линию: %s", rep.geo.issues.empty() ? "площадь суши не сходится" : rep.geo.issues[0].c_str())
                       : "Ядро геометрии не построило береговую линию: " + rep.geo.error);
  rep.tGeo = nowSeconds() - t1;

  // 3. Пирамида.
  t1 = nowSeconds();
  const std::vector<Level> grid = levelGrid(w, h, opt.tile, opt.levels);
  std::vector<Layers> lower(size_t(opt.levels));
  jobs::parallelFor(size_t(opt.levels - 1), [&](size_t k) { lower[k + 1] = downsample(L0, 1 << (k + 1)); });
  auto levelLayers = [&](int z) -> const Layers& { return z == 0 ? L0 : lower[size_t(z)]; };
  rep.tPyramid = nowSeconds() - t1;

  // 4. Запись во временную папку рядом с результатом, затем замена.
  t1 = nowSeconds();
  std::string out = fs::absolute(outDir);
  while (out.size() > 1 && (out.back() == '/' || out.back() == '\\')) out.pop_back();
  const std::string tmp = fs::join(fs::parent(out), "." + fs::filename(out) + ".building");
  std::string err;
  if (!retry([&] { return fs::removeAll(tmp, &err); })) fail("Не удалось очистить " + tmp + ": " + err);
  for (const Level& l : grid)
    if (!fs::makeDirs(fs::join(tmp, "L" + std::to_string(l.z)), &err)) fail("Не удалось создать папку: " + err);

  struct Job { int z, layer, x, y; };
  std::vector<Job> jobsList;
  for (const Level& l : grid)
    for (int layer = 0; layer < 3; layer++)
      for (int y = 0; y < l.rows; y++)
        for (int x = 0; x < l.cols; x++) jobsList.push_back({l.z, layer, x, y});
  std::vector<u8> emptyFlags(jobsList.size(), 0);
  std::vector<u64> sizes(jobsList.size(), 0);
  jobs::parallelFor(jobsList.size(), [&](size_t k) {
    const Job& j = jobsList[k];
    const Level& l = grid[size_t(j.z)];
    const int x0 = j.x * opt.tile, y0 = j.y * opt.tile;
    const int tw = std::min(opt.tile, l.w - x0), th = std::min(opt.tile, l.h - y0);
    bool empty = false;
    const RgbaImage img = tileImage(levelLayers(j.z), j.layer, x0, y0, tw, th, &empty);
    if (empty) {
      emptyFlags[k] = 1;
      return;
    }
    codec::PngEncodeOptions po;
    po.level = clamp(opt.pngLevel, 0, 9);
    po.reduce = true;
    const std::vector<u8> data = codec::encodePng(img, po);
    const std::string path =
        fs::join(tmp, "L" + std::to_string(j.z) + "/" + kLayerNames[j.layer] + "_" + std::to_string(j.x) + "_" + std::to_string(j.y) + ".png");
    std::string e;
    if (data.empty() || !fs::writeFileAtomic(path, std::span<const u8>(data), &e)) fail("Не удалось записать " + path + ": " + e);
    sizes[k] = data.size();
  });
  json::Value empties = json::Value::object();
  for (int layer = 0; layer < 3; layer++) empties.set(kLayerNames[layer], json::Value::array());
  for (size_t k = 0; k < jobsList.size(); k++) {
    if (emptyFlags[k]) {
      rep.emptyTiles++;
      empties[kLayerNames[jobsList[k].layer]].push(json::Value::array({jobsList[k].z, jobsList[k].x, jobsList[k].y}));
    } else {
      rep.tiles++;
      rep.tileBytes += sizes[k];
    }
  }

  // Берег, маска, превью, миниатюра.
  writeText(fs::join(tmp, "coast.json"), coastJson(rings, w, h));
  const int mw = (w + opt.maskScale - 1) / opt.maskScale, mh = (h + opt.maskScale - 1) / opt.maskScale;
  {
    const std::vector<u8> mask = oceanMask(rings, mw, mh, opt.maskScale);
    RgbaImage mi;
    mi.w = mw;
    mi.h = mh;
    mi.rgba.resize(mask.size() * 4);
    for (size_t i = 0; i < mask.size(); i++) {
      mi.rgba[i * 4] = mi.rgba[i * 4 + 1] = mi.rgba[i * 4 + 2] = mask[i];
      mi.rgba[i * 4 + 3] = 255;
    }
    writePng(fs::join(tmp, "mask.png"), mi, opt.pngLevel);
  }
  const Layers& PL = levelLayers(opt.previewLevel);
  const RgbaImage preview = composite(PL, 0, 0, PL.w, PL.h);
  writePng(fs::join(tmp, "preview.png"), preview, opt.pngLevel);
  {
    // Ошибка превью относительно исходника, уменьшенного тем же усреднением по площади.
    const RgbaImage ref = resampleArea(src, PL.w, PL.h);
    double sum = 0;
    int mx = 0;
    i64 o1 = 0, o2 = 0;
    for (size_t i = 0; i < size_t(PL.w) * size_t(PL.h); i++) {
      int pm = 0;
      for (int c = 0; c < 3; c++) {
        const int d = std::abs(int(preview.rgba[i * 4 + size_t(c)]) - int(ref.rgba[i * 4 + size_t(c)]));
        sum += d;
        pm = std::max(pm, d);
      }
      mx = std::max(mx, pm);
      o1 += pm > 1;
      o2 += pm > 2;
    }
    rep.previewError.pixels = i64(PL.w) * PL.h;
    rep.previewError.mean = sum / (3.0 * double(rep.previewError.pixels));
    rep.previewError.max = mx;
    rep.previewError.over1 = o1;
    rep.previewError.over2 = o2;
  }
  const int thW = std::min(opt.thumbWidth, PL.w), thH = std::max(1, int(std::lround(double(thW) * h / w)));
  writePng(fs::join(tmp, "thumb.png"), resampleArea(preview, thW, thH), opt.pngLevel);

  // Манифест.
  json::Value m = json::Value::object();
  m.set("id", opt.id);
  m.set("version", 1);
  m.set("source", opt.sourceName);
  if (!opt.sourceSha256.empty()) m.set("sourceSha256", opt.sourceSha256);
  m.set("width", w);
  m.set("height", h);
  m.set("tile", opt.tile);
  m.set("tiles", "L{z}/{layer}_{x}_{y}.png");
  json::Value lv = json::Value::array();
  for (const Level& l : grid)
    lv.push(json::Value::object({{"z", l.z}, {"w", l.w}, {"h", l.h}, {"cols", l.cols}, {"rows", l.rows}, {"scale", 1 << l.z}}));
  m.set("levels", lv);
  m.set("layers", json::Value::array({kLayerNames[0], kLayerNames[1], kLayerNames[2]}));
  m.set("empty", empties);
  m.set("colors", json::Value::object({{"ocean", hexColor(kOcean)}, {"inland", hexColor(kOcean)}, {"land", hexColor(kLand)}}));
  m.set("coast", json::Value::object({{"file", "coast.json"},
                                      {"rings", rep.coast.rings},
                                      {"points", double(rep.coast.points)},
                                      {"tolerance", opt.simplifyTol},
                                      {"minIsletArea", opt.minIsletArea}}));
  m.set("mask", json::Value::object({{"file", "mask.png"}, {"w", mw}, {"h", mh}, {"scale", opt.maskScale}, {"ocean", 255}}));
  m.set("preview", json::Value::object({{"file", "preview.png"}, {"w", PL.w}, {"h", PL.h}}));
  m.set("thumb", json::Value::object({{"file", "thumb.png"}, {"w", thW}, {"h", thH}}));
  m.set("build", json::Value::object({{"erodeRadius", opt.erodeRadius},
                                      {"isletFillArea", opt.isletFillArea},
                                      {"meanError", std::round(rep.error.mean * 1e4) / 1e4},
                                      {"maxError", rep.error.max},
                                      {"mountains", rep.seg.mountains},
                                      {"pockets", rep.seg.pockets}}));
  json::WriteOptions wo;
  wo.indent = 2;
  writeText(fs::join(tmp, "manifest.json"), json::write(m, wo) + "\n");

  // Замена папки результата.
  replaceDir(tmp, out);
  for (const std::string& f : fs::listTree(out)) rep.bytes += fs::fileSize(fs::join(out, f)).value_or(0);
  rep.tWrite = nowSeconds() - t1;
  rep.seconds = nowSeconds() - t0;

  if (keep) {
    keep->flat = std::move(src);
    keep->layers = std::move(L0);
    keep->rings = std::move(rings);
  }
  return rep;
}

}  // namespace rg::map::bake
