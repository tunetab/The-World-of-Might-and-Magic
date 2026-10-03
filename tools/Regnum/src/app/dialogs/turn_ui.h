// Regnum — общие элементы интерфейса хода, истории ходов и хроники (только для файлов этой области:
// turn.cpp, dialogs/turn_*.cpp, dialogs/history.cpp, panels/chronicle.cpp).
#pragma once
#include <optional>

#include "app/app.h"
#include "core/io.h"

namespace rg::app::turnui {

// ---------------------------------------------------------------- хроника
ui::Tone logTone(LogKind k);              // смысловой тон вида записи
const char* logIcon(LogKind k);
const char* logName(LogKind k);

// Есть ли у записи объект на карте (провинция, войско или фракция).
bool entryFocusable(const World& w, const LogEntry& e);
// Выделить и показать объект записи: провинция → войско → первая фракция.
void focusEntry(App& a, const LogEntry& e);

struct LogRowOpt {
  bool clickable = true;                  // наведение и щелчок (если есть объект)
  bool showTime = true;                   // реальное время записи справа
  bool showTurn = false;                  // «ход N» в строке ссылок
  int maxLines = 4;
};
// Строка записи: значок вида, текст с переносом, ссылки (фракции, провинция), время. true — щелчок.
bool logRow(const World& w, const LogEntry& e, const LogRowOpt& o = {});

// ---------------------------------------------------------------- мелкие элементы
// Отпечаток сеанса (приложение и мир): сменился — состояние панелей (фильтры, черновики) сбрасывается.
u64 sessionTag(App& a);
void iconTile(RectF r, const char* icon, ui::Tone tone);        // значок на тонированной подложке
void factionLabel(const World& w, Id faction, float h = 24);    // флаг и название в строке (ячейка таблицы)
ui::Ink deltaInk(double v);
std::string money(double v);                                    // число без лишних знаков
// Слот текущей ячейки таблицы для своего рисования (занимает ячейку).
RectF cellRect(ui::Table& t);
// Полосы дохода (сверху) и расхода (снизу) в прямоугольнике; scale — общий максимум.
void flowBars(RectF r, double income, double expense, double scale);
// Разбор дохода (income) или расхода фракции для подсказки (строки через перевод строки).
std::string flowText(const rules::FactionCalc& fc, bool income);

// Строки итога хода: государства, затем гильдии, по названию.
std::vector<const rules::TurnFactionLine*> sortedLines(const World& w, const rules::TurnReport& rep);

// Записи хроники хода по смыслу (w — мир после хода, где есть записи rep.logIds).
struct TurnDigest {
  std::vector<const LogEntry*> all;                          // все записи хода, кроме итога
  std::vector<const LogEntry*> builds, techs, deals, other;  // события
  std::vector<const LogEntry*> debts, shortfalls, rebellions;  // предупреждения
  const LogEntry* summary = nullptr;                         // итог хода (LogKind::Turn)
  int warnings() const { return int(debts.size() + shortfalls.size() + rebellions.size()); }
  int events() const { return int(builds.size() + techs.size() + deals.size() + other.size()); }
};
TurnDigest digest(const World& w, const rules::TurnReport& rep);

// Прокрутка высотой по содержимому прошлого кадра, но не выше maxH (модальные окна).
class FitScroll {
 public:
  FitScroll(std::string_view id, float maxH, float minH = 40);
  ~FitScroll();
  FitScroll(const FitScroll&) = delete;
  FitScroll& operator=(const FitScroll&) = delete;

 private:
  float* h_ = nullptr;
  float y0_ = 0;
  std::optional<ui::Scroll> sc_;
};

// ---------------------------------------------------------------- ход и история
// Снимки уникальны (ход + вид + номер) и не заменяют друг друга: начало хода (io::kSnapStart), конец хода
// перед расчётом (kSnapEnd), состояние перед возвратом (kSnapBranch). Ветвь текущего мира — цепочка parent
// от App::Impl::snapHead. Новые снимки живут в памяти до сохранения мира (pendingSnapshots).
// Запомнить мир как снимок вида kind (в памяти до сохранения); возвращает номер снимка.
u64 keepSnapshot(App& a, const World& w, const char* kind, const std::string& label, u64 parent);
// Мир снимка, который открывает строка хода turn в истории; nullopt — снимка нет (уведомление показано).
std::optional<World> loadTurnWorld(App& a, int turn);
// Вернуть мир к снимку хода: подтверждение, текущее состояние — в историю, затем замена мира.
// Ход turn — текущий: мир становится ровно таким, каким был в начале этого хода в текущей ветви.
void askRollback(App& a, int turn);
bool rollbackToTurn(App& a, int turn);
// После открытия или создания мира: история с диска, ветвь сохранённого мира, снимок начала текущего хода.
void syncHistory(App& a);
// Несохранённые снимки (сжатые) для записи при сохранении и номер ветви; historySaved — после записи.
std::vector<io::NewSnapshot> pendingSnapshots(App& a);
void historySaved(App& a);

// Хроника: открыть панель с фильтром по ходу (0 — без фильтра).
void showChronicle(App& a, int turn);

// Диалоги области.
std::unique_ptr<Dialog> makeConfirm(App& a);
std::unique_ptr<Dialog> makeReport(App& a, const rules::TurnReport& rep);
std::unique_ptr<Dialog> makeHistory(App& a);

}  // namespace rg::app::turnui
