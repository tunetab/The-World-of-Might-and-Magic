// Regnum — аудит устойчивости (ui-fuzz): точные воспроизведения находок обезьяньего теста.
// Ожидаемые отказы проверяются только при REGNUM_AUDIT=1; без него тесты печатают замеры и не падают.
#include <cstdio>

#include "core/io.h"
#include "rules/rules.h"
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;
using app::SelType;

namespace {

bool auditStrict() {
  const char* v = std::getenv("REGNUM_AUDIT");
  return v && *v && *v != '0';
}

// Кадры с замером: среднее и худшее время (мс).
std::pair<double, double> timeFrames(Harness& h, int n, const std::function<void(int)>& before) {
  double total = 0, worst = 0;
  for (int i = 0; i < n; i++) {
    if (before) before(i);
    hl::pump();
    double t0 = nowSeconds();
    hl::renderFrame();
    double ms = (nowSeconds() - t0) * 1000;
    total += ms;
    worst = std::max(worst, ms);
    hl::advance(1.0 / 60);
  }
  return {total / n, worst};
}

}  // namespace

// Окно выбора постройки поверх карты: время кадра при движении указателя.
TEST(app_audit_fuzz_probe_picker_perf) {
  Harness h("fuzz_probe_picker", 1920, 1080);
  h->setUiScale(1.5f);
  h.demo();
  h.waitMap();
  Id land = 0;
  h->world().provinces.each([&](const Province& p) {
    if (!land && !p.sea && p.owner) land = p.id;
  });
  CHECK(h->openDialog("build.picker", land));
  h.settle();
  auto [avg, worst] = timeFrames(h, 40, [&](int i) { hl::mouseMove(600 + float(i % 13) * 37, 300 + float(i % 7) * 50); });
  std::printf("  [ui-fuzz probe] build.picker 1920x1080 ui1.5: средний кадр %.1f мс, худший %.1f мс\n", avg, worst);
  h->closeDialogs();
  h.settle();
  // Тот же замер без окна.
  auto [avg2, worst2] = timeFrames(h, 40, [&](int i) { hl::mouseMove(600 + float(i % 13) * 37, 300 + float(i % 7) * 50); });
  std::printf("  [ui-fuzz probe] карта 1920x1080 ui1.5: средний кадр %.1f мс, худший %.1f мс\n", avg2, worst2);
  if (auditStrict()) CHECK(avg < 100);
}

// Завершение хода: длительность расчёта в кадре.
TEST(app_audit_fuzz_probe_endturn_perf) {
  Harness h("fuzz_probe_endturn");
  h.demo();
  h.waitMap();
  double worst = 0;
  for (int k = 0; k < 3; k++) {
    double t0 = nowSeconds();
    CHECK(h->endTurnNow());
    double ms = (nowSeconds() - t0) * 1000;
    worst = std::max(worst, ms);
    h.settle();
  }
  std::printf("  [ui-fuzz probe] endTurnNow: худшее %.1f мс\n", worst);
}

namespace {

// Предупреждения журнала, появившиеся за время жизни объекта.
struct WarnTap {
  std::string path;
  size_t off = 0;
  explicit WarnTap(const std::string& name) : path(fs::join(test::outDir(), "app-tmp/probe-" + name + ".log")) {
    fs::remove(path);
    setLogFile(path);
  }
  ~WarnTap() { setLogFile(""); }
  std::vector<std::string> fresh() {
    std::vector<std::string> out;
    auto t = fs::readFile(path);
    if (!t || t->size() <= off) return out;
    std::string s = t->substr(off);
    off = t->size();
    size_t p = 0;
    while (p < s.size()) {
      size_t q = s.find('\n', p);
      if (q == std::string::npos) q = s.size();
      std::string line = s.substr(p, q - p);
      if (line.find("[WARN]") != std::string::npos || line.find("[ERROR]") != std::string::npos) out.push_back(line.substr(line.find("] ") + 2));
      p = q + 1;
    }
    return out;
  }
};

}  // namespace

// Дерево построек: выбор каждой карточки (боковая панель уровня) — повторяющиеся ID виджетов.
TEST(app_audit_fuzz_probe_buildings_ids) {
  for (int pass = 0; pass < 2; pass++) {
    Harness h("fuzz_probe_bt", pass ? 900 : 1440, pass ? 600 : 900, 1, pass == 0);
    if (pass) h->setUiScale(1.5f);
    h.demo();
    WarnTap tap(pass ? "bt1" : "bt0");
    h->openEditor("buildings", 0);
    h.settle();
    int warns = 0;
    std::vector<Id> ids;
    h->world().buildings.each([&](const Building& b) {
      if (!b.owner) ids.push_back(b.id);
    });
    for (Id id : ids) {
      const RectF* r = h->uiRect("bt.node." + std::to_string(id));
      if (!r) continue;
      h.click(r->cx(), r->cy());
      for (auto& m : tap.fresh()) {
        warns++;
        std::printf("  [ui-fuzz probe] buildings(0) %s, карточка %u: %s\n", pass ? "900x600 ui1.5" : "1440x900", unsigned(id), m.c_str());
      }
      if (const RectF* side = h->uiRect("editor")) {
        for (int k = 0; k < 4; k++) {
          h.wheel(side->right() - 120, side->y + side->h * 0.6f, -6);
          for (auto& m : tap.fresh()) {
            warns++;
            std::printf("  [ui-fuzz probe] buildings(0) %s, карточка %u, прокрутка %d: %s\n", pass ? "900x600 ui1.5" : "1440x900", unsigned(id), k, m.c_str());
          }
        }
      }
    }
    std::printf("  [ui-fuzz probe] buildings(0) %s: предупреждений %d\n", pass ? "900x600 ui1.5" : "1440x900", warns);
    if (auditStrict()) CHECK_EQ(warns, 0);
  }
}
