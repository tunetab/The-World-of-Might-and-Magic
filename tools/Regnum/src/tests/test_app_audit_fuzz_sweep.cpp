// Regnum — аудит устойчивости (ui-fuzz): систематический обход оболочки на демонстрационном мире.
// Каждая сущность × каждая вкладка инспектора, каждая выдвижная панель, каждый редактор со всеми аргументами,
// зарегистрированные окна; журнал WARN/ERROR снимается после каждого кадра с точным контекстом (где возник
// повторяющийся ID виджета и т. п.). Находки печатаются; REGNUM_AUDIT=1 — роняют тест.
#include <cstdio>
#include <map>
#include <set>

#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;
using app::SelType;

namespace {

bool auditStrict() {
  const char* v = std::getenv("REGNUM_AUDIT");
  return v && *v && *v != '0';
}

struct Tap {
  std::string path;
  size_t off = 0;
  explicit Tap(std::string p) : path(std::move(p)) {
    fs::remove(path);
    setLogFile(path);
  }
  ~Tap() { setLogFile(""); }
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

struct Sweep {
  Harness& h;
  Tap tap;
  std::map<std::string, std::pair<std::string, int>> found;   // сообщение -> (первый контекст, раз)
  Sweep(Harness& hh, const std::string& name) : h(hh), tap(fs::join(test::outDir(), "app-tmp/sweep-" + name + ".log")) {}
  void check(const std::string& where) {
    for (auto& m : tap.fresh()) {
      // ID в сообщении о повторе уникален для места: оставляем как ключ вместе с контекстом.
      auto& f = found[m];
      if (f.second++ == 0) f.first = where;
    }
  }
  void settle(const std::string& where) {
    h.settle();
    check(where);
  }
};

void sweepTabs(Harness& h, Sweep& s, SelType t, Id id, const std::string& what) {
  h->select(t, id);
  s.settle(what);
  std::set<std::string> seen;
  for (auto& td : app::tabs()) {
    if (td.type != t) continue;
    bool vis = !td.visible || td.visible(h.a(), id);
    if (!vis) continue;
    h->ui.tabOf[t] = td.id;
    s.settle(what + " tab=" + td.id);
    // Прокрутить вкладку вниз: нижние разделы тоже рисуются.
    if (const RectF* r = h->uiRect("inspector")) {
      for (int k = 0; k < 4; k++) {
        h.wheel(r->cx(), r->y + r->h * 0.6f, -6);
        s.check(what + " tab=" + td.id + " scroll" + std::to_string(k));
      }
    }
  }
}

}  // namespace

TEST(app_audit_fuzz_sweep) {
  for (int pass = 0; pass < 2; pass++) {
    Harness h(pass ? "fuzz_sweep_light" : "fuzz_sweep", pass ? 1100 : 1440, pass ? 700 : 900, 1, pass == 0);
    if (pass) h->setUiScale(1.25f);
    h.demo();
    h.waitMap();
    Sweep s(h, pass ? "light" : "dark");
    s.check("demo");
    const World& w = h->world();
    for (Id id : w.provinces.ids()) sweepTabs(h, s, SelType::Province, id, "province " + std::to_string(id));
    for (Id id : h->world().factions.ids()) sweepTabs(h, s, SelType::Faction, id, "faction " + std::to_string(id));
    for (Id id : h->world().armies.ids()) sweepTabs(h, s, SelType::Army, id, "army " + std::to_string(id));
    for (Id id : h->world().routes.ids()) sweepTabs(h, s, SelType::Route, id, "route " + std::to_string(id));
    for (Id id : h->world().characters.ids()) sweepTabs(h, s, SelType::Character, id, "character " + std::to_string(id));
    h->clearSelection();
    for (auto& d : app::drawers()) {
      h->openDrawer(d.id);
      s.settle(std::string("drawer ") + d.id);
      if (const RectF* r = h->uiRect("drawer"))
        for (int k = 0; k < 4; k++) {
          h.wheel(r->cx(), r->y + r->h * 0.6f, -6);
          s.check(std::string("drawer ") + d.id + " scroll");
        }
      h->openDrawer(d.id);
      s.settle("close drawer");
    }
    // Редакторы: все аргументы.
    std::vector<std::pair<std::string, Id>> eds;
    for (Id f : h->world().factions.ids()) {
      eds.push_back({"techtree", f});
      eds.push_back({"military", f});
      if (h->world().faction(f)->isState()) eds.push_back({"buildings", f});
    }
    eds.push_back({"buildings", 0});
    for (Id m : h->world().modifiers.ids()) eds.push_back({"modifiers", m});
    eds.push_back({"modifiers", 0});
    for (Id k = 0; k <= 7; k++) eds.push_back({"catalogs", k});
    eds.push_back({"trade", 0});
    for (auto& [e, arg] : eds) {
      h->openEditor(e, arg);
      s.settle("editor " + e + "(" + std::to_string(arg) + ")");
      if (const RectF* r = h->uiRect("editor"))
        for (int k = 0; k < 3; k++) {
          h.wheel(r->x + r->w * 0.3f, r->y + r->h * 0.6f, -6);
          s.check("editor " + e + "(" + std::to_string(arg) + ") scroll");
        }
      h->closeEditor();
      s.settle("close editor");
    }
    // Окна.
    Id army = h->world().armies.ids().empty() ? 0 : h->world().armies.ids()[0];
    Id fac = h->world().factions.ids()[0];
    Id land = 0;
    h->world().provinces.each([&](const Province& p) {
      if (!land && !p.sea && p.owner) land = p.id;
    });
    h->endTurnNow();
    s.settle("endTurnNow");
    std::vector<std::pair<std::string, Id>> dl = {{"turn.history", 0}, {"turn.report", 0}, {"flag", fac}, {"tribute", fac},
                                                  {"build.picker", land}, {"army.split", army}, {"chronicle.note", 0}};
    for (auto& [d, arg] : dl) {
      h->openDialog(d, arg);
      s.settle("dialog " + d);
      h->closeDialogs();
      s.settle("close dialog");
    }
    h->showSettings();
    s.settle("settings");
    for (int k = 0; k < 4; k++) {
      if (const RectF* r = h->uiRect("settings.tabs")) {
        h.click(r->x + r->w * (0.125f + 0.25f * float(k)), r->cy());
        s.check("settings tab " + std::to_string(k));
      }
    }
    h->closeDialogs();
    h->showHelp();
    s.settle("help");
    h->closeDialogs();
    h->showPalette();
    s.settle("palette");
    h.type("а");
    s.check("palette type");
    h->closeDialogs();
    s.settle("end");
    std::printf("  [ui-fuzz sweep %s] записей журнала WARN/ERROR: %d\n", pass ? "light 1100x700 ui1.25" : "dark 1440x900", int(s.found.size()));
    for (auto& [m, f] : s.found) std::printf("  [ui-fuzz sweep] x%d %s\n        при: %s\n", f.second, m.c_str(), f.first.c_str());
    std::fflush(stdout);
    // Регрессия: ни одного повтора ID виджета во всех вкладках, панелях, редакторах и окнах (остальные записи журнала —
    // по REGNUM_AUDIT=1).
    int dups = 0;
    for (auto& kv : s.found)
      if (kv.first.find("повторяющийся ID") != std::string::npos) dups++;
    CHECK_EQ(dups, 0);
    if (auditStrict()) CHECK(s.found.empty());
  }
}
