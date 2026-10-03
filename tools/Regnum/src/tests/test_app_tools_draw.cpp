// Сценарии инструментов контура и щелчка (ТЗ 1.a.i — добавление и удаление провинций): новая провинция
// многоугольником через берег (граница прилипает к берегу), от руки (лассо), расширение и вырезание, заливка
// острова, нож, объединение с соседней, удаление провинции; отмена каждого шага.
#include "tests/test_app_tools_util.h"

using namespace rg;
using namespace rg::toolstest;

namespace {

// Новый мир по береговой линии базовой карты, правка границ включена.
void freshWorld(Harness& h) {
  CHECK(h->newWorld("Проверка инструментов"));
  h.settle();
  h.dropToasts();
  h.key(Key::E);
  CHECK(h->ui.editBorders);
}

const geo::Face* largestLand(const geo::FaceSet& fs) {
  const geo::Face* best = nullptr;
  for (auto& f : fs.faces)
    if (f.terrain == Terrain::Land && !f.province && (!best || f.area > best->area)) best = &f;
  return best;
}

bool allLand(const World& w, Id prov) {
  auto fs = geo::buildFaces(w);
  const geo::ProvinceShape* s = fs->shape(prov);
  if (!s) return false;
  for (int f : s->faces)
    if (fs->faces[size_t(f)].terrain != Terrain::Land) return false;
  return true;
}

// Окружность земли вокруг c радиуса r целиком на суше.
bool landDisk(const geo::FaceSet& fs, Vec2 c, double r) {
  for (int k = 0; k < 32; k++)
    for (double q : {0.35, 0.7, 1.0, 1.35}) {
      double a = k * 3.14159265 / 16;
      if (fs.terrainAt(c + Vec2{std::cos(a), std::sin(a)} * (r * q)) != Terrain::Land) return false;
    }
  return true;
}

void clickAll(Harness& h, const std::vector<Vec2>& pts) {
  for (Vec2 p : pts) clickMap(h, p);
}

}  // namespace

TEST(app_tools_new_province_over_coast) {
  ToolGuard guard;
  Harness h("tools_new_coast");
  freshWorld(h);
  auto fs = geo::faces(h->world());
  const geo::Face* land = largestLand(*fs);
  CHECK(land != nullptr);
  // Береговая точка, вокруг которой квадрат захватывает и сушу, и море.
  const double S = 60;
  std::vector<Vec2> sq;
  const auto& ring = land->rings[0];
  for (size_t k = ring.size() / 3; k < ring.size() && sq.empty(); k += 7) {
    Vec2 c = ring[k];
    std::vector<Vec2> corners{{c.x - S, c.y - S}, {c.x + S, c.y - S}, {c.x + S, c.y + S}, {c.x - S, c.y + S}};
    int landN = 0, first = -1;
    for (int i = 0; i < 4; i++)
      if (fs->terrainAt(corners[size_t(i)]) == Terrain::Land) {
        landN++;
        if (first < 0) first = i;
      }
    if (landN < 1 || landN > 3) continue;
    std::rotate(corners.begin(), corners.begin() + first, corners.end());
    sq = corners;
  }
  CHECK(!sq.empty());
  Box2 box;
  for (Vec2 p : sq) box.add(p);
  focus(h, box.inflated(30));
  h.key(Key::P);
  CHECK(h->ui.tool == app::ToolId::NewProvince);
  CHECK(h->uiRect("tool.options") != nullptr);
  CHECK(h->uiRect("tool.options.terrain") != nullptr);
  size_t n0 = h->world().provinces.size();
  // Три вершины, Backspace убирает последнюю, затем четыре вершины и указатель у первой — замыкание.
  clickAll(h, {sq[0], sq[1], sq[2]});
  h.key(Key::Backspace);
  clickAll(h, {sq[2], sq[3]});
  moveMap(h, sq[0] + Vec2{1, 1});
  CHECK(cleanShot(h, "tools_new_province"));
  clickMap(h, sq[0] + Vec2{1, 1});   // щелчок у первой вершины замыкает контур
  CHECK_EQ(h->world().provinces.size(), n0 + 1);
  CHECK(h->ui.sel.type == app::SelType::Province);
  Id pid = h->ui.sel.id;
  const Province* p = h->world().province(pid);
  CHECK(p != nullptr);
  CHECK(!p->sea);
  CHECK_EQ(p->name, std::string("Новая провинция"));
  double area = provArea(h->world(), pid);
  CHECK(area > 1);
  CHECK(area < 4 * S * S - 1);           // морская часть контура отброшена: граница прилипла к берегу
  CHECK(allLand(h->world(), pid));
  CHECK(geo::validate(h->world()).empty());
  CHECK(h->uiRect("inspector") != nullptr);
  h.waitMap();
  CHECK(cleanShot(h, "tools_new_province_done"));
  // Отмена — провинции нет.
  h.key(Key::Z, ctrl());
  CHECK_EQ(h->world().provinces.size(), n0);
  CHECK(!h->ui.sel);
  // Ctrl+Z во время рисования убирает вершину (а не отменяет мир), Enter с двумя точками ничего не создаёт,
  // Esc отменяет начатый контур.
  u64 v = h->store.version();
  clickAll(h, {sq[0], sq[1], sq[2]});
  h.key(Key::Z, ctrl());
  CHECK_EQ(h->store.version(), v);
  h.key(Key::Enter);
  CHECK_EQ(h->store.version(), v);
  CHECK(!h->toasts().empty());
  h.key(Key::Escape);
  h.key(Key::Enter);
  CHECK_EQ(h->store.version(), v);
  CHECK(h->ui.tool == app::ToolId::NewProvince);
}

