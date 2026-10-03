// Regnum — отчёт о завершённом ходе (DialogReg «turn.report»): ход N → N+1, карточки фракций с изменением
// казны, доходом и расходом, изменения запасов; записи хроники хода по видам (щелчок — показать на карте);
// предупреждения: долги, недостачи по сделкам, восстания.
#include "app/app_internal.h"
#include "app/dialogs/turn_ui.h"
#include "app/widgets.h"

namespace rg::app::turnui {

namespace {

// Порядок видов в отчёте: сначала тревожное, затем экономика и события.
constexpr LogKind kOrder[] = {LogKind::Province, LogKind::Economy, LogKind::Trade, LogKind::Diplomacy, LogKind::Build, LogKind::Tech,
                              LogKind::War, LogKind::Battle, LogKind::Army, LogKind::Fleet, LogKind::Guild, LogKind::Population, LogKind::Note};

void kindHeader(LogKind k, size_t n) {
  ui::HStack hs(26, ui::Align::Left, 8);
  RectF r = ui::next(22, 26);
  iconTile(RectF{r.x, r.cy() - 11, 22, 22}, logIcon(k), logTone(k));
  ui::label(logName(k), {.font = ui::Font::Strong});
  ui::badge(std::to_string(n), ui::Tone::Neutral);
}

struct ReportDlg : Dialog {
  rules::TurnReport rep;
  const char* id() const override { return "turn.report"; }
  Style style(App&) override {
    Style s;
    s.title = "Итоги хода " + std::to_string(rep.turnFrom);
    s.icon = "scroll";
    s.width = 940;
    s.dismissOnBackdrop = true;
    return s;
  }

  // Карточка фракции: флаг, название, изменение казны, казна после, полосы дохода и расхода, запасы.
  // true — щелчок (открыть фракцию).
  bool factionCard(const World& w, const rules::TurnFactionLine& l, double scale) {
    const Faction* f = w.faction(l.faction);
    if (!f) return false;
    const ui::Theme& th = ui::theme();
    ui::IdScope s{i64(l.faction)};
    double d = l.treasuryAfter - l.treasuryBefore;
    {
      ui::Card c({.pad = 10});
      {
        ui::Row r({ui::px(24), ui::fr(1), ui::px(78)}, 22, 8);
        if (f->isState()) {
          ui::flag(f->flag, 24, 16, 3);
        } else {
          RectF dr = ui::next(22);
          ui::draw::rect(RectF{dr.x, dr.cy() - 8, 24, 16}, f->color.alpha(0.22f), 3);
          ui::draw::icon("guild", RectF{dr.x + 6, dr.cy() - 6, 12, 12}, f->color);
        }
        ui::label(f->name.empty() ? std::string("Без названия") : f->name, {.font = ui::Font::Strong});
        ui::label(fmtSigned(d), {.font = ui::Font::Small, .ink = deltaInk(d), .align = ui::Align::Right,
                                 .icon = d > 0.5 ? "trend-up" : d < -0.5 ? "trend-down" : nullptr});
      }
      {
        ui::Row r({ui::fr(1), ui::px(96)}, 30, 10);
        ui::label(money(l.treasuryAfter), {.font = ui::Font::Number, .ink = l.treasuryAfter < 0 ? ui::Ink::Danger : ui::Ink::Normal,
                                           .tooltip = "Казна после хода; была " + money(l.treasuryBefore)});
        RectF br = ui::next(30);
        flowBars(br, l.income, l.expenses, scale);
      }
      if (!l.resources.empty()) {
        ui::HStack hs(18, ui::Align::Left, 10);
        int shown = 0;
        for (auto& [rid, v] : l.resources) {
          if (shown++ >= 3) {
            ui::label("ещё " + std::to_string(l.resources.size() - 3), {.font = ui::Font::Small, .ink = ui::Ink::Muted});
            break;
          }
          ui::IdScope rs{i64(rid)};
          const CatalogItem* ci = w.resource(rid);
          ui::iconColored(w::resourceIcon(w, rid), w::resourceColor(w, rid), 14, ci ? std::string_view(ci->name) : std::string_view("Ресурс"));
          ui::label(fmtSigned(v, std::fabs(v - std::round(v)) > 1e-9 ? 1 : 0), {.font = ui::Font::Small, .ink = deltaInk(v)});
        }
      }
    }
    RectF cr = ui::lastItem().rect;
    ui::WidgetId wid = ui::id("##card");
    ui::Interaction it = ui::interact(wid, cr, ui::IfAllowOverlap);
    float hv = ui::animate(wid ^ 0xca7dull, it.hovered ? 1.f : 0.f);
    if (hv > 0.01f) ui::draw::rectStroke(cr, th.accent.alpha(0.6f * hv), th.radiusCard, 1.5f);
    if (it.hovered) ui::setCursor(platform::Cursor::Hand);
    return it.clicked;
  }

