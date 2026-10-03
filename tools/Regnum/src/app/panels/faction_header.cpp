// Regnum — шапка инспектора государства и гильдии: большой флаг (щелчок — редактор флага), вид, цвет,
// название крупным шрифтом с переименованием на месте (щелчок, F2), правитель с титулом, казна;
// меню действий: флаг, переименование, государственная гильдия, удаление с подтверждением.
#include "app/panels/faction_common.h"

namespace rg::app {
namespace {

using namespace fac;
using platform::Key;

struct RenameState {
  Id id = 0;
  bool editing = false;
  int frames = 0;
};

void startRename(RenameState& rs, Id id) {
  rs.id = id;
  rs.editing = true;
  rs.frames = 0;
}

// Таблетка казны: монеты, сумма, чистый доход за ход; щелчок — вкладка экономики.
void treasuryPill(App& a, double treasury, double net, float width) {
  const ui::Theme& t = ui::theme();
  ui::ButtonOpt bo;
  bo.variant = ui::Variant::Ghost;
  bo.fill = true;
  bo.tooltip = "Казна и чистый доход за ход — открыть экономику";
  bool clicked = ui::button("##treasury", bo);
  RectF r = ui::lastItem().rect;
  r.w = std::min(r.w, width);
  bool hov = ui::lastItem().hovered;
  ui::draw::rect(r, t.dark ? (hov ? t.hover : t.stripe) : (hov ? t.surface3 : t.surface3.alpha(0.6f)), t.radiusField);
  ui::draw::rectStroke(r, t.dark ? t.hover : t.border, t.radiusField, 1);
  float x = r.x + 9;
  ui::draw::icon("coins", RectF{x, r.cy() - 8, 16, 16}, t.accent);
  x += 22;
  std::string v = money(treasury);
  float vw = ui::measure(v, ui::Font::Strong);
  ui::draw::text(v, RectF{x, r.y, vw + 2, r.h}, ui::Font::Strong, treasury < 0 ? t.danger : t.text);
  x += vw + 6;
  if (std::fabs(net) >= 0.05) {
    std::string d = moneySigned(net);
    ui::draw::text(d, RectF{x, r.y + 1, r.right() - x - 6, r.h}, ui::Font::Caption, net > 0 ? t.success : t.danger);
  }
  a.markUi("faction.treasury", r);
  if (clicked) showTab(a, kTabEconomy);
}

void drawHeader(App& a, Id id) {
  const World& w = frameWorld(a);
  const Faction* f = w.faction(id);
  if (!f) return;
  const ui::Theme& t = ui::theme();
  const bool ro = a.readOnly();
  auto calc = rules::calc(w);
  const rules::FactionCalc* fc = calc->faction(id);
  const KindInfo kind = kindInfo(*f);
  const std::string name = displayName(*f);

  auto& rs = ui::state<RenameState>(ui::id("##rename"));
  if (rs.id != id) rs = RenameState{id, false, 0};
  if (!ro && takeRename(id)) startRename(rs, id);
  if (!ro && !rs.editing && ui::shortcut({Key::F2, 0})) startRename(rs, id);

  // Ряд 1: флаг, вид и цвет, меню действий.
  {
    ui::Row row({ui::px(72), ui::fr(1), ui::px(30)}, ui::kAuto, 12);
    ui::flag(f->flag, 72, 48, 5, ro ? "Флаг" : "Флаг — изменить");
    ui::Item fi = ui::lastItem();
    a.markUi("faction.flag", fi.rect);
    if (fi.hovered && !ro) {
      ui::draw::rect(fi.rect, t.scrim, 5);
      ui::draw::icon("palette", RectF{fi.rect.cx() - 10, fi.rect.cy() - 10, 20, 20}, t.dark ? t.text : t.surface2);
      ui::setCursor(platform::Cursor::Hand);
    }
    if (fi.clicked && !ro) openFlagEditor(a, id);
    {
      ui::Group g(0, 6);
      {
        ui::HStack hs(22, ui::Align::Left, 6);
        ui::tag(kind.label, kind.tone, kind.icon);
      }
      {
        ui::HStack hs(24, ui::Align::Left, 6);
        Color c = f->color;
        {
          ui::Disabled d(ro);
          if (ui::colorButton("color", c, {.tooltip = "Цвет на карте", .size = ui::Size::Small, .hex = false}))
            a.act("Цвет фракции", [&](Tx& tx) { tx.faction(id).color = c; }, {.coalesce = "faction.color:" + std::to_string(id)});
        }
        a.markUi("faction.color");
        if (f->isGuild() && f->homeState) w::factionChip(f->homeState);
        else if (f->isState() && f->capital) w::provinceChip(f->capital);
      }
    }
    if (ui::iconButton("more-v", "Действия")) ui::openPopup("more");
    a.markUi("faction.more");
    if (ui::beginMenu("more")) {
      ui::menuHeader(kind.label);
      if (ui::menuItem("Изменить флаг…", {.icon = "flag", .disabled = ro})) openFlagEditor(a, id);
      a.markUi("faction.menu.flag");
      if (ui::menuItem("Переименовать", {.icon = "edit", .shortcut = {Key::F2, 0}, .disabled = ro})) startRename(rs, id);
      a.markUi("faction.menu.rename");
      if (f->isState()) {
        ui::menuSeparator();
        if (ui::menuItem("Создать государственную гильдию", {.icon = "guild", .disabled = ro})) createStateGuildUi(a, id);
        a.markUi("faction.menu.stateguild");
      }
      ui::menuSeparator();
      if (ui::menuItem(f->isState() ? "Удалить государство" : "Удалить гильдию", {.icon = "trash", .danger = true, .disabled = ro})) confirmDelete(a, id);
      a.markUi("faction.menu.delete");
      ui::endMenu();
    }
  }

  // Ряд 2: название (крупно), щелчок или F2 — переименовать на месте.
  if (rs.editing && !ro) {
    if (rs.frames == 0) ui::setKeyboardFocus(ui::id("rename"));
    std::string v = f->name;
    bool changed = ui::textField("rename", v, {.placeholder = "Название", .icon = "edit", .maxLength = 80, .selectAllOnFocus = true});
    ui::Item li = ui::lastItem();
    a.markUi("faction.rename");
    if (changed) {
      std::string n = trim(v);
      if (n.empty()) a.toast("Название не может быть пустым", ToastKind::Warning, "edit");
      else if (n != f->name) a.act(f->isState() ? "Переименовать государство" : "Переименовать гильдию", [&](Tx& tx) { tx.faction(id).name = n; });
    }
    if (rs.frames > 1 && (li.deactivated || !li.focused)) rs.editing = false;
    rs.frames++;
  } else {
    float avail = ui::avail().w;
    // Крупно (Display), длинное — меньше (Heading), очень длинное — в две строки.
    bool display = ui::measure(name, ui::Font::Display) <= avail - 26;
    bool wrap = !display && ui::measure(name, ui::Font::Heading) > avail - 26;
    ui::label(name, {.font = display ? ui::Font::Display : ui::Font::Heading, .wrap = wrap, .maxLines = 2,
                     .tooltip = ro ? std::string_view(name) : std::string_view("Переименовать · F2")});
    ui::Item li = ui::lastItem();
    a.markUi("faction.name", li.rect);
    if (li.hovered && !ro) {
      float ix = std::min(li.rect.right() + 6, li.rect.x + avail - 18);
      ui::draw::icon("edit", RectF{ix, li.rect.cy() - 8, 16, 16}, t.textMuted);
      ui::setCursor(platform::Cursor::IBeam);
    }
    if (li.clicked && !ro) startRename(rs, id);
  }

  // Ряд 3: правитель с титулом и казна.
  {
    const Character* ruler = w.character(f->ruler);
    double net = fc ? fc->net : 0;
    std::string tre = money(f->treasury()), delta = std::fabs(net) >= 0.05 ? moneySigned(net) : std::string();
    float pillW = 9 + 22 + ui::measure(tre, ui::Font::Strong) + (delta.empty() ? 4 : 6 + ui::measure(delta, ui::Font::Caption)) + 12;
    ui::Row row({ui::px(42), ui::fr(1), ui::px(std::ceil(pillW))}, 40, 8);
    std::string rulerName = ruler ? (ruler->name.empty() ? std::string("Без имени") : ruler->name) : std::string("?");
    if (ruler) {
      ui::avatar(rulerName, {.image = portraitOf(*ruler), .size = 34, .ring = true, .tooltip = f->isState() ? "Правитель — открыть" : "Глава — открыть"});
      if (ui::lastItem().clicked) a.select(SelType::Character, ruler->id);
    } else {
      // Пусто: пунктирное кольцо и кнопка «назначить» (вкладка «Обзор»).
      RectF r = ui::next(40, 40);
      float cx = r.cx(), cy = r.cy();
      for (int k = 0; k < 12; k++) {
        double a0 = k * kPi / 6, a1 = a0 + kPi / 10;
        ui::draw::line(cx + 18.5f * float(std::cos(a0)), cy + 18.5f * float(std::sin(a0)), cx + 18.5f * float(std::cos(a1)),
                       cy + 18.5f * float(std::sin(a1)), t.borderStrong, 1.5f);
      }
      ui::at(RectF{cx - 15, cy - 15, 30, 30});
      if (ui::iconButton("user-plus", f->isState() ? "Назначить правителя" : "Назначить главу", {.disabled = ro})) showTab(a, kTabOverview);
    }
    {
      ui::Group g(0, 0);
      std::string title = !f->rulerTitle.empty() ? f->rulerTitle : (ruler && !ruler->title.empty() ? ruler->title : std::string(f->isState() ? "Правитель" : "Глава гильдии"));
      ui::label(ruler ? rulerName : std::string(f->isState() ? "Правитель не назначен" : "Глава не назначен"),
                {.font = ui::Font::Strong, .ink = ruler ? ui::Ink::Normal : ui::Ink::Muted});
      ui::label(title, {.font = ui::Font::Small, .ink = ui::Ink::Muted});
    }
    treasuryPill(a, f->treasury(), fc ? fc->net : 0, pillW);
  }
}

HeaderReg header({"faction.header", SelType::Faction, 10, drawHeader});

}  // namespace
}  // namespace rg::app
