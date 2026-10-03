// Regnum — аудит устойчивости (ui-fuzz): точные источники предупреждений «повторяющийся ID виджета».
// Каждый тест воспроизводит одно место; после исправления — регрессионные проверки (повторов ID нет, кнопки работают).
#include <cstdio>

#include "rules/rules.h"
#include "tests/test_app_util.h"

using namespace rg;
using namespace rg::apptest;
using app::SelType;

namespace {

// Новые строки WARN/ERROR журнала.
struct DupTap {
  std::string path;
  size_t off = 0;
  explicit DupTap(const std::string& name) : path(fs::join(test::outDir(), "app-tmp/dup-" + name + ".log")) {
    fs::remove(path);
    setLogFile(path);
  }
  ~DupTap() { setLogFile(""); }
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
      if (line.find("повторяющийся ID") != std::string::npos) out.push_back(line.substr(line.find("] ") + 2));
      p = q + 1;
    }
    return out;
  }
};

std::string pctText(double v) { return fmtPct(v, std::fabs(v - std::round(v)) > 1e-6 ? 1 : 0); }

}  // namespace

// Вкладка «Экономика» провинции: подписи «Налог государства» и «Общий налог» — ui::label с подсказкой, ID = текст.
// При местном налоге 0 % тексты совпадают («10 %» и «10 %») → один ID на два элемента.
TEST(app_audit_fuzz_dup_province_tax) {
  Harness h("fuzz_dup_tax");
  h.demo();
  h.waitMap();
  DupTap tap("tax");
  auto calc = rules::calc(h->world());
  int same = 0, sameWarn = 0, diff = 0, diffWarn = 0;
  std::vector<Id> ids;
  h->world().provinces.each([&](const Province& p) {
    if (!p.sea) ids.push_back(p.id);
  });
  for (Id pid : ids) {
    const rules::ProvinceCalc* pc = calc->province(pid);
    if (!pc) continue;
    h->select(SelType::Province, pid);
    h->ui.tabOf[SelType::Province] = "province.economy";
    h.settle();
    auto w = tap.fresh();
    bool eq = pctText(pc->taxState) == pctText(pc->taxTotal);
    (eq ? same : diff)++;
    if (!w.empty()) (eq ? sameWarn : diffWarn)++;
    if (!w.empty() && (sameWarn + diffWarn) <= 3)
      std::printf("  [ui-fuzz dup] провинция %u: налог государства «%s», общий «%s» → %s\n", unsigned(pid), pctText(pc->taxState).c_str(),
                  pctText(pc->taxTotal).c_str(), w[0].c_str());
  }
  std::printf("  [ui-fuzz dup] экономика провинций: тексты налогов совпадают в %d (предупреждений %d), различаются в %d (предупреждений %d)\n", same,
              sameWarn, diff, diffWarn);
  CHECK_EQ(sameWarn + diffWarn, 0);
}

// Вкладка фракции «Торговля» и выдвижная панель «Торговля»: повтор ID в карточках сделок.
TEST(app_audit_fuzz_dup_trade) {
  Harness h("fuzz_dup_trade");
  h.demo();
  h.waitMap();
  DupTap tap("trade");
  int warns = 0;
  for (Id fid : h->world().factions.ids()) {
    h->select(SelType::Faction, fid);
    h->ui.tabOf[SelType::Faction] = "faction.trade";
    h.settle();
    if (const RectF* r = h->uiRect("inspector"))
      for (int k = 0; k < 3; k++) h.wheel(r->cx(), r->y + r->h * 0.6f, -6);
    for (auto& m : tap.fresh()) {
      warns++;
      std::printf("  [ui-fuzz dup] вкладка «Торговля» фракции %u (%s): %s\n", unsigned(fid), h->world().factionName(fid).c_str(), m.c_str());
      h.shot("fuzz_dup_trade_" + std::to_string(fid));
    }
  }
  h->clearSelection();
  h->openDrawer("trade");
  h.settle();
  for (auto& m : tap.fresh()) {
    warns++;
    std::printf("  [ui-fuzz dup] панель «Торговля»: %s\n", m.c_str());
  }
  std::printf("  [ui-fuzz dup] торговля: предупреждений %d\n", warns);
  CHECK_EQ(warns, 0);
}

