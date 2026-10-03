// Regnum — команда regnum-cli build-basemap: исходная карта -> тайлы слоёв, береговая линия, маска, превью, манифест.
#include <cstdio>

#include "base/fs.h"
#include "cli/basemap_check.h"
#include "cli/cli.h"
#include "codec/png.h"
#include "map/basemap_build.h"

namespace rg::cli {

namespace {

double numOption(const std::vector<std::string>& args, const std::string& key, double def, double lo, double hi) {
  const std::string s = option(args, key);
  if (s.empty()) return def;
  auto v = parseNum(s);
  if (!v || *v < lo || *v > hi) fail(strf("Параметр %s: ожидается число от %g до %g.", key.c_str(), lo, hi));
  return *v;
}

std::string mb(u64 bytes) { return strf("%.2f МБ", double(bytes) / (1024.0 * 1024.0)); }

int runBuildBasemap(const std::vector<std::string>& args) {
  const std::vector<std::string> pos = positional(args);
  if (pos.size() != 2) {
    std::fprintf(stderr, "Использование: regnum-cli build-basemap <исходник.png> <папка> [параметры]\n");
    return 2;
  }
  map::bake::Options opt;
  opt.tile = int(numOption(args, "--tile", 512, 64, 4096));
  opt.levels = int(numOption(args, "--levels", 4, 1, 8));
  opt.erodeRadius = int(numOption(args, "--radius", 6, 1, 120));
  opt.isletFillArea = int(numOption(args, "--islet-fill", 160, 0, 1e7));
  opt.simplifyTol = numOption(args, "--tol", 0.75, 0, 20);
  opt.minIsletArea = numOption(args, "--min-islet", 12, 0, 1e9);
  opt.pngLevel = int(numOption(args, "--level", 9, 0, 9));
  opt.id = option(args, "--id", opt.id);
  std::vector<basemap::Region> regions;
  for (const std::string& a : args) {
    if (a.rfind("--region=", 0) != 0) continue;
    const auto parts = split(a.substr(9), ',');
    std::vector<int> v;
    for (const auto& p : parts)
      if (auto n = parseNum(p)) v.push_back(int(*n));
    if (v.size() != 4 || v[2] <= 0 || v[3] <= 0) fail("Параметр --region: ожидается x,y,ширина,высота.");
    regions.push_back({strf("user%zu", regions.size()), v[0], v[1], v[2], v[3]});
  }
  const std::string checkDir = option(args, "--check");
  const bool verify = hasFlag(args, "--verify");

  const double t0 = nowSeconds();
  std::string err;
  auto bytes = fs::readFile(pos[0], &err);
  if (!bytes) fail(err);
  opt.sourceName = fs::filename(pos[0]);
  opt.sourceSha256 = map::bake::sha256Hex(bytes->data(), bytes->size());
  auto img = codec::decodePng(*bytes, &err);
  if (!img) fail("Не удалось прочитать PNG «" + pos[0] + "»: " + err);
  bytes.reset();
  const double tDecode = nowSeconds() - t0;
  std::printf("Исходник: %s, %d × %d, sha256 %s (чтение %.0f мс)\n", opt.sourceName.c_str(), img->w, img->h, opt.sourceSha256.c_str(), tDecode * 1000);

  map::bake::Artifacts keep;
  const bool needKeep = verify || !checkDir.empty();
  const map::bake::Report r = map::bake::build(std::move(*img), pos[1], opt, needKeep ? &keep : nullptr);

  std::printf("Слои: %.2f с — море %lld пикс., внутренние воды %lld, краска %lld (непрозрачно %lld), областей краски %d, гор %d, снежных шапок %d\n",
              r.tSegment, (long long)r.seg.oceanPx, (long long)r.seg.inlandPx, (long long)r.seg.inkPx, (long long)r.seg.opaquePx, r.seg.components,
              r.seg.mountains, r.seg.snowCaps);
  std::printf("      островков, не мешающих поиску моря: %d (%lld пикс.); карманов моря во внутренние воды: %d (%lld пикс.); "
              "мягкий край берега %lld пикс.; под непрозрачными знаками продолжено %lld пикс.\n",
              r.seg.isletsFilled, (long long)r.seg.isletFillPx, r.seg.pockets, (long long)r.seg.pocketPx, (long long)r.seg.haloPx,
              (long long)r.seg.inpaintedPx);
  std::printf("Композиция против исходника: средняя ошибка %.4f, max %d, пикселей с ошибкой > 1: %lld, > 2: %lld из %lld\n", r.error.mean, r.error.max,
              (long long)r.error.over1, (long long)r.error.over2, (long long)r.error.pixels);
  std::printf("Превью против уменьшенного исходника: средняя ошибка %.4f, max %d\n", r.previewError.mean, r.previewError.max);
  std::printf("Берег: %.2f с — контуров %d (%lld точек), отброшено островков %d, дыр %d; после упрощения колец %d, точек %lld, итераций топологии %d, возвращено точек %lld\n",
              r.tCoast, r.coast.rawRings, (long long)r.coast.rawPoints, r.coast.islets, r.coast.holes, r.coast.rings, (long long)r.coast.points,
              r.coast.rounds, (long long)r.coast.refined);
  std::printf("Проверка колец: самопересечений %lld, пересечений %lld, вложений %lld, вырожденных %lld, вне карты %lld, наименьший зазор %.3f пикс.\n",
              (long long)r.check.selfHits, (long long)r.check.ringHits, (long long)r.check.nested, (long long)r.check.degenerate,
              (long long)r.check.outside, r.check.minGap);
  std::printf("Ядро геометрии: %.2f с — узлов %d, дуг %d (береговых %d), граней суши %d, моря %d; площадь суши %.1f (по кольцам %.1f); замечаний geo::validate: %zu\n",
              r.tGeo, r.geo.nodes, r.geo.edges, r.geo.coastEdges, r.geo.landFaces, r.geo.seaFaces, r.geo.landArea, r.geo.ringArea, r.geo.issues.size());
  std::printf("Тайлы: %d записано (%s), %d пустых пропущено; всего %s; пирамида %.2f с, запись %.2f с\n", r.tiles, mb(r.tileBytes).c_str(), r.emptyTiles,
              mb(r.bytes).c_str(), r.tPyramid, r.tWrite);
  std::printf("Готово за %.2f с: %s\n", nowSeconds() - t0, fs::absolute(pos[1]).c_str());

  bool ok = true;
  if (verify) ok = basemap::verifyOutput(pos[1], keep, r);
  if (!checkDir.empty()) {
    const double t1 = nowSeconds();
    const auto regs = basemap::pickRegions(keep, regions);
    basemap::writeChecks(checkDir, keep, regs);
    for (size_t i = 0; i < regs.size(); i++)
      std::printf("  область r%zu %s: %d,%d %d × %d\n", i, regs[i].name.c_str(), regs[i].x, regs[i].y, regs[i].w, regs[i].h);
    std::printf("Проверочные изображения: %s (%.1f с)\n", fs::absolute(checkDir).c_str(), nowSeconds() - t1);
  }
  return ok ? 0 : 1;
}

const Command reg("build-basemap",
                  "<исходник.png> <папка> [--tile=512] [--levels=4] [--radius=6] [--islet-fill=160] [--tol=0.75] [--min-islet=12] [--level=9] [--verify] "
                  "[--check=<папка> [--region=x,y,w,h ...]]",
                  "Собрать базовую карту: тайлы слоёв (море, реки и озёра, знаки), берег, маску моря, превью и манифест", &runBuildBasemap);

}  // namespace

}  // namespace rg::cli
