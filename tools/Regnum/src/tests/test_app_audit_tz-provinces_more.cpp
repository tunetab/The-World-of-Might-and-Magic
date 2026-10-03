// Аудит ТЗ 1.a (провинции и карта), автор tz-provinces, часть 2: дополнительные воспроизведения.
// Проверки ожидаемого поведения, которые сейчас не выполняются, включаются переменной REGNUM_AUDIT=1
// (по умолчанию только печатаются, чтобы не ронять общий прогон).
#include "tests/test_app_tools_util.h"

using namespace rg;
using namespace rg::toolstest;

namespace {

[[maybe_unused]] bool auditStrict2() {
  const char* v = std::getenv("REGNUM_AUDIT");
  return v && *v && *v != '0';
}

#define AUDIT_EXPECT2(cond, msg)                                                                   \
  do {                                                                                             \
    bool _ok = (cond);                                                                             \
    std::printf("  [audit %s] %s: %s\n", _ok ? "ok" : "DEFECT", #cond, std::string(msg).c_str()); \
    if (!_ok && auditStrict2()) ::rg::test::fail(__FILE__, __LINE__, std::string("AUDIT: ") + (msg)); \
  } while (0)

// Небольшая сухопутная провинция с владельцем, рамка которой (с запасом) целиком на суше.
Id smallInlandOwned(const World& w, Box2& box, double inflate = 25) {
  auto fs = geo::faces(w);
  Id victim = 0;
  double best = 1e18;
  w.provinces.each([&](const Province& p) {
    if (p.sea || !p.owner) return;
    const geo::ProvinceShape* s = fs->shape(p.id);
    if (!s) return;
    Box2 b = s->box.inflated(inflate);
    Vec2 cs[] = {{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}};
    for (Vec2 c : cs)
      if (fs->terrainAt(c) != Terrain::Land) return;
    if (s->area < best) {
      best = s->area;
      victim = p.id;
      box = b;
    }
  });
  return victim;
}

}  // namespace

// ---------------------------------------------------------------- ТЗ 1.a.i: «Вырезать» всю провинцию — запись остаётся без области
TEST(audit_tzprov2_cut_whole_province_ghost) {
  ToolGuard guard;
  Harness h("audit_tzprov2_cut", 1440, 900);
  h.demo();
  Box2 vb;
  Id victim = smallInlandOwned(h->world(), vb);
  CHECK(victim != 0);
  Id owner = h->world().province(victim)->owner;
  focus(h, vb.inflated(40));
  h.key(Key::E);
  CHECK(h->ui.editBorders);
  h->select(app::SelType::Province, victim);
  h.settle();
  h.key(Key::X);
  CHECK(h->ui.tool == app::ToolId::RemoveArea);
  clickMap(h, {vb.x0, vb.y0});
  clickMap(h, {vb.x1, vb.y0});
  clickMap(h, {vb.x1, vb.y1});
  clickMap(h, {vb.x0, vb.y1});
  h.key(Key::Enter);
  h.settle();
  const World& w1 = h->world();
  auto fs1 = geo::faces(w1);
  bool rec = w1.province(victim) != nullptr;
  bool shape = fs1->shape(victim) != nullptr;
  auto calc = rules::calc(w1);
  bool counted = false;
  if (const rules::FactionCalc* fc = calc->faction(owner))
    counted = std::find(fc->provinces.begin(), fc->provinces.end(), victim) != fc->provinces.end();
  std::printf("  after cutting whole province: record %d shape %d counted for owner %d\n", int(rec), int(shape), int(counted));
  CHECK(cleanShot(h, "audit_tzprov2_cut_whole"));
  CHECK_MSG(!(rec && !shape), "провинция, целиком вырезанная инструментом «Вырезать», осталась в базе без области на карте");
}

// ---------------------------------------------------------------- клавиши в текстовом поле не запускают команды карты
TEST(audit_tzprov2_textfield_keys) {
  ToolGuard guard;
  Harness h("audit_tzprov2_keys", 1440, 1180);
  h.demo();
  auto vp = h.visibleProvince();
  CHECK(vp.has_value());
  h->ui.tabOf[app::SelType::Province] = "province.overview";
  h.click(vp->second.x, vp->second.y);
  h.settle();
  CHECK(h->ui.sel.id == vp->first);
  const RectF* r = h->uiRect("province.capital");
  CHECK(r != nullptr);
  if (!r) return;
  h.click(r->cx(), r->cy());
  h.settle();
  CHECK(ui::wantsKeyboard());
  int mode0 = int(h->ui.mapMode);
  size_t n0 = h->world().provinces.size();
  h.key(Key::Delete);
  h.key(Key::E);
  h.key(Key::P);
  h.key(Key::M);
  h.key(Key::D2);
  h.settle();
  std::printf("  dialog %d editBorders %d provinces %zu -> %zu\n", int(h->hasDialog()), int(h->ui.editBorders), n0, size_t(h->world().provinces.size()));
  // Регрессия: клавиши без Ctrl в текстовом поле не запускают команды карты, инструменты и панели.
  CHECK(!h->hasDialog());
  CHECK(!h->ui.editBorders);
  CHECK(h->ui.tool == app::ToolId::Select);
  CHECK_EQ(h->world().provinces.size(), n0);
  CHECK(h->ui.drawer.empty());
  CHECK_EQ(int(h->ui.mapMode), mode0);
}

// ---------------------------------------------------------------- ТЗ 1.a.i: самопересекающийся контур новой провинции
TEST(audit_tzprov2_bowtie_new_province) {
  ToolGuard guard;
  Harness h("audit_tzprov2_bowtie", 1440, 900);
  h.demo();
  Box2 vb;
  Id victim = smallInlandOwned(h->world(), vb, 0);
  CHECK(victim != 0);
  focus(h, vb.inflated(60));
  h.key(Key::E);
  h.key(Key::P);
  CHECK(h->ui.tool == app::ToolId::NewProvince);
  size_t n0 = h->world().provinces.size();
  Vec2 c = vb.center();
  double r = std::min(vb.w(), vb.h()) * 0.3;
  clickMap(h, {c.x - r, c.y - r});
  clickMap(h, {c.x + r, c.y + r});
  clickMap(h, {c.x + r, c.y - r});
  clickMap(h, {c.x - r, c.y + r});
  h.key(Key::Enter);
  h.settle();
  size_t n1 = h->world().provinces.size();
  auto fs = geo::faces(h->world());
  int shapeless = 0;
  h->world().provinces.each([&](const Province& p) {
    if (!fs->shape(p.id)) shapeless++;
  });
  std::printf("  bowtie: provinces %zu -> %zu, shapeless %d, tool %d\n", n0, n1, shapeless, int(h->ui.tool));
  CHECK(cleanShot(h, "audit_tzprov2_bowtie"));
  CHECK(shapeless == 0);
}

// ---------------------------------------------------------------- ТЗ 1.a.iv: режим правки на небольшом окне — снимок ручек выбранной провинции
TEST(audit_tzprov2_edit_selected_small_window) {
  ToolGuard guard;
  Harness h("audit_tzprov2_small", 1280, 720);
  h.demo();
  h.key(Key::E);
  auto vp = h.visibleProvince();
  CHECK(vp.has_value());
  h.click(vp->second.x, vp->second.y);
  h.settle();
  CHECK(h->ui.sel.id == vp->first);
  CHECK(h->ui.tool == app::ToolId::EditBorders);
  CHECK(cleanShot(h, "audit_tzprov2_edit_small"));
  const RectF* tb = h->uiRect("toolbar");
  const RectF* lg = h->uiRect("legend");
  if (tb && lg) std::printf("  toolbar %.0f,%.0f-%.0f,%.0f legend %.0f,%.0f-%.0f,%.0f\n", tb->x, tb->y, tb->right(), tb->bottom(), lg->x, lg->y, lg->right(), lg->bottom());
}