TEST(app_tools_lasso_add_remove_fill) {
  ToolGuard guard;
  Harness h("tools_areas");
  freshWorld(h);
  auto fs0 = geo::faces(h->world());
  const geo::Face* land = largestLand(*fs0);
  CHECK(land != nullptr);
  Vec2 L = land->label;
  const double R = 40;
  CHECK(landDisk(*fs0, L, R + 60));
  focus(h, Box2{L.x - 110, L.y - 80, L.x + 110, L.y + 80});
  h.key(Key::P);
  // Лассо: нажать и обвести круг — контур готов при отпускании.
  std::vector<gfx::Pt> circle;
  for (int k = 0; k <= 40; k++) {
    double a = k * 2 * 3.14159265 / 40;
    circle.push_back(scr(h, L + Vec2{std::cos(a), std::sin(a)} * R));
  }
  stroke(h, circle, false);
  CHECK(cleanShot(h, "tools_lasso"));
  release(h, circle.back());
  CHECK(h->ui.sel.type == app::SelType::Province);
  Id pid = h->ui.sel.id;
  double a0 = provArea(h->world(), pid);
  CHECK_NEAR(a0, 3.14159265 * R * R, 0.06 * 3.14159265 * R * R);

  // Расширение: прямоугольник, заходящий за правый край круга.
  h.key(Key::G);
  CHECK(h->ui.tool == app::ToolId::AddArea);
  std::vector<Vec2> addSq{{L.x + 20, L.y - 25}, {L.x + 80, L.y - 25}, {L.x + 80, L.y + 25}, {L.x + 20, L.y + 25}};
  clickAll(h, addSq);
  moveMap(h, L + Vec2{50, 45});
  CHECK(cleanShot(h, "tools_add_area"));
  h.key(Key::Enter);
  double a1 = provArea(h->world(), pid);
  CHECK(a1 > a0 + 1500);
  CHECK_EQ(h->store.undoLabel(), std::string("Расширить провинцию"));

  // Вырезание: квадрат внутри — дыра становится ничьей.
  h.key(Key::X);
  CHECK(h->ui.tool == app::ToolId::RemoveArea);
  clickAll(h, {{L.x - 15, L.y - 15}, {L.x + 15, L.y - 15}, {L.x + 15, L.y + 15}, {L.x - 15, L.y + 15}});
  CHECK(cleanShot(h, "tools_remove_area"));
  CHECK(h.clickUi("tool.options.done"));   // кнопка «Готово» на панели инструмента
  double a2 = provArea(h->world(), pid);
  CHECK_NEAR(a1 - a2, 900, 30);
  CHECK(geo::faces(h->world())->provinceAt(L) == 0);
  CHECK_EQ(h->store.undoLabel(), std::string("Вырезать часть провинции"));

  // Заливка: Shift+щелчок по дыре возвращает её выбранной провинции.
  h.key(Key::U);
  CHECK(h->ui.tool == app::ToolId::Fill);
  moveMap(h, L);
  CHECK(cleanShot(h, "tools_fill_hover"));
  clickMap(h, L, platform::MouseLeft, platform::ModShift);
  CHECK(geo::faces(h->world())->provinceAt(L) == pid);
  CHECK_NEAR(provArea(h->world(), pid), a1, 1e-6);
  auto fs1 = geo::faces(h->world());
  // Остров: небольшая ничья грань суши — щелчок без Shift создаёт из неё провинцию.
  const geo::Face* island = nullptr;
  for (auto& f : fs1->faces)
    if (f.terrain == Terrain::Land && !f.province && f.area > 800 && f.area < 40000 && (!island || f.area > island->area)) island = &f;
  CHECK(island != nullptr);
  Vec2 il = island->label;
  double ia = island->area;
  h->clearSelection();
  h.frames(2);
  focus(h, island->box.inflated(40));
  // Море рядом с островом.
  Vec2 sea = il;
  for (double t = 5; t < 400; t += 5)
    if (fs1->terrainAt(il + Vec2{t, 0}) == Terrain::Sea) {
      sea = il + Vec2{t + 8, 0};
      break;
    }
  CHECK(fs1->terrainAt(sea) == Terrain::Sea);
  size_t n0 = h->world().provinces.size();
  clickMap(h, il);
  CHECK_EQ(h->world().provinces.size(), n0 + 1);
  Id isl = h->ui.sel.id;
  CHECK(h->ui.sel.type == app::SelType::Province);
  CHECK_NEAR(provArea(h->world(), isl), ia, 1e-6);
  CHECK_EQ(provFaces(h->world(), isl), 1);
  CHECK(!h->world().province(isl)->sea);
  // Shift+щелчок по морю для сухопутной провинции — отказ с понятным сообщением.
  u64 v = h->store.version();
  clickMap(h, sea, platform::MouseLeft, platform::ModShift);
  CHECK_EQ(h->store.version(), v);
  CHECK(!h->toasts().empty());
  CHECK(h->toasts().back().kind == app::ToastKind::Warning);
  CHECK(geo::validate(h->world()).empty());
  // Отмена по шагам: остров, заливка дыры, вырезание, расширение, лассо.
  for (int i = 0; i < 5; i++) h.key(Key::Z, ctrl());
  CHECK(h->world().province(pid) == nullptr);
  CHECK(h->world().province(isl) == nullptr);
}

