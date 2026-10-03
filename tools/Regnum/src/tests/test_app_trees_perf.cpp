// Время кадра полноэкранных схем деревьев (технологии, постройки) и вкладки построек провинции.
// Печатает средние времена; пороги — с большим запасом (защита от регрессий в разы).
#include <cstdio>

#include "app/editors/buildings.h"
#include "app/editors/techtree.h"
#include "tests/test_app_trees_util.h"

using namespace rg;
using namespace rg::apptest;
using namespace rg::apptest::trees;

namespace {

double frameMs(int n, const std::function<void(int)>& before) {
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

TEST(app_trees_perf_frames) {
  HideTestRegs hide;
  Harness h("trees_perf", 1600, 1000);
  h.demo();
  Id fid = 0;
  h->world().techs.each([&](const Tech& t) {
    if (!fid) fid = t.faction;
  });
  // Сорок технологий со связями.
  CHECK(h->act("Дерево", [&](Tx& tx) {
    std::vector<Id> ids;
    for (int k = 0; k < 40; k++) {
      Id id = rules::createTech(tx, fid, "Технология " + std::to_string(k));
      tx.tech(id).desc = "+5 % к доходу в казну";
      if (k >= 3) rules::setPrereq(tx, id, ids[size_t(k / 2)], true);
      ids.push_back(id);
    }
    rules::autoLayout(tx, fid);
  }));
  app::openTechTree(h.a(), fid);
  h.settle();
  // В покое редактор не просит новых кадров.
  for (int i = 0; i < 30 && ui::needsRedraw(); i++) h.frame();
  CHECK(!ui::needsRedraw());
  // Указатель над карточкой: после появления карточки сведений — тоже покой.
  {
    std::vector<Id> ts;
    h->world().techs.each([&](const Tech& t) {
      if (t.faction == fid) ts.push_back(t.id);
    });
    const RectF* nr = h->uiRect("tt.node." + std::to_string(ts.front()));
    CHECK(nr != nullptr);
    h.move(nr->cx(), nr->cy());
    int n = 0;
    for (; n < 200 && ui::needsRedraw(); n++) h.frame();
    std::printf("  кадров до покоя над карточкой: %d\n", n);
    CHECK(n < rg::test::perf(60));
    h.dropToasts();
    h.settle();
    CHECK(h.shot("trees_perf_hover"));
  }
  {
    // Выбранная технология (панель свойств): тоже покой.
    std::vector<Id> ts;
    h->world().techs.each([&](const Tech& t) {
      if (t.faction == fid) ts.push_back(t.id);
    });
    app::openTechTree(h.a(), fid, ts.back());
    h.step();
    int n = 0;
    for (; n < 400 && ui::needsRedraw(); n++) h.frame();
    std::printf("  кадров до покоя с выбранной технологией: %d\n", n);
    CHECK(n < rg::test::perf(60));
  }
  const RectF* cv = h->uiRect("tt.canvas");
  CHECK(cv != nullptr);
  RectF c = *cv;
  double hover = frameMs(30, [&](int i) { hl::mouseMove(c.x + 80 + float(i) * 23, c.cy() + float(i % 5) * 9); });
  double zoom = frameMs(20, [&](int i) { hl::wheel(c.cx(), c.cy(), i % 2 ? 1.f : -1.f); });
  std::printf("  дерево технологий: наведение %.2f мс, масштаб %.2f мс\n", hover, zoom);
  CHECK(hover < rg::test::perf(120));
  CHECK(zoom < rg::test::perf(160));
  app::openBuildingTree(h.a(), 0);
  h.settle();
  for (int i = 0; i < 30 && ui::needsRedraw(); i++) h.frame();
  CHECK(!ui::needsRedraw());
  cv = h->uiRect("bt.canvas");
  CHECK(cv != nullptr);
  c = *cv;
  double bh = frameMs(30, [&](int i) { hl::mouseMove(c.x + 200 + float(i) * 23, c.cy() + float(i % 5) * 9); });
  double bz = frameMs(20, [&](int i) { hl::wheel(c.cx(), c.cy(), i % 2 ? 1.f : -1.f); });
  std::printf("  дерево построек: наведение %.2f мс, масштаб %.2f мс\n", bh, bz);
  CHECK(bh < rg::test::perf(120));
  CHECK(bz < rg::test::perf(160));
}