// Следствие повтора ID на вкладке «Торговля» фракции без сделок: верхняя кнопка «Новая сделка» и кнопка пустого
// состояния «Новая сделка» имеют один ID. Нажатие на нижнюю: при отпускании первая (верхняя) кнопка с тем же ID
// снимает «активный» элемент раньше, щелчок теряется — редактор сделки не открывается.
TEST(app_audit_fuzz_dup_trade_empty_click) {
  Harness h("fuzz_dup_trade_click");
  h.demo();
  h.waitMap();
  Id fid = 0;
  for (Id f : h->world().factions.ids()) {
    bool any = false;
    h->world().deals.each([&](const Deal& d) {
      if (d.a == f || d.b == f) any = true;
    });
    if (!any && !fid) fid = f;
  }
  CHECK(fid != 0);
  h->select(SelType::Faction, fid);
  h->ui.tabOf[SelType::Faction] = "faction.trade";
  h.settle();
  const RectF* top = h->uiRect("faction.trade.new");
  CHECK(top != nullptr);
  if (!top) return;
  RectF t = *top;
  // Кнопка пустого состояния — под значком и подписью «Сделок пока нет.» по центру вкладки; ищем её по пикселям
  // не надёжно, поэтому перебираем точки по центральной оси ниже верхней кнопки.
  const RectF* ins = h->uiRect("inspector");
  CHECK(ins != nullptr);
  if (!ins) return;
  const RectF insR = *ins;   // копия: указатель на прямоугольник живёт только до следующего кадра
  bool opened = false;
  float hitY = -1;
  for (float y = t.bottom() + 120; y < t.bottom() + 220 && !opened; y += 6) {
    h->closeEditor();
    h.settle();
    h->select(SelType::Faction, fid);
    h->ui.tabOf[SelType::Faction] = "faction.trade";
    h.settle();
    h.click(insR.cx(), y);
    if (h->ui.editor == "trade") {
      opened = true;
      hitY = y;
    }
  }
  // Контроль: верхняя кнопка работает.
  h->closeEditor();
  h.settle();
  h->select(SelType::Faction, fid);
  h->ui.tabOf[SelType::Faction] = "faction.trade";
  h.settle();
  h.shot("fuzz_dup_trade_empty");
  h.click(t.cx(), t.cy());
  bool topOk = h->ui.editor == "trade";
  std::printf("  [ui-fuzz dup] фракция %u без сделок: верхняя «Новая сделка» %s; кнопка пустого состояния %s (y=%.0f)\n", unsigned(fid),
              topOk ? "открывает редактор" : "НЕ открывает", opened ? "открывает редактор" : "НЕ открывает редактор ни в одной точке", double(hitY));
  CHECK(topOk);
  CHECK(opened);
}

// То же в выдвижной панели «Торговля»: когда активных сделок нет, кнопка пустого состояния «Новая сделка» имеет
// ID верхней кнопки «Новая сделка» — щелчок по ней не открывает редактор.
TEST(app_audit_fuzz_dup_trade_drawer_empty_click) {
  Harness h("fuzz_dup_trade_drawer");
  h.demo();
  h.waitMap();
  std::vector<Id> deals = h->world().deals.ids();
  h->act("Расторгнуть всё", [&](Tx& tx) {
    for (Id d : deals) rules::cancelDeal(tx, d);
  });
  h.settle();
  DupTap tap("trade_drawer");
  h->openDrawer("trade");
  h.settle();
  auto warns = tap.fresh();
  const RectF* top = h->uiRect("trade.drawer.new");
  const RectF* dr = h->uiRect("drawer");
  CHECK(top && dr);
  if (!top || !dr) return;
  RectF t = *top, d = *dr;
  bool opened = false;
  for (float y = t.bottom() + 100; y < t.bottom() + 300 && !opened; y += 6) {
    h->closeEditor();
    h.settle();
    if (h->ui.drawer != "trade") h->openDrawer("trade");
    h.settle();
    h.click(d.cx(), y);
    if (h->ui.editor == "trade") opened = true;
  }
  h->closeEditor();
  h.settle();
  if (h->ui.drawer != "trade") h->openDrawer("trade");
  h.settle();
  h.shot("fuzz_dup_trade_drawer_empty");
  h.click(t.cx(), t.cy());
  bool topOk = h->ui.editor == "trade";
  std::printf("  [ui-fuzz dup] панель «Торговля» без активных сделок: предупреждений %d%s; верхняя кнопка %s; кнопка пустого состояния %s\n",
              int(warns.size()), warns.empty() ? "" : (" (" + warns[0] + ")").c_str(), topOk ? "работает" : "НЕ работает",
              opened ? "работает" : "НЕ работает");
  CHECK(topOk);
  CHECK(opened);
  CHECK(warns.empty());
}
