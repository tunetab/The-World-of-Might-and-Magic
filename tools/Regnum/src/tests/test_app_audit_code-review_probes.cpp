// Аудит (code-review): воспроизведения найденных дефектов. Все дефекты исправлены — проверки стали
// обычными регрессионными (CHECK).
#include "app/dialogs/turn_ui.h"
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

Id anyLandProvince(const World& w) {
  Id id = 0;
  w.provinces.each([&](const Province& p) {
    if (!id && !p.sea && p.owner) id = p.id;
  });
  return id;
}

}  // namespace

// Просмотр прошлого хода — только чтение, но кнопка-«галочка» правки границ на панели инструментов включает
// режим правки (команда E при этом недоступна): появляются инструменты правки, инструмент границ активен.
TEST(app_audit_codereview_readonly_borders_toggle) {
  Harness h("audit_cr_ro_borders");
  h.demo();
  int t0 = h->store.world().turn();
  CHECK(h->endTurnNow());
  h.settle();
  CHECK(h->viewTurn(t0));
  h.settle();
  CHECK(h->readOnly());
  CHECK(!h->ui.editBorders);
  CHECK(h->uiRect("tool.borders") != nullptr);
  CHECK(h.clickUi("tool.borders"));
  h.settle();
  CHECK_MSG(!h->ui.editBorders, "в режиме только просмотра кнопка правки границ включила правку");
  CHECK_MSG(h->ui.tool != app::ToolId::EditBorders, "в режиме только просмотра активен инструмент правки границ");
  h.dropToasts();
  h.settle();
  CHECK(h.shot("audit_cr_ro_borders"));
}

// «Начать ход заново» после возврата и повторного прохождения хода: снимок текущего хода в истории — из
// брошенной ветки («до возврата»), а диалог обещает «мир станет таким, каким был в начале хода».
TEST(app_audit_codereview_restart_turn_loads_abandoned_branch) {
  Harness h("audit_cr_restart");
  h.demo();
  int t0 = h->store.world().turn();
  Id pid = anyLandProvince(h->store.world());
  CHECK(pid != 0);
  std::string oldName = h->store.world().province(pid)->name;
  CHECK(h->endTurnNow());   // t0 -> t0+1
  CHECK(h->endTurnNow());   // t0+1 -> t0+2
  CHECK_EQ(h->store.world().turn(), t0 + 2);
  // Возврат к ходу t0+1 и новая ветка: переименование и завершение хода.
  CHECK(app::turnui::rollbackToTurn(h.a(), t0 + 1));
  h.settle();
  CHECK_EQ(h->store.world().turn(), t0 + 1);
  CHECK(h->act("Переименовать", [&](Tx& tx) { tx.province(pid).name = "Новая ветвь"; }));
  CHECK(h->endTurnNow());   // новая ветка: t0+1 -> t0+2
  h.settle();
  CHECK_EQ(h->store.world().turn(), t0 + 2);
  CHECK_EQ(h->store.world().province(pid)->name, std::string("Новая ветвь"));
  // В истории есть снимок текущего хода t0+2 — с кнопкой «Начать этот ход заново».
  bool hasCurrent = false;
  for (auto& s : h->snapshots())
    if (s.turn == t0 + 2) hasCurrent = true;
  CHECK(hasCurrent);
  h->openDialog("turn.history");
  h.settle();
  CHECK(h->uiRect("history.rollback." + std::to_string(t0 + 2)) != nullptr);
  CHECK(h.clickUi("history.rollback." + std::to_string(t0 + 2)));
  h.settle();
  CHECK(h->hasDialog("confirm"));
  h.dropToasts();
  h.settle();
  CHECK(h.shot("audit_cr_restart_confirm"));
  CHECK(h.clickUi("dialog.ok"));
  h.settle();
  CHECK_EQ(h->store.world().turn(), t0 + 2);
  const Province* p = h->store.world().province(pid);
  CHECK(p != nullptr);
  std::printf("  restart: name after «начать ход заново» = «%s» (было «%s»)\n", p->name.c_str(), oldName.c_str());
  // Исправлено (снимок начала хода в своей ветви): регрессионная проверка.
  CHECK_MSG(p->name == "Новая ветвь", "«Начать ход заново» загрузил мир брошенной ветки, а не начало текущего хода");
}

namespace {
// Отпечаток области последнего кадра (логические пиксели).
u64 regionHash(Harness& h, RectF r) {
  const platform::Frame& f = hl::lastFrame();
  u64 x = 1469598103934665603ull;
  for (int y = int(r.y * f.scale); y < int(r.bottom() * f.scale) && y < f.h; y++)
    for (int xx = int(r.x * f.scale); xx < int(r.right() * f.scale) && xx < f.w; xx++) {
      x ^= f.row(y)[xx];
      x *= 1099511628211ull;
    }
  (void)h;
  return x;
}
}  // namespace

