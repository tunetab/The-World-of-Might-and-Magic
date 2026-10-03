// Regnum — разделение войска или флота (ТЗ 1.c.iv, кнопка «разделить»): сколько отрядов каждой строки и какие
// герои уходят в новый объект (rules::splitArmy). Новый объект появляется рядом и выделяется.
#include "app/app_internal.h"
#include "app/panels/military.h"

namespace rg::app::mil {

namespace {

struct SplitDialog final : Dialog {
  Id army = 0;
  std::map<std::pair<Id, Id>, i64> take;   // (фракция, строка) -> в новый объект
  std::vector<Id> heroes;
  float bodyH = 0;

  const char* id() const override { return "army.split"; }
  Style style(App& a) override {
    const Army* ar = a.world().army(army);
    return {ar && ar->isFleet() ? "Разделить флот" : "Разделить войско", "split", ui::Tone::Accent, 600};
  }

  i64 moved() const {
    i64 n = 0;
    for (auto& [k, v] : take) n += v;
    return n;
  }

  void groupBlock(App& a, const World& w, const Army& ar, const ArmyGroup& g) {
    const ui::Theme& th = ui::theme();
    bool fleet = ar.isFleet();
    ui::IdScope s(i64(g.faction) + 0x74000000LL);
    if (ar.allied()) {
      ui::HStack hs(22, ui::Align::Left, 6);
      factionFlag(w, g.faction, 24, 16);
      ui::label(w.factionName(g.faction), {.font = ui::Font::Strong});
    }
    for (const ArmyUnit& u : g.units) {
      auto r = unitRow(w, g.faction, u.row, fleet);
      if (!r) continue;
      ui::IdScope rs{i64(u.row)};
      i64& v = take[{g.faction, u.row}];
      v = clamp<i64>(v, 0, u.count);
      ui::Row row({ui::fr(1, 120), ui::fr(1.2f, 120), ui::px(96)}, 40, 10);
      {
        RectF cr = ui::next(40);
        typeTile(RectF{cr.x, cr.cy() - 13, 26, 26}, r->icon, v > 0 ? th.accent : th.textDim);
        cellLines(RectF{cr.x + 34, cr.y, cr.w - 34, cr.h}, r->name, "останется " + fmtCount(u.count - v) + " из " + fmtCount(u.count), ui::Align::Left);
      }
      ui::slider("s", v, 0, double(u.count), {.step = 1, .showValue = false});
      ui::numberField("n", v, {.min = 0, .max = double(u.count), .tooltip = "Сколько уйдёт в новый объект"});
      a.markUi("split.n." + std::to_string(g.faction) + "." + std::to_string(u.row));
    }
    if (!g.heroes.empty()) {
      ui::HStack hs(28, ui::Align::Left, 10);
      ui::icon("hero", ui::Ink::Dim, 16, "Герои уйдут вместе с отрядами");
      for (Id h : g.heroes) {
        ui::IdScope hs2{i64(h)};
        bool on = std::find(heroes.begin(), heroes.end(), h) != heroes.end();
        std::string label = w.characterName(h);
        if (ar.commander == h) label += fleet ? " · флотоводец" : " · полководец";
        if (ui::checkbox(label, on)) {
          if (on) heroes.push_back(h);
          else heroes.erase(std::remove(heroes.begin(), heroes.end(), h), heroes.end());
        }
        a.markUi("split.hero." + std::to_string(h));
      }
    }
  }