  bool draw(App& a) override {
    const World& w = a.store.world();
    TurnDigest d = digest(w, rep);
    {
      ui::HStack hs(26, ui::Align::Left, 8);
      ui::tag("Ход " + std::to_string(rep.turnFrom), ui::Tone::Neutral, "hourglass");
      ui::icon("arrow-right", ui::Ink::Muted, 16);
      ui::tag("Ход " + std::to_string(rep.turnTo), ui::Tone::Accent, "next-turn");
      ui::flex();
      ui::label("Щелчок по записи — показать на карте", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "target"});
    }
    {
      ui::Row r({ui::fr(1), ui::fr(1), ui::fr(1), ui::fr(1)}, 64, 10);
      ui::stat(std::to_string(d.all.size()), "Записей хроники", {.icon = "chronicle", .tone = ui::Tone::Info});
      ui::stat(std::to_string(d.builds.size()), "Достроено", {.icon = "build", .tone = ui::Tone::Info});
      ui::stat(std::to_string(d.techs.size()), "Исследования", {.icon = "research", .tone = ui::Tone::Info});
      ui::stat(std::to_string(rep.rebellions.size()), "Восстаний", {.icon = "rebellion", .tone = rep.rebellions.empty() ? ui::Tone::Neutral : ui::Tone::Danger});
    }
    Id pickFaction = 0;
    const LogEntry* pickEntry = nullptr;
    {
      FitScroll body("body", std::max(200.f, ui::viewport().h - 330));
      // Предупреждения
      std::vector<const rules::TurnFactionLine*> lines = sortedLines(w, rep);
      std::vector<const rules::TurnFactionLine*> debtors;
      for (auto* l : lines)
        if (l->treasuryAfter < 0) debtors.push_back(l);
      if (!debtors.empty() || !d.shortfalls.empty() || !d.rebellions.empty()) {
        ui::Card c({.pad = 12, .icon = "warning", .title = "Требует внимания", .tone = ui::Tone::Danger});
        if (!debtors.empty()) {
          ui::label("Казна в долгу: " + std::to_string(debtors.size()) + " " + plural(i64(debtors.size()), "фракция", "фракции", "фракций"),
                    {.font = ui::Font::Small, .ink = ui::Ink::Dim, .icon = "treasury"});
          ui::Row r({ui::fr(1), ui::fr(1), ui::fr(1)}, 26, 6);
          for (auto* l : debtors) {
            const Faction* f = w.faction(l->faction);
            if (!f) continue;
            ui::IdScope s{i64(l->faction)};
            std::string text = (f->name.empty() ? std::string("Без названия") : f->name) + " · " + money(l->treasuryAfter);
            if (ui::chip(text, {.color = f->color, .tone = ui::Tone::Danger, .clickable = true, .tooltip = "Открыть фракцию"}) == ui::ChipAction::Click)
              pickFaction = l->faction;
          }
        }
        for (const LogEntry* e : d.rebellions)
          if (logRow(w, *e, {.showTime = false, .maxLines = 3})) pickEntry = e;
        for (const LogEntry* e : d.shortfalls)
          if (logRow(w, *e, {.showTime = false, .maxLines = 3})) pickEntry = e;
      }
      // Казна фракций
      if (!lines.empty()) {
        ui::caption("Казна фракций");
        double scale = 1;
        for (auto* l : lines) scale = std::max({scale, l->income, l->expenses});
        ui::Row r({ui::fr(1), ui::fr(1), ui::fr(1)}, ui::kAuto, 10);
        for (auto* l : lines)
          if (factionCard(w, *l, scale)) pickFaction = l->faction;
      }
      // События по видам
      ui::caption("События хода");
      bool any = false;
      for (LogKind k : kOrder) {
        size_t n = 0;
        for (const LogEntry* e : d.all)
          if (e->kind == k) n++;
        if (!n) continue;
        any = true;
        ui::IdScope s{int(k)};
        kindHeader(k, n);
        for (const LogEntry* e : d.all)
          if (e->kind == k && logRow(w, *e, {.showTime = false, .maxLines = 3})) pickEntry = e;
        ui::spacer(2);
      }
      if (!any) ui::emptyState("hourglass", "За ход ничего не произошло.");
    }
    a.markUi("turn.report.body");
    if (pickFaction) {
      detail::later(a, [pickFaction](App& x) { x.select(SelType::Faction, pickFaction, true); });
      return false;
    }
    if (pickEntry) {
      LogEntry e = *pickEntry;
      detail::later(a, [e](App& x) { focusEntry(x, e); });
      return false;
    }
    ui::ModalFooter f;
    if (ui::button("Хроника хода", {.icon = "chronicle"})) {
      int t = rep.turnFrom;
      detail::later(a, [t](App& x) { showChronicle(x, t); });
      return false;
    }
    a.markUi("turn.report.chronicle");
    if (ui::button("История ходов", {.icon = "history"})) {
      detail::later(a, [](App& x) { x.openDialog("turn.history"); });
      return false;
    }
    a.markUi("turn.report.history");
    if (ui::button("Закрыть", {.variant = ui::Variant::Primary, .isDefault = true})) return false;
    a.markUi("dialog.ok");
    return true;
  }
};

std::unique_ptr<Dialog> makeRegistered(App& a, Id) {
  const auto& r = a.lastTurnReport();
  if (!r) {
    a.toast("Ход ещё не завершался", ToastKind::Info, "scroll");
    return nullptr;
  }
  if (r->turnTo != a.store.world().turn()) {
    a.toast("Отчёт устарел: ход отменён или мир возвращён к другому ходу", ToastKind::Info, "scroll");
    return nullptr;
  }
  return makeReport(a, *r);
}

DialogReg reg({"turn.report", makeRegistered});

}  // namespace

std::unique_ptr<Dialog> makeReport(App&, const rules::TurnReport& rep) {
  auto d = std::make_unique<ReportDlg>();
  d->rep = rep;
  return d;
}

}  // namespace rg::app::turnui
