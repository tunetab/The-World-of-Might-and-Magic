// Regnum — подтверждение завершения хода с предварительным итогом: пробный ход (rules::endTurn на черновике,
// мир не меняется) — казна каждой фракции до и после с доходом и расходом, изменения запасов, стройки и
// исследования, которые завершатся, истекающие сделки и выплаты, предупреждения (долги, недостачи, восстания).
#include "app/app_internal.h"
#include "app/dialogs/turn_ui.h"
#include "app/widgets.h"

namespace rg::app::turnui {

namespace {

struct Preview {
  rules::TurnReport rep;
  World before, after;                       // мир до и после пробного хода (записи хроники — в after)
  std::shared_ptr<const rules::Calc> calc;   // расчёт на начало хода (разбор доходов)
  std::vector<const rules::TurnFactionLine*> lines;
  TurnDigest dg;
  double scale = 1;                          // общий максимум дохода и расхода (полосы)
  int up = 0, down = 0, resRows = 0;
};

std::unique_ptr<Preview> makePreview(const World& w) {
  auto p = std::make_unique<Preview>();
  p->before = w;
  {
    Tx tx(w);   // черновик: исходный мир неизменяем
    p->rep = rules::endTurn(tx);
    p->after = std::move(tx).finish();
  }
  p->calc = rules::calc(w);
  p->lines = sortedLines(w, p->rep);
  p->dg = digest(p->after, p->rep);
  for (auto* l : p->lines) {
    p->scale = std::max({p->scale, l->income, l->expenses});
    double d = l->treasuryAfter - l->treasuryBefore;
    if (d > 0.5) p->up++;
    if (d < -0.5) p->down++;
    if (!l->resources.empty()) p->resRows++;
  }
  return p;
}

// «Ход N → Ход N+1»
void turnArrow(int from, int to) {
  ui::HStack hs(26, ui::Align::Left, 8);
  ui::tag("Ход " + std::to_string(from), ui::Tone::Neutral, "hourglass");
  ui::icon("arrow-right", ui::Ink::Muted, 16);
  ui::tag("Ход " + std::to_string(to), ui::Tone::Accent, "next-turn");
  ui::flex();
  ui::label("Мир до хода сохранится в истории · отмена Ctrl+Z", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
}

// Строка «значок ресурса + изменение» (в ячейке).
void resourceDeltas(const World& w, const std::map<Id, double>& res) {
  ui::HStack hs(24, ui::Align::Left, 14);
  for (auto& [rid, v] : res) {
    ui::IdScope s{i64(rid)};
    const CatalogItem* c = w.resource(rid);
    ui::iconColored(w::resourceIcon(w, rid), w::resourceColor(w, rid), 16, c ? std::string_view(c->name) : std::string_view("Ресурс"));
    ui::label(fmtSigned(v, std::fabs(v - std::round(v)) > 1e-9 ? 1 : 0), {.ink = deltaInk(v)});
  }
}

void groupHeader(const char* icon, ui::Tone tone, std::string_view title, size_t n) {
  ui::HStack hs(26, ui::Align::Left, 8);
  RectF r = ui::next(22, 26);
  iconTile(RectF{r.x, r.cy() - 11, 22, 22}, icon, tone);
  ui::label(title, {.font = ui::Font::Strong});
  ui::badge(std::to_string(n), ui::Tone::Neutral);
}

struct ConfirmDlg : Dialog {
  std::unique_ptr<Preview> p;
  int tab = 0;
  const char* id() const override { return "turn.confirm"; }
  Style style(App& a) override {
    Style s;
    s.title = "Завершить ход " + std::to_string(a.store.world().turn()) + "?";
    s.icon = "next-turn";
    s.width = 800;
    return s;
  }

  float bodyH() const { return std::round(clamp(ui::viewport().h - 430, 160.f, 320.f)); }

  void treasuryTab(App&) {
    const World& w = p->before;
    ui::Column cols[] = {{"Фракция", nullptr, ui::fr(1.7f)},
                         {"Доход", "income", ui::fr(1), ui::Align::Right},
                         {"Расход", "expense", ui::fr(1), ui::Align::Right},
                         {"Казна", "treasury", ui::fr(1.5f), ui::Align::Right},
                         {"Изменение", nullptr, ui::fr(0.9f), ui::Align::Right}};
    ui::Table t("treasury", cols, int(p->lines.size()), {.rowHeight = 40, .height = bodyH(), .selectable = false, .emptyIcon = "crown", .emptyText = "Фракций пока нет"});
    for (int i : t) {
      const rules::TurnFactionLine& l = *p->lines[size_t(i)];
      const rules::FactionCalc* fc = p->calc->faction(l.faction);
      t.cell();
      factionLabel(w, l.faction);
      for (int k = 0; k < 2; k++) {
        double v = k == 0 ? l.income : l.expenses;
        RectF cr = cellRect(t);
        std::string tip = fc ? flowText(*fc, k == 0) : std::string();
        ui::at(RectF{cr.x, std::round(cr.cy() - 12), cr.w, 18});
        ui::label(money(v), {.ink = ui::Ink::Dim, .align = ui::Align::Right, .tooltip = tip});
        RectF br{cr.x + cr.w * 0.25f, std::round(cr.cy() + 8), cr.w * 0.75f, 4};
        float bw = float(clamp(v / std::max(1e-9, p->scale), 0.0, 1.0)) * br.w;
        if (v > 0) bw = std::max(bw, 2.f);
        const ui::Theme& th = ui::theme();
        ui::draw::rect(br, th.track, 2);
        if (bw > 0) ui::draw::rect(RectF{br.right() - bw, br.y, bw, br.h}, k == 0 ? th.success : th.danger, 2);
      }
      t.cell();
      {
        ui::HStack hs(24, ui::Align::Right, 6);
        ui::label(money(l.treasuryBefore), {.font = ui::Font::Small, .ink = ui::Ink::Muted});
        ui::icon("arrow-right", ui::Ink::Muted, 14);
        ui::label(money(l.treasuryAfter), {.font = ui::Font::Strong, .ink = l.treasuryAfter < 0 ? ui::Ink::Danger : ui::Ink::Normal});
      }
      double d = l.treasuryAfter - l.treasuryBefore;
      t.text(fmtSigned(d), deltaInk(d));
    }
  }

  void resourcesTab(App&) {
    const World& w = p->before;
    std::vector<const rules::TurnFactionLine*> rows;
    for (auto* l : p->lines)
      if (!l->resources.empty()) rows.push_back(l);
    ui::Column cols[] = {{"Фракция", nullptr, ui::fr(1.2f)}, {"Изменение запасов", "resource", ui::fr(2.2f)}};
    ui::Table t("resources", cols, int(rows.size()),
                {.rowHeight = 40, .height = bodyH(), .selectable = false, .emptyIcon = "resource", .emptyText = "Запасы ресурсов не изменятся"});
    for (int i : t) {
      const rules::TurnFactionLine& l = *rows[size_t(i)];
      t.cell();
      factionLabel(w, l.faction);
      t.cell();
      resourceDeltas(w, l.resources);
    }
  }

  void eventsTab(App&) {
    const TurnDigest& d = p->dg;
    if (d.events() == 0) {
      RectF r = ui::next(bodyH());
      ui::Area ar(RectF{r.x, r.y + r.h * 0.5f - 70, r.w, 140}, 0);
      ui::emptyState("hourglass", "В этом ходу ничего не завершится.");
      return;
    }
    ui::Scroll sc("events", bodyH());
    auto group = [&](const char* icon, ui::Tone tone, std::string_view title, const std::vector<const LogEntry*>& list) {
      if (list.empty()) return;
      ui::IdScope s(title);
      groupHeader(icon, tone, title, list.size());
      for (const LogEntry* e : list) logRow(p->after, *e, {.clickable = false, .showTime = false, .maxLines = 3});
      ui::spacer(4);
    };
    group("build", ui::Tone::Info, "Стройки завершатся", d.builds);
    group("research", ui::Tone::Info, "Исследования", d.techs);
    group("handshake", ui::Tone::Accent, "Сделки и выплаты завершатся", d.deals);
    group("list", ui::Tone::Neutral, "Прочее", d.other);
  }

  void warningsTab(App&) {
    const TurnDigest& d = p->dg;
    if (d.warnings() == 0) {
      RectF r = ui::next(bodyH());
      ui::Area ar(RectF{r.x, r.y + r.h * 0.5f - 70, r.w, 140}, 0);
      ui::emptyState("check-circle", "Всё спокойно: долгов, недостач и восстаний не будет.");
      return;
    }
    ui::Scroll sc("warnings", bodyH());
    auto group = [&](const char* icon, ui::Tone tone, std::string_view title, const std::vector<const LogEntry*>& list) {
      if (list.empty()) return;
      ui::IdScope s(title);
      groupHeader(icon, tone, title, list.size());
      for (const LogEntry* e : list) logRow(p->after, *e, {.clickable = false, .showTime = false, .maxLines = 3});
      ui::spacer(4);
    };
    group("rebellion", ui::Tone::Danger, "Восстания", d.rebellions);
    group("treasury", ui::Tone::Danger, "Долги казны", d.debts);
    group("warning", ui::Tone::Warning, "Недостачи по сделкам", d.shortfalls);
  }

  bool draw(App& a) override {
    if (!p) return false;
    const TurnDigest& d = p->dg;
    turnArrow(p->rep.turnFrom, p->rep.turnTo);
    {
      ui::Row r({ui::fr(1), ui::fr(1), ui::fr(1), ui::fr(1)}, 64, 10);
      ui::stat(std::to_string(p->lines.size()), "Фракций", {.icon = "crown", .tone = ui::Tone::Accent});
      ui::stat(std::to_string(p->up), "Казна растёт", {.icon = "trend-up", .tone = ui::Tone::Success});
      ui::stat(std::to_string(p->down), "Казна убывает", {.icon = "trend-down", .tone = p->down ? ui::Tone::Warning : ui::Tone::Neutral});
      ui::stat(std::to_string(d.events()), "Завершится", {.icon = "check-circle", .tone = ui::Tone::Info});
    }
    if (d.warnings() > 0) {
      std::vector<std::string> parts;
      if (!d.debts.empty()) parts.push_back("долги казны: " + std::to_string(d.debts.size()));
      if (!d.shortfalls.empty()) parts.push_back("недостачи: " + std::to_string(d.shortfalls.size()));
      if (!d.rebellions.empty()) parts.push_back("восстания: " + std::to_string(d.rebellions.size()));
      const ui::Theme& th = ui::theme();
      RectF r = ui::next(40);
      ui::draw::rect(r, th.warning.alpha(th.dark ? 0.10f : 0.12f), th.radiusCard);
      ui::draw::rectStroke(r, th.warning.alpha(0.35f), th.radiusCard, 1);
      ui::draw::icon("warning", RectF{r.x + 12, r.cy() - 9, 18, 18}, th.warning);
      ui::draw::text("Внимание · " + join(parts, " · "), RectF{r.x + 40, r.y, r.w - 170, r.h}, ui::Font::Strong, th.text);
      ui::at(RectF{r.right() - 120, r.cy() - 12, 110, 24});
      if (ui::button("Подробнее", {.variant = ui::Variant::Ghost, .size = ui::Size::Small, .iconRight = "chevron-right"})) tab = 3;
      a.markUi("turn.confirm.warnings");
    }
    ui::tabs("tabs", tab,
             {{"treasury", "Казна"},
              {"resource", "Ресурсы", {}, p->resRows},
              {"check-circle", "События", {}, d.events()},
              {"warning", "Внимание", {}, d.warnings(), ui::Tone::Danger}},
             {.style = ui::TabStyle::Pill, .fill = true});
    a.markUi("turn.confirm.tabs");
    switch (tab) {
      case 1: resourcesTab(a); break;
      case 2: eventsTab(a); break;
      case 3: warningsTab(a); break;
      default: treasuryTab(a); break;
    }
    if (a.store.world().settings->rebellionRoll)
      ui::label("Бросок восстаний включён: итог восстаний предсказан по зерну хода.", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "dice"});
    ui::ModalFooter f;
    if (ui::button("Отмена")) return false;
    a.markUi("dialog.cancel");
    if (ui::button("Завершить ход", {.variant = ui::Variant::Primary, .icon = "next-turn", .isDefault = true})) {
      detail::later(a, [](App& x) { x.endTurnNow(); });
      return false;
    }
    a.markUi("dialog.ok");
    return true;
  }
};

}  // namespace

std::unique_ptr<Dialog> makeConfirm(App& a) {
  auto d = std::make_unique<ConfirmDlg>();
  d->p = makePreview(a.store.world());
  return d;
}

}  // namespace rg::app::turnui