TEST(app_tools_knife_merge_delete) {
  ToolGuard guard;
  Harness h("tools_knife");
  h.demo();
  h.key(Key::E);
  const World& w = h->world();
  // Крупная сухопутная провинция с владельцем.
  Id pid = 0;
  double best = 0;
  w.provinces.each([&](const Province& p) {
    if (p.sea || !p.owner) return;
    double a = provArea(w, p.id);
    if (a > best) {
      best = a;
      pid = p.id;
    }
  });
  CHECK(pid != 0);
  Id owner = w.province(pid)->owner;
  auto fs = geo::faces(w);
  Vec2 c = fs->shape(pid)->label;
  // Линия ножа: от точки подписи влево и вправо до выхода из провинции и чуть дальше.
  auto exitAt = [&](Vec2 d) {
    Vec2 q = c;
    while (fs->provinceAt(q) == pid) q = q + d * 2.0;
    return q + d * 12.0;
  };
  Vec2 l = exitAt({-1, 0}), r = exitAt({1, 0});
  h->select(app::SelType::Province, pid);
  h.frames(2);
  Box2 fb;
  fb.add(l);
  fb.add(r);
  focus(h, fb.inflated(40));
  h.key(Key::K);
  CHECK(h->ui.tool == app::ToolId::Knife);
  size_t n0 = h->world().provinces.size();
  clickMap(h, l);
  moveMap(h, r);
  CHECK(cleanShot(h, "tools_knife"));
  gfx::Pt rs = scr(h, r);
  h.doubleClick(rs.x, rs.y);   // двойной щелчок — последняя точка и готово
  CHECK_EQ(h->world().provinces.size(), n0 + 1);
  Id part = h->ui.sel.id;
  CHECK(part != pid);
  CHECK_EQ(h->world().province(part)->owner, owner);   // новая часть наследует владельца
  CHECK_NEAR(provArea(h->world(), pid) + provArea(h->world(), part), best, 1e-3 * best);
  CHECK(geo::validate(h->world()).empty());

  // Объединение: выбрана исходная, щелчок по отрезанной части — подтверждение — одна провинция.
  h->select(app::SelType::Province, pid);
  h.frames(2);
  h.key(Key::J);
  CHECK(h->ui.tool == app::ToolId::Merge);
  Vec2 pl = geo::faces(h->world())->shape(part)->label;
  moveMap(h, pl);
  CHECK(cleanShot(h, "tools_merge"));
  clickMap(h, pl);
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK(h->world().province(part) == nullptr);
  CHECK_NEAR(provArea(h->world(), pid), best, 1e-3 * best);
  CHECK(h->ui.sel == (app::Selection{app::SelType::Province, pid}));
  h.key(Key::Z, ctrl());
  CHECK(h->world().province(part) != nullptr);

  // Удаление: щелчок — подтверждение — земли ничьи; отмена возвращает.
  h.key(Key::D);
  CHECK(h->ui.tool == app::ToolId::DeleteProvince);
  Vec2 cl = geo::faces(h->world())->shape(pid)->label;
  moveMap(h, cl);
  CHECK(cleanShot(h, "tools_delete"));
  clickMap(h, cl);
  h.settle();
  CHECK(h->hasDialog("confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK(h->world().province(pid) == nullptr);
  CHECK(geo::faces(h->world())->provinceAt(cl) == 0);
  CHECK(!h->ui.sel);
  h.key(Key::Z, ctrl());
  CHECK(h->world().province(pid) != nullptr);
  CHECK(geo::faces(h->world())->provinceAt(cl) == pid);
}
