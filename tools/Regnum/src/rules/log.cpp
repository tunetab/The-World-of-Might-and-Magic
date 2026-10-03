// Regnum — хроника мира.
#include "rules/internal.h"

namespace rg::rules {

using namespace detail;

Id addLog(Tx& tx, LogKind kind, const std::string& text, const LogRefs& refs) {
  if (int(kind) < 0 || kind >= LogKind::Count) fail("Неизвестный вид записи хроники");
  std::string t = trim(text);
  if (t.empty()) fail("Запись хроники не может быть пустой");
  LogEntry e;
  e.turn = tx.w().turn();
  e.kind = kind;
  e.text = std::move(t);
  e.province = refs.province;
  e.army = refs.army;
  for (Id f : refs.factions)
    if (f && !contains(e.factions, f)) e.factions.push_back(f);
  e.at = nowIso();
  return tx.add(std::move(e)).id;
}

}  // namespace rg::rules
