// Производительность кадра оболочки: интерфейс поверх закешированной карты, кадр с перерисовкой карты,
// экран запуска. Печатает средние времена; пороги — с большим запасом (защита от регрессий в разы).
#include <cstdio>

#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

double frameMs(Harness& h, int n, const std::function<void(int)>& before) {
  double total = 0;
  for (int i = 0; i < n; i++) {
    if (before) before(i);
    hl::pump();
    double t0 = nowSeconds();
    hl::renderFrame();
    total += nowSeconds() - t0;
    hl::advance(1.0 / 60);
  }
  return total * 1000 / n;
}

}  // namespace

TEST(app_perf_frames) {
  Harness h("perf", 1600, 1000);
  h.demo();
  h.waitMap();
  auto vp = h.visibleProvince();
  CHECK(vp.has_value());
  h->select(app::SelType::Province, vp->first);
  h.waitMap();
  h.dropToasts();
  h.settle();
  // Интерфейс: указатель ходит по инспектору (карта не меняется — берётся из кеша).
  const RectF* ins = h->uiRect("inspector");
  CHECK(ins != nullptr);
  RectF r = *ins;
  double ui = frameMs(h, 40, [&](int i) { hl::mouseMove(r.x + 40 + float(i % 7) * 30, r.y + 300 + float(i % 5) * 20); });
  // Карта: камера сдвигается каждый кадр (перерисовка карты).
  double mapMs = frameMs(h, 30, [&](int i) {
    h->map().panBy(i % 2 ? 7.f : -7.f, 3.f);
    h->map().waitIdle(5);
  });
  // Наведение на карту: подсветка провинции меняется — перерисовка карты.
  RectF area = h->mapArea();
  double hover = frameMs(h, 30, [&](int i) { hl::mouseMove(area.x + 60 + float(i) * 13, area.cy()); });
  // Модальное окно поверх карты (фон под ним кешируется).
  h->showSettings();
  h.settle();
  double modal = frameMs(h, 30, [&](int i) { hl::mouseMove(700 + float(i % 9) * 11, 420); });
  std::printf("  модальное окно: %.2f мс\n", modal);
  CHECK(modal < rg::test::perf(60));
  h->closeDialogs();
  h.settle();
  h->closeWorld();
  h.settle();
  if (h->hasDialog("choice")) {
    h.clickUi("dialog.button.0");
    h.settle();
  }
  double start = frameMs(h, 30, [&](int i) { hl::mouseMove(200 + float(i) * 9, 300); });
  std::printf("  кадр 1600×1000: интерфейс %.2f мс, панорама карты %.2f мс, наведение на карту %.2f мс, экран запуска %.2f мс\n", ui, mapMs, hover,
              start);
  CHECK(ui < rg::test::perf(40));
  CHECK(mapMs < rg::test::perf(200));
  CHECK(start < rg::test::perf(60));
}
