// Regnum — аудит устойчивости (ui-fuzz): «обезьяний» тест настоящего приложения на демонстрационном мире.
//
// Длинные случайные, но правдоподобные потоки событий: выдвижные панели, выделение провинций, фракций, войск,
// маршрутов и персонажей, все вкладки инспектора, все редакторы (модификаторы, справочники, деревья технологий
// и построек, торговля, войска), щелчки по именованным элементам оболочки и в случайные точки, перетаскивание
// по карте, ввод кириллицы, латиницы и чисел в поля, сочетания клавиш, режим правки границ и все инструменты,
// завершение хода, отмена и повтор, сохранение во временную папку и чтение, тема, масштаб, размер окна.
//
// После каждого действия проверяются: исключения, записи журнала WARN/ERROR (в том числе повторяющиеся ID
// виджетов), уведомления-ошибки, инварианты мира (≤ 5 штабов, влияние ≤ 100 %, резерв ≥ 0, запасы ≥ 0),
// io::normalize на копии мира, geo::validate после правки геометрии, «только чтение» прошлого хода,
// отмена (возврат мира к состоянию до действия), время кадра.
//
// Обычный прогон короткий и ничего не роняет: находки только печатаются.
//   REGNUM_FUZZ_EVENTS=N   — событий на зерно (по умолчанию 120);
//   REGNUM_FUZZ_SEEDS=a,b  — зёрна;
//   REGNUM_FUZZ_VERBOSE=1  — печатать каждое действие;
//   REGNUM_AUDIT=1         — находки роняют тест.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>

#include "core/io.h"
#include "geo/topo.h"
#include "rules/rules.h"
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;
using app::SelType;
using platform::ModShift;