  bool draw(App& a) override {
    const World& w = frameWorld(a);
    const Army* ar = w.army(army);
    if (!ar) return false;
    bool fleet = ar->isFleet();
    ui::text(std::string("Выберите, что уйдёт в новый ") + (fleet ? "флот" : "объект") + " — он появится рядом на карте.", ui::Font::Body, ui::Ink::Dim);
    float maxH = std::max(140.f, ui::viewport().h - 400);
    float h = std::min(bodyH > 0 ? bodyH : 220.f, maxH);
    {
      ui::Scroll sc("rows", h);
      ui::Group g(0, 8);
      for (const ArmyGroup& gr : ar->groups) groupBlock(a, w, *ar, gr);
    }
    // Высота содержимого: Group — последний элемент области прокрутки.
    bodyH = 0;
    for (const ArmyGroup& gr : ar->groups) {
      bodyH += float(gr.units.size()) * 48 + (gr.heroes.empty() ? 0 : 36) + (ar->allied() ? 30 : 0);
    }
    i64 total = unitCount(*ar), out = moved();
    {
      ui::Row r({ui::fr(1), ui::fr(1)}, 64, 10);
      ui::stat(fmtCount(total - out), "Останется", {.icon = fleet ? "fleet" : "army", .tone = ui::Tone::Neutral});
      ui::stat(fmtCount(out), fleet ? "В новом флоте" : "В новом войске", {.icon = "split", .tone = ui::Tone::Accent});
    }
    std::string why;
    if (out <= 0) why = "Выберите отряды для нового объекта";
    else if (out >= total) why = "Нельзя выделить весь состав — оставьте хотя бы один отряд";
    {
      ui::HStack hs(26, ui::Align::Left, 8);
      if (ui::button("Поровну", {.variant = ui::Variant::Ghost, .icon = "split", .size = ui::Size::Small, .tooltip = "Половина каждой строки"})) {
        for (const ArmyGroup& gr : ar->groups)
          for (const ArmyUnit& u : gr.units) take[{gr.faction, u.row}] = u.count / 2;
      }
      a.markUi("split.half");
      if (ui::button("Сбросить", {.variant = ui::Variant::Ghost, .icon = "undo", .size = ui::Size::Small})) {
        take.clear();
        heroes.clear();
      }
      if (!why.empty()) {
        ui::flex();
        ui::label(why, {.font = ui::Font::Small, .ink = out <= 0 ? ui::Ink::Muted : ui::Ink::Warning, .icon = out <= 0 ? "info" : "warning"});
      }
    }
    ui::ModalFooter f;
    if (ui::button("Отмена")) return false;
    a.markUi("split.cancel");
    ui::Disabled dis(a.readOnly());
    if (ui::button("Разделить", {.variant = ui::Variant::Primary, .icon = "split", .disabled = !why.empty()}) && why.empty()) {
      rules::SplitSpec spec;
      for (auto& [k, v] : take)
        if (v > 0) spec.units[k] = v;
      spec.heroes = heroes;
      Id nid = 0, src = army;
      if (a.act(fleet ? "Разделить флот" : "Разделить войско", [&](Tx& tx) { nid = rules::splitArmy(tx, src, spec); })) {
        a.select(SelType::Army, nid);
        if (const Army* n = a.world().army(nid)) a.toast("Выделено: «" + objectName(*n) + "»", ToastKind::Success, "split");
        return false;
      }
    }
    a.markUi("split.ok");
    return true;
  }
};

std::unique_ptr<Dialog> makeRegistered(App& a, Id arg) {
  if (!a.world().army(arg)) fail("Войско не найдено");
  auto d = std::make_unique<SplitDialog>();
  d->army = arg;
  return d;
}

DialogReg reg({"army.split", makeRegistered});

}  // namespace

void openSplit(App& a, Id army) {
  const Army* ar = a.world().army(army);
  if (!ar) return;
  if (a.readOnly()) {
    a.act("Разделить", [](Tx&) {});
    return;
  }
  if (unitCount(*ar) < 2) {
    a.toast(std::string("Делить нечего: в ") + (ar->isFleet() ? "флоте" : "войске") + " меньше двух " + (ar->isFleet() ? "кораблей" : "воинов"),
            ToastKind::Info, "split");
    return;
  }
  a.openDialog("army.split", army);
}

}  // namespace rg::app::mil
