// Regnum — ход: подтверждение с предварительным итогом, снимки мира (конец хода перед расчётом и начало
// следующего), расчёт (rules::endTurn), уведомление с отчётом, просмотр прошлых ходов и возврат к ним.
// История с ветвями: снимки уникальны (ход + вид + номер), ветвь текущего мира — цепочка parent от snapHead;
// новые снимки живут в памяти до сохранения мира. Диалоги — dialogs/turn_confirm.cpp, turn_report.cpp, history.cpp.
#include <unordered_map>

#include "app/app_internal.h"
#include "app/dialogs/turn_ui.h"
#include "base/jobs.h"

namespace rg::app {

using namespace detail;

namespace {

// ---------------------------------------------------------------- снимки истории (диск и память)
struct Snap {
  io::SnapshotInfo info;
  const MemSnapshot* mem = nullptr;   // действителен, пока memSnaps не меняется
};

const io::History& diskHistory(App::Impl& d) {
  if (!d.diskHist) {
    d.diskHist.emplace();
    if (!d.path.empty() && io::isProject(d.path)) {
      try {
        *d.diskHist = io::listHistory(d.path);
      } catch (const std::exception& e) {
        logWarn("История ходов: %s", e.what());
      }
    }
  }
  return *d.diskHist;
}

std::vector<Snap> allSnaps(App::Impl& d) {
  std::vector<Snap> out;
  for (const io::SnapshotInfo& s : diskHistory(d).list) out.push_back(Snap{s, nullptr});
  for (const MemSnapshot& m : d.memSnaps) {
    Snap s;
    s.info.turn = m.turn;
    s.info.at = m.at;
    s.info.label = m.label;
    s.info.seq = m.seq;
    s.info.kind = m.kind;
    s.info.parent = m.parent;
    s.mem = &m;
    out.push_back(std::move(s));
  }
  return out;
}

bool isKind(const Snap& s, const char* kind) { return s.info.kind == kind; }

// Ветвь: снимки от head к началу по ссылкам parent.
std::vector<const Snap*> chainOf(const std::vector<Snap>& all, u64 head) {
  std::unordered_map<u64, const Snap*> bySeq;
  for (const Snap& s : all)
    if (s.info.seq) bySeq[s.info.seq] = &s;
  std::vector<const Snap*> out;
  for (u64 q = head; q && out.size() <= all.size();) {
    auto it = bySeq.find(q);
    if (it == bySeq.end()) break;
    out.push_back(it->second);
    q = it->second->info.parent;
  }
  return out;
}

// Самый новый снимок хода turn, подходящий под условие (номер больше — новее; без номера — старые).
template <class P>
const Snap* latest(const std::vector<Snap>& all, int turn, P pred) {
  const Snap* best = nullptr;
  for (const Snap& s : all)
    if (s.info.turn == turn && pred(s) && (!best || s.info.seq >= best->info.seq)) best = &s;
  return best;
}

// Начало хода turn в ветви head (запасной вариант — самое новое начало этого хода).
const Snap* startOf(const std::vector<Snap>& all, u64 head, int turn) {
  for (const Snap* s : chainOf(all, head))
    if (s->info.turn == turn && isKind(*s, io::kSnapStart)) return s;
  return latest(all, turn, [](const Snap& s) { return isKind(s, io::kSnapStart); });
}

// Снимок, который открывает строка хода turn: текущий ход — его начало; прошлый — конец хода в текущей ветви;
// будущий (после возврата) — самое новое состояние брошенной ветви.
const Snap* rowSnap(const std::vector<Snap>& all, u64 head, int current, int turn) {
  auto any = [](const Snap&) { return true; };
  auto notStart = [](const Snap& s) { return !isKind(s, io::kSnapStart); };
  if (turn == current) return startOf(all, head, turn);
  if (turn < current) {
    for (const Snap* s : chainOf(all, head))
      if (s->info.turn == turn && isKind(*s, io::kSnapEnd)) return s;
    if (const Snap* s = latest(all, turn, [](const Snap& x) { return x.info.kind.empty() || isKind(x, io::kSnapEnd); })) return s;
  }
  if (const Snap* s = latest(all, turn, notStart)) return s;
  return latest(all, turn, any);
}

std::string snapKey(const Snap& s) { return s.mem ? "mem:" + std::to_string(s.info.seq) : s.info.file; }

World loadSnap(App::Impl& d, const Snap& s) {
  if (s.mem) return s.mem->world;
  return std::move(io::loadSnapshotFile(d.path, s.info.file).world);
}

// Смена ветви при «Завершить ход» и возвратах — для Ctrl+Z / Ctrl+Y (подписка на хранилище).
bool headLabel(std::string_view l) {
  return l == "Завершить ход" || startsWith(l, "Возврат к ходу ") || startsWith(l, "Отмена возврата к ходу ");
}

void trackHead(App& a) {
  App::Impl& d = a.impl();
  if (d.headSub) return;
  App* ap = &a;
  d.headSub = a.store.subscribe([ap](const Change& c) {
    App::Impl& x = ap->impl();
    switch (c.kind) {
      case Change::Commit:
        if (!ap->store.canRedo()) x.headRedo.clear();   // новая запись отмены — прежний повтор недоступен
        break;
      case Change::Load:
        x.headUndo.clear();
        x.headRedo.clear();
        break;
      case Change::Undo:
        if (headLabel(c.label) && !x.headUndo.empty()) {
          x.snapHead = x.headUndo.back().first;
          x.headRedo.push_back(x.headUndo.back());
          x.headUndo.pop_back();
        }
        break;
      case Change::Redo:
        if (headLabel(c.label) && !x.headRedo.empty()) {
          x.snapHead = x.headRedo.back().second;
          x.headUndo.push_back(x.headRedo.back());
          x.headRedo.pop_back();
        }
        break;
    }
  });
}

// Ветвь сменилась транзакцией label (только если она попала в стек отмены).
void headChanged(App& a, std::string_view label, u64 before, u64 after) {
  App::Impl& d = a.impl();
  d.snapHead = after;
  if (a.store.canUndo() && a.store.undoLabel() == label) d.headUndo.push_back({before, after});
}

// Начало текущего хода есть в ветви — иначе текущий мир становится началом хода (мир открыт впервые,
// старая история без снимков начала хода).
void ensureTurnStart(App& a) {
  App::Impl& d = a.impl();
  if (a.ui.screen != Screen::Editor) return;
  const World& w = a.store.world();
  auto all = allSnaps(d);
  for (const Snap* s : chainOf(all, d.snapHead))
    if (s->info.turn == w.turn() && isKind(*s, io::kSnapStart)) return;
  d.snapHead = turnui::keepSnapshot(a, w, io::kSnapStart, "Начало хода " + std::to_string(w.turn()), d.snapHead);
}

void removeMem(App::Impl& d, u64 seq) {
  d.memSnaps.erase(std::remove_if(d.memSnaps.begin(), d.memSnaps.end(), [&](const MemSnapshot& m) { return m.seq == seq; }), d.memSnaps.end());
}

}  // namespace

// ================================================================ завершение хода
void App::endTurn() {
  if (ui.screen != Screen::Editor) return;
  if (readOnly()) {
    toast("Открыт прошлый ход — вернитесь к текущему, чтобы завершить ход", ToastKind::Warning, "lock", "К текущему ходу", [](App& a) { a.backToCurrent(); });
    return;
  }
  if (hasDialog("turn.confirm")) return;
  std::unique_ptr<Dialog> dlg;
  try {
    dlg = turnui::makeConfirm(*this);
  } catch (const std::exception& e) {
    error(e);
    return;
  }
  openDialog(std::move(dlg));
}

bool App::endTurnNow() {
  if (ui.screen != Screen::Editor || readOnly()) return false;
  Impl& d = *d_;
  trackHead(*this);
  World before = store.world();
  int turn = before.turn();
  const u64 oldHead = d.snapHead;
  // Конец хода — мир перед расчётом; снимки в памяти до сохранения мира.
  u64 endSeq = turnui::keepSnapshot(*this, before, io::kSnapEnd, "Ход " + std::to_string(turn), oldHead);
  rules::TurnReport rep;
  bool ok = act("Завершить ход", [&](Tx& tx) { rep = rules::endTurn(tx); });
  if (!ok) {
    removeMem(d, endSeq);
    return false;
  }
  // Начало следующего хода — ровно мир после расчёта: к нему ведёт «Начать ход заново».
  u64 startSeq = turnui::keepSnapshot(*this, store.world(), io::kSnapStart, "Начало хода " + std::to_string(rep.turnTo), endSeq);
  headChanged(*this, "Завершить ход", oldHead, startSeq);
  lastReport_ = rep;
  int debts = 0;
  for (auto& l : rep.factions)
    if (l.treasuryAfter < 0 && l.treasuryBefore >= 0) debts++;
  std::string text = "Начался ход " + std::to_string(rep.turnTo);
  if (!rep.rebellions.empty()) text += " · восстаний: " + std::to_string(rep.rebellions.size());
  if (debts) text += " · в долгу: " + std::to_string(debts);
  bool warn = !rep.rebellions.empty() || debts > 0;
  toast(text, warn ? ToastKind::Warning : ToastKind::Success, "next-turn", "Отчёт", [](App& a) { a.openDialog("turn.report"); });
  return true;
}

// ================================================================ история
std::vector<TurnSnapshot> App::snapshots() const {
  Impl& d = *d_;
  std::vector<TurnSnapshot> out;
  auto all = allSnaps(d);
  int current = store.world().turn();
  std::vector<int> turns;
  for (const Snap& s : all) turns.push_back(s.info.turn);
  std::sort(turns.begin(), turns.end());
  turns.erase(std::unique(turns.begin(), turns.end()), turns.end());
  for (int t : turns) {
    const Snap* s = rowSnap(all, d.snapHead, current, t);
    if (!s) continue;
    out.push_back(TurnSnapshot{t, s->info.label, s->info.at, s->mem != nullptr, snapKey(*s), s->info.kind});
  }
  return out;
}

bool App::viewTurn(int turn) {
  Impl& d = *d_;
  if (ui.screen != Screen::Editor) return false;
  if (turn == store.world().turn() && !ui.viewTurn) return true;
  std::optional<World> w = turnui::loadTurnWorld(*this, turn);
  if (!w) return false;
  viewWorld_ = std::move(*w);
  ui.viewTurn = turn;
  setEditBorders(false);
  setTool(ToolId::Select);
  d.map->setWorld(viewWorld_);
  d.mapGen++;
  if (ui.sel && !selectionExists(viewWorld_, ui.sel)) ui.sel = {};
  ui.hover = {};
  requestRedraw();
  return true;
}

void App::backToCurrent() {
  if (!ui.viewTurn) return;
  ui.viewTurn.reset();
  viewWorld_ = World{};
  d_->map->setWorld(store.world());
  d_->mapGen++;
  if (ui.sel && !selectionExists(store.world(), ui.sel)) ui.sel = {};
  ui.hover = {};
  requestRedraw();
}

// ================================================================ снимки и возврат
namespace turnui {

u64 keepSnapshot(App& a, const World& w, const char* kind, const std::string& label, u64 parent) {
  App::Impl& d = a.impl();
  // Номер — больше любого в истории (диск и память): снимки не заменяют друг друга.
  u64 next = d.snapNext;
  for (const io::SnapshotInfo& s : diskHistory(d).list) next = std::max(next, s.seq + 1);
  for (const MemSnapshot& m : d.memSnaps) next = std::max(next, m.seq + 1);
  MemSnapshot m;
  m.turn = std::max(1, w.turn());
  m.world = w;
  m.label = label;
  m.at = nowIso();
  m.seq = next;
  m.kind = kind;
  m.parent = parent;
  // Сжатие для записи — сразу в фоне (мир неизменяем: таблицы общие, копия дешёвая), чтобы сохранение после
  // многих ходов не сжимало все снимки разом.
  World copy = w;
  m.gz = jobs::submit([copy]() { return io::pack(copy, 6); }).share();
  d.memSnaps.push_back(std::move(m));
  d.snapNext = next + 1;
  return next;
}

std::optional<World> loadTurnWorld(App& a, int turn) {
  App::Impl& d = a.impl();
  auto all = allSnaps(d);
  const Snap* s = rowSnap(all, d.snapHead, a.store.world().turn(), turn);
  if (!s) {
    a.toast("Снимка хода " + std::to_string(turn) + " нет", ToastKind::Warning, "history");
    return std::nullopt;
  }
  try {
    return loadSnap(d, *s);
  } catch (const std::exception& e) {
    a.error(e);
    return std::nullopt;
  }
}

bool rollbackToTurn(App& a, int turn) {
  if (a.ui.screen != Screen::Editor) return false;
  if (a.readOnly()) a.backToCurrent();
  App::Impl& d = a.impl();
  trackHead(a);
  World cur = a.store.world();
  const int now = cur.turn();
  const u64 oldHead = d.snapHead;
  u64 target = 0;
  std::optional<World> w;
  {
    auto all = allSnaps(d);
    const Snap* s = rowSnap(all, oldHead, now, turn);
    if (!s) {
      a.toast("Снимка хода " + std::to_string(turn) + " нет", ToastKind::Warning, "history");
      return false;
    }
    try {
      w = loadSnap(d, *s);
    } catch (const std::exception& e) {
      a.error(e);
      return false;
    }
    target = s->info.seq;
  }
  // Текущее состояние остаётся в истории (своей ветвью): к нему можно вернуться тем же способом.
  u64 branchSeq = 0;
  if (now != turn) branchSeq = keepSnapshot(a, cur, io::kSnapBranch, "Ход " + std::to_string(now) + " · до возврата к ходу " + std::to_string(turn), oldHead);
  a.closeDialogs();
  // Возврат — обычная транзакция: отменяется Ctrl+Z, как любое другое изменение мира.
  const std::string label = "Возврат к ходу " + std::to_string(turn);
  const World targetWorld = std::move(*w);
  if (!a.act(label, [&](Tx& tx) { tx.replaceWorld(targetWorld); })) {
    if (branchSeq) removeMem(d, branchSeq);
    return false;
  }
  headChanged(a, label, oldHead, target);
  ensureTurnStart(a);   // старый снимок без ветви: начало хода — мир после возврата
  if (d.snapHead != target && !d.headUndo.empty() && d.headUndo.back().second == target) d.headUndo.back().second = d.snapHead;
  const u64 newHead = d.snapHead;
  a.toast("Мир возвращён к ходу " + std::to_string(turn), ToastKind::Success, "history", "Отменить", [cur, turn, oldHead, newHead](App& x) {
    if (x.readOnly()) x.backToCurrent();
    const std::string l = "Отмена возврата к ходу " + std::to_string(turn);
    if (x.act(l, [&](Tx& tx) { tx.replaceWorld(cur); })) {
      headChanged(x, l, newHead, oldHead);
      x.toast("Возврат отменён: снова ход " + std::to_string(cur.turn()), ToastKind::Info, "undo");
    }
  });
  return true;
}

void askRollback(App& a, int turn) {
  int now = a.store.world().turn();
  std::string text = turn == now ? "Все изменения этого хода будут отброшены: мир станет таким, каким был в начале хода."
                                 : "Мир станет таким, каким был в ходе " + std::to_string(turn) + ". Текущий ход " + std::to_string(now) +
                                       " сохранится в истории — к нему можно вернуться.";
  a.confirm(turn == now ? "Начать ход " + std::to_string(turn) + " заново?" : "Вернуться к ходу " + std::to_string(turn) + "?", text,
            turn == now ? "Начать заново" : "Вернуться", true, [turn](App& x) { rollbackToTurn(x, turn); });
}

void syncHistory(App& a) {
  App::Impl& d = a.impl();
  trackHead(a);
  d.diskHist.reset();
  d.headUndo.clear();
  d.headRedo.clear();
  const io::History& h = diskHistory(d);
  d.snapNext = 1;
  d.snapHead = 0;
  for (const io::SnapshotInfo& s : h.list) {
    d.snapNext = std::max(d.snapNext, s.seq + 1);
    if (h.head && s.seq == h.head) d.snapHead = h.head;   // ветвь сохранённого мира
  }
  if (!d.snapHead) {
    auto all = allSnaps(d);
    if (const Snap* s = latest(all, a.store.world().turn(), [](const Snap& x) { return isKind(x, io::kSnapStart); })) d.snapHead = s->info.seq;
  }
  ensureTurnStart(a);
}

std::vector<io::NewSnapshot> pendingSnapshots(App& a) {
  App::Impl& d = a.impl();
  std::vector<io::NewSnapshot> out(d.memSnaps.size());
  for (size_t i = 0; i < d.memSnaps.size(); i++) {
    const MemSnapshot& m = d.memSnaps[i];
    io::NewSnapshot& n = out[i];
    n.info.turn = m.turn;
    n.info.seq = m.seq;
    n.info.kind = m.kind;
    n.info.parent = m.parent;
    n.info.at = m.at;   // время создания снимка сохраняется
    n.info.label = m.label;
    n.info.name = m.world.meta->name;
    // Готовое сжатие из фона (ждём здесь, в основном потоке, а не внутри задачи пула).
    if (m.gz.valid()) {
      try {
        n.gz = m.gz.get();
      } catch (const std::exception& e) {
        logWarn("Снимок хода %d: сжатие в фоне не удалось: %s", m.turn, e.what());
      }
    }
  }
  jobs::parallelFor(out.size(), [&](size_t i) {
    if (out[i].gz.empty()) out[i].gz = io::pack(d.memSnaps[i].world, 6);
  }, 1);
  return out;
}

void historySaved(App& a) {
  App::Impl& d = a.impl();
  d.memSnaps.clear();
  d.diskHist.reset();
}

}  // namespace turnui

// ================================================================ встроенные фабрики (app.cpp)
namespace detail {

std::unique_ptr<Dialog> turnConfirmDialog(App& a, rules::TurnReport) { return turnui::makeConfirm(a); }
std::unique_ptr<Dialog> turnReportDialog(App& a, rules::TurnReport report) { return turnui::makeReport(a, report); }
std::unique_ptr<Dialog> historyDialog(App& a) { return turnui::makeHistory(a); }

}  // namespace detail
}  // namespace rg::app
