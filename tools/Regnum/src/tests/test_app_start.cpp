// Сценарии экрана запуска: снимки в тёмной и светлой темах, недавние миры с миниатюрами, восстановление
// автосохранения, новый мир в папке.
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;

namespace {

// Два настоящих проекта в недавних (с миниатюрами) и один исчезнувший.
void seedRecent(Harness& h) {
  std::string a = fs::join(h.root, "Арденские марки");
  h->loadWorld(map::makeDemoWorld(*h->basemap()), "Демо");
  CHECK(h->saveTo(a));
  World w = h->store.world();
  {
    Tx tx(w);
    tx.meta().name = "Северный союз";
    w = std::move(tx).finish();
  }
  std::string b = fs::join(h.root, "Северный союз.regnum");
  h->loadWorld(w, "Демо 2");
  CHECK(h->saveTo(b));
  io::addRecent(fs::join(h.root, "Потерянный мир"), "Потерянный мир", h.dataDir);
  h->closeWorld();
  h.settle();
}

}  // namespace

TEST(app_start_screen_dark) {
  Harness h("start_dark");
  seedRecent(h);
  CHECK(h->ui.screen == app::Screen::Start);
  CHECK(h->uiRect("start.new") != nullptr);
  CHECK(h->uiRect("start.recent.0") != nullptr);
  CHECK(h->uiRect("start.recent.2") != nullptr);
  CHECK(fs::isFile(app::detail::thumbPath(h.dataDir, fs::join(h.root, "Арденские марки"))));
  h.dropToasts();
  h.settle();
  CHECK(h.shot("start_dark"));
  CHECK(platform::headless::title() == "Regnum");
}

TEST(app_start_screen_light) {
  Harness h("start_light", 1280, 800, 1, false);
  seedRecent(h);
  h.dropToasts();
  h.settle();
  CHECK(!ui::theme().dark);
  CHECK(h.shot("start_light"));
  // Тема переключается и с экрана запуска.
  CHECK(h.clickUi("start.theme"));
  h.settle();
  CHECK(h->ui.darkTheme);
}

TEST(app_start_open_recent) {
  Harness h("start_recent");
  seedRecent(h);
  // Новые первыми: «Потерянный мир», затем архив «Северный союз».
  CHECK(h.clickUi("start.recent.1"));
  h.settle();
  CHECK(h->ui.screen == app::Screen::Editor);
  CHECK(h->projectIsBundle());
  CHECK_EQ(h->worldTitle(), std::string("Северный союз"));
}

TEST(app_start_recovery) {
  Harness h("start_recovery");
  World w = map::makeDemoWorld(*h->basemap());
  {
    Tx tx(w);
    tx.meta().name = "Несохранённый мир";
    w = std::move(tx).finish();
  }
  io::writeAutosave(w, {}, h.dataDir);
  h->impl().recoveryDirty = true;
  h.settle();
  CHECK(h->uiRect("start.recovery.restore") != nullptr);
  CHECK(h.shot("start_recovery"));
  CHECK(h.clickUi("start.recovery.restore"));
  h.settle();
  CHECK(h->ui.screen == app::Screen::Editor);
  CHECK_EQ(h->worldTitle(), std::string("Несохранённый мир"));
  CHECK(h->dirty());
  CHECK(h->projectPath().empty());
}

TEST(app_new_world_in_folder) {
  Harness h("new_world");
  CHECK(h.clickUi("start.new"));
  h.settle();
  CHECK(h->hasDialog("world.new"));
  CHECK(h.shot("new_world_dialog"));
  // Название: поле в фокусе, текст выделен.
  h.retype("Земли Ардена");
  CHECK(h.clickUi("newworld.create"));
  h.settle();
  CHECK(h->ui.screen == app::Screen::Editor);
  std::string folder = fs::join(h.root, "Земли Ардена");
  CHECK_EQ(h->projectPath(), fs::absolute(folder));
  CHECK(fs::isFile(fs::join(folder, "world.json")));
  CHECK(fs::isFile(fs::join(folder, "data/geo.json")));
  CHECK(!h->dirty());
  CHECK(h->store.world().edges.size() > 10);   // береговая линия базовой карты
  CHECK(platform::headless::title() == "Земли Ардена — Regnum");
  h.waitMap();
  h.dropToasts();
  h.settle();
  CHECK(h.shot("new_world_editor"));
  // Повторное создание в ту же папку — новая папка «… 2».
  CHECK(h->newWorld("Земли Ардена", app::detail::newWorldFolder(h.root, "Земли Ардена")));
  CHECK(endsWith(h->projectPath(), "Земли Ардена 2"));
}