namespace {

bool auditStrict() {
  const char* v = std::getenv("REGNUM_AUDIT");
  return v && *v && *v != '0';
}

int envInt(const char* name, int def) {
  const char* v = std::getenv(name);
  if (!v || !*v) return def;
  return std::atoi(v);
}

std::vector<u64> envSeeds(std::vector<u64> def) {
  const char* v = std::getenv("REGNUM_FUZZ_SEEDS");
  if (!v || !*v) return def;
  std::vector<u64> r;
  std::string s = v;
  size_t p = 0;
  while (p < s.size()) {
    size_t q = s.find(',', p);
    if (q == std::string::npos) q = s.size();
    if (q > p) r.push_back(u64(std::strtoull(s.substr(p, q - p).c_str(), nullptr, 10)));
    p = q + 1;
  }
  return r.empty() ? def : r;
}

// Журнал приложения дублируется в файл; после каждого действия читаются новые строки.
struct LogTap {
  std::string path;
  size_t off = 0;
  explicit LogTap(std::string p) : path(std::move(p)) {
    fs::remove(path);
    setLogFile(path);
  }
  ~LogTap() { setLogFile(""); }
  std::vector<std::string> fresh() {
    std::vector<std::string> out;
    auto t = fs::readFile(path);
    if (!t || t->size() <= off) return out;
    std::string s = t->substr(off);
    size_t last = s.rfind('\n');
    if (last == std::string::npos) return out;
    off += last + 1;
    size_t p = 0;
    while (p <= last) {
      size_t q = s.find('\n', p);
      if (q == std::string::npos) break;
      out.push_back(s.substr(p, q - p));
      p = q + 1;
    }
    return out;
  }
};

std::string randomText(Rng& r) {
  static const char* pool[] = {"Арден", "Новый Эльдор", "ёжик в тумане", "Lorem ipsum", "Ironforge", "42", "-17", "0", "3,14", "2.5",
                               "1e9", "999999999999", "-0", "  ", "100", "-100", "50%", "abc", "<b>&amp;</b>", "\"кавычки\" «ёлочки»",
                               "Ǆ̃ɮ‏שלום", "日本語", "\xF0\x9F\x90\x89 дракон", "\t", "Щ", "7", "12", "-5", "0,5", "1000000"};
  const int n = int(std::size(pool));
  int k = r.range(0, n + 2);
  if (k < n) return pool[k];
  if (k == n) {
    std::string s;
    for (int i = 0; i < 30; i++) s += "Длинное ";
    return s;
  }
  if (k == n + 1) return std::to_string(r.range(-100000, 100000));
  static const char* letters[] = {"а", "б", "в", "г", "д", "е", "ё", "ж", "з", "и", "й", "к", "л", "м", "н", "о", "п", "р", "с", "т", "у",
                                  "ф", "х", "ц", "ч", "ш", "щ", "ъ", "ы", "ь", "э", "ю", "я", "Я", "Ж", "x", "Q", "z", "1", "9", " ", "-", "."};
  std::string s;
  int len = r.range(1, 12);
  for (int i = 0; i < len; i++) s += letters[r.range(0, int(std::size(letters)) - 1)];
  return s;
}

struct KeyDef {
  Key k;
  u32 mods;
  const char* name;
};

std::vector<KeyDef> keyPool() {
  u32 c = ctrl();
  std::vector<KeyDef> v = {
      {Key::Delete, 0, "Del"},       {Key::Escape, 0, "Esc"},        {Key::Enter, 0, "Enter"},     {Key::Tab, 0, "Tab"},
      {Key::Tab, ModShift, "S+Tab"}, {Key::Backspace, 0, "Bksp"},    {Key::Left, 0, "Left"},       {Key::Right, 0, "Right"},
      {Key::Up, 0, "Up"},            {Key::Down, 0, "Down"},         {Key::Home, 0, "Home"},       {Key::End, 0, "End"},
      {Key::PageUp, 0, "PgUp"},      {Key::PageDown, 0, "PgDn"},     {Key::E, 0, "E"},             {Key::V, 0, "V"},
      {Key::H, 0, "H"},              {Key::B, 0, "B"},               {Key::P, 0, "P"},             {Key::G, 0, "G"},
      {Key::X, 0, "X"},              {Key::K, 0, "K"},               {Key::U, 0, "U"},             {Key::J, 0, "J"},
      {Key::D, 0, "D"},              {Key::A, 0, "A"},               {Key::A, ModShift, "S+A"},    {Key::R, 0, "R"},
      {Key::F, 0, "F"},              {Key::M, 0, "M"},               {Key::L, 0, "L"},             {Key::F1, 0, "F1"},
      {Key::F2, 0, "F2"},            {Key::F11, 0, "F11"},           {Key::Minus, 0, "-"},         {Key::Equal, 0, "="},
      {Key::Equal, ModShift, "+"},   {Key::Space, 0, "Space"},       {Key::Z, c, "C+Z"},           {Key::Y, c, "C+Y"},
      {Key::Z, c | ModShift, "C+S+Z"}, {Key::A, c, "C+A"},           {Key::C, c, "C+C"},           {Key::V, c, "C+V"},
      {Key::X, c, "C+X"},            {Key::K, c, "C+K"},             {Key::Enter, c, "C+Enter"},   {Key::S, c, "C+S"},
      {Key::S, c | ModShift, "C+S+S"}, {Key::N, c, "C+N"},           {Key::O, c, "C+O"},           {Key::W, c, "C+W"},
      {Key::Comma, c, "C+,"},        {Key::Q, c, "C+Q"},
  };
  const Key digits[] = {Key::D1, Key::D2, Key::D3, Key::D4, Key::D5, Key::D6, Key::D7, Key::D8, Key::D9};
  static const char* dn[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
  static const char* cdn[] = {"C+1", "C+2", "C+3", "C+4", "C+5", "C+6", "C+7", "C+8", "C+9"};
  for (int i = 0; i < 9; i++) {
    v.push_back({digits[i], 0, dn[i]});
    v.push_back({digits[i], c, cdn[i]});
  }
  return v;
}

const char* selName(SelType t) {
  switch (t) {
    case SelType::Province: return "province";
    case SelType::Faction: return "faction";
    case SelType::Army: return "army";
    case SelType::Route: return "route";
    case SelType::Character: return "character";
    default: return "none";
  }
}

std::string fmt(const char* f, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

struct Finding {
  std::string kind, key, detail, context;
  std::vector<std::string> trail;
  int count = 0;
  int step = 0;
  u64 seed = 0;
};

class Monkey {
 public:
  Monkey(Harness& h, u64 seed, std::string tag)
      : h_(h), rng_(seed), seed_(seed), tag_(std::move(tag)), log_(fs::join(test::outDir(), "app-tmp/fuzz-" + tag_ + ".log")) {
    verbose_ = envInt("REGNUM_FUZZ_VERBOSE", 0) != 0;
    keys_ = keyPool();
    sub_ = h_->store.subscribe([this](const Change& c) {
      changes_.push_back({c.kind, c.label});
      tables_ |= c.tables;
    });
  }
  ~Monkey() { h_->store.unsubscribe(sub_); }

  std::map<std::string, Finding> findings;
  double worstFrame = 0;
  std::string worstFrameCtx;
  int frames = 0;
  int actions = 0;

  void run(int events) {
    baseline();
    for (int i = 0; i < events; i++) {
      actions = i + 1;
      action(i);
    }
    finalChecks();
  }

  // Именованное действие для воспроизведения (тесты-репродукции).
  void doAction(int i) { action(i); }
  Harness& harness() { return h_; }
  void frame() {
    double t0 = nowSeconds();
    hl::renderFrame();
    double ms = (nowSeconds() - t0) * 1000;
    hl::advance(1.0 / 60);
    frames++;
    curFrameMax_ = std::max(curFrameMax_, ms);
  }
  void step(int max = 200) {
    frame();
    for (int i = 0; i < max && ui::pendingEvents() > 0; i++) frame();
    for (int i = 0; i < 3; i++) frame();
  }

 private:
  Harness& h_;
  Rng rng_;
  u64 seed_;
  std::string tag_;
  LogTap log_;
  bool verbose_ = false;
  std::vector<KeyDef> keys_;
  int sub_ = 0;
  std::vector<std::pair<Change::Kind, std::string>> changes_;
  u32 tables_ = 0;
  std::deque<std::string> trail_;
  std::string cur_;
  int W_ = 1440, H_ = 900;
  float dpi_ = 1;
  double curFrameMax_ = 0;
  std::set<std::string> lastNorm_, lastGeo_;
  int modalStreak_ = 0, startStreak_ = 0;
  int saves_ = 0;
  bool dirtyBefore_ = false, kbBefore_ = false;
  double slowMs_ = envInt("REGNUM_FUZZ_SLOW_MS", 250);
  std::string focus_ = std::getenv("REGNUM_FUZZ_FOCUS") ? std::getenv("REGNUM_FUZZ_FOCUS") : "";

  float rx(float a, float b) { return a + float(rng_.uniform()) * (b - a); }
  bool chance(double p) { return rng_.uniform() < p; }
  template <class T>
  const T& pick(const std::vector<T>& v) {
    return v[size_t(rng_.range(0, int(v.size()) - 1))];
  }

  std::string context() {
    auto& u = h_->ui;
    std::string s = fmt("%s %dx%d@%.2g ui%.2g %s", u.screen == app::Screen::Editor ? "editor" : "start", W_, H_, double(dpi_), double(u.uiScale),
                        u.darkTheme ? "dark" : "light");
    if (!u.drawer.empty()) s += " drawer=" + u.drawer;
    if (!u.editor.empty()) s += " editor=" + u.editor + "(" + std::to_string(u.editorArg) + ")";
    if (u.sel) {
      s += std::string(" sel=") + selName(u.sel.type) + ":" + std::to_string(u.sel.id);
      auto it = u.tabOf.find(u.sel.type);
      if (it != u.tabOf.end()) s += " tab=" + it->second;
    }
    s += " tool=" + std::to_string(int(u.tool));
    if (u.editBorders) s += " edit";
    if (u.viewTurn) s += " view=" + std::to_string(*u.viewTurn);
    for (auto& d : h_->dialogStack()) s += std::string(" dlg=") + d->id();
    return s;
  }

  void note(const std::string& kind, const std::string& key, const std::string& detail) {
    Finding& f = findings[kind + "|" + key];
    if (f.count++ == 0) {
      f.kind = kind;
      f.key = key;
      f.detail = detail;
      f.context = context();
      f.trail.assign(trail_.begin(), trail_.end());
      f.step = actions;
      f.seed = seed_;
    }
  }

  void log(std::string s) {
    cur_ = fmt("#%d ", actions) + s;
    trail_.push_back(cur_);
    if (trail_.size() > 30) trail_.pop_front();
    if (verbose_) std::printf("    %s | %s\n", cur_.c_str(), context().c_str());
  }

  // ---------------------------------------------------------------- ввод
  void click(float x, float y, int button = platform::MouseLeft, u32 mods = 0) {
    hl::click(x, y, button, mods);
    step();
    hl::advance(0.6);
  }
  void dbl(float x, float y) {
    hl::doubleClick(x, y);
    step();
    hl::advance(0.6);
  }
  void drag(float x0, float y0, float x1, float y1, int button = platform::MouseLeft, u32 mods = 0, int steps = 8) {
    hl::mouseMove(x0, y0, mods);
    step();
    hl::mouseDown(x0, y0, button, mods);
    step();
    for (int i = 1; i <= steps; i++) {
      hl::mouseMove(x0 + (x1 - x0) * float(i) / float(steps), y0 + (y1 - y0) * float(i) / float(steps), mods);
      step();
    }
    hl::mouseUp(x1, y1, button, mods);
    step();
    hl::advance(0.6);
  }
  void press(Key k, u32 mods = 0) {
    hl::press(k, mods);
    step();
  }
  void type(const std::string& s) {
    hl::type(s);
    step();
  }

  std::vector<std::pair<std::string, RectF>> named() {
    std::vector<std::pair<std::string, RectF>> v;
    for (auto& [k, r] : h_->impl().rects)
      if (!r.empty() && r.right() > 1 && r.bottom() > 1 && r.x < float(W_) - 1 && r.y < float(H_) - 1) v.push_back({k, r});
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first < b.first; });
    return v;
  }

  RectF mapArea() { return h_->mapArea(); }

  // ---------------------------------------------------------------- случайные сущности
  Id randomId(SelType t) {
    const World& w = h_->world();
    std::vector<Id> ids;
    switch (t) {
      case SelType::Province: ids = w.provinces.ids(); break;
      case SelType::Faction: ids = w.factions.ids(); break;
      case SelType::Army: ids = w.armies.ids(); break;
      case SelType::Route: ids = w.routes.ids(); break;
      case SelType::Character: ids = w.characters.ids(); break;
      default: break;
    }
    if (ids.empty()) return 0;
    return pick(ids);
  }
  Id randomFaction(bool statesOnly = false) {
    std::vector<Id> ids;
    h_->world().factions.each([&](const Faction& f) {
      if (!statesOnly || f.isState()) ids.push_back(f.id);
    });
    return ids.empty() ? 0 : pick(ids);
  }
  Id randomLand() {
    std::vector<Id> ids;
    h_->world().provinces.each([&](const Province& p) {
      if (!p.sea) ids.push_back(p.id);
    });
    return ids.empty() ? 0 : pick(ids);
  }

  // Точка карты в видимой части (с учётом панелей).
  std::optional<gfx::Pt> visible(Vec2 m) {
    gfx::Pt s = h_->map().view().toScreen(m);
    RectF a = mapArea().inset(8);
    if (!a.contains(s.x, s.y)) return std::nullopt;
    return s;
  }
  gfx::Pt randomMapPoint() {
    RectF a = mapArea().inset(4);
    if (a.empty()) a = RectF{0, 0, float(W_), float(H_)};
    // Половина — точки подписи провинций (чаще попадаем в сушу), половина — любые.
    if (chance(0.5)) {
      auto fs = geo::faces(h_->world());
      for (int t = 0; t < 8; t++) {
        Id pid = randomId(SelType::Province);
        if (const geo::ProvinceShape* sh = fs->shape(pid))
          if (auto s = visible(sh->label)) return *s;
      }
    }
    return gfx::Pt{rx(a.x, a.right()), rx(a.y, a.bottom())};
  }

  // ---------------------------------------------------------------- действия
  void action(int i) {
    changes_.clear();
    tables_ = 0;
    curFrameMax_ = 0;
    h_->store.endCoalesce();
    bool roBefore = h_->readOnly();
    dirtyBefore_ = h_->dirty();
    kbBefore_ = ui::wantsKeyboard();
    World before = h_->store.world();
    u64 v0 = h_->store.version();
    h_->impl().builtinBrowser = false;   // проводник не выходит за временную папку теста
    std::string what;
    try {
      what = act();
    } catch (const test::Failure&) {
      throw;
    } catch (const std::exception& e) {
      note("exception", e.what(), std::string("исключение дошло до теста: ") + e.what());
    } catch (...) {
      note("exception", "unknown", "неизвестное исключение");
    }
    try {
      after(roBefore, before, v0);
    } catch (const test::Failure&) {
      throw;
    } catch (const std::exception& e) {
      note("exception-check", e.what(), std::string("исключение при проверке: ") + e.what());
    }
  }

  std::string act() {
    auto& u = h_->ui;
    // Не застревать на экране запуска и в модальных окнах.
    if (u.screen != app::Screen::Editor || h_->world().provinces.size() < 5) {
      if (++startStreak_ > 3) {
        log("demo: загрузить демонстрационный мир");
        h_.demo();
        startStreak_ = 0;
        return "demo";
      }
    } else {
      startStreak_ = 0;
    }
    if (h_->hasDialog()) {
      const char* top = h_->dialogStack().back()->id();
      if (std::strcmp(top, "browser") == 0 || ++modalStreak_ > 10) {
        log(std::string("esc: закрыть окно ") + top);
        press(Key::Escape);
        modalStreak_ = 0;
        return "esc";
      }
    } else {
      modalStreak_ = 0;
    }
    if (!focus_.empty()) return actFocused();
    struct W {
      double w;
      int id;
    };
    static const W table[] = {{30, 0}, {10, 1}, {9, 2}, {4, 3}, {4, 4}, {3, 5}, {3, 6}, {3, 7}, {3, 8}, {8, 9}, {12, 10}, {3, 11},
                              {6, 12}, {4, 13}, {2, 14}, {2, 15}, {1, 16}, {3, 17}, {0.6, 18}, {0.6, 19}, {0.6, 20}, {0.8, 21}, {0.8, 22},
                              {3, 23}, {4, 24}, {0.4, 25}, {0.3, 26}, {1.5, 27}, {6, 28}, {1.5, 29}};
    double tot = 0;
    for (auto& t : table) tot += t.w;
    double r = rng_.uniform() * tot;
    int id = 0;
    for (auto& t : table) {
      if (r < t.w) {
        id = t.id;
        break;
      }
      r -= t.w;
    }
    switch (id) {
      case 0: return actNamed();
      case 1: return actRandomClick();
      case 2: return actMapClick();
      case 3: return actMapDrag();
      case 4: return actToolSession();
      case 5: return actArmyDrag();
      case 6: return actDouble();
      case 7: return actRight();
      case 8: return actWheel();
      case 9: return actType();
      case 10: return actKey();
      case 11: return actDrawer();
      case 12: return actSelect();
      case 13: return actEditor();
      case 14: return actDialog();
      case 15: return actEditMode();
      case 16: return actEndTurn();
      case 17: return actUndoRedo();
      case 18: return actSaveLoad();
      case 19: return actTheme();
      case 20: return actScale();
      case 21: return actResize();
      case 22: return actViewTurn();
      case 23: return actHover();
      case 24: return actTab();
      case 25: return actDrop();
      case 26: return actFocus();
      case 27: return actHandleDrag();
      case 28: return actDragNamed();
      case 29: return actStale();
    }
    return "?";
  }

  bool fieldLike(const std::string& n) {
    for (const char* k : {"name", "search", "field", "count", "notes", "title", "amount", "turns", "value", "pct", "note", "upkeep", "total",
                          "tax", "desc", "filename", "level", "cost", "from", "to"})
      if (n.find(k) != std::string::npos) return true;
    return false;
  }

  std::string actNamed() {
    auto v = named();
    if (v.empty()) return actRandomClick();
    auto& [n, r] = pick(v);
    float x = chance(0.6) ? r.cx() : rx(r.x + 1, r.right() - 1), y = chance(0.6) ? r.cy() : rx(r.y + 1, r.bottom() - 1);
    log(fmt("click '%s' (%.0f,%.0f)", n.c_str(), double(x), double(y)));
    click(x, y);
    if (fieldLike(n) && chance(0.6)) {
      std::string t = randomText(rng_);
      if (chance(0.5)) {
        log("ctrl+a");
        press(Key::A, ctrl());
      }
      log("type «" + t.substr(0, 40) + "»");
      type(t);
      if (chance(0.5)) {
        Key k = chance(0.6) ? Key::Enter : (chance(0.5) ? Key::Tab : Key::Escape);
        log(k == Key::Enter ? "enter" : k == Key::Tab ? "tab" : "esc");
        press(k);
      }
    }
    return "named";
  }
  std::string actRandomClick() {
    // Чаще — внутри панелей (выдвижная, инспектор, редактор), иначе — где угодно.
    std::vector<RectF> areas;
    for (const char* n : {"drawer", "inspector", "editor", "topbar", "toolbar", "rail", "status", "legend", "minimap", "zoom", "banner"})
      if (const RectF* r = h_->uiRect(n)) areas.push_back(*r);
    RectF a = !areas.empty() && chance(0.7) ? pick(areas) : RectF{0, 0, float(W_), float(H_)};
    float x = rx(a.x, a.right()), y = rx(a.y, a.bottom());
    log(fmt("click random (%.0f,%.0f)", double(x), double(y)));
    click(x, y);
    return "click";
  }
  std::string actMapClick() {
    if (h_->ui.screen != app::Screen::Editor) return actRandomClick();
    gfx::Pt p = randomMapPoint();
    u32 mods = chance(0.15) ? ModShift : 0;
    log(fmt("map click (%.0f,%.0f)%s", double(p.x), double(p.y), mods ? " shift" : ""));
    click(p.x, p.y, platform::MouseLeft, mods);
    return "mapclick";
  }
  std::string actMapDrag() {
    gfx::Pt p = randomMapPoint();
    float dx = rx(-250, 250), dy = rx(-200, 200);
    int b = chance(0.7) ? platform::MouseLeft : (chance(0.5) ? platform::MouseMiddle : platform::MouseRight);
    log(fmt("map drag b%d (%.0f,%.0f)->(%.0f,%.0f)", b, double(p.x), double(p.y), double(p.x + dx), double(p.y + dy)));
    drag(p.x, p.y, p.x + dx, p.y + dy, b);
    return "mapdrag";
  }
  std::string actToolSession() {
    if (h_->ui.screen != app::Screen::Editor) return actRandomClick();
    static const app::ToolId tools[] = {app::ToolId::NewProvince, app::ToolId::AddArea, app::ToolId::RemoveArea, app::ToolId::Fill,
                                        app::ToolId::Knife,       app::ToolId::Merge,   app::ToolId::DeleteProvince, app::ToolId::NewArmy,
                                        app::ToolId::NewFleet,    app::ToolId::Route,   app::ToolId::EditBorders, app::ToolId::Select};
    app::ToolId t = tools[rng_.range(0, int(std::size(tools)) - 1)];
    const app::ToolDef* d = app::findTool(t);
    if (d && d->editMode && !h_->ui.editBorders && !h_->readOnly()) {
      log("setEditBorders(true)");
      h_->setEditBorders(true);
      step();
    }
    if ((t == app::ToolId::AddArea || t == app::ToolId::RemoveArea || t == app::ToolId::EditBorders) && h_->ui.sel.type != SelType::Province) {
      Id pid = randomLand();
      log(fmt("select province %u", unsigned(pid)));
      h_->select(SelType::Province, pid);
      step();
    }
    log(fmt("setTool %d", int(t)));
    h_->setTool(t);
    step();
    gfx::Pt c = randomMapPoint();
    int n = rng_.range(1, 6);
    for (int k = 0; k < n; k++) {
      float x = c.x + rx(-120, 120), y = c.y + rx(-90, 90);
      RectF a = mapArea();
      x = clamp(x, a.x + 2, a.right() - 2);
      y = clamp(y, a.y + 2, a.bottom() - 2);
      log(fmt("tool click (%.0f,%.0f)", double(x), double(y)));
      click(x, y);
    }
    double f = rng_.uniform();
    if (f < 0.45) {
      log("enter");
      press(Key::Enter);
    } else if (f < 0.65) {
      log(fmt("tool dbl (%.0f,%.0f)", double(c.x), double(c.y)));
      dbl(c.x, c.y);
    } else if (f < 0.8) {
      log("esc");
      press(Key::Escape);
    } else if (f < 0.9) {
      log("backspace");
      press(Key::Backspace);
    }
    return "tool";
  }
  std::string actArmyDrag() {
    if (h_->ui.screen != app::Screen::Editor) return actRandomClick();
    const World& w = h_->world();
    std::vector<std::pair<Id, gfx::Pt>> vis;
    w.armies.each([&](const Army& a) {
      if (auto s = visible(a.pos)) vis.push_back({a.id, *s});
    });
    if (vis.empty()) {
      // Показать случайное войско и попробовать в следующий раз.
      Id a = randomId(SelType::Army);
      log(fmt("select+focus army %u", unsigned(a)));
      if (a) h_->select(SelType::Army, a, true);
      step();
      return "army-focus";
    }
    auto [aid, p] = pick(vis);
    gfx::Pt q{p.x + rx(-160, 160), p.y + rx(-120, 120)};
    if (vis.size() > 1 && chance(0.5)) q = pick(vis).second;   // на другое войско — встреча
    if (h_->ui.tool != app::ToolId::Select && chance(0.7)) {
      log("setTool select");
      h_->setTool(app::ToolId::Select);
      step();
    }
    log(fmt("army drag %u (%.0f,%.0f)->(%.0f,%.0f)", unsigned(aid), double(p.x), double(p.y), double(q.x), double(q.y)));
    drag(p.x, p.y, q.x, q.y);
    return "armydrag";
  }
  std::string actHandleDrag() {
    // Ручки границ: включить правку, выделить провинцию, тянуть узел рядом с ней.
    if (h_->ui.screen != app::Screen::Editor || h_->readOnly()) return actRandomClick();
    const World& w = h_->world();
    if (!h_->ui.editBorders) {
      log("setEditBorders(true)");
      h_->setEditBorders(true);
      step();
    }
    if (h_->ui.sel.type != SelType::Province) {
      auto vp = h_.visibleProvince();
      if (!vp) return "handle-none";
      log(fmt("map click province %u", unsigned(vp->first)));
      click(vp->second.x, vp->second.y);
    }
    std::vector<gfx::Pt> pts;
    w.nodes.each([&](const Node& n) {
      if (auto s = visible(n.p)) pts.push_back(*s);
    });
    if (pts.empty()) return "handle-none";
    gfx::Pt p = pick(pts);
    float dx = rx(-50, 50), dy = rx(-50, 50);
    log(fmt("handle drag (%.0f,%.0f)->(%.0f,%.0f)", double(p.x), double(p.y), double(p.x + dx), double(p.y + dy)));
    drag(p.x, p.y, p.x + dx, p.y + dy);
    return "handle";
  }
  std::string actDouble() {
    float x = rx(0, float(W_)), y = rx(0, float(H_));
    if (chance(0.5)) {
      auto v = named();
      if (!v.empty()) {
        auto& [n, r] = pick(v);
        x = r.cx();
        y = r.cy();
        log(fmt("dblclick '%s'", n.c_str()));
        dbl(x, y);
        return "dbl";
      }
    }
    log(fmt("dblclick (%.0f,%.0f)", double(x), double(y)));
    dbl(x, y);
    return "dbl";
  }
  std::string actRight() {
    float x, y;
    auto v = named();
    if (!v.empty() && chance(0.5)) {
      auto& [n, r] = pick(v);
      x = r.cx();
      y = r.cy();
      log(fmt("right click '%s'", n.c_str()));
    } else {
      gfx::Pt p = randomMapPoint();
      x = p.x;
      y = p.y;
      log(fmt("right click (%.0f,%.0f)", double(x), double(y)));
    }
    click(x, y, platform::MouseRight);
    // Контекстное меню — иногда выбрать пункт щелчком чуть ниже.
    if (chance(0.5)) {
      float yy = y + rx(10, 120), xx = x + rx(5, 120);
      log(fmt("click menu (%.0f,%.0f)", double(xx), double(yy)));
      click(xx, yy);
    }
    return "right";
  }
  std::string actWheel() {
    float x = rx(0, float(W_)), y = rx(0, float(H_));
    float n = float(rng_.range(-4, 4));
    if (n == 0) n = 1;
    log(fmt("wheel (%.0f,%.0f) %+.0f", double(x), double(y), double(n)));
    hl::mouseMove(x, y);
    step();
    hl::wheel(x, y, n);
    step();
    return "wheel";
  }
  std::string actType() {
    std::string t = randomText(rng_);
    log("type «" + t.substr(0, 40) + "»");
    type(t);
    return "type";
  }
  std::string actKey() {
    const KeyDef& k = pick(keys_);
    log(std::string("key ") + k.name);
    press(k.k, k.mods);
    return "key";
  }
  std::string actDrawer() {
    auto& ds = app::drawers();
    if (ds.empty()) return "drawer-none";
    const auto& d = ds[size_t(rng_.range(0, int(ds.size()) - 1))];
    log(std::string("openDrawer ") + d.id);
    h_->openDrawer(d.id);
    step();
    return "drawer";
  }
  std::string actSelect() {
    static const SelType ts[] = {SelType::Province, SelType::Province, SelType::Faction, SelType::Faction, SelType::Army, SelType::Route, SelType::Character};
    SelType t = ts[rng_.range(0, int(std::size(ts)) - 1)];
    Id id = randomId(t);
    if (!id) return "select-none";
    bool focus = chance(0.5);
    log(fmt("select %s %u%s", selName(t), unsigned(id), focus ? " focus" : ""));
    h_->select(t, id, focus);
    step();
    return "select";
  }
  std::string actTab() {
    const RectF* r = h_->uiRect("inspector.tabs");
    if (!r) return actSelect();
    float x = rx(r->x + 2, r->right() - 2);
    log(fmt("tab click x=%.0f", double(x)));
    click(x, r->cy());
    return "tab";
  }
  std::string actEditor() {
    int k = rng_.range(0, 6);
    std::string id;
    Id arg = 0;
    switch (k) {
      case 0: id = "techtree"; arg = randomFaction(); break;
      case 1: id = "buildings"; arg = chance(0.5) ? 0 : randomFaction(true); break;
      case 2: id = "modifiers"; arg = chance(0.5) ? 0 : randomId(SelType::None); break;
      case 3: id = "catalogs"; arg = Id(rng_.range(0, 7)); break;
      case 4: id = "trade"; break;
      case 5: id = "military"; arg = randomFaction(); break;
      default: id = "modifiers"; {
          auto ids = h_->world().modifiers.ids();
          arg = ids.empty() ? 0 : pick(ids);
        }
        break;
    }
    log(fmt("openEditor %s %u", id.c_str(), unsigned(arg)));
    h_->openEditor(id, arg);
    step();
    return "editor";
  }
  std::string actDialog() {
    int k = rng_.range(0, 7);
    std::string id;
    Id arg = 0;
    switch (k) {
      case 0: id = "turn.history"; break;
      case 1: id = "turn.report"; break;
      case 2: id = "flag"; arg = randomFaction(); break;
      case 3: id = "tribute"; arg = randomFaction(); break;
      case 4: id = "build.picker"; arg = randomLand(); break;
      case 5: id = "army.split"; arg = randomId(SelType::Army); break;
      case 6: id = "chronicle.note"; break;
      default: id = "battle"; arg = randomId(SelType::Army); break;
    }
    log(fmt("openDialog %s %u", id.c_str(), unsigned(arg)));
    h_->openDialog(id, arg);
    step();
    return "dialog";
  }
  std::string actEditMode() {
    if (chance(0.5)) {
      log("key E");
      press(Key::E);
    } else {
      log(fmt("setEditBorders(%d)", int(!h_->ui.editBorders)));
      h_->setEditBorders(!h_->ui.editBorders);
      step();
    }
    return "edit";
  }
  std::string actEndTurn() {
    if (chance(0.5)) {
      log("endTurnNow");
      h_->endTurnNow();
      step();
    } else {
      log("key C+Enter");
      press(Key::Enter, ctrl());
      if (chance(0.7)) {
        log("enter");
        press(Key::Enter);
      }
    }
    return "endturn";
  }
  std::string actUndoRedo() {
    int n = rng_.range(1, 4);
    bool undo = chance(0.65);
    for (int k = 0; k < n; k++) {
      log(undo ? "key C+Z" : "key C+Y");
      press(undo ? Key::Z : Key::Y, ctrl());
    }
    return "undo";
  }
  std::string actSaveLoad() {
    if (h_->ui.screen != app::Screen::Editor) return "save-skip";
    bool bundle = chance(0.3);
    std::string path = fs::join(h_.root, fmt("save-%d", saves_++)) + (bundle ? ".regnum" : "");
    log("saveTo " + fs::filename(path));
    bool ok = h_->saveTo(path);
    step();
    h_->waitBackground();
    if (!ok) {
      note("save", "saveTo failed", "saveTo вернул false для " + path);
      return "save";
    }
    io::LoadResult r = io::load(path);
    if (!r.warnings.empty()) {
      std::string d = "после сохранения и чтения нормализация исправила " + std::to_string(r.warnings.size()) + " знач.:";
      for (size_t i = 0; i < std::min<size_t>(6, r.warnings.size()); i++) d += "\n        " + r.warnings[i].text();
      note("roundtrip-normalize", r.warnings[0].where.empty() ? r.warnings[0].msg : r.warnings[0].file + ":" + r.warnings[0].msg, d);
    }
    std::string a = io::toJson(h_->store.world()), b = io::toJson(r.world);
    if (a != b) {
      size_t p = 0;
      while (p < a.size() && p < b.size() && a[p] == b[p]) p++;
      size_t s = p > 80 ? p - 80 : 0;
      note("roundtrip-diff", "world differs after save/load",
           "мир после чтения отличается от сохранённого; память: …" + a.substr(s, 200) + "…\n        файл: …" + b.substr(s, 200) + "…");
    }
    if (chance(0.5)) {
      log("loadProject " + fs::filename(path));
      h_->loadProject(path);
      step();
    }
    return "save";
  }
  std::string actTheme() {
    log("setTheme");
    h_->setTheme(!h_->ui.darkTheme);
    step();
    return "theme";
  }
  std::string actScale() {
    static const float s[] = {0.9f, 1.0f, 1.1f, 1.25f, 1.5f};
    float v = s[rng_.range(0, 4)];
    log(fmt("setUiScale %.2f", double(v)));
    h_->setUiScale(v);
    step();
    return "scale";
  }
  std::string actResize() {
    static const int sz[][2] = {{1440, 900}, {1280, 800}, {1024, 640}, {900, 600}, {800, 500}, {1920, 1080}, {2200, 1300}, {1600, 1000}, {700, 450}};
    static const float dp[] = {1.f, 1.f, 1.f, 1.25f, 1.5f, 2.f};
    auto& z = sz[rng_.range(0, int(std::size(sz)) - 1)];
    W_ = z[0];
    H_ = z[1];
    dpi_ = dp[rng_.range(0, int(std::size(dp)) - 1)];
    if (W_ * dpi_ > 2600) dpi_ = 1;
    log(fmt("resize %dx%d@%.2g", W_, H_, double(dpi_)));
    hl::configure(W_, H_, dpi_);
    step();
    return "resize";
  }
  std::string actViewTurn() {
    if (h_->readOnly() && chance(0.5)) {
      log("backToCurrent");
      h_->backToCurrent();
      step();
      return "back";
    }
    auto s = h_->snapshots();
    if (s.empty()) {
      log("endTurnNow (для снимка)");
      h_->endTurnNow();
      step();
      return "endturn";
    }
    int t = pick(s).turn;
    log(fmt("viewTurn %d", t));
    h_->viewTurn(t);
    step();
    return "view";
  }
  std::string actHover() {
    float x = rx(0, float(W_)), y = rx(0, float(H_));
    log(fmt("hover (%.0f,%.0f)", double(x), double(y)));
    hl::mouseMove(x, y);
    step();
    hl::advance(1.0);   // подсказка
    step();
    return "hover";
  }
  std::string actDrop() {
    // Неподходящие файлы: мусорный архив, текст, пустая папка.
    int k = rng_.range(0, 2);
    std::string p;
    if (k == 0) {
      p = fs::join(h_.root, "мусор.regnum");
      fs::writeFileAtomic(p, std::string_view("PK\x03\x04 not a zip at all"));
    } else if (k == 1) {
      p = fs::join(h_.root, "заметка.txt");
      fs::writeFileAtomic(p, std::string_view("просто текст"));
    } else {
      p = fs::join(h_.root, "пустая папка");
      fs::makeDirs(p);
    }
    log("drop " + fs::filename(p));
    hl::dropFiles(rx(100, float(W_) - 100), rx(100, float(H_) - 100), {p});
    step();
    return "drop";
  }
  // Перетаскивание от одного именованного элемента к другому (связи узлов деревьев, строки списков, ячейки совета).
  std::string actDragNamed() {
    auto v = named();
    if (v.size() < 2) return actRandomClick();
    auto& [na, ra] = pick(v);
    auto& [nb, rb] = pick(v);
    log(fmt("drag '%s' -> '%s'", na.c_str(), nb.c_str()));
    drag(ra.cx(), ra.cy(), rb.cx(), rb.cy(), platform::MouseLeft, 0, rng_.range(3, 10));
    return "dragnamed";
  }
  // Удалить показанную сущность «из другой панели» (как пользователь со списка), затем иногда отменить.
  std::string actStale() {
    if (h_->readOnly() || h_->ui.screen != app::Screen::Editor) return actKey();
    auto& u = h_->ui;
    std::string label;
    std::function<void(Tx&)> fn;
    Id id = 0;
    if (!u.editor.empty() && u.editorArg && chance(0.6)) {
      id = u.editorArg;
      if (u.editor == "techtree" || u.editor == "military" || (u.editor == "buildings" && h_->world().faction(id))) {
        label = "stale: removeFaction";
        fn = [id](Tx& tx) { rules::removeFaction(tx, id); };
      } else if (u.editor == "modifiers" && h_->world().modifier(id)) {
        label = "stale: removeModifier";
        fn = [id](Tx& tx) { rules::removeModifier(tx, id); };
      }
    }
    if (!fn && u.sel) {
      id = u.sel.id;
      switch (u.sel.type) {
        case SelType::Province: label = "stale: deleteProvince"; fn = [id](Tx& tx) { rules::deleteProvince(tx, id); }; break;
        case SelType::Faction: label = "stale: removeFaction"; fn = [id](Tx& tx) { rules::removeFaction(tx, id); }; break;
        case SelType::Army: label = "stale: disband"; fn = [id](Tx& tx) { rules::disband(tx, id); }; break;
        case SelType::Route: label = "stale: removeRoute"; fn = [id](Tx& tx) { rules::removeRoute(tx, id); }; break;
        case SelType::Character: label = "stale: removeCharacter"; fn = [id](Tx& tx) { rules::removeCharacter(tx, id); }; break;
        default: break;
      }
    }
    if (!fn && !h_->world().techs.ids().empty() && chance(0.5)) {
      id = pick(h_->world().techs.ids());
      label = "stale: removeTech";
      fn = [id](Tx& tx) { rules::removeTech(tx, id); };
    }
    if (!fn && !h_->world().buildings.ids().empty()) {
      id = pick(h_->world().buildings.ids());
      label = "stale: removeBuilding";
      fn = [id](Tx& tx) { rules::removeBuilding(tx, id); };
    }
    if (!fn) return actKey();
    log(fmt("%s %u", label.c_str(), unsigned(id)));
    h_->act(label, fn);
    step();
    if (chance(0.4)) {
      log("key C+Z");
      press(Key::Z, ctrl());
    }
    return "stale";
  }
  // Сосредоточенный режим (REGNUM_FUZZ_FOCUS=editor:buildings | editor:techtree | … | tab:province | tab:faction |
  // drawer): плотный поток действий внутри одного раздела; раздел открывается заново, если обезьяна его закрыла.
  std::string actFocused() {
    auto& u = h_->ui;
    if (focus_.rfind("editor:", 0) == 0) {
      std::string ed = focus_.substr(7);
      if (u.editor != ed || chance(0.01)) {
        Id arg = 0;
        if (ed == "techtree" || ed == "military") arg = randomFaction();
        else if (ed == "buildings") arg = chance(0.5) ? 0 : randomFaction(true);
        else if (ed == "catalogs") arg = Id(rng_.range(0, 7));
        else if (ed == "modifiers") {
          auto ids = h_->world().modifiers.ids();
          arg = ids.empty() || chance(0.3) ? 0 : pick(ids);
        }
        log(fmt("openEditor %s %u", ed.c_str(), unsigned(arg)));
        h_->openEditor(ed, arg);
        step();
        return "focus-editor";
      }
    } else if (focus_.rfind("tab:", 0) == 0) {
      SelType t = focus_ == "tab:faction" ? SelType::Faction : focus_ == "tab:army" ? SelType::Army : focus_ == "tab:character" ? SelType::Character
                                                                                                    : SelType::Province;
      if (!u.editor.empty()) {
        log("closeEditor");
        h_->closeEditor();
        step();
      }
      if (u.sel.type != t || chance(0.03)) {
        Id id = randomId(t);
        log(fmt("select %s %u", selName(t), unsigned(id)));
        if (id) h_->select(t, id, chance(0.3));
        step();
        return "focus-select";
      }
      if (chance(0.15)) return actTab();
    } else if (focus_ == "drawer") {
      if (u.drawer.empty() || chance(0.04)) return actDrawer();
    }
    double r = rng_.uniform() * 100;
    if (r < 40) return actNamed();
    if (r < 50) return actType();
    if (r < 60) return actKey();
    if (r < 68) return actDragNamed();
    if (r < 73) return actWheel();
    if (r < 80) return actRandomClick();
    if (r < 84) return actUndoRedo();
    if (r < 87) return actHover();
    if (r < 89) return actResize();
    if (r < 91) return actScale();
    if (r < 92) return actTheme();
    if (r < 95) return actStale();
    if (r < 97) return actDouble();
    if (r < 98.5) return actRight();
    return actViewTurn();
  }
  std::string actFocus() {
    log("focus out/in");
    hl::setFocus(false);
    step();
    hl::setFocus(true);
    step();
    return "focus";
  }

  // ---------------------------------------------------------------- проверки
  void baseline() {
    lastNorm_ = normIssues();
    lastGeo_ = geoIssues();
    log_.fresh();
  }
  std::set<std::string> normIssues() {
    World c = h_->store.world();
    io::Warnings ws;
    io::normalize(c, ws);
    std::set<std::string> s;
    for (auto& w : ws) s.insert(w.text());
    return s;
  }
  std::set<std::string> geoIssues() {
    std::set<std::string> s;
    for (auto& i : geo::validate(h_->store.world())) s.insert(i.code + ": " + i.msg);
    return s;
  }

  void invariants() {
    const World& w = h_->store.world();
    w.provinces.each([&](const Province& p) {
      if (p.hqs.size() > 5) note("invariant", "hq>5", fmt("в провинции %u штабов: %d", unsigned(p.id), int(p.hqs.size())));
      std::set<Id> u(p.hqs.begin(), p.hqs.end());
      if (u.size() != p.hqs.size()) note("invariant", "hq-dup", fmt("повтор штаба в провинции %u", unsigned(p.id)));
      double sum = 0;
      for (auto& i : p.influence) {
        sum += i.pct;
        if (i.pct < -1e-9) note("invariant", "influence<0", fmt("отрицательное влияние в провинции %u: %g", unsigned(p.id), i.pct));
      }
      if (sum > 100 + 1e-6) note("invariant", "influence>100", fmt("сумма влияния в провинции %u = %g", unsigned(p.id), sum));
      if (p.resourceAmount < 0) note("invariant", "resourceAmount<0", fmt("количество ресурса провинции %u = %g", unsigned(p.id), p.resourceAmount));
      for (auto& r : p.races)
        if (r.pop < 0) note("invariant", "pop<0", fmt("отрицательная численность расы в провинции %u", unsigned(p.id)));
    });
    w.factions.each([&](const Faction& f) {
      for (auto& [rid, v] : f.res)
        if (rid != kGold && v < -1e-9) note("invariant", "stock<0", fmt("запас ресурса %u у фракции %u = %g", unsigned(rid), unsigned(f.id), v));
      for (auto& r : f.army)
        if (r.total < 0) note("invariant", "row-total<0", fmt("общая численность строки %u < 0", unsigned(r.id)));
    });
    auto c = rules::calc(w);
    for (auto& [fid, fc] : c->factions) {
      for (auto& r : fc.army)
        if (r.reserve < 0) note("invariant", "army-reserve<0", fmt("резерв строки армии %u фракции %u = %lld", unsigned(r.row), unsigned(fid), (long long)r.reserve));
      for (auto& r : fc.fleet)
        if (r.reserve < 0) note("invariant", "fleet-reserve<0", fmt("резерв строки флота %u фракции %u = %lld", unsigned(r.row), unsigned(fid), (long long)r.reserve));
    }
  }

  void after(bool roBefore, const World& before, u64 v0) {
    // Журнал
    for (auto& line : log_.fresh()) {
      bool warn = line.find("[WARN]") != std::string::npos, err = line.find("[ERROR]") != std::string::npos;
      if (!warn && !err) continue;
      size_t p = line.find("] ");
      std::string msg = p == std::string::npos ? line : line.substr(p + 2);
      std::string key = msg;
      if (key.find("повторяющийся ID") != std::string::npos) {
        size_t a = key.find("рядом");
        if (a != std::string::npos) key = key.substr(0, a);
        key += "@" + h_->ui.drawer + "/" + h_->ui.editor + "/" + (h_->ui.sel ? selName(h_->ui.sel.type) : "") + "/" +
               (h_->ui.sel ? h_->ui.tabOf[h_->ui.sel.type] : "") + "/" + (h_->hasDialog() ? h_->dialogStack().back()->id() : "");
      }
      note(err ? "log-error" : "log-warn", key, msg);
    }
    // Уведомления об ошибке
    for (auto& t : h_->toasts())
      if (t.kind == app::ToastKind::Danger) note("toast-danger", t.text, t.text);
    // Выход по команде: оживить платформу и продолжить.
    if (hl::quitRequested()) {
      if (dirtyBefore_) note("quit-dirty", "quit", "приложение вышло без вопроса о несохранённых изменениях");
      else note("info-quit", "quit", "приложение запросило выход (мир сохранён)");
      double now = platform::time();
      hl::reset();
      hl::configure(W_, H_, dpi_);
      hl::advance(now);
      hl::attach(h_.app.get());
      step();
    }
    for (auto& f : hl::fatalMessages()) note("fatal", f, f);
    // Только чтение прошлого хода
    bool commits = false, loads = false, saveCommit = false, undoRedo = false;
    int nCommits = 0;
    for (auto& [k, l] : changes_) {
      if (k == Change::Commit) {
        commits = true;
        nCommits++;
        if (l == "Сохранить") saveCommit = true;
      } else if (k == Change::Load) {
        loads = true;
      } else {
        undoRedo = true;
        loads = true;
      }
    }
    if (roBefore && h_->readOnly() && commits && nCommits > (saveCommit ? 1 : 0)) {   // «Сохранить» — служебная отметка времени
      std::string labels;
      for (auto& [k, l] : changes_) labels += "«" + l + "» ";
      note("readonly", labels, "мир изменился при просмотре прошлого хода: " + labels);
    }
    // Сочетание без Ctrl при фокусе в текстовом поле не должно удалять сущности.
    if (kbBefore_ && cur_.find(" key ") != std::string::npos && cur_.find("C+") == std::string::npos)
      for (auto& [k, l] : changes_)
        if (k == Change::Commit && l.find("Удалить") != std::string::npos) note("kb-shortcut", l, "клавиша при фокусе в поле выполнила «" + l + "»");
    // Время кадра
    if (curFrameMax_ > worstFrame) {
      worstFrame = curFrameMax_;
      worstFrameCtx = cur_ + " | " + context();
    }
    if (curFrameMax_ > slowMs_) {
      std::string k = h_->ui.editor.empty() ? (h_->hasDialog() ? std::string("dialog:") + h_->dialogStack().back()->id() : std::string("map/shell"))
                                            : "editor:" + h_->ui.editor;
      Finding& f = findings["slow|" + k];
      if (f.count++ == 0 || curFrameMax_ > std::atof(f.key.c_str())) {
        f.kind = "slow-frame";
        f.key = fmt("%.0f", curFrameMax_);
        f.detail = fmt("кадр %.0f мс (%s)", curFrameMax_, k.c_str());
        f.context = context();
        f.trail.assign(trail_.end() - std::min<long>(4, long(trail_.size())), trail_.end());
        f.step = actions;
        f.seed = seed_;
      }
    }
    if (h_->store.version() == v0) return;
    // Инварианты мира
    invariants();
    auto ni = normIssues();
    for (auto& s : ni)
      if (!lastNorm_.count(s)) {
        size_t c = s.rfind(": ");
        note("normalize", c == std::string::npos ? s : s.substr(c + 2), "io::normalize исправил бы: " + s);
      }
    lastNorm_ = ni;
    if (tables_ & TB_GEO || loads) {
      auto gi = geoIssues();
      for (auto& s : gi)
        if (!lastGeo_.count(s)) note("geo-validate", s.substr(0, s.find(':')), "geo::validate: " + s);
      lastGeo_ = gi;
    }
    // Отмена: действие с изменениями отменяется и повторяется точно.
    std::string actionLabels;
    for (auto& [k, l] : changes_) actionLabels += (k == Change::Commit ? "«" : k == Change::Load ? "load«" : "undo/redo«") + l + "» ";
    if (commits && !loads && !undoRedo && !saveCommit && !h_->readOnly() && nCommits > 0) {
      World mid = h_->store.world();
      int undone = 0;
      bool restored = false;
      for (int k = 0; k < nCommits + 1; k++) {
        if (!h_->store.undo()) break;
        undone++;
        if (World::diff(h_->store.world(), before) == 0) {
          restored = true;
          break;
        }
      }
      if (!restored) {
        std::string labels;
        for (auto& [k, l] : changes_) labels += "«" + l + "» ";
        note("undo", labels, fmt("после %d отмен мир не вернулся к состоянию до действия (таблицы 0x%x); правки: ", undone,
                                 World::diff(h_->store.world(), before)) + labels);
      }
      for (int k = 0; k < undone; k++) h_->store.redo();
      if (World::diff(h_->store.world(), mid) != 0) note("redo", "redo", fmt("повтор не вернул мир после действия (таблицы 0x%x)", World::diff(h_->store.world(), mid)));
      step();
    }
  }

  void finalChecks() {
    // Сохранение и чтение в конце прогона.
    if (h_->ui.screen == app::Screen::Editor) {
      actions++;
      actSaveLoad();
    }
  }
};

void report(const std::string& name, const std::map<std::string, Finding>& all, const Monkey& m, double sec) {
  std::printf("  [ui-fuzz] %s: %d действий, %d кадров, %.1f с, худший кадр %.0f мс (%s)\n", name.c_str(), m.actions, m.frames, sec, m.worstFrame,
              m.worstFrameCtx.c_str());
  for (auto& [k, f] : all) {
    std::printf("  [ui-fuzz] FINDING %s x%d (seed %llu, step %d): %s\n        context: %s\n", f.kind.c_str(), f.count, (unsigned long long)f.seed, f.step,
                f.detail.c_str(), f.context.c_str());
    size_t from = f.trail.size() > 12 ? f.trail.size() - 12 : 0;
    for (size_t i = from; i < f.trail.size(); i++) std::printf("          %s\n", f.trail[i].c_str());
  }
  std::fflush(stdout);
}

int runSeeds(const std::string& name, std::vector<u64> seeds, int events) {
  int bad = 0;
  for (u64 s : seeds) {
    double t0 = nowSeconds();
    Harness h("fuzz_" + name + "_" + std::to_string(s));
    h.demo();
    h.waitMap();
    Monkey m(h, s, name + "_" + std::to_string(s));
    m.run(events);
    std::map<std::string, Finding> real;
    for (auto& [k, f] : m.findings)
      if (f.kind.rfind("info-", 0) != 0) real[k] = f;
    report(name + " seed " + std::to_string(s), m.findings, m, nowSeconds() - t0);
    bad += int(real.size());
  }
  return bad;
}

}  // namespace

// Короткий прогон в общем наборе: устойчивость (без исключений), находки печатаются.
TEST(app_audit_fuzz_monkey) {
  int events = envInt("REGNUM_FUZZ_EVENTS", 120);
  int bad = runSeeds("monkey", envSeeds({1, 2, 3}), events);
  if (auditStrict()) CHECK_EQ(bad, 0);
}
