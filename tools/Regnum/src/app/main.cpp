// Regnum — точка входа редактора.
//
//   regnum                      — экран запуска
//   regnum <папка мира|.regnum> — открыть мир
//   regnum --selftest           — настоящее окно, ~20 кадров с демонстрационным миром и действиями оболочки,
//                                 код возврата 0 (проверка сборки в CI). Данные — во временной папке.
#include <cstdio>

#include "app/app_internal.h"
#include "base/fs.h"
#include "gfx/text.h"
#include "map/demo_world.h"

using namespace rg;

namespace {

// Самопроверка: сценарий по кадрам поверх настоящего приложения.
struct SelfTest final : platform::App {
  app::App& a;
  int frame = 0;
  int failures = 0;
  std::string log;
  double start = 0;
  explicit SelfTest(app::App& app) : a(app) {}

  void check(bool ok, const char* what) {
    if (!ok) {
      failures++;
      logError("Самопроверка: %s", what);
      std::fprintf(stderr, "selftest: FAIL %s\n", what);
    }
  }

  void step() {
    using app::SelType;
    switch (frame) {
      case 0: {
        start = platform::time();
        const map::Basemap* bm = a.basemap();
        check(bm != nullptr, "базовая карта не найдена");
        if (bm) {
          try {
            a.loadWorld(map::makeDemoWorld(*bm), "Демонстрационный мир");
          } catch (const std::exception& e) {
            check(false, e.what());
            a.newWorld("Самопроверка");
          }
        }
        break;
      }
      case 3: {
        Id pid = 0;
        a.world().provinces.each([&](const Province& p) {
          if (!pid && !p.sea && p.owner) pid = p.id;
        });
        check(pid != 0, "в мире нет провинций");
        if (pid) a.select(SelType::Province, pid, true);
        break;
      }
      case 5: a.showPalette(); break;
      case 7: a.closeDialogs(); break;
      case 8: a.setMapMode(schema::MapMode::Guilds); break;
      case 9: a.setEditBorders(true); break;
      case 10: a.setEditBorders(false); break;
      case 11: a.setMapMode(schema::MapMode::Political); break;
      case 12: {
        int turn = a.store.world().turn();
        check(a.endTurnNow(), "ход не завершён");
        check(a.store.world().turn() == turn + 1, "номер хода не изменился");
        a.undo();
        check(a.store.world().turn() == turn, "отмена хода не сработала");
        break;
      }
      case 14: a.showSettings(); break;
      case 16: a.closeDialogs(); break;
      case 17: a.clearSelection(); break;
      case 20: {
        for (auto& t : a.toasts())
          if (t.kind == app::ToastKind::Danger) check(false, t.text.c_str());
        check(a.frameCount() >= 20, "кадры не рисуются");
        platform::quit(failures ? 1 : 0);
        break;
      }
      default: break;
    }
  }

  void onEvent(const platform::Event& e) override { a.onEvent(e); }
  void onFrame(platform::Frame& f) override {
    try {
      step();
    } catch (const std::exception& e) {
      check(false, e.what());
    }
    a.onFrame(f);
    frame++;
    if (platform::time() - start > 60 && frame > 1) {
      check(false, "самопроверка не уложилась в минуту");
      platform::quit(1);
    }
  }
  bool animating() override { return true; }
  bool onCloseRequest() override { return true; }
};

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; i++) args.push_back(argv[i]);
  bool selftest = false;
  std::string path;
  for (auto& s : args) {
    if (s == "--selftest") selftest = true;
    else if (!s.empty() && s[0] != '-' && path.empty()) path = s;
  }
  std::string dataDir = selftest ? fs::join(fs::tempDir(), "regnum-selftest") : fs::userDataDir();
  fs::makeDirs(dataDir);
  setLogFile(fs::join(dataDir, "regnum.log"));
  logInfo("Regnum: запуск%s", selftest ? " (самопроверка)" : "");

  std::string err;
  if (!gfx::initFonts(&err)) {
    logError("%s", err.c_str());
    platform::showFatal("Regnum", err.empty() ? std::string("Не найден системный шрифт с кириллицей.") : err);
    return 1;
  }
  for (const std::string& line : gfx::fontReport()) logInfo("%s", line.c_str());

  int code = 0;
  try {
    app::AppConfig cfg;
    cfg.dataDir = dataDir;
    cfg.savePrefs = !selftest;
    app::App a(cfg);
    if (!path.empty()) a.loadProject(fs::absolute(path));
    platform::WindowConfig wc;
    wc.title = "Regnum";
    wc.width = 1440;
    wc.height = 900;
    wc.minWidth = 1100;
    wc.minHeight = 700;
    wc.maximized = !selftest;
    wc.darkFrame = a.ui.darkTheme;
    wc.appName = "Regnum";
    wc.placementFile = selftest ? std::string() : fs::join(dataDir, "window.ini");
    try {
      if (selftest) {
        SelfTest st(a);
        code = platform::run(st, wc);
        std::printf("selftest: %s (%d кадров)\n", code == 0 ? "OK" : "FAILED", st.frame);
      } else {
        code = platform::run(a, wc);
      }
    } catch (const std::exception& e) {
      // Аварийное автосохранение: при следующем запуске экран запуска предложит восстановить работу.
      bool saved = false;
      if (a.dirty()) {
        try {
          io::writeAutosave(a.store.world(), a.projectPath(), dataDir);
          saved = true;
        } catch (...) {
        }
      }
      logError("Аварийное завершение: %s", e.what());
      platform::showFatal("Regnum", std::string("Редактор остановлен из-за внутренней ошибки:\n") + e.what() +
                                        (saved ? "\n\nНесохранённая работа записана — при следующем запуске её можно восстановить." : ""));
      code = 2;
    }
  } catch (const std::exception& e) {
    logError("Не удалось запустить редактор: %s", e.what());
    platform::showFatal("Regnum", std::string("Не удалось запустить редактор:\n") + e.what());
    code = 2;
  }
  logInfo("Regnum: завершение, код %d", code);
  return code;
}
