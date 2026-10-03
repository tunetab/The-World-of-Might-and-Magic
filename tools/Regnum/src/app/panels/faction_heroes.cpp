// Regnum — вкладка «Герои» (ТЗ 1.b.iv: список значимых героев государства): портрет, имя, титул, содержание
// за ход (расход «специалисты»), отметка «в войске»; новый герой, назначение персонажа героем, снятие, переход.
#include "app/panels/faction_common.h"

namespace rg::app {
namespace {

using namespace fac;

std::string charName(const Character& c) { return c.name.empty() ? std::string("Без имени") : c.name; }

void newHero(App& a, Id id) {
  Id cid = 0;
  if (!a.act("Новый герой", [&](Tx& tx) {
        cid = rules::createCharacter(tx, id, "Новый герой");
        Character& c = tx.character(cid);
        c.hero = true;
        c.title = "Герой";
      }))
    return;
  a.toast("Добавлен новый герой", ToastKind::Success, "hero", "Открыть", [cid](App& x) { x.select(SelType::Character, cid); });
}

void drawHeroes(App& a, Id id) {
  const World& w = frameWorld(a);
  const Faction* f = w.faction(id);
  if (!f) return;
  const bool ro = a.readOnly();
  std::vector<const Character*> heroes, candidates;
  w.characters.each([&](const Character& c) {
    if (c.faction == id && c.hero) heroes.push_back(&c);
    else if (!c.hero && (c.faction == id || c.faction == 0)) candidates.push_back(&c);
  });
  std::sort(heroes.begin(), heroes.end(), [](const Character* x, const Character* y) { return compareRu(x->name, y->name) < 0; });
  std::sort(candidates.begin(), candidates.end(), [id](const Character* x, const Character* y) {
    if ((x->faction == id) != (y->faction == id)) return x->faction == id;
    return compareRu(x->name, y->name) < 0;
  });
  double upkeep = 0;
  int inArmy = 0;
  for (const Character* c : heroes) {
    upkeep += std::max(0.0, c->upkeep);
    if (armyOfCharacter(w, c->id)) inArmy++;
  }
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 10);
    ui::stat(fmtInt(i64(heroes.size())), plural(i64(heroes.size()), "герой", "героя", "героев"),
             {.icon = "hero", .tone = ui::Tone::Accent, .tooltip = "В войсках и флотах: " + std::to_string(inArmy)});
    ui::stat(money(upkeep), "Содержание за ход", {.icon = "coins", .tone = ui::Tone::Warning, .tooltip = "Входит в расход «специалисты»"});
  }
  ui::spacer(2);
  ui::Section sec("Герои", "hero", {.badge = heroes.empty() ? std::string() : std::to_string(heroes.size()), .actionIcon = ro ? nullptr : "user-plus",
                                    .actionTooltip = "Новый герой"});
  if (sec.action()) newHero(a, id);
  if (!sec) return;
  if (heroes.empty()) {
    ui::emptyState("hero", "Значимых героев пока нет.");
  } else {
    ui::Column cols[] = {{"Герой", nullptr, ui::fr(1, 120), ui::Align::Left, true},
                         {{}, "coins", ui::px(66), ui::Align::Left, true, "Содержание за ход"},
                         {{}, "army", ui::px(40), ui::Align::Center, true, "В войске или флоте"},
                         {{}, nullptr, ui::px(70)}};
    ui::Table t("heroes", cols, int(heroes.size()), {.rowHeight = 46, .selectable = false});
    t.sort([&](int x, int y, int col) {
      const Character& A = *heroes[size_t(x)];
      const Character& B = *heroes[size_t(y)];
      if (col == 1) return A.upkeep < B.upkeep ? -1 : A.upkeep > B.upkeep ? 1 : 0;
      if (col == 2) return int(armyOfCharacter(w, A.id) != 0) - int(armyOfCharacter(w, B.id) != 0);
      return compareRu(A.name, B.name);
    });
    for (int i : t) {
      const Character& c = *heroes[size_t(i)];
      const Id cid = c.id;
      ui::IdScope sc{i64(cid)};
      t.cell();
      {
        bool ruler = f->ruler == cid;
        ui::Row rr({ui::px(38), ui::fr(1)}, 40, 8);
        std::string nm = charName(c);
        ui::avatar(nm, {.image = portraitOf(c), .size = 32, .ring = ruler, .tooltip = ruler ? "Правитель" : ""});
        ui::Group g(0, 0);
        const std::string tip = c.title.empty() ? nm : nm + " · " + c.title;
        ui::label(nm, {.font = ui::Font::Strong, .tooltip = tip});
        ui::label(c.title.empty() ? std::string("Без титула") : c.title, {.font = ui::Font::Small, .ink = ui::Ink::Muted});
      }
      t.cell();
      double up = c.upkeep;
      if (ui::numberField("upkeep", up, {.min = 0, .max = 1e9, .step = 1, .digits = std::fabs(up - std::round(up)) > 1e-9 ? 1 : 0, .disabled = ro,
                                         .tooltip = "Содержание за ход"}))
        a.act("Содержание героя", [&](Tx& tx) { tx.character(cid).upkeep = std::max(0.0, up); }, {.coalesce = "hero.upkeep:" + std::to_string(cid)});
      if (Id army = armyOfCharacter(w, cid)) {
        t.cell();
        const Army* ar = w.army(army);
        std::string an = ar && !ar->name.empty() ? ar->name : std::string(ar && ar->isFleet() ? "Флот" : "Войско");
        ui::iconColored(ar && ar->isFleet() ? "fleet" : "army", ui::theme().accent, 16,
                        (ar && ar->commander == cid ? "Командует: " : "В составе: ") + an + " — показать");
        if (ui::lastItem().hovered) ui::setCursor(platform::Cursor::Hand);
        if (ui::lastItem().clicked) a.select(SelType::Army, army, true);
        a.markUi("heroes.army." + std::to_string(i));
      } else {
        t.text("—", ui::Ink::Muted);
      }
      t.cell();
      {
        ui::HStack hs(24, ui::Align::Right, 4);
        if (ui::iconButton("arrow-right", "Открыть персонажа", {.size = ui::Size::Small})) a.select(SelType::Character, cid);
        if (ui::iconButton("close", "Убрать из героев", {.size = ui::Size::Small, .disabled = ro, .tone = ui::Tone::Danger})) {
          std::string nm = charName(c);
          if (a.act("Убрать из героев", [&](Tx& tx) { tx.character(cid).hero = false; }))
            a.toast(nm + " больше не в списке героев", ToastKind::Info, "hero", "Отменить", [](App& x) { x.undo(); });
        }
        a.markUi("heroes.remove." + std::to_string(i));
      }
    }
    if (t.footer()) {
      t.text("Итого");
      t.text(money(upkeep));
      t.text({});
      t.text({});
    }
  }
  if (!ro) {
    ui::spacer(2);
    ui::Row r({ui::fr(1), ui::px(132)}, 30, 8);
    if (!candidates.empty()) {
      std::vector<std::string> labels, hints;
      std::vector<ui::Option> opts;
      for (const Character* c : candidates) {
        labels.push_back(charName(*c));
        hints.push_back(c->faction == id ? c->title : std::string("без фракции"));
      }
      for (size_t k = 0; k < candidates.size(); k++) opts.push_back(ui::Option{labels[k], "character", Color(0, 0, 0, 0), hints[k]});
      int pick = -1;
      if (ui::combo("assign", pick, std::span<const ui::Option>(opts), {.placeholder = "Назначить героем…", .icon = "hero"}) && pick >= 0) {
        Id cid = candidates[size_t(pick)]->id;
        a.act("Назначить героем", [&](Tx& tx) {
          Character& c = tx.character(cid);
          c.hero = true;
          c.faction = id;
        });
      }
      a.markUi("heroes.assign");
    } else {
      ui::label("Все персонажи фракции — герои", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
    }
    if (ui::button("Новый герой", {.icon = "user-plus", .fill = true})) newHero(a, id);
    a.markUi("heroes.new");
  }
}

TabReg tab({kTabHeroes, "hero", "Герои", 60, SelType::Faction, nullptr, drawHeroes});

}  // namespace
}  // namespace rg::app