// Палитра команд (Ctrl+K): каждый кадр прокручивает список к подсвеченной строке (VirtualList::scrollToRow →
// scrollToItem с requestRedraw) — колесо мыши не листает результаты, а окно перерисовывается без остановки.
TEST(app_audit_codereview_palette_scroll_and_idle) {
  Harness h("audit_cr_palette");
  h.demo();
  h.dropToasts();
  h.settle();
  h.waitMap();
  h.settle();
  bool idleBefore = !h->animating();
  std::printf("  palette: animating before open = %d\n", int(!idleBefore));
  h.key(Key::K, ctrl());
  h.settle();
  CHECK(h->hasDialog("palette"));
  h.type("а");   // много провинций, персонажей и государств
  h.settle();
  const RectF* sr = h->uiRect("palette.search");
  CHECK(sr != nullptr);
  RectF list{sr->x, sr->bottom() + 8, sr->w, 360};
  h.move(list.cx(), list.y + 120);
  h.frames(10);
  u64 before = regionHash(h, list);
  CHECK(h.shot("audit_cr_palette_before"));
  (void)before;
  // Подсвеченная строка (золотая полоска слева) видна до прокрутки.
  auto accentRows = [&] {
    int n = 0;
    for (float y = list.y; y < list.bottom(); y += 1) {
      u32 p = h.pixel(list.x + 1, y);
      int r = int((p >> 16) & 255), g = int((p >> 8) & 255), b = int(p & 255);
      if (r > 190 && g > 130 && g < 190 && b < 110) n++;
    }
    return n;
  };
  int accBefore = accentRows();
  // Колесо вниз на 12 щелчков (≈ 12 строк) без движения мыши: подсвеченная строка должна уйти из вида.
  hl::wheel(list.cx(), list.y + 120, -12);
  h.step();
  h.frames(40);
  int accAfter = accentRows();
  CHECK(h.shot("audit_cr_palette_after_wheel"));
  std::printf("  palette: accent bar pixels before=%d after wheel=%d\n", accBefore, accAfter);
  CHECK(accBefore > 0);
  CHECK_MSG(!(accBefore > 0 && accAfter > 0), "колесо мыши не уводит список палитры от подсвеченной строки (scrollToRow каждый кадр)");
  // Простой: окно с палитрой не должно перерисовываться без событий. Мигание каретки в поле поиска длится 10 с
  // после ввода — пропускаем его, затем дожидаемся конца анимаций.
  hl::advance(11);
  h.settle();
  int busy = 0;
  for (int i = 0; i < 30; i++) {
    h.frame();
    if (h->animating()) busy++;
  }
  std::printf("  palette: busy frames while idle = %d/30\n", busy);
  CHECK_MSG(!idleBefore || busy == 0, "открытая палитра команд перерисовывает окно каждый кадр без событий (ui::requestRedraw в scrollToItem)");
}

// Встроенный проводник (на Linux — единственный способ открыть/сохранить мир): после щелчка по записи список
// каждый кадр прокручивается к ней (browser.cpp: if (sel >= 0) vl.scrollToRow(sel)) — колесо мыши не листает,
// окно перерисовывается без остановки.
TEST(app_audit_codereview_browser_scroll_locked_on_selection) {
  Harness h("audit_cr_browser");
  std::string dir = tempDir("audit_cr_browser_dirs");
  for (int i = 0; i < 40; i++) fs::makeDirs(fs::join(dir, strf("Папка %02d", i)));
  h->openDialog(app::detail::browserDialog(app::detail::BrowseMode::PickFolder, "Папка", dir, [](app::App&, const std::string&) {}));
  h.settle();
  CHECK(h->hasDialog("browser"));
  // Список — правая часть окна проводника (слева «Места»), строки по 32 точки.
  const RectF* lr = h->uiRect("browser.list");
  CHECK(lr != nullptr);
  std::printf("  browser.list rect: %.0f %.0f %.0f %.0f\n", lr->x, lr->y, lr->w, lr->h);
  const RectF* ok = h->uiRect("browser.ok");
  CHECK(ok != nullptr);
  RectF list{ok->right() - 540, ok->y - 400, 540, 352};
  // Щелчок по первой видимой папке — выбрана (золотая полоска слева).
  h.click(list.x + 80, list.y + 30);
  h.frames(20);
  auto accent = [&] {
    int n = 0;
    for (float y = list.y; y < list.bottom(); y += 1)
      for (float x = list.x; x < list.x + 10; x += 1) {
        u32 p = h.pixel(x, y);
        int r = int((p >> 16) & 255), g = int((p >> 8) & 255), b = int(p & 255);
        if (r > 190 && g > 130 && g < 190 && b < 110) n++;
      }
    return n;
  };
  int accBefore = accent();
  CHECK(h.shot("audit_cr_browser_before"));
  h.move(list.cx(), list.cy());
  hl::wheel(list.cx(), list.cy(), -15);
  h.step();
  h.frames(40);
  int accAfter = accent();
  CHECK(h.shot("audit_cr_browser_after_wheel"));
  std::printf("  browser: selected-row accent pixels before=%d after wheel=%d\n", accBefore, accAfter);
  CHECK(accBefore > 0);
  CHECK_MSG(!(accBefore > 0 && accAfter > 0), "встроенный проводник: колесо мыши не уводит список от выбранной записи (scrollToRow каждый кадр)");
  h.settle();   // полоса прокрутки гаснет после колеса — дождаться
  int busy = 0;
  for (int i = 0; i < 30; i++) {
    h.frame();
    if (h->animating()) busy++;
  }
  std::printf("  browser: busy frames while idle = %d/30\n", busy);
  CHECK_MSG(busy == 0, "встроенный проводник с выбранной записью перерисовывает окно каждый кадр без событий");
}
