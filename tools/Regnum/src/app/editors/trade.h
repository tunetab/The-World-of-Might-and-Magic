// Regnum — торговля, подарки, дань и репарации: общее для редактора «Торговля» (editors/trade.cpp), вкладки
// фракции и выдвижной панели (panels/trade.cpp) и диалога дани (dialogs/tribute.cpp). ТЗ 1.b.vii, 1.d.iii, 1.e.ii.
#pragma once
#include "app/app.h"

namespace rg::app::trade {

// ---------------------------------------------------------------- черновик сделки (живёт всю сессию)
struct DraftItem {
  u64 key = 0;                     // устойчивый ID строки интерфейса
  DealSide from = DealSide::A;     // кто передаёт
  Id res = kGold;
  double amount = 100;
  DealMode mode = DealMode::Once;
  int turns = 5;                   // срок для «каждый ход»
};
struct Draft {
  Id a = 0, b = 0;                 // стороны (государства или гильдии)
  std::vector<DraftItem> items;
  u64 nextKey = 1;
  Id highlight = 0;                // сделка, подсвеченная в списке
};
Draft& draft();
void startDraft(Id a, Id b = 0);   // новая сделка со сторонами (позиции очищаются)
DraftItem& addItem(DealSide from); // позиция стороны (золото, 100, разово)
Deal toDeal(const Draft& d);

// ---------------------------------------------------------------- сделки
const char* dealIcon(DealKind k);
ui::Tone dealTone(const Deal& d);
int dealLeft(const Deal& d);       // ходов выплат осталось (максимум по позициям «каждый ход»)
int dealTurns(const Deal& d);      // полный срок выплат
std::string resName(const World& w, Id res);
// Сделки фракции (0 — все): активные сверху (новые первыми), затем завершённые.
std::vector<const Deal*> dealsOf(const World& w, Id faction);

struct CardOpt {
  Id perspective = 0;              // «отдаём / получаем» с точки зрения фракции
  bool clickable = false;          // щелчок по карточке — true из dealCard
  bool highlight = false;
};
// Карточка сделки: стороны, позиции, срок и остаток, состояние, расторжение (с подтверждением).
bool dealCard(App& a, const World& w, const Deal& d, const CardOpt& o = {});
void askCancel(App& a, Id deal);   // подтверждение и rules::cancelDeal

}  // namespace rg::app::trade
