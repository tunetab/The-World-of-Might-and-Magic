// Regnum — история ходов (DialogReg «turn.history»): лента снимков ходов с итогом каждого хода (записи хроники
// по видам, казна), просмотр прошлого хода только для чтения, возврат к снимку (текущее состояние остаётся в
// истории), сравнение показателей фракций по ходам (мини-графики казны, провинций, населения, войск).
// Снимки с диска читаются в фоне (jobs) и кешируются на время работы приложения.
#include <chrono>
#include <future>
#include <mutex>

#include "app/app_internal.h"
#include "app/dialogs/turn_ui.h"
#include "app/widgets.h"
#include "base/jobs.h"

namespace rg::app::turnui {

namespace {

// ---------------------------------------------------------------- показатели снимка
enum Metric { MTreasury, MProvinces, MPopulation, MArmy, MCount };

struct TurnStats {
  int turn = 0;
  std::map<Id, std::array<double, MCount>> v;   // фракция -> показатели
};

TurnStats statsOf(const World& w) {
  TurnStats s;
  s.turn = w.turn();
  w.factions.each([&](const Faction& f) {
    auto& a = s.v[f.id];
    a.fill(0);
    a[MTreasury] = f.treasury();
    double army = 0;
    for (auto& r : f.army) army += double(r.total);
    a[MArmy] = army;
  });
  w.provinces.each([&](const Province& p) {
    if (p.sea || !p.owner) return;
    auto it = s.v.find(p.owner);
    if (it == s.v.end()) return;
    it->second[MProvinces] += 1;
    double pop = 0;
    for (auto& r : p.races) pop += double(std::max<i64>(0, r.pop));
    it->second[MPopulation] += pop;
  });
  return s;
}

using StatsList = std::vector<std::pair<std::string, TurnStats>>;

struct StatsCache {
  std::map<std::string, TurnStats> stats;
  std::future<StatsList> job;
};
StatsCache& cache() {
  static StatsCache c;
  return c;
}

// Ключ снимка: файл истории проекта или номер снимка в памяти этого сеанса.
std::string keyOf(App& a, const TurnSnapshot& s) {
  std::string src = s.memory ? "mem" + std::to_string(sessionTag(a)) : a.projectPath();
  return src + "#" + s.key + "#" + s.at;
}

// Поставить в очередь чтение недостающих снимков. true — всё уже есть.
bool ensureStats(App& a, const std::vector<TurnSnapshot>& list) {
  StatsCache& c = cache();
  if (c.job.valid()) {
    if (c.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    try {
      for (auto& [k, s] : c.job.get()) c.stats[k] = std::move(s);
    } catch (const std::exception& e) {
      logWarn("История ходов: %s", e.what());
    }
  }
  const std::string& path = a.projectPath();
  struct Todo {
    std::string key, file;
    int turn = 0;
  };
  std::vector<Todo> todo;
  for (const TurnSnapshot& s : list) {
    std::string k = keyOf(a, s);
    if (c.stats.count(k)) continue;
    if (s.memory) {
      for (const MemSnapshot& m : a.impl().memSnaps)
        if ("mem:" + std::to_string(m.seq) == s.key) c.stats[k] = statsOf(m.world);
      continue;
    }
    if (!path.empty()) todo.push_back({k, s.key, s.turn});
  }
  if (todo.empty()) return true;
  c.job = jobs::submit([path, todo]() {
    StatsList out;
    for (const Todo& t : todo) {
      try {
        io::LoadResult r = io::loadSnapshotFile(path, t.file);
        out.push_back({t.key, statsOf(r.world)});
      } catch (const std::exception& e) {
        logWarn("Снимок хода %d не прочитан: %s", t.turn, e.what());
        TurnStats none;
        none.turn = t.turn;
        out.push_back({t.key, std::move(none)});   // без показателей, чтобы не читать снова
      }
    }
    platform::wake();
    return out;
  });
  return false;
}

const TurnStats* statsFor(App& a, const TurnSnapshot& s) {
  auto& m = cache().stats;
  auto it = m.find(keyOf(a, s));
  return it == m.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------- диалог
struct HistoryDlg : Dialog {
  std::vector<TurnSnapshot> list;
  bool loaded = false, statsReady = false;
  int tab = 0, metric = 0;
  const char* id() const override { return "turn.history"; }
  Style style(App&) override {
    Style s;
    s.title = "История ходов";
    s.icon = "history";
    s.width = 880;
    s.dismissOnBackdrop = true;
    return s;
  }

  float bodyH() const { return std::round(clamp(ui::viewport().h - 330, 200.f, 470.f)); }

  // Полоса текущего хода (или открытого прошлого) с возвратом.
  bool currentStrip(App& a) {
    const ui::Theme& th = ui::theme();
    int current = a.store.world().turn();
    RectF r = ui::next(52);
    bool viewing = a.readOnly();
    Color tc = viewing ? th.info : th.accent;
    ui::draw::rect(r, tc.alpha(0.10f), th.radiusCard);
    ui::draw::rectStroke(r, tc.alpha(0.4f), th.radiusCard, 1);
    ui::draw::icon(viewing ? "eye" : "hourglass", RectF{r.x + 16, r.cy() - 9, 18, 18}, tc);
    std::string title = viewing ? "Открыт ход " + std::to_string(*a.ui.viewTurn) + " · только просмотр" : "Ход " + std::to_string(current) + " — текущий";
    ui::draw::text(title, RectF{r.x + 46, r.y + 7, r.w - 260, 20}, ui::Font::Strong, th.text);
    size_t unsaved = size_t(std::count_if(list.begin(), list.end(), [](const TurnSnapshot& s) { return s.memory; }));
    std::string sub = std::to_string(list.size()) + " " + plural(i64(list.size()), "ход", "хода", "ходов") + " в истории" +
                      (unsaved ? " · новые снимки запишутся при сохранении мира" : "");
    ui::draw::text(sub, RectF{r.x + 46, r.y + 27, r.w - 260, 18}, ui::Font::Small, th.textMuted);
    if (viewing) {
      ui::at(RectF{r.right() - 212, r.cy() - 14, 200, 28});
      if (ui::button("К текущему ходу " + std::to_string(current), {.variant = ui::Variant::Primary, .icon = "arrow-right", .size = ui::Size::Small})) {
        detail::later(a, [](App& x) { x.backToCurrent(); });
        return false;
      }
      a.markUi("history.back");
    }
    return true;
  }

  // Итог хода k по хронике текущего мира: число записей по видам.
  void turnSummary(App& a, RectF r, int turn) {
    const ui::Theme& th = ui::theme();
    const World& w = a.store.world();
    std::array<int, int(LogKind::Count)> n{};
    int total = 0;
    w.log.each([&](const LogEntry& e) {
      if (e.turn != turn || e.kind == LogKind::Turn || int(e.kind) >= int(LogKind::Count)) return;
      n[size_t(e.kind)]++;
      total++;
    });
    float x = r.x;
    if (total == 0) {
      ui::draw::text("событий нет", RectF{x, r.y, r.w, r.h}, ui::Font::Small, th.textMuted);
      return;
    }
    std::vector<int> kinds;
    for (int k = 0; k < int(LogKind::Count); k++)
      if (n[size_t(k)]) kinds.push_back(k);
    std::stable_sort(kinds.begin(), kinds.end(), [&](int p, int q) { return n[size_t(p)] > n[size_t(q)]; });
    for (int k : kinds) {
      std::string cnt = std::to_string(n[size_t(k)]);
      float tw = ui::measure(cnt, ui::Font::Small) + 2;
      if (x + 20 + tw > r.right()) break;
      Color c = ui::toneColor(logTone(LogKind(k)));
      ui::draw::rect(RectF{x, r.cy() - 11, 22 + tw + 8, 22}, c.alpha(0.12f), 11);
      ui::draw::icon(logIcon(LogKind(k)), RectF{x + 6, r.cy() - 7, 14, 14}, c);
      ui::draw::text(cnt, RectF{x + 23, r.y, tw, r.h}, ui::Font::Small, th.textDim);
      x += 22 + tw + 8 + 6;
    }
  }

  bool timeline(App& a) {
    const ui::Theme& th = ui::theme();
    if (list.empty()) {
      RectF r = ui::next(std::min(bodyH(), 220.f));
      ui::Area ar(RectF{r.x, r.y + r.h * 0.5f - 70, r.w, 140}, 0);
      ui::emptyState("history", "Снимки появятся после первого завершённого хода.");
      return true;
    }
    int current = a.store.world().turn();
    const float rowH = 72;
    float h = std::min(bodyH(), float(list.size()) * rowH);
    ui::VirtualList vl("snaps", int(list.size()), rowH, h);
    for (int i : vl) {
      const TurnSnapshot& s = list[list.size() - 1 - size_t(i)];
      ui::IdScope sc{s.turn};
      RectF r = ui::next(rowH - 8);
      bool viewing = a.ui.viewTurn && *a.ui.viewTurn == s.turn;
      bool future = s.turn > current;
      ui::draw::rect(r, viewing ? th.info.alpha(0.12f) : th.surface2, th.radiusCard);
      ui::draw::rectStroke(r, viewing ? th.info.alpha(0.5f) : th.border, th.radiusCard, 1);
      // Номер хода в кольце
      Color rc = viewing ? th.info : future ? th.textMuted : th.accent;
      float cx = r.x + 30, cy = r.cy();
      ui::draw::circle(cx, cy, 18, rc.alpha(0.12f));
      ui::draw::ring(cx, cy, 18, 1.5f, rc.alpha(0.7f));
      ui::draw::text(std::to_string(s.turn), RectF{cx - 18, cy - 10, 36, 20}, ui::Font::Strong, th.text, ui::Align::Center);
      // Подпись и время
      float tx = r.x + 62;
      std::string title = "Ход " + std::to_string(s.turn);
      ui::draw::text(title, RectF{tx, r.y + 10, 200, 20}, ui::Font::Strong, th.text);
      std::string extra = s.label != title && !s.label.empty() ? s.label : std::string();
      if (startsWith(extra, title + " · ")) extra = extra.substr(title.size() + 4);
      std::string sub = detail::localTime(s.at);
      if (s.memory) sub += sub.empty() ? "в памяти" : " · в памяти";
      if (!extra.empty()) sub += sub.empty() ? extra : " · " + extra;
      ui::draw::text(sub, RectF{tx, r.y + 32, 250, 18}, ui::Font::Small, th.textMuted);
      // Итог хода: события и казна
      float midX = r.x + 330, midW = r.w - 330 - 250;
      turnSummary(a, RectF{midX, r.y + 8, midW, 24}, s.turn);
      if (const TurnStats* st = statsFor(a, s)) {
        double sum = 0;
        int debt = 0;
        for (auto& [fid, v] : st->v) {
          sum += v[MTreasury];
          if (v[MTreasury] < 0) debt++;
        }
        std::string t = "Σ казна " + fmtNum(sum) + (debt ? " · в долгу " + std::to_string(debt) : std::string());
        ui::draw::icon("coins", RectF{midX, r.y + 40, 14, 14}, th.textMuted);
        ui::draw::text(t, RectF{midX + 20, r.y + 37, midW - 20, 20}, ui::Font::Small, th.textDim);
      }
      // Действия
      ui::at(RectF{r.right() - 196, r.cy() - 15, 140, 30});
      bool same = s.turn == current && !a.readOnly();
      if (ui::button(viewing ? "Открыт" : "Посмотреть", {.variant = ui::Variant::Secondary, .icon = "eye", .disabled = viewing || same,
                                                         .tooltip = same ? "Это текущий ход" : "Открыть ход только для просмотра"})) {
        int t = s.turn;
        detail::later(a, [t](App& x) { x.viewTurn(t); });
        return false;
      }
      a.markUi("history.open." + std::to_string(s.turn));
      ui::at(RectF{r.right() - 48, r.cy() - 15, 30, 30});
      std::string tip = s.turn == current ? "Начать этот ход заново" : "Вернуться к этому ходу";
      if (ui::iconButton("undo", tip, {.tone = ui::Tone::Danger})) {
        int t = s.turn;
        detail::later(a, [t](App& x) { askRollback(x, t); });
      }
      a.markUi("history.rollback." + std::to_string(s.turn));
    }
    return true;
  }

  void compare(App& a) {
    const World& w = a.store.world();
    int current = w.turn();
    static const ui::Segment kMetrics[] = {{"treasury", "Казна"}, {"province", "Провинции"}, {"population", "Население"}, {"army", "Войска"}};
    {
      ui::HStack hs(30, ui::Align::Left, 10);
      ui::segmented("metric", metric, std::span<const ui::Segment>(kMetrics), {.fill = false});
      a.markUi("history.metric");
      ui::flex();
      if (!statsReady) {
        ui::spinner(16);
        ui::label("Загрузка снимков…", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
      }
    }
    // Точки: снимки прошлых ходов + текущий мир.
    std::vector<std::pair<int, const TurnStats*>> pts;
    for (const TurnSnapshot& s : list)
      if (s.turn < current)
        if (const TurnStats* st = statsFor(a, s)) pts.push_back({s.turn, st});
    TurnStats now = statsOf(w);
    pts.push_back({current, &now});
    if (pts.size() < 2) {
      RectF r = ui::next(std::min(bodyH(), 220.f));
      ui::Area ar(RectF{r.x, r.y + r.h * 0.5f - 70, r.w, 140}, 0);
      ui::emptyState("chart-bar", statsReady ? "Сравнение появится после первого завершённого хода." : "Снимки загружаются…");
      return;
    }
    std::vector<const Faction*> facs;
    w.factions.each([&](const Faction& f) { facs.push_back(&f); });
    std::stable_sort(facs.begin(), facs.end(), [](const Faction* x, const Faction* y) {
      if (x->kind != y->kind) return x->kind < y->kind;
      return compareRu(x->name, y->name) < 0;
    });
    if (metric == MPopulation || metric == MProvinces)
      facs.erase(std::remove_if(facs.begin(), facs.end(), [](const Faction* f) { return f->isGuild(); }), facs.end());
    struct Line {
      std::vector<float> v;
      double first = 0, last = 0;
    };
    std::vector<Line> lines(facs.size());
    for (size_t i = 0; i < facs.size(); i++) {
      for (auto& [turn, st] : pts) {
        auto it = st->v.find(facs[i]->id);
        double x = it == st->v.end() ? 0 : it->second[size_t(metric)];
        lines[i].v.push_back(float(x));
      }
      lines[i].first = lines[i].v.front();
      lines[i].last = lines[i].v.back();
    }
    auto fmtV = [&](double v) { return metric == MPopulation ? fmtShort(v) : fmtNum(v); };
    std::string range = "Ходы " + std::to_string(pts.front().first) + "–" + std::to_string(pts.back().first);
    ui::Column cols[] = {{"Фракция", nullptr, ui::fr(1.5f)},
                         {range, "chart-bar", ui::fr(2.1f)},
                         {"Начало", nullptr, ui::fr(0.8f), ui::Align::Right, true},
                         {"Сейчас", nullptr, ui::fr(0.8f), ui::Align::Right, true},
                         {"Изменение", nullptr, ui::fr(0.8f), ui::Align::Right, true}};
    ui::Table t("compare", cols, int(facs.size()), {.rowHeight = 40, .height = bodyH() - 38, .selectable = false, .emptyIcon = "crown", .emptyText = "Фракций нет"});
    t.sort([&](int x, int y, int col) {
      double a1 = col == 2 ? lines[size_t(x)].first : col == 3 ? lines[size_t(x)].last : lines[size_t(x)].last - lines[size_t(x)].first;
      double b1 = col == 2 ? lines[size_t(y)].first : col == 3 ? lines[size_t(y)].last : lines[size_t(y)].last - lines[size_t(y)].first;
      return a1 < b1 ? -1 : a1 > b1 ? 1 : 0;
    });
    for (int i : t) {
      const Faction* f = facs[size_t(i)];
      const Line& l = lines[size_t(i)];
      t.cell();
      factionLabel(w, f->id);
      t.cell();
      ui::sparkline(l.v, {.height = 26, .color = f->color});
      t.text(fmtV(l.first), ui::Ink::Muted);
      t.text(fmtV(l.last), l.last < 0 ? ui::Ink::Danger : ui::Ink::Normal);
      double d = l.last - l.first;
      t.text(metric == MPopulation ? (d > 0 ? "+" : "") + fmtShort(d) : fmtSigned(d), deltaInk(d));
    }
  }

  bool draw(App& a) override {
    if (!loaded) {
      list = a.snapshots();
      loaded = true;
    }
    if (!statsReady) {
      statsReady = ensureStats(a, list);
      if (!statsReady) ui::requestRedraw();
    }
    if (!currentStrip(a)) return false;
    ui::tabs("tabs", tab, {{"history", "Ходы", {}, int(list.size())}, {"chart-bar", "Сравнение"}}, {.style = ui::TabStyle::Pill, .fill = true});
    a.markUi("history.tabs");
    if (tab == 0) {
      if (!timeline(a)) return false;
    } else {
      compare(a);
    }
    ui::ModalFooter f;
    if (ui::button("Хроника", {.icon = "chronicle"})) {
      detail::later(a, [](App& x) { showChronicle(x, 0); });
      return false;
    }
    if (ui::button("Закрыть", {.variant = ui::Variant::Primary, .isDefault = true})) return false;
    a.markUi("history.close");
    return true;
  }
};

DialogReg reg({"turn.history", [](App& a, Id) -> std::unique_ptr<Dialog> { return makeHistory(a); }});

}  // namespace

std::unique_ptr<Dialog> makeHistory(App&) { return std::make_unique<HistoryDlg>(); }

}  // namespace rg::app::turnui
