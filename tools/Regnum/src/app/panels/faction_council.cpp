// Regnum — вкладка «Совет» (ТЗ 1.b.iv): редактируемый список советников — назначение в совете (должность
// справочника или своя) и назначенный лорд; содержание советника правится в строке (поле персонажа, расход
// «специалисты»); добавление и удаление мест.
#include "app/panels/faction_common.h"

namespace rg::app {
namespace {

using namespace fac;

constexpr Id kCustom = 0xFFFFFFFEu;

// Место совета внутри транзакции по ID (место могли удалить — тогда UserError).
CouncilSeat& seatOf(Tx& tx, Id faction, Id seat) {
  for (CouncilSeat& s : tx.faction(faction).council)
    if (s.id == seat) return s;
  fail("Место в совете не найдено");
}

void addSeat(App& a, Id id) {
  a.act("Место в совете", [&](Tx& tx) {
    const Faction& f = *tx.w().faction(id);
    // Первая свободная должность справочника.
    std::string pos;
    for (const CatalogItem& c : tx.w().catalogs->positions) {
      bool used = std::any_of(f.council.begin(), f.council.end(), [&](const CouncilSeat& s) { return s.position == c.name; });
      if (!used) {
        pos = c.name;
        break;
      }
    }
    CouncilSeat seat;
    seat.id = tx.nextId(Seq::Council);
    seat.position = pos.empty() ? std::string("Советник") : pos;
    tx.faction(id).council.push_back(seat);
  });
}

// Должность: пункты справочника, своя (текущая, если не из справочника) и «Своя должность…».
void positionPicker(App& a, const World& w, Id faction, const CouncilSeat& seat, bool ro) {
  const auto& cat = w.catalogs->positions;
  std::vector<std::string> labels;
  std::vector<Id> ids;
  bool custom = !seat.position.empty() &&
                std::none_of(cat.begin(), cat.end(), [&](const CatalogItem& c) { return c.name == seat.position; });
  if (custom) {
    labels.push_back(seat.position);
    ids.push_back(0);
  }
  int cur = custom ? 0 : -1;
  for (const CatalogItem& c : cat) {
    if (c.name == seat.position) cur = int(labels.size());
    labels.push_back(c.name.empty() ? std::string("Без названия") : c.name);
    ids.push_back(c.id);
  }
  labels.push_back("Своя должность…");
  ids.push_back(kCustom);
  std::vector<ui::Option> opts(labels.size());
  for (size_t i = 0; i < labels.size(); i++) {
    opts[i].label = labels[i];
    if (ids[i] == kCustom) opts[i].icon = "edit";
  }
  int idx = cur;
  ui::ComboOpt co;
  co.placeholder = "Должность";
  co.disabled = ro;
  co.tooltip = "Назначение в совете";
  if (!ui::combo("pos", idx, std::span<const ui::Option>(opts), co) || idx < 0 || idx >= int(ids.size())) return;
  Id seatId = seat.id;
  if (ids[size_t(idx)] == kCustom) {
    a.prompt("Должность в совете", "Назначение", seat.position, [faction, seatId](App& x, const std::string& v) {
      x.act("Должность в совете", [&](Tx& tx) { seatOf(tx, faction, seatId).position = trim(v); });
    });
    return;
  }
  std::string pos = labels[size_t(idx)];
  if (pos != seat.position) a.act("Должность в совете", [&](Tx& tx) { seatOf(tx, faction, seatId).position = pos; });
}

void drawCouncil(App& a, Id id) {
  const World& w = frameWorld(a);
  const Faction* f = w.faction(id);
  if (!f) return;
  const bool ro = a.readOnly();
  const int n = int(f->council.size());
  double total = 0;
  int vacant = 0;
  for (const CouncilSeat& s : f->council) {
    const Character* c = w.character(s.character);
    if (c) total += std::max(0.0, c->upkeep);
    else vacant++;
  }
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 10);
    ui::stat(fmtInt(n), plural(n, "место в совете", "места в совете", "мест в совете"),
             {.icon = "council", .tone = ui::Tone::Accent, .tooltip = vacant ? "Вакантных мест: " + std::to_string(vacant) : std::string("Все места заняты")});
    ui::stat(money(total), "Содержание за ход", {.icon = "coins", .tone = ui::Tone::Warning,
                                                   .tooltip = "Сумма содержания советников — входит в расход «специалисты»"});
  }
  ui::spacer(2);
  ui::Section sec("Совет", "council", {.badge = n ? std::to_string(n) : std::string(), .actionIcon = ro ? nullptr : "plus",
                                       .actionTooltip = "Добавить место в совете"});
  if (sec.action()) addSeat(a, id);
  if (!sec) return;
  if (n == 0) {
    ui::emptyState("council", "Совет пока пуст.");
    if (!ro) {
      if (ui::button("Добавить место", {.variant = ui::Variant::Primary, .icon = "plus", .fill = true})) addSeat(a, id);
      a.markUi("council.add");
    }
    return;
  }
  const ui::Theme& t = ui::theme();
  const bool wide = ui::avail().w >= 500;
  for (int i = 0; i < n; i++) {
    const CouncilSeat seat = f->council[size_t(i)];
    const Id sid = seat.id;
    const Character* c = w.character(seat.character);
    ui::IdScope sc{i64(sid)};
    if (i > 0) ui::separator();
    auto position = [&] {
      positionPicker(a, w, id, seat, ro);
      a.markUi("council.pos." + std::to_string(i));
    };
    auto lord = [&] {
      Id who = seat.character;
      if (w::characterPicker("who", who, id, "Вакантно", true, ro)) a.act("Советник", [&](Tx& tx) { seatOf(tx, id, sid).character = who; });
      a.markUi("council.who." + std::to_string(i));
    };
    // Содержание советника — поле персонажа (расход «специалисты»); у вакантного места — прочерк.
    auto upkeep = [&] {
      if (!c) {
        ui::label("—", {.ink = ui::Ink::Muted, .align = ui::Align::Center, .tooltip = "Место вакантно — содержания нет"});
        return;
      }
      double up = c->upkeep;
      const Id cid = c->id;
      if (ui::numberField("upkeep", up, {.min = 0, .max = 1e9, .step = 1, .digits = std::fabs(up - std::round(up)) > 1e-9 ? 1 : 0, .icon = "coins",
                                         .disabled = ro, .tooltip = "Содержание советника за ход (расход «специалисты»)"}))
        a.act("Содержание советника", [&](Tx& tx) { tx.character(cid).upkeep = std::max(0.0, up); }, {.coalesce = "council.upkeep:" + std::to_string(cid)});
      a.markUi("council.upkeep." + std::to_string(i));
    };
    auto remove = [&] {
      if (ui::iconButton("trash", "Убрать место из совета", {.size = ui::Size::Small, .disabled = ro, .tone = ui::Tone::Danger})) {
        std::string what = seat.position.empty() ? std::string("Место") : "«" + seat.position + "»";
        if (a.act("Убрать место из совета", [&](Tx& tx) {
              auto& list = tx.faction(id).council;
              list.erase(std::remove_if(list.begin(), list.end(), [&](const CouncilSeat& x) { return x.id == sid; }), list.end());
            }))
          a.toast(what + " убрано из совета", ToastKind::Info, "trash", "Отменить", [](App& x) { x.undo(); });
      }
      a.markUi("council.remove." + std::to_string(i));
    };
    auto avatar = [&] {
      if (c) {
        ui::avatar(c->name.empty() ? std::string("?") : c->name, {.image = portraitOf(*c), .size = 24});
      } else {
        RectF r = ui::next(24, 30);
        ui::draw::ring(r.cx(), r.cy(), 11.5f, 1, t.borderStrong);
        ui::draw::icon("user", RectF{r.cx() - 7, r.cy() - 7, 14, 14}, t.textMuted);
      }
    };
    if (wide) {
      ui::Row row({ui::px(24), ui::fr(1), ui::fr(1.25f), ui::px(84), ui::px(28)}, 30, 10);
      avatar();
      position();
      lord();
      upkeep();
      remove();
    } else {
      {
        ui::Row row({ui::px(24), ui::fr(1), ui::px(28)}, 30, 10);
        RectF r = ui::next(24, 30);
        ui::draw::icon("council", RectF{r.cx() - 8, r.cy() - 8, 16, 16}, c ? t.accent : t.textMuted);
        position();
        remove();
      }
      {
        ui::Row row({ui::px(24), ui::fr(1), ui::px(84)}, 30, 10);
        avatar();
        lord();
        upkeep();
      }
    }
  }
  ui::separator();
  {
    ui::Row row({ui::fr(1), ui::px(120)}, 24, 8);
    ui::label(vacant ? std::to_string(vacant) + " " + plural(vacant, "вакансия", "вакансии", "вакансий") : std::string("Все места заняты"),
              {.font = ui::Font::Small, .ink = ui::Ink::Muted});
    ui::label("Итого " + money(total) + " / ход", {.font = ui::Font::Strong, .align = ui::Align::Right});
  }
  if (!ro) {
    ui::spacer(2);
    if (ui::button("Добавить место", {.icon = "plus", .fill = true})) addSeat(a, id);
    a.markUi("council.add");
  }
}

TabReg tab({kTabCouncil, "council", "Совет", 15, SelType::Faction, nullptr, drawCouncil});

}  // namespace
}  // namespace rg::app
